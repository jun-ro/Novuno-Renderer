#include <webgpu/webgpu.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#include <emscripten/html5.h>
#else
#include <webgpu/wgpu.h>
#include <GLFW/glfw3.h>
#if defined(_WIN32)
#  define GLFW_EXPOSE_NATIVE_WIN32
#elif defined(__APPLE__)
#  define GLFW_EXPOSE_NATIVE_COCOA
#else
#  if defined(RENDERER_USE_WAYLAND)
#    define GLFW_EXPOSE_NATIVE_WAYLAND
#  else
#    define GLFW_EXPOSE_NATIVE_X11
#  endif
#endif
#include <GLFW/glfw3native.h>
#endif

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "renderer.h"

// ─── demo geometry / texture ─────────────────────────────────────────────────

static std::vector<Vertex> make_cube() {
    const float h = 0.5f;
    // CCW winding (back-face culled), normals per face
    std::vector<Vertex> v;
    auto face = [&](glm::vec3 n,
                    glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d) {
        // two triangles: abc, acd — each vertex carries face normal and planar UVs
        v.push_back({ a, n, {0,0} });
        v.push_back({ b, n, {1,0} });
        v.push_back({ c, n, {1,1} });
        v.push_back({ a, n, {0,0} });
        v.push_back({ c, n, {1,1} });
        v.push_back({ d, n, {0,1} });
    };
    // +Z
    face({0,0,1}, {-h,-h,h},{h,-h,h},{h,h,h},{-h,h,h});
    // -Z  (CCW from -Z)
    face({0,0,-1},{h,-h,-h},{-h,-h,-h},{-h,h,-h},{h,h,-h});
    // +X
    face({1,0,0}, {h,-h,h},{h,-h,-h},{h,h,-h},{h,h,h});
    // -X
    face({-1,0,0},{-h,-h,-h},{-h,-h,h},{-h,h,h},{-h,h,-h});
    // +Y
    face({0,1,0}, {-h,h,h},{h,h,h},{h,h,-h},{-h,h,-h});
    // -Y
    face({0,-1,0},{-h,-h,-h},{h,-h,-h},{h,-h,h},{-h,-h,h});
    return v;
}

// Cube already inlines indices (no shared verts) so index buffer is sequential
static std::vector<uint32_t> make_cube_indices(size_t vertCount) {
    std::vector<uint32_t> idx(vertCount);
    for (uint32_t i = 0; i < (uint32_t)vertCount; ++i) idx[i] = i;
    return idx;
}

static std::vector<uint8_t> make_checker(int size, int tiles) {
    std::vector<uint8_t> data(size * size * 4);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            int tx = (x * tiles) / size;
            int ty = (y * tiles) / size;
            bool light = (tx + ty) % 2 == 0;
            uint8_t c = light ? 220 : 60;
            int i = (y * size + x) * 4;
            data[i+0] = c; data[i+1] = c; data[i+2] = c; data[i+3] = 255;
        }
    }
    return data;
}

// ─── global state ────────────────────────────────────────────────────────────

struct State {
    WGPUInstance instance;
    WGPUSurface  surface;
    WGPUAdapter  adapter;
    WGPUDevice   device;
    WGPUQueue    queue;
    bool surface_configured;
    int width;
    int height;
#ifndef __EMSCRIPTEN__
    GLFWwindow* window;
#endif

    Renderer   renderer;
    Mesh*      cubeMesh  = nullptr;
    Texture*   checkTex  = nullptr;
    ViewportId vpLeft    = kInvalidViewport;
    ViewportId vpRight   = kInvalidViewport;
};

static State g_state = {};
static double g_time = 0.0;

// ─── callbacks forward declarations ──────────────────────────────────────────

