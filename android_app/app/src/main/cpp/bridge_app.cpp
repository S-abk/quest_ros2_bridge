/**
 * Main OpenXR app — session lifecycle, controller input, WebSocket bridge.
 */

#include <jni.h>
#include <android/log.h>
#include <android/native_activity.h>

#include <EGL/egl.h>
#include <GLES3/gl3.h>

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "ws_server.h"
#include "xr_controller.h"
#include "xr_hand_tracking.h"
#include "camera_renderer.h"
#include "protocol.h"

#include <cstring>
#include <string>
#include <vector>
#include <thread>
#include <atomic>

#define LOG_TAG "QuestBridge"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#define XR_CHECK(call)                                                        \
    do {                                                                       \
        XrResult _result = (call);                                             \
        if (XR_FAILED(_result)) {                                              \
            LOGE("%s failed: %d", #call, _result);                             \
            return _result;                                                    \
        }                                                                      \
    } while (0)

// ---------------------------------------------------------------------------
// Global state (single-instance VR app)
// ---------------------------------------------------------------------------
static struct {
    JavaVM* vm = nullptr;
    jobject activity = nullptr;

    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId system_id = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace app_space = XR_NULL_HANDLE;

    EGLDisplay egl_display = EGL_NO_DISPLAY;
    EGLContext egl_context = EGL_NO_CONTEXT;
    EGLSurface egl_surface = EGL_NO_SURFACE;
    EGLConfig egl_config = nullptr;

    XrSwapchain swapchain = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageOpenGLESKHR> swapchain_images;
    std::vector<uint32_t> framebuffers;
    int32_t swapchain_width = 0;
    int32_t swapchain_height = 0;

    XrSessionState session_state = XR_SESSION_STATE_UNKNOWN;
    bool session_running = false;
    std::atomic<bool> quit_requested{false};

    ws::Server ws_server;
} g;

// ---------------------------------------------------------------------------
// EGL setup
// ---------------------------------------------------------------------------
static bool init_egl() {
    g.egl_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(g.egl_display, nullptr, nullptr);

    EGLint config_attribs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24,
        EGL_NONE
    };
    EGLint num_configs = 0;
    eglChooseConfig(g.egl_display, config_attribs, &g.egl_config, 1, &num_configs);

    EGLint context_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    g.egl_context = eglCreateContext(g.egl_display, g.egl_config, EGL_NO_CONTEXT, context_attribs);

    EGLint surface_attribs[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
    g.egl_surface = eglCreatePbufferSurface(g.egl_display, g.egl_config, surface_attribs);

    eglMakeCurrent(g.egl_display, g.egl_surface, g.egl_surface, g.egl_context);
    LOGI("EGL initialized");
    return true;
}

static void shutdown_egl() {
    if (g.egl_display != EGL_NO_DISPLAY) {
        eglMakeCurrent(g.egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (g.egl_surface != EGL_NO_SURFACE) eglDestroySurface(g.egl_display, g.egl_surface);
        if (g.egl_context != EGL_NO_CONTEXT) eglDestroyContext(g.egl_display, g.egl_context);
        eglTerminate(g.egl_display);
    }
    g.egl_display = EGL_NO_DISPLAY;
    g.egl_context = EGL_NO_CONTEXT;
    g.egl_surface = EGL_NO_SURFACE;
}

// ---------------------------------------------------------------------------
// OpenXR instance
// ---------------------------------------------------------------------------
static XrResult create_instance() {
    // Initialize the OpenXR loader on Android
    PFN_xrInitializeLoaderKHR xrInitializeLoaderKHR = nullptr;
    xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR",
                          reinterpret_cast<PFN_xrVoidFunction*>(&xrInitializeLoaderKHR));
    if (xrInitializeLoaderKHR) {
        XrLoaderInitInfoAndroidKHR loader_info{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
        loader_info.applicationVM = g.vm;
        loader_info.applicationContext = g.activity;
        xrInitializeLoaderKHR(reinterpret_cast<XrLoaderInitInfoBaseHeaderKHR*>(&loader_info));
    }

    // Enumerate available extensions
    uint32_t ext_count = 0;
    xrEnumerateInstanceExtensionProperties(nullptr, 0, &ext_count, nullptr);
    std::vector<XrExtensionProperties> exts(ext_count, {XR_TYPE_EXTENSION_PROPERTIES});
    xrEnumerateInstanceExtensionProperties(nullptr, ext_count, &ext_count, exts.data());

    auto has_ext = [&](const char* name) {
        for (auto& e : exts)
            if (std::string(e.extensionName) == name) return true;
        return false;
    };

    std::vector<const char*> enabled_exts;
    // Required
    enabled_exts.push_back(XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME);
    enabled_exts.push_back(XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME);
    // Optional
    if (has_ext(XR_EXT_HAND_TRACKING_EXTENSION_NAME)) {
        enabled_exts.push_back(XR_EXT_HAND_TRACKING_EXTENSION_NAME);
        LOGI("Hand tracking extension available");
    }
    if (has_ext("XR_KHR_composition_layer_depth")) {
        enabled_exts.push_back("XR_KHR_composition_layer_depth");
    }
    if (has_ext("XR_FB_passthrough")) {
        enabled_exts.push_back("XR_FB_passthrough");
        LOGI("Passthrough extension available");
    }

    XrInstanceCreateInfoAndroidKHR android_info{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
    android_info.applicationVM = g.vm;
    android_info.applicationActivity = g.activity;

    XrInstanceCreateInfo create_info{XR_TYPE_INSTANCE_CREATE_INFO};
    create_info.next = &android_info;
    std::strncpy(create_info.applicationInfo.applicationName, "QuestROS2Bridge", XR_MAX_APPLICATION_NAME_SIZE);
    create_info.applicationInfo.applicationVersion = 1;
    create_info.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
    create_info.enabledExtensionCount = static_cast<uint32_t>(enabled_exts.size());
    create_info.enabledExtensionNames = enabled_exts.data();

    XR_CHECK(xrCreateInstance(&create_info, &g.instance));
    LOGI("OpenXR instance created");
    return XR_SUCCESS;
}

// ---------------------------------------------------------------------------
// OpenXR system + session
// ---------------------------------------------------------------------------
static XrResult create_session() {
    XrSystemGetInfo system_info{XR_TYPE_SYSTEM_GET_INFO};
    system_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XR_CHECK(xrGetSystem(g.instance, &system_info, &g.system_id));

    // Verify OpenGL ES graphics requirements
    PFN_xrGetOpenGLESGraphicsRequirementsKHR xrGetOpenGLESGraphicsRequirementsKHR = nullptr;
    xrGetInstanceProcAddr(g.instance, "xrGetOpenGLESGraphicsRequirementsKHR",
                          reinterpret_cast<PFN_xrVoidFunction*>(&xrGetOpenGLESGraphicsRequirementsKHR));
    XrGraphicsRequirementsOpenGLESKHR gfx_reqs{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};
    xrGetOpenGLESGraphicsRequirementsKHR(g.instance, g.system_id, &gfx_reqs);

    XrGraphicsBindingOpenGLESAndroidKHR gfx_binding{XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR};
    gfx_binding.display = g.egl_display;
    gfx_binding.config = g.egl_config;
    gfx_binding.context = g.egl_context;

    XrSessionCreateInfo session_info{XR_TYPE_SESSION_CREATE_INFO};
    session_info.next = &gfx_binding;
    session_info.systemId = g.system_id;

    XR_CHECK(xrCreateSession(g.instance, &session_info, &g.session));
    LOGI("OpenXR session created");

    // Create reference space (STAGE)
    XrReferenceSpaceCreateInfo space_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
    space_info.poseInReferenceSpace = {{0, 0, 0, 1}, {0, 0, 0}};
    XR_CHECK(xrCreateReferenceSpace(g.session, &space_info, &g.app_space));

    return XR_SUCCESS;
}

// ---------------------------------------------------------------------------
// Swapchain
// ---------------------------------------------------------------------------
static XrResult create_swapchain() {
    uint32_t view_count = 0;
    xrEnumerateViewConfigurationViews(g.instance, g.system_id,
        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &view_count, nullptr);
    std::vector<XrViewConfigurationView> views(view_count, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    xrEnumerateViewConfigurationViews(g.instance, g.system_id,
        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, view_count, &view_count, views.data());

    g.swapchain_width = static_cast<int32_t>(views[0].recommendedImageRectWidth);
    g.swapchain_height = static_cast<int32_t>(views[0].recommendedImageRectHeight);

    XrSwapchainCreateInfo swapchain_info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    swapchain_info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    swapchain_info.format = GL_SRGB8_ALPHA8;
    swapchain_info.sampleCount = 1;
    swapchain_info.width = g.swapchain_width;
    swapchain_info.height = g.swapchain_height;
    swapchain_info.faceCount = 1;
    swapchain_info.arraySize = 2;  // Stereo: one layer, two array slices
    swapchain_info.mipCount = 1;

    XR_CHECK(xrCreateSwapchain(g.session, &swapchain_info, &g.swapchain));

    uint32_t img_count = 0;
    xrEnumerateSwapchainImages(g.swapchain, 0, &img_count, nullptr);
    g.swapchain_images.resize(img_count, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
    xrEnumerateSwapchainImages(g.swapchain, img_count, &img_count,
        reinterpret_cast<XrSwapchainImageBaseHeader*>(g.swapchain_images.data()));

    // Create framebuffers for each swapchain image
    g.framebuffers.resize(img_count);
    glGenFramebuffers(static_cast<GLsizei>(img_count), g.framebuffers.data());

    LOGI("Swapchain created: %dx%d, %u images", g.swapchain_width, g.swapchain_height, img_count);
    return XR_SUCCESS;
}

// ---------------------------------------------------------------------------
// Event handling
// ---------------------------------------------------------------------------
static void handle_session_state_change(XrSessionState state) {
    g.session_state = state;
    LOGI("Session state → %d", state);

    if (state == XR_SESSION_STATE_READY) {
        XrSessionBeginInfo begin_info{XR_TYPE_SESSION_BEGIN_INFO};
        begin_info.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        xrBeginSession(g.session, &begin_info);
        g.session_running = true;
        LOGI("Session begun");
    } else if (state == XR_SESSION_STATE_STOPPING) {
        xrEndSession(g.session);
        g.session_running = false;
        LOGI("Session ended");
    } else if (state == XR_SESSION_STATE_EXITING || state == XR_SESSION_STATE_LOSS_PENDING) {
        g.quit_requested.store(true);
    }
}

static void poll_events() {
    XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(g.instance, &event) == XR_SUCCESS) {
        if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            auto* sse = reinterpret_cast<XrEventDataSessionStateChanged*>(&event);
            handle_session_state_change(sse->state);
        }
        event = {XR_TYPE_EVENT_DATA_BUFFER};
    }
}

// ---------------------------------------------------------------------------
// Render frame
// ---------------------------------------------------------------------------
static void render_frame() {
    XrFrameWaitInfo wait_info{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState frame_state{XR_TYPE_FRAME_STATE};
    xrWaitFrame(g.session, &wait_info, &frame_state);

    XrFrameBeginInfo begin_info{XR_TYPE_FRAME_BEGIN_INFO};
    xrBeginFrame(g.session, &begin_info);

    // Poll controllers and send state over WebSocket
    xr_controller::update(g.session, g.app_space, frame_state.predictedDisplayTime);
    if (g.ws_server.has_client()) {
        uint8_t ctrl_payload[protocol::CONTROLLER_STATE_SIZE];
        xr_controller::pack_payload(ctrl_payload);
        g.ws_server.send_controller_state(ctrl_payload);
    }

    // Update and send hand tracking data
    xr_hand_tracking::update(g.app_space, frame_state.predictedDisplayTime);
    if (g.ws_server.has_client()) {
        for (int hand = 0; hand < 2; ++hand) {
            if (xr_hand_tracking::is_active(hand)) {
                uint8_t hand_payload[protocol::HAND_STATE_SIZE];
                if (xr_hand_tracking::pack_payload(hand, hand_payload) > 0) {
                    g.ws_server.send_hand_state(hand_payload);
                }
            }
        }
    }

    // Apply haptic feedback from WS commands
    float h_left = g.ws_server.haptic_left.exchange(0.0f);
    float h_right = g.ws_server.haptic_right.exchange(0.0f);
    float h_left_dur = g.ws_server.haptic_left_duration.exchange(0.0f);
    float h_right_dur = g.ws_server.haptic_right_duration.exchange(0.0f);
    if (h_left > 0.0f) {
        xr_controller::apply_haptic(g.session, 0, h_left, h_left_dur);
    }
    if (h_right > 0.0f) {
        xr_controller::apply_haptic(g.session, 1, h_right, h_right_dur);
    }

    std::vector<XrCompositionLayerBaseHeader*> layers;

    XrCompositionLayerProjection projection_layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    std::vector<XrCompositionLayerProjectionView> projection_views;

    if (frame_state.shouldRender == XR_TRUE) {
        // Locate views
        XrViewLocateInfo view_info{XR_TYPE_VIEW_LOCATE_INFO};
        view_info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        view_info.displayTime = frame_state.predictedDisplayTime;
        view_info.space = g.app_space;

        XrViewState view_state{XR_TYPE_VIEW_STATE};
        uint32_t view_count = 0;
        std::vector<XrView> views(2, {XR_TYPE_VIEW});
        xrLocateViews(g.session, &view_info, &view_state, 2, &view_count, views.data());

        // Acquire + render + release swapchain
        XrSwapchainImageAcquireInfo acquire_info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        uint32_t img_index = 0;
        xrAcquireSwapchainImage(g.swapchain, &acquire_info, &img_index);

        XrSwapchainImageWaitInfo swapchain_wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        swapchain_wait.timeout = XR_INFINITE_DURATION;
        xrWaitSwapchainImage(g.swapchain, &swapchain_wait);

        // Bind framebuffer and clear (blank render)
        glBindFramebuffer(GL_FRAMEBUFFER, g.framebuffers[img_index]);
        for (uint32_t eye = 0; eye < 2; ++eye) {
            glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                      g.swapchain_images[img_index].image, 0, eye);
            glViewport(0, 0, g.swapchain_width, g.swapchain_height);
            glClearColor(0.0f, 0.0f, 0.1f, 1.0f);  // Dark blue
            glClear(GL_COLOR_BUFFER_BIT);
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrReleaseSwapchainImage(g.swapchain, &release_info);

        // Build projection views
        projection_views.resize(2);
        for (uint32_t eye = 0; eye < 2; ++eye) {
            projection_views[eye] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
            projection_views[eye].pose = views[eye].pose;
            projection_views[eye].fov = views[eye].fov;
            projection_views[eye].subImage.swapchain = g.swapchain;
            projection_views[eye].subImage.imageRect = {
                {0, 0},
                {g.swapchain_width, g.swapchain_height}
            };
            projection_views[eye].subImage.imageArrayIndex = eye;
        }

        projection_layer.space = g.app_space;
        projection_layer.viewCount = 2;
        projection_layer.views = projection_views.data();
        layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&projection_layer));
    }

    // Add camera quad layer if a frame has been received
    XrCompositionLayerQuad camera_quad;
    if (camera_renderer::has_frame() &&
        camera_renderer::get_quad_layer(camera_quad, g.app_space)) {
        layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&camera_quad));
    }

    XrFrameEndInfo end_info{XR_TYPE_FRAME_END_INFO};
    end_info.displayTime = frame_state.predictedDisplayTime;
    end_info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    end_info.layerCount = static_cast<uint32_t>(layers.size());
    end_info.layers = layers.data();
    xrEndFrame(g.session, &end_info);
}

