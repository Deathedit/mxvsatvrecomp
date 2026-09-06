// D3D9 entry-point hooks.
//
// The title statically links D3D9 v2.0.20209.3; these functions were nameless in
// the XEX until an XDK d3d9.lib was byte-matched (tools/match_d3d9.py). A Xenos
// vfetch carries format and offset but not semantic; the semantics live in the
// D3DVERTEXELEMENT9 arrays handed to CreateVertexDeclaration at runtime.

#include "hooks/hook_common.h"

#include <rex/cvar.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <limits>
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
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <fstream>

// For the emitter coverage probe only: emitting HLSL the compiler then rejects
// is exactly as useless as refusing to emit, so the probe compiles what it
// emits. Nothing else in this file touches D3D.
#include <d3dcompiler.h>
#include <wrl/client.h>

// For the vfetch destination swizzle: the GPU vertex path must merge attributes
// into registers by the same rule shader_alu.cpp seeds its register file with.
#include <rex/graphics/format/ucode.h>

#include "gpu/guard_census.h"
#include "gpu/health.h"
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
#include "hooks/hooks_d3d9_census.h"
#include "hooks/hooks_d3d9_cmdbuf.h"
#include "hooks/hooks_d3d9_shader_compile.h"
#include "hooks/texture_dump.h"         // --texture_dump=true, logs/texdump

// Defined in src/app/graphics_system.cpp with the rest of the Debug cvars.

// Named, not anonymous, so the guest entry points can live in their own TU and
// still reach this state; hooks_d3d9_internal.h publishes it. Forward-declared
// rather than including hooks_d3d9.h, which needs <string>.
bool GuestRangeReadable(uint8_t* base, uint32_t addr, uint32_t bytes);

namespace mx::hooks::d3d9 {

namespace uc = rex::graphics::ucode;

using mx::hle::DeviceState;

// Declarations are built during load and the rotating log only retains the last
// ~50 seconds of a run, so anything created early has to go somewhere that does
// not rotate. Opened with trunc: one run overwrites the last, no wipe needed.
std::ofstream& DeclFile() {
  static std::ofstream f = [] {
    std::error_code ec;
    std::filesystem::create_directories("logs/decldump", ec);
    return std::ofstream("logs/decldump/decls.txt", std::ios::trunc);
  }();
  return f;
}

// D3DVERTEXELEMENT9 is *12 bytes on Xenon*, not the PC struct's 8: both
// CreateVertexDeclaration (0x82550B80) and XGSetVertexDeclaration (0x82550A90)
// walk it with `lhzu r9, 0xC`. Stream is the halfword at 0, terminating at 0xFF;
// the other ten bytes are dumped raw, unpinned. Constants in gpu/d3d9_layout.h.
using mx::hle::kElementSize;
using mx::hle::kMaxElements;   // refuses to walk a runaway array
// How often the whole census prints. TIME, not draw count: a draw-proportional
// throttle gets noisier exactly when the game gets busier. The old draw constant
// is kept as a FLOOR so a report cannot fire twice for the same state on a
// machine with a coarse clock.

uint64_t g_indexed_draws = 0;
uint64_t g_draws = 0;
uint64_t g_up_draws = 0;
uint64_t g_indexed_up_draws = 0;
uint64_t g_indexed_up_skipped = 0;
uint64_t g_patchCalls = 0;

//---------------------------------------------------------------------------
// Finding the active vertex declaration at draw time.
//
// The draw entry points take D3DDevice* in r3 but not the declaration, so it is
// read off the device. **Nothing here dereferences an unknown pointer** -- the
// arena is not fully mapped; every identification compares against an object we
// watched being created.
//---------------------------------------------------------------------------

// Every declaration seen by CreateVertexDeclaration, with what matters about
// it. Ids are creation order. Declared in hooks_d3d9_internal.h.
DeclTable g_declTable;

int KnownDeclId(uint32_t p) {
  if (!p) return -1;
  for (int i = 0; i < g_declTable.count; ++i) {
    if (g_declTable.ptr[i] == p) return i;
  }
  return -1;
}

uint64_t g_declTableFull = 0;   // declarations that arrived with no slot left
uint64_t g_declRebuilt = 0;     // addresses reused for a different element list

// Identifies WHICH declaration an address holds, not merely that we have seen
// the address: the guest frees declarations and the allocator hands the same
// address back, so a pointer match alone is not identity.
uint64_t DeclSignature(uint32_t elems, const mx::hle::D3D9Element* parsed) {
  uint64_t h = 1469598103934665603ull;   // FNV-1a
  auto mix = [&](uint32_t v) {
    for (int b = 0; b < 4; ++b) {
      h ^= uint8_t(v >> (b * 8));
      h *= 1099511628211ull;
    }
  };
  mix(elems);
  for (uint32_t i = 0; i < elems && i < kMaxElements; ++i) {
    const mx::hle::D3D9Element& e = parsed[i];
    mix(e.stream);
    mix(e.offset);
    mix(e.type);
    mix(uint32_t(e.method) | (uint32_t(e.usage) << 8) |
        (uint32_t(e.usage_index) << 16));
  }
  return h;
}

uint64_t g_declSig[kMaxTrackedDecls] = {};

// Fills one slot from a parsed declaration. Split out because it now runs on
// two paths -- a fresh slot, and a slot whose address the guest reused.
void FillDeclSlot(int id, uint32_t decl, bool has_colour, uint32_t elems,
                  const mx::hle::D3D9Element* parsed) {
  g_declTable.ptr[id] = decl;
  g_declTable.elems[id] = elems;
  g_declTable.hasColour[id] = has_colour;
  g_declSig[id] = DeclSignature(elems, parsed);
  g_declTable.layoutOk[id] = mx::hle::BuildInputLayout(parsed, elems, g_declTable.layout[id],
                                                 g_declTable.layoutErr[id]);
}

// Called from the CreateVertexDeclaration hook, where both pointers are valid.
// Returns the id, or -1 if the table is full.
int RecordDeclaration(uint32_t decl, bool has_colour, uint32_t elems,
                      const mx::hle::D3D9Element* parsed) {
  if (!decl) return -1;

  const int existing = KnownDeclId(decl);
  if (existing >= 0) {
    // Same address. Only the same DECLARATION if the elements match -- if they
    // do not, the guest freed the old one and built a different one here, and
    // returning `existing` would hand every draw the previous element list.
    if (g_declSig[existing] == DeclSignature(elems, parsed)) return existing;
    ++g_declRebuilt;
    if (g_declRebuilt == 1) {
      REXLOG_INFO(
          "d3d9: declaration id {} address 0x{:08X} reused for a DIFFERENT "
          "element list ({} elements) -- rebuilding rather than reusing the "
          "old layout",
          existing, decl, elems);
    }
    FillDeclSlot(existing, decl, has_colour, elems, parsed);
    g_declTable.draws[existing] = 0;   // the draw count belonged to the old one
    return existing;
  }

  if (g_declTable.count >= kMaxTrackedDecls) {
    // Loud, and once. Every draw using this declaration is about to be dropped
    // as kNoLayout, which is indistinguishable at the draw site from a game
    // that simply did not submit them.
    if (++g_declTableFull == 1) {
      REXLOG_WARN(
          "d3d9: vertex declaration table FULL at {} entries -- declaration "
          "0x{:08X} and every later one is untracked, and EVERY DRAW USING "
          "THEM WILL BE DROPPED as kNoLayout",
          kMaxTrackedDecls, decl);
    }
    return -1;
  }

  const int id = g_declTable.count++;
  FillDeclSlot(id, decl, has_colour, elems, parsed);
  return id;
}

// Read out of the library, not searched for: D3DDevice_SetVertexDeclaration is
// 20 bytes and does nothing but `stw r4, 0x2ed8(r3)` plus a lazy-state dirty
// mark, and GetVertexDeclaration reads it back.
//
// Two earlier scans "proved" the declaration was not on the device struct; both
// covered device + 0..0x2000 and 0x2ED8 is outside that. Scope a device scan by
// what the struct actually spans (SetStreamSource writes +0x3480).
constexpr uint32_t kDeviceVertexDeclaration = 0x2ED8;

// Reading device + 0x2ED8 is safe in a way that dereferencing its *value* is
// not: the pointer arrives as the draw's own r3 and the offset is well inside
// the struct. The value read is only ever compared against declarations we
// watched being built -- never followed.
int g_currentDecl = -1;

// What PatchVertexShaderToMatchVertexDeclaration last saw. Kept only to measure
// how far it lags: it fires on the lazy-state path, ~1 update per 66 draws, and
// the previous round mistook attribution-to-a-stale-value for attribution.
int g_patchDecl = -1;

// The census about that table. Declared in hooks_d3d9_census.h.
DeclCensus g_declCensus;

//---------------------------------------------------------------------------
// WHICH pointers are unknown, not just how many: a bare count cannot name the
// draw. Bounded at 16 distinct pointers with the overflow counted.
// XGSetVertexDeclaration is RULED OUT -- its only xrefs are inside
// CreateVertexDeclaration. The CVertexDeclaration offsets below come from
// PatchVertexShaderToMatchVertexDeclaration; kDeclElementsOffset is where the
// array BEGINS, not a pointer to it.
//---------------------------------------------------------------------------
constexpr uint32_t kDeclCountOffset = 0x18;
constexpr uint32_t kDeclElementsOffset = 0x34;

UnknownDeclTable g_unknownDecls;

// Adopt a declaration the device holds that we never watched being created.
// +0x34 IS THE ELEMENT ARRAY, NOT A POINTER TO IT -- taken as a pointer it reads
// null. Read guarded (the arena is not fully mapped) and verified before it is
// trusted -- legal element count, whole array readable, BuildInputLayout must
// accept it -- so any failure drops the draw exactly as before.
int AdoptUnknownDecl(uint32_t p, uint8_t* base) {
  if (!base) return -1;
  const uint32_t elem_at = p + kDeclElementsOffset;
  if (!GuestRangeReadable(base, p, kDeclElementsOffset + kElementSize))
    return -1;
  const uint32_t count = REX_LOAD_U32(p + kDeclCountOffset);
  if (count == 0 || count > kMaxElements) return -1;
  if (!GuestRangeReadable(base, elem_at, count * kElementSize)) return -1;

  mx::hle::D3D9Element parsed[kMaxElements] = {};
  bool has_colour = false;
  for (uint32_t i = 0; i < count; ++i) {
    uint8_t raw[kElementSize];
    const uint32_t at = elem_at + i * kElementSize;
    for (uint32_t b = 0; b < kElementSize; ++b) raw[b] = REX_LOAD_U8(at + b);
    parsed[i] = mx::hle::ReadElement(raw);
    if (raw[9] == mx::hle::kUsageColor) has_colour = true;
  }
  const int id = RecordDeclaration(p, has_colour, count, parsed);
  // Recorded but undecodable is not an adoption: leave it unknown so the draw
  // takes the same exit it always did rather than binding an empty layout.
  if (id < 0 || !g_declTable.layoutOk[id]) return -1;
  return id;
}

void ProbeUnknownDecl(uint32_t p, uint8_t* base) {
  if (!base || !GuestRangeReadable(base, p, 0x40)) {
    REXLOG_WARN("d3d9: decl-unknown 0x{:08X} is NOT READABLE -- the device's "
                "declaration field is holding something that is not a mapped "
                "object",
                p);
    return;
  }
  // INFO, not WARN: an unknown pointer used to mean a dropped draw and now means
  // a declaration about to be adopted. `grep "[warning]"` has to keep finding
  // only things that broke. The NOT READABLE case above stays WARN.
  std::string dwords;
  for (uint32_t i = 0; i < 0x40; i += 4)
    dwords += fmt::format(" +{:02X}={:08X}", i, REX_LOAD_U32(p + i));
  REXLOG_INFO("d3d9: decl-unknown 0x{:08X} count(+0x{:02X})={} |{}", p,
              kDeclCountOffset, REX_LOAD_U32(p + kDeclCountOffset), dwords);
}

void NoteUnknownDecl(uint32_t p, uint8_t* base) {
  for (uint32_t i = 0; i < g_unknownDecls.distinct; ++i) {
    if (g_unknownDecls.ptr[i] == p) {
      ++g_unknownDecls.draws[i];
      return;
    }
  }
  if (g_unknownDecls.distinct >= kMaxUnknownDecls) {
    ++g_unknownDecls.overflow;
    return;
  }
  const uint32_t i = g_unknownDecls.distinct++;
  g_unknownDecls.ptr[i] = p;
  g_unknownDecls.draws[i] = 1;
  // Once per distinct pointer, so a per-frame draw cannot turn this into a
  // per-frame line.
  ProbeUnknownDecl(p, base);
}

// Called from both draw hooks.
void NoteDrawDeclaration(uint32_t device, uint8_t* base) {
  g_currentDecl = -1;
  if (device) {
    const uint32_t p = REX_LOAD_U32(device + kDeviceVertexDeclaration);
    if (!p) {
      ++g_declCensus.deviceNull;
    } else {
      g_currentDecl = KnownDeclId(p);
      if (g_currentDecl < 0) {
        ++g_declCensus.deviceUnknown;
        NoteUnknownDecl(p, base);
        // Read it off the device rather than dropping the draw. On success the
        // pointer is in the table from here on, so g_declCensus.deviceUnknown counts
        // FIRST SIGHTINGS, not draws lost.
        g_currentDecl = AdoptUnknownDecl(p, base);
        if (g_currentDecl >= 0) {
          ++g_declCensus.adopted;
        } else {
          ++g_declCensus.adoptRefused;
        }
      }
    }
  }
  if (g_currentDecl >= 0) {
    if (g_currentDecl == g_patchDecl) {
      ++g_declCensus.agree;
    } else {
      ++g_declCensus.disagree;
    }
  }

  DeviceState().current_decl = g_currentDecl;
  if (g_currentDecl < 0) {
    ++g_declCensus.drawsNoDecl;
    return;
  }
  ++g_declTable.draws[g_currentDecl];
}

//---------------------------------------------------------------------------
// HleDraw coverage. Anything missing from the description is counted under the
// field that was missing, never folded into one "incomplete" total. Nothing here
// reads guest memory: every value was captured by the hook that set it.
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

DrawFitCensus g_drawFit;

// Stride disagreements between the declaration and SetStreamSource. The
// layout's own minimum stride cannot exceed the stride the game bound, or the
// last attribute reads past the end of each vertex.
constexpr int kMaxStrideReports = 8;
int g_strideTooSmallNamed = 0;
int g_vbTooSmallNamed = 0;

// Does the buffer hold the vertices the draw asks for? The fetch constant's size
// and the draw's vertex count come from opposite ends of the API, so agreement
// is evidence both were read correctly.

//---------------------------------------------------------------------------
// Stage 0 -- why does the vertex range check fail? Two candidate causes this
// separates: a second binding path (the state-block variants in blocks.obj never
// reach our hooks; `draws since the last bind` measures it), or streams not
// indexed by a common vertex index, which would make the check wrong rather than
// the game.
//
// The offset could not be read out of SetStreamSource's arithmetic the way
// 0x2ED8 was, so it was located empirically: intersect, across many binds, the
// device offsets holding the dwords we know were just written. Comparison only.
//---------------------------------------------------------------------------

// **Ask the OS whether a page is readable instead of guessing where the struct
// ends.** Two guesses faulted (0x4000, then 0x3484). The arena is sparse; a
// bound picked from a different device is not a bound. The scan stops at the
// first page that is not committed and readable.

// VirtualQuery was ~100% of native frame time, at ~6ms per CALL. It reports the
// whole contiguous run it found in mbi.BaseAddress/RegionSize with identical
// State and Protect, so one query legitimately answers for every address in that
// range. Cleared once per swap: a stale *positive* on a decommitted page is a
// crash, which is why this function exists.
//
// Below: how many draw reports may pass with NOTHING NEW before the three
// row-level dumps print anyway. NOT A BLIND MODULO -- a new UP call site or
// stencil configuration prints immediately at any setting, and a one-line
// summary always prints, so "nothing new" and "not running" stay
// distinguishable. 0 = only print rows on a change. 1 = every report.
REXCVAR_DEFINE_INT32(d3d9_diag_row_heartbeat, 16, "Debug",
                     "Draw reports allowed to pass with an unchanged "
                     "population before UP CALLERS and the per-config stencil "
                     "rows dump anyway (0 = only on change, 1 = every report)");

// Change-or-heartbeat, shared by every row dump in the layer. `population` is
// whatever number grows when something NEW appears; all are add-only, so a
// change in the count cannot hide a replacement. The caller keeps its own `last`
// and `since`; returns true when the rows should print and leaves `since`
// counting held-back reports for the summary line.
//
// Published rather than file-static because the row dumps that use it are split
// across two translation units now. Stateless: all of its state is the two
// references the caller owns.
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

REXCVAR_DEFINE_BOOL(d3d9_page_cache_verify, false, "Debug",
                    "Verify every page-readability cache hit against a fresh "
                    "VirtualQuery and log mismatches. Slow; correctness check "
                    "for the region cache");

// The cache works -- 33,043 calls a frame collapse to 39-79 VirtualQuery -- but
// the cost is per CALL (~3.7ms), so the only thing that helps is missing less
// often. Per-thread, not one shared 8-entry cache: 8 thrashed under the parallel
// record path (three workers plus main), and sharing was unsafe -- no lock, and
// a torn read of {base, size, ok} returns a stale positive for a decommitted
// page. Invalidation bumps a generation counter each thread notices on its next
// call.
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

// Retention. The per-frame clear exists so a decommit underneath the cache is
// picked up within a frame, but it also makes every frame pay first-touch misses
// for every region it uses. Guest allocations cluster at load and the arena is
// not torn down mid-scene, so retention is very probably safe -- "probably" is
// why it is a flag and why the backstop clear still runs.
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
FetchConstScan g_fcScan;
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
PerStreamCensus g_perStream;

// The vertex range check, split by stream. A bare total cannot distinguish
// "stream 0 geometry is wrong" from "small auxiliary streams are modelled
// wrong", and those need completely different fixes.

// Does the device's own fetch constant match what SetStreamSource recorded, and
// where it does not, does the device's value explain a draw the snapshot could
// not? `rescues` is how much of the shortfall reading the device would recover.

// Indexed draws were never range-checked on the vertex side, because the range
// depends on the index values. Reading them is **off**: three runs took an
// access violation at guest 0x1D00B000 that a VirtualQuery guard did not stop.
// Almost certainly the address decode -- SetIndices records
// `REX_LOAD_U32(buffer + 0x18) & 0x1FFFFFFF`, the same mask already found wrong
// for vertex buffers (it clears the top three bits rather than the bottom two).
// Kept behind the flag until the decode is read out of D3DDevice_SetIndices the
// way the vertex side was.
constexpr bool kProbeIndexRange = false;

// Scan the device for dwords matching what was last bound, intersecting into
// the candidate sets. Read-only, bounded, and every read is inside a struct
// D3D9 is itself using on both sides of this hook.
void SampleFetchConstantFile(uint32_t device, uint8_t* base) {
  if (!device || !g_haveBind || g_fcScan.samples >= kFcMaxSamples) return;

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
    if (!g_fcScan.primed) {
      g_fcScan.cand0[i] = hit0;
      g_fcScan.cand1[i] = hit1;
    } else {
      g_fcScan.cand0[i] = g_fcScan.cand0[i] && hit0;
      g_fcScan.cand1[i] = g_fcScan.cand1[i] && hit1;
    }
  }
  // Anything past where this sample could read is not a candidate -- leaving it
  // set would let an offset survive on samples that never actually checked it.
  for (uint32_t i = reached / 4; i < kDeviceScanDwords; ++i) {
    g_fcScan.cand0[i] = false;
    g_fcScan.cand1[i] = false;
  }
  if (reached < g_fcScan.reached) g_fcScan.reached = reached;

