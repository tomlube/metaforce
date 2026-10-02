#include "Metaforce/Randomizer/Generator.hpp"

#include "Metaforce/Randomizer/RoomRando.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <random>

namespace metaforce::randomizer {
namespace {

constexpr int kMaxAttempts = 50;
// Random starts pick a new room each attempt, so starts that can't work get rerolled. Shuffled
// rooms are rerolled every attempt too.
constexpr int kMaxRerollingAttempts = 200;
// Shuffled rooms usually shrink what's reachable from the start, so most fills fail and room
// rando gets many more (cheap) attempts.
constexpr int kMaxRoomRandoAttempts = 3000;
// Room layouts are rerolled until the start reaches a pickup with the starting items alone.
constexpr int kMaxLayoutRerolls = 1000;
// Random starts tried on each shuffled room layout.
constexpr int kStartsPerLayout = 20;
constexpr const char* kFillerPickup = "Missile Expansion";

struct PoolEntry {
  std::string name;
  std::string model;
  ItemGrant grant;
  std::vector< std::pair< int, int > > logic; // (item resource, amount)
  bool major = false;
};

uint64_t HashString(std::string_view str, uint64_t hash = 0xcbf29ce484222325ull) {
  for (const char c : str) {
    hash ^= static_cast< unsigned char >(c);
    hash *= 0x100000001b3ull;
  }
  return hash;
}

std::string HashToId(uint64_t hash) {
  static constexpr char kAlphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
  std::string id;
  for (int i = 0; i < 8; ++i) {
    id += kAlphabet[hash & 31];
    hash >>= 5;
  }
  return id;
}

// How far into the room a start at a door puts the player, clear of the door frame.
constexpr float kStartDoorClearance = 4.f;

using Position = std::array< float, 3 >;

struct StartPlacement {
  Position position;
  std::optional< float > yaw; // see Seed::startYaw
};

// Where a start at `node` puts the player, or nothing if the game can't place them there.
// Starting locations at doors are positioned in the doorway itself, so the player goes a few
// steps into the room, toward the nearest of its other nodes (preferring ones away from doors),
// and faces that way, with the door behind them. Morph ball tunnels only fit the player morphed,
// so those aren't starts at all.
std::optional< StartPlacement > PlaceStart(const Database& db, int node) {
  const Node& start = db.GetNode(node);
  if (!start.worldPosition || (start.dock && start.dock->type == "morph_ball")) {
    return std::nullopt;
  }
  const Position& doorway = *start.worldPosition;
  if (!start.dock || start.dock->type == "teleporter") {
    return StartPlacement{doorway, std::nullopt};
  }
  const auto distance = [&](int other) {
    const Position& pos = *db.GetNode(other).worldPosition;
    return std::hypot(pos[0] - doorway[0], pos[1] - doorway[1], pos[2] - doorway[2]);
  };
  // Lower is better, 2 for nodes that won't do.
  const auto rank = [&](int other) {
    const Node& candidate = db.GetNode(other);
    if (other == node || !candidate.worldPosition || distance(other) < 1.f) {
      return 2;
    }
    return candidate.dock ? 1 : 0;
  };
  int best = -1;
  for (const int other : db.Areas()[start.area].nodes) {
    if (rank(other) < 2 && (best < 0 || rank(other) < rank(best) ||
                            (rank(other) == rank(best) && distance(other) < distance(best)))) {
      best = other;
    }
  }
  if (best < 0) {
    return StartPlacement{doorway, std::nullopt};
  }
  // Along the floor, and at most halfway there.
  const Position& target = *db.GetNode(best).worldPosition;
  const float dx = target[0] - doorway[0];
  const float dy = target[1] - doorway[1];
  const float flat = std::hypot(dx, dy);
  if (flat < 0.001f) {
    return StartPlacement{doorway, std::nullopt};
  }
  const float step = std::min(kStartDoorClearance, flat / 2.f);
  // Rotating +Y by `yaw` about Z gives (-sin, cos).
  return StartPlacement{{doorway[0] + dx / flat * step, doorway[1] + dy / flat * step, doorway[2]},
                        std::atan2(-dx, dy)};
}

class Generator {
public:
  Generator(const GeneratorInput& input, const ProgressCallback& progress,
            const std::atomic< bool >* cancel)
  : mDb(input.db)
  , mPickups(input.pickups)
  , mSettings(input.settings)
  , mProgress(progress)
  , mCancel(cancel)
  , mCtx{.db = input.db} {}

