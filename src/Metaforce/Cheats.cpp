#include "Metaforce/Cheats.hpp"

#include "Collision/CCollidableSphere.hpp"
#include "Collision/CMaterialFilter.hpp"
#include "Collision/CMaterialList.hpp"
#include "Collision/CRayCastResult.hpp"
#include "Kyoto/Audio/CSfxManager.hpp"
#include "Kyoto/CResFactory.hpp"
#include "Kyoto/Math/CAABox.hpp"
#include "Kyoto/Math/CSphere.hpp"
#include "Kyoto/Math/CTransform4f.hpp"
#include "Kyoto/Math/CVector3f.hpp"
#include "MetroidPrime/CActor.hpp"
#include "MetroidPrime/CAnimData.hpp"
#include "MetroidPrime/CActorParameters.hpp"
#include "MetroidPrime/CAnimPlaybackParms.hpp"
#include "MetroidPrime/CEntityInfo.hpp"
#include "MetroidPrime/CControlMapper.hpp"
#include "MetroidPrime/CDamageInfo.hpp"
#include "MetroidPrime/CGameCollision.hpp"
#include "MetroidPrime/CHealthInfo.hpp"
#include "MetroidPrime/CStateManager.hpp"
#include "MetroidPrime/Cameras/CBallCamera.hpp"
#include "MetroidPrime/Cameras/CCameraManager.hpp"
#include "MetroidPrime/Cameras/CFirstPersonCamera.hpp"
#include "MetroidPrime/Player/CPlayer.hpp"
#include "MetroidPrime/Player/CPlayerState.hpp"
#include "MetroidPrime/SFX/Misc.h"
#include "MetroidPrime/SFX/MiscSamus.h"
#include "MetroidPrime/ScriptObjects/CScriptEffect.hpp"
#include "MetroidPrime/TCastTo.hpp"
#include "MetroidPrime/Tweaks/CTweakPlayer.hpp"
#include "MetroidPrime/Weapons/CWeapon.hpp"
#include "MetroidPrime/Weapons/WeaponTypes.hpp"

#include <algorithm>
#include <cmath>

