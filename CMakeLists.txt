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

# Sources
set(MX_SOURCES
    src/main.cpp
    src/d3d12_renderer.cpp
    src/native_graphics.cpp
    src/native_heap.cpp
    src/pm4_parser.cpp
    src/pm4_translator.cpp
    src/xenos_gpu_state.cpp
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

target_include_directories(mx PRIVATE "${FFMPEG_ROOT}/include")
