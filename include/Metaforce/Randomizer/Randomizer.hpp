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
// A dock, by its area's MREA, the player went through, from either side.
void MarkDockTraversed(unsigned int areaAssetId, int dock);
// The player went into the room `areaAssetId` (MREA) through its dock `dock`, morphed or not and
// facing `yaw` (radians about Z, 0 facing +Y). Autosaves made in the room load them there.
void MarkRoomEntered(unsigned int areaAssetId, int dock, bool morphed, float yaw);

// Whether R + Z + D-pad Left reloads the last save in a randomized game. On by default.
bool GetQuickReload();
void SetQuickReload(bool enabled);

// Whether R + Z + D-pad Right opens the save screen in a randomized game. On by default.
bool GetQuickSave();
void SetQuickSave(bool enabled);

// Room Rando Map: how the map is drawn when the room randomizer moved doors. Vanilla draws every
// room where it is in the game; Tidy lays the map out so rooms meet at their doors, putting rooms
// that fit by none of their doors off on their own; Connected joins every room by one of its
// doors, overlaps and all. Connected by default.
inline constexpr const char* kMapLayoutNames[] = {"Vanilla", "Tidy", "Connected"};
inline constexpr int kMapLayoutVanilla = 0;
inline constexpr int kMapLayoutTidy = 1;
inline constexpr int kMapLayoutConnected = 2;
int GetMapLayout();
void SetMapLayout(int mode);

// Map Draw Distance: in a randomized game, the map screen draws at most this many rooms around the
// selected room, out to the farthest ring of doors that fits, faded. Mixed regions put every room of
// the game on one map, which is way too much to draw at once on most PCs. 0 draws them all. 30 by
// default.
inline constexpr int kMapDrawDistances[] = {15, 30, 50, 80, 0};
inline constexpr const char* kMapDrawDistanceNames[] = {"15 Rooms", "30 Rooms", "50 Rooms",
                                                        "80 Rooms", "Whole World"};
inline constexpr int kMapDrawDistanceDefault = 30;
int GetMapDrawDistance();
void SetMapDrawDistance(int rooms);

// Autosave: in a randomized game, saves to the game's slot after a pickup, on the first frame
// the game could be saved from the save screen, Samus is on the ground and she hasn't lost energy
// for a moment. Major Only saves after upgrades, Energy Tanks and artifacts; All Pickups after
// expansions too. Auto is Major Only for seeds with the room randomizer and Off for the rest.
// Auto by default.
inline constexpr const char* kAutosaveNames[] = {"Auto", "Off", "Major Only", "All Pickups"};
inline constexpr int kAutosaveAuto = 0;
inline constexpr int kAutosaveOff = 1;
inline constexpr int kAutosaveMajor = 2;
inline constexpr int kAutosaveAll = 3;
int GetAutosave();
void SetAutosave(int mode);
// The mode the game being played autosaves with: Auto resolved for its seed. Off without one.
int GetEffectiveAutosave();

// The save an autosave replaced, kept so a bad autosave (say, in a room there's no way out of)
// can be undone. The last few are kept for each save slot.
struct AutosaveBackup {
  std::filesystem::path file;
  double playTime = 0.0; // seconds
  std::string region;
  int energyTanks = 0;
  int itemPercent = 0;
};
// The backups of the slot of the randomized game being played, newest first.
std::vector< AutosaveBackup > ListAutosaveBackups();
bool CanRestoreAutosaveBackup();
// Leaves the game and loads `file` in place of the last save, like Quick Reload, then saves it to
// the slot once it has loaded (backing up the save it replaces). Empty on success, otherwise why
// it couldn't.
std::string RestoreAutosaveBackup(const std::filesystem::path& file);

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
