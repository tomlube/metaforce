#include "Metaforce/Randomizer/Randomizer.hpp"

#include "Metaforce/MergedWorld.hpp"
#include "Metaforce/Randomizer/CrossWorldDoors.hpp"
#include "Metaforce/Randomizer/Generator.hpp"
#include "Metaforce/Randomizer/Hooks.hpp"
#include "Metaforce/Randomizer/PickupTables.hpp"
#include "Metaforce/SaveAnywhere.hpp"
#include "Metaforce/Warp.hpp"

#include "Kyoto/Audio/CSfxManager.hpp"
#include "Kyoto/Math/CRelAngle.hpp"
#include "Kyoto/Math/CTransform4f.hpp"
#include "MetroidPrime/CEntityInfo.hpp"
#include "MetroidPrime/CMain.hpp"
#include "MetroidPrime/CMemoryCard.hpp"
#include "MetroidPrime/CScriptLayerManager.hpp"
#include "MetroidPrime/CStateManager.hpp"
#include "MetroidPrime/Player/CGameState.hpp"
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
#include <fstream>
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

// The Artifact of Truth has no layer of its own: a relay in Artifact Temple always shows its
// progress, so it's only allowed through once Truth is in hand, matching randomprime.
constexpr uint32_t kTallonWorld = 0x39F2DE28;
constexpr uint32_t kTruthRelay = 0x00100074;
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
  // Whether each shortcut chord was held on the last input seen, so holding one acts once.
  bool quickReloadHeld = false;
  bool quickSaveHeld = false;

  std::optional< Seed > active;
  std::unordered_map< uint64_t, int > pickupByObject;
  std::unordered_map< uint64_t, int > memoByObject;
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
  std::string traversedSlot;

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
  s.dockByKey.clear();
  s.memoText.clear();
  s.templeLocation = -1;
  s.pendingNewGame = false;
  s.traversedDoors.clear();
  s.traversedDocks.clear();
  s.traversedSlot.clear();
}

void SaveTraversed() {
  const auto& s = S();
  if (!s.active || s.traversedSlot.empty()) {
    return;
  }
  const json root{
      {"seed", s.active->hash},
      {"doors", std::vector< uint64_t >(s.traversedDoors.begin(), s.traversedDoors.end())},
      {"docks", std::vector< uint64_t >(s.traversedDocks.begin(), s.traversedDocks.end())}};
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
    s.memoText[i] = Widen(seed.locations[i].name + " acquired!");
    if (loc.mrea == templeArea) {
      s.templeLocation = i;
    }
  }
  for (int i = 0; i < static_cast< int >(seed.docks.size()); ++i) {
    s.dockByKey.emplace(DockKey(seed.docks[i].area, seed.docks[i].dock), i);
  }
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
    if (root.is_discarded() || root.value("format_version", 0) != Seed::kFormatVersion) {
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
    // The game over screen's Continue: kRM_StateSetter rebuilds the game state from the backup
    // the last save left behind (CMain::RefreshGameState), then loads it.
    Log.info("Quick reload: reloading the last save");
    gpGameState->WorldTransitionManager()->DisableTransition();
    gpMain->SetRestartMode(CMain::kRM_StateSetter);
    gpStateManager->QuitGame();
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

bool IsDockTraversed(unsigned int areaAssetId, int dock) {
  const auto& s = S();
  return s.active && s.traversedDocks.count(DockKey(areaAssetId, dock)) != 0;
}

int GetTraversedDockCount() {
  const auto& s = S();
  return s.active ? static_cast< int >(s.traversedDocks.size()) : 0;
}

bool IsDoorTraversed(unsigned int worldId, unsigned int editorId) {
  const auto& s = S();
  return s.active && s.traversedDoors.count(ObjectKey(worldId, editorId)) != 0;
}

bool ShortcutsHoldMap(bool r) {
  return r && S().active && (S().quickReload || S().quickSave);
}

void OnGameLoad() {
  if (gpGameState == nullptr) {
    return;
  }
  auto& s = S();
  const std::string key = SlotKey();
  if (gpGameState->GetInitPowerupsAtFirstSpawn()) {
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
// Puts the player on the seed's start position, facing its start direction.
void TeleportToStart(CStateManager& mgr) {
  const auto& s = S();
  if (!s.active || !s.active->startPosition || mgr.Player() == nullptr) {
    return;
  }
  const auto& pos = *s.active->startPosition;
  CPlayer* samus = mgr.Player();
  const CMatrix3f facing =
      s.active->startYaw
          ? CTransform4f::RotateZ(CRelAngle::FromRadians(*s.active->startYaw)).BuildMatrix3f()
          : samus->GetTransform().BuildMatrix3f();
  const CTransform4f xf(facing, CVector3f(pos[0], pos[1], pos[2]));
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

void OnWorldInitialized(CStateManager& mgr) {
  ApplyCrossWorldArrival(mgr);
  auto& s = S();
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

void OnPickupCollected(CStateManager&, int itemType) {
  if (S().active && itemType >= CPlayerState::kIT_Truth && itemType <= CPlayerState::kIT_Newborn) {
    SyncArtifactLayers();
  }
}

bool StripPickupInputLocks() { return S().active.has_value(); }

bool AllowSpawnPointInventoryReset() { return !S().active; }

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
