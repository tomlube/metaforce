#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>

namespace metaforce::ui {

// Set when any value changes, so settings.json is only rewritten after an edit.
inline bool gRuntimeConfigDirty = false;

// TODO: temp cvar system until I impl borealis::config
template < typename T >
class RuntimeVar {
public:
  explicit RuntimeVar(T defaultValue)
  : mValue(defaultValue), mDefaultValue(std::move(defaultValue)) {}

  const T& getValue() const { return mValue; }
  const T& getDefaultValue() const { return mDefaultValue; }
  void setValue(T value) {
    if (!(mValue == value)) {
      gRuntimeConfigDirty = true;
    }
    mValue = std::move(value);
  }
  operator const T&() const { return mValue; }

private:
  T mValue;
  T mDefaultValue;
};

struct RuntimeConfig {
  struct Video {
    RuntimeVar< bool > fullscreen{false};
    RuntimeVar< bool > lockAspectRatio{false};
  } video;

  struct Input {
    RuntimeVar< bool > allowBackgroundInput{false};
    RuntimeVar< bool > smartLockOn{true};
    RuntimeVar< bool > modernControls{false};
    RuntimeVar< bool > squareDiagonalLook{true};
    RuntimeVar< bool > mouseLook{false};
    // Percent of 0.044 degrees per mouse count (sensitivity 2 in Source and Quake games).
    RuntimeVar< int > mouseSensitivity{100};
    RuntimeVar< bool > invertMouseY{false};
    RuntimeVar< bool > uncappedMouseTurnUnderR{true};
    RuntimeVar< bool > aimAssist{true};
  } input;

  struct Game {
    // metaforce::cutscenes::ESkipMode
    RuntimeVar< int > cutsceneSkips{0};
    RuntimeVar< bool > fusionSuit{false};
  } game;

  struct Interface {
    RuntimeVar< int > scale{100};
    RuntimeVar< bool > sounds{true};
  } ui;

  // Values edited by the controls demo; nothing reads them.
  struct Demo {
    RuntimeVar< bool > scanVisor{true};
    RuntimeVar< bool > hintSystem{true};
    RuntimeVar< bool > hardMode{false};
    RuntimeVar< int > energyTanks{6};
    RuntimeVar< int > visorOpacity{100};
    RuntimeVar< std::string > saveName{"Samus"};
    RuntimeVar< int > suit{0};
    RuntimeVar< int > beam{0};
    RuntimeVar< uint32_t > upgrades{0b0011};
    RuntimeVar< int > logbookEntry{-1};
  } demo;
};

RuntimeConfig& GetRuntimeConfig();

// Reads settings.json from the user directory, keeping defaults for anything missing or invalid.
// Call before anything reads the config. The demo values aren't stored.
void LoadRuntimeConfig(const std::filesystem::path& userPath);
// Writes settings.json if anything changed since it was loaded or last saved.
void SaveRuntimeConfig();

} // namespace metaforce::ui
