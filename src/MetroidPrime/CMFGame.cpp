#include "MetroidPrime/CMFGame.hpp"
#include "Kyoto/CResFactory.hpp"
#include "Kyoto/CSimplePool.hpp"
#include "Kyoto/Graphics/CModel.hpp"
#include "MetroidPrime/CMFGameLoader.hpp"
#include "MetroidPrime/CMemoryCard.hpp"

#include "Kyoto/Audio/CMidiManager.hpp"
#include "Kyoto/Audio/CSfxManager.hpp"
#include "Kyoto/CFrameDelayedKiller.hpp"
#include "Kyoto/Graphics/CGraphics.hpp"
#include "MetroidPrime/CArchitectureQueue.hpp"
#include "MetroidPrime/CDecalManager.hpp"
#include "MetroidPrime/CInGameGuiManager.hpp"
#include "MetroidPrime/CMain.hpp"
#include "MetroidPrime/CStateManager.hpp"
#include "MetroidPrime/Cameras/CCameraManager.hpp"
#include "MetroidPrime/Cameras/CCinematicCamera.hpp"
#include "MetroidPrime/Decode.hpp"
#include "MetroidPrime/DefaultWorld.hpp"
#include "MetroidPrime/Player/CMorphBall.hpp"
#include "MetroidPrime/Player/CPlayer.hpp"
#include "MetroidPrime/Player/CPlayerGun.hpp"
#include "MetroidPrime/TCastTo.hpp"
#include "rstl/math.hpp"

#if defined(TARGET_PC)
#include "Metaforce/CutsceneSkip.hpp"
#include "Metaforce/Input.hpp"
#include "Metaforce/MapLayout.hpp"
#include "Metaforce/Randomizer/Hooks.hpp"
#include "Metaforce/Warp.hpp"
#endif

CMFGame::CMFGame(rstl::ncrc_ptr< CStateManager > stateManager,
                 rstl::ncrc_ptr< CInGameGuiManager > guiManager,
                 CArchitectureQueue& architectureQueue)
: CIOWin(rstl::string_l("CMFGame"))
, mStateManager(stateManager)
, mGuiManager(guiManager)
, mFlowState(kGFS_InGame)
, mSkippedCineCam(kInvalidUniqueId)
, mInitialized(false)
, mPlayerAlive(true) {
  gpMain->SetGameFlowBuilt(true);
#if defined(TARGET_PC)
  metaforce::cutscenes::Reset();
#endif
}

CMFGame::~CMFGame() {
  gpMain->SetGameFlowBuilt(false);
  gpMain->SetScreenFading(false);
  CDecalManager::Reinitialize();
#if defined(TARGET_PC)
  metaforce::cutscenes::Reset();
#endif
}

void CMFGame::Touch() const {
  CStateManager* mgr = &*mStateManager;
  mgr->TouchSky();
  mgr->TouchPlayerActor();

  CPlayer* player = mgr->Player();
  bool touchModel = false;
  bool touchBall = false;
  bool touchGun = false;

  switch (player->GetMorphballTransitionState()) {
  case CPlayer::kMS_Unmorphed:
    touchGun = true;
    break;
  case CPlayer::kMS_Morphed:
    touchBall = true;
    break;
  case CPlayer::kMS_Morphing:
    touchBall = true;
    touchModel = true;
    break;
  case CPlayer::kMS_Unmorphing:
    touchGun = true;
    touchModel = true;
    break;
  }

  if (touchGun) {
    player->GetPlayerGun()->TouchModel(*mgr);
  }

  if (touchModel) {
    player->ModelData()->Touch(*mgr, 0);
  }

  if (touchBall) {
    player->GetMorphBall()->TouchModel(*mgr);
  }
}

