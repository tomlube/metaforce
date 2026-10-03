#include "MetroidPrime/ScriptObjects/CScriptDoor.hpp"

#include "Collision/CMaterialList.hpp"
#include "Kyoto/Math/CFrustumPlanes.hpp"
#include "MetroidPrime/CAnimData.hpp"
#include "MetroidPrime/CAnimPlaybackParms.hpp"
#include "MetroidPrime/CEntity.hpp"
#include "MetroidPrime/CEntityInfo.hpp"
#include "MetroidPrime/CGameArea.hpp"
#include "MetroidPrime/CMapWorld.hpp"
#include "MetroidPrime/CMapWorldInfo.hpp"
#include "MetroidPrime/CObjectList.hpp"
#include "MetroidPrime/CPhysicsActor.hpp"
#include "MetroidPrime/CStateManager.hpp"
#include "MetroidPrime/CWorld.hpp"
#include "MetroidPrime/Cameras/CBallCamera.hpp"
#include "MetroidPrime/Cameras/CCameraManager.hpp"
#include "MetroidPrime/Player/CPlayerState.hpp"
#include "MetroidPrime/ScriptObjects/CScriptDock.hpp"
#include "MetroidPrime/TCastTo.hpp"
#include "MetroidPrime/TGameTypes.hpp"

#if defined(TARGET_PC)
#include "Metaforce/DockPortals.hpp"
#include "Metaforce/MergedWorld.hpp"
#include "Metaforce/Randomizer/Hooks.hpp"
#include "MetroidPrime/Player/CPlayer.hpp"

namespace {

// How near the player's collision box may come to a door's before the door waits to close.
const float kClosingClearance = 0.1f;

// Whether the door's dock was moved by the room randomizer.
bool IsMovedDoor(const CStateManager& mgr, const CScriptDoor& door) {
  const CScriptDock* dock =
      TCastToConstPtr< CScriptDock >(mgr.GetObjectById(door.GetConnectedDockID()));
  CTransform4f xf = CTransform4f::Identity();
  return dock != nullptr && metaforce::portals::GetDockTransform(*mgr.GetWorld(), dock->GetAreaId(),
                                                                 dock->GetDockId(), xf);
}

bool IsPlayerInDoorway(const CStateManager& mgr, const CScriptDoor& door) {
  const CPlayer* player = mgr.GetPlayer();
  if (player == nullptr ||
      !metaforce::merged::SharesSpace(mgr, mgr.GetNextAreaId(), door.GetCurrentAreaId())) {
    return false;
  }
  const CAABox playerBox = player->GetBoundingBox();
  const CVector3f margin(kClosingClearance, kClosingClearance, kClosingClearance);
  return CAABox(playerBox.GetMinPoint() - margin, playerBox.GetMaxPoint() + margin)
      .DoBoundsOverlap(door.GetBoundingBox());
}

} // namespace
#endif

CScriptDoor::CScriptDoor(TUniqueId uid, const rstl::string& name, const CEntityInfo& info,
                         const CTransform4f& xf, const CModelData& modelData,
                         const CActorParameters& actorParameters, const CVector3f& orbitPosition,
                         const CAABox& bounds, const bool active, const bool open,
                         const bool projectilesCollide, const float animationLength,
                         const bool ballDoor)
: CPhysicsActor(uid, active, name, info, xf, modelData,
                open ? CMaterialList(kMT_Solid, kMT_Immovable, kMT_Orbit)
                     : CMaterialList(kMT_Immovable, kMT_Occluder, kMT_Solid, kMT_Orbit),
                bounds, SMoverData(1.f), actorParameters, 0.3f, 0.1f)
