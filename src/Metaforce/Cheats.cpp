#include "Metaforce/Cheats.hpp"

#include "Kyoto/Math/CVector3f.hpp"
#include "MetroidPrime/CControlMapper.hpp"
#include "MetroidPrime/CHealthInfo.hpp"
#include "MetroidPrime/CStateManager.hpp"
#include "MetroidPrime/Player/CPlayer.hpp"
#include "MetroidPrime/Player/CPlayerState.hpp"
#include "MetroidPrime/TCastTo.hpp"
#include "MetroidPrime/Weapons/CWeapon.hpp"

#include <algorithm>

namespace metaforce::cheats {
namespace {

// Upward speed while moon jumping. A normal jump starts at about 10.
constexpr float kMoonJumpSpeed = 12.f;

Toggles sToggles;

void RefillAmmo(CPlayerState& state, CPlayerState::EItemType type) {
  const int capacity = state.GetItemCapacity(type);
  if (capacity > 0 && state.GetItemAmount(type) < capacity) {
    state.SetPickup(type, capacity);
  }
}

} // namespace

Toggles& GetToggles() { return sToggles; }

bool IgnorePlayerDamage() { return sToggles.infiniteHealth; }

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

void Update(CStateManager& mgr) {
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
}

} // namespace metaforce::cheats
