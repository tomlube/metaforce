// randomprime's custom pickup models, built like its custom_assets.rs builds them
// (https://github.com/randovania/randomprime, MIT License).

#include "Metaforce/Randomizer/CustomAssets.hpp"

#include <borealis/log.hpp>

#include <array>
#include <cstring>

namespace metaforce::randomizer {
namespace {
constexpr borealis::Log Log{"randomizer"};

// randomprime's custom_asset_ids, in its order.
enum : uint32_t {
  kPhazonSuitTxtr1 = 0xDEAF0000,
  kPhazonSuitTxtr2,
  kPhazonSuitCmdl,
  kPhazonSuitAncs,
  kNothingTxtr,
  kNothingCmdl,
  kNothingAncs,
  kZoomerCmdl,
  kZoomerAncs,
  kCogCmdl,
  kCogAncs,
  kThermalCmdl,
  kThermalAncs,
  kXRayCmdl,
  kXRayAncs,
  kCombatCmdl,
  kCombatAncs,
  // The shiny missile's assets (a CMDL, ANCS, EVNT, ANIM and three textures) aren't built, so
  // those pickups keep their own model.
  kShinyMissileTxtr0,
  kShinyMissileTxtr1,
  kShinyMissileTxtr2,
  kShinyMissileCmdl,
  kShinyMissileAncs,
  kShinyMissileEvnt,
  kShinyMissileAnim,
  kGamecubeCmdl,
  kGamecubeAncs,
  kGamecubeTxtr0,
  kGamecubeTxtr1,
  kFlamethrowerCmdl,
  kFlamethrowerAncs,
  kFlamethrowerTxtr1,
  kFlamethrowerTxtr2,
  kFlamethrowerTxtr3,
};
constexpr uint32_t kIceTrapAncs = 0xDEAF0072;

// The game's assets they're built from.
constexpr uint32_t kSuitCmdl = 0x95946E41;         // Node1_11.CMDL, the gravity suit pickup
constexpr uint32_t kSuitAncs = 0x27A97006;         // Node1_11.ANCS
constexpr uint32_t kVisorCmdl = 0x61DAB956;        // Node1_39_1.CMDL, the visor pickup
constexpr uint32_t kVisorAncs = 0x9F0C908A;        // Node1_39_1.ANCS
constexpr uint32_t kMetroidCmdl = 0x2F976E86;      // Metroid.CMDL
constexpr uint32_t kPlasmaComboCmdl = 0xC54BBF68;  // plasma_combo.CMDL
constexpr uint32_t kPowerComboAncs = 0x7C04E388;   // power_combo.ANCS
constexpr uint32_t kIceParasiteCmdl = 0xA3108E43;  // new_ice_parasite_bound.CMDL

constexpr const char* kAssetDirectory = "pickup_assets";

struct PickupAsset {
  uint32_t id;
  uint32_t type;
  const char* file; // in res/randomizer/prime1/pickup_assets, or nullptr for a copy of `base`
  uint32_t base;
  // CMDL: the first textures of its first material set to replace, 0 to keep one.
  std::array< uint32_t, 8 > textures;
  // ANCS: the character model to swap for `newModel`.
  uint32_t oldModel;
  uint32_t newModel;
};

constexpr PickupAsset Texture(uint32_t id, const char* file) {
  return {id, kTXTR, file, 0, {}, 0, 0};
}
constexpr PickupAsset Model(uint32_t id, uint32_t base, std::array< uint32_t, 8 > textures) {
  return {id, kCMDL, nullptr, base, textures, 0, 0};
}
constexpr PickupAsset ModelFile(uint32_t id, const char* file,
                                std::array< uint32_t, 8 > textures) {
  return {id, kCMDL, file, 0, textures, 0, 0};
}
constexpr PickupAsset Character(uint32_t id, uint32_t base, uint32_t oldModel,
                                uint32_t newModel) {
  return {id, kANCS, nullptr, base, {}, oldModel, newModel};
}

constexpr uint32_t N = kNothingTxtr;

constexpr std::array kPickupAssets = {
    Texture(kPhazonSuitTxtr1, "phazon_suit_texure_1.txtr"),
    Texture(kPhazonSuitTxtr2, "phazon_suit_texure_2.txtr"),
    Model(kPhazonSuitCmdl, kSuitCmdl, {kPhazonSuitTxtr1, 0, 0, kPhazonSuitTxtr2}),
    Character(kPhazonSuitAncs, kSuitAncs, kSuitCmdl, kPhazonSuitCmdl),
    Texture(kNothingTxtr, "nothing_texture.txtr"),
    Model(kNothingCmdl, kMetroidCmdl, {N, N, N, N, N, N, N, N}),
    Character(kNothingAncs, kSuitAncs, kSuitCmdl, kNothingCmdl),
    ModelFile(kZoomerCmdl, "zoomer.cmdl", {kNothingTxtr}),
    Character(kZoomerAncs, kSuitAncs, kSuitCmdl, kZoomerCmdl),
    ModelFile(kCogCmdl, "cog.cmdl", {}),
    Character(kCogAncs, kSuitAncs, kSuitCmdl, kCogCmdl),
    // The visors are the visor pickup with the wave, ice and power beam door colors.
    Model(kThermalCmdl, kVisorCmdl, {0xFC095F6C}),
    Character(kThermalAncs, kVisorAncs, kVisorCmdl, kThermalCmdl),
    Model(kXRayCmdl, kVisorCmdl, {0xBE4CD99D}),
    Character(kXRayAncs, kVisorAncs, kVisorCmdl, kXRayCmdl),
    Model(kCombatCmdl, kVisorCmdl, {0x1D588B22}),
    Character(kCombatAncs, kVisorAncs, kVisorCmdl, kCombatCmdl),
    ModelFile(kGamecubeCmdl, "randovania_gamecube.cmdl", {kGamecubeTxtr0, kGamecubeTxtr1}),
    Character(kGamecubeAncs, kSuitAncs, kSuitCmdl, kGamecubeCmdl),
    Texture(kGamecubeTxtr0, "randovania_gamecube.txtr"),
    Texture(kGamecubeTxtr1, "randovania_gamecube_text.txtr"),
    Model(kFlamethrowerCmdl, kPlasmaComboCmdl,
          {kFlamethrowerTxtr1, kFlamethrowerTxtr2, kFlamethrowerTxtr3}),
    // power_combo.ANCS's second character, which uses plasma_combo.CMDL.
    Character(kFlamethrowerAncs, kPowerComboAncs, kPlasmaComboCmdl, kFlamethrowerCmdl),
    Texture(kFlamethrowerTxtr1, "flamethrower_vertice_color.txtr"),
    Texture(kFlamethrowerTxtr2, "flamethrower_cap_glow.txtr"),
    Texture(kFlamethrowerTxtr3, "flamethrower_color_body.txtr"),
    Character(kIceTrapAncs, kSuitAncs, kSuitCmdl, kIceParasiteCmdl),
};

const PickupAsset* FindPickupAsset(uint32_t id) {
  for (const PickupAsset& asset : kPickupAssets) {
    if (asset.id == id) {
      return &asset;
    }
  }
  return nullptr;
}

// Points the ANCS's characters that use `oldModel` at `newModel`. randomprime sets the model of
// one character; the model id only appears in the ANCS where a character names it.
bool ReplaceCharacterModel(std::vector< uint8_t >& ancs, uint32_t oldModel, uint32_t newModel) {
  const uint8_t oldBytes[4] = {static_cast< uint8_t >(oldModel >> 24),
                               static_cast< uint8_t >(oldModel >> 16),
                               static_cast< uint8_t >(oldModel >> 8),
                               static_cast< uint8_t >(oldModel)};
  const uint8_t newBytes[4] = {static_cast< uint8_t >(newModel >> 24),
                               static_cast< uint8_t >(newModel >> 16),
                               static_cast< uint8_t >(newModel >> 8),
                               static_cast< uint8_t >(newModel)};
  bool replaced = false;
  for (size_t i = 0; i + 4 <= ancs.size(); ++i) {
    if (std::memcmp(ancs.data() + i, oldBytes, 4) == 0) {
      std::memcpy(ancs.data() + i, newBytes, 4);
      replaced = true;
      i += 3;
    }
  }
  return replaced;
}

} // namespace

unsigned int GetPickupAssetType(unsigned int id) {
  const PickupAsset* asset = FindPickupAsset(id);
  return asset != nullptr ? asset->type : 0;
}

AssetData BuildPickupAsset(unsigned int id) {
  const PickupAsset* asset = FindPickupAsset(id);
  if (asset == nullptr) {
    return std::nullopt;
  }
  AssetData data = asset->file != nullptr ? ReadAssetFile(kAssetDirectory, asset->file)
                                          : ReadGameResource(asset->type, asset->base);
  if (!data) {
    Log.error("Missing {:08X} to build pickup asset {:08X} from", asset->base, id);
    return std::nullopt;
  }
  if (asset->type == kCMDL) {
    int count = 0;
    for (int i = 0; i < static_cast< int >(asset->textures.size()); ++i) {
      if (asset->textures[i] != 0) {
        count = i + 1;
      }
    }
    if (!ReplaceModelTextures(*data, asset->textures.data(), count)) {
      Log.error("Could not retexture pickup model {:08X}", id);
      return std::nullopt;
    }
  } else if (asset->type == kANCS) {
    if (!ReplaceCharacterModel(*data, asset->oldModel, asset->newModel)) {
      Log.error("Character set {:08X} has no model {:08X}", asset->base, asset->oldModel);
      return std::nullopt;
    }
  }
  return data;
}

} // namespace metaforce::randomizer
