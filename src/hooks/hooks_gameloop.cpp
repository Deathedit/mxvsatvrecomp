// Game loop hooks — RenderPipeline.
//
// The MainLoop hook that used to live here is gone; see the note below. What
// remains is a timing probe around the guest's own RenderPipeline.

#include "hooks/hook_common.h"

#include <chrono>

//=============================================================================
// sub_82B70760 — Main game loop (HOOK REMOVED 2026-08-16)
//=============================================================================
// The MainLoop hook is REMOVED. It ran for ~3-4 hours of continuous sessions
// disabled before deletion, across boot, menu, freeroam level load and level
// exit, with no regression — which is the evidence, because several of the
// things it did read as load-bearing and are not.
//
// What it did, and why each is gone:
//
//   ::Sleep(16) at the tail. This is the one with a measurable cost: it paced
//     the guest's own main loop at 60Hz from inside the emulator. Removing it is
//     most of why logo/intro went from 20fps to 40fps.
//
//   REX_STORE_U8(0x82D57994, 1) every iteration. byte_82D57994 gates MainLoop's
//     call to RenderPipeline — at 0x82B707B0 and again at 0x82B7080C a zero
//     jumps straight to the vt[36] tail. The comment argued that "nothing in the
//     guest sets it, so forcing it is load-bearing". That is falsified: with the
//     hook gone RenderPipeline still runs every frame (7,264 times in mx_1270).
//
//   NtSetEvent on *(0x830EC248 + 0x194) once per frame, to drive the loader.
//     The reasoning was that hook #6 skipped the guest renderer that would
//     satisfy LoaderTick's Wait(*(tr+0x194), -1), so the Transition thread would
//     park forever. Levels load AND exit without it, so whatever satisfies that
//     handshake now, it is not this.
//
//   ctx.r3.u32 = 1, to keep the guest's `while (MainLoop != 0)` from exiting.
//   GuestTick(), which armed the stall watchdog in hooks_wait.cpp — itself now
//     removed, since this was its only caller.
//   Per-frame diagnostics: MainLoop body timing, the AssetDB load-machine state,
//     the three per-pass entity counts, and the eng+8 / vtable dumps taken at
//     iteration 1.
//
// Two disabled workarounds documented in that body are worth keeping, because
// both are traps and neither is obvious from code that no longer exists:
//
//   The eng+8 SELF-REF override (`REX_STORE_U32(eng + 8, eng)`) was needed only
//   while hooks #2/#5 skipped the engine init, leaving eng+8 as junk that had to
//   be forced onto Bootstrap's nullsub_1. With the real init running, vt[17]
//   populates eng+8 with the genuine AssetDB, and overwriting it sends vt[36]
//   into garbage — an AV at guest 0x4D5854F1 inside orig_MainLoop.
//
//   Clearing byte_82D57994 at frame 600 was a fabricated "loading complete"
//   signal. It closed the render window ~20s in, entirely inside the 47.4s Bink
//   intro, so RenderPipeline was skipped every time it was reached.
//
// git history has the body.

//=============================================================================
// sub_82B70578 — RenderPipeline
//=============================================================================

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
  if (rp == 1 || (rp % 600) == 0)
    REXLOG_INFO("native: RenderPipeline #{} — calling orig", rp);
  const auto t0 = std::chrono::steady_clock::now();
  orig_RenderPipeline(ctx, base);
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - t0)
                      .count();
  if (ms >= 50) REXLOG_INFO("native: RenderPipeline #{} took {}ms", rp, ms);
}

// The per-callee timing probes that found this are REMOVED 2026-08-06, having
// done their job. They wrapped hot recursive guest functions in two
// steady_clock reads each, which is its own cost on the path being measured.
// The chain they established is in AGENTS.md; git history has the probes if
// another level ever needs walking.
