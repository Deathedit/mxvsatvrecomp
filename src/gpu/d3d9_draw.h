#pragma once

// Build a renderable DrawCall from the D3D9 description.
//
// This is where HLE step 2 pays off: the PM4 path has to infer the stride by
// dividing a buffer size by a vertex count, then guess which attribute is the
// position and which the colour. Here both are stated -- SetStreamSource
// supplies the stride, and the vertex declaration names POSITION 0 and COLOR 0
// by semantic.
//
// **No guest macros here.** The hook owns guest access and hands over plain host
// pointers, so this file stays testable off the guest and cannot acquire an
// unbounded read by accident.

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
  // The guest D3DVertexBuffer `size_bytes` was snapshotted from, kept so the
  // snapshot can be re-checked against that object's LIVE fetch-constant dwords.
  // D3DVertexBuffer_Lock/AsyncLock/Unlock are not hooked, so nothing tells us
  // when a bound buffer is re-pointed or resized. 0 on the UP path.
  uint32_t buffer_obj    = 0;
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

  // Streams whose vertex fetch is indexed by a register other than r0.x -- a
  // value the SHADER computes. One bit per stream, from
  // HlslShader::computed_index_streams.
  //
  // The GPU fetch path indexes these by the named register and copies the whole
  // stream. This path cannot: it is declaration-driven, finds attributes by
  // USAGE, and never runs the ALU that produces the index, so reading vertex N
  // from such a stream is not an approximation but an unrelated row. Those
  // attributes are zero-filled and counted instead.
  uint32_t computed_index_streams = 0;

  // The shader derives a fetch index FROM the vertex index, so SV_VertexID must
  // be the guest's absolute index, not one rebased onto this draw's window.
  //
  // Rebasing (`v - lo`) is right when the vertex index only addresses a stream
  // we windowed to match. It is wrong the moment the shader does arithmetic on
  // it: the billboard shaders compute `instance = vid / 4`, so with a rebased id
  // all 42 foliage draws rendered the SAME billboards. Xenia solves it from the
  // other side, with a vertex index offset system constant; not rebasing needs
  // no constant and no shader change.
  bool absolute_indices = false;

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
  // The clamp is why an overrun can never read past a buffer on hardware: a wild
  // index reads the LAST VALID vertex, not zero. Defaults leave every step inert.
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
  // `vertices` empty and `vertex_stride` zero, skipping the per-vertex pass --
  // 26-31 ms of a menu frame over 289,379 vertices.
  //
  // It is a PREDICTION, not a decision: the fetch path is only settled once the
  // shader's attributes have been cross-checked against the streams, which
  // happens after the draw is built. A draw that turns out not to fetch fills
  // the gap by calling TranscodeHleVertices.
  bool defer_transcode = false;
};

// Cost of BuildHleDraw's per-vertex transcode into the 36-byte host vertex. A
// GPU-fetch draw never reads that output, so this is the size of the second
// prize once the vertex fetch moves into the shader.
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
// because three places have to agree on it, and declared in d3d9_layout.h so the
// renderer and layout tests share one value.

// A sanity ceiling on one draw's vertex range, so a misread count cannot make
// this allocate wildly. Well above anything observed (the largest captured draw
// is a few thousand vertices).
constexpr uint32_t kMaxHleVertices = 1u << 20;

// Fills `out` and returns true, or returns false with `skip` set. `out` is
// cleared on entry either way.
bool BuildHleDraw(const HleDrawInputs& in, DrawCall& out, HleSkip& skip);

// Fill in the 36-byte host vertices for a draw built with `defer_transcode`.
// Reads `out.first_vertex` and `out.vertex_count`, which BuildHleDraw has already
// set, and resolves the declaration's POSITION/COLOR/TEXCOORD0 by exactly the
// rule BuildHleDraw uses -- one implementation, so the deferred and eager forms
// cannot decode a vertex differently. `in` must be the same inputs the draw was
// built from, with its stream pointers still live.
bool TranscodeHleVertices(const HleDrawInputs& in, DrawCall& out,
                          HleSkip& skip);

// Finish primitive types that are expansions rather than native host topologies.
// Kept separate from BuildHleDraw because the HLE hook executes the guest vertex
// shader between the two steps: RectangleList's implied fourth corner must be
// derived from shader outputs, not mistaken for another source vertex and
// fetched from guest memory.
bool FinalizeHleTopology(DrawCall& draw, HleSkip& skip);

// The frame's draws, accumulated by the D3D9 draw hooks and taken by the swap
// hook. THIS THREAD's draws -- the list is thread-local.
//
// It used to be one global vector, on the grounds that every D3D9 entry point in
// this title arrives on one thread, "measured rather than assumed". The
// measurement was real and the conclusion was wrong: it was taken while the
// guest's three parallel record workers were deadlocked in the fence spin.
// Retiring the fence woke them, and the first run with all four threads
// recording crashed inside 20 seconds.
//
// Per-thread lists remove the race without a lock on the hot path, and also fix
// a correctness bug that predates it: each worker drives its OWN D3D9 device, so
// one shared list interleaved three devices' draws in thread-arrival order.
// HleMergeWorkerDraws puts that right.
std::vector<DrawCall>& HleFrameDraws();

// The HLE layer's big lock.
//
// Per-thread draw lists and device state removed the two structural races, but
// the D3D9 hooks carry roughly thirty more shared globals -- mostly std::map
// caches and diagnostic tallies keyed by shader handle -- and every one is
// written from the draw path.
//
// Serialising the hooks is the honest fix. It costs the parallelism the guest
// intended, but we were never exploiting it -- our own HLE work is nearly the
// whole frame. Once it is stable, individual caches can be made thread-local to
// win it back, with a measurement to justify each one.
std::recursive_mutex& HleGlobalMutex();

