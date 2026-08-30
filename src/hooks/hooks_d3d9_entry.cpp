// D3D9 guest entry points.
//
// The 17 REX_FUNC hooks the recompiler routes the guest's D3D9 calls through,
// plus the eight render-state hooks and the handful of helpers that only they
// use. Split out of hooks_d3d9.cpp on 2026-08-12, where they sat at the end of
// a 9,000-line file behind 7,500 lines of the machinery they drive.
//
// These are extern "C" and so cannot live in the layer's namespace; they reach
// into it through hooks_d3d9_internal.h, which documents why this is the only
// boundary in that file worth cutting.
//
// Every hook calls its original exactly once and MX_D3D9_PLUGIN_PASSTHROUGH
// returns straight after it under the GPU plugin -- the per-draw bookkeeping
// alone cost plugin-mode MainLoop ~17.6/s -> ~0.37/s when these ran in both
// modes. Use that macro on anything added here.

#include "hooks/hook_common.h"

// For the small-destination writeback: the SAME tiled address function the
// decoder uses, run in the other direction. See the note at its call site.
#include <rex/graphics/pipeline/texture/util.h>
#include <bit>
namespace tu = rex::graphics::texture_util;

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
// After internal, per the ORDER MATTERS note at the top of this header:
// internal names mx::hle types it does not include itself.
#include "hooks/hooks_d3d9_shared.h"

REXCVAR_DECLARE(bool, hle_capture);
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
// NOT a diagnostic hook. It is kept, stripped of the counters it used to carry,
// because two things in it are LOAD-BEARING for texture invalidation:
//
//   g_glyphCacheGeneration  the atlas staleness key. Glyph atlases are rewritten
//                           IN PLACE, so GuestTextureFingerprint cannot see the
//                           change and this bump is their ONLY invalidation
//                           signal. TextureContentVersion returns it instead of
//                           a fingerprint for any texture IsGlyphCacheTexture
//                           recognises.
//   NoteGlyphCacheGeometry  what teaches IsGlyphCacheTexture which extents ARE
//                           atlases. Without it every kR8 texture either falls
//                           back to the fingerprint or gets invalidated
//                           alongside the atlas, depending on the gate.
//
// The rest of the glyph instrumentation was removed 2026-08-28 after the shape
// fix; this hook survived that pass only because the bump was noticed inside it.
// If it is ever removed, glyph atlases stop being invalidated and text goes
// stale whenever Scaleform repacks -- silently, with no error anywhere.
//
// RELEASE, and AFTER the original: the bump publishes the atlas bytes the
// original just wrote. A reader that acquire-loads the new generation is then
// guaranteed to see those bytes when it re-decodes. Relaxed would let the
// compiler sink the writes past the bump and hand a reader the new generation
// with the old pixels -- the exact stale atlas this mechanism exists to prevent.
//=============================================================================
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

// Every hook below takes the HLE lock for its whole body, including across the
// call to the guest original. The guest's three record workers drive their own
// devices and never take a lock that our hooks could be holding, so the only
// effect is that the workers queue through this layer one at a time — which is
// what the ~30 shared globals in this file require. See HleGlobalMutex.
//
// Passthrough returns BEFORE the lock is taken, so --d3d9_hooks_passthrough=1
// remains a genuine bypass of this layer and stays usable for clearing it of
// blame.
#define MX_D3D9_PLUGIN_PASSTHROUGH(orig)                                 \
  if (mx::native::g_plugin_mode || REXCVAR_GET(d3d9_hooks_passthrough)) { \
    orig(ctx, base);                                                     \
    return;                                                              \
  }                                                                      \
  std::lock_guard<std::recursive_mutex> _hle_lock(mx::hle::HleGlobalMutex())

//=============================================================================
// 0x82945D20 - DefineCompactedFont. THE LOADER THIS GAME ACTUALLY USES.
//
// This title does NOT load fonts through DefineFont, DefineFont2 or
// DefineFont3. Hooks on sub_82947418 and sub_82949E10 measured 0 loads across
// a whole process lifetime, and that counter is cumulative, so log rotation
// cannot explain it. Those two hooks have since been REMOVED -- they wrapped
// guest functions this game never calls -- and the measurement is recorded
// here so the experiment is not repeated. The game ships .gfx (compacted) data
// and fonts arrive as DefineCompactedFont, a third loader with a completely
// different field layout.
//
// This one has exactly the failure shape the truncation theory needs, and it
// computes the answer itself rather than relying on the guest's logging (which
// is unreachable -- 0 sink calls, no GFxLog installed):
//
//     expected = *(DWORD *)(tag + 8) - 2      the payload it intends to read
//     font+32                                  bytes it actually copied in
//     font+84                                  nominal size; 0 means the parse
//                                              produced nothing
//
// The copy loop reads in 4096-byte chunks and BREAKS on a short read, then
// falls through to parse whatever it got. So written < expected is a literally
// truncated font blob -- and sub_82944D90 bails outright if the blob is under
// 15 bytes, leaving nominal size 0, which the loader reports as a broken gfx
// file. Either way the glyph table ends early and every character past the cut
// is unreachable by index, which is the shape of the missing U, S and C.
//
// font+52 is the glyph count as read by sub_82944D90 (which is called with
// font+40, so its a1+12 is font+52). Logged for information, but treat it as
// UNVERIFIED -- the parser also reads its source blob from font+48 while the
// loader fills font+28, and I have not reconciled that. The two numbers this
// hook rests on are `written vs expected` and the nominal size, both of which
// come straight from the loader's own code.
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
  REXLOG_INFO("d3d9: decl #{} written to logs/decldump/decls.txt", n);
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
  NoteUpDrawCaller(static_cast<uint32_t>(ctx.lr), ctx.r6.u32, 0);
  // BEFORE the passthrough return, so the count is comparable across modes.
  g_guestDrawCalls.fetch_add(1, std::memory_order_relaxed);
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
  NoteUpDrawCaller(static_cast<uint32_t>(ctx.lr), ctx.r6.u32, 1);
  g_guestDrawCalls.fetch_add(1, std::memory_order_relaxed);
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

