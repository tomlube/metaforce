#pragma once

#include <borealis/version.h>

#include <fmt/chrono.h>
#include <fmt/format.h>

#include <chrono>
#include <ctime>
#include <filesystem>
#include <optional>
#include <regex>
#include <string>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace metaforce {

// The version from Git: v0.4.0 is tagged, and each commit after it counts up the patch number,
// so "v0.4.0-3-dirty" from git describe shows as "v0.4.3-dirty".
inline std::string VersionText() {
  const std::string describe = BOREALIS_APP_DESCRIBE;
  std::smatch match;
  if (!std::regex_match(describe, match,
                        std::regex(R"(v(\d+)\.(\d+)\.(\d+)(?:-(\d+))?(-dirty)?)"))) {
    return describe;
  }
  const int patch = std::stoi(match[3]) + (match[4].matched ? std::stoi(match[4]) : 0);
  return fmt::format("v{}.{}.{}{}", match[1].str(), match[2].str(), patch, match[5].str());
}

inline std::optional< std::filesystem::path > ExecutablePath() {
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
inline std::string BuildTimeText() {
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

// The version and build time, like "v0.4.1 · Oct 02 21:36".
inline std::string VersionAndBuildTimeText() {
  const std::string buildTime = BuildTimeText();
  return buildTime.empty() ? VersionText() : fmt::format("{} · {}", VersionText(), buildTime);
}

} // namespace metaforce
