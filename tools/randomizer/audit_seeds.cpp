// Generates seeds with Metaforce's randomizer and replays each one the way a player would have to:
// sphere by sphere, walking to every pickup from the start and back again. It reports pickups the
// spoiler counts on that are unreachable, or that leave the player stranded (a one-way trip with
// no way out), and seeds that can't be finished.
//
// The generator already only counts what the player can come back from; this is an independent
// check of that, sharing only the logic database and requirement evaluation with it.
//
// Build, from the repository root, after configuring the relwithdebinfo preset (for the fmt and
// nlohmann_json sources it fetches):
//
//   D=build/relwithdebinfo/_deps
//   clang++ -std=c++20 -O2 -Iinclude -I$D/nlohmann_json-src/include -I$D/fmt-src/include \
//       -DFMT_HEADER_ONLY tools/randomizer/audit_seeds.cpp \
//       src/Metaforce/Randomizer/{Generator,Logic,RoomRando,Settings,Seed}.cpp \
//       -o build/audit_seeds
//
// Run from the repository root:
//
//   build/audit_seeds --count 20 --trick 2 --rooms --random-start
//
// --mixed shuffles rooms with regions mixed (implies --rooms).
//
// Exits with 1 if any seed failed to generate or failed the audit.

#include "Metaforce/Randomizer/Generator.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

using namespace metaforce::randomizer;

namespace {

struct Options {
  int count = 10;
  int trick = 2;
  int damage = 1;
  bool rooms = false;
  bool mixed = false;
  bool randomStart = false;
  std::string prefix = "audit";
  std::string data = "res/randomizer/prime1/";
};

void Usage() {
  std::printf("usage: audit_seeds [--count N] [--trick 0-5] [--damage 0-2] [--rooms] [--mixed]\n"
              "                   [--random-start] [--prefix SEED] [--data DIR]\n");
}

bool ParseArgs(int argc, char** argv, Options& out) {
  for (int i = 1; i < argc; ++i) {
    const char* arg = argv[i];
    const bool hasValue = i + 1 < argc;
    if (std::strcmp(arg, "--rooms") == 0) {
      out.rooms = true;
    } else if (std::strcmp(arg, "--mixed") == 0) {
      out.rooms = true;
      out.mixed = true;
    } else if (std::strcmp(arg, "--random-start") == 0) {
      out.randomStart = true;
    } else if (std::strcmp(arg, "--count") == 0 && hasValue) {
      out.count = std::atoi(argv[++i]);
    } else if (std::strcmp(arg, "--trick") == 0 && hasValue) {
      out.trick = std::clamp(std::atoi(argv[++i]), 0, 5);
    } else if (std::strcmp(arg, "--damage") == 0 && hasValue) {
      out.damage = std::clamp(std::atoi(argv[++i]), 0, 2);
    } else if (std::strcmp(arg, "--prefix") == 0 && hasValue) {
      out.prefix = argv[++i];
    } else if (std::strcmp(arg, "--data") == 0 && hasValue) {
      out.data = argv[++i];
      if (!out.data.empty() && out.data.back() != '/') {
        out.data += '/';
      }
    } else {
      return false;
    }
  }
  return true;
}

// Walks the logic graph of one generated seed.
class Walker {
public:
  Walker(const Database& db, const PickupDatabase& pickups, const Settings& settings)
  : mDb(db), mPickups(pickups), mCtx{.db = db} {
    // Same context as the generator sets up.
    mCtx.trickLevels.assign(db.Tricks().size(), settings.trickLevel);
    mCtx.misc.assign(db.Misc().size(), 0);
    if (const int heat = db.FindMisc("vanilla_heat"); heat >= 0) {
      mCtx.misc[heat] = 1;
    }
    if (const int rooms = db.FindMisc("room_rando"); rooms >= 0 && settings.roomRando != 0) {
      mCtx.misc[rooms] = 1;
    }
    mCtx.damageStrictness = kDamageStrictnessValues[std::clamp(settings.damageStrictness, 0, 2)];
    mCtx.energyTankItem = db.FindItem("EnergyTank");
    mCtx.variaItem = db.FindItem("VariaSuit");
    mCtx.gravityItem = db.FindItem("GravitySuit");
    mCtx.phazonItem = db.FindItem("PhazonSuit");
    mRemap.assign(db.Nodes().size(), -1);
  }

  const LogicContext& Context() const { return mCtx; }
  void SetDoor(int from, int to) { mRemap[from] = to; }

  // Nodes reachable from `from`, through doors as the seed moved them.
  std::vector< uint8_t > Reach(int from, const ResourceState& state) const {
    std::vector< uint8_t > visited(mDb.Nodes().size(), 0);
    std::vector< int > stack{from};
    visited[from] = 1;
    while (!stack.empty()) {
      const int node = stack.back();
      stack.pop_back();
      const Node& n = mDb.GetNode(node);
      for (int c = 0; c < static_cast< int >(n.connections.size()); ++c) {
        const Connection& conn = n.connections[c];
        const bool moved = mRemap[node] >= 0 && n.dock->connection == c;
        const int target = moved ? mRemap[node] : conn.target;
        if (!visited[target] && IsSatisfied(conn.requirement, mCtx, state) &&
            (target == conn.target || IsSatisfied(mDb.GetNode(target).dock->lock, mCtx, state))) {
          visited[target] = 1;
          stack.push_back(target);
        }
      }
    }
    return visited;
  }

