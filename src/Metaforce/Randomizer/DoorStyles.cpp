#include "Metaforce/Randomizer/DoorStyles.hpp"

#include "Metaforce/Randomizer/Hooks.hpp"

#include "Kyoto/Alloc/CMemory.hpp"
#include "Kyoto/CResFactory.hpp"

#include <SDL3/SDL_filesystem.h>
#include <borealis/log.hpp>
#include <zlib.h>

#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace metaforce::randomizer {
namespace {
constexpr borealis::Log Log{"randomizer"};

constexpr uint32_t kCMDL = 0x434D444C;
constexpr uint32_t kTXTR = 0x54585452;

// Custom asset ids, in the range randomprime uses for its own.
constexpr uint32_t kCustomTextureBase = 0xDEAF0000;
constexpr uint32_t kCustomModelBase = 0xDEAF0100;

// randomprime's textures, copied to res/randomizer/prime1/door_assets by the exporter: a set of
// six for each blast shield, plus a few for door colors.
enum TextureSet { kCharge, kFlamethrower, kIceSpreader, kBombs, kPowerBomb, kSuper, kWavebuster };
enum TextureKind { kAnimatedGlow, kGlowBorder, kGlowTrim, kHolorim, kMetalBody, kMetalTrim };
constexpr std::array kTextureSetNames = {"charge_beam", "flamethrower", "ice_spreader",
                                         "morph_ball_bombs", "power_bomb", "super_missile",
                                         "wavebuster"};
constexpr std::array kTextureKindNames = {"animated_glow", "glow_border", "glow_trim",
                                          "holorim",       "metal_body",  "metal_trim"};
constexpr std::array kExtraTextureNames = {"power_beam_holorim", "orange", "pink", "yellow",
                                           "testbnew"};
constexpr int kSetTextureCount = static_cast< int >(kTextureSetNames.size() * kTextureKindNames.size());

constexpr uint32_t SetTexture(TextureSet set, TextureKind kind) {
  return kCustomTextureBase + set * static_cast< uint32_t >(kTextureKindNames.size()) + kind;
}
constexpr uint32_t kPowerBeamHolorim = kCustomTextureBase + kSetTextureCount;
constexpr uint32_t kOrange = kPowerBeamHolorim + 1;
constexpr uint32_t kPink = kPowerBeamHolorim + 2;
constexpr uint32_t kYellow = kPowerBeamHolorim + 3;
constexpr uint32_t kTestbNew = kPowerBeamHolorim + 4;
constexpr uint32_t kTextureEnd =
    kPowerBeamHolorim + static_cast< uint32_t >(kExtraTextureNames.size());

// The game's textures and models that doors use.
constexpr uint32_t kTestb = 0x544A9892;
constexpr uint32_t kThermalSpot = 0xCFA9DFF3;
constexpr uint32_t kBlueShieldModel = 0x0734977A;
constexpr uint32_t kVerticalBlueShieldModel = 0x18D0AEE6;

// A custom model: a copy of one of the game's, with the first few textures of its first
// material set swapped out, as randomprime builds them.
struct ModelRecipe {
  uint32_t base;
  std::array< uint32_t, 5 > textures;
  int textureCount;
};

constexpr ModelRecipe Door(uint32_t holorim) { return {kBlueShieldModel, {holorim}, 1}; }
constexpr ModelRecipe VerticalDoor(uint32_t holorim) {
  return {kVerticalBlueShieldModel, {holorim}, 1};
}
// randomprime's blast shield textures go in the order of the game's missile blast shield model.
constexpr ModelRecipe Shield(TextureSet set) {
  return {kMissileBlastShieldModel,
          {SetTexture(set, kGlowBorder), SetTexture(set, kGlowTrim), SetTexture(set, kMetalBody),
           SetTexture(set, kAnimatedGlow), SetTexture(set, kMetalTrim)},
          5};
}

enum CustomModel {
  kPowerOnlyDoor,
  kPowerOnlyVerticalDoor,
  kPlasmaVerticalDoor,
  kPowerBombDoor,
  kPowerBombVerticalDoor,
  kBombDoor,
  kBombVerticalDoor,
  kMissileDoor,
  kMissileVerticalDoor,
  kChargeDoor,
  kChargeVerticalDoor,
  kSuperDoor,
  kSuperVerticalDoor,
  kDisabledDoor,
  kDisabledVerticalDoor,
  kWavebusterDoor,
  kWavebusterVerticalDoor,
  kIceSpreaderDoor,
  kIceSpreaderVerticalDoor,
  kFlamethrowerDoor,
  kFlamethrowerVerticalDoor,
  kPowerBombShield,
  kSuperShield,
  kWavebusterShield,
  kIceSpreaderShield,
  kFlamethrowerShield,
  kChargeShield,
  kBombShield,
  kCustomModelCount,
};

constexpr uint32_t kMissileHolorim = 0x459582C1;
constexpr uint32_t kDisabledHolorim = 0x717AABCE;
constexpr uint32_t kPlasmaHolorim = 0x61A6945B;

constexpr std::array< ModelRecipe, kCustomModelCount > kModelRecipes = {
    Door(kPowerBeamHolorim),
    VerticalDoor(kPowerBeamHolorim),
    VerticalDoor(kPlasmaHolorim),
    Door(SetTexture(kPowerBomb, kHolorim)),
    VerticalDoor(SetTexture(kPowerBomb, kHolorim)),
    Door(SetTexture(kBombs, kHolorim)),
    VerticalDoor(SetTexture(kBombs, kHolorim)),
    Door(kMissileHolorim),
    VerticalDoor(kMissileHolorim),
    Door(SetTexture(kCharge, kHolorim)),
    VerticalDoor(SetTexture(kCharge, kHolorim)),
    Door(SetTexture(kSuper, kHolorim)),
    VerticalDoor(SetTexture(kSuper, kHolorim)),
    Door(kDisabledHolorim),
    VerticalDoor(kDisabledHolorim),
    Door(SetTexture(kWavebuster, kHolorim)),
    VerticalDoor(SetTexture(kWavebuster, kHolorim)),
    Door(SetTexture(kIceSpreader, kHolorim)),
    VerticalDoor(SetTexture(kIceSpreader, kHolorim)),
    Door(SetTexture(kFlamethrower, kHolorim)),
    VerticalDoor(SetTexture(kFlamethrower, kHolorim)),
    Shield(kPowerBomb),
    Shield(kSuper),
    Shield(kWavebuster),
    Shield(kIceSpreader),
    Shield(kFlamethrower),
    Shield(kCharge),
    Shield(kBombs),
};

constexpr uint32_t Model(CustomModel model) { return kCustomModelBase + model; }

// From randomprime's door_meta.rs.
constexpr std::array< DoorColorStyle, static_cast< size_t >(DoorColor::Count) > kDoorColors = {{
    {kBlueShieldModel, kVerticalBlueShieldModel, kTestb, kTestb, 0x8A7F3683, MapDoorColor::Blue},
    {Model(kPowerOnlyDoor), Model(kPowerOnlyVerticalDoor), kTestb, kTestb, 0x1D588B22,
     MapDoorColor::Blue},
    {0x33188D1B, 0x095B0B93, kTestb, kTestb, 0xF68DF7F1, MapDoorColor::Wave},
    {0x59649E9D, 0xB7A8A4C9, kTestb, kTestb, 0xBE4CD99D, MapDoorColor::Ice},
    {0xBBBA1EC7, Model(kPlasmaVerticalDoor), kTestb, kTestb, 0xFC095F6C, MapDoorColor::Plasma},
    {Model(kPowerBombDoor), Model(kPowerBombVerticalDoor), kPink, kThermalSpot, kTestbNew,
     MapDoorColor::Shield},
    {Model(kBombDoor), Model(kBombVerticalDoor), kOrange, kThermalSpot, kTestbNew,
     MapDoorColor::Blue},
    {Model(kMissileDoor), Model(kMissileVerticalDoor), kTestb, kTestb, 0x8344BEC8,
     MapDoorColor::Shield},
    {Model(kChargeDoor), Model(kChargeVerticalDoor), kYellow, kThermalSpot, kTestbNew,
     MapDoorColor::Blue},
    {Model(kSuperDoor), Model(kSuperVerticalDoor), kTestb, kTestb, 0xD5C17775,
     MapDoorColor::Shield},
    {Model(kDisabledDoor), Model(kDisabledVerticalDoor), kTestb, kTestb, kDisabledHolorim,
     MapDoorColor::Shield},
    {Model(kWavebusterDoor), Model(kWavebusterVerticalDoor), kTestb, kTestb, 0xF68DF7F1,
     MapDoorColor::Wave},
    {Model(kIceSpreaderDoor), Model(kIceSpreaderVerticalDoor), kTestb, kTestb, 0xBE4CD99D,
     MapDoorColor::Ice},
    {Model(kFlamethrowerDoor), Model(kFlamethrowerVerticalDoor), kTestb, kTestb, 0xFC095F6C,
     MapDoorColor::Plasma},
}};

// The game's own scan for its missile blast shields. randomprime makes scans for the others,
// which need new SCAN and STRG assets and room in the save for their scan progress; those blast
// shields have no scan point for now.
constexpr uint32_t kMissileBlastShieldScan = 0x05F56F9D;

constexpr std::array< BlastShieldStyle, static_cast< size_t >(BlastShield::Count) > kBlastShields = {{
    {kMissileBlastShieldModel, DoorColor::Missile, true, kMissileBlastShieldScan},
    {Model(kPowerBombShield), DoorColor::PowerBomb, false, 0},
    {Model(kSuperShield), DoorColor::Super, true, 0},
    {Model(kWavebusterShield), DoorColor::Wavebuster, true, 0},
    {Model(kIceSpreaderShield), DoorColor::IceSpreader, true, 0},
    {Model(kFlamethrowerShield), DoorColor::Flamethrower, true, 0},
    {Model(kChargeShield), DoorColor::Charge, true, 0},
    {Model(kBombShield), DoorColor::Bomb, false, 0},
}};

std::string NormalizeName(std::string_view name) {
  std::string out;
  for (const char c : name) {
    if (c != ' ' && c != '-' && c != '_') {
      out += static_cast< char >(std::tolower(static_cast< unsigned char >(c)));
    }
  }
  return out;
}

// EVulnerability values.
constexpr uint32_t kNormal = 1;
constexpr uint32_t kDeflect = 2;
constexpr uint32_t kImmune = 3;

// randomprime's DamageVulnerability fields, in property order.
struct Vulnerability {
  // Power, Ice, Wave, Plasma, Bomb, Power Bomb, Missile, Boost Ball, Phazon, four enemy
  // weapons, two unknown weapons, then the deflection type.
  std::array< uint32_t, 16 > normal;
  std::array< uint32_t, 5 > charged; // Power, Ice, Wave, Plasma, then the deflection type
  std::array< uint32_t, 5 > combo;
};

enum { kPower, kIce, kWave, kPlasma, kBomb, kPowerBombs, kMissile, kBoostBall, kPhazon };

constexpr Vulnerability kBaseVulnerability = {
    {kDeflect, kDeflect, kDeflect, kDeflect, kImmune, kDeflect, kDeflect, kImmune, kImmune, kImmune,
     kImmune, kImmune, kImmune, kDeflect, kDeflect, kNormal},
    {kDeflect, kDeflect, kDeflect, kDeflect, kNormal},
    {kDeflect, kDeflect, kDeflect, kDeflect, kNormal},
};

Vulnerability DoorVulnerability(DoorColor color) {
  Vulnerability v = kBaseVulnerability;
  const auto beam = [&v](int beam) {
    v.normal[beam] = kNormal;
    v.charged[beam] = kNormal;
    v.combo[beam] = kNormal;
  };
  switch (color) {
  case DoorColor::Blue:
    v.normal = {kNormal, kNormal, kNormal, kNormal, kNormal, kNormal, kNormal, kDeflect,
                kNormal, kImmune, kImmune, kImmune, kImmune, kDeflect, kDeflect, kNormal};
    v.charged = {kNormal, kNormal, kNormal, kNormal, kNormal};
    v.combo = {kNormal, kNormal, kNormal, kNormal, kNormal};
    break;
  case DoorColor::Disabled:
    v.normal.fill(kImmune);
    v.normal[13] = kDeflect;
    v.normal[14] = kDeflect;
    v.normal[15] = kNormal;
    v.charged = {kImmune, kImmune, kImmune, kImmune, kImmune};
    v.combo = {kImmune, kImmune, kImmune, kImmune, kImmune};
    break;
  case DoorColor::PowerOnly:
    beam(kPower);
    break;
  case DoorColor::Wave:
    beam(kWave);
    break;
  case DoorColor::Ice:
    beam(kIce);
    break;
  case DoorColor::Plasma:
    beam(kPlasma);
    break;
  case DoorColor::PowerBomb:
    v.normal[kPowerBombs] = kNormal;
    break;
  case DoorColor::Bomb:
    v.normal[kBomb] = kNormal;
    break;
  case DoorColor::Missile:
    v.normal[kMissile] = kNormal;
    v.combo = {kNormal, kNormal, kNormal, kNormal, kNormal};
    break;
  case DoorColor::Charge:
    v.charged = {kNormal, kNormal, kNormal, kNormal, kNormal};
    break;
  case DoorColor::Super:
    v.combo[kPower] = kNormal;
    break;
  case DoorColor::Wavebuster:
    v.combo[kWave] = kNormal;
    break;
  case DoorColor::IceSpreader:
    v.combo[kIce] = kNormal;
    break;
  case DoorColor::Flamethrower:
    v.combo[kPlasma] = kNormal;
    break;
  case DoorColor::Count:
    break;
  }
  return v;
}

void PutBig(uint8_t*& out, uint32_t value) {
  out[0] = static_cast< uint8_t >(value >> 24);
  out[1] = static_cast< uint8_t >(value >> 16);
  out[2] = static_cast< uint8_t >(value >> 8);
  out[3] = static_cast< uint8_t >(value);
  out += 4;
}

uint32_t GetBig(const uint8_t* in) {
  return (static_cast< uint32_t >(in[0]) << 24) | (static_cast< uint32_t >(in[1]) << 16) |
         (static_cast< uint32_t >(in[2]) << 8) | in[3];
}

// Custom assets ------------------------------------------------------------------------------

std::filesystem::path AssetDirectory() {
  // Resolve like GetDatabase: relative to the app's resources directory.
  std::filesystem::path dir = std::filesystem::path("res") / "randomizer" / "prime1" / "door_assets";
  if (const char* resources = SDL_GetBasePath()) {
    dir = std::filesystem::path(resources) / dir;
  }
  return dir;
}

std::optional< std::vector< uint8_t > > ReadTexture(uint32_t id) {
  const uint32_t index = id - kCustomTextureBase;
  std::string name;
  if (index < static_cast< uint32_t >(kSetTextureCount)) {
    name = std::string(kTextureSetNames[index / kTextureKindNames.size()]) + "_" +
           kTextureKindNames[index % kTextureKindNames.size()];
  } else {
    name = kExtraTextureNames[index - kSetTextureCount];
  }
  const std::filesystem::path path = AssetDirectory() / (name + ".txtr");
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    Log.error("Missing door texture {}", path.string());
    return std::nullopt;
  }
  return std::vector< uint8_t >(std::istreambuf_iterator< char >(file), {});
}

