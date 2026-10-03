#include "MetroidPrime/CInputGenerator.hpp"

#include "MetroidPrime/CArchitectureQueue.hpp"
#include "MetroidPrime/Decode.hpp"

#include "Kyoto/Basics/COsContext.hpp"

#if defined(TARGET_PC)
#include "Metaforce/Input.hpp"
#include "Metaforce/MenuPointer.hpp"
#endif

CInputGenerator::CInputGenerator(COsContext* ctx, float leftDiv, float rightDiv)
: mContext(ctx)
, mController(IController::Create(*ctx))
, mLeftDiv(leftDiv)
, mRightDiv(rightDiv) {
  for (uint i = 0; i <= kIOP_Player4; ++i) {
    mConnectedControllers[i] = false;
  }
}

bool CInputGenerator::Update(float dt, CArchitectureQueue& queue) {
  int availSlot = 0;
  if (!mContext->Update()) {
    return false;
  }

  bool firstController = false;
#if defined(TARGET_PC)
  // Gameplay reads port 0, so the mouse and game keys ride on whichever input lands there.
  float mouseX;
  float mouseY;
  metaforce::input::ConsumeMouseDelta(mouseX, mouseY);
  metaforce::menu_pointer::BeginFrame();
  const uchar beamKeys = metaforce::input::BeamKeysHeld();
  const uchar visorKeys = metaforce::input::ConsumeVisorKeyPresses();
#endif
  if (!mController.null()) {
    const int count = mController->GetDeviceCount();
    mController->Poll();
    for (int i = 0; i < count; ++i) {
      const CControllerGamepadData& cont = mController->GetGamepadData(i);
      if (cont.DeviceIsPresent()) {
        if (i == 0) {
          firstController = true;
        }
        {
#if defined(TARGET_PC)
          CFinalInput input(i, dt, cont, mLeftDiv, mRightDiv);
          if (i == 0) {
            input.SetMouseDelta(mouseX, mouseY);
            input.SetBeamKeys(beamKeys);
            input.SetVisorKeyPresses(visorKeys);
          }
#else
          const CFinalInput input(i, dt, cont, mLeftDiv, mRightDiv);
#endif
          const CArchitectureMessage msg = MakeMsg::CreateUserInput(kAMT_Game, input);
          queue.Push(msg);
        }
        availSlot++;
      }

      const bool connected = cont.DeviceIsPresent();
      if (mConnectedControllers[i] != connected) {
        const CArchitectureMessage msg = MakeMsg::CreateControllerStatus(kAMT_Game, i, connected);
        queue.Push(msg);
        mConnectedControllers[i] = connected;
      }
    }
  }

  if (!firstController) {
#if defined(TARGET_PC)
    CFinalInput input(0, dt, *mContext);
    input.SetMouseDelta(mouseX, mouseY);
    input.SetBeamKeys(beamKeys);
    input.SetVisorKeyPresses(visorKeys);
    const CArchitectureMessage msg = MakeMsg::CreateUserInput(kAMT_Game, input);
#else
    const CArchitectureMessage msg = MakeMsg::CreateUserInput(kAMT_Game, CFinalInput(0, dt, *mContext));
#endif
    queue.Push(msg);
  } else {
    const CArchitectureMessage msg =
        MakeMsg::CreateUserInput(kAMT_Game, CFinalInput(availSlot, dt, *mContext));
    queue.Push(msg);
  }
  return true;
}
