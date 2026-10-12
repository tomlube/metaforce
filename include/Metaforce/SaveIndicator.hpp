#pragma once

// A "Saved" note in the bottom left corner after the game saves without the save screen, drawn in
// the game's own font and HUD message colors.

namespace metaforce::save_indicator {

// Shows the "Saved" note for a few seconds.
void Show();

// Shows a note with the given text for a few seconds, such as a room's name.
void ShowText(const wchar_t* text);

// CInGameGuiManager::Update: counts down while the note is up.
void Update(float dt);

// CInGameGuiManager::Draw, over gameplay.
void Draw();

} // namespace metaforce::save_indicator
