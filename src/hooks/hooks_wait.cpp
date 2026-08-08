// Wait / event hooks.
//
// The native path has no Xenos GPU to signal the events the guest waits on, so
// these short-circuit the waits that would otherwise block forever. NtSetEvent
// is deliberately NOT stubbed — loading depends on the events actually firing.

#include "hooks/hook_common.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <thread>

//=============================================================================
// Guest-stall watchdog
//=============================================================================
// The freeroam hang (2026-08-08) presents as the guest main thread emitting no
// log line of any kind while the render thread goes on replaying the last draw
// list. Nothing in the existing probes can see it: the slow-wait detector below
// only reports once orig_Wait RETURNS, and the Wait(INFINITE) entry log is
// capped at the first five, all of which fire during boot on other threads. So
// a thread parked forever produces silence, which is exactly what we observe
// and exactly what we cannot distinguish from a spin.
//
// This records every wait at ENTRY and clears it on return. When MainLoop stops
// ticking, the watchdog prints what is still outstanding. Two outcomes, both
// decisive: the guest main thread appears in the list with a caller address to
// look up in IDA, or it does not appear at all and the hang is not a wait.

namespace mx::native {
namespace {

struct WaitRec {
  uint32_t handle;
  uint32_t timeout;
  uint32_t lr;
  std::chrono::steady_clock::time_point t0;
};

std::mutex g_waitMu;
std::map<uint32_t, WaitRec> g_inFlight;  // thread id -> outstanding wait
std::atomic<uint64_t> g_ticks{0};
std::atomic<int64_t> g_lastTickMs{0};

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void WatchdogBody() {
  bool reported = false;
  int64_t reportedAt = 0;
  for (;;) {
    ::Sleep(1000);
    const int64_t last = g_lastTickMs.load(std::memory_order_relaxed);
    if (!last) continue;  // MainLoop has not started yet
    const int64_t stalled = NowMs() - last;
    if (stalled < 5000) {
      reported = false;
      continue;
    }
    // Re-report every 30s so a persistent stall stays visible without flooding.
    if (reported && (NowMs() - reportedAt) < 30000) continue;
    reported = true;
    reportedAt = NowMs();

    REXLOG_INFO(
        "native: STALL — MainLoop has not ticked for {}ms (tick #{}); "
        "outstanding guest waits follow",
        stalled, g_ticks.load(std::memory_order_relaxed));
    std::lock_guard<std::mutex> lock(g_waitMu);
    if (g_inFlight.empty()) {
      REXLOG_INFO(
          "native: STALL — no thread is inside a guest wait. The stall is a "
          "spin or a block outside NtWaitForSingleObjectEx.");
      continue;
    }
    const auto now = std::chrono::steady_clock::now();
    for (const auto& [tid, w] : g_inFlight) {
      const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
                           now - w.t0)
                           .count();
      REXLOG_INFO(
          "native: STALL — thread t{} waiting {}ms handle=0x{:08X} "
          "timeout=0x{:08X} from lr=0x{:08X}",
          tid, age, w.handle, w.timeout, w.lr);
    }
  }
}

}  // namespace

void GuestWaitEnter(uint32_t handle, uint32_t timeout, uint32_t lr) {
  std::lock_guard<std::mutex> lock(g_waitMu);
  g_inFlight[::GetCurrentThreadId()] = {handle, timeout, lr,
                                        std::chrono::steady_clock::now()};
}

void GuestWaitLeave() {
  std::lock_guard<std::mutex> lock(g_waitMu);
  g_inFlight.erase(::GetCurrentThreadId());
}

void GuestTick() {
  g_ticks.fetch_add(1, std::memory_order_relaxed);
  g_lastTickMs.store(NowMs(), std::memory_order_relaxed);
  static std::once_flag once;
  std::call_once(once, [] { std::thread(WatchdogBody).detach(); });
}

}  // namespace mx::native

//=============================================================================
// sub_82BFB740 — NtWaitForSingleObjectEx
//=============================================================================

