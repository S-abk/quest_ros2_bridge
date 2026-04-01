---
title: ROS 2 Setup & Topic Reference
tags: [ros2, setup, topics, parameters, colcon]
---

# ROS 2 Setup & Topic Reference

## Supported Distros

| Distro | Python | Status |
|---|---|---|
| Humble | 3.10 | LTS baseline — only Humble-compatible APIs used |
| Jazzy | 3.12 | Current LTS |
| Kilted | 3.12 | Dev environment, tested on Quest 2 |

## Workspace Setup

`pip install -e .` does **not** register the package in the ROS 2 ament index. Use colcon:

```bash
mkdir -p ~/ros2_ws/src
ln -s $(pwd)/ros2_bridge ~/ros2_ws/src/ros2_bridge
cd ~/ros2_ws
colcon build --symlink-install --packages-select ros2_bridge
source install/setup.bash
pip install websockets opencv-python --break-system-packages
```

> `source install/setup.bash` must be re-run in every new terminal.

## Running

```bash
ros2 run ros2_bridge bridge_node
```

With parameters:
```bash
ros2 run ros2_bridge bridge_node --ros-args \
  -p port:=9090 \
  -p image_topic:=/camera/image_raw \
  -p jpeg_quality:=80 \
  -p max_width:=640 \
  -p publish_hands:=true
```

## Parameters

| Parameter | Type | Default | Description |
|---|---|---|---|
| `port` | int | `9090` | WebSocket server port on Quest |
| `adb_serial` | string | `""` | ADB device serial (reserved, unused) |
| `image_topic` | string | `/camera/image_raw` | ROS 2 image topic to relay to headset |
| `jpeg_quality` | int | `80` | JPEG compression quality (1–100) |
| `max_width` | int | `640` | Max image width before downscaling |
| `publish_hands` | bool | `true` | Enable/disable hand tracking publishing |

## Published Topics

| Topic | Message Type | Frame ID | Description |
|---|---|---|---|
| `/oculus/left/pose` | `geometry_msgs/PoseStamped` | `quest_world` | Left controller 6DoF pose |
| `/oculus/right/pose` | `geometry_msgs/PoseStamped` | `quest_world` | Right controller 6DoF pose |
| `/oculus/buttons` | `sensor_msgs/Joy` | — | Buttons + analog triggers/grips |
| `/oculus/left/hand` | `sensor_msgs/JointState` | `quest_world` | Left hand 26-joint tracking |
| `/oculus/right/hand` | `sensor_msgs/JointState` | `quest_world` | Right hand 26-joint tracking |

### Joy Message Layout (`/oculus/buttons`)

**axes** (4 floats): `[left_trigger, left_grip, right_trigger, right_grip]`

**buttons** (32 ints): Bitmask expansion — 8 bits from `buttons_low` + 8 bits from `buttons_high` for each controller (left first, then right). See [[wire_protocol]] for bitmask definitions.

### JointState Layout (`/oculus/{left,right}/hand`)

**name** (26 strings): Canonical joint names matching `XrHandJointEXT` ordering:

```
palm, wrist,
thumb_metacarpal, thumb_proximal, thumb_distal, thumb_tip,
index_metacarpal, index_proximal, index_intermediate, index_distal, index_tip,
middle_metacarpal, middle_proximal, middle_intermediate, middle_distal, middle_tip,
ring_metacarpal, ring_proximal, ring_intermediate, ring_distal, ring_tip,
little_metacarpal, little_proximal, little_intermediate, little_distal, little_tip
```

**position** (78 floats): `[x, y, z]` per joint — 26 × 3 values
**velocity** (104 floats): `[qx, qy, qz, qw]` per joint — 26 × 4 values

> This is a non-standard use of JointState fields to carry full 6DoF per joint. Standard JointState has 1 value per joint per field.

## Subscribed Topics

| Topic | Message Type | Description |
|---|---|---|
| `/oculus/haptic/left` | `std_msgs/Float32` | Left controller vibration intensity (0.0–1.0) |
| `/oculus/haptic/right` | `std_msgs/Float32` | Right controller vibration intensity (0.0–1.0) |
| `{image_topic}` | `sensor_msgs/Image` | Camera feed to display in headset |

### Haptic Commands

Publishing a `Float32` with `data: 0.8` triggers a 100ms vibration at 80% intensity. Publishing `data: 0.0` stops vibration. Duration is hardcoded to 100ms in the bridge node — each new publish resets the timer.

```bash
ros2 topic pub /oculus/haptic/right std_msgs/msg/Float32 "data: 0.8" --once
ros2 topic pub /oculus/haptic/right std_msgs/msg/Float32 "data: 0.0" --once
```

### Image Encoding Support

The bridge node accepts `rgb8`, `bgr8`, and `mono8` encodings. Other encodings are logged as warnings and dropped. Images wider than `max_width` are downscaled with `cv2.INTER_AREA` before JPEG compression. See [[camera_panel]] for the full pipeline.

## Auto-Reconnect

The bridge node uses exponential backoff when the WebSocket connection drops:

| Attempt | Delay |
|---|---|
| 1 | 1s |
| 2 | 2s |
| 3 | 4s |
| 4 | 8s |
| 5 | 16s |
| 6+ | 30s (max) |

Reconnects automatically when the Quest app restarts or ADB reconnects.

## Related Docs

- [[architecture]] — system overview
- [[wire_protocol]] — binary protocol details
- [[camera_panel]] — image pipeline and threading
- [[hardware_testing]] — diagnostic tools
