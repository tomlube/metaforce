#include "Metaforce/UI/MenuBar.hpp"

#include "Metaforce/Runtime.hpp"
#include "Metaforce/SaveAnywhere.hpp"
#include "Metaforce/UI/CheatsWindow.hpp"
#include "Metaforce/UI/RandomizerWindow.hpp"
#include "Metaforce/UI/SettingsWindow.hpp"
#include "Metaforce/UI/WarpWindow.hpp"

#include <borealis/ui/modal.hpp>
#include <borealis/version.h>

#include <RmlUi/Core.h>
#include <fmt/chrono.h>
#include <fmt/format.h>

#include <chrono>
#include <filesystem>
#include <optional>
#include <regex>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace metaforce::ui {
using namespace borealis::ui;
namespace {

// The version from Git: v0.4.0 is tagged, and each commit after it counts up the patch number,
// so "v0.4.0-3-dirty" from git describe shows as "v0.4.3-dirty".
std::string VersionText() {
  const std::string describe = BOREALIS_APP_DESCRIBE;
  std::smatch match;
  if (!std::regex_match(describe, match,
                        std::regex(R"(v(\d+)\.(\d+)\.(\d+)(?:-(\d+))?(-dirty)?)"))) {
    return describe;
  }
  const int patch = std::stoi(match[3]) + (match[4].matched ? std::stoi(match[4]) : 0);
  return fmt::format("v{}.{}.{}{}", match[1].str(), match[2].str(), patch, match[5].str());
}

std::optional< std::filesystem::path > ExecutablePath() {
#if defined(_WIN32)
  wchar_t buffer[MAX_PATH];
  const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
  if (length == 0 || length == MAX_PATH) {
    return std::nullopt;
  }
  return std::filesystem::path(buffer);
#elif defined(__APPLE__)
  char buffer[4096];
  uint32_t size = sizeof(buffer);
  if (_NSGetExecutablePath(buffer, &size) != 0) {
    return std::nullopt;
  }
  return std::filesystem::path(buffer);
#elif defined(__linux__)
  std::error_code ec;
  auto path = std::filesystem::read_symlink("/proc/self/exe", ec);
  return ec ? std::nullopt : std::optional(path);
#else
  return std::nullopt;
#endif
}

// When the running executable was built, which tells a fresh build from a stale one even between
// commits.
std::string BuildTimeText() {
  const auto exe = ExecutablePath();
  std::error_code ec;
  if (!exe) {
    return {};
  }
  const auto fileTime = std::filesystem::last_write_time(*exe, ec);
  if (ec) {
    return {};
  }
  const auto systemTime = std::chrono::time_point_cast< std::chrono::system_clock::duration >(
      fileTime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
  const std::time_t time = std::chrono::system_clock::to_time_t(systemTime);
  std::tm local{};
#if defined(_WIN32)
  localtime_s(&local, &time);
#else
  localtime_r(&time, &local);
#endif
  return fmt::format("{:%b %d %H:%M}", local);
}

} // namespace

MenuBar::MenuBar() {
  build_tabs();
  // The version, at the right end of the bar.
  const std::string buildTime = BuildTimeText();
  const std::string text =
      buildTime.empty() ? VersionText() : fmt::format("{} · {}", VersionText(), buildTime);
  Rml::ElementPtr label = mRoot->GetOwnerDocument()->CreateElement("div");
  label->SetAttribute("style", "flex: 0 0 auto; align-self: center; padding: 0 16dp; "
                               "font-size: 14dp; font-weight: normal; opacity: 0.5;");
  label->SetInnerRML(Rml::StringUtilities::EncodeRml(text));
  mRoot->AppendChild(std::move(label));
}

void MenuBar::build_tabs() {
  mTabBar->add_tab("Settings", [this] { push(std::make_unique< SettingsWindow >()); });
  mTabBar->add_tab("Warp", [this] { push(std::make_unique< WarpWindow >()); });
  mTabBar->add_tab("Cheats", [this] { push(std::make_unique< CheatsWindow >()); });
  mTabBar->add_tab("Save", [this] {
    mTabBar->set_active_tab(-1);
    const std::string reason = save_anywhere::WhyCantSave();
    if (reason.empty()) {
      // The save screen takes the controller, so get out of its way.
      hide(false);
      save_anywhere::RequestSave();
      return;
    }
    play_nav_sound(NavSound::Warning);
    const auto dismiss = [](Modal& modal) { modal.pop(); };
    push(std::make_unique< Modal >(Modal::Props{
        .title = "Can't Save Now",
        .bodyText = reason,
        .actions =
            {
                ModalAction{
                    .label = "OK",
                    .onPressed =
                        [dismiss](Modal& modal) {
                          play_nav_sound(NavSound::WindowClose);
                          dismiss(modal);
                        },
                },
            },
        .onDismiss = dismiss,
        .icon = "warning",
    }));
  });
  mTabBar->add_tab("Randomizer", [this] { push(std::make_unique< RandomizerWindow >()); });
  mTabBar->add_tab("Quit", [this] {
    mTabBar->set_active_tab(-1);
    const auto dismiss = [](Modal& modal) { modal.pop(); };
    push(std::make_unique< Modal >(Modal::Props{
        .title = "Quit Metaforce",
        .bodyText = "Unsaved progress will be lost.",
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
                    .label = "Quit",
                    .onPressed =
                        [dismiss](Modal& modal) {
                          play_nav_sound(NavSound::Click);
                          dismiss(modal);
                          RequestQuit();
                        },
                },
            },
        .onDismiss = dismiss,
        .icon = "question-mark",
    }));
  });
}

} // namespace metaforce::ui
