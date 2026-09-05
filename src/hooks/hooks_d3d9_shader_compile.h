#pragma once

// The seam between hooks_d3d9.cpp and hooks_d3d9_shader_compile.cpp.
//
// Two symbols, deliberately, and its own header rather than two more lines in
// hooks_d3d9_internal.h: that header is the measured 62-symbol boundary to the
// entry points, and widening it for an unrelated seam is how a boundary stops
// meaning anything. This one has exactly two users.

#include <atomic>
#include <cstdint>

namespace mx::hooks::d3d9 {

// Defined in hooks_d3d9.cpp beside the device/shader shadow it belongs to; read
// by the coverage report. The translation cache is keyed on a guest ADDRESS and
// the guest recycles them onto different microcode, which is what this counts.
extern std::atomic<uint64_t> g_shaderHandleRecycled;

// Defined in hooks_d3d9_shader_compile.cpp next to g_translatedMu, which it
// locks. Declared here because the transform probe cross-tabs by shader CONTENT
// rather than by handle, and runs far above the translation cache.
uint64_t VertexShaderContentId(uint32_t handle);

}  // namespace mx::hooks::d3d9
