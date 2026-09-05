// Internal interface of the D3D9 HLE layer.
//
// NOT a public header. It exists so the guest entry points can live in
// hooks_d3d9_entry.cpp while the state and draw-building machinery they drive
// stay in hooks_d3d9.cpp. The split point is the only cheap one the file has:
// measured, a cut at the entry points costs 62 shared symbols where every other
// candidate boundary cost 68 to 173, and the entry points were already outside
// the namespace because REX_FUNC is extern "C".
//
// Keep this list SHORT. Anything added here is state two translation units can
// reach, and the reason the original file was hard to reason about is that ~150
// counters were reachable from anywhere in it.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <fstream>
#include <map>
#include <vector>

#include "gpu/d3d9_draw.h"     // HleStream, kMaxStreams
#include "gpu/d3d9_layout.h"   // D3D9Element, LayoutError
#include "gpu/d3d9_state.h"
#include "gpu/hle_types.h"
#include "gpu/shader_ucode.h"

namespace mx::hooks::d3d9 {

// Pulled into the layer's namespace so both translation units name them the
// same way. DeviceState is keyed by DEVICE, not by thread -- three guest record
// workers each saw their own empty state before that was fixed.
using mx::hle::DeviceState;
using mx::hle::kElementSize;
using mx::hle::kMaxElements;   // refuses to walk a runaway array

// ---- Shared constants -----------------------------------------------------

// A declaration is never destroyed as far as this table is concerned: there is
// no Release hook, so an entry is only ever added. 256 was a silent cliff --
// RecordDeclaration returned -1 past it, which made every draw using a later
// declaration look like one with no declaration bound, which BuildHleDraw
// refuses as kNoLayout and the hook then drops with a bare `return`, so assets
// streamed in later stopped rendering with no report of any kind.
//
// Raised, and exhaustion is now reported by name. The table is flat arrays
// scanned linearly by KnownDeclId, so this trades a larger scan for not losing
// draws; the scan is once per CreateVertexDeclaration and per draw.
constexpr int kMaxTrackedDecls = 4096;
constexpr int kMaxDeclsLogged = 512;
constexpr int kMaxDrawsLogged = 16;
constexpr uint32_t kD3d9ConstRegs = 256;

// Vertex shader object field offsets, read out of the consuming guest code.
constexpr uint32_t kVsCodeAllocAt = 0x20;      // code allocation pointer
constexpr uint32_t kVsPatchHeaderAt = 0x368;   // constant/patch header
constexpr uint32_t kVsPatchOffsetAt = 0x14;    // -> the patch block

// Why a draw could not be fully described from the state shadow. Ordered so the
// report reads as a pipeline: what is bound, then what it points at.
enum DrawGap : uint32_t {
  kGapDeclaration = 0,  // no declaration bound yet
  kGapLayout,           // the declaration bound does not decode
  kGapStream,           // a stream the layout uses was never set
  kGapStreamStride,     // that stream's stride is 0
  kGapIndexBuffer,      // an indexed draw with no index buffer
  kGapVertexShader,
  kGapPixelShader,
  kGapViewport,
  kGapRenderState,      // one of the eight output-merger states never set
  kDrawGapCount,
};

enum class ShaderApplyResult : uint8_t { kApplied, kNoCode, kFailed };

// ---- Shared types ---------------------------------------------------------

// "Bind stream 0 to this pointer with this stride and draw" -- the whole of
// what DrawVerticesUP adds over DrawVertices. Synthesised into stream 0 rather
// than carving a second path through BuildHleDraw.
struct UpVertexData {
  uint32_t address = 0;
  uint32_t stride = 0;
  uint32_t size_bytes = 0;
};

struct PatchPrediction {
  uint32_t dest_addr = 0;
  uint32_t pred[3] = {};
  bool bound = false;
};

struct ResolvedTargetByAddress {
  uint32_t dest_object = 0;
  uint32_t source_object = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  // How far into the destination the resolves recorded here actually reach. A
  // destination is only worth sampling as a snapshot if the GPU wrote most of
  // it: the 2048x2048 menu atlas receives exactly ONE resolve in a whole
  // session, a 256x256 blit at (0,0) -- 1.5% of its area -- and the address
  // match then claimed it permanently, handing every draw a surface that is
  // 98.5% clear colour.
  //
  // Reached extent, not summed area: repeated full-surface resolves must not add
  // up past 100%, and a corner blit must not be mistaken for coverage because it
  // happened often.
  uint32_t reached_x = 0;
  uint32_t reached_y = 0;
  uint32_t resolves = 0;