  g_fcScan.primed = true;
  ++g_fcScan.samples;

  // The scan pinned dword1 to exactly one offset, 0x77C, which retro-fits
  // SetStreamSource's own arithmetic: `subfic r11, r4, 0x11` gives
  // (0x11 - stream) * 8 + 0x6F4 = 0x77C for stream 0. dword0 had no survivor
  // because D3D9 ORs a flag bit in after masking.
  if (g_fcScan.samples <= 8) {
    auto& f = DeclFile();
    f << "FETCH FILE sample " << g_fcScan.samples << ": last bind d0=0x" << std::hex
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

// The DEVICE's own vertex fetch SIZE for a stream, preferred over the size
// snapshotted at SetStreamSource: Xenia takes the window from this register file
// (d3d12_command_processor.cc:3065) and the two sources disagree on 29.7% of
// stream-0 draws. ONLY the size -- `off1 - 4` is NOT dword0, and the address
// half of this file remains unlocated.
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
  // Type 3 is a vertex fetch; Xenia refuses anything else outright. Here it
  // means the slot is not describing this stream, so the snapshot is the better
  // answer.
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
  // NOT APPLIED, and this is the measurement that says why. 137,087 of 1,118,181
  // draws disagree with the snapshot and the device's size is SMALLER on every
  // one -- larger 0. A smaller window can only drop more regions, never rescue
  // one that failed for being too small, so the register file cannot explain the
  // 12-13% of regions that lose their stream. Kept as a counter so the theory is
  // not re-proposed.
  (void)s;
  ++g_fcCompared[stream];
}

//---------------------------------------------------------------------------
// Stage 3 -- the vertex shader float constant file, read out of
// D3DDevice_SetVertexShaderConstantFN's own arithmetic:
//
//     addi   r10, r4, 0x78          ; StartRegister + 0x78
//     rlwinm r10, r10, 4, 0, 27     ; * 16 -- one vec4 per register
//     add    r10, r10, r3           ; + the device
//
// so register N lives at `device + 0x780 + N * 16`; the pixel twin uses 0x1780.
//
// **Not hooked, deliberately.** The device holds the live value whichever path
// wrote it, including the state-block path in blocks.obj.
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
// The live viewport, off the device. D3DDevice_SetViewport forwards to
// sub_8254BCE8, which stores six floats at +0x3218..+0x322C and **clamps Width
// and Height against the render target** first. That clamp is the point: the
// argument shadow recorded 65535x65535 on 9,130 of ~15,500 calls, and
// last-write-wins meant most draws inherited it.
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
// SQ_PROGRAM_CNTL. Bit 18 (`param_gen`) makes the rasterizer generate PS r0 as
// the screen-space position, with no VS export feeding it -- so the UV export
// path, which reads a vertex export named by the pixel shader's texture profile,
// is only correct while the bit is clear.
//
// D3DDevice_DrawVertices flushes the register with sub_82564768(device, 0, 8576,
// device + 10528); 8576 is 0x2180. It is per-shader-PAIR state and has to be
// read per draw.
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

// True when the GPU applies the viewport scale itself, meaning the vertex shader
// exported clip space. False -- including when the register cannot be read --
// means window space and needs the viewport inverse, which is this game's
// measured case (PA_CL_VTE_CNTL = 0x300) and the safe default.
//
// The VGT block at register 0x2100 is m_ValuesPacket, device+0x28CC, the same
// packet convention kDeviceRegBlock2200 uses for 0x2200.
constexpr uint32_t kDeviceRegBlock2100 = 0x28CC;
constexpr uint32_t kDeviceVgt(uint32_t reg) {
  return kDeviceRegBlock2100 + (reg - 0x2100) * 4;
}
constexpr uint32_t kRegPaSuScModeCntl = 0x2205;
constexpr uint32_t kDevicePaSuScModeCntl =
    kDeviceRegBlock2200 + (kRegPaSuScModeCntl - 0x2200) * 4;

// Register numbers from register_table.inc:1262-1265 and 1304 -- MAX comes
// BEFORE MIN. Field widths from registers.h (24 bits each; multi_prim_ib_ena is
// bit 21). Every field defaults to the inert value in HleDrawInputs, so an
// unreadable device leaves the conditioning off rather than clamping every index.
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
  // The register is 0xFFFF or 0xFFFFFF in every sane state (registers.h:392), so
  // 0 means we read the wrong dword; leave the clamp inert.
  if (in.index_max == 0) {
    in.index_max = 0xFFFFFFu;
    in.index_min = 0;
  }
  if (in.index_min > in.index_max) in.index_min = 0;
  ++g_indexCondRead;
  if (in.index_reset_enabled) ++g_indexCondResetOn;
  // IS TESSELLATION USED AT ALL? From the registers, not from hooks:
  // DrawTessellatedVertices being absent from the XEX is only half the question,
  // and the state.obj tessellation leaves are too small to identify by bytes.
  //
  // VGT_HOS_CNTL 0x2285 (tess_mode bits 0-1), MAX_TESS_LEVEL 0x2286 and MIN
  // 0x2287, both floats; max 1.0 with mode 0 is the reset state. THE 0x2200
  // BLOCK, not the 0x2100 one -- the register file is not a flat array.
  const uint32_t kHosCntl = kDeviceRegBlock2200 + (0x2285u - 0x2200u) * 4;
  if (HostPageReadable(REX_RAW_ADDR(device + kHosCntl + 8))) {
    const uint32_t cntl = REX_LOAD_U32(device + kHosCntl);
    const uint32_t maxr = REX_LOAD_U32(device + kHosCntl + 4);
    const uint32_t minr = REX_LOAD_U32(device + kHosCntl + 8);
    static std::map<uint64_t, uint64_t> s_seen;
    const uint64_t key = (uint64_t(cntl) << 40) ^ (uint64_t(maxr) << 20) ^ minr;
    if (s_seen.size() < 8 && s_seen[key]++ == 0) {
      float fmax, fmin;
      std::memcpy(&fmax, &maxr, 4);
      std::memcpy(&fmin, &minr, 4);
      // PLAUSIBILITY, stated rather than assumed. Only 2 bits of HOS_CNTL are
      // defined and the levels are small positive floats, so anything else means
      // the offset is wrong -- which is what the first cut did, and it read as a
      // confident "tessellation unused".
      const bool sane = (cntl & ~3u) == 0 && std::isfinite(fmax) &&
                        std::isfinite(fmin) && fmax >= 0.0f &&
                        fmax <= 4096.0f && fmin >= 0.0f && fmin <= 4096.0f;
      REXLOG_INFO("d3d9: TESSELLATION regs{}: HOS_CNTL 0x{:08X} (mode {}) "
                  "max {:.3f} min {:.3f}",
                  sane ? "" : " IMPLAUSIBLE -- offset wrong, do not read as "
                              "'tessellation unused'",
                  cntl, cntl & 3u, fmax, fmin);
    }
  }
  // The values themselves, not just that they were read. Clamping to a bound
  // nobody has looked at erased the terrain once: a small max_indx squashes a
  // draw onto one vertex and the frame goes blank with the draw count unchanged.
  // Sampled, capped, distinct values only.
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
  // Offset verification. The derivation says 0x2206 lands here and the captured
  // stream says its value is 0x300; if this slot does not read 0x300 the
  // derivation is wrong. Logged once per distinct value, capped.
  static std::map<uint32_t, bool> s_vals;
  if (REXCVAR_GET(hle_diag) && s_vals.size() < 4 &&
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

// The transform the PM4 path applies today, built from the D3D9 viewport rather
// than the Xenos context registers: window coordinates to clip space, using
// D3D9's own scale/offset (xs = width/2, xo = x + width/2, y flipped). Prefers
// the device's clamped copy, falling back to the argument shadow only when the
// device cannot be read and counting which was used.
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
// Stage 2 -- build a renderable draw from the description. The hook owns guest
// access, so it resolves each buffer to a host pointer and hands plain pointers
// to d3d9_draw.cpp, which stays free of the recompiler macros. Every range is
// bounded by the size D3D9 itself recorded on the object.
//---------------------------------------------------------------------------


// (width << 32) | height -> how many SetViewport calls used it.
std::map<uint64_t, uint64_t> g_viewportExtents;

// D3DDevice_Resolve establishes the explicit EDRAM -> texture relationship.
// Keyed by destination D3D texture object; consumed when that same object is
// subsequently bound through SetTexture. This is deliberately object identity,
// not a guessed match between EDRAM tile base and system-memory address.
std::map<uint32_t, uint32_t> g_resolvedTextureTargets;

// The same relationship keyed by the destination's GUEST MEMORY ADDRESS, so a
// draw sampling a DIFFERENT texture object naming the same memory still finds
// the snapshot. Both sides are the same field, `base_address << 12` out of a
// texture fetch constant; the extent is carried so the match can be refused when
// it disagrees, since guest allocators recycle addresses.
//
// Below: destination texture OBJECT -> the physical address its entry is keyed
// by. The object-identity match runs BEFORE the address match and would
// otherwise escape the coverage rule entirely.
std::map<uint32_t, uint32_t> g_resolveDestObjectPhys;
// Keyed by PHYSICAL address -- see GpuPhysicalAddress.
std::map<uint32_t, ResolvedTargetByAddress> g_resolvedTargetsByAddress;

// The pixel shader each DEVICE last had bound, shared across threads.
//
// DeviceState() is `static thread_local`, so a draw submitted on a worker thread
// sees no shader at all and falls to the tex*col stand-in, which samples the
// material's packed normal/gloss atlas as if it were albedo. A pixel shader
// belongs to the device in D3D9, not to the thread that set it. device+0x3244 is
// consulted first; this is the fallback when it reads zero.
std::mutex g_pixelShaderByDeviceMu;
std::map<uint32_t, uint32_t> g_pixelShaderByDevice;
uint32_t g_lastPixelShaderAnyDevice = 0;

// THE RENDER TARGET, KEYED BY DEVICE. Same shape and reason as the pixel-shader
// map below: DeviceState is thread-local, and a command-buffer replay runs on a
// thread that never called SetRenderTarget, so the replayed draw kept the
// surface it was RECORDED against and the renderer filtered it out.
std::mutex g_rtByDeviceMu;
std::map<uint32_t, mx::hle::RenderTargetBinding> g_colourByDevice;
std::map<uint32_t, mx::hle::RenderTargetBinding> g_depthByDevice;

void NoteRenderTargetForDevice(uint32_t device,
                               const mx::hle::RenderTargetBinding& rt,
                               bool is_depth) {
  if (!device) return;
  std::lock_guard<std::mutex> lock(g_rtByDeviceMu);
  // Only valid bindings are remembered. An invalid snapshot is the absence of
  // an observation, not a state transition to "no target": the guest rebinds
  // constantly and a null here would erase a target the device really holds.
  if (!rt.valid) return;
  (is_depth ? g_depthByDevice : g_colourByDevice)[device] = rt;
}

bool RenderTargetForDevice(uint32_t device, mx::hle::RenderTargetBinding& out,
                           bool is_depth) {
  if (!device) return false;
  std::lock_guard<std::mutex> lock(g_rtByDeviceMu);
  auto& m = is_depth ? g_depthByDevice : g_colourByDevice;
  auto it = m.find(device);
  if (it == m.end() || !it->second.valid) return false;
  out = it->second;
  return true;
}

void NotePixelShaderForDevice(uint32_t device, uint32_t shader) {
  std::lock_guard<std::mutex> lock(g_pixelShaderByDeviceMu);
  // A null shader is a real D3D9 state transition, not a missing observation.
  // Dropping it leaves the previous shader cached forever, so draws made after
  // SetPixelShader(nullptr) inherit a stale program and try to sample its slots.
  if (device) g_pixelShaderByDevice[device] = shader;
  g_lastPixelShaderAnyDevice = shader;
}

// `from_fallback` reports whether the answer came from THIS device's record or
// from the global last-shader-seen-anywhere fallback. Without it a draw whose
// device has no record still gets a plausible handle back, so a mis-attributed
// pixel shader looks exactly like a correct one -- the light-prepass draws
// receive SetVertexShader and never SetPixelShader.
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

// The same lookup, but ONLY for the device asked about. The any-device fallback
// above is a guess across devices, and this title drives three of them from
// three worker threads -- fine for a diagnostic, not for deciding which program
// a third of the frame runs.
uint32_t PixelShaderForDeviceStrict(uint32_t device) {
  if (!device) return 0;
  std::lock_guard<std::mutex> lock(g_pixelShaderByDeviceMu);
  const auto it = g_pixelShaderByDevice.find(device);
  return it != g_pixelShaderByDevice.end() ? it->second : 0;
}

// CHECKED AND MEASURED, do not re-investigate: the vertex shader is NOT
// mis-attributed across threads -- over 2.16M draws a per-device record and the
// thread-local field never disagreed. The mis-paired stages that prompted it are
// real; the cause is the translation cache being keyed on a recycled ADDRESS,
// see g_hlslReportedVs.
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
// Below: luminance readings replaced by the floor. A number that keeps climbing
// means the reduction chain produces nothing and the floor is masking it.
uint64_t g_luminanceFloored = 0;
// Draws whose pixel constant bank held a non-finite value, and how many
// components in total. Reported so the experiment can be read even when the
// picture does not change.

// The same physical page is visible through several virtual windows and the
// guest uses different ones for the same surface, so comparing raw
// `base_address << 12` misses every match. TRANSCRIBED FROM THE GUEST:
// D3DDevice_Resolve tail-calls sub_8255BD48, which computes its destination as
//
//     v55 = base & 0xFFFFF000;
//     v68 = ((v55 >> 20) + 512) & 0x1000;      // conditional +4 KB
//     v70 = v55 & 0x1FFFFFFF;                  // low 512 MB
//     v75 = <dest-point offset> + v68 + v70;
//
// Masking alone is wrong in a way that looks right: for 0xA0000000-window
// addresses the adjustment is zero, so it matches most surfaces and leaves the
// rest one page short.
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

// Whether a resolve destination is worth sampling as a snapshot at all: extent
// agreeing, or the object being named by a resolve, is not the same as the GPU
// having WRITTEN the surface. A quarter of the area is the threshold, since a
// genuine render target is resolved whole or in full-width bands while the
// 2048x2048 menu atlas reaches 256x256.
//
// AREA, NOT A BOUNDING BOX. A box only differs for a SCATTER -- the terrain
// deformation buffer is 3.8% covered with a 29.0% box -- and it only GROWS,
// which is why that defect looked intermittent.
//
// Returning false is not sufficient on its own: for a surface the CPU never
// writes the refusal paints WHITE, so the downstream fallback treats a uniform
// decode as empty whenever a partly-written snapshot exists. Unknown coverage
// allows the claim; refusing on absent evidence would undo the Phase 2 rescue.
bool ResolvedDestinationIsMostlyWritten(uint32_t dest_object) {
  const auto po = g_resolveDestObjectPhys.find(dest_object);
  if (po == g_resolveDestObjectPhys.end()) return true;
  const auto it = g_resolvedTargetsByAddress.find(po->second);
  if (it == g_resolvedTargetsByAddress.end()) return true;
  // REAL coverage, not the bounding box: a scatter of small blits stretches the
  // box across most of the surface while covering almost none of it. See the
  // MarkCoverage note on ResolvedTargetByAddress.
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
// thread_local. Fixed array with atomics because every record worker writes it.
std::atomic<uint32_t> g_drawThreadIds[8];
void NoteDrawThread() {
  const uint32_t tid = GetCurrentThreadId();
  for (auto& slot : g_drawThreadIds) {
    uint32_t cur = slot.load(std::memory_order_relaxed);
    if (cur == tid) return;
    if (cur == 0 && slot.compare_exchange_strong(cur, tid)) return;
  }
}

// The consumption record for a resolve destination, or null when this object
// never resolved anywhere. find() only: this runs on guest draw threads and must
// not insert into a map the resolve path is writing.
ResolvedTargetByAddress* ResolveEntryForObject(uint32_t dest_object) {
  const auto po = g_resolveDestObjectPhys.find(dest_object);
  if (po == g_resolveDestObjectPhys.end()) return nullptr;
  const auto it = g_resolvedTargetsByAddress.find(po->second);
  return it == g_resolvedTargetsByAddress.end() ? nullptr : &it->second;
}

// ---- Video render-target consumption --------------------------------------
// See the block comment in hooks_d3d9_internal.h for what this measures and why
// the RESOLVE CONSUMPTION census cannot answer it.

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
  // resolve, unbind, no draw between. Never report binds without this beside it.
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
// or 0. Matches on the guest memory address the two fetch constants agree on and
// refuses when the extents disagree -- a recycled address describes a different
// texture.
//
// Below: DOES THE GPU EVER WRITE THIS RANGE? A render target lives in EDRAM, so
// a RESOLVE is the only way GPU output reaches guest memory, and "no resolve
// overlaps this range" proves the GPU never wrote these bytes.
// ResolvedTargetForAddress cannot answer it: it needs an exact base match AND
// equal extents AND mostly-written coverage, and all three failures print the
// same `resolved=0`. This overlaps RANGES at any extent and offset, the shape an
// atlas built from sub-rect resolves has:
//
//   exact   - a resolve destination starts precisely at this address
//   inside  - destinations whose base falls WITHIN this range (the atlas pattern)
//   below   - nearest destination base below this address, with the delta
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
    // A resolve destination of the SAME extent starting near this address but
    // not at it. Reported and deliberately NOT claimed: an atlas built by many
    // small resolves into sub-rects needs an offset-aware sample this path
    // cannot express.
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

// Are the PER-DRAW diagnostics on? Some of the measurement in the hot path is
// not cheap -- the Stage-3 transform probe reads 256 guest dwords and scores
// every vertex, per draw. Per-FRAME reporting stays on unconditionally; only
// work proportional to draws or vertices is gated here. Default OFF.
//
// Read once per frame rather than per draw. Declared beside the other cvars at
// the top of the file -- a REXCVAR_DECLARE inside this anonymous namespace looks
// for namespace-local storage and does not link.
bool g_diag = false;
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
// last gate, counted in FinishHleDraw. NOT g_pendingQueued, which counts only
// the DEFERRED path and is legitimately zero on a normal frame -- it looked like
// the queue point because it sits at a push_back.
DrawOutcomeCensus g_drawOutcome;
// WHERE THE REST GO: FRAME DRAWS cannot explain `guest > accepted + refused`, a
// 6.9% gap on a level run. BuildAndQueueDraw has three exits and only the
// BuildHleDraw skip was counted at all -- and that only printed under
// --hle_diag. These two close it, reported unconditionally.
constexpr size_t kMaxPendingHleDraws = 2048;

// Vertex shader object layout, read out of sub_82565928's VS branch at
// 0x82566234 and cross-checked against the patcher at 0x82564C50. The pixel
// shader twin is ps + 0x18 / ps + 0x40 / info + 0x28 / info + 0x2C (see
// CollectPixelShaderBlob).


// Overlay the constants a vertex shader carries as literal data.
//
// device + 0x780 is only one of two publishers. The other is sub_825656A0,
// called from the draw-time flush, which walks a table in the shader object and
// emits a PM4 LOAD_ALU_CONSTANT per entry pointing at literal data inside the
// shader's own code allocation:
//
//   H = vs + 0x368;  P = H + *(H + 0x14)
//   P + 0x10  u32   list byte length;  entries at P + 0x14
//   entry: u16 reg_index, u16 dword_count, u32 data_offset   (8 bytes)
//   terminated by dword_count == 0
//   source = *(vs + 0x20) + data_offset
//
// Every shader publishes one entry covering c252..c255, none of it through the
// device shadow. Applied AFTER the device file so a shader literal wins its own
// slots -- the hardware order.
//
// `written`, when given, marks the bank dwords this overlay published: a shader
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
    ++g_drawOutcome.shaderConstOverlays;
  }
}

// PROBE: which VERTEX constant registers arrive as NaN, and whether any is ever
// finite. IDA cannot answer who writes them -- the setter has 20+ callers and
// none passes a literal StartRegister -- so this settles the narrower question:
//
//   ever_finite == 0  -> the guest never writes the register at all
//   ever_finite  > 0  -> we are sampling a stale window, which is OUR bug
//
// Read AFTER OverlayShaderConstants, which is what could be filling these.
// Keyed by SHADER, and only counted for a shader that can read the register
// (`r <= max_const_index`, a BOUND rather than a read set). `before` is one bit
// per register, set if it held a NaN BEFORE the shader's literal overlay ran --
// "which side made it NaN", not "is the bank NaN".
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
  // Applied BEFORE the shader's own load table so a per-draw literal still wins
  // its slots -- the hardware order.
  mx::gpu::alu::OverlayNonFinite(0, out.data(), kD3d9ConstRegs,
                                 /*count_finite_zeros=*/true);
  // One bit per register: did it hold a NaN before the shader's literal overlay
  // ran? Everything above is per-DEVICE; OverlayShaderConstants is the only
  // per-SHADER step, so this is the split that explains a register being 100%
  // finite for one shader and 0% for another.
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
// Stencil sizing census. MEASUREMENT ONLY -- nothing branches on this.
//
// RB_DEPTHCONTROL.stencil_enable ALONE over-counts: the whole register is gated
// on RB_MODECONTROL.edram_mode, and outside kColorDepth (4) and kDepthOnly (5)
// both depth AND stencil are ignored by the hardware (xenia draw_util.cc:90).
// Both are counted separately and the honest figure is the AND. Draws whose
// register was unreadable are counted too, so the denominator is every draw that
// reached the read.
//
// Register offsets follow this file's block rule: 0x28C0 is register 0x2080,
// 0x28CC is 0x2100, 0x2934 is 0x2200, four bytes per register within a block.
// Field layouts from registers.h:798 and :821.
//---------------------------------------------------------------------------
StencilCensus g_stencil;
// (depth_control, stencilrefmask) -> draws, for stencil-effective draws only.

void NoteStencilCensusUnreadable() {
  std::lock_guard<std::mutex> lk(g_stencil.mu);
  ++g_stencil.drawsSeen;
  ++g_stencil.drawsUnreadable;
}

// DEPTH-SURFACE ALIASING CENSUS. The colour census cannot say whether two DEPTH
// surfaces share an EDRAM base, which matters now that stencil is honoured: the
// menu's mask geometry runs against depth surface 682 (768x640) and the
// fullscreen fill that tests it against 387 (1280x720), and we key depth targets
// by OBJECT. Owners per base is the finding.
//
// THE BASE IS VERIFIED, not extrapolated: the guest's D3D9 surface object at
// +0x1C, and RB_DEPTH_INFO places the base in bits [11:0] exactly as
// RB_COLOR_INFO does. The FORMAT field does differ -- depth_format is one bit at
// +16 against colour's four -- so `(color_info >> 16) & 0xF` is meaningless here.
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

// PHASE 1 CHECK. Counts the same population as NoteStencilCensus but from the
// fields carried on the DrawCall, so it verifies that the values the renderer
// will see are the values the guest programmed. It reads dc only -- reading the
// registers itself would agree by construction.
//
// CALLED FROM THE CONSUMER, not the capture site: at the capture site it could
// only fail if the assignment were broken, while Phase 2 reads these fields
// after the deferred queue, which is where a field gets dropped.
//
// It does NOT prove the register OFFSETS -- both sides trust the same two
// constants.

void NotePlumbedStencilImpl(const mx::hle::DrawCall& dc) {
  std::lock_guard<std::mutex> lk(g_stencil.plumbedMu);
  ++g_stencil.plumbedSeen;
  // Either register unreadable is counted apart rather than folded into the
  // config set: an unreadable refmask would otherwise enter the map as
  // 0xFFFFFFFF and invent a nineteenth configuration out of a failed read.
  if (dc.stencil_ref_mask == 0xFFFFFFFFu || dc.edram_mode == 0xFFFFFFFFu) {
    ++g_stencil.plumbedUnreadable;
    return;
  }
  if (!(dc.depth_control & 1u)) return;
  if (dc.edram_mode != 4u && dc.edram_mode != 5u) return;
  ++g_stencil.plumbedEffective;
  ++g_stencil.plumbedConfigs[{dc.depth_control, dc.stencil_ref_mask}];
}

// WHERE IS RB_STENCILREFMASK_BF (0x210E)? We have never read it, and under
// two-sided stencil the BACK face carries its own ref and masks -- the deferred
// light volumes increment through the back face's stencil FAIL op while we apply
// the FRONT ref (0x210D) to both.
//
// The offset is NOT derivable: this shadow is not a flat register file (0x2200
// sits at 0x2934 and 0x210D at 0x2900). So dump a window and let the data name
// it, restricted to two-sided draws: look for a value that is refmask-SHAPED
// (top byte zero, 0x00rrwwss) and is not a copy of 0x2900's.
constexpr uint32_t kBfWindowBase = 0x2900;
constexpr uint32_t kBfWindowDwords = 8;
// offset -> value -> how many two-sided draws saw it.

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
  std::lock_guard<std::mutex> lk(g_stencil.bfWindowMu);
  ++g_stencil.bfWindowDraws;
  for (uint32_t i = 0; i < kBfWindowDwords; ++i)
    ++g_stencil.bfWindow[kBfWindowBase + i * 4][vals[i]];
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

