#include "MetroidPrime/CControlMapper.hpp"

#include "MetroidPrime/Tweaks/CTweaks.hpp"

#include "MetroidPrime/Tweaks/CTweakPlayerControl.hpp"

#include "Kyoto/Input/CFinalInput.hpp"

#if defined(TARGET_PC)
#include "Metaforce/Input.hpp"
#endif

rstl::reserved_vector< bool, 67 > ControlMapper::gCommandFilterFlag(true);

#if defined(TARGET_PC)
// Modern controls give the right stick to turning, so beams bound to it move to the same
// directions on the D-pad behind a Z hold, and visors on the D-pad stand aside while Z is held.
// This assumes the normal layout, so the swapped one (gpTweakPlayerControl2) is ignored.
static ControlMapper::EFunctionList GetMapping(ControlMapper::ECommands command,
                                               const CFinalInput& input) {
  if (!metaforce::input::ModernControlsEnabled()) {
    return gpTweakPlayerControlCurrent->GetMapping(command);
  }

  const ControlMapper::EFunctionList mapping = gpTweakPlayerControl1->GetMapping(command);

  switch (command) {
  case ControlMapper::kC_PowerBeam:
  case ControlMapper::kC_IceBeam:
  case ControlMapper::kC_WaveBeam:
  case ControlMapper::kC_PlasmaBeam: {
    ControlMapper::EFunctionList dpad;
    switch (mapping) {
    case ControlMapper::kFL_RightStickUp:
      dpad = ControlMapper::kFL_DPadUp;
      break;
    case ControlMapper::kFL_RightStickDown:
      dpad = ControlMapper::kFL_DPadDown;
      break;
    case ControlMapper::kFL_RightStickLeft:
      dpad = ControlMapper::kFL_DPadLeft;
      break;
    case ControlMapper::kFL_RightStickRight:
      dpad = ControlMapper::kFL_DPadRight;
      break;
    default:
      return mapping;
    }
    return input.DZ() ? dpad : ControlMapper::kFL_None;
  }
  case ControlMapper::kC_XrayVisor:
  case ControlMapper::kC_ThermoVisor:
  case ControlMapper::kC_EnviroVisor:
  case ControlMapper::kC_NoVisor:
    switch (mapping) {
    case ControlMapper::kFL_DPadUp:
    case ControlMapper::kFL_DPadDown:
    case ControlMapper::kFL_DPadLeft:
    case ControlMapper::kFL_DPadRight:
      return input.DZ() ? ControlMapper::kFL_None : mapping;
    default:
      return mapping;
    }
  default:
    return mapping;
  }
}

// The keyboard's beam keys hold their beam command, the way the C-stick does. Beam changes read
// the analog value, so presses aren't reported.
static bool BeamKeyHeld(ControlMapper::ECommands command, const CFinalInput& input) {
  if (command < ControlMapper::kC_PowerBeam || command > ControlMapper::kC_PlasmaBeam) {
    return false;
  }
  return (input.BeamKeys() & (1 << (command - ControlMapper::kC_PowerBeam))) != 0;
}
#endif

const FAnalogInput ControlMapper::gAnalogInputs[] = {
    nullptr,
    &CFinalInput::ALAUp,
    &CFinalInput::ALADown,
    &CFinalInput::ALALeft,
    &CFinalInput::ALARight,
    &CFinalInput::ARAUp,
    &CFinalInput::ARADown,
    &CFinalInput::ARALeft,
    &CFinalInput::ARARight,
    &CFinalInput::ALTrigger,
    &CFinalInput::ARTrigger,
    &CFinalInput::ADPUp,
    &CFinalInput::ADPDown,
    &CFinalInput::ADPLeft,
    &CFinalInput::ADPRight,
    &CFinalInput::AA,
    &CFinalInput::AB,
    &CFinalInput::AX,
    &CFinalInput::AY,
    &CFinalInput::AZ,
    &CFinalInput::AL,
    &CFinalInput::AR,
    &CFinalInput::AStart,
};

const FDigitalInput ControlMapper::gDigitalInputs[] = {
    nullptr,
    &CFinalInput::DLAUp,
    &CFinalInput::DLADown,
    &CFinalInput::DLALeft,
    &CFinalInput::DLARight,
    &CFinalInput::DRAUp,
    &CFinalInput::DRADown,
    &CFinalInput::DRALeft,
    &CFinalInput::DRARight,
    &CFinalInput::DLTrigger,
    &CFinalInput::DRTrigger,
    &CFinalInput::DDPUp,
    &CFinalInput::DDPDown,
    &CFinalInput::DDPLeft,
    &CFinalInput::DDPRight,
    &CFinalInput::DA,
    &CFinalInput::DB,
    &CFinalInput::DX,
    &CFinalInput::DY,
    &CFinalInput::DZ,
    &CFinalInput::DL,
    &CFinalInput::DR,
    &CFinalInput::DStart,
};

