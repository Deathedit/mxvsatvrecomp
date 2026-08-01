// PLUGIN-MODE DIAGNOSTIC HOOKS — log critical functions that fail in native
// mode. These only fire when --gpu_plugin=xenos (g_plugin_mode=true). In
// native mode they're unreachable (mid-ASM hooks skip the calling code) and
// each one falls straight through to the guest original.

#include "hooks/hook_common.h"

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
  if (mx::native::g_plugin_mode) {
    static int tm = 0;
    ++tm;
    if (tm <= 5 || (tm % 1000) == 0)
      REXLOG_INFO("plugin: Timing(sub_82B70370) #{} a1=0x{:08X}", tm, ctx.r3.u32);
    orig_Timing(ctx, base);
    return;
  }
  orig_Timing(ctx, base);
}

// sub_8253AA40 — AssetDB_LoadStateMachine (LoaderTick's gate, 12-state)
REX_IMPORT(__imp__sub_8253AA40, orig_LoadStateMachine, void());
extern "C" REX_FUNC(sub_8253AA40) {
  if (mx::native::g_plugin_mode) {
    static int sm = 0;
    ++sm;
    uint32_t a1 = ctx.r3.u32;
    uint32_t state = a1 ? REX_LOAD_U32(a1 + 110796) : 0;
    if (sm <= 50 || (sm % 200) == 0)
      REXLOG_INFO("plugin: LoadStateMachine #{} a1=0x{:08X} state={}", sm, a1, state);
    orig_LoadStateMachine(ctx, base);
    if (sm <= 50 || (sm % 200) == 0)
      REXLOG_INFO("plugin: LoadStateMachine #{} returned r3=0x{:08X}", sm, ctx.r3.u32);
    return;
  }
  orig_LoadStateMachine(ctx, base);
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
