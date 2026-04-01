---
title: Wire Protocol Reference
tags: [protocol, wire-format, binary, websocket]
---

# Wire Protocol Reference

All communication between the Quest app and the Python bridge uses binary WebSocket frames over ADB port forwarding (`adb forward tcp:9090 tcp:9090`).

## Transport

- **Quest side:** uWebSockets C++ server on port 9090
- **Python side:** `websockets>=11.0` async client connecting to `ws://127.0.0.1:9090`
- **Connection:** Single WebSocket connection, full-duplex binary frames

## Message Envelope

Every message starts with a 5-byte header:

```
[1 byte: MSG_TYPE] [4 bytes: uint32 payload_length (big-endian)] [N bytes: payload]
```

| Field | Size | Encoding |
|---|---|---|
| MSG_TYPE | 1 byte | unsigned, see table below |
| payload_length | 4 bytes | uint32, big-endian (network byte order) |
| payload | variable | message-type-specific |

## Message Types

| Name | ID | Direction | Payload Size |
|---|---|---|---|
| CONTROLLER_STATE | `0x01` | Quest → Python | 64 bytes |
| HAPTIC_CMD | `0x02` | Python → Quest | 9 bytes |
| CAMERA_FRAME | `0x03` | Python → Quest | variable (JPEG) |
| HAND_STATE | `0x04` | Quest → Python | 729 bytes |

## Pose Format

All poses use a 28-byte position + quaternion representation:

```
[4B float32: x] [4B float32: y] [4B float32: z]
[4B float32: qx] [4B float32: qy] [4B float32: qz] [4B float32: qw]
```

All floats are **big-endian** (network byte order). In C++, conversion uses safe `memcpy` + `htonl`/`ntohl` to avoid undefined behavior — see `protocol.h` `float_to_be()` / `be_to_float()`.

### Design decision: pos+quat over 4×4 matrix

The original spec called for a 4×4 float32 homogeneous matrix (64 bytes per pose). During implementation, this was changed to position + quaternion (28 bytes) because:
- 56% smaller per pose — significant for hand tracking (26 joints)
- ROS 2 `PoseStamped` and `JointState` use pos+quat natively
- No information loss for rigid body transforms (no scale/shear needed)

## CONTROLLER_STATE (0x01)

Two 32-byte controller blocks concatenated: left then right.

```
Per-controller block (32 bytes):
  [1B: button_bitmask_low]
  [1B: button_bitmask_high]
  [1B: trigger (float * 255, quantized to uint8)]
  [1B: grip (float * 255, quantized to uint8)]
  [28B: pose — x, y, z, qx, qy, qz, qw as 7× float32 BE]

Full payload: left_block (32B) || right_block (32B) = 64 bytes
```

### Button Bitmask (buttons_low)

| Bit | Button |
|---|---|
| 0 | X (left) / A (right) |
| 1 | Y (left) / B (right) |
| 2 | Menu |
| 3 | Thumbstick click |
| 4 | Trigger touch |
| 5 | Thumb touch |
| 6-7 | Reserved |

`buttons_high` is reserved for future use.

### Trigger/Grip Quantization

Analog values (0.0–1.0) are quantized to uint8 (0–255) on the Quest side and divided by 255.0 on the Python side. Maximum quantization error is ±1/255 ≈ 0.004.

## HAPTIC_CMD (0x02)

```
[1B: hand (0=left, 1=right)]
[4B: float32 BE intensity (0.0–1.0)]
[4B: float32 BE duration_ms]

Total: 9 bytes
```

Setting intensity to 0.0 calls `xrStopHapticFeedback`. Duration is converted from milliseconds to nanoseconds on the Quest side for `XrHapticVibration.duration`.

## CAMERA_FRAME (0x03)

```
[N bytes: raw JPEG data]

Total: variable
```

The Python bridge encodes ROS 2 `sensor_msgs/Image` to JPEG using OpenCV (`cv2.imencode`). Encoding normalization: rgb8, bgr8, and mono8 are supported. Images wider than `max_width` (default 640) are downscaled before encoding. See [[camera_panel]] for the full pipeline.

## HAND_STATE (0x04)

```
[1B: hand (0=left, 1=right)]
[26 joints × 28B: pose — x, y, z, qx, qy, qz, qw as 7× float32 BE each]

Total: 1 + (26 × 28) = 729 bytes
```

Joint ordering follows `XrHandJointEXT` from the OpenXR hand tracking extension. See [[ros2_setup]] for the canonical joint name list.

## Size Constants Summary

| Constant | Python | C++ | Value |
|---|---|---|---|
| Header | `HEADER_SIZE` | `HEADER_SIZE` | 5 |
| Pose | `POSE_SIZE` | `POSE_SIZE` | 28 |
| Controller block | `CONTROLLER_BLOCK_SIZE` | `CONTROLLER_BLOCK_SIZE` | 32 |
| Controller state | `CONTROLLER_STATE_SIZE` | `CONTROLLER_STATE_SIZE` | 64 |
| Hand joint count | `HAND_JOINT_COUNT` | `HAND_JOINT_COUNT` | 26 |
| Hand state | `HAND_STATE_SIZE` | `HAND_STATE_SIZE` | 729 |
| Haptic cmd | — | `HAPTIC_CMD_SIZE` | 9 |

## Related Docs

- [[architecture]] — system overview and threading model
- [[camera_panel]] — CAMERA_FRAME pipeline details
- [[ros2_setup]] — ROS 2 topic and message type reference
