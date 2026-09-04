// Boot / teardown hooks — the engine bring-up chain and its cleanup.
//
// Most exist for diagnostics: they log guest state around the guest original and
// otherwise leave behaviour alone. The two that do more are EngineInit
// (installs the guest memory base on NativeGraphics, then parks the thread in a
// sleep loop) and the two Cleanup hooks (zero their globals instead of running
// the guest teardown).

#include "hooks/hook_common.h"

//=============================================================================
// sub_82AB7848 — GpuAlloc
//=============================================================================

REX_IMPORT(__imp__sub_82AB7848, orig_GpuAlloc, void());
extern "C" REX_FUNC(sub_82AB7848) {
  static int ga = 0;
  ++ga;
  uint32_t sz = ctx.r3.u32;
  orig_GpuAlloc(ctx, base);
  uint32_t addr = ctx.r3.u32;
  if (ga <= 30) {
    REXLOG_INFO("native: GpuAlloc #{} size=0x{:08X} -> 0x{:08X}", ga, sz, addr);
  }
}

//=============================================================================
// sub_82533D80 — Cleanup1
//=============================================================================

extern "C" REX_FUNC(sub_82533D80) {
  REXLOG_INFO("native: Cleanup1 (0x82533D80)");
  REX_STORE_U32(0x830577C0, 0);
}

//=============================================================================
// sub_82B70BE8 — Cleanup2 (stubbed apart from the worker join)
//=============================================================================

// SetEvent, sub_82BFB748. Only the worker kick below needs it.
REX_IMPORT(__imp__sub_82BFB748, orig_SetEvent, int(uint32_t));

extern "C" REX_FUNC(sub_82B70BE8) {
  REXLOG_INFO("native: Cleanup2 (0x82B70BE8) — stubbed");
  // The rest of the guest teardown is skipped deliberately; these two writes
  // are not skippable. SetupRenderer starts a job worker (sub_82B708D8) that
  // waits on self+0x190 with NO timeout and leaves the loop only when
  // self+0x1C0 reads -1, so without the sentinel and the wake it parks there
  // for the life of the process.
  const uint32_t self = ctx.r3.u32;
  if (self) {
    REX_STORE_U32(self + 0x1C0u, 0xFFFFFFFFu);
    const uint32_t go = REX_LOAD_U32(self + 0x190u);
    if (!go)
      REXLOG_WARN("native: Cleanup2 worker go-event is NULL, nothing woken");
    else if (!orig_SetEvent(go))
      REXLOG_ERROR("native: Cleanup2 SetEvent(0x{:08X}) failed", go);
  }
  REX_STORE_U32(0x830BE190, 0);
}

//=============================================================================
// sub_82AE9658 — Post-GraphicsInit setup (called from GraphicsInit)
//=============================================================================

REX_IMPORT(__imp__sub_82AE9658, orig_PostGfxInit, void());
extern "C" REX_FUNC(sub_82AE9658) {
  REXLOG_INFO("native: PostGfxInit (0x82AE9658)");
  orig_PostGfxInit(ctx, base);
}

//=============================================================================
// sub_82373660 — Texture manager (called after GraphicsInit in SetupRenderer)
//=============================================================================

REX_IMPORT(__imp__sub_82373660, orig_TexManager, void());
extern "C" REX_FUNC(sub_82373660) {
  static int tm = 0;
  ++tm;
  if (tm <= 5 || (tm % 1000) == 0)
    REXLOG_INFO("native: TexManager #{} (0x82373660)", tm);
  orig_TexManager(ctx, base);
}

//=============================================================================
// sub_82B6F820 — Bind texture (called after GraphicsInit in SetupRenderer)
//=============================================================================

REX_IMPORT(__imp__sub_82B6F820, orig_BindTexture, void());
extern "C" REX_FUNC(sub_82B6F820) {
  REXLOG_INFO("native: BindTexture (0x82B6F820)");
  orig_BindTexture(ctx, base);
}

//=============================================================================
// Bootstrap / GraphicsInit / EngineInit (logging)
//=============================================================================

REX_IMPORT(__imp__sub_82ABB838, orig_Bootstrap, void());
extern "C" REX_FUNC(sub_82ABB838) {
  // The crash reporter needs this to tell a guest address from a host pointer,
  // and it is set before the original runs -- a fault inside orig_Bootstrap
  // should still classify. EngineInit used to own this; that hook was deleted
  // and took the only writer with it, which left `in_guest` false for every
  // address until Bootstrap picked it up.
  mx::native::NativeGraphics::Get().SetGuestMemory(base);
  REXLOG_INFO("native: Bootstrap (0x82ABB838)");
  orig_Bootstrap(ctx, base);
}

REX_IMPORT(__imp__sub_82AEBF40, orig_GraphicsInit, void());
extern "C" REX_FUNC(sub_82AEBF40) {
  uint32_t a1 = ctx.r3.u32;
  REXLOG_INFO("native: GraphicsInit a1=0x{:08X}", a1);
  if (a1) {
    // Dump initial render state fields
    REXLOG_INFO("native: GraphicsInit dev +56=0x{:08X} +104=0x{:08X} +2388=0x{:08X}",
      REX_LOAD_U32(a1 + 56),
      REX_LOAD_U32(a1 + 104),
      REX_LOAD_U32(a1 + 2388));
  }
  orig_GraphicsInit(ctx, base);
  // After GraphicsInit, read what it stored
  if (a1) {
    uint32_t gpu_base = REX_LOAD_U32(0x830B03EC);
    REXLOG_INFO("native: GraphicsInit done +56=0x{:08X} +104=0x{:08X} gpu_phys=0x{:08X}",
      REX_LOAD_U32(a1 + 56),
      REX_LOAD_U32(a1 + 104),
      gpu_base);
  }
}
// The sub_82BA7F58 (EngineInit) hook is REMOVED, having run disabled for ~3-4
// hours of sessions covering boot, menu, level load and level exit. It did three
// things, and the middle one is why it could not stay:
//
//   NativeGraphics::SetGuestMemory(base) -- and that was the ONLY caller.
//
//     CORRECTED: this block used to continue "GetGuestMemory() was already
//     called by nobody, [so] the accessor pair and the member are therefore
//     dead". **That was wrong.** app/mx_app.cpp calls GetGuestMemory() in the
//     crash reporter to decide whether a faulting address lies inside guest
//     memory, so deleting this hook made `in_guest` false for EVERY address for
//     a day. The claim was made by grepping src/hooks/ instead of the whole tree
//     -- scope a deadness check to the repo, never to a directory.
//
//   `for (;;) ::Sleep(16);` after calling the original -- it parked the guest's
//     init thread forever to keep the process alive, a bring-up scaffold from
//     before the render thread owned the frame loop.
//
//   Two log lines and the eng+8 slot dump.
//
// git history has the body.
