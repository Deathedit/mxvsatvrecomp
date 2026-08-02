// DIAGNOSTIC HOOKS — log critical functions that fail in native mode. Most
// only log when --gpu_plugin=xenos (g_plugin_mode=true) and otherwise fall
// straight through to the guest original; sub_8253AA40 logs in both modes.
//
// Note the mid-ASM hooks that skip these call sites are unconditional (see
// midasm_stubs.cpp) — they fire in plugin mode too, so a hook being silent
// means its call site is jumped, not that the mode is wrong.

#include "hooks/hook_common.h"

#include <rex/cvar.h>

#include <algorithm>
#include <bit>
#include <string>

// The loader reaches state 2 (IdleClearRenderBusy) and parks there — it is idle,
// not stuck, and nothing in the game ever asks it for the next load.
//
// `sub_82534980(AssetDB, name, flags)` is the guest's own load-request API: it
// copies up to 260 bytes of `name` into AssetDB+29540 and, if the selector is
// sitting at 2, moves it to 3. Set `force_load = "<scene>"` in mx.toml (or pass
// --force_load=<scene>) to make that call once from idle, so the content/entity/
// draw path downstream of a load can be exercised at all. Empty means off.
//
// This supersedes the earlier force_launch, which wrote AssetDB+28 directly.
// That was the wrong lever — see AGENTS.md — and is gone.
REXCVAR_DEFINE_STRING(force_load, "", "Debug",
                      "Scene name to request from the AssetDB loader once it goes idle");

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

  // --force_load=<scene>: make the load request the front end never makes, using
  // the guest's own API rather than writing engine state.
  //
  // sub_82534980(AssetDB, name, flags) copies up to 260 bytes of `name` into
  // AssetDB+29540 and, when the selector is at 2, sets it to 3 and notifies the
  // listener at *(a1+110788). Everything downstream reads AssetDB+29540 as
  // "is a load pending" (name[0] != 0), which is why an empty name parks at 2.
  //
  // Wait for 30 consecutive ticks in state 2 so this cannot race the 0->1->2
  // boot sequence, and fire exactly once.
  if (!mx::native::g_plugin_mode && a1) {
    static int s_idle = 0;
    static bool s_fired = false;
    const std::string& scene = REXCVAR_GET(force_load);
    if (!s_fired && !scene.empty()) {
      s_idle = (state_out == 2) ? s_idle + 1 : 0;
      if (s_idle >= 30) {
        s_fired = true;
        // sub_82352AE0 reads its AssetDB from a different global than our hooks
        // do. Log both so the assumption that they are the same object is
        // visible in the log rather than buried here.
        REXLOG_INFO("native: force_load \"{}\" at call #{} — a1=0x{:08X} "
                    "*(0x830577C0)=0x{:08X} *(0x830A77C0)=0x{:08X} state={}",
                    scene, sm, a1, REX_LOAD_U32(0x830577C0),
                    REX_LOAD_U32(0x830A77C0), state_out);

        // Carve a scratch buffer out of the guest stack for the name. PPC frames
        // grow down and callees do `stwu r1,-N(r1)`, so parking r1 below the
        // buffer keeps the callee's frames clear of it and our caller's frame
        // above it. Restore r1 and the argument registers afterwards.
        const uint32_t saved_r1 = ctx.r1.u32;
        const uint32_t saved_r3 = ctx.r3.u32;
        const uint32_t saved_r4 = ctx.r4.u32;
        const uint32_t saved_r5 = ctx.r5.u32;
        const uint32_t buf = (saved_r1 - 1024) & ~15u;
        const size_t n = std::min<size_t>(scene.size(), 259);
        for (size_t i = 0; i < n; ++i)
          REX_STORE_U8(buf + static_cast<uint32_t>(i),
                       static_cast<uint8_t>(scene[i]));
        REX_STORE_U8(buf + static_cast<uint32_t>(n), 0);

        ctx.r1.u32 = buf - 256;
        ctx.r3.u32 = a1;
        ctx.r4.u32 = buf;
        ctx.r5.u32 = 0;
        REX_CALL_INDIRECT_FUNC(0x82534980);
        ctx.r1.u32 = saved_r1;
        ctx.r3.u32 = saved_r3;
        ctx.r4.u32 = saved_r4;
        ctx.r5.u32 = saved_r5;

        REXLOG_INFO("native: force_load returned — state now {}, name[0]=0x{:02X}",
                    REX_LOAD_U32(a1 + 28), REX_LOAD_U8(a1 + 29540));
      }
    }
  }
}

