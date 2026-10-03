#pragma once

// The map of a world whose doors the room randomizer moved. Every room stays where its own region
// put it in world space, so on the vanilla map the room behind a moved door is drawn somewhere
// else entirely, often over other rooms (with mixed regions, every region's rooms share one map).
//
// This lays the map out again. Each room gets a rigid transform from world space into map space,
// a turn about Z and a move, chosen so that as many moved doors as possible meet on the map the
// way they do in the game, with no room drawn over another:
//
// - Rooms joined by doors the randomizer left alone keep their vanilla arrangement, as one group.
// - Groups are placed one at a time, each through a moved door joining it to a group already
//   placed, the door closest to where the layout started first, as long as it fits there.
// - Moved doors joined up in loops almost never close up, so some doors can't meet. A group that
//   fits next to none of its doors goes into the nearest free space, as an island.
//
// Overlaps are judged on the rooms' map geometry: each map area's triangles are rasterized into
// columns on a grid in X and Y, each with the range of Z it covers. The layout is the same
// whichever region is loaded as the host, so the map doesn't move around between visits.

#include "MetroidPrime/TGameTypes.hpp"

#include <vector>

class CStateManager;
class CTransform4f;
class CWorld;
class IWorld;

namespace metaforce::maplayout {

// CWorld load, once its map world is cached and its docks lead where the randomizer says: lays out
// the map if any door was moved.
void Build(const CWorld& world);

// ~CWorld.
void Release(const CWorld* world);

// DockPortals::OnPlayerCrossedDock, after the randomizer noted the door: the player went through
// dock `dock` of `area` into `enteredArea`. When its two sides don't meet on the map, the layout is
// worked out again in the background, joining the doors the player went through first, with
// `enteredArea` kept where it is.
void OnDockCrossed(const CWorld& world, int area, int dock, int enteredArea);

// Every frame, on the main thread: takes a layout worked out in the background into use, and lays
// the map out again around the player's room when the Room Rando Map setting was changed.
void Update(const CStateManager& mgr);

// Whether `world` is drawn with a layout of its own rather than the vanilla map.
bool IsActive(const IWorld& world);

// World space to map space for `area`. Only while IsActive.
CTransform4f GetAreaTransform(const IWorld& world, int area);

// The turn about Z in GetAreaTransform, in radians. Zero while not IsActive.
float GetAreaYaw(const IWorld& world, int area);

// CMapWorld::DrawAreas, after the map is drawn: shades where two rooms drawn on the map overlap.
// `drawn` holds, by area, whether the area was drawn; `modelXf` takes map space to the screen.
void DrawOverlaps(const IWorld& world, const std::vector< bool >& drawn,
                  const CTransform4f& modelXf, float alpha);

} // namespace metaforce::maplayout