REX_IMPORT(__imp__sub_82BFB740, orig_Wait, void());
extern "C" REX_FUNC(sub_82BFB740) {
  if (mx::native::g_plugin_mode) { orig_Wait(ctx, base); return; }
  // The blanket "every 500ms wait returns SUCCESS immediately" short-circuit is
  // REMOVED 2026-08-06. It dated to 3cca295 (2026-08-01), predating the D3D9 HLE
  // layer, and was never scoped to a thread or a handle — it spun every 500ms
  // wait in the process, not just the renderer handshake it was written for.
  //
  // MainLoop does contain a `li r4,500` wait, in its poll loop at 0x82B707E4
  // (`Wait(*(r30+732), 500)`, looping while r3 == 258 STATUS_TIMEOUT), so this
  // looked like the reason native MainLoop crawls. It is not: restoring the
  // short-circuit leaves the loop body at 350ms rising to 3000ms, unchanged.
  // Measured, not assumed — do not restore it on the strength of that call site.
  if (ctx.r4.u32 == 0xFFFFFFFF) {
    static int inf = 0;
    ++inf;
    if (inf <= 5)
      REXLOG_INFO("native: Wait(INFINITE) #{} handle=0x{:08X}", inf, ctx.r3.u32);
    // The blanket "after 3s, every INFINITE wait returns SUCCESS" fallback is
    // DISABLED 2026-08-02. It existed because SetupRenderer's NtSetEvent band
    // was skipped by hook #5 so nothing was ever signalled. With #5 off the
    // real events fire, and faking success releases threads to walk
    // structures that are not populated yet — the racy null deref inside
    // sub_82AFF560 (a RtlEnterCriticalSection-guarded registry walk).
    // Note the old timer was a single process-wide `static t0`, not per-wait,
    // so 3s after the FIRST wait every wait in the process became a no-op.
  }
  // Slow-wait detector. Native MainLoop manages under 61 iterations in 75s
  // where plugin mode does 1200 in 73s, and it is not HLE cost — a native run
  // with hle_render and hle_shader_exec off behaves identically. So something
  // blocks, and a wait is the first place to look. Log the handle, the
  // requested timeout and the caller for anything that actually stalls.
  const uint32_t handle = ctx.r3.u32;
  const uint32_t timeout = ctx.r4.u32;
  const uint32_t lr = uint32_t(ctx.lr);
  const auto t0 = std::chrono::steady_clock::now();
  mx::native::GuestWaitEnter(handle, timeout, lr);
  orig_Wait(ctx, base);
  mx::native::GuestWaitLeave();
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - t0)
                      .count();
  if (ms >= 50) {
    static uint64_t s_slow = 0;
    static std::chrono::steady_clock::time_point s_last{};
    const auto now = std::chrono::steady_clock::now();
    const bool due = (now - s_last) >= std::chrono::seconds(5);
    if (++s_slow <= 20 || due) {
      if (due) s_last = now;
      REXLOG_INFO(
          "native: slow wait #{} {}ms handle=0x{:08X} timeout=0x{:08X} "
          "r3=0x{:08X} from lr=0x{:08X}",
          s_slow, ms, handle, timeout, ctx.r3.u32, lr);
    }
  }
}

// The sub_82BFB748 (NtSetEvent) hook is REMOVED 2026-08-06. It was a frontier
// probe for a crash between LoaderTick #1 and Timing #2 that no longer happens,
// and it called the original unconditionally in both modes — eight log lines and
// no behaviour. Nothing depended on it.

//=============================================================================
// sub_82BFBF48 — CRT per-thread errno accessor (no longer stubbed)
//=============================================================================
// Named "error recovery" and stubbed to nothing since 2026-08-01. That name was
// a guess and it was wrong. The function tail-calls `sub_82C01138`, which is a
// pure read of the CRT thread block: `r13+336 ? 0 : *(*(r13+256) + 352)` — an
// errno-style pointer accessor with no side effects and no GPU dependency.
//
// Stubbing it did not "skip error recovery"; it left r3 holding whatever the
// caller had, at 156 call sites. UNSTUBBED 2026-08-06.
REX_IMPORT(__imp__sub_82BFBF48, orig_CrtErrnoPtr, void());
extern "C" REX_FUNC(sub_82BFBF48) { orig_CrtErrnoPtr(ctx, base); }
