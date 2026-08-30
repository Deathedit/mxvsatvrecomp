// D3D9 entry-point hooks -- observation only.
//
// The title statically links D3D9 v2.0.20209.3, so these functions are in the
// XEX but were nameless until an XDK d3d9.lib was matched against it by bytes
// (see "The D3D9 entry points are located" in AGENTS.md, and
// tools/match_d3d9.py). The control for that match was D3DDevice_Swap, whose
// COMDAT is 0x684 bytes -- the exact size of the already-confirmed
// sub_82566B58, and its pattern matched that address and nothing else.
//
// Why this is worth hooking at all: every colour round so far has inferred
// vertex layout from PM4 and shader microcode, because on Xenos a vfetch
// carries format and offset but not semantic. The semantics exist one layer
// up, in the D3DVERTEXELEMENT9 arrays the game hands to
// D3DDevice_CreateVertexDeclaration. They are not static data -- four different
// 12-byte D3DDECL_END sentinels return zero matches binary-wide -- so the only
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
#include <iterator>
#include <map>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
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

#include "gpu/guard_census.h"
#include "gpu/d3d9_draw.h"
#include "gpu/d3d9_layout.h"
#include "gpu/d3d9_texture.h"
#include "gpu/shader_ucode.h"   // DecodeVertexShaderFetches, VertexAttribute
#include "gpu/shader_alu.h"     // ExecuteVertexShader
#include "gpu/shader_hlsl.h"    // EmitShaderHlsl
#include <cmath>
#include "gpu/d3d9_state.h"
#include "gpu/hle_types.h"      // g_luminanceReadbackBits/Seq
#include "gpu/xenos_gpu_state.h"  // mx::gpu::alu -- the PM4 ALU constant file
#include "hooks/hooks_d3d9_shared.h"  // shared with hooks_d3d9_entry.cpp
#include "hooks/texture_dump.h"         // --texture_dump=true, logs/texdump

// Defined in src/app/graphics_system.cpp with the rest of the Debug cvars.

// A NAMED namespace, not an anonymous one, so that the guest entry points can
// move to their own translation unit and still reach the state they operate on.
// Everything here was internal-linkage until 2026-08-12 and is still private to
// the D3D9 HLE layer by convention -- the namespace is the boundary, and
// hooks_d3d9_internal.h is the only place that publishes anything out of it.
namespace mx::hooks::d3d9 {

namespace uc = rex::graphics::ucode;

using mx::hle::DeviceState;

// Declarations are built during load, and the rotating log (3 x 5MB) only
// retains the last ~50 seconds of a 165s run -- the first attempt at this probe
// logged every declaration and then lost all of them. Anything created early
// has to go somewhere that does not rotate, so this writes its own file into
// logs/decldump/, alongside the other dump directories. Opened with trunc, so
// unlike hlsldump and pm4dump it needs no wipe -- one run overwrites the last.
std::ofstream& DeclFile() {
  static std::ofstream f = [] {
    std::error_code ec;
    std::filesystem::create_directories("logs/decldump", ec);
    return std::ofstream("logs/decldump/decls.txt", std::ios::trunc);
  }();
  return f;
}

// D3DVERTEXELEMENT9 is *12 bytes on Xenon*, not the 8 of the PC struct. This
// is not an assumption: both D3DDevice_CreateVertexDeclaration (0x82550B80)
// and XGSetVertexDeclaration (0x82550A90) walk the array with `lhzu r9, 0xC`,
// and XGSetVertexDeclaration copies each element as three dwords. An earlier
// round searched the XEX for the 8-byte PC sentinel, which is one reason it
// found nothing.
//
// Stream is the halfword at offset 0 and terminates the array at 0xFF -- that
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
uint64_t g_indexed_up_draws = 0;
uint64_t g_indexed_up_skipped = 0;
uint64_t g_decls = 0;
uint64_t g_patchCalls = 0;

//---------------------------------------------------------------------------
// Finding the active vertex declaration at draw time.
//
// The draw entry points take D3DDevice* in r3 but not the declaration, so it
// has to be read off the device. D3DDevice_SetVertexDeclaration would be the
// obvious hook instead, but it is 20 bytes and under 128 bytes a byte match is
// not evidence -- hence reading the device.
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
// covered device + 0..0x2000, and 0x2ED8 is outside that -- the scans were
// under-scoped, not the conclusion sound. Scoping a scan by what the struct
// actually spans (SetStreamSource writes +0x3480) was the missing step both
// times, and reading the offset from the code that writes it makes the scan
// unnecessary altogether.
constexpr uint32_t kDeviceVertexDeclaration = 0x2ED8;

// Reading device + 0x2ED8 is safe in a way that dereferencing its *value* is
// not: the device pointer arrives as the draw's own r3, D3D9 is reading the
// same struct on either side of this hook, and the offset is well inside it.
// The value read is only ever compared against declarations we watched
// CreateVertexDeclaration build -- never followed.
int g_currentDecl = -1;

// What PatchVertexShaderToMatchVertexDeclaration last saw. Kept only to measure
// how far it lags: it fires on the lazy-state path, ~1 update per 66 draws, and
// the previous round mistook attribution-to-a-stale-value for attribution.
int g_patchDecl = -1;

uint64_t g_declDeviceNull = 0;      // field is 0 -- no declaration bound yet
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
// missing, never folded into one "incomplete" total -- a renderer built on a
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
// API, so agreeing is evidence that both were read correctly -- and this is the
// number the PM4 path had to *infer* the stride from.
uint64_t g_vbFits = 0;
uint64_t g_vbTooSmall = 0;
uint64_t g_ibFits = 0;
uint64_t g_ibTooSmall = 0;

//---------------------------------------------------------------------------
// Stage 0 -- why does the vertex range check fail?
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
//     `index % 4` is legal -- and would make the check, not the game, wrong.
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
// different binds leaves only offsets that track the binding. Comparison only --
// no value read out of the device is ever dereferenced.
//---------------------------------------------------------------------------

// **The scan asks the OS whether a page is readable instead of guessing where
// the struct ends.** Two guesses were tried and both faulted at guest
// 0x1D00B000: first 0x4000, then 0x3484 -- the latter chosen because
// SetStreamSource writes +0x3480, which proves that offset is mapped for *some*
// device and proves nothing about this one. The arena is sparse; a bound picked
// from a different object is not a bound.
//
// VirtualQuery per 4 KiB page costs one call per page per sample and removes
// the question entirely. The scan stops at the first page that is not
// committed and readable, so it reads exactly as far as memory exists.
constexpr uint32_t kDeviceScanBytes = 0x4000;
constexpr uint32_t kDeviceScanDwords = kDeviceScanBytes / 4;

// VirtualQuery here was ~100% of native frame time: 502 calls a frame costing
// 3082ms of a 3128ms MainLoop body, measured 2026-08-06. Note the shape -- it is
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
// How many draw reports may pass with NOTHING NEW before the three row-level
// dumps -- UP CALLERS' per-site rows, the per-config stencil lines and the
// per-declaration decl-draws rows -- print anyway.
//
// Measured over run mx_1781, 357 draw reports in 14.46 MB of log: UP CALLERS is
// 357 lines and 0.67 MB, averaging 1955 BYTES A LINE, and the per-config
// stencil rows are 5973 lines and 1.30 MB; decl-draws is 17,987 rows and
// 0.88 MB. Together 19.7% of the run, in a log whose segments rotate every
// ~30 seconds. Both are cumulative whole-population
// snapshots, so consecutive prints are identical apart from counter drift, and
// drift is not worth a log segment.
//
// (The first cut of this comment said 267 lines and 6.4%. It was wrong: the
// census key was truncated at 44 characters, so "52 distinct call sites" and
// "51 distinct call sites" landed in different buckets and only the largest was
// read. Normalise the digits out before counting line kinds.)
//
// NOT A BLIND MODULO, which would throw away the thing they are for. A new UP
// call site or a new stencil configuration prints IMMEDIATELY at any setting --
// the appearance of one is the entire diagnostic -- and the heartbeat covers
// only the case where the population has not moved. Neither report ever goes
// silent: when the rows are held back a one-line summary still prints, so
// "nothing new" and "not running" stay distinguishable. See the same
// change-or-heartbeat rule on RESOLVE CONSUMPTION below.
//
// 0 = only ever print rows on a change. 1 = every report, the old behaviour.
REXCVAR_DEFINE_INT32(d3d9_diag_row_heartbeat, 16, "Debug",
                     "Draw reports allowed to pass with an unchanged "
                     "population before UP CALLERS and the per-config stencil "
                     "rows dump anyway (0 = only on change, 1 = every report)");

namespace {

// Change-or-heartbeat, shared by every row dump in this file:
// UP CALLERS' per-site rows, the per-config stencil lines, the
// per-declaration decl-draws rows, STENCIL PLUMBED's key set, TEXTURE
// REPEATS' worst-offender list and the BACKFACE STENCIL WINDOW scan.
//
// `population` is whatever number grows when something NEW appears -- the
// distinct-site count, the distinct-config count, the declaration count. All
// are all add-only, so a change in the count is a faithful "there is
// something here you have not seen"; none can shrink and hide a replacement.
//
// The caller keeps its own `last` and `since`. Returns true when the rows
// should print, and leaves `since` counting reports that were held back so the
// summary line can say how many.
bool RowDumpDue(uint64_t population, uint64_t& last, uint32_t& since) {
  const int heartbeat = REXCVAR_GET(d3d9_diag_row_heartbeat);
  ++since;
  const bool changed = population != last;
  last = population;
  // heartbeat <= 0 disables the drift dump entirely, but never the change one.
  const bool due = changed || (heartbeat > 0 && since >= uint32_t(heartbeat));
  if (due) since = 0;
  return due;
}

}  // namespace

REXCVAR_DEFINE_BOOL(d3d9_page_cache_verify, false, "Debug",
                    "Verify every page-readability cache hit against a fresh "
                    "VirtualQuery and log mismatches. Slow; correctness check "
                    "for the region cache");

// Measured in real gameplay for the first time in mx_698: 33,043 calls a frame
// collapse to 39-79 VirtualQuery, so the cache works -- but those few cost
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
// Per-thread caches fix both at once -- no lock, no sharing, and each thread's
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
// cheap and very probably safe -- "probably" is why it is a flag and why the
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
                "d3d9: page cache MISMATCH #{} at {} -- cached {}, actual {} "
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
// depends on the index values. Reading them looked safe -- the index buffer
// "holds its range" 66,726/66,726 -- and it is **off**, because it faults.
//
// Three runs took an access violation at guest 0x1D00B000, and a VirtualQuery
// guard on the device scan did not stop it, which is what identified this read
// rather than that one as the source.
//
// The reason is almost certainly the address decode: SetIndices records
// `address = REX_LOAD_U32(buffer + 0x18) & 0x1FFFFFFF`, and that mask is the
// same one already found wrong for vertex buffers -- it clears the top three
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
  // Anything past where this sample could read is not a candidate -- leaving it
  // set would let an offset survive on samples that never actually checked it.
  for (uint32_t i = reached / 4; i < kDeviceScanDwords; ++i) {
    g_fcCand0[i] = false;
    g_fcCand1[i] = false;
  }
  if (reached < g_fcReached) g_fcReached = reached;

  g_fcPrimed = true;
  ++g_fcSamples;

  // The scan pinned dword1 to exactly one offset, 0x77C, and that retro-fits
  // SetStreamSource's own arithmetic: `subfic r11, r4, 0x11` -- which a first
  // reading dismissed as dead -- gives (0x11 - stream) * 8 + 0x6F4 = 0x77C for
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

// The DEVICE's own vertex fetch SIZE for a stream, preferred over the size we
// snapshotted from the D3DVertexBuffer header at SetStreamSource.
//
// Xenia takes the vertex buffer window from this register file --
// `regs.GetVertexFetch(vfetch_index)`, d3d12_command_processor.cc:3065 -- and
// never consults a buffer object. Measured with --hle_capture, the two sources
// disagree on 29.7% of stream-0 draws (same 601476 / differ 254455), and
// stream 0 is the only stream that ever zero-fills or loses a GPU-fetch
// region; stream 1 agrees 52677/52677 and never fails.
//
// ONLY the size is taken, and that is deliberate. Every dropped region is a
// SIZE failure -- `offset + first_vertex * stride` past the end -- and never an
// address failure, so the base is not needed to fix one. A first cut also read
// a base from `off1 - 4`, which yielded CpuToGpu(snapshot) + 0x1000 on all
// 1,571,568 draws of run mx_1824: the same page skew for three different
// buffers, but Xenia's CpuToGpu is a plain `& 0x1FFFFFFF` with no such skew
// (xenos.h:1132), and a real one-page base error would garble every draw in the
// game rather than 30% of them. So `off1 - 4` is not dword0, and the address
// half of this file remains unlocated. dword1 is not in doubt: the sizes at
// 0x77C matched the snapshot exactly (33800, 67080, 24) in every sample where
// the two agreed.
//
// Substituting the size alone is consistent with the existing arithmetic. The
// agreeing 70% show the device's size IS the whole-buffer size measured from
// `address`, the same origin `size_bytes` uses, so
// `avail = size - (offset_bytes + first_vertex * stride)` keeps its meaning.
uint64_t g_fcCompared[mx::hle::kMaxStreams] = {};
uint64_t g_fcSizeDiffer[mx::hle::kMaxStreams] = {};
uint64_t g_fcSizeLarger = 0, g_fcSizeSmaller = 0;
uint64_t g_fcUnreadable = 0;
uint64_t g_fcBadType = 0;

void ApplyDeviceFetchConstant(mx::hle::HleStream& s,
                              const mx::hle::StreamBinding& b, uint32_t stream,
                              uint32_t device, uint8_t* base) {
  if (!device || !base) return;
  const uint32_t off1 = FetchFileDword1Offset(stream);
  if (!HostPageReadable(REX_RAW_ADDR(device + off1 - 4)) ||
      !HostPageReadable(REX_RAW_ADDR(device + off1))) {
    ++g_fcUnreadable;
    return;
  }
  // Type 3 is a vertex fetch. Xenia refuses anything else outright
  // (kInvalidVertex, behind --gpu_allow_invalid_fetch_constants). Here it means
  // the slot is not describing this stream, and the snapshot is a better answer
  // than a window read out of an unrelated constant.
  if ((REX_LOAD_U32(device + off1 - 4) & 0x3u) != 0x3u) {
    ++g_fcBadType;
    return;
  }
  const uint32_t d1 = REX_LOAD_U32(device + off1);
  const uint32_t size = ((d1 >> 2) & 0xFFFFFFu) * 4;
  if (!size) return;
  if (size != b.size_bytes) {
    ++g_fcSizeDiffer[stream];
    (size > b.size_bytes ? g_fcSizeLarger : g_fcSizeSmaller) += 1;
  }
  // NOT APPLIED, and this is the measurement that says why. Run mx_1826:
  // 137,087 of 1,118,181 draws disagree with the snapshot and the device's
  // size is SMALLER on every one of them -- larger 0. A smaller window can
  // only drop more regions, never rescue one that failed for being too small,
  // so the register file cannot be the explanation for the 12-13% of regions
  // that lose their stream. That kills the theory outright rather than
  // weakening it.
  //
  // "Smaller" is also just what an OffsetInBytes-tightened window looks like,
  // which `start = offset_bytes + first_vertex * stride` already accounts for.
  // The snapshot size was correct all along.
  //
  // Kept as a counter, not a behaviour: the comparison is what stops this
  // theory being re-proposed, and it costs two loads on a path that already
  // reads the device.
  (void)s;
  ++g_fcCompared[stream];
}

//---------------------------------------------------------------------------
// Stage 3 -- the vertex shader float constant file.
//
// Read out of D3DDevice_SetVertexShaderConstantFN's own arithmetic
// (shader.obj, and 0x82550320 in the XEX), which is four instructions long
// before it starts storing:
//
//     addi   r10, r4, 0x78          ; StartRegister + 0x78
//     rlwinm r10, r10, 4, 0, 27     ; * 16 -- one vec4 per register
//     add    r10, r10, r3           ; + the device
//
// so register N lives at `device + 0x780 + N * 16`. The pixel-shader twin at
// 0x825503F8 is the same function with 0x178 in place of 0x78, giving 0x1780 --
// two 256-register files, 0x1000 bytes each, and they land exactly between the
// vertex fetch constants (which end at 0x780) and the declaration at 0x2ED8.
// Three independently-derived offsets tiling the struct with no overlap is what
// makes this a layout rather than three lucky guesses.
//
// **Not hooked, deliberately.** The device holds the live value whichever path
// wrote it -- including the state-block path in blocks.obj that bypasses every
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
// target** first -- the surface extent it reads from `0x24(r9)` bounds
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
// 9,130 of ~15,500 calls -- a full-surface reset -- and last-write-wins meant
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
// register with `sub_82564768(device, 0, 8576, device + 10528)` -- 8576 is
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
// Draws whose index-conditioning registers were readable, and how many had
// primitive restart enabled. If `read` is 0 the offsets are wrong and every
// index is unconditioned -- which is the state that lost the ground -- so this
// must not be allowed to sit silently at zero.
uint64_t g_indexCondRead = 0, g_indexCondResetOn = 0;
uint64_t g_vteSeen[4] = {};  // [0]=unreadable [1]=scale off [2]=scale on

// True when the GPU applies the viewport scale itself, meaning the vertex
// shader exported clip space. False -- including when the register cannot be
// read -- means the export is window space and needs the viewport inverse,
// which is the measured case for this game (PA_CL_VTE_CNTL = 0x300) and the
// safe default: it is what the code did unconditionally before.
// The VGT block at register 0x2100 is m_ValuesPacket, device+0x28CC -- the same
// packet convention kDeviceRegBlock2200 above uses for 0x2200, and the one the
// scissor read verified against IDA (0x28C0 is 0x2080, 0x2934 is 0x2200).
constexpr uint32_t kDeviceRegBlock2100 = 0x28CC;
constexpr uint32_t kDeviceVgt(uint32_t reg) {
  return kDeviceRegBlock2100 + (reg - 0x2100) * 4;
}
constexpr uint32_t kRegPaSuScModeCntl = 0x2205;
constexpr uint32_t kDevicePaSuScModeCntl =
    kDeviceRegBlock2200 + (kRegPaSuScModeCntl - 0x2200) * 4;

// Register numbers from register_table.inc:1262-1265 and 1304 -- note MAX comes
// BEFORE MIN, which is the opposite of the obvious guess. Field widths from
// registers.h:354/382/393 (24 bits each) and 480 (multi_prim_ib_ena, bit 21).
//
// Every field defaults to the inert value in HleDrawInputs, so a device whose
// pages cannot be read leaves the conditioning switched off rather than
// clamping every index to a bogus bound.
void ReadIndexConditioning(uint32_t device, uint8_t* base,
                           mx::hle::HleDrawInputs& in) {
  if (!device || !base) return;
  if (!HostPageReadable(REX_RAW_ADDR(device + kDeviceVgt(0x2100))) ||
      !HostPageReadable(REX_RAW_ADDR(device + kDeviceVgt(0x2103))) ||
      !HostPageReadable(REX_RAW_ADDR(device + kDevicePaSuScModeCntl)))
    return;
  in.index_max = REX_LOAD_U32(device + kDeviceVgt(0x2100)) & 0xFFFFFFu;
  in.index_min = REX_LOAD_U32(device + kDeviceVgt(0x2101)) & 0xFFFFFFu;
  in.index_offset = REX_LOAD_U32(device + kDeviceVgt(0x2102)) & 0xFFFFFFu;
  in.index_reset = REX_LOAD_U32(device + kDeviceVgt(0x2103)) & 0xFFFFFFu;
  in.index_reset_enabled =
      (REX_LOAD_U32(device + kDevicePaSuScModeCntl) & (1u << 21)) != 0;
  // A max of 0 would clamp the whole draw onto vertex 0 and erase the frame.
  // Treat it as "not set" rather than obeying it: the register is 0xFFFF or
  // 0xFFFFFF in every sane state (registers.h:392), so 0 means we read the
  // wrong dword and the safe response is to leave the clamp inert.
  if (in.index_max == 0) {
    in.index_max = 0xFFFFFFu;
    in.index_min = 0;
  }
  if (in.index_min > in.index_max) in.index_min = 0;
  ++g_indexCondRead;
  if (in.index_reset_enabled) ++g_indexCondResetOn;
  // The values themselves, not just that they were read. Clamping to a bound
  // nobody has looked at is how the terrain got erased a second time: a small
  // max_indx squashes an entire draw onto one vertex and the frame goes blank
  // with the draw count unchanged, which reads like "the geometry collapsed"
  // and not like "a register said so". Sampled, capped, distinct values only.
  {
    static std::map<uint64_t, uint64_t> s_seen;
    const uint64_t key = (uint64_t(in.index_max) << 40) ^
                         (uint64_t(in.index_min) << 16) ^ in.index_offset;
    auto it = s_seen.find(key);
    if (it == s_seen.end() && s_seen.size() < 8) {
      s_seen.emplace(key, 1);
      REXLOG_INFO(
          "d3d9: index conditioning regs: max {} min {} offset {} reset 0x{:X} "
          "restart {} | this draw base_vertex {}",
          in.index_max, in.index_min, in.index_offset, in.index_reset,
          in.index_reset_enabled ? "on" : "off", in.base_vertex);
    }
  }
}

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
// when the device cannot be read, counting which was used -- a silent fallback
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
// Stage 2 -- build a renderable draw from the description.
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

// CHECKED AND MEASURED, do not re-investigate: the vertex shader is NOT
// mis-attributed across threads.
//
// The pixel half above was moved off DeviceState() because that is `static
// thread_local` and this title submits draws from worker threads. The same
// treatment was built for the vertex shader, on the theory that a draw could
// take its pixel shader from the device while taking its vertex shader from
// whatever that thread last set, pairing two stages from different materials.
//
// It was instrumented and the answer was zero: across two sessions and 2.16M
// draws, the per-device record and the thread-local field NEVER disagreed,
// and the thread-local fallback was never even reached. The machinery was
// removed rather than left in, because it cost a mutex-guarded map lookup
// twice per draw to reproduce a value the existing field already had.
//
// The mis-paired stages that prompted this are real, but the cause is the
// translation cache being keyed on a recycled ADDRESS -- see
// g_hlslReportedVs.
// Addresses observed carrying a DIFFERENT shader than the one translated for
// them. Zero means handles are never recycled and that fix is inert.
std::atomic<uint64_t> g_shaderHandleRecycled{0};

std::string ShaderTranslationSummary() {
  return fmt::format(
      "shader handles RECYCLED onto different microcode: {}",
      g_shaderHandleRecycled.load());
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
ResolveAddressCensus g_resolveAddr;

// Whether a resolve destination is worth sampling as a snapshot at all.
//
// Extent agreeing, or the object being named by a resolve, is not the same as
// the GPU having WRITTEN the surface. A destination the resolves reach only a
// corner of is mostly clear colour, so this refuses the claim and lets the CPU
// decode run instead -- which keeps alive the re-read that picks the texture up
// once the guest fills its memory.
//
// STALE UNTIL 2026-08-14: this used to justify itself with "it paints the same
// black AND suppresses the re-read". Both halves are wrong for a surface the CPU
// never writes. The terrain heightmap is one: guest memory for it decodes to a
// uniform 0xFF, so refusing the claim paints WHITE, not black, and the re-read
// this protects never arrives -- zero `RECOVERED` lines across a whole freeroam
// run (mx_1147). Returning false here is still correct; what was missing is the
// downstream fallback, which now treats a uniform decode as empty whenever a
// partly-written snapshot exists. See the `decode_is_uniform` note at the bind.
//
// A quarter of the area is the threshold. A genuine render target is resolved
// whole, or in full-width bands that reach the full extent between them, so
// nothing legitimate sits near it -- while the 2048x2048 menu atlas that
// motivated this reaches 256x256, one sixty-fourth. The terrain heightmap at
// phys 0x1A2E3000 reaches 768x256 of 2048x2048, 4.7%, and is the case the
// fallback exists for.
//
// AREA, NOT A BOUNDING BOX, since 2026-08-29. This used to compare
// reached_x * reached_y against the extent, and for every destination above
// that is the same number -- a whole-surface resolve and a full-width band
// both have a box equal to their coverage. It is only wrong for a SCATTER,
// and the terrain deformation buffer is one: 39 resolves of 128x32 over
// 2048x2048 is 3.8% covered with a 1152x1056 box, 29.0%. It passed, the
// snapshot was claimed, and the untouched 96% sampled 0 in place of the
// neutral 0x80 the guest had written -- every terrain tile 512/255 = 2.008
// world units low, which is the floating bike.
//
// The box also only ever GROWS, so the destination was refused early in a
// run and claimed permanently once enough scattered blits had stretched it
// past a quarter. That is why the defect looked intermittent, and it is
// observable: in mx_1750 phys 0x1A2E3000 froze at part1007 while snap ran
// 225 -> 750, mid-run, without the guest changing anything.
//
// Unknown coverage allows the claim: a destination whose fetch constant could
// not be read has no entry, and refusing on absent evidence would undo the
// Phase 2 rescue for every surface this measurement missed.
bool ResolvedDestinationIsMostlyWritten(uint32_t dest_object) {
  const auto po = g_resolveDestObjectPhys.find(dest_object);
  if (po == g_resolveDestObjectPhys.end()) return true;
  const auto it = g_resolvedTargetsByAddress.find(po->second);
  if (it == g_resolvedTargetsByAddress.end()) return true;
  // REAL coverage, not the bounding box. See the MarkCoverage note on
  // ResolvedTargetByAddress for why the box had to go: a scatter of small
  // blits stretches it across most of the surface while covering almost none
  // of it, and that is what claimed the terrain deformation buffer and sank
  // the ground 2.008 units.
  const uint32_t total = it->second.total_cells();
  if (!total) return true;  // extent never learned -- unknown, not empty
  if (it->second.covered_cells * 4 >= total) return true;
  ++g_resolveAddr.partial;
  static std::set<uint32_t> s_logged;
  if (s_logged.insert(dest_object).second && s_logged.size() <= 8) {
    // Both numbers, always. The bounding box alone is what made this
    // decision unreadable for a session: 1152x1056 of 2048x2048 looks like a
    // surface that is a third written, and it was 3.8% written.
    REXLOG_INFO("d3d9: resolve dest 0x{:08X} phys 0x{:08X} {}x{} covered only "
                "{}% ({} of {} cells, box {}x{}) over {} resolves -- not "
                "claimed, CPU decode keeps it",
                dest_object, po->second, it->second.width, it->second.height,
                it->second.coverage_percent(), it->second.covered_cells,
                total, it->second.reached_x, it->second.reached_y,
                it->second.resolves);
  }
  return false;
}

// Which guest threads actually build draws. The companion to
// ResolvedTargetByAddress::last_bind_thread: a bind on a thread that never
// appears here can never be seen by a draw, because DeviceState() is
// thread_local. Fixed array with atomics rather than a set, because this is
// written from every record worker and a std::set insert would race.
std::atomic<uint32_t> g_drawThreadIds[8];
void NoteDrawThread() {
  const uint32_t tid = GetCurrentThreadId();
  for (auto& slot : g_drawThreadIds) {
    uint32_t cur = slot.load(std::memory_order_relaxed);
    if (cur == tid) return;
    if (cur == 0 && slot.compare_exchange_strong(cur, tid)) return;
  }
}

// The consumption record for a resolve destination, by its texture object, or
// null when this object never resolved anywhere. find() only: this runs on
// guest draw threads and must not insert into a map the resolve path is
// writing.
ResolvedTargetByAddress* ResolveEntryForObject(uint32_t dest_object) {
  const auto po = g_resolveDestObjectPhys.find(dest_object);
  if (po == g_resolveDestObjectPhys.end()) return nullptr;
  const auto it = g_resolvedTargetsByAddress.find(po->second);
  return it == g_resolvedTargetsByAddress.end() ? nullptr : &it->second;
}

// ---- Video render-target consumption --------------------------------------
//
// See the block comment in hooks_d3d9_internal.h for what this measures and
// why the RESOLVE CONSUMPTION census cannot answer it.

struct VideoShapeRow {
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t last_object = 0;
  uint64_t binds = 0;
  uint32_t sampler_mask = 0;
  uint32_t last_device = 0;
  uint32_t last_thread = 0;
  // Guest Draw calls issued while it sat on a sampler, and how many windows.
  // set_texture_binds alone conflates sampling with the resolve idiom -- bind,
  // resolve, unbind, no draw between -- and reading it as "the guest asked to
  // sample it" cost five rounds of instrumenting the draw path. Never report
  // binds without this beside it.
  uint64_t guest_draws_spanned = 0;
  uint64_t bind_windows = 0;
  // Reached the draw path's slot loop as a shader-declared sampler. binds > 0
  // with slot_seen == 0 means the bind never became a draw slot.
  uint64_t slot_seen = 0;
};

std::mutex g_videoShapeMu;
std::map<uint32_t, VideoShapeRow> g_videoShapeRows;  // keyed by base address
uint64_t g_videoShapeBinds = 0;      // binds at a video shape, before dedupe
uint64_t g_videoShapeDropped = 0;    // rows refused because the map was full

constexpr size_t kMaxVideoShapeRows = 64;

// The three _VideoRenderTarget assets declared in MXUI.xenon.database, by
// extent. Taken from the shipped data, not guessed: 1280x430 is FE_Smoke's,
// and matches the 1280x430 the engine's own texture-header hook reports.
bool IsVideoTargetShape(uint32_t w, uint32_t h) {
  return (w == 1280 && h == 720) ||    // 1280_720_VideoRenderTarget
         (w == 1024 && h == 512) ||    // 1024_512_VideoRenderTarget
         (w == 1280 && h == 430);      // Smoke_VideoRenderTarget
}

// Extent and base address straight off the bound fetch constants. Deliberately
// NOT DescribeHleTexture2D: this must observe what the guest bound even when
// the describe path would refuse it, or it inherits that path's blind spots.
bool DecodeVideoShape(const uint32_t* fetch, bool fetch_valid, uint32_t* w,
                      uint32_t* h, uint32_t* base) {
  if (!fetch || !fetch_valid) return false;
  namespace xe = rex::graphics::xenos;
  xe::xe_gpu_texture_fetch_t f{};
  std::memcpy(&f, fetch, sizeof(uint32_t) * 6);
  if (f.dimension != xe::DataDimension::k2DOrStacked) return false;
  *w = f.size_2d.width + 1;
  *h = f.size_2d.height + 1;
  *base = f.base_address << 12;
  return true;
}

void NoteVideoShapeBind(uint32_t sampler, uint32_t object, const uint32_t* fetch,
                        bool fetch_valid, uint32_t device) {
  if (sampler >= mx::hle::kMaxSamplers) return;
  // The window this sampler was already holding, closed here because this is
  // the only moment it is known to have ended.
  struct Watch {
    uint32_t base = 0;
    uint64_t draws_at_bind = 0;
  };
  static thread_local Watch t_watch[mx::hle::kMaxSamplers];
  Watch& w = t_watch[sampler];

  uint32_t width = 0, height = 0, base_addr = 0;
  const bool matched =
      object && DecodeVideoShape(fetch, fetch_valid, &width, &height, &base_addr) &&
      IsVideoTargetShape(width, height);

  if (w.base && (!matched || w.base != base_addr)) {
    const uint64_t spanned = mx::hle::D3D9DrawCounter() - w.draws_at_bind;
    std::lock_guard<std::mutex> lk(g_videoShapeMu);
    if (const auto it = g_videoShapeRows.find(w.base);
        it != g_videoShapeRows.end()) {
      it->second.guest_draws_spanned += spanned;
      ++it->second.bind_windows;
    }
    w.base = 0;
  }
  if (!matched) return;

  {
    std::lock_guard<std::mutex> lk(g_videoShapeMu);
    ++g_videoShapeBinds;
    auto it = g_videoShapeRows.find(base_addr);
    if (it == g_videoShapeRows.end()) {
      if (g_videoShapeRows.size() >= kMaxVideoShapeRows) {
        ++g_videoShapeDropped;
        return;
      }
      it = g_videoShapeRows.emplace(base_addr, VideoShapeRow{}).first;
    }
    VideoShapeRow& row = it->second;
    row.width = width;
    row.height = height;
    row.last_object = object;
    ++row.binds;
    row.sampler_mask |= 1u << sampler;
    row.last_device = device;
    row.last_thread = GetCurrentThreadId();
  }
  if (!w.base) {
    w.base = base_addr;
    w.draws_at_bind = mx::hle::D3D9DrawCounter();
  }
}

void NoteVideoShapeSlot(const uint32_t* fetch, bool fetch_valid) {
  uint32_t width = 0, height = 0, base_addr = 0;
  if (!DecodeVideoShape(fetch, fetch_valid, &width, &height, &base_addr)) return;
  if (!IsVideoTargetShape(width, height)) return;
  std::lock_guard<std::mutex> lk(g_videoShapeMu);
  // find() only -- a slot must not create a row. A row that exists only because
  // the draw path saw it, with binds == 0, would be a contradiction that reads
  // like data.
  if (const auto it = g_videoShapeRows.find(base_addr);
      it != g_videoShapeRows.end())
    ++it->second.slot_seen;
}

// The destination texture object whose snapshot covers this described texture,
// or 0. Matches on the guest memory address the two fetch constants agree on,
// and refuses when the extents disagree -- an address the guest allocator has
// recycled describes a different texture, and inheriting the old snapshot would
// swap a black surface for a confidently wrong one.
// DOES THE GPU EVER WRITE THIS RANGE? Answers with a denominator.
//
// On Xenos a render target lives in EDRAM. The ONLY way GPU output reaches
// guest memory is a RESOLVE. So "no resolve overlaps this range" is not a hint
// -- it is proof the GPU never wrote these bytes, and a texture that reads as
// uniform in guest memory really is uniform rather than a surface we failed to
// claim. That distinction is the whole question for the terrain's
// virtual-texture page table, which is uniform in memory while the tile atlas
// beside it streams correctly.
//
// ResolvedTargetForAddress cannot answer it. That function needs an EXACT base
// match AND equal extents AND mostly-written coverage, and returns nullptr for
// all three failures, every one of which prints `resolved=0`. So the flag we
// already had cannot separate "nothing resolves here" from "something resolves
// here and we declined to claim it" -- the same collapse as a reason-code chain
// whose branches share an outcome.
//
// This overlaps RANGES at any extent and any offset, which is also the shape an
// atlas built from small sub-rect resolves actually has. Destination byte size
// is not recorded, so rather than guess a bytes-per-pixel and risk a fabricated
// overlap, the test is stated in terms this data supports exactly:
//
//   exact   - a resolve destination starts precisely at this address
//   inside  - resolve destinations whose base falls WITHIN this range, i.e.
//             the range is being filled piecewise (the atlas pattern)
//   below   - nearest destination base below this address, with the delta, so
//             a range that sits INSIDE a larger destination is still visible
//
// `total` is the denominator: without it a zero cannot be told from a registry
// that was never populated.
ResolveRangeProbe ProbeResolveRange(uint32_t address, uint32_t bytes) {
  ResolveRangeProbe p;
  if (!address) return p;
  const uint32_t base = GpuPhysicalAddress(address);
  const uint32_t end = bytes ? base + bytes : base + 1u;
  p.total = uint32_t(g_resolvedTargetsByAddress.size());
  for (const auto& [addr, e] : g_resolvedTargetsByAddress) {
    if (addr == base) {
      p.exact = 1;
      p.first_width = e.width;
      p.first_height = e.height;
      p.first_addr = addr;
    } else if (addr > base && addr < end) {
      if (!p.inside) {
        p.first_addr = addr;
        p.first_width = e.width;
        p.first_height = e.height;
      }
      ++p.inside;
    } else if (addr < base) {
      const uint32_t delta = base - addr;
      if (!p.below_addr || delta < p.below_delta) {
        p.below_addr = addr;
        p.below_delta = delta;
        p.below_width = e.width;
        p.below_height = e.height;
      }
    }
  }
  return p;
}

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
    ++g_resolveAddr.extentMiss;
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
// Draws whose vertex shader indexes some stream by a register it computed.
// The denominator for HleComputedIndexSkips: a zero skip count means
// "the CPU path never touched such a stream" only if this is non-zero.
uint64_t g_computedIndexDraws = 0;

// Are the PER-DRAW diagnostics on?
//
// This session's investigation left a lot of measurement in the hot path, and
// some of it is not cheap: the Stage-3 transform probe alone reads 256 guest
// dwords and scores every vertex, FOR EVERY DRAW, purely to log a ranking
// nothing acts on. Per-FRAME reporting (FRAME COST, the periodic summaries) is
// negligible and stays on unconditionally -- only work proportional to draws or
// vertices is gated here.
//
// Default OFF, so a plain run is the fast one and `--hle_diag=1` is what you
// pass to get the counters back. That also makes the cost of the instrumentation
// itself an A/B rather than a rebuild.
// Read once per frame rather than per draw: the cvar lookup is itself the sort
// of per-draw cost this exists to remove. Declared beside the other cvars at
// the top of the file -- a REXCVAR_DECLARE inside this anonymous namespace looks
// for namespace-local storage and does not link.
bool g_diag = false;
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

BinkPlaneRefusals BinkPlaneRefusalStats();
bool CopyTexturePhysical(const mx::hle::HleTextureSource& source, uint8_t* base,
                         std::vector<uint8_t>& out);


std::vector<PendingHleDraw> g_pendingHleDraws;
// Defined next to FinalizePendingD3D9DrawsImpl, called from both push sites.
void NoteQueueThread(uint32_t thread, bool is_resolve);
void NoteResolvePosition(uint32_t dest, size_t index);
uint64_t g_pendingQueued = 0, g_pendingApplied = 0, g_pendingDropped = 0;

// Draws that actually reached the frame's draw list, and draws refused at the
// last gate. Counted in FinishHleDraw, which is where a built draw becomes a
// draw the renderer will issue.
//
// The FIRST version of the FRAME DRAWS line used g_pendingQueued for this and
// reported `queued 0` on a native run whose capture plainly contains 340 host
// draws. g_pendingQueued counts only the DEFERRED path -- draws with no shader
// code yet, waiting on this frame's PM4 packets -- and on a normal frame that
// is legitimately zero. It looked like the queue point because it sits at a
// push_back; it is the wrong population.
uint64_t g_hleDrawsAccepted = 0, g_hleDrawsRefused = 0;
// WHERE THE REST GO. FRAME DRAWS reports `guest`, `accepted` and `refused`, and
// its own comment names the case those three cannot explain:
//
//     guest >  accepted + refused    draws vanish before BuildAndQueueDraw
//
// A level run sits there permanently -- cumulative guest 234,174 against
// accepted 219,328 with refused 0, a gap of 14,846 (6.9%) that no counter
// attributes. BuildAndQueueDraw has three exits and only one of them was
// counted at all:
//
//   BuildHleDraw skip     counted, but only PRINTED under --hle_capture, so it
//                         is invisible on every normal run
//   no viewport           counted NOWHERE except for Bink draws
//   shader not applied    counted NOWHERE unless the result was kNoCode
//
// These two close it. Reported unconditionally beside FRAME DRAWS, because a
// gap that needs a debug cvar to explain is a gap nobody explains.
uint64_t g_drawNoViewport = 0;
uint64_t g_drawShaderFailed = 0;   // ApplyShaderOutputs returned kFailed
uint64_t g_drawShaderNoCodeFull = 0;  // kNoCode, and the pending queue was full
constexpr size_t kMaxPendingHleDraws = 2048;

// Vertex shader object layout, read out of sub_82565928's VS branch at
// 0x82566234 and cross-checked against the patcher at 0x82564C50. The pixel
// shader twin is ps + 0x18 / ps + 0x40 / info + 0x28 / info + 0x2C (see
// CollectPixelShaderBlob).

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
// there and why the one-instruction compositor shaders -- MAD on c255 -- exported
// (0,0,0,0).
//
// Read from the shader object, not from the ring: the packet only carries an
// address, and that address is guest memory we can read directly. No PM4.
//
// Applied AFTER the device file so a shader literal wins for its own slots.
// That is the hardware order -- the load is emitted at draw time, after any
// SetVertexShaderConstantF the app made.
// `written`, when given, receives one byte per bank dword, non-zero where this
// overlay published a value -- the vertex twin of ApplyShaderLoadTable's param.
// Its only consumer was the reverted vs-c32 fill, and it is kept because the
// distinction it draws is the one any future fill here needs first: a shader
// that writes a deliberate ZERO and a slot nothing ever wrote are the same bits
// and the opposite decision.
void OverlayShaderConstants(uint32_t shader, uint8_t* base,
                            std::array<uint32_t, kD3d9ConstRegs * 4>& out,
                            std::array<uint8_t, kD3d9ConstRegs * 4>* written =
                                nullptr) {
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
      if (written) (*written)[reg * 4 + i] = 1;
    }
    ++g_shaderConstOverlays;
  }
}

