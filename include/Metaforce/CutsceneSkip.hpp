#pragma once

// The Game tab's Cutscene Skips setting.
//
// Retail lets Start skip a cutscene only when the area wires a CinematicSkip special function to
// it and the player has watched it before (CSystemState's cinematic flags). This setting can treat
// every such cutscene as watched, and can fast-forward cutscenes that have no skip function at all:
// the screen fades out and the game runs many updates per frame until the cinematic cameras are
// done, so the area's script reaches its normal end state on its own.

class CStateManager;

namespace metaforce::cutscenes {

enum ESkipMode {
  kCS_Watched = 0, // retail
  kCS_Skippable,   // every cutscene with a skip function, watched or not
  kCS_Any,         // that, and Start fast-forwards cutscenes without one
};

// CScriptSpecialFunction::ShouldSkipCinematic: whether to skip even unwatched cutscenes.
bool TreatAllAsWatched();

// CMFGame, on Start during a cutscene the retail skip can't handle. Returns whether a
// fast-forward started.
bool TryStartFastForward(CStateManager& mgr);
// True from the fade out until the fade in ends.
bool IsFastForwarding();

// CMFGame, in place of CStateManager::Update while gameplay runs.
void Update(CStateManager& mgr, float dt);
// CMFGame::Draw: the brightness for the fade, or 1 when nothing is fading.
float Brightness();

// CMFGame's constructor and destructor: drops a fast-forward the last game left behind.
void Reset();

} // namespace metaforce::cutscenes