// Tag this thread as parallel record worker `index` (0..2). Called from the
// worker hook so the merge below can order the batches the way the guest's own
// join consumes them, rather than by whichever thread happened to finish first.
void HleSetThreadRecordIndex(uint32_t index);

// Append the workers' draws to the calling thread's list, in worker-index order,
// and empty theirs. Call from the join (sub_82AC8B68) — by then every worker has
// signalled its done-event and parked on its next go-event, so the guest's own
// fork/join discipline is what makes this safe.
void HleMergeWorkerDraws();

// Why draws did not build, by reason. Indexed by HleSkip.
uint64_t* HleSkipCounts();
uint64_t& HleBuiltCount();

// Vertices whose stream ran out before the end of the stride, and which were
// zero-filled from that point rather than costing the whole draw. See
// CopyVertex: this is the gate that `kVertexOutOfRange` used to be, and it
// discarded 16% of every frame's draws.
uint64_t& HleVertexZeroFillCount();

// Attributes this path could not read because their stream is fetched with a
// SHADER-COMPUTED index. Not a failure to be fixed here: the value does not exist
// without running the ALU, so the attribute keeps its default. Counted because
// the alternative was reading an unrelated vertex and never knowing, and because
// a non-zero here means those draws want the GPU fetch path.
uint64_t& HleComputedIndexSkips();

// Primitive restart. `Draws` counts draws that contained at least one marker and
// were therefore walked into a list topology; `Count` counts the markers. A
// marker is not a vertex, so substituting one does not end a strip -- it welds
// the last vertices of one patch onto the first of the next, which was the
// sheets of terrain stretching over the horizon. If `Draws` is 0 on a run with
// ground in frame, the walk never executed and nothing here has been tested.
uint64_t& HleRestartCutDraws();
uint64_t& HleRestartCutCount();

// Who is actually short, and by how much. The bare count above says 304 million
// and nothing about whether that is every stream a little or one stream
// completely. `worst_vertices_past_end` is the discriminator: 1 is a buffer one
// vertex short, thousands means the index does not address this stream at all.
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
// Stage 3 -- where the transform comes from.
//
// The plan said to cross-check the HLE matrix against "the PM4 constant shadow".
// **There is no such shadow.** `Pm4Translator::m_mvp` is dead: this title emits
// neither SET_CONSTANT nor SET_SHADER_CONSTANTS, so `DrawCall::mvp` is
// `BuildViewportMvp` -- the viewport *inverse*, which treats the guest's vertex
// positions as already being in window coordinates. That is a claim about the
// geometry, not a matrix to compare against, and it is what this stage tests.
//
// So the cross-check is the instrument that settled the viewport question
// before: transform real positions by each candidate and count how many land
// inside the clip volume, both readings scored over the same vertices in the
// same pass.
//
// The constants are not hooked. D3DDevice_SetVertexShaderConstantFN writes them
// to `device + (StartRegister + 0x78) * 16`, so the device holds the live value
// whichever path wrote it -- a state block would bypass the hook and not the
// field.
//===========================================================================

// Vec4 registers sampled from the constant file per draw. A 4x4 matrix can
// start at any of these, and which one it is is exactly the open question.
constexpr uint32_t kHleProbeRegs = 64;

// Layouts a candidate can be read in. A D3D-era compiler packs a matrix into
// four constants either way round, and picking wrong transposes the transform --
// which looks like plausible geometry in the wrong place, not like a failure.
// The probe below scores both layouts internally and names the winner.

// Score one built draw's positions against every candidate. `consts` is
// kHleProbeRegs*4 floats already in host order; `viewport_mvp` is the control,
// the same transform the PM4 path uses today (may be null if no viewport).
//
// **Measurement only.** Nothing here selects a matrix -- the draw is rendered
// with whatever HleDrawInputs::mvp carried.
//
// The winner is cross-tabbed against the SHADER, and the shader is identified by
// the CONTENT of its microcode, never by its handle. `vs_content` is an FNV-1a
// over the microcode dwords; `vs_handle` is the guest address, carried only so
// the report can say how many handles one shader wore.
//
// WHY NOT THE HANDLE: handles are guest addresses and the runtime recycles them
// onto DIFFERENT microcode within a single run -- 9,968 of 10,074 compiles.
// Keying by handle both splits one shader across several rows and merges several
// shaders into one. That matters because the whole verdict here is a judgment
// about SCATTER, and a handle covering three shaders manufactures exactly that.
// `named_reg` is the constant register the SHADER'S OWN ASSET calls
// gViewProjection, or -1 when it is unknown. Passing it turns this from a
// search into a CHECK: the probe has been ranking candidate registers by how
// much geometry each puts inside the clip volume, and the answer has been
// sitting in the .shader assets' constant table all along.
//
// Still measurement only. A disagreement is reported, never acted on -- and it
// is worth having, because either the probe's scoring or the asset join is
// wrong and the report says which shader to look at.
void ScoreHleTransform(const DrawCall& dc, const float* consts,
                       const float* viewport_mvp, uint64_t vs_content,
                       uint32_t vs_handle, int32_t named_reg);

// The verdict, written to the log. Reports the controls first so a candidate
// that merely beats nothing is visible as such.
void ReportHleTransform();

}  // namespace mx::hle
