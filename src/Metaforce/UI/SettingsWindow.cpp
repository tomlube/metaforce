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

constexpr std::array kSuitNames = {
    "Power Suit",
    "Varia Suit",
    "Gravity Suit",
    "Phazon Suit",
};

constexpr std::array kBeamNames = {
    "Power Beam",
    "Wave Beam",
    "Ice Beam",
    "Plasma Beam",
};

constexpr std::array kUpgradeNames = {
    "Morph Ball",       "Morph Ball Bomb", "Boost Ball",  "Spider Ball",
    "Space Jump Boots", "Grapple Beam",    "Charge Beam", "Super Missile",
};

constexpr std::array kLogbookEntries = {
    "Chozo Artifacts",  "Parasite Queen", "Space Pirate Frigate",
    "Tallon Overworld", "Chozo Ruins",    "Magmoor Caverns",
    "Phendrana Drifts", "Phazon Mines",   "Impact Crater",
    "Flaahgra",         "Thardus",        "Omega Pirate",
    "Meta Ridley",      "Metroid Prime",
};

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
      "replace them whenever gameplay starts, so every file plays the same way.<br/><br/>You can "
      "still change options from the pause menu; they reset to these the next time you load.",
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

  leftPane.add_section("Cutscenes");
  config_choice_select(
      leftPane, rightPane, GetSettings().game.cutsceneSkips, "Cutscene Skips",
      kCutsceneSkipNames,
      "<br/>When Start skips a cutscene.<br/><br/><b>Watched Only</b>: the original behavior. "
      "Cutscenes the game lets you skip can be skipped once you've watched them; the save "
      "remembers which.<br/><br/><b>All Skippable</b>: those cutscenes can be skipped the first "
      "time too.<br/><br/><b>Any Cutscene</b>: Start also skips cutscenes the game never lets you "
      "skip. The screen fades out while the cutscene plays at high speed in the background, so "
      "the room ends up exactly as if you had watched it.");

  leftPane.add_section("Suit");
  add_setting< BoolButton >(
      leftPane, rightPane,
      bind(GetSettings().game.fusionSuit,
           {
               .key = "Fusion Suit",
               .setValue = [](bool value) { fusion::SetSuitEnabled(value); },
           }),
      "Wear the Fusion Suit from Metroid Fusion, the bonus the original unlocked by linking a Game "
      "Boy Advance. It changes Samus's look only.<br/><br/>This applies to every file, starting "
      "the next time you start or load one. The Fusion Bonus screen on the title menu changes "
      "this setting too.");
}

