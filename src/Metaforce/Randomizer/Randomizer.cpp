#include "Metaforce/Randomizer/Randomizer.hpp"

#include "Metaforce/MergedWorld.hpp"
#include "Metaforce/Randomizer/CrossWorldDoors.hpp"
#include "Metaforce/Randomizer/DoorLocks.hpp"
#include "Metaforce/Randomizer/Generator.hpp"
#include "Metaforce/Randomizer/Hooks.hpp"
#include "Metaforce/Randomizer/PickupTables.hpp"
#include "Metaforce/SaveAnywhere.hpp"
#include "Metaforce/SaveIndicator.hpp"
#include "Metaforce/Warp.hpp"

#include "Kyoto/Audio/CSfxManager.hpp"
#include "Kyoto/Math/CRelAngle.hpp"
#include "Kyoto/Math/CTransform4f.hpp"
#include "MetroidPrime/CEntityInfo.hpp"
#include "MetroidPrime/CGameArea.hpp"
#include "MetroidPrime/CMapWorld.hpp"
#include "MetroidPrime/CMapWorldInfo.hpp"
#include "MetroidPrime/CMain.hpp"
#include "MetroidPrime/CMemoryCard.hpp"
#include "MetroidPrime/CScriptLayerManager.hpp"
#include "MetroidPrime/CStateManager.hpp"
#include "MetroidPrime/CMemoryCardDriver.hpp"
#include "MetroidPrime/CWorld.hpp"
#include "MetroidPrime/Player/CGameState.hpp"
#include "MetroidPrime/Player/CMorphBall.hpp"
#include "MetroidPrime/Player/CPlayer.hpp"
#include "MetroidPrime/Player/CPlayerState.hpp"
#include "MetroidPrime/Player/CWorldState.hpp"
#include "MetroidPrime/Player/CWorldTransManager.hpp"
#include "MetroidPrime/SFX/UI.h"

#include <SDL3/SDL_filesystem.h>
#include <borealis/log.hpp>
#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace metaforce::randomizer {
namespace {
namespace fs = std::filesystem;
using json = nlohmann::json;

constexpr borealis::Log Log{"randomizer"};

// Artifact Temple keeps one layer per artifact (Strength through Newborn on layers 2-12) that
// shows the artifact on its totem. The vanilla pickups switch these layers through layer
// controllers wired to their original locations, so the randomizer drives them from the
// player's inventory instead.
constexpr uint32_t kArtifactTempleSaveId = 0xCD2B0EA2;
constexpr int kFirstArtifactLayer = 2;
constexpr int kLastArtifactLayer = 12;

// The Artifact of Truth has no layer of its own: "Relay One Shot Out" (0x04100574) in Artifact
// Temple always shows its progress, so it's only allowed through once Truth is in hand, matching
// randomprime. Editor ids are compared without their layer bits.
constexpr uint32_t kTallonWorld = 0x39F2DE28;
constexpr uint32_t kTruthRelay = 0x00100574;
constexpr uint32_t kTruthProgressRelay = 0x00100125;

// Landing Site plays the ship's arrival cinematic the first time it loads, which leaves the
// ship missing (and its save station unusable) in games that start elsewhere. Like randomprime,
// those games get Landing Site set up as if Samus had already landed.
constexpr uint32_t kLandingSiteArea = 0xB2701146;
constexpr uint32_t kStartOverworldCinematicTrigger = 0x000000DD;
constexpr uint32_t kPlayerModelLoadedRelay = 0x000001F4;
constexpr uint32_t kReadySamusPlayerActor = 0x000001CE;
constexpr uint32_t kBackFromLoadTrigger = 0x000001F2;
constexpr uint32_t kSaveStationBeamActor = 0x000001CF;
constexpr uint32_t kBaseLightsEffect = 0x000001E4;
constexpr uint32_t kSamusShipPlatform = 0x00000141;
constexpr uint32_t kShipExitTimer = 0x0000FFF0;
constexpr uint32_t kBackFromLoadTimer = 0x0000FFF1;

constexpr ScriptConnection kBackFromLoadConnections[] = {
    {kSS_Entered, kSM_ResetAndStart, kShipExitTimer},
    {kSS_Entered, kSM_Activate, kReadySamusPlayerActor},
};
constexpr ScriptConnection kShipExitTimerConnections[] = {
    {kSS_Zero, kSM_SetToZero, kPlayerModelLoadedRelay},
};
constexpr ScriptConnection kBackFromLoadTimerConnections[] = {
    {kSS_Zero, kSM_Deactivate, kBackFromLoadTrigger},
};
constexpr ScriptTimerSpawn kLandingSiteTimers[] = {
    {kShipExitTimer, 0.6f, false, kShipExitTimerConnections, 1},
    {kBackFromLoadTimer, 0.02f, true, kBackFromLoadTimerConnections, 1},
};

// Each elevator room's "Memory Relay - dim scan holo", which scanning the elevator's hologram
// activates: it dims the hologram and switches on the trigger that starts the ride. Editor ids
// are without area bits; the game drops script object names, so they can't be found by name.
struct ElevatorRelay {
  uint32_t area; // MREA
  uint32_t relay;
};
constexpr ElevatorRelay kElevatorRelays[] = {
    {0x8316EDF5, 0x0040}, // Chozo Ruins / Transport to Magmoor Caverns North
    {0xA5FA69A1, 0x0046}, // Chozo Ruins / Transport to Tallon Overworld East
    {0x3E6B2BB7, 0x0097}, // Chozo Ruins / Transport to Tallon Overworld North
    {0x236E1B0F, 0x0042}, // Chozo Ruins / Transport to Tallon Overworld South
    {0x3BEAADC9, 0x004C}, // Magmoor Caverns / Transport to Chozo Ruins North
    {0xEF2F1440, 0x003E}, // Magmoor Caverns / Transport to Phazon Mines West
    {0xDCA9A28B, 0x004F}, // Magmoor Caverns / Transport to Phendrana Drifts North
    {0xC1AC9233, 0x0056}, // Magmoor Caverns / Transport to Phendrana Drifts South
    {0x4C3D244C, 0x004D}, // Magmoor Caverns / Transport to Tallon Overworld West
    {0xE2C2CF38, 0x003F}, // Phazon Mines / Transport to Magmoor Caverns South
    {0x430E999C, 0x004A}, // Phazon Mines / Transport to Tallon Overworld South
    {0xDD0B0739, 0x0073}, // Phendrana Drifts / Transport to Magmoor Caverns South
    {0xC00E3781, 0x0046}, // Phendrana Drifts / Transport to Magmoor Caverns West
    {0x8A31665E, 0x0066}, // Tallon Overworld / Transport to Chozo Ruins East
    {0x0CA514F0, 0x0052}, // Tallon Overworld / Transport to Chozo Ruins South
    {0x11A02448, 0x002E}, // Tallon Overworld / Transport to Chozo Ruins West
    {0x15D6FF8B, 0x004B}, // Tallon Overworld / Transport to Magmoor Caverns East
    {0x7D106670, 0x003D}, // Tallon Overworld / Transport to Phazon Mines East
};

// An autosave waits until Samus has gone this long (seconds) without losing energy, so it isn't
// made in the middle of a fight, in lava or in a heated room.
constexpr float kAutosaveUnhurtTime = 2.f;
// And this long (seconds) after the pickup. The pickup is only remembered as collected once its
// message reaches a memory relay, which relays and timers on the way pass on a frame or more
// later: saved before then, the save has the item and the pickup both, and loading it puts the
// pickup back to be collected again.
constexpr float kAutosavePickupSettleTime = 1.f;
// Saves kept for each save slot from before its autosaves replaced them.
constexpr size_t kAutosaveBackups = 5;

uint64_t ObjectKey(uint32_t world, uint32_t editorId) {
  return (static_cast< uint64_t >(world) << 32) | (editorId & 0x3FFFFFF);
}

uint64_t DockKey(uint32_t area, int dock) {
  return (static_cast< uint64_t >(area) << 32) | static_cast< uint32_t >(dock);
}

struct Session {
  fs::path root;

