#include "renderer.h"

#include <stb_image.h>

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cassert>

// ─── GPU uniform layout ───────────────────────────────────────────────────────
// Must match WGSL structs exactly.
struct CameraUniformsGPU {
    float viewProj[16];          // 64
    float lightDir[3]; float _p0; // 16
    float lightColor[3]; float _p1; // 16
};                               // 96 bytes

// mat3 packed as 3 × vec4 (WGSL mat3x3 has 48-byte pitch per column)
struct ObjectUniformsGPU {
    float model[16];    // 64
    float nc0[4];       // normalCol0, w unused
    float nc1[4];       // normalCol1, w unused
    float nc2[4];       // normalCol2, w unused
};                      // 112 bytes — fits in 256-byte stride

static_assert(sizeof(ObjectUniformsGPU) <= 256, "ObjectUniformsGPU too large");

// ─── WGSL shader ─────────────────────────────────────────────────────────────
static const char* WGSL = R"(
struct CameraUniforms {
    viewProj   : mat4x4<f32>,
    lightDir   : vec3<f32>,
    _pad0      : f32,
    lightColor : vec3<f32>,
    _pad1      : f32,
}
struct ObjectUniforms {
    model  : mat4x4<f32>,
    nc0    : vec4<f32>,
    nc1    : vec4<f32>,
    nc2    : vec4<f32>,
}

@group(0) @binding(0) var<uniform> camera : CameraUniforms;
@group(1) @binding(0) var albedo        : texture_2d<f32>;
@group(1) @binding(1) var albedoSampler : sampler;
@group(2) @binding(0) var<uniform> object : ObjectUniforms;

struct VsIn {
    @location(0) pos : vec3<f32>,
    @location(1) nor : vec3<f32>,
    @location(2) uv  : vec2<f32>,
}
struct VsOut {
    @builtin(position) clip : vec4<f32>,
    @location(0) wNor : vec3<f32>,
    @location(1) uv   : vec2<f32>,
}

@vertex
fn vs_main(in: VsIn) -> VsOut {
    let normalMat = mat3x3<f32>(object.nc0.xyz, object.nc1.xyz, object.nc2.xyz);
    var out: VsOut;
    out.clip = camera.viewProj * object.model * vec4<f32>(in.pos, 1.0);
    out.wNor = normalize(normalMat * in.nor);
    out.uv   = in.uv;
    return out;
}

@fragment
fn fs_main(in: VsOut) -> @location(0) vec4<f32> {
    let N       = normalize(in.wNor);
    let L       = normalize(-camera.lightDir);
    let diffuse = max(dot(N, L), 0.0);
    let light   = camera.lightColor * (diffuse + 0.1);
    let tex     = textureSample(albedo, albedoSampler, in.uv).rgb;
    return vec4<f32>(tex * light, 1.0);
}
)";

// ─── helpers ─────────────────────────────────────────────────────────────────

WGPUBuffer Renderer::makeBuffer(uint64_t size, WGPUFlags usage,
                                 const void* initData) {
    WGPUBufferDescriptor bd = {};
    bd.size  = size;
    bd.usage = usage;
    bd.mappedAtCreation = (initData != nullptr);
    WGPUBuffer buf = wgpuDeviceCreateBuffer(m_device, &bd);
    if (initData) {
        void* ptr = wgpuBufferGetMappedRange(buf, 0, size);
        memcpy(ptr, initData, (size_t)size);
        wgpuBufferUnmap(buf);
    }
    return buf;
}

static WGPUShaderModule make_shader(WGPUDevice dev, const char* src) {
#ifdef __EMSCRIPTEN__
    WGPUShaderSourceWGSL wgsl = {};
    wgsl.chain.sType = WGPUSType_ShaderSourceWGSL;
    wgsl.code        = { src, WGPU_STRLEN };
    WGPUShaderModuleDescriptor sd = {};
    sd.nextInChain = (WGPUChainedStruct*)&wgsl;
#else
    WGPUShaderModuleWGSLDescriptor wgsl = {};
    wgsl.chain.sType = WGPUSType_ShaderModuleWGSLDescriptor;
    wgsl.code        = src;
    WGPUShaderModuleDescriptor sd = {};
    sd.nextInChain = (const WGPUChainedStruct*)&wgsl;
#endif
    return wgpuDeviceCreateShaderModule(dev, &sd);
}