  std::lock_guard<std::mutex> lk(g_stencil.mu);
  ++g_stencil.drawsSeen;
  ++g_stencil.edramModes[edram_mode];
  if (bit) ++g_stencil.bitSet;
  if (bit && mode_honours) {
    ++g_stencil.effective;
    ++g_stencil.configs[{depth_control, refmask}];
  }
}

// A draw record carrying NEITHER shader -- the entire unexplained remainder of
// the renderer's "no-handle" stand-in population: one signature, 4 indices, ~2
// per frame, colour write on, no YUV planes, not a clear and not a surface bind.
// Logged with the bound texture objects, which the renderer does not have: if
// one is a resolve destination, this is the consumer the menu backdrop is
// missing.
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
  // Compare last-non-null against the two handles on the "Bink composite shaders
  // created" line at startup: a match means these draws inherit a Bink composite
  // shader and IsBinkCompositeDraw refuses them only because SetPixelShader(NULL)
  // cleared the slot it tests.
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

MeshNameCensus g_meshNames;

std::map<std::string, uint64_t>& ReplayMissByShader() {
  static std::map<std::string, uint64_t> m;
  return m;
}

namespace {

// content key -> the .surface part it came from, from tools/surface_manifest.py
const std::unordered_map<uint64_t, std::string>& MeshNames() {
  static const std::unordered_map<uint64_t, std::string> names = [] {
    std::unordered_map<uint64_t, std::string> m;
    std::ifstream f("userdata/surface_names.txt");
    if (!f) {
      REXLOG_INFO("d3d9: userdata/surface_names.txt absent -- meshes will "
                  "report unnamed. Build it with tools/surface_manifest.py");
      return m;
    }
    std::string line;
    while (std::getline(f, line)) {
      const size_t tab = line.find('\t');
      if (tab != 16) continue;
      char* end = nullptr;
      const uint64_t key =
          std::strtoull(line.substr(0, tab).c_str(), &end, 16);
      if (!end || *end) continue;
      std::string name = line.substr(tab + 1);
      while (!name.empty() && (name.back() == '\r' || name.back() == '\n'))
        name.pop_back();
      m.emplace(key, std::move(name));
    }
    REXLOG_INFO("d3d9: mesh names loaded: {} entries", m.size());
    return m;
  }();
  return names;
}

// Byte-wise FNV-1a 64, the same function tools/surface_manifest.py applies to
// the asset bytes. Byte-wise, NOT dword-wise: the shader join hashes microcode
// DWORDS and this one hashes BYTES, and mixing the two reads as a total miss
// with nothing to say why.
uint64_t MeshFnv64(const uint8_t* p, size_t n) {
  uint64_t h = 1469598103934665603ull;
  for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
  return h;
}

constexpr size_t kMeshPrefixBytes = 4096;
// Below this a buffer cannot be a mesh. Counted apart rather than dropped,
// because an excluded population that is never reported is how a coverage
// figure ends up measured against a denominator it could never reach.
//
// This was 256, which was too high and excluded real geometry: UI_Vehicle_Box
// is one part whose index block is 70 bytes, and the corpus has parts with as
// few as five indices. The floor exists only to keep constant-sized buffers out
// of the hash, and 32 does that. Hashing more small buffers costs nothing here
// because the lookup is memoised per (pointer, size).
constexpr uint32_t kMeshMinBytes = 32;

// Look one buffer up, memoised on (pointer, size). Guest buffers CAN be
// rewritten in place at the same address -- the glyph atlas is -- so this memo
// can go stale. It is measurement, and a stale name is visible as a mesh that
// stops changing; a full hash of every bound buffer on every draw is not
// affordable and would change what is being measured.
const std::string* MeshNameFor(const uint8_t* host, uint32_t bytes,
                               bool* by_prefix, bool* first_miss) {
  struct Memo { uint32_t bytes; const std::string* name; bool prefix; };
  static std::unordered_map<const void*, Memo> s_memo;
  *by_prefix = false;
  *first_miss = false;
  auto it = s_memo.find(host);
  if (it != s_memo.end() && it->second.bytes == bytes) {
    ++g_meshNames.hashMemoHit;
    *by_prefix = it->second.prefix;
    return it->second.name;
  }
  ++g_meshNames.hashMemoMiss;
  // First sight of this buffer, so this is the per-buffer population rather
  // than the per-draw one.
  ++g_meshNames.distinctSeen;

  const auto& names = MeshNames();
  const std::string* found = nullptr;
  bool prefix = false;
  // Computed here rather than inside the lookup below, so that the content
  // population is counted for every buffer that reaches this point -- including
  // the ones that go on to match nothing. Counting it only on the matched path
  // would make the ratio 100% by construction.
  const uint64_t full_key = MeshFnv64(host, bytes);
  static std::unordered_set<uint64_t> s_content;
  const bool new_content = s_content.insert(full_key).second;
  if (new_content) ++g_meshNames.contentSeen;
  // The FULL key first: it is the one a whole-block upload produces, and it is
  // the only key the manifest emits for a part under 4096 bytes.
  auto f = names.find(full_key);
  if (f != names.end()) {
    found = &f->second;
  } else if (bytes > kMeshPrefixBytes) {
    auto p = names.find(MeshFnv64(host, kMeshPrefixBytes));
    if (p != names.end()) { found = &p->second; prefix = true; }
  }
  if (found) {
    ++g_meshNames.distinctNamed;
    if (new_content) ++g_meshNames.contentNamed;
  } else {
    if (bytes > g_meshNames.largestUnmatched)
      g_meshNames.largestUnmatched = bytes;
    if (bytes >= 64 * 1024) ++g_meshNames.unmatchedOver64K;
    // Only on the FIRST sighting of this content, so the breakdown below counts
    // meshes and not bindings. A mesh redrawn every frame would otherwise bury
    // every other row.
    *first_miss = new_content;
  }
  s_memo[host] = Memo{bytes, found, prefix};
  *by_prefix = prefix;
  return found;
}

// Every buffer this draw brings, counted whether or not it resolves. Called
// BEFORE BuildHleDraw so a draw that fails to translate is still an
// opportunity -- gating the census on the thing it exists to diagnose is
// exactly what made the Bink probe unreadable.
//
// WHAT THIS DENOMINATOR EXCLUDES, said here because a coverage figure whose
// population is not stated is the failure this project keeps repeating:
// command-buffer replay calls FinishHleDraw directly (hooks_d3d9_cmdbuf.cpp)
// and never passes through BuildAndQueueDraw, so the palm foliage, the rider
// and the bike -- around 123k draws a run -- are NOT counted here at all. They
// are the draws whose geometry most needs naming. Reading `draws` as "every
// draw in the frame" would overstate coverage by exactly that population.
// Which shader drew an unmatched mesh, counted once per distinct mesh.
//
// This is what turns "1291 meshes matched nothing" into an answer. Shaders are
// named by content at 99.9% (userdata/shader_names.txt), so the miss can say
// what it IS: the HFTerrain clipmap and the Scaleform UI have no .surface asset
// and never will, and a miss under those names is correct behaviour rather than
// a join failure. A miss under a prop or vehicle shader is the opposite, and is
// the only kind worth chasing.
std::map<std::string, uint64_t>& MeshMissByShader() {
  static std::map<std::string, uint64_t> m;
  return m;
}

// Returns the name of the first VERTEX stream that resolved, for the DrawCall
// to carry into replay. Index-buffer names are counted but not returned: the
// question a replayed draw has to answer is which mesh it draws.
const std::string* CensusMeshNames(const mx::hle::HleDrawInputs& in,
                                   const TranslatedShader* vs) {
  ++g_meshNames.draws;
  const std::string* vertex_name = nullptr;
  const auto offer = [&](const uint8_t* host, uint32_t bytes, bool is_index,
                         uint32_t offset_bytes, uint32_t stride) {
    ++g_meshNames.buffers;
    if (!host || bytes == 0) { ++g_meshNames.noHost; return; }
    if (bytes < kMeshMinBytes) { ++g_meshNames.unmatchedTiny; return; }
    bool by_prefix = false, first_miss = false;
    const std::string* name = MeshNameFor(host, bytes, &by_prefix, &first_miss);
    if (!name) {
      ++g_meshNames.unmatched;
      if (first_miss) {
        // Three outcomes, kept apart. A runtime-generated shader HAS no asset
        // name and a mesh drawn by one is not expected to match; reporting it
        // as "unnamed" would put it in the same row as a real failure.
        const char* who = !vs                   ? "(no vertex shader)"
                          : vs->runtime_generated ? "(runtime-generated shader)"
                          : vs->name              ? vs->name->c_str()
                                                  : "(shader unnamed)";
        ++MeshMissByShader()[who];
        // AND THE FIRST BYTES, for a miss under a shader that HAS an asset
        // name. Those are the ones that should have matched: a vehicle or prop
        // shader names geometry the assets are supposed to contain, and after
        // three separate key bugs this session the question "are these bytes in
        // any asset" needs settling from the asset side rather than argued from
        // this one. Runtime-generated and unnamed shaders are excluded because
        // their misses are expected and would fill the cap.
        //
        // offset_bytes is printed because the hash ignores it. If the guest
        // packs several parts into one buffer, this draw's data starts there
        // and hashing from the base is the wrong extent -- which is exactly the
        // shape of the last two bugs.
        // PER SHADER, not the first N overall. A flat cap of ten filled up
        // with HFT_Helper and T_EcoLeaves -- a procedural terrain grid and
        // foliage, both of which are SUPPOSED to miss -- and never reached the
        // vehicle shaders, which are the only reason this logging exists. The
        // texture census made this exact mistake: a cap of twelve filled with
        // formats the asset corpus does not contain and never showed FMT_4_5.
        if (vs && !vs->runtime_generated && vs->name && bytes >= 32) {
          static std::map<std::string, uint32_t> s_shown;
          if (s_shown[*vs->name]++ < 2) {
            std::string hex;
            for (size_t i = 0; i < 32 && i < bytes; ++i)
              hex += fmt::format("{:02X}", host[i]);
            REXLOG_INFO("d3d9: MESH UNMATCHED {} -- {} bytes, offset {}, "
                        "stride {}, {} -- first32 {}",
                        *vs->name, bytes, offset_bytes, stride,
                        is_index ? "indices" : "vertices", hex);
          }
        }
      }
      return;
    }
    if (by_prefix) ++g_meshNames.byPrefix;
    // Which SIDE matched is the whole diagnostic: vertices verbatim means the
    // asset can be substituted, indices-only means the guest re-packs vertices
    // on load and step 4 needs a converter.
    if (is_index) {
      ++g_meshNames.namedIndex;
    } else {
      ++g_meshNames.namedVertex;
      if (!vertex_name) vertex_name = name;
    }
    static std::map<std::string, uint64_t> s_seen;
    if (++s_seen[*name] == 1 && s_seen.size() <= 12)
      REXLOG_INFO("d3d9: MESH NAMED {} ({} bytes, {})", *name, bytes,
                  is_index ? "indices" : "vertices");
  };
  if (in.streams) {
    for (uint32_t s = 0; s < mx::hle::kMaxStreams; ++s) {
      if (!in.streams[s].bound) continue;
      offer(in.streams[s].host, in.streams[s].size_bytes, false,
            in.streams[s].offset_bytes, in.streams[s].stride);
    }
  }
  if (in.index.bound) offer(in.index.host, in.index.size_bytes, true, 0, 2);
  if (vertex_name) ++g_meshNames.drawsNamed;
  return vertex_name;
}

}  // namespace