  bool databasesAttempted = false;
  bool databasesLoaded = false;
  Database db;
  PickupDatabase pickups;
  std::string loadError;

  Settings settings;
  bool settingsLoaded = false;
  std::string armedSeed;
  std::map< std::string, std::string > slots; // save slot key -> seed hash
  bool quickReload = true;
  bool quickSave = true;
  int mapLayout = kMapLayoutConnected;
  int autosave = kAutosaveAuto;
  // Whether each shortcut chord was held on the last input seen, so holding one acts once.
  bool quickReloadHeld = false;
  bool quickSaveHeld = false;
  // A quick reload left the game, and the loader hasn't picked it up yet.
  bool quickReloadPending = false;
  // A pickup the autosave mode covers was collected, and the game hasn't been autosaved since.
  bool autosavePending = false;
  // How long since the last pickup the autosave is waiting on.
  float sincePickup = 0.f;
  // An autosave backup is being loaded, and is to be saved once it has.
  bool restoredBackupPending = false;
  // Samus's energy on the last frame, and how long since it last went down.
  float lastEnergy = -1.f;
  float unhurtTime = 0.f;
  // How Samus came into the room she's in, where autosaves load her: the room keeps little of
  // what happened in it (fallen debris is back up, say), but the way she came in is open.
  struct RoomEntry {
    uint32_t area = 0; // MREA
    int dock = -1;     // the dock she came through, or -1 to load at `transform`
    bool morphed = false;
    float yaw = 0.f; // radians about Z, 0 facing +Y; for doors in floors and ceilings
    std::optional< CTransform4f > transform;
  };
  std::optional< RoomEntry > entry;

  std::optional< Seed > active;
  std::unordered_map< uint64_t, int > pickupByObject;
  std::unordered_map< uint64_t, int > memoByObject;
  std::unordered_map< uint64_t, int > audioByObject;
  // DockKey(area, dock) -> index into active->docks
  std::unordered_map< uint64_t, int > dockByKey;
  std::vector< std::wstring > memoText;
  int templeLocation = -1;
  bool pendingNewGame = false;
  // Warp to Start: put the player on the start position once the start area has loaded.
  bool pendingStartTeleport = false;
  // Doors gone through in the game being played, by ObjectKey, and the save slot they belong to.
  // Written to TraversedFile() as they're added.
  std::unordered_set< uint64_t > traversedDoors;
  // Docks gone through, by DockKey(area MREA, dock), both sides of each.
  std::unordered_set< uint64_t > traversedDocks;
  // Rooms the player has been in, by MREA, which stay on the map. Kept with the doors.
  std::unordered_set< uint32_t > visitedAreas;
  std::string traversedSlot;
  // Blast shields broken, by DockKey(area MREA, dock): in the game being played, and as of its
  // last save. Only the saved ones go in TraversedFile(), so reloading brings back the others.
  std::unordered_set< uint64_t > brokenShields;
  std::unordered_set< uint64_t > savedBrokenShields;

