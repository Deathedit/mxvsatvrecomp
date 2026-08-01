#include "native_graphics.h"
#include "pm4_parser.h"
#include "xenos_gpu_state.h"

#include <rex/logging.h>
#include <rex/system/thread_state.h>
#include "mx_init.h"

namespace mx::native {

// True when --gpu_plugin=<name> is requested. Hooks short-circuit to call
// their original function instead of diverting through our native path.
bool g_plugin_mode = false;

NativeGraphics& NativeGraphics::Get() {
  static NativeGraphics instance;
  return instance;
}

void SetRenderer(D3D12Renderer* renderer) {
  NativeGraphics::Get().Attach(renderer);
  REXLOG_INFO("NativeGraphics: renderer set");
}

void NativeGraphics::Shutdown() {
  m_renderer = nullptr;
}

void NativeGraphics::BeginFrame() {
  if (!m_renderer) return;
  m_renderer->BeginFrame();
}

void NativeGraphics::EndFrame() {
  if (!m_renderer) return;
  m_renderer->EndFrame();
}

void NativeGraphics::SetDrawCalls(const std::vector<mx::pm4::DrawCall>& calls) {
  std::lock_guard<std::mutex> lock(m_drawMutex);
  m_drawCalls = calls;
}

std::vector<mx::pm4::DrawCall> NativeGraphics::GetDrawCalls() {
  std::lock_guard<std::mutex> lock(m_drawMutex);
  return std::move(m_drawCalls);
}

void NativeGraphics::ClearDrawCalls() {
  std::lock_guard<std::mutex> lock(m_drawMutex);
  m_drawCalls.clear();
}

}  // namespace mx::native

namespace {

HWND g_native_hwnd = nullptr;
bool g_bink_playing = false;

// Helper: log the current value of `*(dword_830BE400 + 8)` (the engine's
// "slot 2"). In plugin mode this is populated to a real object (0x40BCF740);
// in native mode Bootstrap leaves it NULL. Triangulating which SetupRenderer
// vtable call writes it requires dumping at multiple hook points.
inline void LogEngSlot8(uint8_t* base, const char* where) {
  uint32_t eng = REX_LOAD_U32(0x830BE400);
  uint32_t slot8 = eng ? REX_LOAD_U32(eng + 8) : 0;
  REXLOG_INFO("plugin: [{}] eng+8 = 0x{:08X} (eng=0x{:08X})", where, slot8, eng);
}

}  // namespace

//=============================================================================
// Mid-ASM hook: skip game tick virtual call at 0x82B70854
//=============================================================================

void NativeGameTickSkip() {}
void NativeSetupDeviceSkip() {}
void NativeSkipVtable8() { REXLOG_INFO("native: skip vtable[8]"); }
void NativeSkipVtable17() { REXLOG_INFO("native: skip vtable[17]"); }
void NativeSkipRendererInit() { REXLOG_INFO("native: skip renderer init -> Transition thread"); }
void NativeSkipLoaderRenderer() {}

// Bisection stubs (2026-07-31) — currently UNUSED (mid-ASM hooks for these
// were commented out in mx_config.toml when the LoaderTick bisection was
// reverted and hook #6 (NativeSkipLoaderRenderer) was restored). Stubs
// remain exported (CMakeLists) for future re-use without a codegen round.
// Finding they produced: when hook #6 is disabled, NONE of these three
// stubs fire, proving execution stalls in `bctrl sub_82B3C7D0` at 0x82B70EE8
// (the lazy-init alloc) — see AGENTS.md "Hook #6 hang point CONFIRMED".

static int g_post_lazy_init_reaches = 0;
void NativePostLazyInitLog() {
  ++g_post_lazy_init_reaches;
  if (g_post_lazy_init_reaches <= 20) {
    REXLOG_INFO("native: PostLazyInit #{} (reached 0x82B70EEC, lazyinit returned)",
                g_post_lazy_init_reaches);
  }
}

static int g_pre_dispatch_reaches = 0;
void NativePreDispatchLog() {
  ++g_pre_dispatch_reaches;
  if (g_pre_dispatch_reaches <= 20) {
    REXLOG_INFO("native: PreDispatch #{} (reached loc_82B70EF0, just before bl sub_82B34998)",
                g_pre_dispatch_reaches);
  }
}

static int g_renderer_dispatch_skips = 0;
void NativeSkipRendererDispatch() {
  ++g_renderer_dispatch_skips;
  if (g_renderer_dispatch_skips <= 5 || (g_renderer_dispatch_skips % 100) == 0) {
    REXLOG_INFO("native: SkipRendererDispatch #{} (sub_82B34998 call bypassed)",
                g_renderer_dispatch_skips);
  }
}

void NativeSkipLoaderEarly() {}
void NativeSkipLoaderAll() {}

void mx::native::SetWindowHandle(HWND hwnd) {
  g_native_hwnd = hwnd;
}

void mx::native::SetBinkPlaying(bool playing) {
  g_bink_playing = playing;
}

//=============================================================================
// sub_82566B58 — VdSwap
//=============================================================================

