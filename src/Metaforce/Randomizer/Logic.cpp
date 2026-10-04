#include "Metaforce/Randomizer/Logic.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <iterator>

namespace metaforce::randomizer {
namespace {
using json = nlohmann::json;

constexpr int kLogicVersion = 3;

// Locks Randovania can give doors that Metaforce doesn't offer: a Power Beam Only door is more
// annoying than interesting.
constexpr std::string_view kUnofferedDoorLocks[] = {"door/Power Beam Only Door"};

std::string NodeKey(std::string_view region, std::string_view area, std::string_view node) {
  std::string key;
  key.reserve(region.size() + area.size() + node.size() + 2);
  key.append(region).append("\x1f").append(area).append("\x1f").append(node);
  return key;
}

template < typename T >
int IndexOf(const std::vector< T >& list, std::string_view name) {
  for (size_t i = 0; i < list.size(); ++i) {
    if (list[i].name == name) {
      return static_cast< int >(i);
    }
  }
  return -1;
}

class RequirementParser {
public:
  RequirementParser(const Database& db, const json& templates) : mDb(db), mTemplates(templates) {}

  Requirement Parse(const json& node) {
    const std::string& kind = node.at(0).get_ref< const std::string& >();
    if (kind == "and" || kind == "or") {
      Requirement req{.kind = kind == "and" ? Requirement::Kind::And : Requirement::Kind::Or};
      for (const auto& child : node.at(1)) {
        req.children.push_back(Parse(child));
      }
      return req;
    }
    if (kind == "t") {
      const auto& name = node.at(1).get_ref< const std::string& >();
      if (auto it = mCache.find(name); it != mCache.end()) {
        return it->second;
      }
      Requirement req = Parse(mTemplates.at(name));
      mCache.emplace(name, req);
      return req;
    }
    if (kind == "r") {
      const auto& type = node.at(1).get_ref< const std::string& >();
      const auto& name = node.at(2).get_ref< const std::string& >();
      Requirement req{.kind = Requirement::Kind::Resource,
                      .negate = node.at(4).get< bool >(),
                      .amount = node.at(3).get< int >()};
      if (type == "items") {
        req.resourceType = ResourceType::Item;
        req.resource = IndexOf(mDb.Items(), name);
      } else if (type == "events") {
        req.resourceType = ResourceType::Event;
        req.resource = IndexOf(mDb.Events(), name);
      } else if (type == "tricks") {
        req.resourceType = ResourceType::Trick;
        req.resource = IndexOf(mDb.Tricks(), name);
      } else if (type == "damage") {
        req.resourceType = ResourceType::Damage;
        req.resource = IndexOf(mDb.Damage(), name);
      } else if (type == "misc") {
        req.resourceType = ResourceType::Misc;
        req.resource = IndexOf(mDb.Misc(), name);
      } else {
        throw std::runtime_error("unknown resource type " + type);
      }
      if (req.resource < 0) {
        throw std::runtime_error("unknown " + type + " resource " + name);
      }
      return req;
    }
    throw std::runtime_error("unknown requirement kind " + kind);
  }

private:
  const Database& mDb;
  const json& mTemplates;
  std::unordered_map< std::string, Requirement > mCache;
};

Requirement MakeAnd(Requirement a, Requirement b) {
  if (a.IsTrivial()) {
    return b;
  }
  if (b.IsTrivial()) {
    return a;
  }
  Requirement req{.kind = Requirement::Kind::And};
  req.children.push_back(std::move(a));
  req.children.push_back(std::move(b));
  return req;
}

} // namespace

bool Database::Load(const std::filesystem::path& path, std::string& error) {
  try {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
      error = "Could not open " + path.string();
      return false;
    }
    const json root = json::parse(file);
    if (root.at("version").get< int >() != kLogicVersion) {
      error = "Unsupported logic database version";
      return false;
    }

    const json& res = root.at("resources");
    for (const auto& item : res.at("items")) {
      mItems.push_back({item.at("name"), item.at("long_name"), item.at("max"), item.at("id")});
    }
    for (const auto& ev : res.at("events")) {
      mEvents.push_back({ev.at("name"), ev.at("long_name")});
    }
    for (const auto& trick : res.at("tricks")) {
      mTricks.push_back({trick.at("name"), trick.at("long_name"), trick.at("description")});
    }
    for (const auto& dmg : res.at("damage")) {
      mDamage.push_back({dmg.at("name"), dmg.at("long_name")});
    }
    for (const auto& misc : res.at("misc")) {
      mMisc.push_back({misc.at("name"), misc.at("long_name")});
    }
    mDamageReductions.resize(mDamage.size());
    for (const auto& [name, reductions] : root.at("damage_reductions").items()) {
      const int damage = IndexOf(mDamage, name);
      if (damage < 0) {
        continue;
      }
      for (const auto& reduction : reductions) {
        const int item = FindItem(reduction.at("item").get_ref< const std::string& >());
        if (item >= 0) {
          mDamageReductions[damage].push_back({item, reduction.at("multiplier").get< float >()});
        }
      }
    }

    RequirementParser parser(*this, root.at("templates"));

    for (const auto& [name, weakness] : root.at("dock_weaknesses").items()) {
      DockWeakness w{.name = name, .type = name.substr(0, name.find('/')),
                     .open = parser.Parse(weakness.at("open"))};
      if (!weakness.at("lock").is_null()) {
        w.lock = parser.Parse(weakness.at("lock"));
        const std::string& lockType = weakness.at("lock_type").get_ref< const std::string& >();
        w.lockType = lockType == "front-blast-back-free-unlock" ? DockLockType::FrontBlastBackFreeUnlock
                     : lockType == "front-blast-back-impossible" ? DockLockType::FrontBlastBackImpossible
                     : lockType == "front-blast-back-if-matching"
                         ? DockLockType::FrontBlastBackIfMatching
                         : DockLockType::FrontBlastBackBlast;
      }
      if (!weakness.at("shield").is_null()) {
        w.shield = weakness.at("shield");
      }
      if (!weakness.at("blast_shield").is_null()) {
        w.blastShield = weakness.at("blast_shield");
      }
      w.unsafe = weakness.at("unsafe");
      mWeaknesses.push_back(std::move(w));
    }
    const auto weaknessIndex = [this](const std::string& name) {
      const int index = FindWeakness(name);
      if (index < 0) {
        throw std::runtime_error("unknown dock weakness " + name);
      }
      return index;
    };
    for (const auto& [type, distributor] : root.at("dock_types").items()) {
      DockTypeDistributor d{.type = type, .label = distributor.at("label"),
                            .unlocked = weaknessIndex(distributor.at("unlocked"))};
      if (!distributor.at("locked").is_null()) {
        d.locked = weaknessIndex(distributor.at("locked"));
      }
      for (const auto& name : distributor.at("change_from")) {
        d.changeFrom.push_back(weaknessIndex(name));
      }
      for (const auto& name : distributor.at("change_to")) {
        if (std::find(std::begin(kUnofferedDoorLocks), std::end(kUnofferedDoorLocks),
                      name.get_ref< const std::string& >()) == std::end(kUnofferedDoorLocks)) {
          d.changeTo.push_back(weaknessIndex(name));
        }
      }
      mDockTypes.push_back(std::move(d));
    }

    // First pass: create regions, areas and nodes so connections can be resolved by name.
    struct PendingDock {
      int node;
      std::string target;
    };
    std::vector< PendingDock > docks;
    std::vector< std::pair< int, const json* > > pendingConnections;

    for (const auto& regionJson : root.at("regions")) {
      Region region{regionJson.at("name"), regionJson.at("asset_id").get< uint32_t >()};
      const int regionIndex = static_cast< int >(mRegions.size());
      for (const auto& areaJson : regionJson.at("areas")) {
        Area area{areaJson.at("name"), areaJson.at("asset_id").get< uint32_t >(), regionIndex};
        area.saveStation = areaJson.at("save_station");
        const int areaIndex = static_cast< int >(mAreas.size());
        const std::string& defaultNode = areaJson.at("default_node").is_null()
                                             ? std::string{}
                                             : areaJson.at("default_node").get< std::string >();
        for (const auto& nodeJson : areaJson.at("nodes")) {
          Node node{nodeJson.at("name"), NodeType::Generic, areaIndex};
          const std::string& type = nodeJson.at("type").get_ref< const std::string& >();
          node.heal = nodeJson.value("heal", false);
          node.validStart = nodeJson.value("start", false);
          if (nodeJson.contains("pos")) {
            const auto& pos = nodeJson.at("pos");
            node.worldPosition = {pos.at(0).get< float >(), pos.at(1).get< float >(),
                                  pos.at(2).get< float >()};
          }
          const int nodeIndex = static_cast< int >(mNodes.size());
          if (type == "pickup") {
            node.type = NodeType::Pickup;
            node.pickupIndex = nodeJson.at("index");
            node.major = nodeJson.value("category", "minor") == "major";
            if (node.pickupIndex >= static_cast< int >(mPickupNodes.size())) {
              mPickupNodes.resize(node.pickupIndex + 1, -1);
            }
            mPickupNodes[node.pickupIndex] = nodeIndex;
          } else if (type == "event") {
            node.type = NodeType::Event;
            node.event = FindEvent(nodeJson.at("event").get_ref< const std::string& >());
          } else if (type == "dock") {
            node.type = NodeType::Dock;
            const json& dock = nodeJson.at("dock");
            const json& target = dock.at("target");
            DockInfo& info = node.dock.emplace();
            info.type = dock.at("type");
            info.weakness = dock.at("weakness");
            info.weaknessIndex = weaknessIndex(info.weakness);
            info.excludeFromDockRando = dock.at("exclude");
            for (const auto& name : dock.at("incompatible")) {
              info.incompatibleWeaknesses.push_back(weaknessIndex(name));
            }
            if (!dock.at("open").is_null()) {
              info.openOverride = parser.Parse(dock.at("open"));
            }
            if (!dock.at("lock").is_null()) {
              info.lockOverride = parser.Parse(dock.at("lock"));
            }
            info.index = dock.at("index").is_null() ? -1 : dock.at("index").get< int >();
            info.nonstandard = dock.at("nonstandard");
            if (dock.contains("shape")) {
              const json& shape = dock.at("shape");
              info.hasShape = true;
              info.width = shape.at(0);
              info.height = shape.at(1);
              info.facing = shape.at(2);
            }
            docks.push_back({nodeIndex, NodeKey(target.at(0).get_ref< const std::string& >(),
                                                target.at(1).get_ref< const std::string& >(),
                                                target.at(2).get_ref< const std::string& >())});
          }
          if (node.name == defaultNode) {
            area.defaultNode = nodeIndex;
          }
          mNodeLookup.emplace(NodeKey(region.name, area.name, node.name), nodeIndex);
          area.nodes.push_back(nodeIndex);
          pendingConnections.emplace_back(nodeIndex, &nodeJson.at("connections"));
          mNodes.push_back(std::move(node));
        }
        region.areas.push_back(areaIndex);
        mAreas.push_back(std::move(area));
      }
      mRegions.push_back(std::move(region));
    }

    // Second pass: in-area connections, then dock connections between areas.
    for (const auto& [nodeIndex, connections] : pendingConnections) {
      const Area& area = mAreas[mNodes[nodeIndex].area];
      const Region& region = mRegions[area.region];
      for (const auto& [targetName, req] : connections->items()) {
        auto it = mNodeLookup.find(NodeKey(region.name, area.name, targetName));
        if (it == mNodeLookup.end()) {
          continue; // Target is on a layer that isn't exported
        }
        Requirement parsed = parser.Parse(req);
        if (parsed.kind == Requirement::Kind::Or && parsed.children.empty()) {
          continue;
        }
        mNodes[nodeIndex].connections.push_back({it->second, std::move(parsed)});
      }
    }
    for (auto& dock : docks) {
      auto target = mNodeLookup.find(dock.target);
      if (target == mNodeLookup.end()) {
        continue;
      }
      DockInfo& info = *mNodes[dock.node].dock;
      info.target = target->second;
      const Requirement* lock = DockLockRequirement(dock.node, info.weaknessIndex);
      if (lock != nullptr) {
        info.lock = *lock;
      }
      // Blast shields are treated as requiring their weapon from both sides, which is never
      // more permissive than the game.
      Requirement open = DockOpenRequirement(dock.node, info.weaknessIndex);
      Requirement req = lock != nullptr ? MakeAnd(std::move(open), *lock) : std::move(open);
      if (req.kind == Requirement::Kind::Or && req.children.empty()) {
        continue;
      }
      info.connection = static_cast< int >(mNodes[dock.node].connections.size());
      mNodes[dock.node].connections.push_back({target->second, std::move(req)});
    }
    mHasDockShapes = std::all_of(mNodes.begin(), mNodes.end(), [](const Node& node) {
      return !node.dock || node.dock->type == "teleporter" || node.dock->hasShape;
    });

    const json& start = root.at("starting_location");
    mStartNode = FindNode(start.at(0).get_ref< const std::string& >(),
                          start.at(1).get_ref< const std::string& >(),
                          start.at(2).get_ref< const std::string& >());
    mVictory = parser.Parse(root.at("victory"));
  } catch (const std::exception& e) {
    error = std::string("Failed to load logic database: ") + e.what();
    mNodes.clear();
    return false;
  }
  return true;
}

