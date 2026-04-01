# Third-Party Dependencies

## Vendored in-tree (`android_app/app/src/main/cpp/`)

| Library | Version | License | Source |
|---|---|---|---|
| uWebSockets | v20.75.0 | Apache 2.0 | https://github.com/uNetworking/uWebSockets |
| uSockets | bundled with uWebSockets v20.75.0 | Apache 2.0 | https://github.com/uNetworking/uSockets |
| stb_image.h | v2.30 | MIT / Public Domain | https://github.com/nothings/stb |

The Apache 2.0 LICENSE files for uWebSockets and uSockets are included in the
vendored source tree. Neither upstream repo ships a NOTICE file (Apache 2.0
Section 4(d) only requires redistribution of NOTICE if one exists).

## Runtime dependencies (not vendored, fetched by build system)

| Library | Version | License | Source |
|---|---|---|---|
| OpenXR Loader for Android | 1.1.57 | MIT | https://github.com/KhronosGroup/OpenXR-SDK |
| websockets (Python) | >=11.0 | BSD 3-Clause | https://github.com/python-websockets/websockets |
| opencv-python | (system) | Apache 2.0 | https://github.com/opencv/opencv |