REX_IMPORT(__imp__sub_82566B58, orig_VdSwap, void());
extern "C" REX_FUNC(sub_82566B58) {
  static int swap_count = 0;
  ++swap_count;
  if (swap_count <= 5) REXLOG_INFO("native: VdSwap #{} ENTER", swap_count);
  uint32_t cpu_val = REX_LOAD_U32(0x82D21818);
  REX_STORE_U32(0x83144208, cpu_val);
  uint32_t a1 = ctx.r3.u32;

  uint32_t pm4_write_before = REX_LOAD_U32(a1 + 48);
  uint32_t pm4_end = REX_LOAD_U32(a1 + 52);
  uint32_t pm4_base = REX_LOAD_U32(a1 + 44);

  bool is_plugin = mx::native::g_plugin_mode;
  const char* tag = is_plugin ? "plugin" : "native";

  if (swap_count == 1) {
    REXLOG_INFO("{}: PM4 ring dev+40=0x{:08X} +44=0x{:08X} +48=0x{:08X} +52=0x{:08X}",
      tag, REX_LOAD_U32(a1 + 40), pm4_base, pm4_write_before, pm4_end);
  }

  orig_VdSwap(ctx, base);

  uint32_t pm4_write_after = REX_LOAD_U32(a1 + 48);

  // Log VdSwap only at sparse checkpoints to avoid flooding during gameplay.
  bool log_this_swap = (swap_count <= 20) || (swap_count % 100 == 0);
  if (log_this_swap) {
    if (pm4_write_after >= pm4_write_before) {
      uint32_t sz = pm4_write_after - pm4_write_before;
      REXLOG_INFO("{}: VdSwap #{} wrote {} bytes at guest 0x{:08X}",
                  tag, swap_count, sz, pm4_write_before);
    } else {
      REXLOG_INFO("{}: VdSwap #{} ring WRAP (before=0x{:08X} after=0x{:08X})",
                  tag, swap_count, pm4_write_before, pm4_write_after);
    }
  }

// In plugin mode, parse boot swaps (1-20) + spot checks (300/600/1000) +
// every 100th swap after 1200 (sparse to avoid overload) — game may reach
// 3D gameplay at any time and we want to catch indexed draws.
int parse_limit = is_plugin ? 20 : 5;
bool should_parse = (swap_count <= parse_limit) ||
                    (is_plugin && (swap_count == 300 || swap_count == 600 ||
                                   swap_count == 1000)) ||
                    (is_plugin && swap_count >= 1200 && (swap_count % 100 == 0));

  if (should_parse) {
    mx::pm4::Pm4Parser parser;

    if (pm4_write_after >= pm4_write_before) {
      uint32_t sz = pm4_write_after - pm4_write_before;
      if (sz > 0 && sz < 1024 * 1024) {
        if (swap_count == 1) {
          const uint32_t* raw = reinterpret_cast<const uint32_t*>(base + pm4_write_before);
          uint32_t dump_count = (sz / 4) < 16 ? (sz / 4) : 16;
          for (uint32_t i = 0; i < dump_count; ++i) {
            REXLOG_INFO("{}: PM4 raw[{}] = 0x{:08X}  (guest: 0x{:08X})", tag, i, raw[i], _byteswap_ulong(raw[i]));
          }
        }
        parser.ParseRange(
            reinterpret_cast<const uint32_t*>(base + pm4_write_before),
            sz / 4, pm4_write_before);
      }
    } else {
      uint32_t ring_size = pm4_end - pm4_base;
      uint32_t sz1 = pm4_end - pm4_write_before;
      if (sz1 > 0 && sz1 <= ring_size) {
        parser.ParseRange(
            reinterpret_cast<const uint32_t*>(base + pm4_write_before),
            sz1 / 4, pm4_write_before);
      }
      uint32_t sz2 = pm4_write_after - pm4_base;
      if (sz2 > 0 && sz2 <= ring_size) {
        parser.ParseRange(
            reinterpret_cast<const uint32_t*>(base + pm4_base),
            sz2 / 4, pm4_base);
      }
    }

    auto& packets = parser.Packets();
    REXLOG_INFO("{}: PM4 #{}: {} packets", tag, swap_count, packets.size());

    // Only write dump files for spot-check swaps — keeps the disk clean when
    // we're parsing every swap >= 1200 looking for indexed draws.
    bool should_dump_file = (swap_count <= 20) ||
                            swap_count == 300 || swap_count == 600 ||
                            swap_count == 1000 ||
                            (swap_count >= 1200 && (swap_count % 500 == 0));
    if (should_dump_file) {
      char dumpfname[64];
      snprintf(dumpfname, sizeof(dumpfname), "pm4_dump_%s_%02d.txt", tag, swap_count);
      mx::pm4::Pm4Parser::DumpPackets(packets, dumpfname);
    }

    // Skip ApplyPackets gpu_state tracking + per-packet logging for high swap
    // counts (too noisy) — only the translator needs to run.
    if (swap_count <= 20 || swap_count == 300 || swap_count == 600 ||
        swap_count == 1000) {
      REXLOG_INFO("{}: starting ApplyPackets for {} packets", tag, packets.size());
      static mx::gpu::XenosGpuState gpu_state;
      size_t n = packets.size();
      for (size_t i = 0; i < n; ++i) {
        if ((i & 0x3FF) == 0) REXLOG_INFO("{}: packet {}/{}", tag, i, n);
        const auto& p = packets[i];
        if (p.type == mx::pm4::PacketType::Type0) {
          uint32_t cnt = p.reg_count;
          if (cnt > p.body.size()) cnt = (uint32_t)p.body.size();
          if (cnt > 0) {
            gpu_state.ApplyType0Write(p.reg_base, p.body.data(), cnt);
          }
        } else if (p.type == mx::pm4::PacketType::Type3) {
          gpu_state.ApplyType3Packet(p);
        }
      }
      REXLOG_INFO("{}: ApplyPackets done, {} regs", tag, gpu_state.Registers().size());
    }

    static mx::pm4::Pm4Translator translator;
    translator.Clear();
    translator.TranslatePackets(packets, base, 0xBEDA0000);
    auto& draws = translator.DrawCalls();
    REXLOG_INFO("{}: translator produced {} draw calls", tag, draws.size());
    // Always propagate, even when empty: empty list clears the renderer's stale
    // draw data so frames without a VdSwap don't replay the last captured draws.
    if (!is_plugin) {
      mx::native::NativeGraphics::Get().SetDrawCalls(draws);
    }

    // (gpu_state dump only ran for swap_count <= 1000 above — diff disabled for
    // high swap counts to avoid noise.)
    if (swap_count <= 20) {
      char diffpath[MAX_PATH];
      GetModuleFileNameA(nullptr, diffpath, sizeof(diffpath));
      char* difflast = strrchr(diffpath, '\\');
      if (difflast) *(difflast + 1) = '\0';
      char diffname[64];
      snprintf(diffname, sizeof(diffname), "gpu_state_diff_%s_%02d.txt", tag, swap_count);
      strcat_s(diffpath, diffname);
      FILE* df = nullptr;
      fopen_s(&df, diffpath, "w");
      if (df) { fclose(df); }
    }
  }
}

