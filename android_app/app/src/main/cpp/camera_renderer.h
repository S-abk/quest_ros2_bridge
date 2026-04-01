#pragma once
/**
 * Camera panel renderer — XrCompositionLayerQuad with JPEG texture.
 * World-anchored, 2m forward at eye height.
 */

#include <openxr/openxr.h>
#include <GLES3/gl3.h>
#include <mutex>
#include <vector>
#include <cstdint>

namespace camera_renderer {

/**
 * Initialize and store the session handle for lazy swapchain creation.
 * Call after EGL context is current and session is created.
 */
void init(XrSession session);

/**
 * Update the texture from a JPEG buffer.
 * Thread-safe — can be called from the WS thread.
 */
void update_frame(const uint8_t* jpeg_data, size_t jpeg_len);

/**
 * Upload staged pixels to the swapchain image if dirty.
 * Creates or recreates the swapchain lazily on first call / dimension change.
 * Must be called from the render thread each frame.
 * Returns true if at least one camera frame has been received and uploaded.
 */
bool has_frame();

/**
 * Fill out a quad layer for frame submission.
 * Call from the render thread during frame composition.
 * Returns true if the layer was filled (swapchain exists, frame uploaded).
 */
bool get_quad_layer(XrCompositionLayerQuad& layer, XrSpace space);

/**
 * Clean up swapchain and GL resources.
 */
void destroy();

}  // namespace camera_renderer