// ─── init / shutdown ─────────────────────────────────────────────────────────

void Renderer::init(WGPUDevice device, WGPUQueue queue,
                    WGPUTextureFormat surfaceFmt,
                    uint32_t width, uint32_t height) {
    m_device = device;
    m_queue  = queue;
    m_fmt    = surfaceFmt;
    m_surfW  = width;
    m_surfH  = height;

    // ── bind group layouts ──────────────────────────────────────────────────

    // group(0): camera uniform
    {
        WGPUBindGroupLayoutEntry e = {};
        e.binding               = 0;
        e.visibility            = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
        e.buffer.type           = WGPUBufferBindingType_Uniform;
        e.buffer.minBindingSize = sizeof(CameraUniformsGPU);
        WGPUBindGroupLayoutDescriptor d = {};
        d.entryCount = 1; d.entries = &e;
        m_camBGL = wgpuDeviceCreateBindGroupLayout(m_device, &d);
    }

    // group(1): texture + sampler
    {
        WGPUBindGroupLayoutEntry e[2] = {};
        e[0].binding               = 0;
        e[0].visibility            = WGPUShaderStage_Fragment;
        e[0].texture.sampleType    = WGPUTextureSampleType_Float;
        e[0].texture.viewDimension = WGPUTextureViewDimension_2D;
        e[0].texture.multisampled  = false;
        e[1].binding               = 1;
        e[1].visibility            = WGPUShaderStage_Fragment;
        e[1].sampler.type          = WGPUSamplerBindingType_Filtering;
        WGPUBindGroupLayoutDescriptor d = {};
        d.entryCount = 2; d.entries = e;
        m_matBGL = wgpuDeviceCreateBindGroupLayout(m_device, &d);
    }

    // group(2): object uniform (dynamic offset)
    {
        WGPUBindGroupLayoutEntry e = {};
        e.binding               = 0;
        e.visibility            = WGPUShaderStage_Vertex;
        e.buffer.type           = WGPUBufferBindingType_Uniform;
        e.buffer.hasDynamicOffset = true;
        e.buffer.minBindingSize = sizeof(ObjectUniformsGPU);
        WGPUBindGroupLayoutDescriptor d = {};
        d.entryCount = 1; d.entries = &e;
        m_objBGL = wgpuDeviceCreateBindGroupLayout(m_device, &d);
    }

    // ── object UBO (large dynamic buffer) ──────────────────────────────────
    m_objUBO = makeBuffer((uint64_t)kObjStride * kMaxObjects,
                          WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst);

    {
        WGPUBindGroupEntry e = {};
        e.binding = 0;
        e.buffer  = m_objUBO;
        e.offset  = 0;
        e.size    = sizeof(ObjectUniformsGPU);
        WGPUBindGroupDescriptor d = {};
        d.layout     = m_objBGL;
        d.entryCount = 1;
        d.entries    = &e;
        m_objBG = wgpuDeviceCreateBindGroup(m_device, &d);
    }

    // ── render pipeline ─────────────────────────────────────────────────────
    WGPUShaderModule shader = make_shader(m_device, WGSL);

    WGPUBindGroupLayout bgls[3] = { m_camBGL, m_matBGL, m_objBGL };
    WGPUPipelineLayoutDescriptor pld = {};
    pld.bindGroupLayoutCount = 3;
    pld.bindGroupLayouts     = bgls;
    WGPUPipelineLayout pipeLayout = wgpuDeviceCreatePipelineLayout(m_device, &pld);

    // vertex attributes: pos(0), nor(1), uv(2)
    WGPUVertexAttribute attrs[3] = {};
    attrs[0].shaderLocation = 0;
    attrs[0].format         = WGPUVertexFormat_Float32x3;
    attrs[0].offset         = offsetof(Vertex, position);
    attrs[1].shaderLocation = 1;
    attrs[1].format         = WGPUVertexFormat_Float32x3;
    attrs[1].offset         = offsetof(Vertex, normal);
    attrs[2].shaderLocation = 2;
    attrs[2].format         = WGPUVertexFormat_Float32x2;
    attrs[2].offset         = offsetof(Vertex, uv);

    WGPUVertexBufferLayout vbl = {};
    vbl.arrayStride    = sizeof(Vertex);
    vbl.stepMode       = WGPUVertexStepMode_Vertex;
    vbl.attributeCount = 3;
    vbl.attributes     = attrs;

    WGPUBlendState blend = {};
    blend.color.operation = WGPUBlendOperation_Add;
    blend.color.srcFactor = WGPUBlendFactor_One;
    blend.color.dstFactor = WGPUBlendFactor_Zero;
    blend.alpha           = blend.color;

    WGPUColorTargetState ct = {};
    ct.format    = m_fmt;
    ct.writeMask = WGPUColorWriteMask_All;
    ct.blend     = &blend;

    WGPUFragmentState frag = {};
    frag.module      = shader;
    frag.targetCount = 1;
    frag.targets     = &ct;

    WGPUDepthStencilState ds = {};
    ds.format            = WGPUTextureFormat_Depth24Plus;
#ifdef __EMSCRIPTEN__
    ds.depthWriteEnabled = WGPUOptionalBool_True;
#else
    ds.depthWriteEnabled = 1; // WGPUBool
#endif
    ds.depthCompare      = WGPUCompareFunction_Less;
    ds.stencilFront.compare  = WGPUCompareFunction_Always;
    ds.stencilBack.compare   = WGPUCompareFunction_Always;
    ds.stencilFront.failOp   = WGPUStencilOperation_Keep;
    ds.stencilFront.depthFailOp = WGPUStencilOperation_Keep;
    ds.stencilFront.passOp   = WGPUStencilOperation_Keep;
    ds.stencilBack.failOp    = WGPUStencilOperation_Keep;
    ds.stencilBack.depthFailOp = WGPUStencilOperation_Keep;
    ds.stencilBack.passOp    = WGPUStencilOperation_Keep;

    WGPURenderPipelineDescriptor rpd = {};
    rpd.layout               = pipeLayout;
    rpd.vertex.module         = shader;
    rpd.vertex.bufferCount    = 1;
    rpd.vertex.buffers        = &vbl;
    rpd.primitive.topology   = WGPUPrimitiveTopology_TriangleList;
    rpd.primitive.cullMode   = WGPUCullMode_Back;
    rpd.primitive.frontFace  = WGPUFrontFace_CCW;
    rpd.primitive.stripIndexFormat = WGPUIndexFormat_Undefined;
    rpd.depthStencil         = &ds;
    rpd.multisample.count    = 1;
    rpd.multisample.mask     = 0xFFFFFFFF;
    rpd.fragment             = &frag;

#ifdef __EMSCRIPTEN__
    frag.entryPoint           = { "fs_main", WGPU_STRLEN };
    rpd.vertex.entryPoint     = { "vs_main", WGPU_STRLEN };
#else
    frag.entryPoint           = "fs_main";
    rpd.vertex.entryPoint     = "vs_main";
#endif

    m_pipeline = wgpuDeviceCreateRenderPipeline(m_device, &rpd);
    wgpuShaderModuleRelease(shader);
    wgpuPipelineLayoutRelease(pipeLayout);

    // ── white fallback texture ──────────────────────────────────────────────
    static const uint8_t white[4] = { 255, 255, 255, 255 };
    m_whiteTex = makeTexture(white, 1, 1);
}

