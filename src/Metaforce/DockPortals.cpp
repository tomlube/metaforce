#include "Metaforce/DockPortals.hpp"

#include "Metaforce/Randomizer/Hooks.hpp"

#include "Kyoto/Graphics/CGraphics.hpp"
#include "Kyoto/Math/CFrustumPlanes.hpp"
#include "Kyoto/Math/CRelAngle.hpp"
#include "Kyoto/Math/CTransform4f.hpp"
#include "Kyoto/Math/CVector3f.hpp"
#include "MetroidPrime/CGameArea.hpp"
#include "MetroidPrime/CStateManager.hpp"
#include "MetroidPrime/CWorld.hpp"
#include "MetroidPrime/Cameras/CCameraManager.hpp"
#include "MetroidPrime/Cameras/CGameCamera.hpp"
#include "MetroidPrime/Player/CPlayer.hpp"
#include "MetroidPrime/Player/CPlayerState.hpp"
#include "MetroidPrime/ScriptObjects/CScriptDock.hpp"
#include "MetroidPrime/ScriptObjects/CScriptDoor.hpp"
#include "MetroidPrime/TCastTo.hpp"
#include "MetroidPrime/Weapons/CGameProjectile.hpp"
#include "MetaRender/CCubeRenderer.hpp"

#include <dolphin/gx/GXPixel.h>

