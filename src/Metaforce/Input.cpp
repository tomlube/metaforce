#include "Metaforce/Input.hpp"

#include "Metaforce/UI/RuntimeConfig.hpp"

#include <aurora/binding.hpp>
#include <aurora/input.hpp>

#include <array>
#include <memory>
#include <numbers>

namespace metaforce::input {
namespace {

// Source and Quake's yaw/pitch scale (0.022 degrees per count) at sensitivity 2.
constexpr float kRadiansPerCountAt100 = 0.044f * std::numbers::pi_v< float > / 180.f;
// Capture is applied after the next event poll, so the first frames can still carry free cursor
// motion (or SDL's jump into relative mode). Drop them.
constexpr int kCaptureSettleFrames = 2;

aurora::input::LayerId sGameLayer = aurora::input::kInvalidLayerId;
bool sCaptureRequested = false;
bool sCaptureWanted = false;
int sCapturedFrames = 0;
float sMouseX = 0.f;
float sMouseY = 0.f;

// In beam command order: Power, Ice, Wave, Plasma.
std::array< aurora::binding::ControlId, 4 > sBeamControls{};
aurora::binding::State sBeamKeys;

void bind_beam_keys() {
  constexpr std::array< const char*, 4 > kNames{
      "metaforce.beam_power",
      "metaforce.beam_ice",
      "metaforce.beam_wave",
      "metaforce.beam_plasma",
  };
  // Acquisition order on 1-4: Power, Wave, Ice, Plasma.
  constexpr std::array< SDL_Scancode, 4 > kKeys{
      SDL_SCANCODE_1,
      SDL_SCANCODE_3,
      SDL_SCANCODE_2,
      SDL_SCANCODE_4,
  };
  const auto keyboard = aurora::input::keyboard_source().id;
  auto set = std::make_shared< aurora::binding::BindingSet >();
  for (size_t i = 0; i < sBeamControls.size(); ++i) {
    sBeamControls[i] = aurora::binding::register_control({
        .name = kNames[i],
        .kind = aurora::binding::ControlKind::Button,
    });
    set->bindings.push_back({
        .input = {.source = keyboard,
                  .control = aurora::binding::PhysicalInput::Key{.scancode = kKeys[i]}},
        .target = sBeamControls[i],
    });
  }
  (void)sBeamKeys.set_bindings(std::move(set));
}

aurora::input::EventResult on_game_event(const aurora::input::InputEvent& event, void*) {
  // Menus above take the keyboard while open; the router cancels held keys when they do.
  (void)sBeamKeys.process(event);
  if (sCaptureWanted && event.source.kind == aurora::input::InputSource::Kind::Mouse) {
    if (const auto* pointer = event.payload.get_if< aurora::input::InputEvent::PointerChanged >()) {
      if (pointer->phase == aurora::input::InputEvent::PointerChanged::Phase::Move) {
        sMouseX += pointer->delta.x;
        sMouseY += pointer->delta.y;
      }
    }
  }
  // The PAD layer below still needs the keys and mouse buttons.
  return aurora::input::EventResult::Pass;
}

aurora::input::PointerMode mouse_pointer_mode(void*) {
  return sCaptureWanted ? aurora::input::PointerMode::Relative
                        : aurora::input::PointerMode::None;
}

// CDolphinController::ProcessAnalogButton scales clamped trigger values by 1/150.
constexpr u8 kTriggerFull = 150;
// Mirrors CFinalInput::kInput_AnalogTriggerOnThreshhold (private), the DLTrigger/soft-lock cutoff.
constexpr float kAnalogTriggerOnThreshold = 0.05f;

bool sPrevDigitalL[PAD_CHANMAX] = {};
u8 sPrevTriggerL[PAD_CHANMAX] = {};

// Set by a Z press seen in game; cleared by a beam chord, Start, or opening the map. The map
// screen eats the Z press that closes it, so a Z release without a press we saw never opens it.
bool sMapArmed = false;

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

bool ModernControlsEnabled() { return ui::GetRuntimeConfig().input.modernControls.getValue(); }

bool FilterMapButton(bool zPressed, bool zHeld, bool dpadPressed, bool startPressed) {
  if (!ModernControlsEnabled()) {
    sMapArmed = false;
    return zPressed;
  }

  if (zPressed) {
    sMapArmed = true;
  }
  if ((zHeld && dpadPressed) || startPressed) {
    sMapArmed = false;
  }
  if (!zHeld && sMapArmed) {
    sMapArmed = false;
    return true;
  }
  return false;
}

void InitializeGameInput() {
  if (sGameLayer != aurora::input::kInvalidLayerId) {
    return;
  }
  bind_beam_keys();
  // Above the PAD layer so it sees motion first, below RmlUi and ImGui so their menus take the
  // cursor back.
  sGameLayer = aurora::input::register_layer({
      .label = "metaforce.game",
      .priority = aurora::input::kGameLayerPriority + 1,
      .onEvent = on_game_event,
      .pointerMode = mouse_pointer_mode,
  });
}

void ShutdownGameInput() {
  if (sGameLayer != aurora::input::kInvalidLayerId) {
    aurora::input::unregister_layer(sGameLayer);
    sGameLayer = aurora::input::kInvalidLayerId;
  }
  (void)sBeamKeys.reset();
  sCaptureRequested = false;
  sCaptureWanted = false;
}

unsigned char BeamKeysHeld() {
  unsigned char held = 0;
  for (size_t i = 0; i < sBeamControls.size(); ++i) {
    if (sBeamControls[i] != aurora::binding::kInvalidControlId &&
        sBeamKeys.value(sBeamControls[i]) >= 0.5f) {
      held |= 1 << i;
    }
  }
  return held;
}

bool MouseLookEnabled() {
  const auto& input = ui::GetRuntimeConfig().input;
  return input.modernControls.getValue() && input.mouseLook.getValue();
}

void RequestMouseCapture() { sCaptureRequested = true; }

void ConsumeMouseDelta(float& x, float& y) {
  x = sMouseX;
  y = sMouseY;
  sMouseX = 0.f;
  sMouseY = 0.f;

  sCaptureWanted = sCaptureRequested && MouseLookEnabled();
  sCaptureRequested = false;
  // Another layer (a menu) can hold the cursor even while gameplay asks for it.
  if (!sCaptureWanted || aurora::input::pointer_mode() != aurora::input::PointerMode::Relative) {
    sCapturedFrames = 0;
  }
  if (sCapturedFrames < kCaptureSettleFrames) {
    ++sCapturedFrames;
    x = 0.f;
    y = 0.f;
  }
}

float MouseRadiansPerCount() {
  return kRadiansPerCountAt100 *
         static_cast< float >(ui::GetRuntimeConfig().input.mouseSensitivity.getValue()) / 100.f;
}

bool InvertMouseY() { return ui::GetRuntimeConfig().input.invertMouseY.getValue(); }

bool SquareDiagonalLook() { return ui::GetRuntimeConfig().input.squareDiagonalLook.getValue(); }

bool AimAssistEnabled() { return ui::GetRuntimeConfig().input.aimAssist.getValue(); }

} // namespace metaforce::input