  // Adds what a pickup gives, by its name in the pickup database.
  void Grant(ResourceState& state, const std::string& name, int amount) const {
    const auto add = [&](const std::string& itemName, int value) {
      const int item = mDb.FindItem(itemName);
      if (item >= 0 && value > 0) {
        state.items[item] = std::min(state.items[item] + value,
                                     std::max(mDb.Items()[item].maxCapacity, value));
      }
    };
    if (const StandardPickupDef* def = mPickups.FindStandard(name)) {
      for (const auto& item : def->progression) {
        add(item, 1);
      }
      for (const auto& item : def->ammo) {
        add(item, amount);
      }
    } else if (const AmmoPickupDef* def = mPickups.FindAmmo(name)) {
      if (!def->refill) {
        add(def->resource, amount);
      }
    } else {
      for (const auto& artifact : mPickups.artifacts) {
        if (artifact.name == name) {
          add(artifact.resource, 1);
        }
      }
    }
  }

private:
  const Database& mDb;
  const PickupDatabase& mPickups;
  LogicContext mCtx;
  std::vector< int > mRemap;
};

// Replays a seed's spoiler. Returns the number of problems found, printing each.
int AuditSeed(const Database& db, const PickupDatabase& pickups, const Settings& settings,
              const Seed& seed, const std::unordered_map< std::string, int >& nodes,
              const std::string& label) {
  Walker walker(db, pickups, settings);
  for (const DockConnection& door : seed.docks) {
    walker.SetDoor(nodes.at(door.name), nodes.at(door.targetName));
  }
  const int start = nodes.at(seed.startName);
  const int victory = db.Victory().resource;

  ResourceState state(db);
  for (const StartingItem& item : seed.startingItems) {
    walker.Grant(state, item.name, item.grant.amount);
  }

  // Does every event the player can reach and still come back from, except the credits, which
  // end the game.
  const auto settleEvents = [&] {
    for (bool progress = true; progress;) {
      progress = false;
      const std::vector< uint8_t > reach = walker.Reach(start, state);
      for (size_t i = 0; i < reach.size() && !progress; ++i) {
        const Node& node = db.GetNode(static_cast< int >(i));
        if (!reach[i] || node.type != NodeType::Event || node.event < 0 ||
            state.events[node.event]) {
          continue;
        }
        ResourceState after = state;
        after.events[node.event] = 1;
        if (node.event == victory || walker.Reach(static_cast< int >(i), after)[start]) {
          state = std::move(after);
          progress = true;
        }
      }
    }
  };

  int problems = 0;
  for (size_t sphere = 0; sphere < seed.spheres.size(); ++sphere) {
    settleEvents();
    const std::vector< uint8_t > reach = walker.Reach(start, state);
    ResourceState next = state;
    for (const std::string& line : seed.spheres[sphere]) {
      const int node = nodes.at(line.substr(0, line.rfind(": ")));
      const PlacedPickup& pickup = seed.locations[db.GetNode(node).pickupIndex];
      ResourceState after = state;
      walker.Grant(after, pickup.name, pickup.grant.amount);
      const char* problem = nullptr;
      if (!reach[node]) {
        problem = "unreachable";
      } else if (!walker.Reach(node, after)[start]) {
        problem = "one-way, no way back";
      }
      if (problem != nullptr) {
        std::printf("  %s: sphere %zu, %s: %s\n", label.c_str(), sphere + 1, problem, line.c_str());
        ++problems;
      }
      walker.Grant(next, pickup.name, pickup.grant.amount);
    }
    state = std::move(next);
  }
  settleEvents();
  if (!IsSatisfied(db.Victory(), walker.Context(), state)) {
    std::printf("  %s: the game can't be finished after the last sphere\n", label.c_str());
    ++problems;
  }
  return problems;
}

} // namespace

int main(int argc, char** argv) {
  Options options;
  if (!ParseArgs(argc, argv, options)) {
    Usage();
    return 2;
  }

  Database db;
  PickupDatabase pickups;
  std::string error;
  if (!db.Load(options.data + "logic.json", error) ||
      !pickups.Load(options.data + "pickups.json", error)) {
    std::printf("%s\n", error.c_str());
    return 2;
  }
  std::unordered_map< std::string, int > nodes;
  for (int i = 0; i < static_cast< int >(db.Nodes().size()); ++i) {
    nodes.emplace(db.NodeName(i), i);
  }

  int generated = 0;
  int failedAudit = 0;
  for (int i = 0; i < options.count; ++i) {
    Settings settings;
    settings.ResetToDefaults(pickups);
    settings.seedString = options.prefix + std::to_string(i);
    settings.trickLevel = options.trick;
    settings.damageStrictness = options.damage;
    settings.roomRando = options.mixed ? kRoomRandoMixed : options.rooms ? 1 : 0;
    if (options.randomStart) {
      settings.startingLocation = kRandomStartingLocation;
    }
    const std::optional< Seed > seed = Generate({db, pickups, settings}, error);
    if (!seed) {
      std::printf("%s: generation failed: %s\n", settings.seedString.c_str(), error.c_str());
      continue;
    }
    ++generated;
    const int problems = AuditSeed(db, pickups, settings, *seed, nodes, settings.seedString);
    failedAudit += problems > 0 ? 1 : 0;
    const auto crossRegion = std::count_if(seed->docks.begin(), seed->docks.end(),
                                           [](const DockConnection& door) {
                                             return door.world != door.targetWorld;
                                           });
    std::printf("%s: %s, start %s, %zu spheres, %zu doors moved (%td across regions)\n",
                settings.seedString.c_str(), problems > 0 ? "FAILED" : "ok",
                seed->startName.c_str(), seed->spheres.size(), seed->docks.size() / 2,
                crossRegion / 2);
  }
  std::printf("%d/%d generated, %d failed the audit\n", generated, options.count, failedAudit);
  return generated == options.count && failedAudit == 0 ? 0 : 1;
}
