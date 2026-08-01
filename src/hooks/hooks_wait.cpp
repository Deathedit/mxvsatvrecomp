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
  if (ctx.r4.u32 == 500) {
    ctx.r3.u32 = 0;
    return;
  }
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

//=============================================================================
// sub_82BFB748 — NtSetEvent wrapper (call orig, events needed for loading)
//=============================================================================

REX_IMPORT(__imp__sub_82BFB748, orig_SetEvent, void());
extern "C" REX_FUNC(sub_82BFB748) {
  if (mx::native::g_plugin_mode) { orig_SetEvent(ctx, base); return; }
  // Frontier probe: Transition's loop calls NtSetEvent(tr+0x2DC) after each
  // LoaderTick. The crash lands between LoaderTick #1 returning and Timing #2.
  static int se = 0;
  ++se;
  bool loud = se <= 8;
  if (loud) REXLOG_INFO("native: NtSetEvent #{} ENTER handle=0x{:08X}", se, ctx.r3.u32);
  orig_SetEvent(ctx, base);
  if (loud) REXLOG_INFO("native: NtSetEvent #{} RETURNED r3=0x{:08X}", se, ctx.r3.u32);
}

//=============================================================================
// sub_82BFBF48 — error recovery (stubbed)
//=============================================================================

REX_IMPORT(__imp__sub_82BFBF48, orig_ErrorRecovery, void());
extern "C" REX_FUNC(sub_82BFBF48) {}
