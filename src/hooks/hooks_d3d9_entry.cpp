// D3D9 guest entry points.
//
// The REX_FUNC hooks the recompiler routes the guest's D3D9 calls through, plus
// the macro-generated render-state hooks and the helpers only they use.
// extern "C", so they reach into the layer's namespace through
// hooks_d3d9_internal.h.
//
// Deliberately no count here. This said "the 17 REX_FUNC hooks" while there
// were 23 and nine render-state leaves, because a number in a comment goes
// stale the first time someone adds a hook and nothing checks it. Count them if
// you need the figure:
//
//   grep -c '^extern "C" REX_FUNC' src/hooks/hooks_d3d9_entry.cpp
//
// Two families that used to live here are now their own translation units, both
// of them entry points and neither of them small: Resolve and Clear are in
// hooks_d3d9_resolve.cpp, and the PM4 scanning that sat behind
// ExecuteCommandBuffer is in hooks_d3d9_pm4.cpp.
//
// Every hook calls its original exactly once and opens with MX_D3D9_HLE_LOCK,
// which is defined in hooks_d3d9_internal.h -- there is exactly one definition
// of it in src/ and it must stay that way. Use it on anything added here.

#include "gpu/health.h"
#include "hooks/hook_common.h"

#include <bit>

#include <rex/cvar.h>

#include <array>
#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "gpu/d3d9_draw.h"
#include "gpu/d3d9_layout.h"
#include "gpu/d3d9_state.h"
#include "gpu/d3d9_texture.h"
#include "gpu/hle_types.h"
#include "gpu/shader_ucode.h"

#include "hooks/guest_read_watch.h"
#include "hooks/hooks_d3d9_internal.h"
#include "hooks/hooks_d3d9_pm4.h"

REXCVAR_DECLARE(bool, hle_diag);

// The guest entry points below are extern "C" and cannot live in the namespace,
// so they reach into it wholesale. This is the seam hooks_d3d9_entry.cpp uses.
using namespace mx::hooks::d3d9;

void FinalizePendingD3D9Draws(uint8_t* base) {
  FinalizePendingD3D9DrawsImpl(base);
}

void ReportHostPageQueryStats() {
  static uint64_t s_calls = 0, s_queries = 0, s_nanos = 0;
  const uint64_t now_calls = g_hprCalls.load(std::memory_order_relaxed);
  const uint64_t now_queries = g_hprQueries.load(std::memory_order_relaxed);
  const uint64_t now_nanos = g_hprNanos.load(std::memory_order_relaxed);
  const uint64_t calls = now_calls - s_calls;
  const uint64_t queries = now_queries - s_queries;
  const uint64_t nanos = now_nanos - s_nanos;
  s_calls = now_calls;
  s_queries = now_queries;
  s_nanos = now_nanos;
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
        calls, queries, nanos / 1000000, now_queries);
  }
  // Bound the staleness: see the note on HostPageReadable.
  InvalidateHostPageCache();
}

//=============================================================================
// 0x8293C778 - Scaleform GFx: the glyph cache flush.
//
// NOT a diagnostic hook. Two things in it are LOAD-BEARING:
//
//   g_glyphCacheGeneration  the atlas staleness key. Glyph atlases are rewritten
//                           IN PLACE, so GuestTextureFingerprint cannot see the
//                           change and this bump is their ONLY invalidation.
//   NoteGlyphCacheGeometry  what teaches IsGlyphCacheTexture which extents ARE
//                           atlases.
//
// Remove it and text goes stale whenever Scaleform repacks, silently.
//
// RELEASE, and AFTER the original: the bump publishes the atlas bytes the
// original just wrote.
REX_IMPORT(__imp__sub_8293C778, orig_GlyphCacheFlush, void());
extern "C" REX_FUNC(sub_8293C778) {
  const uint32_t cache = ctx.r3.u32;
  uint32_t pending = 0;
  if (cache && HostPageReadable(REX_RAW_ADDR(cache + 28)))
    pending = REX_LOAD_U32(cache + 28);
  // The extent is the discriminator between a glyph atlas and any other kR8
  // texture, so it has to be recorded even on a flush that carries no rects.
  if (cache && HostPageReadable(REX_RAW_ADDR(cache)) &&
      HostPageReadable(REX_RAW_ADDR(cache + 4))) {
    NoteGlyphCacheGeometry(REX_LOAD_U32(cache), REX_LOAD_U32(cache + 4));
  }

  orig_GlyphCacheFlush(ctx, base);

  // Nothing was uploaded, so nothing to invalidate.
  if (!pending) return;
  g_glyphCacheGeneration.fetch_add(1, std::memory_order_release);
}

//=============================================================================
// 0x82945D20 - DefineCompactedFont. THE LOADER THIS GAME ACTUALLY USES.
//
// This title does NOT load fonts through DefineFont, DefineFont2 or DefineFont3
// -- hooks on those measured 0 loads across a whole process lifetime. The game
// ships .gfx (compacted) data.
//
// This loader computes the answer itself, which matters because the guest's own
// logging is unreachable (no GFxLog installed):
//
//     expected = *(DWORD *)(tag + 8) - 2      the payload it intends to read
//     font+32                                  bytes it actually copied in
//     font+84                                  nominal size; 0 means the parse
//                                              produced nothing
//
// The copy loop reads in 4096-byte chunks and BREAKS on a short read, then
// parses whatever it got, and sub_82944D90 bails outright under 15 bytes. Either
// way the glyph table ends early.
//
// font+52 is the glyph count as read by sub_82944D90. UNVERIFIED: the parser
// reads its source blob from font+48 while the loader fills font+28, and that is
// not reconciled. The two numbers this hook rests on are `written vs expected`
// and the nominal size.
//=============================================================================
namespace {

// Bounded copy of a NUL-terminated guest string, page-checked at the start and
// at every page boundary it crosses. Kept when the GFx log sinks were removed
// because the font diagnostics below still read guest strings.
void GfxGuestStr(uint8_t* base, uint32_t addr, char* out, size_t max) {
  size_t i = 0;
  if (addr && max) {
    for (; i + 1 < max; ++i) {
      const uint32_t a = addr + static_cast<uint32_t>(i);
      if ((i == 0 || (a & 0xFFF) == 0) && !HostPageReadable(REX_RAW_ADDR(a)))
        break;
      const uint8_t c = REX_LOAD_U8(a);
      if (!c) break;
      out[i] = static_cast<char>(c);
    }
  }
  out[i] = '\0';
}

}  // namespace

REX_IMPORT(__imp__sub_82550B80, orig_CreateVertexDeclaration, void());
extern "C" REX_FUNC(sub_82550B80) {
  MX_D3D9_HLE_LOCK;
  const uint32_t elements = ctx.r3.u32;
  const uint64_t n = ++g_decls;

  orig_CreateVertexDeclaration(ctx, base);

  const uint32_t decl = ctx.r3.u32;

  // Record it for the draw-time correlation before anything else. This is the
  // only place the element array can be read safely -- the runtime has just
  // walked it -- and it is what makes the draw-side lookup a comparison rather
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
  } else if (decl_id >= 0 && g_declLayoutErr[decl_id].skipped) {
    // The declaration decoded, but not all of it. The dropped elements are ones
    // the transcode never reads, but that is a claim about what reads them and
    // has to stay checkable. Printed WITH its denominator, once per
    // reason+detail.
    const auto& e = g_declLayoutErr[decl_id];
    static std::set<uint64_t> s_seen;
    const uint64_t key =
        (uint64_t(uint32_t(e.skip_reason)) << 32) ^ e.skip_detail;
    if (s_seen.insert(key).second) {
      REXLOG_INFO(
          "d3d9: declaration id {} dropped {} of {} elements, first is "
          "element {}: {} (detail 0x{:08X}) -- not read by the transcode, "
          "geometry kept",
          decl_id, e.skipped, e.offered, e.skip_element,
          mx::hle::LayoutErrorText(e.skip_reason), e.skip_detail);
    }
    DeclFile() << "  LAYOUT DROPPED " << e.skipped << " of " << e.offered
               << " elements, first element " << e.skip_element << ": "
               << mx::hle::LayoutErrorText(e.skip_reason) << " detail 0x"
               << std::hex << e.skip_detail << std::dec << "\n";
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
  REXLOG_INFO("d3d9: decl #{} written to logs/decldump/decls.txt", n);
}

//=============================================================================
// 0x82564C50 -- D3D::PatchVertexShaderToMatchVertexDeclaration(
//                  CVertexShader*, ULONG*, const CVertexDeclaration*,
//                  const BYTE*, ULONG)
//
// Where semantics get bound to shader inputs at runtime -- the reason they do
// not survive into the microcode. Only 3 xrefs, all D3D9-internal, because it is
// reached from the lazy-state path at draw time.
//
// **Which register holds the declaration is determined by comparison, not by
// reading the mangled signature.** A signature misread would be invisible in the
// output; a mismatch here is loud.
//=============================================================================

