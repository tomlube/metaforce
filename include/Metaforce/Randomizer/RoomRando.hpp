#pragma once

#include "Metaforce/Randomizer/Logic.hpp"

#include <random>
#include <set>
#include <string>
#include <vector>

namespace metaforce::randomizer {

// Two-way room randomizer, after Randovania's Metroid Prime room rando: within each region, every
// shuffleable door is paired with another door, both ways. On top of Randovania's rules, doors are
// only paired when they're the same kind and their dock planes match in size and facing, so walking
// through one lines up exactly with coming out of the other.
//
// With mixRegions, the doors of every mixable region go into one pool, so a door can lead into
// another region. The rest keep a pool of their own.

struct RoomRandoOptions {
  std::set< std::string > excludedRegions; // region names left vanilla
  bool morphBallDoors = true;
  bool mixRegions = false;
};

// Returns the node each dock node now leads to, indexed by node, -1 for docks left alone. Empty
// with `error` set when no valid layout was found.
std::vector< int > ShuffleRooms(const Database& db, const RoomRandoOptions& options,
                                std::mt19937_64& rng, std::string& error);

// Whether a dock node can take part in the room randomizer.
bool IsShuffleableDock(const Database& db, const RoomRandoOptions& options, int node);

// Whether a region's doors join the shared pool when regions are mixed.
bool IsMixableRegion(const Database& db, int region);

// Number of doors in a region the room randomizer could shuffle.
int CountShuffleableDocks(const Database& db, int region);

} // namespace metaforce::randomizer
