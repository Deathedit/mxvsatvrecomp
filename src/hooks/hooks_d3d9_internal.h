// Internal interface of the D3D9 HLE layer.
//
// NOT a public header. It exists so the guest entry points can live in
// hooks_d3d9_entry.cpp while the state and the draw-building machinery they
// drive stay in hooks_d3d9.cpp -- one 9,000-line translation unit was the
// largest file in the project and the one every rendering change has to be made
// in.
//
// The split point is the only cheap one the file has. Everything here was
// internal linkage inside an anonymous namespace until 2026-08-12; measured
// then, a cut at the entry points costs 62 shared symbols, where every other
// candidate boundary cost 68 to 173. The entry points are also the one region
// that was already outside the namespace, because REX_FUNC is extern "C".
//
// Keep this list SHORT. Anything added here is state two translation units can
// now reach, and the reason the original file was hard to reason about is that
// ~150 counters were reachable from anywhere in it. If a new entry point needs
// something not already here, prefer giving it a function to call over
// exporting another variable.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <fstream>
#include <map>
#include <vector>

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

constexpr int kMaxTrackedDecls = 256;
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
  // How far into the destination the resolves recorded here actually reach.
  // A destination is only worth sampling as a snapshot if the GPU wrote most
  // of it. Measured in mx_778: the 2048x2048 menu atlas at phys 0x1A2E3000
  // receives exactly ONE resolve in a whole session, a 256x256 blit at (0,0)
  // -- 1.5% of its area -- and the address match then claimed it permanently,
  // handing every draw that samples it a surface that is 98.5% clear colour
  // and, worse, suppressing the CPU decode that would have re-read guest
  // memory. Reached extent, not summed area: repeated full-surface resolves
  // must not add up past 100%, and a corner blit must not be mistaken for
  // coverage because it happened often.
  uint32_t reached_x = 0;
  uint32_t reached_y = 0;
  uint32_t resolves = 0;
  // How many times the guest handed this destination BACK to SetTexture.
  //
  // The question this exists to answer is the one the backdrop turns on: a
  // resolve that is produced and never consumed has two completely different
  // causes, and they need opposite fixes.
  //
  //   binds > 0   the guest does ask for it and OUR binding path loses it --
  //               a fetch constant we describe wrongly, a slot we resolve to
  //               something else, a draw we drop. Fixable here.
  //   binds == 0  the guest never asks for it at all. Nothing in the host
  //               renderer can make an unrequested texture appear; the missing
  //               consumer is guest-side.
  //
  // Counted at SetTexture rather than at draw time on purpose: it must measure
  // whether the GUEST asked, independently of whether our slot resolution then
  // succeeded. A draw-time counter conflates the two and can only ever report
  // the second question.
  uint64_t set_texture_binds = 0;
  // What the DRAW path then did with it. set_texture_binds says the guest
  // asked; these three say whether we honoured the ask, and they are the only
  // way to tell a binding we lost from one we never received.
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
  //     iterates the SHADER's declared samplers, so a texture bound to a
  //     sampler nobody reads is never looked up; or
  //   the bind landed on a different D3DDevice than the one draws are built
  //     for, in which case DeviceState() at draw time never sees it.
  //
  // Compare the device here against the one the WORKING destinations report:
  // this is self-normalising, so it needs no separate record of "the" device.
  uint32_t bind_sampler_mask = 0;
  uint32_t last_bind_device = 0;
  // Draws built while this destination was still bound to a sampler, and which
  // guest samplers those draws' shaders actually FETCH from.
  //
  // The slot loop cannot measure this: it walks the shader's samplers, so a
  // destination bound to a sampler no shader reads is never looked up and
  // produces no failure, no counter and no log line. These three close that
  // blind spot for a `bind>0 seen0` row:
  //
  //   draws_while_bound == 0        the bind is transient -- something rebinds
  //                                 the sampler before any draw is built.
  //   draws_no_translation == most  the consumer IS drawing, as a stand-in.
  //   declared mask lacks the bind  draws happen, translate, and simply never
  //                                 fetch from that sampler.
  uint64_t draws_while_bound = 0;
  uint64_t draws_no_translation = 0;
  uint32_t declared_sampler_mask = 0;
  // The guest thread that last bound this destination.
  //
  // DeviceState() is `static thread_local` -- deliberately, because the guest's
  // three record workers each drive their own D3D9 device on their own thread.
  // So a SetTexture on thread A is invisible to a draw built on thread B, by
  // design. `bind>0 draws0` is exactly what that looks like from here, and this
  // field against the draw-thread list below is what proves or disproves it.
  uint32_t last_bind_thread = 0;
  // Guest Draw calls issued while this destination sat on a sampler, summed
  // over every bind window, and how many windows that was. D3D9DrawCounter()
  // is bumped at the guest's Draw entry points before any of our filtering, so
  // `spanned == 0` over many windows means the GUEST never draws with it --
  // our draw path is not losing anything. `spanned > 0` with slot_seen == 0
  // means it does, and we drop those draws before the slot loop.
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
extern int g_patchDecl;

// ---- Shaders --------------------------------------------------------------

