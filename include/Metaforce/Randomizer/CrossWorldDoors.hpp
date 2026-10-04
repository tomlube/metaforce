#pragma once

// Doors the room randomizer paired across worlds. Only one world is loaded at a time, so these
// doors never open onto the room behind them: the door stays shut, and walking up to it fades
// out, loads the other world and brings the player out of the door it's paired with.

#include "Kyoto/Math/CTransform4f.hpp"

#include <optional>

class CGameArea;
class CStateManager;

namespace metaforce::randomizer {

// End of CStateManager::InitializeState, on every world load. After a cross-world door brought
// the player here, places them just inside the door they came out of.
void ApplyCrossWorldArrival(CStateManager& mgr);

// Where the player stands coming out of dock `dock` of `area`: a few steps inside the doorway,
// on the floor there, facing into the room. Doors in floors and ceilings keep the facing `yaw`
// (radians about Z, 0 faces +Y). Nothing if the area has no such dock.
std::optional< CTransform4f > DockArrivalTransform(const CStateManager& mgr,
                                                   const CGameArea& area, int dock,
                                                   bool morphBall, float yaw);

} // namespace metaforce::randomizer
