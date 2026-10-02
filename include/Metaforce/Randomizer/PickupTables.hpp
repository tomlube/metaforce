#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace metaforce::randomizer {

// One of the game's 100 vanilla pickup locations, indexed by Randovania pickup index.
// Script object ids are editor ids without the layer bits.
struct PickupLocationInfo {
  uint32_t mlvl;
  uint32_t mrea;
  uint32_t pickupId;
  uint32_t hudMemoId;
  uint32_t audioId;
  uint32_t relayId;
  float position[3];
};

constexpr int kPickupLocationCount = 100;
extern const PickupLocationInfo kPickupLocations[kPickupLocationCount];

// A serialized Pickup script object (property count included) carrying an item's model,
// animation, actor parameters and collision.
struct PickupModelTemplate {
  const char* name;
  const unsigned char* data;
  size_t size;
};

extern const PickupModelTemplate kPickupModels[];
extern const int kPickupModelCount;

// Model-space bounds of a pickup CMDL (min xyz, max xyz) as float bit patterns.
struct PickupModelAabb {
  uint32_t cmdl;
  uint32_t bounds[6];
};

extern const PickupModelAabb kPickupModelAabbs[];
extern const int kPickupModelAabbCount;

struct ModelAlias {
  const char* alias;
  const char* name;
};

extern const ModelAlias kModelAliases[];
extern const int kModelAliasCount;

// Case-insensitive lookup by model name, as used in Randovania's pickup database.
const PickupModelTemplate* FindPickupModel(std::string_view name);

} // namespace metaforce::randomizer
