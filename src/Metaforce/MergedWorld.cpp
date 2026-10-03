#include "Metaforce/MergedWorld.hpp"

#include "Metaforce/Randomizer/Randomizer.hpp"

#include "Kyoto/Audio/CStreamAudioManager.hpp"
#include "Kyoto/Basics/CCast.hpp"
#include "Kyoto/CResFactory.hpp"
#include "Kyoto/CResLoader.hpp"
#include "Kyoto/CSimplePool.hpp"
#include "Kyoto/Graphics/CGraphics.hpp"
#include "Kyoto/Graphics/CModel.hpp"
#include "Kyoto/Streams/CInputStream.hpp"
#include "MetaRender/CCubeRenderer.hpp"
#include "MetroidPrime/CGameArea.hpp"
#include "MetroidPrime/CInGameTweakManager.hpp"
#include "MetroidPrime/CMapWorld.hpp"
#include "MetroidPrime/CScriptLayerManager.hpp"
#include "MetroidPrime/CScriptMailbox.hpp"
#include "MetroidPrime/CStateManager.hpp"
#include "MetroidPrime/CWorld.hpp"
#include "MetroidPrime/Player/CGameState.hpp"
#include "MetroidPrime/Player/CPlayer.hpp"
#include "MetroidPrime/Player/CWorldState.hpp"
#include "MetroidPrime/ScriptObjects/CScriptAreaAttributes.hpp"
#include "MetroidPrime/ScriptObjects/CScriptStreamedMusic.hpp"

#include <borealis/log.hpp>

#include <algorithm>
#include <cstdlib>
#include <map>
#include <memory>
#include <vector>

