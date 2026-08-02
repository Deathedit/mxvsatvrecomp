// DIAGNOSTIC HOOKS — log critical functions that fail in native mode. Most
// only log when --gpu_plugin=xenos (g_plugin_mode=true) and otherwise fall
// straight through to the guest original; sub_8253AA40 logs in both modes.
//
// Note the mid-ASM hooks that skip these call sites are unconditional (see
// midasm_stubs.cpp) — they fire in plugin mode too, so a hook being silent
// means its call site is jumped, not that the mode is wrong.

#include "hooks/hook_common.h"

#include <rex/cvar.h>

#include <bit>

// The loader reaches state 2 (IdleClearRenderBusy) and parks there — it is idle,
// not stuck, and nothing in the game ever asks it for the next load. Set
// `force_launch = true` in mx.toml (or pass --force_launch=true) to fabricate
// that request once, so the content/entity/draw path downstream of a load can be
// exercised at all. Diagnostic only; it is not how the game is supposed to work.
REXCVAR_DEFINE_BOOL(force_launch, false, "Debug",
                    "Force the AssetDB load state machine from idle into LaunchActivity");

// sub_82B34998 — RendererDispatchBlock (fatal vtable dispatches in native)
REX_IMPORT(__imp__sub_82B34998, orig_RendererDispatch, void());
extern "C" REX_FUNC(sub_82B34998) {
  if (mx::native::g_plugin_mode) {
    static int rd = 0;
    ++rd;
    if (rd <= 20 || (rd % 500) == 0)
      REXLOG_INFO("plugin: RendererDispatch #{} a1=0x{:08X} f1={:.2f}",
                  rd, ctx.r3.u32, *(float*)&ctx.f1);
    orig_RendererDispatch(ctx, base);
    if (rd <= 20 || (rd % 500) == 0)
      REXLOG_INFO("plugin: RendererDispatch #{} returned r3=0x{:08X}", rd, ctx.r3.u32);
    return;
  }
  orig_RendererDispatch(ctx, base);
}

// sub_82B3C7D0 — lazy-init alloc (hangs in Transition thread in native)
REX_IMPORT(__imp__sub_82B3C7D0, orig_LazyInit, void());
extern "C" REX_FUNC(sub_82B3C7D0) {
  if (mx::native::g_plugin_mode) {
    static int li = 0;
    ++li;
    if (li <= 10) {
      REXLOG_INFO("plugin: LazyInit(sub_82B3C7D0) #{}", li);
      LogEngSlot8(base, "LazyInit ENTER");
    }
    orig_LazyInit(ctx, base);
    if (li <= 10) {
      REXLOG_INFO("plugin: LazyInit #{} returned r3=0x{:08X}", li, ctx.r3.u32);
      LogEngSlot8(base, "LazyInit RETURNED");
    }
    return;
  }
  orig_LazyInit(ctx, base);
}

// sub_82B70370 — timing function (busy-wait spin in native)
REX_IMPORT(__imp__sub_82B70370, orig_Timing, void());
extern "C" REX_FUNC(sub_82B70370) {
  static int tm = 0;
  ++tm;
  bool loud = tm <= 5 || (tm % 1000) == 0;
  if (mx::native::g_plugin_mode) {
    if (loud) REXLOG_INFO("plugin: Timing(sub_82B70370) #{} a1=0x{:08X}", tm, ctx.r3.u32);
    orig_Timing(ctx, base);
    return;
  }
  // NATIVE: stubbed. This is frame pacing — it QPCs a delta, divides by the
  // perf frequency, and stores elapsed seconds at a1+24. Two things in it are
  // fatal here, both because hook #5 skips the SetupRenderer band that would
  // have initialized this struct (0x830EC248):
  //   - a1+20 (target frame time) drives a busy-wait at 0x82B703D4
  //   - a1+32 is a ring index used as an unbounded store offset
  //     (`REX_STORE_U32(*(a1+32) + a1, dt)`), which access-violates on garbage.
  // Downstream only consumes the dt at a1+24, so supply a fixed 60Hz step.
  uint32_t a1 = ctx.r3.u32;
  if (loud) {
    REXLOG_INFO("native: Timing #{} STUBBED a1=0x{:08X} +20=0x{:08X} +32=0x{:08X}",
                tm, a1, a1 ? REX_LOAD_U32(a1 + 20) : 0, a1 ? REX_LOAD_U32(a1 + 32) : 0);
  }
  if (a1) REX_STORE_U32(a1 + 24, std::bit_cast<uint32_t>(1.0f / 60.0f));
}

// sub_82B6D230 — called from LoaderTick's entity block @0x82B70E4C with f1=dt.
// Frontier probe: with hook #7 off, execution reaches TexManager @0x82B70E44
// then dies. This separates "dies in sub_82B6D230" from "dies in the entity
// loops @0x82B70E54".
REX_IMPORT(__imp__sub_82B6D230, orig_EntityDt, void());
extern "C" REX_FUNC(sub_82B6D230) {
  static int ed = 0;
  ++ed;
  bool loud = ed <= 5;
  if (loud) REXLOG_INFO("native: sub_82B6D230 #{} ENTER", ed);
  orig_EntityDt(ctx, base);
  if (loud) REXLOG_INFO("native: sub_82B6D230 #{} RETURNED", ed);
}

