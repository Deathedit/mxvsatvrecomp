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
// The output is the existing mx::hle::DrawCall, so the whole downstream path —
// NativeGraphics::SetDrawCalls, the render thread's filter loop, AddGameDraw —
// is reused unchanged and no gfx code moves.

#include <cstdint>
#include <mutex>

#include "gpu/d3d9_layout.h"
#include "gpu/d3d9_state.h"
#include "gpu/hle_types.h"

namespace mx::hle {

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

  // How the hardware conditions an index before it reaches the vertex fetch.
  // Ignoring these is what lost the ground: one 0xFFFF primitive-restart index
  // made vmax 65535, the vertex window exploded to 65536, and the draw was
  // refused as out of range.
  //
  // From draw_extent_estimator.cc:173-197, in order:
  //   index &= 0xFFFFFF                      -- only 24 bits are used
  //   if (reset_enabled && index == reset)   -- restart, not a vertex
  //   index = (index + offset) & 0xFFFFFF
  //   index = clamp(index, min_index, max_index)
  //
  // The clamp is the important one and is why an overrun can never read past a
  // buffer on hardware: a wild index reads the LAST VALID vertex, it does not
  // read zero. Defaults leave every step inert, so a caller that does not fill
  // these behaves exactly as before.
  uint32_t index_max     = 0xFFFFFF;  // VGT_MAX_VTX_INDX.max_indx  (0x2100)
  uint32_t index_min     = 0;         // VGT_MIN_VTX_INDX.min_indx  (0x2101)
  uint32_t index_offset  = 0;         // VGT_INDX_OFFSET            (0x2102)
  uint32_t index_reset   = 0xFFFFFF;  // ..._IB_RESET_INDX.reset_indx (0x2103)
  bool     index_reset_enabled = false;  // PA_SU_SC_MODE_CNTL bit 21

  // 16 row-major floats, or null for identity. Stage 3: the caller decides the
  // transform, this file only applies it — so changing where the matrix comes
  // from never means touching the vertex path.
  const float* mvp = nullptr;

  // The caller expects this draw to fetch its vertices on the GPU, where the
  // 36-byte host vertex below is never read. Set it and BuildHleDraw leaves
  // `vertices` empty and `vertex_stride` zero, skipping the per-vertex pass
  // entirely — 26-31 ms of a menu frame, measured over 289,379 vertices.
  //
  // It is a PREDICTION, not a decision: the fetch path is only finally settled
  // once the shader's attributes have been cross-checked against the streams,
  // which happens after the draw is built. A draw that turns out not to fetch
  // fills the gap by calling TranscodeHleVertices below, so a wrong prediction
  // costs nothing beyond doing the work later.
  bool defer_transcode = false;
};

// Cost of BuildHleDraw's per-vertex transcode into the 36-byte host vertex.
// A GPU-fetch draw never reads that output, so this is the size of the second
// prize once the vertex fetch moves into the shader. Accumulated here, read and
// reset by the frame report in the D3D9 layer.
extern uint64_t g_transcodeUs;
extern uint64_t g_transcodeVerts;

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

// Fill in the 36-byte host vertices for a draw built with `defer_transcode`.
// Reads `out.first_vertex` and `out.vertex_count`, which BuildHleDraw has
// already set, and resolves the declaration's POSITION/COLOR/TEXCOORD0 by
// exactly the rule BuildHleDraw uses — one implementation, so the deferred and
// eager forms cannot decode a vertex differently.
//
// `in` must be the same inputs the draw was built from, with its stream
// pointers still live.
bool TranscodeHleVertices(const HleDrawInputs& in, DrawCall& out,
                          HleSkip& skip);

// Finish primitive types that are expansions rather than native host
// topologies. Kept separate from BuildHleDraw because the HLE hook executes the
// guest vertex shader between the two steps: RectangleList's implied fourth
// corner must be derived from shader outputs, not mistaken for another source
// vertex and fetched from guest memory.
bool FinalizeHleTopology(DrawCall& draw, HleSkip& skip);

