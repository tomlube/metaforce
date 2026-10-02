#pragma once

#include <borealis/ui/window.hpp>

namespace metaforce::ui {

// Inventory editor and cheat toggles for the game in progress.
class CheatsWindow : public borealis::ui::Window {
public:
  CheatsWindow();

private:
  void build_inventory_tab(Rml::Element* content);
  void build_cheats_tab(Rml::Element* content);
};

} // namespace metaforce::ui
