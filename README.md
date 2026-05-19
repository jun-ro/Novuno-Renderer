# renderer — minimal WebGPU triangle (WASM + native)

Proof-of-concept: one C++ file, one WGSL shader, compiles to both browser WASM and native desktop.

Uses the standard `webgpu.h` throughout — the v0.19.4.1 wgpu-native header uses
`wgpuSurfaceConfigure` + `wgpuSurfaceGetCurrentTexture` + `wgpuSurfacePresent`
(no SwapChain API). The same surface API is available in current Emscripten.

## Directory layout

```
renderer/
├── CMakeLists.txt
├── src/
│   └── main.cpp          # shared rendering; platform ifdefs for surface creation
├── web/
│   └── index.html        # loads renderer.js / .wasm produced by Emscripten build
├── scripts/
│   └── fetch_wgpu_native.sh
└── wgpu-native/          # created by fetch script (gitignored)
    ├── include/webgpu/webgpu.h
    ├── include/webgpu/wgpu.h
    └── lib/libwgpu_native.a
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

### 4. Run

```sh
./build/native/renderer
```

A 800×600 window opens with a dark background and a blue triangle.

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

**Linux surface creation** — `glfwGetWGPUSurface()` is a Dawn helper that doesn't ship
with wgpu-native. Surface creation is done manually via `WGPUSurfaceDescriptorFromXlibWindow`
(X11) or `WGPUSurfaceDescriptorFromWaylandSurface` (Wayland), selected at compile time
based on which GLFW native handle macro is available.

**macOS** — Requires a small ObjC shim to obtain a `CAMetalLayer` from the `NSWindow*`
returned by `glfwGetCocoaWindow()`. See the `create_metal_layer` extern in `main.cpp`.
For now only Linux and Windows are fully wired up out of the box.

**Emscripten version** — Test with emsdk `3.1.50+`; that's when `wgpuSurfaceConfigure`
landed in the JS bindings.