bool FinishHleDraw(mx::hle::DrawCall& dc) {
  mx::hle::HleSkip skip = mx::hle::HleSkip::kNone;
  if (!mx::hle::FinalizeHleTopology(dc, skip)) {
    ++mx::hle::HleSkipCounts()[uint32_t(skip)];
    ++g_drawOutcome.refused;
    return false;
  }
  NoteShaderlessDraw(dc);
  mx::hle::HleFrameDraws().push_back(std::move(dc));
  ++g_drawOutcome.accepted;
  return true;
}

void FinalizePendingD3D9DrawsImpl(uint8_t* base);

// D3DDevice_SetFVF stores the FVF code at device + 12608 and never builds a
// vertex declaration, so a draw using it has no layout and is dropped as
// kNoLayout -- the Bink composite is exactly this case, FVF 0x102 through
// DrawVerticesUP. Only the bits this game was measured to use are decoded;
// anything else returns false, so the draw stays visible in the skip histogram.
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
    // The identity swizzle for this component count, as the guest's own decoder
    // produces it (table in d3d9_layout.h). Zero is NOT the identity: every
    // selector reads component x, so the element comes back splatted.
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

// Vertex data supplied inline by D3DDevice_DrawVerticesUP rather than through a
// bound stream. That call is semantically "bind stream 0 to this pointer with
// this stride and draw", so it is modelled as exactly that below.