void Renderer::shutdown() {
    for (auto& vp : m_viewports) {
        if (!vp.active) continue;
        freeDepth(vp);
        if (vp.cameraUBO) wgpuBufferRelease(vp.cameraUBO);
        if (vp.camBG)     wgpuBindGroupRelease(vp.camBG);
    }
    m_viewports.clear();

    if (m_whiteTex) {
        wgpuBindGroupRelease(m_whiteTex->matBindGroup);
        wgpuSamplerRelease(m_whiteTex->sampler);
        wgpuTextureViewRelease(m_whiteTex->view);
        wgpuTextureRelease(m_whiteTex->gpuTexture);
        delete m_whiteTex;
        m_whiteTex = nullptr;
    }

    if (m_objBG)    wgpuBindGroupRelease(m_objBG);
    if (m_objUBO)   wgpuBufferRelease(m_objUBO);
    if (m_pipeline) wgpuRenderPipelineRelease(m_pipeline);
    if (m_camBGL)   wgpuBindGroupLayoutRelease(m_camBGL);
    if (m_matBGL)   wgpuBindGroupLayoutRelease(m_matBGL);
    if (m_objBGL)   wgpuBindGroupLayoutRelease(m_objBGL);
}

// ─── mesh ─────────────────────────────────────────────────────────────────────

