#include "Metaforce/Randomizer/CrossWorldDoors.hpp"

#include "Metaforce/MergedWorld.hpp"
#include "Metaforce/Randomizer/Hooks.hpp"
#include "Metaforce/Randomizer/Randomizer.hpp"
#include "Metaforce/Warp.hpp"

#include "Collision/CMaterialFilter.hpp"
#include "Collision/CMaterialList.hpp"
#include "Collision/CRayCastResult.hpp"
#include "Kyoto/Math/CTransform4f.hpp"
#include "Kyoto/Math/CVector3f.hpp"
#include "MetroidPrime/CGameArea.hpp"
#include "MetroidPrime/CGameCollision.hpp"
#include "MetroidPrime/CStateManager.hpp"
#include "MetroidPrime/CWorld.hpp"
#include "MetroidPrime/Cameras/CCameraFilterPass.hpp"
#include "MetroidPrime/Player/CPlayer.hpp"
#include "MetroidPrime/ScriptObjects/CScriptDock.hpp"
#include "MetroidPrime/ScriptObjects/CScriptDoor.hpp"
#include "MetroidPrime/TCastTo.hpp"

#include <borealis/log.hpp>

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

namespace metaforce::randomizer {
namespace {

constexpr borealis::Log Log{"randomizer"};

// How long the screen takes to fade out before the other world loads, and back in after.
constexpr float kFadeTime = 0.35f;
// How close to a door that's waiting to open, on its own side, the player has to come to go
// through it.
constexpr float kDoorApproachDistance = 4.f;
// How far outside a doorway's outline the player may be and still count as in it.
constexpr float kDoorwayMargin = 0.5f;
// Where the player comes out: this far inside the doorway, standing this far above its bottom
// edge. Morph Ball tunnels are narrower and come out morphed.
constexpr float kArrivalDepth = 3.f;
constexpr float kArrivalHeight = 1.f;
constexpr float kBallArrivalDepth = 2.f;
constexpr float kBallArrivalHeight = 0.5f;
// Below a door in a ceiling: the player's feet this far under the doorway.
constexpr float kCeilingArrivalDrop = 3.f;
// On top of a (closed) door in a floor.
constexpr float kFloorArrivalHeight = 0.5f;
// Finding the floor in front of a wall door: the ray starts this far under the top of the
// doorway and looks this far below its bottom edge.
constexpr float kFloorProbeMargin = 0.1f;
constexpr float kFloorProbeDepth = 4.f;

constexpr CStateManager::ECameraFilterStage kFadeStage = CStateManager::kCFS_Seven;

struct DockFrame {
  CVector3f center;
  CVector3f normal; // points out of the dock's room
  float radius;     // farthest corner from the center
  float bottom;     // lowest corner's height
};

bool GetDockFrame(const CGameArea& area, int dock, DockFrame& out) {
  if (dock < 0 || dock >= area.GetDockCount()) {
    return false;
  }
  const rstl::reserved_vector< CVector3f, 4 >& verts = area.GetDock(dock).GetPlaneVertices();
  if (verts.size() < 3) {
    return false;
  }
  CVector3f sum = CVector3f::Zero();
  out.bottom = verts[0].GetZ();
  for (int i = 0; i < verts.size(); ++i) {
    sum += verts[i];
    out.bottom = std::min(out.bottom, verts[i].GetZ());
  }
  out.center = sum * (1.f / static_cast< float >(verts.size()));
  out.radius = 0.f;
  for (int i = 0; i < verts.size(); ++i) {
    out.radius = std::max(out.radius, (verts[i] - out.center).Magnitude());
  }
  // Same winding as CScriptDock's crossing plane.
  const CVector3f normal = CVector3f::Cross(verts[1] - verts[0], verts[2] - verts[0]);
  if (!normal.CanBeNormalized()) {
    return false;
  }
  out.normal = normal.AsNormalized();
  return true;
}

uint64_t DockKey(uint32_t area, int dock) {
  return (static_cast< uint64_t >(area) << 32) | static_cast< uint32_t >(dock);
}

struct Arrival {
  uint32_t world;
  uint32_t area;
  int dock;
  bool morphBall;
  float yaw; // the player's facing when they left, kept for doors in floors and ceilings
};

struct State {
  // Cross-world doors of the active seed by DockKey(area, dock), as indices into its docks.
  std::string seedHash;
  std::unordered_map< uint64_t, int > doors;

  // The world the per-world state below belongs to.
  const CStateManager* mgr = nullptr;
  uint32_t world = 0;
  std::optional< CVector3f > lastPlayerPos;
  TAreaId lastPlayerArea = kInvalidAreaId;

