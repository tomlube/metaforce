#ifndef _CSCRIPTDOOR
#define _CSCRIPTDOOR

#include "MetroidPrime/CPhysicsActor.hpp"

class CScriptDoor : public CPhysicsActor {
public:
  rstl::optional_object< CAABox > GetTouchBounds() const override;
  CVector3f GetOrbitPosition(const CStateManager& mgr) const override;
  void AcceptScriptMsg(EScriptObjectMessage msg, TUniqueId other, CStateManager& mgr) override;
  void Think(float dt, CStateManager& mgr) override;
  void AddToRenderer(const CFrustumPlanes& frustum, const CStateManager& mgr) const override;
  void Render(const CStateManager& mgr) const override;
  DECLARE_TYPES_MATCH_OR_ACCEPT;

  enum EDoorAnimType {
    kDAT_Open,
    kDAT_Close,
    kDAT_Ready,
  };

  enum EDoorOpenCondition {
    kDOC_NotReady,
    kDOC_Loading,
    kDOC_Ready,
  };

  CScriptDoor(TUniqueId uid, const rstl::string& name, const CEntityInfo& info,
              const CTransform4f& xf, const CModelData& modelData,
              const CActorParameters& actorParameters, const CVector3f& orbitPosition,
              const CAABox& bounds, const bool active, const bool open,
              const bool projectilesCollide, float animationLength, const bool ballDoor);

  void SetDoorAnimation(EDoorAnimType state);

  rstl::optional_object< CAABox > GetProjectileBounds() const;

  EDoorOpenCondition GetDoorOpenCondition(CStateManager& mgr);
  void OpenDoor(TUniqueId uid, CStateManager& mgr);

  const TUniqueId GetConnectedDockID() const { return mDockId; }
  bool IsOpen() const { return mIsOpen; }
  bool IsBallDoor() const { return mBallDoor; }
#if defined(TARGET_PC)
  // Something asked the door to open, and it opens as soon as the room behind it is ready.
  // Closing or locking the door in the meantime calls that off.
  bool IsWaitingToOpen() const { return mConditionsMet; }
  // Open, or still swinging shut.
  bool IsOpenOrClosing() const { return mWasOpen; }
#endif
  void SetDoClose(const bool close) { mDoClose = close; }

  bool IsConnectedToArea(const CStateManager& mgr, TAreaId area) const;
  void ForceClosed(CStateManager& mgr);

private:
#if defined(TARGET_PC)
  // Whether a room randomizer door told to close should wait: the door (or the one it closes
  // along with) would turn solid around the player and shove them out of the doorway.
  bool IsClosingBlocked(const CStateManager& mgr) const;
#endif

  float mAnimLength;
  float mAnimTime;
  EDoorAnimType mDoorState;
  CAABox x264_;
  TUniqueId mPartner1;
  TUniqueId mPartner2;
  TUniqueId mPrevDoor;
  TUniqueId mDockId;
  CAABox mModelBounds;
  CVector3f mOrbitPos;

  bool mClosing : 1;
  bool mWasOpen : 1;
  bool mIsOpen : 1;
  bool mConditionsMet : 1;
  bool mProjectilesCollide : 1;
  bool mBallDoor : 1;
  bool mDoClose : 1;

#if defined(TARGET_PC)
  // A close (kSM_Close or kSM_Action) held back by IsClosingBlocked, or kSM_None. Think delivers
  // it once the player is out of the way.
  EScriptObjectMessage mPendingCloseMsg;
  TUniqueId mPendingCloseSender;
#endif
};
CHECK_CHILD_SIZEOF(CScriptDoor, CPhysicsActor, 0x58)

#endif // _CSCRIPTDOOR
