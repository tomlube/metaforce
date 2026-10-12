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
inline constexpr uint32_t kSTRG = 0x53545247;

// The Phazon Beam's paused message box string table, built in PickupAssets.cpp.
inline constexpr uint32_t kPhazonBeamMessage = 0xDEAF0100;
// The Phazon Beam's scan, a copy of the Energy Tank's scan with its own text, and that text's
// string table. Both are built in PickupAssets.cpp.
inline constexpr uint32_t kSCAN = 0x5343414E;
inline constexpr uint32_t kPhazonBeamScan = 0xDEAF0101;
inline constexpr uint32_t kPhazonBeamScanText = 0xDEAF0102;

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

// The scan kPhazonBeamScan is built from: the Energy Tank's own scan, which the Phazon Beam's
// pickup has when it loads (see GetPickupScanOverride).
void SetPhazonBeamScanSource(unsigned int scan);

} // namespace metaforce::randomizer