  std::optional< Seed > Run(std::string& error) {
    if (!SetupContext(error) || !BuildPool(error)) {
      return std::nullopt;
    }

    const uint64_t seedHash = HashString(mSettings.ToJson().dump(), HashString(mSettings.seedString));
    const bool roomRando = mSettings.roomRando != 0;
    const int maxAttempts = roomRando                ? kMaxRoomRandoAttempts
                            : mRandomStarts.empty() ? kMaxAttempts
                                                    : kMaxRerollingAttempts;
    for (int attempt = 0; attempt < maxAttempts; ++attempt) {
      if (mCancel != nullptr && mCancel->load()) {
        error = "Generation was cancelled";
        return std::nullopt;
      }
      Report(static_cast< float >(attempt) / maxAttempts, fmt::format("Attempt {}", attempt + 1));
      std::mt19937_64 rng(seedHash + attempt);
      if (!mRandomStarts.empty()) {
        // A new start each attempt, so starts that can't reach enough locations get rerolled.
        mStartNode = mRandomStarts[rng() % mRandomStarts.size()];
      }
      if (roomRando && !ShuffleRoomsWithOpenStart(rng)) {
        ++mRoomFailures;
        continue;
      }
      std::vector< int > placement;
      if (!Fill(rng, placement)) {
        ++mFillFailures;
        continue;
      }
      std::vector< std::vector< std::string > > spheres;
      if (!Playthrough(placement, &spheres)) {
        ++mPlaythroughFailures;
        continue;
      }
      return MakeSeed(placement, std::move(spheres), seedHash);
    }
    error = fmt::format("Could not generate a beatable seed after {} attempts ({} fill, {} "
                        "playthrough, {} room layout failures). Try more lenient settings, a "
                        "higher trick level, or more items in the pool.",
                        maxAttempts, mFillFailures, mPlaythroughFailures, mRoomFailures);
    return std::nullopt;
  }

private:
  bool SetupContext(std::string& error) {
    mCtx.trickLevels.assign(mDb.Tricks().size(), mSettings.trickLevel);
    mCtx.misc.assign(mDb.Misc().size(), 0);
    // Metaforce runs the game unpatched, where every suit after the Power Suit resists heat.
    if (const int heat = mDb.FindMisc("vanilla_heat"); heat >= 0) {
      mCtx.misc[heat] = 1;
    }
    // Some tricks only work with the vanilla room layout.
    if (const int rooms = mDb.FindMisc("room_rando"); rooms >= 0 && mSettings.roomRando != 0) {
      mCtx.misc[rooms] = 1;
    }
    mCtx.damageStrictness =
        kDamageStrictnessValues[std::clamp(mSettings.damageStrictness, 0, 2)];
    mCtx.energyTankItem = mDb.FindItem("EnergyTank");
    mCtx.variaItem = mDb.FindItem("VariaSuit");
    mCtx.gravityItem = mDb.FindItem("GravitySuit");
    mCtx.phazonItem = mDb.FindItem("PhazonSuit");

    mStartNode = mDb.StartNode();
    if (mSettings.startingLocation == kRandomStartingLocation) {
      for (const auto& [key, name] : StartingLocations(mDb)) {
        mRandomStarts.push_back(FindStartNode(key));
      }
    } else if (!mSettings.startingLocation.empty()) {
      for (const auto& [key, name] : StartingLocations(mDb)) {
        if (key == mSettings.startingLocation) {
          mStartNode = FindStartNode(key);
        }
      }
    }
    if (mStartNode < 0) {
      error = "Unknown starting location " + mSettings.startingLocation;
      return false;
    }
    if (mSettings.roomRando != 0 && !mDb.HasDockShapes()) {
      error = "The logic database has no dock shapes; re-export it with --disc to use the room "
              "randomizer";
      return false;
    }

    for (int index = 0; index < static_cast< int >(mDb.PickupNodes().size()); ++index) {
      if (mDb.PickupNodes()[index] >= 0) {
        mLocations.push_back(index);
      }
    }
    if (mLocations.size() != static_cast< size_t >(mDb.PickupNodes().size())) {
      error = "The logic database is missing pickup locations";
      return false;
    }

    mNegatedEvents.assign(mDb.Events().size(), 0);
    mVictoryEvents.assign(mDb.Events().size(), 0);
    for (const Node& node : mDb.Nodes()) {
      for (const Connection& conn : node.connections) {
        MarkEvents(conn.requirement, true, mNegatedEvents);
      }
      if (node.dock) {
        MarkEvents(node.dock->lock, true, mNegatedEvents);
      }
    }
    MarkEvents(mDb.Victory(), false, mVictoryEvents);
    RebuildIncoming();
    return true;
  }