, mAnimLength(animationLength)
, mAnimTime(0.f)
, mDoorState(kDAT_Open)
, x264_(GetBoundingBox())
, mPartner1(kInvalidUniqueId)
, mPartner2(kInvalidUniqueId)
, mPrevDoor(kInvalidUniqueId)
, mDockId(kInvalidUniqueId)
, mModelBounds(modelData.GetBounds(xf.GetRotation()))
, mOrbitPos(orbitPosition)
, mClosing(false)
, mWasOpen(open)
, mIsOpen(open)
, mConditionsMet(false)
, mProjectilesCollide(projectilesCollide)
, mBallDoor(ballDoor)
, mDoClose(false)
#if defined(TARGET_PC)
, mPendingCloseMsg(kSM_None)
, mPendingCloseSender(kInvalidUniqueId)
#endif
{
  SetThermalFlags(kTF_Cold);

  if (open) {
    SetDoorAnimation(kDAT_Open);
  }

  SetMass(0.f);
}

rstl::optional_object< CAABox > CScriptDoor::GetTouchBounds() const {
  if (GetActive() && GetMaterialList().HasMaterial(kMT_Solid)) {
    return CPhysicsActor::GetBoundingBox();
  }

  return rstl::optional_object_null();
}

rstl::optional_object< CAABox > CScriptDoor::GetProjectileBounds() const {
  if (mProjectilesCollide) {
    return CAABox(mModelBounds.GetMinPoint() + GetTranslation(),
                  mModelBounds.GetMaxPoint() + GetTranslation());
  }

  return rstl::optional_object_null();
}

CVector3f CScriptDoor::GetOrbitPosition(const CStateManager& mgr) const {
  return GetTranslation() + mOrbitPos;
}

CScriptDoor::EDoorOpenCondition CScriptDoor::GetDoorOpenCondition(CStateManager& mgr) {
  CScriptDock* const dock = TCastToPtr< CScriptDock >(mgr.ObjectById(mDockId));
  if (!dock) {
    return kDOC_Ready;
  }
#if defined(TARGET_PC)
  // A door into another world has nothing loaded behind it to open onto.
  if (metaforce::randomizer::OnCrossWorldDoorOpen(mgr, dock->GetAreaId().Value(),
                                                  dock->GetDockId())) {
    return kDOC_Loading;
  }
#endif

  if (mAnimTime < 0.05f || mDoClose) {
    return kDOC_Loading;
  }

  const CWorld* world = mgr.GetWorld();
  if (!world->DoesAreaExist(dock->GetAreaId())) {
    return kDOC_NotReady;
  }
  if (!world->IsAreaValid(dock->GetAreaId())) {
    return kDOC_Loading;
  }
  if (!world->AreSkyNeedsMet()) {
    return kDOC_Loading;
  }

  const IGameArea::Dock& gameDock =
      mgr.GetWorld()->GetAreaAlways(dock->GetAreaId()).GetDock(dock->GetDockId());
  const TAreaId connectedArea = gameDock.GetConnectedAreaId(dock->GetDockReference(mgr));
  if (!mgr.GetWorld()->DoesAreaExist(connectedArea)) {
    return kDOC_NotReady;
  }

  CGameArea* area = mgr.World()->Area(connectedArea);
  if (!area->IsLoaded()) {
    mgr.DeliverScriptMsg(dock, GetUniqueId(), kSM_SetToMax);
    return kDOC_Loading;
  }
  if (area->GetPostConstructed()->mPlayerActorsLoading != 0) {
    return kDOC_Loading;
  }

  const CObjectList& objects = mgr.ObjectListById(kOL_PlatformAndDoor);
  for (int i = objects.GetFirstObjectIndex(); i != -1; i = objects.GetNextObjectIndex(i)) {
    if (const CScriptDoor* const door = TCastToConstPtr< CScriptDoor >(objects[i])) {
      if (door->GetUniqueId() != GetUniqueId() &&
          (door->GetCurrentAreaId() == GetCurrentAreaId() ||
           door->GetCurrentAreaId() == connectedArea) &&
          door->mWasOpen && door->mDockId != kInvalidUniqueId) {
        return kDOC_Loading;
      }
    }
  }

  for (CGameArea::CConstChainIterator it = mgr.GetWorld()->GetChainHead(CWorld::kC_Alive);
       it != CWorld::skGlobalEnd; ++it) {
    if (it->GetAreaId() != area->GetAreaId() && !it->IsFinishedOccluding()) {
      return kDOC_Loading;
    }
  }

  if (!area->TryTakingOutOfARAM()) {
    return kDOC_Loading;
  }

  return mgr.GetWorld()->GetMapWorld()->IsMapAreasStreaming() ? kDOC_Loading : kDOC_Ready;
}