void BuildAndQueueDraw(bool indexed, uint32_t prim_type, uint32_t first,
                       uint32_t count, int32_t base_vertex, uint32_t device,
                       uint8_t* base, const UpVertexData* up) {
  using namespace mx::hle;
  const auto& st = DeviceState();

  // DIAG: census SQ_PROGRAM_CNTL per VS/PS pair, read AT DRAW TIME. Third site
  // for this log; the earlier two produced confident WRONG numbers, because at
  // AttachTranslatedPixelShader the vertex handle is not set yet and at
  // ApplyShaderOutputs the register is still being programmed. BuildAndQueueDraw
  // is where the draw is issued, so this is the one to trust.
  //
  // vs_export_count is the interpolator count MINUS ONE (SDK registers.h:144).
  {
    SqProgramCntl pc{};
    if (ReadSqProgramCntl(device, base, &pc)) {
      SqContextMisc cm{};
      const bool have_cm = ReadSqContextMisc(device, base, &cm);
      const uint32_t vs_h = st.vs_seen ? st.vertex_shader : 0;
      // Three answers to "which pixel shader is this draw running", side by side
      // because the disagreement is the finding:
      //   ps_tl     - DeviceState().pixel_shader; thread_local, so empty for
      //               draws off worker threads.
      //   ps_strict - THIS device's record only; what the draw path falls back
      //               to, so this decides what renders.
      //   ps_any    - strict, else the last shader seen on ANY device.
      // ps_strict 0 with ps_any set means the handle shown is borrowed.
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

  // Before any early return. This probe previously sat after BuildHleDraw, so a
  // draw that failed to translate never reached it -- the probe was gated on the
  // very thing it exists to diagnose.
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
  if (id >= 0 && g_declTable.layoutOk[id]) in.layout = &g_declTable.layout[id];

  // A UP draw usually has no declaration -- it uses SetFVF instead -- so fall
  // back to a layout derived from the FVF the device is holding. Only when a
  // declaration did not already supply one, so nothing that works today changes.
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
    // big-endian; 8in32 is the swap every bound stream in this game carries.
    // Stated here so a UP draw rendering as noise has an obvious first suspect.
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
      // Overrides host/size/offset/endian from the device's own fetch constant
      // where that file agrees about the base. See ApplyDeviceFetchConstant: the
      // snapshot is a bind-time copy of the buffer header, the register file is
      // what the GPU actually reads.
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
  // inverse -- the same one the PM4 path uses. Not a claim that it is right; it
  // is the only transform with evidence behind it today, and it makes the HLE
  // picture directly comparable to the PM4 one on screen.
  float vp[16];
  uint32_t viewport_width = 0, viewport_height = 0;
  const bool have_vp = BuildViewportMvp(device, base, vp, &viewport_width,
                                        &viewport_height);
  // PA_CL_VTE_CNTL disables the hardware X/Y scale and offset for these draws,
  // so a shader output without the inverse viewport cannot be submitted under
  // identity. Unknown is not a usable viewport.
  if (!have_vp) {
    bink_lost("no viewport");
    ++g_drawOutcome.noViewport;
    return;
  }
  if (have_vp) in.mvp = vp;

  // Will this draw fetch its own vertices on the GPU? Asked HERE, before the
  // draw is built, because the answer decides whether to spend a per-vertex pass
  // transcoding a 36-byte host vertex the fetch path never reads -- 26-31ms of a
  // menu frame. Only the conditions knowable this early are tested; draws
  // refused later are deferred and transcoded then, at exactly the cost of
  // having done it now. The two big refusals ARE covered: RECTLIST (85% of
  // draws) and a vertex shader with no fetch variant.
  {
    const uint32_t vs = st.vs_seen ? st.vertex_shader : 0;
    const TranslatedShader* vst = vs ? TranslatedVertexShader(vs) : nullptr;
    // Streams this shader indexes by a computed register. The CPU vertex path
    // reads them as if indexed by the vertex, which is an unrelated row rather
    // than an approximation, so it zero-fills them instead.
    if (vst) in.computed_index_streams = vst->computed_index_streams;
    // Any computed fetch index makes the WHOLE draw absolute: the shader does
    // arithmetic on the vertex id, so it must see the guest's own number. Every
    // stream is then addressed from its origin -- mixing a rebased window with
    // an absolute id made all 42 foliage draws render the same billboards.
    if (vst) in.absolute_indices = vst->computed_index_fetches != 0;
    // The DENOMINATOR for the skip counter below. "0 attributes left default"
    // reads both as "never happens" and as "the flag never arrived". This counts
    // every draw that CARRIES a computed-index stream, whichever path it takes.
    if (in.computed_index_streams) ++g_computedIndexDraws;
    in.defer_transcode =
        vst && vst->source && vst->fetch_source && vst->sampler_count == 0 &&
        prim_type != uint32_t(mx::hle::PrimitiveType::kRectangleList) &&
        VportScaleEnabled(device, base);
    if (in.defer_transcode) ++g_transcodeDeferred;
  }

  // Measurement only. Nothing here selects geometry -- the draw is built from
  // the guest's own streams exactly as before.
  const std::string* mesh_name = nullptr;
  {
    const uint32_t vs_h = st.vs_seen ? st.vertex_shader : 0;
    mesh_name =
        CensusMeshNames(in, vs_h ? TranslatedVertexShader(vs_h) : nullptr);
  }

  DrawCall dc;
  dc.mesh_name = mesh_name;
  HleSkip skip = HleSkip::kNone;
  if (!BuildHleDraw(in, dc, skip)) {
    ++HleSkipCounts()[uint32_t(skip)];
    // Which primitive types are being refused, rather than how many. The bare
    // count says 62% of draws fail and nothing about whether that is one type
    // needing expansion or a wrong prim-type argument.
    if (skip == HleSkip::kBadTopology && prim_type < 64) {
      ++g_drawOutcome.badPrimType[prim_type];
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
  // PsParamGen is draw state, not a vertex-shader export: Xenos writes the
  // generated pixel parameters to the register selected by SQ_CONTEXT_MISC,
  // independently of SQ_PROGRAM_CNTL.vs_export_count. The SDK limits it to the
  // sixteen interpolator registers; malformed state is left disabled.
  //
  // MUST PRECEDE the zero-fill census below, which excludes this register by
  // reading dc.pixel_param_gen. This sat after that census until 2026-09-05, so
  // the exclusion read 0 on every draw and could not fire: every row of
  // ZERO-FILLED INTERPOLATORS printed `param_gen -1` even though a third of the
  // title's shader pairs enable it, and the census counted the PsParamGen
  // register itself as invented output -- the exact misreading the exclusion
  // exists to prevent.
  {
    SqProgramCntl pc{};
    SqContextMisc cm{};
    if (ReadSqProgramCntl(device, base, &pc) && pc.param_gen &&
        ReadSqContextMisc(device, base, &cm) && cm.param_gen_pos < 16) {
      dc.pixel_param_gen = cm.param_gen_pos + 1;
    }
  }

  // INTERPOLATOR ZERO-FILL, counted where BOTH stages are known. A slot the
  // pixel shader reads that the vertex shader never exports arrives as a literal
  // float4(0,0,0,0) -- invented output. A slot nobody reads is not, which is why
  // counting this at VS translation read ~80% in both scenes and meant nothing.
  //
  // TWO EXCLUSIONS. PARAM_GEN, which the hardware fills and the VS does not
  // export -- counting it produced the (wrong) "terrain reads an unexported
  // interpolator" theory. And input_mask OVER-REPORTS: it marks any temp read
  // before written in walk order.
  //
  // READ THIS BEFORE ACTING ON THE NUMBER: that bound is loose enough that the
  // figure is NOT a defect count. The worst eight pairs are all benign, seven of
  // them SCRATCH REGISTERS whose tell is the SHAPE -- ps_in a contiguous low
  // run, vs_out a shorter one, missing exactly the difference, never a gap in
  // the middle, where a real linkage mismatch would scatter. Checking the
  // generated HLSL proves nothing: the emitter reads whatever input_mask says.
  //
  // A NEW pair appearing is worth seeing; the absolute level is not.
  {
    const uint32_t vs_h = st.vs_seen ? st.vertex_shader : 0;
    const uint32_t ps_h = st.ps_seen ? st.pixel_shader : 0;
    const TranslatedShader* v = vs_h ? TranslatedVertexShader(vs_h) : nullptr;
    const TranslatedShader* p = ps_h ? TranslatedPixelShader(ps_h) : nullptr;
    // SHADER NAMES, counted here and NOT inside the `if (v && p)` below: the
    // question is what fraction of the frame we can name, so a draw whose
    // shaders never translated is an unnamed draw, not an absent one.
    {
      const bool vn = v && v->name;
      const bool pn = p && p->name;
      ++g_shaderNames.draws;
      const bool vc = v && v->constant_table;
      const bool pc = p && p->constant_table;
      if (vc) ++g_shaderNames.constVs;
      if (pc) ++g_shaderNames.constPs;
      if (vc && pc) ++g_shaderNames.constBoth;
      if (vn && pn) {
        ++g_shaderNames.bothNamed;
      } else if (vn) {
        ++g_shaderNames.vsOnly;
      } else if (pn) {
        ++g_shaderNames.psOnly;
      } else if (!v && !p) {
        ++g_shaderNames.noShader;
      } else if ((!v || v->runtime_generated) &&
                 (!p || p->runtime_generated)) {
        // Every stage that exists is one the guest built itself. Nothing to
        // name, so this is not a gap.
        ++g_shaderNames.generated;
      } else {
        ++g_shaderNames.unknown;
      }
      // Which shaders are the REAL gap, by the key the map is keyed on, so the
      // answer is "dump these and re-run the tool". A runtime-generated shader
      // is deliberately not recorded here: re-running the tool cannot help it,
      // and listing it would send the next reader after work that does not
      // exist.
      if ((v && !v->name && !v->runtime_generated) ||
          (p && !p->name && !p->runtime_generated)) {
        std::lock_guard<std::mutex> lk(g_shaderNames.mu);
        if (v && !v->name && !v->runtime_generated &&
            g_shaderNames.unnamedVs.size() < ShaderNameCensus::kMaxUnnamed)
          ++g_shaderNames.unnamedVs[VertexShaderContentId(vs_h)];
        if (p && !p->name && !p->runtime_generated &&
            g_shaderNames.unnamedPs.size() < ShaderNameCensus::kMaxUnnamed)
          ++g_shaderNames.unnamedPs[PixelShaderContentId(ps_h)];
      }
    }
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
      // NAME THE PAIRS. A percentage is not actionable; a handful of named VS/PS
      // pairs is. Keyed on (vs, ps, missing-slot mask) and weighted by DRAW
      // COUNT -- one pair drawn 40,000 times and forty pairs drawn once are the
      // same rate and completely different problems. Handles are addresses and
      // vary per run, so the row carries the masks too.
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

  // Stage 3's measurement, on the built positions rather than raw bytes, so the
  // probe scores the numbers the renderer would receive. The single most
  // expensive diagnostic in the tree -- 256 guest dword loads plus a scoring
  // pass over every vertex, per draw, to rank candidate matrices nothing
  // selects. Behind hle_diag so it can be re-run rather than deleted.
  if (g_diag) {
    static float consts[kHleProbeRegs * 4];
    if (ReadVsConstants(device, base, consts)) {
      const uint32_t vs_h = st.vs_seen ? st.vertex_shader : 0;
      // What the shader's own asset calls its view-projection register. The
      // probe has been SEARCHING for this; the constant table names it.
      int32_t named_reg = -1;
      if (const TranslatedShader* vts = vs_h ? TranslatedVertexShader(vs_h)
                                             : nullptr) {
        if (vts->constant_table) {
          for (const auto& [reg, name] : vts->constant_table->constants) {
            if (name == "gViewProjection") {
              named_reg = int32_t(reg);
              break;
            }
          }
        }
      }
      ScoreHleTransform(dc, consts, have_vp ? vp : nullptr,
                        VertexShaderContentId(vs_h), vs_h, named_reg);
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
  // thread_local`, and both this and the Resolve hook read st.render_target[]
  // out of it, so a draw recorded on a worker and a resolve issued on the guest
  // thread can disagree about what the target is -- which is exactly what "ever
  // a draw target: NO" on every missing resolve source looks like.
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
    // RB_COLOR_INFO carries an EXPONENT BIAS we have never read. Layout from
    // Xenia's registers.h (the SDK's xenos.h is out of date): color_base:12,
    // _pad:4, color_format:4 at +16, and int32_t color_exp_bias:6 at +20 --
    // SIGNED. We take bits [16:19] for the format and discard the bias, so a
    // target the guest asked to be scaled by 2^bias is rendered at 2^0. Xenia
    // applies it as a multiplier on the pixel shader's colour output, built as
    // 0x3F800000 + (bias << 23). Suggestive rather than proven: log before
    // acting.
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
  // REVERTED: do NOT widen this to the last non-null pixel shader. The theory
  // was sound -- sub_82565928 emits no pixel IM_LOAD for a null shader, so the
  // GPU keeps the previous program and the shaderless 4-index quads DO inherit
  // ps_yuv -- but "inherits the Bink shader" is not "is a Bink composite": this
  // title submits ~96k null-pixel-shader depth draws in a menu run, each
  // following a composite and inheriting it too. Whatever admits those quads has
  // to discriminate on the PLANES.
  if (IsBinkCompositeDraw(bound_ps, base) &&
      PrepareBinkPlanes(dc, device, base)) {
    // Bink intentionally skips PrepareDrawTexture because its composite needs
    // three or four planes rather than the stand-in path's single winning
    // texture. Capture only the c0 modulation the dedicated YUV shader consumes:
    // attaching the whole translated shader here changes pipeline selection, and
    // the 640x216 FE_Smoke quad then runs through the general pixel path against
    // its 1280x720 target and becomes a full white rectangle.
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
  // Carry the two output-merger states this HLE path knows exactly into the same
  // raw fields the PM4 path uses. D3D9 and Xenos both use RGBA bits 0..3 for the
  // colour mask; RB_DEPTHCONTROL bits 1/2 are depth-test/write enable, and
  // ColorWriteEnable=0 identifies the depth-only passes.
  //
  // Read the effective Xenos mask from the DEVICE, not the setter call observed
  // on this worker: the setter shadow is thread-local while a device's render
  // state is not, so a draw from another worker would inherit the default write
  // mask and turn depth-only passes into black overpaint.
  constexpr uint32_t kRbColorMask = 0x28DC;  // RB_COLOR_MASK 0x2104
  if (device && HostPageReadable(REX_RAW_ADDR(device + kRbColorMask))) {
    dc.colour_mask = REX_LOAD_U32(device + kRbColorMask) & 0xFu;
    dc.om_seen |= 1u << 0;
  } else if (st.render_state.Seen(kRsColorWriteEnable)) {
    dc.colour_mask = st.render_state.value[kRsColorWriteEnable] & 0xFu;
    dc.om_seen |= 1u << 0;
  }
  // RB_DEPTHCONTROL, from the device's register shadow -- same block base and
  // reason as RB_COLOR_MASK above. Register 0x2200 is the m_ControlPacket base
  // itself, so it sits at device+0x2934 exactly.
  //
  // This was the LAST output-merger state coming from the thread-local setter
  // shadow, and it carried both of that shadow's defects: a draw from a worker
  // that never called SetRenderState(ZENABLE) got depth silently off, and
  // z_write was FABRICATED from the test bit, so a UI plate drawn depth-testing
  // but not depth-writing occluded everything after it.
  //
  // Stored RAW so a misread shows up as a wrong number rather than a plausible
  // depth mode. Bit layout from registers.h:799: stencil_enable +0, z_enable +1,
  // z_write_enable +2, zfunc +4..6. zfunc is carried but not honoured -- the
  // pipelines hardcode LESS_EQUAL.
  constexpr uint32_t kRbDepthControl = 0x2934;  // RB_DEPTHCONTROL 0x2200
  if (device && HostPageReadable(REX_RAW_ADDR(device + kRbDepthControl))) {
    dc.depth_control = REX_LOAD_U32(device + kRbDepthControl);
    dc.om_seen |= 1u << 1;
    // One line per DISTINCT value, not the first N draws. A cap on occurrences
    // samples whatever runs first and says nothing about the population that
    // matters -- how the alpha-test probe reported "enable 0" for a game that
    // uses it 160 times a frame.
    static std::map<uint32_t, uint64_t> s_depth;
    if (++s_depth[dc.depth_control] == 1 && s_depth.size() <= 32) {
      REXLOG_INFO("d3d9: RB_DEPTHCONTROL 0x{:08X}: z_enable {} z_write {} "
                  "zfunc {} stencil {}",
                  dc.depth_control, (dc.depth_control >> 1) & 1u,
                  (dc.depth_control >> 2) & 1u, (dc.depth_control >> 4) & 7u,
                  dc.depth_control & 1u);
    }
    // Captured HERE, in the same block that reads RB_DEPTHCONTROL, so all three
    // registers describe one draw. NoteStencilCensus reads the same two from the
    // same device a line later, which is what makes the PLUMBED census below a
    // real check rather than a restatement.
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
  // D3DDevice_SetScissorRect writes its TL/BR members directly. Field widths
  // from registers.h:622: 14 bits per edge, bit 31 of TL disables the window
  // offset. D3D9's setter masks with 15 bits, which only matters above 8191.
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
  // device+0x2EF8/0x2EFC. D3DDevice_DrawVertices flushes the 0x2200 block from
  // device+0x2934, putting RB_BLENDCONTROL0 (0x2201) at +0x2938. This is also
  // how xenia-edge decides host blending: Xenos has no separate RB blend-enable
  // bit, so any equation other than ONE/ZERO/ADD is enabled. Using the D3D9-side
  // bit made the menu's SRC_ALPHA/INV_SRC_ALPHA draw opaque and erased the
  // backdrop, because its pixel shader deliberately exports transparent black.
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
      // disassembly is a hypothesis until its values look like the thing they
      // describe. A reference outside [0,1] or a func above 7 means the offsets
      // are wrong, not that the game is unusual.
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
  // all: both PSO paths hardcode D3D12_CULL_MODE_NONE. Register 0x2205, so
  // device+0x2934 + 5*4 = device+0x2948.
  //
  // Why it is suspected: the draw that erases the menu background is a closed
  // 24-vertex BOX covering every pixel sampled, which means the camera is INSIDE
  // it, so every visible face is a back face. Cull back and the console draws
  // nothing; cull NONE, as we do, rasterises the interior. For opaque solids an
  // unculled back face is hidden by the depth test, which is why nothing else
  // obviously broke.
  //
  // Bit layout from registers.h:456: cull_front +0, cull_back +1, face +2 (0 =
  // front is CCW).
  uint32_t pa_su_sc = 0;
  bool pa_su_sc_seen = false;
  {
    constexpr uint32_t kPaSuScModeCntl = 0x2948;  // PA_SU_SC_MODE_CNTL 0x2205
    if (device && HostPageReadable(REX_RAW_ADDR(device + kPaSuScModeCntl))) {
      pa_su_sc = REX_LOAD_U32(device + kPaSuScModeCntl);
      pa_su_sc_seen = true;
      dc.pa_su_sc_mode_cntl = pa_su_sc;
      // GUEST VIEWPORT vs the one we actually set. PA_CL_VTE_CNTL reads 0x43F,
      // so the GPU applies the viewport transform and the guest exports CLIP
      // SPACE -- hence the identity mvp on 99.8% of draws. What is NOT
      // established is that the host viewport we hand D3D12 is the one the guest
      // asked for: the renderer uses the full render-target extent and never
      // consults these registers. The 0x21xx block base is device+0x28CC, so
      // PA_CL_VPORT_XSCALE (0x210F) is at +0x2908.
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
            // Recorded, not classified. Comparing against dc.render_target_width
            // is the wrong reference: the renderer hands D3D12 `drawTarget->
            // width` from its OWN lookup. The comparison belongs where the
            // viewport is actually set, so it is made in RenderGameFrame.
            dc.guest_vp_width = uint32_t(x1 - x0);
            dc.guest_vp_height = uint32_t(y1 - y0);
          }
        }
      }

      // PA_SU_VTX_CNTL is NOT here, and is not read at all. Recorded so the next
      // person does not spend the afternoon this cost.
      //
      // The register is 0x2302. 0x2206, the "next one along" from
      // PA_SU_SC_MODE_CNTL, is PA_CL_VTE_CNTL: decoding it here reads
      // VPORT_X_SCALE_ENA as PIX_CENTER, and 0x0000043F is nonsense as a vertex
      // control (pix_center:1, round_mode:2, quant_mode:3, then 26 bits of
      // PADDING). Extrapolating instead -- 0x2934 + (reg - 0x2200) * 4 -- does
      // not span blocks: it gives +0x2D3C, which reads 0 and decodes as a
      // plausible pixel centre while that range actually holds guest heap
      // pointers, an XEX text address and the ASCII tag "REX".
      //
      // Finding it needs IDA, not a third extrapolation.
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

      // PA_CL_CLIP_CNTL, the register one below, at device+0x2944. Read only to
      // answer whether DepthClipEnable needs to be draw state: every PSO now
      // hardcodes TRUE, which is right exactly while clip_disable (bit 16) is
      // clear. If this ever logs a value with bit 16 set, the flag has to be
      // plumbed through DrawCall and the PSO key like the cull bits were.
      {
        constexpr uint32_t kPaClClipCntl = 0x2944;  // 0x2204
        if (HostPageReadable(REX_RAW_ADDR(device + kPaClClipCntl))) {
          const uint32_t clip = REX_LOAD_U32(device + kPaClClipCntl);
          dc.pa_cl_clip_cntl = clip;
          static std::mutex s_cmu;
          static std::map<uint32_t, uint64_t> s_clips;
          bool cfresh = false;
          {
            std::lock_guard<std::mutex> lk(s_cmu);
            cfresh = ++s_clips[clip] == 1 && s_clips.size() <= 16;
          }
          // Reported every 100,000 draws rather than once per distinct
          // value: "0x00090000 was seen" and "0x00090000 was seen on one draw
          // in a hundred thousand" are different findings, and only the second
          // one explains why no clip-disabled state ever reached the renderer.
          {
            static std::atomic<uint64_t> s_clipDraws{0};
            if ((++s_clipDraws % 100000) == 0) {
              std::string rows;
              std::lock_guard<std::mutex> lk(s_cmu);
              for (const auto& [v, n] : s_clips)
                rows += fmt::format(" [0x{:08X} x{}]", v, n);
              REXLOG_INFO("d3d9: PA_CL_CLIP_CNTL census over {} draws:{}",
                          s_clipDraws.load(), rows);
            }
          }
          if (cfresh) {
            REXLOG_INFO("d3d9: PA_CL_CLIP_CNTL 0x{:08X}: clip_disable {} "
                        "dx_clip_space_def {} ucp_ena 0x{:X} (clip_disable 1 "
                        "means DepthClipEnable TRUE is wrong for this draw)",
                        clip, (clip >> 16) & 1u, (clip >> 19) & 1u,
                        clip & 0x3Fu);
          }
        }
      }
    }
  }

  // The 35-index draw that overpaints the menu background black, replacing an
  // HDR ~11.4 background with (0,0,0,0). One line per DISTINCT state tuple, and
  // placed after the alpha-test read so the whole output-merger verdict is on
  // ONE line -- separate first-N probes are separate chances to sample different
  // draws.
  //
  // Already settled, so the next run does not re-ask:
  //   - the pixel shader is 1 ALU and ALWAYS outputs (0,0,0,0) (`max oC0._000`
  //     + `sgts oC0.x___, -r_abs[0].x`, and -|x| > 0 is unsatisfiable).
  //   - blend is ONE/ZERO/ADD and the colour mask is 0xF, so the black lands.
  //   - the geometry is a closed 24-vertex BOX drawn as one degenerate-joined
  //     triangle strip -- a stencil/light volume, not a colour draw.
  // A shader that deliberately writes zero over a closed volume is gated by
  // something we do not implement, and stencil is the candidate.
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
    // A draw made on a RECORDING device is not rendered now -- on the console
    // it is written into the command buffer and rendered once per instance by
    // the replay. Issuing it here drew the palm foliage twice a RUN instead of
    // once per tree.
    if (!CaptureDrawIfRecording(device, dc)) FinishHleDraw(dc);
    return;
  }
  if (applied != ShaderApplyResult::kNoCode ||
      g_pendingHleDraws.size() >= kMaxPendingHleDraws) {
    if (applied == ShaderApplyResult::kNoCode) {
      ++g_pendingDropped;
      ++g_drawOutcome.shaderNoCodeFull;
    } else {
      // kFailed. Previously returned with nothing incremented at all, which is
      // how a draw disappears between `guest` and `accepted` leaving `refused`
      // at zero -- the exact shape of the 6.9% gap.
      ++g_drawOutcome.shaderFailed;
    }
    return;
  }

  // A recorded draw that defers cannot be captured: the recording has
  // ended by the time the pending queue finalizes. Counted rather than
  // lost quietly, so "captured N" always has its denominator beside it.
  if (CmdBufForDevice(device)) NoteCmdBufDeferredDraw();
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
  ++g_drawFit.checked;
  for (uint32_t s = 0; s < mx::hle::kMaxStreams; ++s) ++g_drawsSinceBind[s];
  bool complete = true;
  auto gap = [&](uint32_t g) { ++g_drawFit.gaps[g]; complete = false; };

  const int id = g_currentDecl;
  if (id < 0) {
    gap(kGapDeclaration);
  } else if (!g_declTable.layoutOk[id]) {
    gap(kGapLayout);
  } else {
    const auto& layout = g_declTable.layout[id];
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
        // attribute would read past the end of each vertex. Either the decode is
        // wrong or the stream was bound for a different declaration. The first
        // few are named, since a bare count cannot say which.
        ++g_drawFit.strideTooSmall;
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
        ++g_drawFit.strideMismatch;   // padding at the end of the vertex; legal
      } else {
        ++g_drawFit.strideOk;
      }

      // For a non-indexed draw the vertex range is known exactly. For an indexed
      // one it depends on the index values -- readable, since the index buffer
      // provably holds its own range, so the real highest index is used rather
      // than skipping the check.
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

      if (indexed && !have_range) ++g_drawFit.idxRangeUnread;

      // **The decisive comparison.** If binds are reaching the device through a
      // path this file does not hook, the device's own fetch constant will
      // differ from the snapshot SetStreamSource recorded -- and the size is the
      // field the range check depends on.
      {
        const uint32_t d1 = REX_LOAD_U32(device + FetchFileDword1Offset(s));
        const uint32_t live = ((d1 >> 2) & 0xFFFFFF) * 4;
        if (live == b.size_bytes) {
          ++g_perStream.fileAgree[s];
        } else {
          ++g_perStream.fileDiffer[s];
          // Does the device's size explain a draw the snapshot could not?
          //
          // `have_range` is REQUIRED. Without it an indexed draw arrives with
          // hi_vertex == 0 and `0 * stride <= live` is true whatever the sizes
          // are -- reporting "explains 254455 of 254455", a clean 100% that
          // measured nothing. A counter whose test cannot fail is not a
          // measurement.
          if (have_range && b.stride &&
              static_cast<uint64_t>(hi_vertex) * b.stride <= live) {
            ++g_perStream.fileRescues[s];
          }
        }
      }

      if (have_range && b.stride) {
        const uint64_t need = static_cast<uint64_t>(hi_vertex) * b.stride;
        const bool fits = need <= b.size_bytes;
        if (indexed) {
          (fits ? g_drawFit.idxRangeFits : g_drawFit.idxRangeFails) += 1;
        }
        // Bind age, split the same way: a stale shadow shows up as failing
        // draws sitting far from their last bind while passing ones sit near it.
        const uint64_t age = g_drawsSinceBind[s];
        if (fits) {
          g_perStream.bindAgeFitSum[s] += age;
        } else {
          g_perStream.bindAgeFailSum[s] += age;
          if (age > g_perStream.bindAgeFailMax[s]) g_perStream.bindAgeFailMax[s] = age;
        }
        (fits ? g_perStream.vbFits[s] : g_perStream.vbFails[s]) += 1;

        if (!indexed && fits) {
          ++g_drawFit.vbFits;
        } else if (!indexed) {
          ++g_drawFit.vbTooSmall;
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
      (need <= st.index.size_bytes ? g_drawFit.ibFits : g_drawFit.ibTooSmall) += 1;
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

  if (complete) ++g_drawFit.complete;
}

//---------------------------------------------------------------------------
// Stage A is GONE. It located the vertex microcode inside the blob by searching
// for what PM4 had decoded from the ring, because DecodeVertexShaderFetches
// needs an array starting at the control-flow section. CapturePatchedCode now
// takes the microcode straight out of the command-ring destination inside the
// PatchVertexShader hook and records `code_off` by decoding it. If a future
// change needs the offset again, use `g_patch.patched`, not a content search.
//---------------------------------------------------------------------------
constexpr uint32_t kMaxBlobDwords = 4096;   // 16 KB ceiling on one blob


//---------------------------------------------------------------------------
// Stage C -- execute the shader and see where the position lands.
//
// **Two honest bridges, both temporary and both stated.**
//
// 1. The attributes come from PM4's decode of the same shader, not from the
//    declaration: the copy at +0x40 is the unpatched template, and pairing
//    declaration elements to vfetch instructions is a rule this has not read out
//    of PatchVertexShaderToMatchVertexDeclaration yet.
// 2. Attribute values are read from stream 0. PM4's fetch_slot is a Xenos fetch
//    constant index (95), not a D3D9 stream number, and that mapping is also
//    unread. Draws whose bound stride disagrees with the shader's are skipped.
//
// Neither affects what the measurement can conclude: if executing the shader
// puts positions in the clip volume, the microcode and the constants are right.
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
// Stage G -- execute the shader that was actually bound. Matching draws to
// microcode by >=90% content similarity against PM4's cache is a heuristic that
// can pick a near-identical wrong variant and fails outright on ~63% of draws;
// the patch hook has the real thing, r4 being where D3D9 writes the patched
// microcode and r3 naming the shader.
//
// **The window's start is searched, not assumed** -- the CF section does not sit
// 0x40 bytes before dest. The search has a verifiable answer: the binding table
// says how many vfetches this shader has, and only the true CF start decodes to
// exactly that many. Resolved once per shader handle and reused.
//---------------------------------------------------------------------------

VsWindowCensus g_vsWindow;


ShaderPatchState g_patch;

uint32_t ReadPatchFetchCount(uint32_t self, uint32_t variant, uint8_t* base);

// The shader's OWN microcode, for shaders the patch hook never saw. That hook
// fires once per upload, not per draw, so any shader uploaded before we were
// watching had no program at all -- 41% of draws in one run, every one dropped
// by ApplyShaderOutputs before the renderer saw it.
//
// The address is the one this file already computed as `field_abs`, verified
// over 28,000 shaders against captures the decode had already proved. The
// variant is not known at draw time, so each is tried and accepted only if its
// program decodes to exactly the fetch count the binding table states.
//
// LIMITATION: the guest patches the RING copy, so the shader's own allocation
// keeps the UNPATCHED template, whose vfetch instructions carry the template's
// constants rather than ones rewritten for the bound declaration.
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


// Stage I -- REMOVED; `ShaderScore` was declared and NEVER INSTANTIATED.
//
// The reasoning is general enough to keep: every count it was meant to replace
// was one percentage over a mixed population with no known target value. Real
// scenes cull, draw shadow maps and run off-screen passes, so 100% in-clip is
// wrong and 36% may be right. **A number with no target value cannot judge a
// change** -- ask where the failure is, not how big it is.

// HLE rendering must consume the shader's position export, not the raw
// declaration POSITION that BuildHleDraw initially packs. Deliberately separate
// from the sampling/probe counters below: rendering executes every vertex, while
// hle_shader_exec may sample only a few.
uint64_t g_hleShaderAttempts = 0, g_hleShaderDraws = 0;
uint64_t g_hleShaderVertices = 0;
uint64_t g_hleShaderNoCode = 0, g_hleShaderBadDecode = 0;
uint64_t g_hleShaderBadStream = 0, g_hleShaderBadConstants = 0;
uint64_t g_hleShaderBadVertex = 0;
// The three reasons that ONE counter was pooling, and they are not the same
// defect:
//
//   short      the index buffer is smaller than index_count says
//   range      an index >= the draw's vertex_count
//   nonfinite  the transformed position is NaN/Inf
//
// `range` is the one that matters: a COMPUTED-INDEX draw addresses its stream
// absolutely and unrebased by design, so its indices are SUPPOSED to exceed the
// local vertex_count. The concrete case is printed once, because a count alone
// cannot show whether the index is a strip-cut marker (0xFFFF), an absolute
// stream offset, or corruption.
uint64_t g_hleShaderBadVertexShort = 0;
uint64_t g_hleShaderBadVertexRange = 0;
uint64_t g_hleShaderBadVertexNonFinite = 0;
uint64_t g_hleShaderBadVertexRangeComputed = 0;  // of those, index == 0xFFFF
uint64_t g_hleShaderIdentityMvp = 0, g_hleShaderViewportMvp = 0;

// How many draws qualify for the GPU vertex path -- the migration's bisector. It
// says what share of the frame can leave the interpreter before anything is
// switched over, so a regression can be attributed to the switch rather than to
// the qualifying rule.
uint64_t g_gpuVertexDraws = 0, g_gpuVertexSkipped = 0;
uint64_t g_gpuVertexUndeclared = 0;
// Why a draw was refused the GPU vertex path. As one number, a refusal we could
// lift is indistinguishable from one we could not. The skinned-mesh question
// needs exactly this split: a vertex shader that samples a texture is refused by
// `sampler_count == 0` and then falls to an interpreter with no texture fetch at
// all, so its result is a silent zero.
uint64_t g_gpuVertexNoVs = 0, g_gpuVertexVsSamplers = 0;
uint64_t g_gpuVertexNoVte = 0, g_gpuVertexNoPs = 0, g_gpuVertexTooManyInputs = 0;
// Of the no-PS draws, the ones now allowed onto the GPU vertex path anyway
// because they cannot write colour. A SUBSET of g_gpuVertexNoPs, not a peer:
// both are incremented for the same draw, so the refused population is
// NoPs - DepthOnly. Silently changing the no-PS count's meaning would invalidate
// every earlier number.
uint64_t g_gpuVertexDepthOnly = 0;
// The GPU vertex FETCH path: draws taking it, and why a draw that qualified for
// the GPU vertex stage still could not.
GpuFetchCensus g_gpuFetch;
ShaderNameCensus g_shaderNames;
uint64_t g_gpuFetchUnaligned = 0;
// Draws whose vertex shader has no fetch variant at all, because the emitter
// refused to translate one. This had NO counter -- the refusal was folded into
// the `gpu_fetch = ... && vs_translated->fetch_source` initialiser -- so of
// 243,162 draws leaving the fetch path the listed reasons accounted for 40,146
// and the other 203,000 were invisible. A refusal with no counter reads as no
// refusal.
uint64_t g_gpuFetchNoVariant = 0;
// Streams whose window ran past the bound size and were shortened. NOT a
// refusal: the draw takes the GPU path, and the shader reads zero past
// RawFetch::limit. This REPLACES g_gpuFetchOutOfRange rather than sitting beside
// it -- that counter could no longer fire, and a counter that cannot fire reads
// as a measurement of zero instead of as dead code.
uint64_t g_gpuFetchClamped = 0;
// The clamp above is a magnitude with no denominator and does not separate its
// two cases: a window shortened by a few vertices loses a tail the shader
// zeroes, while a window shortened to NOTHING loses the whole stream. As one
// counter, xe_vf[0] arriving as (base 0, stride 16, limit 0) on the tree
// billboards produced no log line. Counted on every region considered, so the
// denominator is structural rather than the population of failures.
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
// is spending it. Deliberately coarse -- three buckets and a total; a finer
// breakdown is worth having only once one bucket is known to dominate.
uint64_t g_phaseVertexUs = 0;   // ApplyShaderOutputs, all of it
uint64_t g_phaseInterpUs = 0;   // the software vertex shader inside it
// The texture bucket of this breakdown is g_tex.phaseUs, defined with the
// rest of the texture counters below.
uint64_t g_phaseDrawCount = 0;
uint64_t g_phaseVertexLoopUs = 0;  // the per-vertex loop alone

// WHY a draw is still paying for the per-vertex loop, and what that costs. The
// aggregate said 144,163 vertices cost 119ms while the 145,216 the fetch path
// took away cost only 24ms, so the vertices left on the CPU are five times more
// expensive EACH: the remaining work is concentrated in a subset, and widening
// fetch coverage is only worth doing for whichever subset that is.
enum LoopReason : uint8_t { kLoopRectList = 0, kLoopNoPs = 1, kLoopOther = 2 };
uint64_t g_loopUs[3] = {}, g_loopVerts[3] = {}, g_loopDraws[3] = {};
const char* const kLoopReasonName[3] = {"rectlist", "no-PS", "other"};
uint64_t g_phaseVertexCount = 0;


TextureStats g_tex;

// WHICH textures re-decode, and WHY the cache did not hold them: three decodes,
// 32 MB, every frame, 104ms, against a 99.6% hit rate on the other 1600 binds --
// a property of three specific textures rather than of the path.
//
// Keyed by guest address, which survives across frames where the fetch-constant
// hash does not -- itself a candidate answer, since that key is FNV over all six
// fetch dwords, so one texture bound with two sampler states is two entries and
// two decodes of the same bytes.
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
      // THROTTLED. As every 250th attempt this was 19% of a run by bytes, and
      // `attempt` is a draw counter, so the cadence tracked how BUSY the frame
      // was rather than whether anything changed. It now prints when the SUM of
      // the diagnostic counters changes, plus a 10s heartbeat and the first ten
      // attempts; the sum is a sound change detector because every member is
      // monotonically increasing.
      //
      // ONLY STATIC-OR-FROZEN COUNTERS MAY JOIN THE SUM. A climbing member makes
      // the predicate always true, printing on nearly every draw -- ~250x WORSE
      // than the modulo it replaced. The trap is two adjacent zero-fill counters
      // with near-identical labels:
      //   "CPU zero-filled {} vertices"   g_hleShaderZeroFilledVertex   STATIC
      //   "BUILD zero-filled {} vertices" HleVertexZeroFillCount()      CLIMBS
      //
      // How to check a member before adding one, since reading the body failed
      // twice: pull every occurrence of this line out of a real log, extract the
      // integers in format-string order, and diff consecutive rows.
      //
      // Frozen counters belong here too: VS-samplers frozen at 4 means a fifth
      // appearing is exactly the event worth a line.
      static uint64_t s_lastFailures = ~0ull;
      static std::chrono::steady_clock::time_point s_lastReport{};
      const uint64_t failures =
          g_hleShaderNoCode + g_hleShaderBadDecode + g_hleShaderBadStream +
          g_hleShaderBadConstants + g_hleShaderBadVertex + g_vteSeen[0] +
          g_liveVertexResolved + g_liveVertexNoMatch + g_liveVertexAmbiguous +
          g_liveVertexUnreadable +
          // PRESENCE, NOT MAGNITUDE -- the second time this sum has been broken
          // by one climbing member. g_gpuVertexUndeclared moved on 3922 of the
          // 3943 lines it printed and cost 18% of the log. It is genuine news
          // the FIRST time an undeclared register appears and nothing after. Its
          // real value is still printed below; only the trigger changes.
          (g_gpuVertexUndeclared ? 1u : 0u) + g_gpuVertexNoVs +
          g_gpuVertexVsSamplers + g_gpuVertexTooManyInputs +
          g_gpuFetchNoVariant + g_gpuFetch.ordinalMismatch + g_gpuFetchUnaligned +
          g_hleShaderBadAttribute + g_hleShaderZeroFilledVertex +
          mx::hle::g_rectDegenerate.load();
      const auto now = std::chrono::steady_clock::now();
      // d3d9_diag_row_heartbeat is counted in DRAW REPORTS at its other two
      // sites and this one is per-attempt, so it is not a period here -- only
      // the 0 case carries over, meaning "drift never prints, changes always do".
      // The 10s heartbeat is worth ~11 lines in a two-minute run.
      const bool drift_ok = REXCVAR_GET(d3d9_diag_row_heartbeat) > 0;
      if (attempt > 10 && failures == s_lastFailures &&
          (!drift_ok || now - s_lastReport < std::chrono::seconds(10)))
        return;
      s_lastFailures = failures;
      s_lastReport = now;
      REXLOG_INFO(
          "d3d9: HLE shader output attempts {}: applied {} draws / {} "
          "vertices; skipped no-code {} decode {} stream {} constants {} "
          "vertex {} (short {} range {} of-which-0xFFFF {} nonfinite {}); "
          "output transform identity {} viewport {} (VTE scale-on "
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
          g_hleShaderBadVertexShort, g_hleShaderBadVertexRange,
          g_hleShaderBadVertexRangeComputed, g_hleShaderBadVertexNonFinite,
          g_hleShaderIdentityMvp, g_hleShaderViewportMvp, g_vteSeen[2],
          g_vteSeen[1], g_vteSeen[0], g_hleShaderMvpDisagree,
          g_liveVertexResolved, g_liveVertexNoMatch, g_liveVertexAmbiguous,
          g_liveVertexUnreadable, g_gpuVertexDraws, g_gpuVertexSkipped,
          g_gpuVertexUndeclared, g_gpuVertexNoVs,
          g_gpuVertexVsSamplers, g_gpuVertexNoVte, g_gpuVertexNoPs,
          g_gpuVertexDepthOnly,
          g_gpuVertexTooManyInputs, g_gpuFetch.draws, g_gpuFetchNoVariant,
          g_gpuFetch.rectList,
          g_gpuFetch.ordinalMismatch, g_gpuFetchUnaligned, g_gpuFetchClamped,
          g_hleShaderZeroFilledVertex, g_hleShaderBadAttribute,
          mx::hle::HleVertexZeroFillCount(),
          mx::hle::g_rectArrangement[0].load(),
          mx::hle::g_rectArrangement[1].load(),
          mx::hle::g_rectArrangement[2].load(),
          mx::hle::g_rectDegenerate.load());
    }
  } report{attempt};
  // Who is short, and by how much. Printed on the same cadence and NOT behind
  // hle_diag: the bare BUILD zero-fill total above is 304 million and says
  // nothing actionable. `past-end` is the discriminator -- 1 vertex means a
  // buffer one short, thousands means the index does not address that stream.
  struct ReportZeroFill {
    uint64_t attempt;
    ~ReportZeroFill() {
      // TIME, not attempt count -- the same fix and reason as
      // kDrawReportPeriodMs. `attempt % 250` made this the single largest line
      // in the log, 13% of everything written, and every counter on it is
      // cumulative.
      //
      // A change-detector would be WRONG here, unlike the sibling report above:
      // every counter this line carries climbs monotonically with draws, so a
      // "did anything move" predicate is true on essentially every attempt.
      if (attempt > 10) {
        using namespace std::chrono;
        static std::atomic<int64_t> s_lastMs{
            std::numeric_limits<int64_t>::min() / 2};
        const int64_t now_ms = duration_cast<milliseconds>(
                                   steady_clock::now().time_since_epoch())
                                   .count();
        int64_t last = s_lastMs.load(std::memory_order_relaxed);
        if (now_ms - last < 10000) return;
        if (!s_lastMs.compare_exchange_strong(last, now_ms,
                                              std::memory_order_relaxed))
          return;
      }
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
      // The GPU fetch side of the same question. `dropped` is the one that loses
      // geometry outright: base == limit means every fetch bound to that stream
      // reads zero for every vertex, so the draw renders but its vertices do not
      // exist. The first case is printed whole because the ratio alone cannot
      // say which of stride/size/offset/first_vertex is wrong.
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
      // The fetch-constant override, reported unconditionally because it decides
      // which bytes every draw reads. `unmatched` is the one to watch: neither
      // base composition fitted, so that stream silently kept the old snapshot
      // window.
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
          "d3d9: index conditioning: registers read {} draws [{}], restart "
          "enabled {}, cut {} draws at {} markers{}{}{}",
          g_indexCondRead,
          // NOT a zero check -- the opposite. A zero here means the register
          // offsets are wrong and every index goes through unconditioned,
          // which is the state that lost the ground.
          mx::gpu::health::Tag(mx::gpu::health::NonZero(
              "index_cond.registers_read", g_indexCondRead, g_draws)),
          g_indexCondResetOn,
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
  // validated live allocation fallback above. The >=90% PM4 content match
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
  // Emitting the fetch into HLSL means addressing the vertex buffer with
  // SV_VertexID, which is only correct if every fetch really does index by the
  // vertex ID -- the CPU path has always assumed that without checking -- and
  // exp_adjust is decoded and applied nowhere, so a non-zero one is a silently
  // dropped power-of-two scale.
  //
  // exp_adjust unconditionally and once, because the census below is behind
  // hle_diag and the "always 0 in this game" claim written into
  // shader_hlsl.cpp's refusal has only been checked on runs nobody makes.
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
  // NOT behind g_diag. One line per DISTINCT (register, swizzle, rounded,
  // exp_adjust) combination, so the whole cost is a handful of lines per run,
  // and it answers the question the emitter's own comment says has never been
  // checked: whether every vfetch really is indexed by the vertex ID. A draw of
  // 148 vertices was measured whose stream 1 holds FOUR (stride 16, size 64), a
  // corner table that cannot be addressed by vertex ID at all.
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

  uint8_t vtx[kMaxStreams][256];
  std::vector<std::array<float, 4>> values(attrs.size());

  // ---- The GPU vertex path ------------------------------------------------
  //
  // Qualifying is deliberately narrow, and every condition is a thing that would
  // otherwise be guessed at:
  //
  //  - BOTH stages must have translated. A GPU vertex stage under the stand-in
  //    pixel shader would still owe it the reconstructed param_gen UV and the
  //    selected interpolator; taking the stages together means the rasterizer
  //    does that natively.
  //  - Every attribute's destination must be a register the shader declares.
  //    Dropping an undeclared one silently would feed the shader a zero it never
  //    saw on the console.
  //  - The vertex shader must read no textures. The translated root signature
  //    gives its SRV and sampler tables PIXEL visibility only, so a vertex fetch
  //    would fail pipeline creation.
  //  - PA_CL_VTE_CNTL must say the hardware applies the viewport transform, so
  //    the position export is clip space and dc.mvp is identity; the translated
  //    vertex stage does not apply mvp at all.
  //
  // The layout is one element per REGISTER, not per attribute: 5.4% of draws
  // have two vfetches sharing a register with complementary destination
  // swizzles, and one element each would clobber rather than merge.
  const TranslatedShader* vs_translated = TranslatedVertexShader(handle);
  // Evaluated as separate tests rather than one `&&` chain so each refusal is
  // attributed. The chain short-circuits, so read the counters as "the reason
  // that fired", not as a partition of independent causes.
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
    // descriptor sits at that index. This counter therefore means "has a sampler
    // we could not fill", not "has a sampler".
    gpu_vertex = false;
    ++g_gpuVertexVsSamplers;
  } else if (!VportScaleEnabled(device, base)) {
    gpu_vertex = false;
    ++g_gpuVertexNoVte;
  } else if (dc.pixel_shader_hlsl == nullptr) {
    // A null pixel shader used to end the GPU vertex path outright, which cost
    // ~45 draws and ~21,000 vertices a frame to the software interpreter -- the
    // most expensive vertices left on the CPU by a wide margin.
    //
    // What they ARE is the guest's DEPTH passes: SetPixelShader(NULL) is legal
    // for a pass that writes only depth, and the guest emits one 48-dword
    // program that writes position and exports no interpolators. Over 70,000 of
    // them, EVERY one that binds a colour target has RB_COLOR_MASK 0.
    //
    // Gated on that rather than assumed: `paints_colour` is the renderer's own
    // `colorWrite` rule, spelled identically so the two cannot drift.
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
  // Strictly an accelerated form of gpu_vertex: everything that refuses that
  // refuses this, and anything this refuses falls back to it. A defect here
  // costs frame time, not pixels.
  bool gpu_fetch = gpu_vertex && vs_translated;
  if (gpu_fetch && !vs_translated->fetch_source) {
    gpu_fetch = false;
    ++g_gpuFetchNoVariant;
  }
  if (gpu_fetch && dc.prim_type == uint32_t(mx::hle::PrimitiveType::kRectangleList)) {
    // ExpandRectangleList synthesizes a fourth vertex as v1 + v2 - v0 from the
    // host vertices AND from vertex_inputs. The fetch path produces neither, and
    // raw guest bytes cannot be combined affinely without first decoding them --
    // which is the work being removed. Every full-screen post pass is a RECTLIST
    // but they are 3-6 vertices each, so this costs nothing.
    gpu_fetch = false;
    ++g_gpuFetch.rectList;
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
  // SPEEDTREE PATH census -- which vegetation shader each draw runs, by the
  // CONSTANTS it reads.
  //
  // THE SAME REGISTER MEANS DIFFERENT THINGS IN DIFFERENT SHADERS, from the
  // shader assets themselves (tools/shader_code.py):
  //
  //   T_EcoLeaves (3D)   c69 g_TreeLerps  c70 g_TreeFade  c82 gTreeLODParams3
  //   TreeShader   (BB)  c69 g_BBWorldX   c70 g_BBWorldY  c71 g_BBWorldZ
  //                      c72 g_AngleDot   c80 g_BBTreeTypes x60  -> c80..c139
  //
  // So c69/c70 alias and c82 falls INSIDE g_BBTreeTypes; a compound predicate
  // put every draw in "both" and per-constant counts came back flat.
  // g_BBTreeTypes is the separator -- the billboard shader ALWAYS reads c80 and
  // the 3D vegetation shaders never do:
  //
  //   reads c70 and NOT c80  ->  T_EcoLeaves 3D geometry
  //   reads c80              ->  billboard
  //
  // A 3D count of ~0 while driving into a tree means the guest only ever submits
  // billboards, which is upstream of the renderer. NOT behind g_diag: the
  // failure cannot be captured, so it has to be answered from a log.
  if (vs_translated) {
    static std::mutex s_stMutex;
    static uint64_t s_stDraws = 0, s_st3d = 0, s_stBb = 0, s_stBark = 0;
    static uint64_t s_stRel = 0, s_stTree = 0, s_stTreeStatic = 0;
    const auto reads = [&](uint32_t c) {
      return ((vs_translated->const_mask[(c & 255u) >> 6] >> (c & 63u)) &
              1ull) != 0;
    };
    // A shader that indexes xe_c[] through a0 has a SATURATED mask, so it
    // "reads" every slot and can be identified by none of them. Counted apart
    // rather than folded into billboard: BBVertexShader does exactly this, and
    // so do skinned meshes reaching gBoneMatrixVectors.
    //
    // TWO REMOVED FINGERPRINTS, both over-matching: "has a stride-16/64-byte
    // corner table" hit 79% of all draws, and the .tree vertex layout (stride 7
    // dwords, k_16_16_16_16_FLOAT positions) hit 42% of ~900,000, all
    // static-form. The second one's NEGATIVE is worth having: we receive and
    // translate that geometry class in bulk, so the 16 vertex shaders in Xenia's
    // dump we never translate are more likely session coverage than a gap.
    //
    // StaticVertexShader -- the entry point the material XML lists FIRST --
    // reads none of c69/c70/c82, so 3D vegetation drawn through the DEFAULT
    // entry point is invisible to every constant-based classifier.
    bool tree_layout = false;
    for (const auto& a : attrs) {
      if (a.format == 32u && a.stride_bytes == 28u) {
        tree_layout = true;
        break;
      }
    }
    const bool rel = vs_translated->const_relative;
    const bool bb = !rel && reads(80);
    const bool leaf3d = !rel && reads(70) && !reads(80);          // T_EcoLeaves
    const bool bark3d =
        !rel && reads(69) && reads(82) && !reads(70) && !reads(80);  // T_EcoBark
    std::lock_guard<std::mutex> st_lock(s_stMutex);
    ++s_stDraws;
    if (bb) ++s_stBb;
    if (leaf3d) ++s_st3d;
    if (bark3d) ++s_stBark;
    if (rel) ++s_stRel;
    if (tree_layout) {
      ++s_stTree;
      // Reads none of the tree LOD constants -> the StaticVertexShader form,
      // the one every previous census could not see.
      if (!reads(70) && !reads(69) && !reads(82)) ++s_stTreeStatic;
    }
    if ((s_stDraws % 20000) == 0) {
      // THE DELTA IS THE POINT, not the total. A cumulative count cannot show
      // whether the billboard rate COLLAPSES as the camera closes on a tree,
      // which is the whole question. Each line carries the change since the
      // previous one.
      static uint64_t s_prevBb = 0, s_prev3d = 0, s_prevBark = 0, s_prevRel = 0;
      static uint64_t s_prevTree = 0, s_prevTreeStatic = 0;
      REXLOG_INFO(
          "d3d9: SPEEDTREE path census over {} draws: billboard (reads c80 "
          "g_BBTreeTypes) {} (+{} in the last 20000), 3D leaf (c70 "
          "g_TreeFade, no c80) {} (+{}), 3D bark (c69+c82, no c70/c80) {} "
          "(+{}), a0-relative/unclassifiable {} (+{}), .tree LAYOUT "
          "(stride 28, FMT_16_16_16_16_FLOAT) {} (+{}) of which STATIC-form "
          "(no c69/c70/c82) {} (+{}). A billboard delta "
          "falling to 0 while driving INTO a tree means the guest stopped "
          "submitting it. NOTE: BBVertexShader is itself a0-relative, so the "
          "TRUE billboard draws are in the relative bucket, not the c80 one.",
          s_stDraws, s_stBb, s_stBb - s_prevBb, s_st3d, s_st3d - s_prev3d,
          s_stBark, s_stBark - s_prevBark, s_stRel, s_stRel - s_prevRel,
          s_stTree, s_stTree - s_prevTree, s_stTreeStatic,
          s_stTreeStatic - s_prevTreeStatic);
      s_prevBb = s_stBb;
      s_prev3d = s_st3d;
      s_prevBark = s_stBark;
      s_prevRel = s_stRel;
      s_prevTree = s_stTree;
      s_prevTreeStatic = s_stTreeStatic;
    }
  }
  if (gpu_fetch && attrs.size() != vs_translated->vertex_fetch_count) {
    // The emitter and DecodeVertexShaderFetches walk the same instruction
    // stream, so these must agree. If they ever do not, the xe_vf[] entries
    // pair with the wrong fetches and geometry is misaddressed with no symptom
    // at the point of the mistake.
    gpu_fetch = false;
    ++g_gpuFetch.ordinalMismatch;
  }
  if (gpu_fetch) {
    // Merge the used streams into one buffer, in first-use order, and describe
    // each fetch's window into it.
    uint32_t region_of_stream[kMaxStreams];
    // One past each region's valid bytes. Separate from region_of_stream because
    // several attributes commonly share one stream and each RawFetch needs the
    // same limit, and because a clamped region is shorter than vertex_count *
    // stride so the end cannot be re-derived from the base.
    uint32_t limit_of_stream[kMaxStreams];
    std::memset(region_of_stream, 0xFF, sizeof(region_of_stream));
    std::memset(limit_of_stream, 0, sizeof(limit_of_stream));
    // Which streams cannot be windowed by the draw's vertex range.
    //
    // A fetch indexed by r0.x is indexed by the VERTEX, so copying only
    // [first_vertex, first_vertex + vertex_count) and rebasing to 0 is exact. A
    // fetch indexed by any other register is indexed by something the shader
    // COMPUTED -- an absolute row in a per-object table -- with no relationship
    // to the draw's vertex range, and windowing such a stream produced every
    // dropped region. Xenia never windows at all; done here only for the streams
    // that need it, because the whole-stream copy is 325KB against 7KB.
    //
    // Taken from the TRANSLATOR, which tracked ALU writes while walking the
    // instruction stream. Recomputing it from attrs[] is the bug:
    // DecodeVertexShaderFetches records the index REGISTER, and testing
    // `src_reg == 0 && swizzle == 0` calls a fetch vertex-indexed whenever it
    // reads r0.x -- true at shader entry, false once the shader has written it,
    // and the billboard shaders compute BOTH indices into r0.x.
    //
    // ALL of them, not just the computed ones: indices are absolute for this
    // draw, so a vertex-indexed fetch in the same shader is absolute too.
    bool whole_stream[kMaxStreams] = {};
    if (vs_translated->computed_index_fetches != 0)
      for (size_t a = 0; a < attrs.size(); ++a)
        whole_stream[attr_stream[a]] = true;
    dc.raw_vertex_bytes.clear();
    dc.raw_fetch_count = 0;
    for (size_t a = 0; a < attrs.size() && gpu_fetch; ++a) {
      if (attrs[a].fetch_slot != vs_translated->vertex_fetch_slot[a]) {
        gpu_fetch = false;
        ++g_gpuFetch.ordinalMismatch;
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
        // buffer". The hardware does not do that: an over-long vertex fetch
        // reads zero and the draw still renders
        // (metal_command_processor.cc:2377), and RawFetch::limit now gives the
        // shader that bound. start >= size_bytes falls out with no special case:
        // avail and bytes are 0, limit == base, and every fetch reads zero.
        const uint64_t avail =
            s.size_bytes > start ? uint64_t(s.size_bytes) - start : 0;
        const uint64_t bytes = std::min(want, avail);
        ++g_gpuFetchRegions;
        if (bytes < want) ++g_gpuFetchClamped;
        if (!bytes && want) {
          ++g_gpuFetchDropped;
          // Is our snapshot stale? size_bytes was read from the buffer object at
          // SetStreamSource and nothing is hooked that would tell us the guest
          // re-pointed or resized it since. Re-read the object's own size field
          // NOW and ask whether the live one would have held this window: a high
          // rescue count means the bug is the snapshot, not the guest
          // over-indexing, and those want opposite fixes.
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
          // THE DECISIVE CASE. The size theory is dead (the device's own size is
          // never larger), so what is left is whether `first_vertex` -- the
          // MINIMUM index in the conditioned index buffer -- is real. Print
          // every bound stream of the failing draw beside it: if another stream
          // comfortably holds index first_vertex+vertex_count while this one
          // cannot, the guest is fetching one attribute from a buffer sized for
          // fewer vertices and our zero-fill is correct. If NO stream holds it,
          // the index range itself is wrong.
          //
          // Keyed by SHADER, not "the first N drops": one case per distinct
          // producer is what is needed.
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
        // refused rather than assumed, because misalignment reads silently wrong
        // data rather than faulting.
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
      }
      auto& rf = dc.raw_fetch[dc.raw_fetch_count++];
      // No first_vertex adjustment: in absolute mode the index the shader
      // forms already carries it, and outside absolute mode the region is
      // windowed so its origin already is first_vertex.
      rf.base = region_of_stream[si];
      rf.stride = s.stride;
      rf.endian = s.endian;
      rf.limit = limit_of_stream[si];
    }
    if (!gpu_fetch) {
      dc.raw_vertex_bytes.clear();
      dc.raw_fetch_count = 0;
    }

    // Self-check on the ADDRESSING, bounded to the first draws of a run. The
    // picture is the only real verdict, but the half most likely to be silently
    // wrong -- the base offset, whether first_vertex is folded in, the stride,
    // and which bytes were copied -- can be checked without a GPU: decode the
    // same attribute from the guest stream and from the merged buffer at the
    // address the shader will form, and require them to be bit-identical. It
    // does NOT check the HLSL format decode or the emitted endian shuffle.
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
        // A mismatch means the shader is pointed at the wrong bytes, which
        // misaddresses geometry with no symptom at the point of the mistake.
        // One-shot: it reports once at 400 draws and never again, which
        // staleness tolerates because a check seen once is never stale.
        REXLOG_INFO("d3d9: VFETCH addressing self-check: {} draws, {} "
                    "mismatches [{}]",
                    s_checked, s_mismatch,
                    mx::gpu::health::Tag(mx::gpu::health::Zero(
                        "gpu_fetch.address_mismatch", s_mismatch, s_checked)));
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
    ++g_gpuFetch.draws;
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
  // figures of vertices. ExecuteVertexShader already returns all 16 exports per
  // vertex and this function has always discarded every one except the
  // interpolator the texture profile named; reusing that discarded work is what
  // makes the translated pixel path affordable.
  constexpr uint32_t kInterpStride =
      mx::hle::kHlslInterpolatorLinkage * 4 * uint32_t(sizeof(float));
  // Not for a draw on the GPU vertex path: its pixel stage reads what the
  // rasterizer interpolates from the vertex stage's own exports, so this stream
  // would be built at 128 bytes a vertex and then never bound.
  const bool want_interpolators = dc.pixel_shader_hlsl != nullptr && !gpu_vertex;
  if (want_interpolators)
    dc.interpolators.assign(size_t(dc.vertex_count) * kInterpStride, 0);

  // A fetch draw is finished. Everything below exists to produce per-vertex data
  // the GPU is now producing for itself: the attribute decode, the input
  // registers, the transformed positions, the interpolator stream and the UV
  // reconstruction. The mvp must be identity, which VportScaleEnabled already
  // guaranteed as a condition of gpu_vertex.
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
    // `transformed` used to be copied from dc.vertices at the TOP of this
    // function, which for a deferred draw was empty, so it had to be re-copied
    // here or the per-vertex loop wrote through a null data(). It is now
    // declared below this block, from the vertices the transcode has just
    // filled, so the crash is structurally impossible rather than patched.
    ++g_transcodeLate;
  }

  // THE INDEX WALK RUNS HERE, not before the two blocks above, and that
  // placement is the whole point. It builds `referenced[]` and rejects a draw
  // whose indices fall outside its vertex buffer; both questions are only
  // answerable once dc.vertices is FINAL, and above this line it is not:
  //
  //   - a gpu_fetch draw returns kApplied before ever needing CPU vertices; its
  //     indices are ABSOLUTE and unrebased by design, so
  //     `index >= dc.vertex_count` is the contract, not a defect;
  //   - a deferred draw arrives with vertex_stride 0 and an EMPTY dc.vertices.
  //
  // Running it at the top returned kFailed, and the caller drops a kFailed draw
  // without FinishHleDraw: 16,795 draws killed that way in one run.
  std::vector<uint8_t> transformed = dc.vertices;
  std::vector<uint8_t> referenced(dc.vertex_count, dc.index_count ? 0 : 1);
  if (dc.index_count) {
    const uint32_t iw = dc.index_16bit ? 2u : 4u;
    if (dc.indices.size() < uint64_t(dc.index_count) * iw) {
      ++g_hleShaderBadVertex;
      ++g_hleShaderBadVertexShort;
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
        ++g_hleShaderBadVertexRange;
        if (index == 0xFFFFu) ++g_hleShaderBadVertexRangeComputed;
        static bool s_shown = false;
        if (!s_shown) {
          s_shown = true;
          // The INDEX VALUE separates the candidates on its own: exactly
          // 0xFFFF is a strip cut being read as a vertex number, anything else
          // above vertex_count is a stream addressed absolutely.
          REXLOG_INFO(
              "d3d9: BAD VERTEX range (first case) index {} of vertex_count {} "
              "at i {} of {}; index_16bit {} stride {} vertices {} B; "
              "0xFFFF here would be a strip cut read as a vertex",
              index, dc.vertex_count, i, dc.index_count,
              dc.index_16bit ? 1 : 0, dc.vertex_stride,
              uint32_t(dc.vertices.size()));
        }
        return ShaderApplyResult::kFailed;
      }
      referenced[index] = 1;
    }
  }

  uint64_t applied_vertices = 0;
  uint64_t identity_in_clip = 0, viewport_in_clip = 0;
  uint64_t uv_vertices = 0, uv_missing_export = 0;
  float uv_min[2] = {1.0e30f, 1.0e30f};
  float uv_max[2] = {-1.0e30f, -1.0e30f};
  // First referenced vertex, kept whole so a collapsed UV can be traced to the
  // stage that produced it: the attribute values that went into the shader, and
  // every export that came out -- not only the one the texture profile selected.
  // The selected export alone cannot distinguish "the shader exported zero" from
  // "the UV is in a different export".
  AluResult probe{};
  std::vector<std::array<float, 4>> probe_values;
  bool have_probe = false;

  // Does a VS export feed this interpolator at all, or does the rasterizer
  // generate it? SQ_PROGRAM_CNTL bit 18 (`param_gen`) makes the hardware
  // synthesise an interpolator holding the screen-space position, and
  // SQ_CONTEXT_MISC selects its destination register; vs_export_count only
  // describes how many interpolators the vertex shader exports and cannot select
  // this input. PM4 captures show vs_export_count=2 while the following
  // SQ_CONTEXT_MISC selects r3, and exports[3] reads zero because no vertex
  // export is supposed to feed it.
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
        // Short window: zero what is missing rather than abandoning the draw --
        // the same rule and reason as the GPU clamp above, and this is the
        // fallback those refusals land in. Zeroing the WHOLE stride first means
        // a partially readable vertex reads its valid bytes and zeros past them.
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
        // A different rule: the attribute extends past the STRIDE, not past the
        // buffer (shader_ucode.cpp:220). Left fatal on purpose so the window
        // bound is judged alone; counted apart so its share stays visible.
        ++g_hleShaderBadAttribute;
        return ShaderApplyResult::kFailed;
      }
      values[a] = {f[0], f[1], f[2], f[3]};
    }

    // Merge the attributes into their destination registers, by exactly the rule
    // shader_alu.cpp:614 seeds its register file with -- three bits per
    // destination component, 0-3 selecting x/y/z/w of the fetched value, 4 and 5
    // the constants 0.0 and 1.0, 7 meaning keep. `kKeep` is why two fetches can
    // share a register, so writing all four components would reintroduce the
    // clobber that decoder documents.
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
      //     shader's exports, which it does natively;
      //   - the param_gen UV reconstructs the hardware's screen-space parameter,
      //     which in a pixel shader that reads it IS SV_Position;
      //   - the finite/NaN rejection guards against an INTERPRETER emitting
      //     garbage, and there is no interpreter here;
      //   - the in-clip scoring only feeds g_hleShaderMvpDisagree.
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
      ++g_hleShaderBadVertexNonFinite;
      return ShaderApplyResult::kFailed;
    }

    // The position export is homogeneous clip space, passed to the host
    // untouched. Dropping w -- on the claim that PA_CL_VTE_CNTL is 0x300, i.e.
    // XYZ already multiplied by 1/W0 -- scaled all 3D geometry by whatever it
    // should have divided by; that register actually reads 0x400 or 0x43F.
    //
    // Dividing here instead is not enough either: 1.2 million vertices per run
    // carry w <= 0, behind the eye, and a negative w mirrors the vertex through
    // the origin rather than removing it. Those triangles must be clipped
    // against the near plane *before* any divide, which D3D12 does in hardware
    // given clip space.
    const float p[4] = {r.position[0], r.position[1], r.position[2], w};
    std::memcpy(transformed.data() + size_t(v) * dc.vertex_stride, p,
                sizeof(p));

    // The shader's own interpolators, verbatim, for the translated pixel path.
    // No reconstruction and no viewport transform: these are the values the
    // pixel shader's registers are seeded with on the hardware. The param_gen
    // synthesis further down is a separate thing -- it fabricates the ONE
    // interpolator the hardware generates, and only the stand-in path needs it.
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
    // Resolved render-target samples intentionally carry no CPU texture payload:
    // the renderer samples the ordered target resource instead. UVs are shader
    // outputs and must be populated for both resource paths -- the old
    // `dc.texture` guard left every resolved sample at BuildHleDraw's default
    // (0,0), which explains the flat single-texel compositor wedges.
    if (texture_binding && uv_generated) {
      // The rasterizer's parameter, reconstructed: its screen-space position is
      // exactly the viewport transform of the clip-space position this vertex
      // already carries, with NDC y inverted because screen y runs downward.
      //
      // Written normalized and NOT divided by the texture extent. The fetch is
      // unnormalized and the hardware parameter is in pixels, but that pixel
      // count is the render target's, not the sampled texture's, and the divide
      // below uses the sampled extent.
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
    // shader output itself: normalized coordinates are already host-ready, while
    // pre-transformed window coordinates only enter clip after the live viewport
    // inverse. Scored over every referenced vertex, per draw.
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
    // happen first: all 256 slots once went to the pre-load compositor, 28
    // seconds before `force_load` fired, so every UV line described a scene that
    // was not loaded yet. Rate-limit by time instead.
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
        // A shader with one float2 input and one exported interpolator has to be
        // computing that interpolator from constants. Whether it asked for any,
        // and whether they came back empty, separates "the interpreter dropped
        // the maths" from "the constant file we handed it was blank".
        REXLOG_INFO(
            "d3d9:   alu status={} export_mask=0x{:04X} const_reads={} "
            "zero_reads={} index_range={}..{} const_dwords={}",
            AluStatusName(probe.status), probe.export_mask, probe.const_reads,
            probe.const_zero_reads, probe.const_min_index,
            probe.const_max_index, alu_in.alu_const_dwords);
        // How much of the constant file is populated at all. "c255 is zero"
        // means one thing if the file is otherwise full and quite another if the
        // whole file is blank -- the second would indict the read, not the guest.
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
        // saturated index as with a real c255.
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
  // itself. Its shadow follows the SQ_PROGRAM_CNTL pattern: the draw-time flush
  // issues sub_82564768(device, 0, 8704, device + 10548) with 8704 = 0x2200, and
  // sub_82564768 sends register base+i from shadow+i*4, so 0x2206 sits at
  // device + 10548 + 6*4.
  //
  //   vport_x_scale_ena (bit 0) == 0 -> no viewport scale, so the shader
  //   exported window space and we apply the inverse. == 1 -> the export is clip
  //   space and the transform here is identity.
  //
  // The old rule was `identity_in_clip > viewport_in_clip`, a strict > so ties
  // went to viewport; with in-clip commonly 0 for both, an unknown share of
  // viewport draws defaulted rather than won.
  const bool hw_applies_viewport = VportScaleEnabled(device, base);
  const bool contest_says_identity = identity_in_clip > viewport_in_clip;
  if (contest_says_identity != hw_applies_viewport) ++g_hleShaderMvpDisagree;
  // The offset was checked before being acted on, because the derivation
  // disagreed with this file's note that PA_CL_VTE_CNTL is 0x300. A dump of the
  // surrounding dwords settled it: 17 dwords below sits 640.0, 640.0, -90.0,
  // 90.0, 1.0 -- PA_CL_VPORT_XSCALE/XOFFSET/YSCALE/YOFFSET/ZSCALE, whose own
  // shadow base puts XSCALE exactly there. The register reads 0x43F on every
  // draw, so the shader exports clip space and the transform here is identity.
  // `contest_says_identity` survives only to feed g_hleShaderMvpDisagree.
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
// Everything downstream preserves the order of this list, and none of that helps
// if the order was already wrong when entries arrived. A capture showed the
// 1280x720 light-buffer snapshot sampled by all 44 draws of the HDR scene pass
// and written only by copies AFTER that pass ended, so one of two things is true
// and they need opposite fixes:
//
//   resolve and the scene draws on DIFFERENT threads
//       -> we flatten independent streams by arrival, and ordering is the fix
//   all on ONE thread
//       -> the guest really does resolve late, and the defect is that the
//          snapshot does not survive from the frame that filled it
//
// Counts by thread at PUSH time, which does not depend on how the batching
// falls: g_pendingHleDraws is flushed constantly in batches of a handful.
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

// Where each resolve sits in the frame's ordered list, and how long that list is
// by the end. A resolve at index 12 of 900 is early in the frame; one at 880 is
// late. This says whether the guest resolves before or after the draws that
// sample the result.
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
// dword_830B334C is read and written NOWHERE ELSE in the image, so if the
// counter it compares against never moves, the page table is never refined and
// keeps every entry at 0xF00A, the guest's own not-available marker:
//
//   latch never moves  -> the gate never fires; chase the counter's producer
//   latch moves        -> the update DOES run and writes only sentinels, and the
//                         question becomes what our feedback contains
//
// Both are FIXED guest globals (imagebase 0x82000000, no ASLR), so no hook is
// needed. Reported unconditionally and NOT folded into FRAME COST, which is
// gated on cost thresholds -- a cheap frame would print nothing and "the gate
// never fired" would be indistinguishable from "the probe never ran".
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
    // to finalize -- both would refuse it and count it as a dropped draw. It
    // still has to keep its slot in the frame's ordered list: the snapshot it
    // stands for is the target's contents *at this point*, and every draw after
    // it must sample that rather than the surface's later state.
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
  // Every other frame signal is gated or sampled -- FRAME COST fires only on
  // busy frames, VdSwap logs periodically -- and reading a frame rate off either
  // gave a wrong answer three times in one session, twice in the flattering
  // direction. A frame rate has to come from something that logs every frame.
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

  // Every frame, not sampled: a slow frame is the one worth seeing.
  //
  // The texture bucket is NOT inside finalize -- PrepareDrawTexture runs when
  // the draw is recorded, not when the frame is flushed -- so the two are
  // reported side by side and not subtracted from one another. Gated on the
  // vertex buckets too: without them, a run whose vertex work has moved to the
  // GPU logs only its texture-heavy loading frames.
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
    // Inside the texture bucket. Printed beside it for the same reason as LOOP
    // BY REASON below: the parts must be checkable against the total rather than
    // trusted on their own. A large remainder means the cost is somewhere none
    // of these five timers is watching.
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
        // found the texture still flat -- completely different diagnoses.
        g_flat.notCached, g_flat.retriesDue, g_flat.volatileKeys);
    // On this line rather than its own cadence: decodes are rare once the
    // cache is warm, so anything keyed on a decode count never prints.
    // Unnamed is expected for a render target, a glyph atlas, and the 94
    // assets whose level 0 is shared with another asset.
    // The first three PARTITION `seen`; shortBuffer cuts across them and is
    // printed apart for that reason. An earlier version reported only three of
    // the four and they did not add up, which left 666 textures unaccounted
    // for and invited exactly the wrong conclusion about the rest.
    REXLOG_INFO("d3d9: TEXTURE NAMES -- {} decodes = {} named ({:.1f}%, {} by "
                "prefix) + {} keyed but MATCHED NOTHING + {} with no key; "
                "separately, {} had a buffer SHORTER than the described level 0",
                g_texNames.seen, g_texNames.named,
                g_texNames.seen
                    ? g_texNames.named * 100.0 / double(g_texNames.seen)
                    : 0.0,
                g_texNames.byPrefix, g_texNames.unmatched, g_texNames.noKey,
                g_texNames.shortBuffer);
    // BY FORMAT, named against unmatched. A format the asset corpus does not
    // contain cannot be matched, so its unmatched count is not a failure --
    // it is the measure of how much of the frame the game generates rather
    // than loads. Only a format that appears in BOTH columns is evidence of
    // the join losing textures it should have found.
    {
      std::string rows;
      for (uint32_t f = 0; f < 64; ++f) {
        const uint64_t n = g_texNames.namedByFormat[f];
        const uint64_t u = g_texNames.unmatchedByFormat[f];
        if (!n && !u) continue;
        rows += fmt::format(" [{} named {} unmatched {}]",
                            mx::hle::GuestTextureFormatName(f), n, u);
      }
      REXLOG_INFO("d3d9: TEXTURE NAMES   by format --{}", rows);
    }
    // GEOMETRY, the same join. `buffers` PARTITIONS into the five counters
    // below it -- named vertex, named index, unmatched, too small to be a mesh,
    // and unresolved -- so a reader can see the whole population without
    // subtracting. The texture version of this line shipped reporting three of
    // four buckets and left 666 decodes unaccounted for.
    REXLOG_INFO("d3d9: MESH NAMES -- {} draws, {} buffers = {} vertex + {} "
                "index named ({:.1f}%, {} by prefix) + {} MATCHED NOTHING + {} "
                "under {}B (not a mesh) + {} unresolved; memo {}/{} hit",
                g_meshNames.draws, g_meshNames.buffers,
                g_meshNames.namedVertex, g_meshNames.namedIndex,
                g_meshNames.buffers
                    ? (g_meshNames.namedVertex + g_meshNames.namedIndex) *
                          100.0 / double(g_meshNames.buffers)
                    : 0.0,
                g_meshNames.byPrefix, g_meshNames.unmatched,
                g_meshNames.unmatchedTiny, kMeshMinBytes, g_meshNames.noHost,
                g_meshNames.hashMemoHit,
                g_meshNames.hashMemoHit + g_meshNames.hashMemoMiss);
    // The same join over DISTINCT buffers. This is the number that answers
    // "is this mesh in the assets"; the line above answers "how much of the
    // frame is asset geometry", and they are not the same question.
    REXLOG_INFO("d3d9: MESH NAMES   by CONTENT {} of {} named ({:.1f}%) -- the "
                "mesh count; by address {} of {} ({:.1f}%) -- inflated by ring "
                "reallocation; of the misses {} were 64KB or larger, largest {}"
                " bytes",
                g_meshNames.contentNamed, g_meshNames.contentSeen,
                g_meshNames.contentSeen
                    ? g_meshNames.contentNamed * 100.0 /
                          double(g_meshNames.contentSeen)
                    : 0.0,
                g_meshNames.distinctNamed, g_meshNames.distinctSeen,
                g_meshNames.distinctSeen
                    ? g_meshNames.distinctNamed * 100.0 /
                          double(g_meshNames.distinctSeen)
                    : 0.0,
                g_meshNames.unmatchedOver64K, g_meshNames.largestUnmatched);
    // The misses, by the shader that drew them, worst first. Distinct meshes,
    // not bindings.
    {
      std::vector<std::pair<uint64_t, const std::string*>> worst;
      for (const auto& [who, n] : MeshMissByShader())
        worst.emplace_back(n, &who);
      std::sort(worst.begin(), worst.end(),
                [](const auto& a, const auto& b) { return a.first > b.first; });
      std::string rows;
      for (size_t i = 0; i < worst.size() && i < 10; ++i)
        rows += fmt::format(" [{} x{}]", *worst[i].second, worst[i].first);
      REXLOG_INFO("d3d9: MESH NAMES   unmatched meshes by shader, {} distinct "
                  "shaders --{}", worst.size(), rows);
    }
    // Replay, which the per-draw counters above cannot reach. Added to them it
    // gives the frame's real draw-weighted coverage; on its own it says how
    // much of the recorded geometry the assets can name.
    REXLOG_INFO("d3d9: MESH NAMES   cmdbuf replay: {} re-issued draws, {} "
                "({:.1f}%) run geometry with an asset name. Frame total with "
                "replay: {} of {} draws carry a named mesh",
                g_meshNames.replayDraws, g_meshNames.replayNamed,
                g_meshNames.replayDraws
                    ? g_meshNames.replayNamed * 100.0 /
                          double(g_meshNames.replayDraws)
                    : 0.0,
                g_meshNames.drawsNamed + g_meshNames.replayNamed,
                g_meshNames.draws + g_meshNames.replayDraws);
    // And what the unnamed replays ARE. Foliage the assets keep in .tree
    // rather than .surface is a correct miss; anything else is not.
    {
      std::vector<std::pair<uint64_t, const std::string*>> worst;
      for (const auto& [who, n] : ReplayMissByShader())
        worst.emplace_back(n, &who);
      std::sort(worst.begin(), worst.end(),
                [](const auto& a, const auto& b) { return a.first > b.first; });
      std::string rows;
      for (size_t i = 0; i < worst.size() && i < 8; ++i)
        rows += fmt::format(" [{} x{}]", *worst[i].second, worst[i].first);
      REXLOG_INFO("d3d9: MESH NAMES   unnamed REPLAYED draws by shader, {} "
                  "distinct --{}", worst.size(), rows);
    }

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
      // when no address was added. Byte totals climbing behind a stable top five
      // is drift and belongs to the heartbeat.
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
    // whether it ever asked for it back through SetTexture. The whole
    // population, total printed first, and NOT gated on there being orphans --
    // "0 orphans of 14" and "no line at all" have to be distinguishable.
    {
      // THROTTLED. These two are the widest lines in the log -- RESOLVE
      // CONSUMPTION is 6181 bytes per line, and the pair was 3.6MB of one 10.8MB
      // run, which is what rotates the log away every ~30 seconds. They inherit
      // the enclosing cost trigger, which is right for FRAME COST but not for
      // these: they are cumulative whole-population snapshots, identical on
      // consecutive slow frames apart from drift. So: print when the POPULATION
      // or the FINDING changes, plus a heartbeat.
      //
      // The signature is computed BEFORE the row strings are built, and the
      // strings are built ONLY when printing -- this runs on the render thread
      // and formatted ~9KB per slow frame regardless.
      constexpr auto kCensusHeartbeat = std::chrono::seconds(10);

      // VIDEO TARGET CONSUMPTION. Every texture bound at one of the three
      // _VideoRenderTarget extents, by base address. The whole population: "0
      // rows" means the guest never binds a texture at any of those extents, a
      // completely different finding from "rows exist and none of them draw".
      //
      // 1280x720 is also the scene render-target extent, so a row at that shape
      // is not on its own the video asset. Read the ADDRESSES.
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
      // whether it ever asked for it back through SetTexture. Orphans are listed
      // by name because the interesting one is a specific surface: the menu
      // backdrop is a 1280x430 resolve produced exactly once and never sampled.
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
                // REACHED, printed next to the extent it is judged against.
                // Without it this row cannot say WHY a destination was claimed:
                // the 2048x2048 ping-pong pair reads `part0` here, and whether
                // that means "the GPU wrote all of it" or "the coverage entry
                // was never consulted" is the whole difference.
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
  // `lost` should stay at zero, and means a deferred draw reached a caller that
  // could not supply the inputs to fill it. Checked HERE, immediately before the
  // reset, rather than beside the FRAME COST line that prints it: that line is
  // gated on an expensive frame, so a check inside it would only see slow frames.
  mx::gpu::health::Zero("transcode.lost", g_transcodeLost, g_transcodeDeferred);
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

void CollectPixelShaderBlob(uint32_t handle, uint8_t* base) {
  (void)base;
  if (!handle || g_patch.psBlobs.count(handle)) return;
// D3DDevice_CreatePixelShader copies the source header to object+0x28, allocates
// pFunction[2] code bytes separately, and sub_825506B0 stores that allocation at
// object+0x18. Pixel shader objects do not share the vertex shader's inline
// +0x368 representation.
//
// **The CF stream does not start at the beginning of that allocation** -- big
// shaders carry a prologue of zeros. Searching the blob, or trying every offset
// and accepting a unique valid decode, left 14 shaders on "ambiguous CF offset"
// with no texture bindings at all. Neither is needed: the object states where
// the code begins, from the shader flush sub_82565928:
//
//     v22 = *((char *)v8 + v8[16] + 40) + v8[6];        // program base
//     *v23 = *((char *)v8 + v8[16] + 44) >> 2;          // size in dwords
//
// with v8 the shader object, v8[6] = +0x18 (the code allocation) and v8[16] =
// +0x40 (an info block within the object), so the address D3D9 hands the
// hardware is *(object+0x18) + *(object + *(object+0x40) + 0x28) and its length
// is at +0x2C. +0x30 is the *allocation* size and is kept only as a bound.
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
  // fields are bounds-checked against the allocation rather than trusted: a bad
  // info offset must degrade to the old whole-allocation behaviour, not read off
  // the end.
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
// 120,000 menu draws bind a NULL pixel shader and so keep the tex*col stand-in.
// The guest's own PM4 flush sub_82565928 has an explicit path for them: with no
// pixel object, and when `vs[218] & 0x20`, it emits a second blob selected by
// `vs[226]` instead of the usual `vs[224]`.
//
// The packet encoding argues it is a VERTEX program, not the missing pixel one.
// Both blobs are loaded with PM4_IM_LOAD, whose first data dword carries the
// shader type in its low two bits (kVertex 0, kPixel 1), and the real pixel path
// forces that bit:
//
//     (v22 & 0x1FFFFFFE) | 1        <- pixel object, type = kPixel
//      v69 & 0x1FFFFFFF             <- vs[224] AND vs[226] alike, type = kVertex
//
// If that is right, these draws emit no pixel IM_LOAD at all and inherit
// whichever pixel program was loaded last -- a completely different fix.
//
// Decided by structure: both blobs are emitted as each stage and the results
// compared, since a vertex program exports position (register 62) and a pixel
// program exports colour (0-3). vs[224] is known-vertex and is probed as a
// control.
//
// Layout from the decompile: the blob header sits at `vs + vs[table] + 872`, and
// within it dword 0 is the microcode offset (added to `vs[8]`), dword 1 the size
// in BYTES, dwords 2 and 3 the SQ_PROGRAM_CNTL and SQ_CONTEXT_MISC the flush
// later writes to 0x2180. `base` is not unused: REX_LOAD_U32 and REX_RAW_ADDR
// expand to reference it by name.
void ProbeVertexObjectSecondBlob(uint32_t device, uint8_t* base) {
  (void)base;
  static std::mutex s_mu;
  static std::set<uint32_t> s_seenVs;
  static uint64_t s_withSecond = 0, s_withoutSecond = 0;
  if (!device || !HostPageReadable(REX_RAW_ADDR(device + 0x3248))) return;
  const uint32_t vs = REX_LOAD_U32(device + 0x3248);
  // Deduplicated by VERTEX OBJECT, not by call count. Capped at two reports,
  // both landed on the same object, which says nothing about whether the rest of
  // the population looks like it. The tally below covers every draw; the
  // expensive decode runs only for the first few distinct objects.
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


}  // namespace mx::hooks::d3d9

// Declared in hooks_d3d9.h, as a free function rather than the atomic itself
// because hooks_frame.cpp reads this and cannot include the internal header --
// that header needs mx::hle types (HleStream, D3D9Element, LayoutError) which
// hooks_frame.cpp does not pull in. Same shape as GuestDrawCalls below.
void NotePlumbedStencil(const mx::hle::DrawCall& dc) {
  mx::hooks::d3d9::NotePlumbedStencilImpl(dc);
}

// Declared in hooks_d3d9.h. The three exits that make `guest` exceed `accepted +
// refused`, so the FRAME DRAWS gap is attributable without a debug cvar. `skips`
// is the BuildHleDraw population, which was already counted but only ever
// printed under --hle_diag.
void UnbuiltDrawReasons(uint64_t& no_viewport, uint64_t& shader_failed,
                        uint64_t& nocode_queue_full, uint64_t& skips) {
  no_viewport = mx::hooks::d3d9::g_drawOutcome.noViewport;
  shader_failed = mx::hooks::d3d9::g_drawOutcome.shaderFailed;
  nocode_queue_full = mx::hooks::d3d9::g_drawOutcome.shaderNoCodeFull;
  skips = 0;
  const uint64_t* counts = mx::hle::HleSkipCounts();
  for (uint32_t i = 1; i < uint32_t(mx::hle::HleSkip::kCount); ++i)
    skips += counts[i];
}

std::string UnbuiltSkipBreakdown() {
  // One run attributed the whole gap to BuildHleDraw -- 16,706 of 16,706, with
  // no-viewport and shader-failed both at zero -- so the reasons ARE the answer,
  // and they were only printed under --hle_diag. Promoted here because the
  // gap is a standing property of every run.
  //
  // Ranked, and zero rows omitted: with eleven possible reasons a full list is
  // mostly zeros and the one that matters does not stand out.
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

uint64_t HleDrawsAccepted() { return mx::hooks::d3d9::g_drawOutcome.accepted; }
uint64_t HleDrawsRefused() {
  // Both last-gate refusals and the deferred path's own discards, because a
  // draw lost either way is a draw the renderer never issues and the caller is
  // asking "did we lose it", not "where".
  return mx::hooks::d3d9::g_drawOutcome.refused + mx::hooks::d3d9::g_pendingDropped;
}