// Build draws from the BeginVertices/EndVertices path.
//
// ALWAYS ON as of 2026-08-26. It was behind --d3d9_begin_vertices, default off,
// because the run that first proved the hook works also took an access
// violation the previous eleven runs had not:
//
//     write to guest 0x58 in sub_8234CE20 +0x10B
//     sub_8234CE20:  if (!this[105]) { v2 = this[37]; *(v2 + 88) = 1; ... }
//
// 0x58 is 88 decimal, so `this[37]` (+148) was null: a one-shot init against a
// half-constructed object, and the guard at the top is not atomic with the
// `this[105] = 1` at the bottom. It reached 2 crashes in 5 hook-on runs and
// then stopped reproducing.
//
// The cvar is gone because this path is not optional: it is the ONLY way the
// engine's UI draws reach us, and without it the intro logo is never submitted
// at all. A flag defaulted off is a flag that is never exercised, and the
// counters it was protecting -- FRAME DRAWS comparing against historical logs
// -- stopped being the live question once the draws became load-bearing.
//
// What that crash was never explained. If guest-side faults reappear around
// front-end construction, this is the first thing to suspect and
// ui-draws-bypass-hooked-entry-points carries the register dump and the
// decompilation. Reverting is a two-line change: restore the early-out at the
// top of each hook.
namespace {

// Set while D3DDevice_DrawVerticesUP's original is running. See the
// BeginVertices hook below for why.
thread_local uint32_t t_inDrawVerticesUP = 0;
struct UpDepthGuard {
  UpDepthGuard() { ++t_inDrawVerticesUP; }
  ~UpDepthGuard() { --t_inDrawVerticesUP; }
};

// What BeginVertices reserved, carried to the matching EndVertices.
//
// Thread-local because this title submits draws from several threads
// (device-state-is-thread-local), and the pair is strictly nested within one
// call chain on one thread.
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
// 0x825556C8 / 0x825556B8 — D3DDevice_BeginVertices / D3DDevice_EndVertices
//
// The FOURTH draw path, and the one that made the intro logo invisible.
//
// BeginVertices reserves command-ring space, EMITS THE PM4 DRAW PACKET ITSELF,
// and returns a guest pointer for the caller to write inline vertices into;
// EndVertices closes the reservation. The draw packet is plain in the
// decompilation of sub_825556C8:
//
//     v25[5] = primType & 0x3F | (vertexCount << 16) | 0x80;
//
// so nothing on this path passes through DrawIndexedVertices, DrawVertices or
// DrawVerticesUP — the three entry points this file hooks. The engine's UI
// draws exclusively this way: sub_82B296B0 sets the state and calls
// sub_82B27390, which is BeginVertices + memcpy + EndVertices.
//
// That is why every stage of the UI submit measured as PASSING while
// GuestDrawCalls never moved (UI RENDER DRAW: 0 of 2816 READY entries moved
// it). The counter can only see hooked entry points, and this path uses none.
//
// TWO THINGS THIS MUST NOT DO:
//
//   * Double count. DrawVerticesUP reserves through this same function, so a
//     naive hook builds a second DrawCall for every UP draw. t_inDrawVerticesUP
//     suppresses those, and they are COUNTED as suppressed rather than dropped
//     silently — if that number is ever zero on a run with UP draws, the guard
//     is not doing what this comment claims.
//   * Read the vertices too early. BeginVertices returns a pointer to UNWRITTEN
//     ring space; the caller memcpys into it afterwards. The bytes are read at
//     EndVertices, which is the first moment they exist.
//-----------------------------------------------------------------------------
REX_IMPORT(__imp__sub_825556C8, orig_BeginVertices, void());
extern "C" REX_FUNC(sub_825556C8) {
  const bool nested = t_inDrawVerticesUP != 0;
  // Only the outermost reservation is a draw of its own; the UP wrapper counts
  // its own.
  if (!nested) g_guestDrawCalls.fetch_add(1, std::memory_order_relaxed);
  MX_D3D9_PLUGIN_PASSTHROUGH(orig_BeginVertices);

  const uint32_t device = ctx.r3.u32;
  const uint32_t prim_type = ctx.r4.u32;
  const uint32_t count = ctx.r5.u32;
  const uint32_t stride = ctx.r6.u32;

  if (!nested) {
    ++mx::hle::D3D9DrawCounter();
    NoteDrawDeclaration(device, base);
    if (REXCVAR_GET(hle_capture)) {
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
  MX_D3D9_PLUGIN_PASSTHROUGH(orig_EndVertices);
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
// 0x82555B88 — D3DDevice_DrawVerticesUP(D3DDevice*, D3DPRIMITIVETYPE,
//                  UINT VertexCount, const void* pVertexStreamZeroData,
//                  UINT VertexStreamZeroStride)
//
// The third draw entry point, and the one this port had never hooked. It does
// not call either of the other two: it reserves ring space via sub_825556C8,
// memcpys VertexCount*Stride bytes of inline vertex data into it, and returns.
// So no bound stream describes its geometry and nothing downstream would ever
// have seen these draws.
//
// About 30 functions across the engine draw through it — UI, particles, and
// the Bink frame composite sub_8234C7C0, which is what made its absence
// visible. See docs/guest_binary.md.
//
// The data pointer is frequently a caller stack local (it is in the Bink
// case), so the bytes are read here, inside the call, while that frame is
// still live. BuildHleDraw copies them into the DrawCall; nothing retains the
// pointer.
//-----------------------------------------------------------------------------
REX_IMPORT(__imp__sub_82555B88, orig_DrawVerticesUP, void());
extern "C" REX_FUNC(sub_82555B88) {
  // DrawVerticesUP reserves its ring space THROUGH BeginVertices, so the
  // hook below would build a second DrawCall for every UP draw. RAII, not a
  // plain ++/--, because the plugin passthrough returns early.
  const UpDepthGuard up_depth_guard;
  g_guestDrawCalls.fetch_add(1, std::memory_order_relaxed);
  MX_D3D9_PLUGIN_PASSTHROUGH(orig_DrawVerticesUP);
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
  if (REXCVAR_GET(hle_capture)) {
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
// THE FOURTH DRAW ENTRY POINT, and the second one this port shipped without.
// DrawVerticesUP was the first (see its hook above); this is its INDEXED twin,
// and it is the ONLY path Scaleform SHAPES use.
//
// How it was found: a caller census on the link register across all three
// previously-hooked entry points found 31 distinct sites in a menu run and
// exactly ONE of them in GFx -- lr 0x829E1314, inside DrawBitmaps. GFx's shape
// path, GRenderer::DrawIndexedTriList (sub_829E0C80), draws through this
// function instead, so every shape was invisible to us and never reached the
// renderer. That single fact accounts for the missing panels, bar backgrounds,
// star widget and button glyphs (all shapes), for text surviving (glyphs are
// bitmaps, drawn by DrawBitmaps through DrawVerticesUP), and for the vanishing
// menu text: the stencil MASK SHAPE never drew, so the plane kept the 0 that
// BeginSubmitMask cleared it to and 7,465 EQUAL-ref-1 draws per run were all
// rejected.
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
//     r1  + 0x54  vertex STRIDE         (9th arg: `lwz r30, 0x104(r1)` after a
//                                        0xB0 stwu, so +0x54 at hook entry)
//
// The index WIDTH rule is the callee's own, not the caller's: it computes
// `(r9 & 4) ? 4 : 2`. sub_82555BD0, the reserve helper, applies the identical
// test (`rlwinm. r22, r8, 0,29,29`) and sizes vertices as `r6 * r9`, which
// independently confirms the count and stride mapping.
//
// INDICES ARE ABSOLUTE. The guest uploads vertices starting at
// `pVertexStreamZeroData + MinVertexIndex * stride` and passes -MinVertexIndex
// to the reserve as the base-vertex bias, so the index values address the
// caller's FULL array. Stream 0 is therefore synthesised at the array base with
// the bias left at zero, rather than re-basing both and having to keep the two
// consistent.
//
// The UpDepthGuard is applied even though sub_82555BD0 reserves ring space
// itself rather than through BeginVertices: it costs nothing when nothing
// nests, and if that reserve ever does reach BeginVertices the guard is what
// stops this draw being built twice -- the exact bug it was added for on the
// DrawVerticesUP path.
//-----------------------------------------------------------------------------
REX_IMPORT(__imp__sub_82556110, orig_DrawIndexedVerticesUP, void());
extern "C" REX_FUNC(sub_82556110) {
  const UpDepthGuard up_depth_guard;
  g_guestDrawCalls.fetch_add(1, std::memory_order_relaxed);
  MX_D3D9_PLUGIN_PASSTHROUGH(orig_DrawIndexedVerticesUP);

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
  if (REXCVAR_GET(hle_capture)) {
    DeviceState().NoteDevice(device, mx::hle::kEpDraw);
    SampleFetchConstantFile(device, base);
  }

  orig_DrawIndexedVerticesUP(ctx, base);

  // Same ordering as the other three: the original performs D3D9's lazy
  // vertex-shader patching, so translating after it returns lets a shader's
  // first draw use the code captured during this call.
  //
  // Vertices are needed up to MinVertexIndex + NumVertices, because the indices
  // are absolute into the caller's array.
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
    // duration of the build, exactly as UpVertexData is synthesised into
    // stream 0 -- so this reuses BuildHleDraw whole instead of carving a second
    // indexed path through it. DeviceState is thread-local, so no other thread
    // can observe the swap.
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

//-----------------------------------------------------------------------------
// Does SetVertexShader publish literal constants, and is c255 among them?
//
// From the disassembly of D3DDevice_SetVertexShader (0x825508A8), which walks a
// patch block carried by the shader object:
//
//   H = pShader + 0x368                       (the header)
//   P = H + *(u32*)(H + 0x14)                 (the patch block)
//   P + 0x00  u64   dirty bits ANDC-cleared from device + 0x00
//   P + 0x10  u32   byte length of the entry list
//   P + 0x14        first entry
//
//   entry: u16 byte_offset, u16 dword_count, then dword_count dwords of data.
//   memcpy(device + 0x480 + byte_offset, entry_payload, dword_count * 4)
//
// `addi r28, r30, 0x480` is the destination base, so offsets are relative to the
// whole constants block, not to the VS float file. VS constant i therefore sits
// at 0x780 - 0x480 + i*16, making c255 offset 0x12F0.
//
// Read either side of the original call on purpose. We call through to the
// guest's own D3D9, so if this list is what fills c255 then the value must
// change across that call — and if it does not, this mechanism is not the
// answer whatever the list contains. That distinction is the whole point of the
// probe; the list alone would only show intent.
//-----------------------------------------------------------------------------

// Walk the shader's embedded constant-load table and report what it publishes.
//
// sub_825656A0, called from the draw-time flush sub_82565928 as
// sub_825656A0(device, vs + 0x368, *(vs + 0x20)), walks the list at P + 0x14
// (P = H + *(H + 0x14), H = vs + 0x368) and emits, per entry, a PM4 Type-3
// packet with header 0xC0022F00 — opcode 0x2F, LOAD_ALU_CONSTANT — whose body
// is [source_address, 4 * reg_index, dword_count].
//
//   entry: u16 reg_index, u16 dword_count, u32 data_offset   (8 bytes)
//   terminated by dword_count == 0
//   source = *(vs + 0x20) + data_offset
//
// These are the entries D3DDevice_SetVertexShader's FIRST loop steps over
// without acting on; its memcpy loop is a different, later list in the same
// block. An earlier version of this probe walked the first list with the
// second's layout, read reg_index 0xFC as a byte offset, compared it against
// c255's byte offset 0x12F0, and concluded nothing published c255. 0xFC is
// c252, and the count of 16 dwords covers c252..c255 — which is register
// 0x4000 + 252*4 = 0x43F0, the LOAD_ALU_CONSTANT already on record.
//
// So this data never passes through device + 0x780. It is literal constant
// data carried inside the shader's own code allocation and handed to the GPU
// by address, which is why the device shadow the HLE reads shows zeros.
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
  MX_D3D9_PLUGIN_PASSTHROUGH(orig_SetVertexShader);
  auto& st = DeviceState();
  const uint32_t device = ctx.r3.u32;
  const uint32_t shader = ctx.r4.u32;
  st.NoteDevice(device, mx::hle::kEpSetVertexShader);
  st.vertex_shader = shader;
  st.vs_seen = true;
  const bool capture = REXCVAR_GET(hle_capture);
  if (capture) {
    DumpVertexShaderObject(shader, base);
  }
  orig_SetVertexShader(ctx, base);
  // Behind hle_capture like every other diagnostic here. No longer sampled
  // either side of the original call: that comparison was built to test whether
  // SetVertexShader itself writes c255 into the device shadow. It does not, and
  // the reason is now known — the value never goes through the shadow at all.
  if (capture && shader) {
    ProbeVertexShaderConstantPatch(shader, device, nullptr, base);
  }
}

REX_IMPORT(__imp__sub_825506E8, orig_SetPixelShader, void());
extern "C" REX_FUNC(sub_825506E8) {
  MX_D3D9_PLUGIN_PASSTHROUGH(orig_SetPixelShader);
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

// Tell the renderer a surface EXISTS, whether or not a draw ever names it.
//
// Host storage used to be created on the first draw that targeted a surface,
// which loses any pass that binds and resolves without a draw we route. The
// menu's shadow atlas is exactly that: bound depth-only with no colour target,
// resolved every frame, and never instantiated -- so the backdrop shader's
// slot 15 found no snapshot, its draw was discarded whole, and the arena
// rendered black.
//
// Deduped per frame on object and extent. A bind fires far more often than a
// surface changes -- the guest re-binds the same depth surface around every
// pass -- and the renderer only needs to be told once per frame that it exists.
// Keyed on the extent too so a surface rebound at a new size still reaches the
// factory, which replaces in place rather than refusing.
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
  MX_D3D9_PLUGIN_PASSTHROUGH(orig_SetRenderTarget);
  const uint32_t slot = ctx.r4.u32;
  const uint32_t object = ctx.r5.u32;
  if (slot < 4) {
    auto& st = DeviceState();
    st.NoteDevice(ctx.r3.u32, mx::hle::kEpSetRenderTarget);
    st.render_target[slot] = SnapshotRenderTarget(object, base);
    st.render_target_seen_mask |= 1u << slot;

    const auto& rt = st.render_target[slot];
    // Slot 0 only. The renderer models one colour attachment (DrawCall's
    // render_target_object comes from render_target[0]), so instantiating
    // slots 1-3 would spend budget on surfaces nothing routes -- and worse,
    // would let a resolve out of an MRT slot copy a freshly cleared target
    // instead of failing, trading a missing image for a confidently blank one.
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
  MX_D3D9_PLUGIN_PASSTHROUGH(orig_SetDepthStencil);
  auto& st = DeviceState();
  st.NoteDevice(ctx.r3.u32, mx::hle::kEpSetDepthStencil);
  st.depth_stencil = SnapshotRenderTarget(ctx.r4.u32, base);
  // The depth half of the same story, and the one that motivated it: every
  // depth target we have ever created arrived paired with a colour target, so
  // a depth-only pass instantiated nothing at all.
  NoteSurfaceBind(st.depth_stencil, /*is_depth=*/true);
  // DIAG: the depth surfaces the guest binds, and their
  // OWN extents. Sizing a host depth surface to the colour target instead
  // collapsed the frame, because one depth object is bound alongside colour
  // targets of several extents (the 1280x720 surface and its 1280x640 and
  // 1280x80 EDRAM bands). Whether the guest declares one 1280x720 depth for all
  // of them, or a separate depth surface per band, decides whether the host
  // pool can be keyed by object alone.
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
// 0x8255CE98 — D3DDevice_Resolve.
//
// r4 low three bits select colour target 0..3 or depth target 4; r6 is the
// destination D3DBaseTexture. The internal helper reads that texture's fetch
// descriptor at +0x1C, proving this call — not SetTexture — is the EDRAM to
// system-memory bridge. Record the ordered relationship for host-side
// render-to-texture routing.
//-----------------------------------------------------------------------------
// 0x8255B258 - D3DDevice_Clear.
//
// Only the measured full-surface colour form is modelled here. The front-end
// default-texture atlas binds a 256x256 scratch target, clears it, and resolves
// it three times without issuing a draw. Without this ordered event the host
// has no source resource for those resolves, so the final compositor shader is
// rejected even though the atlas tiles are intentionally blank defaults.
// Partial rectangle and depth/stencil clears still pass through to the guest.
REX_IMPORT(__imp__sub_8255B258, orig_Clear, void());
extern "C" REX_FUNC(sub_8255B258) {
  MX_D3D9_PLUGIN_PASSTHROUGH(orig_Clear);
  const uint32_t rect_count = ctx.r4.u32;
  const uint32_t rects = ctx.r5.u32;
  const uint32_t flags = ctx.r6.u32;
  const uint32_t color = ctx.r7.u32;
  const auto& target = DeviceState().render_target[0];

  // CLEAR CENSUS -- every call, BEFORE any bit test.
  //
  // The existing CLEAR line below lives inside the `flags & 1` branch, so it
  // can only ever report clears that touch colour. A depth-only clear is
  // invisible to it, which makes it useless for the one question being asked:
  // does the guest ask for depth clears we drop on the floor?
  //
  // That question is live. freeroam.rdc: the D32S8 depth target ResourceId::384
  // is Cleared exactly ONCE in the whole frame (event 15183) and then used as
  // the DepthStencilTarget by six separate passes with no clear between them --
  // 17635..17963, 18005..18902, 19031..19941, 19969..23822, 23863..23889,
  // 24199..24803. Draw 19889 paints the ground: it has xe_tex0/1/2 bound, a
  // translated shader, and a shaderOut of (0.309, 0.300, 0.026) -- sand -- and
  // it is discarded `depthTestFailed` against depth it did not write.
  //
  // So the ground is not a texture bug. It samples its textures and computes
  // the right colour, and stale depth rejects it. See also the known-open note
  // that D3DCLEAR_ZBUFFER is 0x10 on this hardware, not the 0x2 a PC D3D9
  // header would tell you.
  //
  // Reported as a HISTOGRAM with a total, not a sample, so the answer carries
  // its own denominator: "0 of N calls had 0x10" and "the probe never ran" have
  // to be different-looking outcomes. One line per distinct flags value, so a
  // clear issued every frame costs one line, and the total keeps counting past
  // the line budget.
  //
  // LET THE FLAGS NAME THEMSELVES. The first cut printed the bit pattern and
  // annotated it from an assumed layout (bit 0 TARGET, bit 4 ZBUFFER). Run 1442
  // showed seven distinct values -- 0x1, 0xF, 0x1F, 0x20, 0x30, 0x3F, 0x60 --
  // and 0x60 alone was 18280 of 28000 calls, which no assumed layout accounted
  // for. Guessing what bit 6 means is exactly the move that has cost this
  // session twice already.
  //
  // The call carries the answer. The decompiled signature is
  //
  //   D3DDevice_Clear(pDevice, Count, pRects, Flags, Color, Z, Stencil,
  //                   EDRAMClear)
  //
  // so Z arrives in f1, Stencil in r8 and EDRAMClear in r9. A flag value whose
  // calls carry Z=1.0 is a DEPTH clear whatever its bit pattern; one that never
  // varies Z is not. That identifies each value from its own arguments instead
  // of from a header this project does not have -- the same "let it name
  // itself" move that resolved the UI submit target.
  {
    struct FlagStat {
      uint64_t calls = 0;
      double first_z = 0.0;
      uint32_t first_r8 = 0, first_r9 = 0, first_r10 = 0;
      uint64_t r9_nonzero = 0;
      bool z_varies = false;
    };
    // ARGUMENT SLOTS ARE NOT ASSUMED. Run 1444 read Z as *(float*)&ctx.f1 and
    // got 0 for every one of 24000 calls -- which is what the LOW half of a
    // double looks like, and rex::ppc's FP register is a union with separate
    // f32 and f64 members. Z = 1.0 as a double has a zero low word, so the
    // instrument was blind to exactly the value it existed to find. Read f64.
    //
    // The integer slots were wrong too. Taking r8 as Stencil produced 47185920
    // for flags 0x30, 0x3F AND 0x60 alike -- one identical value across three
    // unrelated groups is a leftover register, not an argument. PowerPC ABIs
    // differ over whether a float argument also consumes its GPR slot, so
    // Stencil may sit at r9 or r10 rather than r8. Print r8, r9 and r10 raw and
    // let the correlation say which one tracks the flags; do not annotate them
    // until it does.
    static std::mutex s_mu;
    static std::map<uint32_t, FlagStat> s_byFlags;
    static uint64_t s_total = 0, s_fullSurface = 0;
    const double z = ctx.f1.f64;
    const uint32_t r8 = ctx.r8.u32;
    const uint32_t r9 = ctx.r9.u32;
    const uint32_t r10 = ctx.r10.u32;
    std::lock_guard<std::mutex> lk(s_mu);
    ++s_total;
    if (rect_count == 0 && rects == 0) ++s_fullSurface;
    FlagStat& st = s_byFlags[flags];
    const bool fresh = ++st.calls == 1;
    if (fresh) {
      st.first_z = z;
      st.first_r8 = r8;
      st.first_r9 = r9;
      st.first_r10 = r10;
    } else if (z != st.first_z) {
      st.z_varies = true;
    }
    if (r9) ++st.r9_nonzero;
    if ((fresh && s_byFlags.size() <= 32) || (s_total % 4000) == 0) {
      std::string hist;
      for (const auto& [f, v] : s_byFlags) {
        hist += fmt::format(
            " [0x{:X} x{} z={:g}{} r8=0x{:X} r9=0x{:X}(nz{}) r10=0x{:X}]", f,
            v.calls, v.first_z, v.z_varies ? "(varies)" : "", v.first_r8,
            v.first_r9, v.r9_nonzero, v.first_r10);
      }
      REXLOG_INFO("d3d9: CLEAR CENSUS {} calls, {} whole-surface, {} distinct "
                  "flag values; handled today: only those with bit0 and a "
                  "valid colour target --{}",
                  s_total, s_fullSurface, s_byFlags.size(), hist);
    }
  }

  // D3DCLEAR_TARGET is bit 0. Count zero and a null rectangle pointer are the
  // whole-target form used by the measured atlas initializer.
  if ((flags & 1u) && rect_count == 0 && rects == 0 && target.valid) {
    mx::hle::DrawCall clear{};
    clear.clear_color_target = true;
    clear.clear_color = color;
    clear.render_target_object = target.object;
    clear.render_target_surface_info = target.surface_info;
    clear.render_target_color_info = target.color_info;
    clear.render_target_width = target.width;
    clear.render_target_height = target.height;
    clear.surface_base = target.color_info & 0xFFFu;
    mx::hle::HleFrameDraws().push_back(std::move(clear));
    // Keyed on (target, COLOUR), not on the target alone. Deduping by target
    // logged only the FIRST colour each surface was ever cleared to and
    // silently swallowed every later one -- so a run whose targets are first
    // cleared to black reads as "this game only ever clears to 0x00000000",
    // which is not a measurement of the clear colours at all. That reading was
    // used to argue a mid-grey clear could not be the guest's, and it could not
    // support the claim.
    //
    // Same correction as the texture-reject log above, which is keyed on
    // (format, reason) for exactly this reason. The budget is per distinct
    // pair, so a surface cleared to two colours costs two lines, not one per
    // clear.
    static std::set<std::pair<uint32_t, uint32_t>> s_logged;
    if (s_logged.insert({target.object, color}).second &&
        s_logged.size() <= 64) {
      REXLOG_INFO("d3d9: CLEAR target 0x{:08X} {}x{} color=0x{:08X} "
                  "flags=0x{:X}",
                  target.object, target.width, target.height, color, flags);
    }
  }
  // D3DCLEAR_ZBUFFER is 0x10 on this hardware, NOT the 0x2 a PC D3D9 header
  // would tell you. Narrow on purpose: run 1445 saw seven distinct flag values
  // and FIVE of them carry Z=1.0 (0xF, 0x1F, 0x30, 0x3F, 0x60), which is
  // 22703 of 24000 calls. Z is an ARGUMENT though, not a flag -- a caller
  // passing 1.0 suggests depth-clear intent without proving the bit was set --
  // so this gates on 0x10 alone (0x1F, 0x30, 0x3F = 4930 calls) rather than on
  // the wider Z reading.
  //
  // 0x60 is deliberately EXCLUDED even though it carries Z=1.0 and is 16139 of
  // the 24000. It is the only value whose r9 is set, on every single call, and
  // r9 is the EDRAMClear argument -- so it is a distinct operation, most likely
  // the EDRAM tile clear, and wiping the whole depth buffer for each one would
  // erase the depth prepass and be worse than the bug. If the narrow gate does
  // not restore the ground, widening to 0x60 is the next experiment, not a
  // correction of this one.
  //
  // Bits 1..6 are still not named. IDA's bounds for D3DDevice_Clear stop at
  // 0x8255B284 on a misdecoded vcmpneb., so the body is unreachable through the
  // decompiler, and the constants are in no header in this tree or the SDK.
  // Anything beyond bit 0 and bit 4 here is measurement, not documentation.
  {
    const auto& depth = DeviceState().depth_stencil;
    // 0x20 is D3DCLEAR_STENCIL. PROVEN, not extrapolated, from the guest's own
    // clear emitter sub_8255A510 (default.xex.probe.i64, 2026-08-27):
    //
    //     if ( (Flags & 0x10) != 0 )  v41 |= 1u;          // depth
    //     if ( (Flags & 0x20) != 0 ) {
    //         v41 |= 4u;                                  // stencil
    //         ...
    //         *v44++ = 8461;                              // 8461 == 0x210D,
    //         *v44 = 0x00FF0000 | (Stencil & 0xFF);       // RB_STENCILREFMASK
    //     }
    //
    // so 0x20 is the bit that both enables the stencil half and publishes the
    // caller's Stencil value as the ref with mask 0xFF. The caller-side decode
    // agrees: sub_8255AAB0 loops bits 0..3 over the four render targets at
    // device+12616 and masks off 0xF0 after the first one, so bits 0-3 are the
    // MRT colour targets and 0x10/0x20/0x40/0x80 are the depth-stencil group.
    //
    // The observed flag distribution confirms it rather than merely permitting
    // it: 0xF colours, 0x1F colours+depth, 0x20 stencil alone, 0x30
    // depth+stencil, 0x3F all three, 0x60 stencil+EDRAM.
    //
    // STENCIL IS r9, NOT r8, and this file said otherwise until run 1536.
    //
    // The prototype is (pDevice, Count, pRects, Flags, Color, Z, Stencil,
    // EDRAMClear). On this ABI a float argument consumes its integer register
    // slot, so Z in f1 RESERVES r8 and the two integer args after it land in r9
    // and r10. Census, reproduced over two runs:
    //
    //   r8   0x2D00000 / 0x810000 / 0x18280186   never 0..255, and constant
    //                                            within a flag group: a
    //                                            leftover, not an argument
    //   r9   0 everywhere, 1 on 0x60
    //   r10  0 always
    //
    // The first cut read r8 and logged `s=0` on every line, which looked
    // correct and was not: 0x2D00000 & 0xFF is 0.
    //
    // PROVEN 2026-08-27, by following the value through four frames rather than
    // inferring it from the ABI. The earlier note here said the census only
    // CORROBORATED this -- r9 being 0/1 is predicted equally well by
    // EDRAMClear-as-a-BOOL -- and that was right to say. This is the trace that
    // settles it:
    //
    //   D3DDevice_Clear   8255b270  mr  r27, r9
    //                     8255b2b8  mr  r8, r27          -> sub_8255B130
    //   sub_8255B130                r8 untouched          -> sub_8255AAB0
    //   sub_8255AAB0      prologue  mr  r22, r8
    //                     8255afdc  stw r22, 0x100+var_A4(r1)   ; r1 + 0x5C
    //                     8255b000  bl  sub_8255A510
    //   sub_8255A510      8255a5d0  lwz r29, 0x130+arg_5C(r1)   ; same slot
    //                     8255a5e4  insrwi r30, r29, 8,16
    //                               and the RB_STENCILREFMASK write below it
    //
    // So r9 IS Stencil, r10 IS EDRAMClear, and the value this hook carries is
    // the one that reaches the hardware register. The clear VALUE is therefore
    // not a suspect in anything downstream.
    const uint32_t stencil_value = ctx.r9.u32;
    const bool want_depth = (flags & 0x10u) != 0;
    // 0x60 IS HONOURED. It was excluded twice, on two wrong readings:
    //
    //   1. "the EDRAM tile clear", because r9 was set on every one of its
    //      calls. r9 is the Stencil ARGUMENT, so that was a stencil value of 1
    //      read as a boolean. r10, the real EDRAMClear, is zero on all 20,000
    //      calls -- nothing in the run is an EDRAM clear.
    //   2. "the 0x40 path is not a stencil clear". Reading sub_8255A510 again
    //      says it is:
    //
    //          if ( (Flags & 0x20) != 0 ) {
    //              v41 |= 4u;                                    // stencil
    //              if ( (Flags & 0x40) != 0 )
    //                  v41 = (Stencil << 8) & 0xFF00 | v41 & 0xFFFF00DF;
    //              *v44 = 0x00FF0000 | (Stencil & 0xFF);         // REFMASK
    //          }
    //
    //      0x40 does NOT suppress the clear -- bit 2 of v41 is set before the
    //      branch and stays set. It only moves the value into bits 8-15 and
    //      clears bit 5. So 0x60 is a stencil clear that carries its value in a
    //      second field, and r9 = 1 on every one of those calls: "clear stencil
    //      to 1", 13,370 times a run.
    //
    // Dropping them left the plane stuck at 0, and a terrain testing
    // NotEqual-0 against 0 fails everywhere -- which is the broken ground.
    const bool want_stencil = (flags & 0x20u) != 0;
    if ((want_depth || want_stencil) && rect_count == 0 && rects == 0 &&
        depth.valid) {
      mx::hle::DrawCall dclear{};
      dclear.clear_depth_target = want_depth;
      dclear.clear_stencil_target = want_stencil;
      dclear.clear_stencil = uint8_t(stencil_value & 0xFFu);
      // The guest's own Z, not a hardcoded 1.0. It is 1.0 in every call
      // measured, but reading it costs nothing and a reversed-depth pass would
      // otherwise be cleared to the wrong end.
      dclear.clear_depth = float(ctx.f1.f64);
      dclear.depth_target_object = depth.object;
      dclear.depth_target_width = depth.width;
      dclear.depth_target_height = depth.height;
      dclear.depth_target_base = depth.color_info & 0xFFFu;
      mx::hle::HleFrameDraws().push_back(std::move(dclear));
      // Keyed on (target, flags), not on the target alone: a surface cleared
      // depth-only and later depth+stencil is two different behaviours and
      // deduping by object would log only whichever came first.
      static std::set<std::pair<uint32_t, uint32_t>> s_logged;
      if (s_logged.insert({depth.object, flags}).second &&
          s_logged.size() <= 24) {
        REXLOG_INFO("d3d9: DEPTH/STENCIL CLEAR target 0x{:08X} {}x{} z={:g} "
                    "depth={} stencil={} s={} flags=0x{:X}",
                    depth.object, depth.width, depth.height,
                    double(dclear.clear_depth), want_depth, want_stencil,
                    stencil_value & 0xFFu, flags);
      }
    }
  }
  orig_Clear(ctx, base);
}

REX_IMPORT(__imp__sub_8255CE98, orig_Resolve, void());
extern "C" REX_FUNC(sub_8255CE98) {
  MX_D3D9_PLUGIN_PASSTHROUGH(orig_Resolve);
  auto& st = DeviceState();
  st.NoteDevice(ctx.r3.u32, mx::hle::kEpResolve);
  const uint32_t resolve_flags = ctx.r4.u32;
  const uint32_t source_slot = resolve_flags & 7u;
  const uint32_t dest_texture = ctx.r6.u32;
  // Decompiled signature (default.xex.probe.i64):
  //   D3DDevice_Resolve(pDevice, Flags, pSourceRect, pDestTexture, pDestPoint,
  //                     DestLevel, DestSliceOrFace, pClearColor, ...)
  // so r5 is the source rectangle and r7 the destination point. Both are LONG
  // pairs/quads (D3DRECT{x1,y1,x2,y2}, D3DPOINT{x,y}) and both may be null.
  const uint32_t source_rect_ptr = ctx.r5.u32;
  const uint32_t dest_point_ptr = ctx.r7.u32;
  int32_t src_rect[4] = {0, 0, 0, 0};
  bool have_src_rect = false;
  if (source_rect_ptr && HostPageReadable(REX_RAW_ADDR(source_rect_ptr)) &&
      HostPageReadable(REX_RAW_ADDR(source_rect_ptr + 12))) {
    for (uint32_t i = 0; i < 4; ++i)
      src_rect[i] = int32_t(REX_LOAD_U32(source_rect_ptr + i * 4));
    have_src_rect = src_rect[2] > src_rect[0] && src_rect[3] > src_rect[1];
  }
  int32_t dest_point[2] = {0, 0};
  bool have_dest_point = false;
  if (dest_point_ptr && HostPageReadable(REX_RAW_ADDR(dest_point_ptr)) &&
      HostPageReadable(REX_RAW_ADDR(dest_point_ptr + 4))) {
    dest_point[0] = int32_t(REX_LOAD_U32(dest_point_ptr));
    dest_point[1] = int32_t(REX_LOAD_U32(dest_point_ptr + 4));
    have_dest_point = dest_point[0] >= 0 && dest_point[1] >= 0;
  }
  const mx::hle::RenderTargetBinding* source = nullptr;
  if (source_slot < 4)
    source = &st.render_target[source_slot];
  else if (source_slot == 4)
    source = &st.depth_stencil;

  // A resolve that names a destination but cannot be recorded. Counted and
  // logged because it used to be silent, and it is one of the two ways a
  // resolved surface ends up sampled as black.
  if (dest_texture && (!source || !source->valid)) {
    ++g_resolveDroppedNoSource;
    static std::map<uint32_t, uint64_t> s_dropped;
    if (s_dropped[dest_texture]++ == 0) {
      REXLOG_INFO("d3d9: resolve DROPPED (no valid source): slot {} dest "
                  "0x{:08X}; source {} {}x{} -- this destination will decode "
                  "from guest memory and read black",
                  source_slot, dest_texture,
                  source ? "invalid" : "absent",
                  source ? source->width : 0, source ? source->height : 0);
    }
  }

  // The other half of the thread pairing above.
  {
    static std::set<uint64_t> s_seen;
    const uint64_t id =
        (uint64_t(GetCurrentThreadId()) << 32) | (source ? source->object : 0);
    if (s_seen.insert(id).second && s_seen.size() <= 32) {
      REXLOG_INFO("d3d9: RESOLVE thread {} source slot {} object 0x{:08X} "
                  "valid={}",
                  GetCurrentThreadId(), source_slot,
                  source ? source->object : 0, source && source->valid);
    }
  }

  uint32_t dest_extent_width = 0, dest_extent_height = 0;
  if (dest_texture && source && source->valid) {
    g_resolvedTextureTargets[dest_texture] = source->object;

    // The destination's own fetch constant, which is where its guest memory
    // address lives. The six dwords sit at +0x1C..+0x30 -- the same offsets
    // SetTexture copies from -- and DescribeHleTexture2D already turns them
    // into an address and an extent, so nothing here decodes a bitfield by
    // hand. Only dword 0 used to be read, and only to print.
    if (HostPageReadable(REX_RAW_ADDR(dest_texture + 0x1C)) &&
        HostPageReadable(REX_RAW_ADDR(dest_texture + 0x30))) {
      uint32_t dest_fetch[6] = {};
      for (uint32_t i = 0; i < 6; ++i)
        dest_fetch[i] = REX_LOAD_U32(dest_texture + 0x1C + i * 4);
      mx::hle::HleTextureSource dest_desc;
      if (mx::hle::DescribeHleTexture2D(dest_fetch, dest_desc, nullptr) &&
          dest_desc.address) {
        const uint32_t physical = GpuPhysicalAddress(dest_desc.address);
        auto& entry = g_resolvedTargetsByAddress[physical];
        const bool first = entry.dest_object == 0;
        entry.dest_object = dest_texture;
        entry.source_object = source->object;
        entry.width = dest_desc.width;
        entry.height = dest_desc.height;
        // Where this resolve lands in the destination. No rect means the whole
        // surface, which is the common case and must read as full coverage.
        {
          const uint32_t dx = have_dest_point ? dest_point[0]
                              : have_src_rect ? src_rect[0]
                                              : 0;
          const uint32_t dy = have_dest_point ? dest_point[1]
                              : have_src_rect ? src_rect[1]
                                              : 0;
          uint32_t w = have_src_rect && src_rect[2] > src_rect[0]
                           ? uint32_t(src_rect[2] - src_rect[0])
                           : source->width;
          uint32_t h = have_src_rect && src_rect[3] > src_rect[1]
                           ? uint32_t(src_rect[3] - src_rect[1])
                           : source->height;
          entry.reached_x = std::max(entry.reached_x, dx + w);
          entry.reached_y = std::max(entry.reached_y, dy + h);
          // The bounding box stays -- it is still worth SEEING in the
          // census, and it is what makes a scattered destination legible
          // once printed next to the real coverage. It is no longer what
          // the claim decision reads; MarkCoverage is. Marked AFTER
          // width/height are assigned above, because the grid is derived
          // from them.
          entry.MarkCoverage(dx, dy, dx + w, dy + h);
          ++entry.resolves;
          g_resolveDestObjectPhys[dest_texture] = physical;
        }
        // SMALL DESTINATION WRITEBACK -- the terrain virtual-texture feedback
        // buffer, and anything else the guest resolves small and then LOADS
        // rather than samples.
        //
        // 0x1A2DD000 is 64x64, resolved once per frame, and `bind0 seen0
        // draws0`: no shader ever touches it. The GPU writes page IDs there so
        // the CPU can decide which tiles to stream. Landing the resolve only in
        // a host snapshot leaves the guest reading whatever was at that address
        // when it was allocated, so it never learns which pages the camera
        // needs -- which is a uniform index map and 3 of 64 atlas tiles.
        //
        // Same moment as the 1x1 case above and for the same reason: the
        // resolve the guest just issued is the one it is about to read, and the
        // value is the previous frame's, which is the latency the console gives
        // it anyway.
        //
        // NOT GATED ON THE DESTINATION'S EXTENT. It used to require
        // `width <= 64 && height <= 64`, the same assumption the renderer side
        // carried and for the same reason: the feedback buffer is 64x64 into a
        // 64x64 destination, so the region and the resource were never told
        // apart. The terrain deformation resolves a 128x32 tile into a
        // 2048x2048 accumulation and was refused here even after the readback
        // had been copied, drained and matched -- a second copy of a bug I had
        // just fixed one file over, still standing.
        //
        // Nothing is lost by dropping it. `rb.destObject == dest_texture`
        // below is the real discriminator and it is exact: only a destination
        // that actually won a readback slot can match, and the region's size is
        // already bounded by the 16 KB CPU buffer it had to fit in. The seq
        // test above means at most one readback's worth of resolves reach the
        // lock per frame.
        if (dest_desc.width > 1 && dest_desc.height > 1 &&
            dest_desc.bytes_per_block && dest_desc.address) {
          //
          // EVERY SLOT, not "has the global sequence moved". The old form kept
          // one `s_surfaceWroteSeq`: the first destination to match stamped the
          // frame consumed, and any other destination delivered in the same
          // frame was skipped without being looked at. That was invisible while
          // one readback was in flight at a time. Each slot now carries its own
          // seq and is remembered separately, so several destinations can be
          // written in one frame.
          //
          // The acquire load below is kept for its fence, not as a gate: it
          // pairs with the release bump in DrainSurfaceReadback so the bytes a
          // slot's seq advertises are visible before the seq is.
          static uint32_t s_slotSeq[mx::hle::kSurfaceReadbackSlots] = {};
          (void)mx::hle::g_surfaceReadbackSeq.load(std::memory_order_acquire);
          for (uint32_t slot = 0; slot < mx::hle::kSurfaceReadbackSlots;
               ++slot) {
            uint32_t wrote = 0;
            uint32_t skipped_unwritable = 0;
            bool matched = false;
            // WHAT WE ACTUALLY DELIVER, over the exact byte the guest gates on.
            //
            // The guest's page-table update (sub_82AF5D38) walks this buffer
            // and, for each texel, reads the LOW BYTE of the big-endian dword
            // as a mip level:
            //
            //     v92 = (unsigned __int8)*v91;
            //     if (v92 < v66) { ...refine this page... }
            //
            // v66 is the level count, so every texel whose low byte is >= that
            // is skipped outright. If all 4096 are skipped the update runs to
            // completion and refines nothing -- which is exactly what we
            // observe: the latch moves every frame and the page table stays at
            // its 0xF00A not-available fill.
            //
            // "wrote 4096 texels" could never distinguish that from a healthy
            // feed. It counts stores, not content.
            uint32_t low_hist[256] = {};
            // Per-byte spread of the feedback texel, in GUEST byte order,
            // over the whole run rather than one writeback -- the interesting
            // frames are a minority and any single-frame view is dominated by
            // whichever phase the log cap happened to land in.
            // ROW/COL HIGH NIBBLES AS A DISTRIBUTION, not a min and a max.
            //
            // The page table's coarse levels are exactly half resident, with a
            // knife edge at the midpoint: at L7 the bottom four rows are all
            // populated and the top four are all the 0xF00A sentinel, and that
            // survived four minutes of driving across the map. The guest
            // decodes ROW from the HIGH nibble of byte +0, so if it never sees
            // rowHi 0 or 1, the top half can never go resident.
            //
            // The byte's RANGE said 0x21..0x33 on a short run and 0x00..0x33 on
            // a long one, and I read the second as "requests reach the top half
            // after all". A min is one texel. It cannot distinguish a handful
            // of stray samples from a real share, and the page table did not
            // move either way -- so the range was the wrong statistic and this
            // is the right one. See `a-total-without-a-denominator`.
            //
            // Same gate and same population as the byte spread below it, so the
            // two are directly comparable: rowHi[0..3] must sum to the same
            // total as b0's `seen`.
            // THE LEVEL BYTE, AFTER THE GATE.
            //
            // Byte +3 is the LOD, and the guest does far more with it than
            // accept or reject: it shifts BOTH coordinates by it
            // (ROW >>= LEVEL, COL >>= LEVEL) and indexes THAT level's table.
            // So a LOD that is constant does not merely lose detail, it aims
            // every request in the run at one level of the pyramid and leaves
            // the rest to the propagation loops -- which is the shape of the
            // half-populated pyramid we are chasing (L8 reads 8/16, and level
            // 8's table is 4x4 = 16 entries).
            //
            // The comment beside the channel-order fix flagged this as "a
            // constant 8 ... still-open" and it was never measured. Its raw
            // range is known (00..FF, mean 0x87) and that is the WRONG
            // population: the guest skips any texel whose level is >= the level
            // count, so the only levels that matter are the ones that pass.
            //
            // Bucketed 0..15 because the gate is `< 16`; the 16th bucket can
            // therefore never fill and a non-zero there would mean the gate
            // moved.
            static uint64_t s_lod[16] = {};
            static uint64_t s_rowHi[4] = {}, s_colHi[4] = {};
            static uint64_t s_byteSeen[4] = {}, s_byteHigh[4] = {};
            static uint32_t s_byteMin[4] = {255, 255, 255, 255};
            static uint32_t s_byteMax[4] = {};
            {
              std::lock_guard<std::mutex> lk(mx::hle::g_surfaceReadbackMutex);
              const auto& rb = mx::hle::g_surfaceReadback[slot];
              // FORMAT CONVERSION, because a Xenos resolve converts.
              //
              // The old guard demanded `rb.bytesPerTexel ==
              // dest_desc.bytes_per_block`, which only the VT feedback buffer
              // satisfies (4 == 4). The terrain deformation resolves an
              // R32_FLOAT tile into a destination the guest then fetches as
              // FMT_8 -- 4 bytes against 1 -- so the pair was rejected before a
              // byte moved.
              //
              // The mapping is not a guess: sub_82AF7240 memsets that buffer to
              // 0x80, and the float accumulation's measured maximum is 0.5021 =
              // 128/255. The float side already carries the unorm value, so
              // round(saturate(f) * 255) is the conversion and 0x80 is its
              // neutral in both representations.
              const bool same_texel =
                  rb.bytesPerTexel == dest_desc.bytes_per_block;
              // DISABLED 2026-08-28 pending a correct mapping. Restore by
              // deleting the `false &&`.
              //
              // The R32_FLOAT deform tile is written raw as round(f * 255) into
              // a buffer the guest MEMSETS TO 0x80. That is defensible only if
              // the tile carries the accumulated height, and in a real level it
              // does not: run 1677 in freeroam wrote
              //
              //   0xF102E000 x42 bpb1 164667/172032 usable, byte 00..FF mean 07
              //   0xF0C2D000 x39 bpb1 152713/159744 usable, byte 00..FF mean 06
              //
              // -- a mean of 6-7 against a neutral of 128, near-zero over ~96%
              // of the surface. The user reports the bike jumping as if it
              // cannot track the terrain, which is what a collapsed height
              // field would look like if the guest reads this.
              //
              // An earlier menu run read `byte 7A..80 mean 7F` and I took that
              // as proof the mapping was right. It was not: in the MENU the
              // tile is neutral, so the raw write happened to land near 0x80 by
              // accident. Menu data cannot validate a level-time mapping.
              //
              // Until it is known whether the tile is the accumulation or a
              // DELTA to be combined with the previous half (the guest has an
              // hft_deform_copy pass that has never been observed executing),
              // writing it raw is worse than not writing it: the buffer keeps
              // its 0x80 memset, which is the neutral the guest itself chose,
              // and the ruts stay missing as they were before this session.
              const bool float_to_unorm8 =
                  false &&
                  rb.bytesPerTexel == 4 && dest_desc.bytes_per_block == 1 &&
                  rb.srcFormat == uint32_t(DXGI_FORMAT_R32_FLOAT);
              matched = rb.seq && rb.seq != s_slotSeq[slot] &&
                        rb.destObject == dest_texture && rb.width &&
                        rb.height && rb.bytesPerTexel &&
                        (same_texel || float_to_unorm8);
              if (matched) {
                s_slotSeq[slot] = rb.seq;
                const uint32_t bpb = dest_desc.bytes_per_block;
                const uint32_t bpb_log2 = uint32_t(std::bit_width(bpb)) - 1u;
                const uint32_t w = std::min(rb.width, dest_desc.width);
                const uint32_t h = std::min(rb.height, dest_desc.height);
                for (uint32_t y = 0; y < h; ++y) {
                  for (uint32_t x = 0; x < w; ++x) {
                    // The READBACK's texel size, which is not the
                    // destination's once a conversion is in play.
                    const size_t srcOff =
                        size_t(y) * rb.rowPitch + size_t(x) * rb.bytesPerTexel;
                    if (srcOff + rb.bytesPerTexel > rb.byteCount) continue;
                    // AT THE DESTPOINT. The copied region is a sub-rect of the
                    // destination; every caller before the terrain deformation
                    // resolved to (0,0), so writing at the origin was right by
                    // accident rather than by rule.
                    const uint32_t dx = x + rb.destX;
                    const uint32_t dy = y + rb.destY;
                    if (dx >= dest_desc.width || dy >= dest_desc.height)
                      continue;
                    // The guest's own layout, tiled or linear, exactly as the
                    // DECODER reads it -- the same tu::GetTiledOffset2D, run in
                    // the other direction. Two address rules that disagree is
                    // the bug an address rule exists to prevent.
                    const uint32_t dstOff =
                        dest_desc.tiled
                            ? uint32_t(tu::GetTiledOffset2D(
                                  int32_t(dx), int32_t(dy),
                                  dest_desc.pitch_blocks, bpb_log2))
                            : (dy * dest_desc.pitch_blocks + dx) * bpb;
                    const uint32_t at = dest_desc.address + dstOff;
                    if (!HostPageReadable(REX_RAW_ADDR(at)) ||
                        !HostPageReadable(REX_RAW_ADDR(at + bpb - 1))) {
                      ++skipped_unwritable;
                      continue;
                    }
                    // Byte-reversed for the guest's endian, the same swap the
                    // upload path applies coming the other way.
                    uint8_t tmp[16];
                    if (float_to_unorm8) {
                      float f = 0.0f;
                      std::memcpy(&f, rb.bytes + srcOff, sizeof(f));
                      if (!(f > 0.0f)) f = 0.0f;  // also catches NaN
                      if (f > 1.0f) f = 1.0f;
                      tmp[0] = uint8_t(f * 255.0f + 0.5f);
                    } else {
                      std::memcpy(tmp, rb.bytes + srcOff, bpb);
                    }
                    if (bpb == 2 && dest_desc.endian != 0) {
                      std::swap(tmp[0], tmp[1]);
                    } else if (bpb == 4 && dest_desc.endian != 0) {
                      // R<->B FIRST, then the reversal.
                      //
                      // The endian reversal was right and it was being applied
                      // to the wrong channel order. The host resource is
                      // DXGI_FORMAT_R8G8B8A8_UNORM, so `rb.bytes` runs R,G,B,A;
                      // the guest's k_8_8_8_8 surface is B,G,R,A. Reversing the
                      // host order alone produced A,B,G,R where the guest reads
                      // A,R,G,B -- red and blue transposed.
                      //
                      // Established from the guest's own feedback walk in
                      // sub_82AF5D38 (0x82AF6054), which is unambiguous about
                      // which byte is which:
                      //
                      //   lwzu   r10, 4(r5)          big-endian texel
                      //   clrlwi r11, r10, 24        byte +3
                      //   cmplw  r11, r22
                      //   bge    -> skip             +3 is the LEVEL
                      //   or     r8, ..., r6         ROW = (+0 hi)<<8 | +2
                      //   or     r9, ..., r20        COL = (+0 lo)<<8 | +1
                      //   srw    r8, r8, r11         both >>= level
                      //   mullw  r7, r6, r8          index = width*ROW + COL
                      //
                      // ps_hft_fback writes o0 = (page X, page Y, LOD, index).
                      // So the guest needs LEVEL <- o0.z (B) at byte +3, and
                      // X's low bits <- o0.x (R) at +1. Before this it got the
                      // page X as the level and the LOD as X's low bits.
                      //
                      // CONFIRMED QUANTITATIVELY, not just derived. Under the
                      // wrong order the guest computes
                      // X = (o0.w low nibble) << 8 | o0.z, and o0.z is a
                      // constant 0x08 while o0.w only ever spans 0x22..0x33 --
                      // so X could only ever land in 0x208..0x308 = 520..776.
                      // Every resident page-table entry in ground-tiles-3/4.rdc
                      // sits at x 512..531, 18-20 columns wide, against a Y
                      // extent of ~283 rows. That strip, its width, and its
                      // start at exactly the level midpoint all fall out of
                      // this transposition.
                      //
                      // The strip is therefore the test: if it does not change
                      // shape on the next run, this derivation is wrong.
                      //
                      // NOTE the LOD is a separate, still-open question -- it
                      // is a constant 8 against a level count of 8, so the gate
                      // may now reject everything. That is the NEXT
                      // measurement, and it could not be taken at all while the
                      // guest was reading a different field entirely.
                      std::swap(tmp[0], tmp[2]);
                      std::swap(tmp[0], tmp[3]);
                      std::swap(tmp[1], tmp[2]);
                    }
                    std::memcpy(REX_RAW_ADDR(at), tmp, bpb);
                    // The low byte of the value the GUEST will load. tmp is
                    // already in guest byte order, so for a 4-byte texel the
                    // guest's (uint8)value is the last byte of tmp.
                    ++low_hist[tmp[bpb - 1]];
                    // EVERY byte, not just the one the mip gate reads.
                    //
                    // "usable 4881" says the mip byte is sane, and that was
                    // enough to conclude the feed is ALIVE. It is not enough to
                    // conclude it is CORRECT: the page x and y ride in the
                    // other bytes of the same texel, and a wrong channel order
                    // leaves the mip plausible while putting the page
                    // coordinates somewhere else entirely.
                    //
                    // Why look now: in ground-tiles-3.rdc every resident
                    // page-table entry sits at x >= half the level width --
                    // x 512..530 of 1024 at mip 0, x 64..97 of 128 at mip 3 --
                    // while the ground being rendered samples x ~401. Two
                    // levels starting at exactly the midpoint is not a
                    // camera-shaped region; it is the shape of a coordinate
                    // with a high bit stuck on.
                    //
                    //   one byte always >= 0x80  -> the skew is in what WE
                    //                               deliver, guest innocent
                    //   all four span their range -> the addressing is the
                    //                               guest's, and IDA is next
                    //
                    // All four counted rather than one guessed: which byte
                    // carries x is not established.
                    //
                    // ONLY THE TEXELS THE GUEST ACTS ON, and cumulative across
                    // writebacks. The first cut of this measured every texel of
                    // every writeback and printed inside a cap of 8 -- so all
                    // it ever reported was the MENU, where the surface is the
                    // guest's own 0xFF clear and every byte reads FF..FF by
                    // construction. `b0[FF..FF high 4096/4096]` on all four
                    // bytes says nothing about page coordinates; it says the
                    // terrain has not drawn yet. Same log-cap trap this file
                    // already records, committed again in a new form.
                    //
                    // The guest's own gate is the right population: it skips
                    // any texel whose low byte is >= the level count, so a
                    // spread taken over the rest describes exactly the page
                    // requests it consumes, and the menu drops out on its own.
                    if (tmp[bpb - 1] < 16) {
                      for (uint32_t b = 0; b + 1 < bpb && b < 4; ++b) {
                        ++s_byteSeen[b];
                        if (tmp[b] & 0x80u) ++s_byteHigh[b];
                        if (tmp[b] < s_byteMin[b]) s_byteMin[b] = tmp[b];
                        if (tmp[b] > s_byteMax[b]) s_byteMax[b] = tmp[b];
                      }
                      // Byte +0 packs both high nibbles, exactly as the guest
                      // reads them: ROW = (+0 hi) << 8 | +2, COL = (+0 lo) << 8
                      // | +1. Bucketed to 4 each because a 1024-wide level
                      // needs 10 bits and the nibble carries the top two.
                      ++s_rowHi[(tmp[0] >> 4) & 3u];
                      ++s_colHi[tmp[0] & 3u];
                      ++s_lod[tmp[bpb - 1] & 15u];
                    }
                    ++wrote;
                  }
                }
              }
            }
            if (matched) {
              uint32_t distinct = 0, dominant = 0, dominant_n = 0, usable = 0;
              // THE ACTUAL DISTRIBUTION, not a borrowed gate.
              //
              // `usable` counts texels whose low byte is < 16, which is the
              // guest's MIP-LEVEL test in the feedback walk. For the terrain
              // deformation that byte is a HEIGHT, and the same test reads as
              // "more than half the tile is near zero" -- true, and it says
              // nothing about whether zero is right. The buffer's neutral is
              // 0x80 (sub_82AF7240 memsets it, and the accumulation's measured
              // max is 0.502 = 128/255), so min/max/mean over what we write is
              // what separates a rut from a trench.
              uint32_t vmin = 255, vmax = 0;
              uint64_t vsum = 0, vcount = 0;
              for (uint32_t v = 0; v < 256; ++v) {
                if (!low_hist[v]) continue;
                ++distinct;
                if (v < vmin) vmin = v;
                if (v > vmax) vmax = v;
                vsum += uint64_t(v) * low_hist[v];
                vcount += low_hist[v];
                if (low_hist[v] > dominant_n) {
                  dominant_n = low_hist[v];
                  dominant = v;
                }
                // A mip level the guest would act on. 16 is generous: a
                // 1024x1024 page table has 11 levels, so anything under 16 is
                // at least plausible and everything above is certainly skipped.
                if (v < 16) usable += low_hist[v];
              }
              static uint32_t s_logged = 0;
              static uint64_t s_writebacks = 0;
              ++s_writebacks;
              if (s_logged++ < 8) {
                REXLOG_INFO(
                    "d3d9: SURFACE WRITEBACK dest 0x{:08X} addr 0x{:08X} "
                    "{}x{} bpb {} tiled {} pitch {} -- wrote {} texels, {} "
                    "unwritable | GUEST-VISIBLE low byte: {} distinct, "
                    "dominant 0x{:02X} x{}, {} of {} usable (< 16)",
                    dest_texture, dest_desc.address, dest_desc.width,
                    dest_desc.height, dest_desc.bytes_per_block,
                    dest_desc.tiled ? 1 : 0, dest_desc.pitch_blocks, wrote,
                    skipped_unwritable, distinct, dominant, dominant_n, usable,
                    wrote);
              }
              // The capped line above stops after 8 and this does not, so a
              // feed that starts healthy and later goes flat is still visible.
              // WHICH DESTINATIONS actually get written, uncapped.
              //
              // The `SURFACE WRITEBACK` line stops after 8, and two
              // destinations that queue every frame consumed all eight long
              // before the terrain deformation was ever eligible. So "the
              // deform address never appears" was not evidence of anything.
              // A tiny fixed table, printed with the census, says outright
              // which addresses this path has ever written.
              //
              // PER DESTINATION, because `usable of wrote` is meaningless
              // summed across them. It was a single running total while the
              // feedback buffer was the only destination this path ever
              // reached. The moment the terrain deformation started landing,
              // its 1-byte texels -- which are ~0x00 almost everywhere, and so
              // trivially satisfy the guest's `< 16` mip gate -- were counted
              // under a heading that says "a mip the guest would act on". Run
              // 1634 printed `2,842,930 of 6,881,280` and read like the
              // feedback feed had come back from nothing; ~89% of that
              // numerator was deform bytes. The feedback buffer's own share
              // was ~7%, against 5.6% earlier in the SAME run before any
              // deform writeback existed. Two populations under one name.
              //
              // The byte spread below is not affected: its loop is
              // `b + 1 < bpb`, so a 1-byte texel contributes nothing to it.
              struct WroteTo {
                uint32_t addr;
                uint64_t count;
                uint64_t wrote;
                uint64_t usable;
                uint32_t bpb;
                uint32_t bmin;
                uint32_t bmax;
                uint64_t bsum;
                uint64_t bcount;
              };
              // READ WATCH, armed AFTER the write so the next toucher is
              // somebody else.
              //
              // Scoped to the 129x129 terrain HEIGHT buffer because that is the
              // one whose reader is in doubt: raising the readback cap to
              // deliver it (0 of 1880 -> 1562 of 1562) stopped the tile churn
              // but did not put the bike down, and the only consumer we can see
              // binds the host SNAPSHOT instead of decoding this memory. So
              // "the guest reads these bytes" is an assumption the whole path
              // rests on and has never been tested.
              //
              // A load leaves no trace unless the memory reports it -- see
              // `guest-reads-resolves-from-memory`, where the exposure came back
              // through a plain load with no call we hook.
              if (dest_desc.width == 129 && dest_desc.height == 129 &&
                  mx::watch::GuestReadWatchActive()) {
                const size_t span = size_t(dest_desc.width) *
                                    size_t(dest_desc.height) *
                                    size_t(dest_desc.bytes_per_block);
                mx::watch::ArmGuestReadWatch(
                    reinterpret_cast<void*>(REX_RAW_ADDR(dest_desc.address)),
                    span, dest_desc.address, "terrain height 129x129");
              }

              static WroteTo s_dests[8] = {};
              static bool s_destsInit = false;
              if (!s_destsInit) {
                for (auto& wd : s_dests) wd.bmin = 255;
                s_destsInit = true;
              }
              static uint64_t s_destOverflow = 0;
              bool placed = false;
              for (auto& wd : s_dests) {
                if (wd.addr == dest_desc.address || !wd.addr) {
                  wd.addr = dest_desc.address;
                  wd.bpb = dest_desc.bytes_per_block;
                  ++wd.count;
                  wd.wrote += wrote;
                  wd.usable += usable;
                  if (vcount) {
                    if (vmin < wd.bmin) wd.bmin = vmin;
                    if (vmax > wd.bmax) wd.bmax = vmax;
                    wd.bsum += vsum;
                    wd.bcount += vcount;
                  }
                  placed = true;
                  break;
                }
              }
              if (!placed) ++s_destOverflow;
              if ((s_writebacks % 120) == 0) {
                std::string dests;
                for (const auto& wd : s_dests) {
                  if (!wd.addr) continue;
                  dests += fmt::format(
                      " 0x{:08X} x{} bpb{} {}/{} usable, byte {:02X}..{:02X} "
                      "mean {:02X}",
                      wd.addr, wd.count, wd.bpb, wd.usable, wd.wrote, wd.bmin,
                      wd.bmax, wd.bcount ? uint32_t(wd.bsum / wd.bcount) : 0u);
                }
                if (s_destOverflow)
                  dests += fmt::format(" (+{} unplaced)", s_destOverflow);
                std::string spread;
                for (uint32_t b = 0; b < 4; ++b) {
                  if (!s_byteSeen[b]) continue;
                  spread += fmt::format(" b{}[{:02X}..{:02X} high {}/{}]", b,
                                        s_byteMin[b], s_byteMax[b],
                                        s_byteHigh[b], s_byteSeen[b]);
                }
                // The share, not the extremes. rowHi 0 and 1 address the
                // TOP HALF of the page table, which is exactly the half that
                // never goes resident.
                std::string lod;
                for (uint32_t l = 0; l < 16; ++l) {
                  if (!s_lod[l]) continue;
                  lod += fmt::format(" L{}={}", l, s_lod[l]);
                }
                const uint64_t rowTot = s_rowHi[0] + s_rowHi[1] + s_rowHi[2] +
                                        s_rowHi[3];
                std::string nib;
                if (rowTot) {
                  nib = fmt::format(
                      " | LOD{} | rowHi {}/{}/{}/{} ({:.3f}% in the TOP HALF)"
                      " colHi {}/{}/{}/{}",
                      lod.empty() ? std::string(" (none)") : lod,
                      s_rowHi[0], s_rowHi[1], s_rowHi[2], s_rowHi[3],
                      100.0 * double(s_rowHi[0] + s_rowHi[1]) / double(rowTot),
                      s_colHi[0], s_colHi[1], s_colHi[2], s_colHi[3]);
                }
                REXLOG_INFO(
                    "d3d9: WRITEBACK census: {} writebacks | this frame {} "
                    "distinct low bytes, dominant 0x{:02X} | ACTED-ON byte "
                    "spread{}{} | destinations written{}",
                    s_writebacks, distinct, dominant,
                    spread.empty() ? std::string(" (none yet)") : spread, nib,
                    dests.empty() ? std::string(" (none)") : dests);
              }
            }
          }
        }
        if (dest_desc.width == 1 && dest_desc.height == 1) {
          // Write the GPU's answer where the guest is about to read it.
          //
          // sub_82AFB8A8 resolves the 1x1 and then loads its bytes straight
          // out of guest memory (`lhz` at the texture base) rather than
          // sampling them, so a host-only resolve leaves it reading whatever
          // was there -- zero -- and its exposure comes out as a division by
          // zero. This is the moment to write: the resolve the guest just
          // issued is the one it is about to read. The value is the previous
          // frame's, which is what the console's own latency gives it anyway,
          // and the pass filters over time by construction.
          //
          // FMT_16_FLOAT with endian 1 is an 8-in-16 swap, so the host's
          // little-endian half goes out byte-reversed.
          //
          // Written to EVERY 1x1 destination seen, not just the one this
          // resolve names. sub_82AFB8A8 ping-pongs two of them (a1+12 and
          // a1+16, alternating on a frame counter) and READS the one it is not
          // resolving into, so writing only the current destination leaves the
          // buffer the guest actually loads untouched -- which is what the
          // first cut of this did. They are successive samples of one
          // quantity, so giving both the latest value costs the adaptation
          // one frame of history and nothing else.
          if (dest_desc.bytes_per_block == 2)
            g_luminanceDestAddrs[dest_texture] = dest_desc.address;
          const uint32_t seq =
              mx::hle::g_luminanceReadbackSeq.load(std::memory_order_acquire);
          if (seq != g_luminanceWroteSeq) {
            g_luminanceWroteSeq = seq;
            uint32_t wrote = 0, offered = 0;
            std::string all;
            {
              std::lock_guard<std::mutex> lk(
                  mx::hle::g_luminanceReadbackMutex);
              offered = mx::hle::g_luminanceReadbackCount;
              // The newest reading, whichever destination it was resolved into.
              // Every 1x1 destination is a successive sample of ONE quantity —
              // the scene's average luminance — so the latest value is the right
              // answer for all of them.
              bool have_latest = false;
              uint32_t latest_bits = 0;
              for (uint32_t i = 0; i < offered; ++i) {
                const auto& r = mx::hle::g_luminanceReadbacks[i];
                if (!g_luminanceDestAddrs.count(r.destObject)) continue;
                latest_bits = r.bits;
                have_latest = true;
              }
              // EVERY known destination, which is what the note above this
              // block has always claimed and what the code did NOT do: it
              // matched each readback to its own destObject, so it wrote one of
              // the three known destinations and left the other two at whatever
              // they held. sub_82AFB8A8 ping-pongs and READS the buffer it is
              // not resolving into, so the guest was loading a stale or zero
              // luminance, computing exposure = g_KeyValue / 0 = +Inf, and
              // parking 0x7F800000 in pixel constant c100. Every shader reading
              // it then output NaN, which is what blacks out the whole 3D layer
              // of the main menu while the UI survives.
              if (have_latest) {
                // NEVER hand the guest a zero. sub_82AFB8A8 DIVIDES by this --
                // exposure is g_KeyValue / averageLuminance -- so a zero makes
                // it +Inf, and the adaptation that consumes it is a feedback
                // filter (adapted = lerp(adapted, target, k)). Once `adapted`
                // is Inf it stays Inf for the rest of the run however good the
                // later readings are, and every shader reading the exposure
                // constant outputs NaN. Traced in capture--.rdc at draw 10012:
                // 0x7F800000 enters r1.w at instruction 52 and is NaN by 53.
                //
                // Zero is our artefact, not the scene's: it is what the
                // reduction chain reads before it has ever run. The floor is
                // g_MinLuminance (0.075), which is the value the guest's own
                // pass clamps to, so this cannot push exposure anywhere the
                // guest would not have gone by itself.
                constexpr uint32_t kMinLuminanceHalf = 0x2CCD;  // 0.075
                if ((latest_bits & 0x7FFFu) == 0) {
                  latest_bits = kMinLuminanceHalf;
                  ++g_luminanceFloored;
                }
                const uint16_t be = uint16_t(((latest_bits & 0xFFu) << 8) |
                                             ((latest_bits >> 8) & 0xFFu));
                for (const auto& [obj, addr] : g_luminanceDestAddrs) {
                  if (!HostPageReadable(REX_RAW_ADDR(addr))) continue;
                  *reinterpret_cast<uint16_t*>(REX_RAW_ADDR(addr)) = be;
                  ++wrote;
                  all += fmt::format(" 0x{:08X}=0x{:04X}", addr, latest_bits);
                }
              }
            }
            static uint64_t s_wrote = 0;
            if ((++s_wrote % 600) == 1)
              REXLOG_INFO("d3d9: EXPOSURE writeback #{} seq {} wrote {} of {} "
                          "offered ({} dests known, {} floored):{}",
                          s_wrote, seq, wrote, offered,
                          g_luminanceDestAddrs.size(), g_luminanceFloored, all);
          }
        }
        // Carried to the renderer so the snapshot is sized to the destination
        // TEXTURE rather than to the region this one resolve covers.
        dest_extent_width = dest_desc.width;
        dest_extent_height = dest_desc.height;
        if (first) {
          // DestLevel and DestSliceOrFace, which this hook has never read. A
          // resolve into a level or slice lands at base + that subresource's
          // offset, so ignoring them is a candidate explanation for the atlas
          // whose sampled base sits exactly one 4 KB page above the base
          // recorded here.
          REXLOG_INFO("d3d9: resolve dest addr 0x{:08X} (phys 0x{:08X}) {}x{} "
                      "<- texture 0x{:08X} from surface 0x{:08X} ({}x{}); "
                      "level={} slice={} destpoint={} ({},{}) srcrect={} "
                      "({},{})..({},{})",
                      dest_desc.address, physical, dest_desc.width,
                      dest_desc.height, dest_texture, source->object,
                      source->width, source->height, ctx.r8.u32, ctx.r9.u32,
                      have_dest_point, dest_point[0], dest_point[1],
                      have_src_rect, src_rect[0], src_rect[1], src_rect[2],
                      src_rect[3]);
        }
      }
    }
    // Queue the resolve itself, not just the relationship.
    //
    // Recording the mapping alone left the renderer binding the source target's
    // one live surface to every draw that sampled any texture resolved out of
    // it. One guest surface is a shared scratch buffer — six distinct textures
    // were measured resolving from a single target in one run — so all of them
    // aliased one resource and each showed whatever had been drawn most
    // recently. Queuing it here, in the same ordered list as draws, is what
    // lets the renderer take a snapshot at the right moment.
    //
    // A resolve has nothing that needs deferred shader finalisation. Putting it
    // in g_pendingHleDraws delayed it until VdSwap, while ordinary draws whose
    // shaders were already available went straight into HleFrameDraws. That
    // reversed the guest command stream: a resolve issued before a sampling
    // draw appeared after that draw in the host list, so the translated shader
    // was discarded for a snapshot that was created a few commands later.
    //
    // Every D3D9 hook holds HleGlobalMutex, so inserting directly here is both
    // ordered and synchronized with FinishHleDraw. The rare kNoCode draw is the
    // only entry that still belongs in g_pendingHleDraws.
    mx::hle::DrawCall resolve{};
    resolve.resolve_dest_texture = dest_texture;
    resolve.resolve_source_object = source->object;
    resolve.resolve_source_is_depth = (source_slot == 4);
    resolve.resolve_source_base = source->color_info & 0xFFFu;
    resolve.resolve_source_width = source->width;
    resolve.resolve_source_height = source->height;
    resolve.resolve_dest_width = dest_extent_width;
    resolve.resolve_dest_height = dest_extent_height;
    // Without an explicit destination point, the source rectangle's origin is
    // the best available answer: a banded resolve names the band's place in
    // the full image there. Zero when neither is supplied, which is the whole
    // -surface case and correct for it.
    resolve.resolve_dest_x =
        have_dest_point ? dest_point[0] : (have_src_rect ? src_rect[0] : 0);
    resolve.resolve_dest_y =
        have_dest_point ? dest_point[1] : (have_src_rect ? src_rect[1] : 0);
    if (have_src_rect) {
      resolve.resolve_src_x1 = src_rect[0];
      resolve.resolve_src_y1 = src_rect[1];
      resolve.resolve_src_x2 = src_rect[2];
      resolve.resolve_src_y2 = src_rect[3];
    }
    NoteQueueThread(GetCurrentThreadId(), true);
    NoteResolvePosition(dest_texture, mx::hle::HleFrameDraws().size());
    mx::hle::HleFrameDraws().push_back(std::move(resolve));
    // D3DRESOLVE_CLEARRENDERTARGET (0x100) is not metadata on the copy: the
    // guest implementation tests this bit after issuing the resolve and calls
    // sub_8255BA10 to clear the source EDRAM surface. Bink relies on exactly
    // this sequence: render FE_Smoke into the shared 1280x720 scratch surface,
    // resolve its 1280x430 texture, then clear the scratch before the following
    // swap resolve. Dropping the clear makes the smoke texture become the next
    // presented frame.
    if ((resolve_flags & 0x100u) && source_slot < 4) {
      mx::hle::DrawCall clear{};
      clear.clear_color_target = true;
      clear.clear_color_is_float = true;
      clear.render_target_object = source->object;
      clear.render_target_surface_info = source->surface_info;
      clear.render_target_color_info = source->color_info;
      clear.render_target_width = source->width;
      clear.render_target_height = source->height;
      clear.surface_base = source->color_info & 0xFFFu;

      // pClearColor is argument 8 (r10). A null pointer selects the runtime's
      // default float4 at 0x82012FC0, as transcribed from sub_8255BD48.
      uint32_t clear_ptr = ctx.r10.u32;
      if (!clear_ptr) clear_ptr = 0x82012FC0u;
      if (HostPageReadable(REX_RAW_ADDR(clear_ptr)) &&
          HostPageReadable(REX_RAW_ADDR(clear_ptr + 12))) {
        for (uint32_t i = 0; i < 4; ++i) {
          const uint32_t bits = REX_LOAD_U32(clear_ptr + i * 4);
          std::memcpy(&clear.clear_color_float[i], &bits, sizeof(bits));
        }
      }
      mx::hle::HleFrameDraws().push_back(std::move(clear));

      // Was a flat "first 12" cap, which spends its whole budget on whatever
      // happens at boot -- here, twelve identical (0,0,0,0) lines -- and can
      // never show a DIFFERENT colour arriving later. Keyed on the colour
      // itself instead, so each distinct clear colour reports once however late
      // it first appears. Same correction as the CLEAR log above.
      static std::set<std::array<uint32_t, 4>> s_resolveClearColors;
      const auto& cc = mx::hle::HleFrameDraws().back().clear_color_float;
      std::array<uint32_t, 4> key{};
      for (uint32_t i = 0; i < 4; ++i) std::memcpy(&key[i], &cc[i], 4);
      if (s_resolveClearColors.insert(key).second &&
          s_resolveClearColors.size() <= 32) {
        const auto& c = mx::hle::HleFrameDraws().back().clear_color_float;
        REXLOG_INFO(
            "d3d9: resolve 0x{:08X} clears source 0x{:08X} after copy to "
            "({:.3f},{:.3f},{:.3f},{:.3f}) flags=0x{:X}",
            dest_texture, source->object, c[0], c[1], c[2], c[3],
            resolve_flags);
      }
    }
    static std::map<uint64_t, uint64_t> s_resolves;
    const uint64_t key = (uint64_t(source->object) << 32) | dest_texture;
    const bool first = s_resolves.emplace(key, 0).second;
    ++s_resolves[key];
    if (first) {
      uint32_t fetch0 = 0;
      if (HostPageReadable(REX_RAW_ADDR(dest_texture + 0x1C)))
        fetch0 = REX_LOAD_U32(dest_texture + 0x1C);
      // FLAGS, in full. Xenia applies an EXPONENT BIAS on resolve --
      // RB_COPY_DEST_INFO.copy_dest_exp_bias, a signed 6-bit field at bit +16
      // (registers.h:878) read straight into `exp_bias` in GetResolveInfo. Our
      // resolve is a bitwise CopyTextureRegion and `copy_dest_exp_bias` appears
      // nowhere in this tree, so if the guest ever asks for a scale we drop it
      // and every surface sampled from that resolve reads 2^bias too dark.
      //
      // That is the missing factor the rider's arithmetic demands. Its material
      // computes saturate(tex5.y + rcp(luminance(tex4))) and the saturate pins
      // at 1, zeroing red, unless that luminance exceeds 1.03. tex4 is the
      // resolve of the ambient pre-pass, which measures 0.109 and cannot exceed
      // 0.619 -- the sum of its six light colours. A bias of +4 (x16) gives
      // 1.74 and +5 (x32) gives 3.5; either clears the bar.
      //
      // The bit position is READ OUT OF THE GUEST, not guessed. Resolve's body
      // sub_8255BD48 (0x8255CE98 is a thunk to it) builds RB_COPY_DEST_INFO and
      // stores it at device+10788:
      //
      //   v81 = (((((8 * ((v77 << 8) & 0x100 | (v38 >> 26))) | v79 & 7) << 6)
      //           | v73 & 0x3F) << 7) | ((unsigned __int8)v67 >> 6);
      //   *(_DWORD *)(a1 + 10788) = v81;
      //
      // with v38 the Flags argument. Unpacking: bits 7-12 destination format,
      // 13-15 endian, and bits 16-21 = `v38 >> 26`. Xenia's registers.h puts
      // `int32_t copy_dest_exp_bias : 6` at +16, so the field is the TOP SIX
      // BITS OF Flags, signed. An arithmetic shift of the signed word extracts
      // it directly.
      const int32_t copy_dest_exp_bias = int32_t(resolve_flags) >> 26;
      REXLOG_INFO(
          "d3d9: resolve slot {} target 0x{:08X} {}x{} -> texture "
          "0x{:08X} FLAGS=0x{:08X} EXP_BIAS={} (x{}) fetch0=0x{:08X} rect={} "
          "({},{})..({},{}) point={} ({},{})",
          source_slot, source->object, source->width, source->height,
          dest_texture, resolve_flags, copy_dest_exp_bias,
          std::exp2(float(copy_dest_exp_bias)), fetch0, have_src_rect,
          src_rect[0], src_rect[1], src_rect[2], src_rect[3], have_dest_point,
          dest_point[0], dest_point[1]);
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
    // Did the guest just ask for a surface it had previously resolved into?
    // See the note on ResolvedTargetByAddress::set_texture_binds -- this is the
    // branch point for every "produced but never drawn" defect.
    //
    // How many GUEST Draw calls happen while a resolve destination sits on a
    // sampler. D3D9DrawCounter() is bumped at the three Draw entry points
    // before any of our filtering, so this counts what the guest asked for,
    // not what we built -- which is the whole point:
    //
    //   0 draws   the guest binds it and never draws with it. Our draw path is
    //             innocent and the real consumer is somewhere else entirely.
    //   N draws   the guest DOES draw with it and we drop those draws before
    //             they ever reach the texture slot loop. Ours to fix.
    //
    // Closed out on the next SetTexture to the same sampler, which is the only
    // moment the window is known to have ended.
    {
      struct OrphanWatch {
        uint32_t object = 0;
        uint64_t draws_at_bind = 0;
      };
      static thread_local OrphanWatch t_watch[mx::hle::kMaxSamplers];
      // ACCUMULATED, not logged. The first cut of this printed a line per
      // window under a global cap of 24, and the whole cap was spent in the
      // first 0.3 seconds of the run by one Bink-era object -- so the surface
      // it was built to measure never got a line. Same trap as every other
      // capped counter in this tree: a cap shared across a population reports
      // on whoever is loudest, not on whoever is asked about.
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
  // base address and NOT restricted to resolve destinations -- see the block
  // comment on NoteVideoShapeBind. Takes a lock, but only past a shape test
  // that rejects every bind in the game bar a handful.
  //
  // Deliberately AFTER the device-fetch-file cross-check above, so it reads the
  // corrected constants. Placed before it, this would silently miss every bind
  // that took the s_device_fallback path -- and it would miss them by reading
  // `valid == false`, which is indistinguishable here from "not a texture".
  if (sampler < mx::hle::kMaxSamplers) {
    const auto& binding = DeviceState().texture[sampler];
    NoteVideoShapeBind(sampler, texture, binding.fetch, binding.valid, device);
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

//=============================================================================
// 0x82AD0FC8 - HFTerrain: APPEND ONE TRACK SEGMENT. The deform producer.
//
// The consumer side (sub_82AD49A0, below) says the guest splats exactly ONCE
// per run -- 60 track points and 4 splat triangles on the first call, then zero
// for 300+ frames of riding. This is the other end of that, found by searching
// for stores to the point-count offset 0x22A8: every one of them is in this
// function, and it is reached only through a method table (one DATA xref at
// 0x821C1F30, no direct calls), so who invokes it cannot be read statically.
//
// What it does, from the decompile: takes two segment endpoints and appends
// SIX vertices of five floats each to `obj + 7684*half + 1192`, bumping the
// float count at `+8872` thirty times, then
//
//     *(int*)(obj + 4*(half + 296)) += 2;      // obj+1184 / obj+1188
//
// two triangles per call. So the census's "60 points, 4 splat-tris" is exactly
// TWO calls to this function, ever. 30 floats x 2 = 60, 2 triangles x 2 = 4.
//
// AND IT IS GATED, on a different array from the one it fills:
//
//     if (*(uint*)(obj + 520*half + 656) < 0x40u) { ...append... }
//
// a second list (8 bytes per entry at obj + 520*half + 144, count at +656)
// capped at 64. So "the guest stopped splatting" has two possible shapes and
// the draw-side census cannot tell them apart:
//
//     never called          -> the track system is not running; guest-side,
//                              upstream of anything we do
//     called and REFUSED    -> the cap is full and nothing drains it, which is
//                              a state we might be perturbing
//
// This counts both, with the gate value, so one run separates them. Reads only;
// the original runs untouched.
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
// PURE MEASUREMENT, and it exists because the draw-side census cannot answer
// the question. `SQUARE TARGET` reports exactly ONE draw per frame into the
// 512x512 deform surface, every frame of a level, and ground-tiles.rdc shows
// that draw (event 15834, a 6-index quad = ps_hft_deform_copy) writing ZERO --
// as do both halves of the ping-pong it feeds, all 512x512 texels. So the ruts
// are missing because nothing ever seeds them, and "one draw" is consistent
// with two completely different causes:
//
//   the guest has no track segments to splat        -> guest-side, not ours
//   the guest HAS them and the splat draw is lost   -> ours
//
// The decompile of this function separates them, because the splat is behind
// its own count and its own branch:
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
// Reading those two counts is the whole hook. A run where `splat-tris` is 0 on
// every call says the guest never asks for a splat and the deform buffer is
// CORRECTLY empty from our side; a run where it is non-zero while the draw
// census still reports one draw a frame puts the loss on our path, and
// sub_82555B88 is hooked so that would be a narrow search.
//
// `tiles` is printed beside them because it is the loop bound for the whole
// pass: obj+16944 carries the tile list forward between frames, so a non-zero
// tiles with a zero point count is the guest re-copying a tile it visited
// earlier, which is exactly the one draw a frame we see.
//
// Costs two guarded loads and some arithmetic on a function that runs at most
// once per frame.
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

  // READ THE COUNTS BEFORE THE BODY RUNS. This was wrong and it may have been
  // wrong from the first version of this hook.
  //
  // They are INPUTS: the body tests `if (splat count > 0)` and draws on that.
  // The producer's own log proves they are reset every frame -- sub_82AD0FC8
  // sees `tris 0` on the first append of every frame -- so reading them AFTER
  // the body returns whatever survived the pass, and if the consumer is what
  // clears them, that is a guaranteed zero no matter what the body did. A
  // census that reads a consumed value after consumption reports "there was
  // nothing" for both "there was nothing" and "there was something and it was
  // used", which are opposite answers.
  //
  // That is the same shape as the UI defect on this branch: the draws were
  // happening and the instrument could not see them. Here the instrument reads
  // the wrong side of the call.
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

  // PER OBJECT. Run 1599 printed one counter across TWO deformation objects --
  // `extent 32` for the first 1800+ calls and `extent 2048` from 01:36:50, when
  // the level came up -- so "1 of 2100 calls had splats" was a ratio between a
  // numerator from one object and a denominator dominated by the other. The
  // reading it invited (the guest almost never lays tracks) is not supported by
  // it. See [[measure-the-right-population]]: a denominator drawn from a
  // different population is worse than none.
  //
  // Fixed table, claimed by CAS, no allocation and no lock -- this runs on
  // guest threads. Four is generous: two objects have ever been seen.
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
  // The producer (sub_82AD0FC8) writes block `obj[124]`. This render reads
  // block `!obj[124]` -- deliberately opposite, a double buffer. TERRAIN TRACK
  // APPEND says the producer appends on every call (200 of 200, 0 refused) and
  // reaches 60 points / 4 triangles every frame, while this side reads 0 every
  // frame. Both hooks are on the SAME object, so the data is not missing: it is
  // in a block nobody reads.
  //
  // Reading both halves says so outright instead of leaving it to be inferred
  // from two logs with different `half` conventions -- the producer prints
  // obj[124], this prints `obj[124] == 0`, and comparing them across two logs
  // is exactly the kind of off-by-one-convention that has cost this thread
  // whole sessions.
  // THE TWO PLAIN ALLOCATIONS, obj+112 and obj+116.
  //
  // sub_82AF7240 creates them with sub_82AB7848 (a plain allocator, 4096
  // alignment -- NOT a texture creation) and memsets both to 0x80. It then
  // calls sub_82629998(obj+8, buf0) and sub_82629998(obj+60, buf1), which is
  // D3D9's offset-the-resource-address helper: it ADDS the pointer into the
  // resource header's address fields. So the D3D9 textures at obj+8 / obj+60
  // are BACKED BY those allocations.
  //
  // And sub_82AC7850 -- the deform pass's resolve destination -- returns
  // `obj + 8 + 52 * (obj[120] == 0)`, i.e. one of exactly those two textures.
  //
  // So the deform RESOLVE DESTINATION and a plain guest allocation the guest
  // memsets to 0x80 are the same bytes. Logging the pointers ties the runtime
  // address in the resolve log (0x10C2E000) to this field, which is the last
  // inference in that chain.
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

  // POPULATION *AND* FIRES. Run 1621 printed at ran<=4 and every 300, and the
  // deform body ran fewer than 300 times in a 46-second run -- so every sample
  // came from the first three seconds, while the bike was STATIONARY. The
  // producer does not run then: sub_82AD0FC8 made two calls at 12:48:48, none
  // for the next eight seconds, and then ~11 a second once the bike moved. The
  // whole riding window went unsampled and the line still read "1 with track
  // points", which is true of the window it saw and says nothing about the one
  // that matters.
  //
  // So: every call that HAS points or splats is printed (capped), the periodic
  // sample is 60 rather than 300, and the cumulative counters carry the
  // denominator as before. A schedule that can only sample the idle part of a
  // run is not a sample of the run.
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

//-----------------------------------------------------------------------------
// 0x82557F28 / 0x82556F10 -- D3DVertexBuffer_Lock / _Unlock
//
// The guest's only route to modifying a vertex buffer: AsyncLock is not linked
// into this XEX (byte-matched against resource.obj, with the SetStreamSource
// control proving the matcher finds positives), and Lock/Unlock have 34 and 32
// callers, paired from the same functions.
//
// Unhooked until now, and the gap has already cost a theory: "is our snapshot
// stale?" could only be answered by re-reading the buffer object, never by
// knowing when the guest wrote it. The generation these bump is what lets the
// merged raw vertex buffer be reused across draws without risking stale bytes.
//
// Bumped on UNLOCK, not on Lock. A lock that has not been released has not
// necessarily written anything yet, and the draw cannot legally read the
// buffer while it is held -- so the release is the edge that matters. Bumping
// on Lock as well would only cost cache misses, but it would also hide a
// mismatch between the two counts, which is worth being able to see.
// QUALIFIED: this file is outside the namespace and reaches in with a
// using-directive, which affects lookup but not definitions -- unqualified
// here would quietly define a SECOND array at file scope and leave the real
// one undefined at link time.
std::atomic<uint32_t> mx::hooks::d3d9::g_vbGeneration[kVbGenSlots] = {};
uint64_t g_vbLocks = 0, g_vbUnlocks = 0;

REX_IMPORT(__imp__sub_82557F28, orig_VertexBufferLock, void());
extern "C" REX_FUNC(sub_82557F28) {
  ++g_vbLocks;
  orig_VertexBufferLock(ctx, base);
}

REX_IMPORT(__imp__sub_82556F10, orig_VertexBufferUnlock, void());
extern "C" REX_FUNC(sub_82556F10) {
  ++g_vbUnlocks;
  NoteVbWritten(ctx.r3.u32);
  orig_VertexBufferUnlock(ctx, base);
}
