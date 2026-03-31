# CLAUDE.md — quest_ros2_bridge

## Project Goal

A modern, from-scratch replacement for `rail-berkeley/oculus_reader`.
Bidirectional ROS 2 ↔ Quest bridge with no legacy SDK dependencies.

Inspired by oculus_reader but built clean with:
- OpenXR (no old VrApi/Oculus Mobile SDK)
- WebSocket (no logcat hack)
- ROS 2 multi-distro support (Humble LTS baseline, Jazzy LTS, Kilted)
- Modern Android Studio + Gradle 8 + NDK r25c
- Compatible with Quest 2, Quest 3, Quest 3S, Quest Pro without code changes

---

## Resuming after a context reset

If you are reading this mid-project:

1. Run `git log --oneline` to see which commits are complete.
2. Run `pytest tests/test_protocol.py -v` to confirm the protocol layer is healthy.
3. Read `DEPENDENCY_VERSIONS.md` for all confirmed pinned versions — do not
   re-look them up or alter them.
4. Continue from the next unfinished commit in the starter prompt.

---

## Decision policy for ambiguous cases

If the spec is silent on a detail, make the simplest reasonable choice and
leave a `// TODO(spec-gap): <what you chose and why>` comment so it can be
reviewed. Do not silently pick a non-obvious behaviour.

Stop and ask before proceeding if any ambiguity affects:
- The wire protocol (message layout, byte order, field sizes)
- The ROS 2 topic names, message types, or frame IDs
- Which OpenXR extensions are required vs optional

---

## Features

1. **Controller poses + buttons** — read 6DoF transforms and button states, publish to ROS 2
2. **Haptic vibration** — subscribe to ROS 2 topics, vibrate controllers
3. **Robot camera feed in headset** — subscribe to ROS 2 camera topics, render as floating panel
4. **Hand tracking** — read hand joint poses, publish to ROS 2

---

## Compatibility Matrix

### ROS 2 Distros (target all three)

| Distro | Status | Python | Notes |
|---|---|---|---|
| Humble | LTS until May 2027 | 3.10 | **Baseline** — use only APIs available here |
| Jazzy | LTS until May 2029 | 3.12 | Current LTS |
| Kilted | Latest | 3.12 | Dev environment |

**Rules:**
- Use only rclpy APIs present in Humble as the baseline
- Use `self.get_logger().warning()` — not `.warn()` (deprecated in Humble)
- Import message types explicitly: `from geometry_msgs.msg import PoseStamped`
- All message package deps declared in `package.xml` with `<exec_depend>` for rosdep
- Parameters via `--ros-args` syntax (consistent across all distros)

### Python Versions
- Support **3.10, 3.11, 3.12**
- No `match/case` statements (avoid for 3.10 baseline clarity)
- Use `asyncio.new_event_loop()` explicitly — do not rely on `get_event_loop()` (behaviour changed in 3.10)
- Pin `websockets>=11.0` in install_requires — major API break at v10→v11

### Quest Headsets
- Quest 2, Quest 3, Quest 3S, Quest Pro — all arm64, all OpenXR 1.0
- OpenXR runtime differences handled automatically by the Meta runtime
- Optional extensions (passthrough, eye tracking, face tracking) requested gracefully
  with fallback if unavailable — do not hard-require them

### Android Build
- `minSdk 29` (Quest 2 minimum)
- `targetSdk 33` (Meta submission requirement)
- `abiFilters 'arm64-v8a'` only — all Quest headsets are arm64, no 32-bit needed
- **NDK**: pin to `r25c` (25.2.9519653)
- **OpenXR loader**: see `DEPENDENCY_VERSIONS.md` — do not use `+` or `latest`

---

## Environment (dev machine)