  // REAL COVERAGE, because `reached` is a BOUNDING BOX and a bounding box is not
  // coverage. What it cannot survive is resolves SCATTERED across the surface:
  // the terrain deformation buffer takes 39 blits of 128x32, 3.8% of its
  // 2048x2048, whose bounding box is 29.0%. It cleared the 25% threshold, was
  // claimed, and the 96% no resolve ever touched then sampled 0 where the guest
  // had written the NEUTRAL 0x80 -- dropping the whole terrain by 512/255 =
  // 2.008 world units. That is the floating bike.
  //
  // A bitmask answers both questions at once: overlap saturates instead of
  // summing, and a scatter reports the area it actually covers. 64x64 cells, one
  // bit each: 512 bytes per destination, sized so a cell is 32x32 texels on a
  // 2048 surface -- fine enough that the 128x32 deform blits register.
  static constexpr uint32_t kCoverageGrid = 64;
  static constexpr uint32_t kCoverageWords = kCoverageGrid * kCoverageGrid / 64;
  uint64_t coverage[kCoverageWords] = {};
  // Maintained alongside the bits so the saturation early-out is O(1). Without
  // it a full-surface target pays 4096 cell tests on every one of the ~2000
  // resolves it takes per frame.
  uint32_t covered_cells = 0;

  // Cells per axis and the texel size of one cell, DERIVED rather than stored:
  // width/height are reassigned on every resolve, and a cached grid would be
  // free to drift out of step with them.
  uint32_t cell_w() const {
    return width ? (width + kCoverageGrid - 1) / kCoverageGrid : 0;
  }
  uint32_t cell_h() const {
    return height ? (height + kCoverageGrid - 1) / kCoverageGrid : 0;
  }
  uint32_t cells_x() const {
    const uint32_t c = cell_w();
    return c ? (width + c - 1) / c : 0;
  }
  uint32_t cells_y() const {
    const uint32_t c = cell_h();
    return c ? (height + c - 1) / c : 0;
  }
  uint32_t total_cells() const { return cells_x() * cells_y(); }

  // Mark every cell the rect [x0,x1) x [y0,y1) covers FULLY.
  //
  // Fully, not "touches". Rounding outward is what produced this bug in the first
  // place, and a threshold deciding whether to trust a snapshot must not round in
  // favour of trusting it. Rounding inward would normally cost the far edge, but
  // the clamps below give the last cell back to a resolve that reaches the
  // surface edge.
  void MarkCoverage(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1) {
    const uint32_t nx = cells_x(), ny = cells_y();
    if (!nx || !ny) return;
    if (covered_cells >= nx * ny) return;  // saturated; nothing left to mark
    const uint32_t cw = cell_w(), ch = cell_h();
    if (x1 > width) x1 = width;
    if (y1 > height) y1 = height;
    if (x0 >= x1 || y0 >= y1) return;
    uint32_t cx0 = (x0 + cw - 1) / cw;
    uint32_t cx1 = (x1 >= width) ? nx : x1 / cw;
    uint32_t cy0 = (y0 + ch - 1) / ch;
    uint32_t cy1 = (y1 >= height) ? ny : y1 / ch;
    if (cx1 > nx) cx1 = nx;
    if (cy1 > ny) cy1 = ny;
    for (uint32_t cy = cy0; cy < cy1; ++cy) {
      for (uint32_t cx = cx0; cx < cx1; ++cx) {
        const uint32_t bit = cy * kCoverageGrid + cx;
        uint64_t& word = coverage[bit >> 6];
        const uint64_t mask = 1ull << (bit & 63);
        if (!(word & mask)) {
          word |= mask;
          ++covered_cells;
        }
      }
    }
  }

