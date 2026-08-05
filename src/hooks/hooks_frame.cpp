// Frame-lifecycle hooks.
//
// VdSwap is the interesting one: it is where the guest's PM4 command ring is
// parsed, applied to the Xenos register shadow, and translated into draw calls
// for the D3D12 renderer. The rest are the guest's Begin/End frame entry
// points, which the native path stubs out because there is no Xenos GPU behind
// them.
//
// Two ranges, and the distinction matters more than anything else in this file:
//
//   frame range  [prev_after, write_before)  — everything the guest wrote since
//                                              the last swap. This is the frame.
//   swap range   [write_before, write_after) — what VdSwap itself emits.
//
// Until 2026-08-02 only the swap range was parsed, so every "zero DRAW_*"
// result in this effort was measured over a present sequence: DISPLAY_TIMING,
// DISP_TG_CTL, EVENT_WRITE_SHD, and a SET_LOOP_CONST whose first data word is
// 0x53574150 — ASCII "SWAP". A draw could not have appeared there. The frame
// range ran ~11552 bytes per frame at boot and was never looked at.

#include "hooks/hook_common.h"
#include "hooks/hooks_d3d9.h"

#include <chrono>

#include "gpu/d3d9_draw.h"
#include "gpu/d3d9_state.h"

#include "gpu/pm4_parser.h"
#include "gpu/xenos_gpu_state.h"

//=============================================================================
// sub_82566B58 — D3D9's swap, NOT VdSwap itself
//
// The kernel VdSwap is the import thunk at 0x82CE9F98 and is already named by
// the recompiler as __imp__VdSwap. sub_82566B58 *calls* it: the call site
// returns to 0x82566E1C, and the next function after 0x82566B58 is 0x825671E0,
// so it is inside this body. D3D9 is statically linked into the XEX and nothing
// but D3D9's present path calls VdSwap, which makes this a confirmed anchor
// inside the D3D9 library block — see the D3D9 note in AGENTS.md.
//
// Hooking here is still right: it is the frame boundary with the ring buffer
// state we need. Only the name was wrong.
//=============================================================================

// Defined in src/app/graphics_system.cpp.
REXCVAR_DECLARE(bool, hle_render);

namespace {

// Type3 opcode histogram for a parsed range. This is what says whether draws
// are inline (0x22/0x34/0x35/0x36), hidden behind INDIRECT_BUFFER (0x3F/0x37),
// or simply absent — the translator ignores IB dispatches today, so a range
// full of them would read as "no draws" without this.
void LogOpcodeHistogram(const char* tag, const char* range, int swap_count,
                        const std::vector<mx::pm4::Pm4Packet>& packets) {
  uint32_t counts[128] = {};
  uint32_t type0 = 0, type2 = 0;
  for (const auto& p : packets) {
    if (p.type == mx::pm4::PacketType::Type3) counts[p.opcode & 0x7F]++;
    else if (p.type == mx::pm4::PacketType::Type0) ++type0;
    else if (p.type == mx::pm4::PacketType::Type2) ++type2;
  }
  REXLOG_INFO("{}: hist #{} {} — Type0={} Type2={}", tag, swap_count, range,
              type0, type2);
  for (uint32_t op = 0; op < 128; ++op) {
    if (!counts[op]) continue;
    const char* name = mx::pm4::Pm4Parser::OpcodeName(op);
    REXLOG_INFO("{}: hist #{} {} — Type3 0x{:02X} {} x{}", tag, swap_count,
                range, op, name ? name : "???", counts[op]);
  }
}

}  // namespace

