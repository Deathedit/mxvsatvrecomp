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

#include <rex/cvar.h>

#include <array>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "gpu/d3d9_draw.h"
#include "gpu/d3d9_layout.h"
#include "gpu/d3d9_state.h"
#include "gpu/d3d9_texture.h"
#include "gpu/hle_types.h"
#include "gpu/shader_ucode.h"

#include "hooks/hooks_d3d9_internal.h"

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

//=============================================================================
// 0x8293C778 — Scaleform GFx: flush the raster glyph cache's dirty rects.
//
// The one guest call that means "the font atlas contents just changed". See
// the long note by g_glyphCacheGeneration for how it was found and why the
// texture cache cannot notice this on its own.
//
// The pending-rect count at +28 is read BEFORE the original runs, because the
// original clears it on the way out. Zero pending means this call updates
// nothing, so frames where no glyph moved cost one load and no invalidation.
//
// The atlas EXTENT at +0 and +4 is read here too, and unconditionally -- it is
// the cache's own answer to "how big are the textures I create", straight from
// sub_8293A888's InitTexture(cache[0], cache[1], 9, 0, 16). That pair is what
// lets the invalidation name the glyph atlases instead of every single-channel
// texture in the game; see the note by g_glyphCacheGeneration for the 5 MB of
// unrelated kR8 the format-only test used to sweep up. Unconditional because a
// flush with nothing pending still tells the truth about the geometry, and the
// registration is a relaxed atomic compare on all but the first call.
//=============================================================================
REX_IMPORT(__imp__sub_8293C778, orig_GlyphCacheFlush, void());
extern "C" REX_FUNC(sub_8293C778) {
  const uint32_t cache = ctx.r3.u32;
  uint32_t pending = 0;
  if (cache && HostPageReadable(REX_RAW_ADDR(cache + 28)))
    pending = REX_LOAD_U32(cache + 28);
  uint32_t atlas_w = 0, atlas_h = 0;
  if (cache && HostPageReadable(REX_RAW_ADDR(cache)) &&
      HostPageReadable(REX_RAW_ADDR(cache + 4))) {
    atlas_w = REX_LOAD_U32(cache);
    atlas_h = REX_LOAD_U32(cache + 4);
    NoteGlyphCacheGeometry(atlas_w, atlas_h);
  }

  orig_GlyphCacheFlush(ctx, base);

  if (!pending) return;
  ++g_glyphCacheGeneration;
  ++g_glyphCacheFlushes;
  if (g_glyphCacheFlushes <= 8 || (g_glyphCacheFlushes % 250) == 0) {
    // The extent is printed because it is now load-bearing, not decoration: it
    // is the whole discriminator between a glyph atlas and the 4 MB kR8 that
    // used to be invalidated alongside it. A 0x0 here means the cache object
    // did not read and every kR8 texture has fallen back to the fingerprint.
    REXLOG_INFO("d3d9: glyph cache flushed {} times ({} rects this time); "
                "atlas generation {}; atlas extent {}x{}",
                g_glyphCacheFlushes, pending, g_glyphCacheGeneration, atlas_w,
                atlas_h);
  }
}

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
  g_guestDrawCalls.fetch_add(1, std::memory_order_relaxed);
  MX_D3D9_PLUGIN_PASSTHROUGH(orig_DrawVerticesUP);
  const uint32_t device = ctx.r3.u32;
  const uint32_t primitive_type = ctx.r4.u32;
  const uint32_t vertex_count = ctx.r5.u32;
  const uint32_t vertex_data = ctx.r6.u32;
  const uint32_t stride = ctx.r7.u32;
  const uint64_t n = ++g_up_draws;
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
    static std::set<uint32_t> s_logged;
    if (s_logged.insert(target.object).second && s_logged.size() <= 32) {
      REXLOG_INFO("d3d9: CLEAR target 0x{:08X} {}x{} color=0x{:08X} "
                  "flags=0x{:X}",
                  target.object, target.width, target.height, color, flags);
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
          ++entry.resolves;
          g_resolveDestObjectPhys[dest_texture] = physical;
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

      static uint32_t s_resolveClearLogs = 0;
      if (s_resolveClearLogs++ < 12) {
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
