#pragma once

#include <borealis/config.hpp>

#include <cstdint>
#include <string>

namespace metaforce {

using borealis::config::Var;

// Persistent user settings. Keys and defaults live in Settings.cpp.
struct Settings {
  struct Video {
    Var< bool > fullscreen;
    Var< bool > lockAspectRatio;
  } video;

  struct Input {
    Var< bool > allowBackgroundInput;
    Var< bool > smartLockOn;
    Var< bool > modernControls;
    Var< bool > squareDiagonalLook;
    Var< bool > mouseLook;
    // Percent of 0.044 degrees per mouse count (sensitivity 2 in Source and Quake games).
    Var< int > mouseSensitivity;
    Var< bool > invertMouseY;
    Var< bool > uncappedMouseTurnUnderR;
    Var< bool > aimAssist;
  } input;

  struct Game {
    // metaforce::cutscenes::ESkipMode
    Var< int > cutsceneSkips;
    Var< bool > fusionSuit;
  } game;

  struct Interface {
    Var< int > scale;
    Var< bool > sounds;
  } ui;

  // Values edited by the controls demo; nothing reads them and they aren't saved.
  struct Demo {
    Var< bool > scanVisor;
    Var< bool > hintSystem;
    Var< bool > hardMode;
    Var< int > energyTanks;
    Var< int > visorOpacity;
    Var< std::string > saveName;
    Var< int > suit;
    Var< int > beam;
    Var< uint32_t > upgrades;
    Var< int > logbookEntry;
  } demo;
};

Settings& GetSettings();

} // namespace metaforce