#ifdef __EMSCRIPTEN__
static void on_adapter(WGPURequestAdapterStatus, WGPUAdapter, WGPUStringView, void*, void*);
static void on_device(WGPURequestDeviceStatus, WGPUDevice, WGPUStringView, void*, void*);
#else
static void on_adapter(WGPURequestAdapterStatus, WGPUAdapter, const char*, void*);
static void on_device(WGPURequestDeviceStatus, WGPUDevice, const char*, void*);
#endif

static void configure_surface();
static void renderer_init_scene();
static void frame();

#ifdef __EMSCRIPTEN__
static EM_BOOL em_frame(double time_ms, void* /*ud*/) {
    g_time = time_ms * 0.001;
    frame();
    return EM_TRUE;
}
#endif

// ─── surface creation ────────────────────────────────────────────────────────

#ifndef __EMSCRIPTEN__
static WGPUSurface create_surface_native(WGPUInstance instance, GLFWwindow* window) {
    WGPUSurfaceDescriptor sdesc = {};
#if defined(_WIN32)
    WGPUSurfaceDescriptorFromWindowsHWND chain = {};
    chain.chain.sType = WGPUSType_SurfaceDescriptorFromWindowsHWND;
    chain.hinstance = GetModuleHandle(NULL);
    chain.hwnd      = glfwGetWin32Window(window);
    sdesc.nextInChain = (const WGPUChainedStruct*)&chain;
#elif defined(__APPLE__)
    extern void* create_metal_layer(void* ns_window);
    WGPUSurfaceDescriptorFromMetalLayer chain = {};
    chain.chain.sType = WGPUSType_SurfaceDescriptorFromMetalLayer;
    chain.layer       = create_metal_layer(glfwGetCocoaWindow(window));
    sdesc.nextInChain = (const WGPUChainedStruct*)&chain;
#else
#  if defined(RENDERER_USE_WAYLAND)
    WGPUSurfaceDescriptorFromWaylandSurface chain = {};
    chain.chain.sType = WGPUSType_SurfaceDescriptorFromWaylandSurface;
    chain.display = glfwGetWaylandDisplay();
    chain.surface = glfwGetWaylandWindow(window);
    sdesc.nextInChain = (const WGPUChainedStruct*)&chain;
#  else
    WGPUSurfaceDescriptorFromXlibWindow chain = {};
    chain.chain.sType = WGPUSType_SurfaceDescriptorFromXlibWindow;
    chain.display = glfwGetX11Display();
    chain.window  = (uint64_t)glfwGetX11Window(window);
    sdesc.nextInChain = (const WGPUChainedStruct*)&chain;
#  endif
#endif
    return wgpuInstanceCreateSurface(instance, &sdesc);
}

static void framebuffer_size_cb(GLFWwindow* /*win*/, int w, int h) {
    g_state.width  = w;
    g_state.height = h;
    if (g_state.surface_configured) configure_surface();
}
#endif

// ─── entry point ─────────────────────────────────────────────────────────────

