#include "Metaforce/ObjectBudget.hpp"

#include "Metaforce/Warp.hpp"

#include "Kyoto/CResFactory.hpp"
#include "Kyoto/CResLoader.hpp"
#include "Kyoto/SObjectTag.hpp"
#include "Kyoto/Streams/CInputStream.hpp"
#include "MetroidPrime/CGameArea.hpp"
#include "MetroidPrime/CObjectList.hpp"
#include "MetroidPrime/CScriptLayerManager.hpp"
#include "MetroidPrime/CStateManager.hpp"
#include "MetroidPrime/CWorld.hpp"
#include "MetroidPrime/ScriptObjects/CScriptDoor.hpp"
#include "MetroidPrime/TCastTo.hpp"

#include <borealis/log.hpp>
#include <fmt/format.h>

#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace metaforce::objectbudget {
namespace {

constexpr borealis::Log Log{"ObjectBudget"};

// What the rooms may fill between them. The rest is for objects that belong to no room and for
// ones spawned during play: shots, effects, generated enemies, the randomizer's door locks.
constexpr int kAreaBudget = kMaxObjects - 256;

constexpr uint kMreaMagic = 0xDEADBEEF;
constexpr uint kSclyMagic = 'SCLY';
constexpr uint kMaxLayers = 64;

enum ERank {
  kR_Visible,  // Through a doorway with no door, or a door open or swinging shut
  kR_Awaited,  // A door is waiting for it to load
  kR_Loaded,
  kR_Unloaded,
};

struct Candidate {
  TAreaId area;
  int cost;
  bool hasDoor;
  bool doorOpen;
  bool doorWaiting;
  ERank rank;
};

// Objects in each script layer, by MREA. Empty for one that couldn't be read.
std::map< CAssetId, std::vector< int > > sLayerObjects;
std::vector< TAreaId > sPicked;
// What was last logged, so a room left out is reported once, not every frame. TAreaId() is
// kInvalidAreaId, which may not be initialized yet when this is.
TAreaId sLoggedArea;
std::vector< TAreaId > sLoggedSkipped;
bool sLoggedOver = false;

// `length` bytes at `offset` into the MREA, or null if that runs past its end.
std::unique_ptr< CInputStream > ReadPart(CResLoader& loader, const SObjectTag& tag, uint size,
                                         uint offset, uint length) {
  if (length == 0 || offset > size || length > size - offset) {
    return nullptr;
  }
  return std::unique_ptr< CInputStream >(loader.LoadNewResourcePartSync(
      tag, static_cast< int >(offset), static_cast< int >(length), nullptr));
}

// Object count of each script layer, found the way CGameArea finds its SCLY section. Only the
// headers are read: a few small reads per room, once.
std::vector< int > ReadLayerObjects(CAssetId mrea) {
  std::vector< int > result;
  CResLoader& loader = gpResourceFactory->GetResLoader();
  const SObjectTag tag('MREA', mrea);
  if (!loader.ResourceExists(tag) ||
      loader.GetResourceCompression(tag) != CResLoader::kCompressionType_Uncompressed) {
    return result;
  }
  const uint size = loader.ResourceSize(tag);

  std::unique_ptr< CInputStream > header = ReadPart(loader, tag, size, 0, 0x60);
  if (!header || header->ReadLong() != kMreaMagic) {
    return result;
  }
  const uint version = header->ReadLong();
  if (version < 12 || version > 15) {
    return result;
  }
  for (int i = 0; i < 13; ++i) {
    header->ReadLong(); // transform, model count
  }
  const uint sectionCount = header->ReadLong();
  header->ReadLong(); // geometry section
  const uint scriptSection = header->ReadLong();
  if (scriptSection >= sectionCount || sectionCount > (size - 0x60) / 4) {
    return result;
  }

  const uint sizesLength = (sectionCount * 4 + 31) & ~31u;
  std::unique_ptr< CInputStream > sizes = ReadPart(loader, tag, size, 0x60, sizesLength);
  if (!sizes) {
    return result;
  }
  uint offset = 0x60 + sizesLength;
  for (uint i = 0; i < scriptSection; ++i) {
    const uint sectionSize = sizes->ReadLong();
    if (sectionSize > size - offset) {
      return result;
    }
    offset += sectionSize;
  }

  std::unique_ptr< CInputStream > scly = ReadPart(loader, tag, size, offset, 12);
  if (!scly || scly->ReadLong() != kSclyMagic) {
    return result;
  }
  scly->ReadLong(); // version
  const uint layerCount = scly->ReadLong();
  if (layerCount == 0 || layerCount > kMaxLayers) {
    return result;
  }
  std::unique_ptr< CInputStream > layerSizes =
      ReadPart(loader, tag, size, offset + 12, layerCount * 4);
  if (!layerSizes) {
    return result;
  }
  std::vector< int > counts;
  counts.reserve(layerCount);
  uint layerOffset = offset + 12 + layerCount * 4;
  for (uint i = 0; i < layerCount; ++i) {
    const uint layerSize = layerSizes->ReadLong();
    std::unique_ptr< CInputStream > layer = ReadPart(loader, tag, size, layerOffset, 5);
    if (layerSize < 5 || layerSize > size - layerOffset || !layer) {
      return result;
    }
    layer->ReadChar();
    counts.push_back(static_cast< int >(layer->ReadLong()));
    layerOffset += layerSize;
  }
  result.swap(counts);
  return result;
}

// Objects the room creates when it loads with its layers as they are now.
int GetAreaCost(CStateManager& mgr, const CWorld& world, TAreaId area) {
  const CAssetId mrea = world.GetArea(area)->GetAreaAssetId();
  auto it = sLayerObjects.find(mrea);
  if (it == sLayerObjects.end()) {
    it = sLayerObjects.emplace(mrea, ReadLayerObjects(mrea)).first;
    if (it->second.empty()) {
      Log.warn("Couldn't read the script layers of MREA 0x{:08X}; it won't count", mrea);
    }
  }
  const std::vector< int >& counts = it->second;
  const CScriptLayerManager& layers = *mgr.WorldLayerState();
  const int layerCount =
      std::min(static_cast< int >(counts.size()), layers.GetAreaLayerCount(area));
  int cost = 0;
  for (int i = 0; i < layerCount; ++i) {
    if (layers.IsLayerActive(area, TLayerId(i))) {
      cost += counts[i];
    }
  }
  return cost;
}

// Notes which candidates have a door between them and the current room, and what it's doing.
// Only the current room's doors decide whether there is one: the other room may not be loaded.
void FindDoors(const CStateManager& mgr, TAreaId current, std::vector< Candidate >& candidates) {
  const CObjectList& objects = mgr.GetObjectListById(kOL_PlatformAndDoor);
  for (int i = objects.GetFirstObjectIndex(); i != -1; i = objects.GetNextObjectIndex(i)) {
    const CScriptDoor* door = TCastToConstPtr< CScriptDoor >(objects[i]);
    if (door == nullptr) {
      continue;
    }
    const TAreaId doorArea = door->GetCurrentAreaId();
    for (Candidate& candidate : candidates) {
      if (doorArea == current && door->IsConnectedToArea(mgr, candidate.area)) {
        candidate.hasDoor = true;
        candidate.doorOpen = candidate.doorOpen || door->IsOpenOrClosing();
        candidate.doorWaiting = candidate.doorWaiting || door->IsWaitingToOpen();
      } else if (doorArea == candidate.area && door->IsConnectedToArea(mgr, current)) {
        // The other room's half of the doorway, still open behind the player.
        candidate.doorOpen = candidate.doorOpen || door->IsOpenOrClosing();
      }
    }
  }
}

std::string DescribeArea(const CWorld& world, TAreaId area, int cost) {
  const CAssetId mrea = world.GetArea(area)->GetAreaAssetId();
  for (const warp::World& catalogWorld : warp::GetWorlds()) {
    for (const warp::Area& catalogArea : catalogWorld.areas) {
      if (catalogArea.mrea == mrea) {
        return fmt::format("{} ({})", catalogArea.name, cost);
      }
    }
  }
  return fmt::format("area {} (MREA 0x{:08X}) ({})", area.Value(), mrea, cost);
}

void LogPick(const CWorld& world, TAreaId current, int currentCost,
             const std::vector< Candidate >& candidates, const std::vector< TAreaId >& skipped,
             int used) {
  const bool over = used > kAreaBudget;
  if (current == sLoggedArea && skipped == sLoggedSkipped && over == sLoggedOver) {
    return;
  }
  sLoggedArea = current;
  sLoggedSkipped = skipped;
  sLoggedOver = over;

  std::string loaded;
  std::string left;
  for (const Candidate& candidate : candidates) {
    const bool picked =
        std::find(skipped.begin(), skipped.end(), candidate.area) == skipped.end();
    std::string& list = picked ? loaded : left;
    if (!list.empty()) {
      list += ", ";
    }
    list += DescribeArea(world, candidate.area, candidate.cost);
  }
  if (over) {
    // Only rooms in view get past the budget, so they alone are over it.
    Log.warn("{} and the rooms in view, {}, need {} objects, more than the {} budgeted",
             DescribeArea(world, current, currentCost), loaded, used, kAreaBudget);
  } else if (!skipped.empty()) {
    Log.info("{} with every room around it is over {} objects. Loading {}; leaving out {}",
             DescribeArea(world, current, currentCost), kAreaBudget, loaded, left);
  }
}

} // namespace

