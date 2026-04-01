"""Wire protocol for quest_ros2_bridge — zero ROS 2 dependencies (stdlib only)."""

import struct
from enum import IntEnum


class MsgType(IntEnum):
    CONTROLLER_STATE = 0x01
    HAPTIC_CMD = 0x02
    CAMERA_FRAME = 0x03
    HAND_STATE = 0x04
    SCENE_CONFIG = 0x05


HEADER_SIZE = 5  # 1B type + 4B uint32 length
POSE_SIZE = 28  # 7 × float32: [x, y, z, qx, qy, qz, qw]
CONTROLLER_BLOCK_SIZE = 32  # 4B buttons/triggers + 28B pose
CONTROLLER_STATE_SIZE = 64  # 2 × 32B (left + right)
HAND_JOINT_COUNT = 26
HAND_STATE_SIZE = 729  # 1B hand + 26 × 28B poses
SCENE_CONFIG_SIZE = 1  # 1B show_grid (0=off, 1=on)

_POSE_FMT = "!7f"  # 7 big-endian float32
_HEADER_FMT = "!BI"  # 1B msg_type + 4B uint32 length


def pack_message(msg_type: MsgType, payload: bytes) -> bytes:
    """Wrap a payload with the 5-byte message header."""
    return struct.pack(_HEADER_FMT, int(msg_type), len(payload)) + payload


def unpack_header(data: bytes) -> tuple:
    """Return (msg_type, payload_length) from the first 5 bytes."""
    msg_type, length = struct.unpack(_HEADER_FMT, data[:HEADER_SIZE])
    return MsgType(msg_type), length


def _pack_controller_block(buttons_low: int, buttons_high: int,
                           trigger: float, grip: float,
                           pose: tuple) -> bytes:
    """Pack a single controller block (32 bytes).

    pose: (x, y, z, qx, qy, qz, qw)
    trigger/grip: 0.0–1.0, quantised to uint8.
    """
    trigger_u8 = max(0, min(255, int(trigger * 255)))
    grip_u8 = max(0, min(255, int(grip * 255)))
    header = struct.pack("!BBBB", buttons_low & 0xFF, buttons_high & 0xFF,
                         trigger_u8, grip_u8)
    return header + struct.pack(_POSE_FMT, *pose)


def _unpack_controller_block(data: bytes) -> dict:
    """Unpack a single 32-byte controller block."""
    buttons_low, buttons_high, trigger_u8, grip_u8 = struct.unpack("!BBBB", data[:4])
    pose = struct.unpack(_POSE_FMT, data[4:32])
    return {
        "buttons_low": buttons_low,
        "buttons_high": buttons_high,
        "trigger": trigger_u8 / 255.0,
        "grip": grip_u8 / 255.0,
        "pose": pose,
    }


def pack_controller_state(left: dict, right: dict) -> bytes:
    """Pack CONTROLLER_STATE payload (64 bytes).

    Each dict: {buttons_low, buttons_high, trigger, grip, pose}.
    """
    return (
        _pack_controller_block(left["buttons_low"], left["buttons_high"],
                               left["trigger"], left["grip"], left["pose"])
        + _pack_controller_block(right["buttons_low"], right["buttons_high"],
                                 right["trigger"], right["grip"], right["pose"])
    )


def unpack_controller_state(payload: bytes) -> tuple:
    """Unpack 64-byte CONTROLLER_STATE payload → (left_dict, right_dict)."""
    left = _unpack_controller_block(payload[:CONTROLLER_BLOCK_SIZE])
    right = _unpack_controller_block(payload[CONTROLLER_BLOCK_SIZE:CONTROLLER_STATE_SIZE])
    return left, right


def pack_haptic_cmd(hand: int, intensity: float, duration_ms: float) -> bytes:
    """Pack HAPTIC_CMD payload (9 bytes): [1B hand] [4B float32 intensity] [4B float32 duration_ms]."""
    return struct.pack("!Bff", hand, intensity, duration_ms)


def unpack_haptic_cmd(payload: bytes) -> tuple:
    """Unpack 9-byte HAPTIC_CMD payload → (hand, intensity, duration_ms)."""
    return struct.unpack("!Bff", payload[:9])


def pack_hand_state(hand: int, joints: list) -> bytes:
    """Pack HAND_STATE payload (729 bytes).

    hand: 0=left, 1=right.
    joints: list of 26 tuples, each (x, y, z, qx, qy, qz, qw).
    """
    buf = struct.pack("!B", hand)
    for pose in joints:
        buf += struct.pack(_POSE_FMT, *pose)
    return buf


def unpack_hand_state(payload: bytes) -> tuple:
    """Unpack 729-byte HAND_STATE payload → (hand, list_of_26_pose_tuples)."""
    hand = struct.unpack("!B", payload[:1])[0]
    joints = []
    for i in range(HAND_JOINT_COUNT):
        offset = 1 + i * POSE_SIZE
        pose = struct.unpack(_POSE_FMT, payload[offset:offset + POSE_SIZE])
        joints.append(pose)
    return hand, joints


def pack_scene_config(show_grid: bool) -> bytes:
    """Pack SCENE_CONFIG payload (1 byte): [1B show_grid]."""
    return struct.pack("!B", 1 if show_grid else 0)


def unpack_scene_config(payload: bytes) -> bool:
    """Unpack 1-byte SCENE_CONFIG payload → show_grid bool."""
    return struct.unpack("!B", payload[:1])[0] != 0
