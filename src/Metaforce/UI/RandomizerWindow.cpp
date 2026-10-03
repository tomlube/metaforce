#include "Metaforce/UI/RandomizerWindow.hpp"

#include "Metaforce/Randomizer/Generator.hpp"
#include "Metaforce/Randomizer/RoomRando.hpp"

#include <borealis/ui/bool_button.hpp>

#include <borealis/ui/dropdown_button.hpp>
#include <borealis/ui/list.hpp>
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

constexpr int kStandardShuffled = 0;
constexpr int kStandardStarting = 1;
constexpr int kStandardRemoved = 2;

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
                            std::function< bool() > isModified) {
  auto& button = leftPane.add_child< DropdownButton >(DropdownButton::Props{
      .key = std::move(key),
      .options = std::move(options),
      .getValue = std::move(get),
      .setValue =
          [set = std::move(set)](int v) {
            set(v);
            Save();
          },
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

Rml::String SeedLabel(const rando::SeedSummary& seed) {
  return fmt::format("{}  ·  {}", seed.hash, seed.randomStart ? "Random start" : seed.startName);
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

  add_tab("Seeds", [this](Rml::Element* content) {
    reset_tab_elements();
    build_seeds_tab(content);
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
}

void RandomizerWindow::reset_tab_elements() {
  mStatusText = nullptr;
  mArmedText = nullptr;
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

  leftPane.add_section("Generate");
  auto& seedString = leftPane.add_child< StringButton >(StringButton::Props{
      .key = "Seed String",
      .getValue = [&settings] { return settings.seedString; },
      .setValue =
          [&settings](Rml::String value) {
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

  leftPane.add_section("New Games");
  mArmedText = leftPane.add_rml(ArmedRml());
  mShownArmed = rando::GetArmedSeed();

  auto& disarm = leftPane.add_button("Start New Games Without Randomizer");
  disarm.on_pressed([] {
    play_nav_sound(NavSound::ItemDisable);
    rando::SetArmedSeed({});
  });
  SetHelp(leftPane, rightPane, disarm,
          "New games start vanilla. Saves that were started with a seed keep using it.");

  leftPane.add_section("Seeds");
  auto& seeds = leftPane.add_group_button({.text = "Generated Seeds"});
  leftPane.register_control(seeds, rightPane, [this](Pane& pane) {
    pane.clear();
    add_seed_list(pane);
  });
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

void RandomizerWindow::build_logic_tab(Rml::Element* content) {
  auto& leftPane = add_child< Pane >(content, Pane::Type::Controlled);
  auto& rightPane = add_child< Pane >(content, Pane::Type::Uncontrolled);
  auto& settings = rando::GetSettings();
  const rando::Database& db = *rando::GetDatabase();
  build_start_regions(db);

  leftPane.add_section("Logic");
  std::vector< DropdownButton::Option > trickLevels;
  for (const char* name : rando::kTrickLevelNames) {
    trickLevels.push_back({name});
  }
  AddDropdown(leftPane, rightPane, "Trick Level",
              "The highest difficulty of tricks the generator may require, for every trick in "
              "Randovania's database.<br/><br/><b>Disabled</b> only requires the intended way "
              "through the game.",
              std::move(trickLevels), [&settings] { return settings.trickLevel; },
              [&settings](int v) { settings.trickLevel = std::clamp(v, 0, 5); },
              [&settings] { return settings.trickLevel != 0; });

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
    const int defaultShuffled = std::max(def.defaultShuffled, 1);
    const int defaultMode = def.defaultStarting > 0  ? kStandardStarting
                            : def.defaultShuffled > 0 ? kStandardShuffled
                                                      : kStandardRemoved;
    auto mode = [state] {
      if (state->starting > 0) {
        return kStandardStarting;
      }
      return state->shuffled > 0 ? kStandardShuffled : kStandardRemoved;
    };
    AddDropdown(leftPane, rightPane, def.name,
                fmt::format("<b>Shuffled</b> places {} somewhere in the world.<br/><b>Starting "
                            "Item</b> gives it to Samus from the start.<br/><b>Removed</b> "
                            "leaves it out of the game.",
                            EscapeRml(def.name)),
                {{"Shuffled"}, {"Starting Item"}, {"Removed"}}, mode,
                [state, defaultShuffled](int v) {
                  state->shuffled = v == kStandardShuffled ? defaultShuffled : 0;
                  state->starting = v == kStandardStarting ? 1 : 0;
                },
                [mode, defaultMode] { return mode() != defaultMode; });
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
