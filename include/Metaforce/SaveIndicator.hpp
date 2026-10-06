#pragma once

// A "Saved" note in the bottom left corner after the game saves without the save screen, drawn in
// the game's own font and HUD message colors.

namespace metaforce::save_indicator {

// Shows the note for a few seconds.
void Show();

// CInGameGuiManager::Update: counts down while the note is up.
void Update(float dt);

// CInGameGuiManager::Draw, over gameplay.
void Draw();

} // namespace metaforce::save_indicator