Mesh* Renderer::createMesh(const std::vector<Vertex>& verts,
                            const std::vector<uint32_t>& indices) {
    Mesh* m = new Mesh();
    m->indexCount   = (uint32_t)indices.size();
    m->vertexBuffer = makeBuffer(verts.size() * sizeof(Vertex),
                                 WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst,
                                 verts.data());
    m->indexBuffer  = makeBuffer(indices.size() * sizeof(uint32_t),
                                 WGPUBufferUsage_Index | WGPUBufferUsage_CopyDst,
                                 indices.data());
    return m;
}

void Renderer::destroyMesh(Mesh* mesh) {
    if (!mesh) return;
    if (mesh->vertexBuffer) wgpuBufferRelease(mesh->vertexBuffer);
    if (mesh->indexBuffer)  wgpuBufferRelease(mesh->indexBuffer);
    delete mesh;
}

// ─── texture ─────────────────────────────────────────────────────────────────

Texture* Renderer::makeTexture(const uint8_t* rgba, int w, int h) {
    Texture* t = new Texture();

    WGPUTextureDescriptor td = {};
    td.usage          = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
    td.dimension      = WGPUTextureDimension_2D;
    td.size           = { (uint32_t)w, (uint32_t)h, 1 };
    td.format         = WGPUTextureFormat_RGBA8Unorm;
    td.mipLevelCount  = 1;
    td.sampleCount    = 1;
    t->gpuTexture = wgpuDeviceCreateTexture(m_device, &td);

#ifdef __EMSCRIPTEN__
    WGPUTexelCopyTextureInfo dst = {};
    dst.texture  = t->gpuTexture;
    dst.mipLevel = 0;
    dst.origin   = { 0, 0, 0 };
    dst.aspect   = WGPUTextureAspect_All;

    WGPUTexelCopyBufferLayout layout = {};
    layout.offset       = 0;
    layout.bytesPerRow  = (uint32_t)(w * 4);
    layout.rowsPerImage = (uint32_t)h;
#else
    WGPUImageCopyTexture dst = {};
    dst.texture  = t->gpuTexture;
    dst.mipLevel = 0;
    dst.origin   = { 0, 0, 0 };
    dst.aspect   = WGPUTextureAspect_All;

    WGPUTextureDataLayout layout = {};
    layout.offset       = 0;
    layout.bytesPerRow  = (uint32_t)(w * 4);
    layout.rowsPerImage = (uint32_t)h;
#endif

    WGPUExtent3D extent = { (uint32_t)w, (uint32_t)h, 1 };
    // wgpuQueueWriteTexture uses an internal staging buffer
    wgpuQueueWriteTexture(m_queue, &dst, rgba, (size_t)(w * h * 4), &layout, &extent);

    t->view = wgpuTextureCreateView(t->gpuTexture, nullptr);

    WGPUSamplerDescriptor sd = {};
    sd.addressModeU  = WGPUAddressMode_Repeat;
    sd.addressModeV  = WGPUAddressMode_Repeat;
    sd.addressModeW  = WGPUAddressMode_Repeat;
    sd.magFilter     = WGPUFilterMode_Linear;
    sd.minFilter     = WGPUFilterMode_Linear;
    sd.mipmapFilter  = WGPUMipmapFilterMode_Linear;
    sd.maxAnisotropy = 1;
    t->sampler = wgpuDeviceCreateSampler(m_device, &sd);

    WGPUBindGroupEntry e[2] = {};
    e[0].binding     = 0;
    e[0].textureView = t->view;
    e[1].binding     = 1;
    e[1].sampler     = t->sampler;

    WGPUBindGroupDescriptor bgd = {};
    bgd.layout     = m_matBGL;
    bgd.entryCount = 2;
    bgd.entries    = e;
    t->matBindGroup = wgpuDeviceCreateBindGroup(m_device, &bgd);

    return t;
}