namespace metaforce::merged {
namespace {

constexpr borealis::Log Log{"merged"};

// Set METAFORCE_NO_MERGE=1 to load mixed regions one at a time, with fades between them.
const bool sDisabled = std::getenv("METAFORCE_NO_MERGE") != nullptr;

constexpr uint kMlvlMagic = 0xDEAFBABE;
// TEditorId keeps the area index in 10 bits.
constexpr int kMaxAreas = 0x3ff;
constexpr uint kAreaMask = 0x3ffu << 16;

struct Source {
  CAssetId mlvl;
  int first; // index of its first area in the loaded world
  int count;
  std::vector< CRelay > relays;
  std::vector< CAssetId > mapAreas;
  std::vector< std::pair< int, CAssetId > > soundGroups;
  CAssetId skybox = kInvalidAssetId;
  rstl::string music;
  uchar musicVolume = 127;
};

struct State {
  const CWorld* owner = nullptr;
  CAssetId host = kInvalidAssetId;
  int hostCount = 0;
  std::vector< Source > foreign;
  rstl::string hostMusic;
  uchar hostMusicVolume = 127;
  int musicGroup = 0; // region whose default music is playing
};

State sState;

// Skies of the appended regions, by CMDL, loaded as they're first needed.
std::map< CAssetId, TCachedToken< CModel > > sSkies;

// Region of each area for queries: 0 for the host, 1 + index into sState.foreign for the others.
// -1 means "where the player is".
int sQueryGroup = -1;

const Source* FindSource(int area) {
  for (const Source& source : sState.foreign) {
    if (area >= source.first && area < source.first + source.count) {
      return &source;
    }
  }
  return nullptr;
}

const Source* FindSource(CAssetId mlvl) {
  for (const Source& source : sState.foreign) {
    if (source.mlvl == mlvl) {
      return &source;
    }
  }
  return nullptr;
}

int GroupOf(int area) {
  if (area < sState.hostCount) {
    return 0;
  }
  for (int i = 0; i < static_cast< int >(sState.foreign.size()); ++i) {
    const Source& source = sState.foreign[i];
    if (area >= source.first && area < source.first + source.count) {
      return i + 1;
    }
  }
  return 0;
}

int PlayerGroup(const CStateManager& mgr) {
  const TAreaId area = mgr.GetNextAreaId();
  return area == kInvalidAreaId ? 0 : GroupOf(area.Value());
}

int QueryGroup(const CStateManager& mgr) {
  return sQueryGroup >= 0 ? sQueryGroup : PlayerGroup(mgr);
}

// The regions the active seed's cross-region doors reach from `host`, host first.
std::vector< CAssetId > MixedWorlds(CAssetId host) {
  std::vector< CAssetId > worlds{host};
  const randomizer::Seed* seed = randomizer::GetActiveSeed();
  if (seed == nullptr) {
    return worlds;
  }
  for (size_t i = 0; i < worlds.size(); ++i) {
    for (const randomizer::DockConnection& dock : seed->docks) {
      if (dock.world == worlds[i] && dock.targetWorld != dock.world &&
          std::find(worlds.begin(), worlds.end(), dock.targetWorld) == worlds.end()) {
        worlds.push_back(dock.targetWorld);
      }
    }
  }
  return worlds;
}

std::vector< CAssetId > ReadMapAreas(CResLoader& loader, CAssetId mapw) {
  std::vector< CAssetId > result;
  if (mapw == kInvalidAssetId || !loader.ResourceExists(SObjectTag('MAPW', mapw))) {
    return result;
  }
  std::unique_ptr< CInputStream > in(loader.LoadNewResourceSync(SObjectTag('MAPW', mapw), nullptr));
  if (!in) {
    return result;
  }
  in->ReadLong();
  in->ReadLong();
  const int count = in->ReadLong();
  result.reserve(count);
  for (int i = 0; i < count; ++i) {
    result.push_back(in->ReadLong());
  }
  return result;
}

// Mirrors the MLVL reading in CWorld::CheckWorldComplete.
bool AppendWorld(CAssetId mlvl, rstl::vector< rstl::auto_ptr< CGameArea > >& areas) {
  CResLoader& loader = gpResourceFactory->GetResLoader();
  std::unique_ptr< CInputStream > in(loader.LoadNewResourceSync(SObjectTag('MLVL', mlvl), nullptr));
  if (!in) {
    Log.error("Couldn't load MLVL 0x{:08X}", mlvl);
    return false;
  }
  CInputStream& r = *in;
  if (r.ReadLong() != kMlvlMagic) {
    Log.error("MLVL 0x{:08X} has a bad header", mlvl);
    return false;
  }
  const int version = r.ReadLong();
  r.ReadLong(); // name STRG
  if (static_cast< uint >(version) >= 15) {
    r.ReadLong(); // SAVW
  }
  Source source;
  source.mlvl = mlvl;
  if (static_cast< uint >(version) >= 12) {
    source.skybox = r.ReadLong();
  }
  source.first = areas.size();
  if (static_cast< uint >(version) >= 17) {
    const rstl::vector< CRelay > relays(r);
    for (int i = 0; i < relays.size(); ++i) {
      source.relays.push_back(relays[i]);
    }
  }
  source.count = r.ReadLong();
  r.ReadLong();
  if (source.first + source.count > kMaxAreas) {
    Log.error("Can't append MLVL 0x{:08X}: {} areas would be more than {}", mlvl,
              source.first + source.count, kMaxAreas);
    return false;
  }
  for (int i = 0; i < source.count; ++i) {
    CGameArea* area = rs_new CGameArea(r, source.first + i, version);
    area->OffsetAreaReferences(source.first);
    areas.push_back(area);
  }

  const CAssetId mapw = r.ReadLong();
  r.ReadChar(); // world-level script objects, which Prime doesn't use
  r.ReadLong();
  if (static_cast< uint >(version) > 10) {
    const int audioGroupCount = r.ReadLong();
    for (int i = 0; i < audioGroupCount; ++i) {
      const int groupId = r.ReadLong();
      const CAssetId agsc = r.ReadLong();
      source.soundGroups.emplace_back(groupId, agsc);
    }
  }
  if (static_cast< uint >(version) > 12) {
    // Same as CWorld's default music, tweaks included.
    source.music = rstl::string(r);
    const rstl::string trackKey = CInGameTweakManager::GetIdentifierForWorldDefaultMusic(mlvl);
    if (gpTweakManager->HasTweakValue(trackKey)) {
      source.music = gpTweakManager->GetTweakValue(trackKey)->GetAudio().GetFileName();
      source.musicVolume = static_cast< uchar >(
          CCast::ToInt8(127.f * gpTweakManager->GetTweakValue(trackKey)->GetAudio().GetVolume()));
    }
  }
  // Sets up the region's layer state if it was never visited.
  CWorldLayers::ReadWorldLayers(r, version, mlvl);

  source.mapAreas = ReadMapAreas(loader, mapw);
  if (static_cast< int >(source.mapAreas.size()) != source.count) {
    Log.warn("MAPW of 0x{:08X} has {} areas for {} rooms", mlvl, source.mapAreas.size(),
             source.count);
  }
  Log.info("Appended {} areas of MLVL 0x{:08X} at {}", source.count, mlvl, source.first);
  sState.foreign.push_back(std::move(source));
  return true;
}

// Diagnostics: logs pairs of host and appended areas whose bounds overlap.
void LogOverlaps(const rstl::vector< rstl::auto_ptr< CGameArea > >& areas) {
  int overlaps = 0;
  for (int i = sState.hostCount; i < static_cast< int >(areas.size()); ++i) {
    for (int j = 0; j < i; ++j) {
      if (GroupOf(i) != GroupOf(j) && areas[i]->GetAABB().DoBoundsOverlap(areas[j]->GetAABB())) {
        ++overlaps;
      }
    }
  }
  Log.info("{} pairs of areas from different regions overlap in space", overlaps);
}

} // namespace

void AppendForeignAreas(const CWorld* owner, CAssetId hostMlvl,
                        rstl::vector< rstl::auto_ptr< CGameArea > >& areas) {
  sSkies.clear();
  sState = State();
  sState.owner = owner;
  sState.host = hostMlvl;
  sState.hostCount = areas.size();
  sQueryGroup = -1;
  if (sDisabled) {
    return;
  }
  const std::vector< CAssetId > worlds = MixedWorlds(hostMlvl);
  for (size_t i = 1; i < worlds.size(); ++i) {
    if (!AppendWorld(worlds[i], areas)) {
      break;
    }
  }
  if (!sState.foreign.empty()) {
    LogOverlaps(areas);
  }
}

void AppendForeignMapAreas(CMapWorld& map) {
  rstl::vector< CAssetId > ids;
  for (const Source& source : sState.foreign) {
    for (int i = 0; i < source.count; ++i) {
      if (i < static_cast< int >(source.mapAreas.size())) {
        ids.push_back(source.mapAreas[i]);
      } else {
        // Keeps the map areas lined up with the rooms; the map shows the wrong room here.
        ids.push_back(source.mapAreas.empty() ? kInvalidAssetId : source.mapAreas.front());
      }
    }
  }
  map.SetAppendedMapAreas(sState.hostCount, ids);
}

rstl::vector< rstl::pair< int, CAssetId > > GetForeignSoundGroups() {
  rstl::vector< rstl::pair< int, CAssetId > > result;
  for (const Source& source : sState.foreign) {
    for (const auto& [groupId, agsc] : source.soundGroups) {
      result.push_back(rstl::pair< int, CAssetId >(groupId, agsc));
    }
  }
  return result;
}

rstl::vector< CAssetId > GetForeignWorlds() {
  rstl::vector< CAssetId > result;
  for (const Source& source : sState.foreign) {
    result.push_back(source.mlvl);
  }
  return result;
}

void SetHostDefaultAudio(const rstl::string& track, uchar volume) {
  sState.hostMusic = track;
  sState.hostMusicVolume = volume;
}

void ReleaseWorld(const CWorld* owner) {
  if (owner != sState.owner) {
    return;
  }
  sSkies.clear();
  sState = State();
  sQueryGroup = -1;
}

void OnPlayerAreaChanged(TAreaId area) {
  if (!IsMerged() || area == kInvalidAreaId) {
    return;
  }
  const int group = GroupOf(area.Value());
  if (group == sState.musicGroup) {
    return;
  }
  sState.musicGroup = group;
  const rstl::string& track = group == 0 ? sState.hostMusic : sState.foreign[group - 1].music;
  const uchar volume =
      group == 0 ? sState.hostMusicVolume : sState.foreign[group - 1].musicVolume;
  if (track.size() != 0 && !CScriptStreamedMusic::IsAudioTrackNameSoftware(track)) {
    CStreamAudioManager::SetDefaultAudio(track, 1.f, 1.f, volume);
  }
}

bool DrawForeignSky(const CWorld& world, TAreaId area, const CTransform4f& xf) {
  const Source* source =
      area == kInvalidAreaId || !world.DoesAreaExist(area) ? nullptr : FindSource(area.Value());
  if (source == nullptr) {
    return false;
  }
  // The room's own sky, as CWorld::Update picks it, or else its region's.
  CAssetId skyId = source->skybox;
  const CGameArea& gameArea = world.GetAreaAlways(area);
  if (gameArea.IsPostConstructed()) {
    const CScriptAreaAttributes* attrs = gameArea.GetPostConstructed()->mAreaAttributes;
    if (attrs != nullptr && attrs->GetSkyModel() != kInvalidAssetId) {
      skyId = attrs->GetSkyModel();
    }
  }
  if (skyId == kInvalidAssetId) {
    return true;
  }
  auto it = sSkies.find(skyId);
  if (it == sSkies.end()) {
    it = sSkies
             .emplace(skyId,
                      TCachedToken< CModel >(gpSimplePool->GetObj(SObjectTag('CMDL', skyId))))
             .first;
    it->second.Lock();
  }
  if (!it->second.TryCache()) {
    return true;
  }
  CModel* sky = it->second.GetObject();
  sky->Touch(0);
  if (!sky->IsLoaded(0)) {
    return true;
  }
  // Same as CWorld::DrawSky.
  CGraphics::DisableAllLights();
  gpRender->SetModelMatrix(xf);
  gpRender->SetAmbientColor(CColor::White());
  CGraphics::SetDepthRange(0.999f, 1.f);
  sky->Draw(CModelFlags::Normal().DepthCompareUpdate(true, false));
  CGraphics::SetDepthRange(0.125f, 1.f);
  return true;
}

bool IsMerged() { return !sState.foreign.empty(); }

bool IsWorldLoaded(CAssetId mlvl) { return mlvl == sState.host || FindSource(mlvl) != nullptr; }

bool IsForeignArea(TAreaId area) {
  return area != kInvalidAreaId && area.Value() >= sState.hostCount && IsMerged();
}

CAssetId GetSourceWorld(CAssetId loadedWorld, TAreaId area) {
  if (area == kInvalidAreaId) {
    return loadedWorld;
  }
  const Source* source = FindSource(area.Value());
  return source != nullptr ? source->mlvl : loadedWorld;
}

int GetSourceAreaIndex(TAreaId area) {
  const Source* source = area == kInvalidAreaId ? nullptr : FindSource(area.Value());
  return source != nullptr ? area.Value() - source->first : area.Value();
}

CScriptLayerManager* GetSourceLayerState(TAreaId area) {
  const Source* source = area == kInvalidAreaId ? nullptr : FindSource(area.Value());
  if (source == nullptr || gpGameState == nullptr) {
    return nullptr;
  }
  return gpGameState->StateForWorld(source->mlvl).GetLayerState().GetPtr();
}

int ToLoadedAreaIndex(TAreaId area, int sourceIndex) {
  const Source* source = area == kInvalidAreaId ? nullptr : FindSource(area.Value());
  if (source == nullptr || sourceIndex < 0 || sourceIndex >= source->count) {
    return sourceIndex;
  }
  return source->first + sourceIndex;
}

uint ToLoadedEditorId(TAreaId area, uint editorId) {
  const Source* source = area == kInvalidAreaId ? nullptr : FindSource(area.Value());
  if (source == nullptr) {
    return editorId;
  }
  const int sourceArea = (editorId & kAreaMask) >> 16;
  if (sourceArea >= source->count) {
    return editorId;
  }
  return (editorId & ~kAreaMask) | (static_cast< uint >(source->first + sourceArea) << 16);
}

uint ToSourceEditorId(uint editorId, CAssetId& world) {
  const Source* source = FindSource(static_cast< int >((editorId & kAreaMask) >> 16));
  if (source == nullptr) {
    return editorId;
  }
  world = source->mlvl;
  const int sourceArea = ((editorId & kAreaMask) >> 16) - source->first;
  return (editorId & ~kAreaMask) | (static_cast< uint >(sourceArea) << 16);
}

CScriptMailbox* GetMailboxForEditorId(CStateManager& mgr, uint& editorId) {
  CAssetId world = kInvalidAssetId;
  const uint sourceId = ToSourceEditorId(editorId, world);
  if (world == kInvalidAssetId || gpGameState == nullptr) {
    return mgr.Mailbox();
  }
  editorId = sourceId;
  return gpGameState->StateForWorld(world).Mailbox().GetPtr();
}

bool SendForeignRelayMsgs(TAreaId area, CStateManager& mgr) {
  const Source* source = area == kInvalidAreaId ? nullptr : FindSource(area.Value());
  if (source == nullptr || gpGameState == nullptr) {
    return false;
  }
  CScriptMailbox& mailbox = *gpGameState->StateForWorld(source->mlvl).Mailbox();
  const int sourceArea = area.Value() - source->first;
  // Same as CScriptMailbox::SendMsgs, with the targets moved into the loaded world.
  bool hasActiveRelays = false;
  for (const CRelay& relay : source->relays) {
    if (relay.GetTargetId().AreaNum() != sourceArea || !mailbox.HasMsg(relay.GetRelayId())) {
      continue;
    }
    const TEditorId target(ToLoadedEditorId(area, relay.GetTargetId().Value()));
    mgr.SendScriptMsg(kInvalidUniqueId, target, EScriptObjectMessage(relay.GetMessage()), kSS_Any);
    hasActiveRelays = hasActiveRelays || relay.GetActive();
  }
  if (hasActiveRelays) {
    for (const CRelay& relay : source->relays) {
      if (relay.GetTargetId().AreaNum() == sourceArea && relay.GetActive() &&
          mailbox.HasMsg(relay.GetRelayId())) {
        mailbox.RemoveMsg(relay.GetRelayId());
      }
    }
  }
  return true;
}

TAreaId GetQueryArea(const CStateManager& mgr, const CEntity& ent) {
  const CEntity* player = mgr.GetPlayer();
  return &ent == player ? mgr.GetNextAreaId() : ent.GetCurrentAreaId();
}

bool SharesSpace(const CStateManager& mgr, TAreaId area) {
  if (!IsMerged() || area == kInvalidAreaId) {
    return true;
  }
  return GroupOf(area.Value()) == QueryGroup(mgr);
}

bool SharesSpace(const CStateManager& mgr, TAreaId actor, TAreaId other) {
  if (!IsMerged()) {
    return true;
  }
  const int actorGroup = actor == kInvalidAreaId ? QueryGroup(mgr) : GroupOf(actor.Value());
  const int otherGroup = other == kInvalidAreaId ? PlayerGroup(mgr) : GroupOf(other.Value());
  return actorGroup == otherGroup;
}

QueryScope::QueryScope(TAreaId area) : mPrevious(sQueryGroup) {
  if (IsMerged() && area != kInvalidAreaId) {
    sQueryGroup = GroupOf(area.Value());
  }
}

QueryScope::~QueryScope() { sQueryGroup = mPrevious; }

} // namespace metaforce::merged
