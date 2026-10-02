#pragma once

#include <filesystem>

class CGameOptions;

namespace metaforce::options {

// Universal defaults for the in-game Options menu. Saves carry their own CGameOptions, so loading
// one replaces whatever the player set elsewhere. When enabled, these values are written over the
// save's options every time gameplay starts (new game, file load, warp).
struct GameOptionDefaults {
  bool enabled = false;
  int visorOpacity = 100; // percent
  int helmetOpacity = 100; // percent
  bool hudLag = true;
  bool hintSystem = true;
  bool invertY = false;
  bool rumble = true;
  bool swapBeamControls = false;
};

void Initialize(const std::filesystem::path& userPath);
GameOptionDefaults& Get();
// Persists the current defaults and, if enabled, applies them to the running game.
void Commit();
// Writes the defaults into the options if enabled. Does not call EnsureOptions.
void ApplyTo(CGameOptions& options);

} // namespace metaforce::options
