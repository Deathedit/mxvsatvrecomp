// Frame-lifecycle hooks.
//
// VdSwap is the interesting one: it is the frame boundary. The guest's PM4
// command ring is still parsed and applied to the Xenos register shadow here,
// but purely as diagnostics -- the frame's draws come from the D3D9 HLE path.
// The rest are the guest's Begin/End frame entry points, four of which were
// unhooked once the D3D9 HLE layer made "there is no Xenos GPU behind them"
// false; only the frame-pending poll still returns a fabricated value.
//
// Two ranges, and the distinction matters more than anything else in this file:
//
//   frame range  [prev_after, write_before)  -- everything the guest wrote since
//                                               the last swap. This is the frame.
//   swap range   [write_before, write_after) -- what VdSwap itself emits.
//
// Only the swap range used to be parsed, so every "zero DRAW_*" result in this
// effort was measured over a present sequence -- DISPLAY_TIMING, DISP_TG_CTL,
// EVENT_WRITE_SHD and a SET_LOOP_CONST whose first data word is ASCII "SWAP" --
// where a draw could not have appeared.

#include <filesystem>
#include <system_error>

#include "gpu/health.h"
#include "hooks/hook_common.h"

// timeBeginPeriod. WIN32_LEAN_AND_MEAN keeps mmsystem.h out of windows.h,
// and timeapi.h is the narrow header for just the multimedia timer calls.
#include <timeapi.h>
#include "hooks/hooks_d3d9.h"

#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

#include <rex/cvar.h>

#include "gpu/d3d9_draw.h"
#include "gpu/d3d9_state.h"

#include "gpu/pm4_parser.h"
#include "gpu/xenos_gpu_state.h"

//=============================================================================
// sub_82566B58 -- D3D9's swap, NOT VdSwap itself
//
// The kernel VdSwap is the import thunk at 0x82CE9F98, already named by the
// recompiler. sub_82566B58 *calls* it: the call site returns to 0x82566E1C and
// the next function after 0x82566B58 is 0x825671E0. D3D9 is statically linked
// into the XEX and nothing but its present path calls VdSwap, which makes this a
// confirmed anchor inside the D3D9 library block. Hooking here is still right:
// it is the frame boundary with the ring buffer state we need.
//=============================================================================

namespace {

// logs/pm4dump, emptied once per process before the first dump of the run.
// Mirrors EnsureHlslDumpDir, and for the same reason: these filenames are
// (swap_count) and repeat every run, so a short run leaves the high frame
// numbers of a longer earlier run sitting in the directory, where they read as
// belonging to the current one.
void EnsurePm4DumpDir() {
  static const bool s_cleared = [] {
    std::error_code ec;
    std::filesystem::remove_all("logs/pm4dump", ec);
    return true;
  }();
  (void)s_cleared;
  std::error_code ec;
  std::filesystem::create_directories("logs/pm4dump", ec);
}

// Type3 opcode histogram for a parsed range. This is what says whether draws
// are inline (0x22/0x34/0x35/0x36), hidden behind INDIRECT_BUFFER (0x3F/0x37),
// or simply absent. Diagnostic only now that nothing translates the ring.
void LogOpcodeHistogram(const char* range, int swap_count,
                        const std::vector<mx::pm4::Pm4Packet>& packets) {
  uint32_t counts[128] = {};
  uint32_t type0 = 0, type2 = 0;
  for (const auto& p : packets) {
    if (p.type == mx::pm4::PacketType::Type3) counts[p.opcode & 0x7F]++;
    else if (p.type == mx::pm4::PacketType::Type0) ++type0;
    else if (p.type == mx::pm4::PacketType::Type2) ++type2;
  }
  REXLOG_INFO("native: hist #{} {} — Type0={} Type2={}", swap_count, range,
              type0, type2);
  for (uint32_t op = 0; op < 128; ++op) {
    if (!counts[op]) continue;
    const char* name = mx::pm4::Pm4Parser::OpcodeName(op);
    REXLOG_INFO("native: hist #{} {} — Type3 0x{:02X} {} x{}", swap_count,
                range, op, name ? name : "???", counts[op]);
  }
}

}  // namespace

// Splits our PM4 work from the guest's. RenderPipeline is 100% of MainLoop's
// 400ms-rising-to-3100ms body, and VdSwap fires once per RenderPipeline while
// parsing ~11,600 packets a swap — so the question is whether the cost is this
// hook or the original underneath it. RAII because the hook has several exits.
namespace {
struct SwapTimer {
  const char* what;
  int n;
  std::chrono::steady_clock::time_point t0{std::chrono::steady_clock::now()};
  ~SwapTimer() {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
    if (ms >= 50) REXLOG_INFO("native: {} #{} took {}ms", what, n, ms);
  }
};

}  // namespace