// PROBE: which VERTEX constant registers arrive as NaN, and whether any of them
// is ever finite.
//
// The legal screen's white backdrop traces to vertex c136-c139 reading NaN at
// draw 1010 of legal-2.rdc: the VS computes o3 = r0.y*c137 + c139 + r0.x*c136,
// so the interpolator arrives NaN and the pixel shader saturates to (1,1,1,1).
// That white is then resolved into three snapshots which the backdrop shader
// samples, and the whole HDR chain downstream is a constant from there.
//
// IDA cannot answer who writes those registers: the setter at 0x82550320 has
// 20+ callers and no call site passes a literal StartRegister, so the index is
// computed. The question this probe settles instead is much narrower and is the
// one that decides the next move:
//
//   ever_finite == 0  -> the guest never writes the register at all, and the
//                        shader is reading a slot only the hardware constant
//                        file would have held from an earlier frame
//   ever_finite  > 0  -> the guest does write it and we are sampling a window
//                        where it is stale, which is OUR bug to fix
//
// Read AFTER OverlayShaderConstants so it reports the bank the shader actually
// sees, not the raw device shadow -- the overlay is exactly the thing that could
// be filling these and being missed.
// NARROWED 2026-08-16, and the old shape is why this probe was misleading.
//
// It used to tally NaN per register over EVERY draw, which made its own stated
// rule unsound. `c136=10977nan/9023ok` was read as "ever_finite > 0, therefore
// the guest writes it and we are sampling stale" -- but the denominator included
// every draw whose shader never reads c136 at all, and OverlayShaderConstants
// writes shader literals into the same bank, so an unrelated shader publishing
// c136 scores as "the guest writes it". Two opposite defects, one number.
//
// Keyed by SHADER now, and a register is only counted for a shader that can
// actually read it (`r <= max_const_index`, the highest constant the translated
// microcode references). `max_const_index` is a BOUND, not a read set, so a
// shader can still be charged for a register it happens not to touch -- but it
// can never be charged for one it provably cannot touch, which is the half that
// was wrong before.
//
// The question this now answers, for the one VS that computes
// `o3 = r0.y*c137 + c139 + r0.x*c136`:
//
//   that shader's c136 never finite -> nothing publishes it; hunt the writer,
//                                      and indirect buffers are the last place
//                                      left to look (the PM4 parser knows
//                                      INDIRECT_BUFFER by name and never
//                                      follows one)
//   that shader's c136 sometimes finite -> real staleness in our per-draw
//                                          rebuild; the fix is ordering
// `before` is one bit per register, set if that register held a NaN in any
// component BEFORE OverlayShaderConstants ran. It turns this from "is the bank
// NaN" into "which side made it NaN", which is the question the per-shader
// spread raised: c136 is 100% finite for vs 0x2160DD20 and 0% finite for
// vs 0x216066A0, and the only per-shader step between the device file and here
// is the shader's own literal overlay.
void NoteVertexConstantNaN(const std::array<uint32_t, kD3d9ConstRegs * 4>& bank,
                           uint32_t shader,
                           const std::array<uint32_t, kD3d9ConstRegs / 32>& before) {
  struct PerShader {
    uint64_t nan[kD3d9ConstRegs] = {};
    uint64_t finite[kD3d9ConstRegs] = {};
    // Transitions across OverlayShaderConstants. `f2n` is the one that would
    // mean the overlay is the corruption rather than the cure.
    uint64_t n2n[kD3d9ConstRegs] = {};
    uint64_t n2f[kD3d9ConstRegs] = {};
    uint64_t f2n[kD3d9ConstRegs] = {};
    uint64_t draws = 0;
    uint32_t max_const = 0;
  };
  static std::mutex s_mu;
  static std::map<uint32_t, PerShader> s_byShader;
  static uint64_t s_draws = 0;
  // A NaN is exponent all-ones with a non-zero mantissa. Testing the bits keeps
  // this independent of the host's floating-point flags.
  const auto is_nan = [](uint32_t b) {
    return (b & 0x7F800000u) == 0x7F800000u && (b & 0x007FFFFFu) != 0;
  };
  // Read outside the lock -- TranslatedVertexShader takes its own.
  const TranslatedShader* ts = shader ? TranslatedVertexShader(shader) : nullptr;
  const uint32_t max_const = ts ? ts->max_const_index : 0;

  std::string report;
  {
    std::lock_guard<std::mutex> lock(s_mu);
    auto& e = s_byShader[shader];
    ++e.draws;
    // Untranslated shaders report 0 here; keep the largest ever seen so a draw
    // taken before translation finished cannot shrink the gate.
    if (max_const > e.max_const) e.max_const = max_const;
    const uint32_t limit = std::min<uint32_t>(e.max_const + 1, kD3d9ConstRegs);
    for (uint32_t r = 0; r < limit; ++r) {
      bool nan = false;
      for (uint32_t c = 0; c < 4; ++c) nan = nan || is_nan(bank[r * 4 + c]);
      ++(nan ? e.nan[r] : e.finite[r]);
      const bool was = (before[r >> 5] >> (r & 31)) & 1u;
      if (was && nan) ++e.n2n[r];
      else if (was && !nan) ++e.n2f[r];
      else if (!was && nan) ++e.f2n[r];
    }
    if ((++s_draws % 20000) != 0) return;
    for (const auto& [h, s] : s_byShader) {
      std::string regs;
      for (uint32_t r = 0; r <= s.max_const && r < kD3d9ConstRegs; ++r) {
        if (!s.nan[r] && !s.f2n[r] && !s.n2f[r]) continue;
        // NN = NaN on both sides (neither source has it)
        // NF = the overlay REPAIRED it (device file was the problem)
        // FN = the overlay CORRUPTED it (the overlay is the bug)
        regs += fmt::format(" c{}={}nan/{}ok[{}NN {}NF {}FN]", r, s.nan[r],
                            s.finite[r], s.n2n[r], s.n2f[r], s.f2n[r]);
      }
      if (regs.empty()) continue;
      report += fmt::format("\n  vs 0x{:08X} maxc c{} over {} draws:{}", h,
                            s.max_const, s.draws, regs);
    }
  }
  if (report.empty()) {
    REXLOG_INFO("d3d9: VS CONST NaN probe over {} draws: none", s_draws);
    return;
  }
  REXLOG_INFO("d3d9: VS CONST NaN probe over {} draws, per shader, counting "
              "only registers the shader can reach:{}",
              s_draws, report);
}

bool CaptureVertexConstants(uint32_t device, uint8_t* base, uint32_t shader,
                            std::array<uint32_t, kD3d9ConstRegs * 4>& out) {
  const uint32_t bytes = kD3d9ConstRegs * 16;
  if (!device || !HostPageReadable(REX_RAW_ADDR(device + 0x780)) ||
      !HostPageReadable(REX_RAW_ADDR(device + 0x780 + bytes - 4)))
    return false;
  for (uint32_t i = 0; i < out.size(); ++i)
    out[i] = REX_LOAD_U32(device + 0x780 + i * 4);
  // Repair registers the device file never held, from the ALU constant file the
  // PM4 stream publishes. Vertex constants are 0..255, so first_reg is 0.
  // Applied BEFORE the shader's own load table so a per-draw literal still
  // wins its slots -- the hardware order, same reasoning as the comment on
  // OverlayShaderConstants.
  mx::gpu::alu::OverlayNonFinite(0, out.data(), kD3d9ConstRegs,
                                 /*count_finite_zeros=*/true);
  // One bit per register: did it hold a NaN before the shader's literal overlay
  // ran? Everything up to this point is per-DEVICE and shared by every draw;
  // OverlayShaderConstants is the only per-SHADER step, so this is the split
  // that explains a register being 100% finite for one shader and 0% for
  // another. Cheap: 256 bit-sets, against a 4096-dword read we already did.
  std::array<uint32_t, kD3d9ConstRegs / 32> before{};
  for (uint32_t r = 0; r < kD3d9ConstRegs; ++r) {
    bool nan = false;
    for (uint32_t c = 0; c < 4; ++c) {
      const uint32_t b = out[r * 4 + c];
      nan = nan || ((b & 0x7F800000u) == 0x7F800000u && (b & 0x007FFFFFu) != 0);
    }
    if (nan) before[r >> 5] |= 1u << (r & 31);
  }
  OverlayShaderConstants(shader, base, out);
  NoteVertexConstantNaN(out, shader, before);
  return true;
}

//---------------------------------------------------------------------------
// Stencil sizing census.
//
// MEASUREMENT ONLY -- nothing branches on any of this. It exists to answer one
// question before the stencil work is scheduled: how many draws actually want
// stencil, and how many distinct configurations would have to be translated.
// We have no stencil at all today (depth surfaces are D32_FLOAT with no
// stencil plane and StencilEnable is never set), and implementing it means
// changing the depth format on every surface and DSV, so it should not start
// on a guess about how much of the frame cares.
//
// The population has to be right or the number is worthless. Two traps:
//
//   - RB_DEPTHCONTROL.stencil_enable ALONE over-counts. Xenia gates the whole
//     register on RB_MODECONTROL.edram_mode: outside kColorDepth (4) and
//     kDepthOnly (5) both depth AND stencil are ignored by the hardware, and
//     it returns a zeroed RB_DEPTHCONTROL
//     (xenia/gpu/draw_util.cc:90, GetNormalizedDepthControl). So a draw can
//     have the bit set and mean nothing by it. Both are counted separately
//     here and the honest figure is the AND.
//   - Draws whose register was unreadable are counted too, so the denominator
//     is every draw that reached the read rather than only the ones that
//     answered.
//
// Register offsets follow the block rule this file already establishes three
// times over (see the window-scissor comment below): 0x28C0 is register
// 0x2080, 0x28CC is 0x2100, 0x2934 is 0x2200, four bytes per register within
// a block. RB_MODECONTROL is 0x2208 -> 0x2954; RB_STENCILREFMASK is 0x210D ->
// 0x2900. Field layouts are the reference's (`registers.h:798` and `:821`).
std::mutex g_stencilCensusMu;
uint64_t g_stencilDrawsSeen = 0;        // reached the RB_DEPTHCONTROL read
uint64_t g_stencilDrawsUnreadable = 0;  // ...and could not read it
uint64_t g_stencilBitSet = 0;           // stencil_enable, mode ignored
uint64_t g_stencilEffective = 0;        // stencil_enable AND edram_mode says so
std::map<uint32_t, uint64_t> g_edramModes;  // edram_mode -> draws
// (depth_control, stencilrefmask) -> draws, for stencil-effective draws only.
std::map<std::pair<uint32_t, uint32_t>, uint64_t> g_stencilConfigs;

void NoteStencilCensusUnreadable() {
  std::lock_guard<std::mutex> lk(g_stencilCensusMu);
  ++g_stencilDrawsSeen;
  ++g_stencilDrawsUnreadable;
}

