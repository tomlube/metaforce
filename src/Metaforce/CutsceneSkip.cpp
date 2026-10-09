#include "Metaforce/CutsceneSkip.hpp"

#include "Metaforce/Settings.hpp"

#include "Kyoto/Audio/CAudioSys.hpp"
#include "Kyoto/Audio/CMidiManager.hpp"
#include "MetroidPrime/CStateManager.hpp"
#include "MetroidPrime/Cameras/CCameraManager.hpp"
#include "MetroidPrime/Player/CGameOptions.hpp"
#include "MetroidPrime/Player/CGameState.hpp"
#include "MetroidPrime/Player/CPlayerState.hpp"

#include <algorithm>
#include <chrono>

namespace metaforce::cutscenes {
namespace {

using Clock = std::chrono::steady_clock;

// The retail skip fades for a second before it jumps; this fades out faster since the
// fast-forward itself takes a moment.
constexpr float kFadeOutTime = 1.0f;
constexpr float kFadeInTime = 0.5f;
// Real time spent on extra updates each frame, and a ceiling on the updates themselves.
constexpr auto kFrameBudget = std::chrono::milliseconds(12);
constexpr int kMaxStepsPerFrame = 120;
// Gives up on a cutscene whose cameras never finish, like one waiting on the player.
constexpr float kMaxGameTime = 180.f;
// Milliseconds for musyx to ramp the master volume.
constexpr unsigned short kVolumeRampMs = 500;

enum EPhase {
  kP_Idle,
  kP_FadeOut, // normal speed while the screen goes dark
  kP_Running, // many updates per frame
  kP_FadeIn,  // normal speed again, cutscene over
};

EPhase sPhase = kP_Idle;
float sTime = 0.f;     // seconds into the current fade
float sGameTime = 0.f; // seconds of game time fast-forwarded

int Mode() { return GetSettings().game.cutsceneSkips.get(); }

void RestoreVolume(unsigned short rampMs) {
  if (gpGameState != nullptr) {
    CAudioSys::SysSetSfxVolume(static_cast< unsigned char >(gpGameState->GameOptions().mSfxVol),
                               rampMs, true, true);
  }
}

void BeginFadeIn() {
  sPhase = kP_FadeIn;
  sTime = 0.f;
  RestoreVolume(kVolumeRampMs);
}

} // namespace

bool TreatAllAsWatched() { return Mode() >= kCS_Skippable; }

bool TryStartFastForward(CStateManager& mgr) {
  if (Mode() < kCS_Any || !mgr.GetCameraManager()->IsInCinematicCamera()) {
    return false;
  }
  if (sPhase == kP_FadeOut || sPhase == kP_Running) {
    return true;
  }
  // A cutscene that starts while the last one fades back in picks the fade up where it is.
  const float brightness = Brightness();
  sPhase = kP_FadeOut;
  sTime = (1.f - brightness) * kFadeOutTime;
  sGameTime = 0.f;
  CMidiManager::StopAll();
  // Music and sound effects; streamed audio keeps playing, so music a cutscene starts survives.
  CAudioSys::SysSetSfxVolume(0, kVolumeRampMs, true, true);
  return true;
}

bool IsFastForwarding() { return sPhase != kP_Idle; }

void Update(CStateManager& mgr, float dt) {
  switch (sPhase) {
  case kP_Idle:
    mgr.Update(dt);
    break;
  case kP_FadeOut:
    mgr.Update(dt);
    sTime += dt;
    if (!mgr.GetCameraManager()->IsInCinematicCamera()) {
      BeginFadeIn();
    } else if (sTime >= kFadeOutTime) {
      sPhase = kP_Running;
    }
    break;
  case kP_Running: {
    const Clock::time_point start = Clock::now();
    for (int i = 0; i < kMaxStepsPerFrame; ++i) {
      mgr.Update(dt);
      sGameTime += dt;
      if (!mgr.GetCameraManager()->IsInCinematicCamera() || !mgr.GetPlayerState()->IsAlive() ||
          sGameTime >= kMaxGameTime) {
        BeginFadeIn();
        break;
      }
      // A message screen, pause or world change wants the frame loop. A fast-forward that's
      // interrupted carries on when gameplay comes back.
      if (mgr.GetDeferredStateTransition() != kSMT_InGame || mgr.GetWantsToQuit() ||
          Clock::now() - start >= kFrameBudget) {
        break;
      }
    }
    break;
  }
  case kP_FadeIn:
    mgr.Update(dt);
    sTime += dt;
    if (sTime >= kFadeInTime) {
      sPhase = kP_Idle;
    }
    break;
  }
}

float Brightness() {
  switch (sPhase) {
  case kP_FadeOut:
    return std::clamp(1.f - sTime / kFadeOutTime, 0.f, 1.f);
  case kP_Running:
    return 0.f;
  case kP_FadeIn:
    return std::clamp(sTime / kFadeInTime, 0.f, 1.f);
  default:
    return 1.f;
  }
}

void Reset() {
  if (sPhase == kP_FadeOut || sPhase == kP_Running) {
    RestoreVolume(0);
  }
  sPhase = kP_Idle;
  sTime = 0.f;
  sGameTime = 0.f;
}

} // namespace metaforce::cutscenes
