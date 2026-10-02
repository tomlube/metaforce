#pragma once

#include "Metaforce/Randomizer/Logic.hpp"
#include "Metaforce/Randomizer/Seed.hpp"
#include "Metaforce/Randomizer/Settings.hpp"

#include <atomic>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace metaforce::randomizer {

struct GeneratorInput {
  const Database& db;
  const PickupDatabase& pickups;
  Settings settings;
};

using ProgressCallback = std::function< void(float progress, std::string_view status) >;

// Generates a seed with assumed fill over the logic database, then verifies it with a
// playthrough from the starting location. Thread-safe as long as the inputs aren't mutated.
std::optional< Seed > Generate(const GeneratorInput& input, std::string& error,
                               const ProgressCallback& progress = {},
                               const std::atomic< bool >* cancel = nullptr);

// Number of pickups the settings put in the pool, before padding to the location count.
int CountPoolSize(const PickupDatabase& pickups, const Settings& settings);

// Valid starting locations, as "Region|Area|Node" keys paired with display names.
std::vector< std::pair< std::string, std::string > > StartingLocations(const Database& db);

// Random seed string for new seeds.
std::string RandomSeedString();

} // namespace metaforce::randomizer