// ---------------------------------------------------------------------------
// Main loop (runs on a background thread)
// ---------------------------------------------------------------------------
static void main_loop() {
    if (!init_egl()) {
        LOGE("EGL init failed");
        return;
    }

    if (XR_FAILED(create_instance())) {
        LOGE("Failed to create OpenXR instance");
        shutdown_egl();
        return;
    }

    if (XR_FAILED(create_session())) {
        LOGE("Failed to create OpenXR session");
        xrDestroyInstance(g.instance);
        shutdown_egl();
        return;
    }

    if (XR_FAILED(create_swapchain())) {
        LOGE("Failed to create swapchain");
        xrDestroySession(g.session);
        xrDestroyInstance(g.instance);
        shutdown_egl();
        return;
    }

    // Initialize camera renderer
    camera_renderer::init();

    // Initialize controller input
    if (XR_FAILED(xr_controller::init(g.instance, g.session))) {
        LOGW("Controller init failed — continuing without controller input");
    }

    // Initialize hand tracking (optional extension)
    if (!xr_hand_tracking::init(g.instance, g.session)) {
        LOGW("Hand tracking not available — continuing without hand tracking");
    }

    // Start WebSocket server
    g.ws_server.start(9090);

    while (!g.quit_requested.load()) {
        poll_events();
        if (g.session_running) {
            render_frame();
        }
    }

    // Cleanup
    g.ws_server.stop();
    xr_hand_tracking::destroy();
    xr_controller::destroy(g.instance);
    camera_renderer::destroy();

    if (!g.framebuffers.empty()) {
        glDeleteFramebuffers(static_cast<GLsizei>(g.framebuffers.size()), g.framebuffers.data());
        g.framebuffers.clear();
    }
    if (g.swapchain != XR_NULL_HANDLE) xrDestroySwapchain(g.swapchain);
    if (g.app_space != XR_NULL_HANDLE) xrDestroySpace(g.app_space);
    if (g.session != XR_NULL_HANDLE) xrDestroySession(g.session);
    if (g.instance != XR_NULL_HANDLE) xrDestroyInstance(g.instance);
    shutdown_egl();

    LOGI("Main loop exited");
}

// ---------------------------------------------------------------------------
// JNI entry points
// ---------------------------------------------------------------------------
static std::thread g_main_thread;

extern "C" {

JNIEXPORT void JNICALL
Java_com_quest_bridge_MainActivity_nativeOnCreate(JNIEnv* env, jobject thiz) {
    env->GetJavaVM(&g.vm);
    g.activity = env->NewGlobalRef(thiz);
    g.quit_requested.store(false);
    g_main_thread = std::thread(main_loop);
    LOGI("nativeOnCreate");
}

JNIEXPORT void JNICALL
Java_com_quest_bridge_MainActivity_nativeOnDestroy(JNIEnv* env, jobject thiz) {
    LOGI("nativeOnDestroy");
    g.quit_requested.store(true);
    if (g_main_thread.joinable()) {
        g_main_thread.join();
    }
    if (g.activity) {
        env->DeleteGlobalRef(g.activity);
        g.activity = nullptr;
    }
}

}  // extern "C"
