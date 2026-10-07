#pragma once

// Doors that lead somewhere other than where the room geometry says they do, as made by the room
// randomizer. Every room stays where it is in world space. Walking through a moved door carries
// the player by the rigid transform between the two dock planes, and the room behind a moved door
// is drawn through the doorway from a camera moved by that same transform, so the far room
// appears exactly where the doorway ends.

#include "MetroidPrime/TGameTypes.hpp"

class CAABox;
class CActor;
class CDamageInfo;
class CFrustumPlanes;
class CGameProjectile;
class CMaterialFilter;
class CStateManager;
class CTransform4f;
class CVector3f;
class CWorld;

namespace metaforce::portals {

// Takes points around `dock` of `area` to the matching place around the dock it currently leads
// to. False when the two docks coincide (a vanilla door) or the dock leads nowhere.
bool GetDockTransform(const CWorld& world, TAreaId area, int dock, CTransform4f& out);

// CScriptTrigger: the player's touch bounds in the space of `area`, while the player is in another
// room just inside a moved doorway leading to `area`, and in `toPlayer` the turn from `area`'s
// space into the player's. Rooms joined by a vanilla door share their space, so a trigger reaching
// through the doorway (like the lifts in Elder Chamber's ceiling) keeps holding the player on the
// far side; through a moved door that part of the trigger is in another room's space. False when
// the player is in `area` or not by such a doorway.
bool GetPlayerBoundsInArea(const CStateManager& mgr, TAreaId area, CAABox& bounds,
                           CTransform4f& toPlayer);

// CScriptDock: the player crossed `dock` of `area`. Carries the player and cameras through if
// the door was moved.
void OnPlayerCrossedDock(CStateManager& mgr, TAreaId area, int dock);

// CStateManager::Update, after actors think and before the cameras update: carries the player through a moved door
// the moment they cross its plane. CScriptDock notices crossings a few frames late, by which
// time the player has dropped a little with no floor under them.
void UpdatePlayerCrossing(CStateManager& mgr);

// CEnergyProjectile::Think, after the shot moves: carries it through a moved door it went through
// this frame, and keeps its area up to date as it goes through doors.
void UpdateProjectileCrossing(CStateManager& mgr, CGameProjectile& projectile);

// Whether shots keep their own area rather than following the player's. True while any loaded
// room has a moved door.
bool TracksProjectileAreas();

// CPowerBomb: radius damage from `damager` at `pos` also reaches through the open moved doors of
// its room that are within `info`'s radius, into the rooms behind them. A blast is centered in its
// own room, so without this it never touches what stands just past a moved doorway.
void ApplyDamageThroughDocks(CStateManager& mgr, const CActor& damager, const CVector3f& pos,
                             const CDamageInfo& info, const CMaterialFilter& filter);

// Where the segment `from` -> `to` goes in through `dock` of `area`, from outside the room, nudged
// just inside. False when it doesn't cross the doorway.
bool GetDoorwayEntry(const CWorld& world, TAreaId area, int dock, const CVector3f& from,
                     const CVector3f& to, CVector3f& out);

// Collision and actor queries. Two loaded rooms whose doorways meet in world space, but that no
// door in that place joins any more, are separated: neither collides with or sees the other.
// Otherwise a player standing in a moved doorway runs into the closed door and walls of the room
// that doorway used to lead to, and gets shoved back out.
bool AreAreasSeparated(const CStateManager& mgr, TAreaId a, TAreaId b);

// Whether any loaded rooms are separated that way.
bool HasSeparatedAreas(const CStateManager& mgr);

// Rendering. Each frame is drawn from a root area: the area the camera is in, which is the
// player's area unless a trailing camera is still on the far side of a moved door. The main pass
// draws the root and its vanilla neighbors, then each moved door of the root is drawn as a portal
// pass of the area behind it.

// CStateManager::PreRender: works out the root area and portals for this frame.
void PrepareFrame(const CStateManager& mgr);

// The transform applied to the camera to draw `area` this frame, or null when the area isn't
// drawn at all. Identity for the root and its vanilla neighbors in the usual case.
const CTransform4f* GetAreaCameraTransform(TAreaId area);

// CStateManager::DrawWorld: draws every portal pass by calling DrawWorld again for each.
// Returns whether any portal was drawn, in which case ResetDepthForMainPass has to follow.
bool DrawPortalPasses(const CStateManager& mgr);

// Whether DrawWorld is currently drawing a portal rather than the main view.
bool InPortalPass();

// The transform applied to the camera in the current pass.
const CTransform4f& GetPassCameraTransform();

// Whether this frame is drawn from the far side of a moved door the camera has gone through ahead
// of the player.
bool IsRootShifted();

// The area the current pass treats as the one the camera is in.
TAreaId GetPassVisArea(TAreaId vanilla);

// Whether `area` is drawn in the current pass.
bool IsAreaInPass(TAreaId area);

// After the main view is set up: clears depth left by the portal passes and seals each portal's
// doorway in depth, so the main pass, sky included, covers the portals everywhere except through
// the doorways. Parts of a doorway nearer than the near plane are sealed at the near plane, so the
// seal holds with the camera standing in the doorway.
void ResetDepthForMainPass(const CStateManager& mgr);

// Diagnostics (METAFORCE_PORTAL_LOG): an area drawn in the current pass, with its PVS state.
void LogPassArea(TAreaId area, int visState);

} // namespace metaforce::portals
