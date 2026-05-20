#pragma once
#include <webgpu/webgpu.h>
#ifdef __EMSCRIPTEN__
#  include <emscripten/emscripten.h>
#  include <emscripten/html5.h>
#else
#  include <webgpu/wgpu.h>
#endif

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

using ViewportId = uint32_t;
static constexpr ViewportId kInvalidViewport = UINT32_MAX;

struct Vertex {
    glm::vec3 position;
    glm::vec3 normal;
    glm::vec2 uv;
};

struct Mesh {
    WGPUBuffer vertexBuffer = nullptr;
    WGPUBuffer indexBuffer  = nullptr;
    uint32_t   indexCount   = 0;
};

struct Texture {
    WGPUTexture     gpuTexture  = nullptr;
    WGPUTextureView view        = nullptr;
    WGPUSampler     sampler     = nullptr;
    WGPUBindGroup   matBindGroup = nullptr;
};

struct Material {
    Texture* texture = nullptr;
};

struct Camera {
    glm::vec3 position   = {0.f, 0.f, 3.f};
    glm::vec3 target     = {0.f, 0.f, 0.f};
    glm::vec3 up         = {0.f, 1.f, 0.f};
    float     fov        = 45.f;
    float     nearPlane  = 0.1f;
    float     farPlane   = 100.f;
    glm::vec3 lightDir   = glm::vec3(1.f, -1.f, -1.f);
    glm::vec3 lightColor = {1.f, 1.f, 1.f};
};

class Renderer {
public:
    void init(WGPUDevice device, WGPUQueue queue, WGPUTextureFormat surfaceFmt,
              uint32_t width, uint32_t height);
    void shutdown();

    Mesh*    createMesh(const std::vector<Vertex>& verts,
                        const std::vector<uint32_t>& indices);
    void     destroyMesh(Mesh* mesh);

    Texture* loadTexture(const char* path);
    Texture* createTexture(const uint8_t* rgba, int w, int h);
    void     destroyTexture(Texture* tex);

    ViewportId addViewport(float x, float y, float w, float h, const Camera& cam);
    void       removeViewport(ViewportId id);
    void       setCamera(ViewportId id, const Camera& cam);

    void beginFrame(uint32_t surfW, uint32_t surfH);
    void submit(const Mesh& mesh, const glm::mat4& transform, const Material& mat);
    void submitTo(ViewportId id, const Mesh& mesh, const glm::mat4& transform,
                  const Material& mat);
    void endFrame(WGPUSurface surface);

private:
    struct DrawCall {
        const Mesh*     mesh;
        glm::mat4       transform;
        const Material* material;
    };

    struct ViewportData {
        float      x = 0, y = 0, w = 1, h = 1;
        Camera     camera;
        uint32_t   pixW = 0, pixH = 0;
        WGPUTexture     depthTex  = nullptr;
        WGPUTextureView depthView = nullptr;
        WGPUBuffer      cameraUBO = nullptr;
        WGPUBindGroup   camBG     = nullptr;
        std::vector<DrawCall> drawList;
        bool active = false;
        ViewportId id = kInvalidViewport;
    };

    void         ensureDepth(ViewportData& vp, uint32_t w, uint32_t h);
    void         freeDepth(ViewportData& vp);
    WGPUBuffer   makeBuffer(uint64_t size, WGPUFlags usage,
                            const void* initData = nullptr);
    Texture*     makeTexture(const uint8_t* rgba, int w, int h);
    WGPUBindGroup makeCamBG(WGPUBuffer ubo);

    WGPUDevice          m_device   = nullptr;
    WGPUQueue           m_queue    = nullptr;
    WGPUTextureFormat   m_fmt      = WGPUTextureFormat_BGRA8Unorm;
    WGPURenderPipeline  m_pipeline = nullptr;
    WGPUBindGroupLayout m_camBGL   = nullptr;
    WGPUBindGroupLayout m_matBGL   = nullptr;
    WGPUBindGroupLayout m_objBGL   = nullptr;

    static constexpr uint32_t kObjStride  = 256;
    static constexpr uint32_t kMaxObjects = 4096;
    WGPUBuffer    m_objUBO = nullptr;
    WGPUBindGroup m_objBG  = nullptr;

    Texture* m_whiteTex = nullptr;

    std::vector<ViewportData> m_viewports;
    ViewportId m_nextId = 0;
    uint32_t   m_surfW  = 0;
    uint32_t   m_surfH  = 0;
};