Texture* Renderer::loadTexture(const char* path) {
    int w, h, ch;
    uint8_t* data = stbi_load(path, &w, &h, &ch, 4);
    if (!data) {
        fprintf(stderr, "stbi_load failed: %s\n", path);
        return m_whiteTex;
    }
    Texture* t = makeTexture(data, w, h);
    stbi_image_free(data);
    return t;
}

Texture* Renderer::createTexture(const uint8_t* rgba, int w, int h) {
    return makeTexture(rgba, w, h);
}

void Renderer::destroyTexture(Texture* tex) {
    if (!tex || tex == m_whiteTex) return;
    wgpuBindGroupRelease(tex->matBindGroup);
    wgpuSamplerRelease(tex->sampler);
    wgpuTextureViewRelease(tex->view);
    wgpuTextureRelease(tex->gpuTexture);
    delete tex;
}

// ─── depth texture ───────────────────────────────────────────────────────────

void Renderer::ensureDepth(ViewportData& vp, uint32_t w, uint32_t h) {
    if (vp.depthTex && vp.pixW == w && vp.pixH == h) return;
    freeDepth(vp);
    vp.pixW = w; vp.pixH = h;

    WGPUTextureDescriptor td = {};
    td.usage         = WGPUTextureUsage_RenderAttachment;
    td.dimension     = WGPUTextureDimension_2D;
    td.size          = { w, h, 1 };
    td.format        = WGPUTextureFormat_Depth24Plus;
    td.mipLevelCount = 1;
    td.sampleCount   = 1;
    vp.depthTex  = wgpuDeviceCreateTexture(m_device, &td);

    WGPUTextureViewDescriptor vd = {};
    vd.format          = WGPUTextureFormat_Depth24Plus;
    vd.dimension       = WGPUTextureViewDimension_2D;
    vd.mipLevelCount   = 1;
    vd.arrayLayerCount = 1;
    vd.aspect          = WGPUTextureAspect_DepthOnly;
    vp.depthView = wgpuTextureCreateView(vp.depthTex, &vd);
}

void Renderer::freeDepth(ViewportData& vp) {
    if (vp.depthView) { wgpuTextureViewRelease(vp.depthView); vp.depthView = nullptr; }
    if (vp.depthTex)  { wgpuTextureRelease(vp.depthTex);      vp.depthTex  = nullptr; }
    vp.pixW = vp.pixH = 0;
}

// ─── viewport management ─────────────────────────────────────────────────────

WGPUBindGroup Renderer::makeCamBG(WGPUBuffer ubo) {
    WGPUBindGroupEntry e = {};
    e.binding = 0;
    e.buffer  = ubo;
    e.offset  = 0;
    e.size    = sizeof(CameraUniformsGPU);
    WGPUBindGroupDescriptor d = {};
    d.layout     = m_camBGL;
    d.entryCount = 1;
    d.entries    = &e;
    return wgpuDeviceCreateBindGroup(m_device, &d);
}

ViewportId Renderer::addViewport(float x, float y, float w, float h,
                                  const Camera& cam) {
    ViewportData vp;
    vp.x      = x; vp.y = y; vp.w = w; vp.h = h;
    vp.camera = cam;
    vp.active = true;
    vp.id     = m_nextId++;

    vp.cameraUBO = makeBuffer(sizeof(CameraUniformsGPU),
                              WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst);
    vp.camBG = makeCamBG(vp.cameraUBO);

    m_viewports.push_back(std::move(vp));
    return m_viewports.back().id;
}

void Renderer::removeViewport(ViewportId id) {
    for (auto& vp : m_viewports) {
        if (vp.id != id || !vp.active) continue;
        freeDepth(vp);
        if (vp.cameraUBO) wgpuBufferRelease(vp.cameraUBO);
        if (vp.camBG)     wgpuBindGroupRelease(vp.camBG);
        vp.active = false;
        break;
    }
}

void Renderer::setCamera(ViewportId id, const Camera& cam) {
    for (auto& vp : m_viewports)
        if (vp.id == id && vp.active) { vp.camera = cam; break; }
}

// ─── frame ───────────────────────────────────────────────────────────────────

void Renderer::beginFrame(uint32_t surfW, uint32_t surfH) {
    m_surfW = surfW;
    m_surfH = surfH;
    for (auto& vp : m_viewports)
        if (vp.active) vp.drawList.clear();
}

