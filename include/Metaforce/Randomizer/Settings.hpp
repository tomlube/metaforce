#pragma once

#include <nlohmann/json_fwd.hpp>

#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace metaforce::randomizer {

// Randovania's Metroid Prime pickup database, as exported by tools/randomizer/export_randovania.py.

struct StandardPickupDef {
  std::string name;
  std::string category;
  std::string model;
  int itemType = -1;
  bool major = true;
  bool hidden = false;
  std::vector< std::string > progression;
  std::vector< std::string > ammo;
  int defaultShuffled = 0;
  int defaultStarting = 0;
  int defaultAmmo = 0;
};

struct AmmoPickupDef {
  std::string name;
  std::string category;
  std::string model;
  std::string resource;
  int itemType = -1;
  bool refill = false;
  int defaultCount = 0;
  int defaultAmmo = 0;
};

struct ArtifactDef {
  std::string name;
  std::string resource;
  std::string model;
  int itemType = -1;
};

struct PickupDatabase {
  std::vector< StandardPickupDef > standard;
  std::vector< AmmoPickupDef > ammo;
  std::vector< ArtifactDef > artifacts;
  int defaultArtifactTarget = 12;

  bool Load(const std::filesystem::path& path, std::string& error);
  const StandardPickupDef* FindStandard(const std::string& name) const;
  const AmmoPickupDef* FindAmmo(const std::string& name) const;
};

struct StandardPickupState {
  int shuffled = 0;
  int starting = 0;
  int ammo = 0;
};

struct AmmoPickupState {
  int count = 0;
  int ammo = 0;
};

inline constexpr const char* kRandomStartingLocation = "random";

struct Settings {
  static constexpr int kVersion = 1;

  std::string seedString;
  int trickLevel = 0;      // 0 (disabled) to 5 (ludicrous), applied to every trick
  int damageStrictness = 1; // index into kDamageStrictnessValues
  int artifactTarget = 6;
  // "Region|Area|Node", kRandomStartingLocation, or empty for the database default
  std::string startingLocation;
  int roomRando = 0; // index into kRoomRandoNames
  // Regions whose doors the room randomizer leaves alone, by name. The Frigate and Impact Crater
  // are excluded by default: one is only visited at the start and the other at the very end.
  static std::set< std::string > DefaultExcludedRegions() {
    return {"Frigate Orpheon", "Impact Crater"};
  }
  std::set< std::string > roomRandoExcludedRegions = DefaultExcludedRegions();
  bool roomRandoMorphBallDoors = true;
  std::map< std::string, StandardPickupState > standard;
  std::map< std::string, AmmoPickupState > ammo;

  void ResetToDefaults(const PickupDatabase& pickups);
  void FillMissing(const PickupDatabase& pickups);

  nlohmann::json ToJson() const;
  bool FromJson(const nlohmann::json& json);

  bool Save(const std::filesystem::path& path) const;
  bool Load(const std::filesystem::path& path);
};

inline constexpr float kDamageStrictnessValues[] = {1.0f, 1.5f, 2.0f};
inline constexpr const char* kDamageStrictnessNames[] = {"Strict", "Medium", "Lenient"};
inline constexpr const char* kRoomRandoNames[] = {"Off", "Two-way", "Two-way, mixed regions"};
// kRoomRandoNames index of the mode that pairs doors across regions.
inline constexpr int kRoomRandoMixed = 2;
inline constexpr const char* kTrickLevelNames[] = {
    "Disabled", "Beginner", "Intermediate", "Advanced", "Expert", "Ludicrous",
};

} // namespace metaforce::randomizer
