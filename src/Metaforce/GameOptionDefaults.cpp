#include "Metaforce/GameOptionDefaults.hpp"

#include "MetroidPrime/Player/CGameOptions.hpp"
#include "MetroidPrime/Player/CGameState.hpp"

#include <borealis/log.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>

namespace metaforce::options {
namespace {
namespace fs = std::filesystem;
using json = nlohmann::json;

constexpr borealis::Log Log{"options"};

GameOptionDefaults sDefaults;
fs::path sFile;

int PercentToAlpha(int percent) { return (std::clamp(percent, 0, 100) * 255 + 50) / 100; }
// The Options menu's volume sliders run from 0 to 127.
int PercentToVolume(int percent) { return (std::clamp(percent, 0, 100) * 127 + 50) / 100; }

void Load() {
  std::ifstream file(sFile, std::ios::binary);
  if (!file) {
    return;
  }
  const json root = json::parse(file, nullptr, false);
  if (!root.is_object()) {
    Log.warn("Ignoring malformed {}", sFile.string());
    return;
  }
  const GameOptionDefaults def;
  sDefaults.enabled = root.value("enabled", def.enabled);
  sDefaults.visorOpacity = std::clamp(root.value("visorOpacity", def.visorOpacity), 0, 100);
  sDefaults.helmetOpacity = std::clamp(root.value("helmetOpacity", def.helmetOpacity), 0, 100);
  sDefaults.hudLag = root.value("hudLag", def.hudLag);
  sDefaults.hintSystem = root.value("hintSystem", def.hintSystem);
  sDefaults.invertY = root.value("invertY", def.invertY);
  sDefaults.rumble = root.value("rumble", def.rumble);
  sDefaults.swapBeamControls = root.value("swapBeamControls", def.swapBeamControls);
  sDefaults.sfxVolume = std::clamp(root.value("sfxVolume", def.sfxVolume), 0, 100);
  sDefaults.musicVolume = std::clamp(root.value("musicVolume", def.musicVolume), 0, 100);
}

void Save() {
  if (sFile.empty()) {
    return;
  }
  const json root{
      {"enabled", sDefaults.enabled},
      {"visorOpacity", sDefaults.visorOpacity},
      {"helmetOpacity", sDefaults.helmetOpacity},
      {"hudLag", sDefaults.hudLag},
      {"hintSystem", sDefaults.hintSystem},
      {"invertY", sDefaults.invertY},
      {"rumble", sDefaults.rumble},
      {"swapBeamControls", sDefaults.swapBeamControls},
      {"sfxVolume", sDefaults.sfxVolume},
      {"musicVolume", sDefaults.musicVolume},
  };
  std::error_code ec;
  fs::create_directories(sFile.parent_path(), ec);
  std::ofstream file(sFile, std::ios::binary | std::ios::trunc);
  file << root.dump(2);
  if (!file) {
    Log.warn("Could not save {}", sFile.string());
  }
}

} // namespace

void Initialize(const fs::path& userPath) {
  sFile = userPath / "game_options.json";
  Load();
}

GameOptionDefaults& Get() { return sDefaults; }

void Commit() {
  Save();
  if (sDefaults.enabled && gpGameState != nullptr) {
    ApplyTo(gpGameState->GameOptions());
  }
}

void ApplyTo(CGameOptions& options) {
  if (!sDefaults.enabled) {
    return;
  }
  options.SetHudAlpha(PercentToAlpha(sDefaults.visorOpacity));
  options.SetHelmetAlpha(PercentToAlpha(sDefaults.helmetOpacity));
  options.SetHUDLag(sDefaults.hudLag);
  options.SetIsHintSystemEnabled(sDefaults.hintSystem);
  options.SetInvertYAxis(sDefaults.invertY);
  options.SetIsRumbleEnabled(sDefaults.rumble);
  options.ToggleControls(sDefaults.swapBeamControls);
  options.SetSfxVolume(PercentToVolume(sDefaults.sfxVolume), true);
  options.SetMusicVolume(PercentToVolume(sDefaults.musicVolume), true);
}

} // namespace metaforce::options
