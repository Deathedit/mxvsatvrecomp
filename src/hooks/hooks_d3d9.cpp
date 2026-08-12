// D3D9 entry-point hooks — observation only.
//
// The title statically links D3D9 v2.0.20209.3, so these functions are in the
// XEX but were nameless until an XDK d3d9.lib was matched against it by bytes
// (see "The D3D9 entry points are located" in AGENTS.md, and
// tools/match_d3d9.py). The control for that match was D3DDevice_Swap, whose
// COMDAT is 0x684 bytes — the exact size of the already-confirmed
// sub_82566B58, and its pattern matched that address and nothing else.
//
// Why this is worth hooking at all: every colour round so far has inferred
// vertex layout from PM4 and shader microcode, because on Xenos a vfetch
// carries format and offset but not semantic. The semantics exist one layer
// up, in the D3DVERTEXELEMENT9 arrays the game hands to
// D3DDevice_CreateVertexDeclaration. They are not static data — four different
// 12-byte D3DDECL_END sentinels return zero matches binary-wide — so the only
// way to see them is to catch them being built.
//
// Everything here passes through to the original and changes no guest state.
// The point of this round is two numbers and one table:
//
//   1. What the declarations actually are.
//   2. Whether the D3D9 draw count matches the translator's transcoded draw
//      count. 53 static call sites are consistent with thousands of draws per
//      frame, but that is an inference; these counters test it.

#include "hooks/hook_common.h"

#include <rex/cvar.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>
#include <fstream>

// For the emitter coverage probe only: emitting HLSL the compiler then rejects
// is exactly as useless as refusing to emit, so the probe compiles what it
// emits. Nothing else in this file touches D3D.
#include <d3dcompiler.h>
#include <wrl/client.h>

// For the vfetch destination swizzle, so the GPU vertex path merges attributes
// into registers by exactly the rule shader_alu.cpp seeds its register file
// with. Two decoders of the same field that disagree is the bug that decoder
// exists to prevent.
#include <rex/graphics/format/ucode.h>

#include "gpu/d3d9_draw.h"
#include "gpu/d3d9_layout.h"
#include "gpu/d3d9_texture.h"
#include "gpu/shader_ucode.h"   // DecodeVertexShaderFetches, VertexAttribute
#include "gpu/shader_alu.h"     // ExecuteVertexShader
#include "gpu/shader_hlsl.h"    // EmitShaderHlsl
#include <cmath>
#include "gpu/d3d9_state.h"
#include "gpu/hle_types.h"      // g_luminanceReadbackBits/Seq
#include "hooks/hooks_d3d9_internal.h"  // shared with hooks_d3d9_entry.cpp

// Defined in src/app/graphics_system.cpp with the rest of the Debug cvars.
REXCVAR_DECLARE(bool, hle_capture);
REXCVAR_DECLARE(bool, hle_diag);

// A NAMED namespace, not an anonymous one, so that the guest entry points can
// move to their own translation unit and still reach the state they operate on.
// Everything here was internal-linkage until 2026-08-12 and is still private to
// the D3D9 HLE layer by convention -- the namespace is the boundary, and
// hooks_d3d9_internal.h is the only place that publishes anything out of it.
namespace mx::hooks::d3d9 {

namespace uc = rex::graphics::ucode;
namespace xn = rex::graphics::xenos;

using mx::hle::DeviceState;

// Declarations are built during load, and the rotating log (3 x 5MB) only
// retains the last ~50 seconds of a 165s run — the first attempt at this probe
// logged every declaration and then lost all of them. Anything created early
// has to go somewhere that does not rotate, so this follows the pm4_dump_*.txt
// convention and writes next to the executable.
std::ofstream& DeclFile() {
  static std::ofstream f("d3d9_dump_decls.txt", std::ios::trunc);
  return f;
}

// D3DVERTEXELEMENT9 is *12 bytes on Xenon*, not the 8 of the PC struct. This
// is not an assumption: both D3DDevice_CreateVertexDeclaration (0x82550B80)
// and XGSetVertexDeclaration (0x82550A90) walk the array with `lhzu r9, 0xC`,
// and XGSetVertexDeclaration copies each element as three dwords. An earlier
// round searched the XEX for the 8-byte PC sentinel, which is one reason it
// found nothing.
//
// Stream is the halfword at offset 0 and terminates the array at 0xFF — that
// much is read directly by both functions. The remaining ten bytes are dumped
// raw rather than decoded, because nothing observed so far pins their layout.
// Both now live in gpu/d3d9_layout.h, which the decoder and its test share.
using mx::hle::kElementSize;
using mx::hle::kMaxElements;   // refuses to walk a runaway array
// A first run hit 23 of a 24 cap, which says nothing about how many exist.
// The dump is a few hundred bytes per declaration and does not rotate, so the
// cap is only here to bound a runaway.
constexpr uint64_t kDrawReportEvery = 2500;  // see the om1 trap in AGENTS.md

uint64_t g_indexed_draws = 0;
uint64_t g_draws = 0;
uint64_t g_up_draws = 0;
uint64_t g_decls = 0;
uint64_t g_patchCalls = 0;

//---------------------------------------------------------------------------
// Finding the active vertex declaration at draw time.
//
// The draw entry points take D3DDevice* in r3 but not the declaration, so it
// has to be read off the device. D3DDevice_SetVertexDeclaration would be the
// obvious hook instead, but it is 20 bytes and under 128 bytes a byte match is
// not evidence — hence reading the device.
//
// **Nothing here dereferences an unknown pointer.** An earlier version walked
// the device treating each dword as a pointer and checking the target for
// XGSetVertexDeclaration's 0x00100005 magic. That crashed the guest with an
// access violation at 0x030013A0: the arena is *not* fully mapped, so a
// speculative read of a garbage value faults. Every identification below is a
// comparison against an object we watched being created.
//---------------------------------------------------------------------------

// Every declaration seen by CreateVertexDeclaration, with what matters about
// it. Ids are creation order.
uint32_t g_declPtr[kMaxTrackedDecls] = {};
uint32_t g_declElems[kMaxTrackedDecls] = {};
bool g_declHasColour[kMaxTrackedDecls] = {};
uint64_t g_declDraws[kMaxTrackedDecls] = {};
int g_declCount = 0;
uint64_t g_drawsNoDecl = 0;      // draws whose declaration we never saw created

// The host input layout each declaration decodes to, built once at creation.
// A declaration that fails to decode keeps `layout_ok = false` and its failure
// reason, so the coverage report can name it rather than count it.
mx::hle::HleInputLayout g_declLayout[kMaxTrackedDecls];
bool g_declLayoutOk[kMaxTrackedDecls] = {};
mx::hle::LayoutError g_declLayoutErr[kMaxTrackedDecls] = {};

int KnownDeclId(uint32_t p) {
  if (!p) return -1;
  for (int i = 0; i < g_declCount; ++i) {
    if (g_declPtr[i] == p) return i;
  }
  return -1;
}

// Called from the CreateVertexDeclaration hook, where both pointers are valid.
// Returns the id, or -1 if the table is full.
int RecordDeclaration(uint32_t decl, bool has_colour, uint32_t elems,
                      const mx::hle::D3D9Element* parsed) {
  if (!decl || g_declCount >= kMaxTrackedDecls) return -1;
  const int existing = KnownDeclId(decl);
  if (existing >= 0) return existing;   // pointer reuse after a free
  const int id = g_declCount++;
  g_declPtr[id] = decl;
  g_declElems[id] = elems;
  g_declHasColour[id] = has_colour;

  g_declLayoutOk[id] = mx::hle::BuildInputLayout(parsed, elems, g_declLayout[id],
                                                 g_declLayoutErr[id]);
  return id;
}

// The current declaration lives at device + 0x2ED8.
//
// **This offset is read out of the library, not searched for.**
// D3DDevice_SetVertexDeclaration is 20 bytes and does nothing but this:
//
//     stw   r4, 0x2ed8(r3)      device->pVertexDeclaration = pDecl
//     ld    r11, 0x10(r3)
//     oris  r11, r11, 0x8       mark the lazy state dirty
//     std   r11, 0x10(r3)
//     blr
//
// D3DDevice_GetVertexDeclaration reads the same field back (`lwz r31,
// 0x2ed8(r3)`), which settles it independently of how the store is read.
//
// Two earlier scans "proved" the declaration was not on the device struct. Both
// covered device + 0..0x2000, and 0x2ED8 is outside that — the scans were
// under-scoped, not the conclusion sound. Scoping a scan by what the struct
// actually spans (SetStreamSource writes +0x3480) was the missing step both
// times, and reading the offset from the code that writes it makes the scan
// unnecessary altogether.
constexpr uint32_t kDeviceVertexDeclaration = 0x2ED8;

// Reading device + 0x2ED8 is safe in a way that dereferencing its *value* is
// not: the device pointer arrives as the draw's own r3, D3D9 is reading the
// same struct on either side of this hook, and the offset is well inside it.
// The value read is only ever compared against declarations we watched
// CreateVertexDeclaration build — never followed.
int g_currentDecl = -1;

// What PatchVertexShaderToMatchVertexDeclaration last saw. Kept only to measure
// how far it lags: it fires on the lazy-state path, ~1 update per 66 draws, and
// the previous round mistook attribution-to-a-stale-value for attribution.
int g_patchDecl = -1;

uint64_t g_declDeviceNull = 0;      // field is 0 — no declaration bound yet
uint64_t g_declDeviceUnknown = 0;   // non-zero, but never seen created
uint64_t g_declAgree = 0;           // device field == the patch hook's value
uint64_t g_declDisagree = 0;        // it does not, i.e. the patch value is stale

// Called from both draw hooks.
void NoteDrawDeclaration(uint32_t device, uint8_t* base) {
  (void)base;
  g_currentDecl = -1;
  if (device) {
    const uint32_t p = REX_LOAD_U32(device + kDeviceVertexDeclaration);
    if (!p) {
      ++g_declDeviceNull;
    } else {
      g_currentDecl = KnownDeclId(p);
      if (g_currentDecl < 0) ++g_declDeviceUnknown;
    }
  }
  if (g_currentDecl >= 0) {
    if (g_currentDecl == g_patchDecl) {
      ++g_declAgree;
    } else {
      ++g_declDisagree;
    }
  }

  DeviceState().current_decl = g_currentDecl;
  if (g_currentDecl < 0) {
    ++g_drawsNoDecl;
    return;
  }
  ++g_declDraws[g_currentDecl];
}

//---------------------------------------------------------------------------
// HleDraw coverage.
//
// The question this round has to answer in writing: at each draw, is the
// description complete? Anything missing is counted under the field that was
// missing, never folded into one "incomplete" total — a renderer built on a
// partial description fails in ways that look like rendering bugs, and by then
// the reason is three layers away.
//
// Nothing here reads guest memory. Every value was captured by the hook that
// set it, at the moment D3D9 was reading the same bytes.
//---------------------------------------------------------------------------

const char* DrawGapName(uint32_t g) {
  switch (g) {
    case kGapDeclaration:  return "no declaration";
    case kGapLayout:       return "declaration does not decode";
    case kGapStream:       return "stream never set";
    case kGapStreamStride: return "stream stride is 0";
    case kGapIndexBuffer:  return "no index buffer";
    case kGapVertexShader: return "no vertex shader";
    case kGapPixelShader:  return "no pixel shader";
    case kGapViewport:     return "no viewport";
    case kGapRenderState:  return "render state never set";
    default:               return "?";
  }
}

uint64_t g_drawGaps[kDrawGapCount] = {};
uint64_t g_drawsComplete = 0;
uint64_t g_drawsChecked = 0;

// Stride disagreements between the declaration and SetStreamSource. The
// layout's own minimum stride cannot exceed the stride the game bound, or the
// last attribute reads past the end of each vertex.
uint64_t g_strideOk = 0;
uint64_t g_strideTooSmall = 0;
uint64_t g_strideMismatch = 0;   // bound stride larger than the layout needs
constexpr int kMaxStrideReports = 8;
int g_strideTooSmallNamed = 0;
int g_vbTooSmallNamed = 0;

// Does the buffer actually hold the vertices the draw asks for? The fetch
// constant's size and the draw's vertex count come from opposite ends of the
// API, so agreeing is evidence that both were read correctly — and this is the
// number the PM4 path had to *infer* the stride from.
uint64_t g_vbFits = 0;
uint64_t g_vbTooSmall = 0;
uint64_t g_ibFits = 0;
uint64_t g_ibTooSmall = 0;

//---------------------------------------------------------------------------
// Stage 0 — why does the vertex range check fail?
//
// 20,125 of 210,799 stream-checks pass, while every one of 66,726 index buffers
// holds its range. Two candidate causes, and this probe separates them.
//
// (a) A second binding path. `?SetStreamSource@D3DDevice@@QAAJIPAUD3DVertexBuffer@@II@Z`
//     exists in blocks.obj beside the `D3DDevice_SetStreamSource` hooked here,
//     as do state-block variants of SetTexture/SetRenderState/SetVertexDeclaration
//     plus D3DStateBlock_Apply. Binds arriving that way never reach the hook and
//     the shadow keeps an older bind. `draws since the last bind` measures it.
//
// (b) Streams are not indexed by a common vertex index. Xenos vertex shaders
//     issue their own vfetch, so a four-entry stream read by something like
//     `index % 4` is legal — and would make the check, not the game, wrong.
//     A per-stream split shows whether the failures are confined to small
//     auxiliary streams.
//
// The device holds the answer either way, because D3D9 writes the bound fetch
// constant into a file on the device. **The offset was meant to be read out of
// SetStreamSource's arithmetic, as `0x2ED8` was.** That failed: the unlinked
// object decodes to `device + StreamNumber*8` for dword0, which collides with
// the lazy-state qword at `+0x10` that SetVertexDeclaration provably uses, so a
// register is being misread and the result must not be built on.
//
// Located empirically instead, by a method that carries its own proof: at
// SetStreamSource we know the exact dwords, so every device offset holding one
// of them is a candidate, and intersecting the candidate sets across many
// different binds leaves only offsets that track the binding. Comparison only —
// no value read out of the device is ever dereferenced.
//---------------------------------------------------------------------------

// **The scan asks the OS whether a page is readable instead of guessing where
// the struct ends.** Two guesses were tried and both faulted at guest
// 0x1D00B000: first 0x4000, then 0x3484 — the latter chosen because
// SetStreamSource writes +0x3480, which proves that offset is mapped for *some*
// device and proves nothing about this one. The arena is sparse; a bound picked
// from a different object is not a bound.
//
// VirtualQuery per 4 KiB page costs one call per page per sample and removes
// the question entirely. The scan stops at the first page that is not
// committed and readable, so it reads exactly as far as memory exists.
constexpr uint32_t kDeviceScanBytes = 0x4000;
constexpr uint32_t kDeviceScanDwords = kDeviceScanBytes / 4;
constexpr uint32_t kHostPageSize = 4096;

// VirtualQuery here was ~100% of native frame time: 502 calls a frame costing
// 3082ms of a 3128ms MainLoop body, measured 2026-08-06. Note the shape — it is
// **~6ms per call**, not a large number of cheap calls. A VirtualQuery is
// normally microseconds; six milliseconds is what it costs against this
// process's address space, and that also explains why a Release build cost
// exactly what Debug did.
//
// The fix is not to call it less often by guesswork. VirtualQuery already
// reports the whole contiguous run it found in mbi.BaseAddress / mbi.RegionSize,
// with identical State and Protect throughout, so one query legitimately answers
// for every address in that range. Cache the region and answer subsequent
// queries from it: the cached answer is exactly what the OS said, not a
// heuristic.
//
// The cache is cleared once per swap (ReportHostPageQueryStats, called from the
// VdSwap hook) so a commit or decommit underneath it is picked up within a
// frame. That matters in one direction specifically: a stale *positive* on a
// decommitted page is a crash, and avoiding exactly that is why this function
// exists. Guest allocations cluster at load, so per-frame is ample.
REXCVAR_DEFINE_BOOL(d3d9_page_cache_verify, false, "Debug",
                    "Verify every page-readability cache hit against a fresh "
                    "VirtualQuery and log mismatches. Slow; correctness check "
                    "for the region cache");

// Measured in real gameplay for the first time in mx_698: 33,043 calls a frame
// collapse to 39-79 VirtualQuery, so the cache works — but those few cost
// 143-298ms of a ~450ms frame, ~3.7ms each. The cost is per CALL, so the only
// thing that helps is missing less often.
//
// Two changes, 2026-08-08. The cache was 8 entries shared by every thread:
//
//  - 8 was too few. The parallel record path (three workers plus the main
//    thread) touches more distinct regions than that, so the round-robin
//    thrashed and re-queried regions it had just evicted.
//  - Shared was unsafe. The three record workers only began running when the
//    fence-retire fix landed (hooks_frame.cpp), and this cache has no lock: a
//    torn read of {base, size, ok} can return a stale positive for a
//    decommitted page, which is the exact crash the function exists to stop.
//
// Per-thread caches fix both at once — no lock, no sharing, and each thread's
// working set is smaller than the union. Invalidation still has to reach every
// thread, so it bumps a generation counter that each thread notices on its next
// call rather than trying to reach into other threads' caches.
struct HostRegionCacheEntry {
  const uint8_t* base = nullptr;
  size_t size = 0;
  bool ok = false;
};
constexpr size_t kHostRegionCacheSize = 64;
thread_local HostRegionCacheEntry t_hprCache[kHostRegionCacheSize];
thread_local size_t t_hprCacheCount = 0;
thread_local size_t t_hprCacheNext = 0;
thread_local uint64_t t_hprGeneration = 0;
// Last readback sequence written into guest memory, so an unchanged value is
// not rewritten every resolve. Only the resolve hook touches it.
uint32_t g_luminanceWroteSeq = 0;
// Destination texture object -> its guest address, for every 1x1 resolve.
// The renderer reports its readbacks by object, which is the only identity
// both sides share; this turns that back into somewhere to write.
std::map<uint32_t, uint32_t> g_luminanceDestAddrs;
std::atomic<uint64_t> g_hprGeneration{1};
std::atomic<uint64_t> g_hprCalls{0};
std::atomic<uint64_t> g_hprQueries{0};
std::atomic<uint64_t> g_hprNanos{0};

// Retention. The per-frame clear exists so that a decommit underneath the cache
// is picked up within a frame: a stale POSITIVE on a decommitted page is a
// crash. But it also means every frame pays first-touch misses for every region
// it uses, which is the whole remaining cost. Guest allocations cluster at load
// and the arena is not torn down mid-scene, so retention across frames is
// cheap and very probably safe — "probably" is why it is a flag and why the
// backstop clear below still runs.
REXCVAR_DEFINE_BOOL(d3d9_page_cache_persist, true, "Debug",
                    "Keep the page-readability cache across frames instead of "
                    "clearing it every swap. Off restores the per-frame clear");
constexpr uint64_t kHostPageCacheBackstopFrames = 300;

bool UncachedHostPageReadable(const void* p) {
  MEMORY_BASIC_INFORMATION mbi = {};
  if (VirtualQuery(p, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
  if (mbi.State != MEM_COMMIT) return false;
  constexpr DWORD kNoRead = PAGE_NOACCESS | PAGE_GUARD;
  return mbi.Protect != 0 && (mbi.Protect & kNoRead) == 0;
}

bool HostPageReadable(const void* p) {
  g_hprCalls.fetch_add(1, std::memory_order_relaxed);
  // Someone invalidated since this thread last looked. Drop our cache rather
  // than reaching into anyone else's.
  const uint64_t gen = g_hprGeneration.load(std::memory_order_relaxed);
  if (t_hprGeneration != gen) {
    t_hprGeneration = gen;
    t_hprCacheCount = 0;
    t_hprCacheNext = 0;
  }
  const auto* addr = static_cast<const uint8_t*>(p);
  for (size_t i = 0; i < t_hprCacheCount; ++i) {
    const auto& e = t_hprCache[i];
    if (addr >= e.base && addr < e.base + e.size) {
      // Paranoid mode: ask the OS anyway and compare. This is the correctness
      // argument for the cache, run rather than asserted. Slow by design.
      if (REXCVAR_GET(d3d9_page_cache_verify)) {
        const bool truth = UncachedHostPageReadable(p);
        if (truth != e.ok) {
          static uint64_t s_bad = 0;
          if (++s_bad <= 40)
            REXLOG_ERROR(
                "d3d9: page cache MISMATCH #{} at {} — cached {}, actual {} "
                "(region {} +{:#x})",
                s_bad, p, e.ok, truth, static_cast<const void*>(e.base),
                e.size);
        }
      }
      return e.ok;
    }
  }

  const auto _t0 = std::chrono::steady_clock::now();
  g_hprQueries.fetch_add(1, std::memory_order_relaxed);
  MEMORY_BASIC_INFORMATION mbi = {};
  const bool queried = VirtualQuery(p, &mbi, sizeof(mbi)) == sizeof(mbi);
  g_hprNanos.fetch_add(
      uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now() - _t0)
                   .count()),
      std::memory_order_relaxed);
  if (!queried) return false;  // no region to cache

  constexpr DWORD kNoRead = PAGE_NOACCESS | PAGE_GUARD;
  const bool ok = mbi.State == MEM_COMMIT && mbi.Protect != 0 &&
                  (mbi.Protect & kNoRead) == 0;

  if (mbi.RegionSize) {
    const size_t slot = t_hprCacheCount < kHostRegionCacheSize
                            ? t_hprCacheCount++
                            : (t_hprCacheNext =
                                   (t_hprCacheNext + 1) % kHostRegionCacheSize);
    t_hprCache[slot] = {static_cast<const uint8_t*>(mbi.BaseAddress),
                        mbi.RegionSize, ok};
  }
  return ok;
}

void InvalidateHostPageCache() {
  // Called once per swap. With persistence on, the backstop still runs so a
  // decommit cannot stay hidden indefinitely.
  static uint64_t s_frames = 0;
  ++s_frames;
  if (REXCVAR_GET(d3d9_page_cache_persist) &&
      (s_frames % kHostPageCacheBackstopFrames) != 0)
    return;
  g_hprGeneration.fetch_add(1, std::memory_order_relaxed);
}

// One bit per dword offset. Starts all-set and is intersected; anything still
// set after many samples held the just-bound value every single time.
bool g_fcCand0[kDeviceScanDwords];
bool g_fcCand1[kDeviceScanDwords];
bool g_fcPrimed = false;
uint32_t g_fcSamples = 0;
uint32_t g_fcReached = kDeviceScanBytes;   // lowest end-of-readable across samples
constexpr uint32_t kFcMaxSamples = 64;

// What the last SetStreamSource for stream 0 bound, kept raw so the scan can
// try the maskings D3D9 might apply rather than assuming one.
uint32_t g_lastBindD0 = 0;
uint32_t g_lastBindD1 = 0;
uint32_t g_lastBindOffset = 0;
bool g_haveBind = false;

// Draws since the last SetStreamSource that touched each stream. If binds are
// arriving through a path this file does not hook, the failing draws sit at
// large values here while the passing ones sit near zero.
uint64_t g_drawsSinceBind[mx::hle::kMaxStreams] = {};
uint64_t g_bindAgeFitSum[mx::hle::kMaxStreams] = {};
uint64_t g_bindAgeFailSum[mx::hle::kMaxStreams] = {};
uint64_t g_bindAgeFailMax[mx::hle::kMaxStreams] = {};

// The vertex range check, split by stream. A bare total cannot distinguish
// "stream 0 geometry is wrong" from "small auxiliary streams are modelled
// wrong", and those need completely different fixes.
uint64_t g_vbFitStream[mx::hle::kMaxStreams] = {};

// Does the device's own fetch constant match what SetStreamSource recorded,
// and where it does not, does the device's value explain a draw the snapshot
// could not? `rescues` is the number that matters: it is how much of the
// shortfall reading the device would recover.
uint64_t g_fileAgree[mx::hle::kMaxStreams] = {};
uint64_t g_fileDiffer[mx::hle::kMaxStreams] = {};
uint64_t g_fileRescues[mx::hle::kMaxStreams] = {};
uint64_t g_vbFailStream[mx::hle::kMaxStreams] = {};

// Indexed draws were never range-checked on the vertex side, because the range
// depends on the index values. Reading them looked safe — the index buffer
// "holds its range" 66,726/66,726 — and it is **off**, because it faults.
//
// Three runs took an access violation at guest 0x1D00B000, and a VirtualQuery
// guard on the device scan did not stop it, which is what identified this read
// rather than that one as the source.
//
// The reason is almost certainly the address decode: SetIndices records
// `address = REX_LOAD_U32(buffer + 0x18) & 0x1FFFFFFF`, and that mask is the
// same one already found wrong for vertex buffers — it clears the top three
// bits rather than the bottom two, so it silently relocates any buffer whose
// address has them set. The 66,726/66,726 result does not contradict this: it
// compares a count against a size and never dereferences the address, so a
// wrong address passes it every time.
//
// Left in place behind this flag rather than deleted: the check is worth having
// once the decode is read out of D3DDevice_SetIndices the way the vertex side
// was. It is not needed for the question Stage 0 is actually asking, because
// only non-indexed draws were ever in the 20,125/210,799 denominator.
constexpr bool kProbeIndexRange = false;
uint64_t g_idxRangeFits = 0;
uint64_t g_idxRangeFails = 0;
uint64_t g_idxRangeUnread = 0;

// Scan the device for dwords matching what was last bound, intersecting into
// the candidate sets. Read-only, bounded, and every read is inside a struct
// D3D9 is itself using on both sides of this hook.
void SampleFetchConstantFile(uint32_t device, uint8_t* base) {
  if (!device || !g_haveBind || g_fcSamples >= kFcMaxSamples) return;

  // The maskings D3D9 might have applied on the way in. Trying several is the
  // point: it avoids assuming which one, and an offset only survives if it
  // matched on every sample.
  const uint32_t want0[4] = {g_lastBindD0, g_lastBindD0 & 0x1FFFFFFFu,
                             g_lastBindD0 + g_lastBindOffset,
                             (g_lastBindD0 + g_lastBindOffset) & 0x1FFFFFFFu};
  const uint32_t want1[2] = {g_lastBindD1, g_lastBindD1 - g_lastBindOffset};

  // How far the scan actually got, so the report can say whether a null result
  // means "not there" or "the struct ended before we looked".
  uint32_t reached = 0;
  for (uint32_t i = 0; i < kDeviceScanDwords; ++i) {
    const uint32_t off = i * 4;
    if ((off & (kHostPageSize - 1)) == 0 || i == 0) {
      if (!HostPageReadable(REX_RAW_ADDR(device + off))) break;
    }
    reached = off + 4;
    const uint32_t v = REX_LOAD_U32(device + off);
    bool hit0 = false;
    for (uint32_t k = 0; k < 4; ++k) hit0 = hit0 || v == want0[k];
    bool hit1 = false;
    for (uint32_t k = 0; k < 2; ++k) hit1 = hit1 || v == want1[k];
    if (!g_fcPrimed) {
      g_fcCand0[i] = hit0;
      g_fcCand1[i] = hit1;
    } else {
      g_fcCand0[i] = g_fcCand0[i] && hit0;
      g_fcCand1[i] = g_fcCand1[i] && hit1;
    }
  }
  // Anything past where this sample could read is not a candidate — leaving it
  // set would let an offset survive on samples that never actually checked it.
  for (uint32_t i = reached / 4; i < kDeviceScanDwords; ++i) {
    g_fcCand0[i] = false;
    g_fcCand1[i] = false;
  }
  if (reached < g_fcReached) g_fcReached = reached;

  g_fcPrimed = true;
  ++g_fcSamples;

  // The scan pinned dword1 to exactly one offset, 0x77C, and that retro-fits
  // SetStreamSource's own arithmetic: `subfic r11, r4, 0x11` — which a first
  // reading dismissed as dead — gives (0x11 - stream) * 8 + 0x6F4 = 0x77C for
  // stream 0. Two independent methods agreeing is what makes this an offset
  // rather than a coincidence.
  //
  // dword0 had no survivor because D3D9 ORs a flag bit in after masking
  // (`rlwinm r11, r11, 0, 19, 19` then `add`), which none of the candidate
  // forms included. Dumping the neighbourhood settles the pair by inspection
  // instead of by another round of guessing at the masking.
  if (g_fcSamples <= 8) {
    auto& f = DeclFile();
    f << "FETCH FILE sample " << g_fcSamples << ": last bind d0=0x" << std::hex
      << g_lastBindD0 << " d1=0x" << g_lastBindD1 << " offset=0x"
      << g_lastBindOffset << "\n           device+0x760..0x790:";
    for (uint32_t o = 0x760; o <= 0x790; o += 4) {
      f << " [" << o << "]=0x" << REX_LOAD_U32(device + o);
    }
    f << std::dec << "\n";
    f.flush();
  }
}

// Where SetStreamSource puts each stream's fetch constant, from the scan above.
// Only dword1 is confirmed; the dump names dword0's slot.
uint32_t FetchFileDword1Offset(uint32_t stream) {
  return 0x6F4 + (0x11 - stream) * 8;
}

//---------------------------------------------------------------------------
// Stage 3 — the vertex shader float constant file.
//
// Read out of D3DDevice_SetVertexShaderConstantFN's own arithmetic
// (shader.obj, and 0x82550320 in the XEX), which is four instructions long
// before it starts storing:
//
//     addi   r10, r4, 0x78          ; StartRegister + 0x78
//     rlwinm r10, r10, 4, 0, 27     ; * 16 — one vec4 per register
//     add    r10, r10, r3           ; + the device
//
// so register N lives at `device + 0x780 + N * 16`. The pixel-shader twin at
// 0x825503F8 is the same function with 0x178 in place of 0x78, giving 0x1780 —
// two 256-register files, 0x1000 bytes each, and they land exactly between the
// vertex fetch constants (which end at 0x780) and the declaration at 0x2ED8.
// Three independently-derived offsets tiling the struct with no overlap is what
// makes this a layout rather than three lucky guesses.
//
// **Not hooked, deliberately.** The device holds the live value whichever path
// wrote it — including the state-block path in blocks.obj that bypasses every
// hook in this file. That is the third time reading the field has beaten
// hooking the setter, after the declaration and the fetch constants.
//---------------------------------------------------------------------------
constexpr uint32_t kDeviceVsConstFile = 0x780;

// Reads kHleProbeRegs vec4s into host order. Bounded by the same page guard the
// device scan uses, and entirely inside a struct D3D9 is using on both sides of
// this hook.
bool ReadVsConstants(uint32_t device, uint8_t* base,
                     float out[mx::hle::kHleProbeRegs * 4]) {
  (void)base;
  if (!device) return false;
  const uint32_t bytes = mx::hle::kHleProbeRegs * 16;
  if (!HostPageReadable(REX_RAW_ADDR(device + kDeviceVsConstFile)) ||
      !HostPageReadable(REX_RAW_ADDR(device + kDeviceVsConstFile + bytes - 4)))
    return false;
  for (uint32_t i = 0; i < mx::hle::kHleProbeRegs * 4; ++i) {
    const uint32_t bits =
        REX_LOAD_U32(device + kDeviceVsConstFile + i * 4);
    std::memcpy(&out[i], &bits, 4);
  }
  return true;
}

//---------------------------------------------------------------------------
// The live viewport, off the device.
//
// `D3DDevice_SetViewport` (0x8254BF50) forwards to sub_8254BCE8, which stores
// six floats and, crucially, **clamps Width and Height against the render
// target** first — the surface extent it reads from `0x24(r9)` bounds
// `X + Width` and `Y + Height` before the store:
//
//   stfs f31, 0x3218(r31)   X
//   stfs f30, 0x321C(r31)   Y
//   stfs f26, 0x3220(r31)   Width    (clamped)
//   stfs f27, 0x3224(r31)   Height   (clamped)
//   stfs f29, 0x3228(r31)   MinZ
//   stfs f28, 0x322C(r31)   MaxZ
//
// That clamp is the whole fix. The argument shadow recorded `65535x65535` on
// 9,130 of ~15,500 calls — a full-surface reset — and last-write-wins meant
// most draws inherited it, so BuildViewportMvp divided by 32767 and collapsed
// every position toward the origin. The device holds what D3D9 actually uses.
//
// Sixth time reading the field has beaten shadowing the call. Same reason each
// time: the device holds the resolved value, whatever path produced it.
//---------------------------------------------------------------------------
constexpr uint32_t kDeviceViewport = 0x3218;

uint64_t g_vpFromDevice = 0, g_vpFromShadow = 0, g_vpDisagreed = 0;

bool ReadDeviceViewport(uint32_t device, uint8_t* base, float out[6]) {
  if (!device) return false;
  if (!HostPageReadable(REX_RAW_ADDR(device + kDeviceViewport)) ||
      !HostPageReadable(REX_RAW_ADDR(device + kDeviceViewport + 20)))
    return false;
  for (uint32_t i = 0; i < 6; ++i) {
    const uint32_t bits = REX_LOAD_U32(device + kDeviceViewport + i * 4);
    std::memcpy(&out[i], &bits, 4);
  }
  // Width and height are the only two this is used for; a zero or non-finite
  // extent is not a viewport and must not become a divide.
  for (uint32_t i = 0; i < 6; ++i)
    if (!std::isfinite(out[i])) return false;
  return out[2] > 0.0f && out[3] > 0.0f;
}

//---------------------------------------------------------------------------
// SQ_PROGRAM_CNTL, and whether the pixel shader's r0 is even an interpolator.
//
// The UV export path reads a *vertex shader export* register named by the
// pixel shader's texture profile. That is only correct while PS r0 is an
// interpolated vertex output. Xenos can instead have the rasterizer generate
// it: SQ_PROGRAM_CNTL bit 18 (`param_gen`) makes PS r0 the screen-space
// position, and no VS export feeds it at all.
//
// The offset is not guessed. `D3DDevice_DrawVertices` (0x825561B0) flushes the
// register with `sub_82564768(device, 0, 8576, device + 10528)` — 8576 is
// 0x2180, SQ_PROGRAM_CNTL, and 10528 is the shadow it sends. Both
// `SetPixelShader` (0x825506E8) and `SetVertexShader` (0x825508A8) reach the
// same word: each walks an AND/OR patch list carried in the shader object and
// applies it to `device + 1152 + offset`, and 10528 - 1152 = 9376 is in range
// of the 16-bit offset those lists use. So the register is per-shader-pair
// state, which is exactly why it has to be read per draw rather than once.
//---------------------------------------------------------------------------
constexpr uint32_t kDeviceSqProgramCntl = 10528;
// SQ_CONTEXT_MISC (0x2181) immediately follows SQ_PROGRAM_CNTL (0x2180) in
// the device register shadow, matching their consecutive PM4 writes.
constexpr uint32_t kDeviceSqContextMisc = kDeviceSqProgramCntl + 4;

// The 0x2200 register block's shadow, from the same flush pattern:
// sub_82564768(device, 0, 8704, device + 10548), 8704 = 0x2200 =
// RB_DEPTHCONTROL. sub_82564768 sends register base+i from shadow + i*4.
constexpr uint32_t kDeviceRegBlock2200 = 10548;
constexpr uint32_t kRegPaClVteCntl = 0x2206;
constexpr uint32_t kDevicePaClVteCntl =
    kDeviceRegBlock2200 + (kRegPaClVteCntl - 0x2200) * 4;

uint64_t g_hleShaderMvpDisagree = 0;
uint64_t g_vteSeen[4] = {};  // [0]=unreadable [1]=scale off [2]=scale on

// True when the GPU applies the viewport scale itself, meaning the vertex
// shader exported clip space. False — including when the register cannot be
// read — means the export is window space and needs the viewport inverse,
// which is the measured case for this game (PA_CL_VTE_CNTL = 0x300) and the
// safe default: it is what the code did unconditionally before.
bool VportScaleEnabled(uint32_t device, uint8_t* base) {
  (void)base;
  if (!device || !HostPageReadable(REX_RAW_ADDR(device + kDevicePaClVteCntl))) {
    ++g_vteSeen[0];
    return false;
  }
  const uint32_t vte = REX_LOAD_U32(device + kDevicePaClVteCntl);
  const bool on = (vte & 1u) != 0;
  ++g_vteSeen[on ? 2 : 1];
  // Offset verification. The derivation says 0x2206 lands here; the captured
  // stream says its value is 0x300. If this slot does not read 0x300 the
  // derivation is wrong, and a dump of the surrounding block says where the
  // register actually is. Logged once per distinct value, capped.
  static std::map<uint32_t, bool> s_vals;
  if (REXCVAR_GET(hle_capture) && s_vals.size() < 4 &&
      s_vals.emplace(vte, true).second) {
    std::string blk;
    for (int32_t d = -32; d <= 32; ++d) {
      const uint32_t at = device + kDevicePaClVteCntl + d * 4;
      if (!HostPageReadable(REX_RAW_ADDR(at))) continue;
      const uint32_t w = REX_LOAD_U32(at);
      if (w) blk += fmt::format(" {:+d}:{:08X}", d, w);
    }
    REXLOG_INFO("d3d9: PA_CL_VTE_CNTL candidate at +{} = {:08X}; nonzero "
                "neighbours (dword offsets):{}",
                kDevicePaClVteCntl, vte, blk);
  }
  return on;
}

struct SqProgramCntl {
  uint32_t raw = 0;
  uint32_t vs_num_reg = 0;
  uint32_t ps_num_reg = 0;
  bool param_gen = false;
  bool gen_index_pix = false;
  uint32_t vs_export_count = 0;
  uint32_t vs_export_mode = 0;
  uint32_t ps_export_mode = 0;
};

bool ReadSqProgramCntl(uint32_t device, uint8_t* base, SqProgramCntl* out) {
  if (!device) return false;
  if (!HostPageReadable(REX_RAW_ADDR(device + kDeviceSqProgramCntl)))
    return false;
  const uint32_t v = REX_LOAD_U32(device + kDeviceSqProgramCntl);
  out->raw = v;
  out->vs_num_reg = v & 0x3F;
  out->ps_num_reg = (v >> 8) & 0x3F;
  out->param_gen = (v & (1u << 18)) != 0;
  out->gen_index_pix = (v & (1u << 19)) != 0;
  out->vs_export_count = (v >> 20) & 0xF;
  out->vs_export_mode = (v >> 24) & 0x7;
  out->ps_export_mode = (v >> 27) & 0x1F;
  return true;
}

struct SqContextMisc {
  uint32_t raw = 0;
  uint32_t param_gen_pos = 0;
};

bool ReadSqContextMisc(uint32_t device, uint8_t* base, SqContextMisc* out) {
  if (!device) return false;
  if (!HostPageReadable(REX_RAW_ADDR(device + kDeviceSqContextMisc)))
    return false;
  const uint32_t v = REX_LOAD_U32(device + kDeviceSqContextMisc);
  out->raw = v;
  out->param_gen_pos = (v >> 8) & 0xFFu;
  return true;
}

// The transform the PM4 path applies today, built from the D3D9 viewport
// instead of from the Xenos context registers. It maps window coordinates to
// clip space.
//
// D3D9's own scale/offset: xs = width/2, xo = x + width/2, and y is flipped.
//
// Prefers the device's clamped copy and falls back to the argument shadow only
// when the device cannot be read, counting which was used — a silent fallback
// to the value that caused the bug would be the worst of both.
bool BuildViewportMvp(uint32_t device, uint8_t* base, float out[16],
                      uint32_t* out_width = nullptr,
                      uint32_t* out_height = nullptr) {
  float dv[6];
  float vx, vy, vw, vh, vminz, vmaxz;
  if (ReadDeviceViewport(device, base, dv)) {
    vx = dv[0]; vy = dv[1]; vw = dv[2]; vh = dv[3];
    vminz = dv[4]; vmaxz = dv[5];
    ++g_vpFromDevice;
    const auto& s = DeviceState().viewport;
    if (s.seen && (float(s.width) != vw || float(s.height) != vh))
      ++g_vpDisagreed;
  } else {
    const auto& v = DeviceState().viewport;
    if (!v.seen || v.width == 0 || v.height == 0) return false;
    vx = float(v.x); vy = float(v.y);
    vw = float(v.width); vh = float(v.height);
    vminz = v.min_z; vmaxz = v.max_z;
    ++g_vpFromShadow;
  }

  const float xs = vw * 0.5f;
  const float xo = vx + xs;
  const float ys = -vh * 0.5f;
  const float yo = vy + vh * 0.5f;
  float zs = vmaxz - vminz;
  const float zo = vminz;
  if (zs == 0.0f) zs = 1.0f;

  if (out_width) *out_width = uint32_t(std::lround(vw));
  if (out_height) *out_height = uint32_t(std::lround(vh));

  static const float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0,
                                      0, 0, 1, 0, 0, 0, 0, 1};
  std::memcpy(out, kIdentity, sizeof(kIdentity));
  out[0]  = 1.0f / xs;  out[3]  = -xo / xs;
  out[5]  = 1.0f / ys;  out[7]  = -yo / ys;
  out[10] = 1.0f / zs;  out[11] = -zo / zs;
  return true;
}

//---------------------------------------------------------------------------
// Stage 2 — build a renderable draw from the description.
//
// The hook owns guest access, so it resolves each buffer to a host pointer and
// hands plain pointers to d3d9_draw.cpp, which stays free of the recompiler
// macros. Every range is bounded by the size D3D9 itself recorded on the
// object.
//---------------------------------------------------------------------------
uint64_t g_badPrimType[64] = {};

// (width << 32) | height -> how many SetViewport calls used it.
std::map<uint64_t, uint64_t> g_viewportExtents;

// D3DDevice_Resolve establishes the explicit EDRAM -> texture relationship.
// Keyed by destination D3D texture object; consumed when that same object is
// subsequently bound through SetTexture. This is deliberately object identity,
// not a guessed match between EDRAM tile base and system-memory address.
std::map<uint32_t, uint32_t> g_resolvedTextureTargets;

// The same relationship keyed by the destination's GUEST MEMORY ADDRESS, so a
// draw that samples a DIFFERENT texture object naming the same memory still
// finds the snapshot. Object identity above cannot see that case, and it is
// what leaves a resolved surface decoding to zeros -- black -- because the CPU
// path then reads memory the GPU wrote and the emulator never populated.
//
// This is NOT the guess the note above refuses. That warning is about matching
// an EDRAM tile base (`color_info & 0xFFF`, a 10 MB tile index) against a
// system-memory address -- two different address spaces. Both sides here are
// the SAME field: `base_address << 12` out of a texture fetch constant, read
// from the destination texture at resolve time and from the sampled texture at
// draw time. Exact equality of one field, not a correspondence between two.
//
// The extent is carried so the match can be refused when it disagrees: guest
// allocators recycle addresses, and a later texture at a freed address must not
// inherit the earlier one's snapshot.
// Destination texture OBJECT -> the physical address its entry is keyed by.
// The object-identity match (g_resolvedTextureTargets) runs BEFORE the address
// match and would otherwise escape the coverage rule below entirely -- which is
// exactly what happened in mx_779: the guard was added to the address path,
// the atlas was claimed by the object path, and the refusal counter read 0.
std::map<uint32_t, uint32_t> g_resolveDestObjectPhys;
// Keyed by PHYSICAL address -- see GpuPhysicalAddress.
std::map<uint32_t, ResolvedTargetByAddress> g_resolvedTargetsByAddress;

// The pixel shader each DEVICE last had bound, shared across threads.
//
// DeviceState() is `static thread_local`, so a draw submitted on a worker
// thread sees ps_seen == false and no shader at all -- the same thread-split
// this file already documents for render targets. Measured on a loaded menu:
// 15,555 draws arrived with no pixel shader handle, including the 21753-index
// rider mesh, and every one of them fell to the tex*col stand-in, which sampled
// the material's PACKED normal/gloss atlas and painted it as if it were albedo.
// That is the magenta-and-green rider.
//
// A pixel shader belongs to the device in D3D9, not to the thread that happened
// to set it, so the device is the right key. device+0x3244 is consulted first
// and this is only the fallback for when that field reads zero.
std::mutex g_pixelShaderByDeviceMu;
std::map<uint32_t, uint32_t> g_pixelShaderByDevice;
uint32_t g_lastPixelShaderAnyDevice = 0;

void NotePixelShaderForDevice(uint32_t device, uint32_t shader) {
  std::lock_guard<std::mutex> lock(g_pixelShaderByDeviceMu);
  // A null shader is a real D3D9 state transition, not a missing observation.
  // Dropping it leaves the previous shader cached forever, so draws made after
  // SetPixelShader(nullptr) inherit a stale program and try to sample its slots.
  if (device) g_pixelShaderByDevice[device] = shader;
  g_lastPixelShaderAnyDevice = shader;
}

// `from_fallback`, when given, reports whether the answer came from THIS
// device's record or from the global last-shader-seen-anywhere fallback below.
//
// The distinction is load-bearing and used to be invisible. A draw whose device
// has no record still gets a plausible handle back, so a mis-attributed pixel
// shader looks exactly like a correct one from the outside. The light-prepass
// draws are the case that exposed it: their device receives SetVertexShader and
// never SetPixelShader, so the pixel half of SQ_PROGRAM_CNTL is empty on those
// draws while the vertex half is set, and the handle returned here is the
// fallback rather than theirs.
uint32_t PixelShaderForDevice(uint32_t device, bool* from_fallback) {
  std::lock_guard<std::mutex> lock(g_pixelShaderByDeviceMu);
  if (device) {
    const auto it = g_pixelShaderByDevice.find(device);
    if (it != g_pixelShaderByDevice.end()) {
      if (from_fallback) *from_fallback = false;
      return it->second;
    }
  }
  if (from_fallback) *from_fallback = true;
  return g_lastPixelShaderAnyDevice;
}

// The same lookup, but ONLY for the device asked about.
//
// The any-device fallback above is a guess across devices, and this title drives
// three of them from three worker threads -- so it can hand a draw a shader that
// was never bound on its device. That is fine for a diagnostic and not fine for
// something that decides which program a third of the frame runs. Kept separate
// rather than changing the existing function, which other callers may want.
uint32_t PixelShaderForDeviceStrict(uint32_t device) {
  if (!device) return 0;
  std::lock_guard<std::mutex> lock(g_pixelShaderByDeviceMu);
  const auto it = g_pixelShaderByDevice.find(device);
  return it != g_pixelShaderByDevice.end() ? it->second : 0;
}
// Every device SetPixelShader has ever been called on, with the shader it last
// received. Rendered for the report below so the devices that SET a shader can
// be read directly against the devices that DRAW without one.
std::string PixelShaderDeviceSummary() {
  std::lock_guard<std::mutex> lock(g_pixelShaderByDeviceMu);
  std::string s;
  for (const auto& [dev, sh] : g_pixelShaderByDevice)
    s += fmt::format(" 0x{:08X}=0x{:08X}", dev, sh);
  return s.empty() ? " (none)" : s;
}

// How often the per-device record is what rescues a draw. Read against
// s_no_shader_no_setter: the gap between them is draws still unattributed.
uint64_t g_psFromDeviceRecord = 0;
// Luminance readings replaced by the floor because the GPU reported exactly
// zero. Should be a handful at startup; a number that keeps climbing means the
// reduction chain is producing nothing and the floor is masking that.
uint64_t g_luminanceFloored = 0;
// Draws whose pixel constant bank held a non-finite value, and how many
// components in total. Reported so the experiment can be read even when the
// picture does not change.

// The same physical page is visible through several virtual windows on this
// console, and the guest uses different ones for the same surface. Measured in
// mx_755: the resolve destinations are 0xBDD20000 and 0xBEDA0000 while the
// draws that sample them describe 0x1DD20000 and 0x1EDA0000 -- identical once
// the window is removed. Comparing the raw `base_address << 12` therefore
// misses every one of them.
//
// The conversion is TRANSCRIBED FROM THE GUEST, not derived. D3DDevice_Resolve
// (0x8255CE98) tail-calls sub_8255BD48, which computes the destination address
// its command packet carries as:
//
//     v55 = base & 0xFFFFF000;
//     v68 = ((v55 >> 20) + 512) & 0x1000;      // conditional +4 KB
//     v70 = v55 & 0x1FFFFFFF;                  // low 512 MB
//     v75 = <dest-point offset> + v68 + v70;
//
// The same idiom appears twice more in that function (the vertex-buffer address
// at v148[2], and v172), so it is the runtime's standard virtual -> GPU
// physical conversion rather than anything specific to one path.
//
// Masking alone was WRONG, and wrong in a way that looked right: for the
// 0xA0000000-window addresses the adjustment term is zero, so plain masking
// matched two of the three surfaces and left the third one page short --
//
//     0xBDD20000 -> 0xDDD & 0x1000 = 0      -> 0x1DD20000   (mask agrees)
//     0xBEDA0000 -> 0xDED & 0x1000 = 0      -> 0x1EDA0000   (mask agrees)
//     0xFA2E2000 -> 0x11A2 & 0x1000 = 0x1000 -> 0x1A2E3000  (mask is 0x1000 low)
//
// -- and 0x1A2E3000 is exactly the address the draw samples for the 2048x2048
// menu-scene atlas. One formula accounts for all three.
uint32_t GpuPhysicalAddress(uint32_t address) {
  const uint32_t page_aligned = address & 0xFFFFF000u;
  return (page_aligned & 0x1FFFFFFFu) +
         (((page_aligned >> 20) + 512u) & 0x1000u);
}

// Resolves that named a destination but were never recorded, because the source
// slot was never seen by SetRenderTarget or SnapshotRenderTarget rejected its
// extent. This was silent, and a resolve lost here is indistinguishable
// downstream from a texture the guest never filled.
uint64_t g_resolveDroppedNoSource = 0;
uint64_t g_resolveAddressMatches = 0;   // blanks rescued by the address match
uint64_t g_resolveAddressExtentMiss = 0;  // same address, different extent
uint64_t g_resolveAddressPartial = 0;  // matched, but barely written by the GPU

// Whether a resolve destination is worth sampling as a snapshot at all.
//
// Extent agreeing, or the object being named by a resolve, is not the same as
// the GPU having WRITTEN the surface. A destination the resolves reach only a
// corner of is mostly clear colour, and claiming it is strictly worse than
// letting the CPU decode run: it paints the same black AND suppresses the
// re-read that would pick the texture up once the guest fills it.
//
// A quarter of the area is the threshold. A genuine render target is resolved
// whole, or in full-width bands that reach the full extent between them, so
// nothing legitimate sits near it -- while the 2048x2048 menu atlas that
// motivated this reaches 256x256, one sixty-fourth.
//
// Unknown coverage allows the claim: a destination whose fetch constant could
// not be read has no entry, and refusing on absent evidence would undo the
// Phase 2 rescue for every surface this measurement missed.
bool ResolvedDestinationIsMostlyWritten(uint32_t dest_object) {
  const auto po = g_resolveDestObjectPhys.find(dest_object);
  if (po == g_resolveDestObjectPhys.end()) return true;
  const auto it = g_resolvedTargetsByAddress.find(po->second);
  if (it == g_resolvedTargetsByAddress.end()) return true;
  const uint64_t reached =
      uint64_t(it->second.reached_x) * it->second.reached_y;
  const uint64_t full = uint64_t(it->second.width) * it->second.height;
  if (!full || reached * 4 >= full) return true;
  ++g_resolveAddressPartial;
  static std::set<uint32_t> s_logged;
  if (s_logged.insert(dest_object).second && s_logged.size() <= 8) {
    REXLOG_INFO("d3d9: resolve dest 0x{:08X} phys 0x{:08X} {}x{} reached only "
                "{}x{} over {} resolves -- not claimed, CPU decode keeps it",
                dest_object, po->second, it->second.width, it->second.height,
                it->second.reached_x, it->second.reached_y,
                it->second.resolves);
  }
  return false;
}

// The destination texture object whose snapshot covers this described texture,
// or 0. Matches on the guest memory address the two fetch constants agree on,
// and refuses when the extents disagree -- an address the guest allocator has
// recycled describes a different texture, and inheriting the old snapshot would
// swap a black surface for a confidently wrong one.
const ResolvedTargetByAddress* ResolvedTargetForAddress(
    const mx::hle::HleTextureSource& described) {
  if (!described.address) return nullptr;
  const uint32_t physical = GpuPhysicalAddress(described.address);
  const auto it = g_resolvedTargetsByAddress.find(physical);
  if (it == g_resolvedTargetsByAddress.end()) {
    // A resolve destination of the SAME extent that starts near this address,
    // but not at it. Reported and deliberately NOT claimed: an atlas built by
    // many small resolves into sub-rects (mx_755 has a 256x256 surface
    // resolving into a 2048x2048 destination) would need an offset-aware
    // sample this path cannot express, and guessing would trade a black
    // texture for a confidently misplaced one.
    static uint64_t s_near = 0;
    for (const auto& [addr, e] : g_resolvedTargetsByAddress) {
      if (e.width != described.width || e.height != described.height) continue;
      const uint32_t delta = physical > addr ? physical - addr : addr - physical;
      if (delta && delta <= 0x10000u && ++s_near <= 8) {
        REXLOG_INFO("d3d9: resolve NEAR-MISS: sampled phys 0x{:08X} {}x{} vs "
                    "resolve dest phys 0x{:08X} (delta 0x{:X}) -- not claimed",
                    physical, described.width, described.height, addr, delta);
      }
    }
    return nullptr;
  }
  if (it->second.width != described.width ||
      it->second.height != described.height) {
    ++g_resolveAddressExtentMiss;
    static uint64_t s_logged = 0;
    if (++s_logged <= 8) {
      REXLOG_INFO("d3d9: resolve phys 0x{:08X} matched but extent differs: "
                  "resolve {}x{} vs sampled {}x{} -- not claimed",
                  physical, it->second.width, it->second.height,
                  described.width, described.height);
    }
    return nullptr;
  }
  if (!ResolvedDestinationIsMostlyWritten(it->second.dest_object)) {
    return nullptr;
  }
  return &it->second;
}

ShaderApplyResult ApplyShaderOutputs(
    mx::hle::DrawCall& dc, uint32_t handle,
    const mx::hle::HleStream* streams, uint32_t device, uint8_t* base,
    const mx::hle::PixelTextureBinding* texture_binding,
    const uint32_t* constant_snapshot = nullptr,
    const mx::hle::HleDrawInputs* deferred_in = nullptr);
bool PrepareDrawTexture(mx::hle::DrawCall& dc, uint32_t pixel_shader,
                        uint32_t device, uint8_t* base,
                        mx::hle::PixelTextureBinding& binding);
void ReportHlslCoverage(mx::hle::HlslStage stage, uint32_t handle,
                        const uint32_t* code, uint32_t count);
// Draws whose 36-byte host vertex was never built because the fetch path was
// predicted, and the two ways that prediction can end. `late` is a wrong guess
// paid for at its ordinary price; `lost` should stay at zero, and means a
// deferred draw reached a caller that could not supply the inputs to fill it.
uint64_t g_transcodeDeferred = 0, g_transcodeLate = 0, g_transcodeLost = 0;

// Are the PER-DRAW diagnostics on?
//
// This session's investigation left a lot of measurement in the hot path, and
// some of it is not cheap: the Stage-3 transform probe alone reads 256 guest
// dwords and scores every vertex, FOR EVERY DRAW, purely to log a ranking
// nothing acts on. Per-FRAME reporting (FRAME COST, the periodic summaries) is
// negligible and stays on unconditionally — only work proportional to draws or
// vertices is gated here.
//
// Default OFF, so a plain run is the fast one and `--hle_diag=1` is what you
// pass to get the counters back. That also makes the cost of the instrumentation
// itself an A/B rather than a rebuild.
// Read once per frame rather than per draw: the cvar lookup is itself the sort
// of per-draw cost this exists to remove. Declared beside the other cvars at
// the top of the file — a REXCVAR_DECLARE inside this anonymous namespace looks
// for namespace-local storage and does not link.
bool g_diag = false;
// The emitted source, kept per shader handle. Shared with every draw that binds
// the shader, so a frame's ~158 draws across a few dozen shaders copy a pointer
// rather than a few kilobytes of text each.
//
// Defined here rather than beside the emitter probe that fills it because
// ApplyShaderOutputs — which is further up the file — now reads the vertex
// stage's input_mask to build the GPU vertex layout.
struct TranslatedShader {
  std::shared_ptr<const std::string> source;  // null unless emitted AND compiled
  uint32_t input_mask = 0;
  uint32_t sampler_mask = 0;
  uint32_t sampler_count = 0;
  uint32_t sampler_array_mask = 0;
  uint32_t slot_guest[mx::hle::HlslShader::kMaxSamplerSlots] = {};
  uint32_t max_const_index = 0;

  // The same vertex shader emitted a second way: performing its own vfetches
  // out of the raw guest vertex buffer, indexed by SV_VertexID. Null when that
  // variant refused or did not compile, in which case the draw stays on the CPU
  // vertex path and `source` above is what runs.
  //
  // Both are kept because they are not interchangeable — the fetch variant has
  // an empty input layout and needs xe_vf[], the other needs an input layout
  // built from input_mask.
  std::shared_ptr<const std::string> fetch_source;
  uint32_t vertex_fetch_count = 0;
  uint32_t vertex_fetch_slot[mx::hle::HlslShader::kMaxVertexFetches] = {};
};
// Vertex fetch translation coverage, keyed by refusal reason.
std::map<std::string, uint64_t> g_vfetchRefused;
uint64_t g_vfetchCompiled = 0;

const TranslatedShader* TranslatedVertexShader(uint32_t handle);
void ProbePixelProfileForDraw(uint32_t pixel_shader, uint32_t device,
                              uint8_t* base,
                              const mx::hle::DrawCall& dc);
void ProbeBinkComposite(uint32_t pixel_shader, uint32_t vertex_shader,
                        uint32_t device, uint8_t* base, uint32_t vertex_count);
bool IsBinkCompositeDraw(uint32_t pixel_shader, uint8_t* base);
bool PrepareBinkPlanes(mx::hle::DrawCall& dc, uint32_t device, uint8_t* base);
bool CopyTexturePhysical(const mx::hle::HleTextureSource& source, uint8_t* base,
                         std::vector<uint8_t>& out);


std::vector<PendingHleDraw> g_pendingHleDraws;
// Defined next to FinalizePendingD3D9DrawsImpl, called from both push sites.
void NoteQueueThread(uint32_t thread, bool is_resolve);
void NoteResolvePosition(uint32_t dest, size_t index);
uint64_t g_pendingQueued = 0, g_pendingApplied = 0, g_pendingDropped = 0;
constexpr size_t kMaxPendingHleDraws = 2048;

// Vertex shader object layout, read out of sub_82565928's VS branch at
// 0x82566234 and cross-checked against the patcher at 0x82564C50. The pixel
// shader twin is ps + 0x18 / ps + 0x40 / info + 0x28 / info + 0x2C (see
// CollectPixelShaderBlob).
constexpr uint32_t kVsInfoOffsetAt = 0x380;    // + variant*8 -> info block
constexpr uint32_t kVsInfoCodeOffset = 0x368;  // CF byte offset in the allocation
constexpr uint32_t kVsInfoCodeSize = 0x36C;    // program length in bytes

uint64_t g_shaderConstOverlays = 0;

// Overlay the constants a vertex shader carries as literal data.
//
// device + 0x780 is only one of two publishers. The other is sub_825656A0,
// called from the draw-time flush, which walks a table in the shader object and
// emits a PM4 LOAD_ALU_CONSTANT (header 0xC0022F00) per entry pointing the GPU
// at literal data inside the shader's own code allocation:
//
//   H = vs + 0x368;  P = H + *(H + 0x14)
//   P + 0x10  u32   list byte length;  entries at P + 0x14
//   entry: u16 reg_index, u16 dword_count, u32 data_offset   (8 bytes)
//   terminated by dword_count == 0
//   source = *(vs + 0x20) + data_offset
//
// Measured: every shader publishes one entry covering c252..c255, holding
// screen-space scale/bias values like (0.5, -0.5, 0, 0) and (0, 1, 0.5, -0.5).
// None of it passes through the device shadow, which is why c255 read as zero
// there and why the one-instruction compositor shaders — MAD on c255 — exported
// (0,0,0,0).
//
// Read from the shader object, not from the ring: the packet only carries an
// address, and that address is guest memory we can read directly. No PM4.
//
// Applied AFTER the device file so a shader literal wins for its own slots.
// That is the hardware order — the load is emitted at draw time, after any
// SetVertexShaderConstantF the app made.
void OverlayShaderConstants(uint32_t shader, uint8_t* base,
                            std::array<uint32_t, kD3d9ConstRegs * 4>& out) {
  (void)base;
  if (!shader) return;
  const uint32_t h = shader + kVsPatchHeaderAt;
  if (!HostPageReadable(REX_RAW_ADDR(h + kVsPatchOffsetAt)) ||
      !HostPageReadable(REX_RAW_ADDR(shader + kVsCodeAllocAt)))
    return;
  const uint32_t rel = REX_LOAD_U32(h + kVsPatchOffsetAt);
  if (!rel || rel >= 0x10000) return;
  const uint32_t pblk = h + rel;
  if (!HostPageReadable(REX_RAW_ADDR(pblk + 0x10))) return;
  const uint32_t bytes = REX_LOAD_U32(pblk + 0x10);
  if (bytes >= 0x10000) return;
  const uint32_t code_alloc = REX_LOAD_U32(shader + kVsCodeAllocAt);

  uint32_t at = pblk + 0x14;
  const uint32_t end = at + bytes;
  while (at + 8 <= end && HostPageReadable(REX_RAW_ADDR(at))) {
    const uint32_t hdr = REX_LOAD_U32(at);
    const uint32_t reg = hdr >> 16, dwords = hdr & 0xFFFF;
    if (!dwords) break;
    const uint32_t data = code_alloc + REX_LOAD_U32(at + 4);
    at += 8;
    if (reg >= kD3d9ConstRegs || dwords > kD3d9ConstRegs * 4 ||
        reg * 4 + dwords > out.size())
      continue;
    for (uint32_t i = 0; i < dwords; ++i) {
      const uint32_t src = data + i * 4;
      if (!HostPageReadable(REX_RAW_ADDR(src))) break;
      out[reg * 4 + i] = REX_LOAD_U32(src);
    }
    ++g_shaderConstOverlays;
  }
}

bool CaptureVertexConstants(uint32_t device, uint8_t* base, uint32_t shader,
                            std::array<uint32_t, kD3d9ConstRegs * 4>& out) {
  const uint32_t bytes = kD3d9ConstRegs * 16;
  if (!device || !HostPageReadable(REX_RAW_ADDR(device + 0x780)) ||
      !HostPageReadable(REX_RAW_ADDR(device + 0x780 + bytes - 4)))
    return false;
  for (uint32_t i = 0; i < out.size(); ++i)
    out[i] = REX_LOAD_U32(device + 0x780 + i * 4);
  OverlayShaderConstants(shader, base, out);
  return true;
}

bool FinishHleDraw(mx::hle::DrawCall& dc) {
  mx::hle::HleSkip skip = mx::hle::HleSkip::kNone;
  if (!mx::hle::FinalizeHleTopology(dc, skip)) {
    ++mx::hle::HleSkipCounts()[uint32_t(skip)];
    return false;
  }
  mx::hle::HleFrameDraws().push_back(std::move(dc));
  return true;
}

void FinalizePendingD3D9DrawsImpl(uint8_t* base);

// D3DDevice_SetFVF (sub_82552420) stores the FVF code at device + 12608 and
// never builds a vertex declaration, so a draw that uses it has no layout for
// BuildHleDraw to read and is dropped as kNoLayout. The Bink composite is
// exactly this case: it sets FVF 0x102 and draws through DrawVerticesUP.
//
// Only the bits this game was measured to use are decoded. An FVF carrying
// anything else returns false rather than guessing, so the draw is still
// counted as kNoLayout and stays visible in the skip histogram.
constexpr uint32_t kDeviceFvf = 12608;

bool BuildFvfLayout(uint32_t fvf, mx::hle::HleInputLayout& out,
                    uint32_t& stride) {
  using namespace mx::hle;
  out = {};
  stride = 0;
  // D3DFVF_XYZ. XYZRHW (0x004) is pre-transformed and would need a different
  // position pipeline, so it is refused rather than silently mistreated.
  if ((fvf & 0x00Eu) != 0x002u) return false;
  {
    HleInputElement e;
    e.semantic_name = "POSITION";
    e.format = DXGI_FORMAT_R32G32B32_FLOAT;
    e.offset = 0;
    e.size_bytes = 12;
    e.usage = 0;  // D3DDECLUSAGE_POSITION
    e.xenos_format = 57;  // xenos::VertexFormat::k_32_32_32_FLOAT
    // The identity swizzle for this component count, as the guest's own
    // decoder produces it — see the table in d3d9_layout.h. Zero is NOT the
    // identity: every selector reads component x, so the element comes back
    // splatted. That is what left the composite's texcoord as (u,u).
    e.swizzle = 0xA88;  // (x,y,z,1)
    out.elements.push_back(e);
    stride = 12;
  }
  if (fvf & 0x040u) {  // D3DFVF_DIFFUSE
    HleInputElement e;
    e.semantic_name = "COLOR";
    e.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    e.offset = stride;
    e.size_bytes = 4;
    e.usage = 10;  // D3DDECLUSAGE_COLOR
    e.xenos_format = 6;  // xenos::VertexFormat::k_8_8_8_8
    e.is_normalized = true;
    e.swizzle = 0x60A;  // (z,y,x,w) — D3DCOLOR is BGRA, wanted as RGBA
    out.elements.push_back(e);
    stride += 4;
  }
  // Texture coordinate count lives in bits 8..11; only the single float2 set
  // the composite uses is handled.
  const uint32_t tex_count = (fvf >> 8) & 0xFu;
  if (tex_count > 1) return false;
  if (tex_count == 1) {
    HleInputElement e;
    e.semantic_name = "TEXCOORD";
    e.format = DXGI_FORMAT_R32G32_FLOAT;
    e.offset = stride;
    e.size_bytes = 8;
    e.usage = 5;  // D3DDECLUSAGE_TEXCOORD
    e.xenos_format = 37;  // xenos::VertexFormat::k_32_32_FLOAT
    e.swizzle = 0xB08;  // (x,y,0,1)
    out.elements.push_back(e);
    stride += 8;
  }
  out.max_stream = 0;
  out.min_stride[0] = stride;
  return true;
}

// Vertex data supplied inline by D3DDevice_DrawVerticesUP rather than through
// a bound stream. That call is semantically "bind stream 0 to this pointer with
// this stride and draw", so it is modelled as exactly that below instead of
// carving a second path through BuildHleDraw.

void BuildAndQueueDraw(bool indexed, uint32_t prim_type, uint32_t first,
                       uint32_t count, int32_t base_vertex, uint32_t device,
                       uint8_t* base, const UpVertexData* up) {
  using namespace mx::hle;
  const auto& st = DeviceState();

  // DIAG (remove before commit): census SQ_PROGRAM_CNTL per VS/PS pair, read
  // AT DRAW TIME.
  //
  // Third site for this log, and the previous two were both wrong in a way that
  // produced confident, WRONG numbers rather than obviously missing ones:
  //   - AttachTranslatedPixelShader: the vertex handle is not set yet, so every
  //     line read "vs 0x00000000" and could not be attributed to a pair.
  //   - ApplyShaderOutputs (vertex attach): both handles are known, but the
  //     register is still being programmed. For ps 0x216AE020 that site read
  //     raw 0x00010002 while the pixel-attach site read raw 0x10210503 for the
  //     same shader -- vs_export_count 0 versus 2. The value CHANGES between
  //     them, so neither belongs to the draw.
  // BuildAndQueueDraw is where the draw is actually issued, so the register has
  // settled. If this reading disagrees with the other two, THIS is the one to
  // trust.
  //
  // vs_export_count is the interpolator count MINUS ONE (SDK registers.h:144).
  {
    SqProgramCntl pc{};
    if (ReadSqProgramCntl(device, base, &pc)) {
      SqContextMisc cm{};
      const bool have_cm = ReadSqContextMisc(device, base, &cm);
      const uint32_t vs_h = st.vs_seen ? st.vertex_shader : 0;
      // Three different answers to "which pixel shader is this draw running",
      // logged side by side because they disagree and the disagreement is the
      // finding:
      //   ps_tl     - DeviceState().pixel_shader. thread_local, and these draws
      //               come off worker threads, so it is empty for them.
      //   ps_strict - THIS device's record only. What the real draw path falls
      //               back to when neither the setter nor device+0x3244 has a
      //               shader, so this is the one that decides what renders.
      //   ps_any    - strict, else the last shader seen on ANY device. A guess
      //               across devices; fine for a diagnostic, wrong for binding.
      // If ps_strict is 0 while ps_any is not, this device never received
      // SetPixelShader and any handle we show for it is borrowed.
      const uint32_t ps_tl = st.ps_seen ? st.pixel_shader : 0;
      const uint32_t ps_strict = PixelShaderForDeviceStrict(device);
      bool ps_was_fallback = false;
      const uint32_t ps_h = PixelShaderForDevice(device, &ps_was_fallback);
      static std::set<uint64_t> s_seen;
      const uint64_t key = (uint64_t(vs_h) << 32) | ps_h;
      if (s_seen.size() < 128 && s_seen.insert(key).second) {
        REXLOG_INFO(
            "d3d9: SQ_PROGRAM_CNTL ATDRAW dev 0x{:08X} vs 0x{:08X} ps 0x{:08X} "
            "(strict 0x{:08X} tl 0x{:08X} fallback {}): raw "
            "0x{:08X} param_gen {} param_gen_pos {} context_misc 0x{:08X} "
            "gen_index_pix {} vs_export_count {} (= {} "
            "interpolators) vs_export_mode {} ps_export_mode {} vs_num_reg {} "
            "ps_num_reg {}",
            device, vs_h, ps_h, ps_strict, ps_tl, ps_was_fallback ? 1 : 0,
            pc.raw, pc.param_gen ? 1 : 0,
            have_cm ? cm.param_gen_pos : 0xFFFFFFFFu,
            have_cm ? cm.raw : 0xFFFFFFFFu, pc.gen_index_pix ? 1 : 0,
            pc.vs_export_count, pc.vs_export_count + 1, pc.vs_export_mode,
            pc.ps_export_mode, pc.vs_num_reg, pc.ps_num_reg);
      }
    }
  }

  // Before any early return. This probe previously sat after BuildHleDraw, so
  // a draw that failed to translate never reached it — the probe was gated on
  // the very thing it exists to diagnose, which is the third time that shape
  // of mistake has cost this branch a round.
  ProbeBinkComposite(st.ps_seen ? st.pixel_shader : 0,
                     st.vs_seen ? st.vertex_shader : 0, device, base, count);
  // Whether this is the video composite has to be known here, not where the
  // planes are gathered, because every early return below is a place the draw
  // can be lost before it gets there. Naming which one is the whole point.
  const bool is_bink =
      IsBinkCompositeDraw(st.ps_seen ? st.pixel_shader : 0, base);
  auto bink_lost = [&](const char* where) {
    if (!is_bink) return;
    static std::map<std::string, uint64_t> s_lost;
    if (++s_lost[where] == 1)
      REXLOG_INFO("d3d9: Bink composite draw dropped at {}", where);
  };

  HleDrawInputs in;
  in.indexed = indexed;
  in.prim_type = prim_type;
  in.first = first;
  in.count = count;
  in.base_vertex = base_vertex;

  const int id = g_currentDecl;
  if (id >= 0 && g_declLayoutOk[id]) in.layout = &g_declLayout[id];

  // A UP draw usually has no declaration — it uses SetFVF instead — so fall
  // back to a layout derived from the FVF the device is holding. Only when a
  // declaration did not already supply one, so nothing that works today
  // changes path.
  mx::hle::HleInputLayout fvf_layout;
  if (up && !in.layout && device &&
      HostPageReadable(REX_RAW_ADDR(device + kDeviceFvf))) {
    const uint32_t fvf = REX_LOAD_U32(device + kDeviceFvf);
    uint32_t fvf_stride = 0;
    if (BuildFvfLayout(fvf, fvf_layout, fvf_stride)) {
      static std::map<uint32_t, uint64_t> s_fvf;
      if (++s_fvf[fvf] == 1) {
        REXLOG_INFO("d3d9: UP draw FVF 0x{:03X} -> {} elements, stride {} "
                    "(draw stride {})",
                    fvf, fvf_layout.elements.size(), fvf_stride, up->stride);
      }
      in.layout = &fvf_layout;
    } else {
      static std::map<uint32_t, uint64_t> s_bad;
      if (++s_bad[fvf] == 1)
        REXLOG_INFO("d3d9: UP draw FVF 0x{:03X} not decoded", fvf);
    }
  }

  HleStream streams[kMaxStreams];
  if (up) {
    // No fetch constant exists for a UP draw, so there is no endian field to
    // read. The bytes were written by the guest CPU and are therefore
    // big-endian; 8in32 is the swap every bound stream in this game carries
    // (measured: endian=2 on all of them), and 32-bit position/colour data is
    // what this path feeds. Stated here rather than buried so that a UP draw
    // rendering as noise has an obvious first suspect.
    streams[0].host =
        reinterpret_cast<const uint8_t*>(REX_RAW_ADDR(up->address));
    streams[0].size_bytes = up->size_bytes;
    streams[0].stride = up->stride;
    streams[0].offset_bytes = 0;
    streams[0].endian = 2;
    streams[0].bound = true;
  } else {
    for (uint32_t i = 0; i < kMaxStreams; ++i) {
      const auto& b = st.stream[i];
      if (!b.bound || !b.address || !b.size_bytes) continue;
      streams[i].host =
          reinterpret_cast<const uint8_t*>(REX_RAW_ADDR(b.address));
      streams[i].size_bytes = b.size_bytes;
      streams[i].stride = b.stride;
      streams[i].offset_bytes = b.offset_bytes;
      streams[i].endian = b.endian;
      streams[i].bound = true;
    }
  }
  in.streams = streams;

  if (indexed && st.index.bound && st.index.address && st.index.size_bytes) {
    in.index.host =
        reinterpret_cast<const uint8_t*>(REX_RAW_ADDR(st.index.address));
    in.index.size_bytes = st.index.size_bytes;
    in.index.is_32bit = st.index.is_32bit;
    in.index.bound = true;
  }

  // The transform the draw is *rendered* with, this stage, is the viewport
  // inverse — the same one the PM4 path uses. That is not a claim that it is
  // right; it is the only transform with evidence behind it today, and it makes
  // the HLE picture directly comparable to the PM4 one on screen. What the
  // constant file says is measured beside it, below, and acted on afterwards.
  float vp[16];
  uint32_t viewport_width = 0, viewport_height = 0;
  const bool have_vp = BuildViewportMvp(device, base, vp, &viewport_width,
                                        &viewport_height);
  // PA_CL_VTE_CNTL disables the hardware X/Y scale and offset for these draws,
  // so a shader output without the inverse viewport cannot be submitted under
  // identity. Unknown is not a usable viewport.
  if (!have_vp) {
    bink_lost("no viewport");
    return;
  }
  if (have_vp) in.mvp = vp;

  // Will this draw fetch its own vertices on the GPU? Asked HERE, before the
  // draw is built, because the answer decides whether to spend a per-vertex
  // pass transcoding a 36-byte host vertex the fetch path never reads — 26-31ms
  // of a menu frame over 289,379 vertices.
  //
  // Only the conditions knowable this early are tested. The pixel shader has
  // not been resolved yet and the per-attribute stream checks need the built
  // vertex range, so the draws those refuse are deferred and then transcoded
  // late, at exactly the cost of having done it now. The two big refusals ARE
  // covered: RECTLIST, which is 85% of draws, and a vertex shader with no fetch
  // variant.
  {
    const uint32_t vs = st.vs_seen ? st.vertex_shader : 0;
    const TranslatedShader* vst = vs ? TranslatedVertexShader(vs) : nullptr;
    in.defer_transcode =
        vst && vst->source && vst->fetch_source && vst->sampler_count == 0 &&
        prim_type != uint32_t(mx::hle::PrimitiveType::kRectangleList) &&
        VportScaleEnabled(device, base);
    if (in.defer_transcode) ++g_transcodeDeferred;
  }

  DrawCall dc;
  HleSkip skip = HleSkip::kNone;
  if (!BuildHleDraw(in, dc, skip)) {
    ++HleSkipCounts()[uint32_t(skip)];
    // Which primitive types are being refused, rather than how many. The bare
    // count says 62% of draws fail and nothing about whether that is one type
    // needing expansion or a wrong prim-type argument.
    if (skip == HleSkip::kBadTopology && prim_type < 64) {
      ++g_badPrimType[prim_type];
    }
    if (is_bink) {
      static uint64_t s_first = 0;
      if (++s_first == 1)
        REXLOG_INFO("d3d9: Bink composite draw dropped in BuildHleDraw, skip={}"
                    " prim={} count={} stride={}",
                    uint32_t(skip), prim_type, count, up ? up->stride : 0u);
    }
    return;
  }
  ++HleBuiltCount();

  // PsParamGen is draw state, not a vertex-shader export. Xenos writes the
  // generated pixel parameters to the register selected by SQ_CONTEXT_MISC,
  // independently of SQ_PROGRAM_CNTL.vs_export_count. Preserve that exact
  // destination for the translated pixel stage. The SDK limits it to the
  // sixteen interpolator registers; malformed state is left disabled.
  {
    SqProgramCntl pc{};
    SqContextMisc cm{};
    if (ReadSqProgramCntl(device, base, &pc) && pc.param_gen &&
        ReadSqContextMisc(device, base, &cm) && cm.param_gen_pos < 16) {
      dc.pixel_param_gen = cm.param_gen_pos + 1;
    }
  }

  // Stage 3's measurement, on the built positions rather than on raw bytes: the
  // vertices are already decoded and in host order here, so the probe scores the
  // same numbers the renderer would receive.
  //
  // The single most expensive diagnostic in the tree: 256 guest dword loads plus
  // a scoring pass over every vertex, per draw, to rank candidate matrices that
  // nothing selects. Its verdict was read long ago; it stays behind hle_diag so
  // it can be re-run rather than deleted.
  if (g_diag) {
    static float consts[kHleProbeRegs * 4];
    if (ReadVsConstants(device, base, consts)) {
      ScoreHleTransform(dc, consts, have_vp ? vp : nullptr,
                        st.vs_seen ? st.vertex_shader : 0);
      // The first few register files in full. A ranking with no numbers behind
      // it cannot be checked by eye, and "c3 col-major, 94%" is worth much less
      // than seeing that c3..c6 look like a projection matrix.
      static uint32_t s_dumped = 0;
      if (s_dumped < 4) {
        ++s_dumped;
        auto& f = DeclFile();
        f << "VS CONSTANTS (draw " << g_draws << ", device+0x780):\n";
        for (uint32_t r = 0; r < 12; ++r) {
          f << "    c" << r << " = " << consts[r * 4 + 0] << " "
            << consts[r * 4 + 1] << " " << consts[r * 4 + 2] << " "
            << consts[r * 4 + 3] << "\n";
        }
        // Empty for a draw whose transcode was deferred to the fetch path;
        // data() is then null and dereferencing it reads address 0.
        if (dc.vertices.size() >= 12) {
          const auto* hp = reinterpret_cast<const float*>(dc.vertices.data());
          f << "    first host position = " << hp[0] << " " << hp[1] << " "
            << hp[2] << "\n";
        } else {
          f << "    first host position = (not transcoded — GPU fetch)\n";
        }
        f.flush();
      }
    }
  }


  // The declaration supplies inputs, not the position the GPU rasterizes.
  // Execute the bound guest shader and replace POSITION with its homogeneous
  // screen-space export before the inverse viewport in dc.mvp is applied.
  PixelTextureBinding texture_binding;
  dc.viewport_width = viewport_width;
  dc.viewport_height = viewport_height;
  const auto& rt = st.render_target[0];
  // Which THREAD saw which render target. DeviceState() is `static
  // thread_local` (d3d9_state.cpp:15), and both this and the Resolve hook read
  // st.render_target[] out of it -- so a draw recorded on a worker thread and a
  // resolve issued on the guest thread can disagree about what the target is.
  // Every missing resolve source measured in mx_759 reported "ever a draw
  // target: NO", which is exactly what that disagreement looks like from the
  // renderer. This pairs the two so they can be compared directly.
  {
    static std::mutex s_mu;
    static std::set<uint64_t> s_seen;
    const uint64_t id = (uint64_t(GetCurrentThreadId()) << 32) | rt.object;
    bool first = false;
    {
      std::lock_guard<std::mutex> lock(s_mu);
      first = s_seen.insert(id).second && s_seen.size() <= 48;
    }
    if (first) {
      REXLOG_INFO("d3d9: DRAW thread {} render target 0x{:08X} {}x{} valid={}",
                  GetCurrentThreadId(), rt.object, rt.width, rt.height,
                  rt.valid);
    }
  }
  if (rt.valid) {
    dc.render_target_object = rt.object;
    dc.render_target_surface_info = rt.surface_info;
    dc.render_target_color_info = rt.color_info;
    // RB_COLOR_INFO carries an EXPONENT BIAS we have never read.
    //
    // Layout, from Xenia's registers.h (the SDK's xenos.h is out of date and is
    // not the reference here): color_base:12, _pad:4, color_format:4 at +16,
    // and int32_t color_exp_bias:6 at +20 -- SIGNED. We take bits [16:19] for
    // the format and discard the bias, so a target the guest asked to be scaled
    // by 2^bias is rendered at 2^0.
    //
    // Xenia applies it as a multiplier on the pixel shader's colour output
    // (d3d12_command_processor.cc, `sc.color_exp_bias[i]`, built as
    // 0x3F800000 + (bias << 23) -- literally 2^bias as a float).
    //
    // Suspected in the rider's gear rendering green: its shader reads the scene
    // snapshot and computes rcp(luminance), which saturates and kills the red
    // channel unless that luminance exceeds 3.42. Measured 0.296, and the scene
    // target is guest colour format 5. A bias of +5 would multiply by exactly
    // 32, taking 0.296 to 9.48 -- and the alpha in that texel, 0.03125, to
    // exactly 1.0. Suggestive, not yet proven: log the field before acting.
    {
      const int32_t bias = int32_t(rt.color_info << 6) >> 26;  // sign-extend :6
      static std::mutex s_biasMutex;
      static std::set<std::pair<uint32_t, uint32_t>> s_biasSeen;
      std::lock_guard<std::mutex> lock(s_biasMutex);
      if (s_biasSeen.emplace(rt.object, rt.color_info).second) {
        REXLOG_INFO(
            "d3d9: RB_COLOR_INFO object 0x{:08X} {}x{} raw 0x{:08X} format {} "
            "exp_bias {} (x{})",
            rt.object, rt.width, rt.height, rt.color_info,
            (rt.color_info >> 16) & 0xFu, bias, std::exp2(float(bias)));
      }
    }
    dc.render_target_width = rt.width;
    dc.render_target_height = rt.height;
    // Reuse the established PM4-facing fields so diagnostics can compare the
    // two independent paths without another parallel vocabulary.
    dc.surface_base = rt.color_info & 0xFFFu;
    dc.surface_pitch = rt.surface_info & 0x3FFFu;
  }
  // The depth surface bound alongside it. Recorded even when the colour target
  // is not, because the two are independent bindings on the guest.
  if (st.depth_stencil.valid) {
    dc.depth_target_object = st.depth_stencil.object;
    dc.depth_target_width = st.depth_stencil.width;
    dc.depth_target_height = st.depth_stencil.height;
    dc.depth_target_base = st.depth_stencil.color_info & 0xFFFu;
  }
  ProbePixelProfileForDraw(st.ps_seen ? st.pixel_shader : 0, device, base, dc);
  // The Bink composite needs its whole plane set, so it takes its own path
  // rather than competing in the single-winner binding contest.
  const uint32_t bound_ps = st.ps_seen ? st.pixel_shader : 0;
  if (IsBinkCompositeDraw(bound_ps, base) &&
      PrepareBinkPlanes(dc, device, base)) {
    // Bink intentionally skips PrepareDrawTexture because its composite needs
    // three or four planes rather than the stand-in path's single winning
    // texture. Capture only the c0 modulation consumed by the dedicated YUV
    // shader. Attaching the whole translated shader here changes pipeline
    // selection: the 640x216 FE_Smoke quad then runs through the general pixel
    // path against its 1280x720 target and becomes a full white rectangle.
    // Keeping pixel_shader_hlsl unset preserves the purpose-built YUV path.
    constexpr uint32_t kPixelConstBase = 0x1780;
    if (device &&
        HostPageReadable(REX_RAW_ADDR(device + kPixelConstBase)) &&
        HostPageReadable(REX_RAW_ADDR(device + kPixelConstBase + 12))) {
      dc.pixel_constants.resize(4);
      for (uint32_t i = 0; i < 4; ++i)
        dc.pixel_constants[i] =
            REX_LOAD_U32(device + kPixelConstBase + i * 4);
    }
  }
  const bool have_texture =
      !dc.yuv_composite &&
      PrepareDrawTexture(dc, bound_ps, device, base, texture_binding);
  if (have_texture && texture_binding.sampler < kMaxSamplers) {
    const auto& sampled_texture = st.texture[texture_binding.sampler];
    const uint32_t texture_object = sampled_texture.object;
    if (const auto it = g_resolvedTextureTargets.find(texture_object);
        it != g_resolvedTextureTargets.end()) {
      dc.sampled_render_target_object = it->second;
      // Which resolve result, not just which surface produced it. See the field
      // comment: several textures resolve out of one shared target.
      dc.sampled_texture_object = texture_object;
      static uint64_t s_resolved_samples = 0;
      if (++s_resolved_samples <= 16 || (s_resolved_samples % 1000) == 0) {
        REXLOG_INFO("d3d9: draw samples resolved texture 0x{:08X} from "
                    "target 0x{:08X}", texture_object, it->second);
      }
    }
  }
  // Carry the two output-merger states this HLE path knows exactly into the
  // same raw fields the PM4 path uses. D3D9 and Xenos both use RGBA bits 0..3
  // for the colour mask. RB_DEPTHCONTROL bits 1/2 are depth-test/write enable;
  // ZWriteEnable is not one of the uniquely identified D3D9 entry points yet,
  // so use D3D9's normal writable-depth mode whenever ZEnable is on. Most
  // importantly, ColorWriteEnable=0 identifies the depth-only passes that the
  // host previously painted opaque white.
  // Read the effective Xenos mask from the device, not merely the setter call
  // observed on this worker. D3DDevice_SetRenderState_ColorWriteEnable stores
  // RB_COLOR_MASK at device+0x28DC after applying the device's active-target
  // gate (confirmed from the XDK-matched function at 0x8254A078). The setter
  // shadow is thread-local, while a device's render state is not, so relying on
  // the shadow lets a draw submitted by another worker inherit the default
  // host write mask and turns depth-only passes into black overpaint.
  constexpr uint32_t kRbColorMask = 0x28DC;  // RB_COLOR_MASK 0x2104
  if (device && HostPageReadable(REX_RAW_ADDR(device + kRbColorMask))) {
    dc.colour_mask = REX_LOAD_U32(device + kRbColorMask) & 0xFu;
    dc.om_seen |= 1u << 0;
  } else if (st.render_state.Seen(kRsColorWriteEnable)) {
    dc.colour_mask = st.render_state.value[kRsColorWriteEnable] & 0xFu;
    dc.om_seen |= 1u << 0;
  }
  if (st.render_state.Seen(kRsZEnable)) {
    if (st.render_state.value[kRsZEnable]) dc.depth_control = (1u << 1) | (1u << 2);
    dc.om_seen |= 1u << 1;
  }
  // Read the effective Xenos equation, not the D3D9-side requested state at
  // device+0x2EF8/0x2EFC. D3DDevice_DrawVertices flushes the 0x2200 register
  // block from device+0x2934, putting RB_BLENDCONTROL0 (0x2201) at +0x2938.
  // This is also how xenia-edge decides host blending: Xenos has no separate
  // RB blend-enable bit, so any equation other than ONE/ZERO/ADD is enabled.
  //
  // Using the D3D9-side bit made the menu's SRC_ALPHA/INV_SRC_ALPHA draw
  // opaque. Its pixel shader deliberately exports transparent black, which
  // should preserve the destination but instead erased the whole backdrop.
  // The thread-local setter shadow remains only as a guarded fallback.
  constexpr uint32_t kRbBlendControl0 = 0x2938;  // RB_BLENDCONTROL0 0x2201
  if (device && HostPageReadable(REX_RAW_ADDR(device + kRbBlendControl0))) {
    const uint32_t packed = REX_LOAD_U32(device + kRbBlendControl0);
    dc.src_blend = packed & 0x1Fu;
    dc.blend_op = (packed >> 5) & 0x7u;
    dc.dest_blend = (packed >> 8) & 0x1Fu;
    const uint32_t alpha_src = (packed >> 16) & 0x1Fu;
    const uint32_t alpha_op = (packed >> 21) & 0x7u;
    const uint32_t alpha_dest = (packed >> 24) & 0x1Fu;
    dc.blend_enable =
        dc.src_blend != 1 || dc.dest_blend != 0 || dc.blend_op != 0 ||
        alpha_src != 1 || alpha_dest != 0 || alpha_op != 0;
    dc.blend_control = packed;
    dc.om_seen |= 1u << 2;

    // This is the exact draw that RenderDoc event 6809 identifies as the first
    // black overpaint in test-2.rdc. Keep the first few occurrences visible so
    // the next run proves that the effective Xenos equation reaches the host.
    static uint32_t s_zero_ps_state_logs = 0;
    if (dc.index_count == 35 && s_zero_ps_state_logs++ < 8) {
      REXLOG_INFO("d3d9: 35-index OM state device 0x{:08X}: "
                  "RB_BLENDCONTROL0 0x{:08X} => enable {} color "
                  "src {} dest {} op {}, alpha src {} dest {} op {}; "
                  "color mask 0x{:X}",
                  device, packed, dc.blend_enable, dc.src_blend,
                  dc.dest_blend, dc.blend_op, alpha_src, alpha_dest, alpha_op,
                  dc.colour_mask);
    }
  } else {
    // Only meaningful when the guest actually enabled it, so the factors are
    // read but not defaulted: an entirely unobserved state stays opaque.
    if (st.render_state.Seen(kRsAlphaBlendEnable)) {
      dc.blend_enable = st.render_state.value[kRsAlphaBlendEnable];
      dc.om_seen |= 1u << 2;
    }
    if (st.render_state.Seen(kRsSrcBlend))
      dc.src_blend = st.render_state.value[kRsSrcBlend];
    if (st.render_state.Seen(kRsDestBlend))
      dc.dest_blend = st.render_state.value[kRsDestBlend];
    if (st.render_state.Seen(kRsBlendOp))
      dc.blend_op = st.render_state.value[kRsBlendOp];
  }

  // The alpha test, from the device's Xenos register shadow. See the note on
  // DrawCall::colour_control for where these two offsets come from.
  {
    constexpr uint32_t kRbColorControl = 0x293C;  // RB_COLORCONTROL 0x2202
    constexpr uint32_t kRbAlphaRef = 0x2904;      // RB_ALPHA_REF    0x210E
    if (device && HostPageReadable(REX_RAW_ADDR(device + kRbColorControl)) &&
        HostPageReadable(REX_RAW_ADDR(device + kRbAlphaRef))) {
      dc.colour_control = REX_LOAD_U32(device + kRbColorControl);
      const uint32_t bits = REX_LOAD_U32(device + kRbAlphaRef);
      std::memcpy(&dc.alpha_ref, &bits, 4);
      dc.alpha_state_seen = true;
      // Logged before anything depends on it: an offset derived from a
      // disassembly is a hypothesis until the values it produces look like the
      // thing they are supposed to be. A reference outside [0,1] or a func
      // above 7 means the offsets are wrong, not that the game is unusual.
      static uint32_t s_logged = 0;
      if (s_logged++ < 6) {
        REXLOG_INFO("d3d9: alpha test RB_COLORCONTROL=0x{:08X} (enable {}, "
                    "func {}) ref={}",
                    dc.colour_control, (dc.colour_control >> 3) & 1u,
                    dc.colour_control & 7u, dc.alpha_ref);
      }
    }
  }

  const uint32_t vertex_shader = st.vs_seen ? st.vertex_shader : 0;
  const ShaderApplyResult applied = ApplyShaderOutputs(
      dc, vertex_shader, streams, device, base,
      have_texture ? &texture_binding : nullptr, nullptr,
      in.defer_transcode ? &in : nullptr);
  if (applied == ShaderApplyResult::kApplied) {
    FinishHleDraw(dc);
    return;
  }
  if (applied != ShaderApplyResult::kNoCode ||
      g_pendingHleDraws.size() >= kMaxPendingHleDraws) {
    if (applied == ShaderApplyResult::kNoCode) ++g_pendingDropped;
    return;
  }

  PendingHleDraw pending;
  pending.draw = std::move(dc);
  std::copy_n(streams, kMaxStreams, pending.streams.begin());
  pending.texture_binding = texture_binding;
  pending.vertex_shader = vertex_shader;
  pending.device = device;
  pending.thread = GetCurrentThreadId();
  NoteQueueThread(pending.thread, false);
  pending.have_texture = have_texture;
  if (!CaptureVertexConstants(device, base, vertex_shader,
                              pending.constants)) {
    ++g_pendingDropped;
    return;
  }
  g_pendingHleDraws.push_back(std::move(pending));
  ++g_pendingQueued;
}

// Records every gap this draw has, rather than stopping at the first, so the
// report says which fields are actually missing across the population instead
// of which one happens to be checked earliest.
void ScoreDraw(bool indexed, uint32_t first, uint32_t count,
               uint32_t device, uint8_t* base) {
  const auto& st = DeviceState();
  ++g_drawsChecked;
  for (uint32_t s = 0; s < mx::hle::kMaxStreams; ++s) ++g_drawsSinceBind[s];
  bool complete = true;
  auto gap = [&](uint32_t g) { ++g_drawGaps[g]; complete = false; };

  const int id = g_currentDecl;
  if (id < 0) {
    gap(kGapDeclaration);
  } else if (!g_declLayoutOk[id]) {
    gap(kGapLayout);
  } else {
    const auto& layout = g_declLayout[id];
    // Only the streams this layout actually reads from matter. A declaration
    // using stream 0 alone says nothing about stream 1 being unset.
    bool used[mx::hle::kMaxStreams] = {};
    for (const auto& e : layout.elements) used[e.stream] = true;
    for (uint32_t s = 0; s < mx::hle::kMaxStreams; ++s) {
      if (!used[s]) continue;
      const auto& b = st.stream[s];
      if (!b.seen || !b.bound) {
        gap(kGapStream);
      } else if (b.stride == 0) {
        gap(kGapStreamStride);
      } else if (b.stride < layout.min_stride[s]) {
        // The layout needs more bytes per vertex than the game bound: the last
        // attribute would read past the end of each vertex. Either the decode
        // is wrong or the stream was bound for a different declaration than the
        // one in force. Counted separately, and the first few are named — a
        // bare count would say nothing about which of the two it is.
        ++g_strideTooSmall;
        complete = false;
        if (g_strideTooSmallNamed < kMaxStrideReports) {
          ++g_strideTooSmallNamed;
          auto& f = DeclFile();
          f << "STRIDE TOO SMALL: declaration id " << id << " stream " << s
            << " needs " << layout.min_stride[s] << " bytes, bound stride is "
            << b.stride << " (vb addr=0x" << std::hex << b.address << std::dec
            << " size=" << b.size_bytes << ")\n";
          for (const auto& e : layout.elements) {
            if (e.stream != s) continue;
            f << "    " << e.semantic_name << e.semantic_index
              << " off=" << e.offset << " size=" << e.size_bytes << "\n";
          }
          f.flush();
        }
      } else if (b.stride != layout.min_stride[s]) {
        ++g_strideMismatch;   // padding at the end of the vertex; legal
      } else {
        ++g_strideOk;
      }

      // For a non-indexed draw the vertex range is known exactly. For an
      // indexed one it depends on the index values — which are readable, since
      // the index buffer provably holds its own range, so the real highest
      // index is used rather than skipping the check.
      uint32_t hi_vertex = 0;
      bool have_range = false;
      if (!indexed) {
        hi_vertex = first + count;
        have_range = true;
      } else if (kProbeIndexRange && st.index.bound && st.index.address &&
                 !st.index.is_32bit) {
        // Bounded by the index buffer's own size, which the previous round
        // verified holds for every indexed draw.
        const uint64_t end = static_cast<uint64_t>(first + count) * 2;
        if (end <= st.index.size_bytes) {
          uint32_t hi = 0;
          for (uint32_t i = 0; i < count; ++i) {
            const uint32_t v = REX_LOAD_U16(st.index.address + (first + i) * 2);
            if (v > hi) hi = v;
          }
          hi_vertex = hi + 1;
          have_range = true;
        }
      }

      if (indexed && !have_range) ++g_idxRangeUnread;

      // **The decisive comparison.** If binds are reaching the device through a
      // path this file does not hook, the device's own fetch constant will
      // differ from the snapshot SetStreamSource recorded — and the size is the
      // field the range check actually depends on.
      {
        const uint32_t d1 = REX_LOAD_U32(device + FetchFileDword1Offset(s));
        const uint32_t live = ((d1 >> 2) & 0xFFFFFF) * 4;
        if (live == b.size_bytes) {
          ++g_fileAgree[s];
        } else {
          ++g_fileDiffer[s];
          // Does the device's size explain a draw the snapshot could not?
          if (b.stride && static_cast<uint64_t>(hi_vertex) * b.stride <= live) {
            ++g_fileRescues[s];
          }
        }
      }

      if (have_range && b.stride) {
        const uint64_t need = static_cast<uint64_t>(hi_vertex) * b.stride;
        const bool fits = need <= b.size_bytes;
        if (indexed) {
          (fits ? g_idxRangeFits : g_idxRangeFails) += 1;
        }
        // Bind age, split the same way: a stale shadow shows up as failing
        // draws sitting far from their last bind while passing ones sit near it.
        const uint64_t age = g_drawsSinceBind[s];
        if (fits) {
          g_bindAgeFitSum[s] += age;
        } else {
          g_bindAgeFailSum[s] += age;
          if (age > g_bindAgeFailMax[s]) g_bindAgeFailMax[s] = age;
        }
        (fits ? g_vbFitStream[s] : g_vbFailStream[s]) += 1;

        if (!indexed && fits) {
          ++g_vbFits;
        } else if (!indexed) {
          ++g_vbTooSmall;
          if (g_vbTooSmallNamed < kMaxStrideReports) {
            ++g_vbTooSmallNamed;
            auto& f = DeclFile();
            f << "VB DOES NOT HOLD RANGE: declaration id " << id << " stream "
              << s << " start_vertex=" << first << " count=" << count
              << " stride=" << b.stride << " needs " << need << "B, buffer is "
              << b.size_bytes << "B (addr=0x" << std::hex << b.address
              << std::dec << " offset=" << b.offset_bytes
              << " endian=" << b.endian << ")\n";
            f.flush();
          }
        }
      }
    }
  }

  if (indexed) {
    if (!st.index.seen || !st.index.bound) {
      gap(kGapIndexBuffer);
    } else {
      const uint64_t need =
          static_cast<uint64_t>(first + count) * (st.index.is_32bit ? 4 : 2);
      (need <= st.index.size_bytes ? g_ibFits : g_ibTooSmall) += 1;
    }
  }
  if (!st.vs_seen) gap(kGapVertexShader);
  if (!st.ps_seen) gap(kGapPixelShader);
  if (!st.viewport.seen) gap(kGapViewport);

  // BlendFactor has zero call sites in this title, so requiring it would mark
  // every draw incomplete for a state the game never uses. The other seven are
  // required.
  for (uint32_t r = 0; r < mx::hle::kRenderStateCount; ++r) {
    if (r == mx::hle::kRsBlendFactor) continue;
    if (!st.render_state.Seen(r)) { gap(kGapRenderState); break; }
  }

  if (complete) ++g_drawsComplete;
}

//---------------------------------------------------------------------------
// Stage A is GONE (2026-08-06), and this note is its epitaph because the
// question it answered still matters.
//
// It located the vertex microcode *inside* the blob by searching for what PM4
// had decoded from the ring — `DecodeVertexShaderFetches` needs an array
// starting at the control-flow section ("the blob carries no header saying
// so", shader_ucode.cpp:396), and no UCODE header parser exists in this tree
// or in the SDK at rex/graphics/format/ucode.h.
//
// That search is unnecessary now. `CapturePatchedCode` takes the microcode
// straight out of the command-ring destination inside the PatchVertexShader
// hook and records `code_off` by decoding it, so the offset is known rather
// than found by comparison — and stageG measured that route at 100% of draws
// against the search's 0%. The vertex blob at +0x368 is no longer collected at
// all; only the pixel-shader blob below is still read.
//
// If a future change needs the offset again, use `g_patchedCode`, not a
// content search. The search was only ever a way to work around not having
// the code.
//---------------------------------------------------------------------------
constexpr uint32_t kMaxBlobDwords = 4096;   // 16 KB ceiling on one blob

std::map<uint32_t, std::vector<uint32_t>> g_psBlobs;



constexpr uint32_t kPhysProbeDwords = 256;
//---------------------------------------------------------------------------
// Stage C — execute the shader and see where the position lands.
//
// Everything needed is now located: the microcode at SH_pPhysical + 0x40, the
// constants at device + 0x780, and an interpreter that is already validated.
// The number this exists to produce is the in-clip fraction from *running the
// guest's code*, against the 55% the best scored constant register managed and
// the 0% the viewport inverse did.
//
// **Two honest bridges, both temporary and both stated.**
//
// 1. The attributes come from PM4's decode of the same shader, not from the
//    declaration. The copy at +0x40 is the unpatched template — its format,
//    offset and stride are blank (Stage B) — and pairing declaration elements
//    to vfetch instructions is a rule this has not read out of
//    PatchVertexShaderToMatchVertexDeclaration yet. Guessing that pairing here
//    would put a second unknown inside the one measurement meant to settle the
//    first.
// 2. Attribute values are read from stream 0. PM4's fetch_slot is a Xenos
//    fetch constant index (95), not a D3D9 stream number, and that mapping is
//    also unread. Draws whose bound stride disagrees with the shader's are
//    skipped and counted rather than read anyway.
//
// Neither bridge affects what the measurement can conclude: if executing the
// shader puts positions in the clip volume, the microcode and the constants are
// right, because nothing else would produce that.
//---------------------------------------------------------------------------
//---------------------------------------------------------------------------
// Stage D2 — where the exported positions land, as a distribution.
//
// Stage C reported one number: 35% inside the clip volume. That number cannot
// be recorded as a result, because a single cutoff cannot tell "the transform
// is right and this geometry is off-screen" from "the transform is wrong by a
// factor of a thousand". Both are simply "not in clip".
//
// So bucket by how far outside it lands. A pile at 1-2 says the first; a pile
// past 100 says the second; an even spread across every bucket says it is not
// a transform at all. The buckets are on x and y only — z has its own near
// plane convention and folding it in would blur the one axis being read.
//
// The viewport inverse gets the identical treatment on the identical vertices.
// Without a reference the buckets are just numbers: it scored 0% under the
// Stage 3 threshold, so what it looks like as a *distribution* is what says
// whether the shader's output is different in kind or merely in degree.
//---------------------------------------------------------------------------
enum ClipBucket : uint32_t {
  kClipIn = 0,      // <= 1: inside, on x and y
  kClipJustOut,     // 1-2:   off-screen, but the same order of magnitude
  kClipOut,         // 2-10
  kClipFarOut,      // 10-100
  kClipWild,        // > 100: the scale is wrong, not the framing
  kClipBehind,      // w <= 0: behind the eye, no meaningful projection
  kClipNonFinite,   // inf/nan
  kClipBucketCount,
};

const char* const kClipBucketName[kClipBucketCount] = {
    "<=1", "1-2", "2-10", "10-100", ">100", "w<=0", "nonfinite"};

uint32_t ClassifyClip(const float p[4]) {
  if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2]) ||
      !std::isfinite(p[3]))
    return kClipNonFinite;
  if (p[3] <= 0.0f) return kClipBehind;
  const float x = std::fabs(p[0] / p[3]);
  const float y = std::fabs(p[1] / p[3]);
  const float d = x > y ? x : y;
  if (!std::isfinite(d)) return kClipNonFinite;
  if (d <= 1.0f)   return kClipIn;
  if (d <= 2.0f)   return kClipJustOut;
  if (d <= 10.0f)  return kClipOut;
  if (d <= 100.0f) return kClipFarOut;
  return kClipWild;
}


//---------------------------------------------------------------------------
// Stage G — execute the shader that was actually bound.
//
// Draws are matched to microcode by >=90% content similarity against PM4's
// cache (g_bestKeyAtDraw). That is a heuristic on two counts: it can pick a
// near-identical wrong variant, and it fails outright on ~63% of draws, so
// every number so far comes from a 37% minority.
//
// The patch hook has the real thing. r4 is where D3D9 writes the patched
// microcode and r3 names the shader — an exact key, no similarity involved.
//
// **The window's start is checked, not assumed.** Vfetch triples land at
// dest + 12*index, so dest is the instruction section and the CF section
// precedes it; Stage A found that gap to be 0x40 bytes. Rather than trust
// that, the capture records the binding table's own vfetch count and the
// decode has to produce exactly that many attributes. A wrong window start
// decodes into plausible nonsense, and this makes that countable instead.
//---------------------------------------------------------------------------
// A first attempt assumed the CF section sat 0x40 bytes before dest, the gap
// Stage A found inside SH_pPhysical. It does not: every decode refused with
// "exec target at address 0", which is the self-check earning its place — a
// wrong start would otherwise have decoded into plausible nonsense.
//
// So the start is *searched* rather than assumed, and the search has a
// verifiable answer: the binding table says how many vfetches this shader has,
// and only the true CF start decodes to exactly that many. Resolved once per
// shader handle and reused, because the offset is a property of the layout.
constexpr uint32_t kPatchWindowBack = 128;   // dwords captured before dest

uint64_t g_vsWindowAgree = 0, g_vsWindowDisagree = 0, g_vsWindowNoField = 0;
uint64_t g_vsWindowAtDest = 0, g_vsWindowEarly = 0, g_vsWindowLate = 0;
uint64_t g_vsWindowLenRejected = 0;

struct PatchedCode {
  std::vector<uint32_t> code;   // host-endian, from dest - kPatchWindowBack*4
  uint32_t expect_fetches = 0;  // what the binding table said
  uint32_t variant = 0;
  uint32_t code_off = 0;        // dwords into `code` where the CF section is
  uint32_t code_len_dwords = 0;  // real program length, 0 = unknown
  bool     resolved = false;    // code_off was found by decoding, not assumed
};

// Winning start, as a signed dword distance from dest. The histogram is the
// finding: one value across every shader means a fixed layout.
std::map<int32_t, uint64_t> g_patchCodeOffsets;
std::map<uint32_t, PatchedCode> g_patchedCode;   // shader handle -> latest

uint32_t ReadPatchFetchCount(uint32_t self, uint32_t variant, uint8_t* base);

// The shader's OWN microcode, for shaders the patch hook never saw.
//
// The patch hook was our only source, and it fires once per upload -- not per
// draw -- so any shader uploaded before we were watching had no program at
// all. That was 41% of draws in mx_711 (164,648 of 401,750), and those draws
// were not merely shaded wrong: ApplyShaderOutputs refused them, so they were
// dropped before the renderer ever saw them (82,324 of 129,004 deferred).
//
// The address is the one this file already computed as `field_abs` and used
// only for reporting. It was verified over 28,000 shaders in mx_715 against
// captures the decode had already proved: every one readable, every one
// agreeing on its first eight dwords, and none differing in more than half its
// program. An earlier reading -- blob rather than *blob -- matched ZERO, which
// is what makes the confirmed one worth trusting rather than merely plausible.
//
// The variant is not known at draw time, so each is tried and accepted only if
// its program decodes to exactly the fetch count the shader's own binding table
// states. That is the same self-verifying rule the ring-window search uses: a
// wrong variant does not produce a slightly-off answer, it fails to decode.
//
// LIMITATION, deliberately not papered over: the guest patches the RING copy,
// so the shader's own allocation keeps the UNPATCHED template. Its vfetch
// instructions carry the template's constants, not ones rewritten to match the
// bound vertex declaration. For these draws that is still strictly better than
// no program at all -- but it is not equivalent to a patch-hook capture, and a
// draw whose declaration disagrees with the template will decode its attributes
// wrongly rather than not at all.
const PatchedCode* CodeFromShaderObject(uint32_t shader, uint8_t* base) {
  if (!shader) return nullptr;
  static std::map<uint32_t, PatchedCode> s_cache;
  static std::map<uint32_t, bool> s_failed;
  if (const auto it = s_cache.find(shader); it != s_cache.end())
    return &it->second;
  if (s_failed.contains(shader)) return nullptr;

  static uint64_t s_tried = 0, s_built = 0;
  ++s_tried;
  for (uint32_t variant = 0; variant < 4; ++variant) {
    const uint32_t info_at = shader + kVsInfoOffsetAt + variant * 8;
    if (!HostPageReadable(REX_RAW_ADDR(info_at)) ||
        !HostPageReadable(REX_RAW_ADDR(shader + kVsCodeAllocAt)))
      continue;
    const uint32_t info = shader + REX_LOAD_U32(info_at);
    if (!HostPageReadable(REX_RAW_ADDR(info + kVsInfoCodeSize))) continue;
    const uint32_t addr =
        REX_LOAD_U32(shader + kVsCodeAllocAt) + REX_LOAD_U32(info + kVsInfoCodeOffset);
    const uint32_t len = REX_LOAD_U32(info + kVsInfoCodeSize);
    if (!addr || !len || (len & 3) || len > 64u * 1024u) continue;

    const uint32_t want = ReadPatchFetchCount(shader, variant, base);
    if (!want) continue;

    PatchedCode pc;
    pc.variant = variant;
    pc.expect_fetches = want;
    pc.code.reserve(len / 4);
    for (uint32_t i = 0; i < len / 4; ++i) {
      const uint32_t at = addr + i * 4;
      if ((at & (kHostPageSize - 1)) == 0 && !HostPageReadable(REX_RAW_ADDR(at)))
        break;
      pc.code.push_back(REX_LOAD_U32(at));
    }
    if (pc.code.size() < 8 || pc.code.size() != len / 4) continue;

    // The program starts here -- confirmed by the first-8-dword agreement over
    // 28,000 shaders -- so unlike the ring window there is no offset to hunt.
    // The decode still has to agree with the binding table, or this is refused.
    pc.code_off = 0;
    std::vector<mx::hle::VertexAttribute> attrs;
    const char* why = nullptr;
    if (!DecodeVertexShaderFetches(pc.code.data(), uint32_t(pc.code.size()),
                                   attrs, &why) ||
        attrs.size() != want)
      continue;
    pc.resolved = true;
    ++s_built;
    if (s_built <= 8 || (s_built % 500) == 0) {
      REXLOG_INFO("d3d9: VS code from the shader object: 0x{:08X} variant {} "
                  "{} dwords, {} fetches; {} built of {} shaders tried",
                  shader, variant, uint32_t(pc.code.size()), want, s_built,
                  s_tried);
    }
    return &(s_cache[shader] = std::move(pc));
  }
  s_failed.emplace(shader, true);
  return nullptr;
}




constexpr float kCtlSpreadEpsilon = 1e-4f;

// Stage I — the same numbers, attributed to the shader that produced them.
//
// Every count above is one percentage over a mixed population, and no one knows
// what that percentage is supposed to be: real scenes cull, draw shadow maps and
// run off-screen passes, so 100% in-clip is wrong and 36% may be right. Four
// independent improvements moved it by nothing and a fifth appeared to move it
// for a reason that cannot have caused it. A number with no target value cannot
// judge a change.
//
// So stop asking how big the failure is and ask *where* it is. If the 29% that
// land in no modelled space come from three shaders, that is a bug with an
// address. If they are spread evenly over forty, the defect is in the model —
// the constants, the interpreter, or the space hypothesis itself — and no amount
// of further input precision will touch it. Both answers are useful; the single
// global percentage can express neither.
struct ShaderScore {
  uint64_t execs = 0;
  uint64_t space[4] = {};                 // indexed by ExportSpace
  uint64_t clip[kClipBucketCount] = {};
  uint64_t in_clip = 0;
  uint64_t degenerate = 0;
  uint64_t draws = 0;
  uint32_t attrs = 0;                     // fetch count, from the binding table
  uint32_t first_dword = 0;               // identity check, with attrs
  uint64_t vp_extent = 0;                 // (w<<32)|h, last seen
  // Range of the position attribute's four components over every execution. A
  // component that never moves is not a coordinate — it is padding, or a
  // homogeneous 1 the shader needs, and reading it as z is a misinterpretation
  // no amount of decode correctness will catch.
  float in_lo[4] = {1e30f, 1e30f, 1e30f, 1e30f};
  float in_hi[4] = {-1e30f, -1e30f, -1e30f, -1e30f};
  uint64_t in_seen = 0;
  // The first execution in full, captured as it happens. Which shader is worst
  // is not known until the report, and by then the vertex is long gone — so
  // every shader records its first, and the report prints the one that earned
  // it. Costs one short string per distinct shader.
  std::string first_exec;
};

// HLE rendering must consume the shader's position export, not the raw
// declaration POSITION that BuildHleDraw initially packs. These counters are
// deliberately separate from the sampling/probe counters below: rendering
// executes every vertex, while hle_shader_exec may sample only a few.
uint64_t g_hleShaderAttempts = 0, g_hleShaderDraws = 0;
uint64_t g_hleShaderVertices = 0;
uint64_t g_hleShaderNoCode = 0, g_hleShaderBadDecode = 0;
uint64_t g_hleShaderBadStream = 0, g_hleShaderBadConstants = 0;
uint64_t g_hleShaderBadVertex = 0;
uint64_t g_hleShaderIdentityMvp = 0, g_hleShaderViewportMvp = 0;

// How many draws qualify for the GPU vertex path. This is the migration's
// bisector: it says what share of the frame can leave the interpreter before
// anything is actually switched over, so a regression can be attributed to the
// switch rather than to the qualifying rule.
uint64_t g_gpuVertexDraws = 0, g_gpuVertexSkipped = 0;
uint64_t g_gpuVertexUndeclared = 0;
// Why a draw was refused the GPU vertex path. "skipped" has only ever been one
// number, so a refusal for a reason we could lift is indistinguishable from one
// we could not. The skinned-mesh question needs exactly this split: a vertex
// shader that samples a texture is refused by `sampler_count == 0` and then
// falls to an interpreter that has no texture fetch at all, so its result is a
// silent zero rather than a fallback.
uint64_t g_gpuVertexNoVs = 0, g_gpuVertexVsSamplers = 0;
uint64_t g_gpuVertexNoVte = 0, g_gpuVertexNoPs = 0, g_gpuVertexTooManyInputs = 0;
// The GPU vertex FETCH path: draws taking it, and why a draw that qualified for
// the GPU vertex stage still could not.
uint64_t g_gpuFetchDraws = 0, g_gpuFetchRectList = 0;
uint64_t g_gpuFetchOrdinalMismatch = 0, g_gpuFetchOutOfRange = 0;
uint64_t g_gpuFetchUnaligned = 0;

// Where the frame goes. The guest's RenderPipeline call is measured whole in
// hooks_gameloop.cpp, which says the frame is slow but not which of our stages
// is spending it. These accumulate per frame and reset each frame, so the
// numbers are a frame's cost rather than a run's.
//
// Deliberately coarse -- three buckets and a total. A finer breakdown is worth
// having only once one bucket is known to dominate.
uint64_t g_phaseVertexUs = 0;   // ApplyShaderOutputs, all of it
uint64_t g_phaseInterpUs = 0;   // the software vertex shader inside it
uint64_t g_phaseTextureUs = 0;  // describe + copy + decode, per draw
uint64_t g_phaseDrawCount = 0;
uint64_t g_phaseVertexLoopUs = 0;  // the per-vertex loop alone

// WHY a draw is still paying for the per-vertex loop, and what that costs.
//
// The aggregate said 144,163 vertices cost 119ms while the 145,216 the fetch
// path took away cost only 24ms -- the vertices left on the CPU are five times
// more expensive EACH than the ones removed. That means the remaining work is
// concentrated in a subset, and widening fetch coverage is only worth doing for
// whichever subset it is. Three counters, incremented once per draw, rather
// than another inference from two aggregate numbers.
enum LoopReason : uint8_t { kLoopRectList = 0, kLoopNoPs = 1, kLoopOther = 2 };
uint64_t g_loopUs[3] = {}, g_loopVerts[3] = {}, g_loopDraws[3] = {};
const char* const kLoopReasonName[3] = {"rectlist", "no-PS", "other"};
uint64_t g_phaseVertexCount = 0;

struct PhaseTimer {
  uint64_t& sink;
  std::chrono::steady_clock::time_point t0;
  explicit PhaseTimer(uint64_t& s)
      : sink(s), t0(std::chrono::steady_clock::now()) {}
  ~PhaseTimer() {
    sink += uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                         std::chrono::steady_clock::now() - t0)
                         .count());
  }
};

uint64_t g_liveVertexResolved = 0, g_liveVertexAmbiguous = 0;
uint64_t g_liveVertexUnreadable = 0, g_liveVertexNoMatch = 0;


ShaderApplyResult ApplyShaderOutputs(
    mx::hle::DrawCall& dc, uint32_t handle,
    const mx::hle::HleStream* streams, uint32_t device, uint8_t* base,
    const mx::hle::PixelTextureBinding* texture_binding,
    const uint32_t* constant_snapshot,
    const mx::hle::HleDrawInputs* deferred_in) {
  using namespace mx::hle;
  PhaseTimer phase_timer(g_phaseVertexUs);
  ++g_phaseDrawCount;
  const uint64_t attempt = ++g_hleShaderAttempts;
  struct ReportApply {
    uint64_t attempt;
    ~ReportApply() {
      if (attempt > 10 && (attempt % 250) != 0) return;
      REXLOG_INFO(
          "d3d9: HLE shader output attempts {}: applied {} draws / {} "
          "vertices; skipped no-code {} decode {} stream {} constants {} "
          "vertex {}; output transform identity {} viewport {} (VTE scale-on "
          "{} off {} unreadable {}, disagrees with old tie-break {}); live "
          "shader resolved {} no-match {} ambiguous {} unreadable {}; GPU "
          "vertex path {} draws qualify, {} skipped ({} undeclared reg, "
          "{} no-VS, {} VS-samplers, {} no-VTE, {} no-PS, "
          "{} too-many-inputs); GPU FETCH {} draws (refused: rectlist {}, "
          "ordinal-mismatch {}, out-of-range {}, unaligned {})",
          g_hleShaderAttempts, g_hleShaderDraws, g_hleShaderVertices,
          g_hleShaderNoCode, g_hleShaderBadDecode, g_hleShaderBadStream,
          g_hleShaderBadConstants, g_hleShaderBadVertex,
          g_hleShaderIdentityMvp, g_hleShaderViewportMvp, g_vteSeen[2],
          g_vteSeen[1], g_vteSeen[0], g_hleShaderMvpDisagree,
          g_liveVertexResolved, g_liveVertexNoMatch, g_liveVertexAmbiguous,
          g_liveVertexUnreadable, g_gpuVertexDraws, g_gpuVertexSkipped,
          g_gpuVertexUndeclared, g_gpuVertexNoVs,
          g_gpuVertexVsSamplers, g_gpuVertexNoVte, g_gpuVertexNoPs,
          g_gpuVertexTooManyInputs, g_gpuFetchDraws, g_gpuFetchRectList,
          g_gpuFetchOrdinalMismatch, g_gpuFetchOutOfRange, g_gpuFetchUnaligned);
    }
  } report{attempt};
  if (!handle || !device) {
    ++g_hleShaderNoCode;
    return ShaderApplyResult::kNoCode;
  }

  // Prefer the exact capture made by PatchVertexShaderToMatchVertexDeclaration.
  // Shaders resident before that hook first fires may use the independently
  // validated live allocation fallback above.  The >=90% PM4 content match
  // remains diagnostic-only and is never a source for pixels.
  auto pi = g_patchedCode.find(handle);
  const PatchedCode* patchp =
      pi != g_patchedCode.end() && pi->second.resolved ? &pi->second : nullptr;
  if (!patchp) patchp = CodeFromShaderObject(handle, base);
  if (!patchp) {
    ++g_hleShaderNoCode;
    static uint32_t s_logged_no_code = 0;
    if (s_logged_no_code++ < 24) {
      REXLOG_INFO("d3d9: HLE producer rejected: vertex shader 0x{:08X} has "
                  "no exact patched code, target 0x{:08X} {}x{}, viewport "
                  "{}x{}, {} verts / {} indices",
                  handle, dc.render_target_object, dc.render_target_width,
                  dc.render_target_height, dc.viewport_width,
                  dc.viewport_height, dc.vertex_count, dc.index_count);
    }
    return ShaderApplyResult::kNoCode;
  }
  const PatchedCode& patch = *patchp;
  if (patch.code_off >= patch.code.size()) {
    ++g_hleShaderBadDecode;
    return ShaderApplyResult::kFailed;
  }
  ReportHlslCoverage(mx::hle::HlslStage::kVertex, handle,
                     patch.code.data() + patch.code_off,
                     uint32_t(patch.code.size() - patch.code_off));

  std::vector<VertexAttribute> attrs;
  const char* why = nullptr;
  if (!DecodeVertexShaderFetches(patch.code.data() + patch.code_off,
                                 uint32_t(patch.code.size() - patch.code_off),
                                 attrs, &why) ||
      attrs.empty() || attrs.size() != patch.expect_fetches) {
    ++g_hleShaderBadDecode;
    return ShaderApplyResult::kFailed;
  }

  // The index operand and exp_adjust, censused once per distinct combination.
  //
  // Emitting the fetch into HLSL means addressing the vertex buffer with
  // SV_VertexID, which is only correct if every fetch really does index by the
  // vertex ID. The CPU path has always assumed that without checking, and
  // exp_adjust is decoded and applied nowhere at all -- a non-zero one is a
  // silently dropped power-of-two scale. Both are cheap to see and expensive to
  // get wrong, so they are measured before anything is built on them.
  if (g_diag) {
    static std::map<uint64_t, bool> s_seen;
    for (const auto& a : attrs) {
      const uint64_t sig = (uint64_t(a.src_reg) << 32) |
                           (uint64_t(a.src_swizzle) << 16) |
                           (uint64_t(a.is_index_rounded ? 1 : 0) << 8) |
                           uint64_t(uint8_t(a.exp_adjust));
      if (!s_seen.emplace(sig, true).second) continue;
      REXLOG_INFO("d3d9: VFETCH index census: src r{}.{} rounded={} "
                  "exp_adjust={} (fmt {} slot {})",
                  a.src_reg, "xyzw"[a.src_swizzle & 3], a.is_index_rounded,
                  a.exp_adjust, a.format, a.fetch_slot);
    }
  }

  std::vector<uint32_t> attr_stream(attrs.size());
  for (size_t a = 0; a < attrs.size(); ++a) {
    const uint32_t fs = attrs[a].fetch_slot;
    if (fs > 95 || (95u - fs) >= kMaxStreams) {
      ++g_hleShaderBadStream;
      return ShaderApplyResult::kFailed;
    }
    const uint32_t si = 95u - fs;
    const HleStream& s = streams[si];
    if (!s.bound || !s.host || s.stride == 0 || s.stride > 256 ||
        s.stride != attrs[a].stride_bytes) {
      ++g_hleShaderBadStream;
      return ShaderApplyResult::kFailed;
    }
    attr_stream[a] = si;
  }

  std::vector<uint32_t> consts;
  if (constant_snapshot) {
    consts.assign(constant_snapshot,
                  constant_snapshot + kD3d9ConstRegs * 4);
  } else {
    std::array<uint32_t, kD3d9ConstRegs * 4> captured;
    if (!CaptureVertexConstants(device, base, handle, captured)) {
      ++g_hleShaderBadConstants;
      return ShaderApplyResult::kFailed;
    }
    consts.assign(captured.begin(), captured.end());
  }

  AluInputs alu_in;
  alu_in.alu_consts = consts.data();
  alu_in.alu_const_dwords = uint32_t(consts.size());

  // Commit only after every vertex succeeds. Mixing shader outputs and raw
  // declaration positions within one primitive produces plausible-looking but
  // invalid geometry and is worse than dropping the draw explicitly.
  std::vector<uint8_t> transformed = dc.vertices;
  std::vector<uint8_t> referenced(dc.vertex_count, dc.index_count ? 0 : 1);
  if (dc.index_count) {
    const uint32_t iw = dc.index_16bit ? 2u : 4u;
    if (dc.indices.size() < uint64_t(dc.index_count) * iw) {
      ++g_hleShaderBadVertex;
      return ShaderApplyResult::kFailed;
    }
    for (uint32_t i = 0; i < dc.index_count; ++i) {
      uint32_t index = 0;
      if (dc.index_16bit) {
        uint16_t v;
        std::memcpy(&v, dc.indices.data() + size_t(i) * 2, 2);
        index = v;
      } else {
        std::memcpy(&index, dc.indices.data() + size_t(i) * 4, 4);
      }
      if (index >= dc.vertex_count) {
        ++g_hleShaderBadVertex;
        return ShaderApplyResult::kFailed;
      }
      referenced[index] = 1;
    }
  }
  uint8_t vtx[kMaxStreams][256];
  std::vector<std::array<float, 4>> values(attrs.size());

  // ---- The GPU vertex path ------------------------------------------------
  //
  // Qualifying is deliberately narrow, and every condition is a thing that
  // would otherwise be guessed at:
  //
  //  - BOTH stages must have translated. A GPU vertex stage under the stand-in
  //    pixel shader would still owe it the reconstructed param_gen UV and the
  //    single selected interpolator, both of which are computed from
  //    ExecuteVertexShader's result below. Taking the stages together means
  //    none of that has to be reproduced — the rasterizer does it natively.
  //  - Every attribute's destination must be a register the shader declares.
  //    An attribute writing an undeclared register has nowhere to go, and
  //    dropping it silently would feed the shader a zero it never saw on the
  //    console. Measured over 15,000 draws this never happened (0 undeclared),
  //    but the check costs nothing and turns a would-be silent wrong answer
  //    into a fallback.
  //
  // The layout is one element per REGISTER, not per attribute: 5.4% of draws
  // have two vfetches sharing a register with complementary destination
  // swizzles, and one element each would clobber rather than merge.
  //  - The vertex shader must read no textures. The translated root signature
  //    gives its SRV and sampler tables PIXEL visibility only, so a vertex
  //    fetch would fail pipeline creation rather than render wrongly — but
  //    refusing here keeps the draw on a path that works instead of on one that
  //    silently produces no pipeline.
  //  - PA_CL_VTE_CNTL must say the hardware applies the viewport transform, so
  //    the shader's position export is clip space and dc.mvp is identity. The
  //    translated pipeline's vertex stage does not apply mvp at all — neither
  //    the passthrough one nor the guest's — so a draw needing the viewport
  //    inverse has nowhere to apply it. Measured, the register reads 0x43F on
  //    every draw in this game, so this refuses nothing today; it is here so
  //    that if it ever does not, the draw falls back rather than moves.
  const TranslatedShader* vs_translated = TranslatedVertexShader(handle);
  // Evaluated as separate tests rather than one `&&` chain so each refusal is
  // attributed. The chain short-circuits, so a draw refused for two reasons is
  // counted against the first — read the counters as "the reason that fired",
  // not as a partition of independent causes.
  bool gpu_vertex = true;
  if (!vs_translated || !vs_translated->source) {
    gpu_vertex = false;
    ++g_gpuVertexNoVs;
  } else if (vs_translated->sampler_count != 0 &&
             dc.vertex_sampler_count != vs_translated->sampler_count) {
    // A sampling vertex shader is allowed through now that it has its own
    // descriptor range (t17+/s16+) -- but only once every one of its slots
    // resolved to a texture. Refusing on a short fill keeps the all-or-nothing
    // rule the pixel stage already has: a slot left unbound samples whatever
    // descriptor happens to sit at that index, which is a confident wrong
    // answer rather than a visible failure.
    //
    // This counter therefore changes meaning. It used to mean "has a sampler";
    // it now means "has a sampler we could not fill", which should be rare.
    gpu_vertex = false;
    ++g_gpuVertexVsSamplers;
  } else if (!VportScaleEnabled(device, base)) {
    gpu_vertex = false;
    ++g_gpuVertexNoVte;
  } else if (dc.pixel_shader_hlsl == nullptr) {
    gpu_vertex = false;
    ++g_gpuVertexNoPs;
  }
  // Which bucket this draw's loop time lands in, if it reaches the loop at all.
  // Set at the point of refusal so it names the reason that actually fired
  // rather than the first one that could have.
  uint8_t loop_reason = kLoopOther;
  if (!gpu_vertex && dc.pixel_shader_hlsl == nullptr) loop_reason = kLoopNoPs;
  if (gpu_vertex) {
    for (const auto& a : attrs) {
      if (a.dest_reg >= 32 ||
          !(vs_translated->input_mask & (1u << a.dest_reg))) {
        gpu_vertex = false;
        ++g_gpuVertexUndeclared;
        break;
      }
    }
  }
  // Can this draw let the SHADER do the vertex fetch, out of the raw guest
  // buffer, instead of the CPU unpacking every attribute of every vertex?
  //
  // Strictly an accelerated form of gpu_vertex: everything that refuses that
  // refuses this, and anything this refuses falls back to it rather than
  // failing. So a defect here costs frame time, not pixels.
  bool gpu_fetch =
      gpu_vertex && vs_translated && vs_translated->fetch_source;
  if (gpu_fetch && dc.prim_type == uint32_t(mx::hle::PrimitiveType::kRectangleList)) {
    // ExpandRectangleList synthesizes a fourth vertex as v1 + v2 - v0 from the
    // host vertices AND from vertex_inputs. The fetch path produces neither,
    // and raw guest bytes cannot be combined affinely without first decoding
    // them -- which is the work being removed. Every full-screen post pass is a
    // RECTLIST, but they are 3-6 vertices each, so this costs nothing.
    gpu_fetch = false;
    ++g_gpuFetchRectList;
    loop_reason = kLoopRectList;
  }
  if (g_diag) {
    // Independent check on the refusal above: 88% of qualifying draws coming
    // back RECTLIST is not a plausible frame, so the primitive type is counted
    // directly rather than inferred from the refusal.
    static std::map<uint32_t, uint64_t> s_prims;
    static uint64_t s_primTotal = 0;
    ++s_prims[dc.prim_type];
    if ((++s_primTotal % 5000) == 0) {
      std::string h;
      for (const auto& [p, n] : s_prims) h += fmt::format(" {}={}", p, n);
      REXLOG_INFO("d3d9: prim_type histogram over {} draws:{}", s_primTotal, h);
    }
  }
  if (gpu_fetch && attrs.size() != vs_translated->vertex_fetch_count) {
    // The emitter and DecodeVertexShaderFetches walk the same instruction
    // stream, so these must agree. If they ever do not, the xe_vf[] entries
    // would be paired with the wrong fetches and geometry would be misaddressed
    // with no symptom at the point of the mistake.
    gpu_fetch = false;
    ++g_gpuFetchOrdinalMismatch;
  }
  if (gpu_fetch) {
    // Merge the used streams into one buffer, in first-use order, and describe
    // each fetch's window into it.
    uint32_t region_of_stream[kMaxStreams];
    std::memset(region_of_stream, 0xFF, sizeof(region_of_stream));
    dc.raw_vertex_bytes.clear();
    dc.raw_fetch_count = 0;
    for (size_t a = 0; a < attrs.size() && gpu_fetch; ++a) {
      if (attrs[a].fetch_slot != vs_translated->vertex_fetch_slot[a]) {
        gpu_fetch = false;
        ++g_gpuFetchOrdinalMismatch;
        break;
      }
      const uint32_t si = attr_stream[a];
      const mx::hle::HleStream& s = streams[si];
      if (region_of_stream[si] == 0xFFFFFFFFu) {
        const uint64_t start = uint64_t(s.offset_bytes) +
                               uint64_t(dc.first_vertex) * s.stride;
        const uint64_t bytes = uint64_t(dc.vertex_count) * s.stride;
        if (start + bytes > s.size_bytes) {
          // The same bound BuildHleDraw enforces per vertex, applied once to
          // the whole window. A stream the shader indexes past is a refusal,
          // not a clamp into whatever follows the buffer.
          gpu_fetch = false;
          ++g_gpuFetchOutOfRange;
          break;
        }
        // ByteAddressBuffer.Load needs 4-byte alignment, and every address the
        // shader forms is base + vid * stride + a dword-derived offset. Guest
        // strides and offsets are dword counts so this should always hold --
        // refused rather than assumed, because misalignment reads silently
        // wrong data rather than faulting.
        if ((dc.raw_vertex_bytes.size() % 4) != 0 || (s.stride % 4) != 0 ||
            (start % 4) != 0) {
          gpu_fetch = false;
          ++g_gpuFetchUnaligned;
          break;
        }
        region_of_stream[si] = uint32_t(dc.raw_vertex_bytes.size());
        dc.raw_vertex_bytes.insert(dc.raw_vertex_bytes.end(),
                                   s.host + start, s.host + start + bytes);
      }
      auto& rf = dc.raw_fetch[dc.raw_fetch_count++];
      rf.base = region_of_stream[si];
      rf.stride = s.stride;
      rf.endian = s.endian;
    }
    if (!gpu_fetch) {
      dc.raw_vertex_bytes.clear();
      dc.raw_fetch_count = 0;
    }

    // Self-check on the ADDRESSING, bounded to the first draws of a run.
    //
    // The picture is the only real verdict and it needs an attended run, but
    // the half of this most likely to be silently wrong -- the base offset,
    // whether first_vertex is folded in correctly, the stride, and which bytes
    // were copied -- can be checked without a GPU at all. Decode the same
    // attribute twice: once from the guest stream the way the CPU path always
    // has, and once from the merged buffer at the address the shader will form.
    // They must be bit-identical.
    //
    // This does NOT check the HLSL format decode or the endian shuffle emitted
    // into the shader; both sides here use the CPU decoder. It checks that the
    // shader is pointed at the right bytes.
    static uint64_t s_checked = 0, s_mismatch = 0;
    if (g_diag && gpu_fetch && s_checked < 400) {
      ++s_checked;
      const uint32_t probe[2] = {0, dc.vertex_count ? dc.vertex_count - 1 : 0};
      for (uint32_t pi = 0; pi < 2; ++pi) {
        const uint32_t v = probe[pi];
        for (size_t a = 0; a < attrs.size(); ++a) {
          const mx::hle::HleStream& s = streams[attr_stream[a]];
          const auto& rf = dc.raw_fetch[a];
          uint8_t direct[256] = {};
          const uint64_t off =
              uint64_t(s.offset_bytes) +
              (uint64_t(dc.first_vertex) + v) * s.stride;
          if (off + s.stride > s.size_bytes || s.stride > sizeof(direct))
            continue;
          std::memcpy(direct, s.host + off, s.stride);
          const uint64_t raw_off = uint64_t(rf.base) + uint64_t(v) * rf.stride;
          if (raw_off + rf.stride > dc.raw_vertex_bytes.size()) continue;
          float fa[4] = {}, fb[4] = {};
          const bool oka = mx::hle::ReadVertexAttribute(direct, s.stride,
                                                        attrs[a], s.endian, fa);
          const bool okb = mx::hle::ReadVertexAttribute(
              dc.raw_vertex_bytes.data() + raw_off, rf.stride, attrs[a],
              rf.endian, fb);
          if (oka != okb || (oka && std::memcmp(fa, fb, sizeof(fa)) != 0)) {
            if (++s_mismatch <= 8) {
              REXLOG_INFO(
                  "d3d9: VFETCH ADDRESSING MISMATCH vs 0x{:08X} attr {} fmt {} "
                  "vertex {}: stream ({:.6g},{:.6g},{:.6g},{:.6g}) vs raw "
                  "({:.6g},{:.6g},{:.6g},{:.6g}); base {} stride {} endian {}",
                  handle, a, attrs[a].format, v, fa[0], fa[1], fa[2], fa[3],
                  fb[0], fb[1], fb[2], fb[3], rf.base, rf.stride, rf.endian);
            }
          }
        }
      }
      if (s_checked == 400) {
        REXLOG_INFO("d3d9: VFETCH addressing self-check: {} draws, {} "
                    "mismatches",
                    s_checked, s_mismatch);
      }
    }
  }

  uint32_t input_stride = 0;
  // reg_slot[r] = which input element register r occupies, or 0xFF.
  uint8_t reg_slot[32];
  std::memset(reg_slot, 0xFF, sizeof(reg_slot));
  if (gpu_vertex) {
    dc.vertex_input_count = 0;
    for (uint32_t r = 0; r < 32; ++r) {
      if (!(vs_translated->input_mask & (1u << r))) continue;
      if (dc.vertex_input_count >= mx::hle::DrawCall::kMaxVertexInputs) {
        gpu_vertex = false;
        ++g_gpuVertexTooManyInputs;
        break;
      }
      reg_slot[r] = uint8_t(dc.vertex_input_count);
      dc.vertex_input_regs[dc.vertex_input_count++] = uint8_t(r);
    }
  }
  // gpu_fetch can only have been set while gpu_vertex held, but the input-count
  // loop above can still clear gpu_vertex afterwards -- so re-check rather than
  // leave a draw claiming a fetch path its vertex stage was just refused.
  if (!gpu_vertex) gpu_fetch = false;
  if (gpu_fetch) {
    // No vertex_inputs and no per-vertex loop: the shader reads the raw bytes
    // itself. This is the whole point of the path.
    dc.vertex_input_count = 0;
    dc.vertex_shader_handle = handle;
    dc.vertex_shader_hlsl = vs_translated->fetch_source;
    dc.vertex_constants = consts;
    ++g_gpuVertexDraws;
    ++g_gpuFetchDraws;
  } else if (gpu_vertex) {
    input_stride = dc.vertex_input_count * 16;
    // Zeroed, and that is the correct default rather than a convenience: the
    // interpreter's register file starts at zero, so a destination swizzle of
    // "keep" on a component no fetch writes must read zero, not stale bytes.
    dc.vertex_inputs.assign(size_t(dc.vertex_count) * input_stride, 0);
    dc.vertex_shader_handle = handle;
    dc.vertex_shader_hlsl = vs_translated->source;
    dc.vertex_constants = consts;
    ++g_gpuVertexDraws;
  } else {
    dc.vertex_input_count = 0;
    ++g_gpuVertexSkipped;
  }

  // The interpolator stream for a translated pixel shader. Allocated only when
  // this draw has one: it is 128 bytes per vertex and a frame carries six
  // figures of vertices.
  //
  // ExecuteVertexShader already returns all 16 exports per vertex, and this
  // function has always discarded every one except the interpolator the texture
  // profile named. Reusing that discarded work is what makes the translated
  // pixel path affordable without moving the vertex shader to the GPU first.
  constexpr uint32_t kInterpStride =
      mx::hle::kHlslInterpolatorLinkage * 4 * uint32_t(sizeof(float));
  // Not for a draw on the GPU vertex path: its pixel stage reads what the
  // rasterizer interpolates from the vertex stage's own exports, so this stream
  // would be built at 128 bytes a vertex and then never bound.
  const bool want_interpolators = dc.pixel_shader_hlsl != nullptr && !gpu_vertex;
  if (want_interpolators)
    dc.interpolators.assign(size_t(dc.vertex_count) * kInterpStride, 0);

  // A fetch draw is finished. Everything below this point exists to produce
  // per-vertex data the GPU is now producing for itself: the attribute decode,
  // the input registers, the transformed positions, the interpolator stream and
  // the UV reconstruction. Returning here is what removes the 145ms.
  //
  // The mvp must be identity, which VportScaleEnabled already guaranteed as a
  // condition of gpu_vertex -- the translated vertex stage applies no mvp at
  // all, on either path.
  if (gpu_fetch) {
    static constexpr float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0,
                                            0, 0, 1, 0, 0, 0, 0, 1};
    std::memcpy(dc.mvp, kIdentity, sizeof(dc.mvp));
    ++g_hleShaderIdentityMvp;
    ++g_hleShaderDraws;
    g_hleShaderVertices += dc.vertex_count;
    return ShaderApplyResult::kApplied;
  }

  // The fetch prediction was made before this draw was built, so its 36-byte
  // host vertices were never transcoded — and everything from here down reads
  // them. Filling the gap now costs exactly what building them eagerly would
  // have; the saving is entirely on the draws that DID fetch and returned above.
  if (dc.vertex_stride == 0 && dc.vertex_count) {
    if (!deferred_in) {
      // Should be unreachable: deferral requires a translated vertex shader, and
      // the only caller without inputs to hand is the kNoCode replay, which by
      // definition has none.
      ++g_transcodeLost;
      return ShaderApplyResult::kFailed;
    }
    HleSkip tskip = HleSkip::kNone;
    if (!mx::hle::TranscodeHleVertices(*deferred_in, dc, tskip)) {
      ++HleSkipCounts()[uint32_t(tskip)];
      return ShaderApplyResult::kFailed;
    }
    // `transformed` was copied from dc.vertices at the top of this function,
    // which for a deferred draw was EMPTY. The per-vertex loop below writes the
    // shader's clip-space export into it by offset, so leaving it empty is a
    // memcpy to nullptr — an access violation writing address 0, which is
    // exactly what mx_882 and mx_883 crashed on.
    transformed = dc.vertices;
    ++g_transcodeLate;
  }

  uint64_t applied_vertices = 0;
  uint64_t identity_in_clip = 0, viewport_in_clip = 0;
  uint64_t uv_vertices = 0, uv_missing_export = 0;
  float uv_min[2] = {1.0e30f, 1.0e30f};
  float uv_max[2] = {-1.0e30f, -1.0e30f};
  // First referenced vertex, kept whole so a collapsed UV can be traced to the
  // stage that produced it: the attribute values that went into the shader,
  // and every export that came out — not only the one the texture profile
  // selected. Reporting the selected export alone cannot distinguish "the
  // shader exported zero" from "the UV is in a different export".
  AluResult probe{};
  std::vector<std::array<float, 4>> probe_values;
  bool have_probe = false;

  // Does a VS export feed this interpolator at all, or does the rasterizer
  // generate it?
  //
  // SQ_PROGRAM_CNTL bit 18 (`param_gen`) makes the hardware synthesise an
  // interpolator holding the screen-space position. SQ_CONTEXT_MISC selects
  // its destination register; vs_export_count only describes how many
  // interpolators the vertex shader exports and cannot select this input.
  //
  // PM4 captures show the distinction directly: SQ_PROGRAM_CNTL may report
  // vs_export_count=2 while the following SQ_CONTEXT_MISC selects r3. Reading
  // exports[3] returns zero because no vertex export is supposed to feed it.
  uint32_t uv_export_reg = texture_binding ? texture_binding->src_reg : 0;
  bool uv_generated = false;
  if (texture_binding && dc.pixel_param_gen &&
      texture_binding->src_reg == dc.pixel_param_gen - 1)
    uv_generated = true;
  // Not a PhaseTimer: the loop below has early returns on malformed geometry,
  // and a scope guard there would have to survive them. Those paths abandon the
  // draw anyway, so losing their timing is correct rather than merely tolerable.
  const auto loop_t0 = std::chrono::steady_clock::now();
  g_phaseVertexCount += dc.vertex_count;
  for (uint32_t v = 0; v < dc.vertex_count; ++v) {
    if (!referenced[v]) continue;
    const uint64_t src = uint64_t(dc.first_vertex) + v;
    bool have[kMaxStreams] = {};
    for (size_t a = 0; a < attrs.size(); ++a) {
      const uint32_t si = attr_stream[a];
      const HleStream& s = streams[si];
      if (!have[si]) {
        const uint64_t byte_off = src * s.stride + s.offset_bytes;
        if (byte_off + s.stride > s.size_bytes) {
          ++g_hleShaderBadVertex;
          return ShaderApplyResult::kFailed;
        }
        std::memcpy(vtx[si], s.host + byte_off, s.stride);
        have[si] = true;
      }
      float f[4] = {0, 0, 0, 1};
      if (!ReadVertexAttribute(vtx[si], s.stride, attrs[a], s.endian, f)) {
        ++g_hleShaderBadVertex;
        return ShaderApplyResult::kFailed;
      }
      values[a] = {f[0], f[1], f[2], f[3]};
    }

    // Merge the attributes into their destination registers, by exactly the
    // rule shader_alu.cpp:614 seeds its register file with — three bits per
    // destination component, 0-3 selecting x/y/z/w of the fetched value, 4 and
    // 5 the constants 0.0 and 1.0, 7 meaning keep. `kKeep` is the whole reason
    // two fetches can share a register, so writing all four components would
    // reintroduce the clobber that decoder documents.
    if (gpu_vertex) {
      float* regs = reinterpret_cast<float*>(dc.vertex_inputs.data() +
                                             size_t(v) * input_stride);
      for (size_t a = 0; a < attrs.size(); ++a) {
        const uint32_t slot = reg_slot[attrs[a].dest_reg & 31];
        if (slot == 0xFF) continue;  // cannot happen; qualifying proved it
        float* d = regs + size_t(slot) * 4;
        const uint32_t swiz = attrs[a].dest_swizzle;
        for (uint32_t c = 0; c < 4; ++c) {
          switch (uc::GetFetchDestinationComponentSwizzle(swiz, c)) {
            case uc::FetchDestinationSwizzle::kX: d[c] = values[a][0]; break;
            case uc::FetchDestinationSwizzle::kY: d[c] = values[a][1]; break;
            case uc::FetchDestinationSwizzle::kZ: d[c] = values[a][2]; break;
            case uc::FetchDestinationSwizzle::kW: d[c] = values[a][3]; break;
            case uc::FetchDestinationSwizzle::k0: d[c] = 0.0f; break;
            case uc::FetchDestinationSwizzle::k1: d[c] = 1.0f; break;
            default: break;  // kKeep, and the one undefined encoding
          }
        }
      }

      // The interpreter is done for this vertex, because nothing below it is
      // still needed:
      //
      //   - the transformed position it writes is what the vertex stage now
      //     produces;
      //   - the interpolator copy exists so the rasterizer can interpolate the
      //     shader's exports, which the rasterizer does natively;
      //   - the param_gen UV reconstructs the hardware's screen-space
      //     parameter, which in a pixel shader that reads it IS SV_Position;
      //   - the finite/NaN rejection guards against an INTERPRETER emitting
      //     garbage, and there is no interpreter here to emit any;
      //   - the in-clip scoring only feeds g_hleShaderMvpDisagree, and this
      //     path already requires the register that contest was replaced by.
      //
      // This is the whole point of the migration: 112,700 vertices x 4.9us was
      // 550ms, which was the entire frame.
      ++applied_vertices;
      continue;
    }

    AluResult r;
    {
      PhaseTimer interp_timer(g_phaseInterpUs);
      r = ExecuteVertexShader(
          patch.code.data() + patch.code_off,
          uint32_t(patch.code.size() - patch.code_off), attrs, values, alu_in);
    }
    if (!have_probe) {
      have_probe = true;
      probe = r;
      probe_values.assign(values.begin(), values.begin() + attrs.size());
    }
    const float w = r.position[3];
    if (r.status != AluStatus::kOk || !std::isfinite(r.position[0]) ||
        !std::isfinite(r.position[1]) || !std::isfinite(r.position[2]) ||
        !std::isfinite(w)) {
      ++g_hleShaderBadVertex;
      return ShaderApplyResult::kFailed;
    }

    // The position export is homogeneous clip space, passed through to the
    // host untouched. This used to drop w on the claim that PA_CL_VTE_CNTL was
    // 0x300 — XYZ already multiplied by 1/W0. Measured, that register lives at
    // device+10572 and reads 0x400 or 0x43F, never 0x300, and bits 8 and 9
    // (VTX_XY_FMT, VTX_Z_FMT) — the ones that would mean "already divided" —
    // are clear in every sample. Dropping w scaled all 3D geometry by whatever
    // it should have divided by, which is why the front end's pre-transformed
    // 2D (w = 1) was unaffected while everything else blew up.
    //
    // Dividing here instead is not enough either, and the earlier note about
    // this "clipping the entire coloured scene away" is the reason: 1.2 million
    // vertices per run carry w <= 0, behind the eye. A negative w mirrors the
    // vertex through the origin rather than removing it, so those triangles
    // must be clipped against the near plane *before* any divide. D3D12 does
    // exactly that, in hardware, given clip space — so give it clip space.
    const float p[4] = {r.position[0], r.position[1], r.position[2], w};
    std::memcpy(transformed.data() + size_t(v) * dc.vertex_stride, p,
                sizeof(p));

    // The shader's own interpolators, verbatim, for the translated pixel path.
    // No reconstruction and no viewport transform: these are the values the
    // pixel shader's registers are seeded with on the hardware, and the
    // rasterizer interpolates them. The param_gen synthesis further down is a
    // separate thing — it fabricates the ONE interpolator the hardware
    // generates rather than the shader exporting, and only the stand-in path
    // needs it.
    if (want_interpolators) {
      uint8_t* dst = dc.interpolators.data() + size_t(v) * kInterpStride;
      for (uint32_t i = 0; i < mx::hle::kHlslInterpolatorLinkage; ++i) {
        float e[4] = {0, 0, 0, 0};
        if (i < r.exports.size()) {
          for (uint32_t c = 0; c < 4; ++c) {
            const float f = r.exports[i][c];
            e[c] = std::isfinite(f) ? f : 0.0f;
          }
        }
        std::memcpy(dst + size_t(i) * 16, e, sizeof(e));
      }
    }
    // Resolved render-target samples intentionally carry no CPU texture
    // payload: the renderer samples the ordered target resource instead. UVs
    // are shader outputs and must be populated for both resource paths. The old
    // `dc.texture` guard left every resolved sample at BuildHleDraw's default
    // (0,0), which explains the flat single-texel compositor wedges.
    if (texture_binding && uv_generated) {
      // The rasterizer's parameter, reconstructed. Its screen-space position is
      // exactly the viewport transform of the clip-space position this vertex
      // already carries, so it needs no export and no guesswork: NDC x maps to
      // u, and NDC y maps to v inverted, because screen y runs downward.
      //
      // Written normalized and NOT divided by the texture extent. The fetch is
      // unnormalized and the hardware parameter is in pixels, but that pixel
      // count is the render target's, not the sampled texture's — the divide
      // below uses the sampled extent, and the two are only incidentally equal.
      // Normalizing here is the same result without depending on that.
      float uv[2] = {0.0f, 0.0f};
      const float pw = p[3];
      if (std::isfinite(pw) && std::abs(pw) > 1.0e-12f) {
        uv[0] = (p[0] / pw) * 0.5f + 0.5f;
        uv[1] = 0.5f - (p[1] / pw) * 0.5f;
      }
      if (std::isfinite(uv[0]) && std::isfinite(uv[1])) {
        std::memcpy(transformed.data() + size_t(v) * dc.vertex_stride + 32, uv,
                    sizeof(uv));
        uv_min[0] = std::min(uv_min[0], uv[0]);
        uv_min[1] = std::min(uv_min[1], uv[1]);
        uv_max[0] = std::max(uv_max[0], uv[0]);
        uv_max[1] = std::max(uv_max[1], uv[1]);
        ++uv_vertices;
      } else {
        ++uv_missing_export;
      }
    } else if (texture_binding &&
        uv_export_reg < AluResult::kMaxInterpolators &&
        (r.export_mask & (1u << uv_export_reg))) {
      const auto& e = r.exports[uv_export_reg];
      const uint32_t sx = (texture_binding->src_swizzle >> 0) & 3u;
      const uint32_t sy = (texture_binding->src_swizzle >> 2) & 3u;
      float uv[2] = {e[sx], e[sy]};
      bool uv_valid = true;
      if (texture_binding->unnormalized) {
        const uint32_t width = dc.texture ? dc.texture->width
                                          : dc.sampled_texture_width;
        const uint32_t height = dc.texture ? dc.texture->height
                                           : dc.sampled_texture_height;
        if (!width || !height) {
          ++uv_missing_export;
          uv_valid = false;
        } else {
          uv[0] /= float(width);
          uv[1] /= float(height);
        }
      }
      if (uv_valid && std::isfinite(uv[0]) && std::isfinite(uv[1])) {
        std::memcpy(transformed.data() + size_t(v) * dc.vertex_stride + 32,
                    uv, sizeof(uv));
        uv_min[0] = std::min(uv_min[0], uv[0]);
        uv_min[1] = std::min(uv_min[1], uv[1]);
        uv_max[0] = std::max(uv_max[0], uv[0]);
        uv_max[1] = std::max(uv_max[1], uv[1]);
        ++uv_vertices;
      }
    } else if (texture_binding) {
      ++uv_missing_export;
    }

    // VTE scale/offset enable state is draw state, but the HLE draw entry point
    // does not carry that register. Distinguish the two legal modes using the
    // shader output itself: normalized coordinates are already host-ready,
    // while pre-transformed window coordinates only enter clip after the live
    // viewport inverse. This is scored over every referenced vertex, per draw.
    if (p[0] >= -1.0f && p[0] <= 1.0f && p[1] >= -1.0f && p[1] <= 1.0f &&
        p[2] >= 0.0f && p[2] <= 1.0f) {
      ++identity_in_clip;
    }
    const float vx = dc.mvp[0] * p[0] + dc.mvp[1] * p[1] +
                     dc.mvp[2] * p[2] + dc.mvp[3];
    const float vy = dc.mvp[4] * p[0] + dc.mvp[5] * p[1] +
                     dc.mvp[6] * p[2] + dc.mvp[7];
    const float vz = dc.mvp[8] * p[0] + dc.mvp[9] * p[1] +
                     dc.mvp[10] * p[2] + dc.mvp[11];
    const float vw = dc.mvp[12] * p[0] + dc.mvp[13] * p[1] +
                     dc.mvp[14] * p[2] + dc.mvp[15];
    if (vw > 0.0f && vx >= -vw && vx <= vw && vy >= -vw && vy <= vw &&
        vz >= 0.0f && vz <= vw) {
      ++viewport_in_clip;
    }
    ++applied_vertices;
  }
  {
    const uint64_t loop_us =
        uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                     std::chrono::steady_clock::now() - loop_t0)
                     .count());
    g_phaseVertexLoopUs += loop_us;
    g_loopUs[loop_reason] += loop_us;
    g_loopVerts[loop_reason] += applied_vertices;
    ++g_loopDraws[loop_reason];
  }

  dc.vertices = std::move(transformed);
  if (texture_binding) {
    static uint64_t s_uv_reports = 0;
    const bool collapsed = uv_vertices &&
        std::abs(uv_max[0] - uv_min[0]) < 1.0e-6f &&
        std::abs(uv_max[1] - uv_min[1]) < 1.0e-6f;
    // A fixed budget for collapsed reports spends itself on whatever draws
    // happen first. In the 2026-08-05 runs that was the pre-load compositor:
    // all 256 slots were gone 28 seconds before `force_load` fired, so every
    // UV line in the log described a scene that was not loaded yet and the
    // 300k textured world vertices were never measured at all. Rate-limit by
    // time instead, so each phase of the run gets reported.
    static std::chrono::steady_clock::time_point s_last_collapsed{};
    const auto now = std::chrono::steady_clock::now();
    const bool collapsed_due =
        collapsed && (now - s_last_collapsed) >= std::chrono::seconds(5);
    if (collapsed_due) s_last_collapsed = now;
    if (++s_uv_reports <= 32 || collapsed_due || (s_uv_reports % 1000) == 0) {
      REXLOG_INFO(
          "d3d9: HLE UV r{}->e{}{} swiz=0x{:02X} denorm={} wrote {}/{} "
          "missing {}; range ({:.5g},{:.5g})..({:.5g},{:.5g}) collapsed={} "
          "cpu={} resolved=0x{:08X} extent={}x{}",
          texture_binding->src_reg, uv_export_reg,
          uv_generated ? " GENERATED" : "", texture_binding->src_swizzle,
          texture_binding->unnormalized, uv_vertices, applied_vertices,
          uv_missing_export, uv_min[0], uv_min[1], uv_max[0], uv_max[1],
          collapsed, bool(dc.texture), dc.sampled_render_target_object,
          dc.sampled_texture_width, dc.sampled_texture_height);

      // A collapsed range is where the reporting used to stop. Everything
      // below distinguishes the three stages that can produce it.
      if (collapsed && have_probe) {
        SqProgramCntl pc{};
        if (ReadSqProgramCntl(device, base, &pc)) {
          REXLOG_INFO(
              "d3d9:   SQ_PROGRAM_CNTL=0x{:08X} param_gen={} gen_index_pix={} "
              "vs_num_reg={} ps_num_reg={} vs_export_count={} "
              "vs_export_mode={} ps_export_mode={}",
              pc.raw, pc.param_gen, pc.gen_index_pix, pc.vs_num_reg,
              pc.ps_num_reg, pc.vs_export_count, pc.vs_export_mode,
              pc.ps_export_mode);
        } else {
          REXLOG_INFO("d3d9:   SQ_PROGRAM_CNTL unreadable (device=0x{:08X})",
                      device);
        }
        for (size_t a = 0; a < probe_values.size() && a < attrs.size(); ++a) {
          const HleStream& s = streams[attr_stream[a]];
          REXLOG_INFO(
              "d3d9:   attr[{}] stream={} slot={} off={} stride={} fmt={} "
              "dest=r{} dswiz=0x{:03X} -> ({:.5g},{:.5g},{:.5g},{:.5g})",
              a, attr_stream[a], attrs[a].fetch_slot, attrs[a].offset_bytes,
              s.stride, attrs[a].format, attrs[a].dest_reg,
              attrs[a].dest_swizzle, probe_values[a][0],
              probe_values[a][1], probe_values[a][2], probe_values[a][3]);
        }
        for (uint32_t e = 0; e < AluResult::kMaxInterpolators; ++e) {
          if (!(probe.export_mask & (1u << e))) continue;
          REXLOG_INFO("d3d9:   export[{}] = ({:.5g},{:.5g},{:.5g},{:.5g})", e,
                      probe.exports[e][0], probe.exports[e][1],
                      probe.exports[e][2], probe.exports[e][3]);
        }
        // A shader with one float2 input and one exported interpolator has to
        // be computing that interpolator from constants. Whether it asked for
        // any, and whether they came back empty, separates "the interpreter
        // dropped the maths" from "the constant file we handed it was blank".
        REXLOG_INFO(
            "d3d9:   alu status={} export_mask=0x{:04X} const_reads={} "
            "zero_reads={} index_range={}..{} const_dwords={}",
            AluStatusName(probe.status), probe.export_mask, probe.const_reads,
            probe.const_zero_reads, probe.const_min_index,
            probe.const_max_index, alu_in.alu_const_dwords);
        // How much of the constant file is populated at all. "c255 is zero"
        // means one thing if the file is otherwise full and quite another if
        // the whole file is blank — the second would indict the read, not the
        // guest.
        {
          uint32_t live = 0, first_live = 0xFFFFFFFF, last_live = 0;
          for (uint32_t s = 0; s * 4 + 3 < alu_in.alu_const_dwords; ++s) {
            const uint32_t* v = alu_in.alu_consts + s * 4;
            if (v[0] || v[1] || v[2] || v[3]) {
              ++live;
              if (first_live == 0xFFFFFFFF) first_live = s;
              last_live = s;
            }
          }
          const uint32_t* c255 = alu_in.alu_consts + 255 * 4;
          REXLOG_INFO(
              "d3d9:   vs const file: {} live vec4 of {}, live range {}..{}; "
              "c255 raw = {:08X} {:08X} {:08X} {:08X}",
              live, alu_in.alu_const_dwords / 4, first_live, last_live,
              c255[0], c255[1], c255[2], c255[3]);
        }
        // The microcode itself. A shader that reads exactly one constant, and
        // that constant is index 255 = 0xFF, is as consistent with a masked or
        // saturated index as with a real c255 — and only the instruction words
        // tell the two apart.
        {
          const uint32_t* code = patch.code.data() + patch.code_off;
          const uint32_t n =
              uint32_t(patch.code.size() - patch.code_off);
          std::string dw;
          for (uint32_t i = 0; i < n && i < 96; ++i) {
            char one[12];
            std::snprintf(one, sizeof(one), "%08X ", code[i]);
            dw += one;
            if ((i % 12) == 11) {
              REXLOG_INFO("d3d9:   ucode[{:3}] {}", i - 11, dw);
              dw.clear();
            }
          }
          if (!dw.empty())
            REXLOG_INFO("d3d9:   ucode[...] {}", dw);
          REXLOG_INFO("d3d9:   ucode dwords={} code_off={}", n,
                      patch.code_off);
        }
        // The bytes themselves, last: if the attribute values above are zero
        // the question becomes whether the stream held zeros or the read
        // missed them, and only the raw window answers that.
        for (uint32_t si = 0; si < kMaxStreams; ++si) {
          const HleStream& s = streams[si];
          if (!s.host || !s.stride) continue;
          const uint64_t off = uint64_t(dc.first_vertex) * s.stride +
                               s.offset_bytes;
          if (off + s.stride > s.size_bytes) continue;
          std::string hex;
          for (uint32_t b = 0; b < s.stride && b < 64; ++b) {
            char byte[4];
            std::snprintf(byte, sizeof(byte), "%02X ", s.host[off + b]);
            hex += byte;
          }
          REXLOG_INFO(
              "d3d9:   stream[{}] stride={} base_off={} endian={} v0: {}", si,
              s.stride, off, uint32_t(s.endian), hex);
        }
      }
    }
  }
  // Which transform this draw needs is stated by the hardware, not decided by a
  // contest between two candidates.
  //
  // PA_CL_VTE_CNTL (0x2206) says whether the GPU applies the viewport transform
  // itself. Its shadow follows the pattern already established for
  // SQ_PROGRAM_CNTL above: the draw-time flush issues
  // sub_82564768(device, 0, 8704, device + 10548) with 8704 = 0x2200 =
  // RB_DEPTHCONTROL, and sub_82564768 sends register base+i from shadow+i*4, so
  // 0x2206 sits at device + 10548 + 6*4.
  //
  //   vport_x_scale_ena (bit 0) == 0 -> the GPU applies no viewport scale, so
  //   the shader already exported window space and we must apply the inverse.
  //   == 1 -> the GPU would transform it, so the export is clip space and the
  //   transform here is identity.
  //
  // Measured 0x300 in the captured stream: scale/offset all disabled, xy and z
  // already divided by w. That is the viewport-inverse case, for every draw.
  //
  // The old rule was `identity_in_clip > viewport_in_clip`, a strict > so ties
  // went to viewport. With in-clip commonly 0 for both candidates an unknown
  // share of viewport draws defaulted rather than won. Both counters are kept
  // and a third records disagreement, so the register's answer can be compared
  // against what the contest would have chosen instead of silently replacing it.
  const bool hw_applies_viewport = VportScaleEnabled(device, base);
  const bool contest_says_identity = identity_in_clip > viewport_in_clip;
  if (contest_says_identity != hw_applies_viewport) ++g_hleShaderMvpDisagree;
  // The offset was checked before being acted on, because the derivation
  // disagreed with this file's note that PA_CL_VTE_CNTL is 0x300. A dump of the
  // surrounding dwords settled it: 17 dwords below sits 640.0, then 640.0,
  // -90.0, 90.0, 1.0 — PA_CL_VPORT_XSCALE/XOFFSET/YSCALE/YOFFSET/ZSCALE
  // (0x210F..0x2113), whose own shadow base puts XSCALE exactly there. 640 is
  // half of 1280. The offset is right and the 0x300 note is stale.
  //
  // The register reads 0x43F, one value across every draw: all six viewport
  // enables set, vtx_w0_fmt set. The GPU applies the viewport transform, so the
  // shader exports clip space and the transform here is identity.
  //
  // `contest_says_identity` survives only to feed g_hleShaderMvpDisagree above:
  // it measures how often the old in-clip contest disagreed with the register
  // (82.7% of draws). It is not an input to the choice.
  const bool use_identity = hw_applies_viewport;
  if (use_identity) {
    // The GPU does the viewport transform, so the export is clip space already.
    static constexpr float kIdentity[16] = {
        1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    std::memcpy(dc.mvp, kIdentity, sizeof(dc.mvp));
    ++g_hleShaderIdentityMvp;
  } else {
    ++g_hleShaderViewportMvp;
  }
  ++g_hleShaderDraws;
  g_hleShaderVertices += applied_vertices;
  return ShaderApplyResult::kApplied;
}

// Is this frame's list order the guest's submission order, or just the order
// three workers happened to win one mutex in?
//
// Everything downstream -- the Resolve hook, the finalize below, the graphics
// system's `submittable`, the renderer's inline resolve execution -- is careful
// to preserve the order of this list. None of that helps if the order was
// already wrong when entries arrived. A capture (render.rdc) showed the
// 1280x720 light-buffer snapshot being sampled by all 44 draws of the HDR scene
// pass and written only by copies AFTER that pass ended, so one of the two is
// true and they need opposite fixes:
//
//   resolve and the scene draws on DIFFERENT threads
//       -> we flatten independent streams by arrival, and ordering is the fix
//   all on ONE thread
//       -> we reproduce the guest's order faithfully, the guest really does
//          resolve late, and the defect is that the snapshot does not survive
//          from the frame that filled it
//
// Prints the distinct threads with their entry counts, and every resolve's
// position in the list with its destination, so the two cases are told apart by
// reading one line. Bounded to the first few frames: this is a question with an
// answer, not a counter to watch.
// Who queues draws and who queues resolves.
//
// A first attempt read g_pendingHleDraws expecting a frame's worth of entries.
// It is not that: it is flushed constantly in batches of a handful, so it never
// reached even 100 and the report never fired. The ordered per-frame list is
// HleFrameDraws(); this counts by thread at PUSH time, which does not depend on
// how the batching happens to fall.
//
// If draws and resolves come from different threads, our single global list
// flattens independent guest streams by arrival and ordering is the fix. If
// they share one thread, our order IS the guest's order, the guest really does
// resolve after the pass that samples the result, and the defect is that the
// snapshot does not survive the frame that filled it. Opposite fixes.
void NoteQueueThread(uint32_t thread, bool is_resolve) {
  static std::map<uint32_t, std::pair<uint64_t, uint64_t>> s_byThread;
  auto& e = s_byThread[thread];
  (is_resolve ? e.second : e.first)++;
  static uint64_t s_total = 0;
  if ((++s_total % 20000) != 0) return;
  std::string by;
  for (const auto& [tid, dr] : s_byThread)
    by += fmt::format(" t{:X}={}draws/{}resolves", tid, dr.first, dr.second);
  REXLOG_INFO("d3d9: QUEUE BY THREAD:{}", by);
}

// Where each resolve sits in the frame's ordered list, and how long that list
// is by the end. A resolve at index 12 of 900 is early in the frame; one at 880
// is late. This is the number that says whether the guest resolves before or
// after the draws that sample the result.
void NoteResolvePosition(uint32_t dest, size_t index) {
  static uint32_t s_logged = 0;
  if (s_logged >= 40) return;
  ++s_logged;
  REXLOG_INFO("d3d9: RESOLVE at frame-list index {} -> dest 0x{:08X}", index,
              dest);
}

void FinalizePendingD3D9DrawsImpl(uint8_t* base) {
  const size_t count = g_pendingHleDraws.size();
  uint64_t applied = 0, dropped = 0;
  const auto finalize_t0 = std::chrono::steady_clock::now();
  for (PendingHleDraw& pending : g_pendingHleDraws) {
    // A resolve carries no geometry, so it has no shader to run and no topology
    // to finalize — both would refuse it and it would be counted as a dropped
    // draw. It still has to keep its slot in the frame's ordered list: the
    // snapshot it stands for is the target's contents *at this point*, and
    // every draw after it must sample that rather than the surface's later
    // state.
    if (pending.draw.resolve_dest_texture) {
      NoteResolvePosition(pending.draw.resolve_dest_texture,
                          mx::hle::HleFrameDraws().size());
      mx::hle::HleFrameDraws().push_back(std::move(pending.draw));
      ++applied;
      continue;
    }
    const ShaderApplyResult result = ApplyShaderOutputs(
        pending.draw, pending.vertex_shader, pending.streams.data(),
        pending.device, base,
        pending.have_texture ? &pending.texture_binding : nullptr,
        pending.constants.data());
    if (result == ShaderApplyResult::kApplied && FinishHleDraw(pending.draw))
      ++applied;
    else
      ++dropped;
  }
  g_pendingApplied += applied;
  g_pendingDropped += dropped;
  g_pendingHleDraws.clear();
  const uint64_t finalize_us =
      uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                   std::chrono::steady_clock::now() - finalize_t0)
                   .count());
  if (count && (g_pendingQueued <= 32 || (g_pendingQueued % 1000) < count)) {
    REXLOG_INFO("d3d9: deferred HLE draws: frame {} applied {} dropped {}; "
                "cumulative queued {} applied {} dropped {}",
                count, applied, dropped, g_pendingQueued, g_pendingApplied,
                g_pendingDropped);
  }
  // One unsampled line per frame with the wall-clock gap to the previous one.
  //
  // Every existing frame signal is gated or sampled -- FRAME COST fires only on
  // busy frames, VdSwap logs periodically -- and reading a frame rate off either
  // gave a wrong answer three times in one session, twice in the flattering
  // direction. A frame rate has to come from something that logs every frame and
  // nothing else, so this is it. Cheap: one clock read and one line.
  {
    static auto s_prev = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    const uint64_t gap_us = uint64_t(
        std::chrono::duration_cast<std::chrono::microseconds>(now - s_prev)
            .count());
    s_prev = now;
    static uint64_t s_frame = 0;
    REXLOG_INFO("d3d9: FRAMETIME {} {}us", ++s_frame, gap_us);
  }

  // Read once per frame. Every gated diagnostic below and in the draw path tests
  // this rather than the cvar, so the lookup cannot itself become a per-draw
  // cost -- which is the thing being measured.
  g_diag = REXCVAR_GET(hle_diag);

  // Every frame, not sampled: a slow frame is the one worth seeing, and a
  // sampled report would miss exactly those.
  //
  // The texture bucket is NOT inside finalize: PrepareDrawTexture runs when the
  // draw is recorded, not when the frame is flushed. So the two are reported
  // side by side and not subtracted from one another.
  // Gated on the vertex buckets too. Without them a run whose vertex work has
  // been moved to the GPU logs only its texture-heavy loading frames, and the
  // busy frames -- the ones the whole change is about -- never appear at all.
  // That made an A/B look like a 30% win when the two runs had simply reached
  // different scenes.
  if (finalize_us >= 20000 || g_phaseTextureUs >= 20000 ||
      mx::hle::g_transcodeUs >= 20000 || g_phaseVertexUs >= 20000 ||
      g_phaseVertexCount >= 50000) {
    REXLOG_INFO("d3d9: FRAME COST {} draws {} verts | vertex {}ms = per-vertex "
                "loop {}ms (interpreter {}ms) + per-draw setup {}ms | "
                "transcode {}ms over {} verts (skipped {} draws, {} paid late, "
                "{} lost) | texture {}ms | finalize {}ms",
                g_phaseDrawCount, g_phaseVertexCount, g_phaseVertexUs / 1000,
                g_phaseVertexLoopUs / 1000, g_phaseInterpUs / 1000,
                (g_phaseVertexUs > g_phaseVertexLoopUs
                     ? (g_phaseVertexUs - g_phaseVertexLoopUs) / 1000
                     : 0),
                mx::hle::g_transcodeUs / 1000, mx::hle::g_transcodeVerts,
                g_transcodeDeferred, g_transcodeLate, g_transcodeLost,
                g_phaseTextureUs / 1000, finalize_us / 1000);
    // Who owns the loop. Printed beside the total so the two can be checked
    // against each other rather than trusted separately.
    std::string split;
    for (uint32_t r = 0; r < 3; ++r) {
      split += fmt::format(" {}={}ms/{}v/{}d", kLoopReasonName[r],
                           g_loopUs[r] / 1000, g_loopVerts[r], g_loopDraws[r]);
    }
    REXLOG_INFO("d3d9: LOOP BY REASON{}", split);
  }
  for (uint32_t r = 0; r < 3; ++r)
    g_loopUs[r] = g_loopVerts[r] = g_loopDraws[r] = 0;
  mx::hle::g_transcodeUs = mx::hle::g_transcodeVerts = 0;
  g_transcodeDeferred = g_transcodeLate = g_transcodeLost = 0;
  g_phaseVertexUs = g_phaseInterpUs = g_phaseTextureUs = 0;
  g_phaseVertexLoopUs = g_phaseVertexCount = 0;
  g_phaseDrawCount = 0;
}

//===========================================================================
// Emitter coverage.
//
// Measured before anything renders through it, because the whole plan rests on
// a claim that has not been tested: that a straight-line HLSL emitter can carry
// this game's shaders. If most of them refuse, the wiring downstream is worth
// nothing and the design has to change — so the cheap decisive number comes
// first.
//
// Per distinct shader handle, not per draw: the question is how much of the
// game's shader set is covered, and a hot shader translating 12,000 times would
// otherwise drown out a cold one that fails.
//===========================================================================
struct HlslCoverage {
  uint64_t ok = 0;              // emitted AND compiled
  uint64_t compile_failed = 0;  // emitted, but FXC rejected the source
  std::map<std::string, uint64_t> failures;      // status name -> shaders
  std::map<uint32_t, uint64_t> blocking_opcode;  // opcode -> shaders
};
HlslCoverage g_hlslVs, g_hlslPs;
// map rather than set only because <map> is already included here and <set> is
// not; the value is unused.
std::map<uint32_t, bool> g_hlslReportedVs, g_hlslReportedPs;

// logs/hlsldump, emptied once per process before the first file of the run.
//
// These dumps are named by guest shader HANDLE, and a handle is an address that
// varies per run -- so a stale file neither collides with nor is overwritten by
// the current run's. The directory simply accumulated every run's output with
// nothing to say whose was whose, and that cost real confusion on 2026-08-12: a
// FAILED_ dump written by an earlier binary was read as evidence about the
// current one, and only its mtime settled it.
//
// Cleared lazily at the first dump rather than at startup, so a run that
// translates nothing leaves the previous run's files alone to be read. Both
// dump sites call this; the magic static makes the clear happen exactly once
// however many threads reach it.
void EnsureHlslDumpDir() {
  static const bool s_cleared = [] {
    std::error_code ec;
    std::filesystem::remove_all("logs/hlsldump", ec);
    return true;
  }();
  (void)s_cleared;
  std::error_code ec;
  std::filesystem::create_directories("logs/hlsldump", ec);
}

std::string HlslCoverageSummary(const HlslCoverage& c) {
  std::string s = fmt::format("{} translated+compiled", c.ok);
  if (c.compile_failed) s += fmt::format(", FXC-REJECTED={}", c.compile_failed);
  for (const auto& [why, n] : c.failures) s += fmt::format(", {}={}", why, n);
  if (!c.blocking_opcode.empty()) {
    s += "; blocking opcodes";
    for (const auto& [op, n] : c.blocking_opcode)
      s += fmt::format(" {}x{}", op, n);
  }
  return s;
}

std::map<uint32_t, TranslatedShader> g_translatedPs;
std::map<uint32_t, TranslatedShader> g_translatedVs;

const TranslatedShader* TranslatedPixelShader(uint32_t handle) {
  const auto it = g_translatedPs.find(handle);
  return it == g_translatedPs.end() ? nullptr : &it->second;
}

const TranslatedShader* TranslatedVertexShader(uint32_t handle) {
  const auto it = g_translatedVs.find(handle);
  return it == g_translatedVs.end() ? nullptr : &it->second;
}

void ReportHlslCoverage(mx::hle::HlslStage stage, uint32_t handle,
                        const uint32_t* code, uint32_t count) {
  auto& seen = stage == mx::hle::HlslStage::kPixel ? g_hlslReportedPs
                                                   : g_hlslReportedVs;
  if (!seen.emplace(handle, true).second) return;
  auto& cov = stage == mx::hle::HlslStage::kPixel ? g_hlslPs : g_hlslVs;

  mx::hle::HlslShader out;
  // MUST match the width the renderer's vertex stage offers, or the two
  // signatures cannot link and pipeline creation fails with no message. See
  // kHlslInterpolatorLinkage.
  mx::hle::EmitShaderHlsl(code, count, stage,
                          mx::hle::kHlslInterpolatorLinkage, out);

  // Emitting is only half the claim. Source FXC rejects is exactly as useless
  // as a shader the emitter refused, and the two failures have entirely
  // different causes — so they are counted apart and the compiler's own message
  // is logged, since "it did not compile" without the reason is not a finding.
  std::string compile_error;
  bool compiled = false;
  if (out.status == mx::hle::HlslStatus::kOk) {
    Microsoft::WRL::ComPtr<ID3DBlob> blob, errors;
    const char* target =
        stage == mx::hle::HlslStage::kPixel ? "ps_5_0" : "vs_5_0";
    const HRESULT hr = D3DCompile(
        out.source.data(), out.source.size(), nullptr, nullptr, nullptr,
        "main", target, D3DCOMPILE_OPTIMIZATION_LEVEL0, 0, &blob, &errors);
    compiled = SUCCEEDED(hr) && blob;
    // DIAG (remove before commit): dump the generated HLSL beside the DXBC the
    // compiler produced from it, for every pixel shader that compiles.
    //
    // The reason both halves are needed: a RenderDoc pixel trace numbers its
    // steps by DXBC INSTRUCTION, not by line of our HLSL, so "instruction 147"
    // cannot be located in the source alone. The disassembly is the only thing
    // that maps one to the other. renderdoc-mcp's get_shader refuses these
    // shaders outright ("; Invalid Shader Specified") while still serving
    // reflection, so the capture cannot supply it either.
    //
    // Unconditional rather than cvar-gated: two cvar-gated diagnostics
    // (hle_skip_untextured, hle_dump_shaders) were added this session and
    // NEITHER ever armed in this environment. Bounded to 96 files instead, which
    // is above the ~72 pipelines a menu run builds, so the cap is a safety net
    // rather than a filter.
    //
    // Compiled at OPTIMIZATION_LEVEL0 above, so the DXBC follows the emitted
    // source closely and the mapping stays readable.
    // Read BEFORE the dump below, which prints it. It used to be extracted
    // after, which was harmless only because the dump could not see a failure.
    if (!compiled && errors) {
      compile_error.assign(
          static_cast<const char*>(errors->GetBufferPointer()),
          errors->GetBufferSize());
      if (compile_error.size() > 400) compile_error.resize(400);
    }
    // Dumped whether or not FXC accepted it. This was `if (compiled)`, which
    // made the ONE shader worth reading the one shader never written out: X4532
    // on VS 0x26EFD9A0 (mx_1030) had to be diagnosed from the emitter's source
    // instead of from the source the emitter produced.
    //
    // Failures carry their own budget rather than sharing s_dumped. They are
    // rare and they are the point; letting 160 successes arrive first would
    // starve exactly the file anyone came looking for.
    {
      static uint32_t s_dumped = 0;
      static uint32_t s_dumpedFailed = 0;
      uint32_t& budget = compiled ? s_dumped : s_dumpedFailed;
      // 160 was a menu-sized budget: a freeroam session saturates it, and a
      // saturated cap silently truncates the corpus that
      // `xenos_shader_disasm.py --xenia` diffs against Xenia. Xenia's dump of
      // this title holds 269 distinct blobs (70 vertex, 199 pixel), so 512
      // leaves headroom without pretending to be unbounded. The directory is
      // emptied once per process, and a dump is ~15 KB.
      const uint32_t cap = compiled ? 512u : 64u;
      if (budget < cap) {
        ++budget;
        std::error_code ec;
        EnsureHlslDumpDir();
        char path[128];
        // Vertex shaders included: a light-prepass draw was found exporting a
        // correct SV_Position and then ZERO for every interpolator, which is a
        // defect on the VERTEX side, and the pixel-only dump could not show it.
        //
        // A rejection gets its own prefix so it sorts apart from the ~4900
        // files that compiled, and so `ls FAILED_*` names a run's failures
        // without grepping every file in the directory.
        std::snprintf(path, sizeof(path), "logs/hlsldump/%s%s_%08X.txt",
                      compiled ? "" : "FAILED_",
                      stage == mx::hle::HlslStage::kPixel ? "ps" : "vs",
                      handle);
        std::ofstream f(path, std::ios::trunc | std::ios::binary);
        if (f) {
          f << "; guest "
            << (stage == mx::hle::HlslStage::kPixel ? "pixel" : "vertex")
            << " shader 0x" << std::hex << handle << std::dec
            << "\n; sampler_count " << out.sampler_count << " max_const_index "
            << out.max_const_index << " input_mask 0x" << std::hex
            << out.input_mask << " export_mask 0x" << out.export_mask
            << " dropped_export_mask 0x" << out.dropped_export_mask << std::dec
            << " writes_position " << (out.writes_position ? 1 : 0);
          // Only when non-zero, so its presence in a dump means something.
          if (out.unhonoured_predicate_ops)
            f << "\n; UNHONOURED PREDICATE OPS: "
              << out.unhonoured_predicate_ops
              << " (setp_* translated for its value; p0 is not acted on, so "
                 "this shader may run instructions the console skipped)";
          if (!compiled) f << "\n; FXC REJECTED: " << compile_error;
          // The guest's own bits, so a translation can be checked against its
          // INPUT instead of against itself.
          //
          // The DXBC section below is this file's HLSL compiled, so the two
          // agree by construction -- comparing them only ever proves FXC
          // works. Chasing why the rider's red channel cancels reached exactly
          // that wall: the emitted swizzle `r[2].zwww` and a two-component
          // write mask are either a faithful decode or the bug, and nothing in
          // the file could say which. These dwords can.
          if (code && count) {
            f << "\n\n=== GUEST MICROCODE (" << count << " dwords) ===\n";
            char line[160];
            for (uint32_t i = 0; i < count; i += 8) {
              int n = std::snprintf(line, sizeof(line), "; %04X:", i);
              if (n > 0) f.write(line, n);
              for (uint32_t j = i; j < i + 8 && j < count; ++j) {
                n = std::snprintf(line, sizeof(line), " %08X", code[j]);
                if (n > 0) f.write(line, n);
              }
              f << "\n";
            }
          }
          f << "\n\n=== EMITTED HLSL ===\n" << out.source;
          // Only a shader that compiled has DXBC to disassemble. The section
          // header is inside the guard too, so a failure file does not end with
          // an empty heading that reads like the disassembler broke.
          if (compiled && blob) {
            f << "\n=== DXBC DISASSEMBLY ===\n";
            Microsoft::WRL::ComPtr<ID3DBlob> disasm;
            if (SUCCEEDED(D3DDisassemble(blob->GetBufferPointer(),
                                         blob->GetBufferSize(), 0, nullptr,
                                         &disasm)) &&
                disasm) {
              f.write(static_cast<const char*>(disasm->GetBufferPointer()),
                      std::streamsize(disasm->GetBufferSize()));
            } else {
              f << "; D3DDisassemble failed\n";
            }
          }
        }
      }
    }
  }

  // Read before the move below; the report prints it.
  const size_t source_size = out.source.size();

  if (out.status != mx::hle::HlslStatus::kOk) {
    ++cov.failures[mx::hle::HlslStatusName(out.status)];
    if (out.blocking_opcode) ++cov.blocking_opcode[out.blocking_opcode];
  } else if (compiled) {
    ++cov.ok;
    // Retained only for shaders that both emitted and compiled. A source the
    // compiler rejects must never reach the renderer, which would only discover
    // the same failure later and with less context.
    TranslatedShader& kept = (stage == mx::hle::HlslStage::kPixel
                                  ? g_translatedPs
                                  : g_translatedVs)[handle];
    kept.source = std::make_shared<const std::string>(std::move(out.source));
    kept.input_mask = out.input_mask;
    kept.sampler_mask = out.sampler_mask;
    kept.sampler_count = out.sampler_count;
    kept.sampler_array_mask = out.sampler_array_mask;
    for (uint32_t i = 0; i < out.sampler_count; ++i)
      kept.slot_guest[i] = out.sampler_slot_guest[i];
    kept.max_const_index = out.max_const_index;

    // The vertex fetch variant of the same blob. Emitted and compiled here,
    // beside the one that already works, so a shader whose fetch form is
    // refused or rejected shows up as a counter rather than as a draw that
    // silently stayed slow. Failure is not an error: it means this shader keeps
    // the CPU vertex path, which still renders correctly.
    if (stage == mx::hle::HlslStage::kVertex) {
      mx::hle::HlslShader fetched;
      mx::hle::EmitShaderHlsl(code, count, stage,
                              mx::hle::kHlslInterpolatorLinkage, fetched,
                              /*emit_vertex_fetch=*/true);
      if (fetched.status != mx::hle::HlslStatus::kOk) {
        ++g_vfetchRefused[mx::hle::HlslStatusName(fetched.status)];
      } else {
        Microsoft::WRL::ComPtr<ID3DBlob> fblob, ferrors;
        const HRESULT fhr = D3DCompile(
            fetched.source.data(), fetched.source.size(), nullptr, nullptr,
            nullptr, "main", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL0, 0,
            &fblob, &ferrors);
        if (SUCCEEDED(fhr) && fblob) {
          // DIAG (remove before commit): dump the FETCH variant separately.
          // This is the form that actually runs for a gpuVertexFetch draw, and
          // it is NOT the blob dumped at the main compile site above -- a
          // light-prepass draw was found exporting a correct SV_Position and
          // then zero for every interpolator, and only this variant can show
          // why. Written before the source is moved out of `fetched`.
          {
            static uint32_t s_vf_dumped = 0;
            // Saturated at 96 in a freeroam run; see the cap note above.
            if (s_vf_dumped < 256) {
              ++s_vf_dumped;
              EnsureHlslDumpDir();
              char vpath[128];
              std::snprintf(vpath, sizeof(vpath),
                            "logs/hlsldump/vsfetch_%08X.txt", handle);
              std::ofstream vf(vpath, std::ios::trunc | std::ios::binary);
              if (vf) {
                vf << "; guest vertex shader 0x" << std::hex << handle
                   << std::dec << " FETCH VARIANT\n; input_mask 0x" << std::hex
                   << fetched.input_mask << " export_mask 0x"
                   << fetched.export_mask << " dropped_export_mask 0x"
                   << fetched.dropped_export_mask << std::dec
                   << " writes_position " << (fetched.writes_position ? 1 : 0)
                   << " vertex_fetch_count " << fetched.vertex_fetch_count
                   << "\n\n=== EMITTED HLSL ===\n"
                   << fetched.source << "\n=== DXBC DISASSEMBLY ===\n";
                Microsoft::WRL::ComPtr<ID3DBlob> fdis;
                if (SUCCEEDED(D3DDisassemble(fblob->GetBufferPointer(),
                                             fblob->GetBufferSize(), 0, nullptr,
                                             &fdis)) &&
                    fdis) {
                  vf.write(static_cast<const char*>(fdis->GetBufferPointer()),
                           std::streamsize(fdis->GetBufferSize()));
                } else {
                  vf << "; D3DDisassemble failed\n";
                }
              }
            }
          }
          kept.fetch_source =
              std::make_shared<const std::string>(std::move(fetched.source));
          kept.vertex_fetch_count = fetched.vertex_fetch_count;
          for (uint32_t i = 0; i < fetched.vertex_fetch_count; ++i)
            kept.vertex_fetch_slot[i] = fetched.vertex_fetch_slot[i];
          ++g_vfetchCompiled;
        } else {
          ++g_vfetchRefused["FXC rejected"];
          static uint32_t s_vf_logged = 0;
          if (s_vf_logged++ < 6 && ferrors) {
            std::string msg(
                static_cast<const char*>(ferrors->GetBufferPointer()),
                ferrors->GetBufferSize());
            if (msg.size() > 400) msg.resize(400);
            REXLOG_INFO("d3d9: VFETCH VS 0x{:08X} FXC REJECTED: {}", handle,
                        msg);
          }
        }
      }
    }

    // One line per distinct VERTEX shader. There are 32 of them in a run, so
    // this is bounded and unconditional. A vertex shader that samples is the
    // shape a bone-matrix palette takes when the engine binds it as
    // g_BoneMatrixVectors rather than as a constant array, and it is currently
    // refused the GPU vertex path -- see the g_gpuVertexVsSamplers counter.
    if (stage == mx::hle::HlslStage::kVertex) {
      REXLOG_INFO(
          "d3d9: VS census 0x{:08X}: samplers {} (mask 0x{:X}) inputs 0x{:08X} "
          "max const c{}",
          handle, out.sampler_count, out.sampler_mask, out.input_mask,
          out.max_const_index);
    }
    if (out.sampler_array_mask) {
      REXLOG_INFO("d3d9: HLSL {} 0x{:08X} declares cube slots 0x{:X}{}",
                  stage == mx::hle::HlslStage::kPixel ? "PS" : "VS",
                  handle, out.sampler_array_mask,
                  out.cube_fetch_without_cube_op
                      ? " -- WITHOUT a cube ALU op, coordinate form unverified"
                      : "");
    }
  } else {
    ++cov.compile_failed;
  }

  const char* tag = stage == mx::hle::HlslStage::kPixel ? "PS" : "VS";
  // Every compile failure, not just the first few: this is the one outcome that
  // means the emitter produced something plausible-looking and wrong, and each
  // distinct message is a separate defect to fix.
  if (!compile_error.empty()) {
    static uint32_t s_logged = 0;
    if (s_logged++ < 12) {
      REXLOG_INFO("d3d9: HLSL {} 0x{:08X} FXC REJECTED: {}", tag, handle,
                  compile_error);
    }
  }
  // The first few in full, so a failure can be read rather than inferred from a
  // count, and the source itself can be eyeballed for obvious nonsense.
  if (seen.size() <= 6) {
    REXLOG_INFO("d3d9: HLSL {} 0x{:08X}: {} ({} dwords) inputs 0x{:X} "
                "exports 0x{:X} samplers 0x{:X} consts<={} {}",
                tag, handle, mx::hle::HlslStatusName(out.status), count,
                out.input_mask, out.export_mask, out.sampler_mask,
                out.max_const_index,
                out.status == mx::hle::HlslStatus::kOk
                    ? fmt::format("{} bytes", source_size)
                    : fmt::format("opcode {}", out.blocking_opcode));
  }
  if ((seen.size() % 16) == 0 || seen.size() <= 6) {
    REXLOG_INFO("d3d9: HLSL {} coverage over {} shaders: {}", tag, seen.size(),
                HlslCoverageSummary(cov));
  }
  // Every new vertex shader, not on the coverage report's schedule: that one
  // fires at 6 and then at multiples of 16, so a run ending at 11 shaders never
  // shows its final tally -- which is exactly what happened the first time.
  if (stage == mx::hle::HlslStage::kVertex) {
    std::string refused;
    for (const auto& [why, n] : g_vfetchRefused)
      refused += fmt::format(" {}={}", why, n);
    REXLOG_INFO(
        "d3d9: VFETCH coverage: {} of {} vertex shaders fetch on the GPU;{}",
        g_vfetchCompiled, seen.size(),
        refused.empty() ? " none refused" : refused);
  }
}

void CollectPixelShaderBlob(uint32_t handle, uint8_t* base) {
  (void)base;
  if (!handle || g_psBlobs.count(handle)) return;
  // D3DDevice_CreatePixelShader (0x82552148) copies the source header to
  // object+0x28, allocates pFunction[2] code bytes separately, and
  // sub_825506B0 stores that allocation at object+0x18. Pixel shader objects
  // do not share the vertex shader's inline +0x368 representation.
  //
  // **The CF stream does not start at the beginning of that allocation.** Big
  // shaders carry a prologue — the first dwords read as zeros — and only the
  // small ones begin at dword 0. This used to be worked around by searching
  // the blob for what PM4 had loaded, and failing that by trying every offset
  // and accepting a unique valid decode, which left 14 shaders on
  // "ambiguous CF offset" and no texture bindings at all.
  //
  // Neither is needed: the object states where the code begins. From the
  // shader flush sub_82565928, which is what actually programs the GPU:
  //
  //     v22 = *((char *)v8 + v8[16] + 40) + v8[6];        // program base
  //     *v23 = *((char *)v8 + v8[16] + 44) >> 2;          // size in dwords
  //
  // with v8 the shader object, v8[6] = +0x18 (the code allocation) and
  // v8[16] = +0x40 (the offset of an info block within the object). So the
  // address D3D9 hands the hardware is
  //
  //     *(object+0x18) + *(object + *(object+0x40) + 0x28)
  //
  // and its length is *(object + *(object+0x40) + 0x2C). Read out of the
  // consuming code, never guessed — same rule as every offset in AGENTS.md's
  // device table. +0x30 is the *allocation* size and is kept only as a bound.
  constexpr uint32_t kPsCodePointerAt = 0x18;
  constexpr uint32_t kPsAllocSizeAt = 0x30;
  constexpr uint32_t kPsInfoOffsetAt = 0x40;
  constexpr uint32_t kPsInfoCodeOffset = 0x28;
  constexpr uint32_t kPsInfoCodeSize = 0x2C;
  if (!HostPageReadable(REX_RAW_ADDR(handle + kPsCodePointerAt)) ||
      !HostPageReadable(REX_RAW_ADDR(handle + kPsAllocSizeAt)) ||
      !HostPageReadable(REX_RAW_ADDR(handle + kPsInfoOffsetAt)))
    return;
  uint32_t code = REX_LOAD_U32(handle + kPsCodePointerAt);
  uint32_t size_bytes = REX_LOAD_U32(handle + kPsAllocSizeAt);
  if (!size_bytes || size_bytes > kMaxBlobDwords * 4 || (size_bytes & 3))
    return;
  if (!code || !HostPageReadable(REX_RAW_ADDR(code))) return;

  // Narrow the allocation to the program the hardware is actually given. Both
  // fields are bounds-checked against the allocation rather than trusted: a
  // bad info offset must degrade to the old whole-allocation behaviour, not
  // read off the end.
  const uint32_t info = REX_LOAD_U32(handle + kPsInfoOffsetAt);
  if (info && info < 0x10000 &&
      HostPageReadable(REX_RAW_ADDR(handle + info + kPsInfoCodeSize))) {
    const uint32_t off = REX_LOAD_U32(handle + info + kPsInfoCodeOffset);
    const uint32_t len = REX_LOAD_U32(handle + info + kPsInfoCodeSize);
    if ((off & 3) == 0 && (len & 3) == 0 && len && off + len <= size_bytes) {
      code += off;
      size_bytes = len;
    }
  }
  std::vector<uint32_t> blob(size_bytes / 4);
  for (uint32_t i = 0; i < blob.size(); ++i) {
    const uint32_t at = code + i * 4;
    if ((at & (kHostPageSize - 1)) == 0 &&
        !HostPageReadable(REX_RAW_ADDR(at))) {
      blob.resize(i);
      break;
    }
    blob[i] = REX_LOAD_U32(at);
  }
  if (!blob.empty()) {
    static uint32_t s_logged = 0;
    if (s_logged++ < 8) {
      REXLOG_INFO("d3d9: captured pixel shader object 0x{:08X}: "
                  "{} dwords from 0x{:08X}, code {:08X} {:08X} {:08X}",
                  handle, blob.size(), code, blob[0],
                  blob.size() > 1 ? blob[1] : 0,
                  blob.size() > 2 ? blob[2] : 0);
    }
    g_psBlobs.emplace(handle, std::move(blob));
  }
}

// PROBE, not a fix: is the vertex object's SECOND blob a pixel program?
//
// 120,000 menu draws bind a NULL pixel shader (see the NO-PS DEVICES tally in
// PrepareDrawTexture) and so keep the tex*col stand-in. The guest's own PM4
// flush sub_82565928 has an explicit path for them: with no pixel object, and
// when `vs[218] & 0x20`, it emits a second blob selected by `vs[226]` instead
// of the usual `vs[224]`. The open question is what that blob IS.
//
// The packet encoding argues it is a VERTEX program, not the missing pixel one.
// Both blobs are loaded with PM4_IM_LOAD (opcode 0x27), whose first data dword
// carries the shader type in its low two bits -- kVertex 0, kPixel 1, per
// Xenia's ExecutePacketType3_IM_LOAD. The real pixel path forces that bit:
//
//     (v22 & 0x1FFFFFFE) | 1        <- pixel object, type = kPixel
//      v69 & 0x1FFFFFFF             <- vs[224] AND vs[226] alike, type = kVertex
//
// If that reading is right, these draws emit no pixel IM_LOAD at all and simply
// inherit whichever pixel program was loaded last -- a completely different fix
// from reading a blob, so it is worth one run to know rather than guessing.
//
// Decided by structure, not by inference. Both blobs are emitted as each stage
// and the results compared: a vertex program exports position (register 62) and
// a pixel program exports colour (0-3). vs[224] is known-vertex and is probed
// alongside purely as a control -- if the control does not come out vertex, the
// offsets are wrong and nothing else here should be believed.
//
// Layout read out of the decompile, never guessed: the blob header sits at
// `vs + vs[table] + 872`, and within it dword 0 is the microcode offset (added
// to `vs[8]`), dword 1 the size in BYTES, dwords 2 and 3 the SQ_PROGRAM_CNTL
// and SQ_CONTEXT_MISC the flush later writes as a type-0 packet to 0x2180.
// `base` is not unused: REX_LOAD_U32 and REX_RAW_ADDR expand to reference it by
// name, exactly as in CollectPixelShaderBlob above.
void ProbeVertexObjectSecondBlob(uint32_t device, uint8_t* base) {
  (void)base;
  static std::mutex s_mu;
  static std::set<uint32_t> s_seenVs;
  static uint64_t s_withSecond = 0, s_withoutSecond = 0;
  if (!device || !HostPageReadable(REX_RAW_ADDR(device + 0x3248))) return;
  const uint32_t vs = REX_LOAD_U32(device + 0x3248);
  // Deduplicated by VERTEX OBJECT, not by call count. The first cut capped at
  // two reports and both landed on the same object, which says nothing about
  // whether the rest of the population looks like it -- the one question the
  // probe exists to answer. The tally below covers every draw; the expensive
  // decode runs only for the first few distinct objects.
  bool decode = false;
  {
    std::lock_guard<std::mutex> lk(s_mu);
    const bool fresh = s_seenVs.insert(vs).second;
    decode = fresh && s_seenVs.size() <= 8;
  }
  // vs[218] is the flag word, and 872 == 218 * 4 is the same place: the blob
  // headers are addressed from it, so one readability check covers both.
  if (!vs || !HostPageReadable(REX_RAW_ADDR(vs + 872)) ||
      !HostPageReadable(REX_RAW_ADDR(vs + 32)))
    return;
  const uint32_t flags = REX_LOAD_U32(vs + 872);
  const uint32_t ucode_base = REX_LOAD_U32(vs + 32);

  auto probe_one = [&](const char* what, uint32_t table_dword) {
    const uint32_t table_at = vs + table_dword * 4;
    if (!HostPageReadable(REX_RAW_ADDR(table_at))) return;
    const uint32_t hdr = vs + REX_LOAD_U32(table_at) + 872;
    if (!HostPageReadable(REX_RAW_ADDR(hdr)) ||
        !HostPageReadable(REX_RAW_ADDR(hdr + 12)))
      return;
    const uint32_t ucode_off = REX_LOAD_U32(hdr);
    const uint32_t size_bytes = REX_LOAD_U32(hdr + 4);
    const uint32_t prog_cntl = REX_LOAD_U32(hdr + 8);
    const uint32_t ctx_misc = REX_LOAD_U32(hdr + 12);
    if (!size_bytes || (size_bytes & 3) || size_bytes > kMaxBlobDwords * 4) {
      REXLOG_INFO("d3d9: PROBE {} vs 0x{:08X}: bad size {} -- offsets wrong?",
                  what, vs, size_bytes);
      return;
    }
    const uint32_t addr = ucode_off + ucode_base;
    if (!addr || !HostPageReadable(REX_RAW_ADDR(addr))) return;
    std::vector<uint32_t> code(size_bytes / 4);
    for (uint32_t i = 0; i < code.size(); ++i) {
      const uint32_t at = addr + i * 4;
      if ((at & (kHostPageSize - 1)) == 0 &&
          !HostPageReadable(REX_RAW_ADDR(at))) {
        code.resize(i);
        break;
      }
      code[i] = REX_LOAD_U32(at);
    }
    if (code.empty()) return;

    mx::hle::HlslShader as_vs{}, as_ps{};
    EmitShaderHlsl(code.data(), uint32_t(code.size()),
                   mx::hle::HlslStage::kVertex,
                   mx::hle::kHlslInterpolatorLinkage, as_vs);
    EmitShaderHlsl(code.data(), uint32_t(code.size()),
                   mx::hle::HlslStage::kPixel,
                   mx::hle::kHlslInterpolatorLinkage, as_ps);
    REXLOG_INFO(
        "d3d9: PROBE {} vs 0x{:08X} ucode 0x{:08X} {} dwords "
        "prog_cntl 0x{:08X} ctx_misc 0x{:08X}; head {:08X} {:08X} {:08X} "
        "{:08X}; AS-VERTEX {} writes_position {} export 0x{:X}; AS-PIXEL {} "
        "colour 0x{:X} writes_depth {}",
        what, vs, addr, code.size(), prog_cntl, ctx_misc, code[0],
        code.size() > 1 ? code[1] : 0, code.size() > 2 ? code[2] : 0,
        code.size() > 3 ? code[3] : 0,
        mx::hle::HlslStatusName(as_vs.status), as_vs.writes_position ? 1 : 0,
        as_vs.export_mask, mx::hle::HlslStatusName(as_ps.status),
        as_ps.export_mask, as_ps.writes_depth ? 1 : 0);
  };

  // The population question, over EVERY null-PS draw rather than the sampled
  // few: if the second blob is absent throughout, the guest emits no pixel
  // IM_LOAD for any of them and they inherit whatever was loaded last.
  {
    std::lock_guard<std::mutex> lk(s_mu);
    if (flags & 0x20)
      ++s_withSecond;
    else
      ++s_withoutSecond;
    if (((s_withSecond + s_withoutSecond) % 20000) == 0) {
      REXLOG_INFO("d3d9: PROBE population: {} draws with a second blob, {} "
                  "without, over {} distinct vertex objects",
                  s_withSecond, s_withoutSecond, s_seenVs.size());
    }
  }
  if (!decode) return;

  REXLOG_INFO("d3d9: PROBE vertex object 0x{:08X} flags 0x{:08X}, second blob {}",
              vs, flags, (flags & 0x20) ? "PRESENT" : "ABSENT");
  probe_one("blob[224] (control, known vertex)", 224);
  if (flags & 0x20) probe_one("blob[226] (the question)", 226);
}

struct ResolvedPixelBinding {
  std::vector<mx::hle::PixelTextureBinding> bindings;
  const char* fail = nullptr;
  uint32_t code_offset_dwords = 0;
  bool decoded = false;
};
std::map<uint32_t, ResolvedPixelBinding> g_resolvedPixelBindings;
std::map<uint64_t, std::shared_ptr<const mx::hle::HleTexturePayload>>
    g_hleCpuTextures;
// Keys whose decode came out entirely zero. This used to be a set-and-forget
// flag, which made "blank" permanent: a texture sampled once while the guest
// was still streaming into it could never be reconsidered, because the key
// hashes the six fetch dwords -- where the texture lives and what shape it is
// -- and never its contents.
//
// Retrying is what lets a streamed texture appear, but it cannot be free.
// Measured on the attract sequence, the blank set is three FMT_8_8_8_8
// surfaces, one 2048x2048 and two at 1280x720 -- the game's render resolution,
// so they are surfaces the GPU rendered into whose guest copy is legitimately
// empty and will never fill in. Re-untiling ~9 MB of those every frame forever
// is pure waste, so each retry that comes back blank doubles the wait before
// the next one, up to a cap. A texture that is about to arrive is retried
// almost immediately; one that never arrives settles into costing nothing.
struct BlankState {
  uint64_t last_frame = 0;
  uint32_t strikes = 0;
};
std::map<uint64_t, BlankState> g_hleEmptyTextures;

// How many frames to wait after `strikes` consecutive blank decodes.
uint64_t BlankRetryDelay(uint32_t strikes) {
  constexpr uint32_t kMaxShift = 7;  // 128 frames, ~2s at 60fps
  return uint64_t(1) << std::min(strikes, kMaxShift);
}

// True when a key found blank before is due another look. Unknown keys are due
// by definition -- they have not been tried.
bool BlankRetryDue(uint64_t key) {
  auto it = g_hleEmptyTextures.find(key);
  if (it == g_hleEmptyTextures.end()) return true;
  const uint64_t now = mx::hle::D3D9FrameCount();
  return now >= it->second.last_frame + BlankRetryDelay(it->second.strikes);
}

//===========================================================================
// Scaleform's raster glyph cache, and why a texture cache keyed on the fetch
// constant cannot see it change.
//
// The UI is Scaleform GFx 3.x ("Warning: Increase raster glyph cache capacity
// - TextureConfig." at 0x820D98D8). It keeps ONE 512x512 FMT_8 atlas per font
// and repacks it at runtime as strings appear and disappear -- dumping the
// decoded payload three times in one run caught it holding "Loading" /
// "Press START", then "PHOENIX...", then nearly empty mid-rewrite.
//
// The guest side, from the IDB:
//
//   sub_8293E720  rasterises one glyph into the cache. It writes rows straight
//                 into the cache buffer -- `sub_82BDB3C0(row, 0, w)` to clear
//                 and `sub_82BDAAF0(dst, src, w)` to copy, addressed as
//                 `(y)*tex[5] + tex[6]` where tex = *(cache+696), [5] is the
//                 pitch and [6] the base -- then records a dirty rect through
//                 sub_8293DA08.
//   sub_8293C778  FLUSHES those rects: it walks the texture slots at +56
//                 (stride 5, matching sub_8293A888's `cache[5*i + 14]`),
//                 gathers the rects belonging to each, calls the texture's
//                 vtable slot 3 -- GTexture::Update(level, n, rects, image) --
//                 and then clears the count at +28.
//
// So sub_8293C778 is the exact moment the atlas contents change, and the
// pending-rect count at +28 says whether this call will change anything. That
// is the signal, and it costs nothing on frames where no glyph moved -- which
// is why it is worth decompiling for rather than hashing every texture every
// frame.
//
// It does NOT say which host texture changed, only that the glyph atlases did.
// Invalidating every cached single-channel texture is precise enough: kR8 is
// the glyph format, and the only other kR8 textures in a run are two 32x32
// ones, so the over-invalidation costs two trivial re-decodes.
//===========================================================================
uint32_t g_glyphCacheGeneration = 1;
uint64_t g_glyphCacheFlushes = 0;

bool IsGlyphCacheFormat(mx::hle::HostTextureFormat format) {
  return format == mx::hle::HostTextureFormat::kR8;
}

// True when this cached payload predates the newest glyph-cache flush and so
// may be showing glyphs that have since been repacked.
bool GlyphCacheStale(const mx::hle::HleTexturePayload& payload) {
  return IsGlyphCacheFormat(payload.format) &&
         payload.content_version != g_glyphCacheGeneration;
}

void NoteBlankDecode(uint64_t key) {
  BlankState& s = g_hleEmptyTextures[key];
  s.last_frame = mx::hle::D3D9FrameCount();
  ++s.strikes;
}
// The blank payload itself, so the draws that sample a still-blank key within
// one frame share a decode instead of repeating it.
std::map<uint64_t, std::shared_ptr<const mx::hle::HleTexturePayload>>
    g_hleBlankPayloads;

const ResolvedPixelBinding* ResolvePixelProfile(uint32_t handle) {
  // This used to search every PM4-captured pixel shader for a byte match inside
  // the D3D9 allocation, to locate where the CF stream began. CollectPixelShaderBlob
  // now reads that offset out of the shader object itself, the same field
  // sub_82565928 reads when it programs the hardware, so the search has nothing
  // left to find and the resolve is final on the first try.
  auto known = g_resolvedPixelBindings.find(handle);
  if (known != g_resolvedPixelBindings.end()) return &known->second;
  auto bi = g_psBlobs.find(handle);
  if (bi == g_psBlobs.end()) return nullptr;

  ResolvedPixelBinding resolved;
  const uint32_t* code = bi->second.data();
  uint32_t code_count = uint32_t(bi->second.size());
  // Applied here rather than at startup: this is the first point every run
  // reaches before any shader is emitted, and the emitter has no cvar access.
  ReportHlslCoverage(mx::hle::HlslStage::kPixel, handle, code, code_count);
  resolved.decoded = mx::hle::DecodePixelTextureFetches(
      code, code_count, resolved.bindings, &resolved.fail);
  if (!resolved.decoded) {
    // Pixel shader allocations with literal constants place those values in
    // front of the CF stream (the loaded main-pass shaders consistently begin
    // at dword 16). Do not hardcode that observation: try a bounded set of
    // suffixes and accept only a unique valid decode. A second valid alignment
    // makes the blob ambiguous and leaves the draw on the colour fallback.
    uint32_t valid_offsets = 0;
    std::vector<mx::hle::PixelTextureBinding> unique_bindings;
    uint32_t unique_offset = 0;
    const uint32_t limit =
        std::min<uint32_t>(64, uint32_t(bi->second.size()));
    for (uint32_t offset = 1; offset + 3 <= limit; ++offset) {
      std::vector<mx::hle::PixelTextureBinding> candidate;
      const char* candidate_fail = nullptr;
      if (!mx::hle::DecodePixelTextureFetches(
              bi->second.data() + offset,
              uint32_t(bi->second.size()) - offset, candidate,
              &candidate_fail))
        continue;
      ++valid_offsets;
      unique_offset = offset;
      unique_bindings = std::move(candidate);
    }
    if (valid_offsets == 1) {
      resolved.decoded = true;
      resolved.fail = nullptr;
      resolved.code_offset_dwords = unique_offset;
      resolved.bindings = std::move(unique_bindings);
    } else if (valid_offsets > 1) {
      resolved.fail = "ambiguous CF offset in D3D9 allocation";
    }
  }
  g_resolvedPixelBindings[handle] = std::move(resolved);
  auto& profile = g_resolvedPixelBindings[handle];
  std::string linkage;
  for (const auto& b : profile.bindings) {
    linkage += fmt::format(" s{}<-r{}{}", b.sampler, b.src_reg,
                           b.unnormalized ? "(unnorm)" : "");
  }
  REXLOG_INFO("d3d9: pixel shader 0x{:08X} texture profile: {}{}{}; source {}",
              handle,
              profile.decoded
                  ? fmt::format("{} 2D fetch(es)", profile.bindings.size())
                  : "rejected",
              linkage,
              profile.decoded
                  ? ""
                  : fmt::format(" ({})", profile.fail ? profile.fail : "?"),
              profile.code_offset_dwords
                  ? fmt::format("unique CF suffix at blob+0x{:X}",
                                profile.code_offset_dwords * 4)
                  : fmt::format("whole {}-dword D3D9 allocation",
                                bi->second.size()));
  if (!profile.decoded) {
    static std::map<uint32_t, bool> s_dumped_rejected;
    if (s_dumped_rejected.size() < 16 &&
        s_dumped_rejected.emplace(handle, true).second) {
      std::string words;
      uint32_t shown = 0;
      for (uint32_t i = 0; i < bi->second.size() && shown < 16; ++i) {
        if (!bi->second[i]) continue;
        words += fmt::format(" [{}]={:08X}", i, bi->second[i]);
        ++shown;
      }
      REXLOG_INFO("d3d9: rejected pixel shader 0x{:08X} first nonzero "
                  "allocation dwords:{}",
                  handle, words.empty() ? " none" : words);
    }
  }
  return &profile;
}

bool ResolvePixelBinding(uint32_t handle,
                         mx::hle::PixelTextureBinding& out) {
  const ResolvedPixelBinding* profile = ResolvePixelProfile(handle);
  if (!profile || !profile->decoded || profile->bindings.empty())
    return false;
  if (profile->bindings.size() == 1) {
    out = profile->bindings.front();
    return true;
  }

  // Multiple fetch instructions may still describe one host texture. Blur
  // passes in ST_Southwest issue 3 or 9 taps of s0 from the same interpolator;
  // base-mip HLE cannot reproduce their offsets/ALU yet, but one s0 sample is
  // the explicit approximation this milestone permits.
  const auto& first = profile->bindings.front();
  bool same_linkage = true;
  bool same_interpolator = true;
  bool same_sampler = true;
  for (const auto& b : profile->bindings) {
    same_interpolator = same_interpolator && b.src_reg == first.src_reg;
    same_linkage = same_linkage && b.src_reg == first.src_reg &&
                   b.src_swizzle == first.src_swizzle &&
                   b.unnormalized == first.unnormalized;
    same_sampler = same_sampler && b.sampler == first.sampler;
  }
  if (same_linkage && same_sampler) {
    out = first;
    return true;
  }

  // Evidence-selected final compositor profile, measured on the 1280x720
  // ST_Southwest draw: s0 is the resolved 1280x720 scene, s1 is 160x90, s2 is
  // 1x1 and s3 is another full-size input. Select only the base scene and only
  // for this exact fetch order/linkage; do not generalise "sampler zero wins"
  // to unrelated shaders.
  static constexpr uint32_t kFinalSamplers[4] = {3, 1, 2, 0};
  if (same_interpolator &&
      profile->bindings.size() == std::size(kFinalSamplers)) {
    bool exact = true;
    for (uint32_t i = 0; i < std::size(kFinalSamplers); ++i)
      exact = exact && profile->bindings[i].sampler == kFinalSamplers[i];
    if (exact) {
      out = profile->bindings.back();  // s0, the resolved 1280x720 scene.
      static bool s_logged = false;
      if (!s_logged) {
        s_logged = true;
        REXLOG_INFO("d3d9: selected s0 base scene for observed "
                    "four-input final compositor shader 0x{:08X}", handle);
      }
      return true;
    }
  }
  return false;
}

// Texture fetch constants embedded in a shader object's state-patch list.
//
// SetPixelShader and SetVertexShader both walk the same three-part block. The
// first list emits LOAD_ALU_CONSTANT packets. The SECOND list is different:
// each entry is `(u16 byte_offset, u16 dword_count, inline payload)` and the
// guest copies that payload to `device + 0x480 + byte_offset`. The first 0xC0
// bytes of that device block are the 32 six-dword texture fetch constants.
//
// This distinction is verified directly in the recompiled guest functions
// sub_825506E8 and sub_825508A8. An earlier implementation searched the first
// list for ALU register indexes reaching 0x4800; no shader published such an
// entry, so the runtime correctly reported zero captured descriptors.
//
// Cached per pixel-shader handle rather than re-walked per draw. The pixel and
// vertex shader patch lists are merged because either may carry state for the
// shared device constants block used by the draw.
struct ShaderFetchConstants {
  static constexpr uint32_t kDwords = 6;
  uint32_t words[mx::hle::kMaxSamplers * kDwords] = {};
  // Which of the six dwords of each sampler have arrived. A descriptor can be
  // split across entries, and the two halves are only usable together.
  uint8_t partial[mx::hle::kMaxSamplers] = {};
  // Bit s set = ALL SIX dwords of sampler s arrived. A partial publish is not
  // usable -- DescribeHleTexture2D reads all six and would describe a texture
  // out of half a descriptor and half zeros, which is worse than reporting the
  // slot unbound.
  uint32_t complete_mask = 0;
};
std::mutex g_shaderFetchMu;
std::map<uint32_t, ShaderFetchConstants> g_shaderFetch;
// Slot fills served from a shader-embedded descriptor rather than the device
// shadow. Reported beside the unbound-sampler counts, because these two are the
// same population before and after the fix and only mean something together.
std::atomic<uint64_t> g_shaderFetchServed{0};
std::atomic<uint64_t> g_shaderFetchPublished{0};

// Copy sampler `sampler`'s published fetch constant, if this shader published a
// complete one. Returns false otherwise, leaving `out` untouched.
bool ShaderPublishedFetch(uint32_t shader, uint32_t sampler, uint32_t out[6]) {
  if (!shader || sampler >= mx::hle::kMaxSamplers) return false;
  std::lock_guard<std::mutex> lock(g_shaderFetchMu);
  const auto it = g_shaderFetch.find(shader);
  if (it == g_shaderFetch.end()) return false;
  if (!(it->second.complete_mask & (1u << sampler))) return false;
  std::memcpy(out, &it->second.words[sampler * ShaderFetchConstants::kDwords],
              sizeof(uint32_t) * 6);
  return true;
}

// `ps_handle`, when given, is the pixel shader whose load table may carry this
// sampler's descriptor. Pass the PER-DEVICE handle (PixelShaderForDeviceStrict),
// never the thread-local one: DeviceState() is thread_local and a worker-thread
// draw would name the wrong shader, which here would mean binding another
// draw's texture rather than merely missing one.
bool ReadLiveTextureFetch(uint32_t device, uint8_t* base, uint32_t sampler,
                          uint32_t out[6], uint32_t ps_handle = 0) {
  if (!out || sampler >= mx::hle::kMaxSamplers) return false;
  std::memset(out, 0, sizeof(uint32_t) * 6);
  const uint32_t fetch_at = device + 0x480 + sampler * 24;
  if (device && HostPageReadable(REX_RAW_ADDR(fetch_at)) &&
      HostPageReadable(REX_RAW_ADDR(fetch_at + 20))) {
    for (uint32_t i = 0; i < 6; ++i)
      out[i] = REX_LOAD_U32(fetch_at + i * 4);
    // FetchConstantType::kTexture == 2 (SDK rex/graphics/xenos.h:1093-1098).
    if ((out[0] & 3u) == 2u) return true;
  }
  // Second, and only when the device shadow has nothing: the descriptor the
  // shader published for itself. A live SetTexture must still win, so this sits
  // below the shadow rather than above it.
  if (ps_handle) {
    uint32_t published[6] = {};
    if (ShaderPublishedFetch(ps_handle, sampler, published) &&
        (published[0] & 3u) == 2u) {
      std::memcpy(out, published, sizeof(uint32_t) * 6);
      ++g_shaderFetchServed;
      return true;
    }
  }
  const auto& tb = DeviceState().texture[sampler];
  if (!tb.bound || !tb.valid) return false;
  std::memcpy(out, tb.fetch, sizeof(uint32_t) * 6);
  return true;
}

// The milestone can sample one texture even when the guest shader uses many.
// Pick from evidence in the live descriptors: normalized colour storage is a
// closer approximation to the shader's visible base colour than BC5 normal
// maps, float intermediates, or unnormalized render-target inputs. Ties retain
// shader instruction order; no sampler number is treated as a semantic.
// Per-guest-format tally of descriptors the HLE decoder turned down, shared by
// both rejection sites. This replaced a flat "log the first 12" cap, which
// could spend its whole budget on one format and leave every other one
// invisible — the reason "unsupported texture format" has never once told us
// which format to add. Keyed by the base format index; the value counts
// sightings and the first of each is logged in full.
std::map<uint32_t, uint64_t> g_hleRejectedFormats;

void NoteRejectedTextureFormat(const char* site, uint32_t sampler,
                               const mx::hle::HleTextureSource& source,
                               const char* why, const uint32_t fetch[6]) {
  const uint32_t fmt = source.guest_format;
  const bool first = ++g_hleRejectedFormats[fmt] == 1;
  if (!first) return;
  REXLOG_INFO("d3d9: HLE texture reject [{}]: sampler {} format {} ({}) — {}; "
              "words {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}",
              site, sampler, fmt, mx::hle::GuestTextureFormatName(fmt),
              why ? why : "?", fetch[0], fetch[1], fetch[2], fetch[3],
              fetch[4], fetch[5]);
}

// Renders the tally as "4:FMT_5_6_5=1832 15:FMT_4_4_4_4=97", ranked by count,
// for the periodic summary. Empty string when nothing has been rejected.
std::string RejectedFormatSummary() {
  std::vector<std::pair<uint64_t, uint32_t>> ranked;
  ranked.reserve(g_hleRejectedFormats.size());
  for (const auto& [fmt, count] : g_hleRejectedFormats)
    ranked.emplace_back(count, fmt);
  std::sort(ranked.begin(), ranked.end(), std::greater<>());
  std::string out;
  for (const auto& [count, fmt] : ranked) {
    if (!out.empty()) out += ' ';
    out += std::to_string(fmt);
    out += ':';
    out += mx::hle::GuestTextureFormatName(fmt);
    out += '=';
    out += std::to_string(count);
  }
  return out;
}

// The guest's Bink frame composite, identified from the binary rather than by
// heuristic. sub_8234D630 (XenonBinkVideo vtable [8]) clears a render target,
// calls sub_8234C7C0, then Resolves into a texture. sub_8234C7C0 binds three
// plane textures to samplers 0/1/2 — Y, Cr, Cb — plus an optional alpha plane
// on sampler 3 whose presence selects the second pixel shader. The guest keeps
// all three shader handles in these globals, so a draw can be matched exactly.
constexpr uint32_t kBinkPixelShaderYuv = 0x82DD7130;
constexpr uint32_t kBinkPixelShaderYuvAlpha = 0x82DD7134;
constexpr uint32_t kBinkVertexShader = 0x82DD7138;

uint32_t ReadGuestGlobalPtr(uint8_t* base, uint32_t addr) {
  return HostPageReadable(REX_RAW_ADDR(addr)) ? REX_LOAD_U32(addr) : 0;
}

// Exact identity: the guest's own two Bink composite pixel shaders, read from
// its globals. Not a heuristic on texture count or draw shape.
bool IsBinkCompositeDraw(uint32_t pixel_shader, uint8_t* base) {
  if (!pixel_shader) return false;
  return pixel_shader == ReadGuestGlobalPtr(base, kBinkPixelShaderYuv) ||
         pixel_shader == ReadGuestGlobalPtr(base, kBinkPixelShaderYuvAlpha);
}

void ProbeBinkComposite(uint32_t pixel_shader, uint32_t vertex_shader,
                        uint32_t device, uint8_t* base, uint32_t vertex_count) {
  const uint32_t ps_yuv = ReadGuestGlobalPtr(base, kBinkPixelShaderYuv);
  const uint32_t ps_yuv_alpha =
      ReadGuestGlobalPtr(base, kBinkPixelShaderYuvAlpha);
  const uint32_t vs_bink = ReadGuestGlobalPtr(base, kBinkVertexShader);

  // Report the handles themselves whether or not a draw ever matches. All
  // three zero means the guest never created its Bink shaders — a different
  // problem from a plane format we cannot decode, and otherwise identical from
  // the outside, since both produce a probe that never fires.
  static bool s_reported_live = false;
  if (!s_reported_live && (ps_yuv || ps_yuv_alpha || vs_bink)) {
    s_reported_live = true;
    REXLOG_INFO("d3d9: Bink composite shaders created: ps_yuv=0x{:08X} "
                "ps_yuv_alpha=0x{:08X} vs=0x{:08X}",
                ps_yuv, ps_yuv_alpha, vs_bink);
  }
  if (!pixel_shader ||
      (pixel_shader != ps_yuv && pixel_shader != ps_yuv_alpha)) {
    return;
  }

  static std::map<uint32_t, uint64_t> s_hits;
  const uint64_t n = ++s_hits[pixel_shader];
  if (n != 1 && (n % 600) != 0) return;
  REXLOG_INFO("d3d9: Bink composite draw #{} ps=0x{:08X} ({}) vs=0x{:08X}{} "
              "verts={}",
              n, pixel_shader,
              pixel_shader == ps_yuv_alpha ? "YUV+alpha" : "YUV",
              vertex_shader,
              vertex_shader == vs_bink ? "" : " <-- not the Bink VS",
              vertex_count);
  // All four samplers, not just whichever one the binding selector would pick:
  // the whole point is that this draw needs several at once.
  for (uint32_t s = 0; s < 4 && s < mx::hle::kMaxSamplers; ++s) {
    uint32_t fetch[6] = {};
    if (!ReadLiveTextureFetch(device, base, s, fetch)) {
      REXLOG_INFO("d3d9:   Bink sampler {}: no live fetch", s);
      continue;
    }
    mx::hle::HleTextureSource src;
    const char* why = nullptr;
    if (mx::hle::DescribeHleTexture2D(fetch, src, &why)) {
      REXLOG_INFO("d3d9:   Bink sampler {}: format {} ({}) {}x{} tiled={} "
                  "pitch_blocks={} bpb={} decodes",
                  s, src.guest_format,
                  mx::hle::GuestTextureFormatName(src.guest_format), src.width,
                  src.height, src.tiled, src.pitch_blocks, src.bytes_per_block);
    } else {
      REXLOG_INFO("d3d9:   Bink sampler {}: REJECTED ({}); format {} ({}); "
                  "words {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}",
                  s, why ? why : "?", src.guest_format,
                  mx::hle::GuestTextureFormatName(src.guest_format), fetch[0],
                  fetch[1], fetch[2], fetch[3], fetch[4], fetch[5]);
    }
  }
}

// Decode the Bink composite's plane set into the DrawCall. Deliberately
// separate from PrepareDrawTexture rather than folded into it:
//
//  - it must bind *several* textures, which the single-winner binding contest
//    in ResolvePixelBindingForDraw cannot express;
//  - the planes are k_8, which the semantic gate correctly refuses as base
//    colour for a mask but wrongly for a luma plane. Here the guest's own
//    shader identity says what they are, so the gate is not consulted;
//  - it must not touch g_hleCpuTextures. The planes are new guest memory every
//    video frame, so caching them by payload key would grow the cache without
//    bound; at 30 fps that is ~90 dead entries a second.
bool PrepareBinkPlanes(mx::hle::DrawCall& dc, uint32_t device, uint8_t* base) {
  using namespace mx::hle;
  uint32_t decoded = 0;
  for (uint32_t s = 0; s < DrawCall::kMaxPlanes && s < kMaxSamplers; ++s) {
    uint32_t fetch[6] = {};
    if (!ReadLiveTextureFetch(device, base, s, fetch)) break;
    HleTextureSource source;
    const char* why = nullptr;
    if (!DescribeHleTexture2D(fetch, source, &why)) {
      NoteRejectedTextureFormat("bink", s, source, why, fetch);
      return false;
    }
    std::vector<uint8_t> guest;
    if (!CopyTexturePhysical(source, base, guest)) return false;
    auto payload = std::make_shared<HleTexturePayload>();
    if (!DecodeHleTexture2D(source, guest.data(), guest.size(), *payload, &why))
      return false;
    // An all-zero plane is normal for a video that has not decoded its first
    // frame yet, so unlike the immutable path this is not memoised as empty —
    // the same descriptor will carry real pixels a frame later.
    payload->key = HleTextureKey(fetch);
    // Crop the chroma planes to their logical extent, while the payload is
    // still local and mutable.
    //
    // The guest allocates them with the dimensions rounded up, so half of a
    // 216-row luma arrives as a 320x112 descriptor rather than 320x108. The
    // composite shader samples every plane with the same normalized uv and
    // leaves the half-size difference to the sampler, which is only correct
    // when chroma is *exactly* half: with four rows of padding, uv.y = 1.0
    // reads past the image into zeros, and zero chroma over white luma decodes
    // through BT.601 to (0.29, 1.0, 0.08) — the saturated green line seen
    // across the bottom edge of the video. Measured on the 640x216 overlay;
    // the luma plane itself has no padding (137888 of 138240 bytes nonzero,
    // under one row).
    //
    // Cropping rather than scaling uv in the shader keeps the sampler's
    // normalized mapping right by construction and costs no constant-buffer
    // plumbing. Only ever shrinks, so a plane already at or under the logical
    // size is left alone. Planes 1 and 2 are Cr and Cb; plane 0 is luma and
    // plane 3 the alpha, both full resolution.
    if ((s == 1 || s == 2) && dc.planes[0]) {
      const uint32_t chroma_w = (dc.planes[0]->width + 1) / 2;
      const uint32_t chroma_h = (dc.planes[0]->height + 1) / 2;
      if (chroma_w && payload->width > chroma_w) payload->width = chroma_w;
      if (chroma_h && payload->height > chroma_h) {
        payload->height = chroma_h;
        if (payload->row_pitch) {
          const size_t used = size_t(chroma_h) * payload->row_pitch;
          if (used < payload->data.size()) payload->data.resize(used);
        }
      }
    }
    dc.planes[decoded++] = std::move(payload);
  }
  // Y, Cr and Cb are always present; the fourth is the alpha plane and its
  // presence is what selects the guest's alpha-capable pixel shader.
  if (decoded < 3) return false;
  dc.plane_count = decoded;
  dc.yuv_has_alpha = decoded >= 4;
  dc.yuv_composite = true;
  // The composite samples the full frame, so its logical extent is the luma
  // plane's; the chroma planes are half-size and the shader normalises.
  dc.sampled_texture_width = dc.planes[0]->width;
  dc.sampled_texture_height = dc.planes[0]->height;
  static uint64_t s_ok = 0;
  if (++s_ok <= 4 || (s_ok % 600) == 0) {
    // Nonzero byte counts per plane. Green output from the YUV shader is what
    // all-zero planes produce, so "did the guest actually decode a frame" and
    // "did our upload work" have to be told apart here rather than guessed at
    // from the colour on screen.
    size_t nz[DrawCall::kMaxPlanes] = {};
    for (uint32_t i = 0; i < decoded; ++i)
      HleTextureHasNonzeroData(*dc.planes[i], &nz[i]);
    REXLOG_INFO("d3d9: Bink planes ready #{}: {} planes, luma {}x{}, alpha {};"
                " nonzero bytes Y={} Cr={} Cb={} A={}",
                s_ok, decoded, dc.planes[0]->width, dc.planes[0]->height,
                dc.yuv_has_alpha, nz[0], nz[1], nz[2], nz[3]);
  }
  return true;
}

bool ResolvePixelBindingForDraw(uint32_t handle, uint32_t device,
                                uint8_t* base,
                                mx::hle::PixelTextureBinding& out) {
  if (ResolvePixelBinding(handle, out)) return true;
  const ResolvedPixelBinding* profile = ResolvePixelProfile(handle);
  if (!profile || !profile->decoded || profile->bindings.empty()) return false;

  int best_score = -1;
  uint64_t best_texels = 0;
  mx::hle::HleTextureSource best_source;
  bool found = false;
  for (const auto& candidate : profile->bindings) {
    if (candidate.sampler >= mx::hle::kMaxSamplers) continue;
    uint32_t fetch[6];
    if (!ReadLiveTextureFetch(device, base, candidate.sampler, fetch)) continue;
    mx::hle::HleTextureSource source;
    const char* candidate_why = nullptr;
    if (!mx::hle::DescribeHleTexture2D(fetch, source, &candidate_why)) {
      // Selection used to discard this reason entirely, so a format rejected
      // while choosing a binding produced no log line at all — half the
      // rejections in any run were invisible.
      NoteRejectedTextureFormat("select", candidate.sampler, source,
                                candidate_why, fetch);
      continue;
    }

    // A D3D9 Resolve establishes an ordered host render-target dependency.
    // Its guest backing may legitimately be all zero in native mode because
    // the skipped Xenos dispatch never populated that memory, so this identity
    // is stronger evidence than the descriptor's storage format.
    const auto& texture_state = DeviceState().texture[candidate.sampler];
    const bool mapped_render_target =
        texture_state.object &&
        g_resolvedTextureTargets.contains(texture_state.object);
    const uint64_t candidate_key = mx::hle::HleTextureKey(fetch);
    if (!mapped_render_target && g_hleEmptyTextures.contains(candidate_key))
      continue;
    if (!mapped_render_target &&
        (source.host_format == mx::hle::HostTextureFormat::kBc5 ||
         source.host_format == mx::hle::HostTextureFormat::kR16Float ||
         source.host_format == mx::hle::HostTextureFormat::kRgba16Float ||
         source.host_format == mx::hle::HostTextureFormat::kR8 ||
         source.host_format == mx::hle::HostTextureFormat::kR16 ||
         source.host_format == mx::hle::HostTextureFormat::kR32Float))
      continue;
    // An 8x8 immutable texture is a lookup table, not a material. Both that
    // this front end owns are ordered-dither matrices the guest thresholds
    // against for stipple transparency (RenderDoc texture 1316, 8x8 BC1, and
    // the 8x8 k_4_4_4_4 at s12 of shader 0x216A8C20), and the generic host
    // pixel shader has no threshold step — it samples whatever wins and shows
    // it, which is how a Bayer checkerboard ended up painted across the main
    // menu. The tie-break below covers the case where a real texture is also
    // present; this covers the case where it is not, and falling through to
    // the colour-only pipeline is the honest answer. Cut measured, not
    // guessed: across a front-end run the smallest immutable winner other
    // than these two is 64x8.
    if (!mapped_render_target && uint64_t(source.width) * source.height <= 64)
      continue;
    // A mapped render target is authoritative storage, but it is not normally
    // the visible base colour of a material. Multi-input world shaders often
    // combine one or more scene/intermediate targets with immutable colour
    // atlases. Giving mapped targets absolute priority made those shaders
    // sample a black native-mode intermediate instead of their BC1 diffuse
    // texture. Prefer normalized immutable colour assets when both kinds are
    // present. The observed final compositor is handled explicitly above and
    // still selects its mapped s0 scene input.
    int score = mapped_render_target ? 40 :
                (candidate.unnormalized ? 0 : 200);
    switch (source.host_format) {
      case mx::hle::HostTextureFormat::kRgba8:
      case mx::hle::HostTextureFormat::kBc1:
      case mx::hle::HostTextureFormat::kBc2:
      case mx::hle::HostTextureFormat::kBc3:
      // k_4_4_4_4 is a four-channel colour format and, in three front-end
      // runs, the only format the front end asked for at all. It scores with
      // the other colour assets or it would never win a binding.
      case mx::hle::HostTextureFormat::kBgra4:
        score += mapped_render_target ? 40 : 200;
        break;
      case mx::hle::HostTextureFormat::kR8:
      case mx::hle::HostTextureFormat::kR16:
      case mx::hle::HostTextureFormat::kR32Float:
        // Single-channel; decodable, but not base colour. Same rationale as
        // the semantic gate in PrepareDrawTexture.
        score += mapped_render_target ? 10 : 0;
        break;
      case mx::hle::HostTextureFormat::kR16Float:
      case mx::hle::HostTextureFormat::kRgba16Float:
        // Float descriptors observed in ST_Southwest are render/resolve
        // intermediates. Only the mapped host-target path above may select
        // them; immutable guest copies are black while GPU dispatch is skipped.
        score += mapped_render_target ? 30 : 0;
        break;
      case mx::hle::HostTextureFormat::kBc5:
        // DXN/BC5 is a normal map. Keep support for inspection and future
        // shader translation, but never prefer it as visible base colour.
        score += mapped_render_target ? 10 : 0;
        break;
    }
    // Ties were previously broken by fetch program order, which is not
    // evidence of anything, and it lost a 2048x2048 colour atlas to an 8x8
    // ordered-dither matrix that happened to be fetched first (shader
    // 0x216A8C20: s12 8x8 k_4_4_4_4 and s11 2048x2048 RGBA8, both scoring
    // 400). Between two candidates the descriptor cannot otherwise separate,
    // the larger one is the material and the smaller one is a lookup table.
    const uint64_t texels = uint64_t(source.width) * source.height;
    if (score < best_score || (score == best_score && texels <= best_texels))
      continue;
    best_score = score;
    best_texels = texels;
    out = candidate;
    best_source = source;
    found = true;
  }
  if (!found) return false;
  static std::map<uint32_t, bool> s_logged;
  if (s_logged.size() < 32 && s_logged.emplace(handle, true).second) {
    const auto& selected_state = DeviceState().texture[out.sampler];
    const bool mapped = selected_state.object &&
                        g_resolvedTextureTargets.contains(selected_state.object);
    REXLOG_INFO("d3d9: selected s{} r{} {}x{} format {} from {}-fetch "
                "pixel shader 0x{:08X} (descriptor score {}, mapped {})",
                out.sampler, out.src_reg, best_source.width,
                best_source.height, uint32_t(best_source.host_format),
                profile->bindings.size(), handle, best_score, mapped);
  }
  return true;
}

void ProbePixelProfileForDraw(uint32_t pixel_shader, uint32_t device,
                              uint8_t* base,
                              const mx::hle::DrawCall& dc) {
  if (!pixel_shader && device &&
      HostPageReadable(REX_RAW_ADDR(device + 0x3244))) {
    pixel_shader = REX_LOAD_U32(device + 0x3244);
  }
  if (!pixel_shader) return;
  CollectPixelShaderBlob(pixel_shader, base);
  const ResolvedPixelBinding* profile = ResolvePixelProfile(pixel_shader);
  if (!profile) return;

  // One line per shader/target pairing is enough to identify the profile used
  // by the present-sized pass without flooding a frame with repeated draws.
  const uint64_t key = (uint64_t(pixel_shader) << 32) |
                       uint64_t(dc.render_target_object);
  static std::map<uint64_t, bool> s_seen;
  if (s_seen.size() >= 64 || !s_seen.emplace(key, true).second) return;
  REXLOG_INFO("d3d9: pixel profile draw ps=0x{:08X}, target=0x{:08X} "
              "{}x{}, viewport={}x{}, fetches={}{}",
              pixel_shader, dc.render_target_object, dc.render_target_width,
              dc.render_target_height, dc.viewport_width, dc.viewport_height,
              profile->bindings.size(),
              profile->decoded ? "" : " (unsupported)");
  if (profile->decoded) {
    std::string inputs;
    for (const auto& binding : profile->bindings) {
      if (binding.sampler >= mx::hle::kMaxSamplers) continue;
      const auto& tb = DeviceState().texture[binding.sampler];
      mx::hle::HleTextureSource source;
      const char* why = nullptr;
      const bool described = tb.valid &&
          mx::hle::DescribeHleTexture2D(tb.fetch, source, &why);
      uint32_t resolved = 0;
      if (const auto it = g_resolvedTextureTargets.find(tb.object);
          it != g_resolvedTextureTargets.end())
        resolved = it->second;
      inputs += fmt::format(" s{}=tex0x{:08X}", binding.sampler, tb.object);
      if (described)
        inputs += fmt::format("({}x{})", source.width, source.height);
      if (resolved) inputs += fmt::format("->rt0x{:08X}", resolved);
    }
    REXLOG_INFO("d3d9: pixel profile inputs ps=0x{:08X}:{}", pixel_shader,
                inputs.empty() ? " none" : inputs);
  }
}

// IDA proves D3DDevice_SetPixelShader stores the live shader at device+0x3244
// (the adjacent vertex shader is device+0x3248). State-block application may
// bypass our public setter hook, so read the authoritative D3D9 device field at
// draw time and still require exact microcode agreement with a captured PM4 PS.
bool ReadBoundPixelShader(uint32_t device, uint8_t* base, uint32_t& handle,
                          mx::hle::PixelTextureBinding& binding) {
  constexpr uint32_t kDevicePixelShaderOffset = 0x3244;
  if (!device ||
      !HostPageReadable(REX_RAW_ADDR(device + kDevicePixelShaderOffset)))
    return false;
  const uint32_t candidate =
      REX_LOAD_U32(device + kDevicePixelShaderOffset);
  if (!candidate) return false;
  CollectPixelShaderBlob(candidate, base);
  if (!ResolvePixelBindingForDraw(candidate, device, base, binding))
    return false;
  handle = candidate;
  static uint32_t s_logged = 0;
  if (s_logged++ < 8) {
    REXLOG_INFO("d3d9: active pixel shader 0x{:08X} read from "
                "device+0x3244 (sampler {}, UV r{})",
                handle, binding.sampler, binding.src_reg);
  }
  return true;
}

bool CopyTexturePhysical(const mx::hle::HleTextureSource& source, uint8_t* base,
                         std::vector<uint8_t>& out) {
  const uint32_t candidates[] = {
      source.address, source.address | 0xA0000000u,
      source.address | 0xC0000000u, source.address | 0xE0000000u};
  for (uint32_t candidate : candidates) {
    bool readable = true;
    for (uint64_t o = 0; o < source.source_bytes; o += kHostPageSize) {
      if (!HostPageReadable(REX_RAW_ADDR(candidate + uint32_t(o)))) {
        readable = false;
        break;
      }
    }
    if (!readable || !HostPageReadable(
                         REX_RAW_ADDR(candidate + source.source_bytes - 1)))
      continue;
    out.resize(source.source_bytes);
    std::memcpy(out.data(), REX_RAW_ADDR(candidate), source.source_bytes);
    return true;
  }
  return false;
}

// Resolve the texture a translated shader reads at one compact sampler slot.
//
// Deliberately NOT PrepareDrawTexture's logic. That function applies a policy
// gate — kR8, kR16, kBc5 and the float formats are decoded and then refused as
// "not an immutable colour asset" — which is correct for the stand-in shader,
// where a single-channel texture bound as base colour would paint the surface
// grey. It is wrong here. The guest's own shader knows that channel is a
// coverage mask or a normal map and says so in its arithmetic; refusing to bind
// it leaves the shader sampling nothing.
//
// FMT_8 is the format fonts use, which is why glyph quads came out as filled
// blocks: the texture decoded fine and was then withheld from the shader that
// knew what to do with it.
//
// Fills exactly one of the two per-slot outputs: a resolved render target if
// the guest bound one there, otherwise a decoded CPU payload. Returns false
// when neither could be produced, and the caller decides whether that is fatal.
// Why a sampler slot could not be filled. A slot that fails sends the WHOLE
// draw back to the tex*col stand-in, so these are the draws the guest's own
// pixel shader was translated for and then not used on -- 26,844 of them in
// mx_705, which is essentially every stand-in draw in that run. Six of the
// seven exits below were previously silent, and the one that logged fired once.
uint64_t g_slotFailRange = 0, g_slotFailFetch = 0, g_slotFailDescribe = 0;
uint64_t g_slotFailCopy = 0, g_slotFailDecode = 0;
uint64_t g_slotBoundZero = 0;   // all-zero, and bound anyway -- see below
uint64_t g_slotBoundUnbound = 0;  // sampler the guest never bound; sampled zero
// Which guest sampler had no readable fetch constant. The open question this
// answers is whether the shaders' samplers 8-15 index the bank the same way
// 0-7 do: a failure spread evenly over low samplers means genuinely unbound
// slots, whereas one concentrated at and above 8 means the indexing is wrong.
std::map<uint32_t, uint64_t> g_slotFailFetchBySampler;

void ReportSlotFailures() {
  const uint64_t total = g_slotFailRange + g_slotFailFetch +
                         g_slotFailDescribe + g_slotFailCopy + g_slotFailDecode;
  if (!total || (total % 5000) != 0) return;
  std::string by;
  for (const auto& [sampler, n] : g_slotFailFetchBySampler)
    by += fmt::format(" s{}={}", sampler, n);
  REXLOG_INFO("d3d9: slot fill outcomes {}: range {} describe {} copy {} "
              "decode {} (these still fail the draw); unbound by sampler:{}",
              total, g_slotFailRange, g_slotFailDescribe, g_slotFailCopy,
              g_slotFailDecode, by.empty() ? " none" : by);
}

// A texture that DECODED but is entirely zero, and a sampler the guest never
// bound, are both bound rather than refused -- deliberately, see the notes in
// ResolvePixelSlotTexture. Their counters used to be printed only inside
// ReportSlotFailures, which returns early when the hard-failure total is zero.
// In mx_736 nothing failed, so neither number was ever printed once: draws
// painting solid black were invisible in the log. They get their own line,
// fired off their own total.
uint64_t g_boundZeroReported = 0;

// One entry per distinct all-zero texture, so the offenders can be named rather
// than counted. The set is small -- nine keys covered 10,890 draws in mx_706 --
// so it is printed in full. `recovered` is the question the log has to answer:
// a key that later decodes non-blank was a texture sampled before the guest
// finished streaming it, which is a caching defect; one that never recovers is
// a genuinely blank guest texture and a different investigation.
// Distinguishes the host upload of a blank decode from the host upload of the
// same texture once it has real contents. Both would otherwise share the
// fetch-word key that EnsureGameTexture caches on, and the recovered texture
// would hit the black resource uploaded before it.
constexpr uint64_t kBlankTextureKeyMarker = 0x8000000000000000ull;

struct BlankTexture {
  uint32_t sampler = 0;
  uint32_t address = 0;
  uint32_t width = 0, height = 0;
  uint32_t guest_format = 0;
  uint32_t swizzle = 0;
  uint64_t first_frame = 0;
  uint64_t draws = 0;
  bool recovered = false;
};
std::map<uint64_t, BlankTexture> g_blankTextures;

void NoteBlankTexture(uint64_t key, uint32_t sampler,
                      const mx::hle::HleTextureSource& source) {
  auto [it, inserted] = g_blankTextures.emplace(key, BlankTexture{});
  BlankTexture& b = it->second;
  ++b.draws;
  if (!inserted) return;
  b.sampler = sampler;
  b.address = source.address;
  b.width = source.width;
  b.height = source.height;
  b.guest_format = source.guest_format;
  b.swizzle = source.swizzle;
  b.first_frame = mx::hle::D3D9FrameCount();
  REXLOG_INFO("d3d9: bound-zero texture {:#x}: sampler {} addr {:#x} {}x{} "
              "guest format {} swizzle {:#o} first seen frame {}",
              key, sampler, source.address, source.width, source.height,
              source.guest_format, source.swizzle, b.first_frame);
}

// Called when a key that was previously all-zero decodes to real data. This is
// the line that convicts or clears the cache: it can only ever print once the
// blank decode stops being cached forever.
void NoteBlankRecovered(uint64_t key) {
  // Both judgements were made when the texture was blank and are now wrong, so
  // they are withdrawn rather than left standing: the stand-in path refuses any
  // key in the empty set as a poor representative of the draw, and the blank
  // payload is what the within-frame retry would hand back.
  g_hleEmptyTextures.erase(key);
  g_hleBlankPayloads.erase(key);
  auto it = g_blankTextures.find(key);
  if (it == g_blankTextures.end() || it->second.recovered) return;
  it->second.recovered = true;
  REXLOG_INFO("d3d9: bound-zero texture {:#x} RECOVERED on frame {} (was blank "
              "from frame {}, {} draws sampled black)",
              key, mx::hle::D3D9FrameCount(), it->second.first_frame,
              it->second.draws);
}

void ReportBoundZero() {
  const uint64_t total = g_slotBoundZero + g_slotBoundUnbound;
  if (!total || total == g_boundZeroReported ||
      (total % 2500) != 0)
    return;
  g_boundZeroReported = total;
  size_t outstanding = 0;
  for (const auto& [key, b] : g_blankTextures)
    if (!b.recovered) ++outstanding;
  std::string by;
  for (const auto& [sampler, n] : g_slotFailFetchBySampler)
    by += fmt::format(" s{}={}", sampler, n);
  REXLOG_INFO("d3d9: draws sampling BLACK {}: all-zero texture {}, "
              "unbound sampler {}; {} distinct blank textures, {} still blank; "
              "unbound by sampler:{}; resolve address matches {} (extent "
              "mismatches {}), resolves dropped for no source {}; "
              "shader-published fetch: {} shaders, {} slots served",
              total, g_slotBoundZero, g_slotBoundUnbound,
              g_blankTextures.size(), outstanding,
              by.empty() ? " none" : by, g_resolveAddressMatches,
              g_resolveAddressExtentMiss, g_resolveDroppedNoSource,
              g_shaderFetchPublished.load(std::memory_order_relaxed),
              g_shaderFetchServed.load(std::memory_order_relaxed));
}

// Every distinct (guest format, swizzle) pair that reaches a binding, printed
// once. Two open questions read straight off it: whether any swizzle component
// is 6 or 7 -- values D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING does not define,
// so the channel would be undefined rather than wrong -- and whether the
// k_8_8_8_8 -> R8G8B8A8 mapping is relying on a swizzle that actually performs
// the BGRA rotation.
// Also censused here: TEX_FORMAT_COMP / GPUSIGN, which the describe step reads
// but nothing acts on. Three of its four values change what a fetch returns --
// kSigned is two's complement (an SNORM host format), kUnsignedBiased is
// 2*c-1, and kGamma is sRGB linearized on sample -- so any of them appearing
// means we hand the shader the wrong numbers, quietly. This says whether the
// game uses them at all before any of it is built.
void NoteSwizzleCensus(const mx::hle::HleTextureSource& source) {
  static std::set<uint64_t> s_seen;
  const uint64_t pair = (uint64_t(source.guest_format) << 32) |
                        (uint64_t(source.signs) << 16) | source.swizzle;
  if (!s_seen.insert(pair).second) return;
  const uint32_t c[4] = {(source.swizzle >> 0) & 7u, (source.swizzle >> 3) & 7u,
                         (source.swizzle >> 6) & 7u, (source.swizzle >> 9) & 7u};
  const bool undefined = c[0] > 5 || c[1] > 5 || c[2] > 5 || c[3] > 5;
  static constexpr const char* kSignName[4] = {"unsigned", "signed", "biased",
                                               "gamma"};
  const uint32_t s[4] = {(source.signs >> 0) & 3u, (source.signs >> 2) & 3u,
                         (source.signs >> 4) & 3u, (source.signs >> 6) & 3u};
  REXLOG_INFO("d3d9: swizzle census: guest format {} swizzle {:#o} "
              "components [{} {} {} {}]{} signs [{} {} {} {}]{}",
              source.guest_format, source.swizzle, c[0], c[1], c[2], c[3],
              undefined ? "  <-- UNDEFINED in D3D12 (>5)" : "", kSignName[s[0]],
              kSignName[s[1]], kSignName[s[2]], kSignName[s[3]],
              source.signs ? "  <-- NOT PLAIN UNSIGNED, we ignore this" : "");
}

// How many slot binds actually carry a sign mode we do not honour, split by
// guest format. The census above says which formats do it; this says whether it
// is one decorative texture or the whole scene, which is the difference between
// closing the question and building a signed decode path.
//
// Float formats are excluded deliberately. A TextureSign on k_*_FLOAT is a
// no-op -- the data is already signed float, and the reference cache only needs
// a separate host texture when a FIXED-POINT format has no signed host
// equivalent (cache.h:488, IsSignedVersionSeparateForFormat). Counting them
// would inflate the number with binds that need nothing done.
bool IsFloatGuestFormat(uint32_t guest_format) {
  switch (xn::TextureFormat(guest_format)) {
    case xn::TextureFormat::k_16_FLOAT:
    case xn::TextureFormat::k_16_16_FLOAT:
    case xn::TextureFormat::k_16_16_16_16_FLOAT:
    case xn::TextureFormat::k_32_FLOAT:
    case xn::TextureFormat::k_32_32_FLOAT:
    case xn::TextureFormat::k_32_32_32_32_FLOAT:
      return true;
    default:
      return false;
  }
}

void NoteSignedBind(const mx::hle::HleTextureSource& source) {
  if (!source.signs || IsFloatGuestFormat(source.guest_format)) return;
  static std::map<uint32_t, uint64_t> s_binds;
  const uint64_t n = ++s_binds[(source.guest_format << 8) | source.signs];
  if ((n % 5000) != 0) return;
  std::string by;
  for (const auto& [k, v] : s_binds)
    by += fmt::format(" fmt{}/signs{:#04x}={}", k >> 8, k & 0xFF, v);
  REXLOG_INFO("d3d9: binds of a non-float texture with an unhonoured sign mode:{}",
              by);
}

// Blast radius of the packed mip tail. A texture whose base is packed used to
// be read from the origin of the tail rather than from its own offset within
// it, so every one of these was returning another texture's bytes. Counted by
// extent and format so the population is visible rather than inferred from the
// one 8x8 DXT1 lookup that made it findable -- that one multiplies the menu's
// deferred lighting, which is why the whole scene came out black.
void NotePackedBase(const mx::hle::HleTextureSource& source) {
  if (!source.packed_offset_x_blocks && !source.packed_offset_y_blocks) return;
  static std::map<uint64_t, uint64_t> s_seen;
  const uint64_t k = (uint64_t(source.guest_format) << 32) |
                     (uint64_t(source.width) << 16) | source.height;
  const uint64_t n = ++s_seen[k];
  if (n != 1 && (n % 20000) != 0) return;
  std::string by;
  for (const auto& [key, v] : s_seen)
    by += fmt::format(" {}x{}/fmt{}={}", (key >> 16) & 0xFFFF, key & 0xFFFF,
                      uint32_t(key >> 32), v);
  REXLOG_INFO("d3d9: textures read from a PACKED MIP TAIL, {} distinct:{}",
              s_seen.size(), by);
}

// A sign mode that reaches a bind and is NOT applied. kSigned needs the
// texture's bits reinterpreted into a signed host format (a decode change, task
// #43); kGamma needs an sRGB curve. Neither is approximated here -- an
// unimplemented mode that silently behaves as unsigned is at least visible in
// this line.
void NoteUnhandledSign(uint32_t guest_format, uint32_t mode) {
  static std::map<uint32_t, uint64_t> s_seen;
  const uint64_t n = ++s_seen[(guest_format << 4) | mode];
  if (n != 1 && (n % 20000) != 0) return;
  static constexpr const char* kName[4] = {"unsigned", "signed", "biased",
                                           "gamma"};
  REXLOG_INFO("d3d9: texture sign mode NOT applied: guest format {} mode {} x{}",
              guest_format, kName[mode & 3], n);
}

// `vertex` selects which stage's slot arrays the result lands in. Everything
// else -- the fetch-constant read, the resolve-snapshot match, the blank-decode
// retry, every counter -- is identical for the two stages, because the question
// "what texture is bound to guest sampler N" does not depend on who is asking.
// Splitting this into two functions would have meant two copies of 250 lines
// that must agree.
//
// dc.pixel_shader_handle is still read for the blank-texture key regardless of
// stage: that key identifies the DRAW's material, and a vertex fetch of the
// same guest memory wants the same memoisation.
bool ResolvePixelSlotTexture(mx::hle::DrawCall& dc, uint32_t slot,
                             uint32_t guest_sampler, uint32_t device,
                             uint8_t* base, bool vertex = false) {
  using namespace mx::hle;
  auto& out_textures = vertex ? dc.vertex_textures : dc.pixel_textures;
  auto& out_objects =
      vertex ? dc.vertex_sampled_objects : dc.pixel_sampled_objects;
  auto& out_signs =
      vertex ? dc.vertex_sampler_signs : dc.pixel_sampler_signs;
  if (slot >= DrawCall::kMaxPixelTextures || guest_sampler >= kMaxSamplers) {
    ++g_slotFailRange;
    ReportSlotFailures();
    return false;
  }

  // A slot the guest points at a resolve result: the renderer samples the live
  // host target, exactly as the single-texture path already does.
  // A destination the GPU wrote whole is a render target: its guest memory is
  // meaningless and the snapshot is the only truthful answer, so take it before
  // spending a decode. 25 of the 26 destinations in mx_780 are this case.
  //
  // A destination the GPU wrote only part of is the interesting one, and it is
  // NOT a choice between a good answer and a bad one -- both sources are
  // partial. The 2048x2048 menu atlas has three 256x256 tiles of real rendered
  // content along its top edge and nothing else, while guest memory for it
  // decodes to zeros. Refusing the snapshot outright, as the first version of
  // this rule did, threw those three tiles away 2272 times and bound zeros
  // instead. So: try memory first, and fall back to the snapshot when memory
  // has nothing -- which also keeps the blank-retry path alive, so the day the
  // guest fills that memory it wins on its own.
  uint32_t partial_snapshot_object = 0;
  const auto& texture_state = DeviceState().texture[guest_sampler];
  if (texture_state.object &&
      g_resolvedTextureTargets.contains(texture_state.object)) {
    if (ResolvedDestinationIsMostlyWritten(texture_state.object)) {
      // Logged HERE as well as at the decode below, because this path RETURNS.
      // The first cut of the SLOT MAP diagnostic sat only after this point and
      // so reported resolved=0 on every line it printed -- blind to precisely
      // the slots that bind a snapshot, which are the ones worth seeing. A slot
      // simply went missing from the table instead, which reads like it was
      // never bound. See the note at the other call for why this matters.
      static std::mutex s_mu;
      static std::set<uint64_t> s_seen;
      const uint64_t key = (uint64_t(dc.pixel_shader_handle) << 8) | slot;
      bool fresh = false;
      {
        std::lock_guard<std::mutex> lk(s_mu);
        // Bounded by the dedupe -- one line per distinct (shader, slot), which
        // is tens of shaders times a handful of slots. A tighter cap filled up
        // on early menu shaders and cut off before the material under
        // investigation ever bound.
        fresh = s_seen.size() < 1024 && s_seen.insert(key).second;
      }
      if (fresh) {
        REXLOG_INFO(
            "d3d9: SLOT MAP ps 0x{:08X} slot {} (guest sampler {}): object "
            "0x{:08X} -> SNAPSHOT of a resolve destination (no guest-memory "
            "decode)",
            dc.pixel_shader_handle, slot, guest_sampler, texture_state.object);
      }
      out_objects[slot] = texture_state.object;
      return true;
    }
    partial_snapshot_object = texture_state.object;
  }

  uint32_t fetch[6] = {};
  // dc.pixel_shader_handle is the handle AttachTranslatedPixelShader resolved
  // for this DEVICE, not the thread-local one -- see the resolution above it --
  // which is the handle whose load table may carry this sampler's descriptor.
  if (!ReadLiveTextureFetch(device, base, guest_sampler, fetch,
                            dc.pixel_shader_handle)) {
    ++g_slotFailFetch;
    ++g_slotFailFetchBySampler[guest_sampler];
    ReportSlotFailures();
    // The guest never bound anything to this sampler, and the shader reads it
    // anyway. Measured over mx_710 the failures fall s5=2420 s6=304 s7=2420
    // s8=2429 s9=2420 s13=4, with samplers 0-4 never failing once -- one shader
    // family reading four slots this title does not bind. It is NOT the
    // thread-local device state losing a binding, which would fail every
    // sampler on the affected thread together rather than the same four.
    //
    // Zero is what the hardware returns for a fetch constant whose type is not
    // kTexture, so an unbound slot samples zero. Refusing instead sent the
    // whole draw to the tex*col stand-in, discarding every OTHER slot's real
    // shading over a slot whose value the shader may not even use -- the same
    // trade already settled for all-zero textures, which cost 10,890 draws.
    //
    // This is a bound zero, not a fabricated colour: nothing here invents a
    // plausible texture, it supplies the value an unbound fetch actually has.
    static const auto s_unbound = [] {
      auto p = std::make_shared<mx::hle::HleTexturePayload>();
      p->width = p->height = 1;
      p->row_pitch = 4;
      p->format = mx::hle::HostTextureFormat::kRgba8;
      p->linear_filter = false;
      p->data.assign(4, 0);
      return p;
    }();
    out_textures[slot] = s_unbound;
    ++g_slotBoundUnbound;
    ReportBoundZero();
    return true;
  }
  HleTextureSource source;
  const char* why = nullptr;
  if (!DescribeHleTexture2D(fetch, source, &why)) {
    NoteRejectedTextureFormat("slot", guest_sampler, source, why, fetch);
    ++g_slotFailDescribe;
    ReportSlotFailures();
    return false;
  }
  NoteSignedBind(source);
  NotePackedBase(source);
  // WHICH guest surface does each sampler slot actually ask for?
  //
  // Traced from the rider's gear rendering green. Its material computes
  // saturate(tex5.y + rcp(luminance(tex4))) and the saturate pins at 1, which
  // zeroes the red channel. Red survives only if that luminance exceeds ~1.03.
  //
  // tex4 resolves to the pre-pass band snapshot, whose content is written by
  // the full-screen ambient lighting draw. That pass sums six directional
  // lights whose colours are c149/151/153/155/157/159 -- measured, sane, and
  // identical across captures -- and their red channels total 0.619. That is a
  // hard ceiling with every dot product at 1.0 simultaneously, which opposing
  // directions make impossible; the measured value is 0.109.
  //
  // So with that surface as tex4 the red channel can NEVER survive, on any
  // hardware, with correct constants. The arithmetic does not merely say the
  // input is dark -- it says it is the WRONG SURFACE. The gained main-pass
  // scene holds 32.6 at the same pixel, and feeding that in yields
  // saturate(0.033 + 0.029) = 0.062, a red multiplier of 0.938: yellow gear.
  //
  // Binding is by guest OBJECT (DeviceState().texture[sampler].object looked up
  // in g_resolvedTextureTargets), so we follow whatever the guest bound. This
  // says what that is: the object, whether a resolve ever named it, and the
  // fetch constant's own address and extent -- enough to tell "the guest asked
  // for the pre-pass" from "the guest asked for the scene and we handed it the
  // pre-pass".
  //
  // Deduplicated per (shader, slot) and capped: one line per distinct binding,
  // not per draw.
  {
    static std::mutex s_mu;
    static std::set<uint64_t> s_seen;
    static uint32_t s_lines = 0;
    const uint64_t key = (uint64_t(dc.pixel_shader_handle) << 8) | slot;
    bool fresh = false;
    {
      std::lock_guard<std::mutex> lk(s_mu);
      fresh = s_lines < 1024 && s_seen.insert(key).second;
      if (fresh) ++s_lines;
    }
    if (fresh) {
      REXLOG_INFO(
          "d3d9: SLOT MAP ps 0x{:08X} slot {} (guest sampler {}): object "
          "0x{:08X} resolved={} mostly_written={} | fetch addr 0x{:08X} "
          "{}x{} fmt {} bytes {}",
          dc.pixel_shader_handle, slot, guest_sampler, texture_state.object,
          texture_state.object &&
                  g_resolvedTextureTargets.contains(texture_state.object)
              ? 1
              : 0,
          partial_snapshot_object ? 0 : 1, source.address, source.width,
          source.height, source.guest_format, source.source_bytes);
    }
  }
  // Permuted into host component order here, at the bind, because this is
  // per-binding state: the same guest memory is sampled with different sign
  // modes by different draws. Applied by the shader after the fetch, which is
  // where it has to happen -- see the note in EmitTextureFetch.
  //
  // Only kUnsignedBiased rides this. kSigned would need the texture's bits
  // reinterpreted into a signed host format, and kGamma is a curve rather than
  // a scale; both are counted by NoteUnhandledSign and left alone rather than
  // approximated, since the census over a full menu run finds no kGamma at all
  // and kSigned on one FMT_4_4_4_4 texture.
  {
    const uint8_t swizzled =
        mx::hle::SwizzleTextureSigns(source.signs, source.swizzle);
    uint8_t biased = 0;
    for (uint32_t c = 0; c < 4; ++c) {
      const uint32_t mode = (swizzled >> (c * 2)) & 3u;
      if (mode == uint32_t(xn::TextureSign::kUnsignedBiased))
        biased |= uint8_t(1u << c);
      else if (mode != uint32_t(xn::TextureSign::kUnsigned) &&
               !IsFloatGuestFormat(source.guest_format))
        NoteUnhandledSign(source.guest_format, mode);
    }
    out_signs[slot] = biased;
  }

  // The same memory a resolve wrote into, reached through a different texture
  // object than the one the resolve named -- so the object test above missed
  // it. Sample the snapshot rather than decoding guest memory the GPU wrote and
  // the emulator never populated, which reads as zeros and paints black.
  //
  // Placed after the describe because the address and extent it matches on come
  // out of it, and before the decode because the decode is precisely what has
  // to be skipped.
  if (const ResolvedTargetByAddress* resolved =
          ResolvedTargetForAddress(source)) {
    out_objects[slot] = resolved->dest_object;
    ++g_resolveAddressMatches;
    return true;
  }

  // NOTE the empty-texture set is deliberately NOT consulted here.
  //
  // It is consulted by the single-texture path, which CHOOSES one sampler to
  // represent the draw -- there, skipping a blank candidate is right, because
  // a blank one is a poor representative of a shader that reads several.
  //
  // This path does not choose. The translated shader NAMES this sampler, and
  // the guest bound a texture to it that decodes, from readable memory, to
  // zeros. Then zero is the value the guest's own shader samples, and black is
  // the correct answer. Refusing it reverted the whole draw to the tex*col
  // stand-in -- discarding every other slot's real shading to avoid a black
  // sample that was never wrong. That cost 10,890 draws in mx_706, from just
  // nine distinct all-zero textures, because the refusal is cached per key.
  const uint64_t key = HleTextureKey(fetch);
  if (auto cached = g_hleCpuTextures.find(key);
      cached != g_hleCpuTextures.end()) {
    // A glyph atlas the guest has repacked since this was decoded falls
    // through to a fresh decode; everything else is served from the cache.
    if (!GlyphCacheStale(*cached->second)) {
      out_textures[slot] = cached->second;
      return true;
    }
    g_hleCpuTextures.erase(cached);
  }
  // A key found blank recently and not yet due another look: bind the decode
  // the earlier draw made rather than repeating it.
  if (!BlankRetryDue(key)) {
    if (partial_snapshot_object && g_hleBlankPayloads.contains(key)) {
      // Known blank and not due a re-read: the snapshot's tiles are the best
      // available answer until the retry says otherwise.
      out_objects[slot] = partial_snapshot_object;
      return true;
    }
    if (auto payload = g_hleBlankPayloads.find(key);
        payload != g_hleBlankPayloads.end()) {
      out_textures[slot] = payload->second;
      ++g_slotBoundZero;
      NoteBlankTexture(key, guest_sampler, source);
      ReportBoundZero();
      return true;
    }
  }
  std::vector<uint8_t> guest;
  if (!CopyTexturePhysical(source, base, guest)) {
    ++g_slotFailCopy;
    ReportSlotFailures();
    return false;
  }
  auto payload = std::make_shared<HleTexturePayload>();
  if (!DecodeHleTexture2D(source, guest.data(), guest.size(), *payload, &why)) {
    ++g_slotFailDecode;
    ReportSlotFailures();
    return false;
  }
  NoteSwizzleCensus(source);
  // Still recorded, so the single-texture path above keeps skipping it as a
  // representative and the count stays visible -- but no longer a refusal here.
  if (!HleTextureHasNonzeroData(*payload)) {
    NoteBlankDecode(key);
    // Memory had nothing and a partly-written snapshot exists: its resolved
    // tiles beat zeros everywhere, and the blank is still recorded above so the
    // retry keeps looking.
    if (partial_snapshot_object) {
      out_objects[slot] = partial_snapshot_object;
      return true;
    }
    ++g_slotBoundZero;
    NoteBlankTexture(key, guest_sampler, source);
    ReportBoundZero();
    // Bind the zero for THIS draw -- that trade is settled -- but do not cache
    // it. The cache key is FNV-1a over the six fetch dwords alone, so it hashes
    // where the texture lives and what shape it is, never what it contains;
    // inserting a blank decode under that key froze the texture black for the
    // rest of the run, because nothing in the tree ever erases or versions an
    // entry. A texture sampled once while the guest was still streaming into it
    // could therefore never appear. Leaving it out means the next draw re-reads
    // guest memory and the texture shows up the frame its upload completes.
    //
    // The host cache in EnsureGameTexture keys on payload->key too, so the
    // blank upload must not sit on the key the recovered texture will use. It
    // is given a marked key of its own; the real decode arrives under the
    // unmarked key and uploads as a new resource rather than hitting the black
    // one.
    payload->key = key ^ kBlankTextureKeyMarker;
    g_hleBlankPayloads[key] = payload;
    out_textures[slot] = std::move(payload);
    return true;
  }
  NoteBlankRecovered(key);
  payload->key = key;
  payload->content_version = g_glyphCacheGeneration;
  out_textures[slot] = payload;
  g_hleCpuTextures.emplace(key, std::move(payload));
  return true;
}

// Overlay a pixel shader's EMBEDDED constants onto the bank read from the
// device shadow.
//
// The shadow at device+0x1780 is not the whole story, and reading it alone is
// why every 3D material in this title rendered black. A shader object carries
// its own constant-load table, and the draw-time flush publishes it straight to
// the GPU by address, never through the shadow:
//
//   sub_82565928, the draw flush, does
//       v8 = *(device + 12868);              // 0x3244, the PIXEL shader
//       sub_825656A0(device, v8 + 10, v8[6]) // table at ps+0x28, data *(ps+0x18)
//       v6 = *(device + 12872);              // 0x3248, the VERTEX shader
//       sub_825656A0(device, v6 + 218, v6[8])// table at vs+0x368, data *(vs+0x20)
//
//   and sub_825656A0 walks, from `rel = *(u32*)(table + 0x14)`, a block at
//   `table + rel` whose entry list starts at +0x14 and runs *(u32*)(+0x10)
//   bytes:
//       entry: u16 reg_index, u16 dword_count, u32 data_offset   (8 bytes)
//       terminated by dword_count == 0
//       source  = data_offset + data_base
//       emitted as PM4 0xC0022F00 (LOAD_ALU_CONSTANT) with body
//       [physical(source), 4 * reg_index, dword_count]
//
// The vertex half of this was already understood -- see
// ProbeVertexShaderConstantPatch, which recorded that the data "never passes
// through device + 0x780". What was missed is that the PIXEL shader has the
// same mechanism at DIFFERENT offsets, so the pixel bank was left with only the
// registers SetPixelShaderConstantF happens to write. Measured over 264 dumped
// shaders: 242 read constants above c217, which the shadow never contains, and
// the 22 that read only written registers are exactly the UI shaders that
// always looked correct.
//
// `reg` is an ALU float4 constant index. The emitted packet uses `4 * reg` as
// its dword offset from register 0x4000, so pixel constants 256..511 map to this
// bank at reg-256. Texture fetch constants do NOT live in this list; they are
// in the second, inline state-patch list walked by ApplyShaderFetchPatchTable.
void ApplyShaderLoadTable(uint32_t shader, uint32_t table_at, uint32_t data_at,
                          uint8_t* base, std::vector<uint32_t>& bank) {
  if (!shader || bank.size() < 256 * 4) return;
  const uint32_t kPsLoadTableAt = table_at;
  const uint32_t kPsDataBaseAt = data_at;
  const uint32_t table = shader + kPsLoadTableAt;
  if (!HostPageReadable(REX_RAW_ADDR(table + 0x14)) ||
      !HostPageReadable(REX_RAW_ADDR(shader + kPsDataBaseAt)))
    return;
  const uint32_t rel = REX_LOAD_U32(table + 0x14);
  if (!rel || rel >= 0x10000) return;
  const uint32_t block = table + rel;
  if (!HostPageReadable(REX_RAW_ADDR(block + 0x10))) return;
  const uint32_t bytes = REX_LOAD_U32(block + 0x10);
  if (!bytes || bytes >= 0x10000) return;
  const uint32_t data_base = REX_LOAD_U32(shader + kPsDataBaseAt);
  if (!data_base) return;

  uint32_t at = block + 0x14;
  const uint32_t end = at + bytes;
  uint32_t applied = 0, entries = 0;
  std::string first;
  while (at + 8 <= end && HostPageReadable(REX_RAW_ADDR(at + 4))) {
    const uint32_t hdr = REX_LOAD_U32(at);
    const uint32_t reg = hdr >> 16;
    const uint32_t dwords = hdr & 0xFFFF;
    if (!dwords) break;
    const uint32_t data_off = REX_LOAD_U32(at + 4);
    at += 8;
    ++entries;
    const uint32_t src = data_base + data_off;
    for (uint32_t j = 0; j < dwords; ++j) {
      const uint32_t abs_reg = reg + j / 4;
      if (abs_reg < 256 || abs_reg >= 512) continue;
      const uint32_t dst = (abs_reg - 256) * 4 + (j % 4);
      if (dst >= bank.size()) continue;
      if (!HostPageReadable(REX_RAW_ADDR(src + j * 4))) continue;
      bank[dst] = REX_LOAD_U32(src + j * 4);
      ++applied;
    }
    if (entries <= 4)
      first += fmt::format(" c{}..c{}", reg, reg + (dwords + 3) / 4 - 1);
  }
}

// Read the second list in the shader patch block -- the one SetPixelShader and
// SetVertexShader memcpy directly into `device + 0x480`.
void ApplyShaderFetchPatchTable(uint32_t shader, uint32_t table_at,
                                uint8_t* base, ShaderFetchConstants& fetch) {
  if (!shader) return;
  const uint32_t table = shader + table_at;
  if (!HostPageReadable(REX_RAW_ADDR(table + 0x14))) return;
  const uint32_t rel = REX_LOAD_U32(table + 0x14);
  if (!rel || rel >= 0x10000) return;
  const uint32_t block = table + rel;
  if (!HostPageReadable(REX_RAW_ADDR(block + 0x10))) return;
  const uint32_t bytes = REX_LOAD_U32(block + 0x10);
  if (!bytes || bytes >= 0x10000) return;

  uint32_t at = block + 0x14;
  const uint32_t end = at + bytes;

  // First list: `(u16 reg, u16 dwords, u32 data_offset)`, terminated by a
  // zero dword count. This is the LOAD_ALU_CONSTANT list handled above.
  while (at + 4 <= end && HostPageReadable(REX_RAW_ADDR(at + 2))) {
    const uint32_t dwords = REX_LOAD_U16(at + 2);
    at += 4;
    if (!dwords) break;
    if (at + 4 > end) return;
    at += 4;
  }

  // Second list: inline device-shadow copies. Offset zero is fetch constant 0;
  // six dwords later begins fetch constant 1. Only retain the first 16 slots,
  // which is the HLE binding width; the guest block itself contains all 32.
  constexpr uint32_t kFetchBytes = mx::hle::kMaxSamplers *
                                   ShaderFetchConstants::kDwords * 4;
  while (at + 4 <= end && HostPageReadable(REX_RAW_ADDR(at + 2))) {
    const uint32_t byte_offset = REX_LOAD_U16(at);
    const uint32_t dwords = REX_LOAD_U16(at + 2);
    at += 4;
    if (!dwords) break;
    const uint64_t payload_bytes = uint64_t(dwords) * 4;
    if (payload_bytes > uint64_t(end - at)) return;
    for (uint32_t j = 0; j < dwords; ++j) {
      const uint64_t dst_byte = uint64_t(byte_offset) + uint64_t(j) * 4;
      if (dst_byte >= kFetchBytes) continue;
      const uint32_t src = at + j * 4;
      if (!HostPageReadable(REX_RAW_ADDR(src))) continue;
      const uint32_t fetch_dword = uint32_t(dst_byte / 4);
      const uint32_t sampler =
          fetch_dword / ShaderFetchConstants::kDwords;
      const uint32_t component =
          fetch_dword % ShaderFetchConstants::kDwords;
      fetch.words[fetch_dword] = REX_LOAD_U32(src);
      fetch.partial[sampler] |= uint8_t(1u << component);
      if (fetch.partial[sampler] == 0x3F)
        fetch.complete_mask |= 1u << sampler;
    }
    at += uint32_t(payload_bytes);
  }
}

// Both shaders publish into the SAME unified ALU constant file -- the draw
// flush calls sub_825656A0 once for the pixel shader and once for the vertex
// shader -- so an entry in EITHER table whose register lands in 256..511 is a
// pixel constant. Taking only the pixel shader's table left c43 and c85 at
// zero, which is most of a material's shading still missing.
// The fetch patches are gathered from BOTH shader objects because both binding
// calls write the same device constants block. They are keyed by the PIXEL
// shader handle because that is the identity the draw's texture resolver owns.
void ApplyPixelShaderLoadTable(
    uint32_t shader, uint32_t device, uint8_t* base,
    std::vector<uint32_t>& bank) {
  ShaderFetchConstants fetch;
  ApplyShaderLoadTable(shader, 0x28, 0x18, base, bank);
  ApplyShaderFetchPatchTable(shader, 0x28, base, fetch);
  // device+0x3248 is the vertex shader object; its table sits at +0x368 with
  // its data base at +0x20.
  constexpr uint32_t kDeviceVertexShaderAt = 0x3248;
  if (device && HostPageReadable(REX_RAW_ADDR(device + kDeviceVertexShaderAt))) {
    const uint32_t vs = REX_LOAD_U32(device + kDeviceVertexShaderAt);
    if (vs) {
      ApplyShaderLoadTable(vs, 0x368, 0x20, base, bank);
      ApplyShaderFetchPatchTable(vs, 0x368, base, fetch);
    }
  }
  if (!shader || !fetch.complete_mask) return;
  bool first = false;
  {
    std::lock_guard<std::mutex> lock(g_shaderFetchMu);
    first = g_shaderFetch.insert_or_assign(shader, fetch).second;
  }
  if (!first) return;
  // Once per shader that publishes any complete descriptor. This is what
  // confirms which shader objects actually embed complete descriptors, so it
  // names the samplers rather than merely counting them.
  ++g_shaderFetchPublished;
  std::string list;
  for (uint32_t s = 0; s < mx::hle::kMaxSamplers; ++s)
    if (fetch.complete_mask & (1u << s)) list += fmt::format(" s{}", s);
  REXLOG_INFO("d3d9: ps 0x{:08X} publishes its own texture fetch constants:{}",
              shader, list);
}

// Attach the guest's own translated pixel shader to a draw: its source, its
// constant bank, and one texture per sampler slot it declares.
//
// Extracted so it can run BEFORE the single-texture binding contest as well
// as after it. The contest answers a different question -- which one texture
// the tex*col stand-in should sample -- and used to gate this, which meant a
// shader fetching no texture at all never got here.
void AttachTranslatedPixelShader(mx::hle::DrawCall& dc, uint32_t handle,
                                 uint32_t device, uint8_t* base) {
  using namespace mx::hle;
  dc.pixel_shader_handle = handle;
  // Draws whose bound shader has no translation at all -- the other half of the
  // stand-in population, the half slot-filling cannot explain. Counted per
  // HANDLE because coverage is measured per shader and the picture is painted
  // per draw: 23 untranslated shaders out of 256 is a small share of the
  // former and may be a large share of the latter, and only this says which.
  if (!TranslatedPixelShader(handle)) {
    static std::map<uint32_t, uint64_t> s_byHandle;
    static uint64_t s_total = 0;
    ++s_total;
    ++s_byHandle[handle];
    // Every 500, not 5000: at 5000 this printed NOTHING across a 1943-frame run
    // while 25,359 draws took the stand-in -- silence that reads identically to
    // zero, and which sent the search to the wrong place until the arithmetic
    // was checked against the earlier exit.
    if ((s_total % 500) == 0) {
      std::vector<std::pair<uint64_t, uint32_t>> top;
      for (const auto& [h, n] : s_byHandle) top.emplace_back(n, h);
      std::sort(top.rbegin(), top.rend());
      std::string worst;
      for (size_t i = 0; i < top.size() && i < 6; ++i)
        worst += fmt::format(" 0x{:08X}={}", top[i].second, top[i].first);
      REXLOG_INFO("d3d9: draws with an UNTRANSLATED pixel shader: {} over {} "
                  "handles; worst:{}",
                  s_total, s_byHandle.size(), worst);
    }
  }
  if (const TranslatedShader* t = TranslatedPixelShader(handle)) {
    dc.pixel_shader_hlsl = t->source;
    dc.pixel_sampler_count = t->sampler_count;
    dc.pixel_sampler_array_mask = t->sampler_array_mask;
    // The PIXEL constant bank, ALU constants 256-511 at device+0x1780. Captured
    // per draw because the guest rewrites it between draws, and captured only
    // for a shader that will use it. Its base is applied here so the shader can
    // index from 0 — see DrawCall::pixel_constants.
    constexpr uint32_t kPixelConstBase = 0x1780;
    constexpr uint32_t kPixelConstRegs = 256;
    if (HostPageReadable(REX_RAW_ADDR(device + kPixelConstBase)) &&
        HostPageReadable(
            REX_RAW_ADDR(device + kPixelConstBase + kPixelConstRegs * 16 - 4))) {
      dc.pixel_constants.resize(kPixelConstRegs * 4);
      for (uint32_t i = 0; i < kPixelConstRegs * 4; ++i)
        dc.pixel_constants[i] = REX_LOAD_U32(device + kPixelConstBase + i * 4);
      // The shadow is only half the bank. Overlay the shader's own literals.
      ApplyPixelShaderLoadTable(handle, device, base, dc.pixel_constants);

      // One texture per slot the shader declares. A shader whose slots cannot
      // all be filled keeps the stand-in: running it with a missing texture
      // would sample whatever descriptor happened to be at that index, which is
      // a confident wrong answer rather than a visible failure.
      uint32_t filled = 0;
      for (uint32_t s = 0; s < t->sampler_count &&
                           s < mx::hle::DrawCall::kMaxPixelTextures; ++s) {
        if (ResolvePixelSlotTexture(dc, s, t->slot_guest[s], device, base))
          ++filled;
      }
      static uint64_t s_slotOk = 0, s_slotShort = 0;
      if (filled < t->sampler_count) {
        ++s_slotShort;
        dc.pixel_shader_hlsl.reset();
        dc.pixel_textures = {};
        dc.pixel_sampled_objects = {};
      } else {
        ++s_slotOk;
      }

      // The VERTEX stage's textures, by exactly the same rule. A vertex shader
      // that samples -- terrain displacement is the case here -- used to be
      // refused the GPU path for having a sampler at all, and the interpreter
      // it fell back to has no texture fetch, so its samples were silent zeros
      // and the positions silently wrong.
      //
      // All-or-nothing like the pixel stage, and for the same reason: a slot
      // left unfilled samples whatever descriptor sits at that index. Falling
      // short here clears the count rather than the shader, which puts the draw
      // back on the CPU path it used to take unconditionally.
      constexpr uint32_t kDeviceVertexShaderAt = 0x3248;
      uint32_t vs_handle = 0;
      if (HostPageReadable(REX_RAW_ADDR(device + kDeviceVertexShaderAt)))
        vs_handle = REX_LOAD_U32(device + kDeviceVertexShaderAt);
      if (const TranslatedShader* vt =
              vs_handle ? TranslatedVertexShader(vs_handle) : nullptr) {
        if (vt->sampler_count) {
          uint32_t vfilled = 0;
          for (uint32_t s = 0; s < vt->sampler_count &&
                               s < mx::hle::DrawCall::kMaxPixelTextures; ++s) {
            if (ResolvePixelSlotTexture(dc, s, vt->slot_guest[s], device, base,
                                        /*vertex=*/true))
              ++vfilled;
          }
          static uint64_t s_vsSlotOk = 0, s_vsSlotShort = 0;
          if (vfilled < vt->sampler_count) {
            ++s_vsSlotShort;
            dc.vertex_textures = {};
            dc.vertex_sampled_objects = {};
            dc.vertex_sampler_count = 0;
          } else {
            ++s_vsSlotOk;
            dc.vertex_sampler_count = vt->sampler_count;
            dc.vertex_sampler_array_mask = vt->sampler_array_mask;
          }
          if (((s_vsSlotOk + s_vsSlotShort) % 5000) == 1) {
            REXLOG_INFO("d3d9: VERTEX texture slots: {} draws bound, {} short",
                        s_vsSlotOk, s_vsSlotShort);
          }
        }
      }
      if (((s_slotOk + s_slotShort) % 5000) == 0) {
        // The resolve-address counters ride here rather than in ReportBoundZero,
        // which only fires every 2500 BLACK draws -- so the moment the black
        // count collapses, the numbers saying whether the address match is
        // doing the work disappear with it. They were invisible in exactly the
        // runs where they mattered most.
        size_t blank_outstanding = 0;
        for (const auto& [key, b] : g_blankTextures)
          if (!b.recovered) ++blank_outstanding;
        REXLOG_INFO("d3d9: translated texture slots: {} draws bound, {} short; "
                    "resolve address matches {} (extent mismatches {}, "
                    "partial-coverage refusals {}), "
                    "blank textures {} still blank of {}",
                    s_slotOk, s_slotShort, g_resolveAddressMatches,
                    g_resolveAddressExtentMiss, g_resolveAddressPartial,
                    blank_outstanding, g_blankTextures.size());
      }
    } else {
      // Without its constants the shader would compute from zeros, which is a
      // confident wrong answer. Drop the translation and keep the stand-in.
      dc.pixel_shader_hlsl.reset();
      static uint32_t s_logged = 0;
      if (s_logged++ < 4)
        REXLOG_INFO("d3d9: pixel constant bank unreadable at device+0x{:X}",
                    kPixelConstBase);
    }
  }
}

bool PrepareDrawTexture(mx::hle::DrawCall& dc, uint32_t pixel_shader,
                        uint32_t device, uint8_t* base,
                        mx::hle::PixelTextureBinding& binding) {
  using namespace mx::hle;
  PhaseTimer phase_timer(g_phaseTextureUs);
  static uint64_t s_attempts = 0, s_ready = 0, s_no_shader = 0;
  static uint64_t s_no_binding = 0, s_bad_desc = 0, s_unreadable = 0;
  static uint64_t s_mapped = 0, s_empty = 0, s_semantic_reject = 0;
  static uint64_t s_no_shader_no_setter = 0;
  ++s_attempts;
  // Driven by attempts, not by successes: the summary further down only fires
  // once a texture is ready, so a run in which every descriptor is rejected —
  // precisely the run this tally exists to characterise — would print nothing.
  if ((s_attempts % 2500) == 0 && !g_hleRejectedFormats.empty()) {
    REXLOG_INFO("d3d9: HLE rejected guest texture formats after {} attempts: {}",
                s_attempts, RejectedFormatSummary());
  }
  // WHICH shader is bound is a separate question from WHICH ONE TEXTURE the
  // stand-in should sample, and conflating them cost 63,207 draws in mx_709 --
  // 27.8% of every draw attempted, the largest single cause of stand-in draws
  // by a wide margin.
  //
  // Both resolutions below end in ResolvePixelBindingForDraw, which gives up
  // when `profile->bindings.empty()`. A pixel shader that fetches no texture at
  // all has exactly that -- no bindings -- so it was reported as "no eligible
  // pixel shader" and the draw kept the tex*col stand-in. For a TRANSLATED
  // shader that is precisely backwards: a shader sampling nothing needs no
  // texture, and the stand-in it fell back to is the one thing that does.
  //
  // So resolve the handle first, from the setter or from the device field, and
  // attach the translation on its own terms. The single-binding contest still
  // runs below, unchanged, because the stand-in still needs a winner -- but it
  // can no longer veto a draw that was never going to use its answer.
  uint32_t resolved = pixel_shader;
  if (!resolved && device &&
      HostPageReadable(REX_RAW_ADDR(device + 0x3244)))
    resolved = REX_LOAD_U32(device + 0x3244);
  // Last resort: the device's own last-bound shader, recorded across threads.
  // Both sources above are thread-local in effect -- the argument comes from a
  // thread_local DeviceState, and device+0x3244 read zero for 15,555 draws on a
  // loaded menu. Without this those draws take the tex*col stand-in and paint
  // whatever their first texture happens to be, which for a character material
  // is a packed normal/gloss map.
  //
  // That fallback was WRITTEN AND NEVER CALLED. This comment described it and
  // the code below stopped at device+0x3244, so PixelShaderForDevice sat dead
  // in the file. Measured cost of the omission, mx_890 at the menu: 79,984 of
  // 240,000 draws arrive with no setter handle at all; 133 of them per frame
  // carry 144,097 vertices, and because no translated pixel shader means no GPU
  // vertex path, they run the software interpreter and the CPU attribute decode
  // -- 121ms of a 225ms frame, the single largest item in it.
  // Gated, because it is both the largest speedup this session and a suspected
  // regression, and those have to be separable.
  //
  // It took the menu 4.45 -> 9.88 fps by giving 80,000 draws a translated pixel
  // shader, which is what lets them onto the GPU vertex path instead of the
  // software interpreter. But "unbound by sampler s0/s1/s2" appears in no run
  // before mx_891 and in every run after it, at ~69,000 draws a frame: the
  // shader this attaches declares sampler slots those draws never bound, and
  // each one gets a 1x1 black. Whether that is a wrong shader or a second
  // missing binding is the open question, and one flag answers it.
  // WHOSE device is it? Recorded BEFORE the fallback runs, so it describes the
  // draw as it arrived rather than after being patched up.
  //
  // The 3D layer of the menu is 133 draws a frame with no pixel shader from
  // either source, and both offsets have been confirmed against the guest:
  // D3DDevice_SetPixelShader writes pDevice[1].m_Constants.Fetch[29] —
  // device+0x3244 — which is exactly what was read above. So the field is not
  // being misread; either the shader was never set on THIS device, or the draw
  // is carrying the wrong device pointer.
  //
  // The guest runs three parallel record workers, each driving its own device
  // (dword_830B2C60[0..2]), and this file has twice been caught by state being
  // read from the wrong one of the three. If the draw devices below do not
  // appear among the setter devices, that is the whole defect.
  if (!resolved) {
    // What ARE these draws running? Self-limiting; see ProbeVertexObjectSecondBlob.
    ProbeVertexObjectSecondBlob(device, base);
    // Does a null-pixel-shader draw bind a COLOUR target?
    //
    // The probe above establishes what these draws are: one 48-dword program
    // that writes position and exports no interpolators at all, i.e. a
    // depth-only pass, which is why a null pixel shader is legal for them. The
    // renderer already has a route for that, but it opens only when NO colour
    // target is bound:
    //
    //     depthOnlyPass = !d.targetObject && d.depthObject && ...
    //
    // If these draws carry a colour target too, they miss it, and each one is
    // given a scratch colour target plus the tex*col stand-in -- painting
    // colour the guest never wrote, into the scene buffer the rider's material
    // later samples for its luminance. That would make the stand-in actively
    // harmful here rather than merely a missing feature.
    //
    // Split by which targets are present, and record the colour extents seen,
    // because "binds the 1280x640 scene band" and "binds some small offscreen
    // target" want different answers.
    {
      static std::mutex s_mu;
      static uint64_t s_colour_and_depth = 0, s_depth_only = 0;
      static uint64_t s_colour_only = 0, s_neither = 0;
      static std::set<uint64_t> s_extents;
      // Whether they actually PAINT is a separate question from whether they
      // bind a target, and it is decided by the colour mask, which the renderer
      // already honours:
      //
      //     colorWrite = (om_seen & 1) == 0 || (colour_mask & 0xF) != 0
      //
      // A depth pass that binds the colour target but masks colour off is
      // harmless -- its PSO gets RenderTargetWriteMask 0 and the stand-in
      // writes nothing. The damaging case is narrower: colour bound and the
      // mask permitting writes, so the stand-in paints.
      //
      // Read RB_COLOR_MASK from the device HERE rather than through dc.
      // dc.colour_mask and dc.om_seen are filled further down this same
      // function, ~34 lines AFTER the PrepareDrawTexture call this runs
      // inside, so consulting them reports every draw in the game as
      // "mask never observed" -- which is exactly what the first cut did, and
      // 41844 of 41844 was the tell. Same address and same gate as the
      // assignment below, so the two cannot drift.
      constexpr uint32_t kRbColorMaskAt = 0x28DC;
      static uint64_t s_wouldPaint = 0, s_maskedOff = 0, s_maskUnreadable = 0;
      uint32_t mask = 0;
      bool mask_readable = false;
      if (device && HostPageReadable(REX_RAW_ADDR(device + kRbColorMaskAt))) {
        mask = REX_LOAD_U32(device + kRbColorMaskAt) & 0xFu;
        mask_readable = true;
      }
      std::lock_guard<std::mutex> lk(s_mu);
      const bool has_colour = dc.render_target_object != 0;
      const bool has_depth = dc.depth_target_object != 0;
      if (has_colour) {
        if (!mask_readable) ++s_maskUnreadable;
        else if (mask != 0) ++s_wouldPaint;
        else ++s_maskedOff;
      }
      if (has_colour && has_depth) ++s_colour_and_depth;
      else if (has_depth) ++s_depth_only;
      else if (has_colour) ++s_colour_only;
      else ++s_neither;
      if (has_colour && s_extents.size() < 32) {
        s_extents.insert((uint64_t(dc.render_target_width) << 32) |
                         dc.render_target_height);
      }
      const uint64_t total = s_colour_and_depth + s_depth_only +
                             s_colour_only + s_neither;
      if ((total % 5000) == 0) {
        std::string extents;
        for (uint64_t e : s_extents)
          extents += fmt::format(" {}x{}", uint32_t(e >> 32), uint32_t(e));
        REXLOG_INFO("d3d9: NULL-PS TARGETS over {} draws: colour+depth {}, "
                    "depth only {}, colour only {}, neither {}; of those with "
                    "colour: WOULD PAINT {}, masked off {}, mask unreadable "
                    "{}; colour extents:{}",
                    total, s_colour_and_depth, s_depth_only, s_colour_only,
                    s_neither, s_wouldPaint, s_maskedOff, s_maskUnreadable,
                    extents.empty() ? " none" : extents);
      }
    }
    static std::mutex s_mu;
    static std::map<uint64_t, uint64_t> s_byDeviceThread;
    static uint64_t s_total = 0;
    bool report = false;
    {
      std::lock_guard<std::mutex> lk(s_mu);
      ++s_byDeviceThread[(uint64_t(device) << 32) | GetCurrentThreadId()];
      report = (++s_total % 20000) == 0;
    }
    if (report) {
      std::string draws;
      {
        std::lock_guard<std::mutex> lk(s_mu);
        for (const auto& [key, n] : s_byDeviceThread) {
          draws += fmt::format(" dev=0x{:08X}/t{}={}", uint32_t(key >> 32),
                               uint32_t(key & 0xFFFFFFFFu), n);
        }
      }
      REXLOG_INFO("d3d9: NO-PS DEVICES over {} draws:{}", s_total, draws);
      REXLOG_INFO("d3d9: SETTER DEVICES:{}", PixelShaderDeviceSummary());
    }
  }
  if (!resolved) {
    resolved = PixelShaderForDeviceStrict(device);
    if (resolved) ++g_psFromDeviceRecord;
  }
  if (resolved) {
    // Normally reached via ReadBoundPixelShader, which is below the contest and
    // therefore never ran for these draws -- so their microcode was never
    // collected and they could not have translated even in principle.
    CollectPixelShaderBlob(resolved, base);
    AttachTranslatedPixelShader(dc, resolved, device, base);
  }

  if ((!pixel_shader ||
       !ResolvePixelBindingForDraw(pixel_shader, device, base, binding)) &&
      !ReadBoundPixelShader(device, base, pixel_shader, binding)) {
    ++s_no_shader;
    // The COUNT, not just a sample. This exit was logged only as "attempt N"
    // with a handle, which said it happens without ever saying how often --
    // and it turns out to be the largest single reason a draw keeps the
    // tex*col stand-in, larger than untranslated shaders and slot-filling put
    // together. A cap of 8 plus a sample every 2500 attempts looked like a
    // quiet failure while it was in fact the dominant one.
    //
    // Split by which of the two resolutions was even possible: no setter
    // handle at all is a different defect from a setter we cannot follow.
    if (!pixel_shader) ++s_no_shader_no_setter;
    if (s_no_shader <= 8 || (s_attempts % 2500) == 0) {
      const uint32_t direct =
          device && HostPageReadable(REX_RAW_ADDR(device + 0x3244))
              ? REX_LOAD_U32(device + 0x3244)
              : 0;
      // Named for what it now means. This used to read "NO ELIGIBLE PIXEL
      // SHADER", which was true when the contest also decided whether the draw
      // could run its guest shader at all -- it no longer does, and a draw
      // reaching here may well be running a translated shader with every slot
      // bound. Left as it was, the next reader would diagnose 80,000 lost
      // draws that are not lost.
      REXLOG_INFO("d3d9: stand-in has no single texture to sample: {} of {} "
                  "attempts ({} with no setter handle at all, {} rescued by "
                  "the per-device record); this one setter=0x{:08X}, "
                  "device+0x3244=0x{:08X}",
                  s_no_shader, s_attempts, s_no_shader_no_setter,
                  g_psFromDeviceRecord, pixel_shader, direct);
    }
    return false;
  }
  // The shader is resolved by here — `pixel_shader` may have been rewritten by
  // ReadBoundPixelShader — so this is the first point at which the draw knows
  // which guest program it is running. Attaching the translation here rather
  // than at the call site keeps the "which shader is really bound" logic in one
  // place; a draw whose shader did not translate simply carries nothing and
  // keeps the tex*col stand-in.
  // The contest may have rewritten `pixel_shader` to the device's own handle,
  // which is the authoritative answer. If it differs from the one attached
  // before the contest, attach again for the shader actually bound.
  if (pixel_shader != resolved)
    AttachTranslatedPixelShader(dc, pixel_shader, device, base);
  dc.pixel_shader_handle = pixel_shader;

  if (binding.sampler >= kMaxSamplers) {
    ++s_no_binding;
    if (s_no_binding <= 8)
      REXLOG_INFO("d3d9: HLE texture fallback: sampler {} out of range",
                  binding.sampler);
    return false;
  }
  // No coverage rule here, deliberately. This path picks ONE texture to
  // represent a draw the translated path could not take, so a partly-written
  // snapshot is still the best thing it has -- there is no second source to
  // prefer over it, which is the only reason the rule exists on the slot path.
  const auto& texture_state = DeviceState().texture[binding.sampler];
  if (texture_state.object &&
      g_resolvedTextureTargets.contains(texture_state.object)) {
    ++s_mapped;
    // The resolved texture still owns a normal fetch descriptor. Capture its
    // logical extent before taking the host-target path so denormalized shader
    // coordinates can be converted without requiring a CPU payload.
    uint32_t mapped_fetch[6] = {};
    HleTextureSource mapped_source;
    const char* mapped_why = nullptr;
    if (ReadLiveTextureFetch(device, base, binding.sampler, mapped_fetch)) {
      if (DescribeHleTexture2D(mapped_fetch, mapped_source, &mapped_why)) {
        dc.sampled_texture_width = mapped_source.width;
        dc.sampled_texture_height = mapped_source.height;
        dc.clamp_x = uint8_t(mapped_source.clamp_x);
        dc.clamp_y = uint8_t(mapped_source.clamp_y);
      } else {
        // This failure used to be discarded outright. Three quarters of all
        // texture attempts take this early return, so an undecodable format
        // arriving on a resolved target produced no log line at all — which
        // is why "no YUV format has ever been rejected" was not evidence of
        // anything. The branch still returns true; only its silence changes.
        NoteRejectedTextureFormat("mapped", binding.sampler, mapped_source,
                                  mapped_why, mapped_fetch);
      }
    }
    // The renderer samples the live host render target identified below. Do
    // not also upload its stale/empty guest storage as an immutable texture.
    dc.texture.reset();
    return true;
  }
  uint32_t fetch[6] = {};
  if (!ReadLiveTextureFetch(device, base, binding.sampler, fetch)) {
    ++s_no_binding;
    if (s_no_binding <= 8) {
      REXLOG_INFO("d3d9: HLE texture fallback: sampler {} has no 2D fetch "
                  "(device words {:08X} {:08X} {:08X} {:08X} {:08X} {:08X})",
                  binding.sampler, fetch[0], fetch[1], fetch[2], fetch[3],
                  fetch[4], fetch[5]);
    }
    return false;
  }
  HleTextureSource source;
  const char* why = nullptr;
  if (!DescribeHleTexture2D(fetch, source, &why)) {
    ++s_bad_desc;
    NoteRejectedTextureFormat("prepare", binding.sampler, source, why, fetch);
    return false;
  }
  dc.sampled_texture_width = source.width;
  dc.sampled_texture_height = source.height;
  dc.clamp_x = uint8_t(source.clamp_x);
  dc.clamp_y = uint8_t(source.clamp_y);

  // The address route, for the same reason as in ResolvePixelSlotTexture: this
  // memory is a resolve destination reached through a texture object the
  // resolve never named, so the object test above missed it.
  //
  // Both routing fields are set here rather than left to PrepareHleDraw, which
  // sets them only when its own object lookup hits. It runs after this and does
  // not clear them on a miss, so these survive.
  if (const ResolvedTargetByAddress* resolved =
          ResolvedTargetForAddress(source)) {
    ++s_mapped;
    ++g_resolveAddressMatches;
    dc.sampled_texture_object = resolved->dest_object;
    dc.sampled_render_target_object = resolved->source_object;
    // The renderer samples the snapshot; uploading the empty guest storage
    // alongside it would just be the black texture again.
    dc.texture.reset();
    return true;
  }

  // kR8 and kR16 join this list on the same reasoning as the others: they are
  // single-channel, so binding one as visible base colour would paint the
  // surface grey. They are decoded rather than rejected so the counters can
  // tell "we cannot read this" apart from "we choose not to show this" — the
  // distinction the old shared "unsupported" string destroyed.
  if (source.host_format == HostTextureFormat::kBc5 ||
      source.host_format == HostTextureFormat::kR16Float ||
      source.host_format == HostTextureFormat::kRgba16Float ||
      source.host_format == HostTextureFormat::kR8 ||
      source.host_format == HostTextureFormat::kR16 ||
      source.host_format == HostTextureFormat::kR32Float) {
    ++s_semantic_reject;
    if (s_semantic_reject <= 12) {
      // Named in guest terms as well: this drop is a policy choice about a
      // format we *can* decode, so telling the two kinds of loss apart in the
      // log matters when deciding what to add next.
      REXLOG_INFO("d3d9: HLE texture fallback: sampler {} guest format {} ({}) "
                  "decodes to host format {} but is not an immutable colour "
                  "asset",
                  binding.sampler, source.guest_format,
                  GuestTextureFormatName(source.guest_format),
                  uint32_t(source.host_format));
    }
    return false;
  }
  const uint64_t key = HleTextureKey(fetch);
  // Blank textures are still a poor representative for the one texture this
  // path picks to stand in for the whole draw -- but only while they ARE blank.
  // The refusal now expires on the same backoff the translated path retries on,
  // so a texture the guest streams in later is reconsidered instead of being
  // written off for the rest of the run.
  if (!BlankRetryDue(key)) {
    ++s_empty;
    return false;
  }
  auto cached = g_hleCpuTextures.find(key);
  if (cached != g_hleCpuTextures.end()) {
    if (!GlyphCacheStale(*cached->second)) {
      dc.texture = cached->second;
      ++s_ready;
      return true;
    }
    g_hleCpuTextures.erase(cached);
  }
  std::vector<uint8_t> guest;
  if (!CopyTexturePhysical(source, base, guest)) {
    ++s_unreadable;
    if (s_unreadable <= 8) {
      REXLOG_INFO("d3d9: HLE texture fallback: sampler {} source 0x{:08X} "
                  "(size {}) unreadable",
                  binding.sampler, source.address, source.source_bytes);
    }
    return false;
  }
  auto payload = std::make_shared<HleTexturePayload>();
  if (!DecodeHleTexture2D(source, guest.data(), guest.size(), *payload, &why)) {
    ++s_bad_desc;
    if (s_bad_desc <= 12)
      REXLOG_INFO("d3d9: HLE texture decode rejected ({})",
                  why ? why : "?");
    return false;
  }
  size_t nonzero_bytes = 0;
  if (!HleTextureHasNonzeroData(*payload, &nonzero_bytes)) {
    NoteBlankDecode(key);
    ++s_empty;
    if (s_empty <= 12) {
      REXLOG_INFO("d3d9: HLE texture fallback: sampler {} {}x{} format {} "
                  "decoded to an all-zero guest payload",
                  binding.sampler, source.width, source.height,
                  uint32_t(source.host_format));
    }
    return false;
  }
  NoteBlankRecovered(key);
  payload->key = key;
  payload->content_version = g_glyphCacheGeneration;
  dc.texture = payload;
  g_hleCpuTextures.emplace(key, std::move(payload));
  ++s_ready;
  if (s_ready <= 8 || (s_attempts % 2500) == 0) {
    REXLOG_INFO("d3d9: HLE textures attempts {} ready {} mapped {} cached {} "
                "empty {} semantic-reject {} no-shader {} no-binding {} "
                "bad-desc {} unreadable {}; latest {}x{} format {} in "
                "viewport {}x{} ({} nonzero bytes)",
                s_attempts, s_ready, s_mapped, g_hleCpuTextures.size(), s_empty,
                s_semantic_reject, s_no_shader, s_no_binding, s_bad_desc,
                s_unreadable, dc.texture->width,
                dc.texture->height, uint32_t(dc.texture->format),
                dc.viewport_width, dc.viewport_height, nonzero_bytes);
  }
  return true;
}


//---------------------------------------------------------------------------
// The declaration-to-vfetch pairing rule, read out of
// D3D::PatchVertexShaderToMatchVertexDeclaration (0x82564C50).
//
// This was the last bridge in the shader-execution path: attributes were taken
// from PM4's decode because nothing said which declaration element feeds which
// vfetch instruction. The function itself says, and it says it with a table.
//
//   this   = r3   CVertexShader*
//   dest   = r4   the microcode being patched — where results are written
//   decl   = r5   CVertexDeclaration*, count at +0x18, elements at +0x34
//   strides= r6   const BYTE*, indexed by stream, stride in DWORDS
//   variant= r7   which patched variant of the shader this is
//
// The shader carries a **binding table**, one dword per vfetch:
//
//   blob  = this + *(this + (variant + 0x70) * 8) + 0x368     ; GetUCode(variant)
//   count = blob[0x1C]
//   table = blob + 4 * (blob[0x18] + 9)
//
//   key[11:0]  -> vfetch instruction index; the patched triple is written to
//                 dest + 12 * index
//   key[15:12] -> D3DDECLUSAGE
//   key[19:16] -> usage index
//
// **The pairing is by semantic, not by position.** The element whose `usage`
// (byte 9) and `usage_index` (byte 10) equal the key's is the one that patches
// that vfetch — a linear search, first match wins. That is why the template's
// format/offset/stride are blank: they are not defaults, they are unbound.
//
// From the matched element:
//   fetch constant index = 95 - element.stream       (subfic r20, r5, 0x5F)
//   format/signed/integer/swizzle from the Type dword, as kType* already say
//   offset field = element.offset / 4                (rlwinm r8, r8, 6, 1, 23)
//   stride field = strides[element.stream]           (lbzx r5, r5, r6)
//
// **No match leaves fetch constant 95 as well**, which is the same value stream
// 0 produces. So a decoded fetch_slot of 95 is ambiguous between "stream 0" and
// "unbound", and PM4's decode showing 95 everywhere never distinguished them.
// The unbound case is identifiable by its canned format bits (0x60000) and
// swizzle (0x9250) instead.
//
// None of this is believed on the strength of the disassembly. The probe
// predicts all three dwords of the patched vfetch *before* the call and
// compares them against what D3D9 actually wrote *after* it. A rule read wrong
// disagrees; a rule read right agrees on every dword of every vfetch.
//---------------------------------------------------------------------------
constexpr uint32_t kUCodePtrTable  = 0x70;   // this + (variant + 0x70) * 8
constexpr uint32_t kUCodeBlobDelta = 0x368;
constexpr uint32_t kBlobTableOff   = 0x18;   // dword: table start, in dwords - 9
constexpr uint32_t kBlobFetchCount = 0x1C;   // dword: how many vfetches
constexpr uint32_t kTemplateBase   = 0x44;   // this + 0x44 + 416 * variant
constexpr uint32_t kTemplateStride = 0x1A0;
constexpr uint32_t kDeclCountOff   = 0x18;
constexpr uint32_t kDeclElemsOff   = 0x34;
constexpr uint32_t kMaxPatchFetch  = 32;

uint64_t g_patchProbed = 0;        // calls the probe actually examined
uint64_t g_patchFetches = 0;       // vfetch slots predicted
uint64_t g_patchBound = 0;         // ... of which a declaration element matched
uint64_t g_patchUnbound = 0;       // ... of which none did
// Per *field*, not per dword. A whole-dword comparison asks a stricter question
// than the one that matters: two of these fields are written by machinery this
// rule does not model (the second pass that coalesces adjacent fetches, and the
// swizzle chain through word_8204E178), and folding them in would report the
// fields that ARE read correctly as failures.
enum PatchField : uint32_t {
  kPfFetchConst = 0,   // dword0 [26:20] — 95 - stream
  kPfCoalesce,         // dword0 [29:27] — second pass; NOT modelled
  kPfFormat,           // dword1 [21:16] — Type[5:0]
  kPfNumFormat,        // dword1 [13:12] — Type[9:8]
  kPfSwizzle,          // dword1 [11:0]  — swizzle chain; NOT modelled
  kPfOffset,           // dword2 [30:8]  — element.offset / 4
  kPfStride,           // dword2 [7:0]   — strides[stream]
  kPatchFieldCount,
};
const char* const kPatchFieldName[kPatchFieldCount] = {
    "fetch const (95-stream)", "coalesce count [29:27] (unmodelled)",
    "format", "signed/integer", "swizzle [11:0] (unmodelled)",
    "offset (elem.offset/4)", "stride (strides[stream])"};
const uint32_t kPatchFieldDword[kPatchFieldCount] = {0, 0, 1, 1, 1, 2, 2};
const uint32_t kPatchFieldMask[kPatchFieldCount] = {
    0x07F00000u, 0x38000000u, 0x003F0000u, 0x00003000u,
    0x00000FFFu, 0x7FFFFF00u, 0x000000FFu};

uint64_t g_pfAgree[kPatchFieldCount] = {};
uint64_t g_pfDisagree[kPatchFieldCount] = {};

uint64_t g_patchAgree[3] = {};     // per dword, prediction == what D3D9 wrote
uint64_t g_patchDisagree[3] = {};
uint64_t g_patchBadTable = 0;      // count or table offset outside anything sane

// Where does D3D9 write the patched microcode? r4 is that destination, and if
// it is the same memory the draw-time probe already reads (SH_pPhysical +
// 0x40), then the patched code is directly readable and none of this rule needs
// reimplementing — the prediction only ever needed to be a *check*. Measured
// rather than assumed, because Stage B concluded that buffer held the unpatched
// template and that conclusion has to be either confirmed or overturned.
uint64_t g_destIsPhys40 = 0;       // dest == unmasked SH_pPhysical + 0x40
uint64_t g_destIsPhysOther = 0;    // inside that allocation, different offset
uint64_t g_destElsewhere = 0;
uint32_t g_destSample[4] = {};     // self, dest, SH_pPhysical, delta
bool     g_destHaveSample = false;
uint32_t g_patchFirstMismatch[6] = {};  // predicted/actual triple, first miss
bool     g_patchHaveMismatch = false;
// usage -> how many vfetches bound to it, so the report says which semantics
// this title actually feeds its shaders.
std::map<uint32_t, uint64_t> g_patchUsage;



// Reads the binding table, predicts every patched vfetch, and returns them for
// comparison after the original runs. Every read is page-guarded; the pointers
// are D3D9's own arguments, which it is about to dereference itself.
void PredictPatchedFetches(uint32_t self, uint32_t dest, uint32_t decl,
                           uint32_t strides, uint32_t variant, uint8_t* base,
                           std::vector<PatchPrediction>& out) {
  out.clear();
  if (!self || !dest || !decl || !strides) return;

  // Is the destination the buffer the draw-time probe already reads?
  if (HostPageReadable(REX_RAW_ADDR(self + 0x20))) {
    const uint32_t phys = REX_LOAD_U32(self + 0x20);
    if (dest == phys + 0x40) {
      ++g_destIsPhys40;
    } else if (phys && dest > phys && dest - phys < 0x1000) {
      ++g_destIsPhysOther;
    } else {
      ++g_destElsewhere;
    }
    if (!g_destHaveSample) {
      g_destHaveSample = true;
      g_destSample[0] = self;
      g_destSample[1] = dest;
      g_destSample[2] = phys;
      g_destSample[3] = dest - phys;
    }
  }

  const uint32_t slot = self + (variant + kUCodePtrTable) * 8;
  if (!HostPageReadable(REX_RAW_ADDR(slot))) return;
  const uint32_t blob = self + REX_LOAD_U32(slot) + kUCodeBlobDelta;
  if (!HostPageReadable(REX_RAW_ADDR(blob + kBlobFetchCount))) return;

  const uint32_t count = REX_LOAD_U32(blob + kBlobFetchCount);
  const uint32_t tbl_off = REX_LOAD_U32(blob + kBlobTableOff);
  if (count == 0 || count > kMaxPatchFetch || tbl_off > 0x10000) {
    ++g_patchBadTable;
    return;
  }
  const uint32_t table = blob + 4 * (tbl_off + 9);

  if (!HostPageReadable(REX_RAW_ADDR(decl + kDeclCountOff))) return;
  const uint32_t nelem = REX_LOAD_U32(decl + kDeclCountOff);
  if (nelem > kMaxElements) { ++g_patchBadTable; return; }

  for (uint32_t i = 0; i < count; ++i) {
    if (!HostPageReadable(REX_RAW_ADDR(table + i * 4))) return;
    const uint32_t key = REX_LOAD_U32(table + i * 4);
    const uint32_t instr = key & 0xFFF;
    const uint32_t usage = (key >> 12) & 0xF;
    const uint32_t uidx = (key >> 16) & 0xF;

    // The template triple this vfetch starts from.
    const uint32_t tmpl = self + kTemplateBase + kTemplateStride * variant +
                          12 * i;
    if (!HostPageReadable(REX_RAW_ADDR(tmpl)) ||
        !HostPageReadable(REX_RAW_ADDR(tmpl + 8)))
      return;
    const uint32_t d0 = REX_LOAD_U32(tmpl);
    const uint32_t d1 = REX_LOAD_U32(tmpl + 4);
    const uint32_t d2 = REX_LOAD_U32(tmpl + 8);

    // The linear search by semantic, exactly as the function does it.
    uint32_t match = nelem;
    for (uint32_t e = 0; e < nelem; ++e) {
      const uint32_t ea = decl + kDeclElemsOff + e * kElementSize;
      if (!HostPageReadable(REX_RAW_ADDR(ea))) return;
      if (REX_LOAD_U8(ea + 9) == usage && REX_LOAD_U8(ea + 10) == uidx) {
        match = e;
        break;
      }
    }

    PatchPrediction p;
    p.dest_addr = dest + 12 * instr;
    if (match < nelem) {
      const uint32_t ea = decl + kDeclElemsOff + match * kElementSize;
      const uint32_t stream = REX_LOAD_U16(ea + 0);
      const uint32_t offset = REX_LOAD_U16(ea + 2);
      const uint32_t type = REX_LOAD_U32(ea + 4);
      if (!HostPageReadable(REX_RAW_ADDR(strides + stream))) return;
      const uint32_t stride = REX_LOAD_U8(strides + stream);

      p.pred[0] = (d0 & 0xC00FFFFFu) | (((95u - stream) & 0x7Fu) << 20);
      p.pred[1] = (d1 & 0xBFC0CFFFu) |
                  ((((type << 12) & 0x3F000u) | (type & 0x300u)) << 4);
      p.pred[2] = (d2 & 0x80000000u) | ((offset << 6) & 0x7FFFFF00u) | stride;
      p.bound = true;
      ++g_patchUsage[usage];
    } else {
      p.pred[0] = (d0 & 0xC00FFFFFu) | 0x5F00000u;
      p.pred[1] = (d1 & 0xBFC0CFFFu) | 0x60000u;
      if (!HostPageReadable(REX_RAW_ADDR(strides))) return;
      p.pred[2] = (d2 & 0x80000000u) | REX_LOAD_U8(strides);
      p.bound = false;
    }
    out.push_back(p);
  }
}

// The binding table's vfetch count on its own, so the capture can run on every
// call while the full prediction stays sampled.
uint32_t ReadPatchFetchCount(uint32_t self, uint32_t variant, uint8_t* base) {
  if (!self) return 0;
  const uint32_t slot = self + (variant + kUCodePtrTable) * 8;
  if (!HostPageReadable(REX_RAW_ADDR(slot))) return 0;
  const uint32_t blob = self + REX_LOAD_U32(slot) + kUCodeBlobDelta;
  if (!HostPageReadable(REX_RAW_ADDR(blob + kBlobFetchCount))) return 0;
  const uint32_t count = REX_LOAD_U32(blob + kBlobFetchCount);
  return count > kMaxPatchFetch ? 0 : count;
}

// Copies the patched microcode out of the destination, keyed by shader handle.
// Must run immediately after the original: the destination is in the command
// ring and will be overwritten.
// Does the shader OBJECT carry its own microcode, and if so where?
//
// This matters because the patch hook is our only source of vertex microcode,
// and it only fires for shaders D3D9 needs to patch. Everything else is
// reported as "no-code": 164,648 of 401,750 draws in mx_711 (41%), which are
// the same draws as the 82,324 of 129,004 dropped before reaching the renderer.
// They are not a rendering fault -- they never had a program to run.
//
// The offset is SEARCHED rather than assumed, against code already proven by
// the patch hook's own decode. If one offset explains every shader, it is a
// property of the layout and can be relied on; if the histogram is spread, the
// premise is wrong and this says so instead of producing plausible garbage.
// Same discipline as the CF-start search this file already documents.
// Where a vertex shader's own microcode lives, and how long it is.
//
// Both transcribed from sub_82565550, the routine that uploads a shader: it
// allocates ring space, copies the code in, and only THEN calls
// PatchVertexShaderToMatchVertexDeclaration on the ring copy.
//
//   v17 = *(*(self + (variant+0x70)*8) + self + 876)   // size in BYTES
//   v23 = *(*(self + (variant+0x70)*8) + self + 872) + *(self + 0x20)
//   v24 = (((v23 >> 20) + 512) & 0x1000) + (v23 & 0x1FFFFFFF) - 0x40000000
//   memcpy(dest, v24, v17)
//
// 872 is kUCodeBlobDelta and 876 is the dword after it, so the size sits at
// blob+4 -- beside the fetch count at blob+0x1C this file already reads. The
// code itself is at blob + *(self+0x20), through an address fixup that clears
// the 0x40000000 segment bit. Searching a window around the blob found NOTHING
// in 36,000 shaders, which is exactly right: the fixup moves it out of range.
uint32_t ShaderObjectBlob(uint32_t self, uint32_t variant, uint8_t* base) {
  const uint32_t slot = self + (variant + kUCodePtrTable) * 8;
  if (!self || !HostPageReadable(REX_RAW_ADDR(slot))) return 0;
  return self + REX_LOAD_U32(slot) + kUCodeBlobDelta;
}

uint32_t ShaderObjectCodeBytes(uint32_t self, uint32_t variant, uint8_t* base) {
  const uint32_t blob = ShaderObjectBlob(self, variant, base);
  if (!blob || !HostPageReadable(REX_RAW_ADDR(blob + 4))) return 0;
  return REX_LOAD_U32(blob + 4);
}

uint32_t ShaderObjectCodeAddress(uint32_t self, uint32_t variant,
                                 uint8_t* base) {
  const uint32_t blob = ShaderObjectBlob(self, variant, base);
  if (!blob || !HostPageReadable(REX_RAW_ADDR(self + 0x20))) return 0;
  // *(blob), not blob. The decompilation reads
  //   v23 = *(_DWORD *)(*(...) + a3 + 872) + *(_DWORD *)(a3 + 32)
  // and that outer dereference is easy to drop, because the very similar
  // expression in PatchVertexShaderToMatchVertexDeclaration uses the same
  // address WITHOUT one (as the base for the fetch table). Taking blob itself
  // put the read 0 of 28,000 shaders' first eight dwords -- caught only
  // because the probe checked alignment separately from content.
  if (!HostPageReadable(REX_RAW_ADDR(blob))) return 0;
  const uint32_t v23 = REX_LOAD_U32(blob) + REX_LOAD_U32(self + 0x20);
  const uint32_t addr =
      (((v23 >> 20) + 512) & 0x1000) + (v23 & 0x1FFFFFFF) - 0x40000000u;
  return HostPageReadable(REX_RAW_ADDR(addr)) ? addr : 0;
}

void ProbeShaderObjectCode(uint32_t self, uint32_t variant,
                           const PatchedCode& known, uint8_t* base) {
  if (!known.resolved || known.code.size() <= known.code_off + 8) return;
  static std::map<int64_t, uint64_t> s_offsets;
  static uint64_t s_probed = 0, s_found = 0;
  ++s_probed;

  const uint32_t src = ShaderObjectCodeAddress(self, variant, base);
  if (!src) return;
  const uint32_t size = ShaderObjectCodeBytes(self, variant, base);
  if (!size || size > 64u * 1024u) return;

  // The ring copy this capture came from IS this memory, byte for byte, at the
  // moment of the copy -- dest was memcpy'd from here. So every dword should
  // agree EXCEPT the vfetch fields the patch then rewrote in the ring. A
  // handful of differing dwords confirms the address; wholesale disagreement
  // means it is the wrong buffer, and the count is what tells them apart.
  const uint32_t have = uint32_t(known.code.size());
  uint32_t compared = 0, differ = 0;
  for (uint32_t i = 0; i < size / 4; ++i) {
    const uint32_t at = kPatchWindowBack + i;
    if (at >= have) break;
    if ((at & (kHostPageSize - 1)) == 0 &&
        !HostPageReadable(REX_RAW_ADDR(src + i * 4)))
      break;
    ++compared;
    if (REX_LOAD_U32(src + i * 4) != known.code[at]) ++differ;
  }
  if (!compared) return;
  ++s_found;

  // As a SHARE of the shader, not an absolute count. 226 differing dwords is
  // 11% of a 2000-dword program and 95% of a 237-dword one, and those mean
  // opposite things -- the first is the patch rewriting fetches, the second is
  // the wrong buffer. The absolute histogram could not tell them apart.
  const uint32_t pct = differ * 100 / compared;
  ++s_offsets[pct < 1 ? 0 : pct < 5 ? 5 : pct < 10 ? 10 : pct < 25 ? 25
              : pct < 50 ? 50 : 100];

  // Independently: does the program START at this address? The copy was
  // byte-for-byte, so a correct address agrees on the leading dwords unless a
  // fetch sits at instruction 0. Alignment is a separate claim from content.
  static uint64_t s_headMatch = 0;
  bool head = true;
  for (uint32_t i = 0; i < 8 && head; ++i)
    head = REX_LOAD_U32(src + i * 4) == known.code[kPatchWindowBack + i];
  if (head) ++s_headMatch;

  if ((s_probed % 2000) == 0) {
    std::string hist;
    for (const auto& [b, n] : s_offsets)
      hist += b == 100   ? fmt::format(" >=50%={}", n)
              : b == 0   ? fmt::format(" 0%={}", n)
                         : fmt::format(" <{}%={}", b, n);
    REXLOG_INFO("d3d9: shader-object code probe: {} of {} readable at "
                "blob+[self+0x20], {} with a matching first 8 dwords; share "
                "of dwords differing from the patched ring copy:{}",
                s_found, s_probed, s_headMatch, hist.empty() ? " none" : hist);
  }
}

void CapturePatchedCode(uint32_t self, uint32_t dest, uint32_t variant,
                        uint32_t expect_fetches, uint8_t* base) {
  if (!self || !dest || dest < kPatchWindowBack * 4) return;
  const uint32_t start = dest - kPatchWindowBack * 4;

  auto it = g_patchedCode.find(self);
  const bool known = it != g_patchedCode.end() && it->second.resolved;
  const uint32_t known_off = known ? it->second.code_off : 0;

  PatchedCode pc;
  pc.expect_fetches = expect_fetches;
  pc.variant = variant;
  pc.code.reserve(kPatchWindowBack + kPhysProbeDwords);
  for (uint32_t i = 0; i < kPatchWindowBack + kPhysProbeDwords; ++i) {
    const uint32_t at = start + i * 4;
    if ((at & (kHostPageSize - 1)) == 0 && !HostPageReadable(REX_RAW_ADDR(at)))
      break;
    pc.code.push_back(REX_LOAD_U32(at));
  }
  if (pc.code.size() < 32) return;

  // Try the known offset first — but *verify* it, do not assume it. An earlier
  // version cached the offset and reused it blind, and the draw-time decode
  // then failed on thousands of captures while the report happily said the
  // shader was resolved. A cached answer that is never re-checked is an
  // assumption wearing a measurement's clothes.
  static std::vector<mx::hle::VertexAttribute> probe;
  auto decodes_at = [&](uint32_t s) {
    if (s >= pc.code.size()) return false;
    probe.clear();
    return mx::hle::DecodeVertexShaderFetches(pc.code.data() + s,
                                              uint32_t(pc.code.size() - s),
                                              probe, nullptr) &&
           probe.size() == expect_fetches;
  };

  // dest first. Measured over 24 distinct shaders: the CF stream starts exactly
  // at the patch destination in 24 of 24, while the upward scan below lands
  // early in 3 of them (off -3, -3, -85) because it takes the first offset that
  // decodes and a false positive can precede the true start. Trying dest before
  // scanning costs one decode and removes that whole failure mode.
  //
  // Still verified, not assumed — same rule as the cached offset below.
  if (decodes_at(kPatchWindowBack)) {
    pc.code_off = kPatchWindowBack;
    pc.resolved = true;
  } else if (known && decodes_at(known_off)) {
    pc.code_off = known_off;
    pc.resolved = true;
  } else {
    // Only the true CF start decodes to the count the binding table states, so
    // this is a search with a checkable answer rather than a guess. Preferring
    // the known offset first also stops a low false positive from winning when
    // the real layout is already established.
    for (uint32_t s = 0; s < pc.code.size(); ++s) {
      if (!decodes_at(s)) continue;
      pc.code_off = s;
      pc.resolved = true;
      ++g_patchCodeOffsets[int32_t(s) - int32_t(kPatchWindowBack)];
      break;
    }
  }

  // Does the guest state the answer the search just hunted for?
  //
  // sub_82565928's VS branch computes the program address the GPU is given as
  // *(vs + 0x20) + *(info + 0x368), where info = vs + *(vs + 0x380 +
  // variant*8), with the length in bytes at info + 0x36C. The patcher
  // (0x82564C50) indexes the identical 0x380 + variant*8 field, so both agree
  // the patched code lives in the shader's own allocation.
  //
  // Compared as ABSOLUTE guest addresses, which is the only common ground: the
  // search's answer is an index into a ring window, the field's is a pointer.
  // Reporting them any other way would compare two different coordinate
  // systems and call the mismatch a finding.
  //
  // Measurement only. Nothing here changes what is captured — if the field is
  // right, the search is still what runs until a separate change says so.
  // The program length, read unconditionally — it bounds the code handed to the
  // decoders and so is not a diagnostic. Only the reporting below is gated.
  //
  // The length is the canonical program's, and 2520 of 2561 captures patch into
  // a buffer other than the shader's own allocation. Taken as applying to the
  // patched copy anyway: patching rewrites fetch instructions in place and
  // cannot change the instruction count. If that were wrong the bound would cut
  // a shader short and the fetch decode would fail loudly rather than silently.
  uint32_t field_abs = 0, field_len = 0;
  {
    const uint32_t info_at = self + kVsInfoOffsetAt + variant * 8;
    if (HostPageReadable(REX_RAW_ADDR(info_at)) &&
        HostPageReadable(REX_RAW_ADDR(self + kVsCodeAllocAt))) {
      const uint32_t info = self + REX_LOAD_U32(info_at);
      if (HostPageReadable(REX_RAW_ADDR(info + kVsInfoCodeSize))) {
        field_abs =
            REX_LOAD_U32(self + kVsCodeAllocAt) + REX_LOAD_U32(info + kVsInfoCodeOffset);
        field_len = REX_LOAD_U32(info + kVsInfoCodeSize);
      }
    }
  }
  // Trim the captured window to the real program. Without this the ALU
  // interpreter and the fetch decoder are handed everything to the end of the
  // capture — 256 dwords past dest — while measured programs run 24 to 174, so
  // a walk that does not stop on its own continues into the next shader in the
  // ring.
  if (pc.resolved && field_len && (field_len & 3) == 0) {
    const size_t want = pc.code_off + field_len / 4;
    if (want >= 8 && want <= pc.code.size()) {
      pc.code.resize(want);
      pc.code_len_dwords = field_len / 4;
    } else {
      ++g_vsWindowLenRejected;
    }
  }

  if (pc.resolved && REXCVAR_GET(hle_capture)) {
    const uint32_t search_abs = start + pc.code_off * 4;
    // Two independent questions, kept apart because they have different
    // answers. Where the CF starts: dest, in 24 of 24 measured. Whether the
    // shader object's own allocation is the buffer that was patched: only
    // sometimes — 16 of 24 — so the field is NOT a drop-in source of code.
    if (search_abs == dest) ++g_vsWindowAtDest;
    else if (search_abs < dest) ++g_vsWindowEarly;
    else ++g_vsWindowLate;
    if (!field_abs) ++g_vsWindowNoField;
    else if (field_abs == dest) ++g_vsWindowAgree;
    else ++g_vsWindowDisagree;
    static std::map<uint64_t, bool> s_logged;
    const uint64_t key = (uint64_t(self) << 32) | variant;
    if (field_abs && s_logged.size() < 24 && s_logged.emplace(key, true).second) {
      REXLOG_INFO(
          "d3d9: vs 0x{:08X} v{} window: search 0x{:08X} (off {}), field "
          "0x{:08X} len {} dwords, dest 0x{:08X} — {}",
          self, variant, search_abs, int32_t(pc.code_off) - int32_t(kPatchWindowBack),
          field_abs, field_len / 4, dest,
          search_abs == dest ? (field_abs == dest ? "at-dest, same buffer"
                                                  : "at-dest, other buffer")
                             : "SEARCH OFF DEST");
    }
    const uint64_t seen = g_vsWindowAtDest + g_vsWindowEarly + g_vsWindowLate;
    if ((seen % 512) == 1) {
      REXLOG_INFO(
          "d3d9: vs code window: CF at dest {} early {} late {}; shader alloc "
          "is the patched buffer {} of {} (other buffer {}, unreadable {}); "
          "length rejected {}",
          g_vsWindowAtDest, g_vsWindowEarly, g_vsWindowLate, g_vsWindowAgree,
          seen, g_vsWindowDisagree, g_vsWindowNoField, g_vsWindowLenRejected);
    }
  }
  // A ring-window read is inherently transient: the destination may wrap or
  // be overwritten between the original call and this hook's copy. Never let
  // such a failed observation destroy a previously proven capture for the
  // same shader variant. This used to turn valid shaders back into
  // "no exact patched code" later in the frame.
  const bool same_variant_as_known = known && it->second.variant == variant;
  static uint64_t s_capture_attempts = 0;
  static uint64_t s_capture_resolved = 0;
  static uint64_t s_capture_preserved = 0;
  static uint64_t s_capture_invalidated = 0;
  ++s_capture_attempts;
  const bool capture_resolved = pc.resolved;
  if (capture_resolved) {
    g_patchedCode[self] = std::move(pc);
    // Only ever against a capture the decode already proved, so a match here
    // is evidence about the LAYOUT rather than about this one shader.
    ProbeShaderObjectCode(self, variant, g_patchedCode[self], base);
    ++s_capture_resolved;
  } else if (same_variant_as_known) {
    ++s_capture_preserved;
  } else {
    // A different variant is a different program. Refuse to use stale exact
    // code if the replacement could not itself be captured.
    if (known) {
      g_patchedCode.erase(it);
      ++s_capture_invalidated;
    }
  }
  if (s_capture_attempts <= 24 || (s_capture_attempts % 1000) == 0) {
    REXLOG_INFO(
        "d3d9: patched VS capture {} self=0x{:08X} bound=0x{:08X} "
        "variant={} fetches={} resolved={} totals resolved {} preserved {} "
        "invalidated {}",
        s_capture_attempts, self, DeviceState().vertex_shader, variant,
        expect_fetches, capture_resolved, s_capture_resolved, s_capture_preserved,
        s_capture_invalidated);
  }
}

// After the original ran: did it write what the rule predicts?
void CheckPatchedFetches(const std::vector<PatchPrediction>& pred,
                         uint8_t* base) {
  if (pred.empty()) return;
  ++g_patchProbed;
  for (const auto& p : pred) {
    ++g_patchFetches;
    if (p.bound) ++g_patchBound; else ++g_patchUnbound;
    if (!HostPageReadable(REX_RAW_ADDR(p.dest_addr)) ||
        !HostPageReadable(REX_RAW_ADDR(p.dest_addr + 8)))
      continue;
    uint32_t got[3] = {REX_LOAD_U32(p.dest_addr), REX_LOAD_U32(p.dest_addr + 4),
                       REX_LOAD_U32(p.dest_addr + 8)};
    for (uint32_t f = 0; f < kPatchFieldCount; ++f) {
      const uint32_t m = kPatchFieldMask[f];
      const uint32_t d = kPatchFieldDword[f];
      if ((got[d] & m) == (p.pred[d] & m)) ++g_pfAgree[f];
      else                                 ++g_pfDisagree[f];
    }
    for (uint32_t d = 0; d < 3; ++d) {
      if (got[d] == p.pred[d]) {
        ++g_patchAgree[d];
      } else {
        ++g_patchDisagree[d];
        // Keep the first disagreement in full. A count says the rule is wrong;
        // the two triples say which field of it is.
        if (!g_patchHaveMismatch) {
          g_patchHaveMismatch = true;
          for (uint32_t k = 0; k < 3; ++k) {
            g_patchFirstMismatch[k] = p.pred[k];
            g_patchFirstMismatch[3 + k] = got[k];
          }
        }
      }
    }
  }
}

void ReportPatchRule() {
  if (!g_patchProbed) {
    if (g_patchBadTable)
      REXLOG_INFO(
          "d3d9: pairing — nothing predicted; {} calls had an out-of-range "
          "table or element count, so the header offsets are wrong",
          g_patchBadTable);
    return;
  }
  REXLOG_INFO(
      "d3d9: pairing — {} patch calls, {} vfetch slots: {} bound to a "
      "declaration element by (usage, usage_index), {} unbound; {} calls "
      "rejected for a bad table",
      g_patchProbed, g_patchFetches, g_patchBound, g_patchUnbound,
      g_patchBadTable);
  for (uint32_t f = 0; f < kPatchFieldCount; ++f) {
    const uint64_t tot = g_pfAgree[f] + g_pfDisagree[f];
    REXLOG_INFO("d3d9: pairing — field {:<36} {} of {} agree ({}%)",
                kPatchFieldName[f], g_pfAgree[f], tot,
                tot ? (g_pfAgree[f] * 100) / tot : 0);
  }
  static const char* const kName[3] = {"dword0", "dword1", "dword2"};
  for (uint32_t d = 0; d < 3; ++d) {
    const uint64_t tot = g_patchAgree[d] + g_patchDisagree[d];
    REXLOG_INFO(
        "d3d9: pairing — whole {} : {} of {} ({}%) — stricter than the "
        "question; the unmodelled fields live here",
        kName[d], g_patchAgree[d], tot,
        tot ? (g_patchAgree[d] * 100) / tot : 0);
  }
  if (g_patchHaveMismatch) {
    REXLOG_INFO(
        "d3d9: pairing — first disagreement: predicted {:08X} {:08X} {:08X}, "
        "D3D9 wrote {:08X} {:08X} {:08X}",
        g_patchFirstMismatch[0], g_patchFirstMismatch[1],
        g_patchFirstMismatch[2], g_patchFirstMismatch[3],
        g_patchFirstMismatch[4], g_patchFirstMismatch[5]);
  }
  std::string u;
  for (const auto& [usage, n] : g_patchUsage) {
    const char* nm = mx::hle::UsageSemanticName(uint8_t(usage));
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%s:%llu ", nm ? nm : "?",
                  (unsigned long long)n);
    u += buf;
  }
  REXLOG_INFO("d3d9: pairing — bound semantics {}", u.empty() ? "none" : u);
  REXLOG_INFO(
      "d3d9: pairing — patch destination: {} calls wrote to SH_pPhysical+0x40 "
      "(what the draw-time probe reads), {} elsewhere in that allocation, {} "
      "somewhere else",
      g_destIsPhys40, g_destIsPhysOther, g_destElsewhere);
  if (g_destHaveSample) {
    REXLOG_INFO(
        "d3d9: pairing — first: shader 0x{:08X}, dest 0x{:08X}, SH_pPhysical "
        "0x{:08X}, dest-phys 0x{:X}",
        g_destSample[0], g_destSample[1], g_destSample[2], g_destSample[3]);
  }
}

void ReportCoverage(uint8_t* base) {
  const auto& st = DeviceState();
  if (g_drawsChecked == 0) {
    REXLOG_INFO("d3d9: hle — no draws scored");
    return;
  }
  REXLOG_INFO("d3d9: hle — {} of {} draws fully described ({}%)",
              g_drawsComplete, g_drawsChecked,
              (g_drawsComplete * 100) / g_drawsChecked);
  for (uint32_t g = 0; g < kDrawGapCount; ++g) {
    if (g_drawGaps[g]) {
      REXLOG_INFO("d3d9: hle   missing: {:<28} x{}", DrawGapName(g), g_drawGaps[g]);
    }
  }
  //-------------------------------------------------------------------------
  // Stage 2: what was actually built, and why the rest was not. Every skip is
  // named — a bare total cannot separate "the decoder refuses this format"
  // from "this stream is not indexed the way we model it", and those need
  // opposite fixes.
  //-------------------------------------------------------------------------
  {
    const uint64_t built = mx::hle::HleBuiltCount();
    const uint64_t* counts = mx::hle::HleSkipCounts();
    uint64_t attempted = built;
    for (uint32_t i = 1; i < uint32_t(mx::hle::HleSkip::kCount); ++i)
      attempted += counts[i];
    if (attempted) {
      REXLOG_INFO("d3d9: hle-render — {} of {} draws built ({}%)", built,
                  attempted, (built * 100) / attempted);
      for (uint32_t i = 1; i < uint32_t(mx::hle::HleSkip::kCount); ++i) {
        if (!counts[i]) continue;
        REXLOG_INFO("d3d9: hle-render   skipped: {:<34} x{}",
                    mx::hle::HleSkipName(mx::hle::HleSkip(i)), counts[i]);
      }
      std::string prims;
      for (uint32_t i = 0; i < 64; ++i) {
        if (!g_badPrimType[i]) continue;
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%u:%llu ", i,
                      (unsigned long long)g_badPrimType[i]);
        prims += buf;
      }
      if (!prims.empty())
        REXLOG_INFO("d3d9: hle-render   refused prim types (type:count) {}",
                    prims);
    }
    mx::hle::ReportHleTransform();
  }

  ReportPatchRule();

  //-------------------------------------------------------------------------
  // Stage 0 verdict.
  //-------------------------------------------------------------------------
  for (uint32_t s = 0; s < mx::hle::kMaxStreams; ++s) {
    const uint64_t fit = g_vbFitStream[s], fail = g_vbFailStream[s];
    if (!fit && !fail) continue;
    REXLOG_INFO(
        "d3d9: stage0  stream {}: holds the range {}/{} | mean draws since "
        "bind: fits {} fails {} (worst {})",
        s, fit, fit + fail, fit ? g_bindAgeFitSum[s] / fit : 0,
        fail ? g_bindAgeFailSum[s] / fail : 0, g_bindAgeFailMax[s]);
  }
  for (uint32_t s = 0; s < mx::hle::kMaxStreams; ++s) {
    if (!g_fileAgree[s] && !g_fileDiffer[s]) continue;
    REXLOG_INFO(
        "d3d9: stage0  stream {}: device fetch constant vs our snapshot — "
        "same {} differ {}, and the device's size explains {} of the failures",
        s, g_fileAgree[s], g_fileDiffer[s], g_fileRescues[s]);
  }
  REXLOG_INFO(
      "d3d9: stage0  indexed draws, by real max index: holds {}/{} (unread {})",
      g_idxRangeFits, g_idxRangeFits + g_idxRangeFails, g_idxRangeUnread);

  // Offsets that held the just-bound value on every one of the samples. One
  // surviving pair is the fetch constant file; none means D3D9 does not keep
  // the value verbatim and the snapshot is the only source available.
  if (g_fcPrimed) {
    uint32_t n0 = 0, n1 = 0;
    std::string o0, o1;
    for (uint32_t i = 0; i < kDeviceScanDwords; ++i) {
      char buf[16];
      if (g_fcCand0[i]) {
        ++n0;
        if (n0 <= 8) { std::snprintf(buf, sizeof(buf), "0x%X ", i * 4); o0 += buf; }
      }
      if (g_fcCand1[i]) {
        ++n1;
        if (n1 <= 8) { std::snprintf(buf, sizeof(buf), "0x%X ", i * 4); o1 += buf; }
      }
    }
    REXLOG_INFO(
        "d3d9: stage0  fetch constant file after {} samples (device readable to "
        "0x{:X}): dword0 offsets={} [{}] dword1 offsets={} [{}]",
        g_fcSamples, g_fcReached, n0, o0, n1, o1);
  } else {
    REXLOG_INFO("d3d9: stage0  fetch constant file: never sampled");
  }

  REXLOG_INFO(
      "d3d9: hle   stride exact={} padded={} TOO SMALL={} (too small means the "
      "layout decode is wrong)",
      g_strideOk, g_strideMismatch, g_strideTooSmall);
  REXLOG_INFO(
      "d3d9: hle   buffer holds the range: vb {}/{} ib {}/{} (denominator is "
      "draws checked for that buffer)",
      g_vbFits, g_vbFits + g_vbTooSmall, g_ibFits, g_ibFits + g_ibTooSmall);
  REXLOG_INFO(
      "d3d9: hle   vs=0x{:08X} ps=0x{:08X} ib=0x{:08X} ({} bit) vp={}x{} "
      "distinct devices={}",
      st.vertex_shader, st.pixel_shader, st.index.address,
      st.index.is_32bit ? 32 : 16, st.viewport.width, st.viewport.height,
      st.device_count);
  for (uint32_t i = 0; i < st.device_count; ++i) {
    std::string who;
    for (uint32_t e = 0; e < mx::hle::kEntryPointCount; ++e) {
      if (!(st.device_call_mask[i] & (1u << e))) continue;
      if (!who.empty()) who += " ";
      who += mx::hle::EntryPointName(e);
    }
    REXLOG_INFO("d3d9: hle   device 0x{:08X} x{} calls from: {}",
                st.device_ptr[i], st.device_calls[i], who);
  }
  for (uint32_t s = 0; s < mx::hle::kMaxStreams; ++s) {
    const auto& b = st.stream[s];
    if (!b.seen) continue;
    REXLOG_INFO(
        "d3d9: hle   stream {}: addr=0x{:08X} size={}B endian={} offset={} "
        "stride={}{}",
        s, b.address, b.size_bytes, b.endian, b.offset_bytes, b.stride,
        b.bound ? "" : " (unbound)");
  }
}

// The fully-resolved draw, written out for the first few of each kind so the
// description can be read and checked by eye rather than only counted. Goes to
// the non-rotating dump: these happen at load and the rotating log has already
// lost two probes this effort.
constexpr uint64_t kMaxHleDumped = 12;

void DumpHleDraw(bool indexed, uint64_t n, uint32_t prim, int32_t base_vertex,
                 uint32_t start, uint32_t count) {
  if (n > kMaxHleDumped) return;
  const auto& st = DeviceState();
  auto& f = DeclFile();

  f << "\nHleDraw " << (indexed ? "indexed" : "non-indexed") << " #" << n
    << " prim=" << prim << (indexed ? " base_vertex=" : " start_vertex=")
    << base_vertex;
  if (indexed) f << " start_index=" << start;
  f << (indexed ? " index_count=" : " vertex_count=") << count << "\n";

  const int id = st.current_decl;
  if (id < 0) {
    f << "  declaration: NONE\n";
  } else if (!g_declLayoutOk[id]) {
    f << "  declaration id " << id << ": DOES NOT DECODE ("
      << mx::hle::LayoutErrorText(g_declLayoutErr[id].reason) << ")\n";
  } else {
    const auto& layout = g_declLayout[id];
    f << "  declaration id " << id << ", " << layout.elements.size()
      << " element(s):\n";
    for (const auto& e : layout.elements) {
      f << "    " << e.semantic_name << e.semantic_index << " s" << e.stream
        << " off=" << e.offset << " size=" << e.size_bytes
        << " dxgi=" << static_cast<int>(e.format);
      if (e.unpack == mx::hle::Unpack::kSnorm2_10_10_10)
        f << " (shader unpacks snorm 2_10_10_10)";
      f << "\n";
    }
    for (uint32_t s = 0; s <= layout.max_stream; ++s) {
      const auto& b = st.stream[s];
      f << "    stream " << s << ": ";
      if (!b.seen) {
        f << "NEVER SET\n";
        continue;
      }
      f << "addr=0x" << std::hex << b.address << std::dec
        << " size=" << b.size_bytes << " offset=" << b.offset_bytes
        << " stride=" << b.stride << " (layout needs " << layout.min_stride[s]
        << ")" << (b.bound ? "" : " UNBOUND") << "\n";
    }
  }

  if (indexed) {
    f << "  index buffer: ";
    if (!st.index.seen || !st.index.bound) {
      f << "NONE\n";
    } else {
      f << "addr=0x" << std::hex << st.index.address << std::dec
        << " size=" << st.index.size_bytes << " "
        << (st.index.is_32bit ? 32 : 16) << "-bit\n";
    }
  }

  f << "  vs=0x" << std::hex << st.vertex_shader << " ps=0x" << st.pixel_shader
    << std::dec << (st.vs_seen ? "" : " (vs NEVER SET)")
    << (st.ps_seen ? "" : " (ps NEVER SET)") << "\n";
  f << "  viewport: ";
  if (!st.viewport.seen) {
    f << "NEVER SET\n";
  } else {
    f << st.viewport.x << "," << st.viewport.y << " " << st.viewport.width
      << "x" << st.viewport.height << " z=[" << st.viewport.min_z << ","
      << st.viewport.max_z << "]\n";
  }
  f << "  render state:";
  for (uint32_t r = 0; r < mx::hle::kRenderStateCount; ++r) {
    f << " " << mx::hle::RenderStateName(r) << "=";
    if (st.render_state.Seen(r)) {
      f << st.render_state.value[r];
    } else {
      f << "unset";
    }
  }
  f << "\n";
  f.flush();
}

// The two histograms the round exists to produce.
void ReportDeclHistogram() {
  uint64_t with = 0, without = 0;
  for (int i = 0; i < g_declCount; ++i) {
    (g_declHasColour[i] ? with : without) += g_declDraws[i];
  }
  REXLOG_INFO(
      "d3d9: decl-draws — {} declarations known; COLOUR={} NO-COLOUR={} "
      "unattributed={} patch_calls={}",
      g_declCount, with, without, g_drawsNoDecl, g_patchCalls);
  // The declaration now comes from device + 0x2ED8, per draw. These four say
  // whether that source is sound and how badly the old one lagged: `unknown`
  // must stay at 0 or the field is not what SetVertexDeclaration writes, and a
  // large `stale` is the 2508-calls-per-165000-draws problem, measured.
  REXLOG_INFO(
      "d3d9: decl-source — from device+0x2ED8: null={} unknown={} | vs the "
      "patch hook: same={} stale={}",
      g_declDeviceNull, g_declDeviceUnknown, g_declAgree, g_declDisagree);
  for (int i = 0; i < g_declCount; ++i) {
    REXLOG_INFO("d3d9: decl-draws   id={} ptr=0x{:08X} elems={} colour={} x{}",
                i, g_declPtr[i], g_declElems[i],
                g_declHasColour[i] ? "yes" : "no", g_declDraws[i]);
  }
}

// All three draw entry points report through here so the counters are always
// read together. A 150s run reaches 5000-10000 transcoded draws, so a coarser
// cadence than 2500 reports nothing at all — the first output-merger probe was
// lost to exactly that.
//
// DrawVerticesUP was added 2026-08-07. It had been unhooked since the start,
// so every draw total this project has ever quoted excluded it — including the
// Bink video composite, which is why no video ever reached the screen.
void ReportDrawCounts(uint8_t* base) {
  const uint64_t total = g_indexed_draws + g_draws + g_up_draws;
  if ((total % kDrawReportEvery) != 0) return;
  REXLOG_INFO("d3d9: draws — DrawIndexedVertices={} DrawVertices={} "
              "DrawVerticesUP={} total={}",
              g_indexed_draws, g_draws, g_up_draws, total);
  ReportDeclHistogram();
  if (REXCVAR_GET(hle_capture)) ReportCoverage(base);
}

}  // namespace mx::hooks::d3d9