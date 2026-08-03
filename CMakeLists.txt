# mx - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(mx LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_definitions(UNICODE _UNICODE)

set(FFMPEG_ROOT "${CMAKE_SOURCE_DIR}/ffmpeg")

include(generated/rexglue.cmake)

# Sources, grouped by layer (see src/ layout in AGENTS.md).
set(MX_SOURCES
    src/main.cpp

    # app — ReXGlue application + graphics system / render thread
    src/app/mx_app.cpp
    src/app/graphics_system.cpp

    # gfx — D3D12 renderer and Bink video decode
    src/gfx/d3d12_device.cpp
    src/gfx/d3d12_video.cpp
    src/gfx/d3d12_game.cpp
    src/gfx/bink_player.cpp

    # gpu — Xenos PM4 parse / translate / register shadow
    src/gpu/pm4_parser.cpp
    src/gpu/pm4_translator.cpp
    src/gpu/shader_ucode.cpp
    src/gpu/shader_alu.cpp
    src/gpu/xenos_gpu_state.cpp

    # hooks — guest function hooks and mid-ASM stubs
    src/hooks/native_bridge.cpp
    src/hooks/midasm_stubs.cpp
    src/hooks/hooks_frame.cpp
    src/hooks/hooks_wait.cpp
    src/hooks/hooks_gameloop.cpp
    src/hooks/hooks_loading.cpp
    src/hooks/hooks_boot.cpp
    src/hooks/hooks_plugin_diag.cpp
    src/hooks/hooks_d3d9.cpp
)

if(WIN32)
    add_executable(mx WIN32 ${MX_SOURCES})
else()
    add_executable(mx ${MX_SOURCES})
endif()

rexglue_setup_target(mx)

# Export mid-ASM hook functions so generated code (with dllimport) can find them
target_link_options(mx PRIVATE "LINKER:/EXPORT:NativeGameTickSkip" "LINKER:/EXPORT:NativeSetupDeviceSkip" "LINKER:/EXPORT:NativeSkipVtable8" "LINKER:/EXPORT:NativeSkipVtable17" "LINKER:/EXPORT:NativeSkipRendererInit" "LINKER:/EXPORT:NativeSkipLoaderRenderer" "LINKER:/EXPORT:NativeSkipLoaderEarly" "LINKER:/EXPORT:NativeSkipLoaderAll" "LINKER:/EXPORT:NativeSkipRendererDispatch" "LINKER:/EXPORT:NativePreDispatchLog" "LINKER:/EXPORT:NativePostLazyInitLog")

target_link_libraries(mx PRIVATE d3d12 dxgi dxguid d3dcompiler winmm
    "${FFMPEG_ROOT}/lib/avcodec.lib"
    "${FFMPEG_ROOT}/lib/avformat.lib"
    "${FFMPEG_ROOT}/lib/avutil.lib"
    "${FFMPEG_ROOT}/lib/swscale.lib"
    "${FFMPEG_ROOT}/lib/swresample.lib"
)

# src/ on the include path so cross-layer includes name their layer:
#   #include "gpu/pm4_parser.h", #include "gfx/d3d12_renderer.h"
target_include_directories(mx PRIVATE "${FFMPEG_ROOT}/include" "${CMAKE_SOURCE_DIR}/src")
