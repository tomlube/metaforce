#pragma once

#include "Metaforce/Randomizer/Logic.hpp"
#include "Metaforce/Randomizer/Settings.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace metaforce::randomizer {

// Share codes, after Dusklight's permalinks: every setting that changes the generated seed, the
// seed string included, packed into bits and written out as "PRIME-" plus base64url.
inline constexpr std::string_view kShareCodePrefix = "PRIME-";

std::string EncodeShareCode(const Database& db, const PickupDatabase& pickups,
                            const Settings& settings);

struct DecodedShareCode {
  Settings settings;
  // False when the settings that were shared couldn't all be written to the code (a value out of
  // the menu's range, an unknown starting location), so the same seed string makes a different seed.
  bool exact = true;
};

// The settings in `code`, or nullopt with the reason in `error`.
std::optional< DecodedShareCode > DecodeShareCode(const Database& db, const PickupDatabase& pickups,
                                                  std::string_view code, std::string& error);

// Whether `text` starts like a share code, for catching one pasted into the seed string.
bool LooksLikeShareCode(std::string_view text);

} // namespace metaforce::randomizer
