#include <webgpu/webgpu.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unordered_map>

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
    std::vector<Vertex> v;
    auto face = [&](glm::vec3 n,
                    glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d) {
        v.push_back({ a, n, {0,0} });
        v.push_back({ b, n, {1,0} });
        v.push_back({ c, n, {1,1} });
        v.push_back({ a, n, {0,0} });
        v.push_back({ c, n, {1,1} });
        v.push_back({ d, n, {0,1} });
    };
    face({0,0,1},  {-h,-h,h},  {h,-h,h},  {h,h,h},   {-h,h,h});
    face({0,0,-1}, {h,-h,-h},  {-h,-h,-h},{-h,h,-h}, {h,h,-h});
    face({1,0,0},  {h,-h,h},   {h,-h,-h}, {h,h,-h},  {h,h,h});
    face({-1,0,0}, {-h,-h,-h},{-h,-h,h},  {-h,h,h},  {-h,h,-h});
    face({0,1,0},  {-h,h,h},   {h,h,h},   {h,h,-h},  {-h,h,-h});
    face({0,-1,0}, {-h,-h,-h},{h,-h,-h},  {h,-h,h},  {-h,-h,h});
    return v;
}

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
    std::unordered_map<int, Mesh*>    meshes;
    std::unordered_map<int, Texture*> textures;
    int nextMeshId = 0;
    int nextTexId  = 0;
    ViewportId vpLeft = kInvalidViewport;
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
extern "C" {
    EMSCRIPTEN_KEEPALIVE void js_beginFrame() {
        g_state.renderer.beginFrame((uint32_t)g_state.width, (uint32_t)g_state.height);
    }

    // verts: flat float array (8 floats per vertex: px,py,pz, nx,ny,nz, u,v)
    // vertFloatCount: total floats (numVerts * 8)
    EMSCRIPTEN_KEEPALIVE int js_createMesh(float* verts, int vertFloatCount,
                                            uint32_t* indices, int indexCount) {
        int numVerts = vertFloatCount / 8;
        std::vector<Vertex> v(numVerts);
        memcpy(v.data(), verts, numVerts * sizeof(Vertex));
        std::vector<uint32_t> idx(indices, indices + indexCount);
        Mesh* m = g_state.renderer.createMesh(v, idx);
        int id = g_state.nextMeshId++;
        g_state.meshes[id] = m;
        return id;
    }

    EMSCRIPTEN_KEEPALIVE void js_destroyMesh(int id) {
        auto it = g_state.meshes.find(id);
        if (it == g_state.meshes.end()) return;
        g_state.renderer.destroyMesh(it->second);
        g_state.meshes.erase(it);
    }

    // rgba: Uint8Array of w*h*4 bytes
    EMSCRIPTEN_KEEPALIVE int js_createTexture(uint8_t* rgba, int w, int h) {
        Texture* t = g_state.renderer.createTexture(rgba, w, h);
        int id = g_state.nextTexId++;
        g_state.textures[id] = t;
        return id;
    }

    EMSCRIPTEN_KEEPALIVE void js_destroyTexture(int id) {
        auto it = g_state.textures.find(id);
        if (it == g_state.textures.end()) return;
        g_state.renderer.destroyTexture(it->second);
        g_state.textures.erase(it);
    }

    // Full TRS submit with mesh + texture handles
    EMSCRIPTEN_KEEPALIVE void js_submit(int meshId,
                                         float px, float py, float pz,
                                         float rx, float ry, float rz,
                                         float sx, float sy, float sz,
                                         int texId) {
        auto mit = g_state.meshes.find(meshId);
        if (mit == g_state.meshes.end()) return;
        auto tit = g_state.textures.find(texId);
        Material mat;
        mat.texture = (tit != g_state.textures.end()) ? tit->second : nullptr;

        glm::mat4 model = glm::translate(glm::mat4(1.f), glm::vec3(px, py, pz));
        if (rx != 0.f) model = glm::rotate(model, rx, glm::vec3(1, 0, 0));
        if (ry != 0.f) model = glm::rotate(model, ry, glm::vec3(0, 1, 0));
        if (rz != 0.f) model = glm::rotate(model, rz, glm::vec3(0, 0, 1));
        model = glm::scale(model, glm::vec3(sx, sy, sz));

        g_state.renderer.submit(*mit->second, model, mat);
    }

    EMSCRIPTEN_KEEPALIVE void js_endFrame() {
        g_state.renderer.endFrame(g_state.surface);
    }

    // Full camera: position, target, fov, lightDir, lightColor
    EMSCRIPTEN_KEEPALIVE void js_setCamera(
        float px, float py, float pz,
        float tx, float ty, float tz,
        float fov,
        float lx, float ly, float lz,
        float lr, float lg, float lb)
    {
        Camera cam;
        cam.position   = { px, py, pz };
        cam.target     = { tx, ty, tz };
        cam.fov        = fov;
        cam.lightDir   = glm::normalize(glm::vec3(lx, ly, lz));
        cam.lightColor = { lr, lg, lb };
        g_state.renderer.setCamera(g_state.vpLeft, cam);
    }
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

    EM_ASM(console.log("[WASM] main() called"));
    WGPUInstanceDescriptor idesc = {};
    g_state.instance = wgpuCreateInstance(&idesc);
    printf("[C] main: instance=%p\n", (void*)g_state.instance);

#ifdef __EMSCRIPTEN__
    WGPUEmscriptenSurfaceSourceCanvasHTMLSelector canvas_src = {};
    canvas_src.chain.sType = WGPUSType_EmscriptenSurfaceSourceCanvasHTMLSelector;
    canvas_src.selector = { "#canvas", WGPU_STRLEN };
    WGPUSurfaceDescriptor sdesc = {};
    sdesc.nextInChain = (WGPUChainedStruct*)&canvas_src;
    g_state.surface = wgpuInstanceCreateSurface(g_state.instance, &sdesc);
    printf("[C] main: surface=%p\n", (void*)g_state.surface);
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
    printf("[C] main: requesting adapter\n");
    WGPURequestAdapterCallbackInfo adapter_cb = {};
    adapter_cb.mode     = WGPUCallbackMode_AllowSpontaneous;
    adapter_cb.callback = on_adapter;
    wgpuInstanceRequestAdapter(g_state.instance, &aopts, adapter_cb);
    printf("[C] main: adapter request submitted\n");
#else
    wgpuInstanceRequestAdapter(g_state.instance, &aopts, on_adapter, NULL);
#endif

#ifndef __EMSCRIPTEN__
    while (g_state.vpLeft == kInvalidViewport) {
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
    EM_ASM(console.log("[WASM] on_adapter called"));
    printf("[C] on_adapter status=%d\n", (int)status);
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
    printf("[C] on_device status=%d\n", (int)status);
    if (status != WGPURequestDeviceStatus_Success) {
        fprintf(stderr, "Device failed: %.*s\n", (int)msg.length, msg.data);
        return;
    }
    g_state.device = device;
    g_state.queue  = wgpuDeviceGetQueue(device);
    configure_surface();
    renderer_init_scene();
    emscripten_run_script(
        // cwrap handles
        "var _bf  = Module.cwrap('js_beginFrame',   null, []);"
        "var _ef  = Module.cwrap('js_endFrame',     null, []);"
        "var _cm  = Module.cwrap('js_createMesh',   'number', ['number','number','number','number']);"
        "var _dm  = Module.cwrap('js_destroyMesh',  null,     ['number']);"
        "var _ct  = Module.cwrap('js_createTexture','number', ['number','number','number']);"
        "var _dt  = Module.cwrap('js_destroyTexture',null,    ['number']);"
        "var _sf  = Module.cwrap('js_submit',       null,     ['number','number','number','number','number','number','number','number','number','number','number']);"
        "var _sc  = Module.cwrap('js_setCamera',    null,     ['number','number','number','number','number','number','number','number','number','number','number','number','number']);"

        // helper: copy TypedArray into WASM heap, call fn(ptr,...), free
        "function _withPtr(arr, fn) {"
        "  var ptr = Module._malloc(arr.byteLength);"
        "  Module.HEAPU8.set(new Uint8Array(arr.buffer, arr.byteOffset, arr.byteLength), ptr);"
        "  var r = fn(ptr); Module._free(ptr); return r;"
        "}"

        "window.renderer = {"
        // frame
        "  beginFrame: _bf,"
        "  endFrame:   _ef,"

        // mesh: verts=Float32Array (8 floats/vertex: px py pz nx ny nz u v), indices=Uint32Array
        "  createMesh: function(verts, indices) {"
        "    var vPtr = Module._malloc(verts.byteLength);"
        "    Module.HEAPF32.set(verts, vPtr >> 2);"
        "    var iPtr = Module._malloc(indices.byteLength);"
        "    Module.HEAPU32.set(indices, iPtr >> 2);"
        "    var id = _cm(vPtr, verts.length, iPtr, indices.length);"
        "    Module._free(vPtr); Module._free(iPtr);"
        "    return id;"
        "  },"
        "  destroyMesh: _dm,"

        // texture: rgba=Uint8Array (w*h*4 bytes)
        "  createTexture: function(rgba, w, h) {"
        "    return _withPtr(rgba, function(ptr){ return _ct(ptr, w, h); });"
        "  },"
        "  destroyTexture: _dt,"

        // submit: pos/rot/size are {x,y,z} objects, rot in radians
        "  submit: function(meshId, pos, rot, size, texId) {"
        "    _sf(meshId, pos.x,pos.y,pos.z, rot.x,rot.y,rot.z, size.x,size.y,size.z, texId);"
        "  },"

        // camera: pos/target {x,y,z}, fov degrees, lightDir {x,y,z}, lightColor {r,g,b}
        "  setCamera: function(pos, target, fov, lightDir, lightColor) {"
        "    _sc(pos.x,pos.y,pos.z, target.x,target.y,target.z, fov,"
        "        lightDir.x,lightDir.y,lightDir.z, lightColor.r,lightColor.g,lightColor.b);"
        "  },"
        "};"
        "window.dispatchEvent(new CustomEvent('rendererReady'));"
    );
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

    Camera cam;
    cam.position   = { 3.f, 2.f, 3.f };
    cam.target     = { 0.f, 0.f, 0.f };
    cam.lightDir   = glm::normalize(glm::vec3(1.f, -2.f, -1.f));
    cam.lightColor = { 1.f, 0.95f, 0.85f };
    g_state.vpLeft = g_state.renderer.addViewport(0.0f, 0.0f, 1.0f, 1.0f, cam);

#ifndef __EMSCRIPTEN__
    // Native: pre-create default cube mesh (id=0) and checker texture (id=0)
    auto verts   = make_cube();
    auto indices = make_cube_indices(verts.size());
    g_state.meshes[g_state.nextMeshId++] = g_state.renderer.createMesh(verts, indices);

    auto checker = make_checker(256, 8);
    g_state.textures[g_state.nextTexId++] = g_state.renderer.createTexture(checker.data(), 256, 256);
#endif
}

// ─── per-frame (native only) ─────────────────────────────────────────────────

static void frame() {
    if (!g_state.surface_configured) return;
    auto mit = g_state.meshes.find(0);
    auto tit = g_state.textures.find(0);
    if (mit == g_state.meshes.end() || tit == g_state.textures.end()) return;

    float t = (float)g_time;

    Camera cam;
    cam.position   = { 3.5f * cosf(t * 0.4f), 2.0f, 3.5f * sinf(t * 0.4f) };
    cam.target     = { 0, 0, 0 };
    cam.lightDir   = glm::normalize(glm::vec3(1.f, -2.f, -1.f));
    cam.lightColor = { 1.f, 0.95f, 0.85f };
    g_state.renderer.setCamera(g_state.vpLeft, cam);

    g_state.renderer.beginFrame((uint32_t)g_state.width, (uint32_t)g_state.height);

    Material mat;
    mat.texture = tit->second;
    glm::mat4 model = glm::rotate(glm::mat4(1.f), t * 0.5f, glm::vec3(0, 1, 0));
    g_state.renderer.submit(*mit->second, model, mat);

    g_state.renderer.endFrame(g_state.surface);
}