// One of the game's resources, decompressed.
std::optional< std::vector< uint8_t > > ReadGameResource(FourCC type, uint32_t id) {
  CResLoader& loader = gpResourceFactory->GetResLoader();
  const SObjectTag tag(type, id);
  if (!loader.ResourceExists(tag)) {
    return std::nullopt;
  }
  const bool compressed =
      loader.GetResourceCompression(tag) == CResLoader::kCompressionType_Compressed;
  char* buffer = nullptr;
  int length = 0;
  loader.LoadMemResourceSync(tag, &buffer, &length);
  std::optional< std::vector< uint8_t > > result;
  const auto* bytes = reinterpret_cast< const uint8_t* >(buffer);
  if (!compressed) {
    result.emplace(bytes, bytes + length);
  } else if (length >= 4) {
    std::vector< uint8_t > out(GetBig(bytes));
    uLongf outLength = static_cast< uLongf >(out.size());
    if (uncompress(out.data(), &outLength, bytes + 4, static_cast< uLong >(length - 4)) == Z_OK &&
        outLength == out.size()) {
      result = std::move(out);
    }
  }
  CMemory::Free(buffer);
  return result;
}

// A copy of `recipe.base` with the first textures of its first material set replaced.
std::optional< std::vector< uint8_t > > BuildModel(const ModelRecipe& recipe) {
  std::optional< std::vector< uint8_t > > model = ReadGameResource(kCMDL, recipe.base);
  if (!model) {
    Log.error("Missing model {:08X} to build a door model from", recipe.base);
    return std::nullopt;
  }
  std::vector< uint8_t >& data = *model;
  // Header: magic, version, flags, bounds, section count, material set count, section sizes,
  // then padding to 32 bytes. The first section is the first material set: a texture count and
  // the texture ids.
  if (data.size() < 44 || GetBig(data.data()) != 0xDEADBABE) {
    return std::nullopt;
  }
  const uint32_t sectionCount = GetBig(data.data() + 36);
  const size_t materials = (44 + sectionCount * 4 + 31) & ~size_t(31);
  if (GetBig(data.data() + 40) == 0 || materials + 4 > data.size() ||
      GetBig(data.data() + materials) < static_cast< uint32_t >(recipe.textureCount) ||
      materials + 4 + recipe.textureCount * 4 > data.size()) {
    return std::nullopt;
  }
  uint8_t* textures = data.data() + materials + 4;
  for (int i = 0; i < recipe.textureCount; ++i) {
    PutBig(textures, recipe.textures[i]);
  }
  return model;
}

