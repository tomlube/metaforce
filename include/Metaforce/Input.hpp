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

// Off, a diagonal on the look stick moves each axis at its share of the push, as the original
// game's free look does. On, a full diagonal moves both at full speed.
bool SquareDiagonalLook();

// The game's own input layer, under the menus: mouse look and the beam keys.
void InitializeGameInput();
void ShutdownGameInput();

// Number keys 1-4 select Power, Wave, Ice and Plasma. Bits follow the beam commands
// (kC_PowerBeam, kC_IceBeam, kC_WaveBeam, kC_PlasmaBeam), set while the key is held.
unsigned char BeamKeysHeld();

// Z, X, C and V select Combat, Scan, Thermal and X-Ray. Bits follow the visor commands
// (kC_XrayVisor, kC_ThermoVisor, kC_EnviroVisor, kC_NoVisor), set for keys pressed since the last
// call. Called once per game frame when input is generated.
unsigned char ConsumeVisorKeyPresses();

// Mouse look builds on modern controls: the mouse turns Samus and pitches her view directly.
bool MouseLookEnabled();
// Captures the mouse for the next game frame. Gameplay calls this every frame it wants to look,
// so menus, the map and anything else that stops gameplay input release the cursor on their own.
void RequestMouseCapture();
// Mouse motion since the last call, in counts (positive right and down). Called once per game
// frame when input is generated. Motion from the frame capture starts is dropped.
void ConsumeMouseDelta(float& x, float& y);
float MouseRadiansPerCount();
bool InvertMouseY();
// Holding R gives the left stick back its original free look movement. On, the mouse still
// turns Samus outright meanwhile; off, it turns as a stick would, capped at the original turn speed.
bool UncappedMouseTurnUnderR();

// Aim assist bends shots toward the targeted enemy when not locked on.
bool AimAssistEnabled();

} // namespace metaforce::input