// FRAME PACING -- the console's swap blocks on vsync and ours never did.
//
// On hardware sub_82566B58 calls VdSwap, which does not return until the scanout
// flips. The whole title is written against that: its script threads, UI
// transitions and timers all advance once per swap, and the fastest that can
// ever happen on a 360 is 60 Hz. We present with sync interval 0, so the guest
// free-runs at whatever the host can manage -- measured at **400+ fps** on the
// user's main PC against 100 in the dev VM and 60 on the console, so the guest's
// frame-driven logic advances nearly SEVEN TIMES faster than any hardware it was
// tested on. This project has already lost one crash to that shape.
//
// NOT VSYNC. Present's sync interval would pace to the MONITOR -- 144 Hz still
// runs the guest 2.4x too fast, and the rate would depend on whose desktop it
// is. This paces the guest's own swap to a fixed period instead, as a ceiling:
// a frame that takes longer is not delayed and no attempt is made to catch up.
//
// d3d9_diag_frame_every is frames between the per-swap diagnostic lines -- FRAME
// DRAWS, UNBUILT WHY, UNBUILT SKIPS BY REASON, RING vs HLE and ALU LOAD, which
// were 15% of the log.
//
// NOTHING IS LOST TO THE SAMPLING, and that is the whole design: FRAME DRAWS and
// RING vs HLE carry PER-FRAME DELTAS, so a plain modulus would silently drop the
// draws in every skipped frame. The deltas ACCUMULATE across the interval and
// the line reports the sum with the frame span it covers.
//
// READ THIS BEFORE LOWERING IT TO A PLAIN MODULUS. FRAME DRAWS has been
// modulus-sampled before and gave a WRONG ANSWER every time: a native run ending
// at 424 swaps logged only #1..#3 while a plugin run reaching 760 logged #600,
// which reads as a divergence in the guest's frame lifecycle and was purely the
// modulus. The first five swaps always print, and every throttled line NAMES its
// interval and frame span.
REXCVAR_DEFINE_INT32(d3d9_diag_frame_every, 30, "Debug",
                     "Frames between the per-swap diagnostic lines. Per-frame "
                     "deltas accumulate across the gap rather than being "
                     "dropped; the first five swaps always print (0 or 1 = "
                     "every swap)");

// True on a swap whose per-frame diagnostics should print. The first five
// always do -- a run that dies early must not look like a run that printed
// nothing.
static bool FrameDiagDue(uint64_t swap_count) {
  const int every = REXCVAR_GET(d3d9_diag_frame_every);
  if (every <= 1 || swap_count <= 5) return true;
  return (swap_count % uint64_t(every)) == 0;
}

REXCVAR_DEFINE_INT32(frame_limit_fps, 60, "Debug",
                     "Pace the guest's swap to at most this many frames per "
                     "second, the way the console's vsync-blocking VdSwap did. "
                     "0 lets the guest free-run");

namespace {

// Destructor-based so it covers the hook's several exits. Declared BEFORE
// SwapTimer at the top of the hook, so it is destroyed AFTER it -- otherwise
// the timer would report the pacing sleep as time the hook spent working.
struct FramePacer {
  ~FramePacer() {
    const int fps = REXCVAR_GET(frame_limit_fps);
    if (fps <= 0) return;
    // Windows' default timer granularity is 15.6ms, which cannot express a
    // 16.67ms period at all -- sleep_until alone would quantise 60 fps into an
    // alternating 64/32. winmm is already linked; this asks for 1ms once and
    // never gives it back, which is what a game does.
    static const bool s_period = [] { return timeBeginPeriod(1) == TIMERR_NOERROR; }();
    (void)s_period;
    const auto period = std::chrono::nanoseconds(1000000000ll / fps);
    static std::chrono::steady_clock::time_point s_next{};
    const auto now = std::chrono::steady_clock::now();
    // First swap, or a frame that overran its budget: re-base rather than try
    // to make the time back. Catching up would mean running several frames
    // uncapped, which is the condition this exists to prevent.
    if (s_next.time_since_epoch().count() == 0 || now > s_next) {
      s_next = now + period;
      return;
    }
    std::this_thread::sleep_until(s_next);
    s_next += period;
  }
};

}  // namespace