  std::thread worker;
  std::atomic< bool > cancel{false};
  std::mutex generationMutex;
  GenerationStatus generation;
};

Session& S() {
  static Session session;
  return session;
}

fs::path SessionFile() { return S().root / "session.json"; }
fs::path SettingsFile() { return S().root / "settings.json"; }
fs::path SeedsRoot() { return S().root / "seeds"; }
fs::path TraversedFile(const std::string& slot) { return S().root / "traversed" / (slot + ".json"); }

void SaveSession() {
  const json root{
      {"armed", S().armedSeed},
      {"slots", S().slots},
      {"quick_reload", S().quickReload},
      {"quick_save", S().quickSave},
      {"map_layout", S().mapLayout},
      {"autosave", S().autosave},
  };
  std::error_code ec;
  fs::create_directories(S().root, ec);
  std::ofstream file(SessionFile(), std::ios::binary | std::ios::trunc);
  file << root.dump(2);
}

void LoadSession() {
  std::ifstream file(SessionFile(), std::ios::binary);
  if (!file) {
    return;
  }
  const json root = json::parse(file, nullptr, false);
  if (root.is_discarded()) {
    return;
  }
  S().armedSeed = root.value("armed", "");
  S().quickReload = root.value("quick_reload", true);
  S().quickSave = root.value("quick_save", true);
  S().mapLayout = std::clamp(root.value("map_layout", static_cast< int >(kMapLayoutConnected)),
                             kMapLayoutVanilla, kMapLayoutConnected);
  S().autosave = std::clamp(root.value("autosave", static_cast< int >(kAutosaveAuto)),
                            kAutosaveAuto, kAutosaveAll);
  if (root.contains("slots") && root["slots"].is_object()) {
    S().slots = root["slots"].get< std::map< std::string, std::string > >();
  }
}

std::wstring Widen(std::string_view str) {
  std::wstring out;
  out.reserve(str.size());
  for (const char c : str) {
    out.push_back(static_cast< wchar_t >(static_cast< unsigned char >(c)));
  }
  return out;
}

void Deactivate() {
  auto& s = S();
  s.active.reset();
  s.pickupByObject.clear();
  s.memoByObject.clear();
  s.audioByObject.clear();
  s.dockByKey.clear();
  s.memoText.clear();
  s.templeLocation = -1;
  s.pendingNewGame = false;
  s.autosavePending = false;
  s.traversedDoors.clear();
  s.traversedDocks.clear();
  s.visitedAreas.clear();
  s.traversedSlot.clear();
  s.brokenShields.clear();
  s.savedBrokenShields.clear();
  door_locks::Deactivate();
}

void SaveTraversed() {
  const auto& s = S();
  if (!s.active || s.traversedSlot.empty()) {
    return;
  }
  const json root{
      {"seed", s.active->hash},
      {"doors", std::vector< uint64_t >(s.traversedDoors.begin(), s.traversedDoors.end())},
      {"docks", std::vector< uint64_t >(s.traversedDocks.begin(), s.traversedDocks.end())},
      {"rooms", std::vector< uint32_t >(s.visitedAreas.begin(), s.visitedAreas.end())},
      {"blast_shields",
       std::vector< uint64_t >(s.savedBrokenShields.begin(), s.savedBrokenShields.end())}};
  std::error_code ec;
  fs::create_directories(TraversedFile(s.traversedSlot).parent_path(), ec);
  std::ofstream file(TraversedFile(s.traversedSlot), std::ios::binary | std::ios::trunc);
  file << root.dump();
}

// Starts tracking doors for `slot`: from its file, or from nothing for a new game.
void LoadTraversed(const std::string& slot, bool newGame) {
  auto& s = S();
  s.traversedDoors.clear();
  s.traversedDocks.clear();
  s.visitedAreas.clear();
  s.brokenShields.clear();
  s.savedBrokenShields.clear();
  s.traversedSlot = slot;
  if (newGame) {
    std::error_code ec;
    fs::remove(TraversedFile(slot), ec);
    return;
  }
  std::ifstream file(TraversedFile(slot), std::ios::binary);
  if (!file || !s.active) {
    return;
  }
  const json root = json::parse(file, nullptr, false);
  if (root.is_discarded() || root.value("seed", "") != s.active->hash ||
      !root.contains("doors") || !root["doors"].is_array()) {
    return;
  }
  for (const json& door : root["doors"]) {
    if (door.is_number_unsigned()) {
      s.traversedDoors.insert(door.get< uint64_t >());
    }
  }
  if (root.contains("docks") && root["docks"].is_array()) {
    for (const json& dock : root["docks"]) {
      if (dock.is_number_unsigned()) {
        s.traversedDocks.insert(dock.get< uint64_t >());
      }
    }
  }
  if (root.contains("rooms") && root["rooms"].is_array()) {
    for (const json& room : root["rooms"]) {
      if (room.is_number_unsigned()) {
        s.visitedAreas.insert(room.get< uint32_t >());
      }
    }
  }
  if (root.contains("blast_shields") && root["blast_shields"].is_array()) {
    for (const json& shield : root["blast_shields"]) {
      if (shield.is_number_unsigned()) {
        s.savedBrokenShields.insert(shield.get< uint64_t >());
      }
    }
  }
  s.brokenShields = s.savedBrokenShields;
}

void Activate(Seed seed) {
  auto& s = S();
  Deactivate();
  if (seed.locations.size() != kPickupLocationCount) {
    Log.error("Seed {} has {} locations, expected {}", seed.hash, seed.locations.size(),
              kPickupLocationCount);
    return;
  }
  uint32_t templeArea = 0;
  if (const Database* db = GetDatabase()) {
    for (const auto& area : db->Areas()) {
      if (area.name == "Artifact Temple") {
        templeArea = area.assetId;
      }
    }
  }
  s.memoText.resize(kPickupLocationCount);
  for (int i = 0; i < kPickupLocationCount; ++i) {
    const PickupLocationInfo& loc = kPickupLocations[i];
    s.pickupByObject.emplace(ObjectKey(loc.mlvl, loc.pickupId), i);
    s.memoByObject.emplace(ObjectKey(loc.mlvl, loc.hudMemoId), i);
    s.audioByObject.emplace(ObjectKey(loc.mlvl, loc.audioId), i);
    s.memoText[i] = Widen(seed.locations[i].name + " acquired!");
    if (loc.mrea == templeArea) {
      s.templeLocation = i;
    }
  }
  for (int i = 0; i < static_cast< int >(seed.docks.size()); ++i) {
    s.dockByKey.emplace(DockKey(seed.docks[i].area, seed.docks[i].dock), i);
  }
  door_locks::Activate(seed);
  Log.info("Activated seed {}", seed.hash);
  s.active = std::move(seed);
}

bool ActivateByHash(const std::string& hash) {
  if (S().active && S().active->hash == hash) {
    return true;
  }
  std::string error;
  auto seed = LoadSeed(hash, error);
  if (!seed) {
    Log.error("Could not load seed {}: {}", hash, error);
    Deactivate();
    return false;
  }
  Activate(std::move(*seed));
  return true;
}

bool TempleHolds(int itemType) {
  const auto& s = S();
  return s.active && s.templeLocation >= 0 &&
         s.active->locations[s.templeLocation].grant.itemType == itemType;
}

void SyncArtifactLayers() {
  if (!S().active || gpGameState == nullptr || gpMemoryCard == nullptr) {
    return;
  }
  const rstl::pair< CAssetId, int > temple =
      gpMemoryCard->GetAreaAndWorldIdForSaveId(static_cast< int >(kArtifactTempleSaveId));
  if (temple.first == kInvalidAssetId) {
    return;
  }
  CScriptLayerManager& layers = *gpGameState->StateForWorld(temple.first).GetLayerState();
  // A new game has no layer state for a world until it's loaded, and SetLayerActive doesn't check
  // bounds. Fill in the world's defaults first, as InitializeMemoryWorlds does; loading the world
  // later keeps layers that are already there.
  if (gpMemoryCard->HasSaveWorldMemory(temple.first)) {
    layers.InitializeWorldLayers(
        gpMemoryCard->GetSaveWorldMemory(temple.first).GetDefaultLayerStates(),
        rstl::rc_ptr< rstl::vector< rstl::string > >(), rstl::rc_ptr< rstl::vector< int > >());
  }
  if (temple.second < 0 || temple.second >= static_cast< int >(layers.GetAreaLayers().size()) ||
      layers.GetAreaLayerCount(TAreaId(temple.second)) <= kLastArtifactLayer) {
    return;
  }
  CPlayerState& player = *gpGameState->PlayerState();
  for (int layer = kFirstArtifactLayer; layer <= kLastArtifactLayer; ++layer) {
    const int itemType = CPlayerState::kIT_Truth + (layer - 1);
    const bool placed =
        player.HasPowerUp(static_cast< CPlayerState::EItemType >(itemType)) || TempleHolds(itemType);
    layers.SetLayerActive(TAreaId(temple.second), TLayerId(layer), placed);
  }
}

std::string SlotKey() {
  return fmt::format("{:016X}-{}", gpGameState->GetCardSerial(), gpGameState->GetFileIdx());
}

// Autosave backups of a save slot: the slot's save from before each autosave, as the card held
// it, named by when they were kept.
fs::path BackupDirectory(const std::string& slot) { return S().root / "autosave" / slot; }

std::vector< fs::path > BackupFiles(const std::string& slot) {
  std::vector< fs::path > files;
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(BackupDirectory(slot), ec)) {
    if (entry.path().extension() == ".sav") {
      files.push_back(entry.path());
    }
  }
  // Names are zero-padded times, so newest first is reverse name order.
  std::sort(files.begin(), files.end(), std::greater<>());
  return files;
}

void KeepBackup(const std::vector< uint8_t >& save) {
  const std::string slot = SlotKey();
  std::error_code ec;
  fs::create_directories(BackupDirectory(slot), ec);
  const auto now = std::chrono::duration_cast< std::chrono::milliseconds >(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
  const fs::path path = BackupDirectory(slot) / fmt::format("{:020}.sav", now);
  {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast< const char* >(save.data()),
               static_cast< std::streamsize >(save.size()));
    if (!file) {
      Log.warn("Could not write autosave backup {}", path.string());
      return;
    }
  }
  const std::vector< fs::path > files = BackupFiles(slot);
  for (size_t i = kAutosaveBackups; i < files.size(); ++i) {
    fs::remove(files[i], ec);
  }
}

