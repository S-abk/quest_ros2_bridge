# DEPENDENCY_VERSIONS

Pre-verified on 2026-03-31. All fields confirmed via live sources. Claude Code treats this file as ground truth and will not re-look up or alter these values.

---

## OpenXR loader (Maven Central)

```
org.khronos.openxr:openxr_loader_for_android:1.1.57
```

Verified at: https://central.sonatype.com/artifact/org.khronos.openxr/openxr_loader_for_android Latest stable as of 2026-03-31. Use this exact version in build.gradle — no `+` or `latest`.

---

## uWebSockets

```
uWebSockets release tag : v20.75.0
uSockets                : bundled git submodule — no separate version tag
```

Verified at: https://github.com/uNetworking/uWebSockets/releases

**Important**: uSockets is not separately versioned. It is a git submodule of uWebSockets. Do NOT copy uSockets independently. Clone uWebSockets at the pinned tag with submodules:

```bash
git clone --recurse-submodules --depth=1 \
  --branch v20.75.0 \
  https://github.com/uNetworking/uWebSockets.git \
  android_app/app/src/main/cpp/uWebSockets
```

This gives you both `uWebSockets/src/` and `uWebSockets/uSockets/src/` at the correct matched versions. Reference both in CMakeLists.txt include paths.

---

## Python websockets library

```
websockets : >=11.0
```

Pin as `>=11.0` in `install_requires` (major API break at v10→v11). The `async with websockets.connect()` API is confirmed stable in v11+. Key v10→v11 breaking change: `websockets.client.connect()` removed; use `websockets.connect()` directly.

---

## NDK

```
NDK version : r25c  (25.2.9519653)
```

Pinned per CLAUDE.md. Do not change.

---

## Android SDK levels

```
minSdk    : 29
targetSdk : 33
```

---

## ROS 2 baseline

```
Baseline distro : Humble
Python baseline : 3.10
```

Use only rclpy APIs present in Humble. APIs confirmed Humble-compatible:

- `declare_parameter`, `get_parameter`
- `create_publisher`, `create_subscription`
- `get_logger().warning()` (not `.warn()`)
- `Node.__init__` with `node_name` positional arg

---

## Flags / notes from pre-verification

- OpenXR 1.1.57 is a minor version bump from 1.0.x — fully backwards compatible, same API, same extension names. CLAUDE.md reference to 1.0.34 is superseded by this file.
- uWebSockets v20.75.0 introduces simdutf for UTF-8 validation — no API changes relevant to our binary WebSocket usage.
- uSockets submodule pattern means CMakeLists.txt must include both: include_directories(uWebSockets/src uWebSockets/uSockets/src)
- XR_EXT_hand_tracking confirmed in OpenXR 1.0 spec as a ratified extension, supported on Quest 2 running firmware 50+ (minSdk 29 is sufficient).
- websockets v11 async context manager confirmed: use async with websockets.connect("ws://...") as ws: not the legacy websockets.client.connect() form.
