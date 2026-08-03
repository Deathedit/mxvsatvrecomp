#pragma once

// Build a renderable DrawCall from the D3D9 description.
//
// This is where HLE step 2 pays off: the PM4 path has to infer the stride by
// dividing a buffer size by a vertex count, then guess which attribute is the
// position and which is the colour. Here both are stated — SetStreamSource
// supplies the stride, and the vertex declaration names POSITION 0 and COLOR 0
// by semantic.
//
// **No guest macros here.** The hook owns guest access and hands over plain
// host pointers, so this file stays testable off the guest and cannot acquire
// an unbounded read by accident. Every range is bounded by the buffer size the
// hook recorded from the object D3D9 itself read.
//
// The output is the existing mx::pm4::DrawCall, so the whole downstream path —
// NativeGraphics::SetDrawCalls, the render thread's filter loop, AddGameDraw —
// is reused unchanged and no gfx code moves.

#include <cstdint>

#include "gpu/d3d9_layout.h"
#include "gpu/d3d9_state.h"
#include "gpu/pm4_translator.h"

namespace mx::pm4 {

// One bound stream, already resolved to a host pointer by the caller.
struct HleStream {
  const uint8_t* host    = nullptr;  // guest bytes, still big-endian
  uint32_t size_bytes    = 0;
  uint32_t stride        = 0;
  uint32_t offset_bytes  = 0;        // SetStreamSource's OffsetInBytes
  uint32_t endian        = 0;        // 0 none, 1 8in16, 2 8in32
  bool     bound         = false;
};

struct HleIndexBuffer {
  const uint8_t* host = nullptr;
  uint32_t size_bytes = 0;
  bool     is_32bit   = false;
  bool     bound      = false;
};

struct HleDrawInputs {
  const HleInputLayout* layout = nullptr;
  const HleStream* streams     = nullptr;   // kMaxStreams entries
  HleIndexBuffer index;
  bool     indexed     = false;
  uint32_t prim_type   = 0;   // D3DPRIMITIVETYPE, which is the Xenos value
  uint32_t first       = 0;   // StartVertex, or StartIndex when indexed
  uint32_t count       = 0;   // VertexCount, or IndexCount when indexed
  int32_t  base_vertex = 0;   // DrawIndexedVertices' BaseVertexIndex
};

// Why a draw produced nothing. Every one is counted and named — a bare
// "skipped" total cannot distinguish a decode gap from a stream this path does
// not yet model, and those need opposite fixes.
enum class HleSkip : uint8_t {
  kNone = 0,
  kNoLayout,           // no declaration bound, or it does not decode
  kNoPosition,         // the declaration has no POSITION 0
  kStreamUnbound,      // a stream the position or colour needs was never set
  kZeroStride,
  kBadTopology,        // a primitive type with no host equivalent
  kEmpty,              // zero vertices or indices
  kVertexOutOfRange,   // the vertex range is outside the bound buffer
  kIndexOutOfRange,    // likewise for the index range
  kUnreadableFormat,   // ReadHleElement refused the format
  kTooManyVertices,    // beyond the per-draw cap below
  kCount,
};

const char* HleSkipName(HleSkip s);

// The renderer's game pipeline takes a fixed 28-byte vertex: float3 POSITION
// then float4 COLOR (see d3d12_game.cpp's input layout). Named here because
// three places have to agree on it.
constexpr uint32_t kHostVertexStride = 28;

// A sanity ceiling on one draw's vertex range, so a misread count cannot make
// this allocate wildly. Well above anything observed (the largest captured draw
// is a few thousand vertices).
constexpr uint32_t kMaxHleVertices = 1u << 20;

// Fills `out` and returns true, or returns false with `skip` set. `out` is
// cleared on entry either way.
bool BuildHleDraw(const HleDrawInputs& in, DrawCall& out, HleSkip& skip);

// The frame's draws, accumulated by the D3D9 draw hooks and taken by the swap
// hook. No lock: every D3D9 entry point in this title arrives on one thread,
// which was measured rather than assumed — each `d3d9: draws` line within a run
// carries a single thread id, and the swap hook runs on that same thread.
std::vector<DrawCall>& HleFrameDraws();

// Why draws did not build, by reason. Indexed by HleSkip.
uint64_t* HleSkipCounts();
uint64_t& HleBuiltCount();

}  // namespace mx::pm4