REX_IMPORT(__imp__sub_82564C50, orig_PatchVertexShader, void());
extern "C" REX_FUNC(sub_82564C50) {
  MX_D3D9_HLE_LOCK;
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

  // Predict before, compare after. The arguments are only guaranteed good across
  // this call, and the point of the test is what the *original* writes. Sampled:
  // the rule either holds on every slot or it does not hold at all.
  static std::vector<PatchPrediction> s_pred;
  const bool probe = REXCVAR_GET(hle_diag) && (g_patchCalls % 16) == 0;
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
// 0x825565C8 -- D3DDevice_DrawIndexedVertices(D3DDevice*, D3DPRIMITIVETYPE,
//                  INT BaseVertexIndex, UINT StartIndex, UINT IndexCount)
//
// Note IndexCount, not PrimitiveCount -- the 360 variant differs from the PC
// API here. 19 call sites in game code.
//=============================================================================

REX_IMPORT(__imp__sub_825565C8, orig_DrawIndexedVertices, void());
extern "C" REX_FUNC(sub_825565C8) {
  NoteUpDrawCaller(static_cast<uint32_t>(ctx.lr), ctx.r6.u32, 0);
  g_guestDrawCalls.fetch_add(1, std::memory_order_relaxed);
  MX_D3D9_HLE_LOCK;
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
  if (REXCVAR_GET(hle_diag)) {
    DeviceState().NoteDevice(ctx.r3.u32, mx::hle::kEpDraw);
    SampleFetchConstantFile(ctx.r3.u32, base);
    ScoreDraw(/*indexed=*/true, ctx.r6.u32, ctx.r7.u32, ctx.r3.u32, base);
    DumpHleDraw(/*indexed=*/true, n, ctx.r4.u32, ctx.r5.s32, ctx.r6.u32,
                ctx.r7.u32);
  }
  orig_DrawIndexedVertices(ctx, base);
  // The original draw performs D3D9's lazy vertex-shader patching. Translate
  // after it returns so a shader's first draw can use the exact patched code
  // captured by sub_82564C50 during this call. Save the PPC arguments above: the
  // guest call is free to clobber volatile registers.
  BuildAndQueueDraw(/*indexed=*/true, primitive_type, start_index,
                    index_count, base_vertex, device, base);
  ReportDrawCounts(base);
}

//=============================================================================
// 0x825561B0 -- D3DDevice_DrawVertices(D3DDevice*, D3DPRIMITIVETYPE,
//                  UINT StartVertex, UINT VertexCount)
//
// 34 call sites in game code.
//=============================================================================

REX_IMPORT(__imp__sub_825561B0, orig_DrawVertices, void());
extern "C" REX_FUNC(sub_825561B0) {
  NoteUpDrawCaller(static_cast<uint32_t>(ctx.lr), ctx.r6.u32, 1);
  g_guestDrawCalls.fetch_add(1, std::memory_order_relaxed);
  MX_D3D9_HLE_LOCK;
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
  if (REXCVAR_GET(hle_diag)) {
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

// Build draws from the BeginVertices/EndVertices path.
//
// ALWAYS ON, and not optional: this is the ONLY way the engine's UI draws reach
// us, and without it the intro logo is never submitted at all.
//
// The run that first proved the hook works also took an access violation:
//
//     write to guest 0x58 in sub_8234CE20 +0x10B
//     sub_8234CE20:  if (!this[105]) { v2 = this[37]; *(v2 + 88) = 1; ... }
//
// 0x58 is 88 decimal, so `this[37]` was null: a one-shot init against a
// half-constructed object, where the guard at the top is not atomic with the
// `this[105] = 1` at the bottom. It reached 2 crashes in 5 runs and then stopped
// reproducing, and was never explained. If guest-side faults reappear around
// front-end construction, suspect this first;
// ui-draws-bypass-hooked-entry-points carries the register dump.
namespace {

// Set while D3DDevice_DrawVerticesUP's original is running. See the
// BeginVertices hook below for why.
thread_local uint32_t t_inDrawVerticesUP = 0;
struct UpDepthGuard {
  UpDepthGuard() { ++t_inDrawVerticesUP; }
  ~UpDepthGuard() { --t_inDrawVerticesUP; }
};

// What BeginVertices reserved, carried to the matching EndVertices. Thread-local
// because this title submits draws from several threads, and the pair is
// strictly nested within one call chain on one thread.
struct PendingVertices {
  uint32_t device = 0;
  uint32_t prim_type = 0;
  uint32_t count = 0;
  uint32_t stride = 0;
  uint32_t data = 0;  // the guest write pointer BeginVertices returned
  bool active = false;
};
thread_local PendingVertices t_pendingVertices;

std::atomic<uint64_t> g_bv_draws{0};
std::atomic<uint64_t> g_bv_suppressed{0};   // nested inside DrawVerticesUP
std::atomic<uint64_t> g_bv_unpaired{0};     // End with no live Begin
std::atomic<uint64_t> g_bv_reserve_failed{0};  // Begin returned 0
std::atomic<uint64_t> g_bv_unreadable{0};

}  // namespace

//-----------------------------------------------------------------------------
// 0x825556C8 / 0x825556B8 -- D3DDevice_BeginVertices / EndVertices
//
// The FOURTH draw path, and the one that made the intro logo invisible.
//
// BeginVertices reserves command-ring space, EMITS THE PM4 DRAW PACKET ITSELF,
// and returns a guest pointer for the caller to write inline vertices into;
// EndVertices closes the reservation. The draw packet is plain in the
// decompilation:
//
//     v25[5] = primType & 0x3F | (vertexCount << 16) | 0x80;
//
// so nothing on this path passes through the three entry points this file
// otherwise hooks. The engine's UI draws exclusively this way, which is why
// every stage of the UI submit measured as PASSING while GuestDrawCalls never
// moved.
//
// TWO THINGS THIS MUST NOT DO:
//
//   * Double count. DrawVerticesUP reserves through this same function.
//     t_inDrawVerticesUP suppresses those, and they are COUNTED as suppressed
//     -- if that number is ever zero on a run with UP draws, the guard is not
//     doing what this claims.
//   * Read the vertices too early. BeginVertices returns a pointer to UNWRITTEN
//     ring space; the bytes exist first at EndVertices.
//-----------------------------------------------------------------------------
REX_IMPORT(__imp__sub_825556C8, orig_BeginVertices, void());
extern "C" REX_FUNC(sub_825556C8) {
  const bool nested = t_inDrawVerticesUP != 0;
  // Only the outermost reservation is a draw of its own; the UP wrapper counts
  // its own.
  if (!nested) g_guestDrawCalls.fetch_add(1, std::memory_order_relaxed);
  MX_D3D9_HLE_LOCK;

  const uint32_t device = ctx.r3.u32;
  const uint32_t prim_type = ctx.r4.u32;
  const uint32_t count = ctx.r5.u32;
  const uint32_t stride = ctx.r6.u32;

  if (!nested) {
    ++mx::hle::D3D9DrawCounter();
    NoteDrawDeclaration(device, base);
    if (REXCVAR_GET(hle_diag)) {
      DeviceState().NoteDevice(device, mx::hle::kEpDraw);
      SampleFetchConstantFile(device, base);
    }
  }

  orig_BeginVertices(ctx, base);

  if (nested) {
    g_bv_suppressed.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  // r3 on return is the write pointer, or 0 when the reservation failed (the
  // guest takes an explicit `return 0` path when it cannot get ring space).
  const uint32_t data = ctx.r3.u32;
  if (!data) {
    g_bv_reserve_failed.fetch_add(1, std::memory_order_relaxed);
    t_pendingVertices = PendingVertices{};
    return;
  }
  t_pendingVertices =
      PendingVertices{device, prim_type, count, stride, data, true};
}

REX_IMPORT(__imp__sub_825556B8, orig_EndVertices, void());
extern "C" REX_FUNC(sub_825556B8) {
  MX_D3D9_HLE_LOCK;
  // Consume unconditionally: a reservation must never outlive its End, or the
  // next unrelated End on this thread would build a draw from stale bounds.
  const PendingVertices pv = t_pendingVertices;
  t_pendingVertices = PendingVertices{};

  orig_EndVertices(ctx, base);

  if (t_inDrawVerticesUP) return;
  if (!pv.active) {
    g_bv_unpaired.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  // Built AFTER the original, like the other three: the guest's own call
  // performs D3D9's lazy vertex-shader patching, so a shader's first draw can
  // use the code captured during it.
  const uint64_t bytes = uint64_t(pv.count) * pv.stride;
  if (pv.stride && pv.count && bytes <= 16u * 1024u * 1024u &&
      HostPageReadable(REX_RAW_ADDR(pv.data)) &&
      HostPageReadable(REX_RAW_ADDR(pv.data + bytes - 1))) {
    const uint64_t n = g_bv_draws.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= kMaxDrawsLogged) {
      auto& f = DeclFile();
      f << "BeginEndVertices #" << n << " dev=0x" << std::hex << pv.device
        << std::dec << " prim=" << pv.prim_type
        << " vertex_count=" << pv.count << " data=0x" << std::hex << pv.data
        << std::dec << " stride=" << pv.stride << "\n";
      f.flush();
    }
    UpVertexData up{pv.data, pv.stride, uint32_t(bytes)};
    BuildAndQueueDraw(/*indexed=*/false, pv.prim_type, /*first=*/0, pv.count,
                      0, pv.device, base, &up);
  } else {
    const uint64_t bad =
        g_bv_unreadable.fetch_add(1, std::memory_order_relaxed) + 1;
    if (bad <= 8) {
      REXLOG_INFO("d3d9: BeginEndVertices skipped - data=0x{:08X} count={} "
                  "stride={} not readable",
                  pv.data, pv.count, pv.stride);
    }
  }
  ReportDrawCounts(base);
}

//-----------------------------------------------------------------------------
// 0x82555B88 -- D3DDevice_DrawVerticesUP(D3DDevice*, D3DPRIMITIVETYPE,
//                  UINT VertexCount, const void* pVertexStreamZeroData,
//                  UINT VertexStreamZeroStride)
//
// The third draw entry point. It does not call either of the other two: it
// reserves ring space via sub_825556C8, memcpys VertexCount*Stride bytes of
// inline vertex data into it, and returns, so no bound stream describes its
// geometry. About 30 engine functions draw through it -- UI, particles, and the
// Bink frame composite, which is what made its absence visible.
//
// The data pointer is frequently a caller stack local (it is in the Bink case),
// so the bytes are read here, inside the call, while that frame is still live.
//-----------------------------------------------------------------------------
//=============================================================================
// 0x8255E9A0 - begin recording into a command buffer, and 0x825601B8 - end.
//
// NOT speculative hooks. sub_823F82D0 names both by hand around its single call
// to the tree draw:
//
//     sub_8255E9A0(dev, cmdbuf, 16, &state, 0, 0, 0);   // r3 dev, r4 cmdbuf
//     ... SetRenderTarget / SetDepthStencilSurface / sub_823F6960 ...
//     sub_825601B8(dev);                                // r3 dev
//
// This pair is the ONLY thing that says which draws belong to which command
// buffer: the recording device is created per recording and destroyed straight
// after.
//=============================================================================
REX_IMPORT(__imp__sub_8255E9A0, orig_BeginCommandBuffer, void());
extern "C" REX_FUNC(sub_8255E9A0) {
  MX_D3D9_HLE_LOCK;
  mx::hooks::d3d9::BeginCmdBufRecording(ctx.r3.u32, ctx.r4.u32);
  orig_BeginCommandBuffer(ctx, base);
}

REX_IMPORT(__imp__sub_825601B8, orig_EndCommandBuffer, void());
extern "C" REX_FUNC(sub_825601B8) {
  MX_D3D9_HLE_LOCK;
  const uint32_t device = ctx.r3.u32;
  orig_EndCommandBuffer(ctx, base);
  // AFTER the original: a draw issued inside the End call still belongs to
  // this recording, and closing first would leak it into the live frame.
  mx::hooks::d3d9::EndCmdBufRecording(device);
}

REX_IMPORT(__imp__sub_825605D8, orig_ExecuteCommandBuffer, void());
extern "C" REX_FUNC(sub_825605D8) {
  MX_D3D9_HLE_LOCK;
  // r4 is the command buffer. Read before the original runs, which consumes
  // and rewrites parts of it.
  const uint32_t cmdbuf = ctx.r4.u32;
  const uint32_t replay_device = ctx.r3.u32;
  NoteCommandBufferExec(cmdbuf, base);
  orig_ExecuteCommandBuffer(ctx, base);
  // THE REPLAY. Every visible vegetation instance comes from here; the recording
  // itself renders nothing on the console. r3 is the real device, holding the
  // per-instance transform the guest wrote just above this call. After the
  // original, matching the two draw hooks. Overlays are walked per execution,
  // since the guest rewrites them between replays.
  std::vector<std::vector<mx::hooks::d3d9::CmdBufConstOverlay>> ov;
  mx::hooks::d3d9::CollectCmdBufConstants(cmdbuf, base, ov);
  mx::hooks::d3d9::ReplayCmdBuf(cmdbuf, replay_device, base, ov);
}

REX_IMPORT(__imp__sub_82555B88, orig_DrawVerticesUP, void());
extern "C" REX_FUNC(sub_82555B88) {
  // DrawVerticesUP reserves its ring space THROUGH BeginVertices, so the
  // hook below would build a second DrawCall for every UP draw. RAII, not a
  // plain ++/--, so the depth unwinds on every exit.
  const UpDepthGuard up_depth_guard;
  g_guestDrawCalls.fetch_add(1, std::memory_order_relaxed);
  MX_D3D9_HLE_LOCK;
  const uint32_t device = ctx.r3.u32;
  const uint32_t primitive_type = ctx.r4.u32;
  const uint32_t vertex_count = ctx.r5.u32;
  const uint32_t vertex_data = ctx.r6.u32;
  const uint32_t stride = ctx.r7.u32;
  const uint64_t n = ++g_up_draws;
  // Read BEFORE the original runs. lr survives the call in this recompiler,
  // but reading it here keeps it next to the arguments it belongs with, and
  // costs nothing.
  NoteUpDrawCaller(static_cast<uint32_t>(ctx.lr), vertex_count, 2);
  ++mx::hle::D3D9DrawCounter();
  NoteDrawDeclaration(device, base);
  if (n <= kMaxDrawsLogged) {
    auto& f = DeclFile();
    f << "DrawVerticesUP #" << n << " dev=0x" << std::hex << device << std::dec
      << " prim=" << primitive_type << " vertex_count=" << vertex_count
      << " data=0x" << std::hex << vertex_data << std::dec
      << " stride=" << stride << "\n";
    f.flush();
  }
  if (REXCVAR_GET(hle_diag)) {
    DeviceState().NoteDevice(device, mx::hle::kEpDraw);
    SampleFetchConstantFile(device, base);
  }
  orig_DrawVerticesUP(ctx, base);
  // Same ordering as the other two: the original call performs D3D9's lazy
  // vertex-shader patching, so translating after it returns lets a shader's
  // first draw use the code captured during this call.
  const uint64_t bytes = uint64_t(vertex_count) * stride;
  if (vertex_data && stride && vertex_count && bytes <= 16u * 1024u * 1024u &&
      HostPageReadable(REX_RAW_ADDR(vertex_data)) &&
      HostPageReadable(REX_RAW_ADDR(vertex_data + bytes - 1))) {
    UpVertexData up{vertex_data, stride, uint32_t(bytes)};
    BuildAndQueueDraw(/*indexed=*/false, primitive_type, /*first=*/0,
                      vertex_count, 0, device, base, &up);
  } else {
    static uint64_t s_unreadable = 0;
    if (++s_unreadable <= 8) {
      REXLOG_INFO("d3d9: DrawVerticesUP #{} skipped — data=0x{:08X} count={} "
                  "stride={} not readable",
                  n, vertex_data, vertex_count, stride);
    }
  }
  ReportDrawCounts(base);
}

//-----------------------------------------------------------------------------
// 0x82556110 - D3DDevice_DrawIndexedVerticesUP(
//                  D3DDevice*, D3DPRIMITIVETYPE, UINT MinVertexIndex,
//                  UINT NumVertices, UINT IndexCount, const void* pIndexData,
//                  D3DFORMAT IndexDataFormat, const void* pVertexStreamZeroData,
//                  UINT VertexStreamZeroStride)
//
// THE FOURTH DRAW ENTRY POINT, and the ONLY path Scaleform SHAPES use.
//
// How it was found: a caller census on the link register across all three
// previously-hooked entry points found 31 distinct sites in a menu run and
// exactly ONE of them in GFx -- inside DrawBitmaps. GFx's shape path,
// GRenderer::DrawIndexedTriList, draws through this function instead. That
// accounts for the missing panels, bar backgrounds, star widget and button
// glyphs (all shapes), for text surviving (glyphs are bitmaps, drawn through
// DrawVerticesUP), and for the vanishing menu text: the stencil MASK SHAPE never
// drew, so the plane kept the 0 BeginSubmitMask cleared it to and 7,465
// EQUAL-ref-1 draws per run were all rejected.
//
// SIGNATURE IS FROM THE DISASSEMBLY, not the decompiler (Hex-Rays gives this
// function 28 phantom int args) and not the PC D3D9 headers, which have no such
// entry point:
//
//     r3        device
//     r4        primitive type          (GFx passes 4 = TRIANGLELIST)
//     r5  -> r29  MinVertexIndex; also negated into the reserve call
//     r6  -> r28  NumVertices           (r28 * stride = vertex bytes memcpy'd)
//     r7  -> r27  IndexCount            (caller passes 3 * triangles)
//     r8  -> r25  INDEX data pointer
//     r9  -> r26  index format          (bit 2 set means 32-bit indices)
//     r10 -> r24  VERTEX data pointer
//     r1  + 0x54  vertex STRIDE         (9th arg, after a 0xB0 stwu)
//
// The index WIDTH rule is the callee's own: `(r9 & 4) ? 4 : 2`. sub_82555BD0,
// the reserve helper, applies the identical test and sizes vertices as
// `r6 * r9`, independently confirming the count and stride mapping.
//
// INDICES ARE ABSOLUTE. The guest uploads vertices starting at
// `pVertexStreamZeroData + MinVertexIndex * stride` and passes -MinVertexIndex
// as the base-vertex bias, so index values address the caller's FULL array.
// Stream 0 is therefore synthesised at the array base with the bias left at zero.
//
// The UpDepthGuard is applied even though sub_82555BD0 reserves ring space
// itself: it costs nothing when nothing nests, and if that reserve ever does
// reach BeginVertices the guard is what stops this draw being built twice.
//-----------------------------------------------------------------------------
REX_IMPORT(__imp__sub_82556110, orig_DrawIndexedVerticesUP, void());
extern "C" REX_FUNC(sub_82556110) {
  const UpDepthGuard up_depth_guard;
  g_guestDrawCalls.fetch_add(1, std::memory_order_relaxed);
  MX_D3D9_HLE_LOCK;

  // Every argument is read BEFORE the original runs: the callee clobbers the
  // volatiles, and both data pointers are frequently caller stack locals --
  // the same reason the DrawVerticesUP hook reads its bytes inside the call.
  const uint32_t device = ctx.r3.u32;
  const uint32_t primitive_type = ctx.r4.u32;
  const uint32_t min_vertex = ctx.r5.u32;
  const uint32_t vertex_count = ctx.r6.u32;
  const uint32_t index_count = ctx.r7.u32;
  const uint32_t index_data = ctx.r8.u32;
  const uint32_t index_format = ctx.r9.u32;
  const uint32_t vertex_data = ctx.r10.u32;
  const uint32_t sp = ctx.r1.u32;
  const uint32_t stride =
      (sp && HostPageReadable(REX_RAW_ADDR(sp + 0x54))) ? REX_LOAD_U32(sp + 0x54)
                                                        : 0;
  const bool index_32 = (index_format & 4u) != 0;
  const uint32_t index_width = index_32 ? 4u : 2u;

  const uint64_t n = ++g_indexed_up_draws;
  NoteUpDrawCaller(static_cast<uint32_t>(ctx.lr), vertex_count, 3);
  ++mx::hle::D3D9DrawCounter();
  NoteDrawDeclaration(device, base);
  if (REXCVAR_GET(hle_diag)) {
    DeviceState().NoteDevice(device, mx::hle::kEpDraw);
    SampleFetchConstantFile(device, base);
  }

  orig_DrawIndexedVerticesUP(ctx, base);

  // Same ordering as the other three: the original performs D3D9's lazy
  // vertex-shader patching, so translating after it returns lets a shader's
  // first draw use the code captured during this call. Vertices are needed up to
  // MinVertexIndex + NumVertices, because the indices are absolute.
  const uint64_t vtx_bytes =
      (uint64_t(min_vertex) + uint64_t(vertex_count)) * uint64_t(stride);
  const uint64_t idx_bytes = uint64_t(index_count) * uint64_t(index_width);
  constexpr uint64_t kSaneBytes = 16ull * 1024ull * 1024ull;

  // The stride comes off the STACK, which is the one argument that can be
  // wrong-but-plausible in a way a register cannot. Bounded hard, and a failure
  // is counted and skipped rather than turned into a draw over garbage.
  const bool sane = device && stride && stride <= 256u && vertex_count &&
                    index_count && vtx_bytes && vtx_bytes <= kSaneBytes &&
                    idx_bytes <= kSaneBytes;
  const bool readable =
      sane && vertex_data && index_data &&
      HostPageReadable(REX_RAW_ADDR(vertex_data)) &&
      HostPageReadable(REX_RAW_ADDR(vertex_data + vtx_bytes - 1)) &&
      HostPageReadable(REX_RAW_ADDR(index_data)) &&
      HostPageReadable(REX_RAW_ADDR(index_data + idx_bytes - 1));

  if (readable) {
    // The inline indices are synthesised into the BOUND INDEX STATE for the
    // duration of the build, exactly as UpVertexData is synthesised into stream
    // 0, so this reuses BuildHleDraw whole. DeviceState is thread-local, so no
    // other thread can observe the swap.
    auto& st = DeviceState();
    const auto saved_index = st.index;
    st.index.seen = true;
    st.index.bound = true;
    st.index.buffer_obj = 0;
    st.index.common = 0;
    st.index.address = index_data;
    st.index.size_bytes = static_cast<uint32_t>(idx_bytes);
    st.index.is_32bit = index_32;

    UpVertexData up{vertex_data, stride, static_cast<uint32_t>(vtx_bytes)};
    BuildAndQueueDraw(/*indexed=*/true, primitive_type, /*first=*/0,
                      index_count, /*base_vertex=*/0, device, base, &up);

    st.index = saved_index;
  } else {
    // Counted, with the values, because a silent skip here reproduces the very
    // defect this hook exists to fix.
    const uint64_t bad = ++g_indexed_up_skipped;
    if (bad <= 8 || (bad % 1000) == 0) {
      REXLOG_INFO(
          "d3d9: DrawIndexedVerticesUP #{} SKIPPED ({}) - vtx=0x{:08X} min={} "
          "count={} stride={} idx=0x{:08X} n={} w={}",
          n, sane ? "unreadable" : "implausible args", vertex_data, min_vertex,
          vertex_count, stride, index_data, index_count, index_width);
    }
  }
  ReportDrawCounts(base);
}

//=============================================================================
// The state entry points.
//
// All pass-through, all recording only, all behind hle_diag except that the
// recording itself is unconditional -- a shadow that only starts filling when
// the cvar is read would be missing everything set before the first draw.
//
// **No guest pointer is dereferenced speculatively.** Where a resource object is
// read (SetStreamSource, SetIndices) it is read here, in the same call where
// D3D9 reads the same fields itself. Reading it later at draw time crashed an
// earlier round: the game can free a buffer without rebinding.
//
// Signatures come from the typed decompilation in assets/default.xex.probe.i64,
// not from the PC D3D9 headers -- several differ.
//=============================================================================

//-----------------------------------------------------------------------------
// 0x8254B7C0 -- D3DDevice_SetStreamSource(D3DDevice*, UINT StreamNumber,
//                  D3DVertexBuffer*, UINT OffsetInBytes, UINT Stride)
//
// D3DVertexBuffer is D3DResource (24 bytes) followed by its two-dword vertex
// fetch constant at +0x18: dword[0] is the base address with flags in the top
// bits, dword[1] the size. SetStreamSource's own first act is to mask that dword
// with 0x1FFFFFFF and write it into the device's fetch constant file.
//-----------------------------------------------------------------------------
// NoteVegetationStride is defined with the LOD census below.
void NoteVegetationStride(uint32_t lr, uint32_t stride);
void NoteStreamStride(uint32_t lr, uint32_t stride);

REX_IMPORT(__imp__sub_8254B7C0, orig_SetStreamSource, void());
extern "C" REX_FUNC(sub_8254B7C0) {
  MX_D3D9_HLE_LOCK;
  const uint32_t stream = ctx.r4.u32;
  const uint32_t buffer = ctx.r5.u32;
  const uint32_t offset = ctx.r6.u32;
  const uint32_t stride = ctx.r7.u32;
  // Same return-address filter as the LOD census: which geometry kind the
  // SpeedTree renderers actually bind.
  NoteVegetationStride(static_cast<uint32_t>(ctx.lr), stride);
  NoteStreamStride(static_cast<uint32_t>(ctx.lr), stride);

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
      // The two dwords are a Xenos vertex fetch constant: dword0 is
      // {type[1:0], address[31:2]} and dword1 is {endian[1:0], size[25:2] in
      // dwords}. NOT 0x1FFFFFFF, copied out of SetStreamSource: that mask is
      // right for what the runtime writes into its fetch constant file, but it
      // leaves the two type bits in the address.
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
// 0x8254B8E0 -- D3DDevice_SetIndices(D3DDevice*, D3DIndexBuffer*)
//
// One argument; the 360 API has no BaseVertexIndex here. D3DIndexBuffer is
// D3DResource plus Address at +0x18 and Size at +0x1C.
//
// **The index width is bit 31 of Common (+0x00), not a separate field.**
// DrawIndexedVertices branches on `if (*pIndexBuffer < 0)` -- a signed test of
// that dword -- and multiplies StartIndex by 4 on that side against 2 on the
// other.
//-----------------------------------------------------------------------------
// VEGETATION LOD, read off SetIndices instead of instrumenting the guest.
//
// sub_823F1168 and sub_823F2728 are the SpeedTree renderers. The LOD is chosen a
// few instructions before their SetIndices:
//
//     823F22C8  fctiwz f2, f3               -> int
//     823F22D0  lwz    r10, ...             THE LOD INDEX
//     823F22D4  subfic r9, r10, 7           7 - lod
//     823F22DC  lwzx   r4, r8, r29          index buffer table[7 - lod]
//     823F22E0  bl     D3DDevice_SetIndices
//
// So `7 - lod` selects one of EIGHT index buffers and the pointer handed to
// SetIndices IS the LOD. The filter is the RETURN ADDRESS, as a RANGE rather
// than two exact call sites: a range cannot go stale against a call site moving
// by a few instructions, and it catches the sibling function too.
//-----------------------------------------------------------------------------
constexpr uint32_t kVegRendererLo = 0x823F1168;   // sub_823F1168
constexpr uint32_t kVegRendererHi = 0x823F3D0C;   // end of sub_823F2728

constexpr uint32_t kMaxVegLods = 16;
uint32_t g_vegLodBuf[kMaxVegLods] = {};
uint64_t g_vegLodHits[kMaxVegLods] = {};
uint32_t g_vegLodDistinct = 0;
uint64_t g_vegLodOverflow = 0;
uint64_t g_vegSetIndices = 0;
std::mutex g_vegLodMu;

// WHICH GEOMETRY KIND the vegetation renderers bind, by vertex stride.
//
// The .tree decoder gives the shapes: branch is stride 36 triangle STRIP, frond
// and 3dleaf stride 36 list, leaf cards stride 32 with no index buffer. The
// guest repacks, and the path measured so far binds stride 28.
//
// The LOD question is settled: 12 distinct index buffers, 88% at one LOD, which
// is a scene whose vegetation is mostly distant, NOT a pinned selector. So the
// leaf cards are fine and the missing geometry is the other kind.
uint32_t g_vegStride[8] = {};
uint64_t g_vegStrideHits[8] = {};
uint32_t g_vegStrideDistinct = 0;
uint64_t g_vegStrideOverflow = 0;

// EVERY stride bound, and who binds it -- not just the SpeedTree ones.
//
// The SpeedTree module provably has no code path that binds stride 36 (six
// stride sites in the whole module, all 28 or 4), yet a stride-36 stream DOES
// get bound, and the asset stores branch, frond and 3dleaf at stride 36. The
// stream reports that showed it are throttled, so this counts every bind and
// records the distinct CALLERS per stride.
constexpr uint32_t kMaxStrides = 12;
constexpr uint32_t kMaxStrideCallers = 6;
uint32_t g_strideVal[kMaxStrides] = {};
uint64_t g_strideHits[kMaxStrides] = {};
uint32_t g_strideCaller[kMaxStrides][kMaxStrideCallers] = {};
uint32_t g_strideCallerN[kMaxStrides] = {};
uint32_t g_strideDistinct = 0;
uint64_t g_strideOverflow = 0;


void NoteStreamStride(uint32_t lr, uint32_t stride) {
  if (!stride) return;
  std::lock_guard<std::mutex> lk(g_vegLodMu);
  uint32_t i = 0;
  for (; i < g_strideDistinct; ++i)
    if (g_strideVal[i] == stride) break;
  if (i == g_strideDistinct) {
    if (g_strideDistinct >= kMaxStrides) {
      ++g_strideOverflow;
      return;
    }
    i = g_strideDistinct++;
    g_strideVal[i] = stride;
  }
  ++g_strideHits[i];
  for (uint32_t c = 0; c < g_strideCallerN[i]; ++c)
    if (g_strideCaller[i][c] == lr) return;
  if (g_strideCallerN[i] < kMaxStrideCallers)
    g_strideCaller[i][g_strideCallerN[i]++] = lr;
}

void NoteVegetationStride(uint32_t lr, uint32_t stride) {
  if (lr < kVegRendererLo || lr >= kVegRendererHi || !stride) return;
  std::lock_guard<std::mutex> lk(g_vegLodMu);
  for (uint32_t i = 0; i < g_vegStrideDistinct; ++i) {
    if (g_vegStride[i] == stride) {
      ++g_vegStrideHits[i];
      return;
    }
  }
  if (g_vegStrideDistinct >= 8) {
    ++g_vegStrideOverflow;
    return;
  }
  const uint32_t i = g_vegStrideDistinct++;
  g_vegStride[i] = stride;
  g_vegStrideHits[i] = 1;
}

void NoteVegetationLod(uint32_t lr, uint32_t buffer) {
  if (lr < kVegRendererLo || lr >= kVegRendererHi || !buffer) return;
  std::lock_guard<std::mutex> lk(g_vegLodMu);
  ++g_vegSetIndices;
  for (uint32_t i = 0; i < g_vegLodDistinct; ++i) {
    if (g_vegLodBuf[i] == buffer) {
      ++g_vegLodHits[i];
      return;
    }
  }
  if (g_vegLodDistinct >= kMaxVegLods) {
    ++g_vegLodOverflow;
    return;
  }
  const uint32_t i = g_vegLodDistinct++;
  g_vegLodBuf[i] = buffer;
  g_vegLodHits[i] = 1;
}

void mx::hooks::d3d9::ReportVegetationLod() {
  std::lock_guard<std::mutex> lk(g_vegLodMu);
  std::string rows;
  for (uint32_t i = 0; i < g_vegLodDistinct; ++i)
    rows += fmt::format(" [0x{:08X} x{}]", g_vegLodBuf[i], g_vegLodHits[i]);
  // Printed at zero too: "the vegetation renderers never ran" and "this
  // report is not wired" are different findings.
  std::string strides;
  for (uint32_t i = 0; i < g_vegStrideDistinct; ++i)
    strides += fmt::format(" {}Bx{}", g_vegStride[i], g_vegStrideHits[i]);
  std::string all;
  for (uint32_t i = 0; i < g_strideDistinct; ++i) {
    all += fmt::format(" {}Bx{}<-", g_strideVal[i], g_strideHits[i]);
    for (uint32_t c = 0; c < g_strideCallerN[i]; ++c)
      all += fmt::format("{}0x{:08X}", c ? "," : "", g_strideCaller[i][c]);
  }
  REXLOG_INFO("d3d9: STREAM STRIDES -- every bind, with up to {} distinct "
              "callers each:{}{}",
              kMaxStrideCallers, all.empty() ? " none" : all,
              g_strideOverflow ? fmt::format(" (+{} past the cap)",
                                             g_strideOverflow)
                               : "");
  REXLOG_INFO("d3d9: VEG GEOMETRY -- stride(s) bound by the SpeedTree "
              "renderers:{}{}. branch/frond/3dleaf are stride 36 in the "
              "asset; leaf cards are the stride-28 quads",
              strides.empty() ? " none" : strides,
              g_vegStrideOverflow
                  ? fmt::format(" (+{} past the cap)", g_vegStrideOverflow)
                  : "");
  REXLOG_INFO("d3d9: VEG LOD -- {} SetIndices from the SpeedTree renderers, "
              "{} DISTINCT index buffer(s){}{}. One distinct = the LOD is "
              "PINNED; several = LOD selection works and the gap is "
              "elsewhere",
              g_vegSetIndices, g_vegLodDistinct,
              rows.empty() ? " none" : rows,
              g_vegLodOverflow
                  ? fmt::format(" (+{} past the {} cap)", g_vegLodOverflow,
                                kMaxVegLods)
                  : "");
}

REX_IMPORT(__imp__sub_8254B8E0, orig_SetIndices, void());
extern "C" REX_FUNC(sub_8254B8E0) {
  MX_D3D9_HLE_LOCK;
  const uint32_t buffer = ctx.r4.u32;
  // Read the caller BEFORE the original runs, for the same reason the draw
  // hooks do: it belongs with the arguments it identifies.
  NoteVegetationLod(static_cast<uint32_t>(ctx.lr), buffer);
  auto& st = DeviceState();
  st.NoteDevice(ctx.r3.u32, mx::hle::kEpSetIndices);
  auto& ib = st.index;
  ib.seen = true;
  ib.buffer_obj = buffer;
  ib.bound = buffer != 0;
  if (buffer) {
    ib.common = REX_LOAD_U32(buffer + 0x00);
    // **Not masked with 0x1FFFFFFF.** D3D9 applies that mask itself because the
    // GPU needs a *physical* address -- but every read on this side goes through
    // the guest's *virtual* space, where the buffer lives at the unmasked
    // address. Masking relocated it: an index buffer at 0xF3B64000 was recorded
    // as 0x13B64000, and reading there faulted in three separate runs. The
    // vertex path uses `& ~3`, keeping the high bits and clearing only the fetch
    // constant's two type bits.
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
// 0x825508A8 / 0x825506E8 -- SetVertexShader / SetPixelShader(D3DDevice*, ptr)
//
// Where a bound shader's microcode lives, read out of the accessors in gpu.obj
// that reach it:
//
//   Promote(D3DVertexShader*)   = blr           ; the handle IS the CVertexShader
//   SH_pPhysical(this)          = *(this + 0x20)
//   GetUCodeHeader()            = this + 0x368
//   GetUCode(i)                 = this + *(this + (i + 0x70)*8) + 0x368
//   GetPhysicalMicrocode(i)     = *(variant + 0x368) + *(this + 0x20)
//   GetPhysicalMicrocodeSize(i) = *(variant + 0x36C)
//
// So the object carries a table of *patched* microcode variants and the bytes
// sit at a physical base plus an offset out of the header.
//
// **The physical base is not dereferenced here.** `SH_pPhysical` is exactly the
// kind of address that cost four access violations: D3D9 keeps it masked for the
// GPU while every read on this side goes through virtual space.
//-----------------------------------------------------------------------------
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

  // The field's top bits are set (0xFD62A000), the *unmasked* form -- the same
  // shape the vertex buffer's address has before D3D9 masks it. So this should
  // be readable as-is, which is exactly what four access violations came from
  // getting wrong. Page-guarded, and 16 dwords only.
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
  // The physical base reads as sixteen zero dwords -- safe, and empty, so the
  // code is not behind that pointer at bind time. CreateVertexShader copies the
  // token stream to `this + 0x368`, and +0x36C here is 0x200, so the object very
  // likely carries the microcode inline.
  if (HostPageReadable(REX_RAW_ADDR(handle + 0x468))) {
    f << "    inline +0x368:";
    for (uint32_t o = 0x368; o < 0x3E8; o += 4) f << " " << std::hex
                                                  << REX_LOAD_U32(handle + o);
    f << std::dec << "\n";
  }
  f.flush();
}

//-----------------------------------------------------------------------------
// Does SetVertexShader publish literal constants, and is c255 among them?
//
// From the disassembly of D3DDevice_SetVertexShader, which walks a patch block
// carried by the shader object:
//
//   H = pShader + 0x368                       (the header)
//   P = H + *(u32*)(H + 0x14)                 (the patch block)
//   P + 0x10  u32   byte length of the entry list
//   P + 0x14        first entry
//
//   entry: u16 byte_offset, u16 dword_count, then dword_count dwords of data.
//   memcpy(device + 0x480 + byte_offset, entry_payload, dword_count * 4)
//
// So offsets are relative to the whole constants block, not to the VS float
// file: VS constant i sits at 0x780 - 0x480 + i*16, making c255 offset 0x12F0.
//
// Read either side of the original call on purpose. We call through to the
// guest's own D3D9, so if this list is what fills c255 the value must CHANGE
// across that call. The list alone would only show intent.
//-----------------------------------------------------------------------------

// Walk the shader's embedded constant-load table and report what it publishes.
//
// sub_825656A0, called from the draw-time flush as
// sub_825656A0(device, vs + 0x368, *(vs + 0x20)), walks the list at P + 0x14 and
// emits per entry a PM4 Type-3 packet with header 0xC0022F00 -- LOAD_ALU_CONSTANT
// -- whose body is [source_address, 4 * reg_index, dword_count].
//
//   entry: u16 reg_index, u16 dword_count, u32 data_offset   (8 bytes)
//   terminated by dword_count == 0
//   source = *(vs + 0x20) + data_offset
//
// These are the entries SetVertexShader's FIRST loop steps over without acting
// on; its memcpy loop is a different, later list in the same block. Walking the
// first list with the second's layout reads reg_index 0xFC as a byte offset and
// concludes nothing publishes c255 -- 0xFC is c252, and the count of 16 dwords
// covers c252..c255.
//
// So this data never passes through device + 0x780: it is literal constant data
// carried inside the shader's own code allocation and handed to the GPU by
// address, which is why the device shadow shows zeros.
void ProbeVertexShaderConstantPatch(uint32_t shader, uint32_t device,
                                    const uint32_t before[4], uint8_t* base) {
  (void)base;
  (void)device;
  (void)before;
  static std::map<uint32_t, bool> s_seen;
  if (s_seen.size() >= 24 || !s_seen.emplace(shader, true).second) return;

  const uint32_t h = shader + kVsPatchHeaderAt;
  if (!HostPageReadable(REX_RAW_ADDR(h + kVsPatchOffsetAt)) ||
      !HostPageReadable(REX_RAW_ADDR(shader + kVsCodeAllocAt)))
    return;
  const uint32_t rel = REX_LOAD_U32(h + kVsPatchOffsetAt);
  if (!rel || rel >= 0x10000) return;
  const uint32_t pblk = h + rel;
  if (!HostPageReadable(REX_RAW_ADDR(pblk + 0x10))) return;
  const uint32_t code_alloc = REX_LOAD_U32(shader + kVsCodeAllocAt);
  const uint32_t bytes = REX_LOAD_U32(pblk + 0x10);
  if (bytes >= 0x10000) return;

  std::string entries;
  std::string c255;
  uint32_t at = pblk + 0x14;
  const uint32_t end = at + bytes;
  uint32_t n = 0;
  while (at + 8 <= end && HostPageReadable(REX_RAW_ADDR(at))) {
    const uint32_t hdr = REX_LOAD_U32(at);
    const uint32_t reg = hdr >> 16, dwords = hdr & 0xFFFF;
    if (!dwords) break;
    const uint32_t data_off = REX_LOAD_U32(at + 4);
    at += 8;
    if (++n <= 6)
      entries += fmt::format(" c{}..c{} @+0x{:X}", reg,
                             reg + dwords / 4 - 1, data_off);
    // The published value of c255 itself, read from where the packet points.
    if (reg <= 255 && 255 < reg + dwords / 4) {
      const uint32_t src = code_alloc + data_off + (255 - reg) * 16;
      if (HostPageReadable(REX_RAW_ADDR(src))) {
        float f[4];
        for (uint32_t i = 0; i < 4; ++i) {
          const uint32_t w = REX_LOAD_U32(src + i * 4);
          std::memcpy(&f[i], &w, 4);
        }
        c255 = fmt::format(" c255 = ({:.6g}, {:.6g}, {:.6g}, {:.6g}) @0x{:08X}",
                           f[0], f[1], f[2], f[3], src);
      }
    }
  }
  REXLOG_INFO("d3d9: vs 0x{:08X} const loads: {} entries{}{}", shader, n,
              entries, c255.empty() ? " — none covers c255" : c255);
}

REX_IMPORT(__imp__sub_825508A8, orig_SetVertexShader, void());
extern "C" REX_FUNC(sub_825508A8) {
  MX_D3D9_HLE_LOCK;
  auto& st = DeviceState();
  const uint32_t device = ctx.r3.u32;
  const uint32_t shader = ctx.r4.u32;
  st.NoteDevice(device, mx::hle::kEpSetVertexShader);
  st.vertex_shader = shader;
  st.vs_seen = true;
  const bool capture = REXCVAR_GET(hle_diag);
  if (capture) {
    DumpVertexShaderObject(shader, base);
  }
  orig_SetVertexShader(ctx, base);
  // Behind hle_diag like every other diagnostic here. No longer sampled
  // either side of the original call: that comparison tested whether
  // SetVertexShader itself writes c255 into the device shadow. It does not --
  // the value never goes through the shadow at all.
  if (capture && shader) {
    ProbeVertexShaderConstantPatch(shader, device, nullptr, base);
  }
}

REX_IMPORT(__imp__sub_825506E8, orig_SetPixelShader, void());
extern "C" REX_FUNC(sub_825506E8) {
  MX_D3D9_HLE_LOCK;
  auto& st = DeviceState();
  st.NoteDevice(ctx.r3.u32, mx::hle::kEpSetPixelShader);
  st.pixel_shader = ctx.r4.u32;
  st.ps_seen = true;
  // Kept across a SetPixelShader(NULL) on purpose -- see the field's note.
  if (ctx.r4.u32) st.last_nonnull_pixel_shader = ctx.r4.u32;
  // Also record it against the DEVICE, so draws submitted on other threads can
  // find it -- see NotePixelShaderForDevice.
  NotePixelShaderForDevice(ctx.r3.u32, ctx.r4.u32);
  CollectPixelShaderBlob(ctx.r4.u32, base);
  orig_SetPixelShader(ctx, base);
}

//-----------------------------------------------------------------------------
// 0x8254C060 / 0x8254C3B0 -- SetRenderTarget / SetDepthStencilSurface.
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

// Tell the renderer a surface EXISTS, whether or not a draw ever names it.
//
// Host storage used to be created on the first draw that targeted a surface,
// which loses any pass that binds and resolves without a draw we route -- the
// menu's shadow atlas is exactly that, so the backdrop shader's slot 15 found no
// snapshot and its draw was discarded whole.
//
// Deduped per frame on object AND extent, so a surface rebound at a new size
// still reaches the factory.
void NoteSurfaceBind(const mx::hle::RenderTargetBinding& rt, bool is_depth) {
  if (!rt.valid || !rt.object || !rt.width || !rt.height) return;
  const uint64_t key = (uint64_t(rt.object) << 32) |
                       (uint64_t(rt.width) << 17) | (rt.height << 1) |
                       (is_depth ? 1u : 0u);
  {
    static std::mutex s_mu;
    static uint64_t s_frame = UINT64_MAX;
    static std::set<uint64_t> s_seen;
    const uint64_t frame = mx::hle::D3D9FrameCount();
    std::lock_guard<std::mutex> lock(s_mu);
    if (frame != s_frame) {
      s_frame = frame;
      s_seen.clear();
    }
    if (!s_seen.insert(key).second) return;
  }
  mx::hle::DrawCall bind{};
  bind.surface_bind = true;
  bind.surface_bind_is_depth = is_depth;
  bind.surface_bind_object = rt.object;
  bind.surface_bind_width = rt.width;
  bind.surface_bind_height = rt.height;
  bind.surface_bind_base = rt.color_info & 0xFFFu;
  bind.surface_bind_color_format = (rt.color_info >> 16) & 0xFu;
  mx::hle::HleFrameDraws().push_back(std::move(bind));
}

REX_IMPORT(__imp__sub_8254C060, orig_SetRenderTarget, void());
extern "C" REX_FUNC(sub_8254C060) {
  MX_D3D9_HLE_LOCK;
  const uint32_t slot = ctx.r4.u32;
  const uint32_t object = ctx.r5.u32;
  if (slot < 4) {
    auto& st = DeviceState();
    st.NoteDevice(ctx.r3.u32, mx::hle::kEpSetRenderTarget);
    st.render_target[slot] = SnapshotRenderTarget(object, base);
    st.render_target_seen_mask |= 1u << slot;
    // Mirrored per DEVICE as well as per thread. A command-buffer replay runs on
    // whatever thread drives the render loop, and thread-local state leaves it
    // with no target at all, which silently filtered out every replayed palm
    // draw.
    if (slot == 0)
      mx::hooks::d3d9::NoteRenderTargetForDevice(ctx.r3.u32,
                                                 st.render_target[slot],
                                                 /*is_depth=*/false);

    const auto& rt = st.render_target[slot];
    // Slot 0 only. The renderer models one colour attachment (DrawCall's
    // render_target_object comes from render_target[0]), so instantiating slots
    // 1-3 would spend budget on surfaces nothing routes -- and would let a
    // resolve out of an MRT slot copy a freshly cleared target instead of
    // failing, trading a missing image for a confidently blank one.
    if (slot == 0) NoteSurfaceBind(rt, /*is_depth=*/false);
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
  MX_D3D9_HLE_LOCK;
  auto& st = DeviceState();
  st.NoteDevice(ctx.r3.u32, mx::hle::kEpSetDepthStencil);
  st.depth_stencil = SnapshotRenderTarget(ctx.r4.u32, base);
  // Per device too, for the same reason as the colour target above.
  mx::hooks::d3d9::NoteRenderTargetForDevice(ctx.r3.u32, st.depth_stencil,
                                             /*is_depth=*/true);
  // The depth half of the same story, and the one that motivated it: every
  // depth target we have ever created arrived paired with a colour target, so
  // a depth-only pass instantiated nothing at all.
  NoteSurfaceBind(st.depth_stencil, /*is_depth=*/true);
  // DIAG: the depth surfaces the guest binds, and their OWN extents. Sizing a
  // host depth surface to the colour target instead collapsed the frame, because
  // one depth object is bound alongside colour targets of several extents (the
  // 1280x720 surface and its 1280x640 and 1280x80 EDRAM bands). Whether the
  // guest declares one depth for all of them decides whether the host pool can
  // be keyed by object alone.
  {
    const auto& ds = st.depth_stencil;
    static std::mutex s_mu;
    static std::map<uint64_t, uint64_t> s_seen;
    if (ds.valid) {
      const uint64_t key = (uint64_t(ds.object) << 32) |
                           (uint64_t(ds.width) << 16) | ds.height;
      bool first = false;
      {
        std::lock_guard<std::mutex> lock(s_mu);
        first = s_seen.emplace(key, 0).second && s_seen.size() <= 24;
      }
      if (first) {
        REXLOG_INFO("d3d9: DEPTH surface object 0x{:08X} {}x{} "
                    "surface=0x{:08X} depthinfo=0x{:08X} base=0x{:03X} "
                    "pitch={} (colour target now 0x{:08X} {}x{})",
                    ds.object, ds.width, ds.height, ds.surface_info,
                    ds.color_info, ds.color_info & 0xFFFu,
                    ds.surface_info & 0x3FFFu, st.render_target[0].object,
                    st.render_target[0].width, st.render_target[0].height);
      }
    }
  }
  orig_SetDepthStencil(ctx, base);
}

//-----------------------------------------------------------------------------
// 0x8254E748 -- D3DDevice_SetTexture(D3DDevice*, DWORD Sampler,
//                                    D3DBaseTexture*)
//
// IDA shows the texture's six hardware-fetch dwords at object+0x1C..+0x30 and
// SetTexture copying them into device+0x480+sampler*24.
//-----------------------------------------------------------------------------
REX_IMPORT(__imp__sub_8254E748, orig_SetTexture, void());
extern "C" REX_FUNC(sub_8254E748) {
  MX_D3D9_HLE_LOCK;
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
    // Did the guest just ask for a surface it had previously resolved into?
    // This is the branch point for every "produced but never drawn" defect.
    //
    // D3D9DrawCounter() is bumped at the three Draw entry points before any of
    // our filtering, so this counts what the guest asked for, not what we built:
    //
    //   0 draws   the guest binds it and never draws with it. Our draw path is
    //             innocent and the real consumer is somewhere else.
    //   N draws   the guest DOES draw with it and we drop those draws before
    //             they reach the texture slot loop. Ours to fix.
    //
    // Closed out on the next SetTexture to the same sampler, the only moment the
    // window is known to have ended.
    {
      struct OrphanWatch {
        uint32_t object = 0;
        uint64_t draws_at_bind = 0;
      };
      static thread_local OrphanWatch t_watch[mx::hle::kMaxSamplers];
      // ACCUMULATED, not logged. A line per window under a global cap of 24
      // spent the whole cap in the first 0.3 seconds on one Bink-era object, so
      // the surface it was built to measure never got a line. A cap shared
      // across a population reports on whoever is loudest.
      OrphanWatch& w = t_watch[sampler];
      if (w.object && w.object != texture) {
        if (const auto po = g_resolveDestObjectPhys.find(w.object);
            po != g_resolveDestObjectPhys.end()) {
          if (const auto it = g_resolvedTargetsByAddress.find(po->second);
              it != g_resolvedTargetsByAddress.end()) {
            it->second.guest_draws_spanned +=
                mx::hle::D3D9DrawCounter() - w.draws_at_bind;
            ++it->second.bind_windows;
          }
        }
        w.object = 0;
      }
      if (texture && g_resolveDestObjectPhys.count(texture)) {
        w.object = texture;
        w.draws_at_bind = mx::hle::D3D9DrawCounter();
      }
    }
    // find() only, never operator[]: this runs on guest threads and the resolve
    // maps are unguarded, so it must not insert. Bumping a counter inside an
    // existing node cannot rehash or rebalance the map; creating one could.
    if (texture) {
      if (const auto po = g_resolveDestObjectPhys.find(texture);
          po != g_resolveDestObjectPhys.end()) {
        if (const auto it = g_resolvedTargetsByAddress.find(po->second);
            it != g_resolvedTargetsByAddress.end()) {
          ++it->second.set_texture_binds;
          it->second.bind_sampler_mask |= 1u << sampler;
          it->second.last_bind_device = device;
          it->second.last_bind_thread = GetCurrentThreadId();
        }
      }
    }
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

  // Is the texture the FE_Smoke quad's MATERIAL names ever sampled? Keyed by
  // base address and NOT restricted to resolve destinations. Takes a lock, but
  // only past a shape test that rejects every bind in the game bar a handful.
  //
  // Deliberately AFTER the device-fetch-file cross-check above, so it reads the
  // corrected constants. Placed before it, this would miss every bind that took
  // the fallback path -- by reading `valid == false`, which is
  // indistinguishable here from "not a texture".
  if (sampler < mx::hle::kMaxSamplers) {
    const auto& binding = DeviceState().texture[sampler];
    NoteVideoShapeBind(sampler, texture, binding.fetch, binding.valid, device);
  }
}

//-----------------------------------------------------------------------------
// 0x8254BF50 -- D3DDevice_SetViewport(D3DDevice*, const D3DVIEWPORT9*)
//
// Six dwords: X, Y, Width, Height as integers then MinZ, MaxZ as floats. The
// struct is read here because the function reads all six itself on the next
// instruction.
//-----------------------------------------------------------------------------
REX_IMPORT(__imp__sub_8254BF50, orig_SetViewport, void());
extern "C" REX_FUNC(sub_8254BF50) {
  MX_D3D9_HLE_LOCK;
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
    // last-write-wins, and the first Stage F run read 65535x65535 out of it,
    // which built a nonsense viewport inverse. Whether that is the only viewport
    // this title sets or merely the most recent one is the difference between a
    // wrong read and a wrong *model*.
    ++g_viewportExtents[(uint64_t(v.width) << 32) | v.height];
  }
  orig_SetViewport(ctx, base);
}

//-----------------------------------------------------------------------------
// 0x8254B678 — D3DDevice_SetScissorRect(D3DDevice*, const RECT*)
//-----------------------------------------------------------------------------
REX_IMPORT(__imp__sub_8254B678, orig_SetScissorRect, void());
extern "C" REX_FUNC(sub_8254B678) {
  MX_D3D9_HLE_LOCK;
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
// All (D3DDevice*, DWORD Value) -- confirmed on ZEnable's decompilation, and
// they are generated from one template so the rest follow.
//
// Only these eight were matched uniquely. The other ~90 leaves in state.obj are
// 20-56 bytes with no relocations and several are byte-identical to each other,
// so a byte match on them would not be an identification.
//
// BlendFactor has **zero call sites** in this title. It is hooked anyway so that
// "never called" stays a measured fact.
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

//=============================================================================
// 0x82AD0FC8 - HFTerrain: APPEND ONE TRACK SEGMENT. The deform producer.
//
// The consumer side (sub_82AD49A0, below) says the guest splats exactly ONCE per
// run -- 60 track points and 4 splat triangles on the first call, then zero for
// 300+ frames of riding. This is the other end of that, found by searching for
// stores to the point-count offset 0x22A8: every one is in this function, and it
// is reached only through a method table.
//
// It appends SIX vertices of five floats each, bumping the float count at +8872
// thirty times, then
//
//     *(int*)(obj + 4*(half + 296)) += 2;      // obj+1184 / obj+1188
//
// two triangles per call. So "60 points, 4 splat-tris" is exactly TWO calls.
//
// AND IT IS GATED, on a different array from the one it fills:
//
//     if (*(uint*)(obj + 520*half + 656) < 0x40u) { ...append... }
//
// a second list capped at 64. So "the guest stopped splatting" has two shapes
// the draw-side census cannot tell apart:
//
//     never called          -> the track system is not running; guest-side
//     called and REFUSED    -> the cap is full and nothing drains it, which is
//                              a state we might be perturbing
//
// This counts both, with the gate value. Reads only.
//=============================================================================
REX_IMPORT(__imp__sub_82AD0FC8, orig_TerrainTrackAppend, void());
extern "C" REX_FUNC(sub_82AD0FC8) {
  const uint32_t obj = ctx.r3.u32;
  uint32_t half = 0, gate = 0xFFFFFFFFu, points_before = 0, tris_before = 0;
  const bool readable = obj && HostPageReadable(REX_RAW_ADDR(obj + 124));
  if (readable) {
    half = REX_LOAD_U32(obj + 124) ? 1u : 0u;
    const uint32_t gate_at = obj + 520u * half + 656u;
    const uint32_t points_at = obj + 7684u * half + 8872u;
    const uint32_t tris_at = obj + 1184u + 4u * half;
    if (HostPageReadable(REX_RAW_ADDR(gate_at))) gate = REX_LOAD_U32(gate_at);
    if (HostPageReadable(REX_RAW_ADDR(points_at)))
      points_before = REX_LOAD_U32(points_at);
    if (HostPageReadable(REX_RAW_ADDR(tris_at)))
      tris_before = REX_LOAD_U32(tris_at);
  }

  orig_TerrainTrackAppend(ctx, base);

  static std::atomic<uint64_t> s_calls{0}, s_refused{0}, s_appended{0};
  static std::atomic<uint64_t> s_maxGate{0};
  const uint64_t calls = s_calls.fetch_add(1, std::memory_order_relaxed) + 1;
  bool appended = false;
  if (readable) {
    const uint32_t tris_at = obj + 1184u + 4u * half;
    if (HostPageReadable(REX_RAW_ADDR(tris_at)))
      appended = REX_LOAD_U32(tris_at) != tris_before;
    if (appended) s_appended.fetch_add(1, std::memory_order_relaxed);
    else s_refused.fetch_add(1, std::memory_order_relaxed);
    uint64_t seen = s_maxGate.load(std::memory_order_relaxed);
    while (gate != 0xFFFFFFFFu && gate > seen &&
           !s_maxGate.compare_exchange_weak(seen, gate)) {
    }
  }
  // Denominator first. "Refused" and "never called" are the whole question, so
  // the line has to print even when nothing is appended -- which is why it is
  // keyed on the CALL count and not on a successful append.
  if (calls <= 4 || (calls % 200) == 0) {
    REXLOG_INFO(
        "d3d9: TERRAIN TRACK APPEND obj 0x{:08X} -- {} calls, {} appended, {} "
        "REFUSED by the 64 cap; peak gate {} | this call: half {} gate {} "
        "points {} tris {}",
        obj, calls, s_appended.load(std::memory_order_relaxed),
        s_refused.load(std::memory_order_relaxed),
        s_maxGate.load(std::memory_order_relaxed), half,
        gate == 0xFFFFFFFFu ? -1 : int32_t(gate), points_before, tris_before);
  }
}

//=============================================================================
// 0x82AD49A0 - HFTerrain: the per-frame deformation render (tyre ruts).
//
// PURE MEASUREMENT, and it exists because the draw-side census cannot answer the
// question. `SQUARE TARGET` reports exactly ONE draw per frame into the 512x512
// deform surface and a capture shows that draw writing ZERO, which is consistent
// with two completely different causes:
//
//   the guest has no track segments to splat        -> guest-side, not ours
//   the guest HAS them and the splat draw is lost   -> ours
//
// The decompile separates them, because the splat is behind its own count and
// its own branch:
//
//     v39   = (obj[124] == 0)                 which ping-pong half is current
//     v40   = obj + 7684 * v39
//     if (*(u32*)(v40 + 8872))  { ... }       track POINTS -> tile list
//     ...
//     if (*(int*)(obj + 4 * (296 + v39)) > 0) {      = obj+1184 / obj+1188
//       SetVertexShader(obj+17076)  vs_hft_deform
//       SetPixelShader (obj+17080)  ps_hft_deform
//       sub_82555B88(dev, 4, 3 * that count, v40 + 1192, stride)
//     }
//
// `splat-tris` 0 on every call says the guest never asks and the deform buffer
// is CORRECTLY empty from our side; non-zero while the draw census still reports
// one draw a frame puts the loss on our path.
//
// `tiles` is printed beside them because it is the loop bound for the whole
// pass: obj+16944 carries the tile list forward between frames, so a non-zero
// tiles with a zero point count is the guest re-copying a tile it visited
// earlier -- exactly the one draw a frame we see.
//=============================================================================
REX_IMPORT(__imp__sub_82AD49A0, orig_TerrainDeformRender, void());
extern "C" REX_FUNC(sub_82AD49A0) {
  const uint32_t obj = ctx.r3.u32;
  const uint32_t extent = ctx.r4.u32;
  // obj+128 is the frame stamp the body writes on entry; if it does not change
  // across the call the body was skipped (already run this frame, or the global
  // enable is off) and this call carries no information about the counts.
  const bool stamp_readable =
      obj && HostPageReadable(REX_RAW_ADDR(obj + 128));
  const uint32_t stamp_before = stamp_readable ? REX_LOAD_U32(obj + 128) : 0;

  // READ THE COUNTS BEFORE THE BODY RUNS. They are INPUTS: the body tests `if
  // (splat count > 0)` and draws on that, and the producer's own log proves they
  // are reset every frame. Reading them AFTER returns whatever survived the
  // pass, and if the consumer is what clears them that is a guaranteed zero
  // whatever the body did.
  //
  // TILES stay read AFTER, and that is not an inconsistency: the body zeroes
  // obj+16944 and REBUILDS it, so the post-call value is the list it drew from.
  uint32_t half_before = 0, points_before = 0, splats_before = 0;
  if (obj && HostPageReadable(REX_RAW_ADDR(obj + 124))) {
    half_before = REX_LOAD_U32(obj + 124) == 0 ? 1u : 0u;
    const uint32_t blk = obj + (half_before ? 7684u : 0u);
    const uint32_t spl = obj + (half_before ? 1188u : 1184u);
    if (HostPageReadable(REX_RAW_ADDR(blk + 8872)))
      points_before = REX_LOAD_U32(blk + 8872);
    if (HostPageReadable(REX_RAW_ADDR(spl))) splats_before = REX_LOAD_U32(spl);
  }

  orig_TerrainDeformRender(ctx, base);

  if (!stamp_readable) return;
  if (REX_LOAD_U32(obj + 128) == stamp_before) return;

  // PER OBJECT. One run printed a single counter across TWO deformation objects
  // -- extent 32 for the first 1800+ calls and extent 2048 once the level came
  // up -- so "1 of 2100 calls had splats" was a ratio between a numerator from
  // one object and a denominator dominated by the other.
  //
  // Fixed table, claimed by CAS, no allocation and no lock -- this runs on guest
  // threads. Four is generous: two objects have ever been seen.
  struct DeformCensus {
    std::atomic<uint32_t> obj{0};
    std::atomic<uint64_t> ran{0}, withPoints{0}, withSplats{0};
    std::atomic<uint64_t> maxPoints{0}, maxSplats{0}, maxTiles{0};
  };
  static DeformCensus s_deform[4];
  DeformCensus* census = nullptr;
  for (auto& slot : s_deform) {
    uint32_t owner = slot.obj.load(std::memory_order_acquire);
    if (owner == obj) {
      census = &slot;
      break;
    }
    if (!owner && slot.obj.compare_exchange_strong(owner, obj)) {
      census = &slot;
      break;
    }
    if (owner == obj) {
      census = &slot;
      break;
    }
  }
  // More objects than the table holds: counted nowhere rather than counted
  // wrongly, and the line below simply never prints for them. Say so if it
  // ever happens instead of silently folding them into a neighbour.
  if (!census) {
    static std::atomic<bool> s_toldOverflow{false};
    if (!s_toldOverflow.exchange(true))
      REXLOG_INFO(
          "d3d9: TERRAIN DEFORM census full -- object 0x{:08X} is not counted",
          obj);
    return;
  }
  const uint64_t ran = census->ran.fetch_add(1, std::memory_order_relaxed) + 1;

  const bool half = half_before != 0;
  const uint32_t points = points_before;
  const uint32_t splats = splats_before;
  // Read AFTER the original, so it is the list the pass just drew from rather
  // than the one it inherited -- the body zeroes obj+16944 and rebuilds it.
  const uint32_t tiles =
      HostPageReadable(REX_RAW_ADDR(obj + 16944)) ? REX_LOAD_U32(obj + 16944)
                                                  : 0;
  // THE OTHER HALF, and it is the whole question now.
  //
  // The producer writes block `obj[124]`; this render reads block `!obj[124]` --
  // deliberately opposite, a double buffer. TERRAIN TRACK APPEND says the
  // producer reaches 60 points / 4 triangles every frame while this side reads 0
  // every frame, and both hooks are on the SAME object, so the data is not
  // missing: it is in a block nobody reads.
  //
  // THE TWO PLAIN ALLOCATIONS, obj+112 and obj+116. sub_82AF7240 creates them
  // with a plain allocator -- NOT a texture creation -- and memsets both to
  // 0x80, then calls D3D9's offset-the-resource-address helper on obj+8 and
  // obj+60, so the D3D9 textures there are BACKED BY those allocations. And
  // sub_82AC7850, the deform pass's resolve destination, returns one of exactly
  // those two textures.
  //
  // So the deform RESOLVE DESTINATION and a plain guest allocation the guest
  // memsets to 0x80 are the same bytes.
  const uint32_t buf0 = HostPageReadable(REX_RAW_ADDR(obj + 112))
                            ? REX_LOAD_U32(obj + 112)
                            : 0;
  const uint32_t buf1 = HostPageReadable(REX_RAW_ADDR(obj + 116))
                            ? REX_LOAD_U32(obj + 116)
                            : 0;
  const uint32_t other_block = obj + (half ? 0u : 7684u);
  const uint32_t other_splat = obj + (half ? 1184u : 1188u);
  const uint32_t other_points =
      HostPageReadable(REX_RAW_ADDR(other_block + 8872))
          ? REX_LOAD_U32(other_block + 8872)
          : 0;
  const uint32_t other_tris = HostPageReadable(REX_RAW_ADDR(other_splat))
                                  ? REX_LOAD_U32(other_splat)
                                  : 0;

  if (points) census->withPoints.fetch_add(1, std::memory_order_relaxed);
  if (int32_t(splats) > 0)
    census->withSplats.fetch_add(1, std::memory_order_relaxed);
  const auto bump = [](std::atomic<uint64_t>& hi, uint64_t v) {
    uint64_t seen = hi.load(std::memory_order_relaxed);
    while (v > seen && !hi.compare_exchange_weak(seen, v)) {
    }
  };
  bump(census->maxPoints, points);
  bump(census->maxSplats, splats);
  bump(census->maxTiles, tiles);

  // POPULATION *AND* FIRES. Printing at ran<=4 and every 300 sampled only the
  // first three seconds of a 46-second run, while the bike was STATIONARY and
  // the producer does not run -- so "1 with track points" was true of the window
  // it saw and said nothing about the one that matters. Every call that HAS
  // points or splats is printed (capped), the periodic sample is 60, and the
  // cumulative counters carry the denominator.
  static std::atomic<uint64_t> s_firePrints{0};
  const bool interesting = points || splats;
  const bool fire_budget =
      interesting && s_firePrints.fetch_add(1, std::memory_order_relaxed) < 40;
  if (ran <= 4 || (ran % 60) == 0 || fire_budget) {
    REXLOG_INFO(
        "d3d9: TERRAIN DEFORM obj 0x{:08X} extent {} -- ran {} times, {} with "
        "track points, {} with SPLAT triangles; peak points {} splat-tris {} "
        "tiles {} | this call: half {} points {} splat-tris {} tiles {} | OTHER "
        "half: points {} splat-tris {} | buffers 0x{:08X} 0x{:08X} sel {}",
        obj, extent, ran, census->withPoints.load(std::memory_order_relaxed),
        census->withSplats.load(std::memory_order_relaxed),
        census->maxPoints.load(std::memory_order_relaxed),
        census->maxSplats.load(std::memory_order_relaxed),
        census->maxTiles.load(std::memory_order_relaxed), half ? 1 : 0, points,
        int32_t(splats), tiles, other_points, int32_t(other_tris), buf0, buf1,
        HostPageReadable(REX_RAW_ADDR(obj + 120)) ? REX_LOAD_U32(obj + 120) : 0);
  }
}

#undef MX_RENDER_STATE_HOOK
