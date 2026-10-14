#include "Metaforce/UI/RandomizerWindow.hpp"

#include "Metaforce/Randomizer/Generator.hpp"
#include "Metaforce/Randomizer/RoomRando.hpp"
#include "Metaforce/Randomizer/ShareCode.hpp"

#include <SDL3/SDL_clipboard.h>

#include <borealis/ui/bool_button.hpp>

#include <borealis/ui/dropdown_button.hpp>
#include <borealis/ui/list.hpp>
#include <borealis/ui/menu_bar.hpp>
#include <borealis/ui/modal.hpp>
#include <borealis/ui/number_button.hpp>
#include <borealis/ui/pane.hpp>
#include <borealis/ui/string_button.hpp>
#include <borealis/ui/ui.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <array>

namespace metaforce::ui {
using namespace borealis::ui;
namespace rando = metaforce::randomizer;

namespace {

// Dropdown entries of a major item: "Shuffled" through "Shuffled ×N" (one entry per copy
// count), then "Starting Item" and "Removed".
constexpr int kMaxStandardCopies = 3;
constexpr int kStandardShuffled = 0; // + copies - 1
constexpr int kStandardStarting = kMaxStandardCopies;
constexpr int kStandardRemoved = kMaxStandardCopies + 1;

// Order of the room randomizer's region toggles. Regions not listed here go after these, in
// database order.
constexpr std::array kRegionOrder = {
    "Tallon Overworld", "Chozo Ruins",     "Magmoor Caverns", "Phendrana Drifts",
    "Phazon Mines",     "Frigate Orpheon", "Impact Crater",
};

void Save() { rando::SaveSettings(); }

// A list shown in a pane that other controls clear and rebuild. Forgets itself in its owner when
// it's destroyed, so the window never touches a list that's gone.
class TrackedList final : public List {
public:
  TrackedList(Rml::Element* parent, List::Props props, List*& owner)
  : List(parent, std::move(props)), mOwner(owner) {
    mOwner = this;
  }
  ~TrackedList() override {
    if (mOwner == this) {
      mOwner = nullptr;
    }
  }

private:
  List*& mOwner;
};

void SetHelp(Pane& leftPane, Pane& rightPane, Component& control, Rml::String help) {
  leftPane.register_control(control, rightPane, [help = std::move(help)](Pane& pane) {
    pane.clear();
    pane.add_rml(help);
  });
}

NumberButton& AddNumber(Pane& leftPane, Pane& rightPane, Rml::String key, Rml::String help,
                        std::function< int&() > value, int defaultValue, int min, int max,
                        int step = 1) {
  auto& button = leftPane.add_child< NumberButton >(NumberButton::Props{
      .key = std::move(key),
      .getValue = [value] { return value(); },
      .setValue =
          [value, min, max](int v) {
            value() = std::clamp(v, min, max);
            Save();
          },
      .isModified = [value, defaultValue] { return value() != defaultValue; },
      .min = min,
      .max = max,
      .step = step,
  });
  SetHelp(leftPane, rightPane, button, std::move(help));
  return button;
}

DropdownButton& AddDropdown(Pane& leftPane, Pane& rightPane, Rml::String key, Rml::String help,
                            std::vector< DropdownButton::Option > options,
                            std::function< int() > get, std::function< void(int) > set,
                            std::function< bool() > isModified,
                            std::function< bool() > isDisabled = {}) {
  auto& button = leftPane.add_child< DropdownButton >(DropdownButton::Props{
      .key = std::move(key),
      .options = std::move(options),
      .getValue = std::move(get),
      .setValue =
          [set = std::move(set)](int v) {
            set(v);
            Save();
          },
      .isDisabled = std::move(isDisabled),
      .isModified = std::move(isModified),
  });
  SetHelp(leftPane, rightPane, button, std::move(help));
  return button;
}

Rml::String EscapeRml(const std::string& str) { return borealis::ui::escape(str); }

Rml::String GenerationStatusRml() {
  const auto status = rando::GetGenerationStatus();
  switch (status.state) {
  case rando::GenerationState::Idle:
    return "Ready to generate.";
  case rando::GenerationState::Running:
    return fmt::format("Generating… {}% ({})", static_cast< int >(status.progress * 100.f),
                       EscapeRml(status.message));
  case rando::GenerationState::Succeeded:
    return fmt::format("<icon class=\"celebration\"/> Generated seed <b>{}</b>. New games will use it.",
                       EscapeRml(status.hash));
  case rando::GenerationState::Failed:
    return fmt::format("<icon class=\"warning\"/> {}", EscapeRml(status.message));
  }
  return {};
}

Rml::String ArmedRml() {
  const std::string& armed = rando::GetArmedSeed();
  Rml::String rml = armed.empty()
                        ? Rml::String("New games are <b>not</b> randomized.")
                        : fmt::format("New games use seed <b>{}</b>.", EscapeRml(armed));
  if (const rando::Seed* active = rando::GetActiveSeed()) {
    rml += fmt::format("<br/>Current game: seed <b>{}</b>, started at {}.", EscapeRml(active->hash),
                       active->randomStart ? Rml::String("a random location")
                                           : EscapeRml(active->startName));
  }
  return rml;
}

Rml::String ArmedButtonText() {
  return rando::GetArmedSeed().empty() ? "Randomizer Disabled" : "Randomizer Enabled";
}

Rml::String SeedLabel(const rando::SeedSummary& seed) {
  return fmt::format("{}  ·  {}", seed.hash, seed.randomStart ? "Random start" : seed.startName);
}

Rml::String BackupLabel(const rando::AutosaveBackup& backup) {
  const int seconds = static_cast< int >(backup.playTime);
  Rml::String label =
      fmt::format("{}:{:02}:{:02}", seconds / 3600, seconds / 60 % 60, seconds % 60);
  if (!backup.region.empty()) {
    label += fmt::format("  ·  {}", backup.region);
  }
  return label + fmt::format("  ·  {} Energy Tanks  ·  {}% items", backup.energyTanks,
                             backup.itemPercent);
}

Rml::String PoolRml() {
  const auto* pickups = rando::GetPickupDatabase();
  if (pickups == nullptr) {
    return {};
  }
  const int count = rando::CountPoolSize(*pickups, rando::GetSettings());
  constexpr int kLocations = 100;
  if (count > kLocations) {
    return fmt::format("<icon class=\"warning\"/> {} pickups for {} locations. {} Missile "
                       "Expansions will be removed.",
                       count, kLocations, count - kLocations);
  }
  if (count < kLocations) {
    return fmt::format("{} pickups for {} locations. {} Missile Expansions will fill the rest.",
                       count, kLocations, kLocations - count);
  }
  return fmt::format("{} pickups for {} locations.", count, kLocations);
}

void SetRml(Rml::Element* element, const Rml::String& rml) {
  if (element != nullptr) {
    element->SetInnerRML(rml);
  }
}

// Long codes have no spaces to wrap at.
Rml::String ShareCodeRml(const std::string& code) {
  return fmt::format("<span style=\"word-break: break-all;\"><b>{}</b></span>", EscapeRml(code));
}

// What a share code sets, shown before it's imported.
Rml::String ShareSummaryRml(const rando::Database& db, const rando::PickupDatabase& pickups,
                            const rando::Settings& settings) {
  Rml::String start = "Landing Site (Tallon Overworld)";
  if (settings.startingLocation == rando::kRandomStartingLocation) {
    start = "Random";
  } else if (!settings.startingLocation.empty()) {
    for (const auto& [key, name] : rando::StartingLocations(db)) {
      if (key == settings.startingLocation) {
        start = EscapeRml(name);
        break;
      }
    }
  }

  int changedPickups = 0;
  for (const auto& def : pickups.standard) {
    const auto& state = settings.standard.at(def.name);
    changedPickups += state.shuffled != def.defaultShuffled ||
                      state.starting != def.defaultStarting || state.ammo != def.defaultAmmo;
  }
  for (const auto& def : pickups.ammo) {
    const auto& state = settings.ammo.at(def.name);
    changedPickups += state.count != def.defaultCount || state.ammo != def.defaultAmmo;
  }

  Rml::String tricks = rando::kTrickLevelNames[settings.trickLevel];
  if (!settings.tricks.empty()) {
    const auto enabled = std::count_if(
        db.Tricks().begin(), db.Tricks().end(),
        [&settings](const rando::TrickResource& t) { return settings.TrickLevel(t.name) != 0; });
    tricks = fmt::format("Custom ({} of {} enabled)", enabled, db.Tricks().size());
  }

  Rml::String rml = fmt::format(
      "Seed string: {}<br/>"
      "Starting location: <b>{}</b><br/>"
      "Tricks: <b>{}</b>  ·  Damage: <b>{}</b><br/>"
      "Chozo Artifacts: <b>{}</b><br/>"
      "Room randomizer: <b>{}</b><br/>"
      "Door locks: <b>{}</b><br/>"
      "Main Plaza Vault Door: <b>{}</b><br/>"
      "Item pool: <b>{}</b>",
      settings.seedString.empty()
          ? Rml::String("<b>none</b> (every seed made from it is different)")
          : fmt::format("<b>{}</b>", EscapeRml(settings.seedString)),
      start, tricks,
      rando::kDamageStrictnessNames[settings.damageStrictness], settings.artifactTarget,
      rando::kRoomRandoNames[settings.roomRando], rando::kDoorLockModeNames[settings.doorLockMode],
      settings.mainPlazaDoor ? "On" : "Off", changedPickups == 0 ? Rml::String("Starter Preset")
                          : fmt::format("{} change{} from the Starter Preset", changedPickups,
                                        changedPickups == 1 ? "" : "s"));
  if (settings.logicDisabled) {
    rml += "<br/><icon class=\"warning\"/> <b>Fully Disable Logic</b> is on.";
  }
  return rml;
}

} // namespace

RandomizerWindow::RandomizerWindow() {
  if (rando::GetDatabase() == nullptr) {
    add_tab("Randomizer", [this](Rml::Element* content) {
      reset_tab_elements();
      auto& pane = add_child< Pane >(content, Pane::Type::Uncontrolled);
      pane.add_rml(fmt::format("<icon class=\"warning\"/> The randomizer data could not be "
                               "loaded.<br/><br/>{}",
                               EscapeRml(rando::LoadError())));
    });
    return;
  }

  add_tab("Seed", [this](Rml::Element* content) {
    reset_tab_elements();
    build_seeds_tab(content);
  });
  add_tab("QoL", [this](Rml::Element* content) {
    reset_tab_elements();
    build_qol_tab(content);
  });
  add_tab("Logic", [this](Rml::Element* content) {
    reset_tab_elements();
    build_logic_tab(content);
  });
  add_tab("Item Pool", [this](Rml::Element* content) {
    reset_tab_elements();
    build_pool_tab(content);
  });
  add_tab("Rooms", [this](Rml::Element* content) {
    reset_tab_elements();
    build_rooms_tab(content);
  });
  add_tab("Door Locks", [this](Rml::Element* content) {
    reset_tab_elements();
    build_doors_tab(content);
  });
  add_tab("Item Warp", [this](Rml::Element* content) {
    reset_tab_elements();
    build_item_warp_tab(content);
  });
  add_tab("Races", [this](Rml::Element* content) {
    reset_tab_elements();
    build_tournament_tab(content);
  });
}

void RandomizerWindow::reset_tab_elements() {
  mStatusText = nullptr;
  mShareText = nullptr;
  mArmedText = nullptr;
  mArmedButton = nullptr;
  mPoolText = nullptr;
  mSeedList = nullptr;
  mStartList = nullptr;
  mStartListDirty = false;
  mShownState = rando::GenerationState::Idle;
  mShownProgress = -1.f;
  mShownPoolSize = -1;
  mShownArmed.clear();
}

void RandomizerWindow::update() {
  const auto status = rando::GetGenerationStatus();
  if (mStatusText != nullptr &&
      (status.state != mShownState || status.progress != mShownProgress)) {
    if (status.state == rando::GenerationState::Succeeded &&
        mShownState == rando::GenerationState::Running) {
      rando::SetArmedSeed(status.hash);
      refresh_seeds();
    }
    mShownState = status.state;
    mShownProgress = status.progress;
    mStatusText->SetInnerRML(GenerationStatusRml());
  }
  if (mStartListDirty) {
    mStartListDirty = false;
    if (mStartList != nullptr) {
      mStartList->set_items(start_list_items());
    }
  }
  if (mArmedText != nullptr && mShownArmed != rando::GetArmedSeed()) {
    mShownArmed = rando::GetArmedSeed();
    mArmedText->SetInnerRML(ArmedRml());
    if (mArmedButton != nullptr) {
      mArmedButton->set_text(ArmedButtonText());
    }
  }
  if (mPoolText != nullptr) {
    if (const auto* pickups = rando::GetPickupDatabase()) {
      const int size = rando::CountPoolSize(*pickups, rando::GetSettings());
      if (size != mShownPoolSize) {
        mShownPoolSize = size;
        mPoolText->SetInnerRML(PoolRml());
      }
    }
  }
  Window::update();
}

void RandomizerWindow::build_seeds_tab(Rml::Element* content) {
  auto& leftPane = add_child< Pane >(content, Pane::Type::Controlled);
  auto& rightPane = add_child< Pane >(content, Pane::Type::Uncontrolled);
  auto& settings = rando::GetSettings();

  leftPane.add_section("RANDOMIZER STATUS");
  mArmedText = leftPane.add_rml(ArmedRml());
  mShownArmed = rando::GetArmedSeed();

  auto& disarm = leftPane.add_button(ArmedButtonText());
  disarm.on_pressed([] {
    play_nav_sound(NavSound::ItemDisable);
    rando::SetArmedSeed({});
  });
  mArmedButton = &disarm;
  SetHelp(leftPane, rightPane, disarm,
          "Shows whether new games are randomized. Press it to turn the randomizer off: new "
          "games start vanilla. Saves that were started with a seed keep using it.<br/><br/>"
          "Generating a seed or choosing one under <b>Generated Seeds</b> turns it back on.");

  leftPane.add_section("Generate");
  auto& seedString = leftPane.add_child< StringButton >(StringButton::Props{
      .key = "Seed String",
      .getValue = [&settings] { return settings.seedString; },
      .setValue =
          [this, &settings](Rml::String value) {
            if (rando::LooksLikeShareCode(value)) {
              play_nav_sound(NavSound::Warning);
              SetRml(mShareText, "<icon class=\"warning\"/> That's a share code, not a seed "
                                 "string. Copy the whole code and press <b>Paste Share "
                                 "Code</b> instead.");
              return;
            }
            settings.seedString = std::move(value);
            Save();
          },
      .isModified = [&settings] { return !settings.seedString.empty(); },
      .maxLength = 32,
  });
  SetHelp(leftPane, rightPane, seedString,
          "The value the generator is seeded with. The same seed string and settings always "
          "produce the same seed.<br/><br/>Leave it empty to use a new random value each time.");

  auto& randomize = leftPane.add_button("New Random Seed String");
  randomize.on_pressed([&settings] {
    play_nav_sound(NavSound::ItemChange);
    settings.seedString = rando::RandomSeedString();
    Save();
  });
  SetHelp(leftPane, rightPane, randomize, "Replaces the seed string with a random one.");

  auto& generate = leftPane.add_button("Generate Seed");
  generate.on_pressed([this] {
    if (rando::GetGenerationStatus().state == rando::GenerationState::Running) {
      play_nav_sound(NavSound::Warning);
      return;
    }
    play_nav_sound(NavSound::Click);
    rando::AcknowledgeGeneration();
    rando::StartGeneration();
  });
  SetHelp(leftPane, rightPane, generate,
          "Generates a seed from the settings on the <b>Logic</b> and <b>Item Pool</b> tabs, "
          "using Randovania's Metroid Prime logic database.<br/><br/>The new seed is used for "
          "the next new game. Your game files are never modified: the randomizer swaps pickups "
          "as rooms load.");

  mStatusText = leftPane.add_rml(GenerationStatusRml());
  mShownState = rando::GetGenerationStatus().state;

  leftPane.add_section("Share");
  auto& copyCode = leftPane.add_button("Copy Share Code");
  copyCode.on_pressed([this] { copy_share_code(); });
  leftPane.register_control(copyCode, rightPane, [](Pane& pane) {
    pane.clear();
    Rml::String rml =
        "Copies a share code for your settings to the clipboard. Whoever pastes it with "
        "<b>Paste Share Code</b> gets the same settings and seed string, so they generate the "
        "same seed as you. Handy for races.<br/><br/>The code covers everything that changes "
        "the seed: the <b>Logic</b>, <b>Item Pool</b>, <b>Rooms</b> and <b>Door Locks</b> "
        "tabs and <b>Fully Disable Logic</b>. Your individual QoL options are kept untouched.";
    if (rando::GetSettings().seedString.empty()) {
      rml += "<br/><br/><icon class=\"warning\"/> Your seed string is empty, so the code shares "
             "settings only and every seed made from it is different.";
    }
    if (rando::GetPickupDatabase() != nullptr) {
      rml += "<br/><br/>Your share code:<br/>" +
             ShareCodeRml(rando::EncodeShareCode(*rando::GetDatabase(),
                                                 *rando::GetPickupDatabase(),
                                                 rando::GetSettings()));
    }
    pane.add_rml(rml);
  });

  auto& pasteCode = leftPane.add_button("Paste Share Code");
  pasteCode.on_pressed([this] {
    char* text = SDL_GetClipboardText();
    const std::string code = text != nullptr ? text : "";
    SDL_free(text);
    if (code.empty()) {
      play_nav_sound(NavSound::Warning);
      SetRml(mShareText, "<icon class=\"warning\"/> The clipboard is empty. Copy a share code "
                         "first.");
      return;
    }
    play_nav_sound(NavSound::Click);
    import_share_code(code);
  });
  SetHelp(leftPane, rightPane, pasteCode,
          "Reads a share code from the clipboard and shows what it sets. Nothing changes "
          "until you choose <b>Import</b>.<br/><br/>Importing replaces your seed string and "
          "every setting that changes the seed. <b>Import and Generate</b> also generates the "
          "seed right away.<br/><br/>Codes only work between copies of Metaforce with the same "
          "randomizer data.");

  mShareText = leftPane.add_rml("Trade settings with a share code.");

  leftPane.add_section("Seeds");
  auto& seeds = leftPane.add_group_button({.text = "Generated Seeds"});
  leftPane.register_control(seeds, rightPane, [this](Pane& pane) {
    pane.clear();
    add_seed_list(pane);
  });
}

// Player preferences that apply during play. None of them change the generated seed.
void RandomizerWindow::build_qol_tab(Rml::Element* content) {
  auto& leftPane = add_child< Pane >(content, Pane::Type::Controlled);
  auto& rightPane = add_child< Pane >(content, Pane::Type::Uncontrolled);

  leftPane.add_section("Shortcuts");
  auto& quickSave = leftPane.add_child< BoolButton >(BoolButton::Props{
      .key = "Quick Save",
      .getValue = [] { return rando::GetQuickSave(); },
      .setValue = [](bool v) { rando::SetQuickSave(v); },
      .isDisabled = [] { return rando::GetTournament(); },
      .isModified = [] { return !rando::GetQuickSave(); },
  });
  SetHelp(leftPane, rightPane, quickSave,
          "Hold <b>R</b> + <b>Z</b> + <b>D-pad Right</b> during a randomized game to open the save "
          "screen wherever you are. Loading the save puts you back on that spot.<br/><br/>You "
          "can't save while paused, in a cutscene, mid-morph, during the escape, or while dead; "
          "a warning sound plays instead.<br/><br/>While this is on, holding R stops Z from "
          "opening the map.<br/><br/>Off while <b>Tournament Mode</b> is on.");

  auto& quickReload = leftPane.add_child< BoolButton >(BoolButton::Props{
      .key = "Quick Reload",
      .getValue = [] { return rando::GetQuickReload(); },
      .setValue = [](bool v) { rando::SetQuickReload(v); },
      .isDisabled = [] { return rando::GetTournament(); },
      .isModified = [] { return !rando::GetQuickReload(); },
  });
  SetHelp(leftPane, rightPane, quickReload,
          "Hold <b>R</b> + <b>Z</b> + <b>D-pad Left</b> during a randomized game to reload your "
          "last save at once, as if you had died and chosen Continue.<br/><br/>Without a save, "
          "the seed starts over. While this is on, holding R stops Z from opening the map.<br/><br/>"
          "Off while <b>Tournament Mode</b> is on.");

  std::vector< DropdownButton::Option > mapWarpModes;
  for (int mode = 0; mode < static_cast< int >(std::size(rando::kMapWarpNames)); ++mode) {
    // Tournament Mode doesn't allow warping to rooms not yet entered. Switching tabs rebuilds
    // this, so it follows the toggle.
    mapWarpModes.push_back({.text = rando::kMapWarpNames[mode],
                            .enabled = mode != rando::kMapWarpOn || !rando::GetTournament()});
  }
  auto& mapWarp = leftPane.add_child< DropdownButton >(DropdownButton::Props{
      .key = "Allow Warping on Map",
      .options = std::move(mapWarpModes),
      .getValue = [] { return rando::GetMapWarp(); },
      .setValue = [](int v) { rando::SetMapWarp(v); },
      .isModified = [] { return rando::GetMapWarp() != rando::kMapWarpVisited; },
  });
  SetHelp(leftPane, rightPane, mapWarp,
          "In a randomized game, holding <b>A</b> on the map warps to the highlighted room."
          "<br/><br/><b>On</b> warps to any room on the map. <b>Visited Only</b> warps only to "
          "rooms you've already entered in that save. <b>Off</b> turns the warp off.<br/><br/>"
          "<b>On</b> isn't allowed while <b>Tournament Mode</b> is on; it acts as Visited Only. "
          "Warp to Start is always allowed.");

  leftPane.add_section("Autosave & Map");
  auto& autosave = leftPane.add_group_button({.text = "Autosave..."});
  leftPane.register_control(autosave, rightPane, [this](Pane& pane) {
    pane.clear();
    add_autosave_panel(pane);
  });

  auto& roomRando = leftPane.add_group_button({.text = "Room Rando Options..."});
  leftPane.register_control(roomRando, rightPane, [this](Pane& pane) {
    pane.clear();
    add_room_rando_panel(pane);
  });

  auto& settings = rando::GetSettings();
  leftPane.add_section("Seed Specific QoL");
  auto& mainPlazaDoor = leftPane.add_child< BoolButton >(BoolButton::Props{
      .key = "Main Plaza Vault Door",
      .getValue = [&settings] { return settings.mainPlazaDoor; },
      .setValue =
          [&settings](bool v) {
            settings.mainPlazaDoor = v;
            Save();
          },
      .isModified = [&settings] { return !settings.mainPlazaDoor; },
  });
  SetHelp(leftPane, rightPane, mainPlazaDoor,
          "Makes the door on Main Plaza's Energy Tank ledge a blue door, so the Vault can be "
          "reached from Main Plaza.<br/><br/>Takes effect on the next seed you generate; the logic "
          "counts on it. Seeds already made keep what they were made with.");
}

// The help strip sits under the selector, where the help for the focused control shows.
void RandomizerWindow::add_autosave_panel(Pane& pane) {
  std::vector< DropdownButton::Option > autosaveModes;
  for (const char* name : rando::kAutosaveNames) {
    autosaveModes.push_back({name});
  }
  auto& autosave = pane.add_child< DropdownButton >(DropdownButton::Props{
      .key = "Autosave",
      .options = std::move(autosaveModes),
      .getValue =
          [] {
            if (!rando::GetTournament()) {
              return rando::GetAutosave();
            }
            const rando::Seed* active = rando::GetActiveSeed();
            return rando::GetTournamentAutosave(active != nullptr
                                                    ? !active->docks.empty()
                                                    : rando::GetSettings().roomRando != 0);
          },
      .setValue = [](int v) { rando::SetAutosave(v); },
      .isDisabled = [] { return rando::GetItemWarp() || rando::GetTournament(); },
      .isModified = [] { return rando::GetAutosave() != rando::kAutosaveAuto; },
  });
  auto& helpPane = pane.add_child< Pane >(Pane::Type::Uncontrolled);
  SetHelp(pane, helpPane, autosave,
          "Saves a randomized game to its slot after you collect a pickup, without the save "
          "screen. Loading the save puts you back on that spot.<br/><br/>The save waits until "
          "Samus is on the ground and she hasn't lost energy for two seconds."
          "<br/><br/><b>Major Only</b> saves after upgrades, Energy Tanks and artifacts. <b>All Pickups</b> saves "
          "after expansions too. <b>Auto</b> is Major Only for seeds with the room randomizer, "
          "where a wrong move is most painful, and Off for the rest.<br/><br/>The save each "
          "autosave replaces is kept; see <b>Autosave Backups</b>.<br/><br/>Off while <b>Item "
          "Warp</b> is on. <b>Tournament Mode</b> locks it to All Pickups for seeds with the room "
          "randomizer and Major Only for the rest.");

  pane.add_section("Autosave Backups");
  add_backup_list(pane);
}

// As in the Autosave panel, the selectors come first and the help strip sits under them.
void RandomizerWindow::add_room_rando_panel(Pane& pane) {
  std::vector< DropdownButton::Option > mapLayouts;
  for (const char* name : rando::kMapLayoutNames) {
    mapLayouts.push_back({name});
  }
  auto& mapLayout = pane.add_child< DropdownButton >(DropdownButton::Props{
      .key = "Room Rando Map",
      .options = std::move(mapLayouts),
      .getValue = [] { return rando::GetMapLayout(); },
      .setValue = [](int v) { rando::SetMapLayout(v); },
      .isModified = [] { return rando::GetMapLayout() != rando::kMapLayoutConnected; },
  });
  std::vector< DropdownButton::Option > drawDistances;
  for (const char* name : rando::kMapDrawDistanceNames) {
    drawDistances.push_back({name});
  }
  auto& drawDistance = pane.add_child< DropdownButton >(DropdownButton::Props{
      .key = "Map Draw Distance",
      .options = std::move(drawDistances),
      .getValue =
          [] {
            const auto& rooms = rando::kMapDrawDistances;
            const auto it = std::find(std::begin(rooms), std::end(rooms), rando::GetMapDrawDistance());
            return it == std::end(rooms) ? 0 : static_cast< int >(it - std::begin(rooms));
          },
      .setValue = [](int v) { rando::SetMapDrawDistance(rando::kMapDrawDistances[v]); },
      .isModified = [] { return rando::GetMapDrawDistance() != rando::kMapDrawDistanceDefault; },
  });
  auto& roomNames = pane.add_child< BoolButton >(BoolButton::Props{
      .key = "Room Names",
      .getValue = [] { return rando::GetRoomNames(); },
      .setValue = [](bool v) { rando::SetRoomNames(v); },
      .isModified = [] { return rando::GetRoomNames(); },
  });
  auto& regionTint = pane.add_child< NumberButton >(NumberButton::Props{
      .key = "Region Tint",
      .getValue = [] { return rando::GetRegionTint(); },
      .setValue = [](int v) { rando::SetRegionTint(v); },
      .isModified = [] { return rando::GetRegionTint() != 0; },
      .min = 0,
      .max = 100,
      .step = 5,
      .suffix = "%",
  });

  auto& helpPane = pane.add_child< Pane >(Pane::Type::Uncontrolled);
  SetHelp(pane, helpPane, roomNames,
          "In a randomized game, shows the name of a room in the bottom left corner for a few "
          "seconds the first time you enter it. Handy for newer players who don't want to constantly check the map.<br/><br/>Rooms "
          "you've already entered in that save don't show up again.");
  SetHelp(pane, helpPane, regionTint,
          "Tints each room on the map toward the color of the region it belongs to, by this "
          "percentage.<br/><br/>The colors match the planet map: Tallon is green, Chozo is amber, "
          "Magmoor is red, Phendrana is white, and Mines is deep purple.");
  SetHelp(pane, helpPane, mapLayout,
          "How the map is drawn when the room randomizer moved doors.<br/><br/><b>Vanilla</b> "
          "draws every room where it is in the game, so the room behind a moved door is "
          "somewhere else on the map.<br/><br/><b>Tidy</b> lays the map out again so rooms meet "
          "at the doors between them, the doors you went through first. Rooms that fit by none "
          "of their doors are drawn apart, next to one of them.<br/><br/><b>Connected</b> joins "
          "every room by one of its doors, even where that draws it over other rooms; where "
          "rooms take up the same space, the map outlines it.<br/><br/>Doors closing a loop of "
          "moved doors can't meet either way.");
  SetHelp(pane, helpPane, drawDistance,
          "How many rooms around the selected room the map screen draws, nearest first by "
          "doors. The farthest of them fade out and every room past them is drawn as an "
          "outline only. Moving around the map brings the rooms around the new selection "
          "in.<br/><br/><b>1 Room</b> draws only the selected room in full, for low end "
          "PCs.<br/><br/>Mixed regions put every room of "
          "the game on one map. Drawing them all at once will be very slow on most PCs. "
          "<b>Whole World</b> does it anyway.");
}

void RandomizerWindow::add_seed_list(Pane& pane) {
  mSeeds = rando::ListSeeds();
  if (mSeeds.empty()) {
    pane.add_text("No seeds yet. Generate one to get started.");
    mSeedList = nullptr;
    return;
  }
  pane.add_text("Choose a seed to use it, view its spoiler log or delete it.");
  std::vector< List::Item > items;
  for (size_t i = 0; i < mSeeds.size(); ++i) {
    items.push_back({.key = i, .label = SeedLabel(mSeeds[i])});
  }
  pane.add_child< TrackedList >(List::Props{
      .items = std::move(items),
      .onPressed =
          [this](uint64_t key) {
            if (key < mSeeds.size()) {
              play_nav_sound(NavSound::Click);
              show_seed_actions(mSeeds[key].hash);
            }
          },
      .isSelected =
          [this](uint64_t key) {
            return key < mSeeds.size() && mSeeds[key].hash == rando::GetArmedSeed();
          },
      },
      mSeedList);
}

void RandomizerWindow::refresh_seeds() {
  if (mSeedList == nullptr) {
    return;
  }
  mSeeds = rando::ListSeeds();
  std::vector< List::Item > items;
  for (size_t i = 0; i < mSeeds.size(); ++i) {
    items.push_back({.key = i, .label = SeedLabel(mSeeds[i])});
  }
  mSeedList->set_items(std::move(items));
}

void RandomizerWindow::show_seed_actions(const std::string& hash) {
  const auto dismiss = [](Modal& modal) { modal.pop(); };
  std::string error;
  const auto seed = rando::LoadSeed(hash, error);
  Rml::String body;
  if (seed) {
    body = fmt::format("Seed string: {}<br/>Start: {}<br/>Starting items: {}",
                       EscapeRml(seed->seedString),
                       seed->randomStart ? Rml::String("Random (see the spoiler log)")
                                         : EscapeRml(seed->startName),
                       seed->startingItems.size());
  } else {
    body = EscapeRml(error);
  }
  push(std::make_unique< Modal >(Modal::Props{
      .title = fmt::format("Seed {}", hash),
      .bodyRml = body,
      .actions =
          {
              ModalAction{
                  .label = "Use for New Games",
                  .onPressed =
                      [hash, dismiss](Modal& modal) {
                        play_nav_sound(NavSound::ItemEnable);
                        rando::SetArmedSeed(hash);
                        dismiss(modal);
                      },
              },
              ModalAction{
                  .label = "Write Spoiler Log",
                  .onPressed =
                      [this, hash, dismiss](Modal& modal) {
                        std::string err;
                        const auto path = rando::WriteSpoiler(hash, err);
                        dismiss(modal);
                        if (path) {
                          play_nav_sound(NavSound::Click);
                          show_message("Spoiler Log",
                                       fmt::format("Written to<br/>{}", EscapeRml(path->string())));
                        } else {
                          show_message("Spoiler Log", EscapeRml(err), true);
                        }
                      },
              },
              ModalAction{
                  .label = "Delete",
                  .onPressed =
                      [this, hash, dismiss](Modal& modal) {
                        play_nav_sound(NavSound::ItemDisable);
                        rando::DeleteSeed(hash);
                        dismiss(modal);
                        refresh_seeds();
                      },
              },
              ModalAction{
                  .label = "Cancel",
                  .onPressed =
                      [dismiss](Modal& modal) {
                        play_nav_sound(NavSound::WindowClose);
                        dismiss(modal);
                      },
              },
          },
      .onDismiss = dismiss,
      .icon = "controller",
      .isVertical = true,
  }));
}

void RandomizerWindow::add_backup_list(Pane& pane) {
  mBackups = rando::ListAutosaveBackups();
  if (!rando::CanRestoreAutosaveBackup()) {
    pane.add_text("Start or load a randomized game to see the backups of its save slot.");
    return;
  }
  if (mBackups.empty()) {
    pane.add_text("No backups yet. One is kept each time an autosave replaces a save.");
    return;
  }
  pane.add_text("The saves your autosaves replaced, newest first. Choose one to go back to it.");
  std::vector< List::Item > items;
  for (size_t i = 0; i < mBackups.size(); ++i) {
    items.push_back({.key = i, .label = BackupLabel(mBackups[i])});
  }
  pane.add_child< List >(List::Props{
      .items = std::move(items),
      .onPressed =
          [this](uint64_t key) {
            if (key < mBackups.size()) {
              play_nav_sound(NavSound::Click);
              show_backup_actions(mBackups[key]);
            }
          },
  });
}

void RandomizerWindow::show_backup_actions(const rando::AutosaveBackup& backup) {
  const auto dismiss = [](Modal& modal) { modal.pop(); };
  push(std::make_unique< Modal >(Modal::Props{
      .title = "Restore Backup",
      .bodyRml = fmt::format("{}<br/><br/>The game reloads from this save, as with Quick Reload, "
                             "and saves it to your slot once it's loaded. The save it replaces "
                             "is kept as a backup.",
                             EscapeRml(BackupLabel(backup))),
      .actions =
          {
              ModalAction{
                  .label = "Restore",
                  .onPressed =
                      [this, file = backup.file, dismiss](Modal& modal) {
                        const std::string error = rando::RestoreAutosaveBackup(file);
                        dismiss(modal);
                        if (error.empty()) {
                          play_nav_sound(NavSound::ItemEnable);
                        } else {
                          show_message("Restore Backup", EscapeRml(error), true);
                        }
                      },
              },
              ModalAction{
                  .label = "Cancel",
                  .onPressed =
                      [dismiss](Modal& modal) {
                        play_nav_sound(NavSound::WindowClose);
                        dismiss(modal);
                      },
              },
          },
      .onDismiss = dismiss,
      .icon = "controller",
      .isVertical = true,
  }));
}

void RandomizerWindow::show_message(const Rml::String& title, const Rml::String& body,
                                    bool danger) {
  const auto dismiss = [](Modal& modal) { modal.pop(); };
  push(std::make_unique< Modal >(Modal::Props{
      .title = title,
      .bodyRml = body,
      .actions = {ModalAction{.label = "OK",
                              .onPressed =
                                  [dismiss](Modal& modal) {
                                    play_nav_sound(NavSound::WindowClose);
                                    dismiss(modal);
                                  }}},
      .onDismiss = dismiss,
      .variant = danger ? "danger" : "",
      .icon = danger ? "error" : "download-done",
  }));
}

void RandomizerWindow::copy_share_code() {
  const rando::PickupDatabase* pickups = rando::GetPickupDatabase();
  if (pickups == nullptr) {
    return;
  }
  const auto& settings = rando::GetSettings();
  const std::string code = rando::EncodeShareCode(*rando::GetDatabase(), *pickups, settings);
  if (!SDL_SetClipboardText(code.c_str())) {
    // Show the code anyway, so it can still be written down.
    play_nav_sound(NavSound::Warning);
    show_message("Share Code",
                 "Couldn't copy to the clipboard. Your share code is:<br/><br/>" +
                     ShareCodeRml(code),
                 true);
    return;
  }
  play_nav_sound(NavSound::ItemEnable);
  SetRml(mShareText,
         settings.seedString.empty()
             ? Rml::String("<icon class=\"download-done\"/> Share code copied. Your seed string "
                           "is empty, so it shares settings only.")
             : fmt::format("<icon class=\"download-done\"/> Share code copied, with seed string "
                           "<b>{}</b>.",
                           EscapeRml(settings.seedString)));
}

void RandomizerWindow::import_share_code(const std::string& code) {
  const rando::Database* db = rando::GetDatabase();
  const rando::PickupDatabase* pickups = rando::GetPickupDatabase();
  if (db == nullptr || pickups == nullptr) {
    return;
  }
  std::string error;
  auto decoded = rando::DecodeShareCode(*db, *pickups, code, error);
  if (!decoded) {
    SetRml(mShareText, "<icon class=\"warning\"/> That share code couldn't be read.");
    show_message("Share Code", EscapeRml(error), true);
    return;
  }

  Rml::String body = ShareSummaryRml(*db, *pickups, decoded->settings);
  if (!decoded->exact) {
    body += "<br/><br/><icon class=\"warning\"/> Some of the shared settings couldn't be put in "
            "the code exactly, so seeds will differ from the sharer's.";
  }
  const auto beam = decoded->settings.standard.find("Phazon Beam");
  const bool usesBeam = beam != decoded->settings.standard.end() &&
                        (beam->second.shuffled > 0 || beam->second.starting > 0);
  if (rando::GetTournament() && (decoded->settings.logicDisabled || usesBeam)) {
    body += "<br/><br/><icon class=\"warning\"/> <b>Tournament Mode</b> turns logic back on and "
            "removes the Phazon Beam, so seeds will differ from the sharer's.";
  }
  body += "<br/><br/>Importing replaces your current settings.";

  const auto dismiss = [](Modal& modal) { modal.pop(); };
  const auto apply = [this, settings = decoded->settings](bool generate) {
    rando::GetSettings() = settings;
    rando::ApplyTournamentSettings(rando::GetSettings());
    rando::SaveSettings();
    play_nav_sound(NavSound::ItemEnable);
    SetRml(mShareText, "<icon class=\"download-done\"/> Share code imported.");
    if (generate && rando::GetGenerationStatus().state != rando::GenerationState::Running) {
      rando::AcknowledgeGeneration();
      rando::StartGeneration();
    }
  };
  push(std::make_unique< Modal >(Modal::Props{
      .title = "Import Share Code",
      .bodyRml = body,
      .actions =
          {
              ModalAction{
                  .label = "Import and Generate",
                  .onPressed =
                      [apply, dismiss](Modal& modal) {
                        apply(true);
                        dismiss(modal);
                      },
              },
              ModalAction{
                  .label = "Import",
                  .onPressed =
                      [apply, dismiss](Modal& modal) {
                        apply(false);
                        dismiss(modal);
                      },
              },
              ModalAction{
                  .label = "Cancel",
                  .onPressed =
                      [dismiss](Modal& modal) {
                        play_nav_sound(NavSound::WindowClose);
                        dismiss(modal);
                      },
              },
          },
      .onDismiss = dismiss,
      .icon = "controller",
      .isVertical = true,
  }));
}

void RandomizerWindow::build_rooms_tab(Rml::Element* content) {
  auto& leftPane = add_child< Pane >(content, Pane::Type::Controlled);
  auto& rightPane = add_child< Pane >(content, Pane::Type::Uncontrolled);
  auto& settings = rando::GetSettings();
  const rando::Database& db = *rando::GetDatabase();
  const auto disabled = [&settings] { return settings.roomRando == 0; };

  leftPane.add_section("Room Randomizer");
  std::vector< DropdownButton::Option > modes;
  for (const char* name : rando::kRoomRandoNames) {
    modes.push_back({name});
  }
  AddDropdown(leftPane, rightPane, "Mode",
              "Shuffles which room each door leads to.<br/><br/><b>Two-way</b> shuffles within each "
              "region and "
              "pairs doors both ways, so going back through a door returns you to the room you "
              "came from.<br/><br/><b>Two-way, mixed regions</b> also pairs doors across "
              "Tallon Overworld, Chozo Ruins, Phendrana Drifts, Phazon Mines and Magmoor Caverns, "
              "so a door can lead into another region. Going through one of those doors loads "
              "the other region. The Frigate and Impact Crater are only shuffled within "
              "themselves. Elevators stay as they are.<br/><br/>"
              "Doors are only paired with doors of the same kind and shape. "
              "Walking through one carries you straight out of the other, and the room on the "
              "other side is visible through the open doorway.<br/><br/>Item placement accounts "
              "for the new layout, but generating takes longer.",
              std::move(modes), [&settings] { return settings.roomRando; },
              [&settings](int v) {
                settings.roomRando =
                    std::clamp(v, 0, static_cast< int >(std::size(rando::kRoomRandoNames)) - 1);
              },
              [&settings] { return settings.roomRando != 0; });
  if (!db.HasDockShapes()) {
    leftPane.add_rml("<icon class=\"warning\"/> The randomizer data has no door shapes, so rooms "
                     "can't be shuffled. Re-export it with <b>--disc</b>.");
  }

  leftPane.add_section("Doors");
  auto& morph = leftPane.add_child< BoolButton >(BoolButton::Props{
      .key = "Shuffle Morph Ball Doors",
      .getValue = [&settings] { return settings.roomRandoMorphBallDoors; },
      .setValue =
          [&settings](bool v) {
            settings.roomRandoMorphBallDoors = v;
            Save();
          },
      .isDisabled = disabled,
      .isModified = [&settings] { return !settings.roomRandoMorphBallDoors; },
  });
  SetHelp(leftPane, rightPane, morph,
          "Whether Morph Ball doors are shuffled too. They're only ever paired with other Morph "
          "Ball doors of the same size.");

  leftPane.add_section("Regions");
  std::vector< int > regions(db.Regions().size());
  for (int i = 0; i < static_cast< int >(regions.size()); ++i) {
    regions[i] = i;
  }
  const auto orderOf = [&db](int region) {
    return std::find(kRegionOrder.begin(), kRegionOrder.end(), db.Regions()[region].name) -
           kRegionOrder.begin();
  };
  std::stable_sort(regions.begin(), regions.end(),
                   [&orderOf](int a, int b) { return orderOf(a) < orderOf(b); });
  for (const int region : regions) {
    const int doors = rando::CountShuffleableDocks(db, region);
    if (doors == 0) {
      continue;
    }
    const std::string name = db.Regions()[region].name;
    auto& button = leftPane.add_child< BoolButton >(BoolButton::Props{
        .key = EscapeRml(name),
        .getValue = [&settings, name] { return !settings.roomRandoExcludedRegions.contains(name); },
        .setValue =
            [&settings, name](bool v) {
              if (v) {
                settings.roomRandoExcludedRegions.erase(name);
              } else {
                settings.roomRandoExcludedRegions.insert(name);
              }
              Save();
            },
        .isDisabled = disabled,
        .isModified =
            [&settings, name] {
              return settings.roomRandoExcludedRegions.contains(name) !=
                     rando::Settings::DefaultExcludedRegions().contains(name);
            },
    });
    SetHelp(leftPane, rightPane, button,
            fmt::format("Whether the doors of {} are shuffled. {} of its doors can be shuffled; "
                        "the rest, like elevators and doors that can't line up with any other, "
                        "always stay as they are.",
                        EscapeRml(name), doors));
  }
}

namespace {
// "door/Missile Blast Shield (randomprime)" -> "Missile Blast Shield"
std::string WeaknessLabel(const std::string& name) {
  std::string label = name.substr(name.find('/') + 1);
  constexpr std::string_view kSuffix = " (randomprime)";
  if (label.ends_with(kSuffix)) {
    label.erase(label.size() - kSuffix.size());
  }
  return label;
}
} // namespace

void RandomizerWindow::build_doors_tab(Rml::Element* content) {
  auto& leftPane = add_child< Pane >(content, Pane::Type::Controlled);
  auto& rightPane = add_child< Pane >(content, Pane::Type::Uncontrolled);
  auto& settings = rando::GetSettings();
  const rando::Database& db = *rando::GetDatabase();
  const rando::DockTypeDistributor* doors = db.FindDockType("door");
  if (doors == nullptr) {
    leftPane.add_rml("<icon class=\"warning\"/> The randomizer data has no door lock settings.");
    return;
  }
  const auto disabled = [&settings] { return settings.doorLockMode == 0; };

  leftPane.add_section(EscapeRml(doors->label));
  std::vector< DropdownButton::Option > modes;
  for (const char* name : rando::kDoorLockModeNames) {
    modes.push_back({name});
  }
  AddDropdown(leftPane, rightPane, "Mode",
              "Changes which weapon opens each door, after Randovania's door lock "
              "randomizer.<br/><br/><b>Unmodified</b> keeps the original door locks.<br/><br/>"
              "<b>Individually</b> randomizes each door individually. Items are placed as if "
              "every door that can change were blue, then each door gets a lock you can open by "
              "the time you first reach it. A door that can be walked around both ways may be "
              "locked for good.<br/><br/>With door locks randomized, the game's own missile blast "
              "shields are replaced with ones that open from behind: opening the door from the "
              "other side removes them.",
              std::move(modes), [&settings] { return settings.doorLockMode; },
              [&settings](int v) {
                const int mode =
                    std::clamp(v, 0, static_cast< int >(std::size(rando::kDoorLockModeNames)) - 1);
                // As Randovania does when the mode changes.
                if (mode != 0 && settings.doorLockMode == 0) {
                  settings.unlockSaveStationDoors = true;
                  settings.blastShieldLockOn = true;
                }
                settings.doorLockMode = mode;
              },
              [&settings] { return settings.doorLockMode != 0; });

  const auto addWeaknesses = [&](const char* section, const std::vector< int >& weaknesses,
                                 std::set< std::string > rando::Settings::*set,
                                 std::set< std::string > (*defaults)(), const char* help) {
    leftPane.add_section(section);
    for (const int weakness : weaknesses) {
      const std::string name = db.Weaknesses()[weakness].name;
      // The unlocked door is what any door falls back to, so it can always be changed into.
      const bool always = set == &rando::Settings::doorLockChangeTo && weakness == doors->unlocked;
      auto& button = leftPane.add_child< BoolButton >(BoolButton::Props{
          .key = EscapeRml(WeaknessLabel(name)),
          .getValue = [&settings, set, name,
                       always] { return always || (settings.*set).contains(name); },
          .setValue =
              [&settings, set, name](bool v) {
                if (v) {
                  (settings.*set).insert(name);
                } else {
                  (settings.*set).erase(name);
                }
                Save();
              },
          .isDisabled = [disabled, always] { return always || disabled(); },
          .isModified =
              [&settings, set, defaults, name] {
                return (settings.*set).contains(name) != defaults().contains(name);
              },
      });
      SetHelp(leftPane, rightPane, button,
              always ? Rml::String("Doors that can't get any other lock stay blue, so this is "
                                   "always allowed.")
                     : Rml::String(help));
    }
  };
  addWeaknesses("Doors to Change", doors->changeFrom, &rando::Settings::doorLockChangeFrom,
                &rando::Settings::DefaultDoorLockChangeFrom,
                "Whether doors with this lock in the original game get a new lock.");
  addWeaknesses("Change Into", doors->changeTo, &rando::Settings::doorLockChangeTo,
                &rando::Settings::DefaultDoorLockChangeTo,
                "Whether doors can get this lock. Locks that need items are only given to doors "
                "you can reach with those items.");

  leftPane.add_section("Changes");
  auto& saves = leftPane.add_child< BoolButton >(BoolButton::Props{
      .key = "Unlock Save Station Doors",
      .getValue = [&settings] { return settings.unlockSaveStationDoors; },
      .setValue =
          [&settings](bool v) {
            settings.unlockSaveStationDoors = v;
            Save();
          },
      .isDisabled = disabled,
      .isModified = [&settings] { return !settings.unlockSaveStationDoors; },
  });
  SetHelp(leftPane, rightPane, saves,
          "Sets all Save Station doors to blue regardless of door randomization mode.");
  auto& lockOn = leftPane.add_child< BoolButton >(BoolButton::Props{
      .key = "Enable Blast Shield Lock-On",
      .getValue = [&settings] { return settings.blastShieldLockOn; },
      .setValue =
          [&settings](bool v) {
            settings.blastShieldLockOn = v;
            Save();
          },
      .isDisabled = disabled,
      .isModified = [&settings] { return !settings.blastShieldLockOn; },
  });
  SetHelp(leftPane, rightPane, lockOn,
          "Makes all Blast Shield locks targetable in Combat Visor.");
}

// Locks out what would give a racer an edge. The locked controls on the other tabs show as
// disabled while it's on.
void RandomizerWindow::build_tournament_tab(Rml::Element* content) {
  auto& leftPane = add_child< Pane >(content, Pane::Type::Controlled);
  auto& rightPane = add_child< Pane >(content, Pane::Type::Uncontrolled);

  leftPane.add_section("Tournament");
  auto& tournament = leftPane.add_child< BoolButton >(BoolButton::Props{
      .key = "Tournament Mode",
      .getValue = [] { return rando::GetTournament(); },
      .setValue =
          [](bool v) {
            rando::SetTournament(v);
            borealis::ui::MenuBar::refresh_tabs();
          },
      .isModified = [] { return rando::GetTournament(); },
  });
  SetHelp(leftPane, rightPane, tournament,
          "Locks some settings for racing, so every runner plays by the same rules.<br/><br/>"
          "· The <b>Warp</b> and <b>Cheats</b> menus are gone, and every cheat is turned "
          "off.<br/>"
          "· <b>Quick Save</b> and <b>Quick Reload</b> are off.<br/>"
          "· <b>Autosave</b> is All Pickups for seeds with the room randomizer, and Major Only "
          "for the rest.<br/>"
          "· <b>Item Warp</b> is off.<br/>"
          "· <b>Allow Warping on Map</b> can't be On; it acts as Visited Only.<br/>"
          "· Seeds are generated with the <b>Phazon Beam</b> cheat item removed and <b>Fully Disable "
          "Logic</b> off.<br/><br/>"
          "Your default settings are preserved with this enabled. ");
}

void RandomizerWindow::build_item_warp_tab(Rml::Element* content) {
  auto& leftPane = add_child< Pane >(content, Pane::Type::Controlled);
  auto& rightPane = add_child< Pane >(content, Pane::Type::Uncontrolled);

  leftPane.add_section("Item Warp");
  auto& itemWarp = leftPane.add_child< BoolButton >(BoolButton::Props{
      .key = "Item Warp",
      .getValue = [] { return rando::GetItemWarp(); },
      .setValue = [](bool v) { rando::SetItemWarp(v); },
      .isDisabled = [] { return rando::GetTournament(); },
      .isModified = [] { return rando::GetItemWarp(); },
  });
  SetHelp(leftPane, rightPane, itemWarp,
          "When enabled, collecting a pickup takes you to a random room a few seconds later. "
          "The room is determined by the seed, so the same item will always lead you to the same "
          "room.<br/><br/>By default the items that warp you are any major upgrades, Energy Tanks, artifacts, as well as missile and "
          "power bomb expansions. <br/><br/>This is inspired by the original Claris Item Warp randomizer.<br/><br/> "
          "There is very limited logic to these, so seeds will often be unbeatable, or require excessive 'Warp to Start' abuse. ");

  auto& allPickups = leftPane.add_child< BoolButton >(BoolButton::Props{
      .key = "Refills and Drops",
      .getValue = [] { return rando::GetItemWarpAll(); },
      .setValue = [](bool v) { rando::SetItemWarpAll(v); },
      .isDisabled = [] { return !rando::GetItemWarp(); },
      .isModified = [] { return rando::GetItemWarpAll(); },
  });
  SetHelp(leftPane, rightPane, allPickups,
          "Enabling this also takes you to a random room after collecting a health drop, or a missile/Power Bomb refill. ");

  leftPane.add_section("Logic");
  auto& settings = rando::GetSettings();
  auto& disableLogic = leftPane.add_child< BoolButton >(BoolButton::Props{
      .key = "Fully Disable Logic",
      .getValue = [&settings] { return settings.logicDisabled; },
      .setValue =
          [&settings](bool v) {
            settings.logicDisabled = v;
            Save();
          },
      .isDisabled = [] { return rando::GetTournament(); },
      .isModified = [&settings] { return settings.logicDisabled; },
  });
  SetHelp(leftPane, rightPane, disableLogic,
          "Places items completely at random, with no verification that the seed can be beaten. Rooms and "
          "door locks are still randomized, without checking they make any logical sense.<br/><br/>This only applies "
          "to seeds generated after you turn the setting on. Expect a high possibility that seeds are impossible "
          "to finish, similar to the original randomizer.<br/><br/>It is advised to set your starting room to a hub with lots of doors. "
          "Otherwise you will often run into softlocks almost immediately.");
}

namespace {
// Trick sections of the Logic tab, by Randovania trick name. Tricks not listed go under "Other".
struct TrickGroup {
  const char* name;
  std::vector< std::string_view > tricks;
};
const std::array< TrickGroup, 4 > kTrickGroups = {{
    {"Movement",
     {"Movement", "LJump", "RJump", "SJump", "Dash", "Standable", "StandEnemies", "DBoosting",
      "UnderwaterMovement"}},
    {"Morph Ball", {"BJ", "BSJ", "CBJ", "IUJ", "WallBoost", "BoostlessSpiner"}},
    {"Knowledge and Combat", {"Knowledge", "InvisibleObjects", "Combat", "HeatRun"}},
    {"Glitches", {"ClipThruObjects", "OoB", "IS"}},
}};

std::vector< DropdownButton::Option > TrickLevelOptions() {
  std::vector< DropdownButton::Option > options;
  for (const char* name : rando::kTrickLevelNames) {
    options.push_back({name});
  }
  return options;
}

// Randovania's descriptions are HTML. The wiki links go, since the help pane can't open them.
Rml::String TrickHelpRml(const rando::TrickResource& trick) {
  Rml::String rml = trick.description;
  for (size_t start; (start = rml.find("<a ")) != Rml::String::npos;) {
    const size_t end = rml.find("</a>", start);
    rml.erase(start, end == Rml::String::npos ? Rml::String::npos : end + 4 - start);
  }
  while (rml.ends_with("<br />") || rml.ends_with("<br/>")) {
    rml.erase(rml.rfind("<br"));
  }
  if (!trick.levels.empty()) {
    Rml::String levels;
    for (const int level : trick.levels) {
      if (level > 0 && level < static_cast< int >(std::size(rando::kTrickLevelNames))) {
        levels += fmt::format("{}<b>{}</b>", levels.empty() ? "" : ", ",
                              rando::kTrickLevelNames[level]);
      }
    }
    rml += fmt::format("<br/><br/>The logic uses this trick at: {}.", levels);
  }
  return rml;
}

// A section of trick level dropdowns per kTrickGroups entry.
void AddTrickSections(Pane& leftPane, Pane& rightPane, rando::Settings& settings,
                      const rando::Database& db) {
  std::vector< const rando::TrickResource* > other;
  for (const auto& trick : db.Tricks()) {
    const bool grouped = std::any_of(kTrickGroups.begin(), kTrickGroups.end(),
                                     [&trick](const TrickGroup& group) {
                                       return std::find(group.tricks.begin(), group.tricks.end(),
                                                        trick.name) != group.tricks.end();
                                     });
    if (!grouped) {
      other.push_back(&trick);
    }
  }
  const auto addTrick = [&](const rando::TrickResource& trick) {
    const std::string name = trick.name;
    AddDropdown(leftPane, rightPane, trick.longName, TrickHelpRml(trick), TrickLevelOptions(),
                [&settings, name] { return settings.TrickLevel(name); },
                [&settings, name](int v) { settings.SetTrickLevel(name, v); },
                [&settings, name] { return settings.TrickLevel(name) != 0; });
  };
  for (const auto& group : kTrickGroups) {
    bool sectionAdded = false;
    for (const std::string_view name : group.tricks) {
      const auto it = std::find_if(db.Tricks().begin(), db.Tricks().end(),
                                   [name](const rando::TrickResource& t) { return t.name == name; });
      if (it == db.Tricks().end()) {
        continue;
      }
      if (!sectionAdded) {
        leftPane.add_section(group.name);
        sectionAdded = true;
      }
      addTrick(*it);
    }
  }
  if (!other.empty()) {
    leftPane.add_section("Other");
    for (const auto* trick : other) {
      addTrick(*trick);
    }
  }
}
} // namespace

void RandomizerWindow::build_logic_tab(Rml::Element* content) {
  auto& leftPane = add_child< Pane >(content, Pane::Type::Controlled);
  auto& rightPane = add_child< Pane >(content, Pane::Type::Uncontrolled);
  auto& settings = rando::GetSettings();
  const rando::Database& db = *rando::GetDatabase();
  build_start_regions(db);

  leftPane.add_section("Logic");
  std::vector< DropdownButton::Option > strictness;
  for (const char* name : rando::kDamageStrictnessNames) {
    strictness.push_back({name});
  }
  AddDropdown(leftPane, rightPane, "Damage Strictness",
              "How much damage the generator assumes you take when logic requires taking "
              "damage, such as crossing heated rooms without a suit.<br/><br/><b>Strict</b> "
              "expects exact play, <b>Lenient</b> doubles every damage requirement.",
              std::move(strictness), [&settings] { return settings.damageStrictness; },
              [&settings](int v) { settings.damageStrictness = std::clamp(v, 0, 2); },
              [&settings] { return settings.damageStrictness != 1; });

  leftPane.add_section("Game");
  AddNumber(leftPane, rightPane, "Chozo Artifacts",
            "How many Chozo Artifacts are shuffled into the item pool. The rest are already in "
            "Artifact Temple when the game starts.<br/><br/>Artifacts are shuffled in order, "
            "starting with the Artifact of Truth.",
            [&settings]() -> int& { return settings.artifactTarget; },
            rando::GetPickupDatabase()->defaultArtifactTarget, 0, 12);

  std::vector< DropdownButton::Option > artifactHints;
  for (const char* name : rando::kArtifactHintNames) {
    artifactHints.push_back({name});
  }
  AddDropdown(leftPane, rightPane, "Chozo Artifact Hints",
              "How precise the hints on the totems in Artifact Temple are. Every totem can be "
              "scanned from the start, and tells you where its artifact is.<br/><br/>"
              "<b>Disabled</b> gives no hint.<br/><br/><b>Region only</b> shows the region "
              "name.<br/><br/><b>Region and area</b> shows the region and area name.<br/><br/>"
              "This is set when the seed is generated.",
              std::move(artifactHints), [&settings] { return settings.artifactHints; },
              [&settings](int v) {
                settings.artifactHints = std::clamp(
                    v, 0, static_cast< int >(std::size(rando::kArtifactHintNames)) - 1);
              },
              [&settings] { return settings.artifactHints != rando::kArtifactHintsPrecise; });

  std::vector< DropdownButton::Option > phazonSuitHints;
  for (const char* name : rando::kArtifactHintNames) {
    phazonSuitHints.push_back({name});
  }
  AddDropdown(leftPane, rightPane, "Phazon Suit Hint",
              "How precise the Phazon Suit's hint is. Once every Chozo Artifact is in Artifact "
              "Temple and Ridley has appeared, the Phazon Suit's entry in the pause menu's "
              "inventory can be opened, and tells you where the suit is.<br/><br/>"
              "<b>Disabled</b> gives no hint.<br/><br/><b>Region only</b> shows the region "
              "name.<br/><br/><b>Region and area</b> shows the region and area name.<br/><br/>",
              std::move(phazonSuitHints), [&settings] { return settings.phazonSuitHints; },
              [&settings](int v) {
                settings.phazonSuitHints = std::clamp(
                    v, 0, static_cast< int >(std::size(rando::kArtifactHintNames)) - 1);
              },
              [&settings] { return settings.phazonSuitHints != rando::kArtifactHintsPrecise; });

  auto& start = leftPane.add_select_button({
      .key = "Starting Location",
      .getValue =
          [this, &settings] {
            if (settings.startingLocation == rando::kRandomStartingLocation) {
              return Rml::String("Random");
            }
            for (const auto& region : mStartRegions) {
              for (const auto& room : region.rooms) {
                if (room.key == settings.startingLocation) {
                  return Rml::String(fmt::format("{} ({})", room.label, region.name));
                }
              }
            }
            return Rml::String("Landing Site (Tallon Overworld)");
          },
      .isModified = [&settings] { return !settings.startingLocation.empty(); },
  });
  leftPane.register_control(start, rightPane, [this](Pane& pane) {
    pane.clear();
    pane.add_text("Where new games begin. Choose a region to see its rooms.");
    pane.add_child< TrackedList >(List::Props{
        .items = start_list_items(),
        .onPressed = [this](uint64_t key) { on_start_pressed(key); },
        .isSelected = [this](uint64_t key) { return is_start_selected(key); },
        },
        mStartList);
  });

  auto& randomStart = leftPane.add_button("Random Starting Location");
  randomStart.on_pressed([&settings] {
    play_nav_sound(NavSound::ItemChange);
    settings.startingLocation = rando::kRandomStartingLocation;
    Save();
  });
  SetHelp(leftPane, rightPane, randomStart,
          "Each seed starts somewhere random, chosen so the seed is still beatable.<br/><br/>"
          "The start stays a surprise: it's only shown in the seed's spoiler log.");

  AddTrickSections(leftPane, rightPane, settings, db);

  leftPane.add_section("Settings");
  auto& reset = leftPane.add_button("Reset All Settings");
  reset.on_pressed([] {
    play_nav_sound(NavSound::ItemDisable);
    rando::ResetSettings();
  });
  SetHelp(leftPane, rightPane, reset,
          "Restores Randovania's Starter Preset for every setting except the seed string.");
}


namespace {
constexpr uint64_t kRandomStartKey = 1;
constexpr int kRegionShift = 20;
constexpr uint64_t kRoomMask = (1ull << kRegionShift) - 1;

uint64_t RegionKey(size_t region) { return static_cast< uint64_t >(region + 1) << kRegionShift; }
uint64_t RoomKey(size_t region, size_t room) { return RegionKey(region) | (room + 1); }
} // namespace

void RandomizerWindow::build_start_regions(const rando::Database& db) {
  mStartRegions.clear();
  if (db.StartNode() >= 0) {
    const auto& node = db.GetNode(db.StartNode());
    const auto& area = db.Areas()[node.area];
    mDefaultStart = db.Regions()[area.region].name + "|" + area.name + "|" + node.name;
  }
  for (const auto& [key, name] : rando::StartingLocations(db)) {
    const auto first = key.find('|');
    const auto second = key.find('|', first + 1);
    const std::string region = key.substr(0, first);
    const std::string area = key.substr(first + 1, second - first - 1);
    const std::string node = key.substr(second + 1);
    if (mStartRegions.empty() || mStartRegions.back().name != region) {
      mStartRegions.push_back({region, {}});
    }
    auto& rooms = mStartRegions.back().rooms;
    const bool duplicate = std::any_of(rooms.begin(), rooms.end(),
                                       [&area](const StartRoom& r) { return r.area == area; });
    rooms.push_back({key, area, duplicate ? fmt::format("{} – {}", area, node) : area});
  }
}

std::vector< List::Item > RandomizerWindow::start_list_items() const {
  std::vector< List::Item > items{{.key = kRandomStartKey, .label = "Random (surprise)"}};
  for (size_t r = 0; r < mStartRegions.size(); ++r) {
    const auto& region = mStartRegions[r];
    const bool expanded = mExpandedRegions.contains(r);
    items.push_back({.key = RegionKey(r),
                     .label = fmt::format("{} {} ({})", expanded ? "–" : "+", region.name,
                                          region.rooms.size())});
    if (expanded) {
      for (size_t i = 0; i < region.rooms.size(); ++i) {
        items.push_back({.key = RoomKey(r, i), .label = fmt::format("· {}", region.rooms[i].label)});
      }
    }
  }
  return items;
}

void RandomizerWindow::on_start_pressed(uint64_t key) {
  auto& settings = rando::GetSettings();
  if (key == kRandomStartKey) {
    play_nav_sound(NavSound::ItemChange);
    settings.startingLocation = rando::kRandomStartingLocation;
    Save();
    return;
  }
  const size_t region = (key >> kRegionShift) - 1;
  const uint64_t room = key & kRoomMask;
  if (region >= mStartRegions.size()) {
    return;
  }
  if (room == 0) {
    // Rebuilding the list from inside its own callback isn't safe, so defer it to update().
    play_nav_sound(NavSound::Click);
    if (!mExpandedRegions.erase(region)) {
      mExpandedRegions.insert(region);
    }
    mStartListDirty = true;
    return;
  }
  const auto& selected = mStartRegions[region].rooms[room - 1].key;
  play_nav_sound(NavSound::ItemChange);
  settings.startingLocation = selected == mDefaultStart ? std::string{} : selected;
  Save();
}

bool RandomizerWindow::is_start_selected(uint64_t key) const {
  const auto& settings = rando::GetSettings();
  if (key == kRandomStartKey) {
    return settings.startingLocation == rando::kRandomStartingLocation;
  }
  const size_t region = (key >> kRegionShift) - 1;
  const uint64_t room = key & kRoomMask;
  if (region >= mStartRegions.size()) {
    return false;
  }
  const std::string& current =
      settings.startingLocation.empty() ? mDefaultStart : settings.startingLocation;
  if (room == 0) {
    const auto& rooms = mStartRegions[region].rooms;
    return std::any_of(rooms.begin(), rooms.end(),
                       [&current](const StartRoom& r) { return r.key == current; });
  }
  return mStartRegions[region].rooms[room - 1].key == current;
}

void RandomizerWindow::build_pool_tab(Rml::Element* content) {
  auto& leftPane = add_child< Pane >(content, Pane::Type::Controlled);
  auto& rightPane = add_child< Pane >(content, Pane::Type::Uncontrolled);
  auto& settings = rando::GetSettings();
  const rando::PickupDatabase& pickups = *rando::GetPickupDatabase();

  mPoolText = leftPane.add_rml(PoolRml());
  mShownPoolSize = rando::CountPoolSize(pickups, settings);

  leftPane.add_section("Major Items");
  for (const auto& def : pickups.standard) {
    if (def.hidden || def.name == "Energy Tank") {
      continue;
    }
    auto* state = &settings.standard[def.name];
    // The dropdown entry for a pickup state: the copy count is part of it, so a preset that
    // shuffles more than one of an item (Randovania's own does for Charge Beam) shows it.
    const auto modeOf = [](const rando::StandardPickupState& s) {
      if (s.starting > 0) {
        return kStandardStarting;
      }
      if (s.shuffled <= 0) {
        return kStandardRemoved;
      }
      return kStandardShuffled + std::min(s.shuffled, kMaxStandardCopies) - 1;
    };
    const int defaultMode = modeOf({def.defaultShuffled, def.defaultStarting, def.defaultAmmo});
    auto mode = [state, modeOf] { return modeOf(*state); };
    std::vector< DropdownButton::Option > options;
    for (int copies = 1; copies <= kMaxStandardCopies; ++copies) {
      options.push_back({copies == 1 ? Rml::String("Shuffled")
                                     : fmt::format("Shuffled ×{}", copies)});
    }
    options.push_back({"Starting Item"});
    options.push_back({"Removed"});
    AddDropdown(leftPane, rightPane, def.name,
                fmt::format("<b>Shuffled</b> places {} somewhere in the world, or several copies "
                            "of it with <b>×2</b> and <b>×3</b>.<br/><b>Starting Item</b> gives "
                            "it to Samus from the start.<br/><b>Removed</b> leaves it out of the "
                            "game.",
                            EscapeRml(def.name)),
                std::move(options), mode,
                [state](int v) {
                  state->shuffled = v < kStandardStarting ? v - kStandardShuffled + 1 : 0;
                  state->starting = v == kStandardStarting ? 1 : 0;
                },
                [mode, defaultMode] { return mode() != defaultMode; },
                [locked = def.name == "Phazon Beam"] { return locked && rando::GetTournament(); });
  }

  for (const auto& def : pickups.standard) {
    if (def.ammo.empty()) {
      continue;
    }
    auto* state = &settings.standard[def.name];
    const bool isPowerBomb = def.itemType == 7;
    AddNumber(leftPane, rightPane, fmt::format("{} Ammo", def.name),
              fmt::format("Ammo given by the {} itself.", EscapeRml(def.name)),
              [state]() -> int& { return state->ammo; }, def.defaultAmmo, 0,
              isPowerBomb ? 99 : 250, isPowerBomb ? 1 : 5);
  }

  if (const auto* tanks = pickups.FindStandard("Energy Tank")) {
    leftPane.add_section("Energy Tanks");
    auto* state = &settings.standard[tanks->name];
    AddNumber(leftPane, rightPane, "Shuffled Energy Tanks",
              "Energy Tanks placed in the world. Each adds 100 energy.",
              [state]() -> int& { return state->shuffled; }, tanks->defaultShuffled, 0, 14);
    AddNumber(leftPane, rightPane, "Starting Energy Tanks",
              "Energy Tanks Samus starts the game with.",
              [state]() -> int& { return state->starting; }, tanks->defaultStarting, 0, 14);
  }

  leftPane.add_section("Expansions");
  for (const auto& def : pickups.ammo) {
    auto* state = &settings.ammo[def.name];
    AddNumber(leftPane, rightPane, def.name,
              fmt::format("How many {}s are placed in the world.", EscapeRml(def.name)),
              [state]() -> int& { return state->count; }, def.defaultCount, 0, 100);
    AddNumber(leftPane, rightPane, fmt::format("{} Amount", def.name),
              def.refill ? fmt::format("How much each {} restores.", EscapeRml(def.name))
                         : fmt::format("Capacity added by each {}.", EscapeRml(def.name)),
              [state]() -> int& { return state->ammo; }, def.defaultAmmo, 1,
              def.itemType == 26 ? 999 : 99);
  }
}

} // namespace metaforce::ui