  // Flags the events `req` refers to, or only the ones it needs to not have happened.
  static void MarkEvents(const Requirement& req, bool negatedOnly, std::vector< uint8_t >& out) {
    if (req.kind != Requirement::Kind::Resource) {
      for (const auto& child : req.children) {
        MarkEvents(child, negatedOnly, out);
      }
    } else if (req.resourceType == ResourceType::Event && (req.negate || !negatedOnly)) {
      out[req.resource] = 1;
    }
  }

  int FindStartNode(const std::string& key) const {
    const auto first = key.find('|');
    const auto second = key.find('|', first + 1);
    return mDb.FindNode(key.substr(0, first), key.substr(first + 1, second - first - 1),
                        key.substr(second + 1));
  }

  PoolEntry MakeStandard(const StandardPickupDef& def, int ammo) const {
    PoolEntry entry{def.name, def.model, {def.itemType, 1, 1}, {}, def.major};
    for (const auto& name : def.progression) {
      if (const int item = mDb.FindItem(name); item >= 0) {
        entry.logic.emplace_back(item, 1);
      }
    }
    if (!def.ammo.empty()) {
      entry.grant.capacity = ammo;
      entry.grant.amount = ammo;
      for (const auto& name : def.ammo) {
        if (const int item = mDb.FindItem(name); item >= 0 && ammo > 0) {
          entry.logic.emplace_back(item, ammo);
        }
      }
    }
    return entry;
  }

  PoolEntry MakeAmmo(const AmmoPickupDef& def, int ammo) const {
    PoolEntry entry{def.name, def.model, {def.itemType, def.refill ? 0 : ammo, ammo}};
    if (!def.refill) {
      if (const int item = mDb.FindItem(def.resource); item >= 0 && ammo > 0) {
        entry.logic.emplace_back(item, ammo);
      }
    }
    return entry;
  }

