// Boot / teardown hooks — the engine bring-up chain and its cleanup.
//
// Most of these exist for diagnostics: they log guest state around the guest
// original and otherwise leave behavior alone. The two that do more are
// EngineInit (installs the guest memory base on NativeGraphics, then parks the
// thread in a sleep loop to keep the process alive) and the two Cleanup hooks
// (zero their globals instead of running the guest teardown).

#include "hooks/hook_common.h"

//=============================================================================
// sub_82AB7848 — GpuAlloc
//=============================================================================

REX_IMPORT(__imp__sub_82AB7848, orig_GpuAlloc, void());
extern "C" REX_FUNC(sub_82AB7848) {
  if (mx::native::g_plugin_mode) {
    static int ga = 0;
    ++ga;
    uint32_t sz = ctx.r3.u32;
    orig_GpuAlloc(ctx, base);
    uint32_t addr = ctx.r3.u32;
    if (ga <= 30) {
      REXLOG_INFO("plugin: GpuAlloc #{} size=0x{:08X} -> 0x{:08X}", ga, sz, addr);
    }
    return;
  }
  static int ga = 0;
  ++ga;
  uint32_t sz = ctx.r3.u32;
  orig_GpuAlloc(ctx, base);
  uint32_t addr = ctx.r3.u32;
  // Same cap as the plugin branch above, deliberately. It used to be 8, which
  // made native look like it stopped allocating after #8 while the plugin ran
  // on to #30 — a divergence that was purely the two gates disagreeing.
  if (ga <= 30) {
    REXLOG_INFO("native: GpuAlloc #{} size=0x{:08X} -> 0x{:08X}", ga, sz, addr);
  }
}

//=============================================================================
// sub_82533D80 — Cleanup1
//=============================================================================

REX_IMPORT(__imp__sub_82533D80, orig_Cleanup1, void());
extern "C" REX_FUNC(sub_82533D80) {
  if (mx::native::g_plugin_mode) { orig_Cleanup1(ctx, base); return; }
  REXLOG_INFO("native: Cleanup1 (0x82533D80)");
  REX_STORE_U32(0x830577C0, 0);
}

//=============================================================================
// sub_82B70BE8 — Cleanup2 (stubbed)
//=============================================================================

REX_IMPORT(__imp__sub_82B70BE8, orig_Cleanup2, void());
extern "C" REX_FUNC(sub_82B70BE8) {
  if (mx::native::g_plugin_mode) { orig_Cleanup2(ctx, base); return; }
  REXLOG_INFO("native: Cleanup2 (0x82B70BE8) — stubbed");
  REX_STORE_U32(0x830BE190, 0);
}

//=============================================================================
// sub_82AE9658 — Post-GraphicsInit setup (called from GraphicsInit)
//=============================================================================

REX_IMPORT(__imp__sub_82AE9658, orig_PostGfxInit, void());
extern "C" REX_FUNC(sub_82AE9658) {
  if (mx::native::g_plugin_mode) {
    REXLOG_INFO("plugin: PostGfxInit (0x82AE9658)");
    LogEngSlot8(base, "PostGfxInit ENTER");
    orig_PostGfxInit(ctx, base);
    LogEngSlot8(base, "PostGfxInit RETURNED");
    return;
  }
  REXLOG_INFO("native: PostGfxInit (0x82AE9658)");
  orig_PostGfxInit(ctx, base);
}

//=============================================================================
// sub_82373660 — Texture manager (called after GraphicsInit in SetupRenderer)
//=============================================================================