REX_IMPORT(__imp__sub_82566B58, orig_VdSwap, void());
extern "C" REX_FUNC(sub_82566B58) {
  static int swap_count = 0;
  ++swap_count;
  // Ordering matters: constructed first so it is destroyed LAST, after
  // SwapTimer has already logged. See FramePacer's note.
  FramePacer _frame_pacer;
  SwapTimer _swap_timer{"VdSwap hook total", swap_count};
  ReportHostPageQueryStats();

  // Frame period, swap to swap. The first swap has no predecessor and is not
  // counted — otherwise the whole of startup lands in the first "frame" and
  // drags the mean somewhere meaningless.
  {
    static std::chrono::steady_clock::time_point s_last{};
    const auto now = std::chrono::steady_clock::now();
    if (s_last.time_since_epoch().count() != 0) {
      ++mx::hle::D3D9FrameCount();
      mx::hle::D3D9FrameNanos() +=
          uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                       now - s_last)
                       .count());
    }
    s_last = now;
  }

  // FRAME DRAWS. One line per swap, in BOTH modes: guest entry calls, HLE draws
  // ACCEPTED into the frame list, and HLE draws REFUSED.
  //
  // This is the measurement that decides where the missing menu backdrop lives.
  // A capture established the backdrop is not a draw we shade wrongly, so the
  // question is whether the guest ever submits it:
  //
  //   plugin guest > native guest   our layer suppresses guest submission
  //                                 upstream of the renderer
  //   guest == accepted (native)    the guest never submits it; guest state
  //   guest >  accepted + refused   draws vanish before BuildAndQueueDraw
  //   refused > 0                   we build and discard them
  //
  // `accepted` is counted in FinishHleDraw, the point a built draw joins the
  // frame's draw list. Using the DEFERRED queue instead reported `queued 0` on a
  // native run whose capture holds 340 host draws.
  //
  // Printed EVERY swap and never sampled: every previous attempt at this number
  // was gated or modulus-sampled and gave a wrong answer.
  {
    // The deltas are taken EVERY swap and held; only the printing is sampled.
    // Summing them across the interval is what makes the cadence lossless --
    // dropping the line on a skipped frame would drop that frame's draws from
    // the accounting entirely, which is the failure this line's history is
    // made of.
    static uint64_t s_prev_guest = 0, s_prev_accepted = 0, s_prev_refused = 0;
    static uint64_t s_accGuest = 0, s_accAccepted = 0, s_accRefused = 0;
    static uint64_t s_spanFrom = 1;
    const uint64_t guest = GuestDrawCalls();
    const uint64_t accepted = HleDrawsAccepted();
    const uint64_t refused = HleDrawsRefused();
    s_accGuest += guest - s_prev_guest;
    s_accAccepted += accepted - s_prev_accepted;
    s_accRefused += refused - s_prev_refused;
    s_prev_guest = guest;
    s_prev_accepted = accepted;
    s_prev_refused = refused;
    if (FrameDiagDue(swap_count)) {
      const uint64_t frames = swap_count - s_spanFrom + 1;
      REXLOG_INFO("native: FRAME DRAWS #{} over {} frame(s) from #{}: guest {} "
                  "accepted {} refused {} (unbuilt {}); cumulative guest {} "
                  "accepted {} refused {}", swap_count,
                  frames, s_spanFrom, s_accGuest, s_accAccepted, s_accRefused,
                  s_accGuest > s_accAccepted + s_accRefused
                      ? s_accGuest - s_accAccepted - s_accRefused
                      : 0,
                  guest, accepted, refused);
      s_accGuest = s_accAccepted = s_accRefused = 0;
      s_spanFrom = swap_count + 1;
    // The gap, attributed. Printed on the same cadence so the two are read
    // together: `unbuilt` above is a subtraction between populations, and these
    // are the actual exits that produce it. They should sum to it.
    uint64_t no_vp = 0, sh_failed = 0, nocode_full = 0, skips = 0;
    UnbuiltDrawReasons(no_vp, sh_failed, nocode_full, skips);
    const uint64_t attributed = no_vp + sh_failed + nocode_full + skips;
    const uint64_t gap =
        guest > accepted + refused ? guest - accepted - refused : 0;
    // "They should sum to it", from the comment above, stated so the run says
    // whether they did rather than leaving the subtraction to the reader. A
    // gap of 0 is UNMEASURED, not ok: with nothing lost there is nothing for
    // the attribution to be checked against.
    const uint64_t unattributed =
        attributed > gap ? attributed - gap : gap - attributed;
    REXLOG_INFO("native: UNBUILT WHY cumulative — {} no viewport, {} shader "
                "failed, {} no-code with the queue full, {} BuildHleDraw skips "
                "= {} attributed against a gap of {} [{}]",
                no_vp, sh_failed, nocode_full, skips, attributed, gap,
                mx::gpu::health::Tag(mx::gpu::health::Zero(
                    "draws.gap_unattributed", unattributed, gap)));
    // Measured 2026-08-27: the skips ARE the gap, 16,706 of 16,706, with every
    // other exit at zero. So the reasons are the finding and belong on the
    // ungated line rather than behind --hle_capture.
    REXLOG_INFO("native: UNBUILT SKIPS BY REASON:{}",
                UnbuiltSkipBreakdown());
    }
  }

  if (swap_count <= 5) REXLOG_INFO("native: VdSwap #{} ENTER", swap_count);
  uint32_t cpu_val = REX_LOAD_U32(0x82D21818);
  REX_STORE_U32(0x83144208, cpu_val);
  uint32_t a1 = ctx.r3.u32;

  uint32_t pm4_write_before = REX_LOAD_U32(a1 + 48);

  // Ring bounds are not established. The fields this code used to read as base
  // and end (+44 and +52) logged 0x00000000 and 0xBEBA0000 at swap #1, but
  // packets parse at 0xBEBB3260 and later swaps write at 0xBED0653C -- so
  // neither means what was assumed, and the old wrap arithmetic
  // (ring_size = end - base) was garbage. Dump the struct once so the real
  // fields can be identified against the observed span; until then refuse to
  // parse a wrapped range rather than fabricate packets.
  if (swap_count == 1) {
    for (uint32_t off = 0; off <= 96; off += 16) {
      REXLOG_INFO("native: VdSwap dev+{:3} = 0x{:08X} 0x{:08X} 0x{:08X} 0x{:08X}",
                  off, REX_LOAD_U32(a1 + off), REX_LOAD_U32(a1 + off + 4),
                  REX_LOAD_U32(a1 + off + 8), REX_LOAD_U32(a1 + off + 12));
    }
  }

  {
    SwapTimer _orig_timer{"VdSwap orig", swap_count};
    orig_VdSwap(ctx, base);
  }

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

  // GUEST DRAWS per frame, reported HERE rather than from BeginFrame: BeginFrame
  // is throttled at `bf <= 3 || bf % 600 == 0`, so a run ending at 424 swaps
  // logs only #1..#3 and reads as a hook that fires three times. VdSwap ticks
  // once per present.
  //
  // Averaged over the interval, not sampled from one frame: the front end
  // alternates cheap and expensive frames.
  {
    static uint64_t s_prev_draws = 0;
    static int s_prev_swap = 0;
    if (swap_count <= 3 || (swap_count % 100) == 0) {
      const uint64_t now = GuestDrawCalls();
      const int frames = swap_count - s_prev_swap;
      REXLOG_INFO("native: GUEST DRAWS #{} — {} total, {} per frame over the last "
                  "{} frames", swap_count, now,
                  frames > 0 ? (now - s_prev_draws) / uint64_t(frames) : 0,
                  frames);
      s_prev_draws = now;
      s_prev_swap = swap_count;
    }
  }

  // Log VdSwap at sparse checkpoints, plus the first 40 swaps and every wrap
  // (either range) so the ring span can be pinned down from one run.
  bool log_this_swap = (swap_count <= 40) || (swap_count % 100 == 0) ||
                       frame_wrapped || (pm4_write_after < pm4_write_before);
  if (log_this_swap) {
    if (pm4_write_after >= pm4_write_before) {
      REXLOG_INFO("native: VdSwap #{} frame [0x{:08X}+{}]{} swap [0x{:08X}+{}] "
                  "ptr span 0x{:08X}..0x{:08X}",
                  swap_count, frame_start, frame_size,
                  frame_wrapped ? " WRAPPED-SKIPPED" : "", pm4_write_before,
                  pm4_write_after - pm4_write_before, s_ptr_min, s_ptr_max);
    } else {
      REXLOG_INFO("native: VdSwap #{} ring WRAP (before=0x{:08X} after=0x{:08X}) "
                  "ptr span 0x{:08X}..0x{:08X}",
                  swap_count, pm4_write_before, pm4_write_after, s_ptr_min,
                  s_ptr_max);
    }
  }

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
          REXLOG_INFO("native: PM4 raw[{}] = 0x{:08X}  (guest: 0x{:08X})", i, raw[i], _byteswap_ulong(raw[i]));
        }
      }
      swap_parser.ParseRange(
          reinterpret_cast<const uint32_t*>(base + pm4_write_before),
          sz / 4, pm4_write_before);
    }
  }

  auto& frame_packets = frame_parser.Packets();
  auto& swap_packets = swap_parser.Packets();

  // Feed the ALU constant file on EVERY parsed swap, deliberately outside the
  // gate below. That gate keeps the noisy per-packet logging off the hot path
  // and only lets swaps <= 20 plus three checkpoints through, so anything hung
  // off it stops updating ~20 frames into a run. Cheap by construction: one
  // range test per Type0 packet.
  //
  // RING vs HLE CENSUS, added to test one premise before building per-draw
  // constant ordering on it: that a draw's position in the RING can be matched
  // to a draw in the D3D9 HLE path. Ordering by index is only meaningful if
  // the two counts track each other.
  //
  // The Type3 constant opcodes are counted for a separate reason. The ALU
  // constant file is fed ONLY from Type0 writes, but SET_CONSTANT,
  // SET_SHADER_CONSTANTS and LOAD_ALU_CONSTANT are Type3 -- if the guest
  // publishes constants through those, we never record them at all.
  //
  // MEASUREMENT ONLY: nothing here changes what is recorded or drawn.

  uint32_t ring_draws = 0, t3_set_const = 0, t3_set_shader = 0, t3_load_alu = 0;
  // Cumulative, so a zero in APPLIED is readable against what we saw and why
  // each skip happened rather than being one undifferentiated number.
  static uint64_t s_alu_applied = 0, s_alu_dwords = 0, s_alu_nonalu = 0,
                  s_alu_unreadable = 0, s_alu_range = 0, s_alu_short = 0;
  for (const auto* list : {&frame_packets, &swap_packets}) {
    for (const auto& p : *list) {
      if (p.type == mx::pm4::PacketType::Type3) {
        switch (static_cast<mx::pm4::Pm4Opcode>(p.opcode)) {
          case mx::pm4::Pm4Opcode::DRAW_INDX:
          case mx::pm4::Pm4Opcode::DRAW_INDX_2:
          case mx::pm4::Pm4Opcode::DRAW_INDX_BIN:
          case mx::pm4::Pm4Opcode::DRAW_INDX_2_BIN:
            ++ring_draws;
            break;
          case mx::pm4::Pm4Opcode::SET_CONSTANT:       ++t3_set_const; break;
          case mx::pm4::Pm4Opcode::SET_SHADER_CONSTANTS: ++t3_set_shader; break;
          case mx::pm4::Pm4Opcode::LOAD_ALU_CONSTANT: {
            // THE SECOND PUBLISHER. The ALU constant file was fed from Type0
            // writes only, and this Type3 opcode runs ~234 times a frame
            // carrying constants we recorded none of.
            //
            // Body layout and the index arithmetic are Xenia's, from
            // ExecutePacketType3_LOAD_ALU_CONSTANT and WriteALURangeFromMem:
            //
            //   body[0] address     & 0x3FFFFFFF
            //   body[1] offset_type -> index = & 0x7FF, type = >> 16 & 0xFF
            //   body[2] size_dwords & 0xFFF          (a REGISTER count)
            //   ALU registers live at 0x4000 + index, which is our kAluRegBase
            //
            // The packet carries an ADDRESS, not values: the constants sit in
            // guest memory, so this reads them the same way
            // OverlayShaderConstants reads a shader's own table. p.body is
            // host-order (the parser byte-swaps on the way in); guest memory
            // is not, hence REX_LOAD_U32.
            //
            // ONLY type 0 (ALU) is applied. 1=FETCH, 2=BOOL, 3=LOOP,
            // 4=REGISTERS are counted and skipped.
            ++t3_load_alu;
            if (p.body.size() < 3) { ++s_alu_short; break; }
            const uint32_t addr = p.body[0] & 0x3FFFFFFFu;
            const uint32_t offset_type = p.body[1];
            const uint32_t index = offset_type & 0x7FFu;
            const uint32_t type = (offset_type >> 16) & 0xFFu;
            uint32_t n = p.body[2] & 0xFFFu;
            if (type != 0) { ++s_alu_nonalu; break; }
            if (!n || index >= mx::gpu::alu::kAluConstants * 4) { ++s_alu_range; break; }
            if (index + n > mx::gpu::alu::kAluConstants * 4)
              n = mx::gpu::alu::kAluConstants * 4 - index;
            // The masked physical address is rarely the readable one, so
            // walk the mirrors. First cut of this skipped 55,357 packets and
            // applied ZERO purely because it trusted `addr` as-is -- the
            // separate unreadable bucket is what said so, rather than the
            // whole thing just reading 0.
            const uint32_t src = ResolveGuestRange(base, addr, n * 4);
            if (!src) {
              ++s_alu_unreadable;
              break;
            }
            static thread_local std::vector<uint32_t> vals;
            vals.resize(n);
            for (uint32_t k = 0; k < n; ++k)
              vals[k] = REX_LOAD_U32(src + k * 4);
            mx::gpu::alu::NoteType0Write(mx::gpu::alu::kAluRegBase + index,
                                         vals.data(), n);
            ++s_alu_applied;
            s_alu_dwords += n;
            break;
          }
          default: break;
        }
        continue;
      }
      if (p.type != mx::pm4::PacketType::Type0 || p.body.empty()) continue;
      mx::gpu::alu::NoteType0Write(p.reg_base, p.body.data(),
                                   static_cast<uint32_t>(p.body.size()));
    }
  }
  {
    // The HLE side of the comparison, as a per-frame delta on the same counter
    // FRAME DRAWS reports, so the two lines can be read together. Both sides
    // ACCUMULATE across the reporting interval: a modulus that sampled one
    // frame in thirty would compare two numbers drawn from a single frame
    // while the other twenty-nine went unexamined.
    static uint64_t s_prev_ring_guest = 0;
    static uint64_t s_accRing = 0, s_accHle = 0;
    static uint64_t s_ringSpanFrom = 1;
    const uint64_t g = GuestDrawCalls();
    s_accHle += g - s_prev_ring_guest;
    s_accRing += ring_draws;
    s_prev_ring_guest = g;
    if (FrameDiagDue(swap_count)) {
      const uint64_t frames = swap_count - s_ringSpanFrom + 1;
      REXLOG_INFO("native: RING vs HLE #{} over {} frame(s) from #{}: ring draws "
                  "{} vs HLE draws {} (ordering by index is only usable if "
                  "these track) | Type3 constant opcodes: SET_CONSTANT {}, "
                  "SET_SHADER_CONSTANTS {}, LOAD_ALU_CONSTANT {} (non-zero "
                  "means the ALU file, which is fed from Type0 only, is "
                  "missing a publish path)",
                  swap_count, frames, s_ringSpanFrom, s_accRing,
                  s_accHle, t3_set_const, t3_set_shader, t3_load_alu);
      s_accRing = s_accHle = 0;
      s_ringSpanFrom = swap_count + 1;
      REXLOG_INFO("native: ALU LOAD applied {} packets / {} dwords into the "
                  "constant file | skipped: {} non-ALU type, {} unreadable "
                  "address, {} out of range, {} short body",
                  s_alu_applied, s_alu_dwords, s_alu_nonalu,
                  s_alu_unreadable, s_alu_range, s_alu_short);
    }
  }

  // Only write dump files for spot-check swaps — keeps the disk clean when
  // we're parsing every swap looking for indexed draws.
  bool should_dump_file = (swap_count <= 20) ||
                          swap_count == 300 || swap_count == 600 ||
                          swap_count == 1000 ||
                          (swap_count >= 1200 && (swap_count % 500 == 0));
  if (should_dump_file) {
    // Into logs/pm4dump/, with every other dump directory. These used to land
    // next to the executable and had accumulated 137 files in the project root
    // before anyone noticed -- they are gitignored, so nothing complained.
    // Wiped once per process, same reason as EnsureHlslDumpDir: the names
    // repeat every run.
    EnsurePm4DumpDir();
    char dumpfname[96];
    snprintf(dumpfname, sizeof(dumpfname), "logs/pm4dump/native_frame_%02d.txt", swap_count);
    mx::pm4::Pm4Parser::DumpPackets(frame_packets, dumpfname);
    snprintf(dumpfname, sizeof(dumpfname), "logs/pm4dump/native_swap_%02d.txt", swap_count);
    mx::pm4::Pm4Parser::DumpPackets(swap_packets, dumpfname);
    LogOpcodeHistogram("frame", swap_count, frame_packets);
    LogOpcodeHistogram("swap", swap_count, swap_packets);
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
    REXLOG_INFO("native: ApplyPackets done, {} regs", gpu_state.Registers().size());
  }

  // The PM4 translator used to run here, building draws from the ring. It is
  // gone: the ring reached ~15k vertices a frame and left 99.9% of them
  // untranscoded, while the D3D9 HLE path carries 27.4M. The packets are
  // still parsed above, but only as diagnostics — nothing downstream of this
  // point consumes them to produce pixels.

  //-----------------------------------------------------------------------
  // Does the ring carry the same number of draws D3D9 was asked for?
  //
  // This is the last route left from a D3D9 shader handle to its microcode.
  // The direct ones are all closed: the blob at +0x368 is not the code (23%
  // agreement against what the ring loaded), SH_pPhysical reads as zeros at
  // bind time, and its address is not the ring key. But the ring does load 41
  // shaders *by address*, so the code is in guest memory -- only the mapping
  // is missing. If each frame's ring draw count equals its D3D9 draw count,
  // the Nth ring draw is the Nth D3D9 draw.
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
    const uint64_t now = mx::hle::D3D9DrawCounter();
    const uint64_t now_idx = mx::hle::D3D9IndexedDrawCounter();
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
            "native: draw correspondence — total equal {}/{} frames; ring {} vs "
            "D3D9 {} (indexed {}); ring by opcode 0x22={} 0x34={} 0x35={} "
            "0x36={}; subsets equal: 0x35-vs-indexed {}, unbinned-vs-"
            "nonindexed {}",
            s_equal, s_swaps, s_ringTotal, s_d3d9Total, s_idxTotal,
            s_opTotal[0], s_opTotal[1], s_opTotal[2], s_opTotal[3],
            s_eqIdx35, s_eqNonIdx);
      }
    }
  }

  if (log_this_swap) {
    REXLOG_INFO("native: PM4 #{}: frame {} packets, swap {} packets",
                swap_count, frame_packets.size(), swap_packets.size());
  }

  // Drains draws parked with ShaderApplyResult::kNoCode. This used to be the
  // retry that mattered, while the code came from the ring; it now comes
  // from CapturePatchedCode inside the PatchVertexShader hook, which runs
  // before the draw it patches.
  //
  // Kept as a drain, not as a working retry: `skipped no-code` reads 0 and
  // the deferred-draw line never prints. It stays because kNoCode is still
  // reachable and a parked draw with nothing to drain it would leak.
  //
  // Same lock the D3D9 hooks take: FinalizePendingD3D9Draws drains
  // g_pendingHleDraws and reads the shader caches, all of which a record
  // worker may be writing at this instant.
  std::lock_guard<std::recursive_mutex> lock(mx::hle::HleGlobalMutex());
  FinalizePendingD3D9Draws(base);
  auto& hle = mx::hle::HleFrameDraws();
  // Always propagate, even when empty: an empty list clears the renderer's
  // stale draw data so frames without a VdSwap don't replay the last capture.
  mx::native::NativeGraphics::Get().SetDrawCalls(hle);
  hle.clear();

  s_prev_after = pm4_write_after;
}