REX_IMPORT(__imp__sub_82566B58, orig_VdSwap, void());
extern "C" REX_FUNC(sub_82566B58) {
  static int swap_count = 0;
  ++swap_count;

  // Frame period, swap to swap. The first swap has no predecessor and is not
  // counted — otherwise the whole of startup lands in the first "frame" and
  // drags the mean somewhere meaningless.
  {
    static std::chrono::steady_clock::time_point s_last{};
    const auto now = std::chrono::steady_clock::now();
    if (s_last.time_since_epoch().count() != 0) {
      ++mx::pm4::D3D9FrameCount();
      mx::pm4::D3D9FrameNanos() +=
          uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                       now - s_last)
                       .count());
    }
    s_last = now;
  }

  if (swap_count <= 5) REXLOG_INFO("native: VdSwap #{} ENTER", swap_count);
  uint32_t cpu_val = REX_LOAD_U32(0x82D21818);
  REX_STORE_U32(0x83144208, cpu_val);
  uint32_t a1 = ctx.r3.u32;

  uint32_t pm4_write_before = REX_LOAD_U32(a1 + 48);

  bool is_plugin = mx::native::g_plugin_mode;
  const char* tag = is_plugin ? "plugin" : "native";

  // Ring bounds are not established. The fields this code used to read as base
  // and end (+44 and +52) logged 0x00000000 and 0xBEBA0000 at swap #1, but
  // packets parse at 0xBEBB3260 and later swaps write at 0xBED0653C — so
  // neither means what was assumed, and the old wrap arithmetic
  // (ring_size = end - base) was garbage. Dump the struct once so the real
  // fields can be identified by matching them against the observed span, and
  // until then refuse to parse a wrapped range rather than fabricate packets
  // from wrong bounds.
  if (swap_count == 1) {
    for (uint32_t off = 0; off <= 96; off += 16) {
      REXLOG_INFO("{}: VdSwap dev+{:3} = 0x{:08X} 0x{:08X} 0x{:08X} 0x{:08X}",
                  tag, off, REX_LOAD_U32(a1 + off), REX_LOAD_U32(a1 + off + 4),
                  REX_LOAD_U32(a1 + off + 8), REX_LOAD_U32(a1 + off + 12));
    }
  }

  orig_VdSwap(ctx, base);

  uint32_t pm4_write_after = REX_LOAD_U32(a1 + 48);

  static uint32_t s_prev_after = 0;
  static uint32_t s_ptr_min = 0xFFFFFFFFu;
  static uint32_t s_ptr_max = 0;
  if (pm4_write_before < s_ptr_min) s_ptr_min = pm4_write_before;
  if (pm4_write_after > s_ptr_max) s_ptr_max = pm4_write_after;

  // The frame range is only usable when the write pointer moved forward across
  // the whole inter-swap span. A wrap is reported and skipped, not guessed at.
  const bool frame_wrapped = s_prev_after != 0 && pm4_write_before < s_prev_after;
  uint32_t frame_start = s_prev_after;
  uint32_t frame_size =
      (!frame_wrapped && s_prev_after != 0 && pm4_write_before > s_prev_after)
          ? pm4_write_before - s_prev_after
          : 0;
  if (frame_size >= 1024 * 1024) frame_size = 0;

  // Log VdSwap at sparse checkpoints, plus the first 40 swaps and every wrap
  // (either range) so the ring span can be pinned down from one run.
  bool log_this_swap = (swap_count <= 40) || (swap_count % 100 == 0) ||
                       frame_wrapped || (pm4_write_after < pm4_write_before);
  if (log_this_swap) {
    if (pm4_write_after >= pm4_write_before) {
      REXLOG_INFO("{}: VdSwap #{} frame [0x{:08X}+{}]{} swap [0x{:08X}+{}] "
                  "ptr span 0x{:08X}..0x{:08X}",
                  tag, swap_count, frame_start, frame_size,
                  frame_wrapped ? " WRAPPED-SKIPPED" : "", pm4_write_before,
                  pm4_write_after - pm4_write_before, s_ptr_min, s_ptr_max);
    } else {
      REXLOG_INFO("{}: VdSwap #{} ring WRAP (before=0x{:08X} after=0x{:08X}) "
                  "ptr span 0x{:08X}..0x{:08X}",
                  tag, swap_count, pm4_write_before, pm4_write_after, s_ptr_min,
                  s_ptr_max);
    }
  }

  // Native mode now parses every swap: the load completes around swap ~600 and
  // the old native limit of 5 meant nothing after boot was ever examined. The
  // expensive extras below (gpu_state, file dumps) stay on their sparse
  // schedules. Plugin mode keeps its original cadence.
  bool should_parse = !is_plugin || (swap_count <= 20) ||
                      (swap_count == 300 || swap_count == 600 ||
                       swap_count == 1000) ||
                      (swap_count >= 1200 && (swap_count % 100 == 0));

  if (should_parse) {
    mx::pm4::Pm4Parser frame_parser;
    if (frame_size > 0) {
      frame_parser.ParseRange(
          reinterpret_cast<const uint32_t*>(base + frame_start),
          frame_size / 4, frame_start);
    }

    mx::pm4::Pm4Parser swap_parser;
    if (pm4_write_after > pm4_write_before) {
      uint32_t sz = pm4_write_after - pm4_write_before;
      if (sz < 1024 * 1024) {
        if (swap_count == 1) {
          const uint32_t* raw = reinterpret_cast<const uint32_t*>(base + pm4_write_before);
          uint32_t dump_count = (sz / 4) < 16 ? (sz / 4) : 16;
          for (uint32_t i = 0; i < dump_count; ++i) {
            REXLOG_INFO("{}: PM4 raw[{}] = 0x{:08X}  (guest: 0x{:08X})", tag, i, raw[i], _byteswap_ulong(raw[i]));
          }
        }
        swap_parser.ParseRange(
            reinterpret_cast<const uint32_t*>(base + pm4_write_before),
            sz / 4, pm4_write_before);
      }
    }

    auto& frame_packets = frame_parser.Packets();
    auto& swap_packets = swap_parser.Packets();

    // Only write dump files for spot-check swaps — keeps the disk clean when
    // we're parsing every swap looking for indexed draws.
    bool should_dump_file = (swap_count <= 20) ||
                            swap_count == 300 || swap_count == 600 ||
                            swap_count == 1000 ||
                            (swap_count >= 1200 && (swap_count % 500 == 0));
    if (should_dump_file) {
      char dumpfname[64];
      snprintf(dumpfname, sizeof(dumpfname), "pm4_dump_%s_frame_%02d.txt", tag, swap_count);
      mx::pm4::Pm4Parser::DumpPackets(frame_packets, dumpfname);
      snprintf(dumpfname, sizeof(dumpfname), "pm4_dump_%s_swap_%02d.txt", tag, swap_count);
      mx::pm4::Pm4Parser::DumpPackets(swap_packets, dumpfname);
      LogOpcodeHistogram(tag, "frame", swap_count, frame_packets);
      LogOpcodeHistogram(tag, "swap", swap_count, swap_packets);
    }

    // Skip ApplyPackets gpu_state tracking + per-packet logging for high swap
    // counts (too noisy) — only the translator needs to run.
    if (swap_count <= 20 || swap_count == 300 || swap_count == 600 ||
        swap_count == 1000) {
      static mx::gpu::XenosGpuState gpu_state;
      for (const auto* list : {&frame_packets, &swap_packets}) {
        for (const auto& p : *list) {
          if (p.type == mx::pm4::PacketType::Type0) {
            uint32_t cnt = p.reg_count;
            if (cnt > p.body.size()) cnt = (uint32_t)p.body.size();
            if (cnt > 0) gpu_state.ApplyType0Write(p.reg_base, p.body.data(), cnt);
          } else if (p.type == mx::pm4::PacketType::Type3) {
            gpu_state.ApplyType3Packet(p);
          }
        }
      }
      REXLOG_INFO("{}: ApplyPackets done, {} regs", tag, gpu_state.Registers().size());
    }

    // One translator, both ranges, frame first — that is the order the guest
    // wrote them, and state-setting packets in the frame must be seen before
    // the swap's.
    static mx::pm4::Pm4Translator translator;
    translator.Clear();
    translator.TranslatePackets(frame_packets, base, 0xBEDA0000);
    translator.TranslatePackets(swap_packets, base, 0xBEDA0000);
    auto& draws = translator.DrawCalls();

    //-----------------------------------------------------------------------
    // Does the ring carry the same number of draws D3D9 was asked for?
    //
    // This is the last route left from a D3D9 shader handle to its microcode.
    // The direct ones are all closed: the blob at +0x368 is not the code (23%
    // agreement against what the ring loaded), SH_pPhysical reads as zeros at
    // bind time, and its address is not the ring key (33 handles landed on a
    // key at +0x1040 and none of them read back that microcode). But the ring
    // does load 41 shaders *by address*, so the code is in guest memory — only
    // the mapping is missing.
    //
    // If each frame's ring draw count equals its D3D9 draw count, the Nth ring
    // draw is the Nth D3D9 draw, and the shader the ring bound for it is the
    // one this handle means. If they differ, that correspondence is not there
    // and saying so is the result — which is why this counts before assuming
    // rather than assuming and checking later.
    //
    // Counted from the *raw draw packets*, not from DrawCalls(): the translator
    // drops draws it cannot build, so its output would understate the ring and
    // manufacture a mismatch that is really a filter.
    //-----------------------------------------------------------------------
    {
      // Per opcode, not as one total: the ring has four draw opcodes and two of
      // them are the binned forms, which replay a draw per bin. A bare total
      // cannot tell that apart from D3D9 issuing draws the game never asked
      // for, and those point opposite ways.
      uint64_t op[4] = {};   // 0x22, 0x34, 0x35, 0x36
      auto count_draw_packets = [&](const std::vector<mx::pm4::Pm4Packet>& v) {
        for (const auto& p : v) {
          if (p.type != mx::pm4::PacketType::Type3) continue;
          switch (p.opcode) {
            case 0x22: ++op[0]; break;
            case 0x34: ++op[1]; break;
            case 0x35: ++op[2]; break;
            case 0x36: ++op[3]; break;
            default: break;
          }
        }
      };
      count_draw_packets(frame_packets);
      count_draw_packets(swap_packets);
      const uint64_t ring = op[0] + op[1] + op[2] + op[3];

      static uint64_t s_lastD3d9 = 0, s_lastIndexed = 0;
      const uint64_t now = mx::pm4::D3D9DrawCounter();
      const uint64_t now_idx = mx::pm4::D3D9IndexedDrawCounter();
      const uint64_t d3d9 = now - s_lastD3d9;
      const uint64_t d3d9_idx = now_idx - s_lastIndexed;
      s_lastD3d9 = now;
      s_lastIndexed = now_idx;

      static uint64_t s_swaps = 0, s_equal = 0, s_ringTotal = 0, s_d3d9Total = 0;
      static uint64_t s_opTotal[4] = {}, s_idxTotal = 0;
      static uint64_t s_eqIdx35 = 0, s_eqNonIdx = 0;
      // Only frames where either side drew say anything; a pair of zeros agrees
      // trivially and would inflate the rate.
      if (ring || d3d9) {
        ++s_swaps;
        s_ringTotal += ring;
        s_d3d9Total += d3d9;
        s_idxTotal += d3d9_idx;
        for (int i = 0; i < 4; ++i) s_opTotal[i] += op[i];
        if (ring == d3d9) ++s_equal;
        // The two subsets worth testing on their own: the binned indexed form
        // against D3D9's indexed draws, and the unbinned forms against the
        // non-indexed ones.
        if (op[2] == d3d9_idx) ++s_eqIdx35;
        if (op[0] + op[3] == d3d9 - d3d9_idx) ++s_eqNonIdx;
        if ((s_swaps % 60) == 0) {
          REXLOG_INFO(
              "{}: draw correspondence — total equal {}/{} frames; ring {} vs "
              "D3D9 {} (indexed {}); ring by opcode 0x22={} 0x34={} 0x35={} "
              "0x36={}; subsets equal: 0x35-vs-indexed {}, unbinned-vs-"
              "nonindexed {}",
              tag, s_equal, s_swaps, s_ringTotal, s_d3d9Total, s_idxTotal,
              s_opTotal[0], s_opTotal[1], s_opTotal[2], s_opTotal[3],
              s_eqIdx35, s_eqNonIdx);
        }
      }
    }

    // The first swap that produces a draw is the whole point of this round, so
    // it is logged unconditionally however sparse the schedule is.
    static bool s_loggedFirstDraw = false;
    if (!draws.empty() && !s_loggedFirstDraw) {
      s_loggedFirstDraw = true;
      REXLOG_INFO("{}: FIRST DRAWS at swap #{} — {} draw calls "
                  "(frame {} packets, swap {} packets)",
                  tag, swap_count, draws.size(), frame_packets.size(),
                  swap_packets.size());
      LogOpcodeHistogram(tag, "frame", swap_count, frame_packets);
    }
    if (log_this_swap) {
      REXLOG_INFO("{}: PM4 #{}: frame {} packets, swap {} packets, {} draw calls",
                  tag, swap_count, frame_packets.size(), swap_packets.size(),
                  draws.size());
    }

    // Always propagate, even when empty: empty list clears the renderer's stale
    // draw data so frames without a VdSwap don't replay the last captured draws.
    if (!is_plugin) {
      // hle_render selects the *source* of the frame's draws, not an extra
      // one. Both paths publish through the same handoff, so letting both run
      // would make whichever finished last win at random.
      if (REXCVAR_GET(hle_render)) {
        // D3D9 draw entry points precede the PM4 IM_LOAD packets that expose
        // the patched shader for this frame. Retry only those same-frame draws
        // now that TranslatePackets has populated CapturedShaders().
        FinalizePendingD3D9Draws(base);
        auto& hle = mx::pm4::HleFrameDraws();
        mx::native::NativeGraphics::Get().SetDrawCalls(hle);
        hle.clear();
      } else {
        mx::native::NativeGraphics::Get().SetDrawCalls(draws);
      }
    }
  }

  s_prev_after = pm4_write_after;
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