//=============================================================================
// The load-request chain
//
// sub_82534980 is the guest's load-request API. It has exactly one caller,
// sub_82352AE0 (mx_recomp.15.cpp:76710), which builds the scene name from a
// registry lookup and is itself a method with five callers. Nothing in this
// chain has ever been observed to run in native mode — the point of these hooks
// is to find out how far up it execution actually reaches, so entry-only logging
// is enough for everything except sub_82534980 itself.
//=============================================================================

namespace {

// Read a NUL-terminated guest string for logging. Bounded at 260 because that is
// the buffer size sub_82534980 copies into.
std::string GuestString(uint8_t* base, uint32_t addr, size_t max = 260) {
  std::string s;
  if (!addr) return s;
  for (size_t i = 0; i < max; ++i) {
    uint8_t c = REX_LOAD_U8(addr + static_cast<uint32_t>(i));
    if (!c) break;
    s.push_back(static_cast<char>(c));
  }
  return s;
}

}  // namespace

// sub_82534980 — AssetDB_RequestLoad(AssetDB, name, flags). Copies up to 260
// bytes of `name` to AssetDB+29540, stores flags at +29800, and moves the
// selector 2 -> 3.
REX_IMPORT(__imp__sub_82534980, orig_RequestLoad, void());
extern "C" REX_FUNC(sub_82534980) {
  const char* tag = mx::native::g_plugin_mode ? "plugin" : "native";
  uint32_t a1 = ctx.r3.u32;
  std::string name = GuestString(base, ctx.r4.u32);
  uint32_t state_in = a1 ? REX_LOAD_U32(a1 + 28) : 0xFFFFFFFF;
  REXLOG_INFO("{}: RequestLoad(sub_82534980) a1=0x{:08X} name=\"{}\" flags=0x{:08X} state={}",
              tag, a1, name, ctx.r5.u32, state_in);
  orig_RequestLoad(ctx, base);
  REXLOG_INFO("{}: RequestLoad returned — state {} -> {}", tag, state_in,
              a1 ? REX_LOAD_U32(a1 + 28) : 0xFFFFFFFF);
}

// sub_82352AE0 — the sole caller of RequestLoad; resolves the scene name from
// the registry. Takes a `this` pointer in r3.
REX_IMPORT(__imp__sub_82352AE0, orig_RequestLoadCaller, void());
extern "C" REX_FUNC(sub_82352AE0) {
  REXLOG_INFO("{}: sub_82352AE0 (RequestLoad caller) ENTER this=0x{:08X}",
              mx::native::g_plugin_mode ? "plugin" : "native", ctx.r3.u32);
  orig_RequestLoadCaller(ctx, base);
}

// The five callers of sub_82352AE0. Entry-only — one line each is enough to see
// where the chain stops.
#define MX_CHAIN_PROBE(addr, sym)                                             \
  REX_IMPORT(__imp__sub_##addr, orig_chain_##addr, void());                   \
  extern "C" REX_FUNC(sub_##addr) {                                           \
    static int n = 0;                                                         \
    if (++n <= 5)                                                             \
      REXLOG_INFO("{}: chain " sym " ENTER #{} r3=0x{:08X}",                  \
                  mx::native::g_plugin_mode ? "plugin" : "native", n,         \
                  ctx.r3.u32);                                                \
    orig_chain_##addr(ctx, base);                                             \
  }

MX_CHAIN_PROBE(82367A50, "sub_82367A50")
MX_CHAIN_PROBE(8236B470, "sub_8236B470")
MX_CHAIN_PROBE(8236B660, "sub_8236B660")
MX_CHAIN_PROBE(824FB1F0, "sub_824FB1F0")
MX_CHAIN_PROBE(824FC9A0, "sub_824FC9A0")

#undef MX_CHAIN_PROBE

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
