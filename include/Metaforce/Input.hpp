#pragma once

#include <dolphin/pad.h>

namespace metaforce::input {

// Smart lock-on: Metroid Prime soft-locks when analog L passes 5% (kC_OrbitFar) and locks onto a
// target on the digital click (kC_OrbitObject). On a GameCube trigger the analog always leads the
// click, so a full pull soft-locks first and then locks on if anything is targetable. A digital-only
// L sends both on the same frame, where lock-on with no target does nothing and soft lock never
// fires. When digital L goes down without analog L leading, this holds the click back one frame and
// drives analog L to full, reproducing the trigger's travel. Call on clamped PADRead output.
void ApplySmartLockOn(PADStatus* status);

// Modern controls: the left stick moves and strafes, the right stick turns. The C-stick beams move
// to Z + D-pad, and the D-pad alone still picks visors.
bool ModernControlsEnabled();

// Z opens the map. With modern controls, Z is also the beam modifier, so the map waits for Z to
// be released and opens only if no D-pad direction was pressed while it was held. Call once for
// every in-game input on port 0, before the pause and map checks. Returns whether to open the map.
bool FilterMapButton(bool zPressed, bool zHeld, bool dpadPressed, bool startPressed);

} // namespace metaforce::input
