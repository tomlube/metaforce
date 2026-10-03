#pragma once

#include "Metaforce/Randomizer/Logic.hpp"
#include "Metaforce/Randomizer/Seed.hpp"
#include "Metaforce/Randomizer/Settings.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace metaforce::randomizer {

// Session state shared by the UI and the game hooks. Main thread only, apart from seed
// generation which runs on a worker thread.

void Initialize(const std::filesystem::path& userPath);
void Shutdown();

// Loads the logic and pickup databases on first use. Null on failure, with the reason in
// LoadError().
const Database* GetDatabase();
const PickupDatabase* GetPickupDatabase();
const std::string& LoadError();

Settings& GetSettings();
void SaveSettings();
void ResetSettings();

struct SeedSummary {
  std::string hash;
  std::string seedString;
  std::string startName;
  std::filesystem::file_time_type modified;
  bool randomStart = false;
};

std::vector< SeedSummary > ListSeeds();
std::optional< Seed > LoadSeed(const std::string& hash, std::string& error);
bool DeleteSeed(const std::string& hash);
std::filesystem::path SeedDirectory(const std::string& hash);
// Writes spoiler.txt next to the seed and returns its path.
std::optional< std::filesystem::path > WriteSpoiler(const std::string& hash, std::string& error);

// The seed new games are started with. Empty when new games are vanilla.
const std::string& GetArmedSeed();
void SetArmedSeed(const std::string& hash);

// The seed of the game being played, if any.
const Seed* GetActiveSeed();

// Records that the player went through a door (by region and editor id) in the game being played.
// Kept per save slot from the moment it happens, whether or not the game is saved after.
void MarkDoorTraversed(unsigned int world, unsigned int editorId);

// Whether R + Z + D-pad Left reloads the last save in a randomized game. On by default.
bool GetQuickReload();
void SetQuickReload(bool enabled);

enum class GenerationState {
  Idle,
  Running,
  Succeeded,
  Failed,
};

struct GenerationStatus {
  GenerationState state = GenerationState::Idle;
  float progress = 0.f;
  std::string message;
  std::string hash;
};

bool StartGeneration();
void CancelGeneration();
GenerationStatus GetGenerationStatus();
// Clears a finished generation result.
void AcknowledgeGeneration();

// Display name for a pickup index, "Region / Area / Node".
std::string LocationName(int pickupIndex);

} // namespace metaforce::randomizer