//=============================================================================
// sub_82BFBF30 — GPU spin-wait sync
//=============================================================================

REX_IMPORT(__imp__sub_82BFBF30, orig_XenosWait, void());
extern "C" REX_FUNC(sub_82BFBF30) {
  REX_STORE_U32(0x83144208, REX_LOAD_U32(0x82D21818));

  // Retire the GPU fence. This is the body of the command-buffer spin loop in
  // the parallel record worker (sub_82AC8CC8 at 0x82AC8D70):
  //
  //   if (use_count && sub_82559A70(cmdbuf))
  //     do sub_82BFBF30(0); while (sub_82559A70(cmdbuf));
  //
  // and sub_82559A70 -> sub_825599E0 decides "still in flight" as
  //
  //   device = *(*VdGlobalDevice)              ; 0x820007DC
  //   return (*(device+0x2A9C) - fence)        ; CPU-submitted fence
  //        < (*(device+0x2A9C) - **(device+0x2A90))   ; GPU-retired fence
  //
  // *(device+0x2A90) points at the counter the Xenos writes as it consumes
  // command buffers. There is no Xenos here, so it never moves: the first time
  // the 12-slot ring wraps back onto a slot used once, the fence sits ahead of
  // it and the test is true forever. The worker spins -- no wait, no log -- and
  // the main thread parks in the join waiting on three done-events that will
  // never be set. That is the freeroam hang: the menu never submits enough to
  // wrap the ring, and freeroam wraps it within a second or two of entry.
  //
  // Our HLE consumes each draw list synchronously at handoff, so by the time the
  // guest asks, the work really is done.
  const uint32_t device_slot = REX_LOAD_U32(0x820007DC);
  if (!device_slot) return;
  const uint32_t device = REX_LOAD_U32(device_slot);
  if (!device) return;
  const uint32_t retired_ptr = REX_LOAD_U32(device + 0x2A90);
  if (!retired_ptr) return;
  const uint32_t submitted = REX_LOAD_U32(device + 0x2A9C);
  if (REX_LOAD_U32(retired_ptr) != submitted) {
    static uint64_t n = 0;
    if (++n <= 8 || (n % 20000) == 0)
      REXLOG_INFO("native: fence retire #{} {} -> {} (device 0x{:08X})", n,
                  REX_LOAD_U32(retired_ptr), submitted, device);
    REX_STORE_U32(retired_ptr, submitted);
  }
}

