#pragma once

#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace metaforce::randomizer {

// What the game gives when a pickup is collected, as CPlayerState operations:
// InitializePowerUp(itemType, capacity) then IncrPickUp(itemType, amount).
struct ItemGrant {
  int itemType = -1;
  int capacity = 0;
  int amount = 0;
};

struct PlacedPickup {
  std::string name;
  std::string model;
  ItemGrant grant;
};

struct StartingItem {
  std::string name;
  ItemGrant grant;
};

// One side of a shuffled door: walking through dock `dock` of room `area` leads out of dock
// `targetDock` of room `targetArea`. Rooms are MREA asset ids; `world` and `targetWorld` are the
// MLVLs they're in, which differ when regions are mixed.
struct DockConnection {
  uint32_t world = 0;
  uint32_t area = 0;
  int dock = -1;
  uint32_t targetWorld = 0;
  uint32_t targetArea = 0;
  int targetDock = -1;
  std::string name;       // "Region / Area / Node"
  std::string targetName; // "Region / Area / Node"
  // The target is a Morph Ball tunnel, which the player has to come out of morphed.
  bool targetMorphBall = false;
};

// A door the door lock randomizer restyles, as randomprime's door types: dock `dock` of room
// `area` (an MREA asset id) in world `world`.
struct DoorLock {
  uint32_t world = 0;
  uint32_t area = 0;
  int dock = -1;
  std::string shield;      // shield color, as Randovania's "shieldType" ("Blue", "Ice Beam", ...)
  std::string blastShield; // blast shield in front of it ("Missile", ...), empty for none
  std::string weakness;    // Randovania's name for the lock, for the spoiler log
  std::string name;        // "Region / Area / Node"
  // False for doors that keep their lock and are only restyled, like the game's own missile
  // blast shields becoming randomprime's.
  bool changed = true;
};

struct Seed {
  // Version 2 added door locks. Version 1 seeds still load, with the game's own doors.
  static constexpr int kFormatVersion = 2;
  static constexpr int kOldestFormatVersion = 1;

  std::string hash;
  std::string seedString;
  nlohmann::json settings;
  std::vector< PlacedPickup > locations; // indexed by pickup index
  std::vector< StartingItem > startingItems;

  std::string startName; // "Region / Area / Node"
  uint32_t startWorld = 0;
  uint32_t startArea = 0;
  std::optional< std::array< float, 3 > > startPosition;
  // Which way the player faces at the start, in radians about the Z axis (0 faces +Y). Unset
  // keeps the facing of the area's own spawn point.
  std::optional< float > startYaw;
  // A start at a door: the game puts the player just inside dock `startDock` of the start area,
  // from the door's own geometry, and startPosition is only the fallback. -1 for other starts.
  int startDock = -1;
  // The start was picked at random, so the UI keeps it a surprise.
  bool randomStart = false;

  // Bit i set when Artifact (Truth + i) is in the item pool rather than given at the start.
  uint32_t shuffledArtifacts = 0;

  // Doors the room randomizer moved, one entry per direction. Empty when rooms aren't shuffled.
  std::vector< DockConnection > docks;

  // Door lock randomizer. With it on, the game's own missile blast shields are replaced (by the
  // entries in doorLocks), and every door listed is restyled.
  bool doorLocksRandomized = false;
  bool blastShieldLockOn = false;
  std::vector< DoorLock > doorLocks;

  // Playthrough spheres, each a list of "Location: Item" lines.
  std::vector< std::vector< std::string > > spheres;
  std::vector< std::string > warnings;

  nlohmann::json ToJson() const;
  static std::optional< Seed > FromJson(const nlohmann::json& json, std::string& error);

  bool Save(const std::filesystem::path& directory, std::string& error) const;
  static std::optional< Seed > Load(const std::filesystem::path& file, std::string& error);

  std::string SpoilerText(const std::vector< std::string >& locationNames) const;
};

} // namespace metaforce::randomizer
