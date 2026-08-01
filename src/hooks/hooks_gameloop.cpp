// Game loop hooks — MainLoop and RenderPipeline.
//
// MainLoop is what keeps the process ticking at 60fps: it forces the render
// flag on, applies the eng+8 self-ref workaround that keeps MainLoop's vt[36]
// call landing on Bootstrap's nullsub_1, forces r3=1 so the guest's
// `while (MainLoop != 0)` never exits, and paces the loop with Sleep(16).

#include "hooks/hook_common.h"

//=============================================================================
// sub_82B70760 — Main game loop
//=============================================================================

REX_IMPORT(__imp__sub_82B70760, orig_MainLoop, void());
extern "C" REX_FUNC(sub_82B70760) {
  if (mx::native::g_plugin_mode) {
    static int plm = 0;
    ++plm;
    if (plm == 1 || (plm % 600) == 0)
      REXLOG_INFO("plugin: MainLoop #{}", plm);
    // At frame 60 and 600, dump engine sub-entity state
    if (plm == 60 || plm == 600 || plm == 1800) {
      REXLOG_INFO("plugin: === engine state dump at MainLoop #{} ===", plm);
      uint32_t eng = REX_LOAD_U32(0x830BE400);
      REXLOG_INFO("plugin: eng=0x{:08X} eng+8=0x{:08X}", eng, eng ? REX_LOAD_U32(eng+8) : 0);
      // Engine sub-entity list (offsets +0x1C..+0x24 per AGENTS.md post-dispatch body)
      if (eng) {
        for (int off = 0x1C; off <= 0x24; off += 4) {
          uint32_t sub_entity_ptr = REX_LOAD_U32(eng + off);
          REXLOG_INFO("plugin: eng+0x{:X}=0x{:08X}", off, sub_entity_ptr);
          if (sub_entity_ptr) {
            uint32_t vt = REX_LOAD_U32(sub_entity_ptr);
            uint32_t f3C = REX_LOAD_U32(sub_entity_ptr + 0x3C);
            REXLOG_INFO("plugin:   vt=0x{:08X} +0x3C=0x{:08X}", vt, f3C);
          }
        }
      }
      // Entity counts
      REXLOG_INFO("plugin: pass0=0x{:08X} pass1=0x{:08X} pass2=0x{:08X}",
        REX_LOAD_U32(0x830C2150), REX_LOAD_U32(0x830C4560), REX_LOAD_U32(0x830C6970));
      // byte_82D57994 (render flag)
      REXLOG_INFO("plugin: byte_82D57994={}", REX_LOAD_U8(0x82D57994));
      // dword_830BE190
      REXLOG_INFO("plugin: dword_830BE190=0x{:08X}", REX_LOAD_U32(0x830BE190));
    }
    orig_MainLoop(ctx, base);
    return;
  }
  static int ml = 0;
  ++ml;
  REX_STORE_U8(0x82D57994, 1);
  if (ml > 600) REX_STORE_U8(0x82D57994, 0);

  if (ml == 1) {
    uint32_t eng = REX_LOAD_U32(0x830BE400);
    uint32_t sub = REX_LOAD_U32(eng + 12);
    uint32_t sub_vt = sub ? REX_LOAD_U32(sub) : 0;
    uint32_t sub_f36 = sub_vt ? REX_LOAD_U32(sub_vt + 144) : 0;
    uint32_t sub_f0 = sub_vt ? REX_LOAD_U32(sub_vt) : 0;
    // Diagnostic: dump the AssetDB global (dword_830577C0) and its vtable.
    // Goal: determine whether writing eng+8 = AssetDB would give MainLoop's
    // vt[36] call a real function (low 0x82XXXXXX address) or string data
    // (high non-function-range value that would crash on call). The Bootstrap
    // vtable's vt[36]=nullsub_1 is the current safe workaround; we want to
    // know if the real AssetDB's vt[36] is also safe to call.
    uint32_t assetdb = REX_LOAD_U32(0x830577C0);
    uint32_t assetdb_vt = assetdb ? REX_LOAD_U32(assetdb) : 0;
    uint32_t assetdb_f0 = assetdb_vt ? REX_LOAD_U32(assetdb_vt) : 0;
    uint32_t assetdb_f36 = assetdb_vt ? REX_LOAD_U32(assetdb_vt + 144) : 0;
    // Also dump the transition_renderer+8 (= dword_830EC250) which EngineInit
    // writes at 0x82ba7fe4 from sub_8253CF08. This is the slot LoaderTick's
    // (a1+8)->vtable[6] = *(a1+0x1C) gating check reads — NOT engine+8.
    uint32_t tr_assetdb = REX_LOAD_U32(0x830EC250);
    uint32_t tr_assetdb_vt = tr_assetdb ? REX_LOAD_U32(tr_assetdb) : 0;
    uint32_t tr_assetdb_f0 = tr_assetdb_vt ? REX_LOAD_U32(tr_assetdb_vt) : 0;
    uint32_t tr_assetdb_f6 = tr_assetdb_vt ? REX_LOAD_U32(tr_assetdb_vt + 24) : 0;
    REX_STORE_U32(eng + 8, eng);  // keep self-ref workaround active
    REXLOG_INFO("native: eng+12=0x{:08X} vt=0x{:08X} vt[0]=0x{:08X} vt[36]=0x{:08X}",
      sub, sub_vt, sub_f0, sub_f36);
    REXLOG_INFO("native: assetdb=0x{:08X} vt=0x{:08X} vt[0]=0x{:08X} vt[36]=0x{:08X}",
      assetdb, assetdb_vt, assetdb_f0, assetdb_f36);
    REXLOG_INFO("native: tr+8(0x830EC250)=0x{:08X} vt=0x{:08X} vt[0]=0x{:08X} vt[6]=0x{:08X}",
      tr_assetdb, tr_assetdb_vt, tr_assetdb_f0, tr_assetdb_f6);
  }

  orig_MainLoop(ctx, base);
  ctx.r3.u32 = 1;
  if ((ml % 60) == 1) REXLOG_INFO("native: MainLoop #{}", ml);
  ::Sleep(16);
}

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
  // Skip orig_RenderPipeline while Bink is playing — the host render thread
  // owns the D3D12 swapchain for Bink video. Calling guest VdSwap concurrently
  // would conflict with the host renderer's Present.
  if (mx::native::IsBinkPlaying()) {
    if (rp == 1) REXLOG_INFO("native: RenderPipeline #{} — skipped (Bink playing)", rp);
    return;
  }
  if (rp == 1 || (rp % 600) == 0)
    REXLOG_INFO("native: RenderPipeline #{} — calling orig", rp);
  orig_RenderPipeline(ctx, base);
}