void NotePixelShaderForDevice(uint32_t device, uint32_t shader);
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
// two unrelated fields, decompiled 2026-08-17:
//
//   <VisibleMaterial> -> sub_82388560 -> this+260 -> render slot 15 binds it
//                        onto ImageIconProperties+164.   THE QUAD SAMPLES THIS.
//   <TextureAsset>    -> sub_8236EB30 -> this+664 -> the video PLAYER's
//                        render destination.            THE VIDEO WRITES THIS.
//
// Nothing copies between them. Five of the six video components in MXUI name
// the same asset in both fields; FE_Smoke names `1280_720_VideoRenderTarget`
// (1280x720) as its material and `Smoke_VideoRenderTarget` (1280x430) as its
// target. The existing RESOLVE CONSUMPTION census cannot answer this, because
// it is keyed on resolve DESTINATIONS and the material's texture need never be
// one -- a different population, which is the trap this file has fallen into
// twice.
//
// So this is keyed by the texture's BASE ADDRESS and reports every row. 1280x720
// is also the scene render-target shape, so a row at that shape is NOT
// self-evidently the video asset; the address is what distinguishes them, and
// printing the whole population is what makes that possible. FE_Smoke's 1280x430
// resolve lands at phys 0x1BE95000.
void NoteVideoShapeBind(uint32_t sampler, uint32_t object, const uint32_t* fetch,
                        bool fetch_valid, uint32_t device);
void NoteVideoShapeSlot(const uint32_t* fetch, bool fetch_valid);

// ---- Counters the entry points maintain -----------------------------------

extern uint64_t g_indexed_draws;
extern uint64_t g_draws;
extern uint64_t g_up_draws;
extern uint64_t g_decls;
extern uint64_t g_patchCalls;
// ATOMIC because the glyph cache is reached from more than one thread, and the
// guest says so itself: sub_828ADA78 wraps BuildLineGlyphs and the flush in
// RtlEnterCriticalSection(glyphCache + 2540), and sub_828AD998 is a second
// wrapper on that same lock. Scaleform does not pay for a critical section on a
// single-threaded path.
//
// g_glyphCacheGeneration is the one that matters for correctness rather than
// for reporting: a lost increment leaves the generation unchanged, so
// TextureContentStale says false, the atlas is never re-decoded, and the host
// keeps serving the previous atlas contents. Stale glyphs, and silent -- the
// flush counter would lose the same update, so the diagnostic under-reports in
// exactly the runs where it happened.
extern std::atomic<uint32_t> g_glyphCacheGeneration;
extern std::atomic<uint64_t> g_glyphCacheFlushes;
// The denominators for g_glyphCacheFlushes. It counts only flushes that carried
// rects, so without these a zero cannot be read: "never called" and "called,
// nothing pending" are the same number. Reported unconditionally, on a cadence
// that fires even when every one of them is zero.
extern std::atomic<uint64_t> g_glyphFlushCalls;
extern std::atomic<uint64_t> g_glyphFlushEmpty;
extern std::atomic<uint64_t> g_glyphFlushRects;
// GetTexture, the glyph chain's only refusal point inside our renderer. A
// failure there makes sub_8293C778 drop that slot's pending rects permanently.
extern std::atomic<uint64_t> g_glyphGetTextureCalls;
extern std::atomic<uint64_t> g_glyphGetTextureFailed;

// Whether the guest is holding the "used this frame" pin on cached glyphs. If
// it is always held, eviction can never succeed and a full atlas refuses every
// new glyph -- absent quads, which is the missing-letters symptom.
extern std::atomic<uint64_t> g_glyphPinModeHeld;
extern std::atomic<uint64_t> g_glyphPinModeReleased;
// The clamp applied before sub_8293E5B8 and the cap it tests against. If the
// clamp is the tighter of the two, that refusal exit cannot fire.
extern std::atomic<uint32_t> g_glyphHeightClamp;
extern std::atomic<uint32_t> g_glyphHeightCap;
// sub_828A8C40's return: the direct count of glyphs asked for and not given.
extern std::atomic<uint64_t> g_glyphRasterCalls;
extern std::atomic<uint64_t> g_glyphRasterRefused;
// Returned success with a null texture -- the loss the return value hides.
// GFx log sinks: the guest naming its own missing glyph. See the hooks.
// sub_828AC620: whether the guest thinks it dropped a glyph from a line.
extern std::atomic<uint64_t> g_lineBuildCalls;
extern std::atomic<uint64_t> g_lineBuildDropped;
extern std::atomic<uint64_t> g_lineBuildCacheFull;
extern std::atomic<uint64_t> g_lineBuildUnread;

extern std::atomic<uint64_t> g_gfxLogCalls;
extern std::atomic<uint64_t> g_gfxMissingGlyph;
extern std::atomic<uint64_t> g_gfxCacheFull;

extern std::atomic<uint64_t> g_glyphRasterSilent;
extern std::atomic<uint64_t> g_glyphRasterUnread;

// The extent of a Scaleform glyph atlas, read off the cache object by the flush
// hook. This is what makes the flush invalidation name the atlases instead of
// every single-channel texture in the game -- see the note by
// g_glyphCacheGeneration. A function rather than an exported container, per the
// rule at the top of this header.
void NoteGlyphCacheGeometry(uint32_t width, uint32_t height);

// Guest D3D9 draw calls, counted in BOTH native and plugin mode.
//
// Every other draw counter in this file lives AFTER
// MX_D3D9_PLUGIN_PASSTHROUGH, so it reads zero under --gpu_plugin=xenos and
// the two modes cannot be compared on it. That comparison is the one that
// matters: plugin mode renders the main-menu backdrop and native does not, and
// native measures 339 guest draw calls per frame against a Xenia reference
// frame carrying far more. Either the guest issues the same work in both --
// in which case our 339 draws produce the wrong image and nothing is missing --
// or it issues less under our hooks, which would mean this layer is changing
// guest behaviour upstream of the renderer. A counter that only runs in one
// mode cannot tell those apart.
//
// Incremented before the passthrough return, so it is the guest's own call
// count and nothing else. Atomic because the three record workers drive it and
// there is no lock on this path in plugin mode.
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

}  // namespace mx::hooks::d3d9
