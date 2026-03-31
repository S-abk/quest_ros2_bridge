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
 * Initialize GL resources (texture, etc.).
 * Call after EGL context is current.
 */
void init();

/**
 * Update the texture from a JPEG buffer.
 * Thread-safe — can be called from the WS thread.
 */
void update_frame(const uint8_t* jpeg_data, size_t jpeg_len);

/**
 * Returns true if at least one camera frame has been received.
 */
bool has_frame();

/**
 * Fill out a quad layer for frame submission.
 * Call from the render thread during frame composition.
 * Returns true if the layer was filled (has_frame is true).
 */
bool get_quad_layer(XrCompositionLayerQuad& layer, XrSpace space);

/**
 * Clean up GL resources.
 */
void destroy();

}  // namespace camera_renderer
