/**
 * Hand tracking — XR_EXT_hand_tracking, 26 joints per hand.
 */

#include "xr_hand_tracking.h"
#include "protocol.h"

#include <android/log.h>
#include <cstring>

#define LOG_TAG "QuestBridge_Hand"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

namespace xr_hand_tracking {

// Function pointers (loaded at runtime since this is an extension)
static PFN_xrCreateHandTrackerEXT xrCreateHandTrackerEXT_ = nullptr;
static PFN_xrDestroyHandTrackerEXT xrDestroyHandTrackerEXT_ = nullptr;
static PFN_xrLocateHandJointsEXT xrLocateHandJointsEXT_ = nullptr;

static XrHandTrackerEXT trackers[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
static XrHandJointLocationEXT joint_locations[2][XR_HAND_JOINT_COUNT_EXT];
static bool joints_valid[2] = {false, false};
static bool initialized = false;

bool init(XrInstance instance, XrSession session) {
    // Load extension functions
    if (xrGetInstanceProcAddr(instance, "xrCreateHandTrackerEXT",
            reinterpret_cast<PFN_xrVoidFunction*>(&xrCreateHandTrackerEXT_)) != XR_SUCCESS) {
        LOGW("XR_EXT_hand_tracking not available");
        return false;
    }
    xrGetInstanceProcAddr(instance, "xrDestroyHandTrackerEXT",
        reinterpret_cast<PFN_xrVoidFunction*>(&xrDestroyHandTrackerEXT_));
    xrGetInstanceProcAddr(instance, "xrLocateHandJointsEXT",
        reinterpret_cast<PFN_xrVoidFunction*>(&xrLocateHandJointsEXT_));

    // Create hand trackers
    for (int hand = 0; hand < 2; ++hand) {
        XrHandTrackerCreateInfoEXT info{XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT};
        info.hand = (hand == 0) ? XR_HAND_LEFT_EXT : XR_HAND_RIGHT_EXT;
        info.handJointSet = XR_HAND_JOINT_SET_DEFAULT_EXT;

        XrResult result = xrCreateHandTrackerEXT_(session, &info, &trackers[hand]);
        if (result != XR_SUCCESS) {
            LOGW("Failed to create hand tracker %d: %d", hand, result);
            trackers[hand] = XR_NULL_HANDLE;
        }
    }

    initialized = true;
    LOGI("Hand tracking initialized");
    return true;
}

void update(XrSpace space, XrTime time) {
    if (!initialized) return;

    for (int hand = 0; hand < 2; ++hand) {
        joints_valid[hand] = false;
        if (trackers[hand] == XR_NULL_HANDLE) continue;

        XrHandJointsLocateInfoEXT locate_info{XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT};
        locate_info.baseSpace = space;
        locate_info.time = time;

        XrHandJointLocationsEXT locations{XR_TYPE_HAND_JOINT_LOCATIONS_EXT};
        locations.jointCount = XR_HAND_JOINT_COUNT_EXT;
        locations.jointLocations = joint_locations[hand];

        XrResult result = xrLocateHandJointsEXT_(trackers[hand], &locate_info, &locations);
        if (result == XR_SUCCESS && locations.isActive == XR_TRUE) {
            joints_valid[hand] = true;
        }
    }
}

bool is_active(int hand) {
    return joints_valid[hand & 1];
}

size_t pack_payload(int hand, uint8_t* out) {
    hand &= 1;
    if (!joints_valid[hand]) return 0;

    // [1B: hand] [26 joints × 28B: 7× float32 BE pose]
    out[0] = static_cast<uint8_t>(hand);
    uint8_t* p = out + 1;

    for (int j = 0; j < static_cast<int>(protocol::HAND_JOINT_COUNT); ++j) {
        const auto& loc = joint_locations[hand][j];
        float pose[7] = {
            loc.pose.position.x,
            loc.pose.position.y,
            loc.pose.position.z,
            loc.pose.orientation.x,
            loc.pose.orientation.y,
            loc.pose.orientation.z,
            loc.pose.orientation.w,
        };
        for (int i = 0; i < 7; ++i) {
            protocol::float_to_be(pose[i], p + i * 4);
        }
        p += protocol::POSE_SIZE;
    }

    return protocol::HAND_STATE_SIZE;
}

void destroy() {
    if (!initialized) return;
    for (auto& tracker : trackers) {
        if (tracker != XR_NULL_HANDLE && xrDestroyHandTrackerEXT_) {
            xrDestroyHandTrackerEXT_(tracker);
            tracker = XR_NULL_HANDLE;
        }
    }
    initialized = false;
}

}  // namespace xr_hand_tracking
