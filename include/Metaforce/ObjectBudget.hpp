#pragma once

// Keeps the rooms loaded around the player within the object limit (kMaxObjects). A room loads
// every room behind a dock it flags to preload; hubs like Gathering Hall and Great Tree Hall flag
// all of theirs, which the game can afford because those neighbours are small corridors. The room
// randomizer can put the largest rooms in the game behind every one of those doors, and loading all
// of them at once fills the object list ("Object list full!").
//
// When everything fits, which it always does in the unrandomized game, every flagged room loads as
// usual. Otherwise rooms in view (through a doorway without a door, or a door open or swinging
// shut) always load. Then come rooms a door is waiting to open onto, rooms already loaded, and the
// rest, smallest first within each, while they fit. A room left out loads when one of its doors is
// shot: the door waits for it like any door into an unloaded room, and a room further down the
// list is dropped to make space.

#include "MetroidPrime/TGameTypes.hpp"

class CStateManager;
class CWorld;

namespace metaforce::objectbudget {

// CWorld::TravelToArea, before it schedules the rooms around `current`: picks the ones that fit.
void PickAdjacentAreas(CStateManager& mgr, const CWorld& world, TAreaId current);

// CWorld::TravelToArea: whether a room around the current one was picked to load or stay loaded.
bool IsAdjacentAreaPicked(TAreaId area);

} // namespace metaforce::objectbudget
