// Game loop hooks — RenderPipeline.
//
// The MainLoop hook that used to live here is gone; see the note below. What
// remains is a timing probe around the guest's own RenderPipeline.

#include "hooks/hook_common.h"

#include <chrono>

//=============================================================================
// sub_82B70760 -- Main game loop (HOOK REMOVED)
//=============================================================================
// It ran for ~3-4 hours of continuous sessions disabled before deletion, across
// boot, menu, freeroam level load and level exit, with no regression -- which is
// the evidence, because several of the things it did read as load-bearing and
// are not:
//
//   ::Sleep(16) at the tail. The one with a measurable cost: it paced the
//     guest's own main loop at 60Hz from inside the emulator, and removing it is
//     most of why logo/intro went from 20fps to 40fps.
//
//   REX_STORE_U8(0x82D57994, 1) every iteration. byte_82D57994 gates MainLoop's
//     call to RenderPipeline, and the comment argued that "nothing in the guest
//     sets it, so forcing it is load-bearing". Falsified: with the hook gone
//     RenderPipeline still runs every frame.
//
//   NtSetEvent on *(0x830EC248 + 0x194) once per frame, to drive the loader, on
//     the reasoning that hook #6 skipped the guest renderer that would satisfy
//     LoaderTick's Wait. Levels load AND exit without it.
//
//   ctx.r3.u32 = 1, GuestTick() (whose only consumer, the stall watchdog, is
//     itself now removed), and per-frame diagnostics.
//
// Two disabled workarounds are worth keeping, because both are traps:
//
//   The eng+8 SELF-REF override was needed only while hooks #2/#5 skipped the
//   engine init, leaving eng+8 as junk. With the real init running, vt[17]
//   populates eng+8 with the genuine AssetDB, and overwriting it sends vt[36]
//   into garbage -- an AV at guest 0x4D5854F1 inside orig_MainLoop.
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

// The per-callee timing probes that found this are REMOVED, having done their
// job. They wrapped hot recursive guest functions in two steady_clock reads
// each, which is its own cost on the path being measured. The chain they
// established is in AGENTS.md.
