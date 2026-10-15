#include "Metaforce/Randomizer/Settings.hpp"

#include <borealis/io.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <iterator>

namespace metaforce::randomizer {
using json = nlohmann::json;

bool PickupDatabase::Load(const std::filesystem::path& path, std::string& error) {
  try {
    const borealis::io::ReadResult file =
        borealis::io::read_file(borealis::io::fs_path_to_string(path));
    if (file.status != borealis::io::Status::Ok) {
      error = "Could not open " + path.string();
      return false;
    }
    const json root = json::parse(file.data);
    standard.clear();
    ammo.clear();
    artifacts.clear();
    for (const auto& entry : root.at("standard")) {
      StandardPickupDef def;
      def.name = entry.at("name");
      def.category = entry.at("category");
      def.model = entry.at("model");
      def.itemType = entry.at("item_type");
      def.major = entry.value("major", true);
      def.hidden = entry.value("hidden", false);
      def.progression = entry.at("progression").get< std::vector< std::string > >();
      def.ammo = entry.at("ammo").get< std::vector< std::string > >();
      def.defaultShuffled = entry.at("default_shuffled");
      def.defaultStarting = entry.at("default_starting");
      const auto& defaultAmmo = entry.at("default_ammo");
      def.defaultAmmo = defaultAmmo.empty() ? 0 : defaultAmmo.at(0).get< int >();
      standard.push_back(std::move(def));
    }
    for (const auto& entry : root.at("ammo")) {
      AmmoPickupDef def;
      def.name = entry.at("name");
      def.category = entry.at("category");
      def.model = entry.at("model");
      def.resource = entry.at("resource");
      def.itemType = entry.at("item_type");
      def.refill = entry.at("refill");
      def.defaultCount = entry.at("default_count");
      def.defaultAmmo = entry.at("default_ammo");
      ammo.push_back(std::move(def));
    }
    for (const auto& entry : root.at("artifacts")) {
      artifacts.push_back({entry.at("name"), entry.at("resource"), entry.at("model"),
                           entry.at("item_type")});
    }
    defaultArtifactTarget = root.at("defaults").at("artifact_target");
  } catch (const std::exception& e) {
    error = std::string("Failed to load pickup database: ") + e.what();
    return false;
  }
  return true;
}

const StandardPickupDef* PickupDatabase::FindStandard(const std::string& name) const {
  for (const auto& def : standard) {
    if (def.name == name) {
      return &def;
    }
  }
  return nullptr;
}

const AmmoPickupDef* PickupDatabase::FindAmmo(const std::string& name) const {
  for (const auto& def : ammo) {
    if (def.name == name) {
      return &def;
    }
  }
  return nullptr;
}

int Settings::TrickLevel(const std::string& trick) const {
  const auto it = tricks.find(trick);
  return it != tricks.end() ? it->second : trickLevel;
}

void Settings::SetTrickLevel(const std::string& trick, int level) {
  level = std::clamp(level, 0, static_cast< int >(std::size(kTrickLevelNames)) - 1);
  if (level == trickLevel) {
    tricks.erase(trick);
  } else {
    tricks[trick] = level;
  }
}

void Settings::ResetToDefaults(const PickupDatabase& pickups) {
  const std::string seed = seedString;
  *this = Settings{};
  seedString = seed;
  artifactTarget = pickups.defaultArtifactTarget;
  FillMissing(pickups);
}

void Settings::FillMissing(const PickupDatabase& pickups) {
  for (const auto& def : pickups.standard) {
    standard.try_emplace(def.name, StandardPickupState{def.defaultShuffled, def.defaultStarting,
                                                       def.defaultAmmo});
  }
  for (const auto& def : pickups.ammo) {
    ammo.try_emplace(def.name, AmmoPickupState{def.defaultCount, def.defaultAmmo});
  }
}

json Settings::ToJson() const {
  json root{
      {"version", kVersion},
      {"seed", seedString},
      {"trick_level", trickLevel},
      {"damage_strictness", damageStrictness},
      {"artifact_target", artifactTarget},
      {"starting_location", startingLocation},
      {"room_rando", roomRando},
      {"room_rando_excluded_regions", roomRandoExcludedRegions},
      {"room_rando_morph_ball_doors", roomRandoMorphBallDoors},
      {"door_lock_mode", doorLockMode},
      {"door_lock_change_from", doorLockChangeFrom},
      {"door_lock_change_to", doorLockChangeTo},
      {"unlock_save_station_doors", unlockSaveStationDoors},
      {"blast_shield_lock_on", blastShieldLockOn},
  };
  // Only written when on, so the hash of a seed made with logic on doesn't change.
  if (logicDisabled) {
    root["logic_disabled"] = true;
  }
  // Only written when changed, for the same reason.
  if (artifactHints != kArtifactHintsPrecise) {
    root["artifact_hints"] = artifactHints;
  }
  if (phazonSuitHints != kArtifactHintsPrecise) {
    root["phazon_suit_hints"] = phazonSuitHints;
  }
  if (mainPlazaDoor) {
    root["main_plaza_door"] = true;
  }
  if (biggerThermalConduits) {
    root["bigger_thermal_conduits"] = true;
  }
  if (backwardsLabs) {
    root["backwards_labs"] = true;
  }
  if (backwardsUpperMines) {
    root["backwards_upper_mines"] = true;
  }
  if (backwardsLowerMines) {
    root["backwards_lower_mines"] = true;
  }
  if (!tricks.empty()) {
    root["tricks"] = tricks;
  }
  json& standardJson = root["standard"] = json::object();
  for (const auto& [name, state] : standard) {
    // Phazon Beam came after seeds were first made, so it's only written when it's in use. That
    // keeps the hash of every older seed.
    if (name == "Phazon Beam" && state.shuffled == 0 && state.starting == 0 && state.ammo == 0) {
      continue;
    }
    standardJson[name] = {
        {"shuffled", state.shuffled}, {"starting", state.starting}, {"ammo", state.ammo}};
  }
  json& ammoJson = root["ammo"] = json::object();
  for (const auto& [name, state] : ammo) {
    ammoJson[name] = {{"count", state.count}, {"ammo", state.ammo}};
  }
  return root;
}

bool Settings::FromJson(const json& root) {
  try {
    if (root.value("version", 0) != kVersion) {
      return false;
    }
    Settings result;
    result.seedString = root.value("seed", "");
    result.trickLevel = std::clamp(root.value("trick_level", 0), 0, 5);
    if (const auto it = root.find("tricks"); it != root.end()) {
      for (const auto& [name, level] : it->items()) {
        result.SetTrickLevel(name, level.get< int >());
      }
    }
    result.damageStrictness = std::clamp(root.value("damage_strictness", 1), 0, 2);
    result.artifactTarget = std::clamp(root.value("artifact_target", 12), 0, 12);
    result.artifactHints = std::clamp(root.value("artifact_hints", kArtifactHintsPrecise), 0,
                                      static_cast< int >(std::size(kArtifactHintNames)) - 1);
    result.phazonSuitHints = std::clamp(root.value("phazon_suit_hints", kArtifactHintsPrecise), 0,
                                        static_cast< int >(std::size(kArtifactHintNames)) - 1);
    result.startingLocation = root.value("starting_location", "");
    result.roomRando = std::clamp(root.value("room_rando", 0), 0,
                                  static_cast< int >(std::size(kRoomRandoNames)) - 1);
    result.roomRandoExcludedRegions =
        root.value("room_rando_excluded_regions", DefaultExcludedRegions());
    result.roomRandoMorphBallDoors = root.value("room_rando_morph_ball_doors", true);
    result.doorLockMode = std::clamp(root.value("door_lock_mode", 0), 0,
                                     static_cast< int >(std::size(kDoorLockModeNames)) - 1);
    result.doorLockChangeFrom = root.value("door_lock_change_from", DefaultDoorLockChangeFrom());
    result.doorLockChangeTo = root.value("door_lock_change_to", DefaultDoorLockChangeTo());
    result.unlockSaveStationDoors = root.value("unlock_save_station_doors", true);
    result.blastShieldLockOn = root.value("blast_shield_lock_on", true);
    result.logicDisabled = root.value("logic_disabled", false);
    result.mainPlazaDoor = root.value("main_plaza_door", false);
    result.biggerThermalConduits = root.value("bigger_thermal_conduits", false);
    result.backwardsLabs = root.value("backwards_labs", false);
    result.backwardsUpperMines = root.value("backwards_upper_mines", false);
    result.backwardsLowerMines = root.value("backwards_lower_mines", false);
    for (const auto& [name, state] : root.at("standard").items()) {
      result.standard[name] = {state.value("shuffled", 0), state.value("starting", 0),
                               state.value("ammo", 0)};
    }
    for (const auto& [name, state] : root.at("ammo").items()) {
      result.ammo[name] = {state.value("count", 0), state.value("ammo", 0)};
    }
    *this = std::move(result);
  } catch (const std::exception&) {
    return false;
  }
  return true;
}

bool Settings::Save(const std::filesystem::path& path) const {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  const std::filesystem::path tmp = path.string() + ".tmp";
  {
    std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
    if (!file) {
      return false;
    }
    file << ToJson().dump(2);
    if (!file) {
      return false;
    }
  }
  std::filesystem::rename(tmp, path, ec);
  return !ec;
}

bool Settings::Load(const std::filesystem::path& path) {
  const borealis::io::ReadResult file =
      borealis::io::read_file(borealis::io::fs_path_to_string(path));
  if (file.status != borealis::io::Status::Ok) {
    return false;
  }
  const json root = json::parse(file.data, nullptr, false);
  return !root.is_discarded() && FromJson(root);
}

} // namespace metaforce::randomizer