void CScriptDoor::OpenDoor(TUniqueId uid, CStateManager& mgr) {
  mgr.MapWorldInfo()->SetDoorVisited(mgr.GetEditorIdForUniqueId(GetUniqueId()), true);
  mIsOpen = true;
  mWasOpen = true;
  mConditionsMet = false;
  mPartner1 = kInvalidUniqueId;
  mPartner2 = kInvalidUniqueId;

  if (const CScriptDoor* const door = TCastToConstPtr< CScriptDoor >(mgr.GetObjectById(uid))) {
    mPartner1 = door->GetUniqueId();
  }

  SetDoorAnimation(kDAT_Open);

  if (mPartner1 != kInvalidUniqueId) {
    SendScriptMsgs(kSS_MaxReached, mgr, kSM_None);
  } else {
    SendScriptMsgs(kSS_Open, mgr, kSM_None);
  }

  if (const CScriptDock* const dock1 = TCastToConstPtr< CScriptDock >(mgr.GetObjectById(mDockId))) {
    CObjectList& list = mgr.ObjectListById(kOL_PlatformAndDoor);
    for (int idx = list.GetFirstObjectIndex(); idx != -1; idx = list.GetNextObjectIndex(idx)) {
      if (CScriptDoor* const door = TCastToPtr< CScriptDoor >(list[idx])) {
        if (door->GetUniqueId() == uid) {
          continue;
        }

        if (const CScriptDock* const dock2 =
                TCastToConstPtr< CScriptDock >(mgr.GetObjectById(door->GetConnectedDockID()))) {
          if (dock2->GetAreaId() == dock1->GetCurrentConnectedAreaId(mgr) &&
              dock2->GetCurrentConnectedAreaId(mgr) == dock1->GetAreaId()) {
            mPartner2 = door->GetUniqueId();
            mgr.DeliverScriptMsg(door, GetUniqueId(), kSM_Open);
            break;
          }
        }
      }
    }
  }

  if (mPartner1 == kInvalidUniqueId && mPartner2 == kInvalidUniqueId) {

    for (rstl::vector< SConnection >::const_iterator it = GetConnectionList().begin();
         it != GetConnectionList().end(); ++it) {
      if (it->mMsg != kSM_Open) {
        continue;
      }

      if (const CScriptDoor* const door =
              TCastToConstPtr< CScriptDoor >(mgr.GetObjectById(mgr.GetIdForScript(it->mObjId)))) {
        mPartner2 = door->GetUniqueId();
        break;
      }
    }
  }
}

void CScriptDoor::SetDoorAnimation(EDoorAnimType state) {
  mDoorState = state;
  if (HasAnimation()) {
    AnimationData()->SetAnimation(CAnimPlaybackParms(static_cast< int >(state), -1, 1.f, true),
                                  false);
  }
}

#if defined(TARGET_PC)
bool CScriptDoor::IsClosingBlocked(const CStateManager& mgr) const {
  const TUniqueId ids[3] = {GetUniqueId(), mPartner1, mPartner2};
  bool moved = false;
  bool blocked = false;
  for (int i = 0; i < 3; ++i) {
    const CScriptDoor* door = TCastToConstPtr< CScriptDoor >(mgr.GetObjectById(ids[i]));
    if (door == nullptr) {
      continue;
    }
    moved = moved || IsMovedDoor(mgr, *door);
    blocked = blocked || (door->GetActive() && door->IsOpen() && IsPlayerInDoorway(mgr, *door));
  }
  return moved && blocked;
}
#endif