  bool BuildPool(std::string& error) {
    for (const auto& def : mPickups.standard) {
      const auto it = mSettings.standard.find(def.name);
      const StandardPickupState state = it != mSettings.standard.end()
                                            ? it->second
                                            : StandardPickupState{def.defaultShuffled,
                                                                  def.defaultStarting,
                                                                  def.defaultAmmo};
      for (int i = 0; i < state.shuffled; ++i) {
        mPool.push_back(MakeStandard(def, state.ammo));
      }
      for (int i = 0; i < state.starting; ++i) {
        mStarting.push_back(MakeStandard(def, state.ammo));
      }
    }
    for (const auto& def : mPickups.ammo) {
      const auto it = mSettings.ammo.find(def.name);
      const AmmoPickupState state = it != mSettings.ammo.end()
                                        ? it->second
                                        : AmmoPickupState{def.defaultCount, def.defaultAmmo};
      for (int i = 0; i < state.count; ++i) {
        mPool.push_back(MakeAmmo(def, state.ammo));
      }
    }
    const int artifactTarget = std::clamp(mSettings.artifactTarget, 0, 12);
    for (int i = 0; i < static_cast< int >(mPickups.artifacts.size()); ++i) {
      const ArtifactDef& def = mPickups.artifacts[i];
      PoolEntry entry{def.name, def.model, {def.itemType, 1, 1}, {}, true};
      if (const int item = mDb.FindItem(def.resource); item >= 0) {
        entry.logic.emplace_back(item, 1);
      }
      if (i < artifactTarget) {
        mPool.push_back(std::move(entry));
        mShuffledArtifacts |= 1u << i;
      } else {
        mStarting.push_back(std::move(entry));
      }
    }

    // Match the pool to the location count, trimming or adding filler pickups.
    const size_t locationCount = mLocations.size();
    const AmmoPickupDef* filler = mPickups.FindAmmo(kFillerPickup);
    while (mPool.size() > locationCount) {
      auto it = std::find_if(mPool.rbegin(), mPool.rend(),
                             [](const PoolEntry& e) { return e.name == kFillerPickup; });
      if (it == mPool.rend()) {
        error = fmt::format("The item pool has {} pickups but there are only {} locations",
                            mPool.size(), locationCount);
        return false;
      }
      mPool.erase(std::next(it).base());
      ++mTrimmed;
    }
    if (mPool.size() < locationCount && filler == nullptr) {
      error = "No filler pickup is available";
      return false;
    }
    while (mPool.size() < locationCount) {
      const auto it = mSettings.ammo.find(kFillerPickup);
      const int ammo = it != mSettings.ammo.end() && it->second.ammo > 0 ? it->second.ammo
                                                                          : filler->defaultAmmo;
      mPool.push_back(MakeAmmo(*filler, ammo));
      ++mPadded;
    }

    mStartState.emplace(mDb);
    for (const auto& entry : mStarting) {
      Apply(*mStartState, entry);
    }
    // Before and after the events that close paths: some locations only open up after them.
    ResourceState all = *mStartState;
    for (const auto& entry : mPool) {
      Apply(all, entry);
    }
    mAllResources.clear();
    for (const bool closed : {false, true}) {
      for (size_t event = 0; event < all.events.size(); ++event) {
        all.events[event] = mNegatedEvents[event] && !closed ? 0 : 1;
      }
      mAllResources.push_back(all);
    }
    return true;
  }

  void Apply(ResourceState& state, const PoolEntry& entry) const {
    for (const auto& [item, amount] : entry.logic) {
      state.items[item] = std::min(state.items[item] + amount,
                                   std::max(mDb.Items()[item].maxCapacity, amount));
    }
  }

  // Shuffles rooms until the layout passes IsLayoutOpen. With random starts, each layout tries
  // several starts, as a start can be what's wrong with it: one in a small corner the shuffle cut
  // off would otherwise use up every reroll.
  bool ShuffleRoomsWithOpenStart(std::mt19937_64& rng) {
    std::string error;
    for (int reroll = 0; reroll < kMaxLayoutRerolls; ++reroll) {
      mDockRemap = ShuffleRooms(mDb,
                                {mSettings.roomRandoExcludedRegions,
                                 mSettings.roomRandoMorphBallDoors,
                                 mSettings.roomRando == kRoomRandoMixed},
                                rng, error);
      RebuildIncoming();
      if (mDockRemap.empty()) {
        return false;
      }
      const int starts = mRandomStarts.empty() ? 1 : kStartsPerLayout;
      for (int i = 0; i < starts; ++i) {
        if (!mRandomStarts.empty()) {
          mStartNode = mRandomStarts[rng() % mRandomStarts.size()];
        }
        if (IsLayoutOpen()) {
          return true;
        }
      }
    }
    mDockRemap.clear();
    RebuildIncoming();
    return false;
  }