const FDigitalInput ControlMapper::gPressInputs[] = {
    nullptr,
    &CFinalInput::PLAUp,
    &CFinalInput::PLADown,
    &CFinalInput::PLALeft,
    &CFinalInput::PLARight,
    &CFinalInput::PRAUp,
    &CFinalInput::PRADown,
    &CFinalInput::PRALeft,
    &CFinalInput::PRARight,
    &CFinalInput::PLTrigger,
    &CFinalInput::PRTrigger,
    &CFinalInput::PDPUp,
    &CFinalInput::PDPDown,
    &CFinalInput::PDPLeft,
    &CFinalInput::PDPRight,
    &CFinalInput::PA,
    &CFinalInput::PB,
    &CFinalInput::PX,
    &CFinalInput::PY,
    &CFinalInput::PZ,
    &CFinalInput::PL,
    &CFinalInput::PR,
    &CFinalInput::PStart,
};

const char* ControlMapper::GetDescriptionForFunction(EFunctionList function) {
  switch (function) {
  case kFL_None:
    return "None";
  case kFL_LeftStickUp:
    return "Left Stick Up";
  case kFL_LeftStickDown:
    return "Left Stick Down";
  case kFL_LeftStickLeft:
    return "Left Stick Left";
  case kFL_LeftStickRight:
    return "Left Stick Right";
  case kFL_RightStickUp:
    return "Right Stick Up";
  case kFL_RightStickDown:
    return "Right Stick Down";
  case kFL_RightStickLeft:
    return "Right Stick Left";
  case kFL_RightStickRight:
    return "Right Stick Right";
  case kFL_LeftTrigger:
    return "Left Trigger";
  case kFL_RightTrigger:
    return "Right Trigger";
  case kFL_DPadUp:
    return "D-Pad Up   ";
  case kFL_DPadDown:
    return "D-Pad Down ";
  case kFL_DPadLeft:
    return "D-Pad Left ";
  case kFL_DPadRight:
    return "D-Pad Right";
  case kFL_AButton:
    return "A Button";
  case kFL_BButton:
    return "B Button";
  case kFL_XButton:
    return "X Button";
  case kFL_YButton:
    return "Y Button";
  case kFL_ZButton:
    return "Z Button";
  case kFL_LeftTriggerPress:
    return "Left Trigger Press";
  case kFL_RightTriggerPress:
    return "Right Trigger Press";
  case kFL_Start:
    return "Start";
  }

  return "UNKNOWN";
}

