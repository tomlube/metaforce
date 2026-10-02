#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace metaforce::randomizer {

// Randovania's Metroid Prime logic database, as exported by tools/randomizer/export_randovania.py.

enum class ResourceType : uint8_t {
  Item,
  Event,
  Trick,
  Damage,
  Misc,
};

struct Requirement {
  enum class Kind : uint8_t {
    And,
    Or,
    Resource,
  };

  Kind kind = Kind::And;
  ResourceType resourceType = ResourceType::Item;
  bool negate = false;
  int resource = -1;
  int amount = 0;
  std::vector< Requirement > children;

  static Requirement Trivial() { return {}; }
  static Requirement Impossible() { return {.kind = Kind::Or}; }
  bool IsTrivial() const { return kind == Kind::And && children.empty(); }
};

struct ItemResource {
  std::string name;
  std::string longName;
  int maxCapacity = 0;
  int itemId = -1; // CPlayerState::EItemType when 0..40, Randovania-only items otherwise
};

struct NamedResource {
  std::string name;
  std::string longName;
};

struct TrickResource {
  std::string name;
  std::string longName;
  std::string description;
};

struct DamageReduction {
  int item;
  float multiplier;
};

enum class NodeType : uint8_t {
  Generic,
  Pickup,
  Event,
  Dock,
};

struct Connection {
  int target;
  Requirement requirement;
};

// A dock node's door: where it leads in the vanilla game and what the room randomizer needs to
// pair it with another door.
struct DockInfo {
  std::string type;     // Randovania dock type: "door", "morph_ball", "teleporter", ...
  std::string weakness; // "type/weakness"
  int index = -1;       // dock number in the area, as in the MLVL and the Dock script objects
  bool nonstandard = false;
  // Dock plane size and facing (0 wall, 1 ceiling, -1 floor, 2 tilted wall), when the database
  // has them.
  bool hasShape = false;
  float width = 0.f;
  float height = 0.f;
  int facing = 0;
  int target = -1;      // vanilla destination node
  int connection = -1;  // index of the dock's connection in Node::connections, -1 if impassable
  Requirement lock;     // what opening the door's lock (blast shield) takes, trivial if none
};

struct Node {
  std::string name;
  NodeType type = NodeType::Generic;
  int area = -1;
  int pickupIndex = -1;
  int event = -1;
  bool major = false;
  bool heal = false;
  bool validStart = false;
  std::optional< std::array< float, 3 > > worldPosition;
  std::vector< Connection > connections;
  std::optional< DockInfo > dock;
};

struct Area {
  std::string name;
  uint32_t assetId = 0;
  int region = -1;
  int defaultNode = -1;
  std::vector< int > nodes;
};

struct Region {
  std::string name;
  uint32_t assetId = 0;
  std::vector< int > areas;
};

class Database {
public:
  bool Load(const std::filesystem::path& path, std::string& error);
  bool IsLoaded() const { return !mNodes.empty(); }

  const std::vector< ItemResource >& Items() const { return mItems; }
  const std::vector< NamedResource >& Events() const { return mEvents; }
  const std::vector< TrickResource >& Tricks() const { return mTricks; }
  const std::vector< NamedResource >& Damage() const { return mDamage; }
  const std::vector< NamedResource >& Misc() const { return mMisc; }
  const std::vector< DamageReduction >& DamageReductions(int damage) const {
    return mDamageReductions[damage];
  }

  const std::vector< Region >& Regions() const { return mRegions; }
  const std::vector< Area >& Areas() const { return mAreas; }
  const std::vector< Node >& Nodes() const { return mNodes; }
  const Node& GetNode(int index) const { return mNodes[index]; }

  int StartNode() const { return mStartNode; }
  const Requirement& Victory() const { return mVictory; }

  // Node index for each pickup index, -1 for unknown indices.
  const std::vector< int >& PickupNodes() const { return mPickupNodes; }

  int FindItem(std::string_view name) const;
  int FindEvent(std::string_view name) const;
  int FindMisc(std::string_view name) const;
  int FindNode(std::string_view region, std::string_view area, std::string_view node) const;
  // Whether every dock node has a shape, which the room randomizer needs.
  bool HasDockShapes() const { return mHasDockShapes; }
  // "Region / Area / Node"
  std::string NodeName(int node) const;

private:
  std::vector< ItemResource > mItems;
  std::vector< NamedResource > mEvents;
  std::vector< TrickResource > mTricks;
  std::vector< NamedResource > mDamage;
  std::vector< NamedResource > mMisc;
  std::vector< std::vector< DamageReduction > > mDamageReductions;
  std::vector< Region > mRegions;
  std::vector< Area > mAreas;
  std::vector< Node > mNodes;
  std::vector< int > mPickupNodes;
  std::unordered_map< std::string, int > mNodeLookup;
  int mStartNode = -1;
  Requirement mVictory;
  bool mHasDockShapes = false;
};

// What the player has collected, as Randovania resources.
struct ResourceState {
  std::vector< int > items;
  std::vector< uint8_t > events;

  explicit ResourceState(const Database& db)
  : items(db.Items().size(), 0), events(db.Events().size(), 0) {}
};

struct LogicContext {
  const Database& db;
  std::vector< int > trickLevels; // per trick, 0 (disabled) to 5 (ludicrous)
  std::vector< uint8_t > misc;    // per misc resource
  float damageStrictness = 1.f;
  int energyPerTank = 100;
  int energyTankItem = -1;
  int variaItem = -1;
  int gravityItem = -1;
  int phazonItem = -1;
};

bool IsSatisfied(const Requirement& req, const LogicContext& ctx, const ResourceState& state);

} // namespace metaforce::randomizer