- **ROS 2**: Kilted Kaiju (dev), Humble-compatible API baseline
- **Python**: rclpy (not rospy)
- **Android Studio**: latest stable
- **Gradle**: 8.x, AGP 8.x
- **OpenXR**: `org.khronos.openxr:openxr_loader_for_android` — version in `DEPENDENCY_VERSIONS.md`

---

## Repo Structure

```
quest_ros2_bridge/
  android_app/                   # Android Studio project
    app/
      src/main/
        cpp/
          uWebSockets/           # vendored — see "Vendoring uWebSockets" below
          uSockets/              # vendored — required by uWebSockets
          CMakeLists.txt
          bridge_app.cpp         # Main OpenXR app loop
          xr_controller.cpp/.h   # Controller input + haptics
          xr_hand_tracking.cpp/.h # Hand joint tracking
          camera_renderer.cpp/.h  # Quad layer camera panel
          ws_server.cpp/.h       # WebSocket server (uWebSockets)
          protocol.h             # Message type definitions (mirrors protocol.py)
        java/com/quest/bridge/
          MainActivity.kt        # Minimal Kotlin glue
        AndroidManifest.xml
      build.gradle
    build.gradle
    settings.gradle
    local.properties             # NOT committed — each developer creates this locally

  ros2_bridge/                   # ROS 2 Python package
    ros2_bridge/
      __init__.py
      protocol.py                # Pure Python — zero ROS deps, serialisation only
      bridge_node.py             # rclpy node — imports protocol.py
    tests/
      test_protocol.py           # Unit tests for protocol round-trips (no ROS needed)
      test_node_import.py        # Smoke test: node constructs without error
    package.xml
    setup.py
    setup.cfg

  DEPENDENCY_VERSIONS.md         # Confirmed pinned versions — created before any code
  README.md
```

`local.properties` is in `.gitignore`. Each developer creates it with:
```
sdk.dir=/home/<user>/Android/Sdk
ndk.dir=/home/<user>/Android/Sdk/ndk/25.2.9519653
```

### Key layering rule
`protocol.py` must have **zero ROS 2 dependencies** — only stdlib (`struct`, `enum`).
This allows unit testing without a ROS 2 environment and reuse in non-ROS scripts.
`bridge_node.py` imports `protocol.py` and adds all rclpy logic on top.

---

## Vendoring uWebSockets

Use the release tag confirmed in `DEPENDENCY_VERSIONS.md`.

**How to vendor** (do this once at project setup):

uSockets is not separately versioned — it ships as a git submodule of uWebSockets.
Clone with `--recurse-submodules` to get both at the correct matched versions:

```bash
git clone --recurse-submodules --depth=1 \
  --branch <tag from DEPENDENCY_VERSIONS.md> \
  https://github.com/uNetworking/uWebSockets.git \
  android_app/app/src/main/cpp/uWebSockets
```

This produces:
```
android_app/app/src/main/cpp/uWebSockets/src/        ← uWebSockets headers
android_app/app/src/main/cpp/uWebSockets/uSockets/src/ ← uSockets headers
```

In `CMakeLists.txt`, include both paths and add a version comment:
```cmake
# uWebSockets <tag> (uSockets bundled submodule) — cloned from GitHub
include_directories(
    uWebSockets/src
    uWebSockets/uSockets/src
)
```

Do not pull from `main`. Do not use a separate uSockets clone or tarball.
Commit the vendored source so builds are hermetic and do not require network access.

---

## Communication Protocol

### Transport
- **WebSocket** over ADB port forward: `adb forward tcp:9090 tcp:9090`
- Python: `websockets>=11.0` library (async)
- Android: uWebSockets (C++, header-only), server mode, port 9090

### Message Format
Binary WebSocket frames, big-endian:

```
[1 byte: MSG_TYPE] [4 bytes: uint32 payload_length] [N bytes: payload]
```

Message types:

