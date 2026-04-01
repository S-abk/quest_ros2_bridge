---
title: Hardware Testing & Diagnostics
tags: [testing, diagnostics, adb, coordinate-system, stage-space]
---

# Hardware Testing & Diagnostics

## diagnose_camera.sh

A single-terminal diagnostic script that runs four parallel probes for 15 seconds, then prints a PASS/FAIL summary. Not committed to the repo (local-only tool in `.gitignore`).

### Probe 1: Test Image Publisher (`[PUB]`)

Publishes a 320×240 RGB gradient image to `/camera/image_raw` at 10 Hz using `bytearray` (no cv_bridge, no numpy in the publisher itself). The gradient has R varying by X, G by Y, B constant at 128 — producing a non-trivial image that's visually distinguishable from black or noise.

**PASS condition:** Log contains "published N frames total"

### Probe 2: Image Subscriber (`[SUB]`)

Subscribes to `/camera/image_raw` and counts frames received over the 15-second window.

**PASS condition:** Log contains "received N frames total"

### Probe 3: Bridge JPEG Enqueue (`[BRIDGE]`)

Searches the ROS 2 log directory (`~/.ros/log/`) for "Enqueuing JPEG" debug lines from bridge_node. This confirms the thread-safe `call_soon_threadsafe` queue put is working.

**PASS condition:** Log contains "enqueuing jpeg" (case-insensitive)

**Important:** The bridge node emits this at DEBUG level. You must run it with:
```bash
ros2 run ros2_bridge bridge_node --ros-args --log-level quest_bridge:=debug
```

### Probe 4: Android Logcat (`[ADB]`)

Tails `adb logcat -s QuestBridge_Cam:* QuestBridge_WS:* QuestBridge:*` for camera-related output from the C++ app.

**PASS condition:** Log contains "QuestBridge_Cam" or "QuestBridge_WS"

**Common gotcha:** An earlier version used the pattern `QuestBridge|camera|frame` which matched unrelated kernel `qcom_camera` lines — a false positive. The fix was to require exact tag matches.

### Shutdown

Both Python probes wrap `rclpy.shutdown()` in `try/except` to prevent core dumps when the process is killed at the end of the 15-second window.

## Quest STAGE Space Coordinate System

This is the single most important thing to understand when positioning objects in the headset.

### The Gotcha

OpenXR uses a right-handed coordinate system where -Z is "forward" in local/view space. But **Quest STAGE space has +Z as the user's forward direction** — the direction they were facing when they set up their guardian boundary.

This means:
- `position = {0, 1.5, -2}` puts an object 2m **behind** the user
- `position = {0, 1.5, +2}` puts an object 2m **in front** of the user
- Y is up, X is right (from the user's perspective at setup time)

### Quad Facing Direction

`XrCompositionLayerQuad` has its normal pointing along +Z in its local space. When placed at +Z in STAGE space, the quad faces **away** from the user (back-face culled = invisible).

The fix is a 180° yaw rotation:
```cpp
pose.orientation = {0.0f, 1.0f, 0.0f, 0.0f};  // quat for 180° around Y
pose.position = {0.0f, 1.5f, 2.0f};
```

This was discovered through bisection: Z=-2 showed the panel behind the user, Z=+2 made it invisible, Z=+2 with 180° yaw fixed it. See [[camera_panel]] Bug 4 and Bug 5 for the full story.

### How We Discovered This

1. Panel at `{0, 1.5, -2}` with identity orientation → visible behind user
2. Changed to `{0, 1.5, +2}` → panel disappeared entirely
3. Bisected: reverted Z to -2 → reappeared behind user (confirming Z change was the cause)
4. Root cause: quad at +Z faces away from user (normal along +Z = same direction as displacement)
5. Added 180° yaw → panel visible in front of user, correctly oriented

## Deployment Workflow

```bash
# Build
cd android_app
./gradlew assembleDebug

# Deploy
adb install -r app/build/outputs/apk/debug/app-debug.apk
adb forward tcp:9090 tcp:9090
adb shell am start -n com.quest.bridge/.MainActivity

# Monitor
adb logcat -s QuestBridge:* QuestBridge_Cam:* QuestBridge_WS:* QuestBridge_Ctrl:* QuestBridge_Hand:*
```

### All Android Log Tags

| Tag | Source File | Content |
|---|---|---|
| `QuestBridge` | bridge_app.cpp | Session lifecycle, main loop |
| `QuestBridge_Ctrl` | xr_controller.cpp | Controller init, action system |
| `QuestBridge_Hand` | xr_hand_tracking.cpp | Hand tracker init |
| `QuestBridge_WS` | ws_server.cpp | Client connect/disconnect |
| `QuestBridge_Cam` | camera_renderer.cpp | Frame receipt, swapchain creation |

## Smoke Tests

```bash
# Controller poses
ros2 topic echo /oculus/right/pose

# Haptics — feel vibration in right controller
ros2 topic pub /oculus/haptic/right std_msgs/msg/Float32 "data: 0.8" --once
ros2 topic pub /oculus/haptic/right std_msgs/msg/Float32 "data: 0.0" --once

# Camera panel — publish a test image
ros2 run image_publisher image_publisher_node /path/to/test.jpg \
  --ros-args -r image:=/camera/image_raw

# Hand tracking
ros2 topic echo /oculus/right/hand
```

## Tested Configurations

| | Quest 2 | Quest 3 | Quest 3S | Quest Pro |
|---|---|---|---|---|
| **Humble** | ⬜ | ⬜ | ⬜ | ⬜ |
| **Jazzy** | ⬜ | ⬜ | ⬜ | ⬜ |
| **Kilted** | ✅ | ⬜ | ⬜ | ⬜ |

## Related Docs

- [[camera_panel]] — full bug history and pipeline details
- [[architecture]] — system overview and threading
- [[android_build]] — build environment setup
- [[ros2_setup]] — ROS 2 workspace and topic reference
