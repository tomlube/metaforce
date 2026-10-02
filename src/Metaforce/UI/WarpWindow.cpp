#include "Metaforce/UI/WarpWindow.hpp"

#include "Metaforce/Warp.hpp"

#include <borealis/ui/pane.hpp>
#include <borealis/ui/ui.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <bit>

namespace metaforce::ui {
using namespace borealis::ui;

namespace {

struct WarpSelectionState {
  int worldIdx = 0;
  int areaIdx = 0; // Index into World::areas, which is sorted by name
  bool customLayers = false;
  uint64_t layerBits = 0;
  bool initialized = false;
};

WarpSelectionState& selection_state() {
  static WarpSelectionState state;
  return state;
}

const warp::World* selected_world(const WarpSelectionState& state) {
  const auto& worlds = warp::GetWorlds();
  if (state.worldIdx < 0 || state.worldIdx >= static_cast< int >(worlds.size())) {
    return nullptr;
  }
  return &worlds[state.worldIdx];
}

const warp::Area* selected_area(const WarpSelectionState& state) {
  const auto* world = selected_world(state);
  if (world == nullptr || state.areaIdx < 0 ||
      state.areaIdx >= static_cast< int >(world->areas.size())) {
    return nullptr;
  }
  return &world->areas[state.areaIdx];
}

void clamp_indices(WarpSelectionState& state) {
  const auto& worlds = warp::GetWorlds();
  if (worlds.empty()) {
    state.worldIdx = -1;
    state.areaIdx = -1;
    return;
  }
  state.worldIdx = std::clamp(state.worldIdx, 0, static_cast< int >(worlds.size()) - 1);
  const auto& areas = worlds[state.worldIdx].areas;
  state.areaIdx =
      areas.empty() ? -1 : std::clamp(state.areaIdx, 0, static_cast< int >(areas.size()) - 1);
}

void reset_layers(WarpSelectionState& state) {
  state.customLayers = false;
  state.layerBits = 0;
}

// Starts at the player's location the first time the window opens during a game.
void select_current_location(WarpSelectionState& state) {
  uint32_t mlvl;
  int areaId;
  if (state.initialized || !warp::GetCurrentLocation(mlvl, areaId)) {
    return;
  }
  const auto& worlds = warp::GetWorlds();
  for (int w = 0; w < static_cast< int >(worlds.size()); ++w) {
    if (worlds[w].mlvl != mlvl) {
      continue;
    }
    const auto& areas = worlds[w].areas;
    for (int a = 0; a < static_cast< int >(areas.size()); ++a) {
      if (areas[a].index == areaId) {
        state.worldIdx = w;
        state.areaIdx = a;
        state.initialized = true;
        reset_layers(state);
        return;
      }
    }
  }
}

uint64_t effective_layer_bits(const WarpSelectionState& state) {
  if (state.customLayers) {
    return state.layerBits;
  }
  const auto* world = selected_world(state);
  const auto* area = selected_area(state);
  return world != nullptr && area != nullptr ? warp::GetLayerBits(*world, *area) : 0;
}

Rml::String layers_label(const WarpSelectionState& state) {
  const auto* area = selected_area(state);
  if (area == nullptr || area->layers.empty()) {
    return "None";
  }
  if (!state.customLayers) {
    return "Current";
  }
  const uint64_t mask =
      area->layers.size() >= 64 ? ~uint64_t(0) : (uint64_t(1) << area->layers.size()) - 1;
  return fmt::format("Custom ({} of {})", std::popcount(state.layerBits & mask),
                     area->layers.size());
}

void populate_layer_picker(Pane& pane, WarpSelectionState& state) {
  pane.clear();
  clamp_indices(state);
  const auto* area = selected_area(state);
  if (area == nullptr) {
    return;
  }
  pane.add_button({
                      .text = "Keep Current Layers",
                      .isSelected = [&state] { return !state.customLayers; },
                  })
      .on_pressed([&state] {
        play_nav_sound(NavSound::ItemChange);
        reset_layers(state);
      });

  pane.add_section("Layers");
  for (int l = 0; l < static_cast< int >(area->layers.size()); ++l) {
    pane.add_button({
                        .text = area->layers[l].name,
                        .isSelected =
                            [l, &state] { return ((effective_layer_bits(state) >> l) & 1) != 0; },
                    })
        .on_pressed([l, &state] {
          if (!state.customLayers) {
            state.layerBits = effective_layer_bits(state);
            state.customLayers = true;
          }
          state.layerBits ^= uint64_t(1) << l;
          play_nav_sound(((state.layerBits >> l) & 1) != 0 ? NavSound::ItemEnable
                                                           : NavSound::ItemDisable);
        });
  }
}

} // namespace

WarpWindow::WarpWindow() {
  add_tab("Warp", [this](Rml::Element* content) {
    auto& leftPane = add_child< Pane >(content, Pane::Type::Controlled);
    auto& rightPane = add_child< Pane >(content, Pane::Type::Uncontrolled);
    auto& state = selection_state();

    if (warp::GetWorlds().empty()) {
      leftPane.add_text("The world list is available once the game data has loaded.");
      return;
    }
    select_current_location(state);
    clamp_indices(state);

    leftPane.add_section("Destination");
    leftPane.register_control(
        leftPane.add_select_button({
            .key = "World",
            .getValue =
                [&state] {
                  clamp_indices(state);
                  const auto* world = selected_world(state);
                  return world == nullptr ? Rml::String{"None"} : Rml::String{world->name};
                },
        }),
        rightPane, [&state](Pane& pane) {
          pane.clear();
          const auto& worlds = warp::GetWorlds();
          for (int i = 0; i < static_cast< int >(worlds.size()); ++i) {
            pane.add_button({
                                .text = worlds[i].name,
                                .isSelected = [i, &state] { return state.worldIdx == i; },
                            })
                .on_pressed([i, &state] {
                  play_nav_sound(NavSound::ItemChange);
                  if (state.worldIdx != i) {
                    state.worldIdx = i;
                    state.areaIdx = 0;
                    reset_layers(state);
                  }
                });
          }
        });

    leftPane.register_control(
        leftPane.add_select_button({
            .key = "Room",
            .getValue =
                [&state] {
                  clamp_indices(state);
                  const auto* area = selected_area(state);
                  return area == nullptr ? Rml::String{"None"} : Rml::String{area->name};
                },
        }),
        rightPane, [&state](Pane& pane) {
          pane.clear();
          clamp_indices(state);
          const auto* world = selected_world(state);
          if (world == nullptr) {
            return;
          }
          for (int i = 0; i < static_cast< int >(world->areas.size()); ++i) {
            pane.add_button({
                                .text = world->areas[i].name,
                                .isSelected = [i, &state] { return state.areaIdx == i; },
                            })
                .on_pressed([i, &state] {
                  play_nav_sound(NavSound::ItemChange);
                  if (state.areaIdx != i) {
                    state.areaIdx = i;
                    reset_layers(state);
                  }
                });
          }
        });

    leftPane.register_control(
        leftPane.add_select_button({
            .key = "Layers",
            .getValue =
                [&state] {
                  clamp_indices(state);
                  return layers_label(state);
                },
            .isDisabled =
                [&state] {
                  clamp_indices(state);
                  const auto* area = selected_area(state);
                  return area == nullptr || area->layers.empty();
                },
            .isModified = [&state] { return state.customLayers; },
        }),
        rightPane, [&state](Pane& pane) { populate_layer_picker(pane, state); });

    leftPane.add_section("Action");
    leftPane.register_control(
        leftPane
            .add_button({
                .text = "Warp",
                .isDisabled =
                    [&state] {
                      clamp_indices(state);
                      return selected_area(state) == nullptr || !warp::CanWarp();
                    },
            })
            .on_pressed([&state] {
              clamp_indices(state);
              const auto* world = selected_world(state);
              const auto* area = selected_area(state);
              if (world == nullptr || area == nullptr || !warp::CanWarp()) {
                play_nav_sound(NavSound::Warning);
                return;
              }
              play_nav_sound(NavSound::Click);
              warp::RequestWarp(world->mlvl, area->index,
                                state.customLayers ? std::optional< uint64_t >(state.layerBits)
                                                   : std::nullopt);
              // Start from the new location next time the window opens.
              state.initialized = false;
              close_all_documents();
            }),
        rightPane, [](Pane& pane) {
          pane.clear();
          if (warp::CanWarp()) {
            pane.add_text("Reloads the game in the selected room, keeping your items and "
                          "progress. Custom layers replace the room's layer state.");
          } else {
            pane.add_text("Start or load a game to warp.");
          }
        });
  });
}

} // namespace metaforce::ui