// The game over screen's Continue: kRM_StateSetter rebuilds the game state from the backup the
// last save left behind (CMain::RefreshGameState), then loads it.
void ReloadLastSave() {
  S().quickReloadPending = true;
  gpGameState->WorldTransitionManager()->DisableTransition();
  gpMain->SetRestartMode(CMain::kRM_StateSetter);
  gpStateManager->QuitGame();
}

// The seed location of a pickup, or -1 for a pickup the seed didn't place: the energy and ammo
// that enemies drop and pickup generators spawn.
int PickupLocation(const CStateManager& mgr, unsigned int editorId) {
  const auto& s = S();
  CAssetId world = mgr.GetWorld()->GetWorldAssetId();
  editorId = merged::ToSourceEditorId(editorId, world);
  const auto it = s.pickupByObject.find(ObjectKey(world, editorId));
  return it != s.pickupByObject.end() ? it->second : -1;
}

// Major Only autosaves after every upgrade, Energy Tank and artifact, but not after expansions,
// refills or nothing. Power Bomb and its expansions give the same item, so this goes by what the
// seed placed at the pickup.
bool IsMajorPickup(const CStateManager& mgr, int itemType, unsigned int editorId) {
  const auto& s = S();
  if (itemType >= CPlayerState::kIT_Truth && itemType <= CPlayerState::kIT_Newborn) {
    return true;
  }
  const int location = PickupLocation(mgr, editorId);
  return location >= 0 && s.pickups.FindStandard(s.active->locations[location].name) != nullptr;
}

// Standing or rolling on the ground. A spider ball on a track counts as on the ground wherever the
// track goes, so it doesn't count here.
bool IsOnGround(const CPlayer& player) {
  if (player.GetPlayerMovementState() != NPlayer::kMS_OnGround) {
    return false;
  }
  return player.GetMorphballTransitionState() != CPlayer::kMS_Morphed ||
         player.GetMorphBall()->GetSpiderBallState() != CMorphBall::kSBS_Active;
}

// The MREA of the room the player is in, or 0 outside gameplay.
uint32_t CurrentAreaAssetId(const CStateManager& mgr) {
  const CWorld* world = mgr.GetWorld();
  const TAreaId area = mgr.GetNextAreaId();
  return world != nullptr && world->DoesAreaExist(area)
             ? world->GetAreaAlways(area).GetAreaAssetId()
             : 0;
}

float PlayerYaw(const CPlayer& player) {
  const CVector3f forward = player.GetTransform().GetForward();
  return std::atan2(-forward.GetX(), forward.GetY());
}

// Where an autosave made now loads the player: where they came into the room. Nothing to load
// them where they are.
std::optional< save_anywhere::SpawnPoint > AutosaveSpawn(const CStateManager& mgr) {
  const auto& entry = S().entry;
  if (!entry || entry->area != CurrentAreaAssetId(mgr)) {
    return std::nullopt;
  }
  if (entry->dock < 0) {
    return save_anywhere::SpawnPoint{*entry->transform, entry->morphed};
  }
  const CGameArea& area = mgr.GetWorld()->GetAreaAlways(mgr.GetNextAreaId());
  if (std::optional< CTransform4f > xf =
          DockArrivalTransform(mgr, area, entry->dock, entry->morphed, entry->yaw)) {
    return save_anywhere::SpawnPoint{*xf, entry->morphed};
  }
  Log.warn("Entry dock {} of 0x{:08X} wasn't found; autosaving where Samus is", entry->dock,
           entry->area);
  return std::nullopt;
}

void WriteAutosave(const CStateManager& mgr) {
  std::vector< uint8_t > previous;
  const std::string error = save_anywhere::SaveSilently(&previous, AutosaveSpawn(mgr));
  if (!error.empty()) {
    Log.warn("Autosave failed: {}", error);
    return;
  }
  Log.info("Autosaved");
  save_indicator::Show();
  if (!previous.empty()) {
    KeepBackup(previous);
  }
}

void JoinWorker() {
  if (S().worker.joinable()) {
    S().worker.join();
  }
}

void SetGeneration(GenerationStatus status) {
  std::lock_guard lock{S().generationMutex};
  S().generation = std::move(status);
}

} // namespace

void Initialize(const fs::path& userPath) {
  auto& s = S();
  s.root = userPath / "randomizer";
  s.settingsLoaded = s.settings.Load(SettingsFile());
  LoadSession();
}

void Shutdown() {
  CancelGeneration();
  JoinWorker();
}

const Database* GetDatabase() {
  auto& s = S();
  if (!s.databasesAttempted) {
    s.databasesAttempted = true;
    // Resolve like aurora's RmlUi file interface: relative to the app's resources directory.
    fs::path base = fs::path("res") / "randomizer" / "prime1";
    if (const char* resources = SDL_GetBasePath()) {
      base = fs::path(resources) / base;
    }
    s.databasesLoaded = s.db.Load(base / "logic.json", s.loadError) &&
                        s.pickups.Load(base / "pickups.json", s.loadError);
    if (!s.databasesLoaded) {
      Log.error("{}", s.loadError);
    } else if (s.settingsLoaded) {
      s.settings.FillMissing(s.pickups);
    } else {
      s.settings.ResetToDefaults(s.pickups);
    }
  }
  return s.databasesLoaded ? &s.db : nullptr;
}

const PickupDatabase* GetPickupDatabase() { return GetDatabase() ? &S().pickups : nullptr; }

const std::string& LoadError() { return S().loadError; }

Settings& GetSettings() {
  GetDatabase();
  return S().settings;
}

void SaveSettings() {
  if (!S().settings.Save(SettingsFile())) {
    Log.warn("Could not save randomizer settings");
  }
}

void ResetSettings() {
  if (GetDatabase()) {
    S().settings.ResetToDefaults(S().pickups);
    SaveSettings();
  }
}

std::vector< SeedSummary > ListSeeds() {
  std::vector< SeedSummary > result;
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(SeedsRoot(), ec)) {
    const fs::path file = entry.path() / "seed.json";
    std::ifstream in(file, std::ios::binary);
    if (!in) {
      continue;
    }
    const json root = json::parse(in, nullptr, false);
    if (root.is_discarded()) {
      continue;
    }
    const int version = root.value("format_version", 0);
    if (version < Seed::kOldestFormatVersion || version > Seed::kFormatVersion) {
      continue;
    }
    SeedSummary summary{root.value("hash", ""), root.value("seed", ""), {},
                        fs::last_write_time(file, ec)};
    if (root.contains("start")) {
      summary.startName = root["start"].value("name", "");
      summary.randomStart = root["start"].value("random", false);
    }
    if (summary.hash == entry.path().filename().string()) {
      result.push_back(std::move(summary));
    }
  }
  std::sort(result.begin(), result.end(),
            [](const SeedSummary& a, const SeedSummary& b) { return a.modified > b.modified; });
  return result;
}

