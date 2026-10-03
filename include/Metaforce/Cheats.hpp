#pragma once

// Cheat toggles from the Cheats window. They last until Metaforce exits and apply to whatever
// game is in progress.

class CActor;
class CStateManager;

namespace metaforce::cheats {

struct Toggles {
  bool infiniteHealth = false;
  bool infiniteMissiles = false;
  bool infinitePowerBombs = false;
  bool moonJump = false; // holding jump in the air keeps rising
  bool oneHitKill = false; // anything Samus can hurt dies to one hit
  bool unlockedDash = false; // the kiosk demo's dash: L orbiting an empty point can dash too
};

Toggles& GetToggles();

// CStateManager::Update, after actors think.
void Update(CStateManager& mgr);

// CStateManager::ApplyLocalDamage: whether damage to the player is ignored.
bool IgnorePlayerDamage();

// CStateManager::ApplyDamage and ApplyRadiusDamage, once the damagee was found vulnerable: the
// damage `damager` (null if unknown) actually deals to `damagee`.
float AdjustDamage(CStateManager& mgr, const CActor* damager, CActor& damagee, float damage);

} // namespace metaforce::cheats
