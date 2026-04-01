/**
 * Spatial grounding elements — ground grid, origin axes, distance rings.
 * Simple unlit colour shader, GL_LINES / GL_LINE_LOOP.
 */

#include "scene_renderer.h"

#include <android/log.h>
#include <cmath>
#include <vector>
#include <atomic>
#include <cstring>

#define LOG_TAG "QuestBridge_Scene"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace scene_renderer {

// ---------------------------------------------------------------------------
// Shader sources
// ---------------------------------------------------------------------------
static const char* VERT_SRC = R"glsl(#version 300 es
uniform mat4 u_vp;
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec4 a_col;
out vec4 v_col;
void main() {
    v_col = a_col;
    gl_Position = u_vp * vec4(a_pos, 1.0);
}
)glsl";

static const char* FRAG_SRC = R"glsl(#version 300 es
precision mediump float;
in vec4 v_col;
out vec4 frag_color;
void main() {
    frag_color = v_col;
}
)glsl";

// ---------------------------------------------------------------------------
// Vertex: position (3f) + colour (4f) = 7 floats
// ---------------------------------------------------------------------------
struct Vertex {
    float x, y, z;
    float r, g, b, a;
};

// GL state
static GLuint program_ = 0;
static GLint u_vp_loc_ = -1;
static GLuint vao_ = 0;
static GLuint vbo_ = 0;

// Geometry counts (for draw calls)
static GLsizei grid_start_ = 0, grid_count_ = 0;
static GLsizei axes_start_ = 0, axes_count_ = 0;
static GLsizei ring1_start_ = 0, ring1_count_ = 0;
static GLsizei ring2_start_ = 0, ring2_count_ = 0;
static GLsizei ring3_start_ = 0, ring3_count_ = 0;

static std::atomic<bool> visible_{true};

// ---------------------------------------------------------------------------
// Matrix helpers (column-major for GL)
// ---------------------------------------------------------------------------

// Build a projection matrix from OpenXR fov angles (asymmetric frustum)
static void make_projection(float* m, const XrFovf& fov, float near_z, float far_z) {
    float l = std::tan(fov.angleLeft) * near_z;
    float r = std::tan(fov.angleRight) * near_z;
    float t = std::tan(fov.angleUp) * near_z;
    float b = std::tan(fov.angleDown) * near_z;

    std::memset(m, 0, 16 * sizeof(float));
    m[0]  = 2.0f * near_z / (r - l);
    m[5]  = 2.0f * near_z / (t - b);
    m[8]  = (r + l) / (r - l);
    m[9]  = (t + b) / (t - b);
    m[10] = -(far_z + near_z) / (far_z - near_z);
    m[11] = -1.0f;
    m[14] = -2.0f * far_z * near_z / (far_z - near_z);
}

// Build a view matrix from an XrPosef (inverse of the pose)
static void make_view(float* m, const XrPosef& pose) {
    float qx = pose.orientation.x, qy = pose.orientation.y;
    float qz = pose.orientation.z, qw = pose.orientation.w;

    // Rotation matrix from quaternion (transposed = inverse rotation)
    float r00 = 1 - 2*(qy*qy + qz*qz);
    float r01 = 2*(qx*qy + qz*qw);
    float r02 = 2*(qx*qz - qy*qw);
    float r10 = 2*(qx*qy - qz*qw);
    float r11 = 1 - 2*(qx*qx + qz*qz);
    float r12 = 2*(qy*qz + qx*qw);
    float r20 = 2*(qx*qz + qy*qw);
    float r21 = 2*(qy*qz - qx*qw);
    float r22 = 1 - 2*(qx*qx + qy*qy);

    float px = pose.position.x, py = pose.position.y, pz = pose.position.z;

    // Column-major: columns are rows of the transposed rotation
    m[0] = r00; m[1] = r01; m[2]  = r02; m[3]  = 0;
    m[4] = r10; m[5] = r11; m[6]  = r12; m[7]  = 0;
    m[8] = r20; m[9] = r21; m[10] = r22; m[11] = 0;
    m[12] = -(r00*px + r10*py + r20*pz);
    m[13] = -(r01*px + r11*py + r21*pz);
    m[14] = -(r02*px + r12*py + r22*pz);
    m[15] = 1;
}

// Multiply two 4×4 column-major matrices: out = a * b
static void mat4_mul(float* out, const float* a, const float* b) {
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            out[c*4 + r] = a[0*4+r]*b[c*4+0] + a[1*4+r]*b[c*4+1]
                         + a[2*4+r]*b[c*4+2] + a[3*4+r]*b[c*4+3];
        }
    }
}

// ---------------------------------------------------------------------------
// Geometry generation
// ---------------------------------------------------------------------------

