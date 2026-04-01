/**
 * Camera panel renderer — decodes JPEG, uploads to OpenXR swapchain,
 * renders as XrCompositionLayerQuad.
 */

#include "camera_renderer.h"

#include <jni.h>
#include <EGL/egl.h>
#include <openxr/openxr_platform.h>
#include <android/log.h>
#include <cstring>
#include <atomic>

// stb_image for JPEG decoding — header-only, define implementation once
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#include "stb_image.h"

#define LOG_TAG "QuestBridge_Cam"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace camera_renderer {

// Session handle (stored at init, used for lazy swapchain creation)
static XrSession session_ = XR_NULL_HANDLE;

// Swapchain state (render thread only — no mutex needed)
static XrSwapchain swapchain_ = XR_NULL_HANDLE;
static XrSwapchainImageOpenGLESKHR swapchain_image_{XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR};
static int swapchain_w_ = 0;
static int swapchain_h_ = 0;

// Staging buffer for decoded pixels (written from WS thread, read from render thread)
static std::mutex staging_mutex_;
static std::vector<uint8_t> staging_pixels_;
static int staging_w_ = 0;
static int staging_h_ = 0;
static std::atomic<bool> staging_dirty_{false};
static std::atomic<bool> has_frame_{false};

// CameraPanelTransform — isolated for easy swap to yaw-locked later
static XrPosef camera_panel_pose() {
    XrPosef pose{};
    pose.orientation = {0.0f, 0.0f, 0.0f, 1.0f};  // Identity rotation
    pose.position = {0.0f, 1.5f, -2.0f};           // 2m forward, eye height
    return pose;
}

static void destroy_swapchain() {
    if (swapchain_ != XR_NULL_HANDLE) {
        xrDestroySwapchain(swapchain_);
        swapchain_ = XR_NULL_HANDLE;
    }
    swapchain_image_ = {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR};
    swapchain_w_ = 0;
    swapchain_h_ = 0;
}

static bool create_swapchain(int w, int h) {
    if (session_ == XR_NULL_HANDLE) return false;

    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    info.format = GL_SRGB8_ALPHA8;
    info.sampleCount = 1;
    info.width = w;
    info.height = h;
    info.faceCount = 1;
    info.arraySize = 1;
    info.mipCount = 1;

    XrResult result = xrCreateSwapchain(session_, &info, &swapchain_);
    if (XR_FAILED(result)) {
        LOGW("xrCreateSwapchain GL_SRGB8_ALPHA8 failed (%d), trying GL_RGBA8", result);
        info.format = GL_RGBA8;
        result = xrCreateSwapchain(session_, &info, &swapchain_);
        if (XR_FAILED(result)) {
            LOGE("xrCreateSwapchain GL_RGBA8 also failed: %d", result);
            return false;
        }
    }

    uint32_t img_count = 0;
    xrEnumerateSwapchainImages(swapchain_, 0, &img_count, nullptr);
    if (img_count == 0) {
        LOGE("Swapchain has 0 images");
        destroy_swapchain();
        return false;
    }
    std::vector<XrSwapchainImageOpenGLESKHR> images(
        img_count, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
    xrEnumerateSwapchainImages(swapchain_, img_count, &img_count,
        reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));
    swapchain_image_ = images[0];

    swapchain_w_ = w;
    swapchain_h_ = h;
    LOGI("Camera swapchain created: %dx%d (texture=%u)", w, h, swapchain_image_.image);
    return true;
}

void init(XrSession session) {
    session_ = session;
    LOGI("Camera renderer initialized");
}

void update_frame(const uint8_t* jpeg_data, size_t jpeg_len) {
    int w, h, channels;
    unsigned char* pixels = stbi_load_from_memory(
        jpeg_data, static_cast<int>(jpeg_len), &w, &h, &channels, 4);

    if (!pixels) {
        LOGW("Failed to decode JPEG frame");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(staging_mutex_);
        staging_pixels_.assign(pixels, pixels + w * h * 4);
        staging_w_ = w;
        staging_h_ = h;
        staging_dirty_.store(true);
    }

    stbi_image_free(pixels);

    if (!has_frame_.load()) {
        LOGI("First camera frame received: %dx%d", w, h);
        has_frame_.store(true);
    }
}

bool has_frame() {
    if (!has_frame_.load()) return false;

    // Ensure swapchain exists (lazy create / recreate on dimension change)
    int w, h;
    {
        std::lock_guard<std::mutex> lock(staging_mutex_);
        w = staging_w_;
        h = staging_h_;
    }
    if (swapchain_ == XR_NULL_HANDLE || w != swapchain_w_ || h != swapchain_h_) {
        destroy_swapchain();
        if (!create_swapchain(w, h)) {
            return false;
        }
    }

    return true;
}

bool get_quad_layer(XrCompositionLayerQuad& layer, XrSpace space) {
    if (swapchain_ == XR_NULL_HANDLE || swapchain_w_ == 0) return false;

    // Acquire swapchain image for this frame
    XrSwapchainImageAcquireInfo acquire_info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    uint32_t img_index = 0;
    XrResult result = xrAcquireSwapchainImage(swapchain_, &acquire_info, &img_index);
    if (XR_FAILED(result)) {
        LOGW("xrAcquireSwapchainImage failed: %d", result);
        return false;
    }

    XrSwapchainImageWaitInfo wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait_info.timeout = XR_INFINITE_DURATION;
    result = xrWaitSwapchainImage(swapchain_, &wait_info);
    if (XR_FAILED(result)) {
        LOGW("xrWaitSwapchainImage failed: %d", result);
        XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrReleaseSwapchainImage(swapchain_, &release);
        return false;
    }

    // Upload new pixels if the staging buffer has been updated
    if (staging_dirty_.load()) {
        std::lock_guard<std::mutex> lock(staging_mutex_);
        staging_dirty_.store(false);

        glBindTexture(GL_TEXTURE_2D, swapchain_image_.image);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, staging_w_, staging_h_,
                        GL_RGBA, GL_UNSIGNED_BYTE, staging_pixels_.data());
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    // Release — must happen every frame, whether or not we uploaded
    XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(swapchain_, &release_info);

    // Populate the quad layer
    layer = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    layer.space = space;
    layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;

    layer.subImage.swapchain = swapchain_;
    layer.subImage.imageRect = {{0, 0}, {swapchain_w_, swapchain_h_}};
    layer.subImage.imageArrayIndex = 0;

    layer.pose = camera_panel_pose();
    float aspect = static_cast<float>(swapchain_w_) / static_cast<float>(swapchain_h_);
    layer.size = {1.0f, 1.0f / aspect};

    return true;
}

void destroy() {
    destroy_swapchain();
    session_ = XR_NULL_HANDLE;
    has_frame_.store(false);
}

}  // namespace camera_renderer
