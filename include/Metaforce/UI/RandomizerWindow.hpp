#pragma once

#include "Metaforce/Randomizer/Randomizer.hpp"

#include <borealis/ui/list.hpp>
#include <borealis/ui/window.hpp>

#include <set>
#include <string>
#include <vector>

namespace borealis::ui {
class Pane;
} // namespace borealis::ui

namespace metaforce::ui {

class RandomizerWindow : public borealis::ui::Window {
public:
  RandomizerWindow();

  void update() override;

private:
  void reset_tab_elements();
  void build_seeds_tab(Rml::Element* content);
  void build_logic_tab(Rml::Element* content);
  void build_pool_tab(Rml::Element* content);
  void build_rooms_tab(Rml::Element* content);
  void build_doors_tab(Rml::Element* content);
  void build_start_regions(const randomizer::Database& db);
  std::vector< borealis::ui::List::Item > start_list_items() const;
  void on_start_pressed(uint64_t key);
  bool is_start_selected(uint64_t key) const;
  void add_seed_list(borealis::ui::Pane& pane);
  void refresh_seeds();
  void show_seed_actions(const std::string& hash);
  void add_backup_list(borealis::ui::Pane& pane);
  void show_backup_actions(const randomizer::AutosaveBackup& backup);
  void show_message(const Rml::String& title, const Rml::String& body, bool danger = false);

  Rml::Element* mStatusText = nullptr;
  Rml::Element* mArmedText = nullptr;
  Rml::Element* mPoolText = nullptr;
  borealis::ui::List* mSeedList = nullptr;

  struct StartRoom {
    std::string key;
    std::string area;
    std::string label;
  };
  struct StartRegion {
    std::string name;
    std::vector< StartRoom > rooms;
  };
  std::vector< StartRegion > mStartRegions;
  std::set< size_t > mExpandedRegions;
  std::string mDefaultStart;
  borealis::ui::List* mStartList = nullptr;
  bool mStartListDirty = false;
  std::vector< randomizer::SeedSummary > mSeeds;
  std::vector< randomizer::AutosaveBackup > mBackups;
  randomizer::GenerationState mShownState = randomizer::GenerationState::Idle;
  float mShownProgress = -1.f;
  int mShownPoolSize = -1;
  std::string mShownArmed;
};

} // namespace metaforce::ui
