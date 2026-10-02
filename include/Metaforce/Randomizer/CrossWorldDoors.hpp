#pragma once

// Doors the room randomizer paired across worlds. Only one world is loaded at a time, so these
// doors never open onto the room behind them: the door stays shut, and walking up to it fades
// out, loads the other world and brings the player out of the door it's paired with.

class CStateManager;

namespace metaforce::randomizer {

// End of CStateManager::InitializeState, on every world load. After a cross-world door brought
// the player here, places them just inside the door they came out of.
void ApplyCrossWorldArrival(CStateManager& mgr);

} // namespace metaforce::randomizer