CIOWin::EMessageReturn CMFGame::OnMessage(const CArchitectureMessage& message,
                                          CArchitectureQueue& queue) {
  switch (message.GetType()) {
  case kAM_FrameBegin: {
    const CArchMsgParmInt32& parm = MakeMsg::GetParmFrameBegin(message);
    mStateManager->FrameBegin(parm.GetInt32());
    break;
  }
  case kAM_TimerTick: {
    bool wasInitialized = mInitialized;
    mInitialized = true;
    float dt = MakeMsg::GetParmTimerTick(message).GetReal();
    switch (mFlowState) {
    case kGFS_CinematicSkip: {
      mCineSkipTime += dt;
      const CGameCamera& camera =
          mStateManager->GetCameraManager()->GetCurrentCamera(*mStateManager);
      const CCinematicCamera* const cineCam = TCastToConstPtr< CCinematicCamera >(camera);
      if ((mCineSkipTime >= 1.f && mStateManager->SpecialSkipCinematic()) || !cineCam ||
          ((cineCam->GetFlags() & 0x10) && mSkippedCineCam != cineCam->GetUniqueId())) {
        gpMain->SetScreenFading(false);
        mFlowState = kGFS_InGame;
        mGuiManager->StartFadeIn();
        mSkippedCineCam = kInvalidUniqueId;
        break;
      }
    }
    // Fall through.
    case kGFS_InGame:
      mStateManager->SetRandomAvailable(true);
      switch (mStateManager->GetDeferredStateTransition()) {
      case kSMT_InGame:
#if defined(TARGET_PC)
        // Runs extra updates while a cutscene fast-forwards.
        metaforce::cutscenes::Update(*mStateManager, dt);
#else
        mStateManager->Update(dt);
#endif
        if (mStateManager->GetWantsToQuit())
          CGraphics::SetIsBeginSceneClearFb(false);
        break;
      case kSMT_MapScreen:
        EnterMapScreen();
        break;
      case kSMT_PauseGame:
        PauseGame();
        break;
      case kSMT_LogBook:
        EnterLogBook();
        break;
      case kSMT_SaveGame:
        SaveGame();
        break;
      case kSMT_MessageScreen:
        EnterMessageScreen(mStateManager->GetHUDMessageTime());
        break;
      }
      if (mPlayerAlive && !mStateManager->GetPlayerState()->IsAlive())
        PlayerDied();
      mStateManager->SetRandomAvailable(false);
      break;
    case kGFS_Paused:
      if (mGuiManager->IsInGameplayStateNotTransitioning()) {
        mStateManager->SetInSaveUI(mGuiManager->IsInSaveUI());
        UnpauseGame();
        if (mStateManager->GetPauseHUDMessage() != kInvalidAssetId)
          mStateManager->IncrementHUDMessageFrameCounter();
      }
      break;
    case kGFS_PlayerDied:
      if (mStateManager->GetPlayer()->IsPlayerDeadEnough()) {
        gpMain->SetRestartMode(CMain::kRM_LoseGame);
        queue.Push(MakeMsg::CreateQuitGameplay(kAMT_Game));
      } else {
        mStateManager->SetRandomAvailable(true);
        mStateManager->Update(dt);
        mStateManager->SetRandomAvailable(false);
      }
      break;
    }
    CStateManager& mgr = *mStateManager;
#if defined(TARGET_PC)
    // Paused too: the map screen is drawn while the game is paused, and a map laid out again in
    // the background, or for a changed Room Rando Map setting, should show there.
    metaforce::maplayout::Update(mgr);
#endif
    mGuiManager->Update(mgr, dt, queue, IsCameraActiveFlow());
    if (!wasInitialized)
      gpGameState->WorldTransitionManager()->EndTransition();
    return kMR_Exit;
  }
  case kAM_UserInput: {
    if (!mInitialized)
      break;
    CArchMsgParmUserInput parm = MakeMsg::GetParmUserInput(message);
    CFinalInput input = parm.GetUserInput();
    if (mFlowState == kGFS_InGame) {
      if (input.ControllerNumber() == 0) {
        const CGameCamera& camera =
            mStateManager->GetCameraManager()->GetCurrentCamera(*mStateManager);
        const CCinematicCamera* const cineCam = TCastToConstPtr< CCinematicCamera >(camera);
#if defined(TARGET_PC)
        // In randomized games, R + Z + D-pad Right saves and R + Z + D-pad Left reloads the last
        // save. R cancels a Z press like a beam chord does, so the map stays shut while a chord is
        // put together.
        const bool rHeld = input.DR() || input.DRTrigger();
        const bool shortcutsHoldMap = metaforce::randomizer::ShortcutsHoldMap(rHeld);
        const bool mapPressed =
            metaforce::input::FilterMapButton(
                input.PZ(), input.DZ(),
                input.PDPUp() || input.PDPDown() || input.PDPLeft() || input.PDPRight() ||
                    shortcutsHoldMap,
                input.PStart()) &&
            !shortcutsHoldMap;
        if (metaforce::randomizer::OnShortcutInput(rHeld, input.DZ(), input.DDPLeft(),
                                                   input.DDPRight())) {
          break;
        }
#endif
        if (input.PStart()) {
          if (cineCam && mStateManager->GetCinematicSkipObject() != kInvalidUniqueId) {
            CMidiManager::StopAll();
            mSkippedCineCam = cineCam->GetUniqueId();
            mFlowState = kGFS_CinematicSkip;
            mCineSkipTime = 0.f;
            break;
          } else if (!cineCam) {
            mStateManager->DeferStateTransition(kSMT_PauseGame);
          }
#if defined(TARGET_PC)
          // A cutscene with no skip function: fast-forward it if the Cutscene Skips setting
          // allows.
          else if (metaforce::cutscenes::TryStartFastForward(*mStateManager)) {
            break;
          }
#endif
        }
#if defined(TARGET_PC)
        else if (mapPressed && !cineCam && mStateManager->CanShowMapScreen()) {
#else
        else if (input.PZ() && !cineCam && mStateManager->CanShowMapScreen()) {
#endif
          mStateManager->DeferStateTransition(kSMT_MapScreen);
        }
      }
      mStateManager->SetRandomAvailable(true);
      mStateManager->ProcessInput(input);
      mStateManager->SetRandomAvailable(false);
    }
    mGuiManager->ProcessControllerInput(*mStateManager, input, queue);
    break;
  }
  case kAM_FrameEnd:
    mStateManager->FrameEnd();
    if (mStateManager->GetWantsToQuit())
      queue.Push(MakeMsg::CreateQuitGameplay(kAMT_Game));
    break;
  case kAM_QuitGameplay:
    CFrameDelayedKiller::StallAndFlushAllAllocations();
    return kMR_RemoveIOWin;
  }
  return kMR_Normal;
}

void CMFGame::Draw() const {
  if (!mInitialized) {
    return;
  }

  Touch();

  if (mGuiManager->GetIsGameDraw()) {
    gpMain->SetGameFrameDrawn(true);
    mStateManager->PreRender();
    mStateManager->DrawWorld();
    (void)mStateManager->GetPlayer()->IsPlayerDeadEnough();
  }

  mGuiManager->PreDraw(*mStateManager, IsCameraActiveFlow());
  mGuiManager->Draw(*mStateManager);

  if (mFlowState == kGFS_CinematicSkip) {
    const float intensity = rstl::min_val(1.f, 1.f - mCineSkipTime);
    CCameraFilterPass::DrawFilter(CCameraFilterPass::kFT_Multiply,
                                  CCameraFilterPass::kFS_Fullscreen,
                                  CColor(intensity, intensity, intensity, intensity), nullptr, 1.f);
  }
#if defined(TARGET_PC)
  // Leaves the pause and message screens visible when they interrupt a fast-forward.
  const float brightness = metaforce::cutscenes::Brightness();
  if (brightness < 1.f && mFlowState != kGFS_Paused) {
    CCameraFilterPass::DrawFilter(CCameraFilterPass::kFT_Multiply,
                                  CCameraFilterPass::kFS_Fullscreen,
                                  CColor(brightness, brightness, brightness, brightness), nullptr,
                                  1.f);
  }
#endif
}

void CMFGame::EnterMapScreen() {
  mFlowState = kGFS_Paused;
  mGuiManager->PauseGame(*mStateManager, kIGGS_MapScreen);
  mStateManager->SetInMapScreen(true);
}

void CMFGame::PauseGame() {
  mFlowState = kGFS_Paused;
  mGuiManager->PauseGame(*mStateManager, kIGGS_PauseGame);
}

void CMFGame::EnterLogBook() {
  mFlowState = kGFS_Paused;
  mGuiManager->PauseGame(*mStateManager, kIGGS_PauseLogBook);
}

void CMFGame::SaveGame() {
  mFlowState = kGFS_Paused;
  mGuiManager->PauseGame(*mStateManager, kIGGS_PauseSaveGame);
}

void CMFGame::EnterMessageScreen(const float time) {
  mFlowState = kGFS_Paused;
  mGuiManager->ShowPauseGameHudMessage(*mStateManager, mStateManager->GetPauseHUDMessage(), time);
}

void CMFGame::UnpauseGame() {
  mFlowState = kGFS_InGame;
  CSfxManager::SetChannel(CSfxManager::kSC_Game);
  mStateManager->DeferStateTransition(kSMT_InGame);
}

void CMFGame::PlayerDied() {
  mFlowState = kGFS_PlayerDied;
  mPlayerAlive = false;
}

bool CMFGame::IsCameraActiveFlow() const {
  const bool ret = (mFlowState == kGFS_InGame || mFlowState == kGFS_PlayerDied);
  return ret;
}

extern int gResFactoryUnknown;

CMFGameLoader::CMFGameLoader()
: CIOWin(rstl::string_l("CMFGameLoader"))
, mInitialized(false)
, mTransitionFinished(false) {
  gResFactoryUnknown = 1;
  CModel::DisableTextureTimeout();
#if defined(TARGET_PC)
  metaforce::randomizer::OnGameLoad();
  metaforce::warp::ApplyPendingWarp();
#endif
  if (gpMain->GetRestartMode() == CMain::kRM_Default ||
      gpMain->GetRestartMode() == CMain::kRM_StateSetter) {
    CAssetId worldId = gpGameState->CurrentWorldAssetId();
    if (gpMemoryCard->HasSaveWorldMemory(worldId)) {
      const CSaveWorldMemory& memory = gpMemoryCard->GetSaveWorldMemory(worldId);
      if (memory.GetWorldNameId() != kInvalidAssetId) {
        CWorldTransManager& trans = *gpGameState->WorldTransitionManager();
        trans.EnableTransition(0xB7BBD0B4, memory.GetWorldNameId(), 1, false, 0.1f, 16.f, 1.f);
      }
    }
  }
  CAssetId worldId = gpGameState->CurrentWorldAssetId();
  if (worldId == skDefaultWorld.GetId() &&
      gpGameState->GetCurrentWorldState().GetCurrentArea() == TAreaId(0)) {
    const SObjectTag* tag = gpResourceFactory->GetResourceIdByName("STRG_IntroLevelLoad");
    if (tag) {
      CWorldTransManager& trans = *gpGameState->WorldTransitionManager();
      trans.EnableTransition(kInvalidAssetId, tag->GetId(), 0, false, 0.1f, 16.f, 1.f);
    }
  }
  mInitialized = true;
}

CMFGameLoader::~CMFGameLoader() {
  CGraphics::SetIsBeginSceneClearFb(true);
  gResFactoryUnknown = 2;
}

void CMFGameLoader::MakeLoadDependancyList() {
  static const char* skPaksToLoad[] = {"aram:TestAnim", "aram:SamusGun", "aram:SamGunFx", nullptr};
  int count = 0;
  for (const char** pak = skPaksToLoad; *pak; ++pak) {
    const rstl::vector< CAssetId >* tags =
        gpResourceFactory->GetTagListForFile(rstl::string_l(*pak));
    if (tags)
      count += tags->size();
  }
  mLoadList.reserve(count);
  for (const char** pak = skPaksToLoad; *pak; ++pak) {
    const rstl::vector< CAssetId >* tags =
        gpResourceFactory->GetTagListForFile(rstl::string_l(*pak));
    if (tags) {
      for (AUTO(it, tags->begin()); it != tags->end(); ++it) {
        const CAssetId& id = *it;
        SObjectTag tag(gpResourceFactory->GetResLoader().GetResourceTypeById(id), id);
        CToken token = gpSimplePool->GetObj(tag);
        mLoadList.push_back(token);
      }
    }
  }
}

CIOWin::EMessageReturn CMFGameLoader::OnMessage(const CArchitectureMessage& message,
                                                CArchitectureQueue& queue) {
  rstl::ncrc_ptr< CWorldTransManager >& transManager = gpGameState->WorldTransitionManager();
  if (message.GetType() == kAM_TimerTick) {
    float dt = MakeMsg::GetParmTimerTick(message).GetReal();
    if (!mInitialized) {
      if (mLoadList.empty()) {
        MakeLoadDependancyList();
        transManager->StartTransition();
        return kMR_Exit;
      }
      int loadingCount = 0;
      for (AUTO(it, mLoadList.begin()); it != mLoadList.end(); ++it) {
        it->Lock();
        if (!it->IsLoaded())
          ++loadingCount;
      }
      transManager->Update(dt);
      if (loadingCount)
        return kMR_Exit;
      mInitialized = true;
    } else {
      transManager->Update(dt);
    }
    if (mStateManager.IsNull()) {
      transManager->WaitForModelsAndTextures();
      CWorldState& world = gpGameState->CurrentWorldState();
      const rstl::ncrc_ptr< CStateManager >& stateManager = rstl::ncrc_ptr< CStateManager >(
          rs_new CStateManager(world.Mailbox(), world.MapWorldInfo(), gpGameState->PlayerState(),
                               transManager, world.GetLayerState()));
      mStateManager = stateManager;
    }
    if (!mStateManager->IsFullyInitialized()) {
      CWorldState& world = gpGameState->CurrentWorldState();
      mStateManager->InitializeState(world.GetWorldAssetId(), world.GetCurrentArea(),
                                        world.GetDesiredAreaAssetId());
      return kMR_Exit;
    }
    if (mGuiManager.IsNull()) {
      gpGameState->CurrentWorldState().SetDesiredAreaAssetId(kInvalidAssetId);
      mGuiManager = rs_new CInGameGuiManager(*mStateManager, queue);
    }
    if (!mGuiManager->CheckLoadComplete(*mStateManager))
      return kMR_Exit;
    mLoadList.clear();
    transManager->StartTextFadeOut();
    mTransitionFinished = transManager->IsTransitionFinished();
    return kMR_Exit;
  } else if (message.GetType() == kAM_FrameEnd) {
    if (mTransitionFinished) {
      CIOWin* game = rs_new CMFGame(mStateManager, mGuiManager, queue);
      queue.Push(MakeMsg::CreateCreateIOWin(kAMT_IOWinManager, 10, 1000, game));
      CModel::EnableTextureTimeout();
      return kMR_RemoveIOWinAndExit;
    }
  }
  return kMR_Exit;
}

void CMFGameLoader::Draw() const { gpGameState->WorldTransitionManager()->Draw(); }
