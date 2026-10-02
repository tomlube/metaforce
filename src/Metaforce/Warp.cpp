#include "Metaforce/Warp.hpp"

#include "Kyoto/CPakFile.hpp"
#include "Kyoto/CResFactory.hpp"
#include "Kyoto/Streams/CInputStream.hpp"
#include "Kyoto/Text/CStringTable.hpp"
#include "Metaforce/ResourceNameDatabase.hpp"
#include "MetroidPrime/CGameArea.hpp"
#include "MetroidPrime/CMain.hpp"
#include "MetroidPrime/CScriptLayerManager.hpp"
#include "MetroidPrime/CStateManager.hpp"
#include "MetroidPrime/CWorld.hpp"
#include "MetroidPrime/Player/CGameState.hpp"
#include "MetroidPrime/Player/CWorldState.hpp"
#include "MetroidPrime/Player/CWorldTransManager.hpp"

#include <borealis/log.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <map>
#include <memory>

namespace metaforce::warp {
namespace {
constexpr borealis::Log Log{"warp"};

struct PendingWarp {
  uint32_t mlvl;
  int area;
  std::optional< uint64_t > layerBits;
};

std::vector< World > sWorlds;
bool sCatalogBuilt = false;
std::optional< PendingWarp > sPending;

void AppendUtf8(std::string& out, uint32_t cp) {
  if (cp < 0x80) {
    out += static_cast< char >(cp);
  } else if (cp < 0x800) {
    out += static_cast< char >(0xC0 | (cp >> 6));
    out += static_cast< char >(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += static_cast< char >(0xE0 | (cp >> 12));
    out += static_cast< char >(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast< char >(0x80 | (cp & 0x3F));
  } else {
    out += static_cast< char >(0xF0 | (cp >> 18));
    out += static_cast< char >(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast< char >(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast< char >(0x80 | (cp & 0x3F));
  }
}

// Converts a STRG string to UTF-8, dropping formatting tags like "&just=center;" and newlines.
std::string ToDisplayString(const wchar_t* str) {
  std::string out;
  if (str == nullptr) {
    return out;
  }
  for (const wchar_t* c = str; *c != 0; ++c) {
    if (*c == L'&') {
      const wchar_t* end = c + 1;
      while (*end != 0 && *end != L';') {
        ++end;
      }
      if (*end == L';') {
        c = end;
        continue;
      }
    }
    if (*c == L'\n' || *c == L'\r') {
      if (!out.empty() && out.back() != ' ') {
        out += ' ';
      }
      continue;
    }
    AppendUtf8(out, static_cast< uint32_t >(*c));
  }
  while (!out.empty() && out.back() == ' ') {
    out.pop_back();
  }
  return out;
}

std::string ReadName(CResLoader& loader, CAssetId strg) {
  if (strg == kInvalidAssetId || loader.GetResourceTypeById(strg) != 'STRG') {
    return {};
  }
  std::unique_ptr< CInputStream > in(loader.LoadNewResourceSync(SObjectTag('STRG', strg), nullptr));
  if (!in) {
    return {};
  }
  CStringTable table(*in);
  return table.GetStringCount() > 0 ? ToDisplayString(table.GetString(0)) : std::string{};
}

std::string FallbackAreaName(CAssetId mrea, int index) {
  if (const rstl::string* name = ResourceNameDatabase::GetNameForResource(mrea)) {
    return name->data();
  }
  return fmt::format("Area {}", index);
}

// Mirrors CDummyWorld::ICheckWorldComplete, keeping the area names and layer tables.
bool ReadWorld(CResLoader& loader, CAssetId mlvl, World& world) {
  std::unique_ptr< CInputStream > in(loader.LoadNewResourceSync(SObjectTag('MLVL', mlvl), nullptr));
  if (!in) {
    return false;
  }
  CInputStream& r = *in;
  if (r.ReadLong() != 0xDEAFBABE) {
    return false;
  }
  const uint version = r.ReadLong();
  const CAssetId worldStrg = r.ReadLong();
  if (version >= 15) {
    r.ReadLong(); // SAVW
  }
  if (version >= 12) {
    r.ReadLong(); // Skybox
  }
  if (version >= 17) {
    rstl::vector< CRelay > relays(r);
  }

  const int areaCount = r.ReadLong();
  r.ReadLong();
  std::vector< std::pair< CAssetId, CAssetId > > areaIds; // MREA, name STRG
  areaIds.reserve(areaCount);
  for (int i = 0; i < areaCount; ++i) {
    CDummyGameArea area(r, i, version);
    areaIds.emplace_back(area.IGetAreaAssetId(), area.IGetStringTableAssetId());
  }

  r.ReadLong(); // MAPW
  r.ReadChar();
  r.ReadLong();
  if (version > 10) {
    const int audioGroupCount = r.ReadLong();
    for (int i = 0; i < audioGroupCount; ++i) {
      r.ReadLong();
      r.ReadLong();
    }
  }
  if (version > 12) {
    rstl::string defaultAudioTrack(r);
  }

  std::vector< CWorldLayers::Area > layerAreas;
  std::vector< std::string > layerNames;
  std::vector< int > layerNameIndices;
  if (version > 14) {
    const rstl::vector< CWorldLayers::Area > areas(r);
    const rstl::vector< rstl::string > names(r);
    const rstl::vector< int > indices(r);
    for (int i = 0; i < areas.size(); ++i) {
      layerAreas.push_back(areas[i]);
    }
    for (int i = 0; i < names.size(); ++i) {
      layerNames.emplace_back(names[i].data());
    }
    for (int i = 0; i < indices.size(); ++i) {
      layerNameIndices.push_back(indices[i]);
    }
  }

  world.mlvl = mlvl;
  world.name = ReadName(loader, worldStrg);
  world.areas.clear();
  world.areas.reserve(areaCount);
  for (int i = 0; i < areaCount; ++i) {
    Area area{
        .index = i,
        .mrea = areaIds[i].first,
        .name = ReadName(loader, areaIds[i].second),
    };
    if (area.name.empty()) {
      area.name = FallbackAreaName(areaIds[i].first, i);
    }
    if (i < layerAreas.size()) {
      const auto& layers = layerAreas[i];
      const int first = i < layerNameIndices.size() ? layerNameIndices[i] : -1;
      for (int l = 0; l < layers.m_layerCount && l < 64; ++l) {
        const int nameIdx = first + l;
        std::string name =
            first >= 0 && nameIdx < layerNames.size() ? layerNames[nameIdx] : std::string{};
        if (name.empty()) {
          name = fmt::format("Layer {}", l);
        }
        area.layers.push_back({
            .name = std::move(name),
            .defaultActive = ((layers.m_layerBits >> l) & 1) != 0,
        });
      }
    }
    world.areas.push_back(std::move(area));
  }
  // Some worlds reuse a room name, such as the Frigate's two Deck Beta elevators.
  std::map< std::string, int > nameCounts;
  for (const auto& area : world.areas) {
    ++nameCounts[area.name];
  }
  for (auto& area : world.areas) {
    if (nameCounts[area.name] > 1) {
      area.name = fmt::format("{} (#{})", area.name, area.index);
    }
  }
  std::stable_sort(world.areas.begin(), world.areas.end(),
                   [](const Area& a, const Area& b) { return a.name < b.name; });
  return true;
}

void BuildCatalog() {
  if (gpResourceFactory == nullptr) {
    return;
  }
  auto& loader = gpResourceFactory->GetResLoader();
  if (!loader.AreAllPaksLoaded()) {
    return;
  }

  std::vector< std::pair< std::string, World > > worlds;
  for (int i = 0; i < loader.GetPakCount(); ++i) {
    const CPakFile* pak = loader.GetPakFile(i);
    if (!pak->IsWorldPak()) {
      continue;
    }
    for (const auto& entry : pak->GetStringToObjectList()) {
      if (entry.second.GetType() != 'MLVL') {
        continue;
      }
      World world;
      if (ReadWorld(loader, entry.second.GetId(), world) && !world.areas.empty()) {
        if (world.name.empty()) {
          world.name = entry.first.data();
        }
        worlds.emplace_back(pak->GetDvdFile().GetFilename().data(), std::move(world));
      }
      break;
    }
  }
  // Metroid1.pak, Metroid2.pak, ... is the game's own world order.
  std::stable_sort(worlds.begin(), worlds.end(),
                   [](const auto& a, const auto& b) { return a.first < b.first; });
  sWorlds.clear();
  for (auto& [pak, world] : worlds) {
    sWorlds.push_back(std::move(world));
  }
  sCatalogBuilt = true;
  Log.info("Loaded {} worlds for warping", sWorlds.size());
}

const World* FindWorld(uint32_t mlvl) {
  for (const auto& world : GetWorlds()) {
    if (world.mlvl == mlvl) {
      return &world;
    }
  }
  return nullptr;
}

const Area* FindArea(const World& world, int index) {
  for (const auto& area : world.areas) {
    if (area.index == index) {
      return &area;
    }
  }
  return nullptr;
}

// A world's layer state is empty until the world is first loaded.
void EnsureLayerState(CScriptLayerManager& layers, const World& world) {
  if (!layers.GetAreaLayers().empty()) {
    return;
  }
  int areaCount = 0;
  for (const auto& area : world.areas) {
    areaCount = std::max(areaCount, area.index + 1);
  }
  rstl::vector< CWorldLayers::Area > defaults;
  defaults.reserve(areaCount);
  for (int i = 0; i < areaCount; ++i) {
    defaults.push_back(CWorldLayers::Area(0, 0));
  }
  for (const auto& area : world.areas) {
    u64 bits = 0;
    for (size_t l = 0; l < area.layers.size(); ++l) {
      if (area.layers[l].defaultActive) {
        bits |= u64(1) << l;
      }
    }
    defaults[area.index] = CWorldLayers::Area(static_cast< int >(area.layers.size()), bits);
  }
  layers.InitializeWorldLayers(
      defaults, rstl::rc_ptr< rstl::vector< rstl::string > >(rs_new rstl::vector< rstl::string >()),
      rstl::rc_ptr< rstl::vector< int > >(rs_new rstl::vector< int >()));
}

} // namespace

const std::vector< World >& GetWorlds() {
  if (!sCatalogBuilt) {
    BuildCatalog();
  }
  return sWorlds;
}

bool CanWarp() {
  return gpStateManager != nullptr && gpStateManager->IsFullyInitialized() &&
         !gpStateManager->GetWantsToQuit() && !sPending && !GetWorlds().empty();
}

bool GetCurrentLocation(uint32_t& mlvl, int& area) {
  if (gpStateManager == nullptr || !gpStateManager->IsFullyInitialized() ||
      gpStateManager->GetWorld() == nullptr) {
    return false;
  }
  const CWorld& world = *gpStateManager->GetWorld();
  mlvl = world.GetWorldAssetId();
  area = world.GetCurrentAreaId().Value();
  return true;
}

uint64_t GetLayerBits(const World& world, const Area& area) {
  if (gpGameState != nullptr) {
    const auto& layers = gpGameState->StateForWorld(world.mlvl).GetLayerState()->GetAreaLayers();
    if (area.index < layers.size()) {
      return layers[area.index].m_layerBits;
    }
  }
  uint64_t bits = 0;
  for (size_t l = 0; l < area.layers.size(); ++l) {
    if (area.layers[l].defaultActive) {
      bits |= uint64_t(1) << l;
    }
  }
  return bits;
}

void RequestWarp(uint32_t mlvl, int area, std::optional< uint64_t > layerBits) {
  if (!CanWarp()) {
    return;
  }
  sPending = PendingWarp{mlvl, area, layerBits};
  gpGameState->WorldTransitionManager()->DisableTransition();
  // Like CScriptWorldTeleporter: kRM_None goes straight back into the game, keeping its state.
  gpMain->SetRestartMode(CMain::kRM_None);
  gpStateManager->QuitGame();
}

void ApplyPendingWarp() {
  if (!sPending || gpGameState == nullptr) {
    return;
  }
  const PendingWarp warp = *sPending;
  sPending.reset();

  const World* world = FindWorld(warp.mlvl);
  const Area* area = world != nullptr ? FindArea(*world, warp.area) : nullptr;
  if (area == nullptr) {
    Log.error("Warp target 0x{:08X} area {} no longer exists", warp.mlvl, warp.area);
    return;
  }

  gpGameState->SetCurrentWorldId(warp.mlvl);
  CWorldState& state = gpGameState->StateForWorld(warp.mlvl);
  state.SetAreaId(TAreaId(warp.area));
  state.SetDesiredAreaAssetId(kInvalidAssetId);
  if (warp.layerBits) {
    CScriptLayerManager& layers = *state.GetLayerState();
    EnsureLayerState(layers, *world);
    const int layerCount = layers.GetAreaLayerCount(TAreaId(warp.area));
    for (int l = 0; l < layerCount && l < 64; ++l) {
      layers.SetLayerActive(TAreaId(warp.area), TLayerId(l), ((*warp.layerBits >> l) & 1) != 0);
    }
  }
  Log.info("Warping to {} / {} (0x{:08X}, area {})", world->name, area->name, warp.mlvl,
           warp.area);
}

} // namespace metaforce::warp