int main() {
    g_state.width  = 1280;
    g_state.height = 600;

    WGPUInstanceDescriptor idesc = {};
    g_state.instance = wgpuCreateInstance(&idesc);

#ifdef __EMSCRIPTEN__
    WGPUEmscriptenSurfaceSourceCanvasHTMLSelector canvas_src = {};
    canvas_src.chain.sType = WGPUSType_EmscriptenSurfaceSourceCanvasHTMLSelector;
    canvas_src.selector = { "#canvas", WGPU_STRLEN };
    WGPUSurfaceDescriptor sdesc = {};
    sdesc.nextInChain = (WGPUChainedStruct*)&canvas_src;
    g_state.surface = wgpuInstanceCreateSurface(g_state.instance, &sdesc);
#else
    if (!glfwInit()) { fprintf(stderr, "glfwInit failed\n"); return 1; }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    g_state.window = glfwCreateWindow(g_state.width, g_state.height,
                                      "WebGPU Renderer", NULL, NULL);
    if (!g_state.window) { fprintf(stderr, "glfwCreateWindow failed\n"); return 1; }
    glfwSetFramebufferSizeCallback(g_state.window, framebuffer_size_cb);
    g_state.surface = create_surface_native(g_state.instance, g_state.window);
#endif

    WGPURequestAdapterOptions aopts = {};
    aopts.compatibleSurface = g_state.surface;
    aopts.powerPreference   = WGPUPowerPreference_HighPerformance;

#ifdef __EMSCRIPTEN__
    WGPURequestAdapterCallbackInfo adapter_cb = {};
    adapter_cb.mode     = WGPUCallbackMode_AllowSpontaneous;
    adapter_cb.callback = on_adapter;
    wgpuInstanceRequestAdapter(g_state.instance, &aopts, adapter_cb);
#else
    wgpuInstanceRequestAdapter(g_state.instance, &aopts, on_adapter, NULL);
#endif

#ifndef __EMSCRIPTEN__
    while (!g_state.cubeMesh) {
        wgpuInstanceProcessEvents(g_state.instance);
    }
    double t0 = glfwGetTime();
    while (!glfwWindowShouldClose(g_state.window)) {
        glfwPollEvents();
        g_time = glfwGetTime() - t0;
        frame();
    }
    g_state.renderer.shutdown();
    wgpuQueueRelease(g_state.queue);
    wgpuDeviceRelease(g_state.device);
    wgpuAdapterRelease(g_state.adapter);
    wgpuSurfaceRelease(g_state.surface);
    wgpuInstanceRelease(g_state.instance);
    glfwDestroyWindow(g_state.window);
    glfwTerminate();
#endif
    return 0;
}

// ─── adapter / device callbacks ──────────────────────────────────────────────

#ifdef __EMSCRIPTEN__
static void on_adapter(WGPURequestAdapterStatus status, WGPUAdapter adapter,
                        WGPUStringView msg, void* /*ud1*/, void* /*ud2*/) {
    if (status != WGPURequestAdapterStatus_Success) {
        fprintf(stderr, "Adapter failed: %.*s\n", (int)msg.length, msg.data);
        return;
    }
    g_state.adapter = adapter;
    WGPUDeviceDescriptor ddesc = {};
    WGPURequestDeviceCallbackInfo device_cb = {};
    device_cb.mode     = WGPUCallbackMode_AllowSpontaneous;
    device_cb.callback = on_device;
    wgpuAdapterRequestDevice(adapter, &ddesc, device_cb);
}

static void on_device(WGPURequestDeviceStatus status, WGPUDevice device,
                       WGPUStringView msg, void* /*ud1*/, void* /*ud2*/) {
    if (status != WGPURequestDeviceStatus_Success) {
        fprintf(stderr, "Device failed: %.*s\n", (int)msg.length, msg.data);
        return;
    }
    g_state.device = device;
    g_state.queue  = wgpuDeviceGetQueue(device);
    configure_surface();
    renderer_init_scene();
    emscripten_request_animation_frame_loop(em_frame, NULL);
}
#else
static void on_adapter(WGPURequestAdapterStatus status, WGPUAdapter adapter,
                        const char* msg, void* /*ud*/) {
    if (status != WGPURequestAdapterStatus_Success) {
        fprintf(stderr, "Adapter failed: %s\n", msg ? msg : "");
        return;
    }
    g_state.adapter = adapter;
    WGPUDeviceDescriptor ddesc  = {};
    WGPURequiredLimits   limits = {};
    ddesc.requiredLimits        = &limits;
    wgpuAdapterRequestDevice(adapter, &ddesc, on_device, NULL);
}

static void on_device(WGPURequestDeviceStatus status, WGPUDevice device,
                       const char* msg, void* /*ud*/) {
    if (status != WGPURequestDeviceStatus_Success) {
        fprintf(stderr, "Device failed: %s\n", msg ? msg : "");
        return;
    }
    g_state.device = device;
    g_state.queue  = wgpuDeviceGetQueue(device);
    configure_surface();
    renderer_init_scene();
}
#endif