//=============================================================================
// GPU call stubs -- UNHOOKED
//
// These four used to run an empty body. They date from before the D3D9 HLE
// layer, when "there is no Xenos GPU behind them" was true of the whole
// backend; all four now call the guest original.
//
// If this reintroduces a hang or an access violation, the two to suspect are
// BeginFrame (sub_82ABF828), which reaches XenonRenderer at gs+80, and
// GpuStateXenos, whose original had not run past call #3. Revert per-hook
// rather than wholesale.
//=============================================================================

REX_IMPORT(__imp__sub_8255D430, orig_BeginFrameXenos, void());
extern "C" REX_FUNC(sub_8255D430) {
  orig_BeginFrameXenos(ctx, base);
}

REX_IMPORT(__imp__sub_8255D470, orig_EndFrameXenos, void());
extern "C" REX_FUNC(sub_8255D470) {
  orig_EndFrameXenos(ctx, base);
}

// The native branch used to call the original INSIDE `if (gs <= 3)`, so what
// reads as a log throttle was a stub: from call #4 the guest function never ran.
// The log shows exactly that — `GpuState #1/#2/#3`, then silence for the rest of
// the run. The throttle now covers only the logging, which is what its shape
// always implied.
REX_IMPORT(__imp__sub_8255D520, orig_GpuStateXenos, void());
extern "C" REX_FUNC(sub_8255D520) {
  static int gs = 0;
  ++gs;
  const bool loud = gs <= 3 || (gs % 600) == 0;
  if (loud) REXLOG_INFO("native: GpuState #{} — calling orig", gs);
  orig_GpuStateXenos(ctx, base);
  if (loud) REXLOG_INFO("native: GpuState #{} — returned", gs);
}