std::optional< Seed > LoadSeed(const std::string& hash, std::string& error) {
  return Seed::Load(SeedDirectory(hash) / "seed.json", error);
}

bool DeleteSeed(const std::string& hash) {
  if (hash.empty()) {
    return false;
  }
  std::error_code ec;
  fs::remove_all(SeedDirectory(hash), ec);
  if (S().armedSeed == hash) {
    S().armedSeed.clear();
  }
  std::erase_if(S().slots, [&hash](const auto& slot) { return slot.second == hash; });
  SaveSession();
  return !ec;
}

fs::path SeedDirectory(const std::string& hash) { return SeedsRoot() / hash; }

std::optional< fs::path > WriteSpoiler(const std::string& hash, std::string& error) {
  auto seed = LoadSeed(hash, error);
  if (!seed) {
    return std::nullopt;
  }
  std::vector< std::string > names;
  for (int i = 0; i < kPickupLocationCount; ++i) {
    names.push_back(LocationName(i));
  }
  const fs::path path = SeedDirectory(hash) / "spoiler.txt";
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file << seed->SpoilerText(names);
  if (!file) {
    error = "Could not write " + path.string();
    return std::nullopt;
  }
  return path;
}

const std::string& GetArmedSeed() { return S().armedSeed; }

void SetArmedSeed(const std::string& hash) {
  S().armedSeed = hash;
  SaveSession();
}

const Seed* GetActiveSeed() { return S().active ? &*S().active : nullptr; }

bool GetQuickReload() { return S().quickReload; }

void SetQuickReload(bool enabled) {
  S().quickReload = enabled;
  SaveSession();
}

bool GetQuickSave() { return S().quickSave; }

void SetQuickSave(bool enabled) {
  S().quickSave = enabled;
  SaveSession();
}

int GetMapLayout() { return S().mapLayout; }

void SetMapLayout(int mode) {
  S().mapLayout = std::clamp(mode, kMapLayoutVanilla, kMapLayoutConnected);
  SaveSession();
}

int GetAutosave() { return S().autosave; }

void SetAutosave(int mode) {
  S().autosave = std::clamp(mode, kAutosaveAuto, kAutosaveAll);
  SaveSession();
}

int GetEffectiveAutosave() {
  const auto& s = S();
  if (s.autosave != kAutosaveAuto) {
    return s.autosave;
  }
  return s.active && !s.active->docks.empty() ? kAutosaveMajor : kAutosaveOff;
}

std::vector< AutosaveBackup > ListAutosaveBackups() {
  std::vector< AutosaveBackup > result;
  if (!S().active || gpGameState == nullptr) {
    return result;
  }
  for (const fs::path& path : BackupFiles(SlotKey())) {
    // LoadGameFileState reads through a stream that claims more than a slot holds.
    std::vector< uint8_t > data(4096);
    std::ifstream file(path, std::ios::binary);
    file.read(reinterpret_cast< char* >(data.data()), CMemoryCardDriver::kFileSlotSize);
    if (file.gcount() != CMemoryCardDriver::kFileSlotSize) {
      continue;
    }
    const CGameState::GameFileStateInfo info = CGameState::LoadGameFileState(data.data());
    AutosaveBackup backup;
    backup.file = path;
    backup.playTime = info.mPlayTime;
    backup.energyTanks = static_cast< int >(info.mEnergyTanks);
    backup.itemPercent = static_cast< int >(info.mItemPercent);
    for (const warp::World& world : warp::GetWorlds()) {
      if (world.mlvl == info.mMlvlId) {
        backup.region = world.name;
      }
    }
    result.push_back(std::move(backup));
  }
  return result;
}

bool CanRestoreAutosaveBackup() {
  return S().active && gpGameState != nullptr && gpStateManager != nullptr &&
         gpStateManager->IsFullyInitialized();
}

std::string RestoreAutosaveBackup(const fs::path& path) {
  if (!CanRestoreAutosaveBackup()) {
    return "There's no randomized game in progress.";
  }
  std::vector< uint8_t > data(CMemoryCardDriver::kFileSlotSize);
  std::ifstream file(path, std::ios::binary);
  file.read(reinterpret_cast< char* >(data.data()), CMemoryCardDriver::kFileSlotSize);
  if (file.gcount() != CMemoryCardDriver::kFileSlotSize) {
    return "The backup couldn't be read.";
  }
  // Quick Reload loads whatever the backup buffer holds, which is a save slot's bytes.
  rstl::vector< uchar >& backup = gpGameState->BackupBuf();
  backup.assign(CMemoryCardDriver::kFileSlotSize);
  std::memcpy(backup.data(), data.data(), data.size());
  Log.info("Restoring autosave backup {}", path.filename().string());
  S().restoredBackupPending = true;
  ReloadLastSave();
  return {};
}

bool StartGeneration() {
  auto& s = S();
  if (GetGenerationStatus().state == GenerationState::Running) {
    return false;
  }
  if (GetDatabase() == nullptr) {
    SetGeneration({GenerationState::Failed, 0.f, s.loadError, {}});
    return false;
  }
  JoinWorker();

  Settings settings = s.settings;
  if (settings.seedString.empty()) {
    settings.seedString = RandomSeedString();
  }
  s.cancel = false;
  SetGeneration({GenerationState::Running, 0.f, "Starting", {}});
  s.worker = std::thread([settings = std::move(settings)] {
    auto& s = S();
    const GeneratorInput input{s.db, s.pickups, settings};
    std::string error;
    auto progress = [](float value, std::string_view status) {
      std::lock_guard lock{S().generationMutex};
      S().generation.progress = value;
      S().generation.message = std::string(status);
    };
    std::optional< Seed > seed = Generate(input, error, progress, &s.cancel);
    if (seed && !seed->Save(SeedDirectory(seed->hash), error)) {
      seed.reset();
    }
    if (!seed) {
      Log.warn("Seed generation failed: {}", error);
      SetGeneration({GenerationState::Failed, 1.f, error, {}});
      return;
    }
    Log.info("Generated seed {} from seed string {}", seed->hash, seed->seedString);
    SetGeneration({GenerationState::Succeeded, 1.f, "Seed generated", seed->hash});
  });
  return true;
}

void CancelGeneration() { S().cancel = true; }

GenerationStatus GetGenerationStatus() {
  std::lock_guard lock{S().generationMutex};
  return S().generation;
}

void AcknowledgeGeneration() {
  std::lock_guard lock{S().generationMutex};
  if (S().generation.state != GenerationState::Running) {
    S().generation = {};
  }
}

std::string LocationName(int pickupIndex) {
  const Database* db = GetDatabase();
  if (db == nullptr || pickupIndex < 0 ||
      pickupIndex >= static_cast< int >(db->PickupNodes().size())) {
    return fmt::format("Location {}", pickupIndex);
  }
  return db->NodeName(db->PickupNodes()[pickupIndex]);
}

// Hooks