  // Whether the room layout is worth filling: the starting items reach a pickup location, and
  // with every item the game can be finished and every location is visited with a way back.
  // Rerolling layouts that fail this is much cheaper than a fill that can't succeed.
  bool IsLayoutOpen() const {
    // Quick reject first: everything collected and every event done, with the events that close
    // paths either all done or none. Most layouts that fail, fail this.
    std::vector< uint8_t > safe(mDb.Nodes().size(), 0);
    for (const ResourceState& all : mAllResources) {
      const std::vector< uint8_t > forward = Forward(mStartNode, all);
      const std::vector< uint8_t > back = Backward(mStartNode, all);
      for (size_t node = 0; node < safe.size(); ++node) {
        safe[node] |= forward[node] & back[node];
      }
    }
    if (std::any_of(mLocations.begin(), mLocations.end(),
                    [&](int index) { return !safe[mDb.PickupNodes()[index]]; })) {
      return false;
    }

    const std::vector< int > noPickups(mDb.PickupNodes().size(), -1);
    std::vector< uint8_t > collected(noPickups.size(), 0);
    ResourceState state = *mStartState;
    const Reach start = Explore(state, noPickups, collected);
    if (std::none_of(mLocations.begin(), mLocations.end(),
                     [&](int index) { return start.IsSafe(mDb.PickupNodes()[index]); })) {
      return false;
    }
    for (const PoolEntry& entry : mPool) {
      Apply(state, entry);
    }
    const Reach all = Explore(state, noPickups, collected);
    return IsSatisfied(mDb.Victory(), mCtx, state) &&
           std::all_of(mLocations.begin(), mLocations.end(),
                       [&](int index) { return all.IsSafe(mDb.PickupNodes()[index]); });
  }

  // Where a node's connection leads, following doors the room randomizer moved. A moved door
  // keeps its own requirement, and also needs the lock on the door it now opens into, as coming
  // out behind a blast shield still means getting past it.
  int ConnectionTarget(int node, int connection) const {
    if (mDockRemap.empty() || mDockRemap[node] < 0) {
      return mDb.GetNode(node).connections[connection].target;
    }
    const Node& from = mDb.GetNode(node);
    return from.dock->connection == connection ? mDockRemap[node]
                                               : from.connections[connection].target;
  }

  bool CanTraverse(int node, int connection, int target, const ResourceState& state) const {
    const Connection& conn = mDb.GetNode(node).connections[connection];
    return IsSatisfied(conn.requirement, mCtx, state) &&
           (target == conn.target || IsSatisfied(mDb.GetNode(target).dock->lock, mCtx, state));
  }

  // Incoming connections of every node, as (source node, connection index), for walking the
  // graph backwards. Depends on the room layout.
  void RebuildIncoming() {
    mIncoming.assign(mDb.Nodes().size(), {});
    for (int node = 0; node < static_cast< int >(mDb.Nodes().size()); ++node) {
      for (int c = 0; c < static_cast< int >(mDb.GetNode(node).connections.size()); ++c) {
        mIncoming[ConnectionTarget(node, c)].emplace_back(node, c);
      }
    }
  }

  // Nodes reachable from `from`. With `stop`, returns as soon as a node flagged in it is found,
  // with that node visited.
  std::vector< uint8_t > Forward(int from, const ResourceState& state,
                                 const std::vector< uint8_t >* stop = nullptr) const {
    std::vector< uint8_t > visited(mDb.Nodes().size(), 0);
    std::vector< int > queue{from};
    visited[from] = 1;
    while (!queue.empty()) {
      const int node = queue.back();
      queue.pop_back();
      if (stop != nullptr && (*stop)[node]) {
        return visited;
      }
      const Node& n = mDb.GetNode(node);
      for (int c = 0; c < static_cast< int >(n.connections.size()); ++c) {
        const int target = ConnectionTarget(node, c);
        if (!visited[target] && CanTraverse(node, c, target, state)) {
          visited[target] = 1;
          queue.push_back(target);
        }
      }
    }
    return visited;
  }

  // Nodes that can reach `to`.
  std::vector< uint8_t > Backward(int to, const ResourceState& state) const {
    std::vector< uint8_t > visited(mDb.Nodes().size(), 0);
    std::vector< int > queue{to};
    visited[to] = 1;
    while (!queue.empty()) {
      const int node = queue.back();
      queue.pop_back();
      for (const auto& [source, c] : mIncoming[node]) {
        if (!visited[source] && CanTraverse(source, c, node, state)) {
          visited[source] = 1;
          queue.push_back(source);
        }
      }
    }
    return visited;
  }

  // Whether the player can get from `node` back to the start. `home` are nodes already known to
  // reach the start with fewer resources than `state`, which only holds when `state` adds nothing
  // that closes paths.
  bool CanReturn(int node, const ResourceState& state, const std::vector< uint8_t >* home) const {
    if (home != nullptr) {
      const std::vector< uint8_t > visited = Forward(node, state, home);
      for (size_t i = 0; i < visited.size(); ++i) {
        if (visited[i] && (*home)[i]) {
          return true;
        }
      }
      return false;
    }
    return Forward(node, state)[mStartNode] != 0;
  }

