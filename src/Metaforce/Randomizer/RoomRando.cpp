#include "Metaforce/Randomizer/RoomRando.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <set>
#include <utility>

namespace metaforce::randomizer {
namespace {

// How far apart two dock planes' edge lengths may be and still count as the same shape.
constexpr float kShapeTolerance = 0.3f;
// Bounds the pair/unpair loop of a single pool before giving up on this attempt: at least
// kMinSteps, more for pools with many doors.
constexpr int kMinSteps = 20000;
constexpr int kMaxStepsPerDoor = 100;
// Regions that keep their own pool when the others are mixed: the Frigate is left behind for
// good, and Impact Crater is only open at the end of the game.
constexpr const char* kUnmixedRegions[] = {"Frigate Orpheon", "Impact Crater"};

using AreaPair = std::pair< int, int >;

AreaPair MakeAreaPair(int a, int b) { return a < b ? AreaPair{a, b} : AreaPair{b, a}; }

// Pairs up the doors of a pool of regions: one region, or every mixed region at once.
class PoolShuffler {
public:
  PoolShuffler(const Database& db, const RoomRandoOptions& options, std::vector< int > regions,
               std::mt19937_64& rng, std::vector< int >& remap)
  : mDb(db), mOptions(options), mRegions(std::move(regions)), mRng(rng), mRemap(remap) {
    mInPool.assign(mDb.Regions().size(), 0);
    for (const int region : mRegions) {
      mInPool[region] = 1;
      mAreas.insert(mAreas.end(), mDb.Regions()[region].areas.begin(),
                    mDb.Regions()[region].areas.end());
    }
  }

  bool Run(std::string& error) {
    for (const int area : mAreas) {
      for (const int node : mDb.Areas()[area].nodes) {
        const Node& n = mDb.GetNode(node);
        if (!n.dock || n.dock->target < 0) {
          continue;
        }
        mNeighbors.insert(MakeAreaPair(area, mDb.GetNode(n.dock->target).area));
        if (IsShuffleableDock(mDb, mOptions, node)) {
          mCandidates.push_back(node);
        }
      }
    }
    if (mCandidates.empty()) {
      return true;
    }
    const int vanillaComponents = CountComponents();

    std::shuffle(mCandidates.begin(), mCandidates.end(), mRng);
    const int maxSteps =
        std::max(kMinSteps, kMaxStepsPerDoor * static_cast< int >(mCandidates.size()));
    for (int step = 0; step < maxSteps; ++step) {
      if (mCandidates.empty()) {
        if (CountComponents() <= vanillaComponents) {
          return true;
        }
        Reconnect();
        continue;
      }

      const int src = mCandidates.front();
      int dst = -1;
      for (size_t i = 1; i < mCandidates.size(); ++i) {
        if (Compatible(src, mCandidates[i])) {
          dst = mCandidates[i];
          break;
        }
      }
      if (dst < 0) {
        // Nothing left fits this door: undo a random pairing and try again.
        if (mPairs.empty()) {
          break;
        }
        Unpair(mPairs[mRng() % mPairs.size()]);
        std::shuffle(mCandidates.begin(), mCandidates.end(), mRng);
        continue;
      }
      Pair(src, dst);
    }
    std::string names;
    for (const int region : mRegions) {
      names += (names.empty() ? "" : ", ") + mDb.Regions()[region].name;
    }
    error = fmt::format("Could not pair the doors of {}", names);
    return false;
  }

private:
  bool Compatible(int a, int b) const {
    const Node& na = mDb.GetNode(a);
    const Node& nb = mDb.GetNode(b);
    if (na.area == nb.area) {
      return false;
    }
    const DockInfo& da = *na.dock;
    const DockInfo& db = *nb.dock;
    if (da.type != db.type || std::abs(da.width - db.width) > kShapeTolerance ||
        std::abs(da.height - db.height) > kShapeTolerance) {
      return false;
    }
    // Wall doors pair with wall doors, and a hole in a floor with a hole in a ceiling.
    if (da.facing != -db.facing) {
      return false;
    }
    // Rooms that are already neighbors may only keep their vanilla door, so a room is never
    // connected to the same room through two doors.
    const AreaPair areas = MakeAreaPair(na.area, nb.area);
    if (mNeighbors.contains(areas) && da.target != b) {
      return false;
    }
    return !mUsedAreaPairs.contains(areas);
  }

  void Pair(int a, int b) {
    std::erase(mCandidates, a);
    std::erase(mCandidates, b);
    mPairs.emplace_back(a, b);
    mUsedAreaPairs.insert(MakeAreaPair(mDb.GetNode(a).area, mDb.GetNode(b).area));
    mRemap[a] = b;
    mRemap[b] = a;
  }

  void Unpair(std::pair< int, int > pair) {
    std::erase(mPairs, pair);
    mUsedAreaPairs.erase(MakeAreaPair(mDb.GetNode(pair.first).area, mDb.GetNode(pair.second).area));
    mRemap[pair.first] = -1;
    mRemap[pair.second] = -1;
    mCandidates.push_back(pair.first);
    mCandidates.push_back(pair.second);
  }

  // Splits two pairings from different parts of a disconnected layout, hoping the doors join the
  // parts when they're paired again.
  void Reconnect() {
    std::shuffle(mPairs.begin(), mPairs.end(), mRng);
    const std::pair< int, int > first = mPairs.back();
    const int component = mComponent[mDb.GetNode(first.first).area];
    std::pair< int, int > second = mPairs.front();
    for (const auto& pair : mPairs) {
      if (mComponent[mDb.GetNode(pair.first).area] != component) {
        second = pair;
        break;
      }
    }
    Unpair(first);
    if (second != first) {
      Unpair(second);
    }
    // Otherwise the freed doors would just pair up the way they were.
    std::shuffle(mCandidates.begin(), mCandidates.end(), mRng);
  }