bool GetDockOverride(unsigned int worldId, unsigned int areaAssetId, int dock,
                     unsigned int& targetAreaAssetId, int& targetDock) {
  const auto& s = S();
  if (!s.active) {
    return false;
  }
  const auto it = s.dockByKey.find(DockKey(areaAssetId, dock));
  if (it == s.dockByKey.end()) {
    return false;
  }
  const DockConnection& conn = s.active->docks[it->second];
  // Doors into another world can't be redirected within this one, unless that world was appended
  // to this one.
  if (conn.world != worldId ||
      (conn.targetWorld != worldId && !merged::IsWorldLoaded(conn.targetWorld))) {
    return false;
  }
  targetAreaAssetId = conn.targetArea;
  targetDock = conn.targetDock;
  return true;
}

bool OnShortcutInput(bool r, bool z, bool dpadLeft, bool dpadRight) {
  auto& s = S();
  const bool reloadHeld = r && z && dpadLeft;
  const bool saveHeld = r && z && dpadRight;
  const bool reloadCompleted = reloadHeld && !s.quickReloadHeld;
  const bool saveCompleted = saveHeld && !s.quickSaveHeld;
  s.quickReloadHeld = reloadHeld;
  s.quickSaveHeld = saveHeld;
  if (!s.active || gpStateManager == nullptr) {
    return false;
  }

  if (reloadCompleted && s.quickReload) {
    Log.info("Quick reload: reloading the last save");
    ReloadLastSave();
    return true;
  }

  if (saveCompleted && s.quickSave) {
    const std::string reason = save_anywhere::WhyCantSave();
    if (!reason.empty()) {
      Log.info("Quick save: can't save now: {}", reason);
      CSfxManager::SfxStart(SFXui_x_warning_00, 0x7f, 0x40, false, CSfxManager::kMedPriority,
                            false, CSfxManager::kAllAreas);
      return false;
    }
    save_anywhere::RequestSave();
    return true;
  }
  return false;
}

void MarkDoorTraversed(unsigned int world, unsigned int editorId) {
  auto& s = S();
  if (s.active && !s.traversedSlot.empty() &&
      s.traversedDoors.insert(ObjectKey(world, editorId)).second) {
    SaveTraversed();
  }
}

void MarkDockTraversed(unsigned int areaAssetId, int dock) {
  auto& s = S();
  if (s.active && !s.traversedSlot.empty() &&
      s.traversedDocks.insert(DockKey(areaAssetId, dock)).second) {
    SaveTraversed();
  }
}

void MarkRoomEntered(unsigned int areaAssetId, int dock, bool morphed, float yaw) {
  Session::RoomEntry entry;
  entry.area = areaAssetId;
  entry.dock = dock;
  entry.morphed = morphed;
  entry.yaw = yaw;
  S().entry = entry;
}

bool IsDockTraversed(unsigned int areaAssetId, int dock) {
  const auto& s = S();
  return s.active && s.traversedDocks.count(DockKey(areaAssetId, dock)) != 0;
}

int GetTraversedDockCount() {
  const auto& s = S();
  return s.active ? static_cast< int >(s.traversedDocks.size()) : 0;
}

bool IsBlastShieldDestroyed(uint32_t area, int dock) {
  return S().brokenShields.count(DockKey(area, dock)) != 0;
}

void MarkBlastShieldDestroyed(uint32_t area, int dock) {
  if (S().active) {
    S().brokenShields.insert(DockKey(area, dock));
  }
}

void OnGameSaved() {
  auto& s = S();
  if (s.active && s.savedBrokenShields != s.brokenShields) {
    s.savedBrokenShields = s.brokenShields;
    SaveTraversed();
  }
}

bool IsDoorTraversed(unsigned int worldId, unsigned int editorId) {
  const auto& s = S();
  return s.active && s.traversedDoors.count(ObjectKey(worldId, editorId)) != 0;
}

bool ShortcutsHoldMap(bool r) {
  return r && S().active && (S().quickReload || S().quickSave);
}

bool TakeQuickReload() {
  auto& s = S();
  const bool pending = s.quickReloadPending;
  s.quickReloadPending = false;
  return pending;
}

void OnGameLoad() {
  if (gpGameState == nullptr) {
    return;
  }
  auto& s = S();
  const std::string key = SlotKey();
  // An autosave still waiting belongs to the game that was left. A restored backup is saved once
  // it's loaded, so that it's what the card holds.
  s.autosavePending = s.restoredBackupPending;
  s.restoredBackupPending = false;
  s.sincePickup = kAutosavePickupSettleTime;
  if (gpGameState->GetInitPowerupsAtFirstSpawn()) {
    // A new game in the slot: the backups were of the game it replaces.
    std::error_code ec;
    fs::remove_all(BackupDirectory(key), ec);
    if (s.armedSeed.empty() || !ActivateByHash(s.armedSeed)) {
      Deactivate();
      if (s.slots.erase(key) != 0) {
        SaveSession();
      }
      return;
    }
    s.slots[key] = s.active->hash;
    SaveSession();
    LoadTraversed(key, true);
    gpGameState->SetCurrentWorldId(s.active->startWorld);
    CWorldState& world = gpGameState->StateForWorld(s.active->startWorld);
    world.SetDesiredAreaAssetId(s.active->startArea);
    s.pendingNewGame = true;
    Log.info("Starting new randomized game ({}) at {}", s.active->hash,
             s.active->randomStart ? "a random location" : s.active->startName);
    return;
  }

  if (s.pendingNewGame) {
    return;
  }
  const auto it = s.slots.find(key);
  if (it == s.slots.end()) {
    Deactivate();
  } else if (ActivateByHash(it->second)) {
    LoadTraversed(key, false);
  }
}

namespace {
// Puts the player on the seed's start position, facing its start direction. Starts at doors go
// just inside the door, facing into the room.
void TeleportToStart(CStateManager& mgr) {
  const auto& s = S();
  if (!s.active || (!s.active->startPosition && s.active->startDock < 0) ||
      mgr.Player() == nullptr) {
    return;
  }
  CPlayer* samus = mgr.Player();
  if (s.active->startDock >= 0 && mgr.GetWorld() != nullptr) {
    const CWorld& world = *mgr.GetWorld();
    const TAreaId area = world.GetAreaId(s.active->startArea);
    if (world.DoesAreaExist(area)) {
      const float yaw = s.active->startYaw.value_or(0.f);
      if (const std::optional< CTransform4f > xf = DockArrivalTransform(
              mgr, world.GetAreaAlways(area), s.active->startDock, false, yaw)) {
        samus->Teleport(*xf, mgr, true);
        return;
      }
    }
    Log.error("Start dock {} of 0x{:08X} wasn't found", s.active->startDock,
              s.active->startArea);
  }
  if (!s.active->startPosition) {
    return;
  }
  const auto& pos = *s.active->startPosition;
  CVector3f at(pos[0], pos[1], pos[2]);
  if (mgr.GetWorld() != nullptr) {
    const CWorld& world = *mgr.GetWorld();
    const TAreaId area = world.GetAreaId(s.active->startArea);
    if (world.DoesAreaExist(area)) {
      at = SettleStartPosition(mgr, world.GetAreaAlways(area), at);
    }
  }
  const CMatrix3f facing =
      s.active->startYaw
          ? CTransform4f::RotateZ(CRelAngle::FromRadians(*s.active->startYaw)).BuildMatrix3f()
          : samus->GetTransform().BuildMatrix3f();
  const CTransform4f xf(facing, at);
  samus->Teleport(xf, mgr, true);
}
} // namespace