//=============================================================================
// sub_82BFBF30 — GPU spin-wait sync
//=============================================================================

REX_IMPORT(__imp__sub_82BFBF30, orig_XenosWait, void());
extern "C" REX_FUNC(sub_82BFBF30) {
  if (mx::native::g_plugin_mode) {
    orig_XenosWait(ctx, base);
    return;
  }
  REX_STORE_U32(0x83144208, REX_LOAD_U32(0x82D21818));
}

//=============================================================================
// sub_82BFB740 — NtWaitForSingleObjectEx
//=============================================================================

REX_IMPORT(__imp__sub_82BFB740, orig_Wait, void());
extern "C" REX_FUNC(sub_82BFB740) {
  if (mx::native::g_plugin_mode) { orig_Wait(ctx, base); return; }
  if (ctx.r4.u32 == 500) {
    ctx.r3.u32 = 0;
    return;
  }
  if (ctx.r4.u32 == 0xFFFFFFFF) {
    static auto t0 = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - t0).count() > 3000) {
      ctx.r3.u32 = 0;
      return;
    }
  }
  orig_Wait(ctx, base);
}

//=============================================================================
// sub_82B70760 — Main game loop
//=============================================================================

REX_IMPORT(__imp__sub_82B70760, orig_MainLoop, void());
extern "C" REX_FUNC(sub_82B70760) {
  if (mx::native::g_plugin_mode) {
    static int plm = 0;
    ++plm;
    if (plm == 1 || (plm % 600) == 0)
      REXLOG_INFO("plugin: MainLoop #{}", plm);
    // At frame 60 and 600, dump engine sub-entity state
    if (plm == 60 || plm == 600 || plm == 1800) {
      REXLOG_INFO("plugin: === engine state dump at MainLoop #{} ===", plm);
      uint32_t eng = REX_LOAD_U32(0x830BE400);
      REXLOG_INFO("plugin: eng=0x{:08X} eng+8=0x{:08X}", eng, eng ? REX_LOAD_U32(eng+8) : 0);
      // Engine sub-entity list (offsets +0x1C..+0x24 per AGENTS.md post-dispatch body)
      if (eng) {
        for (int off = 0x1C; off <= 0x24; off += 4) {
          uint32_t sub_entity_ptr = REX_LOAD_U32(eng + off);
          REXLOG_INFO("plugin: eng+0x{:X}=0x{:08X}", off, sub_entity_ptr);
          if (sub_entity_ptr) {
            uint32_t vt = REX_LOAD_U32(sub_entity_ptr);
            uint32_t f3C = REX_LOAD_U32(sub_entity_ptr + 0x3C);
            REXLOG_INFO("plugin:   vt=0x{:08X} +0x3C=0x{:08X}", vt, f3C);
          }
        }
      }
      // Entity counts
      REXLOG_INFO("plugin: pass0=0x{:08X} pass1=0x{:08X} pass2=0x{:08X}",
        REX_LOAD_U32(0x830C2150), REX_LOAD_U32(0x830C4560), REX_LOAD_U32(0x830C6970));
      // byte_82D57994 (render flag)
      REXLOG_INFO("plugin: byte_82D57994={}", REX_LOAD_U8(0x82D57994));
      // dword_830BE190
      REXLOG_INFO("plugin: dword_830BE190=0x{:08X}", REX_LOAD_U32(0x830BE190));
    }
    orig_MainLoop(ctx, base);
    return;
  }
  static int ml = 0;
  ++ml;
  REX_STORE_U8(0x82D57994, 1);
  if (ml > 600) REX_STORE_U8(0x82D57994, 0);

  if (ml == 1) {
    uint32_t eng = REX_LOAD_U32(0x830BE400);
    uint32_t sub = REX_LOAD_U32(eng + 12);
    uint32_t sub_vt = sub ? REX_LOAD_U32(sub) : 0;
    uint32_t sub_f36 = sub_vt ? REX_LOAD_U32(sub_vt + 144) : 0;
    uint32_t sub_f0 = sub_vt ? REX_LOAD_U32(sub_vt) : 0;
    // Diagnostic: dump the AssetDB global (dword_830577C0) and its vtable.
    // Goal: determine whether writing eng+8 = AssetDB would give MainLoop's
    // vt[36] call a real function (low 0x82XXXXXX address) or string data
    // (high non-function-range value that would crash on call). The Bootstrap
    // vtable's vt[36]=nullsub_1 is the current safe workaround; we want to
    // know if the real AssetDB's vt[36] is also safe to call.
    uint32_t assetdb = REX_LOAD_U32(0x830577C0);
    uint32_t assetdb_vt = assetdb ? REX_LOAD_U32(assetdb) : 0;
    uint32_t assetdb_f0 = assetdb_vt ? REX_LOAD_U32(assetdb_vt) : 0;
    uint32_t assetdb_f36 = assetdb_vt ? REX_LOAD_U32(assetdb_vt + 144) : 0;
    // Also dump the transition_renderer+8 (= dword_830EC250) which EngineInit
    // writes at 0x82ba7fe4 from sub_8253CF08. This is the slot LoaderTick's
    // (a1+8)->vtable[6] = *(a1+0x1C) gating check reads — NOT engine+8.
    uint32_t tr_assetdb = REX_LOAD_U32(0x830EC250);
    uint32_t tr_assetdb_vt = tr_assetdb ? REX_LOAD_U32(tr_assetdb) : 0;
    uint32_t tr_assetdb_f0 = tr_assetdb_vt ? REX_LOAD_U32(tr_assetdb_vt) : 0;
    uint32_t tr_assetdb_f6 = tr_assetdb_vt ? REX_LOAD_U32(tr_assetdb_vt + 24) : 0;
    REX_STORE_U32(eng + 8, eng);  // keep self-ref workaround active
    REXLOG_INFO("native: eng+12=0x{:08X} vt=0x{:08X} vt[0]=0x{:08X} vt[36]=0x{:08X}",
      sub, sub_vt, sub_f0, sub_f36);
    REXLOG_INFO("native: assetdb=0x{:08X} vt=0x{:08X} vt[0]=0x{:08X} vt[36]=0x{:08X}",
      assetdb, assetdb_vt, assetdb_f0, assetdb_f36);
    REXLOG_INFO("native: tr+8(0x830EC250)=0x{:08X} vt=0x{:08X} vt[0]=0x{:08X} vt[6]=0x{:08X}",
      tr_assetdb, tr_assetdb_vt, tr_assetdb_f0, tr_assetdb_f6);
  }

  orig_MainLoop(ctx, base);
  ctx.r3.u32 = 1;
  if ((ml % 60) == 1) REXLOG_INFO("native: MainLoop #{}", ml);
  ::Sleep(16);
}