// The frame's draws, accumulated by the D3D9 draw hooks and taken by the swap
// hook. THIS THREAD's draws — the list is thread-local.
//
// It used to be one global vector, on the grounds that "every D3D9 entry point
// in this title arrives on one thread, which was measured rather than assumed".
// The measurement was real and the conclusion was wrong: it was taken while the
// guest's three parallel record workers (sub_82AC8CC8) were deadlocked in the
// fence spin, so they recorded nothing and the traffic really did arrive on one
// thread. Retiring the fence (hooks_frame.cpp) woke them, and the first run
// with all four threads recording crashed inside 20 seconds — a host-side heap
// write and two threads dereferencing 0xFFFFFFFFFFFFFFFF, which is what
// concurrent push_back into one vector of DrawCall (two vectors and a
// shared_ptr apiece) looks like.
//
// Per-thread lists remove the race without a lock on the hot path, and they
// also fix a correctness bug that predates it: each worker drives its OWN D3D9
// device (dword_830B2C60[0..2]), so one shared list interleaved three devices'
// draws in thread-arrival order. HleMergeWorkerDraws puts that right.
std::vector<DrawCall>& HleFrameDraws();

// The HLE layer's big lock.
//
// Per-thread draw lists and device state removed the two structural races, but
// the D3D9 hooks carry roughly thirty more shared globals — mostly std::map
// caches and diagnostic tallies keyed by shader handle — and every one of them
// is written from the draw path. Concurrent insertion into a red-black tree
// corrupts it, and mx_701 still died at frame 2726 with the same signature: a
// host-side heap write and a read of 0xFFFFFFFFFFFFFFFF.
//
// Serialising the hooks is the honest fix. It costs the parallelism the guest
// intended, but we were never exploiting it — our own HLE work is nearly the
// whole frame, so the three workers were contending on this layer anyway. Once
// it is stable, individual caches can be made thread-local to win it back,
// with a measurement to justify each one.
std::recursive_mutex& HleGlobalMutex();

// Tag this thread as parallel record worker `index` (0..2). Called from the
// worker hook so the merge below can order the batches the way the guest's own
// join consumes them, rather than by whichever thread happened to finish first.
void HleSetThreadRecordIndex(uint32_t index);

// Append the workers' draws to the calling thread's list, in worker-index
// order, and empty theirs. Call from the join (sub_82AC8B68) — by then every
// worker has signalled its done-event and parked on its next go-event, so the
// guest's own fork/join discipline is what makes this safe. We inherit it
// rather than adding a lock of our own.
void HleMergeWorkerDraws();

// Why draws did not build, by reason. Indexed by HleSkip.
uint64_t* HleSkipCounts();
uint64_t& HleBuiltCount();

// Vertices whose stream ran out before the end of the stride, and which were
// zero-filled from that point rather than costing the whole draw. See
// CopyVertex: this is the gate that `kVertexOutOfRange` used to be, and it
// discarded 16% of every frame's draws.
uint64_t& HleVertexZeroFillCount();

// Primitive restart. `Draws` counts draws that contained at least one marker and
// were therefore walked into a list topology; `Count` counts the markers.
//
// Both matter, and for different reasons. A marker is not a vertex, so it cannot
// be turned into an index: substituting one does not end a strip, it welds the
// last vertices of one patch onto the first of the next. Those welds were the
// sheets of terrain stretching over the horizon. If `Draws` is 0 on a run with
// ground in frame, the walk never executed and nothing here has been tested.
uint64_t& HleRestartCutDraws();
uint64_t& HleRestartCutCount();

// Who is actually short, and by how much. The bare count above says 304 million
// and nothing about whether that is every stream a little or one stream
// completely -- and those are different bugs. `worst_vertices_past_end` is the
// discriminator: 1 is a buffer one vertex short, thousands means the index does
// not address this stream at all.
constexpr uint32_t HleZeroFillCensusStreams = 4;
struct HleZeroFillCensusData {
  struct First {
    uint64_t fills = 0;      // doubles as "have I recorded one yet"
    uint32_t stride = 0;
    uint32_t size_bytes = 0;
    uint32_t offset_bytes = 0;
    uint32_t index = 0;
    uint64_t byte_off = 0;
  };
  struct Stream {
    uint64_t fills = 0;
    uint64_t worst_vertices_past_end = 0;
    First first;
  };
  Stream stream[HleZeroFillCensusStreams];
};
HleZeroFillCensusData& HleZeroFillCensus();

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
// HleMatrixLayout (kRowMajor / kColMajor) was declared here and never used —
// removed 2026-08-17. The probe below scores both layouts internally and names
// the winner in its report; it never needed the type to be public.

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

}  // namespace mx::hle