// ─── configure surface ───────────────────────────────────────────────────────

static void configure_surface() {
    WGPUSurfaceConfiguration cfg = {};
    cfg.device      = g_state.device;
    cfg.format      = WGPUTextureFormat_BGRA8Unorm;
    cfg.usage       = WGPUTextureUsage_RenderAttachment;
    cfg.width       = (uint32_t)g_state.width;
    cfg.height      = (uint32_t)g_state.height;
    cfg.presentMode = WGPUPresentMode_Fifo;
    cfg.alphaMode   = WGPUCompositeAlphaMode_Auto;
    wgpuSurfaceConfigure(g_state.surface, &cfg);
    g_state.surface_configured = true;
}

// ─── scene init ──────────────────────────────────────────────────────────────

static void renderer_init_scene() {
    g_state.renderer.init(g_state.device, g_state.queue,
                          WGPUTextureFormat_BGRA8Unorm,
                          (uint32_t)g_state.width, (uint32_t)g_state.height);

    auto verts   = make_cube();
    auto indices = make_cube_indices(verts.size());
    g_state.cubeMesh = g_state.renderer.createMesh(verts, indices);

    auto checker = make_checker(256, 8);
    g_state.checkTex = g_state.renderer.createTexture(checker.data(), 256, 256);

    Camera camLeft;
    camLeft.position   = { 3.f, 2.f, 3.f };
    camLeft.target     = { 0.f, 0.f, 0.f };
    camLeft.lightDir   = glm::normalize(glm::vec3(1.f, -2.f, -1.f));
    camLeft.lightColor = { 1.f, 0.95f, 0.85f };

    Camera camRight;
    camRight.position   = { -3.f, 1.f, -3.f };
    camRight.target     = {  0.f, 0.f,  0.f };
    camRight.lightDir   = glm::normalize(glm::vec3(-1.f, -1.f, 1.f));
    camRight.lightColor = { 0.85f, 0.9f, 1.f };

    g_state.vpLeft  = g_state.renderer.addViewport(0.0f, 0.0f, 0.5f, 1.0f, camLeft);
    g_state.vpRight = g_state.renderer.addViewport(0.5f, 0.0f, 0.5f, 1.0f, camRight);
}

// ─── per-frame ───────────────────────────────────────────────────────────────

static void frame() {
    if (!g_state.surface_configured || !g_state.cubeMesh) return;

    float t = (float)g_time;

    // Orbit cameras
    Camera camLeft;
    camLeft.position   = { 3.5f * cosf(t * 0.4f), 2.0f, 3.5f * sinf(t * 0.4f) };
    camLeft.target     = { 0, 0, 0 };
    camLeft.lightDir   = glm::normalize(glm::vec3(1.f, -2.f, -1.f));
    camLeft.lightColor = { 1.f, 0.95f, 0.85f };

    Camera camRight;
    camRight.position   = { 4.5f * cosf(t * 0.3f + 2.1f), 1.5f, 4.5f * sinf(t * 0.3f + 2.1f) };
    camRight.target     = { 0, 0, 0 };
    camRight.lightDir   = glm::normalize(glm::vec3(-1.f, -1.f, 1.f));
    camRight.lightColor = { 0.85f, 0.9f, 1.f };

    g_state.renderer.setCamera(g_state.vpLeft,  camLeft);
    g_state.renderer.setCamera(g_state.vpRight, camRight);

    g_state.renderer.beginFrame((uint32_t)g_state.width, (uint32_t)g_state.height);

    Material mat;
    mat.texture = g_state.checkTex;
    glm::mat4 model = glm::rotate(glm::mat4(1.f), t * 0.5f, glm::vec3(0, 1, 0));
    g_state.renderer.submit(*g_state.cubeMesh, model, mat);

    g_state.renderer.endFrame(g_state.surface);
}
