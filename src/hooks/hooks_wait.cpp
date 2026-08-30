// Wait / event hooks.
//
// Both surviving hooks are plain pass-throughs. Nothing here short-circuits a
// wait any more, and NtSetEvent was never stubbed — loading depends on the real
// events firing.

#include "hooks/hook_common.h"

//=============================================================================
// sub_82BFB740 — NtWaitForSingleObjectEx
//=============================================================================
// The instrumented version of this hook is REMOVED 2026-08-16, along with the
// guest-stall watchdog it fed. It was diagnostics only — it called the original
// unconditionally — but it was not free: it took a process-wide mutex on EVERY
// guest wait to record the handle, timeout and caller in a thread->wait map, so
// every waiting thread in the process serialised on one lock to service a probe
// nothing was reading.
//
// What it was for, and why that is over:
//
//   - The watchdog existed for the freeroam hang of 2026-08-08, where the guest
//     main thread went silent while the render thread replayed its last draw
//     list. It printed the outstanding waits when MainLoop stopped ticking.
//   - It was armed by GuestTick(), which only MainLoop called. The MainLoop
//     hook is itself gone (see hooks_gameloop.cpp), so the watchdog thread had
//     already stopped being started at all — ~100 lines that read as live and
//     could not run. GuestWaitEnter/GuestWaitLeave/GuestTick went with it.
//   - The slow-wait detector reported only once orig_Wait RETURNED, so it could
//     never see the case it was written for: a thread parked forever.
//
// Two earlier short-circuits are described here because both are traps worth not
// re-laying, and the reasoning is not obvious from the code that no longer
// exists:
//
//   "every 500ms wait returns SUCCESS immediately" (REMOVED 2026-08-06). It
//   predated the D3D9 HLE layer and was never scoped to a thread or a handle,
//   so it spun every 500ms wait in the process. MainLoop does contain a
//   `li r4,500` wait in its poll loop at 0x82B707E4, which makes this look like
//   the reason native MainLoop crawled. It is not: restoring it left the loop
//   body at 350ms rising to 3000ms, unchanged. Measured, not assumed.
//
//   "after 3s, every INFINITE wait returns SUCCESS" (DISABLED 2026-08-02). It
//   existed because SetupRenderer's NtSetEvent band was skipped, so nothing was
//   ever signalled. With the real events firing, faking success releases threads
//   to walk structures that are not populated yet — the racy null deref inside
//   sub_82AFF560, a RtlEnterCriticalSection-guarded registry walk. The timer was
//   also a single process-wide `static t0` rather than per-wait, so 3s after the
//   FIRST wait every wait in the process became a no-op.
//
// A level loads and exits cleanly with this as a pass-through (2026-08-16).

REX_IMPORT(__imp__sub_82BFB740, orig_Wait, void());
extern "C" REX_FUNC(sub_82BFB740) { orig_Wait(ctx, base); }

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
