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
// sub_82BFB748 — NtSetEvent wrapper (call orig, events needed for loading)
//=============================================================================

REX_IMPORT(__imp__sub_82BFB748, orig_SetEvent, void());
extern "C" REX_FUNC(sub_82BFB748) {
  if (mx::native::g_plugin_mode) { orig_SetEvent(ctx, base); return; }
  orig_SetEvent(ctx, base);
}

//=============================================================================
// sub_82BFBF48 — error recovery (stubbed)
//=============================================================================

REX_IMPORT(__imp__sub_82BFBF48, orig_ErrorRecovery, void());
extern "C" REX_FUNC(sub_82BFBF48) {}