int Database::FindItem(std::string_view name) const { return IndexOf(mItems, name); }
int Database::FindEvent(std::string_view name) const { return IndexOf(mEvents, name); }
int Database::FindMisc(std::string_view name) const { return IndexOf(mMisc, name); }
int Database::FindWeakness(std::string_view name) const { return IndexOf(mWeaknesses, name); }

const DockTypeDistributor* Database::FindDockType(std::string_view type) const {
  for (const DockTypeDistributor& d : mDockTypes) {
    if (d.type == type) {
      return &d;
    }
  }
  return nullptr;
}

const Requirement& Database::DockOpenRequirement(int node, int weakness) const {
  const DockInfo& dock = *mNodes[node].dock;
  if (weakness == dock.weaknessIndex && dock.openOverride) {
    return *dock.openOverride;
  }
  return mWeaknesses[weakness].open;
}

const Requirement* Database::DockLockRequirement(int node, int weakness) const {
  const DockInfo& dock = *mNodes[node].dock;
  if (!mWeaknesses[weakness].lock) {
    return nullptr;
  }
  if (weakness == dock.weaknessIndex && dock.lockOverride) {
    return &*dock.lockOverride;
  }
  return &*mWeaknesses[weakness].lock;
}

int Database::FindNode(std::string_view region, std::string_view area, std::string_view node) const {
  auto it = mNodeLookup.find(NodeKey(region, area, node));
  return it == mNodeLookup.end() ? -1 : it->second;
}

