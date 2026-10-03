#pragma once

// The Metroid Fusion connection bonus and the Game tab's Fusion Suit setting.
//
// Retail unlocks the Fusion Suit by linking a GBA running Metroid Fusion (CSystemState's
// FusionLinked flag) and then requires a cleared save to pick it on the title screen's Fusion
// Bonus menu. The port has no GBA link, so every CSystemState starts linked. Which suit is worn
// comes from the Fusion Suit setting; the title screen toggle writes back to it.
//
// The NES Metroid bonus (FusionBeat) stays locked: its emulator is a GameCube REL module.

class CSystemState;

namespace metaforce::fusion {

bool SuitEnabled();
// Saves the setting. It takes effect the next time a file starts or loads.
void SetSuitEnabled(bool enabled);

// CSystemState's constructors: marks the GBA link done and applies the Fusion Suit setting.
void InitSystemState(CSystemState& state);

} // namespace metaforce::fusion
