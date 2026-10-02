#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// In-game warping for the Warp window. The catalog is read from each world's MLVL, so it covers
// every room and script layer on the disc.
namespace metaforce::warp {

struct Layer {
  std::string name;
  bool defaultActive;
};

struct Area {
  int index; // TAreaId within the world
  uint32_t mrea;
  std::string name;
  std::vector< Layer > layers;
};

struct World {
  uint32_t mlvl;
  std::string name;
  std::vector< Area > areas; // Sorted by name
};

// Builds the catalog on first use. Empty until the game's paks are loaded.
const std::vector< World >& GetWorlds();

// True while a game is in progress and a warp can be requested.
bool CanWarp();

// The world and area the player is in, or false outside a game.
bool GetCurrentLocation(uint32_t& mlvl, int& area);

// Active layers of an area in the current game, falling back to the MLVL defaults.
uint64_t GetLayerBits(const World& world, const Area& area);

// Leaves the current game and reloads it in the given area, keeping the player's progress.
// Without layerBits, the area keeps its current layer state.
void RequestWarp(uint32_t mlvl, int area, std::optional< uint64_t > layerBits);

// CMFGameLoader construction: moves the game being loaded to the requested area.
void ApplyPendingWarp();

} // namespace metaforce::warp