void add_input_tab(Window& window, Pane& left, Pane& right) {
  left.register_control(left.add_child< DeviceDropdown >(), right, [](Pane& pane) {
    pane.add_text("Change the input device used for gameplay.");
  });

  left.register_control(left.add_button({.text = "Configure Controls"}).on_pressed([&window] {
    window.push(std::make_unique< ControllerConfigWindow >());
  }), right, [](Pane& pane) {
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
      "a separate button to analog L only.");
  add_setting< BoolButton >(
      left, right, bind(input.modernControls, {.key = "Modern Controls"}),
      "Dual-stick movement: the left stick moves and strafes, and the C-stick turns. Forward, "
      "strafe and turning speeds stay at their original values.<br/><br/>Beams move from the "
      "C-stick to Z + D-pad. The D-pad alone still selects visors, and tapping Z opens the map "
      "when you let go.<br/><br/>Locking on works as it always has.");
  add_setting< BoolButton >(
      left, right,
      bind(input.squareDiagonalLook, {.key = "Full-Speed Diagonal Look", .isDisabled = modernOff}),
      "The original free look moves slower on a diagonal, because each direction only gets its "
      "share of the push. On, a full diagonal on the C-stick turns and looks up or down at full "
      "speed at once.");
  add_setting< BoolButton >(
      left, right, bind(input.mouseLook, {.key = "Mouse Look", .isDisabled = modernOff}),
      "The mouse turns Samus and aims her view up and down. Requires Modern Controls.<br/><br/>"
      "The cursor is captured during gameplay and released in menus. Bind fire, missiles and "
      "lock-on to mouse buttons under Configure Controls.");
  add_setting< NumberButton >(left, right,
                              bind(input.mouseSensitivity, {
                                                               .key = "Mouse Sensitivity",
                                                               .isDisabled = mouseLookOff,
                                                               .step = 5,
                                                               .suffix = "%",
                                                           }),
                              "100% turns 0.044 degrees per mouse count, the same as sensitivity "
                              "2 in Source and Quake games. Scale it to match the sensitivity you "
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
      left, right, bind(input.aimAssist, {.key = "Aim Assist"}),
      "Bends shots toward the targeted enemy, as the original game does.<br/><br/>Turning it off "
      "sends shots exactly where you aim when not locked on. Locked-on shots always lead their "
      "target.");
}

void add_demo_tab(Window& window, Pane& leftPane, Pane& rightPane) {
  auto& demo = GetSettings().demo;

  leftPane.add_section("Toggles");
  add_setting< BoolButton >(leftPane, rightPane, bind(demo.scanVisor, {.key = "Scan Visor"}),
                            "A BoolButton. Confirm, Left and Right flip the value."
                            "<br/><br/>A dot marks values that differ from the default.");
  add_setting< BoolButton >(
      leftPane, rightPane,
      bind(demo.hintSystem,
           {.key = "Hint System", .isDisabled = [&demo] { return !demo.scanVisor.get(); }}),
      "Disabled while the Scan Visor is off, to show a control that tracks another value.");
  add_setting< BoolButton >(leftPane, rightPane,
                            bind(demo.hardMode, {.key = "Hard Mode", .icon = "warning"}),
                            "A BoolButton with an icon.<br/><br/><icon class=\"warning\"/> "
                            "Icons draw from Material Symbols.");

  leftPane.add_section("Values");
  add_setting< NumberButton >(leftPane, rightPane,
                              bind(demo.energyTanks, {.key = "Energy Tanks", .step = 1}),
                              "A NumberButton. Left and Right step through the range; Confirm "
                              "types a value.");
  add_setting< NumberButton >(
      leftPane, rightPane,
      bind(demo.visorOpacity, {.key = "Visor Opacity", .step = 5, .suffix = "%"}),
      "A NumberButton with a suffix and a larger step.");
  add_setting< StringButton >(leftPane, rightPane,
                              bind(demo.saveName, {.key = "Save Name", .maxLength = 16}),
                              "A StringButton. Confirm starts editing; Confirm or Escape stops.");
  add_setting< DropdownButton >(
      leftPane, rightPane,
      bind_dropdown(demo.suit,
                    {
                        .key = "Suit",
                        .options = {{kSuitNames[0]},
                                    {kSuitNames[1]},
                                    {kSuitNames[2]},
                                    {kSuitNames[3], false}},
                    }),
      "A DropdownButton. Its options open in a context menu; unavailable options stay visible "
      "but disabled.");
  config_choice_select(leftPane, rightPane, demo.beam, "Beam", kBeamNames,
                       "<br/>A SelectButton whose options fill the next pane.");

  leftPane.add_section("Groups");
  leftPane.register_control(
      leftPane.add_group_button({.text = "Upgrades"}), rightPane, [&demo](Pane& pane) {
        for (int i = 0; i < static_cast< int >(kUpgradeNames.size()); ++i) {
          const uint32_t bit = 1u << i;
          pane.add_button({
                              .text = kUpgradeNames[i],
                              .isSelected = [&demo, bit] { return (demo.upgrades & bit) != 0; },
                          })
              .on_pressed([&demo, bit] {
                const uint32_t upgrades = demo.upgrades.get() ^ bit;
                play_nav_sound((upgrades & bit) != 0 ? NavSound::ItemEnable
                                                     : NavSound::ItemDisable);
                demo.upgrades.set(upgrades);
              });
        }
      });
  leftPane.register_control(
      leftPane.add_group_button({.text = "Logbook"}), rightPane, [&demo](Pane& pane) {
        std::vector< List::Item > items;
        for (uint64_t i = 0; i < kLogbookEntries.size(); ++i) {
          items.push_back({.key = i, .label = kLogbookEntries[i]});
        }
        pane.add_child< List >(List::Props{
            .items = std::move(items),
            .onPressed =
                [&demo](uint64_t key) {
                  play_nav_sound(NavSound::ItemChange);
                  demo.logbookEntry.set(static_cast< int >(key));
                },
            .isSelected =
                [&demo](uint64_t key) { return demo.logbookEntry == static_cast< int >(key); },
        });
      });

  leftPane.add_section("Dialogs");
  leftPane.register_control(leftPane.add_button("Show Modal").on_pressed([&window] {
    const auto dismiss = [](Modal& modal) { modal.pop(); };
    window.push(std::make_unique< Modal >(Modal::Props{
        .title = "Save Station",
        .bodyRml = "Save your progress?<br/>"
                   "<modal-tip>Tip: Modals take a title, body, icon and actions.</modal-tip>",
        .actions =
            {
                ModalAction{
                    .label = "Cancel",
                    .onPressed =
                        [dismiss](Modal& modal) {
                          play_nav_sound(NavSound::WindowClose);
                          dismiss(modal);
                        },
                },
                ModalAction{
                    .label = "Save",
                    .onPressed =
                        [dismiss](Modal& modal) {
                          play_nav_sound(NavSound::Click);
                          dismiss(modal);
                        },
                },
            },
        .onDismiss = dismiss,
        .icon = "question-mark",
    }));
  }),
                            rightPane,
                            [](Pane& pane) { pane.add_text("Opens a Modal with two actions."); });
  leftPane.register_control(leftPane.add_button("Show Warning").on_pressed([&window] {
    const auto dismiss = [](Modal& modal) { modal.pop(); };
    window.push(std::make_unique< Modal >(Modal::Props{
        .title = "Phazon Detected",
        .bodyText = "Prolonged exposure to Phazon is hazardous.",
        .actions =
            {
                ModalAction{
                    .label = "Continue",
                    .onPressed =
                        [dismiss](Modal& modal) {
                          play_nav_sound(NavSound::Click);
                          dismiss(modal);
                        },
                },
                ModalAction{
                    .label = "Retreat",
                    .onPressed =
                        [dismiss](Modal& modal) {
                          play_nav_sound(NavSound::WindowClose);
                          dismiss(modal);
                        },
                },
            },
        .onDismiss = dismiss,
        .variant = "danger",
        .icon = "warning",
        .isVertical = true,
    }));
  }),
                            rightPane, [](Pane& pane) {
                              pane.add_text(
                                  "Opens a Modal with the danger variant and vertical actions.");
                            });
  auto& contextButton = leftPane.add_button("Show Context Menu");
  contextButton.on_pressed([&contextButton] {
    play_nav_sound(NavSound::Click);
    push_document(std::make_unique< ContextMenu >(
        contextButton.root(), std::vector< ContextMenu::Item >{
                                  {.text = "Scan",
                                   .icon = "search",
                                   .onPressed = [] { play_nav_sound(NavSound::Click); }},
                                  {.text = "Log Entry",
                                   .icon = "description",
                                   .onPressed = [] { play_nav_sound(NavSound::Click); }},
                                  {.text = "Unavailable", .icon = "info", .enabled = false},
                                  {
                                      .text = "Discard",
                                      .icon = "delete",
                                      .onPressed = [] { play_nav_sound(NavSound::Click); },
                                      .destructive = true,
                                      .separatorBefore = true,
                                  },
                              }));
  });
  leftPane.register_control(contextButton, rightPane, [](Pane& pane) {
    pane.add_text("Opens a ContextMenu anchored to the button.");
  });

  leftPane.add_section("Icons");
  auto& icons = leftPane.add_child< Row >(Row::Props{});
  for (const auto& [icon, label] : std::array< std::pair< const char*, const char* >, 5 >{{
           {"play_arrow", "Play"},
           {"pause", "Pause"},
           {"stop", "Stop"},
           {"refresh", "Refresh"},
           {"settings", "Settings"},
       }}) {
    auto& button = icons.add_child< IconButton >(IconButton::Props{.icon = icon, .label = label});
    button.on_pressed([] { play_nav_sound(NavSound::Click); });
    leftPane.register_control(button, rightPane, [](Pane& pane) {
      pane.add_text("A Row of IconButtons. Left and Right move between them; each has a tooltip.");
    });
  }
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

  add_tab("Game", [this](Rml::Element* content) {
    auto& leftPane = add_child< Pane >(content, Pane::Type::Controlled);
    auto& rightPane = add_child< Pane >(content, Pane::Type::Uncontrolled);
    add_game_tab(leftPane, rightPane);
  });

  add_tab("Interface", [this](Rml::Element* content) {
    auto& leftPane = add_child< Pane >(content, Pane::Type::Controlled);
    auto& rightPane = add_child< Pane >(content, Pane::Type::Uncontrolled);
    auto& ui = GetSettings().ui;

    leftPane.add_section("Metaforce");
    add_setting< NumberButton >(
        leftPane, rightPane, bind(ui.scale, {.key = "UI Scale", .step = 25, .suffix = "%"}),
        "Scales the Metaforce interface relative to the display's DPI scale. Has no effect on "
        "the game's HUD and menus.");
    add_setting< BoolButton >(
        leftPane, rightPane, bind(ui.sounds, {.key = "Interface Sounds"}),
        "Play the game's menu sounds while navigating the Metaforce interface.");
  });

  add_tab("Demo", [this](Rml::Element* content) {
    auto& leftPane = add_child< Pane >(content, Pane::Type::Controlled);
    auto& rightPane = add_child< Pane >(content, Pane::Type::Uncontrolled);
    add_demo_tab(*this, leftPane, rightPane);
  });
}

} // namespace metaforce::ui
