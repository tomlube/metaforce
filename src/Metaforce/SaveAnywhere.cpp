#include "Metaforce/SaveAnywhere.hpp"

#include "Metaforce/MergedWorld.hpp"

#include "Kyoto/CResFactory.hpp"
#include "Kyoto/Math/CTransform4f.hpp"
#include "Kyoto/Math/CVector3f.hpp"
#include "MetroidPrime/CGameArea.hpp"
#include "MetroidPrime/CMemoryCardDriver.hpp"
#include "MetroidPrime/CStateManager.hpp"
#include "MetroidPrime/CWorld.hpp"
#include "MetroidPrime/Cameras/CCameraManager.hpp"
#include "MetroidPrime/Player/CGameState.hpp"
#include "MetroidPrime/Player/CPlayer.hpp"
#include "MetroidPrime/Player/CWorldState.hpp"

#include <borealis/io.hpp>
#include <borealis/log.hpp>
#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <bit>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <optional>
#include <vector>

namespace metaforce::save_anywhere {
namespace {
namespace fs = std::filesystem;
using json = nlohmann::json;

constexpr borealis::Log Log{"save"};

// Positions of the most recent saves made with RequestSave; older ones are dropped.
constexpr size_t kMaxSpots = 64;

struct Spot {
  uint64_t playTime = 0; // bits of CGameState's play time when the save was written
  uint32_t world = 0;
  uint32_t area = 0; // MREA
  float pos[3] = {};
  float yaw = 0.f; // radians about Z, 0 facing +Y
  bool morphed = false;
};

fs::path sFile;
std::vector< Spot > sSpots;
// Set by RequestSave until the save screen closes: where the player was when they asked to save.
std::optional< Spot > sPending;
// What BeforeGameSaved changed for the save, put back by AfterGameSaved.
struct Replaced {
  uint32_t currentWorld;  // CGameState's current world
  uint32_t world;         // the world whose state was changed
  TAreaId area;           // its area index
  uint32_t desiredArea;   // its desired area
};
std::optional< Replaced > sReplaced;

// Card driver updates SaveSilently allows for each step. The card is synchronous on PC, so a step
// takes a handful; this only stops a driver that stopped making progress.
constexpr int kMaxCardUpdates = 1000;

uint64_t PlayTimeBits() { return std::bit_cast< uint64_t >(gpGameState->GetTotalPlayTime()); }

void Load() {
  const borealis::io::ReadResult file =
      borealis::io::read_file(borealis::io::fs_path_to_string(sFile));
  if (file.status != borealis::io::Status::Ok) {
    return;
  }
  const json root = json::parse(file.data, nullptr, false);
  if (!root.is_array()) {
    Log.warn("Ignoring malformed {}", sFile.string());
    return;
  }
  for (const json& entry : root) {
    if (!entry.is_object()) {
      continue;
    }
    Spot spot;
    spot.playTime = entry.value("play_time", uint64_t{0});
    spot.world = entry.value("world", 0u);
    spot.area = entry.value("area", 0u);
    const std::vector< float > pos = entry.value("pos", std::vector< float >{});
    if (pos.size() != 3) {
      continue;
    }
    for (int i = 0; i < 3; ++i) {
      spot.pos[i] = pos[i];
    }
    spot.yaw = entry.value("yaw", 0.f);
    spot.morphed = entry.value("morphed", false);
    sSpots.push_back(spot);
  }
}

void Save() {
  json root = json::array();
  for (const Spot& spot : sSpots) {
    root.push_back({
        {"play_time", spot.playTime},
        {"world", spot.world},
        {"area", spot.area},
        {"pos", {spot.pos[0], spot.pos[1], spot.pos[2]}},
        {"yaw", spot.yaw},
        {"morphed", spot.morphed},
    });
  }
  std::error_code ec;
  fs::create_directories(sFile.parent_path(), ec);
  std::ofstream file(sFile, std::ios::binary | std::ios::trunc);
  file << root.dump(2);
  if (!file) {
    Log.warn("Could not save {}", sFile.string());
  }
}

// The room the player is in, while gameplay runs.
const CGameArea* CurrentArea(const CStateManager& mgr) {
  const CWorld* world = mgr.GetWorld();
  const TAreaId area = mgr.GetNextAreaId();
  return world != nullptr && world->DoesAreaExist(area) ? &world->GetAreaAlways(area) : nullptr;
}

// `xf` in the current room, for a save made now.
Spot SpotAt(const CStateManager& mgr, const CTransform4f& xf, bool morphed) {
  Spot spot;
  spot.world = metaforce::merged::GetSourceWorld(mgr.GetWorld()->GetWorldAssetId(),
                                                 mgr.GetNextAreaId());
  spot.area = CurrentArea(mgr)->GetAreaAssetId();
  const CVector3f pos = xf.GetTranslation();
  spot.pos[0] = pos.GetX();
  spot.pos[1] = pos.GetY();
  spot.pos[2] = pos.GetZ();
  const CVector3f forward = xf.GetForward();
  spot.yaw = std::atan2(-forward.GetX(), forward.GetY());
  spot.morphed = morphed;
  return spot;
}

// Where the player is, for a save made now.
Spot CaptureSpot(const CStateManager& mgr) {
  const CPlayer& player = *mgr.GetPlayer();
  return SpotAt(mgr, player.GetTransform(),
                player.GetMorphballTransitionState() == CPlayer::kMS_Morphed);
}

CAssetId ResourceId(const char* name) {
  const SObjectTag* tag = gpResourceFactory->GetResourceIdByName(name);
  return tag != nullptr ? tag->GetId() : kInvalidAssetId;
}

// Updates the card driver until it reaches `done` or stops somewhere else, and returns where it
// stopped. Indexes the card's files when the card check finishes, as the save screen does.
EState Settle(CMemoryCardDriver& card, EState done) {
  for (int i = 0; i < kMaxCardUpdates; ++i) {
    const EState state = card.GetState();
    if (state == done) {
      return state;
    }
    if (state == kS_CardCheckDone) {
      card.IndexFiles();
      if (card.GetState() == kS_CardCheckDone) {
        return state;
      }
      continue;
    }
    if (state != kS_CardProbe && !CMemoryCardDriver::IsCardBusy(state)) {
      return state;
    }
    card.Update();
  }
  return card.GetState();
}

} // namespace

void Initialize(const fs::path& userPath) {
  sFile = userPath / "save_positions.json";
  Load();
}

std::string WhyCantSave() {
  CStateManager* mgr = gpStateManager;
  if (gpGameState == nullptr || mgr == nullptr || !mgr->IsFullyInitialized() ||
      mgr->GetPlayer() == nullptr || CurrentArea(*mgr) == nullptr) {
    return "There's no game in progress.";
  }
  if (gpGameState->GetCardSerial() == 0) {
    return "There's no memory card to save to.";
  }
  if (mgr->GetPlayer()->GetDeathTime() > 0.f) {
    return "Samus is dead.";
  }
  // The pause, map, logbook, save and message screens all hold the state manager's deferred
  // transition from the request until CMFGame::UnpauseGame, so it tells whether one is up or about
  // to be. GetInSaveUI is no use here: it latches whether the last save screen was left by saving
  // (the save station reads it afterwards), and CMFGame copies it on every unpause, so it stays set
  // long after any save screen has closed.
  if (mgr->GetDeferredStateTransition() != kSMT_InGame) {
    return "The game is paused.";
  }
  // Only a finished scan soft-pauses the game, while its text is up.
  if (mgr->GetGameState() != CStateManager::kGS_Running) {
    return "A scan is being shown.";
  }
  if (mgr->GetCameraManager()->IsInCinematicCamera()) {
    return "A cutscene is playing.";
  }
  if (mgr->GetEscapeSequenceTimer() > 0.f) {
    return "The escape sequence is under way.";
  }
  const CPlayer::EPlayerMorphBallState morph = mgr->GetPlayer()->GetMorphballTransitionState();
  if (morph == CPlayer::kMS_Morphing || morph == CPlayer::kMS_Unmorphing) {
    return "Samus is morphing.";
  }
  return {};
}

void RequestSave() {
  if (!WhyCantSave().empty()) {
    return;
  }
  CStateManager& mgr = *gpStateManager;
  sPending = CaptureSpot(mgr);
  mgr.EnterSaveGameScreen();
}

std::string SaveSilently(std::vector< uint8_t >* previous,
                         const std::optional< SpawnPoint >& spawn) {
  if (previous != nullptr) {
    previous->clear();
  }
  if (std::string reason = WhyCantSave(); !reason.empty()) {
    return reason;
  }
  const int slot = static_cast< int >(gpGameState->GetFileIdx());
  CMemoryCardDriver card(CMemoryCardSys::kCS_SlotA, ResourceId("TXTR_SaveBanner"),
                         ResourceId("TXTR_SaveIcon0"), ResourceId("TXTR_SaveIcon1"), false);
  card.StartCardProbe();
  EState state = Settle(card, kS_Ready);
  if (state != kS_Ready) {
    return fmt::format("the memory card couldn't be read (state {}, error {})",
                       static_cast< int >(state), static_cast< int >(card.GetError()));
  }
  // Like the save screen, never write to a card other than the one the game was loaded from.
  if (card.GetCardSerial() != gpGameState->GetCardSerial()) {
    return "the memory card isn't the one the game was loaded from";
  }
  if (previous != nullptr) {
    if (const u8* data = card.GetFileSlotData(slot)) {
      previous->assign(data, data + CMemoryCardDriver::kFileSlotSize);
    }
  }

  sPending = spawn ? SpotAt(*gpStateManager, spawn->transform, spawn->morphed)
                   : CaptureSpot(*gpStateManager);
  card.BuildExistingFileSlot(slot);
  card.StartFileCreateTransactional();
  state = Settle(card, kS_DriverClosed);
  sPending.reset();
  if (state != kS_DriverClosed) {
    return fmt::format("the memory card couldn't be written (state {}, error {})",
                       static_cast< int >(state), static_cast< int >(card.GetError()));
  }
  return {};
}

void OnSaveScreenClosed() { sPending.reset(); }

void BeforeGameSaved() {
  sReplaced.reset();
  CStateManager* mgr = gpStateManager;
  if (gpGameState == nullptr || mgr == nullptr || !mgr->IsFullyInitialized()) {
    return;
  }
  const CGameArea* area = CurrentArea(*mgr);
  const TAreaId areaId = mgr->GetNextAreaId();
  const bool foreign = metaforce::merged::IsForeignArea(areaId);
  if (area == nullptr || (!sPending && !foreign)) {
    return;
  }
  // A room appended from another region is saved as a room of that region, as if it had been
  // loaded on its own: the file select names the right region, and loading the save makes that
  // region the host. The room is also named by its MREA, which finds it however the regions end
  // up loaded, and lets a RequestSave spot be matched to it.
  const uint32_t world = metaforce::merged::GetSourceWorld(mgr->GetWorld()->GetWorldAssetId(),
                                                           areaId);
  CWorldState& state = gpGameState->StateForWorld(world);
  sReplaced = Replaced{gpGameState->CurrentWorldAssetId(), world, state.GetCurrentArea(),
                       state.GetDesiredAreaAssetId()};
  state.SetAreaId(TAreaId(metaforce::merged::GetSourceAreaIndex(areaId)));
  state.SetDesiredAreaAssetId(area->GetAreaAssetId());
  gpGameState->OverrideCurrentWorldId(world);
}

void AfterGameSaved() {
  if (sReplaced) {
    CWorldState& state = gpGameState->StateForWorld(sReplaced->world);
    state.SetAreaId(sReplaced->area);
    state.SetDesiredAreaAssetId(sReplaced->desiredArea);
    gpGameState->OverrideCurrentWorldId(sReplaced->currentWorld);
    sReplaced.reset();
  }
  if (!sPending) {
    return;
  }
  Spot spot = *sPending;
  spot.playTime = PlayTimeBits();
  std::erase_if(sSpots, [&](const Spot& s) { return s.playTime == spot.playTime; });
  sSpots.push_back(spot);
  if (sSpots.size() > kMaxSpots) {
    sSpots.erase(sSpots.begin(), sSpots.end() - kMaxSpots);
  }
  Save();
  Log.info("Saved at ({:.1f}, {:.1f}, {:.1f}) in room 0x{:08X}", spot.pos[0], spot.pos[1],
           spot.pos[2], spot.area);
}

void OnWorldInitialized(CStateManager& mgr) {
  // A request whose save screen never opened belongs to the game that was left.
  sPending.reset();
  if (gpGameState == nullptr || mgr.GetPlayer() == nullptr) {
    return;
  }
  const CGameArea* area = CurrentArea(mgr);
  if (area == nullptr) {
    return;
  }
  const uint64_t playTime = PlayTimeBits();
  const uint32_t world =
      metaforce::merged::GetSourceWorld(mgr.GetWorld()->GetWorldAssetId(), mgr.GetNextAreaId());
  for (const Spot& spot : sSpots) {
    if (spot.playTime != playTime || spot.world != world || spot.area != area->GetAreaAssetId()) {
      continue;
    }
    const CVector3f pos(spot.pos[0], spot.pos[1], spot.pos[2]);
    const CVector3f facing(-std::sin(spot.yaw), std::cos(spot.yaw), 0.f);
    mgr.Player()->Teleport(CTransform4f::LookAt(pos, pos + facing, CVector3f::Up()), mgr, true);
    if (spot.morphed) {
      mgr.Player()->SetSpawnedMorphBallState(CPlayer::kMS_Morphed, mgr);
    }
    Log.info("Loaded at ({:.1f}, {:.1f}, {:.1f}) in room 0x{:08X}", spot.pos[0], spot.pos[1],
             spot.pos[2], spot.area);
    return;
  }
}

} // namespace metaforce::save_anywhere
