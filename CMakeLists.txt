cmake_minimum_required(VERSION 3.20)
project(renderer C CXX)

include(FetchContent)

FetchContent_Declare(glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG        1.0.1
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(glm)

FetchContent_Declare(stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE
)
FetchContent_GetProperties(stb)
if(NOT stb_POPULATED)
    FetchContent_Populate(stb)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(SOURCES src/main.cpp src/renderer.cpp src/stb_image_impl.c)

# ── WASM / Emscripten ──────────────────────────────────────────────────────────
if(EMSCRIPTEN)
    add_executable(renderer ${SOURCES})

    set_target_properties(renderer PROPERTIES
        OUTPUT_NAME "renderer"
        SUFFIX ".js"
    )
    target_compile_options(renderer PRIVATE --use-port=emdawnwebgpu)
    target_link_options(renderer PRIVATE
        --use-port=emdawnwebgpu
        -sALLOW_MEMORY_GROWTH=1
        -sEXPORT_NAME="createModule"
        -sENVIRONMENT=web
        -sMODULARIZE=1
        -sSTACK_SIZE=1048576
    )
    target_include_directories(renderer PRIVATE ${stb_SOURCE_DIR})
    target_link_libraries(renderer PRIVATE glm::glm)
    # Copy JS + WASM into web/ after build
    add_custom_command(TARGET renderer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy
            "${CMAKE_CURRENT_BINARY_DIR}/renderer.js"
            "${CMAKE_SOURCE_DIR}/web/renderer.js"
        COMMAND ${CMAKE_COMMAND} -E copy
            "${CMAKE_CURRENT_BINARY_DIR}/renderer.wasm"
            "${CMAKE_SOURCE_DIR}/web/renderer.wasm"
        COMMENT "Copying WASM artifacts to web/"
    )

# ── Native ─────────────────────────────────────────────────────────────────────
else()
    # Locate wgpu-native prebuilt.
    # Layout expected after fetch_wgpu_native.sh:
    #   ${WGPU_NATIVE_DIR}/include/webgpu/webgpu.h
    #   ${WGPU_NATIVE_DIR}/include/webgpu/wgpu.h
    #   ${WGPU_NATIVE_DIR}/lib/libwgpu_native.a  (or .so)
    #
    # Override via: cmake -DWGPU_NATIVE_DIR=<path> ..
    # or set env var WGPU_NATIVE_DIR before running cmake.
    if(NOT DEFINED WGPU_NATIVE_DIR)
        if(DEFINED ENV{WGPU_NATIVE_DIR})
            set(WGPU_NATIVE_DIR $ENV{WGPU_NATIVE_DIR})
        else()
            set(WGPU_NATIVE_DIR "${CMAKE_SOURCE_DIR}/wgpu-native")
        endif()
    endif()
    message(STATUS "WGPU_NATIVE_DIR = ${WGPU_NATIVE_DIR}")

    find_library(WGPU_LIB
        NAMES wgpu_native
        PATHS "${WGPU_NATIVE_DIR}/lib"
        NO_DEFAULT_PATH
        REQUIRED
    )
    message(STATUS "wgpu_native library: ${WGPU_LIB}")

    find_package(glfw3 3.3 REQUIRED)

    if(APPLE)
        list(APPEND SOURCES src/metal_surface.mm)
    endif()

    add_executable(renderer ${SOURCES})

    # Linux: select Wayland or X11 backend for surface creation
    if(UNIX AND NOT APPLE)
        option(RENDERER_WAYLAND "Use Wayland for surface creation (default: X11)" OFF)
        if(RENDERER_WAYLAND)
            target_compile_definitions(renderer PRIVATE RENDERER_USE_WAYLAND)
        else()
            target_compile_definitions(renderer PRIVATE RENDERER_USE_X11)
        endif()
    endif()

    target_include_directories(renderer PRIVATE
        "${WGPU_NATIVE_DIR}/include"
    )

    target_include_directories(renderer PRIVATE ${stb_SOURCE_DIR})
    target_link_libraries(renderer PRIVATE ${WGPU_LIB} glfw glm::glm)

    if(UNIX AND NOT APPLE)
        target_link_libraries(renderer PRIVATE dl pthread)
        # X11 for surface creation via GLFW native handles
        find_package(X11 QUIET)
        if(X11_FOUND)
            target_link_libraries(renderer PRIVATE ${X11_LIBRARIES})
        endif()
        find_package(Vulkan QUIET)
        if(Vulkan_FOUND)
            target_link_libraries(renderer PRIVATE Vulkan::Vulkan)
        endif()
    endif()

    if(APPLE)
        target_link_libraries(renderer PRIVATE
            "-framework Metal"
            "-framework QuartzCore"
            "-framework IOKit"
            "-framework Cocoa"
        )
    endif()
endif()