void PickAdjacentAreas(CStateManager& mgr, const CWorld& world, TAreaId current) {
  sPicked.clear();
  sPicked.push_back(current);

  const CGameArea* area = world.GetArea(current);
  std::vector< Candidate > candidates;
  for (int i = 0; i < area->GetDockCount(); ++i) {
    const CGameArea::Dock& dock = area->GetDock(i);
    const int refCount = dock.GetDockRefs().size();
    for (int j = 0; j < refCount; ++j) {
      if (!dock.ShouldLoadOtherArea(j)) {
        continue;
      }
      const TAreaId other = dock.GetConnectedAreaId(j);
      if (other == current || !world.DoesAreaExist(other) || !world.GetArea(other)->IsActive()) {
        continue;
      }
      const bool listed =
          std::find_if(candidates.begin(), candidates.end(), [&](const Candidate& candidate) {
            return candidate.area == other;
          }) != candidates.end();
      if (!listed) {
        candidates.push_back(Candidate{other, 0, false, false, false, kR_Unloaded});
      }
    }
  }

  const int currentCost = GetAreaCost(mgr, world, current);
  int total = currentCost;
  for (Candidate& candidate : candidates) {
    candidate.cost = GetAreaCost(mgr, world, candidate.area);
    total += candidate.cost;
  }
  if (total <= kAreaBudget) {
    for (const Candidate& candidate : candidates) {
      sPicked.push_back(candidate.area);
    }
    sLoggedArea = kInvalidAreaId;
    return;
  }

  // TravelToArea validates the current room first, so its doors are there to look at.
  if (area->IsValidated()) {
    FindDoors(mgr, current, candidates);
  }
  for (Candidate& candidate : candidates) {
    if (area->IsValidated() && (!candidate.hasDoor || candidate.doorOpen)) {
      candidate.rank = kR_Visible;
    } else if (candidate.doorWaiting) {
      candidate.rank = kR_Awaited;
    } else if (world.GetArea(candidate.area)->IsLoaded()) {
      candidate.rank = kR_Loaded;
    } else {
      candidate.rank = kR_Unloaded;
    }
  }
  std::stable_sort(candidates.begin(), candidates.end(),
                   [](const Candidate& a, const Candidate& b) {
                     return a.rank != b.rank ? a.rank < b.rank : a.cost < b.cost;
                   });

  int used = currentCost;
  std::vector< TAreaId > skipped;
  for (const Candidate& candidate : candidates) {
    if (candidate.rank == kR_Visible || used + candidate.cost <= kAreaBudget) {
      sPicked.push_back(candidate.area);
      used += candidate.cost;
    } else {
      skipped.push_back(candidate.area);
    }
  }
  LogPick(world, current, currentCost, candidates, skipped, used);
}

bool IsAdjacentAreaPicked(TAreaId area) {
  return std::find(sPicked.begin(), sPicked.end(), area) != sPicked.end();
}

} // namespace metaforce::objectbudget
