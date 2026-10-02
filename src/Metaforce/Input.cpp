#include "Metaforce/Input.hpp"

#include "Metaforce/UI/RuntimeConfig.hpp"

namespace metaforce::input {
namespace {

// CDolphinController::ProcessAnalogButton scales clamped trigger values by 1/150.
constexpr u8 kTriggerFull = 150;
// Mirrors CFinalInput::kInput_AnalogTriggerOnThreshhold (private), the DLTrigger/soft-lock cutoff.
constexpr float kAnalogTriggerOnThreshold = 0.05f;

bool sPrevDigitalL[PAD_CHANMAX] = {};
u8 sPrevTriggerL[PAD_CHANMAX] = {};

bool soft_lock_active(u8 triggerLeft) {
  return static_cast< float >(triggerLeft) / kTriggerFull > kAnalogTriggerOnThreshold;
}

} // namespace

void ApplySmartLockOn(PADStatus* status) {
  const bool enabled = ui::GetRuntimeConfig().input.smartLockOn.getValue();
  for (u32 port = 0; port < PAD_CHANMAX; ++port) {
    PADStatus& pad = status[port];
    if (pad.err != PAD_ERR_NONE) {
      sPrevDigitalL[port] = false;
      sPrevTriggerL[port] = 0;
      continue;
    }

    const bool digitalL = (pad.button & PAD_TRIGGER_L) != 0;
    const bool pressed = digitalL && !sPrevDigitalL[port];
    const bool analogLed = soft_lock_active(sPrevTriggerL[port]);
    sPrevDigitalL[port] = digitalL;
    if (enabled && digitalL) {
      // Hold back the click on the frame it lands unless analog L already started a soft lock.
      if (pressed && !analogLed) {
        pad.button &= ~PAD_TRIGGER_L;
      }
      // A GameCube click always comes with a fully pulled trigger.
      pad.triggerLeft = kTriggerFull;
    }
    sPrevTriggerL[port] = pad.triggerLeft;
  }
}

} // namespace metaforce::input