#include <borealis/log.hpp>
#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace metaforce::portals {
namespace {

constexpr borealis::Log Log{"DockPortals"};

// Docks closer than this are the two sides of one doorway.
constexpr float kCoincidentDistance = 0.5f;
// How far from a moved doorway a trailing camera still counts as having gone through it.
constexpr float kCameraThroughDistance = 8.f;
// How close to a moved doorway, on the near side, the camera switches to drawing from the far
// room. Closer than this the near clip plane cuts into the doorway's depth seal, and the near
// room's own geometry past its doorway (floors often run on under the door) shows over the
// portal. Three times the first person near clip distance covers the near plane's corners.
constexpr float kCameraEarlyThroughDistance = 0.6f;
// How far outside a doorway's outline the camera may be and still switch early.
constexpr float kDoorwayMargin = 0.5f;
// How far past a moved doorway's plane the player may be and still be carried through it.
constexpr float kPlayerCrossingDepth = 3.f;
// How far past a doorway's plane a shot may start and still be carried through it, for shots
// fired with the gun already through the doorway.
constexpr float kProjectileCrossingDepth = 2.f;

struct Portal {
  TAreaId area;
  CTransform4f cameraXf; // camera in root space -> camera for drawing `area`
  rstl::reserved_vector< CVector3f, 4 > doorway; // in root space
};

struct Frame {
  bool valid = false;
  TAreaId root = kInvalidAreaId;
  CTransform4f rootXf = CTransform4f::Identity(); // camera -> camera in root space
  bool shifted = false; // the root isn't the player's area
  bool skipMainSky = false;
  // In a frame whose main pass leaves out the sky: the area whose sky the portal passes draw
  // instead, and the camera transform it's drawn from (rootXf or a portal's cameraXf).
  TAreaId skyArea = kInvalidAreaId;
  CTransform4f skyXf = CTransform4f::Identity();
  std::vector< Portal > portals;
  std::vector< std::pair< TAreaId, CTransform4f > > areaXfs;
  int pass = -1; // portal being drawn, -1 for the main pass
};

Frame sFrame;

// Set METAFORCE_PORTAL_LOG=1 to log the portal state of each frame spent near a moved door.
const bool sLogEnabled = std::getenv("METAFORCE_PORTAL_LOG") != nullptr;
// Diagnostics: METAFORCE_PORTAL_NOSKIPSKY=1 always draws the main pass sky.
const bool sNeverSkipSky = std::getenv("METAFORCE_PORTAL_NOSKIPSKY") != nullptr;
int sLogFramesLeft = 0;
bool sLogThisFrame = false;

std::string FormatVec(const CVector3f& v) {
  return fmt::format("({:.2f}, {:.2f}, {:.2f})", v.GetX(), v.GetY(), v.GetZ());
}

const char* OcclusionName(const CGameArea& area) {
  if (!area.IsPostConstructed()) {
    return "unloaded";
  }
  return area.GetOcclusionState() == CGameArea::kOS_Visible ? "visible" : "occluded";
}

struct DockFrame {
  CVector3f center;
  CVector3f normal; // points out of the dock's room
  float radius;     // farthest corner from the center
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
  for (int i = 0; i < verts.size(); ++i) {
    sum += verts[i];
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

bool IsMovedDock(const CWorld& world, TAreaId area, int dock) {
  CTransform4f unused = CTransform4f::Identity();
  return GetDockTransform(world, area, dock, unused);
}

void AddAreaTransform(TAreaId area, const CTransform4f& xf) {
  for (const auto& entry : sFrame.areaXfs) {
    if (entry.first == area) {
      return;
    }
  }
  sFrame.areaXfs.emplace_back(area, xf);
}

} // namespace

bool GetDockTransform(const CWorld& world, TAreaId area, int dock, CTransform4f& out) {
  if (!world.DoesAreaExist(area)) {
    return false;
  }
  const CGameArea& from = world.GetAreaAlways(area);
  if (dock < 0 || dock >= from.GetDockCount()) {
    return false;
  }
  const IGameArea::Dock& gameDock = from.GetDock(dock);
  if (gameDock.GetDockRefs().empty()) {
    return false;
  }
  const int ref = gameDock.GetReferenceCount();
  const TAreaId toId = gameDock.GetConnectedAreaId(ref);
  if (!world.DoesAreaExist(toId)) {
    return false;
  }
  DockFrame a;
  DockFrame b;
  if (!GetDockFrame(from, dock, a) ||
      !GetDockFrame(world.GetAreaAlways(toId), gameDock.GetOtherDockNumber(ref), b)) {
    return false;
  }
  if ((a.center - b.center).Magnitude() < kCoincidentDistance) {
    return false;
  }

  // Leaving through A means arriving through B facing into B's room, so A's normal maps onto the
  // reverse of B's. The room randomizer only pairs wall docks with wall docks and floors with
  // ceilings, so a turn about Z covers every pairing; for floor and ceiling docks no turn is
  // needed.
  float angle = 0.f;
  if (std::fabs(a.normal.GetZ()) < 0.7f) {
    angle = std::atan2(-b.normal.GetY(), -b.normal.GetX()) -
            std::atan2(a.normal.GetY(), a.normal.GetX());
  }
  const CTransform4f rotation = CTransform4f::RotateZ(CRelAngle::FromRadians(angle));
  out = CTransform4f::Translate(b.center) * rotation * CTransform4f::Translate(-a.center);
  return true;
}

void OnPlayerCrossedDock(CStateManager& mgr, TAreaId area, int dock) {
  randomizer::OnPlayerCrossedDock(mgr, area.Value(), dock);
  CTransform4f xf = CTransform4f::Identity();
  if (!GetDockTransform(*mgr.GetWorld(), area, dock, xf)) {
    return;
  }
  CPlayer* player = mgr.Player();
  if (player == nullptr) {
    return;
  }
  Log.debug("Carrying the player through moved dock {} of area {}", dock, area.Value());
  if (sLogEnabled) {
    Log.info("[portal] frame {}: crossing dock {} of area {}, player {} -> {}",
             mgr.GetUpdateFrameIndex(), dock, area.Value(), FormatVec(player->GetTranslation()),
             FormatVec(xf * player->GetTranslation()));
    sLogFramesLeft = 6;
  }
  player->TransformThroughDock(xf, mgr);
}

} // namespace metaforce::portals

namespace metaforce::portals {

void UpdatePlayerCrossing(CStateManager& mgr) {
  CWorld* world = mgr.World();
  CPlayer* player = mgr.Player();
  const TAreaId current = mgr.GetNextAreaId();
  if (world == nullptr || player == nullptr || !world->DoesAreaExist(current)) {
    return;
  }
  const CGameArea& area = world->GetAreaAlways(current);
  const CVector3f pos = player->GetTranslation();
  for (int dock = 0; dock < area.GetDockCount(); ++dock) {
    CTransform4f xf = CTransform4f::Identity();
    DockFrame dockFrame;
    if (!GetDockTransform(*world, current, dock, xf) || !GetDockFrame(area, dock, dockFrame)) {
      continue;
    }
    const CVector3f offset = pos - dockFrame.center;
    const float planeDist = CVector3f::Dot(offset, dockFrame.normal);
    if (planeDist <= 0.f || planeDist > kPlayerCrossingDepth ||
        (offset - dockFrame.normal * planeDist).Magnitude() > dockFrame.radius + kDoorwayMargin) {
      continue;
    }
    const IGameArea::Dock& gameDock = area.GetDock(dock);
    const int ref = gameDock.GetReferenceCount();
    const TAreaId next = gameDock.GetConnectedAreaId(ref);
    if (!world->DoesAreaExist(next) || !world->GetAreaAlways(next).IsPostConstructed()) {
      continue;
    }

    // What CScriptDock does when the player crosses, plus the carry through.
    OnPlayerCrossedDock(mgr, current, dock);
    mgr.SetCurrentAreaId(next);
    const int otherDock = gameDock.GetOtherDockNumber(ref);
    CObjectList& objects = *world->Area(next)->ObjectList();
    for (int i = objects.GetFirstObjectIndex(); i != -1; i = objects.GetNextObjectIndex(i)) {
      if (CScriptDock* nextDock = TCastToPtr< CScriptDock >(objects[i])) {
        if (nextDock->GetDockId() == otherDock) {
          nextDock->SetLoadConnected(mgr, true);
          break;
        }
      }
    }
    return;
  }
}

namespace {

// Whether a closed door of `area` stands in `dock`'s doorway. A shot fired from right up against
// a closed door can start past the doorway's plane; it has to hit the door, not go through.
bool IsDoorwayClosed(CStateManager& mgr, const CGameArea& area, int dock, const CVector3f& point) {
  const CObjectList& objects = *area.GetPostConstructed()->mAreaObjectList;
  for (int i = objects.GetFirstObjectIndex(); i != -1; i = objects.GetNextObjectIndex(i)) {
    const CScriptDoor* door = TCastToConstPtr< CScriptDoor >(objects[i]);
    if (door == nullptr || !door->GetActive() ||
        !door->GetMaterialList().HasMaterial(kMT_Solid)) {
      continue;
    }
    bool inDoorway = false;
    if (const CScriptDock* doorDock =
            TCastToConstPtr< CScriptDock >(mgr.GetObjectById(door->GetConnectedDockID()))) {
      inDoorway = doorDock->GetAreaId() == area.GetId() && doorDock->GetDockId() == dock;
    }
    if (!inDoorway) {
      if (const rstl::optional_object< CAABox > bounds = door->GetTouchBounds()) {
        inDoorway = bounds->PointInside(point);
      }
    }
    if (inDoorway) {
      return true;
    }
  }
  return false;
}

} // namespace

void UpdateProjectileCrossing(CStateManager& mgr, CGameProjectile& projectile) {
  CWorld* world = mgr.World();
  const TAreaId current = projectile.GetCurrentAreaId();
  if (!TracksProjectileAreas() || world == nullptr || !world->DoesAreaExist(current)) {
    return;
  }
  const CGameArea& area = world->GetAreaAlways(current);
  if (!area.IsPostConstructed()) {
    return;
  }
  const CVector3f prev = projectile.GetPreviousPos();
  const CVector3f pos = projectile.GetTranslation();
  const float travel = (pos - prev).Magnitude();
  for (int dock = 0; dock < area.GetDockCount(); ++dock) {
    DockFrame dockFrame;
    const IGameArea::Dock& gameDock = area.GetDock(dock);
    if (gameDock.GetDockRefs().empty() || !GetDockFrame(area, dock, dockFrame)) {
      continue;
    }
    const float dist = CVector3f::Dot(pos - dockFrame.center, dockFrame.normal);
    const float prevDist = CVector3f::Dot(prev - dockFrame.center, dockFrame.normal);
    if (dist <= 0.f || dist > travel + kProjectileCrossingDepth ||
        prevDist > kProjectileCrossingDepth) {
      continue;
    }
    // Where it met the doorway, or where it started if it was already through.
    CVector3f crossing = prev;
    if (prevDist < 0.f) {
      crossing = prev + (pos - prev) * (-prevDist / (dist - prevDist));
    }
    const CVector3f offset = crossing - dockFrame.center;
    const CVector3f lateral = offset - dockFrame.normal * CVector3f::Dot(offset, dockFrame.normal);
    if (lateral.Magnitude() > dockFrame.radius) {
      continue;
    }
    const int ref = gameDock.GetReferenceCount();
    const TAreaId next = gameDock.GetConnectedAreaId(ref);
    if (!world->DoesAreaExist(next) || !world->GetAreaAlways(next).IsPostConstructed() ||
        IsDoorwayClosed(mgr, area, dock, crossing)) {
      continue;
    }
    CTransform4f xf = CTransform4f::Identity();
    if (GetDockTransform(*world, current, dock, xf)) {
      if (sLogEnabled) {
        Log.info("[portal] frame {}: carrying shot {} through dock {} of area {}, {} -> {}",
                 mgr.GetUpdateFrameIndex(), projectile.GetUniqueId().Value(), dock,
                 current.Value(), FormatVec(pos), FormatVec(xf * pos));
      }
      projectile.TransformThroughDock(xf, crossing, next, mgr);
    } else {
      mgr.SetActorAreaId(projectile, next);
    }
    return;
  }
}

bool TracksProjectileAreas() { return sFrame.valid; }

namespace {

struct Separations {
  const CWorld* world = nullptr;
  uint frame = 0xFFFFFFFF;
  std::vector< std::pair< TAreaId, TAreaId > > pairs;
};

Separations sSeparations;

// Whether a dock of `from` leads to `to` through a doorway that stayed where it is.
bool HasUnmovedDoorTo(const CWorld& world, const CGameArea& from, TAreaId to) {
  for (int dock = 0; dock < from.GetDockCount(); ++dock) {
    const IGameArea::Dock& gameDock = from.GetDock(dock);
    if (!gameDock.GetDockRefs().empty() &&
        gameDock.GetConnectedAreaId(gameDock.GetReferenceCount()) == to &&
        !IsMovedDock(world, from.GetId(), dock)) {
      return true;
    }
  }
  return false;
}

bool HaveMeetingDoorways(const CGameArea& a, const CGameArea& b) {
  for (int i = 0; i < a.GetDockCount(); ++i) {
    DockFrame frameA;
    if (!GetDockFrame(a, i, frameA)) {
      continue;
    }
    for (int j = 0; j < b.GetDockCount(); ++j) {
      DockFrame frameB;
      if (GetDockFrame(b, j, frameB) &&
          (frameA.center - frameB.center).Magnitude() < kCoincidentDistance) {
        return true;
      }
    }
  }
  return false;
}

// Worked out once per update frame; doors only move and rooms only load between frames.
const Separations& GetSeparations(const CStateManager& mgr) {
  const CWorld* world = mgr.GetWorld();
  const uint frame = mgr.GetUpdateFrameIndex();
  if (sSeparations.world == world && sSeparations.frame == frame) {
    return sSeparations;
  }
  sSeparations.world = world;
  sSeparations.frame = frame;
  sSeparations.pairs.clear();
  if (world == nullptr) {
    return sSeparations;
  }
  std::vector< const CGameArea* > alive;
  for (CGameArea::CConstChainIterator it = world->GetChainHead(CWorld::kC_Alive);
       it != CWorld::skGlobalEnd; ++it) {
    if (it->IsPostConstructed()) {
      alive.push_back(&*it);
    }
  }
  for (size_t i = 0; i < alive.size(); ++i) {
    for (size_t j = i + 1; j < alive.size(); ++j) {
      const CGameArea& a = *alive[i];
      const CGameArea& b = *alive[j];
      if (HaveMeetingDoorways(a, b) && !HasUnmovedDoorTo(*world, a, b.GetId()) &&
          !HasUnmovedDoorTo(*world, b, a.GetId())) {
        sSeparations.pairs.emplace_back(a.GetId(), b.GetId());
      }
    }
  }
  return sSeparations;
}

} // namespace

bool AreAreasSeparated(const CStateManager& mgr, TAreaId a, TAreaId b) {
  if (a == b || a == kInvalidAreaId || b == kInvalidAreaId) {
    return false;
  }
  for (const auto& pair : GetSeparations(mgr).pairs) {
    if ((pair.first == a && pair.second == b) || (pair.first == b && pair.second == a)) {
      return true;
    }
  }
  return false;
}

bool HasSeparatedAreas(const CStateManager& mgr) { return !GetSeparations(mgr).pairs.empty(); }

void PrepareFrame(const CStateManager& mgr) {
  Frame& frame = sFrame;
  frame.valid = false;
  frame.pass = -1;
  frame.skipMainSky = false;
  frame.skyArea = kInvalidAreaId;
  frame.portals.clear();
  frame.areaXfs.clear();
  const CWorld* world = mgr.GetWorld();
  if (world == nullptr) {
    return;
  }
  const TAreaId current = world->GetCurrentAreaId();
  if (!world->DoesAreaExist(current)) {
    return;
  }
  // Without moved doors nearby, drawing is left exactly as the game does it.
  bool anyMoved = false;
  for (CGameArea::CConstChainIterator it = world->GetChainHead(CWorld::kC_Alive);
       it != CWorld::GetAliveAreasEnd() && !anyMoved; ++it) {
    for (int dock = 0; dock < it->GetDockCount() && !anyMoved; ++dock) {
      anyMoved = IsMovedDock(*world, it->GetId(), dock);
    }
  }
  if (!anyMoved) {
    return;
  }
  frame.root = current;
  frame.rootXf = CTransform4f::Identity();
  frame.shifted = false;

  // A camera trailing the player can still be on the other side of a moved door the player just
  // went through. Draw from the room the camera is really in. A camera right at a moved doorway
  // also draws from the far room, see kCameraEarlyThroughDistance.
  const CGameArea& currentArea = world->GetAreaAlways(current);
  const CTransform4f camXf = mgr.GetCameraManager()->GetCurrentCameraTransform(mgr);
  const CVector3f camPos = camXf.GetTranslation();
  const CVector3f camForward = camXf.GetForward();
  for (int dock = 0; dock < currentArea.GetDockCount(); ++dock) {
    CTransform4f xf = CTransform4f::Identity();
    DockFrame dockFrame;
    if (!GetDockTransform(*world, current, dock, xf) || !GetDockFrame(currentArea, dock, dockFrame)) {
      continue;
    }
    const CVector3f offset = camPos - dockFrame.center;
    const IGameArea::Dock& gameDock = currentArea.GetDock(dock);
    const TAreaId through = gameDock.GetConnectedAreaId(gameDock.GetReferenceCount());
    const float planeDist = CVector3f::Dot(offset, dockFrame.normal);
    const bool crossed = planeDist > 0.f && offset.Magnitude() < kCameraThroughDistance;
    // Only when looking toward the doorway: just after coming through a door the camera is as
    // close to it, but faces into the room it's in, and that room is what it should draw.
    const bool atDoorway = planeDist > -kCameraEarlyThroughDistance && planeDist <= 0.f &&
                           CVector3f::Dot(camForward, dockFrame.normal) > 0.f &&
                           (offset - dockFrame.normal * planeDist).Magnitude() <
                               dockFrame.radius + kDoorwayMargin;
    if ((crossed || atDoorway) &&
        world->GetAreaAlways(through).GetOcclusionState() == CGameArea::kOS_Visible) {
      frame.root = through;
      frame.rootXf = xf;
      frame.shifted = true;
      break;
    }
  }

  const CGameArea& root = world->GetAreaAlways(frame.root);
  AddAreaTransform(frame.root, frame.rootXf);
  for (int dock = 0; dock < root.GetDockCount(); ++dock) {
    const IGameArea::Dock& gameDock = root.GetDock(dock);
    if (gameDock.GetDockRefs().empty()) {
      continue;
    }
    const TAreaId target = gameDock.GetConnectedAreaId(gameDock.GetReferenceCount());
    if (!world->DoesAreaExist(target)) {
      continue;
    }
    CTransform4f xf = CTransform4f::Identity();
    if (!GetDockTransform(*world, frame.root, dock, xf)) {
      AddAreaTransform(target, frame.rootXf);
      continue;
    }
    const CGameArea& area = world->GetAreaAlways(target);
    if (!area.IsPostConstructed() || area.GetOcclusionState() != CGameArea::kOS_Visible) {
      continue;
    }
    Portal portal{target, xf * frame.rootXf, gameDock.GetPlaneVertices()};
    AddAreaTransform(target, portal.cameraXf);
    frame.portals.push_back(portal);
  }
  frame.valid = true;

  sLogThisFrame = false;
  if (sLogEnabled) {
    const CVector3f playerPos = mgr.GetPlayer()->GetTranslation();
    std::string docks;
    bool near = sLogFramesLeft > 0;
    for (int dock = 0; dock < currentArea.GetDockCount(); ++dock) {
      DockFrame dockFrame;
      if (!IsMovedDock(*world, current, dock) || !GetDockFrame(currentArea, dock, dockFrame)) {
        continue;
      }
      const float camDist = CVector3f::Dot(camPos - dockFrame.center, dockFrame.normal);
      const float playerDist = CVector3f::Dot(playerPos - dockFrame.center, dockFrame.normal);
      if ((camPos - dockFrame.center).Magnitude() < 6.f ||
          (playerPos - dockFrame.center).Magnitude() < 6.f) {
        near = true;
        docks += fmt::format(" dock{}[cam {:.3f} player {:.3f}]", dock, camDist, playerDist);
      }
    }
    if (near) {
      if (sLogFramesLeft > 0) {
        --sLogFramesLeft;
      }
      sLogThisFrame = true;
      std::string portals;
      for (const Portal& portal : frame.portals) {
        portals += fmt::format(" {}({})", portal.area.Value(),
                               OcclusionName(world->GetAreaAlways(portal.area)));
      }
      std::string alive;
      for (CGameArea::CConstChainIterator it = world->GetChainHead(CWorld::kC_Alive);
           it != CWorld::GetAliveAreasEnd(); ++it) {
        alive += fmt::format(" {}({})", it->GetId().Value(), OcclusionName(*it));
      }
      Log.info("[portal] frame {}: world area {} player area {} root {}{} cam {} player {}{} | "
               "portals:{} | alive:{}",
               mgr.GetUpdateFrameIndex(), current.Value(), mgr.GetNextAreaId().Value(),
               frame.root.Value(), frame.shifted ? " (shifted)" : "", FormatVec(camPos),
               FormatVec(playerPos), docks, portals, alive);
    }
  }
}

const CTransform4f* GetAreaCameraTransform(TAreaId area) {
  if (!sFrame.valid) {
    return &CTransform4f::Identity();
  }
  for (const auto& entry : sFrame.areaXfs) {
    if (entry.first == area) {
      return &entry.second;
    }
  }
  return nullptr;
}

// Whether the camera is close enough to a portal's doorway for the near plane to clip its depth
// seal. The main pass then leaves out the sky (see SkipMainPassSky).
bool IsEyeInDoorway(const CStateManager& mgr) {
  // The main pass's eye: the camera, moved into the root's frame.
  const CVector3f eye =
      sFrame.rootXf * mgr.GetCameraManager()->GetCurrentCameraTransform(mgr).GetTranslation();
  // The near plane's corners reach about three times its distance from the eye at the game's
  // fields of view; any doorway closer than that may be clipped out of the seal.
  const float nearReach = 3.f * mgr.GetCameraManager()->GetCurrentCamera(mgr).GetNearClipDistance();
  for (const Portal& portal : sFrame.portals) {
    if (portal.doorway.size() < 3) {
      continue;
    }
    CVector3f center = CVector3f::Zero();
    float radius = 0.f;
    for (int i = 0; i < portal.doorway.size(); ++i) {
      center += portal.doorway[i];
    }
    center = center * (1.f / static_cast< float >(portal.doorway.size()));
    for (int i = 0; i < portal.doorway.size(); ++i) {
      radius = std::max(radius, (portal.doorway[i] - center).Magnitude());
    }
    const CVector3f normal =
        CVector3f::Cross(portal.doorway[1] - portal.doorway[0], portal.doorway[2] - portal.doorway[0]);
    if (!normal.CanBeNormalized()) {
      continue;
    }
    const float planeDist = std::fabs(CVector3f::Dot(eye - center, normal.AsNormalized()));
    if (planeDist < nearReach && (eye - center).Magnitude() < radius + nearReach) {
      return true;
    }
  }
  return false;
}

bool DrawPortalPasses(const CStateManager& mgr) {
  if (!sFrame.valid || sFrame.pass >= 0 || sFrame.portals.empty() ||
      mgr.GetPlayerState()->GetActiveVisor(mgr) == CPlayerState::kPV_Thermal) {
    return false;
  }
  // Decided before the portals are drawn, since in such a frame their sky is what shows behind the
  // whole view. That sky has to be one a room on screen needs: the room the camera is in if it's
  // open to the sky, or else a room through a doorway that is (looking out of a tunnel). An
  // enclosed room on either side would otherwise put its region's sky where the other room's
  // should be.
  sFrame.skipMainSky = IsEyeInDoorway(mgr) && !sNeverSkipSky;
  if (sFrame.skipMainSky) {
    const CWorld& world = *mgr.GetWorld();
    if (world.GetAreaAlways(sFrame.root).DoesAreaNeedSkyNow()) {
      sFrame.skyArea = sFrame.root;
      sFrame.skyXf = sFrame.rootXf;
    } else {
      for (const Portal& portal : sFrame.portals) {
        if (world.GetAreaAlways(portal.area).DoesAreaNeedSkyNow()) {
          sFrame.skyArea = portal.area;
          sFrame.skyXf = portal.cameraXf;
          break;
        }
      }
    }
    if (sLogThisFrame) {
      Log.info("[portal]   eye in doorway: portal passes draw the sky of area {}",
               sFrame.skyArea.Value());
    }
  }
  for (int i = 0; i < static_cast< int >(sFrame.portals.size()); ++i) {
    sFrame.pass = i;
    mgr.DrawWorld();
  }
  sFrame.pass = -1;
  return true;
}

bool InPortalPass() { return sFrame.valid && sFrame.pass >= 0; }

bool IsRootShifted() { return sFrame.valid && sFrame.shifted; }

const CTransform4f& GetPassCameraTransform() {
  if (!sFrame.valid) {
    return CTransform4f::Identity();
  }
  return sFrame.pass >= 0 ? sFrame.portals[sFrame.pass].cameraXf : sFrame.rootXf;
}

TAreaId GetPassVisArea(TAreaId vanilla) {
  if (!sFrame.valid) {
    return vanilla;
  }
  return sFrame.pass >= 0 ? sFrame.portals[sFrame.pass].area : sFrame.root;
}

bool IsAreaInPass(TAreaId area) {
  if (!sFrame.valid) {
    return true;
  }
  if (sFrame.pass >= 0) {
    return area == sFrame.portals[sFrame.pass].area;
  }
  for (const Portal& portal : sFrame.portals) {
    if (portal.area == area) {
      return false;
    }
  }
  return GetAreaCameraTransform(area) != nullptr;
}

void LogPassArea(TAreaId area, int visState) {
  if (sLogThisFrame) {
    Log.info("[portal]   {} pass draws area {} (PVS state {})",
             sFrame.pass >= 0 ? "portal" : "main", area.Value(), visState);
  }
}

bool SkipMainPassSky() { return sFrame.valid && sFrame.pass < 0 && sFrame.skipMainSky; }

namespace {
// Whether the current pass is a portal pass drawing the frame's chosen sky (see Frame::skyArea).
bool DrawsChosenSky() { return sFrame.valid && sFrame.pass >= 0 && sFrame.skipMainSky; }
} // namespace

TAreaId GetPassSkyArea(TAreaId vanilla) {
  return DrawsChosenSky() ? sFrame.skyArea : GetPassVisArea(vanilla);
}

CTransform4f GetPassSkyTransform(const CVector3f& eye) {
  if (!DrawsChosenSky() || sFrame.skyArea == kInvalidAreaId) {
    return CTransform4f::Translate(eye);
  }
  // A point drawn at p from the sky area's camera is drawn at (this pass's camera * sky area's
  // camera^-1) p from this one, so every portal pass puts the sky in the same place on screen.
  const CTransform4f toPass = sFrame.portals[sFrame.pass].cameraXf * sFrame.skyXf.GetInverse();
  return CTransform4f::Translate(eye) * toPass.GetRotation();
}

void ResetDepthForMainPass(const CStateManager& mgr) {
  const CTransform4f& view = CGraphics::GetViewMatrix();
  const CVector3f eye = view.GetTranslation();
  if (sLogThisFrame) {
    Log.info("[portal]   main pass: {} portal(s) drawn, sky {}, eye {}", sFrame.portals.size(),
             sFrame.skipMainSky ? "skipped" : "drawn", FormatVec(eye));
  }
  const CVector3f forward = view.Rotate(CVector3f(0.f, 1.f, 0.f));
  const CVector3f right = view.Rotate(CVector3f(1.f, 0.f, 0.f));
  const CVector3f up = view.Rotate(CVector3f(0.f, 0.f, 1.f));

  gpRender->SetModelMatrix(CTransform4f::Identity());
  CGraphics::SetAlphaCompare(kAF_Always, 0, kAO_And, kAF_Always, 0);
  CGraphics::SetBlendMode(kBM_Blend, kBF_Zero, kBF_One, kLO_Clear);
  CGraphics::SetTevOp(kTS_Stage0, CGraphics::kEnvPassthru);
  CGraphics::SetTevOp(kTS_Stage1, CGraphics::kEnvPassthru);
  CGraphics::SetCullMode(kCM_None);
  GXSetColorUpdate(GX_FALSE);

  // Push depth back to the far plane everywhere: any quad filling the view, drawn into a [1, 1]
  // depth range.
  const CVector3f center = eye + forward * 1.f;
  const float extent = 100.f;
  CGraphics::SetDepthRange(1.f, 1.f);
  CGraphics::SetDepthWriteMode(true, kE_Always, true);
  CGraphics::StreamBegin(kP_TriangleStrip);
  CGraphics::StreamColor(CColor::White());
  CGraphics::StreamVertex(center - right * extent + up * extent);
  CGraphics::StreamVertex(center + right * extent + up * extent);
  CGraphics::StreamVertex(center - right * extent - up * extent);
  CGraphics::StreamVertex(center + right * extent - up * extent);
  CGraphics::StreamEnd();
  CGraphics::SetDepthRange(0.125f, 1.f);

  // Seal each doorway at its real depth. The main pass then draws everything in front of the
  // doorway as usual, while the sky and anything behind the doorway stay out of it.
  CGraphics::SetDepthWriteMode(true, kE_LEqual, true);
  for (const Portal& portal : sFrame.portals) {
    if (portal.doorway.size() < 3) {
      continue;
    }
    // From outside the doorway (just switched early to drawing from this room), the room is what
    // lies past the doorway and sealing it would hide the room.
    const CVector3f outward = CVector3f::Cross(portal.doorway[1] - portal.doorway[0],
                                               portal.doorway[2] - portal.doorway[0]);
    if (CVector3f::Dot(eye - portal.doorway[0], outward) > 0.f) {
      continue;
    }
    CGraphics::StreamBegin(kP_TriangleFan);
    CGraphics::StreamColor(CColor::White());
    for (int i = 0; i < portal.doorway.size(); ++i) {
      CGraphics::StreamVertex(portal.doorway[i]);
    }
    CGraphics::StreamEnd();
  }

  GXSetColorUpdate(GX_TRUE);
  CGraphics::SetCullMode(kCM_Front);
  CGraphics::SetBlendMode(kBM_Blend, kBF_One, kBF_Zero, kLO_Clear);
}

} // namespace metaforce::portals
