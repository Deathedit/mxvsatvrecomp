// Frame-lifecycle hooks.
//
// VdSwap is the interesting one: it is where the guest's PM4 command ring is
// parsed, applied to the Xenos register shadow, and translated into draw calls
// for the D3D12 renderer. The rest are the guest's Begin/End frame entry
// points, which the native path stubs out because there is no Xenos GPU behind
// them.

#include "hooks/hook_common.h"

#include "gpu/pm4_parser.h"
#include "gpu/xenos_gpu_state.h"

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