  // Covered fraction in percent, 0..100. A zero total means the extent was
  // never learned, and the caller must treat that as UNKNOWN rather than as
  // empty -- refusing on absent evidence is the mistake the old rule's
  // "unknown coverage allows the claim" early-return exists to avoid.
  uint32_t coverage_percent() const {
    const uint32_t total = total_cells();
    return total ? uint32_t(uint64_t(covered_cells) * 100u / total) : 0u;
  }
  // How many times the guest handed this destination BACK to SetTexture. A
  // resolve that is produced and never consumed has two completely different
  // causes, and they need opposite fixes:
  //
  //   binds > 0   the guest does ask for it and OUR binding path loses it -- a
  //               fetch constant we describe wrongly, a slot we resolve to
  //               something else, a draw we drop. Fixable here.
  //   binds == 0  the guest never asks for it at all.
  //
  // Counted at SetTexture rather than at draw time on purpose: it must measure
  // whether the GUEST asked, independently of whether our slot resolution then
  // succeeded.
  uint64_t set_texture_binds = 0;
  // What the DRAW path then did with it. set_texture_binds says the guest asked;
  // these three say whether we honoured the ask.
  //
  //   slot_seen      reached ResolvePixelSlotTexture as a known destination
  //   slot_snapshot  ...and was bound to the live host target (the good path)
  //   slot_partial   ...and was sent down the memory-first path instead,
  //                  because the GPU had written under a quarter of it
  //
  // seen == 0 with binds > 0 means the bind never became a draw slot at all.
  uint64_t slot_seen = 0;
  uint64_t slot_snapshot = 0;
  uint64_t slot_partial = 0;
  // WHERE the guest bound it, recorded at SetTexture. `bind==N, seen==0` has
  // exactly two causes and these separate them:
  //
  //   the sampler is one no translated shader fetches from -- the draw loop
  //     iterates the SHADER's declared samplers; or
  //   the bind landed on a different D3DDevice than the one draws are built for,
  //     in which case DeviceState() at draw time never sees it.
  //
  // Compare the device here against the one the WORKING destinations report:
  // this is self-normalising, so it needs no separate record of "the" device.
  uint32_t bind_sampler_mask = 0;
  uint32_t last_bind_device = 0;
  // Draws built while this destination was still bound to a sampler, and which
  // guest samplers those draws' shaders actually FETCH from. The slot loop cannot
  // measure this: it walks the shader's samplers, so a destination bound to a
  // sampler no shader reads produces no failure, no counter and no log line.
  // These three close that blind spot for a `bind>0 seen0` row:
  //
  //   draws_while_bound == 0        the bind is transient -- something rebinds
  //                                 the sampler before any draw is built.
  //   draws_no_translation == most  the consumer IS drawing, as a stand-in.
  //   declared mask lacks the bind  draws happen, translate, and simply never
  //                                 fetch from that sampler.
  uint64_t draws_while_bound = 0;
  uint64_t draws_no_translation = 0;
  uint32_t declared_sampler_mask = 0;
  // The guest thread that last bound this destination. DeviceState() is `static
  // thread_local` -- deliberately, because the guest's three record workers each
  // drive their own D3D9 device on their own thread -- so a SetTexture on thread
  // A is invisible to a draw built on thread B. `bind>0 draws0` is exactly what
  // that looks like, and this field against the draw-thread list below is what
  // proves or disproves it.
  uint32_t last_bind_thread = 0;
  // Guest Draw calls issued while this destination sat on a sampler, summed over
  // every bind window, and how many windows that was. D3D9DrawCounter() is bumped
  // at the guest's Draw entry points before any of our filtering, so
  // `spanned == 0` over many windows means the GUEST never draws with it.
  // `spanned > 0` with slot_seen == 0 means it does, and we drop those draws
  // before the slot loop.
  uint64_t guest_draws_spanned = 0;
  uint64_t bind_windows = 0;
};

struct PendingHleDraw {
  mx::hle::DrawCall draw;
  std::array<mx::hle::HleStream, mx::hle::kMaxStreams> streams;
  std::array<uint32_t, kD3d9ConstRegs * 4> constants;
  mx::hle::PixelTextureBinding texture_binding;
  uint32_t vertex_shader = 0;
  uint32_t device = 0;
  // Which guest thread queued this entry. Recorded at push time, because the
  // whole question this answers is whether the frame's list order is the
  // guest's submission order or just the order three workers won one mutex in.
  uint32_t thread = 0;
  bool have_texture = false;
};

// ---- Draw building --------------------------------------------------------

// Command-buffer record and replay. sub_823F82D0 records vegetation once
// into a guest command buffer and replays it per instance; see the block
// comment above FinishHleDraw for the full chain.
void BeginCmdBufRecording(uint32_t device, uint32_t cmdbuf);
void EndCmdBufRecording(uint32_t device);
uint32_t CmdBufForDevice(uint32_t device);
bool CaptureDrawIfRecording(uint32_t device, mx::hle::DrawCall& dc);
void NoteCmdBufDeferredDraw();
// One block of ALU constants written inside a recorded command buffer.
// sub_82550208 emits these as PM4 type-0 packets whose payload is the data.
struct CmdBufConstOverlay {
  // ALU constant index when !is_fetch (0..255 vertex, 256..511 pixel), or the
  // sampler index when is_fetch.
  uint32_t first_const = 0;
  // A TEXTURE FETCH CONSTANT rather than an ALU one: six dwords describing a
  // texture binding, from Xenos register 0x4800 + sampler*6. The recorded
  // buffers carry 354,640 of these per run, and they are the only reliable
  // source for a replayed draw's textures -- see the note in ReplayCmdBuf.
  bool is_fetch = false;
  // A RAW XENOS REGISTER the buffer programmed, with first_const holding the
  // register index and dwords[0] its value. Only the shader-program registers
  // are collected: the recorded buffers write SQ_PROGRAM_CNTL 51,272 times a
  // run and 27,816 of those enable PARAM_GEN, which neither device reports.
  bool is_reg = false;
  std::vector<uint32_t> dwords;
};

// The constant state in effect at each DRAW_INDX in the buffer, in stream
// order: entry i is everything written before draw i. Ordered, because a
// single flat list applied to every draw gives draw 1 draw 27's transform.
void CollectCmdBufConstants(
    uint32_t cmdbuf, uint8_t* base,
    std::vector<std::vector<CmdBufConstOverlay>>& out);

uint32_t ReplayCmdBuf(uint32_t cmdbuf, uint32_t device, uint8_t* base,
                      const std::vector<std::vector<CmdBufConstOverlay>>& ov);
void ReportCmdBufReplay();

void BuildAndQueueDraw(bool indexed, uint32_t prim_type, uint32_t first,
                       uint32_t count, int32_t base_vertex, uint32_t device,
                       uint8_t* base, const UpVertexData* up = nullptr);
void ScoreDraw(bool indexed, uint32_t first, uint32_t count, uint32_t device,
               uint8_t* base);
bool FinishHleDraw(mx::hle::DrawCall& dc);
void FinalizePendingD3D9DrawsImpl(uint8_t* base);
void NoteQueueThread(uint32_t thread, bool is_resolve);
void NoteResolvePosition(uint32_t dest, size_t index);

extern std::vector<PendingHleDraw> g_pendingHleDraws;

// ---- Guest memory ---------------------------------------------------------

// A VirtualQuery behind an 8-entry MRU region cache. ~6ms per uncached call
// against this process's address space, which was once 100% of native frame
// time -- see docs/renderer_history.md.
bool HostPageReadable(const void* p);
void InvalidateHostPageCache();
uint32_t GpuPhysicalAddress(uint32_t address);

extern std::atomic<uint64_t> g_hprCalls;
extern std::atomic<uint64_t> g_hprQueries;
extern std::atomic<uint64_t> g_hprNanos;

// ---- Vertex declarations --------------------------------------------------

int KnownDeclId(uint32_t p);
int RecordDeclaration(uint32_t decl, bool has_colour, uint32_t elems,
                      const mx::hle::D3D9Element* parsed);
void NoteDrawDeclaration(uint32_t device, uint8_t* base);

extern bool g_declLayoutOk[kMaxTrackedDecls];
extern mx::hle::LayoutError g_declLayoutErr[kMaxTrackedDecls];
// Declarations that arrived after the table was full, and declarations whose
// address was reused by the guest for a different element list. Both used to be
// silent; both cost draws.
extern uint64_t g_declTableFull;
extern uint64_t g_declRebuilt;
extern int g_patchDecl;

// ---- Shaders --------------------------------------------------------------

// ApplyPixelShaderLoadTable repairs the pixel constant bank read off the device:
// overlays the shader's own load tables and replaces the non-finite registers
// the device shadow never held. The raw read alone leaves NaNs in it.
//
// ResolvePixelSlotTexture fills one texture slot of a draw from the DEVICE's
// live texture fetch constants. The replay re-runs this so a replayed draw
// samples the texture bound when it executes, not the one bound while it was
// recorded.
bool ResolvePixelSlotTexture(mx::hle::DrawCall& dc, uint32_t slot,
                             uint32_t guest_sampler, uint32_t device,
                             uint8_t* base, bool vertex = false,
                             uint32_t stage_handle_hint = 0,
                             const uint32_t* fetch_override = nullptr);

void ApplyPixelShaderLoadTable(uint32_t shader, uint32_t device,
                               uint8_t* base,
                               std::vector<uint32_t>& bank);

void NotePixelShaderForDevice(uint32_t device, uint32_t shader);

// The colour and depth surfaces bound on a DEVICE, mirroring the
// thread-local DeviceState copy. Needed because a command-buffer replay
// can run on a thread that never bound one, and a draw with no target is
// filtered out by surface rather than drawn.
void NoteRenderTargetForDevice(uint32_t device,
                               const mx::hle::RenderTargetBinding& rt,
                               bool is_depth);
bool RenderTargetForDevice(uint32_t device, mx::hle::RenderTargetBinding& out,
                           bool is_depth);
void CollectPixelShaderBlob(uint32_t handle, uint8_t* base);
uint32_t ReadPatchFetchCount(uint32_t self, uint32_t variant, uint8_t* base);
void PredictPatchedFetches(uint32_t self, uint32_t dest, uint32_t decl,
                           uint32_t strides, uint32_t variant, uint8_t* base,
                           std::vector<PatchPrediction>& out);
void CapturePatchedCode(uint32_t self, uint32_t dest, uint32_t variant,
                        uint32_t expect_fetches, uint8_t* base);
void CheckPatchedFetches(const std::vector<PatchPrediction>& pred,
                         uint8_t* base);
void SampleFetchConstantFile(uint32_t device, uint8_t* base);

// ---- Resolves and render targets ------------------------------------------

extern std::map<uint32_t, uint32_t> g_resolvedTextureTargets;
extern std::map<uint32_t, ResolvedTargetByAddress> g_resolvedTargetsByAddress;
extern std::map<uint32_t, uint32_t> g_resolveDestObjectPhys;
extern std::map<uint64_t, uint64_t> g_viewportExtents;
extern uint64_t g_resolveDroppedNoSource;

// The 1x1 luminance resolve destination. sub_82AFB8A8 loads those bytes out of
// guest memory rather than sampling them, so a host-only resolve leaves the
// guest dividing by zero -- these track what has been written back.
extern std::map<uint32_t, uint32_t> g_luminanceDestAddrs;
extern uint32_t g_luminanceWroteSeq;
extern uint64_t g_luminanceFloored;

// ---- Video render-target consumption --------------------------------------

// Is the `_VideoRenderTarget` texture the FE_Smoke quad's MATERIAL names ever
// sampled by a draw?
//
// UIVideoComponent keeps the sampled texture and the video's decode target in
// two unrelated fields:
//
//   <VisibleMaterial> -> sub_82388560 -> this+260 -> render slot 15 binds it
//                        onto ImageIconProperties+164.   THE QUAD SAMPLES THIS.
//   <TextureAsset>    -> sub_8236EB30 -> this+664 -> the video PLAYER's
//                        render destination.            THE VIDEO WRITES THIS.
//
// Nothing copies between them. Five of the six video components in MXUI name the
// same asset in both fields; FE_Smoke names `1280_720_VideoRenderTarget` as its
// material and `Smoke_VideoRenderTarget` (1280x430) as its target. The existing
// RESOLVE CONSUMPTION census cannot answer this, because it is keyed on resolve
// DESTINATIONS and the material's texture need never be one.
//
// So this is keyed by the texture's BASE ADDRESS and reports every row. 1280x720
// is also the scene render-target shape, so the address is what distinguishes
// them.
void NoteVideoShapeBind(uint32_t sampler, uint32_t object, const uint32_t* fetch,
                        bool fetch_valid, uint32_t device);
void NoteVideoShapeSlot(const uint32_t* fetch, bool fetch_valid);

// ---- Counters the entry points maintain -----------------------------------

extern uint64_t g_indexed_draws;
extern uint64_t g_draws;
extern uint64_t g_up_draws;
// DrawIndexedVerticesUP (sub_82556110), the fourth entry point. Separate from
// g_indexed_draws so the shape traffic this hook recovers is attributable
// rather than folded into a total that was already non-zero.
extern uint64_t g_indexed_up_draws;
extern uint64_t g_indexed_up_skipped;
extern uint64_t g_decls;
extern uint64_t g_patchCalls;
// ATOMIC because the glyph cache is reached from more than one thread, and the
// guest says so itself: sub_828ADA78 wraps BuildLineGlyphs and the flush in
// RtlEnterCriticalSection(glyphCache + 2540).
//
// g_glyphCacheGeneration is the one that matters for correctness rather than
// reporting: a lost increment leaves the generation unchanged, so
// TextureContentStale says false, the atlas is never re-decoded, and the host
// keeps serving the previous atlas contents -- silently, because the flush
// counter would lose the same update.
extern std::atomic<uint32_t> g_glyphCacheGeneration;








// DrawVerticesUP caller census. lr is the LINK REGISTER at the hook -- the
// return address inside the caller, so it names the call SITE and keeps two
// draws from different points in one function apart.
// kind: 0 = DrawIndexedVertices, 1 = DrawVertices, 2 = DrawVerticesUP.
//
// Extended to all THREE entry points to test one claim: GFx has a bitmap path
// (DrawBitmaps, one site at 0x829E1314) and a SHAPE path, and only the bitmap
// one has ever shown up. Everything missing from the menu is a shape -- panels,
// bar backgrounds, the star widget, and the MASK SHAPE itself -- while
// everything present is a bitmap.
void NoteUpDrawCaller(uint32_t lr, uint32_t verts, uint32_t kind);
void ReportUpDrawCallers();







// The extent of a Scaleform glyph atlas, read off the cache object by the flush
// hook. This is what makes the flush invalidation name the atlases instead of
// every single-channel texture in the game -- see the note by
// g_glyphCacheGeneration. A function rather than an exported container, per the
// rule at the top of this header.
void NoteGlyphCacheGeometry(uint32_t width, uint32_t height);

// Guest D3D9 draw calls. Incremented before anything this layer decides, so it
// is the guest's own call count and nothing else -- ~339 per frame against a
// Xenia reference frame carrying far more. Atomic because the three record
// workers drive it.
extern std::atomic<uint64_t> g_guestDrawCalls;

// Stream-binding recency, for the "was this stream bound for this draw" check.
extern uint32_t g_lastBindD0;
extern uint32_t g_lastBindD1;
extern uint32_t g_lastBindOffset;
extern bool g_haveBind;
extern uint64_t g_drawsSinceBind[mx::hle::kMaxStreams];

// ---- Reporting ------------------------------------------------------------

std::ofstream& DeclFile();
void DumpHleDraw(bool indexed, uint64_t n, uint32_t prim, int32_t base_vertex,
                 uint32_t start, uint32_t count);
void ReportDrawCounts(uint8_t* base);
// Command-buffer replay: the guest submission path that emits PM4 directly
// and touches no D3D9 draw entry point. Defined in hooks_d3d9_entry.cpp.
void ReportCommandBuffers();
// Which vegetation LOD the SpeedTree renderers select, read off the index
// buffer they bind. Defined in hooks_d3d9_entry.cpp.
void ReportVegetationLod();


}  // namespace mx::hooks::d3d9