| Name | ID | Direction | Payload |
|---|---|---|---|
| CONTROLLER_STATE | 0x01 | App → Python | See below |
| HAPTIC_CMD | 0x02 | Python → App | [1B hand] [4B float32 intensity] [4B float32 duration_ms] |
| CAMERA_FRAME | 0x03 | Python → App | JPEG bytes |
| HAND_STATE | 0x04 | App → Python | See below |

**CONTROLLER_STATE payload** — two controllers concatenated, left then right.
Each controller block is identical in layout:

```
Per-controller block (32 bytes):
  [1B: button_bitmask_low]
  [1B: button_bitmask_high]
  [1B: trigger_float * 255]
  [1B: grip_float * 255]
  [28B: 7× float32 BE pose [x, y, z, qx, qy, qz, qw]]

Full message: left_block (32B) || right_block (32B) = 64 bytes total
```

**HAND_STATE payload**:
```
[1B: hand (0=left, 1=right)]
[26 joints × 28B: 7× float32 BE pose [x, y, z, qx, qy, qz, qw] each]
Total: 729 bytes
```

### Float byte order note
Use `memcpy` for BE float conversion in C++ — not `reinterpret_cast` (UB in C++):
```cpp
uint32_t tmp; memcpy(&tmp, buf, 4); tmp = ntohl(tmp); float val; memcpy(&val, &tmp, 4);
```

---

## Android App Architecture

### OpenXR setup (bridge_app.cpp)
- Standard OpenXR instance + session setup for Android
- Extensions to request (check availability, fail gracefully if absent):
  - `XR_KHR_android_create_instance` — required
  - `XR_EXT_hand_tracking` — required for feature 4
  - `XR_KHR_composition_layer_depth` — optional
  - `XR_FB_passthrough` — optional, for future use
- Main loop: `xrWaitFrame` → `xrBeginFrame` → poll input → send state → recv commands → render → `xrEndFrame`
- OpenXR session lifecycle handles focus/visibility correctly — no proximity sensor workaround needed

### WebSocket disconnection handling (Android side)
When the Python bridge client disconnects (or has not yet connected):
- Stop sending CONTROLLER_STATE and HAND_STATE frames (no queuing)
- Log the disconnection at INFO level to Android logcat
- Display a small in-headset text overlay: "Bridge disconnected — waiting for connection"
  rendered as a `XrCompositionLayerQuad` in head-locked space
- The uWebSockets server keeps listening; reconnect is automatic when the Python
  side re-connects
- Do not crash or enter an error state — the app stays running

### Rendering (camera_renderer.cpp)
- Use `XrCompositionLayerQuad` — correct OpenXR primitive for a world-fixed flat panel
- Position: 2m forward, eye height, world-anchored (not head-locked)
- `CameraPanelTransform` matrix isolated in one place for easy swap to yaw-locked later
- Texture: `GL_TEXTURE_2D`, updated from JPEG via `stb_image.h` when a new frame arrives
- Only add the quad layer to frame submission when at least one camera frame has been received

### WebSocket server (ws_server.cpp)
- uWebSockets server mode, port 9090
- Dedicated `std::thread`
- Shared state with render thread:
  - `std::atomic<float>` for haptic intensities (left/right) — no mutex needed
  - `std::mutex` + staging buffer for camera frames
  - `std::atomic<bool>` dirty flag for outgoing controller/hand state

### build.gradle (app level)
```groovy
android {
    defaultConfig {
        minSdk 29
        targetSdk 33
        ndk { abiFilters 'arm64-v8a' }
        externalNativeBuild {
            cmake {
                cppFlags "-std=c++17 -fexceptions"
                arguments "-DANDROID_STL=c++_shared"
            }
        }
    }
    externalNativeBuild {
        cmake { path "src/main/cpp/CMakeLists.txt" }
    }
}
dependencies {
    // Version from DEPENDENCY_VERSIONS.md — do not use + or latest
    implementation 'org.khronos.openxr:openxr_loader_for_android:<version>'
}
```

Replace `<version>` with the value from `DEPENDENCY_VERSIONS.md`.

