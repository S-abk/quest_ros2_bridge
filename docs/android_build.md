---
title: Android Build Guide
tags: [android, build, ndk, gradle, uwebsockets]
---

# Android Build Guide

## Prerequisites

| Tool | Version | Notes |
|---|---|---|
| Android SDK | Platform 33 | `compileSdk 33`, `targetSdk 33` |
| NDK | r25c (25.2.9519653) | Pinned — do not change |
| CMake | 3.22.1 | Auto-installed by Gradle on first build |
| AGP | 8.1.4 | Android Gradle Plugin |
| Gradle | 8.4 | Via wrapper (`./gradlew`) |
| Kotlin | 1.9.22 | Minimal glue — real work is C++ |
| Java | 17+ | For Gradle daemon |

## NDK Pinning Rationale

NDK r25c is pinned in `app/build.gradle` via `ndkVersion '25.2.9519653'`. Reasons:
- Quest 2 minimum (`minSdk 29`) is well-supported by r25c
- Clang 14.0.7 in r25c is the last version tested with uWebSockets v20.75.0
- Newer NDKs may introduce breaking changes in libc++ or compiler warnings
- `ndk.dir` in `local.properties` is deprecated — use `ndkVersion` in build.gradle

Install if missing:
```bash
sdkmanager "ndk;25.2.9519653"
```

## uWebSockets Vendoring

uWebSockets v20.75.0 and uSockets are vendored in-tree at `android_app/app/src/main/cpp/uWebSockets/`. This ensures hermetic builds with no network access required.

### How it was vendored

```bash
git clone --recurse-submodules --depth=1 \
  --branch v20.75.0 \
  https://github.com/uNetworking/uWebSockets.git \
  android_app/app/src/main/cpp/uWebSockets
```

The `.git` directories were removed and the source committed directly. uSockets is a submodule of uWebSockets — do not clone it separately.

### CMake integration

```cmake
include_directories(
    uWebSockets/src
    uWebSockets/uSockets/src
)

# uSockets C sources compiled directly
set(USOCKETS_SOURCES
    uWebSockets/uSockets/src/bsd.c
    uWebSockets/uSockets/src/context.c
    uWebSockets/uSockets/src/loop.c
    uWebSockets/uSockets/src/socket.c
    uWebSockets/uSockets/src/udp.c
    uWebSockets/uSockets/src/eventing/epoll_kqueue.c
)
```

Key compile definitions:
- `LIBUS_NO_SSL` — no TLS (local ADB connection only)
- `XR_USE_PLATFORM_ANDROID` and `XR_USE_GRAPHICS_API_OPENGL_ES` — OpenXR platform

## OpenXR Loader

The OpenXR loader is fetched from Maven Central via Gradle:

```groovy
implementation 'org.khronos.openxr:openxr_loader_for_android:1.1.57'
```

Exposed to CMake via Android Prefab (`buildFeatures { prefab true }`). In CMake:

```cmake
find_package(OpenXR REQUIRED CONFIG)
target_link_libraries(quest_ros2_bridge OpenXR::openxr_loader ...)
```

## Build Commands

```bash
cd android_app
./gradlew assembleDebug
```

Deploy:
```bash
adb install -r app/build/outputs/apk/debug/app-debug.apk
adb forward tcp:9090 tcp:9090
adb shell am start -n com.quest.bridge/.MainActivity
```

## Common Build Errors

### `Could not find a package configuration file provided by "OpenXR"`

**Cause:** `buildFeatures { prefab true }` missing from `app/build.gradle`.
**Fix:** Add the prefab block inside `android {}`.

### `undefined symbol: deflateInit2_` (linker error)

**Cause:** uWebSockets' PerMessageDeflate needs zlib.
**Fix:** Add `z` to `target_link_libraries`.

### `unknown type name 'XrSwapchainImageOpenGLESKHR'`

**Cause:** `openxr/openxr_platform.h` needs JNI and EGL headers included first.
**Fix:** Include `<jni.h>` and `<EGL/egl.h>` before `<openxr/openxr_platform.h>`.

### `no matching member function for call to 'listen'` (uWebSockets)

**Cause:** Lambda captures make it incompatible with `MoveOnlyFunction`. uWS v20.75.0 uses `any_invocable` which rejects certain capture patterns.
**Fix:** Use explicit `us_listen_socket_t*` parameter type, avoid capturing `this` in the listen callback (use a pointer instead).

### `us_listen_socket_t` namespace mismatch

**Cause:** Forward-declaring `us_listen_socket_t` inside a namespace creates a different type from the global one in libusockets.
**Fix:** Forward-declare in global scope: `struct us_listen_socket_t;` before the namespace block.

## Related Docs

- [[architecture]] — system overview
- [[camera_panel]] — camera swapchain build issues
- [[hardware_testing]] — deployment and testing workflow
