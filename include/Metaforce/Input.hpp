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

} // namespace metaforce::input