bool CanWarpToStart() { return S().active.has_value() && warp::CanWarp(); }

void WarpToStart() {
  auto& s = S();
  if (!CanWarpToStart()) {
    return;
  }
  for (const warp::World& world : warp::GetWorlds()) {
    if (world.mlvl != s.active->startWorld) {
      continue;
    }
    for (const warp::Area& area : world.areas) {
      if (area.mrea == s.active->startArea) {
        Log.info("Warping to the start: {}", s.active->startName);
        s.pendingStartTeleport = true;
        warp::RequestWarp(world.mlvl, area.index, std::nullopt);
        return;
      }
    }
  }
  Log.error("The start area 0x{:08X} of world 0x{:08X} wasn't found", s.active->startArea,
            s.active->startWorld);
}

namespace {
// Marks `area` of the loaded world visited on its map, and a room appended from another region
// on that region's own map too, which the map of the other regions shows. True when the loaded
// world's map changed.
bool SetAreaVisited(CStateManager& mgr, TAreaId area) {
  CMapWorldInfo& info = *mgr.MapWorldInfo();
  const bool changed = !info.IsAreaVisited(area);
  info.SetAreaVisited(area, true);
  if (merged::IsForeignArea(area)) {
    const CAssetId source = merged::GetSourceWorld(mgr.GetWorld()->GetWorldAssetId(), area);
    gpGameState->StateForWorld(source).MapWorldInfo()->SetAreaVisited(
        TAreaId(merged::GetSourceAreaIndex(area)), true);
  }
  return changed;
}

// Puts every room of the loaded world the player has been in back on the map. The save only
// holds the rooms visited as of when it was made, and none of those appended from other regions.
void RestoreVisitedAreas(CStateManager& mgr) {
  const auto& s = S();
  const CWorld* world = mgr.GetWorld();
  if (!s.active || s.visitedAreas.empty() || world == nullptr) {
    return;
  }
  bool changed = false;
  for (int i = 0; i < world->GetNumAreas(); ++i) {
    if (s.visitedAreas.count(world->GetAreaAlways(TAreaId(i)).GetAreaAssetId()) != 0) {
      changed |= SetAreaVisited(mgr, TAreaId(i));
    }
  }
  if (changed && world->GetMapWorld() != nullptr) {
    world->GetMapWorld()->RecalculateWorldSphere(*mgr.MapWorldInfo(), *world);
  }
}
} // namespace

void OnAreaVisited(CStateManager& mgr, int area) {
  auto& s = S();
  const CWorld* world = mgr.GetWorld();
  if (!s.active || s.traversedSlot.empty() || world == nullptr ||
      !world->DoesAreaExist(TAreaId(area))) {
    return;
  }
  SetAreaVisited(mgr, TAreaId(area));
  if (s.visitedAreas.insert(world->GetAreaAlways(TAreaId(area)).GetAreaAssetId()).second) {
    SaveTraversed();
  }
}

void OnWorldInitialized(CStateManager& mgr) {
  ApplyCrossWorldArrival(mgr);
  RestoreVisitedAreas(mgr);
  auto& s = S();
  s.lastEnergy = -1.f;
  s.unhurtTime = 0.f;
  s.entry.reset();
  if (s.active && s.pendingStartTeleport) {
    s.pendingStartTeleport = false;
    TeleportToStart(mgr);
  }
  if (!s.active || !s.pendingNewGame) {
    return;
  }
  s.pendingNewGame = false;
  gpGameState->SetDeferPowerupInit(false);

  CPlayerState& player = *mgr.PlayerState();
  for (int i = CPlayerState::kIT_PowerBeam; i < CPlayerState::kIT_Max; ++i) {
    const auto type = static_cast< CPlayerState::EItemType >(i);
    player.InitializePowerUp(type, -player.GetItemCapacity(type));
  }
  for (const auto& item : s.active->startingItems) {
    const auto type = static_cast< CPlayerState::EItemType >(item.grant.itemType);
    if (item.grant.itemType < 0 || item.grant.itemType >= CPlayerState::kIT_Max) {
      continue;
    }
    player.InitializePowerUp(type, item.grant.capacity);
    player.IncrPickUp(type, item.grant.amount);
  }
  player.HealthInfo()->SetHP(player.CalculateHealth());

  TeleportToStart(mgr);
  SyncArtifactLayers();
}

bool GetPickupOverride(unsigned int worldId, unsigned int editorId, PickupOverride& out) {
  const auto& s = S();
  if (!s.active) {
    return false;
  }
  editorId = merged::ToSourceEditorId(editorId, worldId);
  const auto it = s.pickupByObject.find(ObjectKey(worldId, editorId));
  if (it == s.pickupByObject.end()) {
    return false;
  }
  const PlacedPickup& placed = s.active->locations[it->second];
  out.itemType = placed.grant.itemType;
  out.capacity = placed.grant.capacity;
  out.amount = placed.grant.amount;
  const PickupModelTemplate* model = FindPickupModel(placed.model);
  out.modelData = model != nullptr ? model->data : nullptr;
  out.modelSize = model != nullptr ? model->size : 0;
  return true;
}

bool GetPickupModelCenter(unsigned int cmdl, float out[3]) {
  for (int i = 0; i < kPickupModelAabbCount; ++i) {
    const PickupModelAabb& aabb = kPickupModelAabbs[i];
    if (aabb.cmdl != cmdl) {
      continue;
    }
    for (int axis = 0; axis < 3; ++axis) {
      out[axis] = (std::bit_cast< float >(aabb.bounds[axis]) +
                   std::bit_cast< float >(aabb.bounds[axis + 3])) /
                  2.f;
    }
    return true;
  }
  return false;
}

const wchar_t* GetHudMemoOverride(unsigned int worldId, unsigned int editorId) {
  const auto& s = S();
  if (!s.active) {
    return nullptr;
  }
  editorId = merged::ToSourceEditorId(editorId, worldId);
  const auto it = s.memoByObject.find(ObjectKey(worldId, editorId));
  return it == s.memoByObject.end() ? nullptr : s.memoText[it->second].c_str();
}

const char* GetPickupAudioOverride(unsigned int worldId, unsigned int editorId) {
  const auto& s = S();
  if (!s.active) {
    return nullptr;
  }
  editorId = merged::ToSourceEditorId(editorId, worldId);
  const auto it = s.audioByObject.find(ObjectKey(worldId, editorId));
  if (it == s.audioByObject.end()) {
    return nullptr;
  }
  // Like randomprime's attainment_audio_file_name: artifacts get the artifact jingle, major
  // upgrades the long item fanfare, and expansions, refills and the rest the short one.
  const PlacedPickup& placed = s.active->locations[it->second];
  const int type = placed.grant.itemType;
  if (type >= CPlayerState::kIT_Truth && type <= CPlayerState::kIT_Newborn) {
    return "/audio/jin_artifact.dsp";
  }
  const StandardPickupDef* def = s.pickups.FindStandard(placed.name);
  return def != nullptr && def->major ? "/audio/jin_itemattain.dsp" : "/audio/itm_x_short_02.dsp";
}