//=============================================================================
// sub_82ABF828 -- Begin frame. UNHOOKED, see the block above. The old comment
// read "(stubbed -- accesses XenonRenderer at gs+80)", and that is still the
// hazard to watch: if this faults, gs+80 is where to look, and this is the first
// of the four to put back.
//=============================================================================

REX_IMPORT(__imp__sub_82ABF828, orig_BeginFrame, void());
extern "C" REX_FUNC(sub_82ABF828) {
  static int bf = 0;
  ++bf;
  if (bf <= 3 || (bf % 600) == 0)
    REXLOG_INFO("native: BeginFrame #{}", bf);
  orig_BeginFrame(ctx, base);
}

//=============================================================================
// sub_82ABF930 — End frame / VdSwap caller
//=============================================================================

REX_IMPORT(__imp__sub_82ABF930, orig_EndFrame, void());
extern "C" REX_FUNC(sub_82ABF930) {
  static int ef = 0;
  ++ef;
  orig_EndFrame(ctx, base);
}

//=============================================================================
// sub_8255CFE0 -- GPU frame-pending poll (spin loop in VdSwap at 0x82567178).
// Without a GPU the counter never advances, so the spin is infinite. Stubbed to
// return 0 ("not pending").
//
// DELIBERATELY NOT UNHOOKED with the other four. This one is not an empty body
// that discards work -- it returns a value the guest spins on, and the counter
// it would poll is a Xenos GPU register our D3D12 backend does not advance.
// Passing it through hangs VdSwap on the second frame. If the other four turn
// out not to be the UI_World lever, this needs the counter written from the host
// first, not the guard removed.
//=============================================================================
REX_IMPORT(__imp__sub_8255CFE0, orig_FramePendingPoll, int());
extern "C" REX_FUNC(sub_8255CFE0) {
  // No GPU to poll — always "not pending".
  ctx.r3.u32 = 0;
}