static void generate_geometry(std::vector<Vertex>& verts) {
    // --- Ground grid: 10×10m, 0.5m spacing, at Y=0 ---
    const float grid_half = 5.0f;
    const float grid_step = 0.5f;
    const float gc_r = 0.2f, gc_g = 0.2f, gc_b = 0.2f, gc_a = 0.6f;

    grid_start_ = static_cast<GLsizei>(verts.size());
    for (float x = -grid_half; x <= grid_half + 0.01f; x += grid_step) {
        verts.push_back({x, 0, -grid_half, gc_r, gc_g, gc_b, gc_a});
        verts.push_back({x, 0,  grid_half, gc_r, gc_g, gc_b, gc_a});
    }
    for (float z = -grid_half; z <= grid_half + 0.01f; z += grid_step) {
        verts.push_back({-grid_half, 0, z, gc_r, gc_g, gc_b, gc_a});
        verts.push_back({ grid_half, 0, z, gc_r, gc_g, gc_b, gc_a});
    }
    grid_count_ = static_cast<GLsizei>(verts.size()) - grid_start_;

    // --- Origin axis cross: 0.3m long, RGB ---
    const float axis_len = 0.3f;
    // We draw each axis as a thin pair of lines offset slightly for "thickness"
    // (true line width >1 is not guaranteed in GLES 3.0)
    const float t = 0.001f; // half-thickness offset
    axes_start_ = static_cast<GLsizei>(verts.size());
    // X axis (red) — main line + offset for thickness
    verts.push_back({0, 0, 0,        1, 0, 0, 1});
    verts.push_back({axis_len, 0, 0, 1, 0, 0, 1});
    verts.push_back({0, t, 0,        1, 0, 0, 1});
    verts.push_back({axis_len, t, 0, 1, 0, 0, 1});
    // Y axis (green)
    verts.push_back({0, 0, 0,        0, 1, 0, 1});
    verts.push_back({0, axis_len, 0, 0, 1, 0, 1});
    verts.push_back({t, 0, 0,        0, 1, 0, 1});
    verts.push_back({t, axis_len, 0, 0, 1, 0, 1});
    // Z axis (blue)
    verts.push_back({0, 0, 0,        0, 0, 1, 1});
    verts.push_back({0, 0, axis_len, 0, 0, 1, 1});
    verts.push_back({0, t, 0,        0, 0, 1, 1});
    verts.push_back({0, t, axis_len, 0, 0, 1, 1});
    axes_count_ = static_cast<GLsizei>(verts.size()) - axes_start_;

    // --- Distance rings at Y=0: 1m, 2m, 3m radius, 64 segments ---
    const int seg = 64;
    const float rc_r = 0.3f, rc_g = 0.3f, rc_b = 0.35f, rc_a = 0.4f;

    auto add_ring = [&](float radius, GLsizei& start, GLsizei& count) {
        start = static_cast<GLsizei>(verts.size());
        for (int i = 0; i < seg; ++i) {
            float angle = 2.0f * M_PI * static_cast<float>(i) / static_cast<float>(seg);
            verts.push_back({radius * std::cos(angle), 0, radius * std::sin(angle),
                             rc_r, rc_g, rc_b, rc_a});
        }
        count = seg;
    };

    add_ring(1.0f, ring1_start_, ring1_count_);
    add_ring(2.0f, ring2_start_, ring2_count_);
    add_ring(3.0f, ring3_start_, ring3_count_);
}

// ---------------------------------------------------------------------------
// Shader compilation
// ---------------------------------------------------------------------------

static GLuint compile_shader(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        LOGE("Shader compile error: %s", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void init() {
    // Compile shader program
    GLuint vs = compile_shader(GL_VERTEX_SHADER, VERT_SRC);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, FRAG_SRC);
    if (!vs || !fs) {
        LOGE("Failed to compile scene shaders");
        return;
    }
    program_ = glCreateProgram();
    glAttachShader(program_, vs);
    glAttachShader(program_, fs);
    glLinkProgram(program_);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint ok = 0;
    glGetProgramiv(program_, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(program_, sizeof(log), nullptr, log);
        LOGE("Shader link error: %s", log);
        glDeleteProgram(program_);
        program_ = 0;
        return;
    }
    u_vp_loc_ = glGetUniformLocation(program_, "u_vp");

    // Generate geometry
    std::vector<Vertex> verts;
    generate_geometry(verts);

    // Upload to GPU
    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(verts.size() * sizeof(Vertex)),
                 verts.data(), GL_STATIC_DRAW);

    // a_pos = location 0
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          reinterpret_cast<void*>(0));
    // a_col = location 1
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          reinterpret_cast<void*>(3 * sizeof(float)));

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    LOGI("Scene renderer initialized: %zu vertices", verts.size());
}

void render(const XrView& view, int viewport_w, int viewport_h) {
    if (!program_ || !visible_.load()) return;

    // Build view-projection matrix
    float proj[16], viewm[16], vp[16];
    make_projection(proj, view.fov, 0.1f, 100.0f);
    make_view(viewm, view.pose);
    mat4_mul(vp, proj, viewm);

    // Save/restore GL state that we modify
    GLboolean blend_was_on;
    glGetBooleanv(GL_BLEND, &blend_was_on);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glUseProgram(program_);
    glUniformMatrix4fv(u_vp_loc_, 1, GL_FALSE, vp);
    glBindVertexArray(vao_);

    // Ground grid
    glDrawArrays(GL_LINES, grid_start_, grid_count_);

    // Origin axes
    glDrawArrays(GL_LINES, axes_start_, axes_count_);

    // Distance rings
    glDrawArrays(GL_LINE_LOOP, ring1_start_, ring1_count_);
    glDrawArrays(GL_LINE_LOOP, ring2_start_, ring2_count_);
    glDrawArrays(GL_LINE_LOOP, ring3_start_, ring3_count_);

    glBindVertexArray(0);
    glUseProgram(0);

    if (!blend_was_on) glDisable(GL_BLEND);
}

void set_visible(bool visible) {
    visible_.store(visible);
    LOGI("Scene grid visibility: %s", visible ? "on" : "off");
}

bool is_visible() {
    return visible_.load();
}

void destroy() {
    if (vao_) { glDeleteVertexArrays(1, &vao_); vao_ = 0; }
    if (vbo_) { glDeleteBuffers(1, &vbo_); vbo_ = 0; }
    if (program_) { glDeleteProgram(program_); program_ = 0; }
}

}  // namespace scene_renderer