struct CustomAsset {
  bool built = false;
  std::optional< std::vector< uint8_t > > data;
};

std::unordered_map< uint32_t, CustomAsset >& CustomAssets() {
  static std::unordered_map< uint32_t, CustomAsset > assets;
  return assets;
}

} // namespace

bool ParseDoorColor(std::string_view name, DoorColor& out) {
  static const std::unordered_map< std::string, DoorColor > kNames = {
      {"blue", DoorColor::Blue},
      {"poweronly", DoorColor::PowerOnly},
      {"powerbeamonly", DoorColor::PowerOnly},
      {"purple", DoorColor::Wave},
      {"wave", DoorColor::Wave},
      {"wavebeam", DoorColor::Wave},
      {"white", DoorColor::Ice},
      {"ice", DoorColor::Ice},
      {"icebeam", DoorColor::Ice},
      {"red", DoorColor::Plasma},
      {"plasma", DoorColor::Plasma},
      {"plasmabeam", DoorColor::Plasma},
      {"powerbomb", DoorColor::PowerBomb},
      {"bomb", DoorColor::Bomb},
      {"bombs", DoorColor::Bomb},
      {"missile", DoorColor::Missile},
      {"missiles", DoorColor::Missile},
      {"charge", DoorColor::Charge},
      {"chargebeam", DoorColor::Charge},
      {"super", DoorColor::Super},
      {"supermissile", DoorColor::Super},
      {"supermissiles", DoorColor::Super},
      {"disabled", DoorColor::Disabled},
      {"wavebuster", DoorColor::Wavebuster},
      {"icespreader", DoorColor::IceSpreader},
      {"flamethrower", DoorColor::Flamethrower},
  };
  const auto it = kNames.find(NormalizeName(name));
  if (it == kNames.end()) {
    return false;
  }
  out = it->second;
  return true;
}

