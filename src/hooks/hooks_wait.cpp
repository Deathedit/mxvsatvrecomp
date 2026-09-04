// Wait / event hooks.
//
// Both surviving hooks are plain pass-throughs. Nothing here short-circuits a
// wait any more, and NtSetEvent was never stubbed — loading depends on the real
// events firing.

#include "hooks/hook_common.h"

//=============================================================================
// sub_82BFB740 -- NtWaitForSingleObjectEx
//=============================================================================
// The instrumented version of this hook is REMOVED. It was diagnostics only --
// it called the original unconditionally -- but it was not free: it took a
// process-wide mutex on EVERY guest wait to record the handle, timeout and
// caller, so every waiting thread serialised on one lock to service a probe
// nothing was reading. Its watchdog was armed by GuestTick(), which only
// MainLoop called, and the MainLoop hook is itself gone; and its slow-wait
// detector reported only once orig_Wait RETURNED, so it could never see the case
// it was written for.
//
// Two earlier short-circuits, described because both are traps worth not
// re-laying:
//
//   "every 500ms wait returns SUCCESS immediately" (REMOVED). It predated the
//   D3D9 HLE layer and was never scoped to a thread or handle, so it spun every
//   500ms wait in the process. MainLoop does contain a `li r4,500` wait in its
//   poll loop, which makes this look like the reason native MainLoop crawled. It
//   is not: restoring it left the loop body unchanged.
//
//   "after 3s, every INFINITE wait returns SUCCESS" (DISABLED). It existed
//   because SetupRenderer's NtSetEvent band was skipped, so nothing was ever
//   signalled. With the real events firing, faking success releases threads to
//   walk structures that are not populated yet.
//
// A level loads and exits cleanly with this as a pass-through.

REX_IMPORT(__imp__sub_82BFB740, orig_Wait, void());
extern "C" REX_FUNC(sub_82BFB740) { orig_Wait(ctx, base); }

// The sub_82BFB748 (NtSetEvent) hook is REMOVED. It was a frontier probe for a
// crash between LoaderTick #1 and Timing #2 that no longer happens, and it
// called the original unconditionally in both modes.

//=============================================================================
// sub_82BFBF48 -- CRT per-thread errno accessor (no longer stubbed)
//=============================================================================
// Named "error recovery" and stubbed to nothing. That name was a guess and it
// was wrong: the function tail-calls sub_82C01138, a pure read of the CRT thread
// block -- `r13+336 ? 0 : *(*(r13+256) + 352)` -- with no side effects and no
// GPU dependency. Stubbing it did not "skip error recovery"; it left r3 holding
// whatever the caller had, at 156 call sites. UNSTUBBED.

REX_IMPORT(__imp__sub_82BFBF48, orig_CrtErrnoPtr, void());
extern "C" REX_FUNC(sub_82BFBF48) { orig_CrtErrnoPtr(ctx, base); }