namespace metaforce::cheats {
namespace {

// Upward speed while moon jumping. A normal jump starts at about 10.
constexpr float kMoonJumpSpeed = 12.f;

Toggles sToggles;

// Screw Attack values from Echoes' TweakBall (G2ME01).
constexpr float kScrewSpeed = 20.f;       // horizontal speed
constexpr float kScrewApex = 5.f;         // relaunches peak this far above the space jump
constexpr float kScrewDropLimit = 4.5f;   // relaunching works within this of the space jump height
constexpr int kScrewMaxJumps = 5;
constexpr float kScrewDamage = 36.f;      // per frame at 60 fps
constexpr float kScrewRadius = 3.f;
constexpr float kScrewKnockback = 1.f;
constexpr float kScrewRecoilHorizontal = 12.f;
constexpr float kScrewRecoilVertical = 24.f;
constexpr float kScrewForwardDot = 0.95f; // Echoes' "moving forward" test for the trigger
constexpr float kScrewTriggerRise = 14.f; // the space jump's upward speed
constexpr int kScrewStallFrames = 4;      // frames of barely moving that count as a wall hit
constexpr float kScrewWallProbe = 2.f;    // how far ahead of Samus a wall hit looks for the wall
constexpr float kScrewHeadOnDot = 0.9f;   // Echoes' test for a wall hit square-on
constexpr float kScrewSpinPhase = 0.5f; // where in the curl a relaunch restarts it
constexpr float kScrewPulseSpinRate = 3.f;  // playback speed of the flip through an impulse
constexpr float kScrewIdleSpinRate = 0.75f; // playback speed of the tail between impulses
constexpr CAssetId kScrewGlowPart = 0x41A5854A; // the particle effect in the boots cutscene
// The particle events on the root bone of the cutscene's SpaceBoot_PickUP animation.
constexpr CAssetId kScrewBurstParts[] = {0x1E9EC128, 0x2FC54239, 0x13273A3D, 0x208E6EAE};
constexpr int kScrewMaxBursts = 24;
constexpr s16 kScrewBurnVolume = 80; // burn sounds play under the 127 max
constexpr float kScrewGlowPulseHz = 1.5f;      // sine pulses of the resting glow per second
constexpr float kScrewGlowLow = 0.1f;          // the pulse's dimmest and brightest, 0 to 1
constexpr float kScrewGlowHigh = 0.8f;
constexpr float kScrewEffectHeight = 0.8f;     // how far above Samus' origin the effects sit
constexpr float kScrewEffectScale = 1.1f;      // size of the glow and bursts against the cutscene's
constexpr float kScrewGlowBoostDecay = 2.f;     // per second; an impulse's flare lasts half a second
constexpr float kScrewSteerRadians = 3.f * 3.14159265f / 180.f; // per relaunch, at full stick

struct ScrewAttack {
  bool active = false;
  bool thirdPerson = false; // the ball camera is in use and must be handed back
  int presses = 0;        // jump presses since leaving the ground
  int jumps = 0;          // relaunches this attack
  int stalledFrames = 0;  // frames in a row the attack didn't get anywhere
  bool idleSpin = false;  // the impulse flip has finished and the tail loops slowly
  float spinPhase = 0.f;  // where in the curl the pose is, 0 to 1
  float glowBoost = 0.f;  // 1 on an impulse, fading to 0
  float glowPhase = 0.f;  // where in the resting pulse the glow is, 0 to 1
  bool hasGlow = false;   // the flag, not the id, says there is one: kInvalidUniqueId may not be set up yet
  TUniqueId glow = kInvalidUniqueId; // the glow effect object
  TUniqueId bursts[kScrewMaxBursts]; // the live burst effects
  int burstCount = 0;
  CVector3f spaceJumpPos; // where the space jump press was; relaunch heights are measured from it
  CVector3f dir;          // horizontal direction, unit length
  CVector3f lastPos;
} sScrew;

void RefillAmmo(CPlayerState& state, CPlayerState::EItemType type) {
  const int capacity = state.GetItemCapacity(type);
  if (capacity > 0 && state.GetItemAmount(type) < capacity) {
    state.SetPickup(type, capacity);
  }
}

CVector3f Flat(const CVector3f& v) { return CVector3f(v.GetX(), v.GetY(), 0.f); }

void ResetScrewAttack() {
  sScrew.active = false;
  sScrew.thirdPerson = false;
  sScrew.presses = 0;
  sScrew.jumps = 0;
  sScrew.stalledFrames = 0;
  sScrew.idleSpin = false;
  sScrew.hasGlow = false;
  sScrew.burstCount = 0;
  sScrew.glowBoost = 0.f;
}

// Third person: the unmorphed Samus is drawn whenever the camera state isn't first person, so
// handing the view to the ball camera, as the morph does, is enough. The calls follow
// CPlayer::TransitionToMorphBallState and LeaveMorphBallState, minus the morph itself.
void EnterScrewCamera(CStateManager& mgr, CPlayer& player) {
  CCameraManager* cameras = mgr.CameraManager();
  CBallCamera* ballCamera = cameras->BallCamera();
  player.BreakOrbit(CPlayer::kOB_EnterMorphBall, mgr);
  player.HolsterGun(mgr);
  cameras->SetPlayerCamera(mgr, ballCamera->GetUniqueId());
  ballCamera->SetState(CBallCamera::kBCS_Default, mgr);
  player.SetCameraState(CPlayer::kCS_Ball, mgr);
  // Seats the camera where it settles, behind Samus, so it doesn't swing round from the
  // first-person position.
  ballCamera->Reset(cameras->GetFirstPersonCamera()->GetTransform(), mgr);
  cameras->ResetCameraHint(mgr);
  sScrew.thirdPerson = true;
}

void LeaveScrewCamera(CStateManager& mgr, CPlayer& player) {
  sScrew.thirdPerson = false;
  // A cutscene or the like may have taken the camera since.
  if (player.GetCameraState() != CPlayer::kCS_Ball) {
    return;
  }
  CCameraManager* cameras = mgr.CameraManager();
  cameras->SetPlayerCamera(mgr, cameras->FirstPersonCamera()->GetUniqueId());
  cameras->BallCamera()->SetState(CBallCamera::kBCS_Default, mgr);
  player.SetCameraState(CPlayer::kCS_FirstPerson, mgr);
  cameras->FirstPersonCamera()->DeferBallTransitionProcessing();
  cameras->FirstPersonCamera()->Think(0.f, mgr);
  player.ForceGunOrientation(player.GetTransform(), mgr);
  player.DrawGun(mgr);
}

// The airborne morph-in curl, played without the morph; nothing advances it unmorphed, so
// UpdateScrewAttack does, and it holds its last frame.
void StartScrewCurl(CPlayer& player) {
  if (!player.HasAnimation()) {
    return;
  }
  CAnimData& animData = *player.AnimationData();
  animData.SetAnimation(CAnimPlaybackParms(3, -1, 1.f, true), false);
  animData.EnableLooping(false);
  animData.SetIsAnimating(true);
  sScrew.idleSpin = false;
  sScrew.spinPhase = 0.f;
}

// Advances the pose: full speed through an impulse's flip, then the tail loops slowly so Samus
// never freezes.
void AdvanceScrewPose(CStateManager& mgr, CPlayer& player, float dt) {
  if (!player.HasAnimation()) {
    return;
  }
  CAnimData& animData = *player.AnimationData();
  const float rate = sScrew.idleSpin ? kScrewIdleSpinRate : kScrewPulseSpinRate;
  const float duration = animData.GetAnimationDuration(3);
  // The phase is tracked here rather than read back, as a finished animation reports nothing useful.
  if (duration > 0.f) {
    sScrew.spinPhase += dt * rate / duration;
    if (sScrew.spinPhase >= 1.f) {
      sScrew.spinPhase = kScrewSpinPhase;
      sScrew.idleSpin = true;
      animData.SetPhase(kScrewSpinPhase);
      animData.SetIsAnimating(true);
    }
  }
  // Root motion is ignored; the velocity moves her.
  player.UpdateAnimation(dt * rate, mgr, true);
}

// The attack's sounds share a handle so a new one cuts off the last.
void PlayScrewSfx(ushort id, s16 volume = 127) {
  static CSfxHandle sHandle;
  if (sHandle) {
    CSfxManager::SfxStop(sHandle);
  }
  sHandle = CSfxManager::SfxStart(id, volume, 64, true);
}

// The burn-death sound on an impulse; the small burn marks a cancelled attack.
void PlayScrewBurn(bool cancelled = false) {
  PlayScrewSfx(cancelled ? SFXeff_x_ash_01 : SFXeff_x_ash_00, kScrewBurnVolume);
}

// Each relaunch replays the tail of the curl, which reads as another flip.
void ReplayScrewSpin(CPlayer& player) {
  if (!player.HasAnimation()) {
    return;
  }
  CAnimData& animData = *player.AnimationData();
  animData.SetPhase(kScrewSpinPhase);
  animData.SetIsAnimating(true);
  sScrew.idleSpin = false;
  sScrew.spinPhase = kScrewSpinPhase;
}

// Adds a lit particle effect at Samus; false if its world isn't loaded.
bool SpawnScrewEffect(CStateManager& mgr, const CPlayer& player, CAssetId part, TUniqueId& outId) {
  if (gpResourceFactory->GetResourceTypeById(part) == 0) {
    return false;
  }
  const CEntityInfo info(player.GetAreaId(), CEntity::NullConnectionList, kInvalidEditorId);
  const CLightParameters lights(true, 1.f, CLightParameters::kST_Zero, 1.f, 20.f, CColor::White(),
                                true, CLightParameters::kLO_NormalWorld,
                                CLightParameters::kLR_EightFrames, CVector3f::Zero(), 4, 4, false,
                                0);
  CScriptEffect* effect = rs_new CScriptEffect(
      mgr.AllocateUniqueId(), rstl::string_l("Screw Attack Effect"), info,
      CTransform4f::Translate(player.GetTranslation()),
      CVector3f(kScrewEffectScale, kScrewEffectScale, kScrewEffectScale), part,
      kInvalidAssetId, false, true, true, true, false, 0.f, 0.f, 0.f, 0.f, false, 0.f, 0.f, 0.f,
      true, true, true, lights, true);
  mgr.AddObject(effect);
  outId = effect->GetUniqueId();
  return true;
}

// The glow from the Space Jump Boots pickup cutscene: its particle effect, kept on Samus.
void StartScrewGlow(CStateManager& mgr, const CPlayer& player) {
  sScrew.hasGlow = SpawnScrewEffect(mgr, player, kScrewGlowPart, sScrew.glow);
}

// The cutscene's animation also fires four particles from Samus' root bone; they burst on each
// impulse and ride along with her until they finish.
void SpawnScrewBurst(CStateManager& mgr, const CPlayer& player) {
  for (size_t i = 0; i < sizeof(kScrewBurstParts) / sizeof(kScrewBurstParts[0]); ++i) {
    if (sScrew.burstCount < kScrewMaxBursts &&
        SpawnScrewEffect(mgr, player, kScrewBurstParts[i], sScrew.bursts[sScrew.burstCount])) {
      ++sScrew.burstCount;
    }
  }
}

void StopScrewGlow(CStateManager& mgr) {
  // The effects remove themselves when their particles finish, so they may already be gone.
  if (sScrew.hasGlow && mgr.ObjectById(sScrew.glow) != nullptr) {
    mgr.DeleteObjectRequest(sScrew.glow);
  }
  sScrew.hasGlow = false;
  for (int i = 0; i < sScrew.burstCount; ++i) {
    if (mgr.ObjectById(sScrew.bursts[i]) != nullptr) {
      mgr.DeleteObjectRequest(sScrew.bursts[i]);
    }
  }
  sScrew.burstCount = 0;
}

// Keeps the glow on Samus, relit if its particles ran out, and brightest just after an impulse.
void UpdateScrewGlow(CStateManager& mgr, const CPlayer& player, const CVector3f& pos, float dt) {
  ScrewAttack& sa = sScrew;
  if (sa.hasGlow && mgr.ObjectById(sa.glow) == nullptr) {
    sa.hasGlow = false;
    StartScrewGlow(mgr, player);
  }
  sa.glowBoost = std::max(0.f, sa.glowBoost - dt * kScrewGlowBoostDecay);
  int kept = 0;
  for (int i = 0; i < sa.burstCount; ++i) {
    if (CActor* burst = TCastToPtr< CActor >(mgr.ObjectById(sa.bursts[i]))) {
      burst->SetTranslation(pos + CVector3f(0.f, 0.f, kScrewEffectHeight));
      sa.bursts[kept++] = sa.bursts[i];
    }
  }
  sa.burstCount = kept;
  if (!sa.hasGlow) {
    return;
  }
  if (CActor* glow = TCastToPtr< CActor >(mgr.ObjectById(sa.glow))) {
    glow->SetTranslation(pos + CVector3f(0.f, 0.f, kScrewEffectHeight));
    // The resting brightness swells and fades on a sine; an impulse flares it to full.
    sa.glowPhase = std::fmod(sa.glowPhase + dt * kScrewGlowPulseHz, 1.f);
    const float wave = 0.5f + 0.5f * std::sin(sa.glowPhase * 2.f * 3.14159265f);
    const float base = kScrewGlowLow + (kScrewGlowHigh - kScrewGlowLow) * wave;
    glow->SetModelFlags(CModelFlags::AlphaBlended(base + (1.f - base) * sa.glowBoost));
  }
}

void ScrewImpulse(CStateManager& mgr, CPlayer& player) {
  sScrew.glowBoost = 1.f;
  SpawnScrewBurst(mgr, player);
  ReplayScrewSpin(player);
  PlayScrewBurn();
}

// Ends the attack, handing the camera back and the pose to the idle one if they were taken.
void EndScrewAttack(CStateManager& mgr, CPlayer& player) {
  StopScrewGlow(mgr);
  if (sScrew.thirdPerson) {
    LeaveScrewCamera(mgr, player);
    if (player.HasAnimation()) {
      player.SetIntoBallReadyAnimation(mgr);
    }
  }
  ResetScrewAttack();
}

// Everything touching the sphere around Samus takes damage every frame.
void ApplyScrewDamage(CStateManager& mgr, CPlayer& player, float dt) {
  const float frames = 60.f * dt;
  const CDamageInfo info(CWeaponMode(kWT_Power), frames * kScrewDamage,
                         double(frames * kScrewDamage), 0.f, kScrewKnockback);
  const CVector3f center = player.GetTranslation() + CVector3f(0.f, 0.f, 1.3f);
  const CVector3f extent(kScrewRadius, kScrewRadius, kScrewRadius);
  const CMaterialFilter nearFilter = CMaterialFilter::MakeIncludeExclude(
      CMaterialList(kMT_Solid, kMT_Character, kMT_Target, kMT_NonSolidDamageable),
      CMaterialList(kMT_Player, kMT_Projectile));
  TEntityList nearList;
  mgr.BuildNearList(nearList, CAABox(center - extent, center + extent), nearFilter, &player);

  const CSphere sphere(center, kScrewRadius);
  const CMaterialFilter damageFilter =
      CMaterialFilter::MakeIncludeExclude(CMaterialList(kMT_Solid), CMaterialList());
  const TUniqueId playerId = player.GetUniqueId();
  for (int i = 0; i < nearList.size(); ++i) {
    const CActor* actor = TCastToConstPtr< CActor >(mgr.GetObjectById(nearList[i]));
    if (actor == nullptr) {
      continue;
    }
    const rstl::optional_object< CAABox > touchBounds = actor->GetTouchBounds();
    if (touchBounds && !CCollidableSphere::Sphere_AABox_Bool(sphere, *touchBounds)) {
      continue;
    }
    mgr.ApplyDamage(playerId, actor->GetUniqueId(), playerId, info, damageFilter,
                    CVector3f::Zero());
  }
}

// A wall hit square-on bounces Samus off it: she turns round and relaunches from there, with her
// jumps refilled, as a wall jump does in Echoes. Glancing hits and slopes don't, and the attack ends.
bool BounceOffWall(CStateManager& mgr, CPlayer& player, const CVector3f& pos) {
  ScrewAttack& sa = sScrew;
  const CVector3f center = pos + CVector3f(0.f, 0.f, 1.3f);
  const CRayCastResult wall = CGameCollision::RayStaticIntersection(
      mgr, center, sa.dir, kScrewWallProbe, CMaterialFilter::MakeInclude(CMaterialList(kMT_Solid)));
  if (!wall.IsValid()) {
    return false;
  }
  const CVector3f surface = wall.GetPlane().GetNormal();
  CVector3f normal = Flat(surface);
  if (std::fabs(surface.GetZ()) > 0.5f || !normal.CanBeNormalized()) {
    return false;
  }
  normal.Normalize();
  if (CVector3f::Dot(sa.dir, -normal) < kScrewHeadOnDot) {
    return false;
  }
  // The same launch speed as a relaunch made at the top of the jump.
  const float gravity = std::fabs(gpTweakPlayer->GetNormalGravAccel());
  const float rise = std::sqrt(2.f * gravity * kScrewApex);
  sa.dir = normal;
  sa.spaceJumpPos = pos;
  sa.jumps = 0;
  sa.stalledFrames = 0;
  player.SetVelocityWR(CVector3f(normal.GetX() * kScrewSpeed, normal.GetY() * kScrewSpeed, rise));
  CSfxManager::SfxStart(SFXsam_b_jump_00, 127, 64, true);
  ScrewImpulse(mgr, player);
  return true;
}

// Echoes' Screw Attack without its morph ball plumbing: Samus stays unmorphed and the attack is
// a velocity override on top of the normal jump. The trigger is the next jump press after the
// space jump, while moving forward. While falling, each press relaunches Samus to a fixed height
// over the space jump; pressing while rising, falling too far below it or hitting a wall ends it.
void UpdateScrewAttack(CStateManager& mgr, CPlayer& player, const CPlayerState& state, float dt) {
  ScrewAttack& sa = sScrew;
  if (player.GetMorphballTransitionState() != CPlayer::kMS_Unmorphed) {
    // Morphing sets up its own camera.
    StopScrewGlow(mgr);
    ResetScrewAttack();
    return;
  }
  if (player.CheckSubmerged()) {
    EndScrewAttack(mgr, player);
    return;
  }
  if (player.GetPlayerMovementState() == NPlayer::kMS_OnGround) {
    EndScrewAttack(mgr, player);
  }

  const bool pressed =
      ControlMapper::GetPressInput(ControlMapper::kC_JumpOrBoost, mgr.GetFinalInput());
  const int prior = sa.presses;
  if (pressed) {
    ++sa.presses;
  }
  const CVector3f pos = player.GetTranslation();
  const CVector3f velocity = player.GetVelocityWR();

  if (!sa.active) {
    if (!pressed) {
      return;
    }
    if (prior == 1) {
      sa.spaceJumpPos = pos;
    }
    const bool spaceJump = state.GetItemAmount(CPlayerState::kIT_SpaceJumpBoots) != 0;
    if (prior != (spaceJump ? 2 : 1)) {
      return;
    }
    CVector3f flat = Flat(velocity);
    CVector3f forward = Flat(player.GetTransform().GetForward());
    if (!flat.CanBeNormalized() || !forward.CanBeNormalized()) {
      return;
    }
    flat.Normalize();
    forward.Normalize();
    if (CVector3f::Dot(flat, forward) <= kScrewForwardDot) {
      return;
    }
    sa.active = true;
    sa.jumps = 0;
    sa.stalledFrames = 0;
    sa.dir = flat;
    sa.lastPos = pos;
    player.SetVelocityWR(
        CVector3f(flat.GetX() * kScrewSpeed, flat.GetY() * kScrewSpeed, kScrewTriggerRise));
    CSfxManager::SfxStart(SFXsam_b_jump_00, 127, 64, true);
    EnterScrewCamera(mgr, player);
    StartScrewCurl(player);
    sa.glowBoost = 1.f;
    StartScrewGlow(mgr, player);
    SpawnScrewBurst(mgr, player);
    PlayScrewBurn();
    return;
  }

  const float drop = pos.GetZ() - sa.spaceJumpPos.GetZ();
  bool finished = false;
  bool recoil = false;

  // The attack drives straight at a wall, so barely moving means one was hit.
  const float moved = Flat(pos - sa.lastPos).Magnitude();
  sa.lastPos = pos;
  sa.stalledFrames = moved < 0.25f * kScrewSpeed * dt ? sa.stalledFrames + 1 : 0;
  if (sa.stalledFrames >= kScrewStallFrames) {
    if (!BounceOffWall(mgr, player, pos)) {
      finished = true;
      recoil = true;
      PlayScrewSfx(SFXsam_b_highland_00);
    }
  } else if (drop < 0.f && -drop > kScrewDropLimit) {
    finished = true;
  } else if (pressed) {
    if (sa.jumps < kScrewMaxJumps && std::fabs(drop) < kScrewDropLimit && velocity.GetZ() < 0.f) {
      // The launch speed that peaks kScrewApex over the space jump.
      const float gravity = std::fabs(gpTweakPlayer->GetNormalGravAccel());
      const float rise = std::sqrt(std::max(0.f, 2.f * gravity * (kScrewApex - drop)));
      // The view is third person, so steering comes from the stick.
      const CFinalInput& input = mgr.GetFinalInput();
      const float steer = ControlMapper::GetAnalogInput(ControlMapper::kC_TurnRight, input) -
                          ControlMapper::GetAnalogInput(ControlMapper::kC_TurnLeft, input);
      const float angle = -steer * kScrewSteerRadians;
      sa.dir = CVector3f(sa.dir.GetX() * std::cos(angle) - sa.dir.GetY() * std::sin(angle),
                         sa.dir.GetX() * std::sin(angle) + sa.dir.GetY() * std::cos(angle), 0.f);
      ++sa.jumps;
      sa.stalledFrames = 0;
      ScrewImpulse(mgr, player);
      player.SetVelocityWR(
          CVector3f(sa.dir.GetX() * kScrewSpeed, sa.dir.GetY() * kScrewSpeed, rise));
      CSfxManager::SfxStart(SFXsam_b_jump_00, 127, 64, true);
    } else {
      finished = true;
      PlayScrewBurn(true);
    }
  }

  if (finished) {
    if (recoil) {
      player.SetVelocityWR(CVector3f(-sa.dir.GetX() * kScrewRecoilHorizontal,
                                     -sa.dir.GetY() * kScrewRecoilHorizontal,
                                     kScrewRecoilVertical));
    }
    // Further jump presses in this air time don't start another attack.
    const int presses = sa.presses;
    EndScrewAttack(mgr, player);
    sa.presses = presses;
    return;
  }

  // Air control would bleed off the horizontal speed.
  const CVector3f current = player.GetVelocityWR();
  player.SetVelocityWR(
      CVector3f(sa.dir.GetX() * kScrewSpeed, sa.dir.GetY() * kScrewSpeed, current.GetZ()));
  // Samus faces her heading; the look controls don't turn her while the ball camera has the view.
  player.SetTransform(CTransform4f::LookAt(pos, pos + sa.dir));
  AdvanceScrewPose(mgr, player, dt);
  UpdateScrewGlow(mgr, player, pos, dt);
  ApplyScrewDamage(mgr, player, dt);
}

} // namespace

Toggles& GetToggles() { return sToggles; }

bool IgnorePlayerDamage() { return sToggles.infiniteHealth || sScrew.active; }

bool InScrewAttack() { return sScrew.thirdPerson; }

float AdjustDamage(CStateManager& mgr, const CActor* damager, CActor& damagee, float damage) {
  const CPlayer* player = mgr.GetPlayer();
  if (!sToggles.oneHitKill || damager == nullptr || player == nullptr || &damagee == player) {
    return damage;
  }
  // Samus herself (like a Boost Ball hit) or one of her weapons: shots, bombs, Power Bombs.
  bool fromPlayer = damager == player;
  if (const CWeapon* weapon = TCastToConstPtr< CWeapon >(*damager)) {
    fromPlayer = weapon->GetOwnerId() == player->GetUniqueId();
  }
  const CHealthInfo* health = damagee.HealthInfo(mgr);
  if (!fromPlayer || health == nullptr) {
    return damage;
  }
  return std::max(damage, health->GetHP());
}

void Update(CStateManager& mgr, float dt) {
  CPlayerState& state = *mgr.PlayerState();
  if (sToggles.infiniteHealth) {
    // Damage is ignored, but energy can also drain other ways, like a Metroid latching on.
    CHealthInfo& health = *state.HealthInfo();
    health.SetHP(std::max(health.GetHP(), state.CalculateHealth()));
  }
  if (sToggles.infiniteMissiles) {
    RefillAmmo(state, CPlayerState::kIT_Missiles);
  }
  if (sToggles.infinitePowerBombs) {
    RefillAmmo(state, CPlayerState::kIT_PowerBombs);
  }

  CPlayer* player = mgr.Player();
  if (sToggles.moonJump && player != nullptr &&
      player->GetMorphballTransitionState() == CPlayer::kMS_Unmorphed &&
      player->GetPlayerMovementState() != NPlayer::kMS_OnGround &&
      ControlMapper::GetDigitalInput(ControlMapper::kC_JumpOrBoost, mgr.GetFinalInput())) {
    CVector3f velocity = player->GetVelocityWR();
    if (velocity.GetZ() < kMoonJumpSpeed) {
      velocity.SetZ(kMoonJumpSpeed);
      player->SetVelocityWR(velocity);
    }
  }

  if (sToggles.screwAttack && player != nullptr) {
    UpdateScrewAttack(mgr, *player, state, dt);
  } else if (player != nullptr) {
    EndScrewAttack(mgr, *player);
  } else {
    ResetScrewAttack();
  }
}

} // namespace metaforce::cheats
