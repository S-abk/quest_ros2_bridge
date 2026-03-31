"""ROS 2 bridge node — connects to Quest headset via WebSocket."""

import asyncio
import math
import struct
import threading

import rclpy
from rclpy.node import Node
from geometry_msgs.msg import PoseStamped
from sensor_msgs.msg import Joy

from .protocol import (
    MsgType,
    HEADER_SIZE,
    CONTROLLER_STATE_SIZE,
    CONTROLLER_BLOCK_SIZE,
    POSE_SIZE,
    pack_message,
    unpack_header,
    unpack_controller_state,
)

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

        # Publishers
        self._pub_left_pose = self.create_publisher(PoseStamped, "/oculus/left/pose", 10)
        self._pub_right_pose = self.create_publisher(PoseStamped, "/oculus/right/pose", 10)
        self._pub_buttons = self.create_publisher(Joy, "/oculus/buttons", 10)

        # Subscribers for haptics (Commit 3 will use these)
        # Placeholders registered now so test_node_import can verify them
        from std_msgs.msg import Float32
        self._sub_haptic_left = self.create_subscription(
            Float32, "/oculus/haptic/left", self._haptic_left_cb, 10)
        self._sub_haptic_right = self.create_subscription(
            Float32, "/oculus/haptic/right", self._haptic_right_cb, 10)

        # Send queue for haptic/camera commands
        self._send_queue = asyncio.Queue()

        self.get_logger().info(f"BridgeNode initialized, will connect to ws://127.0.0.1:{self._port}")

    def _haptic_left_cb(self, msg):
        """Queue a haptic command for the left controller."""
        payload = struct.pack("!Bff", 0, msg.data, 100.0)
        frame = pack_message(MsgType.HAPTIC_CMD, payload)
        try:
            self._send_queue.put_nowait(frame)
        except asyncio.QueueFull:
            pass

    def _haptic_right_cb(self, msg):
        """Queue a haptic command for the right controller."""
        payload = struct.pack("!Bff", 1, msg.data, 100.0)
        frame = pack_message(MsgType.HAPTIC_CMD, payload)
        try:
            self._send_queue.put_nowait(frame)
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
        # HAND_STATE handled in Commit 5


async def _send_loop(ws, node: BridgeNode):
    """Send queued commands (haptics, camera frames) to the Quest headset."""
    while True:
        frame = await node._send_queue.get()
        await ws.send(frame)


def main(args=None):
    rclpy.init(args=args)
    node = BridgeNode()

    # Spin rclpy in a background thread
    spin_thread = threading.Thread(target=rclpy.spin, args=(node,), daemon=True)
    spin_thread.start()

    # Run the async WS loop in an explicit event loop
    loop = asyncio.new_event_loop()
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
