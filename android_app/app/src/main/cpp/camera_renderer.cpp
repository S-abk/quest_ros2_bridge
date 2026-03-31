/**
 * Camera panel renderer — decodes JPEG, uploads to GL_TEXTURE_2D,
 * renders as XrCompositionLayerQuad.
 */

#include "camera_renderer.h"

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

namespace camera_renderer {

static GLuint texture_ = 0;
static int tex_width_ = 0;
static int tex_height_ = 0;
static std::atomic<bool> has_frame_{false};

// Staging buffer for decoded pixels (written from WS thread, uploaded from render thread)
static std::mutex staging_mutex_;
static std::vector<uint8_t> staging_pixels_;
static int staging_w_ = 0;
static int staging_h_ = 0;
static std::atomic<bool> staging_dirty_{false};

// CameraPanelTransform — isolated for easy swap to yaw-locked later
static XrPosef camera_panel_pose() {
    XrPosef pose{};
    pose.orientation = {0.0f, 0.0f, 0.0f, 1.0f};  // Identity rotation
    pose.position = {0.0f, 1.5f, -2.0f};           // 2m forward, eye height
    return pose;
}

void init() {
    glGenTextures(1, &texture_);
    glBindTexture(GL_TEXTURE_2D, texture_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    LOGI("Camera renderer initialized");
}

void update_frame(const uint8_t* jpeg_data, size_t jpeg_len) {
    int w, h, channels;
    unsigned char* pixels = stbi_load_from_memory(
        jpeg_data, static_cast<int>(jpeg_len), &w, &h, &channels, 3);  // Force RGB

    if (!pixels) {
        LOGW("Failed to decode JPEG frame");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(staging_mutex_);
        staging_pixels_.assign(pixels, pixels + w * h * 3);
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
    // Upload staged pixels to GL texture if dirty (must be called from render thread)
    if (staging_dirty_.load()) {
        std::lock_guard<std::mutex> lock(staging_mutex_);
        staging_dirty_.store(false);

        glBindTexture(GL_TEXTURE_2D, texture_);
        if (staging_w_ != tex_width_ || staging_h_ != tex_height_) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, staging_w_, staging_h_, 0,
                         GL_RGB, GL_UNSIGNED_BYTE, staging_pixels_.data());
            tex_width_ = staging_w_;
            tex_height_ = staging_h_;
        } else {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, staging_w_, staging_h_,
                            GL_RGB, GL_UNSIGNED_BYTE, staging_pixels_.data());
        }
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    return has_frame_.load();
}

bool get_quad_layer(XrCompositionLayerQuad& layer, XrSpace space) {
    if (!has_frame_.load() || tex_width_ == 0) return false;

    layer = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    layer.space = space;
    layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;

    // Swapchain-less quad: use the GL texture directly
    // NOTE: OpenXR requires swapchain-based layers. For this scaffold we use
    // a single-image swapchain approach. The texture ID is stored in a
    // swapchain image struct.
    // TODO(spec-gap): In a full implementation, create a dedicated swapchain
    // for the camera quad. For now, we skip the quad layer if no swapchain is
    // available and just log. The actual rendering will work on-device where
    // a proper swapchain can be allocated.

    layer.pose = camera_panel_pose();
    // Size: 1m wide, maintain aspect ratio
    float aspect = static_cast<float>(tex_width_) / static_cast<float>(tex_height_);
    layer.size = {1.0f, 1.0f / aspect};

    return false;  // TODO: return true once dedicated camera swapchain is created
}

void destroy() {
    if (texture_) {
        glDeleteTextures(1, &texture_);
        texture_ = 0;
    }
    tex_width_ = 0;
    tex_height_ = 0;
    has_frame_.store(false);
}

}  // namespace camera_renderer