  // What the player can reach from the start, and which of it they can also come back from.
  // Pickups and events are only worth anything at nodes the player can come back from: going
  // somewhere one-way and finding nothing that opens a way out is a softlock. The room
  // randomizer makes these one-way trips common, as a one-way drop between two vanilla rooms
  // can now lead into the rest of a region.
  struct Reach {
    std::vector< uint8_t > forward;
    std::vector< uint8_t > back;
    bool IsSafe(int node) const { return forward[node] && back[node]; }
  };

  // Whether collecting `gain` at `node` leaves the player able to get back to the start.
  bool IsCollectable(const Reach& reach, int node, const ResourceState& gain,
                     bool closesPaths) const {
    if (reach.back[node] && !closesPaths) {
      return true;
    }
    return CanReturn(node, gain, closesPaths ? nullptr : &reach.back);
  }

  // Expands reachability until nothing changes, collecting events and every placed pickup the
  // player can collect and still return from.
  Reach Explore(ResourceState& state, const std::vector< int >& placement,
                std::vector< uint8_t >& collected) const {
    for (;;) {
      Reach reach{Forward(mStartNode, state), Backward(mStartNode, state)};
      bool changed = false;
      bool restart = false;
      for (size_t i = 0; i < reach.forward.size() && !restart; ++i) {
        if (!reach.forward[i]) {
          continue;
        }
        const int index = static_cast< int >(i);
        const Node& node = mDb.GetNode(index);
        if (node.type == NodeType::Event && node.event >= 0 && !state.events[node.event]) {
          ResourceState after = state;
          after.events[node.event] = 1;
          const bool closesPaths = mNegatedEvents[node.event] != 0;
          // Beating the game ends it, there's no coming back from the credits.
          if (mVictoryEvents[node.event] || IsCollectable(reach, index, after, closesPaths)) {
            state = std::move(after);
            changed = true;
            // The reach computed before no longer holds once paths close.
            restart = closesPaths;
          }
        } else if (node.type == NodeType::Pickup && !collected[node.pickupIndex] &&
                   placement[node.pickupIndex] >= 0) {
          ResourceState after = state;
          Apply(after, mPool[placement[node.pickupIndex]]);
          if (IsCollectable(reach, index, after, false)) {
            collected[node.pickupIndex] = 1;
            state = std::move(after);
            changed = true;
          }
        }
      }
      if (!changed) {
        return reach;
      }
    }
  }

  bool Fill(std::mt19937_64& rng, std::vector< int >& placement) {
    placement.assign(mDb.PickupNodes().size(), -1);

    std::vector< int > progression;
    std::vector< int > filler;
    for (int i = 0; i < static_cast< int >(mPool.size()); ++i) {
      (mPool[i].logic.empty() ? filler : progression).push_back(i);
    }
    std::shuffle(progression.begin(), progression.end(), rng);
    // Items are placed from the back. Place majors before expansions so they aren't crowded out
    // of the few locations reachable early.
    std::stable_partition(progression.begin(), progression.end(),
                          [this](int i) { return !mPool[i].major; });

    // Assumed fill: place the last item first, assuming everything else is collected.
    while (!progression.empty()) {
      const int item = progression.back();
      progression.pop_back();

      ResourceState state = *mStartState;
      for (const int other : progression) {
        Apply(state, mPool[other]);
      }
      std::vector< uint8_t > collected(placement.size(), 0);
      const Reach reach = Explore(state, placement, collected);
      // Somewhere the player can't come back from only works if this item is the way out.
      ResourceState withItem = state;
      Apply(withItem, mPool[item]);
      const std::vector< uint8_t > backWithItem = Backward(mStartNode, withItem);

      std::vector< int > candidates;
      std::vector< double > weights;
      for (const int index : mLocations) {
        const int node = mDb.PickupNodes()[index];
        if (placement[index] < 0 && reach.forward[node] && backWithItem[node]) {
          candidates.push_back(index);
          weights.push_back(mPool[item].major == mDb.GetNode(node).major ? 3.0 : 1.0);
        }
      }
      if (candidates.empty()) {
        return false;
      }
      std::discrete_distribution< size_t > pick(weights.begin(), weights.end());
      placement[candidates[pick(rng)]] = item;
    }

    std::vector< int > empty;
    for (const int index : mLocations) {
      if (placement[index] < 0) {
        empty.push_back(index);
      }
    }
    std::shuffle(empty.begin(), empty.end(), rng);
    std::shuffle(filler.begin(), filler.end(), rng);
    if (empty.size() != filler.size()) {
      return false;
    }
    for (size_t i = 0; i < empty.size(); ++i) {
      placement[empty[i]] = filler[i];
    }
    return true;
  }