void Renderer::submit(const Mesh& mesh, const glm::mat4& transform,
                      const Material& mat) {
    for (auto& vp : m_viewports)
        if (vp.active) vp.drawList.push_back({ &mesh, transform, &mat });
}

void Renderer::submitTo(ViewportId id, const Mesh& mesh, const glm::mat4& transform,
                        const Material& mat) {
    for (auto& vp : m_viewports)
        if (vp.id == id && vp.active) { vp.drawList.push_back({ &mesh, transform, &mat }); break; }
}

void Renderer::endFrame(WGPUSurface surface) {
    // ── gather all draw calls across all viewports ──────────────────────────
    // Assign each draw call a slot in the object UBO (256-byte stride).
    // Slot order: viewport0_draw0, viewport0_draw1, ..., viewport1_draw0, ...
    struct SlottedDraw {
        const DrawCall* dc;
        uint32_t        slot;
    };

    std::vector<ObjectUniformsGPU> objData;
    objData.reserve(256);

    // Pre-assign slots and build CPU buffer
    std::vector<std::vector<SlottedDraw>> vpSlots(m_viewports.size());
    for (size_t vi = 0; vi < m_viewports.size(); ++vi) {
        auto& vp = m_viewports[vi];
        if (!vp.active) continue;
        // Sort by material for batching
        std::stable_sort(vp.drawList.begin(), vp.drawList.end(),
            [](const DrawCall& a, const DrawCall& b) {
                return a.material < b.material;
            });
        for (auto& dc : vp.drawList) {
            uint32_t slot = (uint32_t)objData.size();
            if (slot >= kMaxObjects) break;

            glm::mat4 model = dc.transform;
            glm::mat3 normalMat = glm::transpose(glm::inverse(glm::mat3(model)));

            ObjectUniformsGPU o = {};
            memcpy(o.model, glm::value_ptr(model), 64);
            o.nc0[0] = normalMat[0][0]; o.nc0[1] = normalMat[0][1]; o.nc0[2] = normalMat[0][2];
            o.nc1[0] = normalMat[1][0]; o.nc1[1] = normalMat[1][1]; o.nc1[2] = normalMat[1][2];
            o.nc2[0] = normalMat[2][0]; o.nc2[1] = normalMat[2][1]; o.nc2[2] = normalMat[2][2];

            objData.push_back(o);
            vpSlots[vi].push_back({ &dc, slot });
        }
    }

    // Upload all object uniforms in one write (data must be padded to stride)
    if (!objData.empty()) {
        // Build stride-padded buffer
        size_t uploadSize = objData.size() * kObjStride;
        std::vector<uint8_t> padded(uploadSize, 0);
        for (size_t i = 0; i < objData.size(); ++i)
            memcpy(padded.data() + i * kObjStride, &objData[i], sizeof(ObjectUniformsGPU));
        wgpuQueueWriteBuffer(m_queue, m_objUBO, 0, padded.data(), uploadSize);
    }

    // ── get surface texture ─────────────────────────────────────────────────
    WGPUSurfaceTexture st = {};
    wgpuSurfaceGetCurrentTexture(surface, &st);
#ifdef __EMSCRIPTEN__
    if (st.status != WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal &&
        st.status != WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal) return;
#else
    if (st.status != WGPUSurfaceGetCurrentTextureStatus_Success) return;
#endif
    WGPUTextureView surfaceView = wgpuTextureCreateView(st.texture, nullptr);

    WGPUCommandEncoderDescriptor ced = {};
    WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(m_device, &ced);

    // ── one render pass per active viewport ─────────────────────────────────
    bool firstVP = true;
    size_t vi = 0;
    for (auto& vp : m_viewports) {
        if (!vp.active) { ++vi; continue; }

        // Pixel rect for this viewport
        uint32_t px = (uint32_t)(vp.x * m_surfW);
        uint32_t py = (uint32_t)(vp.y * m_surfH);
        uint32_t pw = (uint32_t)(vp.w * m_surfW);
        uint32_t ph = (uint32_t)(vp.h * m_surfH);
        if (pw == 0 || ph == 0) { ++vi; continue; }

        ensureDepth(vp, pw, ph);

        // Update camera UBO
        CameraUniformsGPU cu = {};
        float aspect = (float)pw / (float)ph;
        glm::mat4 proj = glm::perspective(glm::radians(vp.camera.fov),
                                          aspect,
                                          vp.camera.nearPlane,
                                          vp.camera.farPlane);
        glm::mat4 view = glm::lookAt(vp.camera.position, vp.camera.target, vp.camera.up);
        glm::mat4 vp_mat = proj * view;
        memcpy(cu.viewProj, glm::value_ptr(vp_mat), 64);
        glm::vec3 ld = glm::normalize(vp.camera.lightDir);
        cu.lightDir[0] = ld.x; cu.lightDir[1] = ld.y; cu.lightDir[2] = ld.z;
        cu.lightColor[0] = vp.camera.lightColor.r;
        cu.lightColor[1] = vp.camera.lightColor.g;
        cu.lightColor[2] = vp.camera.lightColor.b;
        wgpuQueueWriteBuffer(m_queue, vp.cameraUBO, 0, &cu, sizeof(cu));

        // Color attachment: first viewport clears, subsequent load (preserve other viewports)
        WGPURenderPassColorAttachment ca = {};
        ca.view    = surfaceView;
        ca.loadOp  = firstVP ? WGPULoadOp_Clear : WGPULoadOp_Load;
        ca.storeOp = WGPUStoreOp_Store;
        ca.clearValue = { 0.08, 0.08, 0.10, 1.0 };
#ifdef __EMSCRIPTEN__
        ca.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
#endif

        WGPURenderPassDepthStencilAttachment da = {};
        da.view              = vp.depthView;
        da.depthLoadOp       = WGPULoadOp_Clear;
        da.depthStoreOp      = WGPUStoreOp_Store;
        da.depthClearValue   = 1.0f;
        da.stencilLoadOp     = WGPULoadOp_Undefined;
        da.stencilStoreOp    = WGPUStoreOp_Undefined;

        WGPURenderPassDescriptor rpd = {};
        rpd.colorAttachmentCount   = 1;
        rpd.colorAttachments       = &ca;
        rpd.depthStencilAttachment = &da;

        WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(encoder, &rpd);

        wgpuRenderPassEncoderSetViewport(pass,
            (float)px, (float)py, (float)pw, (float)ph, 0.0f, 1.0f);
        wgpuRenderPassEncoderSetScissorRect(pass, px, py, pw, ph);

        wgpuRenderPassEncoderSetPipeline(pass, m_pipeline);
        wgpuRenderPassEncoderSetBindGroup(pass, 0, vp.camBG, 0, nullptr);

        auto& slots = vpSlots[vi];
        const Material* lastMat = nullptr;
        for (auto& sd : slots) {
            const DrawCall& dc = *sd.dc;

            // Bind material only if changed
            Texture* tex = (dc.material && dc.material->texture)
                           ? dc.material->texture : m_whiteTex;
            if (dc.material != lastMat) {
                wgpuRenderPassEncoderSetBindGroup(pass, 1, tex->matBindGroup, 0, nullptr);
                lastMat = dc.material;
            }

            // Dynamic object offset
            uint32_t dynOffset = sd.slot * kObjStride;
            wgpuRenderPassEncoderSetBindGroup(pass, 2, m_objBG, 1, &dynOffset);

            wgpuRenderPassEncoderSetVertexBuffer(pass, 0, dc.mesh->vertexBuffer,
                                                 0, WGPU_WHOLE_SIZE);
            wgpuRenderPassEncoderSetIndexBuffer(pass, dc.mesh->indexBuffer,
                                               WGPUIndexFormat_Uint32, 0, WGPU_WHOLE_SIZE);
            wgpuRenderPassEncoderDrawIndexed(pass, dc.mesh->indexCount, 1, 0, 0, 0);
        }

        wgpuRenderPassEncoderEnd(pass);
        wgpuRenderPassEncoderRelease(pass);

        firstVP = false;
        ++vi;
    }

    WGPUCommandBufferDescriptor cbd = {};
    WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(encoder, &cbd);
    wgpuCommandEncoderRelease(encoder);

    wgpuQueueSubmit(m_queue, 1, &cmd);
    wgpuCommandBufferRelease(cmd);
    wgpuTextureViewRelease(surfaceView);
    wgpuTextureRelease(st.texture);

#ifndef __EMSCRIPTEN__
    wgpuSurfacePresent(surface);
#endif
}
