#pragma once
/**
 * Spatial grounding elements — ground grid, origin axes, distance rings.
 * Rendered as GL_LINES / GL_LINE_LOOP with a simple unlit colour shader.
 */

#include <openxr/openxr.h>
#include <GLES3/gl3.h>
#include <atomic>

namespace scene_renderer {

/**
 * Compile shaders and upload line geometry.
 * Call after EGL context is current.
 */
void init();

/**
 * Render all scene elements for one eye.
 * view_pose + fov define the camera; must be called with the eye framebuffer bound.
 */
void render(const XrView& view, int viewport_w, int viewport_h);

/**
 * Enable or disable the grid overlay. Thread-safe.
 */
void set_visible(bool visible);

/**
 * Returns current visibility state.
 */
bool is_visible();

/**
 * Clean up GL resources.
 */
void destroy();

}  // namespace scene_renderer