REX_IMPORT(__imp__sub_82B70578, orig_RenderPipeline, void());
extern "C" REX_FUNC(sub_82B70578) {
  if (mx::native::g_plugin_mode) {
    static int rp = 0;
    ++rp;
    if (rp <= 5 || (rp % 600) == 0)
      REXLOG_INFO("plugin: RenderPipeline #{}", rp);
    orig_RenderPipeline(ctx, base);
    return;
  }
  static int rp = 0;
  ++rp;
  // Skip orig_RenderPipeline while Bink is playing — the host render thread
  // owns the D3D12 swapchain for Bink video. Calling guest VdSwap concurrently
  // would conflict with the host renderer's Present.
  if (g_bink_playing) {
    if (rp == 1) REXLOG_INFO("native: RenderPipeline #{} — skipped (Bink playing)", rp);
    return;
  }
  if (rp == 1 || (rp % 600) == 0)
    REXLOG_INFO("native: RenderPipeline #{} — calling orig", rp);
  orig_RenderPipeline(ctx, base);
}

//=============================================================================
// sub_82AB7848 — GpuAlloc
//=============================================================================

REX_IMPORT(__imp__sub_82AB7848, orig_GpuAlloc, void());
extern "C" REX_FUNC(sub_82AB7848) {
  if (mx::native::g_plugin_mode) {
    static int ga = 0;
    ++ga;
    uint32_t sz = ctx.r3.u32;
    orig_GpuAlloc(ctx, base);
    uint32_t addr = ctx.r3.u32;
    if (ga <= 30) {
      REXLOG_INFO("plugin: GpuAlloc #{} size=0x{:08X} -> 0x{:08X}", ga, sz, addr);
    }
    return;
  }
  static int ga = 0;
  ++ga;
  uint32_t sz = ctx.r3.u32;
  orig_GpuAlloc(ctx, base);
  uint32_t addr = ctx.r3.u32;
  if (ga <= 8) {
    REXLOG_INFO("native: GpuAlloc #{} size=0x{:08X} -> 0x{:08X}", ga, sz, addr);
  }
}

//=============================================================================
// sub_82533D80 — Cleanup1
//=============================================================================

REX_IMPORT(__imp__sub_82533D80, orig_Cleanup1, void());
extern "C" REX_FUNC(sub_82533D80) {
  if (mx::native::g_plugin_mode) { orig_Cleanup1(ctx, base); return; }
  REXLOG_INFO("native: Cleanup1 (0x82533D80)");
  REX_STORE_U32(0x830577C0, 0);
}

//=============================================================================
// sub_82B70BE8 — Cleanup2 (stubbed)
//=============================================================================

REX_IMPORT(__imp__sub_82B70BE8, orig_Cleanup2, void());
extern "C" REX_FUNC(sub_82B70BE8) {
  if (mx::native::g_plugin_mode) { orig_Cleanup2(ctx, base); return; }
  REXLOG_INFO("native: Cleanup2 (0x82B70BE8) — stubbed");
  REX_STORE_U32(0x830BE190, 0);
}

//=============================================================================
// sub_82B71148 — Renderer setup hook is now in the "GPU renderer shim"
// section below (Path 1 pre-population of dword_830BE190).
//=============================================================================

//=============================================================================
// sub_82AE9658 — Post-GraphicsInit setup (called from GraphicsInit)
//=============================================================================

REX_IMPORT(__imp__sub_82AE9658, orig_PostGfxInit, void());
extern "C" REX_FUNC(sub_82AE9658) {
  if (mx::native::g_plugin_mode) {
    REXLOG_INFO("plugin: PostGfxInit (0x82AE9658)");
    LogEngSlot8(base, "PostGfxInit ENTER");
    orig_PostGfxInit(ctx, base);
    LogEngSlot8(base, "PostGfxInit RETURNED");
    return;
  }
  REXLOG_INFO("native: PostGfxInit (0x82AE9658)");
  orig_PostGfxInit(ctx, base);
}

//=============================================================================
// sub_82373660 — Texture manager (called after GraphicsInit in SetupRenderer)
//=============================================================================

