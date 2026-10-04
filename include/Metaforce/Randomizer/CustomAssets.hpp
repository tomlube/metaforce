#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace metaforce::randomizer {

// Building the assets the randomizer adds to the game (see GetCustomAsset in Hooks.hpp).

using AssetData = std::optional< std::vector< uint8_t > >;

inline constexpr uint32_t kCMDL = 0x434D444C;
inline constexpr uint32_t kTXTR = 0x54585452;
inline constexpr uint32_t kANCS = 0x414E4353;

// One of the game's resources, decompressed.
AssetData ReadGameResource(uint32_t type, uint32_t id);

// A file from res/randomizer/prime1/<directory>.
AssetData ReadAssetFile(std::string_view directory, std::string_view name);

// Replaces the first textures of a CMDL's first material set. A texture id of 0 keeps that
// texture. False when the model has fewer textures or isn't a CMDL.
bool ReplaceModelTextures(std::vector< uint8_t >& cmdl, const uint32_t* textures, int count);

// randomprime's custom pickup models, the ids its pickup objects refer to (PickupTables.cpp).
// GetPickupAssetType is 0 for ids that aren't one.
unsigned int GetPickupAssetType(unsigned int id);
AssetData BuildPickupAsset(unsigned int id);

} // namespace metaforce::randomizer
