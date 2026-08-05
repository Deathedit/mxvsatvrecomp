// Wait / event hooks.
//
// The native path has no Xenos GPU to signal the events the guest waits on, so
// these short-circuit the waits that would otherwise block forever. NtSetEvent
// is deliberately NOT stubbed — loading depends on the events actually firing.

#include "hooks/hook_common.h"

#include <chrono>

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
  // Same vintage and same shape as the mid-ASM renderer skip already retired.
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
  orig_Wait(ctx, base);
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