//=============================================================================
// Parallel command-buffer recording -- fork / worker / join
//=============================================================================
// The freeroam hang is the guest main thread parked forever in sub_82AC8B68 at
// 0x82AC8BCC: Wait(unk_830B0C34[i], INFINITE) for i in 0..2. That is the JOIN of
// a three-way parallel scene record:
//
//   sub_82AC8A18  fork    allocates 5 D3D9 command buffers, clears each
//                         done-event, then NtSetEvent(dword_830B0C28[i]) to
//                         release worker i
//   sub_82AC8CC8  worker  Wait(dword_830B0C28[i], INFINITE), record, signal
//                         done-event unk_830B0C34[i]
//   sub_82AC8B68  join    Wait(unk_830B0C34[i], INFINITE) x3, release buffers
//
// All three are guarded by the same `if (counters != 0)`, which is why the menu
// survives and freeroam does not: the parallel path only engages once the scene
// has content to record.
//
// The stall watchdog showed NO thread waiting at the worker's own go-event, so
// the workers were released and are blocked or lost inside the work rather than
// waiting for a kick. These probes are pure passthrough.

REX_IMPORT(__imp__sub_82AC8A18, orig_RecordFork, void());
extern "C" REX_FUNC(sub_82AC8A18) {
  static uint64_t n = 0;
  ++n;
  const bool loud = n <= 24 || (n % 300) == 0;
  if (loud) REXLOG_INFO("native: RecordFork #{} ENTER", n);
  orig_RecordFork(ctx, base);
  if (loud) REXLOG_INFO("native: RecordFork #{} RETURNED", n);
}

