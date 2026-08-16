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

// ---- Counters the entry points maintain -----------------------------------

extern uint64_t g_indexed_draws;
extern uint64_t g_draws;
extern uint64_t g_up_draws;
extern uint64_t g_decls;
extern uint64_t g_patchCalls;
extern uint32_t g_glyphCacheGeneration;
extern uint64_t g_glyphCacheFlushes;

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