void CScriptDoor::AcceptScriptMsg(EScriptObjectMessage msg, TUniqueId uid, CStateManager& mgr) {
#if defined(TARGET_PC)
  if (msg == kSM_Open) {
    mPendingCloseMsg = kSM_None;
  } else if (((msg == kSM_Close && mIsOpen) ||
              (msg == kSM_Action && (mIsOpen || mPartner1 != kInvalidUniqueId))) &&
             GetActive() && IsClosingBlocked(mgr)) {
    mPendingCloseMsg = msg;
    mPendingCloseSender = uid;
    return;
  }
#endif

  switch (msg) {
  case kSM_Close: {
    if (!GetActive()) {
      return;
    }

    if (mPartner1 == kInvalidUniqueId || mPartner1 == uid) {
      if (mIsOpen) {
        if (mPartner2 != kInvalidUniqueId) {
          // lol, this is its actual name
          static int i = 0;
          if (CEntity* ent = mgr.ObjectById(mPartner2)) {
            ++i;
            mgr.DeliverScriptMsg(ent, GetUniqueId(), kSM_Close);
            --i;
          }
        }
        mIsOpen = false;
        SetDoorAnimation(kDAT_Close);
        mgr.GetCameraManager()->BallCamera()->DoorClosing(GetUniqueId());
      } else if (mConditionsMet) {
        mConditionsMet = false;
        SendScriptMsgs(kSS_Closed, mgr, kSM_None);
      }
    }
    break;
  }
  case kSM_Action: {
    if (mPartner1 != kInvalidUniqueId) {
      if (CScriptDoor* door = TCastToPtr< CScriptDoor >(mgr.ObjectById(mPartner1))) {
        if (door->IsOpen()) {
          mDoClose = true;
          mgr.DeliverScriptMsg(door, GetUniqueId(), kSM_Close);
          door->SetDoClose(true);
        }
      }
    } else if (mIsOpen) {
      mDoClose = true;
      if (CScriptDoor* door = TCastToPtr< CScriptDoor >(mgr.ObjectById(mPartner2))) {
        mgr.DeliverScriptMsg(door, GetUniqueId(), kSM_Close);
        door->SetDoClose(true);
      }
      mIsOpen = false;
      SetDoorAnimation(kDAT_Close);
      mgr.GetCameraManager()->BallCamera()->DoorClosing(GetUniqueId());
    }
    break;
  }
  case kSM_Open: {
    if (!GetActive()) {
      return;
    }

    if (!mIsOpen) {

      const EDoorOpenCondition cond = TCastToConstPtr< CScriptDoor >(mgr.GetObjectById(uid))
                                          ? kDOC_Ready
                                          : GetDoorOpenCondition(mgr);

      switch (cond) {
      case kDOC_Loading:
        mConditionsMet = true;
        mPrevDoor = uid;
        break;
      case kDOC_Ready:
        OpenDoor(uid, mgr);
        break;
      case kDOC_NotReady:
      default:
        mWasOpen = false;
        mClosing = true;
        break;
      }
    }
    break;
  }
  case kSM_InitializedInArea: {
    rstl::vector< SConnection >::const_iterator it = GetConnectionList().begin();
    for (; it != GetConnectionList().end(); ++it) {
      if (it->mMsg != kSM_Increment) {
        continue;
      }

      if (const CScriptDock* dock =
              TCastToConstPtr< CScriptDock >(mgr.GetObjectById(mgr.GetIdForScript(it->mObjId)))) {
        mDockId = dock->GetUniqueId();
        break;
      }
    }
    break;
  }
  case kSM_SetToZero:
    mProjectilesCollide = true;
    mgr.MapWorldInfo()->SetDoorVisited(mgr.GetEditorIdForUniqueId(GetUniqueId()), true);
    break;
  case kSM_SetToMax:
    mProjectilesCollide = false;
    break;
  default:
    CPhysicsActor::AcceptScriptMsg(msg, uid, mgr);
    break;
  }
}

