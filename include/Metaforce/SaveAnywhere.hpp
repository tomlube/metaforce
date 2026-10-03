#pragma once

// Saving from any room through the game's own save screen. Loading such a save puts the player
// back where it was made: the position is kept in a file next to the saves, matched to the save
// by its exact play time (the game only counts play time while gameplay runs, so no other save
// can carry the same value).

#include <filesystem>
#include <string>

class CStateManager;

namespace metaforce::save_anywhere {

void Initialize(const std::filesystem::path& userPath);

// Why the game can't be saved right now, or empty when it can.
std::string WhyCantSave();

// Remembers where the player is and opens the save screen.
void RequestSave();

// CInGameGuiManager, back to gameplay: forgets a RequestSave whose save screen was left without
// saving.
void OnSaveScreenClosed();

// SGameFileSlot::InitializeFromGameState, around writing the game state into a save. Makes the save
// load into the room it was made in, and remembers the player's position for it.
void BeforeGameSaved();
void AfterGameSaved();

// End of CStateManager::InitializeState: puts the player back where a save made with RequestSave
// was made.
void OnWorldInitialized(CStateManager& mgr);

} // namespace metaforce::save_anywhere