  // Collects pickups sphere by sphere from the starting items alone, and checks the victory
  // condition is met at the end. A pickup only counts once the player can collect it and still
  // get back.
  bool Playthrough(const std::vector< int >& placement,
                   std::vector< std::vector< std::string > >* spheres) const {
    ResourceState state = *mStartState;
    std::vector< uint8_t > collected(placement.size(), 0);
    for (;;) {
      // Reach as far as possible with the current items, collecting events but not pickups.
      std::vector< int > noPickups(placement.size(), -1);
      std::vector< uint8_t > none(placement.size(), 0);
      const Reach reach = Explore(state, noPickups, none);

      std::vector< std::string > sphere;
      std::vector< int > found;
      for (const int index : mLocations) {
        const int node = mDb.PickupNodes()[index];
        if (collected[index] || !reach.forward[node]) {
          continue;
        }
        ResourceState after = state;
        Apply(after, mPool[placement[index]]);
        if (IsCollectable(reach, node, after, false)) {
          found.push_back(index);
        }
      }
      if (found.empty()) {
        break;
      }
      for (const int index : found) {
        collected[index] = 1;
        const PoolEntry& entry = mPool[placement[index]];
        Apply(state, entry);
        if (spheres != nullptr && !entry.logic.empty()) {
          sphere.push_back(
              fmt::format("{}: {}", mDb.NodeName(mDb.PickupNodes()[index]), entry.name));
        }
      }
      if (spheres != nullptr && !sphere.empty()) {
        spheres->push_back(std::move(sphere));
      }
    }
    return IsSatisfied(mDb.Victory(), mCtx, state);
  }

  Seed MakeSeed(const std::vector< int >& placement,
                std::vector< std::vector< std::string > > spheres, uint64_t seedHash) const {
    Seed seed;
    seed.hash = HashToId(seedHash);
    seed.seedString = mSettings.seedString;
    seed.settings = mSettings.ToJson();
    seed.spheres = std::move(spheres);
    seed.shuffledArtifacts = mShuffledArtifacts;
    for (const int index : placement) {
      const PoolEntry& entry = mPool[index];
      seed.locations.push_back({entry.name, entry.model, entry.grant});
    }
    for (const auto& entry : mStarting) {
      seed.startingItems.push_back({entry.name, entry.grant});
    }
    const Node& start = mDb.GetNode(mStartNode);
    const Area& area = mDb.Areas()[start.area];
    seed.startName = mDb.NodeName(mStartNode);
    seed.randomStart = !mRandomStarts.empty();
    seed.startWorld = mDb.Regions()[area.region].assetId;
    seed.startArea = area.assetId;
    if (mStartNode != mDb.StartNode()) {
      if (const std::optional< StartPlacement > placement = PlaceStart(mDb, mStartNode)) {
        seed.startPosition = placement->position;
        seed.startYaw = placement->yaw;
      }
    }
    for (int node = 0; node < static_cast< int >(mDockRemap.size()); ++node) {
      const int target = mDockRemap[node];
      if (target < 0 || target == mDb.GetNode(node).dock->target) {
        continue;
      }
      const Node& from = mDb.GetNode(node);
      const Node& to = mDb.GetNode(target);
      seed.docks.push_back({mDb.Regions()[mDb.Areas()[from.area].region].assetId,
                            mDb.Areas()[from.area].assetId, from.dock->index,
                            mDb.Regions()[mDb.Areas()[to.area].region].assetId,
                            mDb.Areas()[to.area].assetId, to.dock->index, mDb.NodeName(node),
                            mDb.NodeName(target), to.dock->type == "morph_ball"});
    }
    if (mTrimmed > 0) {
      seed.warnings.push_back(fmt::format(
          "Removed {} {}(s) to fit the pool into the available locations", mTrimmed, kFillerPickup));
    }
    if (mPadded > 0) {
      seed.warnings.push_back(
          fmt::format("Added {} {}(s) to fill empty locations", mPadded, kFillerPickup));
    }
    return seed;
  }