### AndroidManifest.xml permissions
```xml
<uses-permission android:name="android.permission.INTERNET" />
<uses-feature android:name="android.hardware.vr.headtracking" android:required="true" />
<uses-permission android:name="com.oculus.permission.HAND_TRACKING" />
<meta-data android:name="com.samsung.android.vr.application.mode" android:value="vr_only"/>
```

---

## ROS 2 Python Node (bridge_node.py)

### Parameters (all via declare_parameter — Humble compatible)
- `port` (int, default 9090)
- `adb_serial` (string, default `''`)
- `image_topic` (string, default `/camera/image_raw`)
- `jpeg_quality` (int, default 80)
- `max_width` (int, default 640)
- `publish_hands` (bool, default `true`)

### Publishers
- `/oculus/left/pose`   — `geometry_msgs/msg/PoseStamped`  (frame_id: `"quest_world"`)
- `/oculus/right/pose`  — `geometry_msgs/msg/PoseStamped`  (frame_id: `"quest_world"`)
- `/oculus/buttons`     — `sensor_msgs/msg/Joy`
- `/oculus/left/hand`   — `sensor_msgs/msg/JointState`     (26 joints — see joint name list below)
- `/oculus/right/hand`  — `sensor_msgs/msg/JointState`     (26 joints)

### Subscribers
- `/oculus/haptic/left`    — `std_msgs/msg/Float32` (intensity 0.0–1.0)
- `/oculus/haptic/right`   — `std_msgs/msg/Float32`
- `{image_topic}`          — `sensor_msgs/msg/Image`

### Hand joint name list
These names are canonical and match `XrHandJointEXT` ordering.
Use them verbatim in `JointState.name[]` for both left and right hands.

```python
HAND_JOINT_NAMES = [
    "palm",
    "wrist",
    "thumb_metacarpal",
    "thumb_proximal",
    "thumb_distal",
    "thumb_tip",
    "index_metacarpal",
    "index_proximal",
    "index_intermediate",
    "index_distal",
    "index_tip",
    "middle_metacarpal",
    "middle_proximal",
    "middle_intermediate",
    "middle_distal",
    "middle_tip",
    "ring_metacarpal",
    "ring_proximal",
    "ring_intermediate",
    "ring_distal",
    "ring_tip",
    "little_metacarpal",
    "little_proximal",
    "little_intermediate",
    "little_distal",
    "little_tip",
]
```

### Architecture
- `asyncio.new_event_loop()` — explicit, not relying on default loop
- `websockets>=11.0` async client connecting to `ws://127.0.0.1:9090`
- Separate asyncio tasks: recv loop, send queue
- rclpy node spun in a `threading.Thread` alongside asyncio event loop
- Auto-reconnect with exponential backoff (1s, 2s, 4s, max 30s)
- Image callback: normalize encoding (rgb8/bgr8/mono8), downscale if > max_width,
  `cv2.imencode('.jpg')`, enqueue for send

### protocol.py (zero ROS deps — stdlib only)
```python
import struct
from enum import IntEnum

class MsgType(IntEnum):
    CONTROLLER_STATE = 0x01
    HAPTIC_CMD       = 0x02
    CAMERA_FRAME     = 0x03
    HAND_STATE       = 0x04

HEADER_SIZE = 5  # 1B type + 4B uint32 length

def pack_message(msg_type: MsgType, payload: bytes) -> bytes:
    return struct.pack('!BI', int(msg_type), len(payload)) + payload

def unpack_header(data: bytes) -> tuple:  # (msg_type, payload_length)
    return struct.unpack('!BI', data[:HEADER_SIZE])
```

---

## Tests (tests/)

### test_protocol.py — no ROS needed, run with plain pytest
- Round-trip: `pack_message` → `unpack_header` recovers correct type and length
- HAPTIC_CMD payload: encode intensity 0.0, 0.5, 1.0 → decode → values match within 1e-6
- CONTROLLER_STATE: encode a known pose for both controllers → decode →
  all values match within 1e-6; verify total payload is exactly 64 bytes
