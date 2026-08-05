// Game loop hooks — MainLoop and RenderPipeline.
//
// MainLoop is what keeps the process ticking at 60fps: it forces the render
// flag on, applies the eng+8 self-ref workaround that keeps MainLoop's vt[36]
// call landing on Bootstrap's nullsub_1, forces r3=1 so the guest's
// `while (MainLoop != 0)` never exits, and paces the loop with Sleep(16).

#include "hooks/hook_common.h"

#include <chrono>

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

  // byte_82D57994 gates MainLoop's call to RenderPipeline: at 0x82B707B0 and
  // again at 0x82B7080C a zero here jumps straight to the vt[36] tail, so the
  // guest render path never runs. The only guest write to this byte is in
  // Transition (mx_recomp.82.cpp:67620), and it writes 0 on loader exit —
  // nothing in the guest sets it, so forcing it is load-bearing.
  //
  // The old code also cleared it at frame 600, a leftover fabricated
  // "loading complete" signal. That closed the render window ~20s in, entirely
  // inside the 47.4s Bink intro, so RenderPipeline was skipped every time it
  // was reached. The clear is REMOVED 2026-08-02; log when the guest clears it
  // underneath us so the tug-of-war with Transition stays visible.
  {
    uint8_t before = REX_LOAD_U8(0x82D57994);
    static uint8_t s_last = 1;
    if (before != s_last) {
      REXLOG_INFO("native: byte_82D57994 changed {} -> {} by guest at MainLoop #{}",
                  s_last, before, ml);
      s_last = before;
    }
    REX_STORE_U8(0x82D57994, 1);
  }

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
    // SELF-REF WORKAROUND DISABLED 2026-08-02. `REX_STORE_U32(eng + 8, eng)`
    // was needed when hooks #2/#5 skipped the engine init: eng+8 held junk and
    // MainLoop's eng+8->vt[36] had to be forced onto Bootstrap's nullsub_1.
    // Now that the real init runs, vt[17] populates eng+8 with the genuine
    // AssetDB (logged as "eng+8 already populated") and overwriting it sends
    // vt[36] into garbage — the AV at guest 0x4D5854F1 inside orig_MainLoop.
    REXLOG_INFO("native: eng+8=0x{:08X} (self-ref override disabled)",
                REX_LOAD_U32(eng + 8));
    REXLOG_INFO("native: eng+12=0x{:08X} vt=0x{:08X} vt[0]=0x{:08X} vt[36]=0x{:08X}",
      sub, sub_vt, sub_f0, sub_f36);
    REXLOG_INFO("native: assetdb=0x{:08X} vt=0x{:08X} vt[0]=0x{:08X} vt[36]=0x{:08X}",
      assetdb, assetdb_vt, assetdb_f0, assetdb_f36);
    REXLOG_INFO("native: tr+8(0x830EC250)=0x{:08X} vt=0x{:08X} vt[0]=0x{:08X} vt[6]=0x{:08X}",
      tr_assetdb, tr_assetdb_vt, tr_assetdb_f0, tr_assetdb_f6);
  }

  // Drive the loader. LoaderTick's first instruction is Wait(*(tr+0x194), -1):
  // a per-frame handshake the guest renderer thread would satisfy. Hook #6
  // skips that renderer because our D3D12 backend replaces it, so nothing
  // ticks the loader and the Transition thread parks forever. Signal it here,
  // once per frame, which is the cadence the renderer would have used.
  {
    uint32_t loader_evt = REX_LOAD_U32(0x830EC248 + 0x194);
    if (loader_evt) {
      uint32_t s3 = ctx.r3.u32, s4 = ctx.r4.u32;
      ctx.r3.u32 = loader_evt;
      ctx.r4.u32 = 0;                      // NtSetEvent(handle, prev_state=NULL)
      REX_CALL_INDIRECT_FUNC(0x82BFB748);  // sub_82BFB748 — NtSetEvent
      ctx.r3.u32 = s3;
      ctx.r4.u32 = s4;
    }
  }

  // Time the guest's own body. Native gets under 61 iterations in 75s against
  // plugin mode's 1200 in 73s, and turning the HLE renderer off changes nothing,
  // so the cost is inside orig_MainLoop rather than in anything above it. Log
  // every iteration until the shape is known — at this rate that is a handful of
  // lines, not a flood.
  const auto t0 = std::chrono::steady_clock::now();
  orig_MainLoop(ctx, base);
  const auto body_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - t0)
                           .count();
  if (ml <= 60 || body_ms >= 100)
    REXLOG_INFO("native: MainLoop #{} body={}ms", ml, body_ms);
  ctx.r3.u32 = 1;
  if ((ml % 60) == 1) {
    REXLOG_INFO("native: MainLoop #{}", ml);
    // Is the AssetDB alive, and what state does its load machine sit in?
    // State is *(AssetDB+28), the 0..11 selector of sub_8253AA40's jump table
    // (mx_recomp.31.cpp:36836). tr+8 (0x830EC250) is the slot LoaderTick's
    // (a1+8)->vt[6] actually reads.
    uint32_t adb = REX_LOAD_U32(0x830577C0);
    uint32_t tr8 = REX_LOAD_U32(0x830EC250);
    REXLOG_INFO("native: assetdb=0x{:08X} state={} tr+8=0x{:08X}",
                adb, adb ? REX_LOAD_U32(adb + 28) : 0xFFFFFFFF, tr8);
    // RenderPipeline now runs every frame and its PM4 is present-only: no
    // DRAW_* and no INDIRECT_BUFFER, just SWAP/scanout registers. If these
    // three per-pass entity counts are zero, the scene really is empty and the
    // missing draws are a guest-side content problem, not a translator one.
    REXLOG_INFO("native: entities pass0={} pass1={} pass2={} gpu_phys=0x{:08X}",
                REX_LOAD_U32(0x830C2150), REX_LOAD_U32(0x830C4560),
                REX_LOAD_U32(0x830C6970), REX_LOAD_U32(0x830B03EC));
  }
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
  const auto t0 = std::chrono::steady_clock::now();
  orig_RenderPipeline(ctx, base);
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - t0)
                      .count();
  if (ms >= 50) REXLOG_INFO("native: RenderPipeline #{} took {}ms", rp, ms);
}

