"""ROS 2 bridge node — connects to Quest headset via WebSocket."""

import asyncio
import math
import struct
import threading

import rclpy
from rclpy.node import Node
import numpy as np
import cv2

from geometry_msgs.msg import PoseStamped
from sensor_msgs.msg import Joy, Image, JointState

from .protocol import (
    MsgType,
    HEADER_SIZE,
    CONTROLLER_STATE_SIZE,
    CONTROLLER_BLOCK_SIZE,
    HAND_STATE_SIZE,
    POSE_SIZE,
    pack_message,
    unpack_header,
    unpack_controller_state,
    unpack_hand_state,
)

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

import websockets


class BridgeNode(Node):
    def __init__(self):
        super().__init__("quest_bridge")

        # Parameters
        self.declare_parameter("port", 9090)
        self.declare_parameter("adb_serial", "")
        self.declare_parameter("image_topic", "/camera/image_raw")
        self.declare_parameter("jpeg_quality", 80)
        self.declare_parameter("max_width", 640)
        self.declare_parameter("publish_hands", True)

        self._port = self.get_parameter("port").value

        self._publish_hands = self.get_parameter("publish_hands").value

        # Publishers
        self._pub_left_pose = self.create_publisher(PoseStamped, "/oculus/left/pose", 10)
        self._pub_right_pose = self.create_publisher(PoseStamped, "/oculus/right/pose", 10)
        self._pub_buttons = self.create_publisher(Joy, "/oculus/buttons", 10)
        self._pub_left_hand = self.create_publisher(JointState, "/oculus/left/hand", 10)
        self._pub_right_hand = self.create_publisher(JointState, "/oculus/right/hand", 10)

        # Subscribers for haptics
        from std_msgs.msg import Float32
        self._sub_haptic_left = self.create_subscription(
            Float32, "/oculus/haptic/left", self._haptic_left_cb, 10)
        self._sub_haptic_right = self.create_subscription(
            Float32, "/oculus/haptic/right", self._haptic_right_cb, 10)

        # Image subscriber for camera panel
        image_topic = self.get_parameter("image_topic").value
        self._jpeg_quality = self.get_parameter("jpeg_quality").value
        self._max_width = self.get_parameter("max_width").value
        self._sub_image = self.create_subscription(
            Image, image_topic, self._image_cb, 10)

        # Send queue for haptic/camera commands
        self._send_queue = asyncio.Queue()

        self.get_logger().info(f"BridgeNode initialized, will connect to ws://127.0.0.1:{self._port}")

    def _haptic_left_cb(self, msg):
        """Queue a haptic command for the left controller."""
        payload = struct.pack("!Bff", 0, msg.data, 100.0)
        frame = pack_message(MsgType.HAPTIC_CMD, payload)
        try:
            self._loop.call_soon_threadsafe(self._send_queue.put_nowait, frame)
        except asyncio.QueueFull:
            pass

    def _image_cb(self, msg):
        """Convert ROS Image to JPEG and queue as CAMERA_FRAME."""
        try:
            # Decode image data
            h, w = msg.height, msg.width
            encoding = msg.encoding.lower()

            if encoding in ("rgb8",):
                img = np.frombuffer(msg.data, dtype=np.uint8).reshape(h, w, 3)
            elif encoding in ("bgr8",):
                raw = np.frombuffer(msg.data, dtype=np.uint8).reshape(h, w, 3)
                img = cv2.cvtColor(raw, cv2.COLOR_BGR2RGB)
            elif encoding in ("mono8",):
                raw = np.frombuffer(msg.data, dtype=np.uint8).reshape(h, w)
                img = cv2.cvtColor(raw, cv2.COLOR_GRAY2RGB)
            else:
                self.get_logger().warning(
                    f"Unsupported image encoding: {msg.encoding}", throttle_duration_sec=5.0)
                return

            # Downscale if needed
            if w > self._max_width:
                scale = self._max_width / w
                new_w = self._max_width
                new_h = int(h * scale)
                img = cv2.resize(img, (new_w, new_h), interpolation=cv2.INTER_AREA)

            # Encode as JPEG (cv2 expects BGR)
            bgr = cv2.cvtColor(img, cv2.COLOR_RGB2BGR)
            _, jpeg_buf = cv2.imencode(
                ".jpg", bgr, [cv2.IMWRITE_JPEG_QUALITY, self._jpeg_quality])
            jpeg_bytes = jpeg_buf.tobytes()

            frame = pack_message(MsgType.CAMERA_FRAME, jpeg_bytes)
            try:
                self._loop.call_soon_threadsafe(self._send_queue.put_nowait, frame)
            except asyncio.QueueFull:
                pass

        except Exception as e:
            self.get_logger().warning(
                f"Image processing error: {e}", throttle_duration_sec=5.0)

    def _haptic_right_cb(self, msg):
        """Queue a haptic command for the right controller."""
        payload = struct.pack("!Bff", 1, msg.data, 100.0)
        frame = pack_message(MsgType.HAPTIC_CMD, payload)
        try:
            self._loop.call_soon_threadsafe(self._send_queue.put_nowait, frame)
        except asyncio.QueueFull:
            pass

    def publish_controller_state(self, payload: bytes):
        """Decode a CONTROLLER_STATE payload and publish to ROS 2 topics."""
        left, right = unpack_controller_state(payload)
        now = self.get_clock().now().to_msg()

        # Publish poses
        for hand, pub, data in [
            ("left", self._pub_left_pose, left),
            ("right", self._pub_right_pose, right),
        ]:
            pose_msg = PoseStamped()
            pose_msg.header.stamp = now
            pose_msg.header.frame_id = "quest_world"
            x, y, z, qx, qy, qz, qw = data["pose"]
            pose_msg.pose.position.x = x
            pose_msg.pose.position.y = y
            pose_msg.pose.position.z = z
            pose_msg.pose.orientation.x = qx
            pose_msg.pose.orientation.y = qy
            pose_msg.pose.orientation.z = qz
            pose_msg.pose.orientation.w = qw
            pub.publish(pose_msg)

        # Publish buttons as Joy message
        joy = Joy()
        joy.header.stamp = now
        # Axes: [left_trigger, left_grip, right_trigger, right_grip]
        joy.axes = [
            float(left["trigger"]),
            float(left["grip"]),
            float(right["trigger"]),
            float(right["grip"]),
        ]
        # Buttons: expand bitmasks into individual button states
        # Left: buttons_low bits 0-5, buttons_high bits 0-7
        # Right: same layout
        buttons = []
        for data in [left, right]:
            for bit in range(8):
                buttons.append(float((data["buttons_low"] >> bit) & 1))
            for bit in range(8):
                buttons.append(float((data["buttons_high"] >> bit) & 1))
        joy.buttons = [int(b) for b in buttons]
        self._pub_buttons.publish(joy)

    def publish_hand_state(self, payload: bytes):
        """Decode a HAND_STATE payload and publish to ROS 2 as JointState."""
        if not self._publish_hands:
            return

        hand_id, joints = unpack_hand_state(payload)
        pub = self._pub_left_hand if hand_id == 0 else self._pub_right_hand

        msg = JointState()
        msg.header.stamp = self.get_clock().now().to_msg()
        msg.header.frame_id = "quest_world"
        msg.name = list(HAND_JOINT_NAMES)

        # Store pose data in position field (x, y, z per joint)
        # and orientation as (qx, qy, qz, qw) interleaved in velocity/effort
        msg.position = []
        msg.velocity = []
        msg.effort = []
        for pose in joints:
            x, y, z, qx, qy, qz, qw = pose
            msg.position.extend([x, y, z])
            msg.velocity.extend([qx, qy, qz, qw])
            # effort unused but must match length for some tools
            # TODO(spec-gap): JointState normally has 1 value per joint. We use
            # position for xyz (3 per joint) and velocity for quaternion (4 per
            # joint). This is non-standard but carries the full 6DoF per joint.

        pub.publish(msg)