std::string Database::NodeName(int node) const {
  const Node& n = mNodes[node];
  const Area& area = mAreas[n.area];
  return mRegions[area.region].name + " / " + area.name + " / " + n.name;
}

namespace {

// Matches the vanilla engine: the best suit's tweak reduction applies.
float SuitDamageMultiplier(const LogicContext& ctx, const ResourceState& state) {
  auto has = [&](int item) { return item >= 0 && state.items[item] > 0; };
  if (has(ctx.phazonItem)) {
    return 0.5f;
  }
  if (has(ctx.gravityItem)) {
    return 0.8f;
  }
  if (has(ctx.variaItem)) {
    return 0.9f;
  }
  return 1.f;
}

bool IsDamageSatisfied(const Requirement& req, const LogicContext& ctx,
                       const ResourceState& state) {
  float multiplier = 1.f;
  for (const auto& reduction : ctx.db.DamageReductions(req.resource)) {
    if (state.items[reduction.item] > 0) {
      multiplier = std::min(multiplier, reduction.multiplier);
    }
  }
  const float damage = static_cast< float >(req.amount) * multiplier *
                       SuitDamageMultiplier(ctx, state) * ctx.damageStrictness;
  const int tanks = ctx.energyTankItem >= 0 ? state.items[ctx.energyTankItem] : 0;
  const float energy = static_cast< float >(ctx.energyPerTank - 1 + tanks * ctx.energyPerTank);
  return damage < energy;
}

} // namespace

bool IsSatisfied(const Requirement& req, const LogicContext& ctx, const ResourceState& state) {
  switch (req.kind) {
  case Requirement::Kind::And:
    for (const auto& child : req.children) {
      if (!IsSatisfied(child, ctx, state)) {
        return false;
      }
    }
    return true;
  case Requirement::Kind::Or:
    for (const auto& child : req.children) {
      if (IsSatisfied(child, ctx, state)) {
        return true;
      }
    }
    return false;
  case Requirement::Kind::Resource:
    break;
  }

  bool result = false;
  switch (req.resourceType) {
  case ResourceType::Item:
    result = state.items[req.resource] >= req.amount;
    break;
  case ResourceType::Event:
    result = state.events[req.resource] != 0;
    break;
  case ResourceType::Trick:
    result = ctx.trickLevels[req.resource] >= req.amount;
    break;
  case ResourceType::Misc:
    result = ctx.misc[req.resource] >= req.amount;
    break;
  case ResourceType::Damage:
    // Negated damage requirements don't occur in practice.
    return IsDamageSatisfied(req, ctx, state);
  }
  return req.negate ? !result : result;
}

} // namespace metaforce::randomizer
