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

  // 16 row-major floats, or null for identity. Stage 3: the caller decides the
  // transform, this file only applies it — so changing where the matrix comes
  // from never means touching the vertex path.
  const float* mvp = nullptr;
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

// The renderer's game pipeline takes a fixed 36-byte vertex: float3 POSITION,
// float4 COLOR, then float2 UV (see d3d12_game.cpp's input layout). Named here
// because three places have to agree on it.
// Declared in d3d9_layout.h so the renderer and layout tests share one value.

// A sanity ceiling on one draw's vertex range, so a misread count cannot make
// this allocate wildly. Well above anything observed (the largest captured draw
// is a few thousand vertices).
constexpr uint32_t kMaxHleVertices = 1u << 20;

// Fills `out` and returns true, or returns false with `skip` set. `out` is
// cleared on entry either way.
bool BuildHleDraw(const HleDrawInputs& in, DrawCall& out, HleSkip& skip);

// Finish primitive types that are expansions rather than native host
// topologies. Kept separate from BuildHleDraw because the HLE hook executes the
// guest vertex shader between the two steps: RectangleList's implied fourth
// corner must be derived from shader outputs, not mistaken for another source
// vertex and fetched from guest memory.
bool FinalizeHleTopology(DrawCall& draw, HleSkip& skip);

// The frame's draws, accumulated by the D3D9 draw hooks and taken by the swap
// hook. No lock: every D3D9 entry point in this title arrives on one thread,
// which was measured rather than assumed — each `d3d9: draws` line within a run
// carries a single thread id, and the swap hook runs on that same thread.
std::vector<DrawCall>& HleFrameDraws();

// Why draws did not build, by reason. Indexed by HleSkip.
uint64_t* HleSkipCounts();
uint64_t& HleBuiltCount();

//===========================================================================
// Stage 3 — where the transform comes from.
//
// The plan said to cross-check the HLE matrix against "the PM4 constant
// shadow". **There is no such shadow.** `Pm4Translator::m_mvp` is dead: this
// title emits neither SET_CONSTANT nor SET_SHADER_CONSTANTS, so
// `DrawCall::mvp` is `BuildViewportMvp` — the viewport *inverse*, which treats
// the guest's vertex positions as already being in window coordinates. That is
// a claim about the geometry, not a matrix to compare against, and it is the
// thing this stage has to test.
//
// So the cross-check is the instrument that settled the viewport question
// before: transform real positions by each candidate and count how many land
// inside the clip volume. A matrix that is the right one puts nearly all of
// them there; a wrong one does not. Both readings are scored over the same
// vertices in the same pass, so neither can be favoured by sampling.
//
// The constants are not hooked. `D3DDevice_SetVertexShaderConstantFN`
// (0x82550320) writes them to `device + (StartRegister + 0x78) * 16` — read
// straight off its own arithmetic — so, like the declaration at 0x2ED8 and the
// fetch constants at 0x6F4, the device holds the live value whichever path
// wrote it. Reading beats hooking here for the same reason it did twice
// before: a state block would bypass the hook and not the field.
//===========================================================================

// Vec4 registers sampled from the constant file per draw. A 4x4 matrix can
// start at any of these, and which one it is is exactly the open question.
constexpr uint32_t kHleProbeRegs = 64;

// Layouts a candidate can be read in. A D3D-era compiler packs a matrix into
// four constants either way round, and picking wrong transposes the transform —
// which looks like plausible geometry in the wrong place, not like a failure.
enum class HleMatrixLayout : uint8_t { kRowMajor = 0, kColMajor };

// Score one built draw's positions against every candidate. `consts` is
// kHleProbeRegs*4 floats already in host order; `viewport_mvp` is the control,
// the same transform the PM4 path uses today (may be null if no viewport).
//
// **Measurement only.** Nothing here selects a matrix — the draw is rendered
// with whatever HleDrawInputs::mvp carried, which this stage sets to the
// viewport inverse so the HLE picture is comparable to the PM4 one on screen.
// Wiring a constant-file matrix in is a separate edit, made after reading the
// verdict, not an adaptive choice made mid-run.
// `vertex_shader` is the bound shader handle, cross-tabbed against the winner:
// if the winning register is a property of the shader rather than of the run,
// that is what says so, and a spread of winners across many registers means the
// opposite. Pass 0 when unknown.
void ScoreHleTransform(const DrawCall& dc, const float* consts,
                       const float* viewport_mvp, uint32_t vertex_shader);

// The verdict, written to the log. Reports the controls first so a candidate
// that merely beats nothing is visible as such.
void ReportHleTransform();

}  // namespace mx::pm4
