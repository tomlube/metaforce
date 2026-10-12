#include "Metaforce/UI/SettingsWindow.hpp"

#include "Metaforce/Display.hpp"
#include "Metaforce/FusionBonus.hpp"
#include "Metaforce/GameOptionDefaults.hpp"
#include "Metaforce/Settings.hpp"
#include "Metaforce/UI/ControllerConfigWindow.hpp"
#include "Metaforce/UI/DeviceDropdown.hpp"

#include <aurora/aurora.h>

#include <borealis/ui/bool_button.hpp>
#include <borealis/ui/config.hpp>
#include <borealis/ui/context_menu.hpp>
#include <borealis/ui/dropdown_button.hpp>
#include <borealis/ui/icon_button.hpp>
#include <borealis/ui/list.hpp>
#include <borealis/ui/modal.hpp>
#include <borealis/ui/number_button.hpp>
#include <borealis/ui/pane.hpp>
#include <borealis/ui/row.hpp>
#include <borealis/ui/string_button.hpp>
#include <borealis/ui/ui.hpp>

#include <dolphin/vi.h>

#include <algorithm>
#include <array>
#include <memory>
#include <utility>
#include <vector>

namespace metaforce::ui {
using namespace borealis::ui;
namespace {

// Indexed by metaforce::cutscenes::ESkipMode.
constexpr std::array kCutsceneSkipNames = {
    "Watched Only",
    "All Skippable",
    "Any Cutscene",
};

// Adds a control whose help text fills the next pane while it's focused.
template < typename Control, typename Props >
Control& add_setting(Pane& leftPane, Pane& rightPane, Props props, Rml::String helpText) {
  auto& control = leftPane.add_child< Control >(std::move(props));
  leftPane.register_control(control, rightPane, [helpText = std::move(helpText)](Pane& pane) {
    pane.clear();
    pane.add_rml(helpText);
  });
  return control;
}

template < size_t N >
void config_choice_select(Pane& leftPane, Pane& rightPane, Var< int >& var, Rml::String key,
                          const std::array< const char*, N >& names, Rml::String helpText) {
  leftPane.register_control(
      leftPane.add_select_button({
          .key = std::move(key),
          .getValue = [&var, &names] { return Rml::String{names[var.get()]}; },
          .isModified = [&var] { return var.modified(); },
      }),
      rightPane, [&var, &names, helpText = std::move(helpText)](Pane& pane) {
        for (int i = 0; i < static_cast< int >(names.size()); ++i) {
          pane.add_button({
                              .text = names[i],
                              .isSelected = [&var, i] { return var.get() == i; },
                          })
              .on_pressed([&var, i] {
                play_nav_sound(NavSound::ItemChange);
                var.set(i);
              });
        }
        pane.add_rml(helpText);
      });
}

void option_bool_select(Pane& leftPane, Pane& rightPane, bool options::GameOptionDefaults::*field,
                        Rml::String key, Rml::String helpText, bool isMaster = false) {
  auto& button = leftPane.add_child< BoolButton >(BoolButton::Props{
      .key = std::move(key),
      .getValue = [field] { return options::Get().*field; },
      .setValue =
          [field](bool value) {
            if (value == options::Get().*field) {
              return;
            }
            options::Get().*field = value;
            options::Commit();
          },
      .isDisabled = [isMaster] { return !isMaster && !options::Get().enabled; },
      .isModified =
          [field] { return options::Get().*field != options::GameOptionDefaults{}.*field; },
  });
  leftPane.register_control(button, rightPane, [helpText = std::move(helpText)](Pane& pane) {
    pane.clear();
    pane.add_rml(helpText);
  });
}

void option_percent_select(Pane& leftPane, Pane& rightPane,
                           int options::GameOptionDefaults::*field, Rml::String key,
                           Rml::String helpText) {
  auto& button = leftPane.add_child< NumberButton >(NumberButton::Props{
      .key = std::move(key),
      .getValue = [field] { return options::Get().*field; },
      .setValue =
          [field](int value) {
            options::Get().*field = std::clamp(value, 0, 100);
            options::Commit();
          },
      .isDisabled = [] { return !options::Get().enabled; },
      .isModified =
          [field] { return options::Get().*field != options::GameOptionDefaults{}.*field; },
      .min = 0,
      .max = 100,
      .step = 5,
      .suffix = "%",
  });
  leftPane.register_control(button, rightPane, [helpText = std::move(helpText)](Pane& pane) {
    pane.clear();
    pane.add_rml(helpText);
  });
}

void add_game_tab(Pane& leftPane, Pane& rightPane) {
  using Defaults = options::GameOptionDefaults;

  leftPane.add_section("Option Defaults");
  option_bool_select(
      leftPane, rightPane, &Defaults::enabled, "Apply Defaults",
      "Each save file stores its own Options menu settings. When this is on, the values below "
      "replace all of them, so every file plays the same way.<br/><br/>You can "
      "still change options from the pause menu but they reset to these the next time you load if enabled.",
      true);

  leftPane.add_section("Visor");
  option_percent_select(leftPane, rightPane, &Defaults::visorOpacity, "Visor Opacity",
                        "Opacity of the HUD elements drawn on the visor.");
  option_percent_select(leftPane, rightPane, &Defaults::helmetOpacity, "Helmet Opacity",
                        "Opacity of Samus's helmet frame.");
  option_bool_select(leftPane, rightPane, &Defaults::hudLag, "HUD Lag",
                     "Let the HUD sway behind camera movement.");
  option_bool_select(leftPane, rightPane, &Defaults::hintSystem, "Hint System",
                     "Show hint messages and map markers that point toward your next goal.");

  leftPane.add_section("Sound");
  option_percent_select(leftPane, rightPane, &Defaults::sfxVolume, "Sound Effects Volume",
                        "Volume of sound effects, the same as the Options menu's slider.");
  option_percent_select(leftPane, rightPane, &Defaults::musicVolume, "Music Volume",
                        "Volume of the music, the same as the Options menu's slider.");

  leftPane.add_section("Controller");
  option_bool_select(leftPane, rightPane, &Defaults::invertY, "Reverse Y-Axis",
                     "Invert vertical free-look and aiming.");
  option_bool_select(leftPane, rightPane, &Defaults::rumble, "Rumble",
                     "Enable controller rumble.");
  option_bool_select(leftPane, rightPane, &Defaults::swapBeamControls, "Swap Beam Controls",
                     "Swap the beam and visor controls: the C-Stick selects visors and the "
                     "D-Pad selects beams.");
}

void add_enhancements_tab(Pane& leftPane, Pane& rightPane) {
  config_choice_select(
      leftPane, rightPane, GetSettings().game.cutsceneSkips, "Cutscene Skips",
      kCutsceneSkipNames,
      "<br/>When Start skips a cutscene.<br/><br/><b>Watched Only</b>: the original behavior. "
      "Cutscenes the game lets you skip can be skipped once you've watched them; the save "
      "remembers which.<br/><br/><b>All Skippable</b>: normally skippable cutscenes can be skipped the first "
      "time too.<br/><br/><b>Any Cutscene</b>: Start also skips cutscenes the game never lets you "
      "skip.");

  add_setting< BoolButton >(
      leftPane, rightPane,
      bind(GetSettings().game.fusionSuit,
           {
               .key = "Fusion Suit",
               .setValue = [](bool value) { fusion::SetSuitEnabled(value); },
           }),
      "Wear the Fusion Suit from Metroid Fusion. This only changes the model of Samus.<br/><br/>This applies to every file, starting "
      "the next time you start or load one. The Fusion Bonus screen on the title menu changes "
      "this setting too.");
  add_setting< BoolButton >(
      leftPane, rightPane,
      bind(GetSettings().game.palPowerBombChanges, {.key = "PAL Power Bomb Logic"}),
      "Beam Troopers and Omega Pirate's armor plates accept Power Bomb damage, which they normally reflect on NTSC.<br/><br/>Only applies to entities that spawn after the change. As a result, this takes effect the next time a room loads.");

  auto& ui = GetSettings().ui;
  leftPane.add_section("Metaforce");
  add_setting< NumberButton >(
      leftPane, rightPane, bind(ui.scale, {.key = "UI Scale", .step = 20, .suffix = "%"}),
      "Scales the Metaforce interface relative to the display's DPI scale. Has no effect on "
      "the game's HUD and menus.");
  add_setting< BoolButton >(
      leftPane, rightPane, bind(ui.sounds, {.key = "Interface Sounds"}),
      "Play the game's menu sounds while navigating the Metaforce interface.");
}

void add_input_tab(Window& window, Pane& left, Pane& right) {
  left.register_control(left.add_child< DeviceDropdown >(), right, [](Pane& pane) {
    pane.add_text("Change the input device used for gameplay.");
  });

  left.register_control(left.add_button({.text = "Configure Controls"}).on_pressed([&window] {
    window.push(std::make_unique< ControllerConfigWindow >());
  }),
                        right, [](Pane& pane) {
                          pane.add_text("Customize bindings, deadzones and trigger thresholds.");
                        });

  auto& input = GetSettings().input;
  const auto modernOff = [] { return !GetSettings().input.modernControls.get(); };
  const auto mouseLookOff = [] {
    const auto& input = GetSettings().input;
    return !input.modernControls.get() || !input.mouseLook.get();
  };

  left.add_section("Options");
  add_setting< BoolButton >(left, right,
                            bind(input.allowBackgroundInput,
                                 {
                                     .key = "Allow Background Inputs",
                                     .setValue =
                                         [](bool value) {
                                           GetSettings().input.allowBackgroundInput.set(value);
                                           aurora_set_background_input(value);
                                         },
                                 }),
                            "Allow inputs even when the game window is not focused.");
  add_setting< BoolButton >(
      left, right, bind(input.smartLockOn, {.key = "Smart Lock-On"}),
      "Makes a digital L button act like a fully pulled GameCube trigger: it strafes and holds "
      "your view when nothing is targetable, and locks on when a target reticle is showing."
      "<br/><br/>Analog triggers are unaffected. To strafe past a target without locking on, bind "
      "a separate button to 'Analog L' only.");
  add_setting< BoolButton >(
      left, right, bind(input.modernControls, {.key = "Modern Controls"}),
      "Dual-stick movement: the left stick moves and strafes, and the C-stick turns. Forward, "
      "strafe and turning speeds stay at their original values.<br/><br/>Beams move from the "
      "C-stick to Z + D-pad. The D-pad alone still selects visors, and tapping Z opens the map "
      "when you let go.");
  add_setting< BoolButton >(
      left, right,
      bind(input.squareDiagonalLook, {.key = "Full-Speed Diagonal Look", .isDisabled = modernOff}),
      "The original free look moves slower on a diagonal, because each direction only gets its "
      "share of the push. While set to 'On' a full diagonal on the C-stick turns and looks up or "
      "down at full "
      "speed at once.");
  add_setting< BoolButton >(
      left, right, bind(input.mouseLook, {.key = "Mouse Look", .isDisabled = modernOff}),
      "The mouse turns Samus and aims her view up and down. Requires Modern Controls.<br/><br/>"
      "Cursor is captured during gameplay and released in menus. Bind fire, missiles and "
      "lock-on to mouse buttons under Configure Controls.");
  add_setting< NumberButton >(
      left, right,
      bind(input.mouseSensitivity,
           {
               .key = "Mouse Sensitivity",
               .isDisabled = mouseLookOff,
               .step = 5,
               .suffix = "%",
           }),
      "100% turns 0.044 degrees per mouse count, the same as sensitivity "
      "'2' in Source and Quake games. Scale it to match the sensitivity you "
      "use there.");
  add_setting< BoolButton >(
      left, right, bind(input.invertMouseY, {.key = "Invert Mouse Y", .isDisabled = mouseLookOff}),
      "Moving the mouse forward looks down.");
  add_setting< BoolButton >(
      left, right,
      bind(input.uncappedMouseTurnUnderR,
           {.key = "Uncapped Mouse Turn Under R", .isDisabled = mouseLookOff}),
      "Holding R (free look) gives movement back to the original game's free look physics: "
      "forward pushes, back brakes, no strafing, and Samus stops on the ground.<br/><br/>On, the "
      "mouse keeps turning Samus directly while R is held. Off, it turns like the original control "
      "stick, building up to the original turn speed and no faster.");
  add_setting< BoolButton >(
      left, right, bind(input.aimAssist, {.key = "Aim Assist", .isDisabled = mouseLookOff}),
      "Bends shots toward the targeted enemy, as the original game does.<br/><br/>Turning it off "
      "sends shots exactly where you aim when not locked on. Locked-on shots always lead their "
      "target.");
}

} // namespace

SettingsWindow::SettingsWindow() {
  add_tab("Video", [this](Rml::Element* content) {
    auto& leftPane = add_child< Pane >(content, Pane::Type::Controlled);
    auto& rightPane = add_child< Pane >(content, Pane::Type::Uncontrolled);
    auto& video = GetSettings().video;

    leftPane.add_section("Display");
    add_setting< BoolButton >(leftPane, rightPane, bind(video.fullscreen, {.key = "Fullscreen"}),
                              "Fill the display with the game.");
    leftPane.register_control(leftPane.add_button("Restore Default Window Size").on_pressed([] {
      play_nav_sound(NavSound::ItemChange);
      GetSettings().video.fullscreen.reset();
      VISetWindowFullscreen(false);
      VISetWindowSize(1280, 720);
      VICenterWindow();
    }),
                              rightPane, [](Pane& pane) { pane.clear(); });
    add_setting< BoolButton >(leftPane, rightPane,
                              bind(video.lockAspectRatio, {.key = "Lock 4:3 Aspect Ratio"}),
                              "Lock the game's aspect ratio to the original.");
  });

  add_tab("Input", [this](Rml::Element* content) {
    auto& leftPane = add_child< Pane >(content, Pane::Type::Controlled);
    auto& rightPane = add_child< Pane >(content, Pane::Type::Uncontrolled);
    add_input_tab(*this, leftPane, rightPane);
  });

  add_tab("Enhancements", [this](Rml::Element* content) {
    auto& leftPane = add_child< Pane >(content, Pane::Type::Controlled);
    auto& rightPane = add_child< Pane >(content, Pane::Type::Uncontrolled);
    add_enhancements_tab(leftPane, rightPane);
  });

  add_tab("Defaults", [this](Rml::Element* content) {
    auto& leftPane = add_child< Pane >(content, Pane::Type::Controlled);
    auto& rightPane = add_child< Pane >(content, Pane::Type::Uncontrolled);
    add_game_tab(leftPane, rightPane);
  });
}

} // namespace metaforce::ui