bool ParseBlastShield(std::string_view name, BlastShield& out) {
  static const std::unordered_map< std::string, BlastShield > kNames = {
      {"missile", BlastShield::Missile},
      {"missiles", BlastShield::Missile},
      {"powerbomb", BlastShield::PowerBomb},
      {"powerbombs", BlastShield::PowerBomb},
      {"super", BlastShield::Super},
      {"supermissile", BlastShield::Super},
      {"supermissiles", BlastShield::Super},
      {"wavebuster", BlastShield::Wavebuster},
      {"icespreader", BlastShield::IceSpreader},
      {"flamethrower", BlastShield::Flamethrower},
      {"charge", BlastShield::Charge},
      {"chargebeam", BlastShield::Charge},
      {"bomb", BlastShield::Bomb},
      {"bombs", BlastShield::Bomb},
      {"morphballbomb", BlastShield::Bomb},
      {"morphballbombs", BlastShield::Bomb},
  };
  const auto it = kNames.find(NormalizeName(name));
  if (it == kNames.end()) {
    return false;
  }
  out = it->second;
  return true;
}

const DoorColorStyle& GetDoorColorStyle(DoorColor color) {
  return kDoorColors[static_cast< size_t >(color)];
}

const BlastShieldStyle& GetBlastShieldStyle(BlastShield shield) {
  return kBlastShields[static_cast< size_t >(shield)];
}