void CScriptDoor::Think(float dt, CStateManager& mgr) {
  if (!GetActive()) {
    return;
  }

#if defined(TARGET_PC)
  if (mPendingCloseMsg != kSM_None && !IsClosingBlocked(mgr)) {
    const EScriptObjectMessage msg = mPendingCloseMsg;
    mPendingCloseMsg = kSM_None;
    AcceptScriptMsg(msg, mPendingCloseSender, mgr);
  }
#endif

  if (!mIsOpen && mAnimTime < 0.05f) {
    mAnimTime += dt;
  }

  if (mConditionsMet && GetDoorOpenCondition(mgr) == kDOC_Ready) {
    mConditionsMet = false;
    OpenDoor(mPrevDoor, mgr);
  }

  if (mClosing) {
    mWasOpen = false;
    mgr.GetCameraManager()->BallCamera()->DoorClosed(GetUniqueId());
    mProjectilesCollide = false;
    mClosing = false;
    SendScriptMsgs(kSS_Closed, mgr, kSM_Decrement);
    mAnimTime = 0.f;
    mDoClose = false;
  }

  if (mIsOpen && !GetModelData()->IsAnimating()) {
    RemoveMaterial(kMT_Solid, kMT_Occluder, kMT_Orbit, kMT_Scannable, mgr);
  } else {
    if (mWasOpen && !GetModelData()->IsAnimating()) {
      mWasOpen = false;
      mgr.GetCameraManager()->BallCamera()->DoorClosed(GetUniqueId());
      mProjectilesCollide = false;
      mConditionsMet = false;
      SendScriptMsgs(kSS_Closed, mgr, kSM_None);
      mAnimTime = 0.f;
      mDoClose = false;
    }

    if (GetScannableObjectInfo()) {
      AddMaterial(kMT_Solid, kMT_Metal, kMT_Occluder, kMT_Orbit, kMT_Scannable, mgr);
    } else {
      AddMaterial(kMT_Solid, kMT_Metal, kMT_Occluder, kMT_Orbit, mgr);
    }
  }

  if (GetModelData()->IsAnimating()) {
    float len = GetModelData()->GetAnimationDuration(static_cast< int >(mDoorState));
    len /= mAnimLength;
    UpdateAnimation(len * dt, mgr, true);
  }
  SetTargetable(mgr.GetPlayerState()->GetCurrentVisor() == CPlayerState::kPV_Scan);
}

bool CScriptDoor::IsConnectedToArea(const CStateManager& mgr, TAreaId areaId) const {
  const CScriptDock* dockEnt = TCastToConstPtr< CScriptDock >(mgr.GetObjectById(mDockId));
  if (dockEnt) {
    if (dockEnt->GetAreaId() == areaId) {
      return true;
    }

    const CWorld* world = mgr.GetWorld();
    const CGameArea& area = world->GetAreaAlways(dockEnt->GetAreaId());
    const CGameArea::Dock& dock = area.GetDock(dockEnt->GetDockId());
    if (dock.GetConnectedAreaId(dockEnt->GetDockReference(mgr)) == areaId) {
      return true;
    }
  }
  return false;
}

void CScriptDoor::ForceClosed(CStateManager& mgr) {
#if defined(TARGET_PC)
  mPendingCloseMsg = kSM_None;
#endif
  if (mIsOpen) {
    mIsOpen = false;
    mWasOpen = false;

    mgr.GetCameraManager()->BallCamera()->DoorClosing(GetUniqueId());
    mgr.GetCameraManager()->BallCamera()->DoorClosed(GetUniqueId());

    SetDoorAnimation(kDAT_Close);
    SendScriptMsgs(kSS_Closed, mgr, kSM_None);
    mConditionsMet = false;
    mAnimTime = 0.f;
    mDoClose = false;
  } else if (mConditionsMet) {
    mConditionsMet = false;
    mDoClose = false;
    SendScriptMsgs(kSS_Closed, mgr, kSM_None);
  }
}

void CScriptDoor::AddToRenderer(const CFrustumPlanes& /*frustum*/, const CStateManager& mgr) const {
  if (GetPreRenderClipped()) {
    return;
  }

  CPhysicsActor::Render(mgr);
}

void CScriptDoor::Render(const CStateManager& mgr) const {}

ENTITY_ACCEPT_IMPL(CScriptDoor)