//=============================================================================
// MainLoop's other callees, timed
//=============================================================================
// MainLoop's body runs 350ms rising to 3000ms natively against ~16ms under the
// plugin, and it is not the 500ms poll wait (measured) and not BeginFrame /
// EndFrame (stubbed natively, no original called). These are the rest of what it
// calls directly, read off the recompiled body of sub_82B70760: the pump inside
// the poll loop, and the two calls on the way in.
#define MX_TIME_CALLEE(addr, orig, label)                                  \
  REX_IMPORT(__imp__##addr, orig, void());                                 \
  extern "C" REX_FUNC(addr) {                                              \
    if (mx::native::g_plugin_mode) {                                       \
      orig(ctx, base);                                                     \
      return;                                                              \
    }                                                                      \
    const auto t0 = std::chrono::steady_clock::now();                      \
    orig(ctx, base);                                                       \
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>( \
                        std::chrono::steady_clock::now() - t0)             \
                        .count();                                          \
    if (ms >= 50) {                                                        \
      static uint64_t s_n = 0;                                             \
      REXLOG_INFO("native: {} slow #{} {}ms", label, ++s_n, ms);           \
    }                                                                      \
  }

MX_TIME_CALLEE(sub_82AB6F18, orig_MainLoopPump, "MainLoopPump")
MX_TIME_CALLEE(sub_82BDB290, orig_MainLoopPre1, "MainLoopPre1")
MX_TIME_CALLEE(sub_82BFC090, orig_MainLoopPre2, "MainLoopPre2")

// RenderPipeline turned out to be 100% of MainLoop's body, and VdSwap is at most
// ~87ms of that (its own orig never reaches 50ms). These are the rest of what
// sub_82B70578 calls directly — sub_82ABF828 and sub_82ABF930 are excluded
// because hooks_frame.cpp already stubs them natively, so they cost nothing.
MX_TIME_CALLEE(sub_82B600A8, orig_RpCallee1, "RP.sub_82B600A8")
MX_TIME_CALLEE(sub_82AFE978, orig_RpCallee2, "RP.sub_82AFE978")
MX_TIME_CALLEE(sub_82ABF638, orig_RpCallee3, "RP.sub_82ABF638")
MX_TIME_CALLEE(sub_82ABCC48, orig_RpCallee4, "RP.sub_82ABCC48")
