#include "Metaforce/UI/UI.hpp"

#include "Metaforce/UI/MenuBar.hpp"
#include "Metaforce/UI/RuntimeConfig.hpp"

#include "Kyoto/Audio/CSfxManager.hpp"
#include "MetroidPrime/SFX/UI.h"

#include <borealis/log.hpp>
#include <borealis/ui/input.hpp>
#include <borealis/ui/ui.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <type_traits>

namespace metaforce::ui {
namespace {
using borealis::ui::NavSound;

constexpr ushort kNoSfx = 0xFFFF;

bool sInitialized = false;

ushort nav_sound_sfx(NavSound sound) {
  switch (sound) {
  // TODO: find values that don't suck
  // case NavSound::Click:
  // case NavSound::BindingChanged:
  // case NavSound::WindowOpen:
  //   return SFXui_x_invchoos_00;
  // case NavSound::Play:
  // case NavSound::ItemEnable:
  //   return SFXui_x_quitaff_00;
  // case NavSound::ItemDisable:
  //   return SFXui_x_quitneg_00;
  // case NavSound::MenuOpen:
  //   return SFXui_x_invon_00;
  // case NavSound::MenuClose:
  //   return SFXui_x_invoff_00;
  // case NavSound::WindowClose:
  //   return SFXui_x_invback_00;
  // case NavSound::TabChanged:
  //   return SFXui_x_invflip_00;
  case NavSound::ItemFocus:
    return SFXui_x_invsel_00;
  // case NavSound::ItemChange:
  //   return SFXui_x_quitsel_00;
  // case NavSound::Warning:
  //   return SFXui_x_warning_00;
  default:
    return kNoSfx;
  }
}

void play_nav_sound(NavSound sound) {
  if (!GetRuntimeConfig().ui.sounds) {
    return;
  }
  if (const ushort sfx = nav_sound_sfx(sound); sfx != kNoSfx) {
    CSfxManager::SfxStart(sfx, 0x7f, 0x40, false, CSfxManager::kMedPriority, false,
                          CSfxManager::kAllAreas);
  }
}

void load_fonts() {
  borealis::ui::load_font("TitilliumWeb-Regular.ttf", true);
  borealis::ui::load_font("TitilliumWeb-SemiBold.ttf");
  borealis::ui::load_font("TitilliumWeb-Bold.ttf");
  borealis::ui::load_font("SairaSemiCondensed-Regular.ttf");
  borealis::ui::load_font("SairaSemiCondensed-SemiBold.ttf");
  borealis::ui::load_font("SairaSemiCondensed-Bold.ttf");
  borealis::ui::load_font("MaterialSymbolsRounded-Regular.ttf");
  borealis::ui::load_font("NotoMono-Regular.ttf");
}

} // namespace

RuntimeConfig& GetRuntimeConfig() {
  static RuntimeConfig sConfig;
  return sConfig;
}

namespace {
using json = nlohmann::json;

constexpr borealis::Log ConfigLog{"settings"};

std::filesystem::path sConfigFile;

// Leaves the default in place when the key is missing or holds the wrong type.
template < typename T >
void read_var(const json& section, const char* key, RuntimeVar< T >& var) {
  const auto it = section.find(key);
  if (it == section.end()) {
    return;
  }
  if constexpr (std::is_same_v< T, bool >) {
    if (it->is_boolean()) {
      var.setValue(it->get< bool >());
    }
  } else if constexpr (std::is_same_v< T, int >) {
    if (it->is_number_integer()) {
      var.setValue(it->get< int >());
    }
  }
}

void read_int(const json& section, const char* key, RuntimeVar< int >& var, int min, int max) {
  read_var(section, key, var);
  var.setValue(std::clamp(var.getValue(), min, max));
}

const json& section_of(const json& root, const char* name) {
  static const json empty = json::object();
  const auto it = root.find(name);
  return it != root.end() && it->is_object() ? *it : empty;
}
} // namespace

void LoadRuntimeConfig(const std::filesystem::path& userPath) {
  sConfigFile = userPath / "settings.json";
  std::ifstream file(sConfigFile, std::ios::binary);
  if (!file) {
    gRuntimeConfigDirty = false;
    return;
  }
  const json root = json::parse(file, nullptr, false);
  if (!root.is_object()) {
    ConfigLog.warn("Ignoring malformed {}", sConfigFile.string());
    gRuntimeConfigDirty = false;
    return;
  }

  RuntimeConfig& config = GetRuntimeConfig();
  const json& video = section_of(root, "video");
  read_var(video, "fullscreen", config.video.fullscreen);
  read_var(video, "lockAspectRatio", config.video.lockAspectRatio);

  const json& input = section_of(root, "input");
  read_var(input, "allowBackgroundInput", config.input.allowBackgroundInput);
  read_var(input, "smartLockOn", config.input.smartLockOn);
  read_var(input, "modernControls", config.input.modernControls);
  read_var(input, "squareDiagonalLook", config.input.squareDiagonalLook);
  read_var(input, "mouseLook", config.input.mouseLook);
  read_int(input, "mouseSensitivity", config.input.mouseSensitivity, 5, 1000);
  read_var(input, "invertMouseY", config.input.invertMouseY);
  read_var(input, "uncappedMouseTurnUnderR", config.input.uncappedMouseTurnUnderR);
  read_var(input, "aimAssist", config.input.aimAssist);
  read_var(input, "unlockedDash", config.input.unlockedDash);

  const json& game = section_of(root, "game");
  read_int(game, "cutsceneSkips", config.game.cutsceneSkips, 0, 2);

  const json& ui = section_of(root, "interface");
  read_int(ui, "scale", config.ui.scale, 50, 200);
  read_var(ui, "sounds", config.ui.sounds);

  gRuntimeConfigDirty = false;
}

void SaveRuntimeConfig() {
  if (!gRuntimeConfigDirty || sConfigFile.empty()) {
    return;
  }
  gRuntimeConfigDirty = false;

  const RuntimeConfig& config = GetRuntimeConfig();
  const json root{
      {"video",
       {
           {"fullscreen", config.video.fullscreen.getValue()},
           {"lockAspectRatio", config.video.lockAspectRatio.getValue()},
       }},
      {"input",
       {
           {"allowBackgroundInput", config.input.allowBackgroundInput.getValue()},
           {"smartLockOn", config.input.smartLockOn.getValue()},
           {"modernControls", config.input.modernControls.getValue()},
           {"squareDiagonalLook", config.input.squareDiagonalLook.getValue()},
           {"mouseLook", config.input.mouseLook.getValue()},
           {"mouseSensitivity", config.input.mouseSensitivity.getValue()},
           {"invertMouseY", config.input.invertMouseY.getValue()},
           {"uncappedMouseTurnUnderR", config.input.uncappedMouseTurnUnderR.getValue()},
           {"aimAssist", config.input.aimAssist.getValue()},
           {"unlockedDash", config.input.unlockedDash.getValue()},
       }},
      {"game",
       {
           {"cutsceneSkips", config.game.cutsceneSkips.getValue()},
       }},
      {"interface",
       {
           {"scale", config.ui.scale.getValue()},
           {"sounds", config.ui.sounds.getValue()},
       }},
  };
  std::error_code ec;
  std::filesystem::create_directories(sConfigFile.parent_path(), ec);
  std::ofstream file(sConfigFile, std::ios::binary | std::ios::trunc);
  file << root.dump(2);
  if (!file) {
    ConfigLog.warn("Could not save {}", sConfigFile.string());
  }
}

bool Initialize() {
  if (sInitialized) {
    return true;
  }
  if (!borealis::ui::initialize()) {
    return false;
  }
  load_fonts();
  // R + Start toggles the menu from a controller.
  borealis::ui::input::Settings inputSettings = borealis::ui::input::settings();
  inputSettings.menuChord = true;
  borealis::ui::input::apply_settings(inputSettings);
  borealis::ui::set_nav_sound_handler(&play_nav_sound);
  borealis::ui::set_user_scale(GetRuntimeConfig().ui.scale);
  borealis::ui::push_document(std::make_unique< MenuBar >(), false);
  sInitialized = true;
  return true;
}

void Shutdown() {
  if (!sInitialized) {
    return;
  }
  borealis::ui::shutdown();
  sInitialized = false;
}

void HandleEvent(const SDL_Event& event) {
  if (sInitialized) {
    borealis::ui::handle_event(event);
  }
}

void Update() {
  if (sInitialized) {
    borealis::ui::update();
  }
}

} // namespace metaforce::ui