const char* ControlMapper::GetDescriptionForCommand(ECommands command) {
  switch (command) {
  case kC_Forward:
    return "Forward";
  case kC_Backward:
    return "Backward";
  case kC_TurnLeft:
    return "Turn Left";
  case kC_TurnRight:
    return "Turn Right";
  case kC_StrafeLeft:
    return "Strafe Left";
  case kC_StrafeRight:
    return "Strafe Right";
  case kC_LookLeft:
    return "Look Left";
  case kC_LookRight:
    return "Look Right";
  case kC_LookUp:
    return "Look Up";
  case kC_LookDown:
    return "Look Down";
  case kC_JumpOrBoost:
    return "Jump/Boost";
  case kC_FireOrBomb:
    return "Fire/Bomb";
  case kC_MissileOrPowerBomb:
    return "Missile/PowerBomb";
  case kC_Morph:
    return "Morph";
  case kC_AimUp:
    return "Aim Up";
  case kC_AimDown:
    return "Aim Down";
  case kC_CycleBeamUp:
    return "Cycle Beam Up";
  case kC_CycleBeamDown:
    return "Cycle Beam Down";
  case kC_CycleItem:
    return "Cycle Item";
  case kC_PowerBeam:
    return "Power Beam";
  case kC_IceBeam:
    return "Ice Beam";
  case kC_WaveBeam:
    return "Wave Beam";
  case kC_PlasmaBeam:
    return "Plasma Beam";
  case kC_ToggleHolster:
    return "Toggle Holster";
  case kC_OrbitClose:
    return "Orbit Close";
  case kC_OrbitFar:
    return "Orbit Far";
  case kC_OrbitObject:
    return "Orbit Object";
  case kC_OrbitSelect:
    return "Orbit Select";
  case kC_OrbitConfirm:
    return "Orbit Confirm";
  case kC_OrbitLeft:
    return "Orbit Left";
  case kC_OrbitRight:
    return "Orbit Right";
  case kC_OrbitUp:
    return "Orbit Up";
  case kC_OrbitDown:
    return "Orbit Down";
  case kC_LookHold1:
    return "Look Hold1";
  case kC_LookHold2:
    return "Look Hold2";
  case kC_LookZoomIn:
    return "Look Zoom In";
  case kC_LookZoomOut:
    return "Look Zoom Out";
  case kC_AimHold:
    return "Aim Hold";
  case kC_MapCircleUp:
    return "Map Circle Up";
  case kC_MapCircleDown:
    return "Map Circle Down";
  case kC_MapCircleLeft:
    return "Map Circle Left";
  case kC_MapCircleRight:
    return "Map Circle Right";
  case kC_MapMoveForward:
    return "Map Move Forward";
  case kC_MapMoveBack:
    return "Map Move Back";
  case kC_MapMoveLeft:
    return "Map Move Left";
  case kC_MapMoveRight:
    return "Map Move Right";
  case kC_MapZoomIn:
    return "Map Zoom In";
  case kC_MapZoomOut:
    return "Map Zoom Out";
  case kC_SpiderBall:
    return "SpiderBall";
  case kC_ChaseCamera:
    return "Chase Camera";
  case kC_XrayVisor:
    return "XRay Visor";
  case kC_ThermoVisor:
    return "Thermo Visor";
  case kC_EnviroVisor:
    return "Enviro Visor";
  case kC_NoVisor:
    return "No Visor";
  case kC_VisorMenu:
    return "Visor Menu";
  case kC_VisorUp:
    return "Visor Up";
  case kC_VisorDown:
    return "Visor Down";
  case kC_UseShield:
    return "Use Shield";
  case kC_ScanItem:
    return "Scan Item";
  default:
    break;
  }

  return "UNKNOWN";
}

float ControlMapper::GetAnalogInput(ECommands command, const CFinalInput& input) {
#if defined(TARGET_PC)
  if (gCommandFilterFlag[command]) {
    if (BeamKeyHeld(command, input)) {
      return 1.f;
    }
    const EFunctionList mapping = ::GetMapping(command, input);
    if (gAnalogInputs[mapping] != nullptr) {
      return (input.*gAnalogInputs[mapping])();
    }
  }
  return 0.f;
#else
  if (gCommandFilterFlag[command]) {
    if (gAnalogInputs[gpTweakPlayerControlCurrent->GetMapping(command)] != nullptr) {
      return (input.*gAnalogInputs[gpTweakPlayerControlCurrent->GetMapping(command)])();
    }
  }
  return 0.f;
#endif
}

bool ControlMapper::GetDigitalInput(ECommands command, const CFinalInput& input) {
#if defined(TARGET_PC)
  if (gCommandFilterFlag[command]) {
    if (BeamKeyHeld(command, input)) {
      return true;
    }
    const EFunctionList mapping = ::GetMapping(command, input);
    if (gDigitalInputs[mapping] != nullptr) {
      return (input.*gDigitalInputs[mapping])();
    }
  }
  return false;
#else
  if (gCommandFilterFlag[command]) {
    if (gDigitalInputs[gpTweakPlayerControlCurrent->GetMapping(command)] != nullptr) {
      return (input.*gDigitalInputs[gpTweakPlayerControlCurrent->GetMapping(command)])();
    }
  }
  return false;
#endif
}

bool ControlMapper::GetPressInput(ECommands command, const CFinalInput& input) {
#if defined(TARGET_PC)
  if (gCommandFilterFlag[command]) {
    const EFunctionList mapping = ::GetMapping(command, input);
    if (gPressInputs[mapping] != nullptr) {
      return (input.*gPressInputs[mapping])();
    }
  }
  return false;
#else
  if (gCommandFilterFlag[command]) {
    if (gPressInputs[gpTweakPlayerControlCurrent->GetMapping(command)] != nullptr) {
      return (input.*gPressInputs[gpTweakPlayerControlCurrent->GetMapping(command)])();
    }
  }
  return false;
#endif
}

void ControlMapper::ResetCommandFilters() {
  for (int i = 0; i < gCommandFilterFlag.size(); ++i) {
    gCommandFilterFlag[i] = true;
  }
}

void ControlMapper::SetCommandFiltered(ECommands cmd, bool filtered) {
  gCommandFilterFlag[cmd] = filtered;
}
