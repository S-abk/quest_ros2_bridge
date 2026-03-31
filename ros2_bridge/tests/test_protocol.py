"""Tests for the wire protocol — no ROS 2 needed, plain pytest."""

import math
import struct
import sys
import os

# Ensure the package is importable when running from the tests/ directory.
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from ros2_bridge.protocol import (
    MsgType,
    HEADER_SIZE,
    POSE_SIZE,
    CONTROLLER_BLOCK_SIZE,
    CONTROLLER_STATE_SIZE,
    HAND_STATE_SIZE,
    HAND_JOINT_COUNT,
    pack_message,
    unpack_header,
    pack_controller_state,
    unpack_controller_state,
    pack_haptic_cmd,
    unpack_haptic_cmd,
    pack_hand_state,
    unpack_hand_state,
)


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _make_pose(seed: float = 1.0):
    """Return a 7-tuple pose: (x, y, z, qx, qy, qz, qw)."""
    return (seed, seed + 0.1, seed + 0.2,
            seed + 0.3, seed + 0.4, seed + 0.5, seed + 0.6)


def _make_controller(buttons_low=0xAB, buttons_high=0xCD,
                     trigger=0.75, grip=0.5, pose=None):
    return {
        "buttons_low": buttons_low,
        "buttons_high": buttons_high,
        "trigger": trigger,
        "grip": grip,
        "pose": pose or _make_pose(),
    }


# ---------------------------------------------------------------------------
# Tests: header round-trip
# ---------------------------------------------------------------------------

class TestHeaderRoundTrip:
    def test_pack_unpack_recovers_type_and_length(self):
        payload = b"\x00" * 64
        msg = pack_message(MsgType.CONTROLLER_STATE, payload)
        msg_type, length = unpack_header(msg)
        assert msg_type == MsgType.CONTROLLER_STATE
        assert length == 64

    def test_all_msg_types(self):
        for mt in MsgType:
            payload = b"\xff" * 10
            msg = pack_message(mt, payload)
            recovered_type, recovered_len = unpack_header(msg)
            assert recovered_type == mt
            assert recovered_len == 10

    def test_header_size_is_five(self):
        assert HEADER_SIZE == 5


# ---------------------------------------------------------------------------
# Tests: HAPTIC_CMD
# ---------------------------------------------------------------------------

class TestHapticCmd:
    def test_round_trip_intensity_zero(self):
        payload = pack_haptic_cmd(0, 0.0, 100.0)
        hand, intensity, duration = unpack_haptic_cmd(payload)
        assert hand == 0
        assert abs(intensity - 0.0) < 1e-6
        assert abs(duration - 100.0) < 1e-6

    def test_round_trip_intensity_half(self):
        payload = pack_haptic_cmd(1, 0.5, 200.0)
        hand, intensity, duration = unpack_haptic_cmd(payload)
        assert hand == 1
        assert abs(intensity - 0.5) < 1e-6
        assert abs(duration - 200.0) < 1e-6

    def test_round_trip_intensity_full(self):
        payload = pack_haptic_cmd(0, 1.0, 500.0)
        hand, intensity, duration = unpack_haptic_cmd(payload)
        assert hand == 0
        assert abs(intensity - 1.0) < 1e-6
        assert abs(duration - 500.0) < 1e-6

    def test_payload_size(self):
        payload = pack_haptic_cmd(0, 0.5, 100.0)
        assert len(payload) == 9


# ---------------------------------------------------------------------------
# Tests: CONTROLLER_STATE
# ---------------------------------------------------------------------------

class TestControllerState:
    def test_payload_is_exactly_64_bytes(self):
        left = _make_controller(pose=_make_pose(1.0))
        right = _make_controller(pose=_make_pose(2.0))
        payload = pack_controller_state(left, right)
        assert len(payload) == CONTROLLER_STATE_SIZE == 64

    def test_round_trip_poses(self):
        left_pose = (0.1, 0.2, 0.3, 0.0, 0.0, 0.0, 1.0)
        right_pose = (1.0, 2.0, 3.0, 0.0, 0.707107, 0.0, 0.707107)
        left = _make_controller(buttons_low=0x01, buttons_high=0x02,
                                trigger=0.0, grip=1.0, pose=left_pose)
        right = _make_controller(buttons_low=0xFF, buttons_high=0x00,
                                 trigger=1.0, grip=0.0, pose=right_pose)
        payload = pack_controller_state(left, right)
        left_out, right_out = unpack_controller_state(payload)

        for i in range(7):
            assert abs(left_out["pose"][i] - left_pose[i]) < 1e-6
            assert abs(right_out["pose"][i] - right_pose[i]) < 1e-6

    def test_round_trip_buttons(self):
        left = _make_controller(buttons_low=0xAB, buttons_high=0xCD)
        right = _make_controller(buttons_low=0x12, buttons_high=0x34)
        payload = pack_controller_state(left, right)
        left_out, right_out = unpack_controller_state(payload)
        assert left_out["buttons_low"] == 0xAB
        assert left_out["buttons_high"] == 0xCD
        assert right_out["buttons_low"] == 0x12
        assert right_out["buttons_high"] == 0x34

    def test_trigger_grip_quantisation(self):
        """Trigger/grip are quantised to uint8, so expect ±1/255 error."""
        left = _make_controller(trigger=0.5, grip=0.75)
        right = _make_controller(trigger=0.25, grip=1.0)
        payload = pack_controller_state(left, right)
        left_out, right_out = unpack_controller_state(payload)
        assert abs(left_out["trigger"] - 0.5) < 1.0 / 255 + 1e-9
        assert abs(left_out["grip"] - 0.75) < 1.0 / 255 + 1e-9
        assert abs(right_out["trigger"] - 0.25) < 1.0 / 255 + 1e-9
        assert abs(right_out["grip"] - 1.0) < 1e-6

    def test_block_size_constants(self):
        assert CONTROLLER_BLOCK_SIZE == 32
        assert CONTROLLER_STATE_SIZE == 64
        assert POSE_SIZE == 28


# ---------------------------------------------------------------------------
# Tests: HAND_STATE
# ---------------------------------------------------------------------------

class TestHandState:
    def test_payload_is_exactly_729_bytes(self):
        joints = [_make_pose(float(i)) for i in range(HAND_JOINT_COUNT)]
        payload = pack_hand_state(0, joints)
        assert len(payload) == HAND_STATE_SIZE == 729

    def test_round_trip_all_joints(self):
        joints = [_make_pose(float(i) * 0.1) for i in range(HAND_JOINT_COUNT)]
        payload = pack_hand_state(1, joints)
        hand_out, joints_out = unpack_hand_state(payload)
        assert hand_out == 1
        assert len(joints_out) == HAND_JOINT_COUNT
        for j in range(HAND_JOINT_COUNT):
            for k in range(7):
                assert abs(joints_out[j][k] - joints[j][k]) < 1e-6

    def test_hand_identifier(self):
        joints = [_make_pose(0.0)] * HAND_JOINT_COUNT
        for hand_id in (0, 1):
            payload = pack_hand_state(hand_id, joints)
            hand_out, _ = unpack_hand_state(payload)
            assert hand_out == hand_id

    def test_size_constants(self):
        assert HAND_STATE_SIZE == 1 + HAND_JOINT_COUNT * POSE_SIZE
        assert HAND_JOINT_COUNT == 26