// DEPTH-SURFACE ALIASING CENSUS.
//
// The existing EDRAM aliasing census covers COLOUR targets only -- every owner
// it has ever listed is fmt10/28/34/41 -- so nothing in this tree can say
// whether two DEPTH surfaces share an EDRAM base. That gap has a live
// consequence now that stencil is honoured.
//
// menu.rdc: the geometry that stamps the stencil mask runs against depth
// surface 682 (768x640), and the fullscreen fill that tests the mask runs
// against 387 (1280x720). We key depth targets by OBJECT, so those are two
// D3D12 textures with two independent stencil planes. On the console they may
// be two views of one EDRAM allocation, in which case the mask the first pass
// writes is exactly the mask the second should read -- and we lose it.
//
// This says whether they share a base. Owners per base, so "2 owners" on the
// base the menu uses is the finding, and one owner per base kills the theory
// outright.
//
// THE BASE IS VERIFIED, not extrapolated. It comes from the guest's D3D9
// surface object at +0x1C, the same field for colour and depth surfaces, and
// the reference confirms both registers place the base in the same bits:
// RB_DEPTH_INFO is `depth_base : 11` then `depth_base_bit_11 : 1`
// (rex/graphics/registers.h:834), i.e. bits [11:0], exactly what `& 0xFFF`
// takes -- the same placement as RB_COLOR_INFO::color_base. That check
// mattered: a wrong offset here reads a plausible value rather than failing,
// and the aliasing conclusion rests entirely on these numbers.
//
// The FORMAT field does differ -- depth_format is one bit at +16 against
// colour's four -- so `(color_info >> 16) & 0xF` is meaningless for a depth
// surface. Nothing reads it as one; noted so nobody adds it.
std::mutex g_depthSurfaceMu;
struct DepthSurfaceInfo {
  uint32_t width = 0, height = 0;
  uint64_t draws = 0;
};
// base -> object -> info
std::map<uint32_t, std::map<uint32_t, DepthSurfaceInfo>> g_depthSurfaces;

void NoteDepthSurface(uint32_t object, uint32_t width, uint32_t height,
                      uint32_t base) {
  if (!object) return;
  std::lock_guard<std::mutex> lk(g_depthSurfaceMu);
  // Bounded: a runaway here would mean the base is not what we think it is,
  // and that is itself worth seeing rather than growing without limit.
  if (g_depthSurfaces.size() >= 32 && !g_depthSurfaces.count(base)) return;
  auto& e = g_depthSurfaces[base][object];
  e.width = width;
  e.height = height;
  ++e.draws;
}

std::string DepthSurfaceReport() {
  std::lock_guard<std::mutex> lk(g_depthSurfaceMu);
  std::string out;
  for (const auto& [base, owners] : g_depthSurfaces) {
    out += fmt::format("\n  base 0x{:03X}: {} owner{}", base, owners.size(),
                       owners.size() == 1 ? "" : "s");
    for (const auto& [obj, i] : owners)
      out += fmt::format(" 0x{:08X}({}x{} x{})", obj, i.width, i.height,
                         i.draws);
  }
  return out.empty() ? std::string("\n  (none)") : out;
}

// PHASE 1 CHECK. Counts the same population as NoteStencilCensus, but from the
// fields actually carried on the DrawCall rather than from registers read at
// the census site. The two must agree exactly.
//
// This is the whole point of Phase 1: nothing renders differently yet, so the
// only thing that can be verified is whether the values the renderer will
// eventually see are the values the guest programmed. If these two lines
// disagree on the config set, the plumbing is wrong and it is caught while no
// pixel has moved.
//
// A restatement would be worthless -- if this read the registers itself it
// would agree with the census by construction and say nothing about the
// DrawCall. It reads dc only.
//
// CALLED FROM THE CONSUMER, not from the capture site. The first cut called it
// two lines after the registers were read, on the same thread, from the same
// device -- so it could only ever fail if the assignment itself was broken, and
// it tested none of the trip that matters. Phase 2 reads these fields in the
// RENDERER, after the draw has gone through the deferred queue, and that queue
// is where a field gets dropped by a copy that predates it or read after the
// device has moved on. So the check now runs where the value is used.
//
// What it still does NOT prove: the register OFFSETS. Both this and the census
// trust the same two constants. Their correctness rests on the separate
// distribution test the census documents -- a wrong offset reads a plausible
// value rather than failing, and the tell is edram_mode taking a value other
// than 4 or 5.
std::mutex g_plumbedStencilMu;
uint64_t g_plumbedSeen = 0, g_plumbedUnreadable = 0, g_plumbedEffective = 0;
std::map<std::pair<uint32_t, uint32_t>, uint64_t> g_plumbedConfigs;

void NotePlumbedStencilImpl(const mx::hle::DrawCall& dc) {
  std::lock_guard<std::mutex> lk(g_plumbedStencilMu);
  ++g_plumbedSeen;
  // Either register unreadable is counted apart rather than folded into the
  // config set: an unreadable refmask would otherwise enter the map as
  // 0xFFFFFFFF and invent a nineteenth configuration out of a failed read.
  if (dc.stencil_ref_mask == 0xFFFFFFFFu || dc.edram_mode == 0xFFFFFFFFu) {
    ++g_plumbedUnreadable;
    return;
  }
  if (!(dc.depth_control & 1u)) return;
  if (dc.edram_mode != 4u && dc.edram_mode != 5u) return;
  ++g_plumbedEffective;
  ++g_plumbedConfigs[{dc.depth_control, dc.stencil_ref_mask}];
}

// WHERE IS RB_STENCILREFMASK_BF (0x210E)? We have never read it.
//
// Under two-sided stencil the BACK face carries its own ref and read/write
// masks, and the deferred light volumes are exactly that case -- their marking
// pass increments through the BACK face's stencil FAIL op. We apply the FRONT
// ref (0x210D) to both faces, so if the guest's back-face ref is not the same
// value, our marks differ from the console's. That is the leading explanation
// for the light pass rejecting a contribution it should keep; see
// [[menu-frame-graph]].
//
// The offset is NOT derivable. This shadow is not a flat register file --
// 0x2200 sits at 0x2934 and 0x210D at 0x2900, which are not the same mapping --
// and [[half-pixel-offset-not-applied]] records PA_SU_VTX_CNTL being
// "unlocatable by extrapolation" after exactly this kind of guess. 0x2904 is
// the obvious candidate and guessing it is how that mistake gets made twice.
//
// So: DUMP A WINDOW and let the data name the offset. Restricted to two-sided
// draws, which is the only case where a back-face register means anything and
// is precisely the light volumes -- so the values printed belong to the draws
// under investigation rather than to the whole frame. What to look for is an
// offset whose value is refmask-SHAPED (top byte zero, 0x00rrwwss) and which
// is not simply a copy of 0x2900's.
constexpr uint32_t kBfWindowBase = 0x2900;
constexpr uint32_t kBfWindowDwords = 8;
std::mutex g_bfWindowMu;
// offset -> value -> how many two-sided draws saw it.
std::map<uint32_t, std::map<uint32_t, uint64_t>> g_bfWindow;
uint64_t g_bfWindowDraws = 0;

void NoteBackFaceWindow(uint32_t depth_control, uint32_t device,
                        uint8_t* base) {
  // Bit 7 is BACKFACE_ENABLE. With it clear the guest means the front state for
  // both faces and there is no back-face register to find, so sampling those
  // draws would bury the signal under the other 95% of the frame.
  if (!device || !((depth_control >> 7) & 1u)) return;
  uint32_t vals[kBfWindowDwords];
  for (uint32_t i = 0; i < kBfWindowDwords; ++i) {
    const uint32_t off = kBfWindowBase + i * 4;
    if (!HostPageReadable(REX_RAW_ADDR(device + off))) return;
    vals[i] = REX_LOAD_U32(device + off);
  }
  std::lock_guard<std::mutex> lk(g_bfWindowMu);
  ++g_bfWindowDraws;
  for (uint32_t i = 0; i < kBfWindowDwords; ++i)
    ++g_bfWindow[kBfWindowBase + i * 4][vals[i]];
}

void NoteStencilCensus(uint32_t depth_control, uint32_t device, uint8_t* base) {
  constexpr uint32_t kRbModeControl = 0x2954;      // RB_MODECONTROL   0x2208
  constexpr uint32_t kRbStencilRefMask = 0x2900;   // RB_STENCILREFMASK 0x210D
  NoteBackFaceWindow(depth_control, device, base);
  // 0xFFFFFFFF distinguishes "could not read" from any real register value;
  // edram_mode is 3 bits so no genuine reading can collide with it.
  uint32_t edram_mode = 0xFFFFFFFFu;
  uint32_t refmask = 0xFFFFFFFFu;
  if (device && HostPageReadable(REX_RAW_ADDR(device + kRbModeControl)))
    edram_mode = REX_LOAD_U32(device + kRbModeControl) & 0x7u;
  if (device && HostPageReadable(REX_RAW_ADDR(device + kRbStencilRefMask)))
    refmask = REX_LOAD_U32(device + kRbStencilRefMask);

  const bool bit = (depth_control & 1u) != 0;
  const bool mode_honours = (edram_mode == 4u || edram_mode == 5u);

  std::lock_guard<std::mutex> lk(g_stencilCensusMu);
  ++g_stencilDrawsSeen;
  ++g_edramModes[edram_mode];
  if (bit) ++g_stencilBitSet;
  if (bit && mode_honours) {
    ++g_stencilEffective;
    ++g_stencilConfigs[{depth_control, refmask}];
  }
}

// A draw record carrying NEITHER shader. The renderer counts these as
// "no-handle" stand-ins and, measured 2026-08-17, they are the entire
// unexplained remainder of that population: 3317 records, one signature,
// 4 indices, ~2 per frame, colour write on, no YUV planes, not a clear and not
// a surface bind. They also never reach AttachTranslatedPixelShader -- its
// untranslated counter prints every 500 and never fires -- so nothing on the
// shader path has ever described them.
//
// Logged with the guest context the renderer does not have, and specifically
// with the bound texture objects: if one of them is a resolve destination, this
// is the consumer the menu backdrop has been missing. Keyed so each distinct
// shape reports once rather than 3317 times.
void NoteShaderlessDraw(const mx::hle::DrawCall& dc) {
  if (dc.pixel_shader_handle || dc.vertex_shader_handle) return;
  auto& st = mx::hle::DeviceState();
  static std::mutex s_mu;
  static std::set<uint64_t> s_seen;
  // The inherited shader is part of the key: if these draws inherit DIFFERENT
  // programs the single-line report would hide it behind whichever arrived
  // first, and that is the whole question being asked.
  const uint64_t key = (uint64_t(uint32_t(dc.topology)) << 40) ^
                       (uint64_t(dc.index_count) << 16) ^
                       uint64_t(dc.render_target_object) ^
                       (uint64_t(st.last_nonnull_pixel_shader) << 8);
  {
    std::lock_guard<std::mutex> lk(s_mu);
    if (s_seen.size() >= 16 || !s_seen.insert(key).second) return;
  }
  std::string bound;
  for (uint32_t gs = 0; gs < mx::hle::kMaxSamplers; ++gs) {
    const uint32_t obj = st.texture[gs].object;
    if (!obj) continue;
    const bool is_resolve = g_resolvedTextureTargets.contains(obj);
    bound += fmt::format(" s{}=0x{:08X}{}", gs, obj,
                         is_resolve ? "(RESOLVE-DEST)" : "");
  }
  // Compare last-non-null against the two handles on the "Bink composite
  // shaders created" line at startup: a match means these draws are inheriting
  // a Bink composite shader and IsBinkCompositeDraw is refusing them only
  // because SetPixelShader(NULL) cleared the slot it tests.
  REXLOG_INFO("d3d9: SHADERLESS draw topology {} indices {} verts {} target "
              "0x{:08X} {}x{} colour_mask 0x{:X}; last non-null ps 0x{:08X}; "
              "bound textures:{}",
              uint32_t(dc.topology), dc.index_count,
              dc.vertex_stride ? dc.vertices.size() / dc.vertex_stride : 0,
              dc.render_target_object, dc.render_target_width,
              dc.render_target_height, dc.colour_mask,
              st.last_nonnull_pixel_shader,
              bound.empty() ? " none" : bound);
}