async def ws_loop(node: BridgeNode):
    """Connect to the Quest WS server, receive frames, send commands."""
    port = node._port
    backoff = 1.0

    while rclpy.ok():
        try:
            uri = f"ws://127.0.0.1:{port}"
            node.get_logger().info(f"Connecting to {uri}...")
            async with websockets.connect(uri) as ws:
                node.get_logger().info("Connected")
                backoff = 1.0

                # Concurrent recv + send tasks
                recv_task = asyncio.create_task(_recv_loop(ws, node))
                send_task = asyncio.create_task(_send_loop(ws, node))

                done, pending = await asyncio.wait(
                    [recv_task, send_task],
                    return_when=asyncio.FIRST_COMPLETED,
                )
                for t in pending:
                    t.cancel()
                for t in done:
                    if t.exception():
                        raise t.exception()

        except (ConnectionRefusedError, OSError, websockets.exceptions.ConnectionClosed) as e:
            node.get_logger().warning(f"Connection lost: {e}. Reconnecting in {backoff:.0f}s...")
            await asyncio.sleep(backoff)
            backoff = min(backoff * 2, 30.0)
        except asyncio.CancelledError:
            break


async def _recv_loop(ws, node: BridgeNode):
    """Receive binary frames from the Quest headset."""
    async for message in ws:
        if not isinstance(message, bytes) or len(message) < HEADER_SIZE:
            continue
        msg_type, payload_len = unpack_header(message)
        payload = message[HEADER_SIZE:HEADER_SIZE + payload_len]

        if msg_type == MsgType.CONTROLLER_STATE and len(payload) == CONTROLLER_STATE_SIZE:
            node.publish_controller_state(payload)
        if msg_type == MsgType.HAND_STATE and len(payload) == HAND_STATE_SIZE:
            node.publish_hand_state(payload)


async def _send_loop(ws, node: BridgeNode):
    """Send queued commands (haptics, camera frames) to the Quest headset."""
    while True:
        frame = await node._send_queue.get()
        await ws.send(frame)


def main(args=None):
    rclpy.init(args=args)
    node = BridgeNode()

    loop = asyncio.new_event_loop()
    node._loop = loop  # must be set before spin thread starts

    spin_thread = threading.Thread(target=rclpy.spin, args=(node,), daemon=True)
    spin_thread.start()

    try:
        loop.run_until_complete(ws_loop(node))
    except KeyboardInterrupt:
        pass
    finally:
        loop.close()
        node.destroy_node()
        rclpy.shutdown()


if __name__ == "__main__":
    main()
