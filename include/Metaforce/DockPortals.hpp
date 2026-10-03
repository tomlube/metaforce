#pragma once

// Doors that lead somewhere other than where the room geometry says they do, as made by the room
// randomizer. Every room stays where it is in world space. Walking through a moved door carries
// the player by the rigid transform between the two dock planes, and the room behind a moved door
// is drawn through the doorway from a camera moved by that same transform, so the far room
// appears exactly where the doorway ends.

#include "MetroidPrime/TGameTypes.hpp"

class CFrustumPlanes;
class CGameProjectile;
class CStateManager;
class CTransform4f;
class CVector3f;
class CWorld;

namespace metaforce::portals {

// Takes points around `dock` of `area` to the matching place around the dock it currently leads
// to. False when the two docks coincide (a vanilla door) or the dock leads nowhere.
bool GetDockTransform(const CWorld& world, TAreaId area, int dock, CTransform4f& out);

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
// doorway in depth, so the main pass covers the portals everywhere except through the doorways.
void ResetDepthForMainPass(const CStateManager& mgr);

// Diagnostics (METAFORCE_PORTAL_LOG): an area drawn in the current pass, with its PVS state.
void LogPassArea(TAreaId area, int visState);

// Whether the main pass should leave out the sky. When the camera is standing in a portal's
// doorway the near plane clips the doorway's depth seal, and the sky would cover the portal.
bool SkipMainPassSky();

// The area whose sky the current pass draws, or kInvalidAreaId for none. Normally the pass's own
// area; but in a frame whose main pass leaves out the sky, the portal passes' sky is what shows
// behind the whole view, so they all draw one chosen sky: the room the camera is in if it needs a
// sky, or else the first room through a doorway that does.
TAreaId GetPassSkyArea(TAreaId vanilla);

// The sky's model transform for the current pass, centered on `eye`. A portal pass drawing another
// area's sky turns it to line up with how that area's own pass would have drawn it.
CTransform4f GetPassSkyTransform(const CVector3f& eye);

} // namespace metaforce::portals