void WriteDoorVulnerability(DoorColor color, uint8_t (&out)[kVulnerabilitySize]) {
  const Vulnerability v = DoorVulnerability(color);
  uint8_t* p = out;
  PutBig(p, 18); // 15 weapons, the deflection type and the charged and combo structs
  for (const uint32_t value : v.normal) {
    PutBig(p, value);
  }
  PutBig(p, 5);
  for (const uint32_t value : v.charged) {
    PutBig(p, value);
  }
  PutBig(p, 5);
  for (const uint32_t value : v.combo) {
    PutBig(p, value);
  }
}

unsigned int GetCustomAssetType(unsigned int id) {
  if (id >= kCustomTextureBase && id < kTextureEnd) {
    return kTXTR;
  }
  if (id >= kCustomModelBase && id < kCustomModelBase + kCustomModelCount) {
    return kCMDL;
  }
  return 0;
}

bool GetCustomAsset(unsigned int id, const unsigned char*& data, unsigned int& size) {
  const unsigned int type = GetCustomAssetType(id);
  if (type == 0) {
    return false;
  }
  CustomAsset& asset = CustomAssets()[id];
  if (!asset.built) {
    asset.built = true;
    asset.data = type == kTXTR ? ReadTexture(id) : BuildModel(kModelRecipes[id - kCustomModelBase]);
    if (!asset.data) {
      Log.error("Could not build door asset {:08X}", id);
    }
  }
  if (!asset.data) {
    return false;
  }
  data = asset.data->data();
  size = static_cast< unsigned int >(asset.data->size());
  return true;
}

} // namespace metaforce::randomizer
