#include "Metaforce/UI/CheatsWindow.hpp"

#include "Metaforce/Cheats.hpp"

#include "MetroidPrime/CStateManager.hpp"
#include "MetroidPrime/Player/CPlayerState.hpp"

#include <borealis/ui/bool_button.hpp>
#include <borealis/ui/number_button.hpp>
#include <borealis/ui/pane.hpp>
#include <borealis/ui/ui.hpp>

#include <algorithm>
#include <vector>

namespace metaforce::ui {
using namespace borealis::ui;

namespace {

using EItemType = CPlayerState::EItemType;

struct Item {
  const char* name;
  EItemType type;
};

struct Ammo {
  const char* name;
  EItemType type;
  int max;  // capacity limit, as in CPlayerState's kPowerUpMax
  int step; // capacity per expansion
};

struct Group {
  const char* name;
  std::vector< Item > items;
  std::vector< Ammo > ammo;
};

const std::vector< Group >& Groups() {
  static const std::vector< Group > groups = {
      {"Beams",
       {{"Power Beam", CPlayerState::kIT_PowerBeam},
        {"Ice Beam", CPlayerState::kIT_IceBeam},
        {"Wave Beam", CPlayerState::kIT_WaveBeam},
        {"Plasma Beam", CPlayerState::kIT_PlasmaBeam},
        {"Charge Beam", CPlayerState::kIT_ChargeBeam}},
       {}},
      {"Charge Combos",
       {{"Super Missile", CPlayerState::kIT_SuperMissile},
        {"Wavebuster", CPlayerState::kIT_Wavebuster},
        {"Ice Spreader", CPlayerState::kIT_IceSpreader},
        {"Flamethrower", CPlayerState::kIT_Flamethrower}},
       {}},
      {"Visors",
       {{"Combat Visor", CPlayerState::kIT_CombatVisor},
        {"Scan Visor", CPlayerState::kIT_ScanVisor},
        {"Thermal Visor", CPlayerState::kIT_ThermalVisor},
        {"X-Ray Visor", CPlayerState::kIT_XRayVisor}},
       {}},
      {"Suits",
       {{"Power Suit", CPlayerState::kIT_PowerSuit},
        {"Varia Suit", CPlayerState::kIT_VariaSuit},
        {"Gravity Suit", CPlayerState::kIT_GravitySuit},
        {"Phazon Suit", CPlayerState::kIT_PhazonSuit}},
       {}},
      {"Morph Ball",
       {{"Morph Ball", CPlayerState::kIT_MorphBall},
        {"Morph Ball Bombs", CPlayerState::kIT_MorphBallBombs},
        {"Boost Ball", CPlayerState::kIT_BoostBall},
        {"Spider Ball", CPlayerState::kIT_SpiderBall}},
       {{"Power Bombs", CPlayerState::kIT_PowerBombs, 8, 1}}},
      {"Movement",
       {{"Space Jump Boots", CPlayerState::kIT_SpaceJumpBoots},
        {"Grapple Beam", CPlayerState::kIT_GrappleBeam}},
       {}},
      {"Ammo and Energy",
       {},
       {{"Missiles", CPlayerState::kIT_Missiles, 250, 5},
        {"Power Bombs", CPlayerState::kIT_PowerBombs, 8, 1},
        {"Energy Tanks", CPlayerState::kIT_EnergyTanks, 14, 1}}},
  };
  return groups;
}

// The player state of the game in progress, or null outside a game.
CPlayerState* GetPlayerState() {
  if (gpStateManager == nullptr || !gpStateManager->IsFullyInitialized()) {
    return nullptr;
  }
  return gpStateManager->PlayerState();
}

bool NoGame() { return GetPlayerState() == nullptr; }

// Keeps energy within what the energy tanks allow after they change.
void ClampHealth(CPlayerState& state) {
  CHealthInfo& health = *state.HealthInfo();
  health.SetHP(std::min(health.GetHP(), state.CalculateHealth()));
}

void SetItem(EItemType type, bool owned) {
  if (CPlayerState* state = GetPlayerState()) {
    state->SetPowerUp(type, owned ? 1 : 0);
    state->SetPickup(type, owned ? 1 : 0);
  }
}

void SetCapacity(const Ammo& ammo, int capacity) {
  CPlayerState* state = GetPlayerState();
  if (state == nullptr) {
    return;
  }
  // Energy tanks are always full; ammo keeps what it had, up to the new capacity.
  const bool fill = ammo.type == CPlayerState::kIT_EnergyTanks;
  const int amount = fill ? capacity : std::min(state->GetItemAmount(ammo.type), capacity);
  state->SetPowerUp(ammo.type, capacity);
  state->SetPickup(ammo.type, amount);
  if (fill) {
    ClampHealth(*state);
  }
}

void GiveEverything() {
  CPlayerState* state = GetPlayerState();
  if (state == nullptr) {
    return;
  }
  for (const Group& group : Groups()) {
    for (const Item& item : group.items) {
      SetItem(item.type, true);
    }
    for (const Ammo& ammo : group.ammo) {
      state->SetPowerUp(ammo.type, ammo.max);
      state->SetPickup(ammo.type, ammo.max);
    }
  }
  state->HealthInfo()->SetHP(state->CalculateHealth());
}

void Refill() {
  CPlayerState* state = GetPlayerState();
  if (state == nullptr) {
    return;
  }
  for (const EItemType type : {CPlayerState::kIT_Missiles, CPlayerState::kIT_PowerBombs}) {
    state->SetPickup(type, state->GetItemCapacity(type));
  }
  state->HealthInfo()->SetHP(state->CalculateHealth());
}

void SetHelp(Pane& leftPane, Pane& rightPane, Component& control, Rml::String help) {
  leftPane.register_control(control, rightPane, [help = std::move(help)](Pane& pane) {
    pane.clear();
    pane.add_rml(help);
  });
}

void PopulateGroup(Pane& pane, const Group& group) {
  pane.clear();
  if (NoGame()) {
    pane.add_text("Start or load a game to edit the inventory.");
    return;
  }
  if (!group.items.empty()) {
    pane.add_section(group.name);
  }
  for (const Item& item : group.items) {
    const EItemType type = item.type;
    pane.add_child< BoolButton >(BoolButton::Props{
        .key = item.name,
        .getValue =
            [type] {
              const CPlayerState* state = GetPlayerState();
              return state != nullptr && state->HasPowerUp(type);
            },
        .setValue =
            [type](bool v) {
              SetItem(type, v);
              play_nav_sound(v ? NavSound::ItemEnable : NavSound::ItemDisable);
            },
        .isDisabled = NoGame,
    });
  }
  for (const Ammo& ammo : group.ammo) {
    pane.add_section(ammo.name);
    const bool tanks = ammo.type == CPlayerState::kIT_EnergyTanks;
    pane.add_child< NumberButton >(NumberButton::Props{
        .key = tanks ? "Tanks" : "Capacity",
        .getValue =
            [ammo] {
              const CPlayerState* state = GetPlayerState();
              return state != nullptr ? state->GetItemCapacity(ammo.type) : 0;
            },
        .setValue = [ammo](int v) { SetCapacity(ammo, std::clamp(v, 0, ammo.max)); },
        .isDisabled = NoGame,
        .min = 0,
        .max = ammo.max,
        .step = ammo.step,
    });
    if (tanks) {
      continue;
    }
    pane.add_child< NumberButton >(NumberButton::Props{
        .key = "Amount",
        .getValue =
            [ammo] {
              const CPlayerState* state = GetPlayerState();
              return state != nullptr ? state->GetItemAmount(ammo.type) : 0;
            },
        .setValue =
            [ammo](int v) {
              if (CPlayerState* state = GetPlayerState()) {
                state->SetPickup(ammo.type,
                                 std::clamp(v, 0, state->GetItemCapacity(ammo.type)));
              }
            },
        .isDisabled =
            [ammo] {
              const CPlayerState* state = GetPlayerState();
              return state == nullptr || state->GetItemCapacity(ammo.type) == 0;
            },
        .min = 0,
        .max = ammo.max,
        .step = ammo.step,
    });
  }
}

} // namespace

void CheatsWindow::build_inventory_tab(Rml::Element* content) {
  auto& leftPane = add_child< Pane >(content, Pane::Type::Controlled);
  auto& rightPane = add_child< Pane >(content, Pane::Type::Uncontrolled);

  leftPane.add_section("Items");
  for (const Group& group : Groups()) {
    auto& button = leftPane.add_group_button({.text = group.name});
    leftPane.register_control(button, rightPane,
                              [&group](Pane& pane) { PopulateGroup(pane, group); });
  }

  leftPane.add_section("Quick Actions");
  auto& give = leftPane.add_button({.text = "Give Everything", .isDisabled = NoGame})
                   .on_pressed([] {
                     play_nav_sound(NavSound::Click);
                     GiveEverything();
                   });
  SetHelp(leftPane, rightPane, give,
          "Gives every item, and full Missiles, Power Bombs and Energy Tanks.");
  auto& refill = leftPane.add_button({.text = "Refill", .isDisabled = NoGame}).on_pressed([] {
    play_nav_sound(NavSound::Click);
    Refill();
  });
  SetHelp(leftPane, rightPane, refill, "Refills energy, Missiles and Power Bombs.");
}

namespace {

void AddToggle(Pane& leftPane, Pane& rightPane, Rml::String key, Rml::String help,
               bool cheats::Toggles::*field) {
  auto& button = leftPane.add_child< BoolButton >(BoolButton::Props{
      .key = std::move(key),
      .getValue = [field] { return cheats::GetToggles().*field; },
      .setValue =
          [field](bool v) {
            cheats::GetToggles().*field = v;
            play_nav_sound(v ? NavSound::ItemEnable : NavSound::ItemDisable);
          },
      .isModified = [field] { return cheats::GetToggles().*field; },
  });
  SetHelp(leftPane, rightPane, button, std::move(help));
}

} // namespace

void CheatsWindow::build_cheats_tab(Rml::Element* content) {
  auto& leftPane = add_child< Pane >(content, Pane::Type::Controlled);
  auto& rightPane = add_child< Pane >(content, Pane::Type::Uncontrolled);

  leftPane.add_section("Cheats");
  AddToggle(leftPane, rightPane, "Infinite Health",
            "Samus takes no damage, and her energy stays full.<br/><br/>Hazards that kill "
            "outright without dealing damage still can.",
            &cheats::Toggles::infiniteHealth);
  AddToggle(leftPane, rightPane, "Infinite Missiles",
            "Missiles refill as they're used. Needs the Missile Launcher (any Missile "
            "capacity).",
            &cheats::Toggles::infiniteMissiles);
  AddToggle(leftPane, rightPane, "Infinite Power Bombs",
            "Power Bombs refill as they're used. Needs Power Bombs (any Power Bomb capacity).",
            &cheats::Toggles::infinitePowerBombs);
  AddToggle(leftPane, rightPane, "One Hit Kill",
            "Anything Samus's weapons can hurt dies to a single hit, bosses included. What a "
            "weapon can't hurt still shrugs it off, and boss phases still play out.",
            &cheats::Toggles::oneHitKill);
  AddToggle(leftPane, rightPane, "Moon Jump",
            "Hold jump in the air to keep rising.", &cheats::Toggles::moonJump);
  AddToggle(leftPane, rightPane, "Unlocked Dash",
            "Restores the kiosk demo's dash. Holding L and jumping sideways dashes even when "
            "nothing is locked on, arcing around the point L holds in front of you.<br/><br/>The "
            "retail game only dashes while locked onto a target.",
            &cheats::Toggles::unlockedDash);
  leftPane.add_rml("Cheats stay on until Metaforce closes.");
}

CheatsWindow::CheatsWindow() {
  add_tab("Inventory", [this](Rml::Element* content) { build_inventory_tab(content); });
  add_tab("Cheats", [this](Rml::Element* content) { build_cheats_tab(content); });
}

} // namespace metaforce::ui
