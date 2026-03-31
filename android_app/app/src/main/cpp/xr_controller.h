#pragma once
/**
 * Controller input — 6DoF pose, buttons, trigger, grip.
 * Reads from OpenXR action system each frame.
 */

#include <openxr/openxr.h>
#include <cstdint>

namespace xr_controller {

struct ControllerState {
    // Pose: position + orientation
    float x, y, z;
    float qx, qy, qz, qw;

    // Buttons
    uint8_t buttons_low;   // A/B/X/Y/menu/system (bitmask)
    uint8_t buttons_high;  // thumbstick click, etc.

    // Analog
    float trigger;  // 0.0–1.0
    float grip;     // 0.0–1.0
};

// Button bitmask definitions (buttons_low)
constexpr uint8_t BTN_X_A          = 0x01;  // X (left) or A (right)
constexpr uint8_t BTN_Y_B          = 0x02;  // Y (left) or B (right)
constexpr uint8_t BTN_MENU         = 0x04;
constexpr uint8_t BTN_THUMBSTICK   = 0x08;
constexpr uint8_t BTN_TRIGGER_TOUCH = 0x10;
constexpr uint8_t BTN_THUMB_TOUCH  = 0x20;
// buttons_high reserved for future use

/**
 * Call once after xrCreateSession to set up action sets and bindings.
 * Returns XR_SUCCESS or an error code.
 */
XrResult init(XrInstance instance, XrSession session);

/**
 * Call each frame after xrSyncActions to read controller state.
 * space: the reference space to locate controllers in.
 * time: the predicted display time.
 */
void update(XrSession session, XrSpace space, XrTime time);

/** Retrieve latest state for left (0) or right (1) controller. */
const ControllerState& get_state(int hand);

/** Pack both controllers into the 64-byte CONTROLLER_STATE payload. */
void pack_payload(uint8_t* out);

/**
 * Apply haptic vibration to a controller.
 * hand: 0=left, 1=right.
 * intensity: 0.0–1.0.
 * duration_ms: vibration duration in milliseconds (0 = stop).
 */
void apply_haptic(XrSession session, int hand, float intensity, float duration_ms);

/** Clean up action-related resources. */
void destroy(XrInstance instance);

}  // namespace xr_controller
