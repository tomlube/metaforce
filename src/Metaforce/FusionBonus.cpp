#include "Metaforce/FusionBonus.hpp"

#include "Metaforce/Settings.hpp"

#include "MetroidPrime/Player/CGameState.hpp"
#include "MetroidPrime/Player/CSystemState.hpp"

namespace metaforce::fusion {

bool SuitEnabled() { return GetSettings().game.fusionSuit.get(); }

void SetSuitEnabled(bool enabled) {
  GetSettings().game.fusionSuit.set(enabled);
  if (gpGameState == nullptr) {
    return;
  }
  // CPlayerState picks this up when a file starts or loads. Switching it mid-game would leave the
  // already loaded suit models behind.
  gpGameState->SystemState().SetHasFusion(enabled);
}

void InitSystemState(CSystemState& state) {
  state.SetFusionLinked(true);
  state.SetHasFusion(SuitEnabled());
}

} // namespace metaforce::fusion