- HAND_STATE: encode 26 joint poses → decode → all values match within 1e-6;
  verify total payload is exactly 729 bytes

### test_node_import.py — requires ROS 2 sourced
- Import `bridge_node` without error
- Instantiate node with default params
- Verify publishers and subscribers are created

### Running tests
```bash
# Protocol tests — no ROS needed
cd ros2_bridge
pytest tests/test_protocol.py -v

# Node smoke test — ROS 2 must be sourced
source /opt/ros/humble/setup.bash   # or jazzy / kilted
pytest tests/test_node_import.py -v
```

---

## Build & Run

### Android
```bash
cd android_app
./gradlew assembleDebug
adb uninstall com.quest.bridge 2>/dev/null || true
adb install app/build/outputs/apk/debug/app-debug.apk
adb forward tcp:9090 tcp:9090
adb shell am start -n com.quest.bridge/.MainActivity
```

### ROS 2 node
```bash
cd ros2_bridge
pip install -e . --break-system-packages
ros2 run ros2_bridge bridge_node
```

### Smoke tests
```bash
# Poses
ros2 topic echo /oculus/right/pose

# Haptics — feel vibration in right controller
ros2 topic pub /oculus/haptic/right std_msgs/msg/Float32 "data: 0.8" --once
ros2 topic pub /oculus/haptic/right std_msgs/msg/Float32 "data: 0.0" --once

# Camera panel
ros2 run image_publisher image_publisher_node /path/to/test.jpg \
  --ros-args -r image:=/camera/image_raw

# Hand tracking
ros2 topic echo /oculus/right/hand
```

---

## README "Tested on" table (fill in over time)

| | Quest 2 | Quest 3 | Quest 3S | Quest Pro |
|---|---|---|---|---|
| **Humble** | ⬜ | ⬜ | ⬜ | ⬜ |
| **Jazzy** | ⬜ | ⬜ | ⬜ | ⬜ |
| **Kilted** | ⬜ | ⬜ | ⬜ | ⬜ |

---

## Key Differences from oculus_reader

| | oculus_reader | quest_ros2_bridge |
|---|---|---|
| SDK | Oculus Mobile SDK 1.50.0 (2021) | OpenXR 1.0 (current) |
| Headset support | Quest 1/2 only | Quest 2/3/3S/Pro |
| ROS support | ROS 1 only | ROS 2 Humble / Jazzy / Kilted |
| Communication | logcat (read-only) | WebSocket (bidirectional) |
| Build system | Gradle 7 + VrApp.gradle + SDK zip | Gradle 8, standard AGP, Maven |
| Haptics | Not supported | `xrApplyHapticFeedback` |
| Hand tracking | Not supported | `XR_EXT_hand_tracking` |
| Camera in headset | Not supported | `XrCompositionLayerQuad` |
| Proximity sensor | Kills app when not worn | OpenXR session lifecycle |
| ADB dependency | Required at runtime | Port forward only |
| Tests | None | pytest, no headset needed for protocol tests |

---

## Implementation Order (5 commits — see starter prompt for gate conditions)

1. **1a: Python protocol layer** — `protocol.py` + `test_protocol.py` passing
2. **1b: Android scaffold** — OpenXR session + blank render loop + uWebSockets server stub
3. **Commit 2: Controller I/O** — read poses/buttons → WS → Python publishes to ROS 2
4. **Commit 3: Haptics** — ROS 2 topic → WS → `xrApplyHapticFeedback`
5. **Commit 4: Camera panel** — ROS 2 Image → JPEG → WS → `XrCompositionLayerQuad`
6. **Commit 5: Hand tracking** — `XR_EXT_hand_tracking` → WS → ROS 2 `JointState`
