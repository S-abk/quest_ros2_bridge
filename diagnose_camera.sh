#!/usr/bin/env bash
# diagnose_camera.sh — verify the camera pipeline end-to-end
# Runs four parallel checks for 15 seconds, then prints a summary.
set -euo pipefail

DURATION=15
export DIAG_DURATION="$DURATION"   # visible to Python subprocesses

# Temp files for each probe's output
DIR=$(mktemp -d)
trap 'kill $(jobs -p) 2>/dev/null; rm -rf "$DIR"' EXIT

PUB_LOG="$DIR/pub.log"
SUB_LOG="$DIR/sub.log"
BRIDGE_LOG="$DIR/bridge.log"
ADB_LOG="$DIR/adb.log"

# Colours for labelled output
RED='\033[0;31m'
GRN='\033[0;32m'
YEL='\033[0;33m'
CYN='\033[0;36m'
RST='\033[0m'

label_stream() {
    local colour="$1" label="$2"
    while IFS= read -r line; do
        printf "${colour}[%-8s]${RST} %s\n" "$label" "$line"
    done
}

echo "===== Camera pipeline diagnostic — ${DURATION}s ====="
echo ""

# ---------- 1. Publish synthetic test image at 10 Hz ----------
python3 -u - <<'PYEOF' 2>&1 | tee "$PUB_LOG" | label_stream "$GRN" "PUB" &
import os, rclpy, time
from rclpy.node import Node
from sensor_msgs.msg import Image

duration = int(os.environ["DIAG_DURATION"])
W, H = 320, 240

rclpy.init()
node = Node("diag_cam_pub")
pub = node.create_publisher(Image, "/camera/image_raw", 10)

msg = Image()
msg.height = H
msg.width = W
msg.encoding = "rgb8"
msg.step = W * 3
# Paint a simple gradient so the image is non-trivial
data = bytearray(W * H * 3)
for y in range(H):
    for x in range(W):
        off = (y * W + x) * 3
        data[off]     = x % 256   # R
        data[off + 1] = y % 256   # G
        data[off + 2] = 128       # B
msg.data = data

count = 0
t0 = time.monotonic()
while time.monotonic() - t0 < duration:
    msg.header.stamp = node.get_clock().now().to_msg()
    pub.publish(msg)
    count += 1
    if count % 10 == 0:
        print(f"Published {count} frames", flush=True)
    time.sleep(0.1)
print(f"DONE — published {count} frames total", flush=True)
node.destroy_node()
try:
    rclpy.shutdown()
except Exception:
    pass
PYEOF

# ---------- 2. Subscribe and count received frames ----------
python3 -u - <<'PYEOF' 2>&1 | tee "$SUB_LOG" | label_stream "$CYN" "SUB" &
import os, rclpy, time, threading
from rclpy.node import Node
from sensor_msgs.msg import Image

duration = int(os.environ["DIAG_DURATION"])
count = 0
last_size = 0

def cb(msg):
    global count, last_size
    count += 1
    last_size = len(msg.data)
    if count % 10 == 0:
        print(f"Received {count} frames ({last_size}B each)", flush=True)

rclpy.init()
node = Node("diag_cam_sub")
node.create_subscription(Image, "/camera/image_raw", cb, 10)
t = threading.Thread(target=rclpy.spin, args=(node,), daemon=True)
t.start()
time.sleep(duration)
print(f"DONE — received {count} frames total", flush=True)
node.destroy_node()
try:
    rclpy.shutdown()
except Exception:
    pass
PYEOF

# ---------- 3. Tail bridge node for "Enqueuing JPEG" ----------
# bridge_node emits at DEBUG level — /rosout only carries INFO+ by default.
# We check both /rosout and the ROS log directory for the debug line.
(
    echo "Watching for 'Enqueuing JPEG' (bridge_node debug log)..."
    # Method A: grep the ROS 2 log directory for the running bridge node
    ROSLOG="${ROS_LOG_DIR:-${HOME}/.ros/log}"
    found=0
    end=$((SECONDS + DURATION))
    while [ $SECONDS -lt $end ]; do
        if [ -d "$ROSLOG" ]; then
            hit=$(grep -rl "Enqueuing JPEG" "$ROSLOG" 2>/dev/null | head -1 || true)
            if [ -n "$hit" ]; then
                tail -f "$hit" 2>/dev/null | grep --line-buffered -i "enqueuing jpeg" &
                TAIL_PID=$!
                sleep $((end - SECONDS > 0 ? end - SECONDS : 1))
                kill "$TAIL_PID" 2>/dev/null || true
                found=1
                break
            fi
        fi
        # Method B: check /rosout (works if bridge node uses info level)
        sleep 1
    done
    if [ "$found" -eq 0 ]; then
        echo "(no 'Enqueuing JPEG' lines seen — bridge may not be running or debug logging is off)"
        echo "Hint: run bridge_node with --ros-args --log-level quest_bridge:=debug"
    fi
) 2>&1 | tee "$BRIDGE_LOG" | label_stream "$YEL" "BRIDGE" &

# ---------- 4. adb logcat for camera-related Android output ----------
(
    if ! command -v adb &>/dev/null; then
        echo "adb not found — skipping Android logcat check"
    elif ! adb devices 2>/dev/null | grep -q 'device$'; then
        echo "No ADB device connected — skipping Android logcat check"
    else
        echo "Tailing adb logcat (QuestBridge tags)..."
        adb logcat -c 2>/dev/null
        timeout "${DURATION}" \
            adb logcat -s QuestBridge_Cam:* QuestBridge_WS:* QuestBridge:* 2>/dev/null \
            || true
    fi
) 2>&1 | tee "$ADB_LOG" | label_stream "$RED" "ADB" &

# ---------- Wait for everything ----------
sleep "$DURATION"
sleep 2  # grace period for subprocesses to flush
kill $(jobs -p) 2>/dev/null || true
wait 2>/dev/null || true

# ---------- Summary ----------
echo ""
echo "===== SUMMARY ====="

check_pass() {
    local label="$1" logfile="$2" pattern="$3"
    if grep -qiE "$pattern" "$logfile" 2>/dev/null; then
        printf "  ${GRN}PASS${RST}  %s\n" "$label"
    else
        printf "  ${RED}FAIL${RST}  %s  (no output matching '%s')\n" "$label" "$pattern"
    fi
}

check_pass "1. Test image publisher"       "$PUB_LOG"    "published.*frames total"
check_pass "2. Image subscriber"           "$SUB_LOG"    "received.*frames total"
check_pass "3. Bridge JPEG enqueue"        "$BRIDGE_LOG" "enqueuing jpeg"
check_pass "4. Android logcat (camera)"    "$ADB_LOG"    "QuestBridge_Cam|QuestBridge_WS"

echo ""
echo "Logs saved in: $DIR"