bool FinishHleDraw(mx::hle::DrawCall& dc) {
  mx::hle::HleSkip skip = mx::hle::HleSkip::kNone;
  if (!mx::hle::FinalizeHleTopology(dc, skip)) {
    ++mx::hle::HleSkipCounts()[uint32_t(skip)];
    ++g_hleDrawsRefused;
    return false;
  }
  NoteShaderlessDraw(dc);
  mx::hle::HleFrameDraws().push_back(std::move(dc));
  ++g_hleDrawsAccepted;
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
    // decoder produces it -- see the table in d3d9_layout.h. Zero is NOT the
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
    e.swizzle = 0x60A;  // (z,y,x,w) -- D3DCOLOR is BGRA, wanted as RGBA
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

  // DIAG: census SQ_PROGRAM_CNTL per VS/PS pair, read
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
  // a draw that failed to translate never reached it -- the probe was gated on
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
  ReadIndexConditioning(device, base, in);

  const int id = g_currentDecl;
  if (id >= 0 && g_declLayoutOk[id]) in.layout = &g_declLayout[id];

  // A UP draw usually has no declaration -- it uses SetFVF instead -- so fall
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
      streams[i].buffer_obj = b.buffer_obj;
      streams[i].bound = true;
      // Overrides host/size/offset/endian from the device's own fetch
      // constant where that file agrees about the base. See
      // ApplyDeviceFetchConstant: the snapshot is a bind-time copy of the
      // buffer header, the register file is what the GPU actually reads.
      ApplyDeviceFetchConstant(streams[i], b, i, device, base);
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
  // inverse -- the same one the PM4 path uses. That is not a claim that it is
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
    ++g_drawNoViewport;
    return;
  }
  if (have_vp) in.mvp = vp;

  // Will this draw fetch its own vertices on the GPU? Asked HERE, before the
  // draw is built, because the answer decides whether to spend a per-vertex
  // pass transcoding a 36-byte host vertex the fetch path never reads -- 26-31ms
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
    // Streams this shader indexes by a computed register. The CPU vertex path
    // reads them as if indexed by the vertex, which is an unrelated row rather
    // than an approximation, so it zero-fills them instead.
    if (vst) in.computed_index_streams = vst->computed_index_streams;
    // The DENOMINATOR for the skip counter below. "0 attributes left
    // default" reads as "never happens" and as "the flag never arrived",
    // and those are opposite conclusions -- the same ambiguity that made an
    // unreachable 100% look like a measurement earlier in this branch. This
    // counts every draw that CARRIES a computed-index stream, whichever path
    // it then takes, so the pair can be read.
    if (in.computed_index_streams) ++g_computedIndexDraws;
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
  // INTERPOLATOR ZERO-FILL, counted where BOTH stages are known.
  //
  // A slot the pixel shader reads that the vertex shader never exports arrives
  // as a literal float4(0,0,0,0). That is invented output. A slot nobody reads
  // is not -- which is why the first version of this counter, sitting at VS
  // translation and firing on every unexported slot, read ~80% in both scenes
  // and meant nothing.
  //
  // TWO EXCLUSIONS, both of which this project has already been burned by:
  //
  //   PARAM_GEN. The hardware fills one slot with the pixel position, and the
  //   VS does not export it. Counting that as a missing interpolator is exactly
  //   the mistake that produced the "terrain reads an unexported interpolator"
  //   theory, which was wrong: r6 was PARAM_GEN and working.
  //   dc.pixel_param_gen is zero when disabled, else param_gen_pos + 1.
  //
  //   input_mask OVER-REPORTS. It marks any temp read before written in walk
  //   order, so a conditionally-written register can look like an input.
  //
  // READ THIS BEFORE ACTING ON THE NUMBER. That upper bound turned out to be so
  // loose that the figure is NOT a defect count. Run 1459 named the worst eight
  // pairs by draw count and all eight are benign:
  //
  //   Seven are SCRATCH REGISTERS. ps 0x216B80E0 reports missing 0x38 (r3, r4,
  //   r5) against a VS exporting 0x7 -- and its microcode writes all three
  //   before use: `tfetch2D r3.x???`, `dp3 r4.x`, `dp3 r5.x` at instructions 3,
  //   5 and 7. They were never interpolators. The tell is the SHAPE: in every
  //   row ps_in is a contiguous low run, vs_out a shorter contiguous low run,
  //   and missing exactly the difference -- never a gap in the middle. That is
  //   register allocation. A real linkage mismatch would scatter.
  //
  //   The eighth is ARITHMETICALLY INERT. vs 0x21686F60 / ps 0x21686C60 has
  //   vs_out 0x0, and the VS says so itself: `alloc interpolators/colors
  //   size=0`. Its PS is two instructions -- `max export0 [no write], r0, r0`
  //   and `sgts export0.x, -|r0|.x` -- and sgts on a negated absolute value is
  //   0 for every possible r0. The output cannot depend on the input.
  //
  // A TRAP ON THE WAY: checking the generated HLSL "confirmed" the reads,
  // because the emitter reads whatever input_mask says -- they agree by
  // construction. Only the guest microcode is independent. Same shape as
  // `--verify` only asking ourselves.
  //
  // Fixing it properly means a real interpolator mask -- read before ANY write
  // on ALL paths -- which changes what every shader reads, not just a counter.
  // Not worth that risk for a diagnostic. The figure is kept because a NEW pair
  // appearing is still worth seeing; the absolute level is what means nothing.
  //
  // Per DRAW, not per shader: the zero reaches the pixel stage on every draw of
  // the pair, so draws are the population that says how much of the frame is
  // affected -- and it matches every other per-draw entry in the census.
  {
    const uint32_t vs_h = st.vs_seen ? st.vertex_shader : 0;
    const uint32_t ps_h = st.ps_seen ? st.pixel_shader : 0;
    const TranslatedShader* v = vs_h ? TranslatedVertexShader(vs_h) : nullptr;
    const TranslatedShader* p = ps_h ? TranslatedPixelShader(ps_h) : nullptr;
    if (v && p) {
      const uint32_t gen = dc.pixel_param_gen;  // 0 = disabled, else pos + 1
      uint32_t missing = 0;
      for (uint32_t i = 0; i < mx::hle::kHlslInterpolatorLinkage; ++i) {
        const bool read = (p->input_mask & (1u << i)) != 0;
        const bool exported = (v->export_mask & (1u << i)) != 0;
        const bool param_gen = gen && (gen - 1) == i;
        const bool fill = read && !exported && !param_gen;
        if (fill) missing |= 1u << i;
        mx::gpu::guard::Note(mx::gpu::guard::Guard::kInterpolatorZeroFill,
                             fill);
      }
      // NAME THE PAIRS. A percentage is not actionable; a handful of named
      // VS/PS pairs is. Keyed on (vs, ps, missing-slot mask) and weighted by
      // DRAW COUNT, because one pair drawn 40,000 times and forty pairs drawn
      // once are the same rate and completely different problems.
      //
      // Handles are addresses and vary per run, so the row carries the masks
      // too -- that is what makes a pair identifiable across runs
      // (shader handles are not stable).
      if (missing) {
        struct PairRow {
          uint64_t draws = 0;
          uint32_t missing = 0, ps_in = 0, vs_out = 0, gen = 0;
        };
        static std::mutex s_mu;
        static std::map<uint64_t, PairRow> s_pairs;
        static uint64_t s_total = 0;
        bool report = false;
        {
          std::lock_guard<std::mutex> lk(s_mu);
          const uint64_t key = (uint64_t(vs_h) << 32) | ps_h;
          PairRow& r = s_pairs[key];
          ++r.draws;
          r.missing = missing;
          r.ps_in = p->input_mask;
          r.vs_out = v->export_mask;
          r.gen = gen;
          report = (++s_total % 40000) == 0;
        }
        if (report) {
          std::vector<std::pair<uint64_t, uint64_t>> worst;
          std::string rows;
          {
            std::lock_guard<std::mutex> lk(s_mu);
            for (const auto& [k, r] : s_pairs) worst.emplace_back(r.draws, k);
            std::sort(worst.rbegin(), worst.rend());
            for (size_t i = 0; i < worst.size() && i < 8; ++i) {
              const PairRow& r = s_pairs[worst[i].second];
              rows += fmt::format(
                  " [vs 0x{:08X} ps 0x{:08X} x{} missing 0x{:X} (ps_in 0x{:X} "
                  "vs_out 0x{:X} param_gen {})]",
                  uint32_t(worst[i].second >> 32),
                  uint32_t(worst[i].second & 0xFFFFFFFFu), r.draws, r.missing,
                  r.ps_in, r.vs_out, r.gen ? int(r.gen - 1) : -1);
            }
          }
          REXLOG_INFO("d3d9: ZERO-FILLED INTERPOLATORS {} pairs over {} draws, "
                      "worst first (input_mask over-reports, so this is an "
                      "upper bound):{}",
                      worst.size(), s_total, rows);
        }
      }
    }
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
          f << "    first host position = (not transcoded -- GPU fetch)\n";
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
    // MRT slot 1, if the guest bound a distinct second target. See the note on
    // the field. Guarded on a DIFFERENT object so a device that leaves slot 1
    // pointing at slot 0 cannot produce a self-referential pair.
    const auto& rt1 = st.render_target[1];
    if (rt1.valid && rt1.object && rt1.object != rt.object) {
      dc.render_target1_object = rt1.object;
      dc.render_target1_color_info = rt1.color_info;
      dc.render_target1_width = rt1.width;
      dc.render_target1_height = rt1.height;
    }
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
    NoteDepthSurface(dc.depth_target_object, dc.depth_target_width,
                     dc.depth_target_height, dc.depth_target_base);
  }
  ProbePixelProfileForDraw(st.ps_seen ? st.pixel_shader : 0, device, base, dc);
  // The Bink composite needs its whole plane set, so it takes its own path
  // rather than competing in the single-winner binding contest.
  const uint32_t bound_ps = st.ps_seen ? st.pixel_shader : 0;
  // REVERTED 2026-08-17: do NOT widen this to the last non-null pixel shader.
  //
  // The theory was sound and its evidence real -- sub_82565928 emits no pixel
  // IM_LOAD for a null shader, so the GPU keeps the previous program, and the
  // 3317 shaderless 4-index quads a run DO inherit exactly ps_yuv /
  // ps_yuv_alpha. But "inherits the Bink shader" is not "is a Bink composite":
  // this title submits an enormous number of null-pixel-shader draws (the depth
  // passes, ~96k in a menu run), and every one of them follows a composite and
  // so inherits it too. Measured: 54,000 matches in 40 seconds, **0** of which
  // prepared planes, and the shaderless population did not move. It also cost
  // 54,000 futile texture decode attempts.
  //
  // Whatever admits those quads has to discriminate on the PLANES, not on the
  // inherited shader.
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
  // RB_DEPTHCONTROL, from the device's register shadow -- the same block base
  // and the same reason as RB_COLOR_MASK above, RB_BLENDCONTROL0 and
  // RB_COLORCONTROL below. Register 0x2200 is the m_ControlPacket base itself,
  // so it sits at device+0x2934 exactly; its two neighbours 0x2201 and 0x2202
  // are already read at 0x2938 and 0x293C, which is three registers agreeing on
  // one offset rule.
  //
  // This was the LAST output-merger state still coming from the thread-local
  // D3D9 setter shadow, and it carried both of that shadow's defects:
  //
  //   - A draw submitted by a worker that never called SetRenderState(ZENABLE)
  //     read `Seen` false and got depth_control 0 -- depth silently off -- while
  //     a worker that had called it once applied that value to every later draw.
  //     The colour-mask comment above spells out why the shadow cannot be
  //     trusted for this; depth simply never got the same treatment.
  //   - z_write was FABRICATED: `if (ZEnable) depth_control = z_enable |
  //     z_write` set the write bit from the test bit, so D3DRS_ZWRITEENABLE was
  //     ignored entirely. A UI plate drawn depth-testing but NOT depth-writing
  //     wrote depth here anyway and occluded everything drawn after it -- which
  //     is the menu bike disappearing behind the submenu plates, per-pixel, with
  //     the nearer parts of the bike still showing through.
  //
  // Stored RAW, like the other three, so a misread shows up as a wrong number
  // rather than as a plausible depth mode. Bit layout verified against the
  // reference (`registers.h:799`): stencil_enable +0, z_enable +1,
  // z_write_enable +2, zfunc +4..6.
  //
  // NOTE: zfunc is carried here but still not honoured downstream -- the
  // pipelines hardcode LESS_EQUAL. That is a separate known gap and is
  // deliberately not fixed in the same change, so that a regression in either
  // one can be attributed.
  constexpr uint32_t kRbDepthControl = 0x2934;  // RB_DEPTHCONTROL 0x2200
  if (device && HostPageReadable(REX_RAW_ADDR(device + kRbDepthControl))) {
    dc.depth_control = REX_LOAD_U32(device + kRbDepthControl);
    dc.om_seen |= 1u << 1;
    // One line per DISTINCT value, not the first N draws. A cap on occurrences
    // would sample whatever happens to run first and say nothing about the
    // population that matters -- which is exactly how the alpha-test probe
    // above ended up reporting "enable 0" for a game that uses it 160 times a
    // frame. Every depth mode the guest actually programs appears here exactly
    // once, and the set is small.
    static std::map<uint32_t, uint64_t> s_depth;
    if (++s_depth[dc.depth_control] == 1 && s_depth.size() <= 32) {
      REXLOG_INFO("d3d9: RB_DEPTHCONTROL 0x{:08X}: z_enable {} z_write {} "
                  "zfunc {} stencil {}",
                  dc.depth_control, (dc.depth_control >> 1) & 1u,
                  (dc.depth_control >> 2) & 1u, (dc.depth_control >> 4) & 7u,
                  dc.depth_control & 1u);
    }
    // Captured HERE, in the same block that reads RB_DEPTHCONTROL, so all
    // three registers describe one draw. NoteStencilCensus reads the same two
    // from the same device a line later, which is what makes the PLUMBED census
    // below a real check on this rather than a restatement of it.
    constexpr uint32_t kRbStencilRefMask = 0x2900;  // RB_STENCILREFMASK 0x210D
    constexpr uint32_t kRbModeControl = 0x2954;     // RB_MODECONTROL    0x2208
    if (HostPageReadable(REX_RAW_ADDR(device + kRbStencilRefMask)))
      dc.stencil_ref_mask = REX_LOAD_U32(device + kRbStencilRefMask);
    if (HostPageReadable(REX_RAW_ADDR(device + kRbModeControl)))
      dc.edram_mode = REX_LOAD_U32(device + kRbModeControl) & 0x7u;
    NoteStencilCensus(dc.depth_control, device, base);
  } else if (st.render_state.Seen(kRsZEnable)) {
    // Unreadable register only. Still the old approximation, because there is
    // nothing better to approximate from -- but it no longer hides the register
    // read, and it is now the exception rather than the rule.
    if (st.render_state.value[kRsZEnable])
      dc.depth_control = (1u << 1) | (1u << 2);
    dc.om_seen |= 1u << 1;
    NoteStencilCensusUnreadable();
  }
  // The window scissor. Offsets from IDA's own D3DDevice layout rather than
  // arithmetic: m_WindowPacket sits at device+0x28C0 and is three dwords, and
  // D3DDevice_SetScissorRect (0x8254B678) writes its TL/BR members directly.
  // That base also cross-checks the packet convention the two neighbouring
  // reads above rely on -- 0x28C0 is register 0x2080 exactly as 0x28CC is
  // 0x2100 and 0x2934 is 0x2200, three packets agreeing on one rule.
  //
  // Field widths are the reference's (`registers.h:622`): 14 bits per edge,
  // bit 31 of TL disables the window offset. D3D9's setter happens to mask
  // with 15 bits, which only matters above 8191 -- past any real target.
  constexpr uint32_t kPaScWindowOffset = 0x28C0;     // PA_SC_WINDOW_OFFSET
  constexpr uint32_t kPaScWindowScissorTl = 0x28C4;  // PA_SC_WINDOW_SCISSOR_TL
  constexpr uint32_t kPaScWindowScissorBr = 0x28C8;  // PA_SC_WINDOW_SCISSOR_BR
  if (device && HostPageReadable(REX_RAW_ADDR(device + kPaScWindowOffset))) {
    const uint32_t tl = REX_LOAD_U32(device + kPaScWindowScissorTl);
    const uint32_t br = REX_LOAD_U32(device + kPaScWindowScissorBr);
    int32_t left = int32_t(tl & 0x3FFFu);
    int32_t top = int32_t((tl >> 16) & 0x3FFFu);
    int32_t right = int32_t(br & 0x3FFFu);
    int32_t bottom = int32_t((br >> 16) & 0x3FFFu);
    if ((tl & 0x80000000u) == 0) {
      // The offsets are SIGNED 15-bit fields, so they must be sign-extended
      // before they are added -- a negative offset read as unsigned would push
      // the rectangle off the far side of the target instead of toward zero.
      const uint32_t off = REX_LOAD_U32(device + kPaScWindowOffset);
      auto s15 = [](uint32_t v) {
        return int32_t(v & 0x7FFFu) - int32_t((v & 0x4000u) ? 0x8000 : 0);
      };
      const int32_t ox = s15(off & 0x7FFFu);
      const int32_t oy = s15((off >> 16) & 0x7FFFu);
      left += ox; right += ox;
      top += oy; bottom += oy;
    }
    dc.scissor_left = left;
    dc.scissor_top = top;
    dc.scissor_right = right;
    dc.scissor_bottom = bottom;
    dc.scissor_seen = true;
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

  // PA_SU_SC_MODE_CNTL -- the cull mode, which this renderer does not read at
  // all: both PSO paths hardcode D3D12_CULL_MODE_NONE (d3d12_game.cpp:259 and
  // :890). MEASURED here before anything acts on it.
  //
  // Register 0x2205, so device+0x2934 + 5*4 = device+0x2948, from the same
  // block base as RB_DEPTHCONTROL (0x2200 -> +0x2934), RB_BLENDCONTROL0 (0x2201
  // -> +0x2938) and RB_COLORCONTROL (0x2202 -> +0x293C). Four registers now
  // agree on that offset rule.
  //
  // Why this is suspected: the draw that erases the menu background is a closed
  // 24-vertex BOX (menu1.rdc event 8324, export_mesh: TriangleStrip with
  // degenerate joins) whose pixel shader always outputs (0,0,0,0), and it covers
  // EVERY pixel sampled -- (200,180), (640,100), (50,650). Covering the whole
  // screen from a closed box means the camera is INSIDE it, so every visible
  // face is a back face. Cull back and the console draws nothing at all; cull
  // NONE, as we do, rasterises the interior and paints the screen black.
  //
  // That also explains why nothing else obviously broke: for opaque solids an
  // unculled back face is simply hidden by the depth test. A volume containing
  // the camera is the case where it is catastrophic.
  //
  // Bit layout from the reference (registers.h:456): cull_front +0, cull_back
  // +1, face +2 (0 = front is CCW, 1 = CW). One line per DISTINCT value, so the
  // whole set of modes the game programs appears once each.
  uint32_t pa_su_sc = 0;
  bool pa_su_sc_seen = false;
  {
    constexpr uint32_t kPaSuScModeCntl = 0x2948;  // PA_SU_SC_MODE_CNTL 0x2205
    if (device && HostPageReadable(REX_RAW_ADDR(device + kPaSuScModeCntl))) {
      pa_su_sc = REX_LOAD_U32(device + kPaSuScModeCntl);
      pa_su_sc_seen = true;
      dc.pa_su_sc_mode_cntl = pa_su_sc;
      // GUEST VIEWPORT vs the one we actually set.
      //
      // PA_CL_VTE_CNTL reads 0x43F, so the GPU applies the viewport transform
      // and the guest's vertex shader exports CLIP SPACE -- which is why the
      // mvp here is identity for 99.8% of draws. That part is right. What is
      // NOT established is that the host viewport we hand D3D12 is the one the
      // guest asked for: the renderer sets it to the full render-target extent
      // (drawTarget->width/height) and never consults these registers.
      //
      // The 0x21xx block base is device+0x28CC (register 0x2100), from the same
      // decompiled flush that gives the 0x22xx block its base, so
      // PA_CL_VPORT_XSCALE (0x210F) is at +0x2908. A stray observation in this
      // file already recorded 640/640/-90/90 there, which is x 0..1280 but y
      // 0..180 -- NOT a full 720-tall target. If that is common the host
      // viewport is wrong for those draws.
      //
      // Census only. Distinct rectangles, so the whole set appears once each.
      {
        constexpr uint32_t kPaClVportXScale = 0x2908;  // 0x210F
        if (HostPageReadable(REX_RAW_ADDR(device + kPaClVportXScale)) &&
            HostPageReadable(REX_RAW_ADDR(device + kPaClVportXScale + 12))) {
          auto f = [&](uint32_t i) {
            const uint32_t bits =
                REX_LOAD_U32(device + kPaClVportXScale + i * 4);
            float v;
            std::memcpy(&v, &bits, 4);
            return v;
          };
          const float xs = f(0), xo = f(1), ys = f(2), yo = f(3);
          if (std::isfinite(xs) && std::isfinite(xo) && std::isfinite(ys) &&
              std::isfinite(yo)) {
            const int32_t x0 = int32_t(std::lround(xo - std::fabs(xs)));
            const int32_t x1 = int32_t(std::lround(xo + std::fabs(xs)));
            const int32_t y0 = int32_t(std::lround(yo - std::fabs(ys)));
            const int32_t y1 = int32_t(std::lround(yo + std::fabs(ys)));
            // Recorded, not classified. The first version of this compared
            // against dc.render_target_width -- the D3D9 render-target extent
            // -- and called the result agree/MISMATCH. That is the wrong
            // reference: the renderer hands D3D12 `drawTarget->width` from its
            // OWN target lookup, which comes from a different path and can
            // differ, and three rows came back "target 0x0" because that field
            // is not populated for every draw. The comparison belongs where the
            // viewport is actually set, so it is made in RenderGameFrame.
            dc.guest_vp_width = uint32_t(x1 - x0);
            dc.guest_vp_height = uint32_t(y1 - y0);
          }
        }
      }

      // PA_SU_VTX_CNTL is NOT here, and is not read at all. Recorded so the
      // next person does not spend the afternoon this cost.
      //
      // The register is 0x2302 (register_table.inc:1337). 0x2206, the "next
      // one along" from PA_SU_SC_MODE_CNTL, is PA_CL_VTE_CNTL -- which this
      // file already reads elsewhere for VportScaleEnabled. Reading it here by
      // mistake decoded VPORT_X_SCALE_ENA as PIX_CENTER, and 0x0000043F (all
      // six VTE enables plus VTX_W0_FMT) is a perfectly sensible VTE_CNTL and
      // obvious nonsense as a vertex control: PA_SU_VTX_CNTL is pix_center:1,
      // round_mode:2, quant_mode:3 and then 26 bits of PADDING, so any value
      // with bits 6+ set is not this register.
      //
      // The second guess was worse, because it looked right. The offset rule
      // 0x2934 + (reg - 0x2200) * 4 holds WITHIN a block and does not span
      // them -- 0x2100 sits at +0x28CC and 0x2200 at +0x2934, 256 registers
      // apart but only 0x68 bytes -- so extrapolating to 0x2302 gave +0x2D3C,
      // which read 0x00000000 and decoded as a plausible D3D9 pixel centre.
      // Dumping the neighbourhood killed it: +0x2D00..+0x2D7C holds guest heap
      // pointers (212408C4), an XEX text address (82567668) and the ASCII tag
      // "REX" (52455800). An object with a vtable, not shadowed registers. The
      // zero was a struct field, and a single-address read could never have
      // told that apart from a real register a D3D9 title legitimately sets to
      // zero.
      //
      // Finding it needs IDA -- where the guest writes register 0x2302 -- not a
      // third extrapolation.
      static std::mutex s_mu;
      static std::map<uint32_t, uint64_t> s_modes;
      bool fresh = false;
      {
        std::lock_guard<std::mutex> lk(s_mu);
        fresh = ++s_modes[pa_su_sc] == 1 && s_modes.size() <= 32;
      }
      if (fresh) {
        REXLOG_INFO("d3d9: PA_SU_SC_MODE_CNTL 0x{:08X}: cull_front {} "
                    "cull_back {} face {} (0=CCW front)",
                    pa_su_sc, pa_su_sc & 1u, (pa_su_sc >> 1) & 1u,
                    (pa_su_sc >> 2) & 1u);
      }
    }
  }

  // The 35-index draw that overpaints the menu background black -- event 6809
  // in test-2.rdc, event 8324 in menu1.rdc, where it replaces an HDR ~11.4
  // background with (0,0,0,0) at RT 721.
  //
  // This WAS "first 8 occurrences", which is the wrong population and said so
  // in a way that read like an answer: all 8 reported ONE/ZERO opaque with
  // colour mask 0xF. A cap of 8 samples whatever draws 35 indices FIRST in the
  // run -- loading and intro geometry -- and cannot speak for the menu draw.
  // Now one line per DISTINCT state tuple, so every shape reports exactly once
  // no matter when it occurs.
  //
  // It also sits AFTER the alpha-test read rather than beside the blend read,
  // because the whole output-merger verdict for this draw has to be on ONE
  // line. Three separate first-N probes for blend, alpha and depth are three
  // chances to sample three different draws and read the result as one state.
  //
  // What is already settled about this draw, so the next run does not re-ask:
  //   - the pixel shader (0x264F39E0) is 1 ALU and ALWAYS outputs (0,0,0,0):
  //     `max oC0._000, r0, r0` + `sgts oC0.x___, -r_abs[0].x`, and -|x| > 0 is
  //     unsatisfiable. Xenia's own disassembly of the same bytes agrees slot
  //     for slot, so this is not a translation defect.
  //   - blend is ONE/ZERO/ADD and the colour mask is 0xF, so the black lands.
  //   - the geometry is a closed 24-vertex BOX drawn as one degenerate-joined
  //     triangle strip -- a stencil/light volume, not a colour draw.
  // A shader that deliberately writes zero over a closed volume is gated by
  // something we do not implement, and stencil is the candidate: hence
  // stencilfunc/ops and the ref/masks are decoded here rather than left raw.
  // Bit layout from the reference, registers.h:799.
  if (dc.index_count == 35) {
    static std::mutex s_mu;
    static std::set<uint64_t> s_seen;
    const uint64_t key = (uint64_t(dc.blend_control) << 32) ^
                         (uint64_t(dc.depth_control) << 8) ^
                         (uint64_t(dc.colour_mask) << 4) ^
                         uint64_t(dc.colour_control) ^
                         (uint64_t(dc.pixel_shader_handle) << 16) ^
                         (uint64_t(pa_su_sc) << 40);
    bool fresh = false;
    {
      std::lock_guard<std::mutex> lk(s_mu);
      fresh = s_seen.size() < 32 && s_seen.insert(key).second;
    }
    if (fresh) {
      REXLOG_INFO(
          "d3d9: 35-index OM state ps 0x{:08X}: blend 0x{:08X} (enable {} "
          "src {} dest {} op {}) mask 0x{:X}; alpha 0x{:08X} (enable {} "
          "func {}) ref {}; depth 0x{:08X} z_enable {} z_write {} zfunc {}; "
          "STENCIL enable {} func {} fail {} zpass {} zfail {}; "
          "CULL 0x{:08X} seen {} front {} back {} face {}",
          dc.pixel_shader_handle, dc.blend_control, dc.blend_enable,
          dc.src_blend, dc.dest_blend, dc.blend_op, dc.colour_mask,
          dc.colour_control, (dc.colour_control >> 3) & 1u,
          dc.colour_control & 7u, dc.alpha_ref, dc.depth_control,
          (dc.depth_control >> 1) & 1u, (dc.depth_control >> 2) & 1u,
          (dc.depth_control >> 4) & 7u, dc.depth_control & 1u,
          (dc.depth_control >> 8) & 7u, (dc.depth_control >> 11) & 7u,
          (dc.depth_control >> 14) & 7u, (dc.depth_control >> 17) & 7u,
          pa_su_sc, pa_su_sc_seen ? 1 : 0, pa_su_sc & 1u,
          (pa_su_sc >> 1) & 1u, (pa_su_sc >> 2) & 1u);
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
    if (applied == ShaderApplyResult::kNoCode) {
      ++g_pendingDropped;
      ++g_drawShaderNoCodeFull;
    } else {
      // kFailed. Previously returned with nothing incremented at all, which is
      // how a draw disappears between `guest` and `accepted` leaving `refused`
      // at zero -- the exact shape of the 6.9% gap.
      ++g_drawShaderFailed;
    }
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
        // one in force. Counted separately, and the first few are named -- a
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
      // indexed one it depends on the index values -- which are readable, since
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
      // differ from the snapshot SetStreamSource recorded -- and the size is the
      // field the range check actually depends on.
      {
        const uint32_t d1 = REX_LOAD_U32(device + FetchFileDword1Offset(s));
        const uint32_t live = ((d1 >> 2) & 0xFFFFFF) * 4;
        if (live == b.size_bytes) {
          ++g_fileAgree[s];
        } else {
          ++g_fileDiffer[s];
          // Does the device's size explain a draw the snapshot could not?
          //
          // `have_range` is REQUIRED here. Without it an indexed draw arrives
          // with hi_vertex == 0 -- kProbeIndexRange is false, so have_range is
          // never true for one -- and `0 * stride <= live` is true whatever
          // the sizes are. That reported "explains 254455 of 254455" in run
          // mx_1823, a clean 100% that measured nothing at all and was very
          // nearly acted on. A counter whose test cannot fail is not a
          // measurement.
          if (have_range && b.stride &&
              static_cast<uint64_t>(hi_vertex) * b.stride <= live) {
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
// had decoded from the ring -- `DecodeVertexShaderFetches` needs an array
// starting at the control-flow section ("the blob carries no header saying
// so", shader_ucode.cpp:396), and no UCODE header parser exists in this tree
// or in the SDK at rex/graphics/format/ucode.h.
//
// That search is unnecessary now. `CapturePatchedCode` takes the microcode
// straight out of the command-ring destination inside the PatchVertexShader
// hook and records `code_off` by decoding it, so the offset is known rather
// than found by comparison -- and stageG measured that route at 100% of draws
// against the search's 0%. The vertex blob at +0x368 is no longer collected at
// all; only the pixel-shader blob below is still read.
//
// If a future change needs the offset again, use `g_patch.patched`, not a
// content search. The search was only ever a way to work around not having
// the code.
//---------------------------------------------------------------------------
constexpr uint32_t kMaxBlobDwords = 4096;   // 16 KB ceiling on one blob




//---------------------------------------------------------------------------
// Stage C -- execute the shader and see where the position lands.
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
//    declaration. The copy at +0x40 is the unpatched template -- its format,
//    offset and stride are blank (Stage B) -- and pairing declaration elements
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
// Stage D2 -- REMOVED 2026-08-17 as never-wired scaffolding.
//
// ClipBucket / kClipBucketName / ClassifyClip bucketed exported positions by
// how far outside the clip volume they landed, to replace Stage C's single
// "35% inside the clip volume" with a distribution. ClassifyClip was never
// called and ShaderScore (Stage I, also removed below) was never instantiated,
// so **the distribution was never measured** -- this was intent, not a result.
//
// Kept because the design is sound if the question is ever reopened: bucket on
// x and y only (z has its own near-plane convention and folding it in blurs the
// axis being read), and give the viewport inverse the identical treatment on
// the identical vertices, because without a reference the buckets are just
// numbers. The question itself is likely moot -- the space hypothesis it was
// built to test is settled, see the FINDING block in gpu/hle_types.h.
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
// Stage G -- execute the shader that was actually bound.
//
// Draws are matched to microcode by >=90% content similarity against PM4's
// cache (g_bestKeyAtDraw). That is a heuristic on two counts: it can pick a
// near-identical wrong variant, and it fails outright on ~63% of draws, so
// every number so far comes from a 37% minority.
//
// The patch hook has the real thing. r4 is where D3D9 writes the patched
// microcode and r3 names the shader -- an exact key, no similarity involved.
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
// "exec target at address 0", which is the self-check earning its place -- a
// wrong start would otherwise have decoded into plausible nonsense.
//
// So the start is *searched* rather than assumed, and the search has a
// verifiable answer: the binding table says how many vfetches this shader has,
// and only the true CF start decodes to exactly that many. Resolved once per
// shader handle and reused, because the offset is a property of the layout.

VsWindowCensus g_vsWindow;


ShaderPatchState g_patch;

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




// Stage I -- REMOVED 2026-08-17. `ShaderScore` and `kCtlSpreadEpsilon` were
// declared here and the struct was NEVER INSTANTIATED, so none of it ever ran.
//
// The reasoning that motivated it is worth keeping, because it is general and
// this project keeps rediscovering it: every count it was meant to replace was
// one percentage over a mixed population, with no known target value. Real
// scenes cull, draw shadow maps and run off-screen passes, so 100% in-clip is
// wrong and 36% may be right. Four independent improvements moved that number
// by nothing and a fifth appeared to move it for a reason that cannot have
// caused it. **A number with no target value cannot judge a change** -- ask
// where the failure is, not how big it is. Three shaders is a bug with an
// address; an even spread over forty means the defect is in the model.
//
// Two of its per-shader fields are worth re-deriving if anything like this is
// built again: the RANGE of each position component over every execution (a
// component that never moves is padding or a homogeneous 1, not a coordinate,
// and reading it as z is a misinterpretation no decode correctness will catch),
// and the FIRST execution captured in full (which shader is worst is unknown
// until the report, by which time the vertex is long gone).

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
// Of the no-PS draws, the ones now allowed onto the GPU vertex path anyway
// because they cannot write colour. A subset of g_gpuVertexNoPs, not a peer of
// it: both are incremented for the same draw, so the refused population is
// NoPs - DepthOnly. Kept that way on purpose -- the no-PS count is the thing
// this branch has been reasoning about for several sessions and silently
// changing its meaning would invalidate every earlier number.
uint64_t g_gpuVertexDepthOnly = 0;
// The GPU vertex FETCH path: draws taking it, and why a draw that qualified for
// the GPU vertex stage still could not.
uint64_t g_gpuFetchDraws = 0, g_gpuFetchRectList = 0;
uint64_t g_gpuFetchOrdinalMismatch = 0;
uint64_t g_gpuFetchUnaligned = 0;
// Draws whose vertex shader has no fetch variant at all, because the emitter
// refused to translate one. This had NO counter: the refusal was folded into
// the `gpu_fetch = ... && vs_translated->fetch_source` initialiser, so 243,162
// draws left the fetch path and the reasons listed here accounted for 40,146 of
// them. The other 203,000 were invisible -- one shader, refused for exp_adjust,
// carrying 30% of the frame. Same shape as the no-HANDLE ordering bug: a
// refusal with no counter reads as no refusal.
uint64_t g_gpuFetchNoVariant = 0;
// Streams whose window ran past the bound size and were shortened. NOT a
// refusal: the draw takes the GPU path, and the shader reads zero past
// RawFetch::limit. This replaces g_gpuFetchOutOfRange rather than sitting
// beside it -- that counter can no longer be incremented, and a counter that
// cannot fire reads as a measurement of zero instead of as dead code.
uint64_t g_gpuFetchClamped = 0;
// The clamp above is a magnitude with no denominator, and worse, it does not
// separate the two cases it covers. A window shortened by a few vertices loses
// a tail the shader zeroes; a window shortened to NOTHING loses the whole
// stream, and every fetch bound to it reads zero for every vertex. Those are
// the same counter today, which is why xe_vf[0] arriving as
// (base 0, stride 16, limit 0) on the tree billboards -- an entire vertex
// stream dropped -- produced no log line at all.
//
// Counted on every region considered, so the denominator is structural rather
// than the population of failures, and the first total drop is kept whole
// because one concrete case is what says which field is wrong.
uint64_t g_gpuFetchRegions = 0;
uint64_t g_gpuFetchDropped = 0;
uint32_t g_gpuFetchDropCases = 0;
struct GpuFetchDrop {
  bool seen = false;
  uint32_t handle = 0, stream = 0, stride = 0, size_bytes = 0, offset_bytes = 0;
  uint32_t first_vertex = 0, vertex_count = 0, live_size = 0;
};
GpuFetchDrop g_gpuFetchFirstDrop;
// Drops split by what the buffer object says NOW: `stale` is any disagreement
// with the snapshot, `live_holds` the subset the live size would have rescued.
uint64_t g_gpuFetchDropStale = 0;
uint64_t g_gpuFetchDropLiveHolds = 0;
uint64_t g_gpuFetchDropNoObject = 0;
// Vertices the CPU path zero-filled because the stream ran short, where it
// used to abandon the whole draw. Same rule as the clamp above, same reason.
uint64_t g_hleShaderZeroFilledVertex = 0;
// Attributes ReadVertexAttribute refused because they extend past the stride
// (shader_ucode.cpp:220). A different rule from the window bound, still fatal
// to the draw, counted apart so this change is judged on the window alone.
uint64_t g_hleShaderBadAttribute = 0;

// Where the frame goes. The guest's RenderPipeline call is measured whole in
// hooks_gameloop.cpp, which says the frame is slow but not which of our stages
// is spending it. These accumulate per frame and reset each frame, so the
// numbers are a frame's cost rather than a run's.
//
// Deliberately coarse -- three buckets and a total. A finer breakdown is worth
// having only once one bucket is known to dominate.
uint64_t g_phaseVertexUs = 0;   // ApplyShaderOutputs, all of it
uint64_t g_phaseInterpUs = 0;   // the software vertex shader inside it
// The texture bucket of this breakdown is g_tex.phaseUs, defined with the
// rest of the texture counters below.
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


TextureStats g_tex;

// WHICH textures re-decode, and WHY the cache did not hold them.
//
// The breakdown above got as far as "three decodes, 32 MB, every frame, 104ms"
// with a 99.6% hit rate on the other 1600 binds. That is the whole cost, and it
// is a property of three specific textures rather than of the path -- so the
// next thing needed is their identity and their miss reason, not another timer.
//
// Keyed by guest address, which survives across frames where the fetch-constant
// hash does not. That difference is itself a candidate answer: the cache key is
// FNV over all six fetch dwords, so one texture bound with two different
// sampler states is two entries and two decodes of the same bytes.
TexDecodeIndex g_texIndex;

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
      // THROTTLED 2026-08-26. This was every 250th attempt, which in run 1435
      // meant 1511 lines / 1.23MB -- 19% of the run by bytes and the largest
      // contributor once the two wide censuses were dealt with. `attempt` is a
      // draw counter, so the cadence tracked how BUSY the frame was, not
      // whether anything in the line had changed.
      //
      // Which fields actually move was MEASURED, not guessed -- diffing two
      // reports 526000 attempts apart in run 1435:
      //
      //   CLIMB every frame, routine:  applied, vertices, identity, viewport,
      //     VTE scale-on/off, tie-break disagreement, GPU vertex path qualify,
      //     skipped total, no-VTE, no-PS, depth-only, GPU FETCH draws,
      //     rectlist, CLAMPED, BUILD zero-filled.
      // TWO WRONG MEMBERS, both caught only by running it. Each one made the
      // sum move on nearly every draw, so the predicate was always true and the
      // line printed on nearly every draw -- ~250x WORSE than the modulo it
      // replaced (43795 lines / 152505 attempts in run 1436; 42889 / 293511 in
      // run 1437). A throttle whose predicate is always true is not a throttle.
      //
      //   run 1436: mx::hle::HleVertexZeroFillCount(), which is "BUILD
      //     zero-filled" and CLIMBS -- not "CPU zero-filled"
      //     (g_hleShaderZeroFilledVertex), which is the static one. The two sit
      //     adjacent in the format string with near-identical labels.
      //   run 1437: mx::hle::g_rectArrangement[0..2], which tracks rect draws
      //     and climbs in lockstep with the rectlist refusals right beside it.
      //
      // BOTH came from reading a label off a positional field-diff that only
      // resolved 29 of the ~35 fields -- a limitation I had already noticed and
      // then relied on anyway. The tail fields, which is exactly where both
      // offenders live, were never in that diff at all. What finally settled it
      // was grepping the RAW substring out of three consecutive prints and
      // watching "rect arrangement 0123" go 45918 -> 45919 -> 45920.
      //
      // So: if a member is ever added here, verify it against a real log by
      // reading the raw field, not by trusting a parse of the whole line.
      //
      // THE TRAP, paid for in run 1436: the format string has TWO adjacent
      // zero-fill counters with near-identical labels --
      //   "CPU zero-filled {} vertices"   g_hleShaderZeroFilledVertex   STATIC
      //   "BUILD zero-filled {} vertices" HleVertexZeroFillCount()      CLIMBS
      // The first cut of this signature took the climbing one. The sum then
      // changed on nearly every draw, so the line printed on nearly every draw:
      // 43795 lines over 152505 attempts, ~250x WORSE than the modulo it
      // replaced. A throttle whose predicate is always true is not a throttle,
      // and the measurement that would have caught it was already in hand --
      // BUILD zero-filled was in the CLIMB list below the whole time.
      //
      //   STATIC in a healthy run:  every no-code/decode/stream/constants/
      //     vertex refusal (0), VTE unreadable (0), all four live-shader
      //     outcomes (0), no-VS (0), VS-samplers (frozen at 4-5),
      //     too-many-inputs (0), GPU FETCH no-variant / ordinal-mismatch /
      //     unaligned (0), CPU zero-filled (0), attribute-past-stride (0),
      //     rect arrangement and degenerate.
      //
      //   NOT STATIC, and this list said it was: undeclared reg. Measured over
      //     run mx_1781 it climbs 2051 -> 3921. It is in the sum as a PRESENCE
      //     BIT for that reason -- see the note at the sum itself.
      //
      // How to check a member before adding one, since reading the body is what
      // failed both times: pull every occurrence of this line out of a real
      // log, extract the integers in format-string order, and diff consecutive
      // rows. A member that moves on most lines cannot be in the sum.
      //
      // The second group is the diagnostic payload: each one is zero or frozen
      // while things are working, and each one moving is genuine news. So the
      // line prints when THAT SUM changes, plus a 10s heartbeat so the climbing
      // totals still get sampled, plus the first ten attempts as before.
      //
      // The sum is a sound change-detector because every counter in it is
      // monotonically increasing: the total moves if and only if at least one
      // member moved. Two cannot cancel.
      //
      // VS-samplers frozen at 4 rather than 0 is the case that shows why the
      // frozen ones belong in here as well as the zero ones: a fifth appearing
      // is exactly the event worth a line.
      //
      // The statics are plain, matching every counter they read -- those are
      // non-atomic globals incremented from several draw threads already. The
      // worst a race here can do is duplicate or drop one log line.
      static uint64_t s_lastFailures = ~0ull;
      static std::chrono::steady_clock::time_point s_lastReport{};
      const uint64_t failures =
          g_hleShaderNoCode + g_hleShaderBadDecode + g_hleShaderBadStream +
          g_hleShaderBadConstants + g_hleShaderBadVertex + g_vteSeen[0] +
          g_liveVertexResolved + g_liveVertexNoMatch + g_liveVertexAmbiguous +
          g_liveVertexUnreadable +
          // PRESENCE, NOT MAGNITUDE -- and this is the second time this sum has
          // been broken by one climbing member. g_gpuVertexUndeclared went
          // 2051 -> 3921 across run mx_1781 and moved on 3922 of the 3943 lines
          // it printed, so the predicate was true on essentially every attempt
          // and the line cost 2.59 MB, 18% of the log. It is genuine news the
          // FIRST time an undeclared register appears and nothing after that,
          // which is exactly what a boolean says and a running total does not.
          // Its real value is still printed below; only the trigger changes.
          (g_gpuVertexUndeclared ? 1u : 0u) + g_gpuVertexNoVs +
          g_gpuVertexVsSamplers + g_gpuVertexTooManyInputs +
          g_gpuFetchNoVariant + g_gpuFetchOrdinalMismatch + g_gpuFetchUnaligned +
          g_hleShaderBadAttribute + g_hleShaderZeroFilledVertex +
          mx::hle::g_rectDegenerate.load();
      const auto now = std::chrono::steady_clock::now();
      // d3d9_diag_row_heartbeat is counted in DRAW REPORTS at its other two
      // sites and this one is per-attempt, so it is not a period here -- only
      // the 0 case carries over, meaning "drift never prints, changes always
      // do". The 10s heartbeat is the right unit for a per-attempt site and is
      // worth ~11 lines in a two-minute run.
      const bool drift_ok = REXCVAR_GET(d3d9_diag_row_heartbeat) > 0;
      if (attempt > 10 && failures == s_lastFailures &&
          (!drift_ok || now - s_lastReport < std::chrono::seconds(10)))
        return;
      s_lastFailures = failures;
      s_lastReport = now;
      REXLOG_INFO(
          "d3d9: HLE shader output attempts {}: applied {} draws / {} "
          "vertices; skipped no-code {} decode {} stream {} constants {} "
          "vertex {}; output transform identity {} viewport {} (VTE scale-on "
          "{} off {} unreadable {}, disagrees with old tie-break {}); live "
          "shader resolved {} no-match {} ambiguous {} unreadable {}; GPU "
          "vertex path {} draws qualify, {} skipped ({} undeclared reg, "
          "{} no-VS, {} VS-samplers, {} no-VTE, {} no-PS (of which {} "
          "depth-only, allowed), "
          "{} too-many-inputs); GPU FETCH {} draws (refused: no-variant {}, "
          "rectlist {}, "
          "ordinal-mismatch {}, unaligned {}; CLAMPED {} streams, CPU "
          "zero-filled {} vertices, attribute-past-stride {}); BUILD "
          "zero-filled {} vertices; rect "
          "arrangement 0123 {} / 1203 {} / 2013 {} (degenerate {})",
          g_hleShaderAttempts, g_hleShaderDraws, g_hleShaderVertices,
          g_hleShaderNoCode, g_hleShaderBadDecode, g_hleShaderBadStream,
          g_hleShaderBadConstants, g_hleShaderBadVertex,
          g_hleShaderIdentityMvp, g_hleShaderViewportMvp, g_vteSeen[2],
          g_vteSeen[1], g_vteSeen[0], g_hleShaderMvpDisagree,
          g_liveVertexResolved, g_liveVertexNoMatch, g_liveVertexAmbiguous,
          g_liveVertexUnreadable, g_gpuVertexDraws, g_gpuVertexSkipped,
          g_gpuVertexUndeclared, g_gpuVertexNoVs,
          g_gpuVertexVsSamplers, g_gpuVertexNoVte, g_gpuVertexNoPs,
          g_gpuVertexDepthOnly,
          g_gpuVertexTooManyInputs, g_gpuFetchDraws, g_gpuFetchNoVariant,
          g_gpuFetchRectList,
          g_gpuFetchOrdinalMismatch, g_gpuFetchUnaligned, g_gpuFetchClamped,
          g_hleShaderZeroFilledVertex, g_hleShaderBadAttribute,
          mx::hle::HleVertexZeroFillCount(),
          mx::hle::g_rectArrangement[0].load(),
          mx::hle::g_rectArrangement[1].load(),
          mx::hle::g_rectArrangement[2].load(),
          mx::hle::g_rectDegenerate.load());
    }
  } report{attempt};
  // Who is short, and by how much. Printed on the same cadence and NOT behind
  // hle_capture: the bare BUILD zero-fill total above is 304 million and says
  // nothing actionable, and the run that would have answered this was spent
  // guessing instead. `past-end` is the discriminator -- 1 vertex means a
  // buffer one short, thousands means the index does not address that stream.
  struct ReportZeroFill {
    uint64_t attempt;
    ~ReportZeroFill() {
      if (attempt > 10 && (attempt % 250) != 0) return;
      const auto& c = mx::hle::HleZeroFillCensus();
      std::string s;
      for (uint32_t i = 0; i < mx::hle::HleZeroFillCensusStreams; ++i) {
        const auto& st = c.stream[i];
        if (!st.fills) continue;
        s += fmt::format(
            " | s{} x{} worst-past-end {}v (first: stride {} size {} off {} "
            "index {} at {})",
            i, st.fills, st.worst_vertices_past_end, st.first.stride,
            st.first.size_bytes, st.first.offset_bytes, st.first.index,
            st.first.byte_off);
      }
      // The GPU fetch side of the same question. `dropped` is the one that
      // loses geometry outright: base == limit means every fetch bound to that
      // stream reads zero for every vertex, so the draw renders but its
      // vertices do not exist. The first case is printed whole because the
      // ratio alone cannot say which of stride/size/offset/first_vertex is the
      // wrong one.
      std::string g = " | gpu-fetch regions none";
      if (g_gpuFetchRegions) {
        g = fmt::format(" | gpu-fetch {} regions, {} clamped, {} DROPPED",
                        g_gpuFetchRegions, g_gpuFetchClamped,
                        g_gpuFetchDropped);
        const auto& d0 = g_gpuFetchFirstDrop;
        if (d0.seen)
          g += fmt::format(
              " [snapshot stale {}, live size would hold {}, no object {}]"
              " (first: vs 0x{:08X} stream {} stride {} size {} live {} off {} "
              "first_vertex {} vertex_count {}, wanted {} bytes from {})",
              g_gpuFetchDropStale, g_gpuFetchDropLiveHolds,
              g_gpuFetchDropNoObject, d0.handle, d0.stream, d0.stride,
              d0.size_bytes, d0.live_size, d0.offset_bytes, d0.first_vertex,
              d0.vertex_count, uint64_t(d0.vertex_count) * d0.stride,
              uint64_t(d0.offset_bytes) + uint64_t(d0.first_vertex) * d0.stride);
      }
      // The fetch-constant override, reported unconditionally because it now
      // decides which bytes every draw reads. `unmatched` is the one to watch:
      // it means neither base composition fitted, so that stream silently kept
      // the old snapshot window.
      std::string f;
      {
        uint64_t used = 0, differ = 0;
        for (uint32_t i = 0; i < mx::hle::kMaxStreams; ++i) {
          used += g_fcCompared[i];
          differ += g_fcSizeDiffer[i];
        }
        f = fmt::format(
            " | fetch-constant size compared {} (differs from snapshot {}: "
            "larger {} smaller {} -- NOT applied), bad-type {}, "
            "unreadable {}"
            " | computed-index draws {} (CPU attrs left default {})",
            used, differ, g_fcSizeLarger, g_fcSizeSmaller, g_fcBadType,
            g_fcUnreadable, g_computedIndexDraws,
            mx::hle::HleComputedIndexSkips());
      }
      REXLOG_INFO(
          "d3d9: index conditioning: registers read {} draws, restart enabled "
          "{}, cut {} draws at {} markers{}{}{}",
          g_indexCondRead, g_indexCondResetOn,
          mx::hle::HleRestartCutDraws(), mx::hle::HleRestartCutCount(),
          s.empty() ? " | zero-fill: none" : s, g, f);
    }
  } zreport{attempt};
  if (!handle || !device) {
    ++g_hleShaderNoCode;
    return ShaderApplyResult::kNoCode;
  }

  // Prefer the exact capture made by PatchVertexShaderToMatchVertexDeclaration.
  // Shaders resident before that hook first fires may use the independently
  // validated live allocation fallback above.  The >=90% PM4 content match
  // remains diagnostic-only and is never a source for pixels.
  auto pi = g_patch.patched.find(handle);
  const PatchedCode* patchp =
      pi != g_patch.patched.end() && pi->second.resolved ? &pi->second : nullptr;
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
  // exp_adjust, unconditionally and once. The census below is behind hle_diag,
  // so the claim that this is "always 0 in this game" -- written into
  // shader_hlsl.cpp's refusal -- has only ever been checked on runs nobody
  // makes. VFETCH coverage says one shader IS refused for it. This is the line
  // that decides whether applying the scale changes anything at all: if it
  // never prints, the emitter and the CPU path are both inert and any
  // difference in the picture came from somewhere else.
  {
    static bool s_logged = false;
    if (!s_logged) {
      for (const auto& a : attrs) {
        if (a.exp_adjust == 0) continue;
        s_logged = true;
        REXLOG_INFO(
            "d3d9: VFETCH exp_adjust {} (scale {:g}) on vs 0x{:08X} fmt {} "
            "slot {} dest r{} -- this shader's fetches are scaled by a power "
            "of two that used to be dropped",
            a.exp_adjust, std::ldexp(1.0f, a.exp_adjust), handle, a.format,
            a.fetch_slot, a.dest_reg);
        break;
      }
    }
  }
  // NOT behind g_diag any more. This census is one line per DISTINCT
  // (register, swizzle, rounded, exp_adjust) combination, so its whole cost is
  // a handful of lines per run -- and it answers the question the emitter's own
  // comment says has never been checked: whether every vfetch really is indexed
  // by the vertex ID. Run mx_1827 produced a draw of 148 vertices whose stream
  // 1 holds FOUR (stride 16, size 64) -- a corner table that cannot be
  // addressed by vertex ID at all -- so the assumption now has a concrete
  // counter-example and this needs to be readable without a special run.
  {
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
  //    none of that has to be reproduced -- the rasterizer does it natively.
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
  //    fetch would fail pipeline creation rather than render wrongly -- but
  //    refusing here keeps the draw on a path that works instead of on one that
  //    silently produces no pipeline.
  //  - PA_CL_VTE_CNTL must say the hardware applies the viewport transform, so
  //    the shader's position export is clip space and dc.mvp is identity. The
  //    translated pipeline's vertex stage does not apply mvp at all -- neither
  //    the passthrough one nor the guest's -- so a draw needing the viewport
  //    inverse has nowhere to apply it. Measured, the register reads 0x43F on
  //    every draw in this game, so this refuses nothing today; it is here so
  //    that if it ever does not, the draw falls back rather than moves.
  const TranslatedShader* vs_translated = TranslatedVertexShader(handle);
  // Evaluated as separate tests rather than one `&&` chain so each refusal is
  // attributed. The chain short-circuits, so a draw refused for two reasons is
  // counted against the first -- read the counters as "the reason that fired",
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
    // A null pixel shader used to end the GPU vertex path outright, and that
    // cost this population the whole vertex stage: ~45 draws and ~21,000
    // vertices a frame on the software interpreter, 27-36ms of a FRAME COST
    // vertex bucket that is otherwise almost empty. They are the most expensive
    // vertices left on the CPU by a wide margin -- five times the per-vertex
    // cost of the ones the fetch path already took.
    //
    // What they ARE is the guest's DEPTH passes: SetPixelShader(NULL) is legal
    // for a pass that writes only depth, and the guest emits one 48-dword
    // program that writes position and exports no interpolators at all.
    //
    // So they need no pixel stage worth the name -- and crucially, no colour.
    // Measured over 70,000 of them in mx_1142: "colour+depth 54428, depth only
    // 15572, colour only 0 ... WOULD PAINT 0, masked off 54428". EVERY one that
    // binds a colour target has RB_COLOR_MASK 0. The renderer pairs this with a
    // depth-only stand-in pixel shader whose output is discarded by a zero
    // write mask it already applies.
    //
    // Gated on that rather than assumed: `paints_colour` is the renderer's own
    // `colorWrite` rule, spelled identically so the two cannot drift. A no-PS
    // draw that CAN write colour keeps the old refusal, because for that one
    // the stand-in would have to invent a colour -- which is the question the
    // plan's original design answered with a texture fetch and a white
    // modulation identity, and which this population never asks.
    ++g_gpuVertexNoPs;
    const bool paints_colour =
        (dc.om_seen & (1u << 0)) == 0 || (dc.colour_mask & 0xFu) != 0;
    if (paints_colour) {
      gpu_vertex = false;
    } else {
      ++g_gpuVertexDepthOnly;
    }
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
  bool gpu_fetch = gpu_vertex && vs_translated;
  if (gpu_fetch && !vs_translated->fetch_source) {
    gpu_fetch = false;
    ++g_gpuFetchNoVariant;
  }
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
    // One past each region's valid bytes. Separate from region_of_stream
    // because several attributes commonly share one stream and each RawFetch
    // needs the same limit, and because a clamped region is shorter than
    // vertex_count * stride so the end cannot be re-derived from the base.
    uint32_t limit_of_stream[kMaxStreams];
    std::memset(region_of_stream, 0xFF, sizeof(region_of_stream));
    std::memset(limit_of_stream, 0, sizeof(limit_of_stream));
    // Which streams cannot be windowed by the draw's vertex range.
    //
    // A fetch indexed by r0.x is indexed by the VERTEX, so copying only
    // [first_vertex, first_vertex + vertex_count) and rebasing the index to 0
    // is exact. A fetch indexed by any other register is indexed by something
    // the shader COMPUTED -- an absolute row in a per-object table, 0..6777 for
    // the foliage -- and that number has no relationship to the draw's vertex
    // range. Windowing such a stream is what produced every dropped region:
    // `offset + first_vertex * stride` ran past the buffer while the index the
    // shader would actually use sat comfortably inside it.
    //
    // Xenia never windows at all -- it makes the whole fetch-constant range
    // resident (d3d12_command_processor.cc:3105) and lets the index land where
    // it lands. That is done here only for the streams that need it, because
    // the whole-stream copy is 325KB for the foliage against 7KB for a window,
    // and the census says exactly one fetch form in this title is affected.
    // Taken from the TRANSLATOR, which tracked ALU writes while walking the
    // instruction stream. Recomputing it here from attrs[] was the bug:
    // DecodeVertexShaderFetches records the index REGISTER, and testing
    // `src_reg == 0 && swizzle == 0` calls a fetch vertex-indexed whenever
    // it reads r0.x -- true at shader entry, false once the shader has
    // written it. The billboard shaders compute BOTH their indices into
    // r0.x (corner = vid % 4, instance = vid / 4), so every one of their
    // fetches was misclassified, windowed at first_vertex, and dropped.
    bool whole_stream[kMaxStreams] = {};
    for (size_t a = 0; a < attrs.size(); ++a)
      if (a < 32 && ((vs_translated->computed_index_fetches >> a) & 1u))
        whole_stream[attr_stream[a]] = true;
    dc.raw_vertex_bytes.clear();
    dc.raw_fetch_count = 0;
    // CONTENT KEY for the merged buffer, so the renderer can reuse one
    // upload allocation across draws that build identical bytes.
    //
    // Only whole-stream regions are keyed. A windowed region depends on
    // first_vertex, which differs per draw, so its bytes are not shared and
    // a key would be a lie; any windowed region sets the key to 0, meaning
    // "do not reuse". That is the conservative direction: a missed reuse
    // costs a copy, a wrong reuse draws stale geometry.
    //
    // The generation is part of the key, so a guest Unlock on any of these
    // buffers changes it and the next draw copies afresh.
    uint64_t raw_key = 1469598103934665603ull;
    bool raw_key_valid = true;
    auto mix = [&raw_key](uint64_t v) {
      raw_key = (raw_key ^ v) * 1099511628211ull;
    };
    for (size_t a = 0; a < attrs.size() && gpu_fetch; ++a) {
      if (attrs[a].fetch_slot != vs_translated->vertex_fetch_slot[a]) {
        gpu_fetch = false;
        ++g_gpuFetchOrdinalMismatch;
        break;
      }
      const uint32_t si = attr_stream[a];
      const mx::hle::HleStream& s = streams[si];
      if (region_of_stream[si] == 0xFFFFFFFFu) {
        const uint64_t start =
            whole_stream[si] ? uint64_t(s.offset_bytes)
                             : uint64_t(s.offset_bytes) +
                                   uint64_t(dc.first_vertex) * s.stride;
        const uint64_t want =
            whole_stream[si]
                ? (s.size_bytes > s.offset_bytes
                       ? uint64_t(s.size_bytes) - s.offset_bytes
                       : 0)
                : uint64_t(dc.vertex_count) * s.stride;
        // This used to refuse the draw outright when the window ran past the
        // stream, on the grounds that a clamp would read "whatever follows the
        // buffer". That was the right call while the shader had no bound to
        // check -- but it cost the draw entirely, and the hardware does not do
        // that: an over-long vertex fetch reads zero and the draw still
        // renders (metal_command_processor.cc:2377-2382). Now that
        // RawFetch::limit gives the shader the bound, the window is clamped to
        // what is actually readable and the shader zeroes the rest.
        //
        // Copying `bytes` rather than `want` also closes a latent host-side
        // overrun: the old insert() below read s.host + start + want, past the
        // guest mapping, in exactly the case it was refusing.
        //
        // start >= size_bytes falls out correctly with no special case: avail
        // and bytes are 0, the region is empty, limit == base, and every fetch
        // in the shader reads zero.
        const uint64_t avail =
            s.size_bytes > start ? uint64_t(s.size_bytes) - start : 0;
        const uint64_t bytes = std::min(want, avail);
        ++g_gpuFetchRegions;
        if (bytes < want) ++g_gpuFetchClamped;
        if (!bytes && want) {
          ++g_gpuFetchDropped;
          // Is our snapshot stale? size_bytes was read from the buffer object
          // at SetStreamSource; nothing is hooked that would tell us the guest
          // re-pointed or resized it since. Re-read the object's own size
          // field NOW and ask whether the live one would have held this
          // window. A high rescue count means the bug is the snapshot, not the
          // guest over-indexing -- and those want opposite fixes, which is why
          // this is measured before either is attempted.
          if (s.buffer_obj) {
            const uint32_t d1 = REX_LOAD_U32(s.buffer_obj + 0x1C);
            const uint64_t live = uint64_t((d1 >> 2) & 0xFFFFFFu) * 4;
            if (live != s.size_bytes) ++g_gpuFetchDropStale;
            if (live >= start + want) ++g_gpuFetchDropLiveHolds;
            if (!g_gpuFetchFirstDrop.seen) g_gpuFetchFirstDrop.live_size =
                uint32_t(live);
          } else {
            ++g_gpuFetchDropNoObject;
          }
          // THE DECISIVE CASE. The size theory is dead (see
          // ApplyDeviceFetchConstant: the device's own size is never larger),
          // so what is left is whether `first_vertex` -- the MINIMUM index in
          // the conditioned index buffer -- is real. Print every bound stream
          // of the failing draw beside it: if another stream comfortably holds
          // index first_vertex+vertex_count while this one cannot, the guest is
          // fetching one attribute from a buffer sized for fewer vertices and
          // the hardware zero-fill we emulate is correct. If NO stream holds
          // it, the index range itself is wrong and the fault is on the index
          // path, not the vertex one. One printed case separates those; a
          // counter cannot.
          // Keyed by SHADER, not "the first N drops". The first three cases
          // of run mx_1828 were all one shader (0x21689720, two fetch slots),
          // which is not the tree billboard VS at all -- that one has three.
          // Reading them as representative of a 95,000-drop population was the
          // wrong-population mistake: what is needed is one case per distinct
          // producer, so the shader losing the foliage can be seen among them.
          static std::map<uint64_t, bool> s_dropSeen;
          const uint64_t dkey = (uint64_t(handle) << 8) | si;
          if (s_dropSeen.size() < 12 && s_dropSeen.emplace(dkey, true).second) {
            std::string all;
            for (uint32_t k = 0; k < kMaxStreams; ++k) {
              if (!streams[k].bound || !streams[k].stride) continue;
              all += fmt::format(
                  " s{}(stride {} size {} off {} -> {} verts)", k,
                  streams[k].stride, streams[k].size_bytes,
                  streams[k].offset_bytes,
                  streams[k].size_bytes / streams[k].stride);
            }
            REXLOG_INFO(
                "d3d9: DROP CASE vs 0x{:08X} slot {} stream {}: indices "
                "[{}..{}] ({} verts, {} indices) need {} verts; index src "
                "r{}.{} rounded={}; fetches {}, draws-since-bind {};{}",
                handle, attrs[a].fetch_slot, si, dc.first_vertex,
                dc.first_vertex + dc.vertex_count - 1, dc.vertex_count,
                dc.index_count, dc.first_vertex + dc.vertex_count,
                attrs[a].src_reg, "xyzw"[attrs[a].src_swizzle & 3],
                attrs[a].is_index_rounded ? 1 : 0, attrs.size(),
                g_drawsSinceBind[si], all);
          }
          if (!g_gpuFetchFirstDrop.seen) {
            g_gpuFetchFirstDrop.handle = handle;
            g_gpuFetchFirstDrop.stream = si;
            g_gpuFetchFirstDrop.stride = s.stride;
            g_gpuFetchFirstDrop.size_bytes = s.size_bytes;
            g_gpuFetchFirstDrop.offset_bytes = s.offset_bytes;
            g_gpuFetchFirstDrop.first_vertex = dc.first_vertex;
            g_gpuFetchFirstDrop.vertex_count = dc.vertex_count;
            g_gpuFetchFirstDrop.seen = true;
          }
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
        limit_of_stream[si] = uint32_t(dc.raw_vertex_bytes.size());
        if (whole_stream[si]) {
          mix(s.buffer_obj);
          mix(uint64_t(start) << 32 | bytes);
          mix(VbGeneration(s.buffer_obj));
        } else {
          raw_key_valid = false;
        }
      }
      auto& rf = dc.raw_fetch[dc.raw_fetch_count++];
      // A whole-stream region starts at the buffer's own origin, so a fetch
      // that IS indexed by the vertex has to skip forward to where its window
      // would have begun -- the shader still rebases its index to 0. A fetch
      // indexed by a computed register addresses from the origin directly.
      const bool by_vertex =
          a >= 32 || !((vs_translated->computed_index_fetches >> a) & 1u);
      rf.base = region_of_stream[si];
      if (whole_stream[si] && by_vertex)
        rf.base += uint32_t(uint64_t(dc.first_vertex) * s.stride);
      rf.stride = s.stride;
      rf.endian = s.endian;
      rf.limit = limit_of_stream[si];
    }
    if (!gpu_fetch) {
      dc.raw_vertex_bytes.clear();
      dc.raw_fetch_count = 0;
    }
    dc.raw_vertex_key =
        (gpu_fetch && raw_key_valid && !dc.raw_vertex_bytes.empty())
            ? raw_key
            : 0;

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
    dc.vertex_shader_dxbc = vs_translated->fetch_dxbc;
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
    dc.vertex_shader_dxbc = vs_translated->dxbc;
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
  // host vertices were never transcoded -- and everything from here down reads
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
    // memcpy to nullptr -- an access violation writing address 0, which is
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
  // and every export that came out -- not only the one the texture profile
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
        // Short window: zero what is missing rather than abandoning the draw.
        // The same rule and the same reason as the GPU clamp above -- this is
        // the fallback those refusals used to land in, so leaving it dropping
        // would keep every rectlist and no-PS draw dying on a bound the
        // hardware does not enforce. Zeroing the WHOLE stride first means a
        // partially readable vertex reads its valid bytes and zeros past them,
        // which is what the shader now does on the GPU side.
        const uint64_t avail =
            s.size_bytes > byte_off ? uint64_t(s.size_bytes) - byte_off : 0;
        const uint32_t copy =
            uint32_t(std::min<uint64_t>(avail, s.stride));
        if (copy < s.stride) {
          std::memset(vtx[si], 0, s.stride);
          ++g_hleShaderZeroFilledVertex;
        }
        if (copy) std::memcpy(vtx[si], s.host + byte_off, copy);
        have[si] = true;
      }
      float f[4] = {0, 0, 0, 1};
      if (!ReadVertexAttribute(vtx[si], s.stride, attrs[a], s.endian, f)) {
        // A different rule: the attribute extends past the STRIDE, not past
        // the buffer (shader_ucode.cpp:220). Left fatal on purpose so this
        // change is judged on the window bound alone; counted apart so its
        // share is visible rather than folded into the number that should now
        // be falling.
        ++g_hleShaderBadAttribute;
        return ShaderApplyResult::kFailed;
      }
      values[a] = {f[0], f[1], f[2], f[3]};
    }

    // Merge the attributes into their destination registers, by exactly the
    // rule shader_alu.cpp:614 seeds its register file with -- three bits per
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
    // 0x300 -- XYZ already multiplied by 1/W0. Measured, that register lives at
    // device+10572 and reads 0x400 or 0x43F, never 0x300, and bits 8 and 9
    // (VTX_XY_FMT, VTX_Z_FMT) -- the ones that would mean "already divided" --
    // are clear in every sample. Dropping w scaled all 3D geometry by whatever
    // it should have divided by, which is why the front end's pre-transformed
    // 2D (w = 1) was unaffected while everything else blew up.
    //
    // Dividing here instead is not enough either, and the earlier note about
    // this "clipping the entire coloured scene away" is the reason: 1.2 million
    // vertices per run carry w <= 0, behind the eye. A negative w mirrors the
    // vertex through the origin rather than removing it, so those triangles
    // must be clipped against the near plane *before* any divide. D3D12 does
    // exactly that, in hardware, given clip space -- so give it clip space.
    const float p[4] = {r.position[0], r.position[1], r.position[2], w};
    std::memcpy(transformed.data() + size_t(v) * dc.vertex_stride, p,
                sizeof(p));

    // The shader's own interpolators, verbatim, for the translated pixel path.
    // No reconstruction and no viewport transform: these are the values the
    // pixel shader's registers are seeded with on the hardware, and the
    // rasterizer interpolates them. The param_gen synthesis further down is a
    // separate thing -- it fabricates the ONE interpolator the hardware
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
      // count is the render target's, not the sampled texture's -- the divide
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
        // the whole file is blank -- the second would indict the read, not the
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
        // saturated index as with a real c255 -- and only the instruction words
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
  // -90.0, 90.0, 1.0 -- PA_CL_VPORT_XSCALE/XOFFSET/YSCALE/YOFFSET/ZSCALE
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

// THE PAGE-TABLE UPDATE IS GATED, AND THIS READS THE GATE.
//
// sub_82AF5D38 is the guest's virtual-texture page-table update: it walks the
// feedback buffer and writes 16-bit entries across the mip pyramid. Its entire
// body sits behind a change-latch:
//
//     if (dword_830B334C != *(*(dword_830BE400 + 16) + 84)) { ...update... }
//
// dword_830B334C is read and written NOWHERE ELSE in the image -- 2 xrefs,
// both inside that function -- so it is purely "the value we last acted on".
// If the counter it compares against never moves, the page table is never
// refined and keeps the state we observe: every entry 0xF00A, the guest's own
// not-available marker (sub_82AF5D38 stamps that top nibble with
// `*v132 |= 0xF000u`).
//
// No hook is needed to watch this. Both are FIXED guest globals -- imagebase
// 0x82000000, no ASLR here -- so the host can read them straight out of guest
// memory once a frame. That matters because midasm hooks are disabled.
//
// Reported unconditionally with its denominator, and NOT folded into the
// FRAME COST block below, which is gated on cost thresholds: a cheap frame
// would print nothing and "the gate never fired" would be indistinguishable
// from "the probe never ran".
//
// Two outcomes, and both are useful:
//   latch never moves  -> the gate never fires; chase the counter's producer
//   latch moves        -> the update DOES run and writes only sentinels, and
//                         the question becomes what our feedback contains
void ReportPageTableLatch(uint8_t* base) {
  constexpr uint32_t kLatchAddr = 0x830B334Cu;  // dword_830B334C
  constexpr uint32_t kEngAddr = 0x830BE400u;
  const uint32_t latch = REX_LOAD_U32(kLatchAddr);
  const uint32_t eng = REX_LOAD_U32(kEngAddr);
  const uint32_t sub = eng ? REX_LOAD_U32(eng + 16) : 0;
  const uint32_t counter = sub ? REX_LOAD_U32(sub + 84) : 0;

  static uint64_t s_samples = 0;
  static uint64_t s_latchMoves = 0;
  static uint64_t s_counterMoves = 0;
  static uint32_t s_lastLatch = 0;
  static uint32_t s_lastCounter = 0;
  static bool s_have = false;

  ++s_samples;
  if (s_have) {
    if (latch != s_lastLatch) {
      ++s_latchMoves;
      if (s_latchMoves <= 8)
        REXLOG_INFO("d3d9: PAGE TABLE LATCH moved 0x{:08X} -> 0x{:08X} "
                    "(counter 0x{:08X}) -- the guest ran its page-table update",
                    s_lastLatch, latch, counter);
    }
    if (counter != s_lastCounter) ++s_counterMoves;
  }
  s_lastLatch = latch;
  s_lastCounter = counter;
  s_have = true;

  // Every 300 frames. The chain is printed whole -- eng, eng+16, +84 -- so a
  // zero can be read as "the chain is null" rather than "the counter is zero",
  // which are different failures.
  if ((s_samples % 300) == 0)
    REXLOG_INFO("d3d9: PAGE TABLE LATCH census: {} samples, latch moved {}, "
                "counter moved {} | latch 0x{:08X} counter 0x{:08X} "
                "(eng 0x{:08X} +16 0x{:08X})",
                s_samples, s_latchMoves, s_counterMoves, latch, counter, eng,
                sub);
}

void FinalizePendingD3D9DrawsImpl(uint8_t* base) {
  ReportPageTableLatch(base);
  const size_t count = g_pendingHleDraws.size();
  uint64_t applied = 0, dropped = 0;
  const auto finalize_t0 = std::chrono::steady_clock::now();
  for (PendingHleDraw& pending : g_pendingHleDraws) {
    // A resolve carries no geometry, so it has no shader to run and no topology
    // to finalize -- both would refuse it and it would be counted as a dropped
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
  if (finalize_us >= 20000 || g_tex.phaseUs >= 20000 ||
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
                g_tex.phaseUs / 1000, finalize_us / 1000);
    // Inside the texture bucket. Printed beside it for the same reason as
    // LOOP BY REASON below: the parts must be checkable against the total
    // rather than trusted on their own. describe + stale + copy + decode +
    // scan should account for most of `texture Nms`; a large remainder means
    // the cost is somewhere none of these five timers is watching.
    REXLOG_INFO(
        "d3d9: TEXTURE COST {} slot calls -- describe {}ms, stale-check {}ms, "
        "copy {}ms, decode {}ms, scan {}ms | {} cache hits, {} stale evictions,"
        " {} decodes over {} KB | flat-marked {} retried {} volatile {}",
        g_tex.slotCalls, g_tex.describeUs / 1000, g_tex.staleUs / 1000,
        g_tex.copyUs / 1000, g_tex.decodeUs / 1000, g_tex.scanUs / 1000,
        g_tex.cacheHits, g_tex.staleEvicts, g_tex.decodes,
        g_tex.decodedBytes / 1024,
        // The readout for the flat-retry backoff. Without it a census row of
        // `n=2 flat=2` cannot say whether the retry never fired or fired and
        // found the texture still flat -- completely different diagnoses, and
        // the exact hole this log exists to close.
        g_flatNotCached, g_flatRetriesDue, g_flatVolatile);
    // The repeat offenders, cumulative, worst first. Three textures own this
    // whole bucket; this names them and says why each one misses.
    {
      std::vector<std::pair<uint32_t, const TexDecodeSite*>> worst;
      worst.reserve(g_texIndex.sites.size());
      for (const auto& [addr, s] : g_texIndex.sites) worst.emplace_back(addr, &s);
      std::sort(worst.begin(), worst.end(), [](const auto& a, const auto& b) {
        return a.second->bytes > b.second->bytes;
      });
      // The population is the site count AND the identity of the top five,
      // because a new address entering the list is the event worth a line even
      // when no address was added. Byte totals climbing behind a stable top
      // five is drift and belongs to the heartbeat.
      uint64_t pop = uint64_t(g_texIndex.sites.size());
      for (size_t i = 0; i < worst.size() && i < 5; ++i)
        pop = pop * 1000003ull + worst[i].first;
      static uint64_t s_lastTexPop = 0;
      static uint32_t s_sinceTex = 0;
      if (!RowDumpDue(pop, s_lastTexPop, s_sinceTex)) {
        REXLOG_INFO("d3d9: TEXTURE REPEATS {} addresses -- rows held, same "
                    "worst five ({} report(s) so far; "
                    "d3d9_diag_row_heartbeat={})",
                    g_texIndex.sites.size(), s_sinceTex,
                    REXCVAR_GET(d3d9_diag_row_heartbeat));
      } else {
        std::string top;
        for (size_t i = 0; i < worst.size() && i < 5; ++i) {
          const TexDecodeSite& s = *worst[i].second;
          top += fmt::format(
              " [0x{:08X} {}x{} fmt{} {}x={}MB keys={} miss:{}nocache/{}stale/"
              "{}blank]",
              worst[i].first, s.width, s.height, s.format, s.decodes,
              s.bytes / (1024 * 1024), s.distinct_keys, s.by_reason[0],
              s.by_reason[1], s.by_reason[2]);
        }
        REXLOG_INFO("d3d9: TEXTURE REPEATS {} addresses:{}",
                    g_texIndex.sites.size(), top.empty() ? " (none)" : top);
      }
    }
    // RESOLVE CONSUMPTION. Every destination the guest has resolved into, and
    // whether it ever asked for it back through SetTexture.
    //
    // The whole population, with the total printed first, and NOT gated on
    // there being orphans -- "0 orphans of 14" and "no line at all" have to be
    // distinguishable, or this is another counter that cannot report. Orphans
    // are listed by name because the interesting one is a specific surface: the
    // menu backdrop is a 1280x430 resolve that mx_1288 produced exactly once
    // and nothing sampled.
    {
      // THROTTLED 2026-08-26, and the reason is worth keeping. These two are
      // the widest lines in the log: RESOLVE CONSUMPTION is 6181 bytes per
      // line, and at 422 fires over 2067 frames the pair accounted for 3.6MB
      // of run 1434's 10.8MB. That is what rotates the log away every ~30
      // seconds, which is how three empty greps nearly became a false
      // conclusion about the FE_Smoke gate never firing.
      //
      // They inherit the enclosing cost trigger, which is right for FRAME COST
      // -- that line is ABOUT the slow frame it fires on. These two are not.
      // They are cumulative whole-population snapshots, identical on
      // consecutive slow frames apart from monotonic counter drift.
      //
      // So the fire condition is: print when the POPULATION or the FINDING
      // changes, plus a heartbeat. Every property the original comments
      // defend survives --
      //   - the first report always prints, so "0 orphans of 14" and "no line
      //     at all" stay distinguishable;
      //   - a new destination or video row appearing prints immediately;
      //   - an orphan / asked-but-lost / bound-never-drawn count changing
      //     prints immediately, and that is the diagnostic payload;
      //   - drift alone waits for the heartbeat.
      //
      // The signature is computed BEFORE the row strings are built, and the
      // strings are built ONLY when printing. That is most of the saving: this
      // runs on the render thread and formatted ~9KB per slow frame whether or
      // not anything had changed.
      constexpr auto kCensusHeartbeat = std::chrono::seconds(10);

      // VIDEO TARGET CONSUMPTION. Every texture bound at one of the three
      // _VideoRenderTarget extents, by base address.
      //
      // The whole population. "0 rows" means the guest never binds a texture at
      // any of those extents at all -- a completely different finding from
      // "rows exist and none of them draw", and the two must not collapse into
      // the same silence.
      //
      // 1280x720 is also the scene render-target extent, so a row at that shape
      // is not on its own the video asset. Read the ADDRESSES: FE_Smoke's
      // 1280x430 resolve lands at phys 0x1BE95000.
      {
        std::lock_guard<std::mutex> lk(g_videoShapeMu);
        size_t bound_never_drawn = 0;
        for (const auto& kv : g_videoShapeRows) {
          const auto& r = kv.second;
          if (r.binds && !r.guest_draws_spanned) ++bound_never_drawn;
        }
        static uint64_t s_sig = ~0ull;
        static std::chrono::steady_clock::time_point s_last{};
        const auto now = std::chrono::steady_clock::now();
        const uint64_t sig = (uint64_t(g_videoShapeRows.size()) << 40) ^
                             (uint64_t(bound_never_drawn) << 20) ^
                             uint64_t(g_videoShapeDropped);
        if (sig != s_sig || now - s_last >= kCensusHeartbeat) {
          s_sig = sig;
          s_last = now;
          std::string vrows;
          for (const auto& [addr, r] : g_videoShapeRows) {
            vrows += fmt::format(
                " [0x{:08X} {}x{} obj0x{:08X} bind{} smp{:#x} seen{} span{}/{}win "
                "dev0x{:08X} bt{}]",
                addr, r.width, r.height, r.last_object, r.binds, r.sampler_mask,
                r.slot_seen, r.guest_draws_spanned, r.bind_windows, r.last_device,
                r.last_thread);
          }
          REXLOG_INFO("d3d9: VIDEO TARGET CONSUMPTION {} addresses at a "
                      "_VideoRenderTarget extent, {} binds total, {} bound but "
                      "never drawn with, {} rows dropped (map full) --{}",
                      g_videoShapeRows.size(), g_videoShapeBinds,
                      bound_never_drawn, g_videoShapeDropped,
                      vrows.empty() ? " (none)" : vrows);
        }
      }

      // RESOLVE CONSUMPTION. Every destination the guest has resolved into, and
      // whether it ever asked for it back through SetTexture. Orphans are
      // listed by name because the interesting one is a specific surface: the
      // menu backdrop is a 1280x430 resolve that mx_1288 produced exactly once
      // and nothing sampled.
      size_t orphans = 0, asked_but_lost = 0;
      for (const auto& kv : g_resolvedTargetsByAddress) {
        const auto& e = kv.second;
        if (!e.set_texture_binds) ++orphans;
        // The row that matters: the guest asked for it and no draw slot ever
        // saw it. That is a binding WE lose, and it is invisible to every other
        // counter here.
        if (e.set_texture_binds && !e.slot_seen) ++asked_but_lost;
      }
      {
        static uint64_t s_sig = ~0ull;
        static std::chrono::steady_clock::time_point s_last{};
        const auto now = std::chrono::steady_clock::now();
        const uint64_t sig =
            (uint64_t(g_resolvedTargetsByAddress.size()) << 40) ^
            (uint64_t(orphans) << 20) ^ uint64_t(asked_but_lost);
        if (sig != s_sig || now - s_last >= kCensusHeartbeat) {
          s_sig = sig;
          s_last = now;
          std::string rows;
          for (const auto& [addr, e] : g_resolvedTargetsByAddress) {
            rows += fmt::format(
                // REACHED, printed next to the extent it is judged
                // against. Without it this row cannot say WHY a
                // destination was claimed: the 2048x2048 ping-pong pair
                // reads `part0` here, and whether that means "the GPU
                // wrote all of it" or "the coverage entry was never
                // consulted" is the whole difference between a healthy
                // snapshot and one that samples black.
                " [0x{:08X} {}x{} cov{}% reach{}x{} {}res bind{} seen{} "
                "snap{} part{} smp{:#x} "
                "draws{} untrans{} declared{:#x} bt{} span{}/{}win]",
                addr, e.width, e.height, e.coverage_percent(),
                e.reached_x, e.reached_y,
                e.resolves, e.set_texture_binds,
                e.slot_seen, e.slot_snapshot, e.slot_partial,
                e.bind_sampler_mask, e.draws_while_bound,
                e.draws_no_translation, e.declared_sampler_mask,
                e.last_bind_thread, e.guest_draws_spanned, e.bind_windows);
          }
          std::string draw_threads;
          for (const auto& slot : g_drawThreadIds) {
            if (const uint32_t tid = slot.load(std::memory_order_relaxed))
              draw_threads += fmt::format(" {}", tid);
          }
          REXLOG_INFO("d3d9: RESOLVE CONSUMPTION {} destinations, {} never "
                      "asked for, {} asked for but never reached a draw slot; "
                      "draw threads:{} --{}",
                      g_resolvedTargetsByAddress.size(), orphans,
                      asked_but_lost,
                      draw_threads.empty() ? " none" : draw_threads,
                      rows.empty() ? " (none)" : rows);
        }
      }
    }
    // Bink plane preparation, whole population, NOT gated on non-zero: a run
    // where the composite is never attempted and one where it is attempted and
    // always refused are different diagnoses and must not print the same
    // nothing. The parts sum to `calls`.
    {
      const BinkPlaneRefusals b = BinkPlaneRefusalStats();
      REXLOG_INFO("d3d9: BINK PLANES {} calls = {} ok + {} no-fetch + {} "
                  "describe + {} copy + {} decode + {} too-few (first "
                  "no-fetch slot {})",
                  b.calls, b.ok, b.no_fetch, b.describe, b.copy, b.decode,
                  b.too_few,
                  b.first_fail_slot == 0xFFFFFFFFu
                      ? std::string("none")
                      : std::to_string(b.first_fail_slot));
    }
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
  g_phaseVertexUs = g_phaseInterpUs = g_tex.phaseUs = 0;
  g_phaseVertexLoopUs = g_phaseVertexCount = 0;
  // Per frame, like every other bucket here -- these are a frame's cost, not a
  // run's, and the report above has already consumed them.
  g_tex.describeUs = g_tex.staleUs = g_tex.copyUs = 0;
  g_tex.decodeUs = g_tex.scanUs = 0;
  g_tex.slotCalls = g_tex.cacheHits = g_tex.staleEvicts = 0;
  g_tex.decodes = g_tex.decodedBytes = 0;
  g_phaseDrawCount = 0;
}

//===========================================================================
// Emitter coverage.
//
// Measured before anything renders through it, because the whole plan rests on
// a claim that has not been tested: that a straight-line HLSL emitter can carry
// this game's shaders. If most of them refuse, the wiring downstream is worth
// nothing and the design has to change -- so the cheap decisive number comes
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
  // Shaders (not blocks) carrying a conditional exec we run unconditionally,
  // split by mechanism because the two need different fixes: p0 is evaluable
  // today, a bool constant is not (no bank exists). Counted at run level as
  // well as per dump, because the question "does this game predicate at all"
  // is answered by one number and was never asked.
  uint64_t predicated = 0;    // p0-gated, seen
  uint64_t p0_honoured = 0;   // --¦of which fully obeyed (all blocks emitted)
  uint64_t bool_gated = 0;    // bool-constant-gated
};
HlslCoverage g_hlslVs, g_hlslPs;
// map rather than set only because <map> is already included here and <set> is
// not; the value is unused.
// Handle -> a hash of the GUEST MICROCODE that handle carried when it was
// translated. Was `map<uint32_t, bool>`, i.e. "have we ever seen this handle",
// which is wrong because a handle is an ADDRESS.
//
// The guest frees shaders on a map unload and allocates the next map's at
// recycled addresses. With a bool, the second shader to land on an address was
// never translated at all -- ReportHlslCoverage returned early -- and
// g_translatedVs[handle] went on serving the PREVIOUS shader's translation.
// The draw then ran a faithful vertex shader against a faithful pixel shader
// from a DIFFERENT material, which is why the bike's tyre read fog out of a UV
// and came out a flat ramp: the pixel stage wants fog at interpolator 4 (a
// 5-export vertex variant, export_mask 0x1f) and the stale vertex shader was
// the 6-export one (0x3f) that puts fog at 5.
//
// Keyed on content, so a recycled address re-translates.
std::map<uint32_t, uint64_t> g_hlslReportedVs, g_hlslReportedPs;

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

// Persisted DXBC cache, keyed by the EMITTED HLSL rather than the shader object
// handle. Bink re-creates its shader objects for every video, so a handle-keyed
// cache misses at every video start and pays FXC again -- 18-145ms per shader at
// O0, which is the Bink-start hang. The handle-keyed maps above still serve
// same-object repeats; this one serves repeats across objects, across videos
// and across runs.
//
// Keyed on the SOURCE, not on the guest microcode it was translated from. The
// first version hashed the microcode, which is wrong in the one way that costs
// days: the cached bytes are the output of EmitShaderHlsl, so any change to the
// emitter leaves every already-cached shader loading its stale DXBC while the
// log reports a healthy hit rate. A translation fix would then render nothing
// and read as "no visual change" -- the exact symptom this project spends its
// time chasing. Hashing the source makes the key change whenever the emitter
// does, with no version stamp to remember to bump.
//
// Free to compute: the lookup sites below all run AFTER EmitShaderHlsl, so the
// source string is already in hand.
//
// Lives under userdata/cache, the canonical cache root, so nothing wipes it
// between runs (logs/hlsldump IS wiped, which is why it is not used).
namespace {
uint64_t g_dxbcCacheHits = 0;
uint64_t g_dxbcCacheMisses = 0;

uint64_t ShaderSourceKey(mx::hle::HlslStage stage, const std::string& source) {
  uint64_t h = 1469598103934665603ull;
  h ^= (stage == mx::hle::HlslStage::kPixel ? 0xA5A5ull : 0x3C3Cull);
  for (const char c : source) {
    h ^= uint64_t(uint8_t(c));
    h *= 1099511628211ull;
  }
  return h;
}

std::string ShaderCachePath(mx::hle::HlslStage stage, uint64_t key) {
  return fmt::format("userdata/cache/shaders/{}_{:016X}.dxbc",
                     stage == mx::hle::HlslStage::kPixel ? "ps" : "vs", key);
}

// The DXBC container declares its own total size at byte offset 24, after the
// 4-byte magic and a 16-byte digest. Validating THAT rather than the magic is
// the difference between catching a truncated file and waving it through.
//
// The magic alone was not enough. SaveShaderDxbc used to write straight to the
// final path with `trunc`, so a process killed mid-write left a file that was
// truncated BUT STILL STARTED WITH "DXBC" -- it passed validation on every
// later run and was handed to CreateGraphicsPipelineState as a corrupt blob.
// Self-perpetuating, too: nothing rewrites a cache entry that already exists,
// so one interrupted write poisons that shader until the file is deleted by
// hand. Exactly the shape of an intermittent, machine-specific crash.
//
// Measured before changing anything: 47 of 47 entries on this machine were
// intact, so this is a latent hazard being closed, not a live bug being fixed.
constexpr size_t kDxbcHeaderBytes = 32;
constexpr size_t kDxbcTotalSizeOffset = 24;

std::shared_ptr<const std::vector<uint8_t>> LoadShaderDxbc(
    mx::hle::HlslStage stage, uint64_t key) {
  std::ifstream f(ShaderCachePath(stage, key), std::ios::binary);
  if (!f) return nullptr;
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());
  if (bytes.size() < kDxbcHeaderBytes ||
      std::memcmp(bytes.data(), "DXBC", 4) != 0)
    return nullptr;
  uint32_t declared = 0;
  std::memcpy(&declared, bytes.data() + kDxbcTotalSizeOffset, sizeof(declared));
  if (declared != bytes.size()) {
    // Truncated or overlong. Remove it so the next run recompiles instead of
    // reading the same bad bytes forever, and say so -- a cache entry that
    // silently disappears is worse than one that explains itself.
    REXLOG_WARN("d3d9: DXBC cache entry {} declares {} bytes but is {}; "
                "discarding and recompiling",
                ShaderCachePath(stage, key), declared, bytes.size());
    std::error_code rm;
    std::filesystem::remove(ShaderCachePath(stage, key), rm);
    return nullptr;
  }
  return std::make_shared<const std::vector<uint8_t>>(std::move(bytes));
}

// Written to a temporary file and RENAMED into place, so the final path only
// ever holds a complete blob. rename is atomic within a directory, so a crash
// mid-write leaves a stray .tmp -- harmless, and not something Load will read
// -- rather than a half-written cache entry that validates.
//
// The temp name carries the thread id as well as the key because shaders
// compile on several worker threads and two of them may race on one key.
void SaveShaderDxbc(mx::hle::HlslStage stage, uint64_t key, ID3DBlob* blob) {
  if (!blob || !blob->GetBufferPointer() || blob->GetBufferSize() == 0) return;
  std::error_code ec;
  std::filesystem::create_directories("userdata/cache/shaders", ec);
  const std::string final_path = ShaderCachePath(stage, key);
  const std::string tmp_path =
      fmt::format("{}.{:X}.tmp", final_path, GetCurrentThreadId());
  {
    std::ofstream f(tmp_path, std::ios::trunc | std::ios::binary);
    if (!f) return;
    f.write(static_cast<const char*>(blob->GetBufferPointer()),
            std::streamsize(blob->GetBufferSize()));
    f.flush();
    // Only a fully written file earns the rename. A failed stream here leaves
    // the previous entry -- or no entry -- untouched.
    if (!f) {
      std::error_code rm;
      std::filesystem::remove(tmp_path, rm);
      return;
    }
  }
  std::error_code mv;
  std::filesystem::rename(tmp_path, final_path, mv);
  if (mv) {
    std::error_code rm;
    std::filesystem::remove(tmp_path, rm);
  }
}
}  // namespace

std::string HlslCoverageSummary(const HlslCoverage& c) {
  std::string s = fmt::format("{} translated+compiled", c.ok);
  // Deliberately NOT gated on non-zero: zero here is the finding that makes the
  // setp_* value translation safe, and a counter that vanishes when it reads
  // zero cannot report that.
  // Both printed unconditionally, zero included: an absent line would read as
  // "not measured" rather than "none", which is the failure this file keeps
  // hitting. P0 is fixable now; BOOL needs a constant bank first.
  s += fmt::format(", P0-EXEC={} (honoured {}), BOOL-EXEC={}", c.predicated,
                   c.p0_honoured, c.bool_gated);
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

// ASYNC SHADER COMPILATION.
//
// First-use translation used to run entirely on the GUEST thread that submitted
// the draw: emit (~0ms) + FXC (~143ms) + dump/disassembly (~26ms). A cold cache
// means dozens of those back to back -- run 1673 took 56 cache misses, about
// 8 seconds of guest-thread time -- and that stall is not merely slow, it
// CORRUPTS GUEST STATE by changing which thread wins a race.
//
// The 0x8234CE20 crash is exactly that. The guest's script thread advances with
// frames, the database worker drains its own ring regardless, and the front end
// loads its packages from the script. Stall the renderer and the script arrives
// late, so the worker constructs a BinkVideoComponent before the script has
// asked for the package holding its movie; the asset lookup misses, the NULL is
// cached at component+0x94 and dereferenced later with no null check. Measured
// both ways, same build:
//
//     run 1673 CRASH  BinkInitProps 17:33:35.095 -> lookup MISS
//                     LoadAssetPackage "Rider" 17:33:39.348
//     run 1676 OK     LoadAssetPackage "Rider" 17:40:27.654
//                     BinkInitProps 17:40:28.556 -> lookup FOUND
//
// The script wins by 0.9s when the cache is warm. So compilation has to come
// off the guest thread.
//
// HOW: on a cache MISS the job is handed to the thread below and the function
// returns WITHOUT installing. `TranslatedPixelShader` then keeps returning
// nullptr for that handle, which every consumer already handles -- it is the
// same state as a shader that failed to translate, and the draw takes the
// stand-in path for a few frames until the real one lands. A cache HIT still
// runs inline, because it costs nothing.
//
// The worker RE-ENTERS ReportHlslCoverage with t_shaderCompileWorker set, so
// there is one translation path rather than two that can drift apart. The
// `seen` early-out is skipped for it, and the enqueue is deduped for free by
// that same map: a second first-use of the same handle before the job finishes
// returns early and does not queue again.
//
// The maps were UNGUARDED and are read by every draw. A background writer makes
// that a real race rather than a latent one, so they are locked now. The lock
// is held only around find/insert; the returned pointer stays valid without it
// because std::map nodes are stable and nothing is ever erased.
std::mutex g_translatedMu;
thread_local bool t_shaderCompileWorker = false;

const TranslatedShader* TranslatedPixelShader(uint32_t handle) {
  std::lock_guard<std::mutex> lk(g_translatedMu);
  const auto it = g_translatedPs.find(handle);
  return it == g_translatedPs.end() ? nullptr : &it->second;
}

const TranslatedShader* TranslatedVertexShader(uint32_t handle) {
  std::lock_guard<std::mutex> lk(g_translatedMu);
  const auto it = g_translatedVs.find(handle);
  return it == g_translatedVs.end() ? nullptr : &it->second;
}

void ReportHlslCoverage(mx::hle::HlslStage stage, uint32_t handle,
                        const uint32_t* code, uint32_t count);

struct ShaderCompileJob {
  mx::hle::HlslStage stage = mx::hle::HlslStage::kPixel;
  uint32_t handle = 0;
  std::vector<uint32_t> code;
};

std::mutex g_compileMu;
std::condition_variable g_compileCv;
std::deque<ShaderCompileJob> g_compileQueue;
bool g_compileStop = false;
std::thread g_compileThread;
uint64_t g_compileQueued = 0;
uint64_t g_compileDone = 0;

void ShaderCompileWorker() {
  for (;;) {
    ShaderCompileJob job;
    {
      std::unique_lock<std::mutex> lk(g_compileMu);
      g_compileCv.wait(lk, [] { return g_compileStop || !g_compileQueue.empty(); });
      if (g_compileStop && g_compileQueue.empty()) return;
      job = std::move(g_compileQueue.front());
      g_compileQueue.pop_front();
    }
    t_shaderCompileWorker = true;
    ReportHlslCoverage(job.stage, job.handle, job.code.data(),
                       uint32_t(job.code.size()));
    t_shaderCompileWorker = false;
    std::lock_guard<std::mutex> lk(g_compileMu);
    ++g_compileDone;
  }
}

// Returns true when the job was taken, meaning the caller must NOT install and
// must return. False keeps the caller on the old inline path -- which is what
// the worker itself gets, and what happens if the thread could not start.
bool EnqueueShaderCompile(mx::hle::HlslStage stage, uint32_t handle,
                          const uint32_t* code, uint32_t count) {
  if (!code || !count) return false;
  std::lock_guard<std::mutex> lk(g_compileMu);
  if (g_compileStop) return false;
  if (!g_compileThread.joinable()) {
    try {
      g_compileThread = std::thread(ShaderCompileWorker);
    } catch (...) {
      return false;  // no thread: compile inline rather than never
    }
  }
  g_compileQueue.push_back({stage, handle, std::vector<uint32_t>(code, code + count)});
  ++g_compileQueued;
  g_compileCv.notify_one();
  return true;
}

void ReportHlslCoverage(mx::hle::HlslStage stage, uint32_t handle,
                        const uint32_t* code, uint32_t count) {
  auto& seen = stage == mx::hle::HlslStage::kPixel ? g_hlslReportedPs
                                                   : g_hlslReportedVs;
  // FNV-1a over the microcode. Content, not identity -- see g_hlslReportedVs.
  uint64_t code_key = 1469598103934665603ull;
  for (uint32_t i = 0; i < count; ++i) {
    code_key ^= code[i];
    code_key *= 1099511628211ull;
  }
  // Skipped on the compile worker: the guest thread already recorded this
  // handle before handing the job over, so the worker would take the
  // already-seen early-out and never compile anything.
  if (!t_shaderCompileWorker) {
    std::lock_guard<std::mutex> lk(g_translatedMu);
    const auto [it, inserted] = seen.emplace(handle, code_key);
    if (!inserted) {
      // Same address, same code: already translated OR a compile is in flight
      // for it. Either way nothing to do -- and this is what dedupes the queue.
      if (it->second == code_key) return;
      // Same address, DIFFERENT code: the guest reused it for another shader.
      // Fall through and re-translate, overwriting g_translatedVs[handle].
      it->second = code_key;
      ++g_shaderHandleRecycled;
    }
  }
  auto& cov = stage == mx::hle::HlslStage::kPixel ? g_hlslPs : g_hlslVs;
  // First-use cost split: translation vs FXC vs the dump/disassembly tail.
  //
  // Written to find the phase that stalled Bink start; that turned out to be
  // FXC at 18-145ms per shader, which the persisted cache below now absorbs.
  // KEPT rather than removed, because it is the only thing that can show that
  // cache regressing: a run where `compile` goes back to tens of milliseconds
  // per shader means the cache is missing, and the hits/misses line alone
  // cannot distinguish "missing" from "nothing to hit yet".
  //
  // Costs three steady_clock reads per NEW shader -- this function runs once per
  // handle, not once per draw -- and the log line is capped at the first eight
  // of each stage.
  const auto t_first_use = std::chrono::steady_clock::now();
  auto t_emit = t_first_use, t_compile = t_first_use;

  mx::hle::HlslShader out;
  // MUST match the width the renderer's vertex stage offers, or the two
  // signatures cannot link and pipeline creation fails with no message. See
  // kHlslInterpolatorLinkage.
  mx::hle::EmitShaderHlsl(code, count, stage,
                          mx::hle::kHlslInterpolatorLinkage, out);
  t_emit = std::chrono::steady_clock::now();

  // ZERO-EXPORT census: pixel shaders that name a colour target and never
  // assign it, so the target compiles to `mov o0.xyzw, l(0, 0, 0, 0)`.
  //
  // Placed at the ONE translate site rather than in the dump path, because the
  // dump is capped and would sample whichever shaders happen to be dumped --
  // the same first-N error that made the 35-index OM probe read like an answer.
  // Every shader the game translates passes through here exactly once.
  //
  // Reported unconditionally, zero included, so "this never happens" and "the
  // probe never ran" stay distinguishable. A zero here kills the hypothesis
  // that our translation is what blacks out the menu background, and moves the
  // question back to the guest's own blend state.
  // MEMORY EXPORT census, BOTH STAGES. Counted here rather than only for
  // pixel shaders because memexport is a vertex-stage idiom -- a shader that
  // writes guest memory instead of a render target.
  //
  // The question this answers: the terrain's virtual-texture PAGE TABLE is
  // uniform in guest memory (every one of its 1048576 entries identical, and
  // still identical after 230 forced re-reads), while the tile ATLAS beside it
  // is correctly populated with 39 streamed tiles. Something fills the atlas
  // and nothing fills the page table. Memory export is one way the guest could
  // be writing it that we do not implement -- and until now could not even
  // observe, because the drop was recorded under `if (dest < 32)` and the
  // memexport registers are 32..37.
  //
  // Reported unconditionally, zero included. A zero is the useful answer here:
  // it RULES OUT memexport and sends the page table back to the CPU-write
  // path, which is a different search. An unreported zero would rule out
  // nothing.
  {
    static std::atomic<uint64_t> s_translated{0};
    static std::atomic<uint64_t> s_withMemexport{0};
    static std::atomic<uint64_t> s_memexportOps{0};
    const uint64_t t = ++s_translated;
    if (out.memexport_count) {
      const uint64_t w = ++s_withMemexport;
      s_memexportOps += out.memexport_count;
      if (w <= 16)
        REXLOG_INFO("d3d9: MEMEXPORT {} shader 0x{:08X}: {} export(s) to "
                    "registers 32-37 -- guest writes memory from a shader and "
                    "we drop it",
                    stage == mx::hle::HlslStage::kPixel ? "pixel" : "vertex",
                    handle, out.memexport_count);
    }
    if ((t % 25) == 0)
      REXLOG_INFO("d3d9: MEMEXPORT census: {} shaders translated, {} use "
                  "memory export, {} exports total",
                  t, s_withMemexport.load(), s_memexportOps.load());
  }

  if (stage == mx::hle::HlslStage::kPixel) {
    static std::atomic<uint64_t> s_psTranslated{0};
    static std::atomic<uint64_t> s_psZeroExport{0};
    const uint64_t n = ++s_psTranslated;
    if (out.color_unassigned_mask) {
      const uint64_t z = ++s_psZeroExport;
      if (z <= 16) {
        REXLOG_INFO("d3d9: ZERO-EXPORT PS 0x{:08X}: export_mask 0x{:X} "
                    "unassigned 0x{:X} -- colour target emitted as the zero "
                    "initialiser (opaque black)",
                    handle, out.export_mask, out.color_unassigned_mask);
      }
    }
    // Every 25, not 100: a menu-only run translates ~66 pixel shaders, so a
    // 100 interval reports nothing at all and "no zero-exports" is then
    // indistinguishable from "the census never printed".
    if ((n % 25) == 0) {
      REXLOG_INFO("d3d9: ZERO-EXPORT census: {} pixel shaders translated, {} "
                  "with a colour target named but never assigned",
                  n, s_psZeroExport.load());
    }
  }

  // Emitting is only half the claim. Source FXC rejects is exactly as useless
  // as a shader the emitter refused, and the two failures have entirely
  // different causes -- so they are counted apart and the compiler's own message
  // is logged, since "it did not compile" without the reason is not a finding.
  std::string compile_error;
  bool compiled = false;
  std::shared_ptr<const std::vector<uint8_t>> dxbc_bytes;
  if (out.status == mx::hle::HlslStatus::kOk) {
    Microsoft::WRL::ComPtr<ID3DBlob> blob, errors;
    const char* target =
        stage == mx::hle::HlslStage::kPixel ? "ps_5_0" : "vs_5_0";
    const uint64_t content_key = ShaderSourceKey(stage, out.source);
    dxbc_bytes = LoadShaderDxbc(stage, content_key);
    if (dxbc_bytes) {
      // Cache hit: same microcode seen in an earlier run or an earlier
      // video. Skip FXC entirely -- the emitter still ran above (0ms) for
      // the metadata the draw path needs.
      ++g_dxbcCacheHits;
      D3DCreateBlob(dxbc_bytes->size(), &blob);
      if (blob)
        std::memcpy(blob->GetBufferPointer(), dxbc_bytes->data(),
                    dxbc_bytes->size());
      compiled = blob != nullptr;
    } else if (!t_shaderCompileWorker &&
               EnqueueShaderCompile(stage, handle, code, count)) {
      // OFF THE GUEST THREAD. Nothing is installed, so the draw that triggered
      // this keeps seeing nullptr and takes the stand-in path until the worker
      // finishes. Everything below -- coverage counters, the dump, the vertex
      // fetch variant -- runs on the worker when it re-enters, so no accounting
      // is lost, only deferred.
      return;
    } else {
      const HRESULT hr = D3DCompile(
          out.source.data(), out.source.size(), nullptr, nullptr, nullptr,
          "main", target, D3DCOMPILE_OPTIMIZATION_LEVEL0, 0, &blob, &errors);
      compiled = SUCCEEDED(hr) && blob;
      if (compiled) {
        ++g_dxbcCacheMisses;
        SaveShaderDxbc(stage, content_key, blob.Get());
        // Carry the fresh bytes too, so the renderer skips its own O3
        // recompile of a first-sight shader -- the O0 compile here already
        // produced everything the PSO needs.
        const auto* p = static_cast<const uint8_t*>(blob->GetBufferPointer());
        dxbc_bytes = std::make_shared<const std::vector<uint8_t>>(
            p, p + blob->GetBufferSize());
      }
    }
    t_compile = std::chrono::steady_clock::now();
    // DIAG: dump the generated HLSL beside the DXBC the
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
          // Only when non-zero, so its presence in a dump means the shader has
          // a colour target that compiles to `mov o0, l(0,0,0,0)`.
          if (out.color_unassigned_mask)
            f << "\n; COLOUR TARGET NAMED BUT NEVER ASSIGNED: 0x" << std::hex
              << out.color_unassigned_mask << std::dec
              << " (emitted as the zero initialiser -- opaque black)";
          // Only when non-zero, so its presence in a dump means something.
          if (out.unhonoured_predicate_ops)
            f << "\n; SETP OPS WRITING p0: " << out.unhonoured_predicate_ops;
          // The half that used to be missing entirely. Non-zero means p0 is not
          // merely computed but ACTED ON, per instruction.
          if (out.predicated_alu_ops)
            f << "\n; PREDICATED ALU INSTRUCTIONS: " << out.predicated_alu_ops
              << " (all HONOURED as `if (xe_p0 == ...)`)";
          if (out.predicated_fetches)
            f << "\n; PREDICATED TEXTURE FETCHES: " << out.predicated_fetches
              << " (all HONOURED -- sample unconditionally, gate the "
                 "destination write)";
          // NOT a correctness gap, and it used to be described as one here.
          // The exec-level predicate is a WAVEFRONT branch: ucode.h says "if
          // any of the invocations passes the predicate check, all of them will
          // enter the exec". Lanes whose p0 is clear enter the block anyway, so
          // the block gate never provided per-lane correctness -- which is why
          // the compiler also predicates the instructions inside it, and why
          // the validator reports a mismatch when they disagree.
          //
          // Measured over this title's three heavily predicated pixel shaders:
          // 194 ALU and 46 fetch instructions inside cond_exec_pred blocks,
          // and ALL 240 carry their own (p0). Since 741d243 and 48dfe30 we
          // honour both, so the per-lane semantics are already right. An
          // `if` here would be a wavefront-level SKIP -- a performance
          // optimisation -- and in the pixel stage an illegal one.
          if (out.pred_exec_blocks)
            f << "\n; P0-GATED EXEC BLOCKS: " << out.pred_exec_blocks
              << ", skipped as `if (xe_p0 == ...)`: "
              << out.honoured_pred_exec_blocks
              << " (the rest are ENTERED and their instructions gated"
                 " individually, which is what the console does per lane)";
          if (out.bool_exec_blocks)
            f << "\n; BOOL-GATED EXEC BLOCKS: " << out.bool_exec_blocks
              << " (cond_exec / cond_exec_pred_clean walked as a plain exec -- "
                 "gated on a BOOL CONSTANT, and this translator has no bool "
                 "constant bank, so the condition cannot be evaluated yet)";
          if (out.unhonoured_fetch_ops)
            f << "\n; UNHONOURED FETCH OPS: " << out.unhonoured_fetch_ops
              << " (getCompTexLOD/getGradients/getWeights/getBCF/setGradients "
                 "skipped; their destination keeps its previous value)";
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

  {
    static uint32_t s_timed = 0;
    if (s_timed < 8) {
      ++s_timed;
      const auto now = std::chrono::steady_clock::now();
      const auto emit_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                               t_emit - t_first_use)
                               .count();
      const auto compile_ms =
          std::chrono::duration_cast<std::chrono::milliseconds>(t_compile -
                                                                t_emit)
              .count();
      const auto tail_ms =
          std::chrono::duration_cast<std::chrono::milliseconds>(now - t_compile)
              .count();
      REXLOG_INFO("d3d9: HLSL {} 0x{:08X} first-use: emit {}ms compile {}ms "
                  "dump/disasm/vfetch {}ms",
                  stage == mx::hle::HlslStage::kPixel ? "PS" : "VS", handle,
                  emit_ms, compile_ms, tail_ms);
    }
  }

  // Cache health, every 256 lookups. The number to watch is MISSES on a second
  // run of the same content: it should be ~0, and anything else means the key
  // is not stable. Note what a HIGH hit rate does not prove -- the key is the
  // emitted HLSL, so a hit only says "this exact source compiled before". If
  // the emitter changes, every affected key changes and the misses are correct.
  {
    static uint64_t s_last_reported = 0;
    const uint64_t total = g_dxbcCacheHits + g_dxbcCacheMisses;
    if (total - s_last_reported >= 256) {
      s_last_reported = total;
      REXLOG_INFO("d3d9: HLSL dxbc cache: {} hits {} misses",
                  g_dxbcCacheHits, g_dxbcCacheMisses);
    }
  }

  // Read before the move below; the report prints it.
  const size_t source_size = out.source.size();

  // Counted for every shader that EMITTED, whether or not FXC then took it:
  // predication is a property of the guest microcode, not of our compile, and
  // scoping it to the compiled ones would under-report the population.
  if (out.pred_exec_blocks) {
    ++cov.predicated;
    // Only when EVERY block in the shader was obeyed. A partly-honoured shader
    // still runs some bodies unconditionally, and counting it as honoured
    // would report the job done while it is half done.
    if (out.honoured_pred_exec_blocks == out.pred_exec_blocks) ++cov.p0_honoured;
  }
  if (out.bool_exec_blocks) ++cov.bool_gated;

  if (out.status != mx::hle::HlslStatus::kOk) {
    ++cov.failures[mx::hle::HlslStatusName(out.status)];
    if (out.blocking_opcode) ++cov.blocking_opcode[out.blocking_opcode];
  } else if (compiled) {
    ++cov.ok;
    // Retained only for shaders that both emitted and compiled. A source the
    // compiler rejects must never reach the renderer, which would only discover
    // the same failure later and with less context.
    std::lock_guard<std::mutex> install_lk(g_translatedMu);
    TranslatedShader& kept = (stage == mx::hle::HlslStage::kPixel
                                  ? g_translatedPs
                                  : g_translatedVs)[handle];
    kept.source = std::make_shared<const std::string>(std::move(out.source));
    kept.input_mask = out.input_mask;
    kept.export_mask = out.export_mask;
    kept.sampler_mask = out.sampler_mask;
    kept.sampler_count = out.sampler_count;
    kept.sampler_array_mask = out.sampler_array_mask;
    for (uint32_t i = 0; i < out.sampler_count; ++i)
      kept.slot_guest[i] = out.sampler_slot_guest[i];
    kept.max_const_index = out.max_const_index;
    kept.dxbc = dxbc_bytes;

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
        std::shared_ptr<const std::vector<uint8_t>> fetch_dxbc;
        // No fetch-variant salt any more: the fetch form IS a different source
        // string, so keying on the source separates the two by construction.
        const uint64_t fetch_key = ShaderSourceKey(stage, fetched.source);
        fetch_dxbc = LoadShaderDxbc(stage, fetch_key);
        if (fetch_dxbc) {
          ++g_dxbcCacheHits;
          D3DCreateBlob(fetch_dxbc->size(), &fblob);
          if (fblob)
            std::memcpy(fblob->GetBufferPointer(), fetch_dxbc->data(),
                        fetch_dxbc->size());
        } else {
          const HRESULT fhr = D3DCompile(
              fetched.source.data(), fetched.source.size(), nullptr, nullptr,
              nullptr, "main", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL0, 0,
              &fblob, &ferrors);
          if (SUCCEEDED(fhr) && fblob) {
            ++g_dxbcCacheMisses;
            SaveShaderDxbc(stage, fetch_key, fblob.Get());
            const auto* p =
                static_cast<const uint8_t*>(fblob->GetBufferPointer());
            fetch_dxbc = std::make_shared<const std::vector<uint8_t>>(
                p, p + fblob->GetBufferSize());
          }
        }
        if (fblob) {
          // DIAG: dump the FETCH variant separately.
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
          kept.fetch_dxbc = fetch_dxbc;
          kept.vertex_fetch_count = fetched.vertex_fetch_count;
          for (uint32_t i = 0; i < fetched.vertex_fetch_count; ++i)
            kept.vertex_fetch_slot[i] = fetched.vertex_fetch_slot[i];
          kept.computed_index_streams = fetched.computed_index_streams;
          kept.computed_index_fetches = fetched.computed_index_fetches;
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

    // One line per distinct VERTEX CENSUS. A vertex shader that samples is the
    // shape a bone-matrix palette takes when the engine binds it as
    // g_BoneMatrixVectors rather than as a constant array, and it is currently
    // refused the GPU vertex path -- see the g_gpuVertexVsSamplers counter.
    //
    // This used to be unconditional, on the claim "there are 32 of them in a
    // run, so this is bounded". MEASURED 2026-08-26, run 1433: 4200 of these in
    // a 210-frame segment -- 20 per frame, every one the SAME handle
    // 0x217AB7A0, with byte-identical fields. Together with the VFETCH line
    // below they were 70% of the log and rotated it away every 30 seconds,
    // which is how three empty greps nearly became a false conclusion.
    //
    // They reach this line at all because the (handle, code_key) dedupe at the
    // top of this function sees a DIFFERENT code hash each time, so each one is
    // a full re-translation. That is a real defect and it is not this line's to
    // fix -- so the dedupe here is on the CENSUS ITSELF, which suppresses the
    // repetition without suppressing the evidence: a genuinely different census
    // still prints, and the recycle count now rides on the coverage line below
    // where someone reading coverage will see it.
    if (stage == mx::hle::HlslStage::kVertex) {
      // INTERPOLATOR ZERO-FILL. Every slot in the linkage that this vertex
      // shader does not export is emitted as its float4(0,0,0,0) initialiser.
      // The census for that MOVED to the draw path -- see NoteInterpolatorFill
      // -- because counting it here measured the wrong thing: it fired on every
      // unexported slot, and most shaders simply do not use all eight, so it
      // read ~80% in both scenes and meant nothing. A slot nobody reads is not
      // invented output.
      static std::set<uint64_t> s_census;
      const uint64_t census_key = (uint64_t(handle) << 32) ^
                                  (uint64_t(out.max_const_index) << 24) ^
                                  (uint64_t(out.sampler_count) << 20) ^
                                  (uint64_t(out.sampler_mask) << 8) ^
                                  uint64_t(out.input_mask);
      // Capped as well as deduped: a run that really does recycle a handle onto
      // thousands of distinct shaders must not get the log back by another
      // route.
      if (s_census.size() < 256 && s_census.insert(census_key).second) {
        REXLOG_INFO(
            "d3d9: VS census 0x{:08X}: samplers {} (mask 0x{:X}) inputs "
            "0x{:08X} max const c{}",
            handle, out.sampler_count, out.sampler_mask, out.input_mask,
            out.max_const_index);
      }
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
  // Deliberately NOT on the coverage report's schedule: that one fires at 6 and
  // then at multiples of 16, so a run ending at 11 shaders never shows its final
  // tally -- which is exactly what happened the first time. Worth knowing that
  // the same schedule is why `HLSL VS coverage` stops printing entirely once
  // seen.size() parks on a value that is not a multiple of 16 (42 in run 1433).
  //
  // But "every new vertex shader" was never what this did. It ran on every pass
  // through this function, and g_vfetchCompiled advances on every RE-translation
  // rather than once per shader, which is why run 1433 reports the nonsense
  // "30573 of 42". 4200 lines in 210 frames.
  //
  // Bounded on the only thing here that actually changes -- a new distinct
  // shader -- plus a slow heartbeat, so a run that ends between shaders still
  // shows its last state and the "final tally" property above survives.
  //
  // The recycle count is printed HERE rather than left to its own summary,
  // because it is the denominator that makes the first number readable: 37080
  // recycles against 42 distinct shaders in run 1433 is what "30573 of 42"
  // was trying to say. Its declaration comment still reads "Zero means handles
  // are never recycled and that fix is inert" -- it is not zero, and it is not
  // inert.
  if (stage == mx::hle::HlslStage::kVertex) {
    static size_t s_lastSeen = 0;
    static std::chrono::steady_clock::time_point s_lastReport{};
    const auto now = std::chrono::steady_clock::now();
    const bool grew = seen.size() != s_lastSeen;
    if (grew || now - s_lastReport >= std::chrono::seconds(10)) {
      s_lastSeen = seen.size();
      s_lastReport = now;
      std::string refused;
      for (const auto& [why, n] : g_vfetchRefused)
        refused += fmt::format(" {}={}", why, n);
      REXLOG_INFO(
          "d3d9: VFETCH coverage: {} compiles over {} distinct vertex "
          "shaders;{} (handles recycled onto different microcode: {})",
          g_vfetchCompiled, seen.size(),
          refused.empty() ? " none refused" : refused,
          g_shaderHandleRecycled.load());
    }
  }
}

void CollectPixelShaderBlob(uint32_t handle, uint8_t* base) {
  (void)base;
  if (!handle || g_patch.psBlobs.count(handle)) return;
  // D3DDevice_CreatePixelShader (0x82552148) copies the source header to
  // object+0x28, allocates pFunction[2] code bytes separately, and
  // sub_825506B0 stores that allocation at object+0x18. Pixel shader objects
  // do not share the vertex shader's inline +0x368 representation.
  //
  // **The CF stream does not start at the beginning of that allocation.** Big
  // shaders carry a prologue -- the first dwords read as zeros -- and only the
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
  // consuming code, never guessed -- same rule as every offset in AGENTS.md's
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
    g_patch.psBlobs.emplace(handle, std::move(blob));
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


void ReportCoverage(uint8_t* base) {
  const auto& st = DeviceState();
  if (g_drawsChecked == 0) {
    REXLOG_INFO("d3d9: hle -- no draws scored");
    return;
  }
  REXLOG_INFO("d3d9: hle -- {} of {} draws fully described ({}%)",
              g_drawsComplete, g_drawsChecked,
              (g_drawsComplete * 100) / g_drawsChecked);
  for (uint32_t g = 0; g < kDrawGapCount; ++g) {
    if (g_drawGaps[g]) {
      REXLOG_INFO("d3d9: hle   missing: {:<28} x{}", DrawGapName(g), g_drawGaps[g]);
    }
  }
  //-------------------------------------------------------------------------
  // Stage 2: what was actually built, and why the rest was not. Every skip is
  // named -- a bare total cannot separate "the decoder refuses this format"
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
      REXLOG_INFO("d3d9: hle-render -- {} of {} draws built ({}%)", built,
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
        "d3d9: stage0  stream {}: device fetch constant vs our snapshot -- "
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
      "d3d9: decl-draws -- {} declarations known; COLOUR={} NO-COLOUR={} "
      "unattributed={} patch_calls={}",
      g_declCount, with, without, g_drawsNoDecl, g_patchCalls);
  // The declaration now comes from device + 0x2ED8, per draw. These four say
  // whether that source is sound and how badly the old one lagged: `unknown`
  // must stay at 0 or the field is not what SetVertexDeclaration writes, and a
  // large `stale` is the 2508-calls-per-165000-draws problem, measured.
  REXLOG_INFO(
      "d3d9: decl-source -- from device+0x2ED8: null={} unknown={} | vs the "
      "patch hook: same={} stale={}",
      g_declDeviceNull, g_declDeviceUnknown, g_declAgree, g_declDisagree);
  // One row per declaration, held back while no NEW declaration has appeared.
  // 17,987 rows over 783 reports in run mx_1782 -- ~23 a report, and the only
  // thing moving between prints is each row's draw count. The two summary
  // lines above always print and already carry g_declCount, so the held case
  // is never silent.
  static uint64_t s_lastDecls = 0;
  static uint32_t s_sinceDecls = 0;
  if (!RowDumpDue(uint64_t(g_declCount), s_lastDecls, s_sinceDecls)) {
    REXLOG_INFO("d3d9: decl-draws   rows held -- declaration set unchanged at "
                "{} ({} report(s) so far; d3d9_diag_row_heartbeat={})",
                g_declCount, s_sinceDecls,
                REXCVAR_GET(d3d9_diag_row_heartbeat));
    return;
  }
  for (int i = 0; i < g_declCount; ++i) {
    REXLOG_INFO("d3d9: decl-draws   id={} ptr=0x{:08X} elems={} colour={} x{}",
                i, g_declPtr[i], g_declElems[i],
                g_declHasColour[i] ? "yes" : "no", g_declDraws[i]);
  }
}

// All three draw entry points report through here so the counters are always
// read together. A 150s run reaches 5000-10000 transcoded draws, so a coarser
// cadence than 2500 reports nothing at all -- the first output-merger probe was
// lost to exactly that.
//
// DrawVerticesUP was added 2026-08-07. It had been unhooked since the start,
// so every draw total this project has ever quoted excluded it -- including the
// Bink video composite, which is why no video ever reached the screen.
// DrawVerticesUP CALLER CENSUS.
//
// ~95 of the ~342 draws the guest submits each frame come through
// DrawVerticesUP (sub_82555B88), and about 30 engine functions share it -- UI,
// particles, and the Bink composite. A total tells us nothing about which of
// them drew what, and the UI question is specifically "which caller draws the
// nav bar, and what does it submit".
//
// The LINK REGISTER is the return address inside the caller, so it names the
// call SITE, not just the function -- two draws from different points in the
// same function stay distinguishable, which is what separates a component's
// text batch from its background batch.
//
// BOUNDED, with the overflow counted rather than dropped: a census whose
// denominator quietly stops growing is worse than none. See
// [[a-total-without-a-denominator]].
namespace {

constexpr size_t kMaxUpCallers = 64;

struct UpCaller {
  uint32_t lr = 0;
  uint32_t kind = 0;
  uint64_t calls = 0;
  uint64_t verts = 0;
  uint32_t min_verts = 0xFFFFFFFFu;
  uint32_t max_verts = 0;
};

std::mutex g_upCallerMu;
std::array<UpCaller, kMaxUpCallers> g_upCallers{};
size_t g_upCallerCount = 0;
uint64_t g_upCallerOverflow = 0;

}  // namespace

void NoteUpDrawCaller(uint32_t lr, uint32_t verts, uint32_t kind) {
  std::lock_guard<std::mutex> lk(g_upCallerMu);
  for (size_t i = 0; i < g_upCallerCount; ++i) {
    if (g_upCallers[i].lr != lr || g_upCallers[i].kind != kind) continue;
    auto& c = g_upCallers[i];
    ++c.calls;
    c.verts += verts;
    if (verts < c.min_verts) c.min_verts = verts;
    if (verts > c.max_verts) c.max_verts = verts;
    return;
  }
  if (g_upCallerCount >= kMaxUpCallers) {
    ++g_upCallerOverflow;
    return;
  }
  auto& c = g_upCallers[g_upCallerCount++];
  c.lr = lr;
  c.kind = kind;
  c.calls = 1;
  c.verts = verts;
  c.min_verts = verts;
  c.max_verts = verts;
}

void ReportUpDrawCallers() {
  std::array<UpCaller, kMaxUpCallers> snap{};
  size_t n = 0;
  uint64_t overflow = 0;
  {
    std::lock_guard<std::mutex> lk(g_upCallerMu);
    snap = g_upCallers;
    n = g_upCallerCount;
    overflow = g_upCallerOverflow;
  }
  const std::string cap =
      overflow ? fmt::format(", {} DROPPED past the {}-site cap (the rows are "
                             "then not the whole set)",
                             overflow, kMaxUpCallers)
               : std::string();
  // A new call site moves `n`; a first overflow moves the other half. Either is
  // something unseen, so fold both into the population the heartbeat watches.
  static uint64_t s_last = 0;
  static uint32_t s_since = 0;
  if (!RowDumpDue((uint64_t(n) << 1) | (overflow ? 1u : 0u), s_last, s_since)) {
    REXLOG_INFO("d3d9: UP CALLERS {} distinct call sites{} -- unchanged, rows "
                "held ({} report(s) so far; d3d9_diag_row_heartbeat={})",
                n, cap, s_since, REXCVAR_GET(d3d9_diag_row_heartbeat));
    return;
  }
  std::sort(snap.begin(), snap.begin() + n,
            [](const UpCaller& a, const UpCaller& b) { return a.calls > b.calls; });
  std::string rows;
  for (size_t i = 0; i < n; ++i) {
    rows += fmt::format(" [{} lr0x{:08X} x{} verts{}..{} avg{}]",
                        snap[i].kind == 0   ? "IDX"
                        : snap[i].kind == 1 ? "VTX"
                                            : "UP ",
                        snap[i].lr,
                        snap[i].calls, snap[i].min_verts, snap[i].max_verts,
                        snap[i].calls ? snap[i].verts / snap[i].calls : 0);
  }
  REXLOG_INFO("d3d9: UP CALLERS {} distinct call sites{} --{}", n, cap,
              rows.empty() ? " (none)" : rows);
}

void ReportDrawCounts(uint8_t* base) {
  const uint64_t total = g_indexed_draws + g_draws + g_up_draws + g_indexed_up_draws;
  if ((total % kDrawReportEvery) != 0) return;
  REXLOG_INFO("d3d9: draws -- DrawIndexedVertices={} DrawVertices={} "
              "DrawVerticesUP={} DrawIndexedVerticesUP={} (skipped {}) total={}",
              g_indexed_draws, g_draws, g_up_draws, g_indexed_up_draws,
              g_indexed_up_skipped, total);
  ReportUpDrawCallers();
  // Stencil sizing. See the census at its definition for why the effective
  // count is not just the enable bit. Printed here rather than at first sight
  // of each config because sizing needs the TOTALS, and a first-sight line
  // always reports a count of one.
  {
    std::lock_guard<std::mutex> lk(g_stencilCensusMu);
    std::string modes;
    for (const auto& [mode, n] : g_edramModes) {
      const char* name = mode == 0   ? "NoOperation"
                         : mode == 4 ? "ColorDepth"
                         : mode == 5 ? "DepthOnly"
                         : mode == 6 ? "Copy"
                         : mode == 0xFFFFFFFFu ? "UNREADABLE"
                                               : "reserved";
      modes += fmt::format(" {}({})={}", name, mode, n);
    }
    REXLOG_INFO("d3d9: STENCIL census -- {} draws reached the read ({} could "
                "not); enable bit set {}, of which {} are in an edram_mode "
                "that honours it; {} distinct configs; edram_mode:{}",
                g_stencilDrawsSeen, g_stencilDrawsUnreadable, g_stencilBitSet,
                g_stencilEffective, g_stencilConfigs.size(), modes);
    // One line per distinct configuration, so the translation work is a
    // countable list rather than an impression. Held back while the config set
    // is unchanged -- the census line above already carries the count, so the
    // suppressed case is never silent.
    static uint64_t s_lastCfgs = 0;
    static uint32_t s_sinceCfgs = 0;
    const bool dump_cfgs =
        RowDumpDue(g_stencilConfigs.size(), s_lastCfgs, s_sinceCfgs);
    if (!dump_cfgs) {
      REXLOG_INFO("d3d9:   stencil cfg rows held -- config set unchanged at {} "
                  "({} report(s) so far; d3d9_diag_row_heartbeat={})",
                  g_stencilConfigs.size(), s_sinceCfgs,
                  REXCVAR_GET(d3d9_diag_row_heartbeat));
    } else {
      for (const auto& [key, n] : g_stencilConfigs) {
        const uint32_t dc_bits = key.first;
        const uint32_t rm = key.second;
        REXLOG_INFO("d3d9:   stencil cfg depthcontrol=0x{:08X} refmask=0x{:08X}"
                    " x{} -- func {} fail {} zpass {} zfail {} backface {}"
                    " (bf func {} fail {} zpass {} zfail {});"
                    " ref {} mask 0x{:02X} writemask 0x{:02X}",
                    dc_bits, rm, n, (dc_bits >> 8) & 7u, (dc_bits >> 11) & 7u,
                    (dc_bits >> 14) & 7u, (dc_bits >> 17) & 7u,
                    (dc_bits >> 7) & 1u, (dc_bits >> 20) & 7u,
                    (dc_bits >> 23) & 7u, (dc_bits >> 26) & 7u,
                    (dc_bits >> 29) & 7u, rm & 0xFFu, (rm >> 8) & 0xFFu,
                    (rm >> 16) & 0xFFu);
      }
    }
  }
  // DEPTH SURFACES BY EDRAM BASE. More than one owner on a base means two
  // D3D12 depth textures -- and two independent stencil planes -- stand in for
  // one console allocation, so a mask written through one view is invisible
  // through the other.
  REXLOG_INFO("d3d9: DEPTH SURFACES BY EDRAM BASE (>1 owner means the stencil "
              "plane is split across textures the guest treats as one):{}",
              DepthSurfaceReport());

  // PHASE 1 PASS CONDITION.
  //
  // READ THIS BEFORE CONCLUDING THE COUNTS SHOULD BE EQUAL. They should not,
  // and an earlier version of this line said "must match the census above",
  // which was written when both counters ran at the same site. Since the check
  // moved to the CONSUMER the two count different populations by construction:
  // the census sees every draw that reached the register read, this sees only
  // the ones that reached AddGameDraw. Everything captured and never submitted
  // is legitimately missing here.
  //
  // So the pass condition is:
  //
  //   config KEY SET   must be IDENTICAL. A configuration present in the census
  //                    and absent here would be state Phase 2 can never act on,
  //                    which is the failure this check exists to catch.
  //   counts           must differ by exactly the draws that never reached the
  //                    renderer -- cross-check against FRAME DRAWS
  //                    `guest` minus `accepted`.
  //
  // The gap is PRINTED rather than left to be worked out, because a reader who
  // expects equality will otherwise read a correct result as a failure.
  {
    std::lock_guard<std::mutex> lk(g_plumbedStencilMu);
    // The KEY SET is the payload and it is add-only, so its size is a faithful
    // "a configuration you have not seen has reached the consumer". The counts
    // and the gaps are cumulative and stay on the line either way, because the
    // gap is the number a reader comes here for.
    static uint64_t s_lastPlumbed = 0;
    static uint32_t s_sincePlumbed = 0;
    const bool dump_keys =
        RowDumpDue(g_plumbedConfigs.size(), s_lastPlumbed, s_sincePlumbed);
    std::string cfgs;
    if (dump_keys) {
      for (const auto& [key, n] : g_plumbedConfigs)
        cfgs += fmt::format(" [{:08X}/{:08X} x{}]", key.first, key.second, n);
    } else {
      cfgs = fmt::format(" held, key set unchanged ({} report(s) so far; "
                         "d3d9_diag_row_heartbeat={})",
                         s_sincePlumbed,
                         REXCVAR_GET(d3d9_diag_row_heartbeat));
    }
    // Signed: the consumer cannot legitimately see MORE than the census, so a
    // negative gap is itself a finding rather than an impossible number.
    const int64_t seen_gap =
        int64_t(g_stencilDrawsSeen) - int64_t(g_plumbedSeen);
    const int64_t eff_gap =
        int64_t(g_stencilEffective) - int64_t(g_plumbedEffective);
    REXLOG_INFO("d3d9: STENCIL PLUMBED at the CONSUMER -- {} draws carried the "
                "fields ({} had an unreadable register), {} effective, {} "
                "distinct configs. Counts are EXPECTED to be lower than the "
                "census: {} draws and {} effective never reached the renderer "
                "(cross-check FRAME DRAWS guest-minus-accepted). The KEY SET "
                "is what must match:{}",
                g_plumbedSeen, g_plumbedUnreadable, g_plumbedEffective,
                g_plumbedConfigs.size(), seen_gap, eff_gap,
                cfgs.empty() ? " none" : cfgs);
  }
  // The back-face register window. See NoteBackFaceWindow for why this is a
  // scan and not a read of one guessed offset.
  {
    std::lock_guard<std::mutex> lk(g_bfWindowMu);
    if (g_bfWindowDraws) {
      // Every distinct (offset, value) pair the scan has ever seen. Both map
      // levels are add-only, so this grows exactly when the window shows
      // something new -- which is the entire question the scan asks.
      uint64_t pairs = 0;
      for (const auto& [off, vals] : g_bfWindow) pairs += 1 + vals.size();
      static uint64_t s_lastBf = 0;
      static uint32_t s_sinceBf = 0;
      if (!RowDumpDue(pairs, s_lastBf, s_sinceBf)) {
        REXLOG_INFO("d3d9: BACKFACE STENCIL WINDOW -- {} two-sided draws "
                    "sampled, scan held, no new offset or value ({} report(s) "
                    "so far; d3d9_diag_row_heartbeat={})",
                    g_bfWindowDraws, s_sinceBf,
                    REXCVAR_GET(d3d9_diag_row_heartbeat));
      } else {
        std::string w;
        for (const auto& [off, vals] : g_bfWindow) {
          w += fmt::format(" [+{:04X}", off);
          // At most four values per offset: a register that takes many values is
          // not the one being looked for, and printing them all would bury the
          // one that does.
          uint32_t shown = 0;
          for (const auto& [v, n] : vals) {
            if (shown++ == 4) {
              w += fmt::format(" +{}more", vals.size() - 4);
              break;
            }
            w += fmt::format(" {:08X}x{}", v, n);
          }
          w += "]";
        }
        REXLOG_INFO("d3d9: BACKFACE STENCIL WINDOW -- {} two-sided draws sampled "
                    "(0x2900 is RB_STENCILREFMASK 0x210D; looking for 0x210E, "
                    "which should be refmask-shaped 0x00rrwwss and NOT a copy of "
                    "+2900):{}",
                    g_bfWindowDraws, w.empty() ? " none" : w);
      }
    }
  }
  // The ALU constant file. `repaired 0` is only meaningful next to a non-zero
  // `constants seen` -- with zero seen, the PM4 feed is not reaching the file and
  // the repair count says nothing at all.
  {
    uint64_t written = 0, repaired = 0, zeroed = 0, filled_zero = 0;
    uint32_t seen = 0;
    mx::gpu::alu::Stats(written, repaired, seen, zeroed, filled_zero);
    REXLOG_INFO("d3d9: ALU constant file -- {} dwords written by PM4 over {} "
                "distinct constants; {} repaired from PM4, {} NaN LEFT IN "
                "PLACE (the substitution was deleted 2026-08-27), {} finite "
                "zeros PM4 could fill but we do NOT "
                "(measurement only); shader load-table overlays {}",
                written, seen, repaired, zeroed, filled_zero,
                g_shaderConstOverlays);
    // Which constants the zero-fill hit. A short tail means the fill can be
    // narrowed to a range; a long one means the frame-global PM4 file is simply
    // the wrong authority for a mid-frame draw, and the fix is upstream.
    if (filled_zero)
      REXLOG_INFO("d3d9: ZERO-FILL BY CONSTANT:{}",
                  mx::gpu::alu::FilledHistogram(12));
  {
    // c32 is the vertex tint that renders the legal screen's logos black:
    // the pixel shader multiplies a correctly-sampled white texel by it and
    // LegacyMul forces +0 because it is zero. c252..c255 are the CONTROL --
    // known screen-space scale/bias, so if they read wrong the file cannot be
    // trusted and c32's value means nothing either.
    static const uint32_t kWatch[] = {32, 252, 253, 254, 255};
    REXLOG_INFO("d3d9: WOULD-FILL VALUES (c252-255 are the control -- expect "
                "screen-space scale/bias like 0.5/-0.5):{}",
                mx::gpu::alu::WouldFillValues(kWatch, 5));
  }
    // The narrow window that actually substitutes. Printed unconditionally,
    // zero included: "never fired" and "fired and changed nothing" are the two
    // outcomes worth telling apart, and a suppressed line looks like neither.
    REXLOG_INFO("d3d9: MATERIAL GATE FILL {} substitutions (pixel c84-c87, the "
                "PM4-only material block; c85.w is the terrain diffuse gate)",
                mx::gpu::alu::MaterialGateFilled());
    // Every outcome on one line, zeros included, because each says something
    // different and only one of them means the change worked:
    //   applied 0, denormal 0, unpub N  -> PM4 never publishes c32; remove this
    //   applied 0, denormal N           -> the source is junk; remove this
    //   applied N, filled 0             -> validated, but the slot was never a
    //                                      hole -- the tint comes from
    //                                      elsewhere and c32 was a red herring
    //   applied N, filled M             -> the substitution is live; the logos
    //                                      should now be visible on that share
    //                                      of draws, and flickering means the
    //                                      denormal is next
    // vs c32 WAS filled from this file and is not any more -- see the revert.
    // The value stays on the report because it is the evidence: PM4 publishes
    // it as a plain zero, which is what killed the substitution.
    static const uint32_t kTint[] = {32};
    REXLOG_INFO("d3d9: ALU FILE SPOT CHECK{}",
                mx::gpu::alu::FileValues(kTint, 1));
  }
  ReportDeclHistogram();
  if (REXCVAR_GET(hle_capture)) ReportCoverage(base);
}

}  // namespace mx::hooks::d3d9

// Declared in hooks_d3d9.h. A free function rather than the atomic itself
// because hooks_frame.cpp reads this and cannot include the internal header --
// that header needs mx::hle types (HleStream, D3D9Element, LayoutError) which
// hooks_frame.cpp does not pull in.
// Declared in hooks_d3d9.h. Same free-function shape as GuestDrawCalls below,
// and for the same reason: the app layer consumes DrawCalls and must not
// include the internal header.
void NotePlumbedStencil(const mx::hle::DrawCall& dc) {
  mx::hooks::d3d9::NotePlumbedStencilImpl(dc);
}

// Declared in hooks_d3d9.h. The three exits that make `guest` exceed
// `accepted + refused`, so the FRAME DRAWS gap is attributable without a debug
// cvar. `skips` is the BuildHleDraw population, which was already counted but
// only ever printed under --hle_capture.
void UnbuiltDrawReasons(uint64_t& no_viewport, uint64_t& shader_failed,
                        uint64_t& nocode_queue_full, uint64_t& skips) {
  no_viewport = mx::hooks::d3d9::g_drawNoViewport;
  shader_failed = mx::hooks::d3d9::g_drawShaderFailed;
  nocode_queue_full = mx::hooks::d3d9::g_drawShaderNoCodeFull;
  skips = 0;
  const uint64_t* counts = mx::hle::HleSkipCounts();
  for (uint32_t i = 1; i < uint32_t(mx::hle::HleSkip::kCount); ++i)
    skips += counts[i];
}

std::string UnbuiltSkipBreakdown() {
  // Run 1551 attributed the whole gap to BuildHleDraw -- 16,706 of 16,706, with
  // no-viewport and shader-failed both at zero. So the reasons ARE the answer,
  // and they were only ever printed from ReportCoverage under --hle_capture.
  // Promoted here rather than left there: the gap is a standing property of
  // every run, not a debugging session.
  //
  // Ranked, and zero rows omitted -- with eleven possible reasons a full list
  // is mostly zeros and the one that matters does not stand out.
  const uint64_t* counts = mx::hle::HleSkipCounts();
  std::vector<std::pair<uint64_t, uint32_t>> ranked;
  for (uint32_t i = 1; i < uint32_t(mx::hle::HleSkip::kCount); ++i)
    if (counts[i]) ranked.emplace_back(counts[i], i);
  std::sort(ranked.begin(), ranked.end(), std::greater<>());
  std::string out;
  for (const auto& [n, i] : ranked)
    out += fmt::format(" {}={}", mx::hle::HleSkipName(mx::hle::HleSkip(i)), n);
  return out.empty() ? std::string(" none") : out;
}

uint64_t GuestDrawCalls() {
  return mx::hooks::d3d9::g_guestDrawCalls.load(std::memory_order_relaxed);
}

// Same shape and the same reason: hooks_frame.cpp calls this and cannot include
// the internal header where the counters are declared.
//
// Printed on a swap cadence rather than from inside the flush hook, because the
// case being diagnosed is a run with ZERO flushes -- a line that prints only
// when a flush happens reports exactly that case as silence. Every field being
// zero has to be a readable result, not an absent one.
void ReportGlyphCache() {
  namespace d = mx::hooks::d3d9;

  // Second line rather than a longer first one, so each is readable on its own.
  //
  // How to read it. HELD is the count of flushes that saw glyphCache+36 set,
  // meaning the guest was holding the "used this frame" pin and sub_8293E1C0
  // could not evict anything. RELEASED is the opposite arm. Both zero means the
  // byte was never readable and this line says NOTHING -- do not read that as
  // "released". A large HELD with RELEASED at zero is the case that would
  // explain letters going missing once the atlas is full.
  //
  // clamp/cap are the two bounds behind sub_8293E5B8's `a4 > a1[5]` refusal.
  // sub_8293E720 clamps the cell height to clamp BEFORE that test runs, so
  // clamp <= cap means the exit is unreachable and the height-cap theory for
  // the missing letters is dead. Zeroes mean not read, not "no limit".

  // The one that decides where to look next. REFUSED > 0 keeps the search in
  // the raster cache; REFUSED == 0 with a healthy CALLS count moves it into
  // composition, because then every glyph the guest asked for was delivered
  // and the missing quads were never requested in the first place.





  // The fork. DROPPED > 0 keeps the hunt inside sub_828AC620's pass-dependent
  // flags; DROPPED == 0 against a healthy call count means the letter was never
  // in the line records and the search moves up into composition.

}

bool GuestRangeReadable(uint8_t* base, uint32_t addr, uint32_t bytes) {
  if (!base || !addr || !bytes) return false;
  return mx::hooks::d3d9::HostPageReadable(REX_RAW_ADDR(addr)) &&
         mx::hooks::d3d9::HostPageReadable(REX_RAW_ADDR(addr + bytes - 1));
}

uint32_t ResolveGuestRange(uint8_t* base, uint32_t addr, uint32_t bytes) {
  if (!base || !addr || !bytes) return 0;
  for (uint32_t m : {0u, 0xA0000000u, 0xC0000000u, 0xE0000000u}) {
    const uint32_t candidate = addr | m;
    if (GuestRangeReadable(base, candidate, bytes)) return candidate;
  }
  return 0;
}

uint64_t HleDrawsAccepted() { return mx::hooks::d3d9::g_hleDrawsAccepted; }
uint64_t HleDrawsRefused() {
  // Both last-gate refusals and the deferred path's own discards, because a
  // draw lost either way is a draw the renderer never issues and the caller is
  // asking "did we lose it", not "where".
  return mx::hooks::d3d9::g_hleDrawsRefused + mx::hooks::d3d9::g_pendingDropped;
}
