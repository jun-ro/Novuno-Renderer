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

static const char* WGSL_SHADER = R"(
struct Uniforms {
    rot: mat2x2<f32>,
}
@group(0) @binding(0) var<uniform> u: Uniforms;

@vertex
fn vs_main(@builtin(vertex_index) vi: u32) -> @builtin(position) vec4<f32> {
    var pos = array<vec2<f32>, 3>(
        vec2<f32>( 0.0,  0.5),
        vec2<f32>(-0.5, -0.5),
        vec2<f32>( 0.5, -0.5)
    );
    let p = u.rot * pos[vi];
    return vec4<f32>(p, 0.0, 1.0);
}

@fragment
fn fs_main() -> @location(0) vec4<f32> {
    return vec4<f32>(0.2, 0.6, 1.0, 1.0);
}
)";

struct State {
    WGPUInstance instance;
    WGPUSurface  surface;
    WGPUAdapter  adapter;
    WGPUDevice   device;
    WGPUQueue    queue;
    WGPURenderPipeline pipeline;
    WGPUBindGroupLayout bgl;
    WGPUBindGroup bind_group;
    WGPUBuffer uniform_buf;
    bool surface_configured;
    int width;
    int height;
#ifndef __EMSCRIPTEN__
    GLFWwindow* window;
#endif
};

static State g_state = {};
static float g_angle = 0.0f;

#ifdef __EMSCRIPTEN__
static void on_adapter(WGPURequestAdapterStatus, WGPUAdapter, WGPUStringView, void*, void*);
static void on_device(WGPURequestDeviceStatus, WGPUDevice, WGPUStringView, void*, void*);
#else
static void on_adapter(WGPURequestAdapterStatus, WGPUAdapter, const char*, void*);
static void on_device(WGPURequestDeviceStatus, WGPUDevice, const char*, void*);
#endif

static void configure_surface();
static void create_pipeline();
static void frame();

#ifdef __EMSCRIPTEN__
static EM_BOOL em_frame(double time_ms, void* /*userdata*/) {
    g_angle = (float)(time_ms * 0.001);  // radians, 1 rev/~6.28 s
    frame();
    return EM_TRUE;
}
#endif

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
#endif

int main() {
    g_state.width  = 800;
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
                                      "WebGPU Triangle", NULL, NULL);
    if (!g_state.window) { fprintf(stderr, "glfwCreateWindow failed\n"); return 1; }
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
    while (!g_state.pipeline) {
        wgpuInstanceProcessEvents(g_state.instance);
    }
    while (!glfwWindowShouldClose(g_state.window)) {
        glfwPollEvents();
        g_angle += 0.01f;
        frame();
    }
    wgpuBindGroupRelease(g_state.bind_group);
    wgpuBindGroupLayoutRelease(g_state.bgl);
    wgpuBufferRelease(g_state.uniform_buf);
    wgpuRenderPipelineRelease(g_state.pipeline);
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

#ifdef __EMSCRIPTEN__
static void on_adapter(WGPURequestAdapterStatus status, WGPUAdapter adapter,
                       WGPUStringView msg, void* /*ud1*/, void* /*ud2*/) {
    if (status != WGPURequestAdapterStatus_Success) {
        fprintf(stderr, "Adapter request failed: %.*s\n", (int)msg.length, msg.data);
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
        fprintf(stderr, "Device request failed: %.*s\n", (int)msg.length, msg.data);
        return;
    }
    g_state.device = device;
    g_state.queue  = wgpuDeviceGetQueue(device);
    configure_surface();
    create_pipeline();
    emscripten_request_animation_frame_loop(em_frame, NULL);
}
#else
static void on_adapter(WGPURequestAdapterStatus status, WGPUAdapter adapter,
                        const char* msg, void* /*userdata*/) {
    if (status != WGPURequestAdapterStatus_Success) {
        fprintf(stderr, "Adapter request failed: %s\n", msg ? msg : "");
        return;
    }
    g_state.adapter = adapter;
    WGPUDeviceDescriptor ddesc  = {};
    WGPURequiredLimits   limits = {};
    ddesc.requiredLimits        = &limits;
    wgpuAdapterRequestDevice(adapter, &ddesc, on_device, NULL);
}

static void on_device(WGPURequestDeviceStatus status, WGPUDevice device,
                       const char* msg, void* /*userdata*/) {
    if (status != WGPURequestDeviceStatus_Success) {
        fprintf(stderr, "Device request failed: %s\n", msg ? msg : "");
        return;
    }
    g_state.device = device;
    g_state.queue  = wgpuDeviceGetQueue(device);
    configure_surface();
    create_pipeline();
}
#endif

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