REX_IMPORT(__imp__sub_82AC8CC8, orig_RecordWorker, void());
extern "C" REX_FUNC(sub_82AC8CC8) {
  // r3 points at the worker's index; the proc reads it as `lwz r31, 0(r3)`.
  const uint32_t idx = ctx.r3.u32 ? REX_LOAD_U32(ctx.r3.u32) : 0xFFFFFFFF;
  REXLOG_INFO("native: RecordWorker[{}] ENTER", idx);
  // Tag the thread before the proc records anything, so its draws land in a
  // list the join can order by worker index.
  if (idx < 3) mx::hle::HleSetThreadRecordIndex(idx);
  orig_RecordWorker(ctx, base);
  REXLOG_INFO("native: RecordWorker[{}] RETURNED", idx);
}

REX_IMPORT(__imp__sub_82AC8B68, orig_RecordJoin, void());
extern "C" REX_FUNC(sub_82AC8B68) {
  static uint64_t n = 0;
  ++n;
  const bool loud = n <= 24 || (n % 300) == 0;
  if (loud) REXLOG_INFO("native: RecordJoin #{} ENTER", n);
  orig_RecordJoin(ctx, base);
  // After the original returns, all three workers have signalled done and are
  // parked on their next go-event. Their lists are quiescent, so this is the
  // one point where they can be merged without a lock — and it is the same
  // point at which the guest itself consumes the three command buffers, so the
  // resulting draw order is the guest's, not thread-arrival order.
  mx::hle::HleMergeWorkerDraws();
  if (loud) REXLOG_INFO("native: RecordJoin #{} RETURNED", n);
}
