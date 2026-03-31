# quest_ros2_bridge

Bidirectional ROS 2 <-> Meta Quest bridge via WebSocket + OpenXR.

A modern replacement for `rail-berkeley/oculus_reader` — no legacy SDK dependencies.

## Features

1. **Controller poses + buttons** — 6DoF transforms and button states published to ROS 2
2. **Haptic vibration** — subscribe to ROS 2 topics to vibrate controllers
3. **Robot camera feed** — subscribe to ROS 2 camera topics, render as floating panel in headset
4. **Hand tracking** — 26-joint hand poses published to ROS 2

## Quick Start

### Android (Quest headset)
```bash
cd android_app
./gradlew assembleDebug
adb install app/build/outputs/apk/debug/app-debug.apk
adb forward tcp:9090 tcp:9090
adb shell am start -n com.quest.bridge/.MainActivity
```

### ROS 2 (PC)
```bash
cd ros2_bridge
pip install -e .
ros2 run ros2_bridge bridge_node
```

### Smoke tests
```bash
ros2 topic echo /oculus/right/pose
ros2 topic pub /oculus/haptic/right std_msgs/msg/Float32 "data: 0.8" --once
```

## Tested on

| | Quest 2 | Quest 3 | Quest 3S | Quest Pro |
|---|---|---|---|---|
| **Humble** | ⬜ | ⬜ | ⬜ | ⬜ |
| **Jazzy** | ⬜ | ⬜ | ⬜ | ⬜ |
| **Kilted** | ⬜ | ⬜ | ⬜ | ⬜ |

## Dependencies

See [DEPENDENCY_VERSIONS.md](DEPENDENCY_VERSIONS.md) for all pinned versions.