static void create_pipeline() {
    // Bind group layout: one uniform buffer at binding 0, visible to vertex stage.
    WGPUBindGroupLayoutEntry bgl_entry = {};
    bgl_entry.binding               = 0;
    bgl_entry.visibility            = WGPUShaderStage_Vertex;
    bgl_entry.buffer.type           = WGPUBufferBindingType_Uniform;
    bgl_entry.buffer.minBindingSize = 16; // mat2x2<f32> = 16 bytes

    WGPUBindGroupLayoutDescriptor bgl_desc = {};
    bgl_desc.entryCount = 1;
    bgl_desc.entries    = &bgl_entry;
    g_state.bgl = wgpuDeviceCreateBindGroupLayout(g_state.device, &bgl_desc);

    // Uniform buffer: 16 bytes (mat2x2<f32>), written every frame.
    WGPUBufferDescriptor buf_desc = {};
    buf_desc.size  = 16;
    buf_desc.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
    g_state.uniform_buf = wgpuDeviceCreateBuffer(g_state.device, &buf_desc);

    // Bind group.
    WGPUBindGroupEntry bg_entry = {};
    bg_entry.binding = 0;
    bg_entry.buffer  = g_state.uniform_buf;
    bg_entry.size    = 16;

    WGPUBindGroupDescriptor bg_desc = {};
    bg_desc.layout     = g_state.bgl;
    bg_desc.entryCount = 1;
    bg_desc.entries    = &bg_entry;
    g_state.bind_group = wgpuDeviceCreateBindGroup(g_state.device, &bg_desc);

    // Pipeline layout.
    WGPUPipelineLayoutDescriptor pl_desc = {};
    pl_desc.bindGroupLayoutCount = 1;
    pl_desc.bindGroupLayouts     = &g_state.bgl;
    WGPUPipelineLayout layout = wgpuDeviceCreatePipelineLayout(g_state.device, &pl_desc);

    // Shader.
#ifdef __EMSCRIPTEN__
    WGPUShaderSourceWGSL wgsl = {};
    wgsl.chain.sType = WGPUSType_ShaderSourceWGSL;
    wgsl.code        = { WGSL_SHADER, WGPU_STRLEN };
    WGPUShaderModuleDescriptor sm_desc = {};
    sm_desc.nextInChain = (WGPUChainedStruct*)&wgsl;
#else
    WGPUShaderModuleWGSLDescriptor wgsl = {};
    wgsl.chain.sType = WGPUSType_ShaderModuleWGSLDescriptor;
    wgsl.code        = WGSL_SHADER;
    WGPUShaderModuleDescriptor sm_desc = {};
    sm_desc.nextInChain = (const WGPUChainedStruct*)&wgsl;
#endif
    WGPUShaderModule shader = wgpuDeviceCreateShaderModule(g_state.device, &sm_desc);

    WGPUBlendState blend = {};
    blend.color.operation = WGPUBlendOperation_Add;
    blend.color.srcFactor  = WGPUBlendFactor_One;
    blend.color.dstFactor  = WGPUBlendFactor_Zero;
    blend.alpha            = blend.color;

    WGPUColorTargetState color_target = {};
    color_target.format    = WGPUTextureFormat_BGRA8Unorm;
    color_target.writeMask = WGPUColorWriteMask_All;
    color_target.blend     = &blend;

    WGPUFragmentState frag = {};
    frag.module      = shader;
    frag.targetCount = 1;
    frag.targets     = &color_target;

    WGPURenderPipelineDescriptor rp_desc = {};
    rp_desc.layout        = layout;
    rp_desc.vertex.module = shader;
    rp_desc.primitive.topology  = WGPUPrimitiveTopology_TriangleList;
    rp_desc.primitive.cullMode  = WGPUCullMode_None;
    rp_desc.primitive.frontFace = WGPUFrontFace_CCW;
    rp_desc.multisample.count   = 1;
    rp_desc.multisample.mask    = 0xFFFFFFFF;
    rp_desc.fragment            = &frag;

#ifdef __EMSCRIPTEN__
    frag.entryPoint           = { "fs_main", WGPU_STRLEN };
    rp_desc.vertex.entryPoint = { "vs_main", WGPU_STRLEN };
#else
    frag.entryPoint           = "fs_main";
    rp_desc.vertex.entryPoint = "vs_main";
#endif

    g_state.pipeline = wgpuDeviceCreateRenderPipeline(g_state.device, &rp_desc);
    wgpuShaderModuleRelease(shader);
    wgpuPipelineLayoutRelease(layout);
}

static void frame() {
    if (!g_state.surface_configured) return;

    // Upload rotation matrix: col-major mat2x2 [ cos -sin | sin cos ].
    float c = cosf(g_angle);
    float s = sinf(g_angle);
    float rot[4] = { c, s, -s, c };
    wgpuQueueWriteBuffer(g_state.queue, g_state.uniform_buf, 0, rot, sizeof(rot));

    WGPUSurfaceTexture st = {};
    wgpuSurfaceGetCurrentTexture(g_state.surface, &st);
#ifdef __EMSCRIPTEN__
    if (st.status != WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal &&
        st.status != WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal) return;
#else
    if (st.status != WGPUSurfaceGetCurrentTextureStatus_Success) return;
#endif

    WGPUTextureView view = wgpuTextureCreateView(st.texture, NULL);

    WGPUCommandEncoderDescriptor enc_desc = {};
    WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(g_state.device, &enc_desc);

    WGPURenderPassColorAttachment color_att = {};
    color_att.view       = view;
    color_att.loadOp     = WGPULoadOp_Clear;
    color_att.storeOp    = WGPUStoreOp_Store;
    color_att.clearValue = { 0.1, 0.1, 0.15, 1.0 };
#ifdef __EMSCRIPTEN__
    color_att.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
#endif

    WGPURenderPassDescriptor rp_desc = {};
    rp_desc.colorAttachmentCount = 1;
    rp_desc.colorAttachments     = &color_att;

    WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(encoder, &rp_desc);
    wgpuRenderPassEncoderSetPipeline(pass, g_state.pipeline);
    wgpuRenderPassEncoderSetBindGroup(pass, 0, g_state.bind_group, 0, NULL);
    wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
    wgpuRenderPassEncoderEnd(pass);
    wgpuRenderPassEncoderRelease(pass);

    WGPUCommandBufferDescriptor cb_desc = {};
    WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(encoder, &cb_desc);
    wgpuCommandEncoderRelease(encoder);

    wgpuQueueSubmit(g_state.queue, 1, &cmd);
    wgpuCommandBufferRelease(cmd);
    wgpuTextureViewRelease(view);
    wgpuTextureRelease(st.texture);

#ifndef __EMSCRIPTEN__
    wgpuSurfacePresent(g_state.surface);
#endif
}
