#include "Metaforce/Randomizer/Seed.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <fstream>
#include <tuple>

namespace metaforce::randomizer {
using json = nlohmann::json;

namespace {

json GrantToJson(const ItemGrant& grant) {
  return {{"type", grant.itemType}, {"capacity", grant.capacity}, {"amount", grant.amount}};
}

ItemGrant GrantFromJson(const json& j) {
  return {j.at("type").get< int >(), j.at("capacity").get< int >(), j.at("amount").get< int >()};
}

bool WriteFile(const std::filesystem::path& path, const std::string& contents) {
  const std::filesystem::path tmp = path.string() + ".tmp";
  {
    std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
    if (!file) {
      return false;
    }
    file << contents;
    if (!file) {
      return false;
    }
  }
  std::error_code ec;
  std::filesystem::rename(tmp, path, ec);
  return !ec;
}

} // namespace

json Seed::ToJson() const {
  json root{
      {"format_version", kFormatVersion},
      {"hash", hash},
      {"seed", seedString},
      {"settings", settings},
      {"start",
       {{"name", startName},
        {"world", startWorld},
        {"area", startArea},
        {"random", randomStart}}},
      {"shuffled_artifacts", shuffledArtifacts},
      {"spheres", spheres},
      {"warnings", warnings},
  };
  if (startPosition) {
    root["start"]["position"] = *startPosition;
  }
  if (startYaw) {
    root["start"]["yaw"] = *startYaw;
  }
  if (startDock >= 0) {
    root["start"]["dock"] = startDock;
  }
  json& locationsJson = root["locations"] = json::array();
  for (const auto& loc : locations) {
    locationsJson.push_back(
        {{"name", loc.name}, {"model", loc.model}, {"grant", GrantToJson(loc.grant)}});
  }
  json& startingJson = root["starting_items"] = json::array();
  for (const auto& item : startingItems) {
    startingJson.push_back({{"name", item.name}, {"grant", GrantToJson(item.grant)}});
  }
  if (!docks.empty()) {
    json& docksJson = root["docks"] = json::array();
    for (const auto& dock : docks) {
      docksJson.push_back({{"world", dock.world},
                           {"area", dock.area},
                           {"dock", dock.dock},
                           {"target_world", dock.targetWorld},
                           {"target_area", dock.targetArea},
                           {"target_dock", dock.targetDock},
                           {"name", dock.name},
                           {"target_name", dock.targetName},
                           {"target_morph_ball", dock.targetMorphBall}});
    }
  }
  if (doorLocksRandomized) {
    json& locks = root["door_locks"] = {{"lock_on", blastShieldLockOn}, {"doors", json::array()}};
    for (const auto& door : doorLocks) {
      locks["doors"].push_back({{"world", door.world},
                                {"area", door.area},
                                {"dock", door.dock},
                                {"shield", door.shield},
                                {"blast_shield", door.blastShield},
                                {"weakness", door.weakness},
                                {"name", door.name},
                                {"changed", door.changed}});
    }
  }
  return root;
}

std::optional< Seed > Seed::FromJson(const json& root, std::string& error) {
  try {
    const int version = root.at("format_version").get< int >();
    if (version < kOldestFormatVersion || version > kFormatVersion) {
      error = "Seed was made by an incompatible version of Metaforce";
      return std::nullopt;
    }
    Seed seed;
    seed.hash = root.at("hash");
    seed.seedString = root.at("seed");
    seed.settings = root.at("settings");
    const json& start = root.at("start");
    seed.startName = start.at("name");
    seed.startWorld = start.at("world");
    seed.startArea = start.at("area");
    seed.randomStart = start.value("random", false);
    if (start.contains("position")) {
      seed.startPosition = start.at("position").get< std::array< float, 3 > >();
    }
    if (start.contains("yaw")) {
      seed.startYaw = start.at("yaw").get< float >();
    }
    seed.startDock = start.value("dock", -1);
    seed.shuffledArtifacts = root.at("shuffled_artifacts");
    seed.spheres = root.at("spheres").get< std::vector< std::vector< std::string > > >();
    seed.warnings = root.value("warnings", std::vector< std::string >{});
    for (const auto& loc : root.at("locations")) {
      seed.locations.push_back({loc.at("name"), loc.at("model"), GrantFromJson(loc.at("grant"))});
    }
    for (const auto& item : root.at("starting_items")) {
      seed.startingItems.push_back({item.at("name"), GrantFromJson(item.at("grant"))});
    }
    if (root.contains("docks")) {
      for (const auto& dock : root.at("docks")) {
        // Seeds from before regions could be mixed have no target_world: every door stayed in
        // its own world.
        seed.docks.push_back({dock.at("world"), dock.at("area"), dock.at("dock"),
                              dock.value("target_world", dock.at("world").get< uint32_t >()),
                              dock.at("target_area"), dock.at("target_dock"),
                              dock.value("name", ""), dock.value("target_name", ""),
                              dock.value("target_morph_ball", false)});
      }
    }
    if (root.contains("door_locks")) {
      const json& locks = root.at("door_locks");
      seed.doorLocksRandomized = true;
      seed.blastShieldLockOn = locks.value("lock_on", false);
      for (const auto& door : locks.at("doors")) {
        seed.doorLocks.push_back({door.at("world"), door.at("area"), door.at("dock"),
                                  door.at("shield"), door.value("blast_shield", ""),
                                  door.value("weakness", ""), door.value("name", ""),
                                  door.value("changed", true)});
      }
    }
    return seed;
  } catch (const std::exception& e) {
    error = std::string("Malformed seed: ") + e.what();
    return std::nullopt;
  }
}

bool Seed::Save(const std::filesystem::path& directory, std::string& error) const {
  std::error_code ec;
  std::filesystem::create_directories(directory, ec);
  if (ec) {
    error = "Could not create " + directory.string() + ": " + ec.message();
    return false;
  }
  if (!WriteFile(directory / "seed.json", ToJson().dump(1))) {
    error = "Could not write " + (directory / "seed.json").string();
    return false;
  }
  return true;
}

std::optional< Seed > Seed::Load(const std::filesystem::path& file, std::string& error) {
  std::ifstream in(file, std::ios::binary);
  if (!in) {
    error = "Could not open " + file.string();
    return std::nullopt;
  }
  const json root = json::parse(in, nullptr, false);
  if (root.is_discarded()) {
    error = "Malformed seed file " + file.string();
    return std::nullopt;
  }
  return FromJson(root, error);
}

std::string Seed::SpoilerText(const std::vector< std::string >& locationNames) const {
  std::string out = fmt::format("Metaforce Randomizer\nSeed: {}\nSeed string: {}\nStart: {}\n\n",
                                hash, seedString, startName);
  out += "Starting items:\n";
  for (const auto& item : startingItems) {
    out += fmt::format("  {}\n", item.name);
  }
  out += "\nPlaythrough:\n";
  for (size_t i = 0; i < spheres.size(); ++i) {
    out += fmt::format("  Sphere {}\n", i + 1);
    for (const auto& line : spheres[i]) {
      out += fmt::format("    {}\n", line);
    }
  }
  out += "\nAll locations:\n";
  for (size_t i = 0; i < locations.size(); ++i) {
    const std::string& name = i < locationNames.size() ? locationNames[i] : std::string{};
    out += fmt::format("  [{:3}] {}: {}\n", i, name, locations[i].name);
  }
  if (!docks.empty()) {
    out += "\nDoors:\n";
    for (const auto& dock : docks) {
      // Each pair is listed in both directions; print it once.
      if (std::tie(dock.area, dock.dock) < std::tie(dock.targetArea, dock.targetDock)) {
        out += fmt::format("  {} <-> {}\n", dock.name, dock.targetName);
      }
    }
  }
  if (std::any_of(doorLocks.begin(), doorLocks.end(),
                  [](const DoorLock& door) { return door.changed; })) {
    out += "\nDoor locks:\n";
    for (const auto& door : doorLocks) {
      if (door.changed) {
        out += fmt::format("  {}: {}\n", door.name, door.weakness);
      }
    }
  }
  if (!warnings.empty()) {
    out += "\nWarnings:\n";
    for (const auto& warning : warnings) {
      out += fmt::format("  {}\n", warning);
    }
  }
  return out;
}

} // namespace metaforce::randomizer
