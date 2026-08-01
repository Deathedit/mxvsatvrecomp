// Loading-path hooks — SetupRenderer, Transition, LoaderTick.
//
// This is the part of the boot sequence that mid-ASM hooks #2, #5, #6, #7 and
// #8 carve into: the C++ hooks here run around the guest originals while the
// mid-ASM hooks skip the interior blocks that cannot complete without a GPU.
// See the mid-ASM hook table in AGENTS.md.

#include "hooks/hook_common.h"

//=============================================================================
// GPU renderer shim (Path 2) — DIAGNOSTIC HOOKS DISABLED.
//
// Three hooks were trialed during Path 1/2 experimentation: sub_82B2C9D0 (TLS
// gate), sub_82B307D8 (NULL-deref bypass), and a SetupRenderer pre-population
// of dword_830BE190. They are NOT needed in the baseline — mid-ASM hook #6
// skips sub_82B34998 entirely. Findings preserved in AGENTS.md "PATH 1
// EXPERIMENT" and "Path 2" sections. Re-enabling requires codegen.
//=============================================================================

//=============================================================================
// sub_82B71148 — Renderer setup (called BEFORE MainLoop)
//=============================================================================

REX_IMPORT(__imp__sub_82B71148, orig_SetupRenderer, void());
extern "C" REX_FUNC(sub_82B71148) {
  if (mx::native::g_plugin_mode) {
    LogEngSlot8(base, "SetupRenderer ENTER");
    REXLOG_INFO("plugin: SetupRenderer ENTER a1=0x{:08X}", ctx.r3.u32);
    orig_SetupRenderer(ctx, base);
    LogEngSlot8(base, "SetupRenderer RETURNED");
    REXLOG_INFO("plugin: SetupRenderer RETURNED — dumping state");
    // Dump engine state
    uint32_t eng = REX_LOAD_U32(0x830BE400);
    REXLOG_INFO("plugin: eng(0x830BE400)=0x{:08X}", eng);
    if (eng) {
      for (int off = 0; off <= 40; off += 4)
        REXLOG_INFO("plugin: eng+{}=0x{:08X}", off, REX_LOAD_U32(eng + off));
    }
    // Dump transition renderer state
    uint32_t tr = 0x830EC248;
    REXLOG_INFO("plugin: transition_renderer+8(AssetDB)=0x{:08X}", REX_LOAD_U32(0x830EC250));
    REXLOG_INFO("plugin: transition_renderer+0x190=0x{:08X} +0x194=0x{:08X} +0x2DC=0x{:08X} +0x2E0=0x{:08X}",
      REX_LOAD_U32(tr + 0x190), REX_LOAD_U32(tr + 0x194),
      REX_LOAD_U32(tr + 0x2DC), REX_LOAD_U32(tr + 0x2E0));
    // dword_830BE190 — the 60KB block
    uint32_t be190 = REX_LOAD_U32(0x830BE190);
    REXLOG_INFO("plugin: dword_830BE190=0x{:08X}", be190);
    if (be190) {
      REXLOG_INFO("plugin: BE190+0=0x{:08X} +4=0x{:08X} +8=0x{:08X} +68=0x{:08X} +72=0x{:08X}",
        REX_LOAD_U32(be190), REX_LOAD_U32(be190+4), REX_LOAD_U32(be190+8),
        REX_LOAD_U32(be190+68), REX_LOAD_U32(be190+72));
      uint32_t vt = REX_LOAD_U32(be190);
      REXLOG_INFO("plugin: BE190 vtable=0x{:08X}", vt);
      if (vt >= 0x82000000) {
        for (int i = 0; i < 20; ++i)
          REXLOG_INFO("plugin: BE190 vt[{}]=0x{:08X}", i, REX_LOAD_U32(vt + i*4));
      }
    }
    // Entity count globals
    REXLOG_INFO("plugin: pass0_count(0x830C2150)={} pass1_count(0x830C4560)={} pass2_count(0x830C6970)={}",
      REX_LOAD_U32(0x830C2150), REX_LOAD_U32(0x830C4560), REX_LOAD_U32(0x830C6970));
    // GPU physical base
    REXLOG_INFO("plugin: gpu_phys(0x830B03EC)=0x{:08X}", REX_LOAD_U32(0x830B03EC));
    return;
  }
  REXLOG_INFO("native: SetupRenderer ENTER (0x82B71148)");
  orig_SetupRenderer(ctx, base);
  REXLOG_INFO("native: SetupRenderer RETURNED");

  // Native backend fix: SetupRenderer's vt[17] call (sub_82B43AC8 @ 0x82B71310)
  // is skipped by mid-ASM hook #4. That call writes `*(eng+8) = assetdb_block`
  // (the 545KB block allocated at 0x82B712D8 by sub_82AB73C0(0x85280) and
  // initialized by AssetDB_InnerCtor_VtableInstall at 0x82B712EC). The
  // allocation + ctor themselves actually run in native mode (hook #3 only
  // skips the vt[8] call before them; hook #4 skips vt[17] AFTER them).
  // However, since hook #4 prevents vt[17] from running, eng+8 stays NULL.
  //
  // We replicate vt[17]'s `*(eng+8) = assetdb_block` write here from C++.
  // The 545KB block allocated during orig_SetupRenderer is gone (no global
  // references it unless vt[17] ran to write eng+8). So we re-allocate it
  // ourselves and call AssetDB_InnerCtor_VtableInstall to set up the same
  // vtable its natural code would, then write eng+8.
  //
  // We SKIP the secondary sub_82526D10 (18-subsystem AssetDB registration)
  // call that vt[17] would have made — those subsystems depend on plugin-
  // provided state we don't have, and registering them risks crashes for
  // unclear benefit (assets are loaded host-side in our native path).
  uint32_t eng = REX_LOAD_U32(0x830BE400);
  if (eng && !REX_LOAD_U32(eng + 8)) {
    REXLOG_INFO("native: eng+8 is NULL — replicating vt[17] write from C++");
    // Allocate 545KB block (same size as SetupRenderer natural code 0x85280)
    uint32_t saved_r3 = ctx.r3.u32;
    uint32_t saved_r4 = ctx.r4.u32;
    ctx.r3.u32 = 0x85280;
    REX_CALL_INDIRECT_FUNC(0x82AB73C0);  // sub_82AB73C0(0x85280) — heap alloc
    uint32_t block_ptr = ctx.r3.u32;
    ctx.r3.u32 = saved_r3;
    ctx.r4.u32 = saved_r4;
    if (block_ptr) {
      REXLOG_INFO("native: AssetDB block alloc'd at 0x{:08X}", block_ptr);
      // Call AssetDB_InnerCtor_VtableInstall (sub_82BAB700) — installs vtable
      // off_8214518C at *(block+0) and calls sub_82AB7560(block + 342*4) for
      // inner init. Takes block_ptr as a1 (r3).
      uint32_t ctor_saved_r3 = ctx.r3.u32;
      uint32_t ctor_saved_r4 = ctx.r4.u32;
      ctx.r3.u32 = block_ptr;
      REX_CALL_INDIRECT_FUNC(0x82BAB700);  // AssetDB_InnerCtor_VtableInstall
      ctx.r3.u32 = ctor_saved_r3;
      ctx.r4.u32 = ctor_saved_r4;
      // Write eng+8 = block_ptr (the critical vt[17] effect)
      REX_STORE_U32(eng + 8, block_ptr);
      REXLOG_INFO("native: eng+8 written to 0x{:08X} (was NULL)", block_ptr);
    } else {
      REXLOG_INFO("native: WARNING — 545KB AssetDB block alloc failed");
    }
  } else if (eng) {
    REXLOG_INFO("native: eng+8 already populated (0x{:08X})", REX_LOAD_U32(eng + 8));
  } else {
    REXLOG_INFO("native: WARNING — dword_830BE400 (engine) is NULL");
  }
}

