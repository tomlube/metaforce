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
      if (roomRando) {
        if (!ShuffleRoomsWithOpenStart(rng)) {
          ++mRoomFailures;
          continue;
        }
      } else {
        AssignPreFillWeaknesses();
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
      if (!mDocksToAssign.empty()) {
        Report(static_cast< float >(attempt) / maxAttempts,
               fmt::format("Attempt {}: placing door locks", attempt + 1));
        DistributeDoorLocks(rng, placement);
        spheres.clear();
        if (!Playthrough(placement, &spheres)) {
          ++mDoorLockFailures;
          continue;
        }
      }
      return MakeSeed(placement, std::move(spheres), seedHash);
    }
    error = fmt::format("Could not generate a beatable seed after {} attempts ({} fill, {} "
                        "playthrough, {} room layout, {} door lock failures). Try more lenient "
                        "settings, a higher trick level, or more items in the pool.",
                        maxAttempts, mFillFailures, mPlaythroughFailures, mRoomFailures,
                        mDoorLockFailures);
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
    if (mSettings.doorLockMode == kDoorLockIndividual && !SetupDoorLocks(error)) {
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

  // Door lock randomizer, after Randovania's dock weakness distributor in "Individually" mode:
  // every door whose lock may change is unlocked for the fill, then given a lock afterwards that
  // the player can open by the time they first reach it.
  bool SetupDoorLocks(std::string& error) {
    mDoorTypes = mDb.FindDockType("door");
    if (mDoorTypes == nullptr) {
      error = "The logic database has no door lock randomizer settings";
      return false;
    }
    mChangeFrom.assign(mDb.Weaknesses().size(), 0);
    mChangeTo.assign(mDb.Weaknesses().size(), 0);
    for (const int weakness : mDoorTypes->changeFrom) {
      mChangeFrom[weakness] = mSettings.doorLockChangeFrom.contains(mDb.Weaknesses()[weakness].name);
    }
    for (const int weakness : mDoorTypes->changeTo) {
      mChangeTo[weakness] = mSettings.doorLockChangeTo.contains(mDb.Weaknesses()[weakness].name);
    }
    // The unlocked door is what a door falls back to when nothing else fits.
    mChangeTo[mDoorTypes->unlocked] = 1;
    mForcedUnlocked = mSettings.unlockSaveStationDoors ? mDb.FindWeakness("door/Normal Door (Forced)")
                                                        : -1;
    // Randovania's logic has a few tricks that only work with the game's own door locks.
    if (const int misc = mDb.FindMisc("dock_rando"); misc >= 0) {
      mCtx.misc[misc] = 1;
    }
    if (const int misc = mDb.FindMisc("blue_save_doors"); misc >= 0 && mForcedUnlocked >= 0) {
      mCtx.misc[misc] = 1;
    }
    mDoorLocks = true;
    return true;
  }

  // Where a dock node leads with the current room layout.
  int DockTarget(int node) const {
    return mDockRemap.empty() || mDockRemap[node] < 0 ? mDb.GetNode(node).dock->target
                                                      : mDockRemap[node];
  }

  // Gives every dock its weakness for the fill: save room doors become blue, and the doors whose
  // lock is randomized are unlocked. Depends on the room layout.
  void AssignPreFillWeaknesses() {
    mDocksToAssign.clear();
    if (!mDoorLocks) {
      return;
    }
    const int nodeCount = static_cast< int >(mDb.Nodes().size());
    mWeakness.assign(nodeCount, -1);
    for (int node = 0; node < nodeCount; ++node) {
      if (const Node& n = mDb.GetNode(node); n.dock) {
        mWeakness[node] = n.dock->weaknessIndex;
      }
    }
    // Randovania leaves doors it already changed alone.
    std::vector< uint8_t > forced(nodeCount, 0);
    if (mForcedUnlocked >= 0) {
      for (int node = 0; node < nodeCount; ++node) {
        const Node& n = mDb.GetNode(node);
        if (!n.dock || n.dock->type != "door" || !mDb.Areas()[n.area].saveStation) {
          continue;
        }
        for (const int side : {node, DockTarget(node)}) {
          if (side >= 0 && mDb.GetNode(side).dock && mDb.GetNode(side).dock->type == "door") {
            mWeakness[side] = mForcedUnlocked;
            forced[side] = 1;
          }
        }
      }
    }
    std::vector< uint8_t > listed(nodeCount, 0);
    for (int node = 0; node < nodeCount; ++node) {
      const Node& n = mDb.GetNode(node);
      if (!n.dock || forced[node] || n.dock->type != "door" || n.dock->excludeFromDockRando ||
          !mChangeFrom[n.dock->weaknessIndex]) {
        continue;
      }
      mWeakness[node] = mDoorTypes->unlocked;
      // One entry per pair of doors; the lock goes on both when both may change.
      const int target = DockTarget(node);
      if (target < 0 || !listed[target]) {
        mDocksToAssign.push_back(node);
      }
      listed[node] = 1;
    }
  }

  // Crossing dock `node` into `target` with the door locks of this seed. Like Randovania, the
  // lock on the far side of the door only matters for kinds of lock that can't be removed from
  // behind.
  bool CanCrossDock(int node, int target, const ResourceState& state) const {
    if (!mBlocked.empty() && mBlocked[node]) {
      return false;
    }
    const int weakness = mWeakness[node];
    if (!IsSatisfied(mDb.DockOpenRequirement(node, weakness), mCtx, state)) {
      return false;
    }
    if (const Requirement* lock = mDb.DockLockRequirement(node, weakness);
        lock != nullptr && !IsSatisfied(*lock, mCtx, state)) {
      return false;
    }
    if (!mDb.GetNode(target).dock) {
      return true;
    }
    const int back = mWeakness[target];
    switch (mDb.Weaknesses()[back].lockType) {
    case DockLockType::FrontBlastBackImpossible:
      return false;
    case DockLockType::FrontBlastBackIfMatching:
      return back == weakness;
    case DockLockType::FrontBlastBackBlast:
      return back == weakness || IsSatisfied(*mDb.DockLockRequirement(target, back), mCtx, state);
    default:
      return true;
    }
  }

  // What the player has when they first reach either side of a door, with the door itself
  // impassable: pickups are collected sphere by sphere until one side comes into reach. Returns
  // the side reached as well, or nothing if neither side ever is.
  std::optional< std::pair< ResourceState, int > > ReachDoor(const std::vector< int >& placement,
                                                             int dock, int target) {
    mBlocked[dock] = 1;
    mBlocked[target] = 1;
    std::optional< std::pair< ResourceState, int > > result;
    ResourceState state = *mStartState;
    std::vector< uint8_t > collected(placement.size(), 0);
    const std::vector< int > noPickups(placement.size(), -1);
    for (;;) {
      std::vector< uint8_t > none(placement.size(), 0);
      const Reach reach = Explore(state, noPickups, none);
      if (reach.forward[dock] || reach.forward[target]) {
        result.emplace(state, reach.forward[dock] ? dock : target);
        break;
      }
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
        Apply(state, mPool[placement[index]]);
      }
    }
    mBlocked[dock] = 0;
    mBlocked[target] = 0;
    return result;
  }

  // The weaknesses door `dock` (the side the player reached first) and `target` could get, with
  // their weights, after Randovania's _determine_valid_weaknesses: always the unlocked door; a
  // permanently locked door, twice as likely, when the player can get around it both ways; and
  // any other allowed lock the player can already open.
  std::vector< std::pair< int, double > > ValidWeaknesses(int dock, int target,
                                                          const ResourceState& state) {
    std::vector< std::pair< int, double > > weighted{{mDoorTypes->unlocked, 1.0}};
    std::vector< uint8_t > excluded(mDb.Weaknesses().size(), 0);
    for (const int side : {dock, target}) {
      for (const int weakness : mDb.GetNode(side).dock->incompatibleWeaknesses) {
        excluded[weakness] = 1;
      }
    }
    excluded[mDoorTypes->unlocked] = 1;
    const int locked = mDoorTypes->locked;
    if (locked >= 0 && mChangeTo[locked] && !excluded[locked]) {
      mBlocked[dock] = 1;
      mBlocked[target] = 1;
      if (Forward(dock, state)[target] && Forward(target, state)[dock]) {
        weighted.emplace_back(locked, 2.0);
      }
      mBlocked[dock] = 0;
      mBlocked[target] = 0;
    }
    if (locked >= 0) {
      excluded[locked] = 1;
    }
    for (const int weakness : mDoorTypes->changeTo) {
      const DockWeakness& w = mDb.Weaknesses()[weakness];
      if (mChangeTo[weakness] && !excluded[weakness] && IsSatisfied(w.open, mCtx, state) &&
          (!w.lock || IsSatisfied(*w.lock, mCtx, state))) {
        weighted.emplace_back(weakness, 1.0);
      }
    }
    return weighted;
  }

  // Gives each door listed by AssignPreFillWeaknesses its lock, in random order, each one picked
  // with the locks placed before it in effect.
  void DistributeDoorLocks(std::mt19937_64& rng, const std::vector< int >& placement) {
    mBlocked.assign(mDb.Nodes().size(), 0);
    std::vector< int > order = mDocksToAssign;
    std::shuffle(order.begin(), order.end(), rng);
    const bool onlyUnlocked = std::count(mChangeTo.begin(), mChangeTo.end(), 1) == 1;
    for (const int node : order) {
      const int target = DockTarget(node);
      std::vector< std::pair< int, double > > weighted{{mDoorTypes->unlocked, 1.0}};
      if (!onlyUnlocked && target >= 0) {
        if (auto reached = ReachDoor(placement, node, target)) {
          const bool fromTarget = reached->second == target;
          weighted = ValidWeaknesses(fromTarget ? target : node, fromTarget ? node : target,
                                     reached->first);
        }
      }
      std::vector< double > weights;
      for (const auto& [weakness, weight] : weighted) {
        weights.push_back(weight);
      }
      std::discrete_distribution< size_t > pick(weights.begin(), weights.end());
      const int weakness = weighted[pick(rng)].first;
      mWeakness[node] = weakness;
      if (target >= 0) {
        const DockInfo& other = *mDb.GetNode(target).dock;
        if (other.type == "door" && !other.excludeFromDockRando &&
            mChangeFrom[other.weaknessIndex] && mWeakness[target] != mForcedUnlocked) {
          mWeakness[target] = weakness;
        }
      }
    }
    mBlocked.clear();
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
      // Save room doors and the doors whose locks get randomized depend on where doors lead.
      AssignPreFillWeaknesses();
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
    const Node& from = mDb.GetNode(node);
    if (!mWeakness.empty() && from.dock && from.dock->connection == connection) {
      return CanCrossDock(node, target, state);
    }
    const Connection& conn = from.connections[connection];
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
    if (mDoorLocks) {
      seed.doorLocksRandomized = true;
      seed.blastShieldLockOn = mSettings.blastShieldLockOn;
      for (int node = 0; node < static_cast< int >(mWeakness.size()); ++node) {
        const Node& n = mDb.GetNode(node);
        if (!n.dock || n.dock->type != "door") {
          continue;
        }
        const DockWeakness& weakness = mDb.Weaknesses()[mWeakness[node]];
        const DockWeakness& original = mDb.Weaknesses()[n.dock->weaknessIndex];
        const bool changed =
            weakness.shield != original.shield || weakness.blastShield != original.blastShield;
        // Every blast shield is listed: the game's own missile blast shields are replaced too.
        if ((!changed && weakness.blastShield.empty()) || weakness.shield.empty()) {
          continue;
        }
        const Area& area = mDb.Areas()[n.area];
        seed.doorLocks.push_back({mDb.Regions()[area.region].assetId, area.assetId, n.dock->index,
                                  weakness.shield, weakness.blastShield,
                                  weakness.name.substr(weakness.name.find('/') + 1),
                                  mDb.NodeName(node), changed});
      }
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
  // Door lock randomizer, see SetupDoorLocks.
  bool mDoorLocks = false;
  const DockTypeDistributor* mDoorTypes = nullptr;
  std::vector< uint8_t > mChangeFrom; // per weakness, from the settings
  std::vector< uint8_t > mChangeTo;
  int mForcedUnlocked = -1; // the weakness save room doors get, -1 to leave them alone
  // Per node, its dock's weakness in this seed. Empty when door locks aren't randomized, and
  // docks are crossed by their connections in the database.
  std::vector< int > mWeakness;
  std::vector< int > mDocksToAssign; // doors to give a lock after the fill, one per pair
  std::vector< uint8_t > mBlocked;   // per node, docks closed while their lock is picked
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
  int mDoorLockFailures = 0;
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
