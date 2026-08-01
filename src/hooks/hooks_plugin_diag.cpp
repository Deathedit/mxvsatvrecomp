// DIAGNOSTIC HOOKS — log critical functions that fail in native mode. Most
// only log when --gpu_plugin=xenos (g_plugin_mode=true) and otherwise fall
// straight through to the guest original; sub_8253AA40 logs in both modes.
//
// Note the mid-ASM hooks that skip these call sites are unconditional (see
// midasm_stubs.cpp) — they fire in plugin mode too, so a hook being silent
// means its call site is jumped, not that the mode is wrong.

#include "hooks/hook_common.h"

#include <bit>

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
  uint32_t state = a1 ? REX_LOAD_U32(a1 + 110796) : 0;
  if (sm <= 50 || (sm % 200) == 0)
    REXLOG_INFO("{}: LoadStateMachine #{} a1=0x{:08X} state={}", tag, sm, a1, state);
  orig_LoadStateMachine(ctx, base);
  if (sm <= 50 || (sm % 200) == 0)
    REXLOG_INFO("{}: LoadStateMachine #{} returned r3=0x{:08X} state={}", tag, sm,
                ctx.r3.u32, a1 ? REX_LOAD_U32(a1 + 110796) : 0);
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