// sub_8253AA40 — AssetDB_LoadStateMachine (LoaderTick's gate, 12-state)
REX_IMPORT(__imp__sub_8253AA40, orig_LoadStateMachine, void());
// Logs in BOTH modes: in native this is currently unreachable (mid-ASM hooks
// #7/#8 delete LoaderTick's vt[6] call site), so its absence from the log is
// itself the signal, and it starts reporting the moment those hooks come off.
extern "C" REX_FUNC(sub_8253AA40) {
  const char* tag = mx::native::g_plugin_mode ? "plugin" : "native";
  static int sm = 0;
  ++sm;
  uint32_t a1 = ctx.r3.u32;
  // The state is *(a1+28) — a 0..11 selector. Derived from the recompiled body:
  // mx_recomp.31.cpp:36836 `lwz r11,28(r31)` (r31 = a1, never reassigned) feeds
  // the 12-entry jump table at :36862. The `+110796` this used to read came
  // from pm4_pipeline.md and is a guest heap pointer, not the enum, so every
  // "state=" line logged before 2026-08-02 was meaningless.
  uint32_t state_in = a1 ? REX_LOAD_U32(a1 + 28) : 0xFFFFFFFF;
  // Every store to *(a1+28) I could find is inside this function, but that was
  // a grep of mx_recomp.31.cpp only and would miss a write through a computed
  // pointer regardless. Settle it with data: if the state changed between our
  // last return and this entry, something outside sub_8253AA40 wrote it.
  static uint32_t s_prev_out = 0xFFFFFFFE;
  if (s_prev_out != 0xFFFFFFFE && state_in != s_prev_out) {
    REXLOG_INFO("{}: EXTERNAL WRITE to AssetDB+28: {} -> {} between calls #{} and #{}",
                tag, s_prev_out, state_in, sm - 1, sm);
  }
  orig_LoadStateMachine(ctx, base);
  uint32_t state_out = a1 ? REX_LOAD_U32(a1 + 28) : 0xFFFFFFFF;
  s_prev_out = state_out;
  // Log every transition, plus a periodic heartbeat — a stuck machine should be
  // visible without diffing consecutive lines.
  static uint32_t s_last = 0xFFFFFFFE;
  bool changed = state_out != s_last;
  s_last = state_out;
  if (changed || sm <= 10 || (sm % 200) == 0) {
    REXLOG_INFO("{}: LoadStateMachine #{} state {} -> {} r3=0x{:08X}{}", tag, sm,
                state_in, state_out, ctx.r3.u32, changed ? "  <-- CHANGED" : "");
  }

  // --force_launch=true: fabricate the load request the front end never makes.
  //
  // The machine reaches state 2 (IdleClearRenderBusy) and parks. State 2's body
  // is `*(a1+110328) = 0` then the common tail (mx_recomp.31.cpp:37093) — it
  // never writes *(a1+28), so it cannot self-advance and only an external write
  // gets it moving.
  //
  // Target state 3 (DatabaseLoad), the head of the ordered load sequence
  // 3 -> 4 -> 5 -> 6 -> 7 -> 8 -> 11 -> 2.
  //
  // Do NOT jump straight to 9 (LaunchActivity). Measured 2026-08-02 (mx_018.log):
  // it access-violates reading guest 0x00020768 inside sub_82541F80 +0xE4, whose
  // first instruction is `lwzx r29,r3,0x20768` with r3 = *(a1+23132). That slot
  // is written only by sub_825372C0 (mx_recomp.31.cpp:28616,29130), which is the
  // "Subscene Creation" callback state 4 registers — so 9 is only reachable once
  // 4 has run, and forcing it from idle skips exactly that setup. Both of state
  // 9's branches call sub_82541F80, so neither is safe.
  //
  // Wait for 30 consecutive ticks in state 2 so this cannot race the 0->1->2
  // boot sequence, and fire exactly once.
  if (!mx::native::g_plugin_mode && a1 && REXCVAR_GET(force_launch)) {
    static int s_idle = 0;
    static bool s_fired = false;
    if (!s_fired) {
      s_idle = (state_out == 2) ? s_idle + 1 : 0;
      if (s_idle >= 30) {
        s_fired = true;
        REXLOG_INFO("native: force_launch — kicking AssetDB 0x{:08X} from state 2 "
                    "into 3 (DatabaseLoad) at call #{}; a1+23132=0x{:08X} "
                    "a1+110328=0x{:08X} a1+110260=0x{:08X}",
                    a1, sm, REX_LOAD_U32(a1 + 23132), REX_LOAD_U32(a1 + 110328),
                    REX_LOAD_U32(a1 + 110260));
        REX_STORE_U32(a1 + 28, 3);
      }
    }
  }
}

// sub_82B38558 — TerminatorVtableCtor (installs off_8213F70C vtable)
REX_IMPORT(__imp__sub_82B38558, orig_VtableCtor, void());
extern "C" REX_FUNC(sub_82B38558) {
  if (mx::native::g_plugin_mode) {
    LogEngSlot8(base, "VtableCtor ENTER");
    REXLOG_INFO("plugin: VtableCtor(sub_82B38558) a1=0x{:08X}", ctx.r3.u32);
    orig_VtableCtor(ctx, base);
    uint32_t a1 = ctx.r3.u32;
    uint32_t vt = a1 ? REX_LOAD_U32(a1) : 0;
    REXLOG_INFO("plugin: VtableCtor done r3=0x{:08X} *a1->vt=0x{:08X}", ctx.r3.u32, vt);
    LogEngSlot8(base, "VtableCtor RETURNED");
    return;
  }
  orig_VtableCtor(ctx, base);
}
