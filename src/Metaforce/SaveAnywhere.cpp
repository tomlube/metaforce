#include "Metaforce/SaveAnywhere.hpp"

#include "Metaforce/MergedWorld.hpp"

#include "Kyoto/Math/CTransform4f.hpp"
#include "Kyoto/Math/CVector3f.hpp"
#include "MetroidPrime/CGameArea.hpp"
#include "MetroidPrime/CStateManager.hpp"
#include "MetroidPrime/CWorld.hpp"
#include "MetroidPrime/Cameras/CCameraManager.hpp"
#include "MetroidPrime/Player/CGameState.hpp"
#include "MetroidPrime/Player/CPlayer.hpp"
#include "MetroidPrime/Player/CWorldState.hpp"

#include <borealis/log.hpp>
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

uint64_t PlayTimeBits() { return std::bit_cast< uint64_t >(gpGameState->GetTotalPlayTime()); }

void Load() {
  std::ifstream file(sFile, std::ios::binary);
  if (!file) {
    return;
  }
  const json root = json::parse(file, nullptr, false);
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
  if (mgr->GetGameState() != CStateManager::kGS_Running || mgr->GetInSaveUI() ||
      mgr->GetWantsToEnterSaveGameScreen()) {
    return "The game is paused.";
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
  const CPlayer& player = *mgr.GetPlayer();
  Spot spot;
  spot.world = metaforce::merged::GetSourceWorld(mgr.GetWorld()->GetWorldAssetId(),
                                                 mgr.GetNextAreaId());
  spot.area = CurrentArea(mgr)->GetAreaAssetId();
  const CVector3f pos = player.GetTranslation();
  spot.pos[0] = pos.GetX();
  spot.pos[1] = pos.GetY();
  spot.pos[2] = pos.GetZ();
  const CVector3f forward = player.GetTransform().GetForward();
  spot.yaw = std::atan2(-forward.GetX(), forward.GetY());
  spot.morphed = player.GetMorphballTransitionState() == CPlayer::kMS_Morphed;
  sPending = spot;
  mgr.EnterSaveGameScreen();
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