  int Target(int node) const {
    return mRemap[node] >= 0 ? mRemap[node] : mDb.GetNode(node).dock->target;
  }

  // Strongly connected components of the pool's rooms, through every door that can be opened,
  // elevators between the pool's regions included. Fills mComponent with each area's component.
  int CountComponents() {
    std::vector< std::vector< int > > edges(mDb.Areas().size());
    std::vector< std::vector< int > > reverse(mDb.Areas().size());
    for (const int area : mAreas) {
      for (const int node : mDb.Areas()[area].nodes) {
        const Node& n = mDb.GetNode(node);
        if (!n.dock || n.dock->target < 0 || n.dock->connection < 0) {
          continue;
        }
        const int target = mDb.GetNode(Target(node)).area;
        if (!mInPool[mDb.Areas()[target].region]) {
          continue;
        }
        edges[area].push_back(target);
        reverse[target].push_back(area);
      }
    }

    // Kosaraju: order by finish time, then flood the reversed graph.
    std::vector< int > order;
    std::vector< uint8_t > seen(mDb.Areas().size(), 0);
    for (const int root : mAreas) {
      if (seen[root]) {
        continue;
      }
      std::vector< std::pair< int, size_t > > stack{{root, 0}};
      seen[root] = 1;
      while (!stack.empty()) {
        auto& [area, next] = stack.back();
        if (next < edges[area].size()) {
          const int target = edges[area][next++];
          if (!seen[target]) {
            seen[target] = 1;
            stack.emplace_back(target, 0);
          }
        } else {
          order.push_back(area);
          stack.pop_back();
        }
      }
    }
    mComponent.assign(mDb.Areas().size(), -1);
    int count = 0;
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
      if (mComponent[*it] >= 0) {
        continue;
      }
      std::vector< int > stack{*it};
      mComponent[*it] = count;
      while (!stack.empty()) {
        const int area = stack.back();
        stack.pop_back();
        for (const int source : reverse[area]) {
          if (mComponent[source] < 0) {
            mComponent[source] = count;
            stack.push_back(source);
          }
        }
      }
      ++count;
    }
    return count;
  }

  const Database& mDb;
  const RoomRandoOptions& mOptions;
  const std::vector< int > mRegions;
  std::vector< uint8_t > mInPool; // per region
  std::vector< int > mAreas;      // every area of the pool's regions
  std::mt19937_64& mRng;
  std::vector< int >& mRemap;
  std::vector< int > mCandidates;
  std::vector< std::pair< int, int > > mPairs;
  std::set< AreaPair > mNeighbors;
  std::set< AreaPair > mUsedAreaPairs;
  std::vector< int > mComponent;
};

bool IsCandidateDock(const Database& db, int node) {
  const Node& n = db.GetNode(node);
  if (!n.dock) {
    return false;
  }
  const DockInfo& dock = *n.dock;
  // Tilted doorways can't line up with any other doorway, so they stay as they are.
  return dock.type != "teleporter" && !dock.nonstandard && dock.index >= 0 && dock.target >= 0 &&
         dock.hasShape && dock.facing >= -1 && dock.facing <= 1;
}

} // namespace

bool IsShuffleableDock(const Database& db, const RoomRandoOptions& options, int node) {
  if (!IsCandidateDock(db, node)) {
    return false;
  }
  const DockInfo& dock = *db.GetNode(node).dock;
  if (!options.morphBallDoors && dock.type == "morph_ball") {
    return false;
  }
  const Region& region = db.Regions()[db.Areas()[db.GetNode(node).area].region];
  if (options.excludedRegions.contains(region.name)) {
    return false;
  }
  // Both sides of the vanilla door have to be shuffleable, or the pairing couldn't be two-way.
  const int target = db.GetNode(node).dock->target;
  return IsCandidateDock(db, target) && db.GetNode(target).dock->target == node &&
         db.Areas()[db.GetNode(target).area].region == db.Areas()[db.GetNode(node).area].region;
}

bool IsMixableRegion(const Database& db, int region) {
  return std::find(std::begin(kUnmixedRegions), std::end(kUnmixedRegions),
                   db.Regions()[region].name) == std::end(kUnmixedRegions);
}

int CountShuffleableDocks(const Database& db, int region) {
  int count = 0;
  for (const int area : db.Regions()[region].areas) {
    for (const int node : db.Areas()[area].nodes) {
      count += IsShuffleableDock(db, RoomRandoOptions{}, node) ? 1 : 0;
    }
  }
  return count;
}

std::vector< int > ShuffleRooms(const Database& db, const RoomRandoOptions& options,
                                std::mt19937_64& rng, std::string& error) {
  if (!db.HasDockShapes()) {
    error = "The logic database has no dock shapes; re-export it with --disc to use the room "
            "randomizer";
    return {};
  }
  std::vector< std::vector< int > > pools;
  std::vector< int > mixed;
  for (int region = 0; region < static_cast< int >(db.Regions().size()); ++region) {
    if (options.mixRegions && IsMixableRegion(db, region)) {
      mixed.push_back(region);
    } else {
      pools.push_back({region});
    }
  }
  if (!mixed.empty()) {
    pools.push_back(std::move(mixed));
  }
  std::vector< int > remap(db.Nodes().size(), -1);
  for (std::vector< int >& pool : pools) {
    PoolShuffler shuffler(db, options, std::move(pool), rng, remap);
    if (!shuffler.Run(error)) {
      return {};
    }
  }
  return remap;
}

} // namespace metaforce::randomizer
