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
#include <map>
#include <string>
#include <vector>
#include <fstream>

#include "gpu/d3d9_draw.h"
#include "gpu/d3d9_layout.h"
#include "gpu/d3d9_texture.h"
#include "gpu/shader_ucode.h"   // DecodeVertexShaderFetches, VertexAttribute
#include "gpu/shader_alu.h"     // ExecuteVertexShader
#include <cmath>
#include "gpu/d3d9_state.h"

// Defined in src/app/graphics_system.cpp with the rest of the Debug cvars.
REXCVAR_DECLARE(bool, hle_capture);
REXCVAR_DECLARE(uint32_t, hle_shader_exec);
REXCVAR_DECLARE(uint32_t, hle_shader_verts);

namespace {

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
constexpr int kMaxDeclsLogged = 512;
constexpr int kMaxDrawsLogged = 16;
constexpr uint64_t kDrawReportEvery = 2500;  // see the om1 trap in AGENTS.md
constexpr uint32_t kD3d9ConstRegs = 256;

uint64_t g_indexed_draws = 0;
uint64_t g_draws = 0;
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
constexpr int kMaxTrackedDecls = 256;
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

struct HostRegionCacheEntry {
  const uint8_t* base = nullptr;
  size_t size = 0;
  bool ok = false;
};
constexpr size_t kHostRegionCacheSize = 8;
HostRegionCacheEntry g_hprCache[kHostRegionCacheSize];
size_t g_hprCacheCount = 0;
size_t g_hprCacheNext = 0;
uint64_t g_hprCalls = 0;
uint64_t g_hprQueries = 0;
uint64_t g_hprNanos = 0;

bool UncachedHostPageReadable(const void* p) {
  MEMORY_BASIC_INFORMATION mbi = {};
  if (VirtualQuery(p, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
  if (mbi.State != MEM_COMMIT) return false;
  constexpr DWORD kNoRead = PAGE_NOACCESS | PAGE_GUARD;
  return mbi.Protect != 0 && (mbi.Protect & kNoRead) == 0;
}

bool HostPageReadable(const void* p) {
  ++g_hprCalls;
  const auto* addr = static_cast<const uint8_t*>(p);
  for (size_t i = 0; i < g_hprCacheCount; ++i) {
    const auto& e = g_hprCache[i];
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
  ++g_hprQueries;
  MEMORY_BASIC_INFORMATION mbi = {};
  const bool queried = VirtualQuery(p, &mbi, sizeof(mbi)) == sizeof(mbi);
  g_hprNanos += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now() - _t0)
                             .count());
  if (!queried) return false;  // no region to cache

  constexpr DWORD kNoRead = PAGE_NOACCESS | PAGE_GUARD;
  const bool ok = mbi.State == MEM_COMMIT && mbi.Protect != 0 &&
                  (mbi.Protect & kNoRead) == 0;

  if (mbi.RegionSize) {
    const size_t slot = g_hprCacheCount < kHostRegionCacheSize
                            ? g_hprCacheCount++
                            : (g_hprCacheNext =
                                   (g_hprCacheNext + 1) % kHostRegionCacheSize);
    g_hprCache[slot] = {static_cast<const uint8_t*>(mbi.BaseAddress),
                        mbi.RegionSize, ok};
  }
  return ok;
}

void InvalidateHostPageCache() {
  g_hprCacheCount = 0;
  g_hprCacheNext = 0;
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

// Stage C — run the guest's own vertex shader over this draw's vertices.
// Defined further down, next to the microcode it needs; declared here because
// the draw builder is the only place that has the vertices and the streams
// together.
void ProbeShaderExecution(const mx::hle::DrawCall& dc, uint32_t handle,
                          const mx::hle::HleStream* streams, uint32_t device,
                          uint8_t* base);
enum class ShaderApplyResult : uint8_t { kApplied, kNoCode, kFailed };
ShaderApplyResult ApplyShaderOutputs(
    mx::hle::DrawCall& dc, uint32_t handle,
    const mx::hle::HleStream* streams, uint32_t device, uint8_t* base,
    const mx::hle::PixelTextureBinding* texture_binding,
    const uint32_t* constant_snapshot = nullptr);
bool PrepareDrawTexture(mx::hle::DrawCall& dc, uint32_t pixel_shader,
                        uint32_t device, uint8_t* base,
                        mx::hle::PixelTextureBinding& binding);
void ProbePixelProfileForDraw(uint32_t pixel_shader, uint32_t device,
                              uint8_t* base,
                              const mx::hle::DrawCall& dc);

struct PendingHleDraw {
  mx::hle::DrawCall draw;
  std::array<mx::hle::HleStream, mx::hle::kMaxStreams> streams;
  std::array<uint32_t, kD3d9ConstRegs * 4> constants;
  mx::hle::PixelTextureBinding texture_binding;
  uint32_t vertex_shader = 0;
  uint32_t device = 0;
  bool have_texture = false;
};

std::vector<PendingHleDraw> g_pendingHleDraws;
uint64_t g_pendingQueued = 0, g_pendingApplied = 0, g_pendingDropped = 0;
constexpr size_t kMaxPendingHleDraws = 2048;

bool CaptureVertexConstants(uint32_t device, uint8_t* base,
                            std::array<uint32_t, kD3d9ConstRegs * 4>& out) {
  const uint32_t bytes = kD3d9ConstRegs * 16;
  if (!device || !HostPageReadable(REX_RAW_ADDR(device + 0x780)) ||
      !HostPageReadable(REX_RAW_ADDR(device + 0x780 + bytes - 4)))
    return false;
  for (uint32_t i = 0; i < out.size(); ++i)
    out[i] = REX_LOAD_U32(device + 0x780 + i * 4);
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

void BuildAndQueueDraw(bool indexed, uint32_t prim_type, uint32_t first,
                       uint32_t count, int32_t base_vertex, uint32_t device,
                       uint8_t* base) {
  using namespace mx::hle;
  const auto& st = DeviceState();

  HleDrawInputs in;
  in.indexed = indexed;
  in.prim_type = prim_type;
  in.first = first;
  in.count = count;
  in.base_vertex = base_vertex;

  const int id = g_currentDecl;
  if (id >= 0 && g_declLayoutOk[id]) in.layout = &g_declLayout[id];

  HleStream streams[kMaxStreams];
  for (uint32_t i = 0; i < kMaxStreams; ++i) {
    const auto& b = st.stream[i];
    if (!b.bound || !b.address || !b.size_bytes) continue;
    streams[i].host = reinterpret_cast<const uint8_t*>(REX_RAW_ADDR(b.address));
    streams[i].size_bytes = b.size_bytes;
    streams[i].stride = b.stride;
    streams[i].offset_bytes = b.offset_bytes;
    streams[i].endian = b.endian;
    streams[i].bound = true;
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
  if (!have_vp) return;
  if (have_vp) in.mvp = vp;

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
    return;
  }
  ++HleBuiltCount();

  // Stage 3's measurement, on the built positions rather than on raw bytes: the
  // vertices are already decoded and in host order here, so the probe scores the
  // same numbers the renderer would receive.
  {
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
        f << "    first host position = " << *(const float*)dc.vertices.data()
          << " " << *((const float*)dc.vertices.data() + 1) << " "
          << *((const float*)dc.vertices.data() + 2) << "\n";
        f.flush();
      }
    }
  }

  ProbeShaderExecution(dc, st.vs_seen ? st.vertex_shader : 0, streams, device,
                       base);

  // The declaration supplies inputs, not the position the GPU rasterizes.
  // Execute the bound guest shader and replace POSITION with its homogeneous
  // screen-space export before the inverse viewport in dc.mvp is applied.
  PixelTextureBinding texture_binding;
  dc.viewport_width = viewport_width;
  dc.viewport_height = viewport_height;
  const auto& rt = st.render_target[0];
  if (rt.valid) {
    dc.render_target_object = rt.object;
    dc.render_target_surface_info = rt.surface_info;
    dc.render_target_color_info = rt.color_info;
    dc.render_target_width = rt.width;
    dc.render_target_height = rt.height;
    // Reuse the established PM4-facing fields so diagnostics can compare the
    // two independent paths without another parallel vocabulary.
    dc.surface_base = rt.color_info & 0xFFFu;
    dc.surface_pitch = rt.surface_info & 0x3FFFu;
  }
  ProbePixelProfileForDraw(st.ps_seen ? st.pixel_shader : 0, device, base, dc);
  const bool have_texture =
      PrepareDrawTexture(dc, st.ps_seen ? st.pixel_shader : 0, device, base,
                         texture_binding);
  if (have_texture && texture_binding.sampler < kMaxSamplers) {
    const auto& sampled_texture = st.texture[texture_binding.sampler];
    const uint32_t texture_object = sampled_texture.object;
    if (const auto it = g_resolvedTextureTargets.find(texture_object);
        it != g_resolvedTextureTargets.end()) {
      dc.sampled_render_target_object = it->second;
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
  if (st.render_state.Seen(kRsColorWriteEnable)) {
    dc.colour_mask = st.render_state.value[kRsColorWriteEnable] & 0xFu;
    dc.om_seen |= 1u << 0;
  }
  if (st.render_state.Seen(kRsZEnable)) {
    if (st.render_state.value[kRsZEnable]) dc.depth_control = (1u << 1) | (1u << 2);
    dc.om_seen |= 1u << 1;
  }

  const uint32_t vertex_shader = st.vs_seen ? st.vertex_shader : 0;
  const ShaderApplyResult applied = ApplyShaderOutputs(
      dc, vertex_shader, streams, device, base,
      have_texture ? &texture_binding : nullptr);
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
  pending.have_texture = have_texture;
  if (!CaptureVertexConstants(device, base, pending.constants)) {
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

uint64_t g_aluRuns = 0, g_aluInClip = 0, g_aluDegenerate = 0;
uint64_t g_aluNoShader = 0, g_aluNoAttrs = 0, g_aluStrideMismatch = 0;
// fetch_slot did not invert to a stream in [0, kMaxStreams). Its own counter:
// "the stream mapping does not hold here" and "the bound stride disagrees" are
// different failures and folding them together would hide either one.
uint64_t g_aluBadStream = 0;

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

struct PatchedCode {
  std::vector<uint32_t> code;   // host-endian, from dest - kPatchWindowBack*4
  uint32_t expect_fetches = 0;  // what the binding table said
  uint32_t variant = 0;
  uint32_t code_off = 0;        // dwords into `code` where the CF section is
  bool     resolved = false;    // code_off was found by decoding, not assumed
};

// Winning start, as a signed dword distance from dest. The histogram is the
// finding: one value across every shader means a fixed layout.
std::map<int32_t, uint64_t> g_patchCodeOffsets;
std::map<uint32_t, PatchedCode> g_patchedCode;   // shader handle -> latest

uint64_t g_srcPatchHook = 0;    // draws whose code came from the patch hook
uint64_t g_srcNone = 0;
uint64_t g_patchDecodeOk = 0;     // decoded, and the count matched the table
uint64_t g_patchDecodeCount = 0;  // decoded, count disagreed with the table
uint64_t g_patchDecodeFail = 0;   // refused outright
std::map<std::string, uint64_t> g_patchDecodeFailWhy;
uint64_t g_aluConstReads = 0, g_aluConstZero = 0;
std::map<int, uint64_t> g_aluStatus;
std::map<uint32_t, uint64_t> g_aluBlocking;

// Stage D — cost. Draws entered, not draws offered: the difference between the
// two is every named skip below, and a rate quoted against the wrong
// denominator is how "35% of draws" turns into a claim about the whole title.
uint64_t g_aluDrawsEntered = 0;
uint64_t g_aluNanos = 0;

// Stage F — which space the exported position is in.
//
// Everything before this assumed clip space, and never tested it. The PM4 path
// established on the *same shaders* that the ring's exports read like window
// coordinates, which is the entire reason the renderer applies the viewport
// inverse. If that holds here too then the clip-volume test has been the wrong
// yardstick and the numbers it produced measure the wrong thing.
uint64_t g_spaceCount[4] = {};        // indexed by ExportSpace
// The same histogram as the raw one, but after the viewport inverse. If these
// positions are window coordinates, this is where they collapse into <=1.
uint64_t g_clipExecVp[kClipBucketCount] = {};
uint64_t g_vpApplied = 0;             // executions the inverse could be applied to

// Stage D2 — the two histograms, and the control's own degenerate count.
uint64_t g_clipExec[kClipBucketCount] = {};
uint64_t g_clipCtl[kClipBucketCount] = {};
uint64_t g_ctlVerts = 0, g_ctlDegenerate = 0, g_aluNoViewportDraws = 0;
// The guard the Stage 3 probe needed (d3d9_draw.cpp, kSpreadEpsilon): a
// transform that collapses distinct inputs to a point lands them all in one
// bucket and looks like agreement. Tracked per draw for the control, because a
// degenerate control is not a reference — it is a second way of saying nothing.
uint64_t g_ctlCollapsedDraws = 0;
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
std::map<uint32_t, ShaderScore> g_shaderScore;   // key: shader handle
// If D3D9 recycles a handle for a different shader, the table silently merges
// two populations and reads as a finding. Counted, and reported: a nonzero
// value invalidates every row rather than quietly biasing it.
uint64_t g_shaderIdentityChanged = 0;

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

// Some shaders are already resident before the title reaches
// PatchVertexShaderToMatchVertexDeclaration, so the exact post-call capture
// above never observes them.  SH_pPhysical still names the live allocation
// D3D9 binds for the draw.  Accept that second exact source only when one and
// only one CF start decodes and every resulting vfetch agrees with a currently
// bound stream, including its patched stride.  An unpatched template has blank
// fetch fields and fails this test; an arbitrary offset that happens to decode
// is rejected by the uniqueness requirement.
std::map<uint32_t, PatchedCode> g_liveVertexCode;
std::map<uint32_t, size_t> g_liveVertexFailedAtShaderCount;
uint64_t g_liveVertexResolved = 0, g_liveVertexAmbiguous = 0;
uint64_t g_liveVertexUnreadable = 0, g_liveVertexNoMatch = 0;


ShaderApplyResult ApplyShaderOutputs(
    mx::hle::DrawCall& dc, uint32_t handle,
    const mx::hle::HleStream* streams, uint32_t device, uint8_t* base,
    const mx::hle::PixelTextureBinding* texture_binding,
    const uint32_t* constant_snapshot) {
  using namespace mx::hle;
  const uint64_t attempt = ++g_hleShaderAttempts;
  struct ReportApply {
    uint64_t attempt;
    ~ReportApply() {
      if (attempt > 10 && (attempt % 250) != 0) return;
      REXLOG_INFO(
          "d3d9: HLE shader output attempts {}: applied {} draws / {} "
          "vertices; skipped no-code {} decode {} stream {} constants {} "
          "vertex {}; output transform identity {} viewport {}; live shader "
          "resolved {} no-match {} ambiguous {} unreadable {}",
          g_hleShaderAttempts, g_hleShaderDraws, g_hleShaderVertices,
          g_hleShaderNoCode, g_hleShaderBadDecode, g_hleShaderBadStream,
          g_hleShaderBadConstants, g_hleShaderBadVertex,
          g_hleShaderIdentityMvp, g_hleShaderViewportMvp,
          g_liveVertexResolved, g_liveVertexNoMatch, g_liveVertexAmbiguous,
          g_liveVertexUnreadable);
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

  std::vector<VertexAttribute> attrs;
  const char* why = nullptr;
  if (!DecodeVertexShaderFetches(patch.code.data() + patch.code_off,
                                 uint32_t(patch.code.size() - patch.code_off),
                                 attrs, &why) ||
      attrs.empty() || attrs.size() != patch.expect_fetches) {
    ++g_hleShaderBadDecode;
    return ShaderApplyResult::kFailed;
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
    if (!CaptureVertexConstants(device, base, captured)) {
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

    const AluResult r = ExecuteVertexShader(
        patch.code.data() + patch.code_off,
        uint32_t(patch.code.size() - patch.code_off), attrs, values, alu_in);
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

    // PA_CL_VTE_CNTL is 0x300 in the captured stream: XYZ are already
    // multiplied by 1/W0. Preserve those post-divide values; reconstructing a
    // homogeneous W from the export was tested against ST_Southwest and clipped
    // the entire coloured scene away, so it is not the value D3D12 needs here.
    const float p[3] = {r.position[0], r.position[1], r.position[2]};
    std::memcpy(transformed.data() + size_t(v) * dc.vertex_stride, p,
                sizeof(p));
    // Resolved render-target samples intentionally carry no CPU texture
    // payload: the renderer samples the ordered target resource instead. UVs
    // are shader outputs and must be populated for both resource paths. The old
    // `dc.texture` guard left every resolved sample at BuildHleDraw's default
    // (0,0), which explains the flat single-texel compositor wedges.
    if (texture_binding &&
        texture_binding->src_reg < AluResult::kMaxInterpolators &&
        (r.export_mask & (1u << texture_binding->src_reg))) {
      const auto& e = r.exports[texture_binding->src_reg];
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
        std::memcpy(transformed.data() + size_t(v) * dc.vertex_stride + 28,
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
          "d3d9: HLE UV r{} swiz=0x{:02X} denorm={} wrote {}/{} missing {}; "
          "range ({:.5g},{:.5g})..({:.5g},{:.5g}) collapsed={} cpu={} "
          "resolved=0x{:08X} extent={}x{}",
          texture_binding->src_reg, texture_binding->src_swizzle,
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
              "dest=r{} -> ({:.5g},{:.5g},{:.5g},{:.5g})",
              a, attr_stream[a], attrs[a].fetch_slot, attrs[a].offset_bytes,
              s.stride, attrs[a].format, attrs[a].dest_reg, probe_values[a][0],
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
  if (identity_in_clip > viewport_in_clip) {
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

void FinalizePendingD3D9DrawsImpl(uint8_t* base) {
  const size_t count = g_pendingHleDraws.size();
  uint64_t applied = 0, dropped = 0;
  for (PendingHleDraw& pending : g_pendingHleDraws) {
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
  if (count && (g_pendingQueued <= 32 || (g_pendingQueued % 1000) < count)) {
    REXLOG_INFO("d3d9: deferred HLE draws: frame {} applied {} dropped {}; "
                "cumulative queued {} applied {} dropped {}",
                count, applied, dropped, g_pendingQueued, g_pendingApplied,
                g_pendingDropped);
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

struct ResolvedPixelBinding {
  std::vector<mx::hle::PixelTextureBinding> bindings;
  const char* fail = nullptr;
  uint32_t code_offset_dwords = 0;
  bool decoded = false;
};
std::map<uint32_t, ResolvedPixelBinding> g_resolvedPixelBindings;
std::map<uint64_t, std::shared_ptr<const mx::hle::HleTexturePayload>>
    g_hleCpuTextures;
std::map<uint64_t, bool> g_hleEmptyTextures;

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

bool ReadLiveTextureFetch(uint32_t device, uint8_t* base, uint32_t sampler,
                          uint32_t out[6]) {
  if (!out || sampler >= mx::hle::kMaxSamplers) return false;
  std::memset(out, 0, sizeof(uint32_t) * 6);
  const uint32_t fetch_at = device + 0x480 + sampler * 24;
  if (device && HostPageReadable(REX_RAW_ADDR(fetch_at)) &&
      HostPageReadable(REX_RAW_ADDR(fetch_at + 20))) {
    for (uint32_t i = 0; i < 6; ++i)
      out[i] = REX_LOAD_U32(fetch_at + i * 4);
    if ((out[0] & 3u) == 2u) return true;
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
bool ResolvePixelBindingForDraw(uint32_t handle, uint32_t device,
                                uint8_t* base,
                                mx::hle::PixelTextureBinding& out) {
  if (ResolvePixelBinding(handle, out)) return true;
  const ResolvedPixelBinding* profile = ResolvePixelProfile(handle);
  if (!profile || !profile->decoded || profile->bindings.empty()) return false;

  int best_score = -1;
  mx::hle::HleTextureSource best_source;
  bool found = false;
  for (const auto& candidate : profile->bindings) {
    if (candidate.sampler >= mx::hle::kMaxSamplers) continue;
    uint32_t fetch[6];
    if (!ReadLiveTextureFetch(device, base, candidate.sampler, fetch)) continue;
    mx::hle::HleTextureSource source;
    if (!mx::hle::DescribeHleTexture2D(fetch, source, nullptr)) continue;

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
         source.host_format == mx::hle::HostTextureFormat::kRgba16Float))
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
        score += mapped_render_target ? 40 : 200;
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
    if (score <= best_score) continue;
    best_score = score;
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

bool PrepareDrawTexture(mx::hle::DrawCall& dc, uint32_t pixel_shader,
                        uint32_t device, uint8_t* base,
                        mx::hle::PixelTextureBinding& binding) {
  using namespace mx::hle;
  static uint64_t s_attempts = 0, s_ready = 0, s_no_shader = 0;
  static uint64_t s_no_binding = 0, s_bad_desc = 0, s_unreadable = 0;
  static uint64_t s_mapped = 0, s_empty = 0, s_semantic_reject = 0;
  ++s_attempts;
  if ((!pixel_shader ||
       !ResolvePixelBindingForDraw(pixel_shader, device, base, binding)) &&
      !ReadBoundPixelShader(device, base, pixel_shader, binding)) {
    ++s_no_shader;
    if (s_no_shader <= 8 || (s_attempts % 2500) == 0) {
      const uint32_t direct =
          device && HostPageReadable(REX_RAW_ADDR(device + 0x3244))
              ? REX_LOAD_U32(device + 0x3244)
              : 0;
      REXLOG_INFO("d3d9: HLE texture fallback: no eligible pixel shader "
                  "(attempt {}, setter=0x{:08X}, device=0x{:08X})",
                  s_attempts, pixel_shader, direct);
    }
    return false;
  }
  if (binding.sampler >= kMaxSamplers) {
    ++s_no_binding;
    if (s_no_binding <= 8)
      REXLOG_INFO("d3d9: HLE texture fallback: sampler {} out of range",
                  binding.sampler);
    return false;
  }
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
    if (ReadLiveTextureFetch(device, base, binding.sampler, mapped_fetch) &&
        DescribeHleTexture2D(mapped_fetch, mapped_source, &mapped_why)) {
      dc.sampled_texture_width = mapped_source.width;
      dc.sampled_texture_height = mapped_source.height;
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
    if (s_bad_desc <= 12) {
      REXLOG_INFO("d3d9: HLE texture fallback: sampler {} descriptor rejected "
                  "({}), words {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}",
                  binding.sampler, why ? why : "?", fetch[0], fetch[1],
                  fetch[2], fetch[3], fetch[4], fetch[5]);
    }
    return false;
  }
  dc.sampled_texture_width = source.width;
  dc.sampled_texture_height = source.height;
  if (source.host_format == HostTextureFormat::kBc5 ||
      source.host_format == HostTextureFormat::kR16Float ||
      source.host_format == HostTextureFormat::kRgba16Float) {
    ++s_semantic_reject;
    if (s_semantic_reject <= 12) {
      REXLOG_INFO("d3d9: HLE texture fallback: sampler {} format {} is not "
                  "an immutable colour asset",
                  binding.sampler, uint32_t(source.host_format));
    }
    return false;
  }
  const uint64_t key = HleTextureKey(fetch);
  if (g_hleEmptyTextures.contains(key)) {
    ++s_empty;
    return false;
  }
  auto cached = g_hleCpuTextures.find(key);
  if (cached != g_hleCpuTextures.end()) {
    dc.texture = cached->second;
    ++s_ready;
    return true;
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
    g_hleEmptyTextures.emplace(key, true);
    ++s_empty;
    if (s_empty <= 12) {
      REXLOG_INFO("d3d9: HLE texture fallback: sampler {} {}x{} format {} "
                  "decoded to an all-zero guest payload",
                  binding.sampler, source.width, source.height,
                  uint32_t(source.host_format));
    }
    return false;
  }
  payload->key = key;
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

void ProbeShaderExecution(const mx::hle::DrawCall& dc, uint32_t handle,
                          const mx::hle::HleStream* streams, uint32_t device,
                          uint8_t* base) {
  using namespace mx::hle;
  if (!REXCVAR_GET(hle_capture) || !handle || !device) return;
  // Stage D: the sampling rate is the measurement, so it is a cvar and not a
  // constant. 0 is off, N runs one draw in N, 1 runs every draw — and only the
  // last of those says what using the interpreter would actually cost.
  const uint32_t every = REXCVAR_GET(hle_shader_exec);
  if (every == 0) return;
  static uint64_t s_draws = 0;
  if ((++s_draws % every) != 0) return;

  // Timed from here, so the cost includes the lookups and the 1,024-word
  // constant copy below and not merely the interpreter. Charging the frame only
  // for ExecuteVertexShader would understate it by exactly the part that is
  // easiest to forget.
  const auto t0 = std::chrono::steady_clock::now();
  struct ChargeTime {
    std::chrono::steady_clock::time_point t;
    ~ChargeTime() {
      g_aluNanos += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::steady_clock::now() - t)
                                 .count());
    }
  } charge{t0};
  ++g_aluDrawsEntered;

  // Stage G: the exact code first, the heuristic only as a fallback, and each
  // counted separately so "coverage improved" is a measurement rather than a
  // hope. Both paths are kept because the patch hook fires on the lazy-state
  // path — a shader bound but never re-patched has no entry, and swapping one
  // heuristic for one assumption would not be progress.
  const std::vector<uint32_t>* codep = nullptr;
  uint32_t off = 0;
  static std::vector<VertexAttribute> decoded;
  const std::vector<VertexAttribute>* attrsp = nullptr;

  auto pi = g_patchedCode.find(handle);
  if (pi != g_patchedCode.end() && pi->second.resolved) {
    decoded.clear();
    const char* why = nullptr;
    const uint32_t s = pi->second.code_off;
    if (DecodeVertexShaderFetches(pi->second.code.data() + s,
                                  uint32_t(pi->second.code.size() - s), decoded,
                                  &why)) {
      // The binding table said how many vfetches this shader has. If the decode
      // disagrees, the captured window did not start where it was assumed to —
      // a wrong start would otherwise decode into plausible nonsense.
      if (decoded.size() == pi->second.expect_fetches) {
        ++g_patchDecodeOk;
        codep = &pi->second.code;
        off = s;
        attrsp = &decoded;
      } else {
        ++g_patchDecodeCount;
      }
    } else {
      ++g_patchDecodeFail;
      ++g_patchDecodeFailWhy[why ? why : "?"];
    }
  }

  // The PM4 content-match fallback that used to sit here is REMOVED
  // 2026-08-06, along with the cross-check that compared our decode of D3D9's
  // patched output against PM4's decode of the ring's copy. Both had one
  // source and it is gone.
  //
  // Neither is missed, and that is measured rather than assumed: stageG
  // reported `patch hook (exact) 32212 (100%), content match >=90% 0 (0%),
  // none 0 (0%)`. The heuristic was never once the source, so the branch it
  // fed was dead code carrying a >=90%-similarity guess that could have picked
  // the wrong shader if it ever had fired.
  if (!codep) {
    ++g_srcNone;
    ++g_aluNoShader;
    return;
  }
  ++g_srcPatchHook;

  const std::vector<VertexAttribute>& attrs = *attrsp;
  const std::vector<uint32_t>& code = *codep;
  if (attrs.empty()) { ++g_aluNoAttrs; return; }

  // Which stream each attribute fetches from.
  //
  // No longer a guess. PatchVertexShaderToMatchVertexDeclaration writes
  // `95 - element.stream` into the vfetch's constant field (subfic r20, r5,
  // 0x5F at 0x82564E30), so inverting it gives the D3D9 stream number. The
  // prediction probe agreed with what D3D9 actually wrote on 52 of 53 slots.
  //
  // This also explains why every observed fetch_slot was 95: it is stream 0.
  // The value is ambiguous — the no-match path writes 95 too — but an unmatched
  // vfetch is left with a canned format (0x60000) and swizzle (0x9250), and the
  // measurement found 0 unmatched slots in this title.
  static std::vector<uint32_t> astream;
  astream.assign(attrs.size(), 0);
  for (size_t a = 0; a < attrs.size(); ++a) {
    const uint32_t fs = attrs[a].fetch_slot;
    if (fs > 95 || (95u - fs) >= kMaxStreams) { ++g_aluBadStream; return; }
    astream[a] = 95u - fs;
    const HleStream& sa = streams[astream[a]];
    if (!sa.bound || !sa.host || sa.stride == 0 ||
        sa.stride != attrs[a].stride_bytes) {
      ++g_aluStrideMismatch;
      return;
    }
  }

  // Stage I: this draw's row. Keyed by the shader handle, which is only a valid
  // identity if D3D9 does not reuse it — so the identity is checked, not
  // assumed, against two things that cannot both survive a substitution.
  ShaderScore& ss = g_shaderScore[handle];
  const uint32_t first_dword = off < code.size() ? code[off] : 0;
  if (ss.draws == 0) {
    ss.attrs = uint32_t(attrs.size());
    ss.first_dword = first_dword;
  } else if (ss.attrs != attrs.size() || ss.first_dword != first_dword) {
    ++g_shaderIdentityChanged;
  }
  ++ss.draws;

  // The constant file, straight from the device. Const(i) reads
  // alu_consts[i*4], and D3D9 register N lives at +0x780 + N*16, so the two are
  // the same indexing and no rebase is needed — the API applied the base.
  static std::vector<uint32_t> consts;
  consts.assign(kD3d9ConstRegs * 4, 0);
  if (!HostPageReadable(REX_RAW_ADDR(device + 0x780)) ||
      !HostPageReadable(REX_RAW_ADDR(device + 0x780 + kD3d9ConstRegs * 16 - 4)))
    return;
  for (uint32_t i = 0; i < kD3d9ConstRegs * 4; ++i) {
    const uint32_t bits = REX_LOAD_U32(device + 0x780 + i * 4);
    consts[i] = bits;
  }
  AluInputs in;
  in.alu_consts = consts.data();
  in.alu_const_dwords = uint32_t(consts.size());

  const uint32_t want = REXCVAR_GET(hle_shader_verts);
  const uint32_t n = dc.vertex_count < want ? dc.vertex_count : want;

  // The control's transform: the viewport inverse, which is what this draw is
  // actually rendered with today and what scored 0% under Stage 3's threshold.
  // Built once per draw; a draw with no viewport yet gets no control rather
  // than an identity standing in for one.
  float ctl[16];
  const bool have_ctl = BuildViewportMvp(device, base, ctl);
  if (!have_ctl) ++g_aluNoViewportDraws;
  float ctl_lo[2] = {1e30f, 1e30f}, ctl_hi[2] = {-1e30f, -1e30f};
  uint32_t ctl_scored = 0;

  uint8_t vtx[kMaxStreams][256];
  std::vector<std::array<float, 4>> values(attrs.size());
  for (uint32_t v = 0; v < n; ++v) {
    // The stream index this built vertex came from. dc.vertices packs the
    // referenced range starting at first_vertex, so using v alone would run the
    // shader on one vertex and the control on a different one, and the two
    // histograms would not be comparable — which is the whole point of having
    // a control.
    const uint64_t src = uint64_t(dc.first_vertex) + v;

    // One decoded vertex per stream this shader actually reads, fetched once
    // and shared by every attribute that comes from it.
    bool have[kMaxStreams] = {};
    bool ranged = true;
    for (size_t a = 0; a < attrs.size() && ranged; ++a) {
      const uint32_t si = astream[a];
      if (have[si]) continue;
      const HleStream& sa = streams[si];
      const uint64_t byte_off = src * sa.stride + sa.offset_bytes;
      if (byte_off + sa.stride > sa.size_bytes ||
          sa.stride > sizeof(vtx[0])) {
        ranged = false;
        break;
      }
      // Copied in guest byte order. The swap is applied per attribute below,
      // where the format — and so the correct swap width — is known.
      std::memcpy(vtx[si], sa.host + byte_off, sa.stride);
      have[si] = true;
    }
    if (!ranged) break;

    for (size_t a = 0; a < attrs.size(); ++a) {
      float o[4] = {0, 0, 0, 1};
      ReadVertexAttribute(vtx[astream[a]], streams[astream[a]].stride, attrs[a],
                          streams[astream[a]].endian, o);
      values[a] = {o[0], o[1], o[2], o[3]};
    }

    const AluResult r = ExecuteVertexShader(code.data() + off,
                                            uint32_t(code.size() - off), attrs,
                                            values, in);
    ++g_aluRuns;
    ++g_aluStatus[int(r.status)];
    if (r.blocking_opcode) ++g_aluBlocking[r.blocking_opcode];
    g_aluConstReads += r.const_reads;
    g_aluConstZero += r.const_zero_reads;

    const float* p = r.position;
    const bool finite = std::isfinite(p[0]) && std::isfinite(p[1]) &&
                        std::isfinite(p[2]) && std::isfinite(p[3]);
    // The same guard the transform probe needed: an export of (0,0,0,w=0) sits
    // inside any volume and means nothing.
    ++ss.execs;
    // The shader's own position input, before the shader touches it. attrs[0]
    // is the attribute at offset 0, which every world shader here declares as
    // its position.
    if (!attrs.empty()) {
      ++ss.in_seen;
      for (uint32_t c = 0; c < 4; ++c) {
        const float f = values[0][c];
        if (f < ss.in_lo[c]) ss.in_lo[c] = f;
        if (f > ss.in_hi[c]) ss.in_hi[c] = f;
      }
    }
    if (p[0] == 0.0f && p[1] == 0.0f && p[2] == 0.0f) {
      ++g_aluDegenerate;
      ++ss.degenerate;
    } else {
      // Kept exactly as Stage C measured it — full volume, z included — so the
      // 35% stays reproducible beside the histogram rather than being quietly
      // redefined into a different number with the same name.
      if (finite && p[3] > 0.0f && p[0] >= -p[3] && p[0] <= p[3] &&
          p[1] >= -p[3] && p[1] <= p[3] && p[2] >= 0.0f && p[2] <= p[3]) {
        ++g_aluInClip;
        ++ss.in_clip;
      }
      ++g_clipExec[ClassifyClip(p)];
      ++ss.clip[ClassifyClip(p)];
    }

    // Stage F. Scored on every execution including the degenerate ones —
    // ClassifyExportSpace takes those out itself, and it has to, because the
    // origin is inside both regions.
    {
      float dv[6];
      if (ReadDeviceViewport(device, base, dv)) {
        const float xs = dv[2] * 0.5f;
        const float xo = dv[0] + xs;
        const float ys = -dv[3] * 0.5f;
        const float yo = dv[1] + dv[3] * 0.5f;
        const ExportSpace sp =
            ClassifyExportSpace(p[0], p[1], p[3], xs, xo, ys, yo);
        ++g_spaceCount[uint32_t(sp)];
        ++ss.space[uint32_t(sp)];
        ss.vp_extent = (uint64_t(uint32_t(dv[2])) << 32) | uint32_t(dv[3]);

        // The same buckets after the viewport inverse. Done on the divided
        // position and rewrapped with w=1, so the bucket function sees exactly
        // what the renderer would put on screen.
        if (p[3] != 0.0f && std::isfinite(p[3])) {
          const float dx = p[0] / p[3], dy = p[1] / p[3];
          const float q[4] = {(dx - xo) / xs, (dy - yo) / ys, 0.0f, 1.0f};
          ++g_clipExecVp[ClassifyClip(q)];
          ++g_vpApplied;
        }
      }
    }

    // Stage I: this shader's first execution, in full — the raw bytes in, the
    // decoded attributes, and the position out. Recorded for every shader
    // because which one is worst is not known until the report, by which time
    // the vertex is gone. This is the seed for verifying a draw by hand against
    // the disassembly, and it costs one string per distinct shader.
    if (ss.first_exec.empty() && g_shaderScore.size() <= 64) {
      char buf[256];
      std::snprintf(buf, sizeof(buf),
                    "shader 0x%08X, %u attrs, vertex %llu of draw:\n",
                    handle, uint32_t(attrs.size()),
                    (unsigned long long)src);
      ss.first_exec = buf;
      for (size_t a = 0; a < attrs.size(); ++a) {
        const HleStream& sa = streams[astream[a]];
        std::snprintf(buf, sizeof(buf),
                      "  attr%-2zu r%-2u%s stream %u  fetch %u  fmt 0x%02X  "
                      "off %u  stride %u  ->  %g %g %g %g\n  raw",
                      a, attrs[a].dest_reg,
                      attrs[a].feeds_position ? " (position)" : "          ",
                      astream[a], attrs[a].fetch_slot, attrs[a].format,
                      attrs[a].offset_bytes, attrs[a].stride_bytes,
                      values[a][0], values[a][1], values[a][2], values[a][3]);
        ss.first_exec += buf;
        // The bytes the value was read from, byte-swapped exactly as the fetch
        // saw them, so the decode can be checked by hand and not just believed.
        const uint32_t o = attrs[a].offset_bytes;
        for (uint32_t k = 0; k < 16 && o + k < sa.stride; ++k) {
          std::snprintf(buf, sizeof(buf), " %02X", vtx[astream[a]][o + k]);
          ss.first_exec += buf;
        }
        ss.first_exec += "\n";
      }
      std::snprintf(buf, sizeof(buf), "  position = %g %g %g w=%g\n", p[0],
                    p[1], p[2], p[3]);
      ss.first_exec += buf;
    }

    // The first few exports in full. A bucket count cannot show that every
    // position reads (640, 0, 1, 1) — which is how the ring's space was
    // identified in the first place.
    {
      static uint32_t s_dumped = 0;
      if (s_dumped < 8) {
        ++s_dumped;
        auto& f = DeclFile();
        f << "HLE EXPORT " << s_dumped << ": pos = " << p[0] << " " << p[1]
          << " " << p[2] << " w=" << p[3];
        float dv[6];
        if (ReadDeviceViewport(device, base, dv))
          f << "   device viewport " << dv[0] << "," << dv[1] << " " << dv[2]
            << "x" << dv[3];
        const auto& sv = DeviceState().viewport;
        if (sv.seen)
          f << "   arg shadow " << sv.width << "x" << sv.height;
        f << "\n";
        f.flush();
      }
    }

    // The control, on the same vertex: the host position BuildHleDraw decoded
    // for it, through the viewport inverse.
    if (have_ctl &&
        (size_t(v) + 1) * kHostVertexStride <= dc.vertices.size()) {
      const float* hp =
          reinterpret_cast<const float*>(dc.vertices.data() +
                                         size_t(v) * kHostVertexStride);
      if (hp[0] == 0.0f && hp[1] == 0.0f && hp[2] == 0.0f) {
        ++g_ctlDegenerate;
      } else {
        float o[4];
        for (uint32_t r4 = 0; r4 < 4; ++r4) {
          o[r4] = ctl[r4 * 4 + 0] * hp[0] + ctl[r4 * 4 + 1] * hp[1] +
                  ctl[r4 * 4 + 2] * hp[2] + ctl[r4 * 4 + 3];
        }
        ++g_ctlVerts;
        ++g_clipCtl[ClassifyClip(o)];
        if (o[3] != 0.0f) {
          const float nx = o[0] / o[3], ny = o[1] / o[3];
          if (nx < ctl_lo[0]) ctl_lo[0] = nx;
          if (nx > ctl_hi[0]) ctl_hi[0] = nx;
          if (ny < ctl_lo[1]) ctl_lo[1] = ny;
          if (ny > ctl_hi[1]) ctl_hi[1] = ny;
          ++ctl_scored;
        }
      }
    }
  }

  // Did the control collapse this draw's distinct vertices onto one point? A
  // transform that does lands every vertex in one bucket and reads as a strong
  // signal while meaning nothing — the failure the Stage 3 probe hit and had to
  // guard against (kSpreadEpsilon, d3d9_draw.cpp). Counted, not discarded: how
  // often the reference degenerates is itself part of how much it is worth.
  if (ctl_scored > 1 && (ctl_hi[0] - ctl_lo[0]) < kCtlSpreadEpsilon &&
      (ctl_hi[1] - ctl_lo[1]) < kCtlSpreadEpsilon) {
    ++g_ctlCollapsedDraws;
  }
}

// Stage D — the cost, stated against a frame rather than as a bare total.
// Reported even when nothing ran, because "the interpreter was off and the
// frame took N ms" is the baseline every other row is compared to.
//
// **Windowed, not cumulative.** A first run reported the run-wide mean and it
// climbed monotonically — 219 ms/frame at frame 146, 808 ms at frame 186 —
// because this title genuinely degrades as it runs. A run-wide mean therefore
// measures mostly how long the run had been going, and comparing two configs on
// it compares their durations. The delta since the previous report is the
// number that can be compared; the cumulative figures stay beside it so the
// drift remains visible rather than hidden by the fix.
void ReportShaderExecutionCost() {
  static uint64_t s_frames = 0, s_frameNs = 0, s_aluNs = 0, s_draws = 0,
                  s_runs = 0;
  const uint64_t frames = mx::hle::D3D9FrameCount();
  const uint64_t frame_ns = mx::hle::D3D9FrameNanos();

  const uint64_t d_frames = frames - s_frames;
  const uint64_t d_frame_ns = frame_ns - s_frameNs;
  const uint64_t d_alu_ns = g_aluNanos - s_aluNs;
  const uint64_t d_draws = g_aluDrawsEntered - s_draws;
  const uint64_t d_runs = g_aluRuns - s_runs;
  s_frames = frames; s_frameNs = frame_ns; s_aluNs = g_aluNanos;
  s_draws = g_aluDrawsEntered; s_runs = g_aluRuns;

  const double win_frame_ms =
      d_frames ? double(d_frame_ns) / double(d_frames) / 1e6 : 0.0;
  const double win_alu_ms =
      d_frames ? double(d_alu_ns) / double(d_frames) / 1e6 : 0.0;
  REXLOG_INFO(
      "d3d9: stageD  cost — exec={} verts={} | window: {} frames, {:.1f} "
      "ms/frame, interpreter {:.3f} ms/frame ({:.2f}% of a frame), {} draws "
      "entered, {} vertices | run total: {} frames, {:.1f}s, interpreter "
      "{:.1f} ms, {} vertices",
      REXCVAR_GET(hle_shader_exec), REXCVAR_GET(hle_shader_verts), d_frames,
      win_frame_ms, win_alu_ms,
      win_frame_ms > 0.0 ? (win_alu_ms / win_frame_ms) * 100.0 : 0.0, d_draws,
      d_runs, frames, double(frame_ns) / 1e9, double(g_aluNanos) / 1e6,
      g_aluRuns);
}

// Stage D2 — the two distributions, side by side, as counts.
void ReportClipHistogram() {
  if (!g_aluRuns) return;
  for (uint32_t b = 0; b < kClipBucketCount; ++b) {
    REXLOG_INFO("d3d9: stageD2 clip {:>9} : executed {:>7}   viewport-inverse {:>7}",
                kClipBucketName[b], g_clipExec[b], g_clipCtl[b]);
  }
  REXLOG_INFO(
      "d3d9: stageD2 control — {} vertices transformed, {} skipped as "
      "degenerate input, {} draws had no viewport yet, {} draws collapsed to a "
      "point (a collapsed control is not a reference)",
      g_ctlVerts, g_ctlDegenerate, g_aluNoViewportDraws, g_ctlCollapsedDraws);
  REXLOG_INFO(
      "d3d9: stageD2 buckets are max(|x/w|,|y/w|); the '<=1' row is NOT the "
      "same test as stageC's in-clip count, which also bounds z");

  // Stage F. The question every number above assumed an answer to.
  const uint64_t sp_total = g_spaceCount[0] + g_spaceCount[1] + g_spaceCount[2] +
                            g_spaceCount[3];
  if (sp_total) {
    REXLOG_INFO(
        "d3d9: stageF  export space — clip-like {} ({}%), window-like {} ({}%), "
        "neither {}, degenerate {} — of {} scored (clip wins ties, degenerate "
        "removed first)",
        g_spaceCount[uint32_t(mx::hle::ExportSpace::kClipLike)],
        (g_spaceCount[uint32_t(mx::hle::ExportSpace::kClipLike)] * 100) /
            sp_total,
        g_spaceCount[uint32_t(mx::hle::ExportSpace::kWindowLike)],
        (g_spaceCount[uint32_t(mx::hle::ExportSpace::kWindowLike)] * 100) /
            sp_total,
        g_spaceCount[uint32_t(mx::hle::ExportSpace::kNeither)],
        g_spaceCount[uint32_t(mx::hle::ExportSpace::kDegenerate)], sp_total);
    for (uint32_t b = 0; b < kClipBucketCount; ++b) {
      REXLOG_INFO(
          "d3d9: stageF  clip {:>9} : raw {:>7}   after viewport inverse {:>7}",
          kClipBucketName[b], g_clipExec[b], g_clipExecVp[b]);
    }
    REXLOG_INFO(
        "d3d9: stageF  viewport inverse applied to {} of {} executions; if "
        "these positions are window coordinates this is where they collapse "
        "into <=1",
        g_vpApplied, sp_total);
  }
  if (!g_viewportExtents.empty()) {
    std::string ve;
    for (const auto& [k, n] : g_viewportExtents) {
      char buf[48];
      std::snprintf(buf, sizeof(buf), "%ux%u:%llu ", uint32_t(k >> 32),
                    uint32_t(k), (unsigned long long)n);
      ve += buf;
    }
    REXLOG_INFO(
        "d3d9: stageF  SetViewport *argument* extents seen — {}(the argument "
        "shadow keeps only the last; the transform now reads the device's "
        "clamped copy at +0x3218 instead)",
        ve);
  }
  if (g_vpFromDevice || g_vpFromShadow) {
    REXLOG_INFO(
        "d3d9: stageF  viewport source — device +0x3218 {}, argument shadow "
        "fallback {}; device disagreed with the shadow's extent on {} of them "
        "(that difference is the clamp, and the bug)",
        g_vpFromDevice, g_vpFromShadow, g_vpDisagreed);
  }
}

// Stage I — the same population, broken down by the shader that produced it.
void ReportPerShader() {
  if (g_shaderScore.empty()) return;

  // The per-shader counts must sum to the globals they were taken beside. If
  // they do not, the attribution is wrong and every row below is a plausible
  // table describing nothing — so this is reported, not assumed.
  uint64_t sum_execs = 0, sum_inclip = 0, sum_space = 0;
  for (const auto& [h, s] : g_shaderScore) {
    (void)h;
    sum_execs += s.execs;
    sum_inclip += s.in_clip;
    for (uint32_t i = 0; i < 4; ++i) sum_space += s.space[i];
  }
  const uint64_t g_space_total = g_spaceCount[0] + g_spaceCount[1] +
                                 g_spaceCount[2] + g_spaceCount[3];
  REXLOG_INFO(
      "d3d9: stageI  attribution check — per-shader sums vs globals: execs "
      "{}/{} {}, in-clip {}/{} {}, space-scored {}/{} {}",
      sum_execs, g_aluRuns, sum_execs == g_aluRuns ? "ok" : "MISMATCH",
      sum_inclip, g_aluInClip, sum_inclip == g_aluInClip ? "ok" : "MISMATCH",
      sum_space, g_space_total,
      sum_space == g_space_total ? "ok" : "MISMATCH");
  REXLOG_INFO(
      "d3d9: stageI  handle identity — {} shaders scored, {} times a handle's "
      "fetch count or first dword changed under it ({})",
      g_shaderScore.size(), g_shaderIdentityChanged,
      g_shaderIdentityChanged
          ? "NONZERO: handles are reused, the rows below merge populations"
          : "handles are stable, the rows below are one shader each");

  // Most-executed first: a shader that is 100% broken over six executions is
  // not a finding, and the row has to make that visible.
  std::vector<const std::pair<const uint32_t, ShaderScore>*> rows;
  for (const auto& e : g_shaderScore) rows.push_back(&e);
  std::sort(rows.begin(), rows.end(), [](auto* a, auto* b) {
    return a->second.execs > b->second.execs;
  });

  auto pct = [](uint64_t n, uint64_t d) { return d ? (n * 100) / d : 0; };
  uint32_t clean = 0, broken = 0;
  uint64_t clean_execs = 0, broken_execs = 0;
  const ShaderScore* worst = nullptr;
  uint32_t worst_handle = 0;
  for (const auto* r : rows) {
    const ShaderScore& s = r->second;
    const uint64_t sp = s.space[0] + s.space[1] + s.space[2] + s.space[3];
    if (!sp) continue;
    const uint64_t cl = s.space[uint32_t(mx::hle::ExportSpace::kClipLike)];
    const uint64_t ne = s.space[uint32_t(mx::hle::ExportSpace::kNeither)];
    if (pct(cl, sp) >= 90) { ++clean; clean_execs += s.execs; }
    if (pct(ne, sp) >= 90) { ++broken; broken_execs += s.execs; }
    // Worst = most executions landing in no modelled space. Weighted by count
    // so the seed for hand-verification is a shader that actually matters.
    if (!worst || ne > worst->space[uint32_t(mx::hle::ExportSpace::kNeither)]) {
      worst = &s;
      worst_handle = r->first;
    }
  }

  uint32_t shown = 0;
  for (const auto* r : rows) {
    if (shown++ >= 12) break;
    const ShaderScore& s = r->second;
    const uint64_t sp = s.space[0] + s.space[1] + s.space[2] + s.space[3];
    const uint64_t scored = s.execs - s.degenerate;
    REXLOG_INFO(
        "d3d9: stageI  shader 0x{:08X}: {:>6} execs over {:>5} draws, {} "
        "attrs, rt {}x{} — clip-like {}%  window-like {}%  neither {}%  "
        "degenerate {}%   in-clip {}%",
        r->first, s.execs, s.draws, s.attrs, uint32_t(s.vp_extent >> 32),
        uint32_t(s.vp_extent),
        pct(s.space[uint32_t(mx::hle::ExportSpace::kClipLike)], sp),
        pct(s.space[uint32_t(mx::hle::ExportSpace::kWindowLike)], sp),
        pct(s.space[uint32_t(mx::hle::ExportSpace::kNeither)], sp),
        pct(s.space[uint32_t(mx::hle::ExportSpace::kDegenerate)], sp),
        pct(s.in_clip, scored));
  }
  if (rows.size() > shown) {
    REXLOG_INFO("d3d9: stageI  ... and {} more shaders not shown",
                rows.size() - shown);
  }

  REXLOG_INFO(
      "d3d9: stageI  {} shaders: {} are >=90% clip-like ({} execs, {}% of "
      "all), {} are >=90% neither ({} execs, {}% of all) — a failure held by a "
      "few shaders is a bug with an address; one spread evenly is a wrong "
      "model",
      g_shaderScore.size(), clean, clean_execs, pct(clean_execs, g_aluRuns),
      broken, broken_execs, pct(broken_execs, g_aluRuns));

  // The seeds for verifying draws by hand, written where the declarations
  // already go.
  //
  // Ranking by raw `neither` count picked the 129x129 shadow pass, which is the
  // least diagnostic shader in the table: a cascade covers a slice of the world
  // and the whole scene is drawn against it, so geometry outside its frustum is
  // what that pass is *supposed* to produce. The shaders worth reading by hand
  // are the ones drawing world geometry at the back-buffer's own extent and
  // exporting nothing inside the volume. So dump the busiest few outright, and
  // let whoever reads them pick.
  (void)worst;
  (void)worst_handle;
  auto& f = DeclFile();
  uint32_t dumped = 0;
  for (const auto* r : rows) {
    const ShaderScore& s = r->second;
    if (s.first_exec.empty()) continue;
    if (dumped++ >= 6) break;
    const uint64_t sp = s.space[0] + s.space[1] + s.space[2] + s.space[3];
    const uint64_t scored = s.execs - s.degenerate;
    f << "\nSTAGE I shader 0x" << std::hex << r->first << std::dec << " — "
      << s.execs << " execs, rt " << uint32_t(s.vp_extent >> 32) << "x"
      << uint32_t(s.vp_extent) << ", clip-like "
      << pct(s.space[uint32_t(mx::hle::ExportSpace::kClipLike)], sp)
      << "%, window-like "
      << pct(s.space[uint32_t(mx::hle::ExportSpace::kWindowLike)], sp)
      << "%, neither "
      << pct(s.space[uint32_t(mx::hle::ExportSpace::kNeither)], sp)
      << "%, in-clip " << pct(s.in_clip, scored) << "%\n"
      << s.first_exec;
    if (s.in_seen) {
      f << "  position input range over " << s.in_seen << " executions:\n";
      for (uint32_t c = 0; c < 4; ++c) {
        f << "    ." << "xyzw"[c] << "  [" << s.in_lo[c] << " .. "
          << s.in_hi[c] << "]"
          << (s.in_lo[c] == s.in_hi[c] ? "   CONSTANT" : "") << "\n";
      }
    }
  }
  f.flush();
}

void ReportShaderExecution() {
  ReportShaderExecutionCost();
  if (!g_aluRuns) {
    REXLOG_INFO(
        "d3d9: stageC  shader execution — nothing ran (no located shader {}, "
        "no attrs {}, stride mismatch {})",
        g_aluNoShader, g_aluNoAttrs, g_aluStrideMismatch);
    return;
  }
  const uint64_t scored = g_aluRuns - g_aluDegenerate;
  std::string st;
  for (const auto& [k, n] : g_aluStatus) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d:%llu ", k, (unsigned long long)n);
    st += buf;
  }
  std::string bl;
  for (const auto& [k, n] : g_aluBlocking) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%X:%llu ", k, (unsigned long long)n);
    bl += buf;
  }
  REXLOG_INFO(
      "d3d9: stageC  shader execution — {} vertices run, {} exported a "
      "degenerate position, {} of the remaining {} landed in the clip volume "
      "({}%); status {}; blocking opcodes {}",
      g_aluRuns, g_aluDegenerate, g_aluInClip, scored,
      scored ? (g_aluInClip * 100) / scored : 0, st, bl.empty() ? "none" : bl);
  REXLOG_INFO(
      "d3d9: stageC  constant reads {} of which {} read zero — a shader "
      "computing from an empty file is the failure that still looks like "
      "success",
      g_aluConstReads, g_aluConstZero);
  REXLOG_INFO(
      "d3d9: stageC  skipped: no located shader {}, no attrs {}, bound stride "
      "disagrees with the shader's {}, fetch_slot did not invert to a stream "
      "{} — of {} draws entered",
      g_aluNoShader, g_aluNoAttrs, g_aluStrideMismatch, g_aluBadStream,
      g_aluDrawsEntered);
  ReportClipHistogram();

  // Stage G — where the executed code came from. Now a two-way split: the
  // patch hook, or nothing. The PM4 content-match column is gone with its
  // source, having reported 0 (0%) against the patch hook's 100% over 32212
  // draws before it was removed.
  const uint64_t src_total = g_srcPatchHook + g_srcNone;
  if (src_total) {
    REXLOG_INFO(
        "d3d9: stageG  shader source — patch hook (exact) {} ({}%), none {} "
        "({}%) of {} draws",
        g_srcPatchHook, (g_srcPatchHook * 100) / src_total, g_srcNone,
        (g_srcNone * 100) / src_total, src_total);
    std::string why;
    for (const auto& [w, n] : g_patchDecodeFailWhy) {
      char buf[96];
      std::snprintf(buf, sizeof(buf), "%s:%llu ", w.c_str(),
                    (unsigned long long)n);
      why += buf;
    }
    REXLOG_INFO(
        "d3d9: stageG  patched decode — {} matched the binding table's fetch "
        "count, {} decoded a different count (wrong window start), {} refused "
        "[{}]",
        g_patchDecodeOk, g_patchDecodeCount, g_patchDecodeFail,
        why.empty() ? "none" : why);
    std::string offs;
    for (const auto& [o, n] : g_patchCodeOffsets) {
      char buf[48];
      std::snprintf(buf, sizeof(buf), "%+d:%llu ", o, (unsigned long long)n);
      offs += buf;
    }
    REXLOG_INFO(
        "d3d9: stageG  CF section found at dword offsets from dest — {}(one "
        "value across every shader means a fixed layout; assuming -16 was "
        "wrong and the decode said so)",
        offs.empty() ? "none resolved " : offs);
  }
  ReportPerShader();
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

struct PatchPrediction {
  uint32_t dest_addr = 0;
  uint32_t pred[3] = {};
  bool     bound = false;
};


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

  if (known && decodes_at(known_off)) {
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

  ReportShaderExecution();
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

// Both draw entry points report through here so the two counters are always
// read together. A 150s run reaches 5000-10000 transcoded draws, so a coarser
// cadence than 2500 reports nothing at all — the first output-merger probe was
// lost to exactly that.
void ReportDrawCounts(uint8_t* base) {
  const uint64_t total = g_indexed_draws + g_draws;
  if ((total % kDrawReportEvery) != 0) return;
  REXLOG_INFO("d3d9: draws — DrawIndexedVertices={} DrawVertices={} total={}",
              g_indexed_draws, g_draws, total);
  ReportDeclHistogram();
  if (REXCVAR_GET(hle_capture)) ReportCoverage(base);
}

}  // namespace

void FinalizePendingD3D9Draws(uint8_t* base) {
  FinalizePendingD3D9DrawsImpl(base);
}

void ReportHostPageQueryStats() {
  static uint64_t s_calls = 0, s_queries = 0, s_nanos = 0;
  const uint64_t calls = g_hprCalls - s_calls;
  const uint64_t queries = g_hprQueries - s_queries;
  const uint64_t nanos = g_hprNanos - s_nanos;
  s_calls = g_hprCalls;
  s_queries = g_hprQueries;
  s_nanos = g_hprNanos;
  static uint64_t s_frame = 0;
  ++s_frame;
  static std::chrono::steady_clock::time_point s_last{};
  const auto now = std::chrono::steady_clock::now();
  const bool due = (now - s_last) >= std::chrono::seconds(5);
  if (s_frame <= 8 || due) {
    if (due) s_last = now;
    REXLOG_INFO(
        "d3d9: page checks this frame — {} calls, {} VirtualQuery, {}ms "
        "(total {} queries)",
        calls, queries, nanos / 1000000, g_hprQueries);
  }
  // Bound the staleness: see the note on HostPageReadable.
  InvalidateHostPageCache();
}

//=============================================================================
// Plugin-mode passthrough
//=============================================================================
// This whole layer is the native renderer. When --gpu_plugin=xenos is set the
// plugin owns rendering and none of the work below is wanted — but until
// 2026-08-06 every hook in this file ran in both modes, unlike the other five
// hooks files, which all guard consistently.
//
// It was not free. The per-draw bookkeeping alone (NoteDrawDeclaration,
// ReportDrawCounts, the REXCVAR_GET calls) runs ~1,480 times a frame in a Debug
// build, and plugin-mode MainLoop fell from ~17.6/s on 2026-08-03, before this
// file grew, to ~0.37/s once it had. Every hook here calls its original exactly
// once, so returning straight after it is the complete plugin-mode behaviour.
// d3d9_hooks_passthrough additionally disables this layer in *native* mode. It
// breaks native rendering by design and exists to answer one question: native
// MainLoop spends all its time in a recursive guest scene traversal
// (sub_82B70760 -> sub_82B70578 -> sub_82AFE978 -> sub_82AFCA38 -> sub_82AFA520
// -> sub_82AF93C8, which recurses), and a Release build costs exactly what Debug
// does — so the time is not recompiled PPC compute. That traversal reaches D3D9
// through indirect calls, so either it is blocking inside these hooks or it is
// not. Turning them off separates those two cases in one run.
REXCVAR_DEFINE_BOOL(d3d9_hooks_passthrough, false, "Debug",
                    "Diagnostic: make the D3D9 HLE hooks pass straight through "
                    "in native mode too. Breaks rendering; isolates whether "
                    "native frame time is spent in this layer");

#define MX_D3D9_PLUGIN_PASSTHROUGH(orig)                                 \
  if (mx::native::g_plugin_mode || REXCVAR_GET(d3d9_hooks_passthrough)) { \
    orig(ctx, base);                                                     \
    return;                                                              \
  }

//=============================================================================
// 0x82550B80 — D3DVertexDeclaration* D3DDevice_CreateVertexDeclaration(
//                  const D3DVERTEXELEMENT9* pVertexElements)
//
// One argument, and the declaration object comes back in r3. 18 call sites in
// game code.
//
// The object it returns is laid out by XGSetVertexDeclaration: +0x00 magic
// 0x00100005, +0x04 refcount, +0x18 element count, +0x1C highest stream index,
// +0x20 a 16-byte per-stream usage map, and **+0x34 the copied element array**.
// Recorded here because a later round hooking SetVertexDeclaration will need
// it to get from a declaration pointer back to the elements.
//=============================================================================

REX_IMPORT(__imp__sub_82550B80, orig_CreateVertexDeclaration, void());
extern "C" REX_FUNC(sub_82550B80) {
  MX_D3D9_PLUGIN_PASSTHROUGH(orig_CreateVertexDeclaration);
  const uint32_t elements = ctx.r3.u32;
  const uint64_t n = ++g_decls;

  orig_CreateVertexDeclaration(ctx, base);

  const uint32_t decl = ctx.r3.u32;

  // Record it for the draw-time correlation before anything else. This is the
  // only place the element array can be read safely — the runtime has just
  // walked it — and it is what makes the draw-side lookup a comparison rather
  // than a speculative dereference.
  bool has_colour = false;
  uint32_t n_elems = 0;
  mx::hle::D3D9Element parsed[kMaxElements] = {};
  if (elements) {
    for (uint32_t i = 0; i < kMaxElements; ++i) {
      const uint32_t p = elements + i * kElementSize;
      if (REX_LOAD_U16(p) == 0xFF) break;
      uint8_t raw[kElementSize];
      for (uint32_t b = 0; b < kElementSize; ++b) raw[b] = REX_LOAD_U8(p + b);
      parsed[n_elems] = mx::hle::ReadElement(raw);
      ++n_elems;
      if (raw[9] == mx::hle::kUsageColor) has_colour = true;
    }
  }
  const int decl_id = RecordDeclaration(decl, has_colour, n_elems, parsed);

  // Report a declaration the layout decoder cannot describe immediately and by
  // name. The whole HLE path rests on this decode; a silent miss here would
  // surface much later as geometry that looks almost right.
  if (decl_id >= 0 && !g_declLayoutOk[decl_id]) {
    const auto& e = g_declLayoutErr[decl_id];
    REXLOG_WARN(
        "d3d9: declaration id {} does NOT decode — element {}: {} "
        "(detail 0x{:08X})",
        decl_id, e.failed_element, mx::hle::LayoutErrorText(e.reason), e.detail);
    DeclFile() << "  LAYOUT FAILED element " << e.failed_element << ": "
               << mx::hle::LayoutErrorText(e.reason) << " detail 0x" << std::hex
               << e.detail << std::dec << "\n";
  }

  if (n > kMaxDeclsLogged) return;

  auto& f = DeclFile();
  f << "decl #" << n << " elements=0x" << std::hex << elements << " -> decl=0x"
    << decl << std::dec << "\n";
  if (!elements) {
    f.flush();
    return;
  }

  for (uint32_t i = 0; i < kMaxElements; ++i) {
    const uint32_t p = elements + i * kElementSize;
    const uint16_t stream = REX_LOAD_U16(p);
    if (stream == 0xFF) {
      f << "  [" << i << "] END (" << i << " elements)\n";
      break;
    }
    // Raw, in guest byte order. Decoding is deliberately left to the reader:
    // the field layout past Stream is not established, and a wrong decode here
    // would be indistinguishable from a right one in the dump.
    f << "  [" << i << "] stream=" << stream << " raw=";
    for (uint32_t b = 0; b < kElementSize; ++b) {
      char hex[4];
      std::snprintf(hex, sizeof(hex), "%02X ", REX_LOAD_U8(p + b));
      f << hex;
    }
    f << "\n";
  }

  // The count the runtime itself settled on, as a cross-check against the walk
  // above. If these disagree, the element stride is wrong.
  if (decl) {
    f << "  runtime count=" << REX_LOAD_U32(decl + 0x18)
      << " max_stream=" << REX_LOAD_U32(decl + 0x1C) << "\n";
  }
  f.flush();

  // One line in the log too, so a run with no dump file is distinguishable
  // from a run where the hook was never reached.
  REXLOG_INFO("d3d9: decl #{} written to d3d9_dump_decls.txt", n);
}

//=============================================================================
// 0x82564C50 — void D3D::PatchVertexShaderToMatchVertexDeclaration(
//                  CVertexShader*, ULONG*, const CVertexDeclaration*,
//                  const BYTE*, ULONG)
//
// This is where semantics get bound to shader inputs at runtime — the reason
// they do not survive into the microcode. Only 3 xrefs, all D3D9-internal,
// because it is reached from the lazy-state path at draw time rather than
// called by the game. That is exactly what makes it the right place to read
// the current declaration from.
//
// **Which register holds the declaration is determined by comparison, not by
// reading the mangled signature.** Every argument register is checked against
// declarations we watched CreateVertexDeclaration build; whichever matches is
// the declaration. A signature misread would be invisible in the output,
// whereas a mismatch here is loud.
//=============================================================================

REX_IMPORT(__imp__sub_82564C50, orig_PatchVertexShader, void());
extern "C" REX_FUNC(sub_82564C50) {
  MX_D3D9_PLUGIN_PASSTHROUGH(orig_PatchVertexShader);
  const uint32_t args[5] = {ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32,
                            ctx.r7.u32};
  int found = -1;
  int arg_index = -1;
  for (int a = 0; a < 5; ++a) {
    const int id = KnownDeclId(args[a]);
    if (id >= 0) {
      found = id;
      arg_index = a;
      break;
    }
  }

  static bool s_reported = false;
  if (!s_reported) {
    s_reported = true;
    auto& f = DeclFile();
    if (found >= 0) {
      f << "PatchVertexShader: declaration is argument r" << (3 + arg_index)
        << " (matched declaration id " << found << ")\n";
    } else {
      f << "PatchVertexShader: NO argument register matches a known "
           "declaration — r3..r7 = ";
      for (int a = 0; a < 5; ++a) {
        f << "0x" << std::hex << args[a] << std::dec << " ";
      }
      f << "\n";
    }
    f.flush();
    REXLOG_INFO("d3d9: PatchVertexShader declaration arg = {}",
                found >= 0 ? 3 + arg_index : -1);
  }

  if (found >= 0) g_patchDecl = found;
  ++g_patchCalls;

  // Predict before, compare after. The arguments are only guaranteed good
  // across this call, and the point of the test is what the *original* writes.
  // Sampled: this fires on the lazy-state path, and the rule either holds on
  // every slot or it does not hold at all.
  static std::vector<PatchPrediction> s_pred;
  const bool probe = REXCVAR_GET(hle_capture) && (g_patchCalls % 16) == 0;
  if (probe) {
    PredictPatchedFetches(args[0], args[1], args[2], args[3], args[4], base,
                          s_pred);
  }

  const uint32_t nfetch = ReadPatchFetchCount(args[0], args[4], base);

  orig_PatchVertexShader(ctx, base);

  // Every call, not just the sampled ones: this is the coverage fix, and the
  // destination is in the command ring so there is no second chance at it.
  if (nfetch) CapturePatchedCode(args[0], args[1], args[4], nfetch, base);

  if (probe) CheckPatchedFetches(s_pred, base);
}

//=============================================================================
// 0x825565C8 — void D3DDevice_DrawIndexedVertices(
//                  D3DDevice*, D3DPRIMITIVETYPE, INT BaseVertexIndex,
//                  UINT StartIndex, UINT IndexCount)
//
// Note IndexCount, not PrimitiveCount — the 360 variant differs from the PC
// API here. 19 call sites in game code.
//=============================================================================

REX_IMPORT(__imp__sub_825565C8, orig_DrawIndexedVertices, void());
extern "C" REX_FUNC(sub_825565C8) {
  MX_D3D9_PLUGIN_PASSTHROUGH(orig_DrawIndexedVertices);
  const uint32_t primitive_type = ctx.r4.u32;
  const int32_t base_vertex = ctx.r5.s32;
  const uint32_t start_index = ctx.r6.u32;
  const uint32_t index_count = ctx.r7.u32;
  const uint32_t device = ctx.r3.u32;
  const uint64_t n = ++g_indexed_draws;
  ++mx::hle::D3D9DrawCounter();
  ++mx::hle::D3D9IndexedDrawCounter();
  NoteDrawDeclaration(ctx.r3.u32, base);
  if (n <= kMaxDrawsLogged) {
    // Same rotation problem as the declarations: the first draws happen at
    // load and would not survive a long run's log.
    auto& f = DeclFile();
    f << "DrawIndexedVertices #" << n << " dev=0x" << std::hex << ctx.r3.u32
      << std::dec << " prim=" << ctx.r4.u32 << " base_vertex=" << ctx.r5.s32
      << " start_index=" << ctx.r6.u32 << " index_count=" << ctx.r7.u32 << "\n";
    f.flush();
  }
  if (REXCVAR_GET(hle_capture)) {
    DeviceState().NoteDevice(ctx.r3.u32, mx::hle::kEpDraw);
    SampleFetchConstantFile(ctx.r3.u32, base);
    ScoreDraw(/*indexed=*/true, ctx.r6.u32, ctx.r7.u32, ctx.r3.u32, base);
    DumpHleDraw(/*indexed=*/true, n, ctx.r4.u32, ctx.r5.s32, ctx.r6.u32,
                ctx.r7.u32);
  }
  orig_DrawIndexedVertices(ctx, base);
  // The original draw performs D3D9's lazy vertex-shader patching. Translate
  // after it returns so a shader's first draw can use the exact patched code
  // captured by sub_82564C50 during this call. Save the PPC arguments above:
  // the guest call is free to clobber volatile registers.
  BuildAndQueueDraw(/*indexed=*/true, primitive_type, start_index,
                    index_count, base_vertex, device, base);
  ReportDrawCounts(base);
}

//=============================================================================
// 0x825561B0 — void D3DDevice_DrawVertices(
//                  D3DDevice*, D3DPRIMITIVETYPE, UINT StartVertex,
//                  UINT VertexCount)
//
// 34 call sites in game code.
//=============================================================================

REX_IMPORT(__imp__sub_825561B0, orig_DrawVertices, void());
extern "C" REX_FUNC(sub_825561B0) {
  MX_D3D9_PLUGIN_PASSTHROUGH(orig_DrawVertices);
  const uint32_t primitive_type = ctx.r4.u32;
  const uint32_t start_vertex = ctx.r5.u32;
  const uint32_t vertex_count = ctx.r6.u32;
  const uint32_t device = ctx.r3.u32;
  const uint64_t n = ++g_draws;
  ++mx::hle::D3D9DrawCounter();
  NoteDrawDeclaration(ctx.r3.u32, base);
  if (n <= kMaxDrawsLogged) {
    auto& f = DeclFile();
    f << "DrawVertices #" << n << " dev=0x" << std::hex << ctx.r3.u32
      << std::dec << " prim=" << ctx.r4.u32 << " start_vertex=" << ctx.r5.u32
      << " vertex_count=" << ctx.r6.u32 << "\n";
    f.flush();
  }
  if (REXCVAR_GET(hle_capture)) {
    DeviceState().NoteDevice(ctx.r3.u32, mx::hle::kEpDraw);
    SampleFetchConstantFile(ctx.r3.u32, base);
    ScoreDraw(/*indexed=*/false, ctx.r5.u32, ctx.r6.u32, ctx.r3.u32, base);
    DumpHleDraw(/*indexed=*/false, n, ctx.r4.u32, 0, ctx.r5.u32, ctx.r6.u32);
  }
  orig_DrawVertices(ctx, base);
  // See the indexed path above: the original call makes the current patched
  // vertex shader observable for this same draw.
  BuildAndQueueDraw(/*indexed=*/false, primitive_type, start_vertex,
                    vertex_count, 0, device, base);
  ReportDrawCounts(base);
}

//=============================================================================
// The state entry points.
//
// All pass-through, all recording only, all behind hle_capture except that the
// recording itself is unconditional — a shadow that only starts filling when
// the cvar is read would be missing everything set before the first draw.
//
// **No guest pointer is dereferenced speculatively.** Where a resource object
// is read (SetStreamSource, SetIndices) it is read here, in the same call where
// D3D9 reads the same fields itself, and only the resulting values are kept.
// Reading it later at draw time would be the speculative dereference that
// crashed an earlier round: the game can free a buffer without rebinding, and
// the guest arena is sparse.
//
// Signatures come from the typed decompilation of each function in
// assets/default.xex.probe.i64, not from the PC D3D9 headers — several differ.
//=============================================================================

//-----------------------------------------------------------------------------
// 0x8254B7C0 — D3DDevice_SetStreamSource(D3DDevice*, UINT StreamNumber,
//                  D3DVertexBuffer*, UINT OffsetInBytes, UINT Stride)
//
// D3DVertexBuffer is D3DResource (24 bytes) followed by its two-dword vertex
// fetch constant at +0x18: dword[0] is the base address with flags in the top
// bits, dword[1] the size. SetStreamSource's own first act is to mask that
// dword with 0x1FFFFFFF and write it into the device's fetch constant file, so
// the same mask is applied here.
//-----------------------------------------------------------------------------
REX_IMPORT(__imp__sub_8254B7C0, orig_SetStreamSource, void());
extern "C" REX_FUNC(sub_8254B7C0) {
  MX_D3D9_PLUGIN_PASSTHROUGH(orig_SetStreamSource);
  const uint32_t stream = ctx.r4.u32;
  const uint32_t buffer = ctx.r5.u32;
  const uint32_t offset = ctx.r6.u32;
  const uint32_t stride = ctx.r7.u32;

  if (stream < mx::hle::kMaxStreams) {
    auto& st = DeviceState();
    st.NoteDevice(ctx.r3.u32, mx::hle::kEpSetStreamSource);
    auto& b = st.stream[stream];
    b.seen = true;
    b.buffer_obj = buffer;
    b.offset_bytes = offset;
    b.stride = stride;
    b.bound = buffer != 0;
    if (buffer) {
      // The two dwords are a Xenos vertex fetch constant, decoded exactly as
      // Pm4Translator::CollectVertexFetches already does — dword0 is
      // {type[1:0], address[31:2]} and dword1 is {endian[1:0], size[25:2] in
      // dwords}. That decode is the validated one: it is what produced the
      // stride-28 geometry that currently reaches the screen.
      //
      // A first pass here masked dword0 with 0x1FFFFFFF, copying the mask out
      // of SetStreamSource. That mask is right for what the runtime writes
      // into its fetch constant file, but it leaves the two type bits in the
      // address.
      const uint32_t d0 = REX_LOAD_U32(buffer + 0x18);
      const uint32_t d1 = REX_LOAD_U32(buffer + 0x1C);
      b.fetch_type = d0 & 0x3;
      b.address = d0 & ~0x3u;
      b.endian = d1 & 0x3;
      b.size_bytes = ((d1 >> 2) & 0xFFFFFF) * 4;

      // Stage 0: remember stream 0's raw dwords so the next draw can look for
      // them on the device and locate the fetch constant file.
      if (stream == 0) {
        g_lastBindD0 = d0;
        g_lastBindD1 = d1;
        g_lastBindOffset = offset;
        g_haveBind = true;
      }
    } else {
      b.address = 0;
      b.size_bytes = 0;
      b.endian = 0;
      b.fetch_type = 0;
    }
    g_drawsSinceBind[stream] = 0;
  }

  orig_SetStreamSource(ctx, base);
}

//-----------------------------------------------------------------------------
// 0x8254B8E0 — D3DDevice_SetIndices(D3DDevice*, D3DIndexBuffer*)
//
// One argument; the 360 API has no BaseVertexIndex here. D3DIndexBuffer is
// D3DResource plus Address at +0x18 and Size at +0x1C.
//
// **The index width is bit 31 of Common (+0x00), not a separate field.**
// DrawIndexedVertices branches on `if (*pIndexBuffer < 0)` — a signed test of
// that dword — and multiplies StartIndex by 4 on that side against 2 on the
// other.
//-----------------------------------------------------------------------------
REX_IMPORT(__imp__sub_8254B8E0, orig_SetIndices, void());
extern "C" REX_FUNC(sub_8254B8E0) {
  MX_D3D9_PLUGIN_PASSTHROUGH(orig_SetIndices);
  const uint32_t buffer = ctx.r4.u32;
  auto& st = DeviceState();
  st.NoteDevice(ctx.r3.u32, mx::hle::kEpSetIndices);
  auto& ib = st.index;
  ib.seen = true;
  ib.buffer_obj = buffer;
  ib.bound = buffer != 0;
  if (buffer) {
    ib.common = REX_LOAD_U32(buffer + 0x00);
    // **Not masked with 0x1FFFFFFF.** D3D9 applies that mask itself
    // (`rlwinm r11, r11, 0, 3, 31` in DrawIndexedVertices) because the GPU
    // needs a *physical* address — but every read on this side goes through the
    // guest's *virtual* space, where the buffer lives at the unmasked address.
    // Masking relocated it: an index buffer at 0xF3B64000 was recorded as
    // 0x13B64000, and reading there faulted at 0x1D00B000 in three separate
    // runs before the cause was found.
    //
    // The vertex path never had this bug because it uses `& ~3` — keeping the
    // high bits and clearing only the fetch constant's two type bits.
    //
    // The "index buffer holds its range 66,726/66,726" result did not catch it,
    // and could not: that check compares a count against Size and never
    // dereferences Address, so a relocated address passes it every time.
    ib.address = REX_LOAD_U32(buffer + 0x18);
    ib.size_bytes = REX_LOAD_U32(buffer + 0x1C);
    ib.is_32bit = (ib.common & 0x80000000u) != 0;
  } else {
    ib.common = 0;
    ib.address = 0;
    ib.size_bytes = 0;
    ib.is_32bit = false;
  }

  orig_SetIndices(ctx, base);
}

//-----------------------------------------------------------------------------
// 0x825508A8 / 0x825506E8 — SetVertexShader / SetPixelShader(D3DDevice*, ptr)
//
// Handles only this round. Translating the microcode behind them is the next
// step, and recording the handle is what makes it possible to tell how many
// distinct shaders the population actually uses.
//-----------------------------------------------------------------------------
// Stage 3b — where a bound shader's microcode lives, read out of the accessors
// in gpu.obj that reach it (dis.py prints addi's operands swapped; these are
// corrected):
//
//   Promote(D3DVertexShader*)   = blr           ; the handle IS the CVertexShader
//   SH_pPhysical(this)          = *(this + 0x20)
//   GetUCodeHeader()            = this + 0x368
//   GetUCode(i)                 = this + *(this + (i + 0x70)*8) + 0x368
//   GetPhysicalMicrocode(i)     = *(variant + 0x368) + *(this + 0x20)
//   GetPhysicalMicrocodeSize(i) = *(variant + 0x36C)
//
// So the object carries a table of *patched* microcode variants — which is what
// PatchVertexShaderToMatchVertexDeclaration has been writing all along — and the
// bytes sit at a physical base plus an offset out of the header.
//
// **The physical base is not dereferenced here.** `SH_pPhysical` is exactly the
// kind of address that cost four access violations: D3D9 keeps it masked for the
// GPU, and every read on this side goes through the guest's virtual space. This
// dumps the object's own fields only, so the next step can be decided from what
// they contain rather than from a guess about which space they are in.
uint32_t g_vsDumped = 0;

void DumpVertexShaderObject(uint32_t handle, uint8_t* base) {
  if (!handle || g_vsDumped >= 6) return;
  if (!HostPageReadable(REX_RAW_ADDR(handle)) ||
      !HostPageReadable(REX_RAW_ADDR(handle + 0x380))) return;
  ++g_vsDumped;
  auto& f = DeclFile();
  f << "VERTEX SHADER 0x" << std::hex << handle << ":\n    +0x00..0x40:";
  for (uint32_t o = 0; o < 0x40; o += 4) {
    f << " [" << o << "]=0x" << REX_LOAD_U32(handle + o);
  }
  f << "\n    header +0x360..0x380:";
  for (uint32_t o = 0x360; o < 0x380; o += 4) {
    f << " [" << o << "]=0x" << REX_LOAD_U32(handle + o);
  }
  f << "\n    SH_pPhysical=0x" << REX_LOAD_U32(handle + 0x20)
    << " ucode_offset=0x" << REX_LOAD_U32(handle + 0x368)
    << " ucode_size=0x" << REX_LOAD_U32(handle + 0x36C) << std::dec << "\n";

  // The field's top bits are set (0xFD62A000), which is the *unmasked* form —
  // the same shape the vertex buffer's 0xFD21C003 has before D3D9 masks it to
  // 0x1D21D003 for the fetch constant. So this should be readable as-is, which
  // is exactly the thing four access violations were caused by getting wrong.
  // Page-guarded, and 16 dwords only: if it is microcode the first words will
  // decode as one, and if it is not, that is the finding.
  const uint32_t phys = REX_LOAD_U32(handle + 0x20);
  if (phys && HostPageReadable(REX_RAW_ADDR(phys)) &&
      HostPageReadable(REX_RAW_ADDR(phys + 0x3C))) {
    f << "    ucode @0x" << std::hex << phys << ":";
    for (uint32_t o = 0; o < 0x40; o += 4) f << " " << REX_LOAD_U32(phys + o);
    f << std::dec << "\n";
  } else {
    f << "    ucode @0x" << std::hex << phys << std::dec
      << " NOT READABLE in guest virtual space\n";
  }
  // The physical base reads as sixteen zero dwords — safe, and empty. So the
  // code is not behind that pointer at bind time. CreateVertexShader copies the
  // token stream to `this + 0x368` for *(source + 4) bytes, and +0x36C here is
  // 0x200, so the object very likely carries the microcode inline. Dumped so the
  // next step can compare it against what the ring carried for the same shader,
  // which is a cross-check that actually exists.
  if (HostPageReadable(REX_RAW_ADDR(handle + 0x468))) {
    f << "    inline +0x368:";
    for (uint32_t o = 0x368; o < 0x3E8; o += 4) f << " " << std::hex
                                                  << REX_LOAD_U32(handle + o);
    f << std::dec << "\n";
  }
  f.flush();
}

REX_IMPORT(__imp__sub_825508A8, orig_SetVertexShader, void());
extern "C" REX_FUNC(sub_825508A8) {
  MX_D3D9_PLUGIN_PASSTHROUGH(orig_SetVertexShader);
  auto& st = DeviceState();
  st.NoteDevice(ctx.r3.u32, mx::hle::kEpSetVertexShader);
  st.vertex_shader = ctx.r4.u32;
  st.vs_seen = true;
  if (REXCVAR_GET(hle_capture)) {
    DumpVertexShaderObject(ctx.r4.u32, base);
  }
  orig_SetVertexShader(ctx, base);
}

REX_IMPORT(__imp__sub_825506E8, orig_SetPixelShader, void());
extern "C" REX_FUNC(sub_825506E8) {
  MX_D3D9_PLUGIN_PASSTHROUGH(orig_SetPixelShader);
  auto& st = DeviceState();
  st.NoteDevice(ctx.r3.u32, mx::hle::kEpSetPixelShader);
  st.pixel_shader = ctx.r4.u32;
  st.ps_seen = true;
  CollectPixelShaderBlob(ctx.r4.u32, base);
  orig_SetPixelShader(ctx, base);
}

//-----------------------------------------------------------------------------
// 0x8254C060 / 0x8254C3B0 — SetRenderTarget / SetDepthStencilSurface.
//
// Proven from default.xex.probe.i64 rather than inferred from viewport sizes:
//   device+0x3148 + slot*4 = active colour-surface object
//   device+0x3158          = active depth-surface object
//   surface+0x18           = GPU_SURFACEINFO
//   surface+0x1C           = GPU_COLORINFO / GPU_DEPTHINFO
//   surface+0x24           = packed width/height used by viewport clamping
//-----------------------------------------------------------------------------
mx::hle::RenderTargetBinding SnapshotRenderTarget(uint32_t object,
                                                  uint8_t* base) {
  mx::hle::RenderTargetBinding out;
  out.object = object;
  out.bound = object != 0;
  if (!object || !HostPageReadable(REX_RAW_ADDR(object + 0x18)) ||
      !HostPageReadable(REX_RAW_ADDR(object + 0x24)))
    return out;
  out.surface_info = REX_LOAD_U32(object + 0x18);
  out.color_info = REX_LOAD_U32(object + 0x1C);
  out.extent = REX_LOAD_U32(object + 0x24);
  out.width = (out.extent >> 18) + 1;
  out.height = ((out.extent >> 3) & 0x7FFFu) + 1;
  out.valid = out.width <= 8192 && out.height <= 8192;
  return out;
}

REX_IMPORT(__imp__sub_8254C060, orig_SetRenderTarget, void());
extern "C" REX_FUNC(sub_8254C060) {
  MX_D3D9_PLUGIN_PASSTHROUGH(orig_SetRenderTarget);
  const uint32_t slot = ctx.r4.u32;
  const uint32_t object = ctx.r5.u32;
  if (slot < 4) {
    auto& st = DeviceState();
    st.NoteDevice(ctx.r3.u32, mx::hle::kEpSetRenderTarget);
    st.render_target[slot] = SnapshotRenderTarget(object, base);
    st.render_target_seen_mask |= 1u << slot;

    const auto& rt = st.render_target[slot];
    static std::map<uint64_t, uint64_t> s_targets;
    if (rt.valid) {
      const uint64_t key = (uint64_t(rt.object) << 32) |
                           (uint64_t(rt.width) << 16) | rt.height;
      const bool first = s_targets.emplace(key, 0).second;
      ++s_targets[key];
      if (first) {
        REXLOG_INFO(
            "d3d9: render target slot {} object 0x{:08X} {}x{} "
            "surface=0x{:08X} color=0x{:08X} base=0x{:03X} pitch={}",
            slot, rt.object, rt.width, rt.height, rt.surface_info,
            rt.color_info, rt.color_info & 0xFFFu,
            rt.surface_info & 0x3FFFu);
      }
    }
  }
  orig_SetRenderTarget(ctx, base);
}

REX_IMPORT(__imp__sub_8254C3B0, orig_SetDepthStencil, void());
extern "C" REX_FUNC(sub_8254C3B0) {
  MX_D3D9_PLUGIN_PASSTHROUGH(orig_SetDepthStencil);
  auto& st = DeviceState();
  st.NoteDevice(ctx.r3.u32, mx::hle::kEpSetDepthStencil);
  st.depth_stencil = SnapshotRenderTarget(ctx.r4.u32, base);
  orig_SetDepthStencil(ctx, base);
}

//-----------------------------------------------------------------------------
// 0x8255CE98 — D3DDevice_Resolve.
//
// r4 low three bits select colour target 0..3 or depth target 4; r6 is the
// destination D3DBaseTexture. The internal helper reads that texture's fetch
// descriptor at +0x1C, proving this call — not SetTexture — is the EDRAM to
// system-memory bridge. Record the ordered relationship for host-side
// render-to-texture routing.
//-----------------------------------------------------------------------------
REX_IMPORT(__imp__sub_8255CE98, orig_Resolve, void());
extern "C" REX_FUNC(sub_8255CE98) {
  MX_D3D9_PLUGIN_PASSTHROUGH(orig_Resolve);
  auto& st = DeviceState();
  st.NoteDevice(ctx.r3.u32, mx::hle::kEpResolve);
  const uint32_t source_slot = ctx.r4.u32 & 7u;
  const uint32_t dest_texture = ctx.r6.u32;
  const mx::hle::RenderTargetBinding* source = nullptr;
  if (source_slot < 4)
    source = &st.render_target[source_slot];
  else if (source_slot == 4)
    source = &st.depth_stencil;

  if (dest_texture && source && source->valid) {
    g_resolvedTextureTargets[dest_texture] = source->object;
    static std::map<uint64_t, uint64_t> s_resolves;
    const uint64_t key = (uint64_t(source->object) << 32) | dest_texture;
    const bool first = s_resolves.emplace(key, 0).second;
    ++s_resolves[key];
    if (first) {
      uint32_t fetch0 = 0;
      if (HostPageReadable(REX_RAW_ADDR(dest_texture + 0x1C)))
        fetch0 = REX_LOAD_U32(dest_texture + 0x1C);
      REXLOG_INFO(
          "d3d9: resolve slot {} target 0x{:08X} {}x{} -> texture "
          "0x{:08X} fetch0=0x{:08X}",
          source_slot, source->object, source->width, source->height,
          dest_texture, fetch0);
    }
  }
  // sub_82AFCA38 calls Resolve four times and is where the native frame time
  // goes. Resolve is the EDRAM-to-memory bridge, so it is the call most likely
  // to block on the host side. Timed to confirm or clear it.
  {
    const auto _t0 = std::chrono::steady_clock::now();
    orig_Resolve(ctx, base);
    const auto _ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - _t0)
                         .count();
    if (_ms >= 50) {
      static uint64_t _n = 0;
      REXLOG_INFO("native: Resolve slow #{} {}ms", ++_n, _ms);
    }
  }
}

//-----------------------------------------------------------------------------
// 0x8254E748 — D3DDevice_SetTexture(D3DDevice*, DWORD Sampler,
//                                   D3DBaseTexture*)
//
// IDA shows the texture's six hardware-fetch dwords at object+0x1C..+0x30 and
// SetTexture copying them into device+0x480+sampler*24.
//-----------------------------------------------------------------------------
REX_IMPORT(__imp__sub_8254E748, orig_SetTexture, void());
extern "C" REX_FUNC(sub_8254E748) {
  MX_D3D9_PLUGIN_PASSTHROUGH(orig_SetTexture);
  const uint32_t device = ctx.r3.u32;
  const uint32_t sampler = ctx.r4.u32;
  const uint32_t texture = ctx.r5.u32;
  if (sampler < mx::hle::kMaxSamplers) {
    auto& st = DeviceState();
    st.NoteDevice(ctx.r3.u32, mx::hle::kEpSetTexture);
    auto& binding = st.texture[sampler];
    binding = {};
    binding.object = texture;
    binding.bound = texture != 0;
    // Read while SetTexture guarantees the object is live; never dereference
    // this object later at draw time.
    constexpr uint32_t kTextureFetchObjectOffset = 0x1C;
    if (texture &&
        HostPageReadable(REX_RAW_ADDR(texture + kTextureFetchObjectOffset)) &&
        HostPageReadable(
            REX_RAW_ADDR(texture + kTextureFetchObjectOffset + 20))) {
      for (uint32_t i = 0; i < 6; ++i)
        binding.fetch[i] =
            REX_LOAD_U32(texture + kTextureFetchObjectOffset + i * 4);
      binding.valid = (binding.fetch[0] & 3u) == 2u;
    }
    st.texture_seen_mask |= 1u << sampler;
  }
  orig_SetTexture(ctx, base);

  // Cross-check the object snapshot against D3D9's resolved device fetch file.
  // The object is authoritative when they agree; the post-call device value is
  // a safe fallback for state normalization performed by SetTexture itself.
  if (sampler < mx::hle::kMaxSamplers && texture && device) {
    auto& binding = DeviceState().texture[sampler];
    const uint32_t at = device + 0x480 + sampler * 24;
    if (HostPageReadable(REX_RAW_ADDR(at)) &&
        HostPageReadable(REX_RAW_ADDR(at + 20))) {
      uint32_t resolved[6];
      bool same = binding.valid;
      for (uint32_t i = 0; i < 6; ++i) {
        resolved[i] = REX_LOAD_U32(at + i * 4);
        same = same && resolved[i] == binding.fetch[i];
      }
      static uint64_t s_object_agree = 0, s_device_fallback = 0;
      if (same) {
        ++s_object_agree;
      } else if ((resolved[0] & 3u) == 2u) {
        std::memcpy(binding.fetch, resolved, sizeof(resolved));
        binding.valid = true;
        ++s_device_fallback;
      }
      const uint64_t total = s_object_agree + s_device_fallback;
      if (total && total % 5000 == 0) {
        REXLOG_INFO("d3d9: texture fetch snapshot object agreed {}, device "
                    "resolved fallback {}", s_object_agree,
                    s_device_fallback);
      }
    }
  }
}

//-----------------------------------------------------------------------------
// 0x8254BF50 — D3DDevice_SetViewport(D3DDevice*, const D3DVIEWPORT9*)
//
// Six dwords: X, Y, Width, Height as integers then MinZ, MaxZ as floats. The
// struct is read here because the function reads all six itself on the next
// instruction.
//-----------------------------------------------------------------------------
REX_IMPORT(__imp__sub_8254BF50, orig_SetViewport, void());
extern "C" REX_FUNC(sub_8254BF50) {
  MX_D3D9_PLUGIN_PASSTHROUGH(orig_SetViewport);
  const uint32_t p = ctx.r4.u32;
  if (p) {
    auto& st = DeviceState();
    st.NoteDevice(ctx.r3.u32, mx::hle::kEpSetViewport);
    auto& v = st.viewport;
    v.x = REX_LOAD_U32(p + 0);
    v.y = REX_LOAD_U32(p + 4);
    v.width = REX_LOAD_U32(p + 8);
    v.height = REX_LOAD_U32(p + 12);
    const uint32_t min_bits = REX_LOAD_U32(p + 16);
    const uint32_t max_bits = REX_LOAD_U32(p + 20);
    std::memcpy(&v.min_z, &min_bits, 4);
    std::memcpy(&v.max_z, &max_bits, 4);
    v.seen = true;
    // Every distinct extent, not just the last one. The shadow is
    // last-write-wins, and the first Stage F run read 65535x65535 out of it —
    // which built a nonsense viewport inverse and made the "window-like" test
    // accept almost any position. Whether that is the only viewport this title
    // sets or merely the most recent one is the difference between a wrong
    // read and a wrong *model*, and a single value cannot say which.
    ++g_viewportExtents[(uint64_t(v.width) << 32) | v.height];
  }
  orig_SetViewport(ctx, base);
}

//-----------------------------------------------------------------------------
// 0x8254B678 — D3DDevice_SetScissorRect(D3DDevice*, const RECT*)
//-----------------------------------------------------------------------------
REX_IMPORT(__imp__sub_8254B678, orig_SetScissorRect, void());
extern "C" REX_FUNC(sub_8254B678) {
  MX_D3D9_PLUGIN_PASSTHROUGH(orig_SetScissorRect);
  const uint32_t p = ctx.r4.u32;
  if (p) {
    auto& st = DeviceState();
    st.NoteDevice(ctx.r3.u32, mx::hle::kEpSetScissorRect);
    auto& s = st.scissor;
    s.left = static_cast<int32_t>(REX_LOAD_U32(p + 0));
    s.top = static_cast<int32_t>(REX_LOAD_U32(p + 4));
    s.right = static_cast<int32_t>(REX_LOAD_U32(p + 8));
    s.bottom = static_cast<int32_t>(REX_LOAD_U32(p + 12));
    s.seen = true;
  }
  orig_SetScissorRect(ctx, base);
}

//-----------------------------------------------------------------------------
// The eight D3DDevice_SetRenderState_* leaves.
//
// All (D3DDevice*, DWORD Value) — confirmed on ZEnable's decompilation, and
// they are generated from one template so the rest follow.
//
// Only these eight were matched uniquely. The other ~90 leaves in state.obj are
// 20-56 bytes with no relocations and several are byte-identical to each other,
// so a byte match on them would not be an identification. These eight are the
// output-merger states the renderer needs.
//
// BlendFactor has **zero call sites** in this title. It is hooked anyway so
// that "never called" stays a measured fact.
//-----------------------------------------------------------------------------
#define MX_RENDER_STATE_HOOK(addr_sym, orig_name, state_id)              \
  REX_IMPORT(__imp__##addr_sym, orig_name, void());                      \
  extern "C" REX_FUNC(addr_sym) {                                        \
    auto& st = DeviceState();                                            \
    st.NoteDevice(ctx.r3.u32, mx::hle::kEpSetRenderState);             \
    st.render_state.Set(state_id, ctx.r4.u32);                           \
    orig_name(ctx, base);                                                \
  }

MX_RENDER_STATE_HOOK(sub_82549AD8, orig_RsZEnable, mx::hle::kRsZEnable)
MX_RENDER_STATE_HOOK(sub_82549448, orig_RsAlphaBlendEnable,
                     mx::hle::kRsAlphaBlendEnable)
MX_RENDER_STATE_HOOK(sub_82549568, orig_RsSrcBlend, mx::hle::kRsSrcBlend)
MX_RENDER_STATE_HOOK(sub_825495F8, orig_RsDestBlend, mx::hle::kRsDestBlend)
MX_RENDER_STATE_HOOK(sub_825494D8, orig_RsBlendOp, mx::hle::kRsBlendOp)
MX_RENDER_STATE_HOOK(sub_8254A078, orig_RsColorWriteEnable,
                     mx::hle::kRsColorWriteEnable)
MX_RENDER_STATE_HOOK(sub_825497D8, orig_RsSeparateAlphaBlendEnable,
                     mx::hle::kRsSeparateAlphaBlendEnable)
MX_RENDER_STATE_HOOK(sub_82549900, orig_RsBlendFactor, mx::hle::kRsBlendFactor)

#undef MX_RENDER_STATE_HOOK
