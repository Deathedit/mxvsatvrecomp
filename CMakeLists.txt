# mx - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(mx LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_definitions(UNICODE _UNICODE)


include(generated/rexglue.cmake)

# Sources, grouped by layer (see src/ layout in AGENTS.md).
set(MX_SOURCES
    src/main.cpp

    # app — ReXGlue application + graphics system / render thread
    src/app/mx_app.cpp
    src/app/graphics_system.cpp

    # gfx — D3D12 renderer
    src/gfx/d3d12_device.cpp
    src/gfx/d3d12_game.cpp
    src/gfx/d3d12_game_bind.cpp
    src/gfx/d3d12_game_resource.cpp
    src/gfx/d3d12_game_frame.cpp

    # gpu — Xenos PM4 parse / translate / register shadow
    src/gpu/pm4_parser.cpp
    src/gpu/hle_types.cpp
    src/gpu/shader_ucode.cpp
    src/gpu/shader_alu.cpp
    src/gpu/shader_hlsl.cpp
    src/gpu/guard_census.cpp
    src/gpu/health.cpp
    src/gpu/xenos_gpu_state.cpp
    src/gpu/d3d9_layout.cpp
    src/gpu/d3d9_draw.cpp
    src/gpu/d3d9_texture.cpp
    src/gpu/d3d9_state.cpp

    # hooks — guest function hooks
    src/hooks/native_bridge.cpp
    src/hooks/hooks_frame.cpp
    src/hooks/hooks_wait.cpp
    src/hooks/hooks_gameloop.cpp
    src/hooks/hooks_loading.cpp
    src/hooks/hooks_boot.cpp
    src/hooks/hooks_plugin_diag.cpp
    src/hooks/hooks_debug_unlock.cpp
    src/hooks/guest_read_watch.cpp
    src/hooks/hooks_d3d9.cpp
    src/hooks/hooks_d3d9_texture.cpp
    src/hooks/hooks_d3d9_entry.cpp
    src/hooks/texture_dump.cpp
)

if(WIN32)
    add_executable(mx WIN32 ${MX_SOURCES})
else()
    add_executable(mx ${MX_SOURCES})
endif()

rexglue_setup_target(mx)

# /MAP costs nothing at runtime and makes the next one a lookup rather than an
# argument.
target_link_options(mx PRIVATE "LINKER:/MAP:$<TARGET_FILE_DIR:mx>/mx.map")

# windowscodecs: WIC, for the PNG encoder in src/hooks/texture_dump.cpp. Ships
# with Windows, so this adds no dependency to fetch or vendor.
target_link_libraries(mx PRIVATE d3d12 dxgi dxguid d3dcompiler winmm windowscodecs)

# src/ on the include path so cross-layer includes name their layer:
#   #include "gpu/pm4_parser.h", #include "gfx/d3d12_renderer.h"
target_include_directories(mx PRIVATE "${CMAKE_SOURCE_DIR}/src")
