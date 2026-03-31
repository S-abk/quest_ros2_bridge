/**
 * Controller input — OpenXR action system for 6DoF + buttons + analog.
 */

#include "xr_controller.h"
#include "protocol.h"

#include <android/log.h>
#include <cstring>
#include <string>

#define LOG_TAG "QuestBridge_Ctrl"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#define XR_CHECK(call)                                                         \
    do {                                                                        \
        XrResult _r = (call);                                                   \
        if (XR_FAILED(_r)) {                                                    \
            LOGE("%s failed: %d", #call, _r);                                   \
            return _r;                                                          \
        }                                                                       \
    } while (0)

namespace xr_controller {

// Internal state
static XrActionSet action_set = XR_NULL_HANDLE;
static XrAction pose_action = XR_NULL_HANDLE;
static XrAction trigger_action = XR_NULL_HANDLE;
static XrAction grip_action = XR_NULL_HANDLE;
static XrAction btn_x_a_action = XR_NULL_HANDLE;
static XrAction btn_y_b_action = XR_NULL_HANDLE;
static XrAction btn_menu_action = XR_NULL_HANDLE;
static XrAction btn_thumbstick_action = XR_NULL_HANDLE;
static XrAction haptic_action = XR_NULL_HANDLE;

static XrSpace hand_spaces[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
static XrPath hand_paths[2];
static ControllerState states[2] = {};

// Helper to create an action
static XrResult create_action(XrActionSet as, const char* name, const char* localized,
                               XrActionType type, XrAction* out) {
    XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
    std::strncpy(info.actionName, name, XR_MAX_ACTION_NAME_SIZE);
    std::strncpy(info.localizedActionName, localized, XR_MAX_LOCALIZED_ACTION_NAME_SIZE);
    info.actionType = type;
    info.countSubactionPaths = 2;
    info.subactionPaths = hand_paths;
    return xrCreateAction(as, &info, out);
}

XrResult init(XrInstance instance, XrSession session) {
    // Hand paths
    xrStringToPath(instance, "/user/hand/left", &hand_paths[0]);
    xrStringToPath(instance, "/user/hand/right", &hand_paths[1]);

    // Action set
    XrActionSetCreateInfo as_info{XR_TYPE_ACTION_SET_CREATE_INFO};
    std::strncpy(as_info.actionSetName, "controller", XR_MAX_ACTION_SET_NAME_SIZE);
    std::strncpy(as_info.localizedActionSetName, "Controller", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE);
    as_info.priority = 0;
    XR_CHECK(xrCreateActionSet(instance, &as_info, &action_set));

    // Actions
    XR_CHECK(create_action(action_set, "hand_pose", "Hand Pose",
                           XR_ACTION_TYPE_POSE_INPUT, &pose_action));
    XR_CHECK(create_action(action_set, "trigger", "Trigger",
                           XR_ACTION_TYPE_FLOAT_INPUT, &trigger_action));
    XR_CHECK(create_action(action_set, "grip", "Grip",
                           XR_ACTION_TYPE_FLOAT_INPUT, &grip_action));
    XR_CHECK(create_action(action_set, "btn_x_a", "X/A Button",
                           XR_ACTION_TYPE_BOOLEAN_INPUT, &btn_x_a_action));
    XR_CHECK(create_action(action_set, "btn_y_b", "Y/B Button",
                           XR_ACTION_TYPE_BOOLEAN_INPUT, &btn_y_b_action));
    XR_CHECK(create_action(action_set, "btn_menu", "Menu Button",
                           XR_ACTION_TYPE_BOOLEAN_INPUT, &btn_menu_action));
    XR_CHECK(create_action(action_set, "btn_thumbstick", "Thumbstick Click",
                           XR_ACTION_TYPE_BOOLEAN_INPUT, &btn_thumbstick_action));
    XR_CHECK(create_action(action_set, "haptic", "Haptic Vibration",
                           XR_ACTION_TYPE_VIBRATION_OUTPUT, &haptic_action));

    // Suggest interaction profile: Oculus Touch
    XrPath touch_profile;
    xrStringToPath(instance, "/interaction_profiles/oculus/touch_controller", &touch_profile);

    auto path = [&](const char* p) {
        XrPath xp;
        xrStringToPath(instance, p, &xp);
        return xp;
    };

    XrActionSuggestedBinding bindings[] = {
        {pose_action,          path("/user/hand/left/input/grip/pose")},
        {pose_action,          path("/user/hand/right/input/grip/pose")},
        {trigger_action,       path("/user/hand/left/input/trigger/value")},
        {trigger_action,       path("/user/hand/right/input/trigger/value")},
        {grip_action,          path("/user/hand/left/input/squeeze/value")},
        {grip_action,          path("/user/hand/right/input/squeeze/value")},
        {btn_x_a_action,       path("/user/hand/left/input/x/click")},
        {btn_x_a_action,       path("/user/hand/right/input/a/click")},
        {btn_y_b_action,       path("/user/hand/left/input/y/click")},
        {btn_y_b_action,       path("/user/hand/right/input/b/click")},
        {btn_menu_action,      path("/user/hand/left/input/menu/click")},
        {btn_thumbstick_action, path("/user/hand/left/input/thumbstick/click")},
        {btn_thumbstick_action, path("/user/hand/right/input/thumbstick/click")},
        {haptic_action,         path("/user/hand/left/output/haptic")},
        {haptic_action,         path("/user/hand/right/output/haptic")},
    };

    XrInteractionProfileSuggestedBinding suggested{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    suggested.interactionProfile = touch_profile;
    suggested.suggestedBindings = bindings;
    suggested.countSuggestedBindings = sizeof(bindings) / sizeof(bindings[0]);
    XR_CHECK(xrSuggestInteractionProfileBindings(instance, &suggested));

    // Action spaces for poses
    for (int hand = 0; hand < 2; ++hand) {
        XrActionSpaceCreateInfo space_info{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        space_info.action = pose_action;
        space_info.subactionPath = hand_paths[hand];
        space_info.poseInActionSpace = {{0, 0, 0, 1}, {0, 0, 0}};
        XR_CHECK(xrCreateActionSpace(session, &space_info, &hand_spaces[hand]));
    }

    // Attach action set to session
    XrSessionActionSetsAttachInfo attach_info{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attach_info.countActionSets = 1;
    attach_info.actionSets = &action_set;
    XR_CHECK(xrAttachSessionActionSets(session, &attach_info));

    LOGI("Controller actions initialized");
    return XR_SUCCESS;
}

void update(XrSession session, XrSpace space, XrTime time) {
    // Sync actions
    XrActiveActionSet active{action_set, XR_NULL_PATH};
    XrActionsSyncInfo sync_info{XR_TYPE_ACTIONS_SYNC_INFO};
    sync_info.countActiveActionSets = 1;
    sync_info.activeActionSets = &active;
    xrSyncActions(session, &sync_info);

    for (int hand = 0; hand < 2; ++hand) {
        auto& s = states[hand];
        s.buttons_low = 0;
        s.buttons_high = 0;

        // Pose
        XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
        xrLocateSpace(hand_spaces[hand], space, time, &loc);
        if (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) {
            s.x = loc.pose.position.x;
            s.y = loc.pose.position.y;
            s.z = loc.pose.position.z;
        }
        if (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) {
            s.qx = loc.pose.orientation.x;
            s.qy = loc.pose.orientation.y;
            s.qz = loc.pose.orientation.z;
            s.qw = loc.pose.orientation.w;
        }

        // Trigger
        XrActionStateGetInfo get_info{XR_TYPE_ACTION_STATE_GET_INFO};
        get_info.subactionPath = hand_paths[hand];

        get_info.action = trigger_action;
        XrActionStateFloat trigger_state{XR_TYPE_ACTION_STATE_FLOAT};
        xrGetActionStateFloat(session, &get_info, &trigger_state);
        s.trigger = trigger_state.currentState;

        // Grip
        get_info.action = grip_action;
        XrActionStateFloat grip_state{XR_TYPE_ACTION_STATE_FLOAT};
        xrGetActionStateFloat(session, &get_info, &grip_state);
        s.grip = grip_state.currentState;

        // Buttons
        auto get_bool = [&](XrAction action) -> bool {
            get_info.action = action;
            XrActionStateBoolean bs{XR_TYPE_ACTION_STATE_BOOLEAN};
            xrGetActionStateBoolean(session, &get_info, &bs);
            return bs.currentState == XR_TRUE;
        };

        if (get_bool(btn_x_a_action))       s.buttons_low |= BTN_X_A;
        if (get_bool(btn_y_b_action))       s.buttons_low |= BTN_Y_B;
        if (get_bool(btn_menu_action))       s.buttons_low |= BTN_MENU;
        if (get_bool(btn_thumbstick_action)) s.buttons_low |= BTN_THUMBSTICK;
    }
}

const ControllerState& get_state(int hand) {
    return states[hand & 1];
}

void pack_payload(uint8_t* out) {
    // Pack left (32 bytes) then right (32 bytes) = 64 bytes total
    for (int hand = 0; hand < 2; ++hand) {
        uint8_t* p = out + hand * protocol::CONTROLLER_BLOCK_SIZE;
        const auto& s = states[hand];

        p[0] = s.buttons_low;
        p[1] = s.buttons_high;
        p[2] = static_cast<uint8_t>(s.trigger * 255.0f);
        p[3] = static_cast<uint8_t>(s.grip * 255.0f);

        // Pose: 7 × float32 BE [x, y, z, qx, qy, qz, qw]
        float pose[7] = {s.x, s.y, s.z, s.qx, s.qy, s.qz, s.qw};
        for (int i = 0; i < 7; ++i) {
            protocol::float_to_be(pose[i], p + 4 + i * 4);
        }
    }
}

void apply_haptic(XrSession session, int hand, float intensity, float duration_ms) {
    if (hand < 0 || hand > 1) return;

    XrHapticActionInfo haptic_info{XR_TYPE_HAPTIC_ACTION_INFO};
    haptic_info.action = haptic_action;
    haptic_info.subactionPath = hand_paths[hand];

    if (intensity <= 0.0f) {
        xrStopHapticFeedback(session, &haptic_info);
        return;
    }

    XrHapticVibration vibration{XR_TYPE_HAPTIC_VIBRATION};
    vibration.amplitude = intensity;
    vibration.frequency = XR_FREQUENCY_UNSPECIFIED;
    vibration.duration = static_cast<XrDuration>(duration_ms * 1000000.0f);  // ms → ns

    xrApplyHapticFeedback(session, &haptic_info,
                          reinterpret_cast<XrHapticBaseHeader*>(&vibration));
}

void destroy(XrInstance instance) {
    for (auto& sp : hand_spaces) {
        if (sp != XR_NULL_HANDLE) {
            xrDestroySpace(sp);
            sp = XR_NULL_HANDLE;
        }
    }
    if (action_set != XR_NULL_HANDLE) {
        xrDestroyActionSet(action_set);
        action_set = XR_NULL_HANDLE;
    }
}

}  // namespace xr_controller
