#pragma once

// Regions the room randomizer mixes are loaded as one world. When a world loads, the areas of
// every region its cross-region doors lead to (directly or through other regions) are appended
// after its own, so those doors become ordinary moved doors that DockPortals draws and carries
// the player through, with no load in between.
//
// Each area keeps the coordinates of its own region, so rooms of different regions overlap in
// world space. Collision and actor queries only see areas of one region at a time: the region of
// the actor asking, or of the player's area when nobody in particular is asking.
//
// Appended areas keep their per-region state (script layers, memory relays) in their own
// region's CWorldState. Their editor ids are moved to their index in the loaded world so they
// don't collide with the host's; ToSourceEditorId maps them back.

#include "Kyoto/SObjectTag.hpp"
#include "MetroidPrime/TGameTypes.hpp"

#include "rstl/auto_ptr.hpp"
#include "rstl/pair.hpp"
#include "rstl/string.hpp"
#include "rstl/vector.hpp"

class CEntity;
class CGameArea;
class CMapWorld;
class CScriptLayerManager;
class CScriptMailbox;
class CStateManager;
class CTransform4f;
class CWorld;

namespace metaforce::merged {

// CWorld load, after the world's own areas: appends the areas of the regions mixed with it.
void AppendForeignAreas(const CWorld* owner, CAssetId hostMlvl,
                        rstl::vector< rstl::auto_ptr< CGameArea > >& areas);

// CWorld load, once the map world is cached: gives it a map area for every appended area.
void AppendForeignMapAreas(CMapWorld& map);

// The audio groups (group id, AGSC) of the appended regions, which their enemies and objects
// play their sounds from.
rstl::vector< rstl::pair< int, CAssetId > > GetForeignSoundGroups();

// The appended regions, in the order they were appended.
rstl::vector< CAssetId > GetForeignWorlds();

// CWorld load, after reading its own default music: lets the music switch back to it when the
// player comes back into the host region.
void SetHostDefaultAudio(const rstl::string& track, uchar volume);

// ~CWorld: lets go of everything kept for the world's appended regions, unless another world
// was loaded since.
void ReleaseWorld(const CWorld* owner);

// CStateManager::SetCurrentAreaId: switches the default music when the player went into another
// region.
void OnPlayerAreaChanged(TAreaId area);

// CWorld::DrawSky for a pass drawn from `area`: draws the sky of an appended region instead of the
// host's. False for host areas, which keep the host's sky.
bool DrawForeignSky(const CWorld& world, TAreaId area, const CTransform4f& xf);

// Whether the loaded world has appended areas.
bool IsMerged();

// Whether `mlvl` is the loaded world or one of the regions appended to it.
bool IsWorldLoaded(CAssetId mlvl);

// Whether `area` of the loaded world was appended from another region.
bool IsForeignArea(TAreaId area);

// The region `area` comes from, and its index there. Host areas map to themselves.
CAssetId GetSourceWorld(CAssetId loadedWorld, TAreaId area);
int GetSourceAreaIndex(TAreaId area);

// The CWorldState of the region `area` comes from, for script layers and memory relays.
CScriptLayerManager* GetSourceLayerState(TAreaId area);

// An area index read from the files of `area`'s region (a dock's own area): its index in the
// loaded world.
int ToLoadedAreaIndex(TAreaId area, int sourceIndex);

// An object of `area` read from its region's files: its editor id in the loaded world.
uint ToLoadedEditorId(TAreaId area, uint editorId);

// An editor id of the loaded world: the region it was read from and its editor id there. Ids of
// host objects come back unchanged with `world` left alone.
uint ToSourceEditorId(uint editorId, CAssetId& world);

// CScriptMemoryRelay: the mailbox an object's relay state lives in, and its id there.
CScriptMailbox* GetMailboxForEditorId(CStateManager& mgr, uint& editorId);

// CStateManager::AreaLoaded: delivers the memory relay messages of an appended area, from its own
// region's relays and mailbox. False for host areas, which the game handles itself.
bool SendForeignRelayMsgs(TAreaId area, CStateManager& mgr);

// Collision and actor queries.

// The area `ent` counts as being in for queries. The player's own area id isn't kept up to date,
// so the player is wherever the state manager says.
TAreaId GetQueryArea(const CStateManager& mgr, const CEntity& ent);

// Whether queries made now see `area`: it's in the same region as the current query context, and
// not a room DockPortals separates from the area asking. Also used when the world isn't merged.
bool SharesSpace(const CStateManager& mgr, TAreaId area);

// Whether `actor`'s queries should see `other`. Entities without an area count as being where the
// player is.
bool SharesSpace(const CStateManager& mgr, TAreaId actor, TAreaId other);

// Makes the queries made in this scope those of an actor in `area`.
class QueryScope {
public:
  explicit QueryScope(TAreaId area);
  ~QueryScope();

private:
  int mPrevious;
  int mPreviousArea;
};

} // namespace metaforce::merged
