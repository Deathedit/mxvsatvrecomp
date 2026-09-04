// Loading-path hooks -- SetupRenderer, Transition, LoaderTick.
//
// This is the part of the boot sequence mid-ASM hooks used to carve into. **NO
// MID-ASM HOOK IS ACTIVE ANY MORE**: mx_asm.toml is 107 of 116 lines commented
// with nothing live, mx_config.toml has no mid-ASM section, and the stubs in
// midasm_stubs.cpp are referenced only from commented lines. Nothing is skipped,
// the dispatch RUNS, and it is observed by a plain function hook on
// sub_82B34998.
//
// Comments below still describe hook #6's behaviour in the past tense; read them
// as history, not as the current configuration.

#include "hooks/hook_common.h"

//=============================================================================
// GPU renderer shim (Path 2) -- DIAGNOSTIC HOOKS DISABLED.
//
// Three hooks were trialed during Path 1/2 experimentation: sub_82B2C9D0 (TLS
// gate), sub_82B307D8 (NULL-deref bypass), and a SetupRenderer pre-population of
// dword_830BE190. They are NOT needed in the baseline. Findings preserved in
// AGENTS.md; re-enabling requires codegen.
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

  // DORMANT fallback. SetupRenderer's vt[17] writes `*(eng+8) = assetdb_block`
  // -- the 545KB block allocated and initialized alongside it. This existed
  // because mid-ASM hook #4 used to skip vt[17], leaving eng+8 NULL; #4 is
  // disabled, so vt[17] runs and the branch below is not taken.
  //
  // If it does run it re-allocates the block and installs the same vtable, but
  // SKIPS vt[17]'s secondary sub_82526D10 call (18-subsystem AssetDB
  // registration) -- so it is not a faithful substitute.
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
  // Bisect aid for the LoaderTick entity block (0x82B70E18..0x82B70EC8), which
  // access-violates when mid-ASM hook #7 is disabled. The two candidates are the
  // indirect calls at 0x82B70E28 (engine->vt[2]() -> scene manager) and
  // 0x82B70E3C (sceneMgr->vt[32](dt)). Log the slots read-only -- do NOT call
  // them -- so we can see which resolves to garbage. Sane targets are 0x82XXXXXX.
  {
    uint32_t eng = REX_LOAD_U32(0x830BE400);
    uint32_t eng_vt = eng ? REX_LOAD_U32(eng) : 0;
    uint32_t vt2 = eng_vt ? REX_LOAD_U32(eng_vt + 8) : 0;
    REXLOG_INFO("native: [bisect] eng=0x{:08X} eng_vt=0x{:08X} vt[2]=0x{:08X}",
                eng, eng_vt, vt2);
    // vt[2] (sub_82ABB448) is just `return *(this+52)` — it cannot fault. So
    // the crash is the next call, sceneMgr->vt[32](dt) at 0x82B70E3C. Resolve
    // that chain read-only.
    uint32_t sm = eng ? REX_LOAD_U32(eng + 52) : 0;
    uint32_t sm_vt = sm ? REX_LOAD_U32(sm) : 0;
    uint32_t vt32 = sm_vt ? REX_LOAD_U32(sm_vt + 128) : 0;
    REXLOG_INFO("native: [bisect] sceneMgr=0x{:08X} sm_vt=0x{:08X} vt[32]=0x{:08X}",
                sm, sm_vt, vt32);
    // dword_830B03EC is the GPU physical base; it stays 0 because our
    // SetupGuestGpu is a no-op stub. Entity/scene code may derive from it.
    REXLOG_INFO("native: [bisect] gpu_phys=0x{:08X} tr+24(dt)=0x{:08X}",
                REX_LOAD_U32(0x830B03EC), REX_LOAD_U32(0x830EC248 + 24));
    // LoaderTick's first instruction is Wait(*(tr+0x194), -1). Now that the fake
    // INFINITE-wait success is gone, that wait is real and the Transition thread
    // parks in it. Log the handle so it can be matched against the NtSetEvent
    // handles actually being signalled.
    REXLOG_INFO("native: [bisect] tr+0x194(LoaderTick wait handle)=0x{:08X} tr+0x2DC=0x{:08X}",
                REX_LOAD_U32(0x830EC248 + 0x194), REX_LOAD_U32(0x830EC248 + 0x2DC));
    // LoaderTick's entity loops @0x82B70E54 walk engine sub-entities at
    // eng+0x1C..0x24 and dereference *(sub+0x3C). A NULL sub-entity faults.
    for (uint32_t off = 0x1C; off <= 0x24; off += 4) {
      uint32_t sub = eng ? REX_LOAD_U32(eng + off) : 0;
      REXLOG_INFO("native: [bisect] eng+0x{:X}=0x{:08X} +0x3C=0x{:08X}", off, sub,
                  sub ? REX_LOAD_U32(sub + 0x3C) : 0xDEADDEAD);
    }

    // --- Renderer-block probe -----------------------------------------------
    // Describes 0x82B70EC8..0x82B710BC. Hook #6 used to delete this band
    // wholesale, then narrowed to skipping only the `bl sub_82B34998` dispatch;
    // it is disabled now, so the whole band runs. These reads established that
    // the band's inputs were real before the narrowing.

    // Lazy-init at 0x82B70EE8 is `bctrl` through dword_82D5648C. When #6 was
    // last disabled execution stalled right here (midasm_stubs.cpp:33) — but
    // be190 is already populated by then, so the bne branches past it.
    uint32_t lazy_fn = REX_LOAD_U32(0x82D5648C);
    uint32_t be190 = REX_LOAD_U32(0x830BE190);
    uint32_t be190_vt = be190 ? REX_LOAD_U32(be190) : 0;
    // 0x8213F7A4 = the real vtable (all functions). 0x8213F70C = the stub
    // vtable whose slots dispatch to sub_82BDB190 (fatal exit). Which one the
    // object carries decides whether sub_82B34998 could ever run natively.
    REXLOG_INFO("native: [bisect] lazyinit_fn(0x82D5648C)=0x{:08X} be190=0x{:08X} vt=0x{:08X}",
                lazy_fn, be190, be190_vt);

    // Final call of the band, at 0x82B710A4: engine[0xC]->vt[3]().
    uint32_t scene = eng ? REX_LOAD_U32(eng + 0xC) : 0;
    uint32_t scene_vt = scene ? REX_LOAD_U32(scene) : 0;
    uint32_t scene_vt3 = scene_vt ? REX_LOAD_U32(scene_vt + 12) : 0;
    REXLOG_INFO("native: [bisect] eng+0xC(scene)=0x{:08X} vt=0x{:08X} vt[3]=0x{:08X}",
                scene, scene_vt, scene_vt3);

    // The band's own event handshake: Wait on tr+0x2E0 at 0x82B70FA8, then
    // NtSetEvent(tr+0x190) at 0x82B70FF4. Distinct from tr+0x194/tr+0x2DC.
    REXLOG_INFO("native: [bisect] tr+0x190(band signals)=0x{:08X} tr+0x2E0(band waits)=0x{:08X}",
                REX_LOAD_U32(0x830EC248 + 0x190), REX_LOAD_U32(0x830EC248 + 0x2E0));
  }
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
  // The `if (r3 != 0 && lt > 100) r3 = 0` cap is REMOVED. It was a FABRICATED
  // completion from when LoaderTick's body was deleted by mid-ASM hooks #7/#8
  // and the loop had to be broken artificially -- nothing was ever loaded. The
  // body now runs for real and AssetDB_LoadStateMachine ticks each iteration, so
  // forcing r3=0 would kill the Transition loop mid-load.
  if (lt <= 5 || lt % 500 == 0)
    REXLOG_INFO("native: LoaderTick #{} r3={}", lt, ctx.r3.u32);
}