REX_IMPORT(__imp__sub_82373660, orig_TexManager, void());
extern "C" REX_FUNC(sub_82373660) {
  if (mx::native::g_plugin_mode) {
    static int tm = 0;
    ++tm;
    if (tm <= 5 || (tm % 1000) == 0) {
      REXLOG_INFO("plugin: TexManager #{} a1=0x{:08X}", tm, ctx.r3.u32);
      LogEngSlot8(base, "TexManager");
    }
    orig_TexManager(ctx, base);
    return;
  }
  REXLOG_INFO("native: TexManager (0x82373660)");
  orig_TexManager(ctx, base);
}

//=============================================================================
// sub_82B6F820 — Bind texture (called after GraphicsInit in SetupRenderer)
//=============================================================================

REX_IMPORT(__imp__sub_82B6F820, orig_BindTexture, void());
extern "C" REX_FUNC(sub_82B6F820) {
  if (mx::native::g_plugin_mode) {
    static int bt = 0;
    ++bt;
    if (bt <= 30 || (bt % 1000) == 0) {
      REXLOG_INFO("plugin: BindTexture #{} r3=0x{:08X} r4=0x{:08X} r5=0x{:08X}",
                  bt, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
      LogEngSlot8(base, "BindTexture");
    }
    orig_BindTexture(ctx, base);
    return;
  }
  REXLOG_INFO("native: BindTexture (0x82B6F820)");
  orig_BindTexture(ctx, base);
}

//=============================================================================
// Bootstrap / GraphicsInit / EngineInit (logging)
//=============================================================================

REX_IMPORT(__imp__sub_82ABB838, orig_Bootstrap, void());
extern "C" REX_FUNC(sub_82ABB838) {
  if (mx::native::g_plugin_mode) {
    LogEngSlot8(base, "Bootstrap ENTER");
    orig_Bootstrap(ctx, base);
    LogEngSlot8(base, "Bootstrap RETURNED");
    return;
  }
  REXLOG_INFO("native: Bootstrap (0x82ABB838)");
  orig_Bootstrap(ctx, base);
}

REX_IMPORT(__imp__sub_82AEBF40, orig_GraphicsInit, void());
extern "C" REX_FUNC(sub_82AEBF40) {
  if (mx::native::g_plugin_mode) {
    LogEngSlot8(base, "GraphicsInit ENTER");
    uint32_t a1 = ctx.r3.u32;
    REXLOG_INFO("plugin: GraphicsInit a1=0x{:08X}", a1);
    if (a1) {
      REXLOG_INFO("plugin: GraphicsInit dev +56=0x{:08X} +104=0x{:08X} +2388=0x{:08X}",
        REX_LOAD_U32(a1 + 56),
        REX_LOAD_U32(a1 + 104),
        REX_LOAD_U32(a1 + 2388));
    }
    orig_GraphicsInit(ctx, base);
    if (a1) {
      uint32_t gpu_base = REX_LOAD_U32(0x830B03EC);
      REXLOG_INFO("plugin: GraphicsInit done +56=0x{:08X} +104=0x{:08X} +2388=0x{:08X} gpu_phys=0x{:08X}",
        REX_LOAD_U32(a1 + 56),
        REX_LOAD_U32(a1 + 104),
        REX_LOAD_U32(a1 + 2388),
        gpu_base);
      // Dump more device fields for native backend design
      for (int off = 0; off < 2400; off += 4) {
        uint32_t val = REX_LOAD_U32(a1 + off);
        if (val != 0 && off != 56 && off != 104 && off != 2388) {
          REXLOG_INFO("plugin: dev+{}=0x{:08X}", off, val);
        }
      }
    }
    LogEngSlot8(base, "GraphicsInit RETURNED");
    return;
  }
  uint32_t a1 = ctx.r3.u32;
  REXLOG_INFO("native: GraphicsInit a1=0x{:08X}", a1);
  if (a1) {
    // Dump initial render state fields
    REXLOG_INFO("native: GraphicsInit dev +56=0x{:08X} +104=0x{:08X} +2388=0x{:08X}",
      REX_LOAD_U32(a1 + 56),
      REX_LOAD_U32(a1 + 104),
      REX_LOAD_U32(a1 + 2388));
  }
  orig_GraphicsInit(ctx, base);
  // After GraphicsInit, read what it stored
  if (a1) {
    uint32_t gpu_base = REX_LOAD_U32(0x830B03EC);
    REXLOG_INFO("native: GraphicsInit done +56=0x{:08X} +104=0x{:08X} gpu_phys=0x{:08X}",
      REX_LOAD_U32(a1 + 56),
      REX_LOAD_U32(a1 + 104),
      gpu_base);
  }
}

REX_IMPORT(__imp__sub_82BA7F58, orig_EngineInit, void());
extern "C" REX_FUNC(sub_82BA7F58) {
  if (mx::native::g_plugin_mode) {
    LogEngSlot8(base, "EngineInit ENTER");
    orig_EngineInit(ctx, base);
    LogEngSlot8(base, "EngineInit RETURNED");
    return;
  }
  mx::native::NativeGraphics::Get().SetGuestMemory(base);
  REXLOG_INFO("native: EngineInit (0x82BA7F58)");
  REXLOG_INFO("native: EngineInit — calling orig");
  orig_EngineInit(ctx, base);
  REXLOG_INFO("native: EngineInit returned — keeping alive");
  for (;;) {
    ::Sleep(16);
  }
}

//=============================================================================
// GPU call stubs
//=============================================================================

REX_IMPORT(__imp__sub_8255D430, orig_BeginFrameXenos, void());
extern "C" REX_FUNC(sub_8255D430) {
  if (mx::native::g_plugin_mode) { orig_BeginFrameXenos(ctx, base); return; }
}

REX_IMPORT(__imp__sub_8255D470, orig_EndFrameXenos, void());
extern "C" REX_FUNC(sub_8255D470) {
  if (mx::native::g_plugin_mode) { orig_EndFrameXenos(ctx, base); return; }
}

REX_IMPORT(__imp__sub_8255D520, orig_GpuStateXenos, void());
extern "C" REX_FUNC(sub_8255D520) {
  if (mx::native::g_plugin_mode) {
    static int gs = 0;
    ++gs;
    if (gs <= 3)
      REXLOG_INFO("plugin: GpuState #{}", gs);
    orig_GpuStateXenos(ctx, base);
    return;
  }
  static int gs = 0;
  ++gs;
  if (gs <= 3) {
    REXLOG_INFO("native: GpuState #{} — calling orig", gs);
    orig_GpuStateXenos(ctx, base);
    REXLOG_INFO("native: GpuState #{} — returned", gs);
  }
}

REX_IMPORT(__imp__sub_82BFBF48, orig_ErrorRecovery, void());
extern "C" REX_FUNC(sub_82BFBF48) {}

//=============================================================================

//=============================================================================
// sub_82ABF828 — Begin frame (stubbed — accesses XenonRenderer at gs+80)
//=============================================================================

REX_IMPORT(__imp__sub_82ABF828, orig_BeginFrame, void());
extern "C" REX_FUNC(sub_82ABF828) {
  if (mx::native::g_plugin_mode) {
    static int bf = 0;
    ++bf;
    if (bf <= 3 || (bf % 600) == 0)
      REXLOG_INFO("plugin: BeginFrame #{}", bf);
    orig_BeginFrame(ctx, base);
    return;
  }
}

//=============================================================================
// sub_82ABF930 — End frame / VdSwap caller
//=============================================================================

REX_IMPORT(__imp__sub_82ABF930, orig_EndFrame, void());
extern "C" REX_FUNC(sub_82ABF930) {
  if (mx::native::g_plugin_mode) {
    static int ef = 0;
    ++ef;
    if (ef <= 3 || (ef % 600) == 0)
      REXLOG_INFO("plugin: EndFrame #{}", ef);
    orig_EndFrame(ctx, base);
    return;
  }
  static int ef = 0;
  ++ef;
  orig_EndFrame(ctx, base);
}

//=============================================================================
// sub_8255CFE0 — GPU frame-pending poll (spin loop in VdSwap at 0x82567178).
// Without GPU, the counter never advances → infinite spin. Stub to return 0
// ("not pending") to break the loop and let EndFrame #2+ complete.
//=============================================================================
REX_IMPORT(__imp__sub_8255CFE0, orig_FramePendingPoll, int());
extern "C" REX_FUNC(sub_8255CFE0) {
  if (mx::native::g_plugin_mode) { orig_FramePendingPoll(ctx, base); return; }
  // Native: no GPU to poll — always "not pending".
  ctx.r3.u32 = 0;
}

//=============================================================================
// GPU renderer shim (Path 2) — DIAGNOSTIC HOOKS DISABLED.
//
// Three hooks were trialed during Path 1/2 experimentation: sub_82B2C9D0 (TLS
// gate), sub_82B307D8 (NULL-deref bypass), and a SetupRenderer pre-population
// of dword_830BE190. They are NOT needed in the baseline — mid-ASM hook #6
// skips sub_82B34998 entirely. Findings preserved in AGENTS.md "PATH 1
// EXPERIMENT" and "Path 2" sections. Re-enabling requires codegen.
//=============================================================================

//=============================================================================
// sub_82B71148 — Renderer setup (called BEFORE MainLoop)
//=============================================================================

REX_IMPORT(__imp__sub_82B71148, orig_SetupRenderer, void());
extern "C" REX_FUNC(sub_82B71148) {
  if (mx::native::g_plugin_mode) {
    LogEngSlot8(base, "SetupRenderer ENTER");
    REXLOG_INFO("plugin: SetupRenderer ENTER a1=0x{:08X}", ctx.r3.u32);
    orig_SetupRenderer(ctx, base);
    LogEngSlot8(base, "SetupRenderer RETURNED");
    REXLOG_INFO("plugin: SetupRenderer RETURNED — dumping state");
    // Dump engine state
    uint32_t eng = REX_LOAD_U32(0x830BE400);
    REXLOG_INFO("plugin: eng(0x830BE400)=0x{:08X}", eng);
    if (eng) {
      for (int off = 0; off <= 40; off += 4)
        REXLOG_INFO("plugin: eng+{}=0x{:08X}", off, REX_LOAD_U32(eng + off));
    }
    // Dump transition renderer state
    uint32_t tr = 0x830EC248;
    REXLOG_INFO("plugin: transition_renderer+8(AssetDB)=0x{:08X}", REX_LOAD_U32(0x830EC250));
    REXLOG_INFO("plugin: transition_renderer+0x190=0x{:08X} +0x194=0x{:08X} +0x2DC=0x{:08X} +0x2E0=0x{:08X}",
      REX_LOAD_U32(tr + 0x190), REX_LOAD_U32(tr + 0x194),
      REX_LOAD_U32(tr + 0x2DC), REX_LOAD_U32(tr + 0x2E0));
    // dword_830BE190 — the 60KB block
    uint32_t be190 = REX_LOAD_U32(0x830BE190);
    REXLOG_INFO("plugin: dword_830BE190=0x{:08X}", be190);
    if (be190) {
      REXLOG_INFO("plugin: BE190+0=0x{:08X} +4=0x{:08X} +8=0x{:08X} +68=0x{:08X} +72=0x{:08X}",
        REX_LOAD_U32(be190), REX_LOAD_U32(be190+4), REX_LOAD_U32(be190+8),
        REX_LOAD_U32(be190+68), REX_LOAD_U32(be190+72));
      uint32_t vt = REX_LOAD_U32(be190);
      REXLOG_INFO("plugin: BE190 vtable=0x{:08X}", vt);
      if (vt >= 0x82000000) {
        for (int i = 0; i < 20; ++i)
          REXLOG_INFO("plugin: BE190 vt[{}]=0x{:08X}", i, REX_LOAD_U32(vt + i*4));
      }
    }
    // Entity count globals
    REXLOG_INFO("plugin: pass0_count(0x830C2150)={} pass1_count(0x830C4560)={} pass2_count(0x830C6970)={}",
      REX_LOAD_U32(0x830C2150), REX_LOAD_U32(0x830C4560), REX_LOAD_U32(0x830C6970));
    // GPU physical base
    REXLOG_INFO("plugin: gpu_phys(0x830B03EC)=0x{:08X}", REX_LOAD_U32(0x830B03EC));
    return;
  }
  REXLOG_INFO("native: SetupRenderer ENTER (0x82B71148)");
  orig_SetupRenderer(ctx, base);
  REXLOG_INFO("native: SetupRenderer RETURNED");

  // Native backend fix: SetupRenderer's vt[17] call (sub_82B43AC8 @ 0x82B71310)
  // is skipped by mid-ASM hook #4. That call writes `*(eng+8) = assetdb_block`
  // (the 545KB block allocated at 0x82B712D8 by sub_82AB73C0(0x85280) and
  // initialized by AssetDB_InnerCtor_VtableInstall at 0x82B712EC). The
  // allocation + ctor themselves actually run in native mode (hook #3 only
  // skips the vt[8] call before them; hook #4 skips vt[17] AFTER them).
  // However, since hook #4 prevents vt[17] from running, eng+8 stays NULL.
  //
  // We replicate vt[17]'s `*(eng+8) = assetdb_block` write here from C++.
  // The 545KB block allocated during orig_SetupRenderer is gone (no global
  // references it unless vt[17] ran to write eng+8). So we re-allocate it
  // ourselves and call AssetDB_InnerCtor_VtableInstall to set up the same
  // vtable its natural code would, then write eng+8.
  //
  // We SKIP the secondary sub_82526D10 (18-subsystem AssetDB registration)
  // call that vt[17] would have made — those subsystems depend on plugin-
  // provided state we don't have, and registering them risks crashes for
  // unclear benefit (assets are loaded host-side in our native path).
  uint32_t eng = REX_LOAD_U32(0x830BE400);
  if (eng && !REX_LOAD_U32(eng + 8)) {
    REXLOG_INFO("native: eng+8 is NULL — replicating vt[17] write from C++");
    // Allocate 545KB block (same size as SetupRenderer natural code 0x85280)
    uint32_t saved_r3 = ctx.r3.u32;
    uint32_t saved_r4 = ctx.r4.u32;
    ctx.r3.u32 = 0x85280;
    REX_CALL_INDIRECT_FUNC(0x82AB73C0);  // sub_82AB73C0(0x85280) — heap alloc
    uint32_t block_ptr = ctx.r3.u32;
    ctx.r3.u32 = saved_r3;
    ctx.r4.u32 = saved_r4;
    if (block_ptr) {
      REXLOG_INFO("native: AssetDB block alloc'd at 0x{:08X}", block_ptr);
      // Call AssetDB_InnerCtor_VtableInstall (sub_82BAB700) — installs vtable
      // off_8214518C at *(block+0) and calls sub_82AB7560(block + 342*4) for
      // inner init. Takes block_ptr as a1 (r3).
      uint32_t ctor_saved_r3 = ctx.r3.u32;
      uint32_t ctor_saved_r4 = ctx.r4.u32;
      ctx.r3.u32 = block_ptr;
      REX_CALL_INDIRECT_FUNC(0x82BAB700);  // AssetDB_InnerCtor_VtableInstall
      ctx.r3.u32 = ctor_saved_r3;
      ctx.r4.u32 = ctor_saved_r4;
      // Write eng+8 = block_ptr (the critical vt[17] effect)
      REX_STORE_U32(eng + 8, block_ptr);
      REXLOG_INFO("native: eng+8 written to 0x{:08X} (was NULL)", block_ptr);
    } else {
      REXLOG_INFO("native: WARNING — 545KB AssetDB block alloc failed");
    }
  } else if (eng) {
    REXLOG_INFO("native: eng+8 already populated (0x{:08X})", REX_LOAD_U32(eng + 8));
  } else {
    REXLOG_INFO("native: WARNING — dword_830BE400 (engine) is NULL");
  }
}

//=============================================================================
// sub_82B710D0 — Transition (overridden: skip NtSetEvent block)
//=============================================================================

REX_IMPORT(__imp__sub_82B710D0, orig_Transition, void());
extern "C" REX_FUNC(sub_82B710D0) {
  if (mx::native::g_plugin_mode) {
    static int pt = 0;
    ++pt;
    if (pt <= 3)
      REXLOG_INFO("plugin: Transition #{}", pt);
    orig_Transition(ctx, base);
    if (pt <= 3)
      REXLOG_INFO("plugin: Transition #{} returned", pt);
    return;
  }
  REXLOG_INFO("native: Transition (0x82B710D0)");
  orig_Transition(ctx, base);
  REXLOG_INFO("native: Transition returned");
}

//=============================================================================
// sub_82BFB748 — NtSetEvent wrapper (call orig, events needed for loading)
//=============================================================================

REX_IMPORT(__imp__sub_82BFB748, orig_SetEvent, void());
extern "C" REX_FUNC(sub_82BFB748) {
  if (mx::native::g_plugin_mode) { orig_SetEvent(ctx, base); return; }
  orig_SetEvent(ctx, base);
}

//=============================================================================
// sub_82B70DE8 — LoaderTick (stubbed: crashes without GPU renderer)
//=============================================================================

REX_EXTERN(__imp__sub_82B70DE8);
REX_HOOK_RAW(sub_82B70DE8) {
  if (mx::native::g_plugin_mode) {
    static int plt = 0;
    ++plt;
    __imp__sub_82B70DE8(ctx, base);
    if (plt <= 10 || (plt % 10000) == 0)
      REXLOG_INFO("plugin: LoaderTick #{} r3={}", plt, ctx.r3.u32);
    return;
  }
  static int lt = 0;
  ++lt;
  __imp__sub_82B70DE8(ctx, base);
  // Per IDA decompile of sub_82B70DE8 tail: `if (result) { renderer block; }`
  // gate is bare `if(result)` — ANY nonzero r3 triggers the renderer. The
  // previous `r3 == 1` cap only matched one code path and would miss other
  // nonzero returns (e.g. wait timeouts, pointer values from vtable[6]).
  if (ctx.r3.u32 != 0 && lt > 100) {
    ctx.r3.u32 = 0;
  }
  if (lt <= 5 || lt == 101 || lt % 1000 == 0)
    REXLOG_INFO("native: LoaderTick #{} r3={}", lt, ctx.r3.u32);
}

//=============================================================================
// PLUGIN-MODE DIAGNOSTIC HOOKS — log critical functions that fail in native
// mode. These only fire when --gpu_plugin=xenos (g_plugin_mode=true). In
// native mode they're unreachable (mid-ASM hooks skip the calling code).
//=============================================================================

// sub_82B34998 — RendererDispatchBlock (fatal vtable dispatches in native)
REX_IMPORT(__imp__sub_82B34998, orig_RendererDispatch, void());
extern "C" REX_FUNC(sub_82B34998) {
  if (mx::native::g_plugin_mode) {
    static int rd = 0;
    ++rd;
    if (rd <= 20 || (rd % 500) == 0)
      REXLOG_INFO("plugin: RendererDispatch #{} a1=0x{:08X} f1={:.2f}",
                  rd, ctx.r3.u32, *(float*)&ctx.f1);
    orig_RendererDispatch(ctx, base);
    if (rd <= 20 || (rd % 500) == 0)
      REXLOG_INFO("plugin: RendererDispatch #{} returned r3=0x{:08X}", rd, ctx.r3.u32);
    return;
  }
  orig_RendererDispatch(ctx, base);
}

// sub_82B3C7D0 — lazy-init alloc (hangs in Transition thread in native)
REX_IMPORT(__imp__sub_82B3C7D0, orig_LazyInit, void());
extern "C" REX_FUNC(sub_82B3C7D0) {
  if (mx::native::g_plugin_mode) {
    static int li = 0;
    ++li;
    if (li <= 10) {
      REXLOG_INFO("plugin: LazyInit(sub_82B3C7D0) #{}", li);
      LogEngSlot8(base, "LazyInit ENTER");
    }
    orig_LazyInit(ctx, base);
    if (li <= 10) {
      REXLOG_INFO("plugin: LazyInit #{} returned r3=0x{:08X}", li, ctx.r3.u32);
      LogEngSlot8(base, "LazyInit RETURNED");
    }
    return;
  }
  orig_LazyInit(ctx, base);
}

// sub_82B70370 — timing function (busy-wait spin in native)
REX_IMPORT(__imp__sub_82B70370, orig_Timing, void());
extern "C" REX_FUNC(sub_82B70370) {
  if (mx::native::g_plugin_mode) {
    static int tm = 0;
    ++tm;
    if (tm <= 5 || (tm % 1000) == 0)
      REXLOG_INFO("plugin: Timing(sub_82B70370) #{} a1=0x{:08X}", tm, ctx.r3.u32);
    orig_Timing(ctx, base);
    return;
  }
  orig_Timing(ctx, base);
}

// sub_8253AA40 — AssetDB_LoadStateMachine (LoaderTick's gate, 12-state)
REX_IMPORT(__imp__sub_8253AA40, orig_LoadStateMachine, void());
extern "C" REX_FUNC(sub_8253AA40) {
  if (mx::native::g_plugin_mode) {
    static int sm = 0;
    ++sm;
    uint32_t a1 = ctx.r3.u32;
    uint32_t state = a1 ? REX_LOAD_U32(a1 + 110796) : 0;
    if (sm <= 50 || (sm % 200) == 0)
      REXLOG_INFO("plugin: LoadStateMachine #{} a1=0x{:08X} state={}", sm, a1, state);
    orig_LoadStateMachine(ctx, base);
    if (sm <= 50 || (sm % 200) == 0)
      REXLOG_INFO("plugin: LoadStateMachine #{} returned r3=0x{:08X}", sm, ctx.r3.u32);
    return;
  }
  orig_LoadStateMachine(ctx, base);
}

// sub_82B38558 — TerminatorVtableCtor (installs off_8213F70C vtable)
REX_IMPORT(__imp__sub_82B38558, orig_VtableCtor, void());
extern "C" REX_FUNC(sub_82B38558) {
  if (mx::native::g_plugin_mode) {
    LogEngSlot8(base, "VtableCtor ENTER");
    REXLOG_INFO("plugin: VtableCtor(sub_82B38558) a1=0x{:08X}", ctx.r3.u32);
    orig_VtableCtor(ctx, base);
    uint32_t a1 = ctx.r3.u32;
    uint32_t vt = a1 ? REX_LOAD_U32(a1) : 0;
    REXLOG_INFO("plugin: VtableCtor done r3=0x{:08X} *a1->vt=0x{:08X}", ctx.r3.u32, vt);
    LogEngSlot8(base, "VtableCtor RETURNED");
    return;
  }
  orig_VtableCtor(ctx, base);
}