REX_IMPORT(__imp__sub_82373660, orig_TexManager, void());
extern "C" REX_FUNC(sub_82373660) {
  if (mx::native::g_plugin_mode) {
    static int tm = 0;
    ++tm;
    if (tm <= 5 || (tm % 1000) == 0) {
      REXLOG_INFO("plugin: TexManager #{} a1=0x{:08X}", tm, ctx.r3.u32);
      LogEngSlot8(base, "TexManager");
    }
    orig_TexManager(ctx, base);
    return;
  }
  // Same cap as the plugin branch above, deliberately — the second instance of
  // the GpuAlloc gate mismatch. This branch had no cap at all and logged every
  // call: 7,910 lines in mx_1270, 9.7% of the whole log, on the guest's own
  // thread. The counter is carried so the surviving lines say WHICH call they
  // are, which an uncapped line could not.
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
  if (mx::native::g_plugin_mode) {
    static int bt = 0;
    ++bt;
    if (bt <= 30 || (bt % 1000) == 0) {
      REXLOG_INFO("plugin: BindTexture #{} r3=0x{:08X} r4=0x{:08X} r5=0x{:08X}",
                  bt, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
      LogEngSlot8(base, "BindTexture");
    }
    orig_BindTexture(ctx, base);
    return;
  }
  REXLOG_INFO("native: BindTexture (0x82B6F820)");
  orig_BindTexture(ctx, base);
}

//=============================================================================
// Bootstrap / GraphicsInit / EngineInit (logging)
//=============================================================================

REX_IMPORT(__imp__sub_82ABB838, orig_Bootstrap, void());
extern "C" REX_FUNC(sub_82ABB838) {
  // The crash reporter needs this to tell a guest address from a host pointer,
  // so it is set in BOTH modes and before the original runs -- a fault inside
  // orig_Bootstrap should still classify. EngineInit used to own this; that hook
  // was deleted 2026-08-16 and took the only writer with it, which left
  // `in_guest` false for every address until 2026-08-17. Bootstrap is the
  // earliest hook here that runs unconditionally at boot.
  mx::native::NativeGraphics::Get().SetGuestMemory(base);
  if (mx::native::g_plugin_mode) {
    LogEngSlot8(base, "Bootstrap ENTER");
    orig_Bootstrap(ctx, base);
    LogEngSlot8(base, "Bootstrap RETURNED");
    return;
  }
  REXLOG_INFO("native: Bootstrap (0x82ABB838)");
  orig_Bootstrap(ctx, base);
}

REX_IMPORT(__imp__sub_82AEBF40, orig_GraphicsInit, void());
extern "C" REX_FUNC(sub_82AEBF40) {
  if (mx::native::g_plugin_mode) {
    LogEngSlot8(base, "GraphicsInit ENTER");
    uint32_t a1 = ctx.r3.u32;
    REXLOG_INFO("plugin: GraphicsInit a1=0x{:08X}", a1);
    if (a1) {
      REXLOG_INFO("plugin: GraphicsInit dev +56=0x{:08X} +104=0x{:08X} +2388=0x{:08X}",
        REX_LOAD_U32(a1 + 56),
        REX_LOAD_U32(a1 + 104),
        REX_LOAD_U32(a1 + 2388));
    }
    orig_GraphicsInit(ctx, base);
    if (a1) {
      uint32_t gpu_base = REX_LOAD_U32(0x830B03EC);
      REXLOG_INFO("plugin: GraphicsInit done +56=0x{:08X} +104=0x{:08X} +2388=0x{:08X} gpu_phys=0x{:08X}",
        REX_LOAD_U32(a1 + 56),
        REX_LOAD_U32(a1 + 104),
        REX_LOAD_U32(a1 + 2388),
        gpu_base);
      // Dump more device fields for native backend design
      for (int off = 0; off < 2400; off += 4) {
        uint32_t val = REX_LOAD_U32(a1 + off);
        if (val != 0 && off != 56 && off != 104 && off != 2388) {
          REXLOG_INFO("plugin: dev+{}=0x{:08X}", off, val);
        }
      }
    }
    LogEngSlot8(base, "GraphicsInit RETURNED");
    return;
  }
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
// The sub_82BA7F58 (EngineInit) hook is REMOVED 2026-08-16, having run disabled
// for ~3-4 hours of sessions covering boot, menu, level load and level exit.
//
// It did three things, and the middle one is why it could not stay:
//
//   NativeGraphics::SetGuestMemory(base) -- and that was the ONLY caller.
//
//     CORRECTED 2026-08-17. This block used to continue "GetGuestMemory() was
//     already called by nobody, [so] the accessor pair and the member are
//     therefore dead". **That was wrong.** app/mx_app.cpp calls GetGuestMemory()
//     in the crash reporter to decide whether a faulting address lies inside
//     guest memory. Deleting this hook set m_guest_base to nobody, so gbase was
//     0, so `in_guest` was false for EVERY address and guest faults were
//     reported as host-side pointers for a day. The Bootstrap hook above now
//     sets it. The claim was made by grepping src/hooks/ instead of the whole
//     tree -- scope a deadness check to the repo, never to a directory.
//
//   `for (;;) ::Sleep(16);` after calling the original -- it parked the guest's
//     init thread forever to keep the process alive. That is a bring-up scaffold
//     from before the render thread owned the frame loop, and it means this hook
//     never returned.
//
//   Two log lines and the eng+8 slot dump.
//
// git history has the body.
