# renderer — WebGPU rendering backend (WASM + native)

Multi-viewport WebGPU renderer in C++. Accepts meshes with transforms and materials, draws them through independent camera viewports. Compiles to both browser WASM (Emscripten) and native desktop (wgpu-native).

Uses the standard `webgpu.h` throughout — wgpu-native v0.19.4.1 surface API (`wgpuSurfaceConfigure` / `wgpuSurfaceGetCurrentTexture` / `wgpuSurfacePresent`).

## Features

- **Vertex/index buffers** — `Mesh` with GPU vertex buffer (pos/normal/UV) and index buffer
- **Uniform buffers** — per-viewport `CameraUniforms` (viewProj, lightDir, lightColor) and per-draw `ObjectUniforms` (model, normalMatrix) via dynamic offsets
- **Depth buffer** — `Depth24Plus` per viewport, recreated automatically on resize
- **Texture support** — `Texture` (WGPUTexture + sampler), `loadTexture(path)` via stb_image, 1×1 white fallback
- **Viewport system** — normalized `(x,y,w,h)` rectangles, each with its own camera, depth texture, and draw list
- **Submit interface** — `submit()` to all viewports, `submitTo(id)` to one; draw calls batched by material
- **Directional lighting** — diffuse + 0.1 ambient, normal matrix transform in WGSL

## Directory layout

```
renderer/
├── CMakeLists.txt
├── src/
│   ├── main.cpp             # platform glue + two-viewport demo
│   ├── renderer.h           # public API
│   ├── renderer.cpp         # implementation
│   ├── stb_image_impl.c     # stb_image translation unit
│   └── metal_surface.mm     # macOS CAMetalLayer shim
├── web/
│   └── index.html           # loads renderer.js / .wasm
├── scripts/
│   └── fetch_wgpu_native.sh
└── wgpu-native/             # created by fetch script (gitignored)
```

## API quick reference

```cpp
// One-time setup
Renderer renderer;
renderer.init(device, queue, WGPUTextureFormat_BGRA8Unorm, width, height);

// Resources
Mesh*    mesh = renderer.createMesh(vertices, indices);
Texture* tex  = renderer.loadTexture("path/to/image.png");
// or:    tex  = renderer.createTexture(rgbaPtr, w, h);

// Viewports (call once, not every frame)
ViewportId v0 = renderer.addViewport(0.f, 0.f, 0.5f, 1.f, camera0);
ViewportId v1 = renderer.addViewport(0.5f, 0.f, 0.5f, 1.f, camera1);

// Per-frame
renderer.setCamera(v0, updatedCamera);
renderer.beginFrame(surfaceWidth, surfaceHeight);
renderer.submit(*mesh, modelMatrix, material);        // all viewports
renderer.submitTo(v1, *hudMesh, identity, hudMat);    // one viewport only
renderer.endFrame(surface);
```

---

## Native build (Linux / macOS)

### 1. Install dependencies

**Linux (Debian/Ubuntu)**
```sh
sudo apt install cmake build-essential libglfw3-dev libvulkan-dev libx11-dev
```

**macOS (Homebrew)**
```sh
brew install cmake glfw
```

### 2. Download wgpu-native prebuilt

```sh
./scripts/fetch_wgpu_native.sh          # defaults to v0.19.4.1
```

### 3. Build

```sh
cmake -B build/native -DCMAKE_BUILD_TYPE=Release
cmake --build build/native -j$(nproc)
```

GLM and stb_image are fetched automatically by CMake FetchContent.

### 4. Run

```sh
./build/native/renderer
```

Opens a 1280×600 window with two viewports side by side. Both show the same textured cube rotating under directional lighting; each camera orbits from a different angle and distance.

---

## WASM build (Emscripten)

### 1. Install Emscripten

```sh
git clone https://github.com/emscripten-core/emsdk.git
cd emsdk && ./emsdk install latest && ./emsdk activate latest
source ./emsdk_env.sh
```

### 2. Build

```sh
cd renderer/
emcmake cmake -B build/wasm -DCMAKE_BUILD_TYPE=Release
cmake --build build/wasm -j$(nproc)
```

Output `renderer.js` and `renderer.wasm` are copied into `web/`.

### 3. Serve

WebGPU requires a secure context (HTTPS or localhost):

```sh
python3 -m http.server 8080 --directory web/
# open http://localhost:8080
```

---

## Platform notes

**Linux surface creation** — `glfwGetWGPUSurface()` is a Dawn helper not in wgpu-native. Surface creation is done manually via `WGPUSurfaceDescriptorFromXlibWindow` (X11) or `WGPUSurfaceDescriptorFromWaylandSurface` (Wayland), selected at compile time. Pass `-DRENDERER_WAYLAND=ON` to CMake for Wayland.

**macOS** — Requires the ObjC shim in `src/metal_surface.mm` to obtain a `CAMetalLayer` from `NSWindow*`.

**Emscripten version** — Requires emsdk `3.1.50+` (when `wgpuSurfaceConfigure` landed in JS bindings). Uses the `emdawnwebgpu` port.

**GLM** — Fetched at configure time via FetchContent (tag 1.0.1). `GLM_FORCE_DEPTH_ZERO_TO_ONE` is set globally so projection matrices use WebGPU's [0,1] NDC depth range.

**stb_image** — Fetched at configure time. The implementation translation unit is `src/stb_image_impl.c`.