//=============================================================================
// sub_82B710D0 — Transition (overridden: skip NtSetEvent block)
//=============================================================================

REX_IMPORT(__imp__sub_82B710D0, orig_Transition, void());
extern "C" REX_FUNC(sub_82B710D0) {
  if (mx::native::g_plugin_mode) {
    static int pt = 0;
    ++pt;
    if (pt <= 3)
      REXLOG_INFO("plugin: Transition #{}", pt);
    orig_Transition(ctx, base);
    if (pt <= 3)
      REXLOG_INFO("plugin: Transition #{} returned", pt);
    return;
  }
  REXLOG_INFO("native: Transition (0x82B710D0)");
  orig_Transition(ctx, base);
  REXLOG_INFO("native: Transition returned");
}

//=============================================================================
// sub_82B70DE8 — LoaderTick (stubbed: crashes without GPU renderer)
//=============================================================================

REX_EXTERN(__imp__sub_82B70DE8);
REX_HOOK_RAW(sub_82B70DE8) {
  if (mx::native::g_plugin_mode) {
    static int plt = 0;
    ++plt;
    __imp__sub_82B70DE8(ctx, base);
    if (plt <= 10 || (plt % 10000) == 0)
      REXLOG_INFO("plugin: LoaderTick #{} r3={}", plt, ctx.r3.u32);
    return;
  }
  static int lt = 0;
  ++lt;
  __imp__sub_82B70DE8(ctx, base);
  // Per IDA decompile of sub_82B70DE8 tail: `if (result) { renderer block; }`
  // gate is bare `if(result)` — ANY nonzero r3 triggers the renderer. The
  // previous `r3 == 1` cap only matched one code path and would miss other
  // nonzero returns (e.g. wait timeouts, pointer values from vtable[6]).
  if (ctx.r3.u32 != 0 && lt > 100) {
    ctx.r3.u32 = 0;
  }
  if (lt <= 5 || lt == 101 || lt % 1000 == 0)
    REXLOG_INFO("native: LoaderTick #{} r3={}", lt, ctx.r3.u32);
}