  // The door being gone through: fading out, then waiting for the warp to start.
  std::optional< Arrival > leaving;
  float fadeLeft = 0.f;

  // Where the next world load should put the player.
  std::optional< Arrival > arriving;

  // The door the player came out of after a cross-world door, when it wasn't loaded yet to be
  // marked as gone through, and how many more frames to look for it.
  TAreaId pendingMarkArea = kInvalidAreaId;
  int pendingMarkDock = -1;
  int pendingMarkFrames = 0;
};

State sState;

const DockConnection* FindCrossWorldDoor(uint32_t world, uint32_t area, int dock) {
  const Seed* seed = GetActiveSeed();
  if (seed == nullptr) {
    sState.doors.clear();
    sState.seedHash.clear();
    return nullptr;
  }
  if (sState.seedHash != seed->hash) {
    sState.seedHash = seed->hash;
    sState.doors.clear();
    for (int i = 0; i < static_cast< int >(seed->docks.size()); ++i) {
      if (seed->docks[i].world != seed->docks[i].targetWorld) {
        sState.doors.emplace(DockKey(seed->docks[i].area, seed->docks[i].dock), i);
      }
    }
  }
  const auto it = sState.doors.find(DockKey(area, dock));
  if (it == sState.doors.end()) {
    return nullptr;
  }
  const DockConnection& door = seed->docks[it->second];
  // Into a region that was loaded along with this one: the door was redirected like any other,
  // and DockPortals takes the player through.
  if (merged::IsWorldLoaded(door.targetWorld)) {
    return nullptr;
  }
  return door.world == world ? &door : nullptr;
}

// Resets what's tracked per world when a new one was loaded.
void SyncWorld(const CStateManager& mgr) {
  const uint32_t world = mgr.GetWorld() != nullptr ? mgr.GetWorld()->GetWorldAssetId() : 0;
  if (sState.mgr == &mgr && sState.world == world) {
    return;
  }
  sState.mgr = &mgr;
  sState.world = world;
  sState.lastPlayerPos.reset();
  sState.lastPlayerArea = kInvalidAreaId;
  sState.leaving.reset();
  sState.pendingMarkFrames = 0;
}

// The door standing in dock `dock` of `area`, if it has one.
const CScriptDoor* FindDoor(const CStateManager& mgr, TAreaId area, int dock) {
  const CObjectList& objects = mgr.GetObjectListById(kOL_PlatformAndDoor);
  for (int i = objects.GetFirstObjectIndex(); i != -1; i = objects.GetNextObjectIndex(i)) {
    const CScriptDoor* door = TCastToConstPtr< CScriptDoor >(objects[i]);
    if (door == nullptr) {
      continue;
    }
    const CScriptDock* doorDock =
        TCastToConstPtr< CScriptDock >(mgr.GetObjectById(door->GetConnectedDockID()));
    if (doorDock != nullptr && doorDock->GetAreaId() == area && doorDock->GetDockId() == dock) {
      return door;
    }
  }
  return nullptr;
}

// Marks the door standing in dock `dock` of `area` as gone through. False if it has none loaded.
bool MarkDockDoor(const CStateManager& mgr, TAreaId area, int dock) {
  const CScriptDoor* door = FindDoor(mgr, area, dock);
  if (door == nullptr) {
    return false;
  }
  CAssetId world = mgr.GetWorld()->GetWorldAssetId();
  const uint editorId =
      merged::ToSourceEditorId(mgr.GetEditorIdForUniqueId(door->GetUniqueId()).value, world);
  MarkDoorTraversed(world, editorId);
  return true;
}

float PlayerYaw(const CPlayer& player) {
  const CVector3f forward = player.GetTransform().GetForward();
  return std::atan2(-forward.GetX(), forward.GetY());
}

void BeginLeaving(CStateManager& mgr, const DockConnection& door) {
  Log.info("Going through {} into {}", door.name, door.targetName);
  sState.leaving = Arrival{door.targetWorld, door.targetArea, door.targetDock,
                           door.targetMorphBall, PlayerYaw(*mgr.GetPlayer())};
  sState.fadeLeft = kFadeTime;
  mgr.CameraFilterPass(kFadeStage)
      .SetFilter(CCameraFilterPass::kFT_Blend, CCameraFilterPass::kFS_Fullscreen, kFadeTime,
                 CColor(0.f, 0.f, 0.f, 1.f), kInvalidAssetId);
}

// TAreaId of an MREA within a world, from the warp catalog.
std::optional< int > FindAreaIndex(uint32_t world, uint32_t area) {
  for (const warp::World& w : warp::GetWorlds()) {
    if (w.mlvl != world) {
      continue;
    }
    for (const warp::Area& a : w.areas) {
      if (a.mrea == area) {
        return a.index;
      }
    }
  }
  return std::nullopt;
}

void FinishLeaving() {
  if (!warp::CanWarp()) {
    return; // try again next frame
  }
  const Arrival arrival = *sState.leaving;
  sState.leaving.reset();
  const std::optional< int > index = FindAreaIndex(arrival.world, arrival.area);
  if (!index) {
    Log.error("Cross-world door target 0x{:08X} in world 0x{:08X} wasn't found", arrival.area,
              arrival.world);
    return;
  }
  sState.arriving = arrival;
  warp::RequestWarp(arrival.world, *index, std::nullopt);
}

} // namespace

std::optional< CTransform4f > DockArrivalTransform(const CStateManager& mgr,
                                                   const CGameArea& area, int dock,
                                                   bool morphBall, float yaw) {
  DockFrame frame;
  if (!GetDockFrame(area, dock, frame)) {
    return std::nullopt;
  }
  CVector3f spawn = frame.center;
  CVector3f facing(-std::sin(yaw), std::cos(yaw), 0.f);
  if (frame.normal.GetZ() >= 0.7f) {
    // A door in the ceiling: drop in below it.
    spawn.SetZ(frame.center.GetZ() - kCeilingArrivalDrop);
  } else if (frame.normal.GetZ() <= -0.7f) {
    // A door in the floor: come up and stand on it, closed.
    spawn.SetZ(frame.center.GetZ() + kFloorArrivalHeight);
  } else {
    CVector3f inward = -frame.normal;
    inward.SetZ(0.f);
    inward = inward.AsNormalized();
    const float depth = morphBall ? kBallArrivalDepth : kArrivalDepth;
    const float height = morphBall ? kBallArrivalHeight : kArrivalHeight;
    spawn = frame.center + inward * depth;
    spawn.SetZ(std::min(frame.bottom + height, frame.center.GetZ()));
    // The floor past the door needn't be level with it (Tallon's Transport Tunnel B climbs
    // right away), so stand on whatever floor is there, looking down from the top of the door.
    const float top = 2.f * frame.center.GetZ() - frame.bottom - kFloorProbeMargin;
    const CVector3f probe(spawn.GetX(), spawn.GetY(), top);
    const CRayCastResult floor = CGameCollision::RayStaticIntersection(
        mgr, probe, CVector3f::Down(), top - frame.bottom + kFloorProbeDepth,
        CMaterialFilter::MakeInclude(CMaterialList(kMT_Solid)));
    if (floor.IsValid()) {
      spawn.SetZ(floor.GetPoint().GetZ() + height);
    }
    facing = inward;
  }
  return CTransform4f::LookAt(spawn, spawn + facing, CVector3f::Up());
}

bool OnCrossWorldDoorOpen(CStateManager& mgr, int area, int dock) {
  const CWorld* world = mgr.GetWorld();
  if (world == nullptr || !world->DoesAreaExist(TAreaId(area))) {
    return false;
  }
  const uint32_t areaAsset = world->GetAreaAlways(TAreaId(area)).GetAreaAssetId();
  // The door then waits to open for good, and UpdateCrossWorldDoors takes the player through
  // once they walk up to it.
  return FindCrossWorldDoor(world->GetWorldAssetId(), areaAsset, dock) != nullptr;
}

void UpdateCrossWorldDoors(CStateManager& mgr, float dt) {
  if (GetActiveSeed() == nullptr || mgr.GetWorld() == nullptr || mgr.GetPlayer() == nullptr) {
    return;
  }
  SyncWorld(mgr);
  if (sState.pendingMarkFrames > 0) {
    --sState.pendingMarkFrames;
    if (MarkDockDoor(mgr, sState.pendingMarkArea, sState.pendingMarkDock)) {
      sState.pendingMarkFrames = 0;
    }
  }
  if (sState.leaving) {
    sState.fadeLeft -= dt;
    if (sState.fadeLeft <= 0.f) {
      FinishLeaving();
    }
    return;
  }

  const CWorld& world = *mgr.GetWorld();
  const TAreaId current = mgr.GetNextAreaId();
  const CVector3f pos = mgr.GetPlayer()->GetTranslation();
  const std::optional< CVector3f > lastPos =
      sState.lastPlayerArea == current ? sState.lastPlayerPos : std::nullopt;
  sState.lastPlayerPos = pos;
  sState.lastPlayerArea = current;
  if (!world.DoesAreaExist(current)) {
    return;
  }
  const CGameArea& area = world.GetAreaAlways(current);
  for (int dock = 0; dock < area.GetDockCount(); ++dock) {
    const DockConnection* door =
        FindCrossWorldDoor(world.GetWorldAssetId(), area.GetAreaAssetId(), dock);
    DockFrame frame;
    if (door == nullptr || !GetDockFrame(area, dock, frame)) {
      continue;
    }
    const CVector3f offset = pos - frame.center;
    const float dist = CVector3f::Dot(offset, frame.normal);
    const float lateral = (offset - frame.normal * dist).Magnitude();
    if (lateral > frame.radius + kDoorwayMargin) {
      continue;
    }
    // Up against a door the game would open right now, or through a doorway without a door.
    // A door that was shot is called off again when it's closed, locked (as in an enemy
    // encounter) or deactivated, just as it would be in the vanilla game.
    const CScriptDoor* gameDoor = FindDoor(mgr, current, dock);
    bool go = false;
    if (gameDoor != nullptr) {
      go = gameDoor->GetActive() && !gameDoor->IsOpen() && gameDoor->IsWaitingToOpen() &&
           dist > -kDoorApproachDistance;
    } else {
      go = lastPos && CVector3f::Dot(*lastPos - frame.center, frame.normal) <= 0.f && dist > 0.f;
    }
    if (go) {
      MarkDockDoor(mgr, current, dock);
      BeginLeaving(mgr, *door);
      return;
    }
  }
}

void OnPlayerCrossedDock(CStateManager& mgr, int area, int dock) {
  const CWorld* world = mgr.GetWorld();
  if (GetActiveSeed() == nullptr || world == nullptr || !world->DoesAreaExist(TAreaId(area)) ||
      dock < 0 || dock >= world->GetAreaAlways(TAreaId(area)).GetDockCount()) {
    return;
  }
  MarkDockDoor(mgr, TAreaId(area), dock);
  MarkDockTraversed(world->GetAreaAlways(TAreaId(area)).GetAreaAssetId(), dock);
  const IGameArea::Dock& gameDock = world->GetAreaAlways(TAreaId(area)).GetDock(dock);
  if (gameDock.GetDockRefs().empty()) {
    return;
  }
  const int ref = gameDock.GetReferenceCount();
  const TAreaId other = gameDock.GetConnectedAreaId(ref);
  if (world->DoesAreaExist(other)) {
    MarkDockDoor(mgr, other, gameDock.GetOtherDockNumber(ref));
    MarkDockTraversed(world->GetAreaAlways(other).GetAreaAssetId(),
                      gameDock.GetOtherDockNumber(ref));
  }
}

void ApplyCrossWorldArrival(CStateManager& mgr) {
  // A new state manager can reuse the last one's address, so start this world's tracking over.
  sState.mgr = nullptr;
  SyncWorld(mgr);
  if (!sState.arriving) {
    return;
  }
  const Arrival arrival = *sState.arriving;
  sState.arriving.reset();
  const CWorld* world = mgr.GetWorld();
  CPlayer* player = mgr.Player();
  if (world == nullptr || player == nullptr || world->GetWorldAssetId() != arrival.world) {
    return;
  }
  const TAreaId areaId = world->GetAreaId(arrival.area);
  std::optional< CTransform4f > xf;
  if (world->DoesAreaExist(areaId)) {
    xf = DockArrivalTransform(mgr, world->GetAreaAlways(areaId), arrival.dock,
                              arrival.morphBall, arrival.yaw);
  }
  if (!xf) {
    Log.error("Arrival dock {} of 0x{:08X} wasn't found", arrival.dock, arrival.area);
    return;
  }

  if (!MarkDockDoor(mgr, areaId, arrival.dock)) {
    sState.pendingMarkArea = areaId;
    sState.pendingMarkDock = arrival.dock;
    sState.pendingMarkFrames = 60;
  }

  player->Teleport(*xf, mgr, true);
  if (arrival.morphBall) {
    player->SetSpawnedMorphBallState(CPlayer::kMS_Morphed, mgr);
  }
  CCameraFilterPass& fade = mgr.CameraFilterPass(kFadeStage);
  fade.SetFilter(CCameraFilterPass::kFT_Blend, CCameraFilterPass::kFS_Fullscreen, 0.f,
                 CColor(0.f, 0.f, 0.f, 1.f), kInvalidAssetId);
  fade.DisableFilter(kFadeTime);
  Log.info("Arrived through dock {} of 0x{:08X}", arrival.dock, arrival.area);
}

} // namespace metaforce::randomizer
