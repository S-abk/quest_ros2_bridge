cmake_minimum_required(VERSION 3.22.1)
project(quest_ros2_bridge LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# uWebSockets v20.75.0 (uSockets bundled submodule) — cloned from GitHub
include_directories(
    uWebSockets/src
    uWebSockets/uSockets/src
)

# OpenXR loader from Maven AAR (via Android Prefab)
find_package(OpenXR REQUIRED CONFIG)

# uSockets source files (C, compiled as C)
set(USOCKETS_SOURCES
    uWebSockets/uSockets/src/bsd.c
    uWebSockets/uSockets/src/context.c
    uWebSockets/uSockets/src/loop.c
    uWebSockets/uSockets/src/socket.c
    uWebSockets/uSockets/src/udp.c
    uWebSockets/uSockets/src/eventing/epoll_kqueue.c
)

# App native library
add_library(quest_ros2_bridge SHARED
    bridge_app.cpp
    ws_server.cpp
    xr_controller.cpp
    ${USOCKETS_SOURCES}
)

# uSockets needs LIBUS_NO_SSL for no-TLS builds
target_compile_definitions(quest_ros2_bridge PRIVATE
    LIBUS_NO_SSL
    XR_USE_PLATFORM_ANDROID
    XR_USE_GRAPHICS_API_OPENGL_ES
)

target_link_libraries(quest_ros2_bridge
    OpenXR::openxr_loader
    android
    log
    EGL
    GLESv3
    z  # zlib — required by uWebSockets PerMessageDeflate
)