namespace {
bool PatchesLandingSite() {
  return S().active && S().active->startArea != kLandingSiteArea;
}
} // namespace

bool GetScriptObjectPatch(unsigned int worldId, unsigned int editorId, ScriptObjectPatch& out) {
  if (worldId != kTallonWorld || !PatchesLandingSite()) {
    return false;
  }
  out = {};
  switch (editorId & 0x3FFFFFF) {
  case kStartOverworldCinematicTrigger:
    out.active = 0;
    return true;
  case kPlayerModelLoadedRelay:
  case kSaveStationBeamActor:
  case kBaseLightsEffect:
  case kSamusShipPlatform:
    out.active = 1;
    return true;
  case kReadySamusPlayerActor:
    out.clearConnections = true;
    return true;
  case kBackFromLoadTrigger:
    out.active = 1;
    out.removeTarget = kPlayerModelLoadedRelay;
    out.add = kBackFromLoadConnections;
    out.addCount = static_cast< int >(std::size(kBackFromLoadConnections));
    return true;
  default:
    return false;
  }
}

int GetScriptTimerSpawns(unsigned int worldId, unsigned int areaAssetId,
                         const ScriptTimerSpawn** out) {
  if (worldId != kTallonWorld || areaAssetId != kLandingSiteArea || !PatchesLandingSite()) {
    return 0;
  }
  *out = kLandingSiteTimers;
  return static_cast< int >(std::size(kLandingSiteTimers));
}

bool GetAutoEnabledElevator(unsigned int areaAssetId, unsigned int& relay) {
  const auto& s = S();
  if (!s.active || gpGameState == nullptr) {
    return false;
  }
  const ElevatorRelay* elevator = nullptr;
  for (const ElevatorRelay& candidate : kElevatorRelays) {
    if (candidate.area == areaAssetId) {
      elevator = &candidate;
      break;
    }
  }
  if (elevator == nullptr) {
    return false;
  }
  // A new game's starting items are only given once its first room has loaded.
  if (s.pendingNewGame) {
    for (const auto& item : s.active->startingItems) {
      if (item.grant.itemType == CPlayerState::kIT_ScanVisor && item.grant.capacity > 0) {
        return false;
      }
    }
  } else if (gpGameState->PlayerState()->HasPowerUp(CPlayerState::kIT_ScanVisor)) {
    return false;
  }
  relay = elevator->relay;
  return true;
}

void OnPickupCollected(CStateManager& mgr, int itemType, unsigned int editorId) {
  auto& s = S();
  if (!s.active) {
    return;
  }
  if (itemType >= CPlayerState::kIT_Truth && itemType <= CPlayerState::kIT_Newborn) {
    SyncArtifactLayers();
  }
  const int autosave = GetEffectiveAutosave();
  const bool artifact =
      itemType >= CPlayerState::kIT_Truth && itemType <= CPlayerState::kIT_Newborn;
  if ((autosave == kAutosaveAll && (artifact || PickupLocation(mgr, editorId) >= 0)) ||
      (autosave == kAutosaveMajor && IsMajorPickup(mgr, itemType, editorId))) {
    s.autosavePending = true;
    s.sincePickup = 0.f;
    Log.info("Autosave queued for item {} (object 0x{:08X})", itemType, editorId);
  }
}

void UpdateAutosave(CStateManager& mgr, float dt) {
  auto& s = S();
  if (!s.active || mgr.GetPlayer() == nullptr) {
    return;
  }
  const CPlayer& player = *mgr.GetPlayer();
  const float energy = mgr.GetPlayerState()->GetHealthInfo().GetHP();
  s.unhurtTime = energy < s.lastEnergy ? 0.f : s.unhurtTime + dt;
  s.lastEnergy = energy;
  s.sincePickup += dt;
  // A room come into other than by a door (a load, an elevator, a cross-world door) is entered
  // where Samus is when it is first seen.
  const uint32_t area = CurrentAreaAssetId(mgr);
  if (area != 0 && (!s.entry || s.entry->area != area)) {
    Session::RoomEntry entry;
    entry.area = area;
    entry.morphed = player.GetMorphballTransitionState() == CPlayer::kMS_Morphed;
    entry.yaw = PlayerYaw(player);
    entry.transform = player.GetTransform();
    s.entry = entry;
  }
  // The first frame it can be saved: game running with nothing in the way, on the ground, unhurt
  // for a moment, and the pickup settled.
  if (!s.autosavePending || s.unhurtTime < kAutosaveUnhurtTime ||
      s.sincePickup < kAutosavePickupSettleTime || !IsOnGround(player) ||
      !save_anywhere::WhyCantSave().empty()) {
    return;
  }
  s.autosavePending = false;
  WriteAutosave(mgr);
}

bool StripPickupInputLocks() { return S().active.has_value(); }

bool AllowSpawnPointInventoryReset() { return !S().active; }

bool AllowHintSystem() { return !S().active; }

bool AllowLayerChange(unsigned int areaSaveId, unsigned int layer) {
  return !S().active || areaSaveId != kArtifactTempleSaveId ||
         layer < static_cast< unsigned int >(kFirstArtifactLayer) ||
         layer > static_cast< unsigned int >(kLastArtifactLayer);
}

bool AllowScriptMsg(unsigned int worldId, unsigned int senderEditorId,
                    unsigned int targetEditorId) {
  unsigned int targetWorld = worldId;
  senderEditorId = merged::ToSourceEditorId(senderEditorId, worldId);
  targetEditorId = merged::ToSourceEditorId(targetEditorId, targetWorld);
  if (!S().active || worldId != kTallonWorld || (senderEditorId & 0x3FFFFFF) != kTruthRelay ||
      (targetEditorId & 0x3FFFFFF) != kTruthProgressRelay) {
    return true;
  }
  return gpGameState->PlayerState()->HasPowerUp(CPlayerState::kIT_Truth) ||
         TempleHolds(CPlayerState::kIT_Truth);
}

const PickupModelTemplate* FindPickupModel(std::string_view name) {
  const auto equals = [](std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
             return std::tolower(static_cast< unsigned char >(x)) ==
                    std::tolower(static_cast< unsigned char >(y));
           });
  };
  for (int i = 0; i < kModelAliasCount; ++i) {
    if (equals(name, kModelAliases[i].alias)) {
      name = kModelAliases[i].name;
      break;
    }
  }
  for (int i = 0; i < kPickupModelCount; ++i) {
    if (equals(name, kPickupModels[i].name)) {
      return &kPickupModels[i];
    }
  }
  return nullptr;
}

} // namespace metaforce::randomizer