  void Report(float progress, std::string_view status) const {
    if (mProgress) {
      mProgress(progress, status);
    }
  }

  const Database& mDb;
  const PickupDatabase& mPickups;
  const Settings& mSettings;
  const ProgressCallback& mProgress;
  const std::atomic< bool >* mCancel;
  LogicContext mCtx;
  int mStartNode = -1;
  std::vector< int > mRandomStarts;
  std::vector< int > mDockRemap; // per node, where its dock leads; empty when rooms aren't shuffled
  std::vector< std::vector< std::pair< int, int > > > mIncoming;
  std::vector< uint8_t > mNegatedEvents; // per event, whether a requirement needs it not to happen
  std::vector< uint8_t > mVictoryEvents; // per event, whether the victory condition needs it
  std::vector< int > mLocations;
  std::vector< PoolEntry > mPool;
  std::vector< PoolEntry > mStarting;
  std::optional< ResourceState > mStartState;
  // Every item and event, for quickly rejecting room layouts. See IsLayoutOpen.
  std::vector< ResourceState > mAllResources;
  uint32_t mShuffledArtifacts = 0;
  int mTrimmed = 0;
  int mPadded = 0;
  int mFillFailures = 0;
  int mPlaythroughFailures = 0;
  int mRoomFailures = 0;
};

} // namespace

std::optional< Seed > Generate(const GeneratorInput& input, std::string& error,
                               const ProgressCallback& progress,
                               const std::atomic< bool >* cancel) {
  if (!input.db.IsLoaded()) {
    error = "The logic database isn't loaded";
    return std::nullopt;
  }
  return Generator(input, progress, cancel).Run(error);
}

int CountPoolSize(const PickupDatabase& pickups, const Settings& settings) {
  int count = std::clamp(settings.artifactTarget, 0, 12);
  for (const auto& def : pickups.standard) {
    const auto it = settings.standard.find(def.name);
    count += it != settings.standard.end() ? it->second.shuffled : def.defaultShuffled;
  }
  for (const auto& def : pickups.ammo) {
    const auto it = settings.ammo.find(def.name);
    count += it != settings.ammo.end() ? it->second.count : def.defaultCount;
  }
  return count;
}

std::vector< std::pair< std::string, std::string > > StartingLocations(const Database& db) {
  std::vector< std::pair< std::string, std::string > > result;
  // The generator only counts what the player can come back from, so a start nothing leads back
  // to (the Frigate's one-way rooms, spawn points inside tunnels) can never reach anything.
  std::vector< uint8_t > hasIncoming(db.Nodes().size(), 0);
  for (const Node& node : db.Nodes()) {
    for (const Connection& conn : node.connections) {
      hasIncoming[conn.target] = 1;
    }
  }
  for (const auto& region : db.Regions()) {
    for (const int areaIndex : region.areas) {
      const Area& area = db.Areas()[areaIndex];
      for (const int nodeIndex : area.nodes) {
        const Node& node = db.GetNode(nodeIndex);
        // Only offer starts the game can place Samus at.
        if (!node.validStart || !hasIncoming[nodeIndex] ||
            (!PlaceStart(db, nodeIndex) && nodeIndex != db.StartNode())) {
          continue;
        }
        result.emplace_back(region.name + "|" + area.name + "|" + node.name,
                            area.name + " (" + region.name + ")");
      }
    }
  }
  return result;
}

std::string RandomSeedString() {
  std::random_device rd;
  std::uniform_int_distribution< uint64_t > dist;
  return HashToId(dist(rd)).substr(0, 8) + HashToId(dist(rd)).substr(0, 4);
}

} // namespace metaforce::randomizer
