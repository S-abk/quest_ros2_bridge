#pragma once
/**
 * Hand tracking — XR_EXT_hand_tracking, 26 joints per hand.
 */

#include <openxr/openxr.h>
#include <cstdint>

namespace xr_hand_tracking {

/**
 * Initialize hand trackers. Must be called after session creation.
 * Returns false if XR_EXT_hand_tracking is not available.
 */
bool init(XrInstance instance, XrSession session);

/**
 * Locate hand joints for a given time.
 * Call each frame.
 */
void update(XrSpace space, XrTime time);

/**
 * Returns true if hand tracking data is available for the given hand.
 * hand: 0=left, 1=right.
 */
bool is_active(int hand);

/**
 * Pack HAND_STATE payload for the given hand (729 bytes).
 * Returns number of bytes written (729), or 0 if hand not tracked.
 */
size_t pack_payload(int hand, uint8_t* out);

/**
 * Clean up hand tracker resources.
 */
void destroy();

}  // namespace xr_hand_tracking
