#pragma once

#include <cstdint>

namespace metaforce::randomizer {

struct Seed;

// The door lock randomizer in the game: doors restyled as the seed says (shield color,
// vulnerability, map color) and the blast shields put in front of them. The hooks the game calls
// are in Hooks.hpp.
namespace door_locks {

// Seed activation: indexes the seed's doors by the script objects to restyle.
void Activate(const Seed& seed);
void Deactivate();

} // namespace door_locks

// Blast shields the player broke in the game being played, by room (MREA) and dock. Kept in the
// save slot's randomizer file, and only made permanent when the game is saved, like the game's
// own memory relays. Defined in Randomizer.cpp.
bool IsBlastShieldDestroyed(uint32_t area, int dock);
void MarkBlastShieldDestroyed(uint32_t area, int dock);

} // namespace metaforce::randomizer
