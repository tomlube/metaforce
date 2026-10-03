#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace metaforce::randomizer {

// randomprime's door colors (its DoorType) and blast shields (BlastShieldType): the models,
// textures and vulnerabilities the door lock randomizer gives doors. Colors and blast shields
// that aren't in the game use custom assets, see GetCustomAsset in Hooks.hpp.

enum class DoorColor : uint8_t {
  Blue,
  PowerOnly,
  Wave,
  Ice,
  Plasma,
  PowerBomb,
  Bomb,
  Missile,
  Charge,
  Super,
  Disabled,
  Wavebuster,
  IceSpreader,
  Flamethrower,
  Count,
};

enum class BlastShield : uint8_t {
  Missile,
  PowerBomb,
  Super,
  Wavebuster,
  IceSpreader,
  Flamethrower,
  Charge,
  Bomb,
  Count,
};

// How the map draws a door, as the color of one of the game's own door types.
enum class MapDoorColor : uint8_t {
  Blue,
  Shield, // gray, for locks the game doesn't have
  Ice,
  Wave,
  Plasma,
};

struct DoorColorStyle {
  uint32_t shieldModel;         // CMDL of the shield actor
  uint32_t verticalShieldModel; // the same, for doors in a floor or ceiling
  // Textures of the damageable trigger taking the door's shots.
  uint32_t pattern0;
  uint32_t pattern1;
  uint32_t color;
  MapDoorColor map;
};

struct BlastShieldStyle {
  uint32_t model;
  DoorColor counterpart; // the door color with the same vulnerability and map color
  bool lockOn;           // whether "Blast Shield Lock-On" makes it targetable
  uint32_t scan;         // SCAN of its scan point, 0 for none
};

// By Randovania's "shieldType" and "blastShieldType" names, ignoring case, spaces, '-' and '_'.
bool ParseDoorColor(std::string_view name, DoorColor& out);
bool ParseBlastShield(std::string_view name, BlastShield& out);

const DoorColorStyle& GetDoorColorStyle(DoorColor color);
const BlastShieldStyle& GetBlastShieldStyle(BlastShield shield);

// The door color's damage vulnerability, serialized as a DamageVulnerability script property
// (big endian, property count first) for CDamageVulnerability(CInputStream&).
inline constexpr size_t kVulnerabilitySize = 29 * 4;
void WriteDoorVulnerability(DoorColor color, uint8_t (&out)[kVulnerabilitySize]);

// The game's own missile blast shield model, which marks its blast shield actors.
inline constexpr uint32_t kMissileBlastShieldModel = 0xEFDFFB8C;

} // namespace metaforce::randomizer
