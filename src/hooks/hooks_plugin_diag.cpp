// DIAGNOSTIC HOOKS — log critical functions that fail in native mode. Most
// only log when --gpu_plugin=xenos (g_plugin_mode=true) and otherwise fall
// straight through to the guest original; sub_8253AA40 logs in both modes.
//
// Note the mid-ASM hooks that skip these call sites are unconditional (see
// midasm_stubs.cpp) — they fire in plugin mode too, so a hook being silent
// means its call site is jumped, not that the mode is wrong.

#include "hooks/hook_common.h"
#include "hooks/hooks_d3d9.h"  // GuestDrawCalls
#include "gpu/xenos_gpu_state.h"  // mx::gpu::alu -- the PM4 ALU constant file

#include <rex/cvar.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

// force_load is GONE, 2026-08-28, cvar and implementation both. It named a
// scene to request from the AssetDB once the loader went idle, by calling the
// guest's own load-request API sub_82534980(AssetDB, name, flags) from the
// LoadStateMachine hook below. It superseded an earlier force_launch that wrote
// AssetDB+28 directly, which was the wrong lever.
//
// The observation behind it still stands and is still unexplained: the loader
// reaches state 2 (IdleClearRenderBusy) and parks, because nothing in the game
// ever calls sub_82534980. That same idle AssetDB is why UI_World never loads,
// and it is the root of the 0x8234CE20 crash -- a bink asset in the
// never-requested UIAnimations package resolves NULL and the guest dereferences
// it with no check. Neither lever ever explained WHY nothing asks.

// Every string setting the guest reads comes from MXRegistry.bxml through one
// function, sub_825487C8(registry, key, out, size, 0). `registry_override` takes
// comma-separated key=value pairs and substitutes the value for a matching key
// as it is read, which is what shipping a different MXRegistry.bxml would do —
// tools/ has bxml decoders but no encoder, so this is the only way to change one.
// Empty means off. Diagnostic only. See AGENTS.md "the registry chokepoint".
// Defined in hooks_wait.cpp, beside the wait hook that fills it.
std::string GuestWaitReport(double seconds);

REXCVAR_DEFINE_STRING(registry_override, "", "Debug",
                      "Comma-separated key=value overrides for guest registry string reads");

// HOW MANY TIMES THE WORLD IS ADVANCED PER FRAME, and by what dt.
//
// Measured on the user's main rig: the guest's own clock tracks real time to
// 0.1% (GUEST CLOCK 0.9987x), dt equals host elapsed, the timer fires 1.005
// times per swap, and capping the frame rate at 30 changes nothing -- yet the
// game runs visibly fast. Every one of those rules out a timing error.
//
// What survives is an update that runs MORE THAN ONCE per frame. World time
// would still be correct, because the timer advances once; entities would
// advance twice. That is frame-rate independent, which is precisely why the
// 30 fps cap made no difference.
//
// So this counts the dt consumers per frame rather than inferring them. dt is
// read as ctx.f1.f64 -- a PPCRegister's value member is a double, and the
// existing `*(float*)&ctx.f1` in sub_82B34998 reads the low four bytes of one,
// which is why that probe has printed 0.00/-0.00 in every run ever taken.
namespace {
std::atomic<uint32_t> g_updEntity{0};     // sub_82B6D230, the entity pass
std::atomic<uint32_t> g_updRenderer{0};   // sub_82B34998, RendererDispatch
std::atomic<uint32_t> g_updFixed60{0};    // entity-pass calls at a hardcoded 1/60
std::atomic<uint32_t> g_updAltPath{0};    // sub_82AB58F8, which supplies that 1/60
std::atomic<uint32_t> g_updRendererExec{0};  // sub_82B33EC0, dispatches EXECUTED
std::atomic<uint64_t> g_gpuIntVblank{0};    // graphics interrupt, source 0
std::atomic<uint64_t> g_gpuIntSwap{0};      // graphics interrupt, source 1
std::atomic<uint64_t> g_gpuIntOther{0};
std::atomic<uint64_t> g_vblankHandler{0};   // sub_825663B0, the flip pass

// IS ANY ENTITY STEPPED TWICE IN ONE FRAME?
//
// The only shape left that fits every measurement. sub_82B6D230 walks a vector
// of pointers calling sub_82B6A448(entity, dt) on each; the vector runs once a
// frame with the correct dt, so the world CLOCK is right whatever it contains.
// But if an entity is present twice, it is integrated twice per frame -- and
// motion doubles while every clock, call count and execution count stays at
// 1.000/frame. That is exactly the evidence: correct time, wrong speed.
//
// vt[128] is ruled out as the world update -- it resolved to sub_82B6E298,
// which walks four controller ports through XamInputGetCapabilities. Input,
// not physics.
//
// Counted per frame: total steps, and how many DISTINCT entities they landed
// on. calls == distinct is healthy. calls == 2 x distinct is the bug, and the
// duplicate's address names the entity.
struct StepVt {
  uint32_t vt = 0, f24 = 0, f28 = 0;
  uint64_t hits = 0;
};
constexpr size_t kMaxStepVts = 16;
StepVt g_stepVts[kMaxStepVts];
uint32_t g_stepVtCount = 0;
uint64_t g_stepVtOverflow = 0;

std::mutex g_stepMu;
std::vector<uint32_t> g_stepSeen;      // entities stepped this frame, in order
uint64_t g_stepCalls = 0;
uint64_t g_stepDup = 0;                // steps that hit an already-stepped entity
uint32_t g_stepDupExample = 0;
uint64_t g_stepFrames = 0;
uint64_t g_stepCallsTotal = 0, g_stepDistinctTotal = 0;

// THE GUEST'S FRAME CADENCE IS VBLANK-COUNTED, and we have never measured the
// rate we deliver it at.
//
// D3D9 registers sub_825582E0 through VdSetGraphicsInterruptCallback
// (sub_82569A50). Its first argument is the interrupt SOURCE -- 0 vblank,
// 1 swap-complete -- and on a vblank it runs sub_825663B0, which is the flip:
//
//     ++a1[4190];                                  // THE VBLANK COUNTER
//     if (pending[i].target_vblank > a1[4190]) break;   // not due yet
//     MEMORY[0x7FC86110] = front_buffer;           // flip
//     ++a1[4196];                                  // swaps completed
//
// So a queued swap waits until the vblank counter reaches its target, which is
// how the present interval is expressed on this hardware. Everything the title
// paces off that counter runs at whatever rate we deliver vblanks -- and that
// rate is independent of our swap rate, of frame_limit_fps, and of the QPC dt
// the Timing hook measures, which is why every clock measurement so far has
// come back at 1.00 while the game runs fast.
//
// The console delivers 60 vblanks a second. If we deliver the HOST's, a 120 Hz
// monitor is exactly 2x -- and it would be machine-dependent, which nothing
// else measured so far is.
constexpr double kConsoleVblankHz = 60.0;

// A SECOND WORLD CLOCK, AND AN EXPLICIT TIME SCALE.
//
// sub_82B09FF8 is a __noreturn guest worker with its own pacing and its own
// notion of time -- neither of which the tick's timing struct knows about, so
// every measurement taken so far has been blind to it:
//
//     while (1) {
//       Wait(a1+12768, INFINITE);
//       v6 = *(float*)(engine[16] + 64);    // THE CLOCK -- not 0x830EC248
//       v7 = *(float*)(engine[16] + 72);    // max delta
//       for each obj:
//           v11 = v6 - obj[9];              // elapsed since its last update
//           if (v11 >= obj[8]) {            // its own update interval
//               if (v11 > v7) v11 = v7;
//               obj[9] = v6;
//               obj->vt[84](obj, *(float*)(a1+12884) * v11);   // dt x SCALE
//           }
//       SetEvent(a1+12772);
//     }
//
// So objects on this thread advance by (their own elapsed) x (a scale), and
// both terms are unmeasured. GUEST CLOCK tracks 0x830EC248+60 and would read
// 1.00 no matter what this clock does, which is exactly the shape of the
// evidence: correct time, wrong speed.
//
// Read from the Timing hook rather than from inside the worker, because the
// worker never returns -- a hook there can log on entry and nothing after.
constexpr uint32_t kEngineSubSlot = 16;   // dword_830BE400 + 16
constexpr uint32_t kSubClock = 64;        // float, the clock
constexpr uint32_t kSubMaxDelta = 72;     // float
constexpr uint32_t kWorkerScale = 12884;  // float, on the worker's own object
std::atomic<uint32_t> g_physWorker{0};    // sub_82B09FF8's a1, captured at entry
std::atomic<uint64_t> g_gpuIntDropped{0};

}  // namespace

// THE VBLANK RATE IS THE HOST'S, AND THE CONSOLE'S WAS 60.
//
// The Xbox 360 delivered 60 vblanks a second, full stop, and the title's frame
// cadence counts them (sub_825663B0: ++a1[4190], retire any queued swap whose
// target vblank has arrived). Our source is the host display, so the guest sees
// whatever the monitor runs at -- 175 Hz on the user's main rig, which is 2.9x
// the rate every timer in this title was written against, and 0 on the dev VM,
// which has no real display and therefore no vsync source at all.
//
// That asymmetry is why this bug is machine-dependent and why nothing else in
// this hunt reproduced it: dt, GUEST CLOCK, world steps per frame and dispatch
// executions all measured identical on both machines, because all of them ride
// on QPC rather than on vblanks.
//
// This paces the guest's vblank to a fixed period the way the hardware did.
// Excess interrupts are DROPPED rather than coalesced -- the guest counts
// edges, so delivering one edge per period is exactly what the console did,
// and delivering a "catch-up" burst afterwards would be the thing being
// avoided. Same reasoning as the swap pacer in hooks_frame.cpp.
//
// Set to 0 to pass every host interrupt through, which is the old behaviour and
// the A/B control. Dropped counts are logged, so this can never be silent.
REXCVAR_DEFINE_INT32(guest_vblank_hz, 60, "Debug",
                     "Pace the guest's vblank interrupt to this many per "
                     "second, the way the console's fixed 60Hz did. 0 passes "
                     "the host's rate straight through");

namespace {

// A CALL IS NOT AN EXECUTION, and RendererDispatch is the case that proves it.
//
// sub_82B34998 opens with a queue-or-execute gate:
//
//     if (!sub_82B2C9D0()) return sub_82B36298(a1, 2, &dt, 0, 0, 0);   // ENQUEUE
//     sub_82B33EC0(a1, dt); vt[32](a1, dt); sub_82B307D8(a1, dt); vt[28](a1, dt);
//
// sub_82B36298 pushes command 2 carrying dt, and sub_82B34A68 -- a command
// dispatcher, a switch over ~26 ids -- pulls it back out at `case 2` and calls
// sub_82B34998 AGAIN. So one frame's dispatch is TWO calls and ONE execution,
// and the 1.999 calls/frame first measured here is the normal idiom rather
// than the world advancing twice. sub_82B2C9D0 has 25+ xrefs, every one the
// same shape; this is how the whole subsystem defers work off-thread.
//
// sub_82B33EC0 has exactly ONE code xref, from inside sub_82B34998 past the
// gate, so counting it counts executions exactly.
std::atomic<uint64_t> g_updEntityDtBits{0};
std::atomic<uint64_t> g_updRendererDtBits{0};

// THE SUSPECT, found in IDA 2026-08-30.
//
// sub_82AB58F8 advances the world through the SAME two calls the real tick
// (sub_82B70DE8) uses -- the engine update at vtable slot 128, then the entity
// pass sub_82B6D230 -- but with a HARDCODED 0.016666668, which is 1/60, in
// place of the measured frame dt:
//
//     v3 = (*(...)(*v2 + 128))(v2, 0.016666668);
//     v4 = sub_82373660(v3);
//     sub_82B6D230(v4, 0.016666668);
//
// If it runs alongside the tick, the world advances by real_dt + 1/60 each
// frame: 2.0x at 60 fps and 1.5x at 30, which is exactly the shape of the
// report -- clearly double at first, still fast after --frame_limit_fps=30 --
// and it is invisible to every clock measurement because the TIMER only ever
// accumulates its own dt. GUEST CLOCK stays at 1.00 while the world runs fast.
//
// It has no code xref: it is reached through a function-pointer table, so the
// caller cannot be named statically. Hence counted here rather than argued.
constexpr double kFixed60 = 0.016666668;
}  // namespace

// sub_82B34998 — RendererDispatchBlock, called from LoaderTick on the Transition
// thread.
//
// MADE MODE-NEUTRAL 2026-08-06, and keep it that way. It used to log only under
// the plugin and call the original silently in native, which hid the number that
// found the frame-pacing bug: `f1` arriving here was exactly 0.00 in native and
// varied under the plugin. **A one-sided probe was read as evidence that native
// behaved the same.** See the sub_82B70370 hook below.
//
// The same warning still applies to the rest of this file: most Transition-thread
// probes are plugin-only, so their absence from a native log means nothing.
REX_IMPORT(__imp__sub_82B34998, orig_RendererDispatch, void());
extern "C" REX_FUNC(sub_82B34998) {
  const char* tag = mx::native::g_plugin_mode ? "plugin" : "native";
  static int rd = 0;
  ++rd;
  const bool loud = rd <= 20 || (rd % 500) == 0;
  const uint32_t a1 = ctx.r3.u32;
  // ctx.f1.f64, NOT *(float*)&ctx.f1. PPCRegister's value member is a double,
  // so the old read took the low four bytes of one and printed a denormal --
  // 0.00 or -0.00 in every run this project has ever taken. The comment below
  // cites "f1 arriving here was exactly 0.00 in native" as evidence; that
  // reading was an artefact of this bug, though the dispatch-count measurements
  // beside it stand on their own.
  const double dt = ctx.f1.f64;
  g_updRenderer.fetch_add(1, std::memory_order_relaxed);
  g_updRendererDtBits.store(std::bit_cast<uint64_t>(dt),
                            std::memory_order_relaxed);
  orig_RendererDispatch(ctx, base);
  if (loud) {
    REXLOG_INFO("{}: RendererDispatch #{} a1=0x{:08X} f1={:.6f} -> r3=0x{:08X}",
                tag, rd, a1, dt, ctx.r3.u32);
  }
}

// sub_82B3C7D0 — accessor for dword_830BE190, with a null assert.
//
//   result = dword_830BE190;
//   if (!dword_830BE190) sub_82AB73C0(61568);   // assert helper, line number
//   return result;
//
// It was labelled "lazy-init alloc (hangs in Transition thread in native)".
// Both halves are wrong: it allocates nothing, and a load / branch / return
// cannot hang. The label predates the D3D9 HLE layer and was never re-derived.
//
// Corrected 2026-08-06 by decompiling it *and* measuring it, because reading a
// body is how the last three stale claims in this file survived. Native run
// mx_480: called twice, returns 0x212859A0 both times, assert path never taken.
// It neither hangs nor is skipped.
//
// Kept, and made mode-neutral, for the one thing worth watching: whether
// dword_830BE190 is ever null, which is the assert this function exists to
// raise.
REX_IMPORT(__imp__sub_82B3C7D0, orig_GetEngineGlobal, void());
extern "C" REX_FUNC(sub_82B3C7D0) {
  static int li = 0;
  ++li;
  orig_GetEngineGlobal(ctx, base);
  const bool null_global = ctx.r3.u32 == 0;
  if (li <= 5 || null_global) {
    REXLOG_INFO("{}: GetEngineGlobal(sub_82B3C7D0) #{} -> 0x{:08X}{}",
                mx::native::g_plugin_mode ? "plugin" : "native", li, ctx.r3.u32,
                null_global ? "  <-- NULL, assert path" : "");
  }
}

// sub_82B70370 — frame pacing: QPC delta / perf frequency -> dt at a1+24, then
// a 5-sample smoothing pass and the running totals at a1+56/60/64.
//
// This was stubbed in native mode until 2026-08-06, and that stub was the reason
// the front end never ran: it wrote a fixed 1/60 to a1+24 and nothing else, so
// a1+60 (total elapsed time) never advanced and the dt reaching LoaderTick's
// RendererDispatch was exactly 0.00. Unstubbing took the script VM from 28
// dispatches to 686, script assets from 2 to 4 and Bink opens from 0 to 3 —
// plugin mode's numbers exactly, 3/3 runs.
//
// The stub's two stated hazards were both false, and neither had been checked
// against the guest code:
//
//   - "a1+20 drives a busy-wait". The guest's own test is
//     `if (*(float*)(a1+20) != 3.4028235e38 && dt < target)`, and a1+20 holds
//     0x7F7FFFFF — exactly that FLT_MAX sentinel — so the guest disables the
//     spin itself. The one-time log below re-checks it at runtime.
//   - "a1+32 is an unbounded store offset". It is `v9 = *(a1+32) + 9;
//     *(float*)(4*v9 + a1) = dt;` with `if (v10 >= 5) *(a1+32) = 0` — a bounded
//     5-entry ring at a1+36..a1+52, guarded by `if (*(a1+28))`.
//
// No cvar to restore the stub. It was wrong, not a trade-off; git has it.
// GAME SPEED IS THIS FUNCTION'S dt, and nothing else.
//
// The guest is already frame-rate independent by construction: it computes
// dt = (int32)(QPC_now - QPC_prev) / KeQueryPerformanceFrequency(), smooths it
// over a 5-entry ring, and clamps it to a maximum at a1+72. Physics and script
// then advance by that dt. So a game running at the WRONG SPEED at any frame
// rate is a dt that does not equal real elapsed time, and there are exactly two
// ways to get one:
//
//   the CLAMP at a1+72, which is a deliberate slow-motion floor. A frame longer
//     than the clamp advances the world by the clamp, so on a machine too slow
//     to hold it the whole game runs slow -- measured on the dev VM at 39.0s of
//     game time over 48s of wall clock, 81%. That is the guest's own design and
//     is not a defect.
//   a WRONG dt SCALE, if QPC and QPF disagree about their units. That is a
//     constant multiplier, independent of frame rate, and is what a steady "2x
//     speed" looks like.
//
// The two are told apart by the ratio below and by nothing else, so it is
// printed on every report: guest seconds elapsed against host seconds elapsed
// over the same span. 1.00 is correct. Below 1.00 with dt pinned at the clamp
// is the first case. Anything else, at any frame rate, is the second.
REXCVAR_DEFINE_BOOL(guest_dt_from_host, false, "Debug",
                    "Replace the guest's computed frame dt with true host "
                    "elapsed time, keeping its own maximum-frame-time clamp. "
                    "Forces game speed to real time at any frame rate");

REX_IMPORT(__imp__sub_82B70370, orig_Timing, void());
extern "C" REX_FUNC(sub_82B70370) {
  static int tm = 0;
  ++tm;
  const uint32_t a1 = ctx.r3.u32;
  // The guards are what make this call safe, so record them once rather than
  // trusting the reading above to still hold.
  if (tm == 1 && a1) {
    REXLOG_INFO("{}: Timing guards +20=0x{:08X} (FLT_MAX={}) +28=0x{:08X} +32=0x{:08X}",
                mx::native::g_plugin_mode ? "plugin" : "native", REX_LOAD_U32(a1 + 20),
                REX_LOAD_U32(a1 + 20) == 0x7F7FFFFFu, REX_LOAD_U32(a1 + 28),
                REX_LOAD_U32(a1 + 32));
  }
  // WHAT IS AT vt[128]? The last unmeasured consumer of dt.
  //
  // The tick (sub_82B70DE8) runs, in order:
  //
  //     sub_82B70370(a1);                          // this hook -- dt
  //     v2 = (*(*engine + 8))(engine);             // a subsystem getter
  //     v3 = (*(*v2 + 128))(v2, dt);               // <-- NEVER COUNTED
  //     sub_82B6D230(&unk_82DD8088, dt);           // entity pass, 1.000/frame
  //     sub_82B34998(..., dt);                     // dispatch, 1.000 exec/frame
  //
  // Both counted consumers are clean and every clock reads 1.00, so this is the
  // only call left that could be stepping the world more than once. It is a
  // virtual call, so the target cannot be named statically -- resolved here
  // instead, once, from the live vtables.
  //
  // The getter is re-called through an isolated CallFrame the way the
  // AcquirePlayer repair does. It is safe to call twice: the tick itself calls
  // it every frame immediately after this hook returns, so any lazy init it
  // performs has either already happened or is about to.
  if (a1) {
    static std::atomic<bool> s_resolved{false};
    bool expected = false;
    if (s_resolved.compare_exchange_strong(expected, true)) {
      const uint32_t engine = GuestRangeReadable(base, 0x830BE400u, 4)
                                  ? REX_LOAD_U32(0x830BE400u) : 0;
      const uint32_t engine_vt =
          (engine && GuestRangeReadable(base, engine, 4)) ? REX_LOAD_U32(engine) : 0;
      const uint32_t getter = (engine_vt && GuestRangeReadable(base, engine_vt + 8u, 4))
                                  ? REX_LOAD_U32(engine_vt + 8u) : 0;
      uint32_t sub = 0, sub_vt = 0, target = 0;
      // Guest address -> recompiled function, through the table the crash
      // reporter already resolves RIP with. Linear because PPCFuncMappings is
      // sorted by guest address and this runs exactly once.
      PPCFunc* getter_fn = nullptr;
      if (getter) {
        for (const PPCFuncMapping* m = PPCFuncMappings; m->guest; ++m) {
          if (uint32_t(m->guest) == getter) { getter_fn = m->host; break; }
        }
      }
      if (getter_fn) {
        rex::CallFrame frame(ctx);
        frame.ctx.r1.u32 -= 0x70;
        frame.ctx.r3.u64 = engine;
        getter_fn(frame.ctx, base);
        sub = frame.ctx.r3.u32;
      }
      if (sub && GuestRangeReadable(base, sub, 4)) sub_vt = REX_LOAD_U32(sub);
      if (sub_vt && GuestRangeReadable(base, sub_vt + 128u, 4))
        target = REX_LOAD_U32(sub_vt + 128u);
      REXLOG_INFO("{}: TICK vt[128] RESOLVE engine=0x{:08X} vt=0x{:08X} "
                  "getter=0x{:08X} -> subsystem=0x{:08X} vt=0x{:08X} | "
                  "vt[128] = 0x{:08X}{}  <-- the unmeasured world update",
                  mx::native::g_plugin_mode ? "plugin" : "native", engine,
                  engine_vt, getter, sub, sub_vt, target,
                  getter_fn ? "" : " (getter NOT in PPCFuncMappings)");
    }
  }

  const auto host_now = std::chrono::steady_clock::now();
  orig_Timing(ctx, base);
  if (!a1) return;

  // Offsets are the guest's own, from the decompilation above:
  //   +24 dt   +60 total elapsed   +64 total * +68   +72 maximum frame time
  constexpr uint32_t kDt = 24, kTotal = 60, kScaled = 64, kScale = 68,
                     kMaxFrame = 72;
  static std::chrono::steady_clock::time_point s_hostPrev{};
  static double s_hostElapsed = 0.0;
  // The guest's OWN dt, accumulated separately and BEFORE any correction.
  //
  // The first cut of this reported total/host as GAME SPEED, and with
  // guest_dt_from_host=1 that is TAUTOLOGICAL: the lever forces dt to the host
  // delta, so the total tracks the host by construction and the ratio reads
  // 1.00 whatever the guest's clock was doing. A measurement that cannot fail
  // while the fix is engaged is not a measurement -- the same trap as a
  // throttle predicate that is always true. This accumulator is the guest's
  // unmodified answer and is honest with the lever either way.
  static double s_guestElapsed = 0.0;
  static uint32_t s_lastA1 = 0;
  static uint64_t s_a1Changes = 0;
  const float guest_dt = std::bit_cast<float>(REX_LOAD_U32(a1 + kDt));
  double host_dt = 0.0;
  if (s_hostPrev.time_since_epoch().count() != 0)
    host_dt = std::chrono::duration<double>(host_now - s_hostPrev).count();
  s_hostPrev = host_now;
  s_hostElapsed += host_dt;
  s_guestElapsed += guest_dt;
  // One timing object, or several? The host delta is a single static, so if
  // this function serves more than one timer the deltas interleave and both
  // the measurement and the lever are wrong. Counted rather than assumed.
  if (s_lastA1 && a1 != s_lastA1) ++s_a1Changes;
  s_lastA1 = a1;

  // THE LEVER. Rewriting dt alone would leave the two derived fields the guest
  // just computed from the old value inconsistent, so all three move together:
  // +60 is a running total the guest advanced by the old dt, and +64 is +68
  // times that total. The guest's OWN clamp is re-applied rather than dropped
  // -- it exists so a hitch cannot teleport the world, and that is as true of
  // a host-measured hitch as of a guest-measured one.
  if (REXCVAR_GET(guest_dt_from_host) && host_dt > 0.0) {
    const float max_frame = std::bit_cast<float>(REX_LOAD_U32(a1 + kMaxFrame));
    float dt = float(host_dt);
    if (max_frame > 0.0f && dt > max_frame) dt = max_frame;
    const float total =
        std::bit_cast<float>(REX_LOAD_U32(a1 + kTotal)) - guest_dt + dt;
    const float scale = std::bit_cast<float>(REX_LOAD_U32(a1 + kScale));
    REX_STORE_U32(a1 + kDt, std::bit_cast<uint32_t>(dt));
    REX_STORE_U32(a1 + kTotal, std::bit_cast<uint32_t>(total));
    REX_STORE_U32(a1 + kScaled, std::bit_cast<uint32_t>(scale * total));
  }

  // Taken every frame and reported on the same cadence as the rest: these are
  // per-frame counts, so a sampled read of a live counter would be meaningless.
  const uint32_t ent = g_updEntity.exchange(0, std::memory_order_relaxed);
  const uint32_t ren = g_updRenderer.exchange(0, std::memory_order_relaxed);
  const double ent_dt =
      std::bit_cast<double>(g_updEntityDtBits.load(std::memory_order_relaxed));
  const double ren_dt = std::bit_cast<double>(
      g_updRendererDtBits.load(std::memory_order_relaxed));
  const uint32_t exec = g_updRendererExec.exchange(0, std::memory_order_relaxed);
  static uint64_t s_execTotal = 0;
  s_execTotal += exec;
  const uint32_t fixed60 = g_updFixed60.exchange(0, std::memory_order_relaxed);
  {
    // Close the frame's step census here: the Timing hook is the tick's first
    // call, so everything since the previous one is exactly one frame's steps.
    std::lock_guard<std::mutex> lk(g_stepMu);
    if (g_stepCalls) {
      ++g_stepFrames;
      g_stepCallsTotal += g_stepCalls;
      g_stepDistinctTotal += g_stepSeen.size();
    }
    g_stepCalls = 0;
    g_stepSeen.clear();
  }
  // The second clock, against real time. Sampled every frame so the rate is a
  // true average rather than two points that might straddle a stall.
  static double s_physFirst = -1.0, s_physLast = 0.0;
  double phys_clock = 0.0, phys_max = 0.0, phys_scale = 0.0;
  {
    const uint32_t engine = GuestRangeReadable(base, 0x830BE400u, 4)
                                ? REX_LOAD_U32(0x830BE400u) : 0;
    const uint32_t sub =
        (engine && GuestRangeReadable(base, engine + kEngineSubSlot, 4))
            ? REX_LOAD_U32(engine + kEngineSubSlot) : 0;
    if (sub && GuestRangeReadable(base, sub + kSubClock, 12)) {
      phys_clock = std::bit_cast<float>(REX_LOAD_U32(sub + kSubClock));
      phys_max = std::bit_cast<float>(REX_LOAD_U32(sub + kSubMaxDelta));
      if (s_physFirst < 0.0) s_physFirst = phys_clock;
      s_physLast = phys_clock;
    }
    const uint32_t w = g_physWorker.load(std::memory_order_relaxed);
    if (w && GuestRangeReadable(base, w + kWorkerScale, 4))
      phys_scale = std::bit_cast<float>(REX_LOAD_U32(w + kWorkerScale));
  }
  const uint64_t vbl = g_gpuIntVblank.load(std::memory_order_relaxed);
  const uint64_t swp = g_gpuIntSwap.load(std::memory_order_relaxed);
  const uint64_t oth = g_gpuIntOther.load(std::memory_order_relaxed);
  const uint64_t flips = g_vblankHandler.load(std::memory_order_relaxed);
  const uint32_t alt = g_updAltPath.exchange(0, std::memory_order_relaxed);
  static uint64_t s_fixed60Total = 0, s_altTotal = 0;
  s_fixed60Total += fixed60;
  s_altTotal += alt;
  static uint64_t s_entTotal = 0, s_renTotal = 0, s_frames = 0;
  s_entTotal += ent;
  s_renTotal += ren;
  ++s_frames;

  if (tm <= 5 || (tm % 250) == 0) {
    const float total = std::bit_cast<float>(REX_LOAD_U32(a1 + kTotal));
    const float max_frame = std::bit_cast<float>(REX_LOAD_U32(a1 + kMaxFrame));
    // THE TIMING STRUCT'S OWN SCALE, never read until now. The guest computes
    //     *(a1+64) = *(a1+68) * *(a1+60)
    // so +68 multiplies accumulated time into a second, scaled clock at +64 --
    // a time-dilation factor by construction, and exactly the "something sets
    // game speed" shape. Assumed to be 1.0 for this whole hunt and never
    // checked, which is how the last nine hypotheses went wrong.
    const float tscale = std::bit_cast<float>(REX_LOAD_U32(a1 + kScale));
    const float scaled = std::bit_cast<float>(REX_LOAD_U32(a1 + kScaled));
    // GAME SPEED, as one number. Guest seconds per host second over the whole
    // run: 1.00 is correct, and it is frame-rate independent by construction,
    // so it is comparable between machines and between runs.
    REXLOG_INFO("{}: Timing #{} dt={:.6f} host_dt={:.6f} | GUEST CLOCK {:.4f}x "
                "({:.3f}s guest / {:.3f}s host, unmodified) | applied total "
                "{:.3f} | a1=0x{:08X} ({} object changes){} (clamp {:.4f}){}",
                mx::native::g_plugin_mode ? "plugin" : "native", tm, guest_dt,
                host_dt,
                s_hostElapsed > 0.0 ? s_guestElapsed / s_hostElapsed : 0.0,
                s_guestElapsed, s_hostElapsed, total, a1, s_a1Changes,
                guest_dt >= max_frame && max_frame > 0.0f
                    ? " <-- dt PINNED AT THE CLAMP, the world is in slow motion"
                    : "",
                max_frame,
                REXCVAR_GET(guest_dt_from_host) ? " [dt forced from host]" : "");
    REXLOG_INFO("{}: TIME SCALE +68 = {:.4f}, scaled clock +64 = {:.3f} against "
                "total {:.3f} (ratio {:.4f}){}",
                mx::native::g_plugin_mode ? "plugin" : "native", tscale, scaled,
                total, total > 0.0f ? double(scaled) / double(total) : 0.0,
                (tscale > 1.01f || tscale < 0.99f)
                    ? "  <-- NOT 1.0, this is a time dilation factor"
                    : "");
    // THE COUNT THAT MATTERS. Anything other than 1.00 advances per frame is
    // the world being stepped more than once for one tick of the clock, and
    // that is a speed multiplier no frame cap can reach. The dt each consumer
    // actually received is printed beside it: equal to the timer's dt means
    // the step is duplicated, double it means the step is scaled.
    REXLOG_INFO("{}: WORLD STEPS over {} frames -- entity pass {} ({:.3f}/frame,"
                " last dt {:.6f}), RendererDispatch {} ({:.3f}/frame, last dt "
                "{:.6f}, of which {} EXECUTED = {:.3f}/frame, the rest "
                "enqueued); timer dt {:.6f}",
                mx::native::g_plugin_mode ? "plugin" : "native", s_frames,
                s_entTotal, double(s_entTotal) / double(s_frames), ent_dt,
                s_renTotal, double(s_renTotal) / double(s_frames), ren_dt,
                s_execTotal, double(s_execTotal) / double(s_frames), guest_dt);
    // Which of those entity passes carried the hardcoded 1/60 rather than the
    // measured dt, and how often the path that supplies it ran. A non-zero
    // pair here IS the speed multiplier: the world advances by
    // real_dt + 1/60 per frame, and no frame cap can reach the second term.
    // The rate we deliver the guest's frame heartbeat at, against the 60 Hz the
    // console delivered. A ratio of 2.00 here is a 2x game, and unlike every
    // other number in this file it can differ between machines.
    {
      const double adv = (s_physFirst >= 0.0) ? (s_physLast - s_physFirst) : 0.0;
      REXLOG_INFO("{}: PHYSICS CLOCK engine[16]+64 = {:.3f} (advanced {:.3f}s "
                  "over {:.3f}s host = {:.4f}x) | max delta {:.4f} | worker "
                  "0x{:08X} time scale {:.4f}{}",
                  mx::native::g_plugin_mode ? "plugin" : "native", phys_clock,
                  adv, s_hostElapsed,
                  s_hostElapsed > 0.0 ? adv / s_hostElapsed : 0.0, phys_max,
                  g_physWorker.load(std::memory_order_relaxed), phys_scale,
                  (s_hostElapsed > 1.0 && adv / s_hostElapsed > 1.5)
                      ? "  <-- THIS CLOCK RUNS FAST"
                      : (phys_scale > 1.5 ? "  <-- SCALE IS NOT 1.0" : ""));
    }
    REXLOG_INFO("{}: GPU INTERRUPTS over {:.1f}s -- vblank {} ({:.1f}/s = "
                "{:.2f}x the console's 60Hz), swap-done {}, other {}; flip pass "
                "ran {} times{}",
                mx::native::g_plugin_mode ? "plugin" : "native", s_hostElapsed,
                vbl, s_hostElapsed > 0.0 ? double(vbl) / s_hostElapsed : 0.0,
                s_hostElapsed > 0.0
                    ? double(vbl) / s_hostElapsed / kConsoleVblankHz
                    : 0.0,
                swp, oth, flips,
                vbl == 0
                    ? "  <-- NO VBLANKS DELIVERED AT ALL"
                    : "");
    REXLOG_INFO("{}: VBLANK PACING guest_vblank_hz={} -- {} host vblanks seen, "
                "{} dropped, {} delivered ({:.1f}/s to the guest)",
                mx::native::g_plugin_mode ? "plugin" : "native",
                REXCVAR_GET(guest_vblank_hz), vbl,
                g_gpuIntDropped.load(std::memory_order_relaxed),
                vbl - g_gpuIntDropped.load(std::memory_order_relaxed),
                s_hostElapsed > 0.0
                    ? double(vbl - g_gpuIntDropped.load(std::memory_order_relaxed)) /
                          s_hostElapsed
                    : 0.0);
    {
      // Waits per thread per second. A guest loop that paces itself blocks
      // every iteration and shows a rate near the frame rate; one that
      // free-runs shows none at all while still consuming the machine.
      REXLOG_INFO("{}: GUEST WAITS over {:.1f}s --{}",
                  mx::native::g_plugin_mode ? "plugin" : "native", s_hostElapsed,
                  GuestWaitReport(s_hostElapsed));
    }
    {
      std::lock_guard<std::mutex> lk(g_stepMu);
      const double per = g_stepFrames ? double(g_stepCallsTotal) / double(g_stepFrames) : 0.0;
      const double dist = g_stepFrames ? double(g_stepDistinctTotal) / double(g_stepFrames) : 0.0;
      {
        std::string vts;
        for (uint32_t i = 0; i < g_stepVtCount; ++i)
          vts += fmt::format(" [vt0x{:08X} -> vt24=0x{:08X} vt28=0x{:08X} x{}]",
                             g_stepVts[i].vt, g_stepVts[i].f24, g_stepVts[i].f28,
                             g_stepVts[i].hits);
        REXLOG_INFO("{}: ENTITY STEP TARGETS -- {} distinct entity vtable(s){}:{}",
                    mx::native::g_plugin_mode ? "plugin" : "native",
                    g_stepVtCount,
                    g_stepVtOverflow
                        ? fmt::format(" (+{} past the {}-slot cap)",
                                      g_stepVtOverflow, kMaxStepVts)
                        : std::string(),
                    vts.empty() ? " (none)" : vts);
      }
      REXLOG_INFO("{}: ENTITY STEPS over {} frames -- {} steps on {} distinct "
                  "entities ({:.1f} vs {:.1f} per frame) | {} steps hit an "
                  "entity ALREADY STEPPED this frame{}{}",
                  mx::native::g_plugin_mode ? "plugin" : "native", g_stepFrames,
                  g_stepCallsTotal, g_stepDistinctTotal, per, dist, g_stepDup,
                  g_stepDupExample
                      ? fmt::format(", e.g. 0x{:08X}", g_stepDupExample)
                      : std::string(),
                  (dist > 0.0 && per / dist > 1.5)
                      ? "  <-- THE WORLD IS BEING STEPPED TWICE"
                      : "");
    }
    REXLOG_INFO("{}: FIXED-60 STEPS over {} frames -- {} entity passes at the "
                "hardcoded 1/60 ({:.3f}/frame), sub_82AB58F8 ran {} times "
                "({:.3f}/frame). Implied world advance {:.3f}x real",
                mx::native::g_plugin_mode ? "plugin" : "native", s_frames,
                s_fixed60Total, double(s_fixed60Total) / double(s_frames),
                s_altTotal, double(s_altTotal) / double(s_frames),
                guest_dt > 0.0f
                    ? 1.0 + (double(s_fixed60Total) / double(s_frames)) *
                                kFixed60 / double(guest_dt)
                    : 0.0);
  }
}

// sub_82B6D230 — called from LoaderTick's entity block @0x82B70E4C with f1=dt.
// It iterates a vector — `n = (v[1] - v[0]) >> 2` elements — calling
// sub_82B6A448(elem, dt) on each, so it is an entity update pass, not the
// "frontier probe" it was labelled as.
//
// That label said execution "reaches TexManager @0x82B70E44 then dies" here.
// It does not, and has not for a long time: ENTER and RETURNED balance 5/5 in
// mx_473, mx_477 and mx_479. Corrected 2026-08-06.
REX_IMPORT(__imp__sub_82B6D230, orig_EntityDt, void());
extern "C" REX_FUNC(sub_82B6D230) {
  const double dt = ctx.f1.f64;
  g_updEntity.fetch_add(1, std::memory_order_relaxed);
  // Exactly the constant the guest compiles in, so the test is an equality on
  // the value the alternate path passes and not a guess at a range.
  if (std::abs(dt - kFixed60) < 1e-9)
    g_updFixed60.fetch_add(1, std::memory_order_relaxed);
  g_updEntityDtBits.store(std::bit_cast<uint64_t>(dt), std::memory_order_relaxed);
  orig_EntityDt(ctx, base);
}

// WHICH GUEST THREADS EXIST, AND HOW FAST DOES EACH ONE SPIN?
//
// Every counter in this file is per TICK FRAME, so a second guest thread
// running its own update loop is structurally invisible to all of them --
// which is exactly the gap left after the whole tick chain measured correct
// during riding (entity steps 1:1, dispatch 1.000 executed, GUEST CLOCK 1.00).
//
// Two halves, because "which threads exist" and "what are they doing" are
// different questions and the log's own thread ids only ever show the threads
// that happen to call something we already hook.
//
//   sub_82BFC370   the guest's thread create. Logs each new thread's ENTRY
//                  POINT, which is what makes it identifiable in IDA.
//   sub_82BFB740   NtWaitForSingleObjectEx, the guest's block -- counted in
//                  hooks_wait.cpp, which already hooks it. A loop that
//                  paces itself passes through here every iteration; one that
//                  free-runs does not, so waits-per-second separates a paced
//                  thread from an unpaced one without having to find its loop.
// sub_82B09FF8 — the interval-driven update worker. __noreturn, so this can
// only record its `this` on the way in; everything else is read from the
// Timing hook. See the note at kWorkerScale.
REX_IMPORT(__imp__sub_82B09FF8, orig_PhysWorker, void());
extern "C" REX_FUNC(sub_82B09FF8) {
  g_physWorker.store(ctx.r3.u32, std::memory_order_relaxed);
  REXLOG_INFO("{}: PHYSICS WORKER entered, this=0x{:08X}",
              mx::native::g_plugin_mode ? "plugin" : "native", ctx.r3.u32);
  orig_PhysWorker(ctx, base);
}

REX_IMPORT(__imp__sub_82BFC370, orig_GuestCreateThread, void());
extern "C" REX_FUNC(sub_82BFC370) {
  // r5 is the entry point and r6 the parameter at the two call sites read so
  // far (the AssetDB worker in sub_82BAB700). Captured before the original in
  // case it clobbers them.
  const uint32_t entry = ctx.r5.u32;
  const uint32_t param = ctx.r6.u32;
  orig_GuestCreateThread(ctx, base);
  static std::atomic<uint32_t> s_n{0};
  const uint32_t n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n <= 32)
    REXLOG_INFO("{}: GUEST THREAD #{} created, entry=0x{:08X} param=0x{:08X} "
                "-> r3=0x{:08X}",
                mx::native::g_plugin_mode ? "plugin" : "native", n, entry,
                param, ctx.r3.u32);
}

// sub_82B6A448(entity, dt) — one entity's step, from the vector sub_82B6D230
// walks. See the note at g_stepSeen.
//
// It dispatches each of four slots to vt[24] or vt[28] with (this, slot, dt):
//
//     if (*v5) { if (v2[2]) vt[28](v2, slot, dt); else vt[24](v2, slot, dt); }
//
// THOSE are where a fixed-timestep accumulator would live -- the
// `while (accum >= STEP) { step(STEP); accum -= STEP; }` every physics engine
// is written around. A wrong STEP or a doubly-fed accumulator there produces
// exactly the evidence this hunt has: the outer call stays at 1.000/frame with
// the correct dt, every clock reads 1.00, and the world still moves twice as
// far. Nothing counted so far can see inside that call.
//
// Resolved rather than guessed, because they are virtual. Distinct vtables are
// collected with their two targets so the population is visible instead of one
// lucky sample.
REX_IMPORT(__imp__sub_82B6A448, orig_EntityStep, void());
extern "C" REX_FUNC(sub_82B6A448) {
  const uint32_t self = ctx.r3.u32;
  {
    std::lock_guard<std::mutex> lk(g_stepMu);
    ++g_stepCalls;
    // The entity's vtable and its two step targets. Read under the lock that
    // already guards this hook rather than adding a second one.
    if (self && GuestRangeReadable(base, self, 4)) {
      const uint32_t vt = REX_LOAD_U32(self);
      if (vt && GuestRangeReadable(base, vt + 24u, 8)) {
        uint32_t i = 0;
        for (; i < g_stepVtCount; ++i)
          if (g_stepVts[i].vt == vt) break;
        if (i == g_stepVtCount) {
          if (g_stepVtCount < kMaxStepVts) {
            g_stepVts[g_stepVtCount++] = {vt, REX_LOAD_U32(vt + 24u),
                                          REX_LOAD_U32(vt + 28u), 1};
          } else {
            ++g_stepVtOverflow;
          }
        } else {
          ++g_stepVts[i].hits;
        }
      }
    }
    // Linear: the list is small and this replaces a per-entity hash that would
    // cost more than it measures. If it ever is not small the counts still hold.
    bool seen = false;
    for (uint32_t e : g_stepSeen)
      if (e == self) { seen = true; break; }
    if (seen) {
      ++g_stepDup;
      if (!g_stepDupExample) g_stepDupExample = self;
    } else if (g_stepSeen.size() < 4096) {
      g_stepSeen.push_back(self);
    }
  }
  orig_EntityStep(ctx, base);
}

// sub_825582E0(source, device) — D3D9's graphics interrupt callback, registered
// through VdSetGraphicsInterruptCallback in sub_82569A50. source 0 is vblank,
// 1 is swap-complete. See the note at kConsoleVblankHz.
REX_IMPORT(__imp__sub_825582E0, orig_GpuInterrupt, void());
extern "C" REX_FUNC(sub_825582E0) {
  const uint32_t source = ctx.r3.u32;
  if (source == 1) {
    g_gpuIntSwap.fetch_add(1, std::memory_order_relaxed);
    orig_GpuInterrupt(ctx, base);
    return;
  }
  if (source != 0) {
    g_gpuIntOther.fetch_add(1, std::memory_order_relaxed);
    orig_GpuInterrupt(ctx, base);
    return;
  }
  // Vblank. Counted BEFORE the pacing decision, so the line always reports the
  // rate the host is actually producing rather than the rate we let through --
  // otherwise the measurement would be tautological once the lever is on, which
  // is a mistake already made once in this file with GAME SPEED.
  g_gpuIntVblank.fetch_add(1, std::memory_order_relaxed);
  const int hz = REXCVAR_GET(guest_vblank_hz);
  if (hz > 0) {
    static std::mutex s_mu;
    static std::chrono::steady_clock::time_point s_next{};
    const auto period = std::chrono::nanoseconds(1000000000ll / hz);
    const auto now = std::chrono::steady_clock::now();
    bool deliver;
    {
      std::lock_guard<std::mutex> lk(s_mu);
      if (s_next.time_since_epoch().count() == 0 || now >= s_next) {
        // Re-base rather than accumulate: a long stall must not be repaid as a
        // burst of edges the console could never have produced.
        s_next = (s_next.time_since_epoch().count() == 0 || now - s_next > period)
                     ? now + period
                     : s_next + period;
        deliver = true;
      } else {
        deliver = false;
      }
    }
    if (!deliver) {
      g_gpuIntDropped.fetch_add(1, std::memory_order_relaxed);
      return;
    }
  }
  orig_GpuInterrupt(ctx, base);
}

// sub_825663B0 — the vblank flip pass: bumps the vblank counter and retires any
// queued swap whose target vblank has arrived.
REX_IMPORT(__imp__sub_825663B0, orig_VblankFlip, void());
extern "C" REX_FUNC(sub_825663B0) {
  g_vblankHandler.fetch_add(1, std::memory_order_relaxed);
  orig_VblankFlip(ctx, base);
}

// sub_82B33EC0 — the first thing RendererDispatch does once it is past its
// queue-or-execute gate. One code xref, so this counts executions exactly.
REX_IMPORT(__imp__sub_82B33EC0, orig_RendererExec, void());
extern "C" REX_FUNC(sub_82B33EC0) {
  g_updRendererExec.fetch_add(1, std::memory_order_relaxed);
  orig_RendererExec(ctx, base);
}

// sub_82AB58F8 — the fixed-1/60 world advance. See the note at kFixed60.
//
// Counted, not altered. Whether the right fix is to stop it running, to pass
// it the real dt, or to leave it alone because some screen legitimately wants
// a fixed step, depends on WHEN it runs -- and that is what this measures.
REX_IMPORT(__imp__sub_82AB58F8, orig_FixedStepAdvance, void());
extern "C" REX_FUNC(sub_82AB58F8) {
  g_updAltPath.fetch_add(1, std::memory_order_relaxed);
  orig_FixedStepAdvance(ctx, base);
}

// sub_8253AA40 — AssetDB_LoadStateMachine (LoaderTick's gate, 12-state)
REX_IMPORT(__imp__sub_8253AA40, orig_LoadStateMachine, void());
// Logs in BOTH modes. The note that used to sit here — "in native this is
// currently unreachable (mid-ASM hooks #7/#8 delete LoaderTick's vt[6] call
// site), so its absence from the log is itself the signal" — is STALE. Every
// mid-ASM hook in mx_config.toml is commented out, so LoaderTick's vt[6] gate
// runs and this fires continuously: 1600 calls in a two-minute native run
// (mx_1196, 2026-08-16). Its absence would now mean something is wrong, not
// something is skipped.
//
// What that run showed: the machine goes 0 -> 1 -> 2 and then stays at 2 for
// every subsequent tick. State 2 is idle-awaiting-a-request — sub_82534980 is
// what moves it 2 -> 3, and in a front-end-only run nothing calls it. The
// AssetDB is healthy and unasked, which is why the UI's own world (UI_World)
// never loads and the main menu has no stadium behind it.
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

  // State 6 parks. Exactly one predicate is responsible: loc_8253B504 reads
  // *(a1+110328) and, finding it zero, goes to loc_8253B560, which reads the
  // listener at *(a1+110788) and calls its vt[2]. A zero return jumps to
  // loc_8253B6A4 — an early return that leaves the selector at 6. The per-player
  // loop and the `state = 7` write at loc_8253B694 are both downstream of that
  // gate, so they are not what blocks us.
  //
  // The listener is the same object sub_82534980 notifies via vt[0]
  // (mx_recomp.31.cpp:22395), and it is assigned once in the AssetDB constructor
  // sub_8253CB38 (:41688) — so it is never null, and the vt[2] branch is always
  // the one taken. Dump it once so vt[2] can be resolved statically.
  if (a1 && state_out == 6) {
    static bool s_dumped = false;
    if (!s_dumped) {
      s_dumped = true;
      uint32_t obj = REX_LOAD_U32(a1 + 110788);
      uint32_t sib = REX_LOAD_U32(a1 + 110792);
      uint32_t vt = obj ? REX_LOAD_U32(obj) : 0;
      REXLOG_INFO("{}: state6 gate — listener(+110788)=0x{:08X} sibling(+110792)=0x{:08X} "
                  "vt=0x{:08X} +110328=0x{:08X}",
                  tag, obj, sib, vt, REX_LOAD_U32(a1 + 110328));
      if (vt) {
        // A real guest function pointer lives in 0x82xxxxxx. Anything else in a
        // vtable slot is data — assetdb vt[36] reads 0x53505F45 ("SP_E") — so
        // print the slots raw and judge them by range, never call them blind.
        REXLOG_INFO("{}: state6 gate — vt[0]=0x{:08X} vt[1]=0x{:08X} vt[2]=0x{:08X} vt[3]=0x{:08X}",
                    tag, REX_LOAD_U32(vt), REX_LOAD_U32(vt + 4),
                    REX_LOAD_U32(vt + 8), REX_LOAD_U32(vt + 12));
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

//=============================================================================
// The registry chokepoint
//
// Registry reads funnel through a family of getters that all hash the key with
// sub_82473360 and differ only in the value type. Two matter here, both reading
// the same registry global *(0x82D6605C):
//
//   sub_825487C8(registry, key, out, size, 0)   string, non-zero if found
//   sub_82548758(registry, key, out, 0)         int, written through `out`
//
// Their callers:
//
//   sub_82352AE0 (mx_recomp.15.cpp:76622)  key 0x8200C864 = "Location",
//     256-byte buffer -> the name it hands to sub_82534980. This is the load
//     path we have never seen run.
//   sub_82536250 (mx_recomp.31.cpp:26068)  key 0x8200C870 = "PlayerMode",
//     30-byte buffer -> mapped by sub_82533D20 to an index in the 5-entry
//     string table at 0x82D1F810 (5 if no entry matches; sub_82536250 returns 1
//     if the registry read itself fails).
//
// Measured, not inferred — an earlier reading of this file guessed the keys were
// "UISceneName"/"startMode" from .rdata spacing and the MXRegistry.bxml.xml
// contents. The runtime dump below says otherwise, and the table is a network
// mode vocabulary rather than a boot mode one:
//
//   0 SplitScreen   1 SinglePlayer   2 Online   3 LAN   4 None
//
// The state 6 gate wants 2 or 3, and the registry says "None". So its first term
// is asking "are we in a network session", which offline we are legitimately not
// — the term worth moving is the third one, the int at key 0x8204C630.
//=============================================================================

namespace {

// Guest addresses established statically above. Dumped once so the inference
// that named them is checked against the running image.
constexpr uint32_t kModeTable = 0x82D1F810;  // 5 char* — the PlayerMode vocabulary
constexpr uint32_t kSceneKey = 0x8200C864;   // "Location"
constexpr uint32_t kModeKey = 0x8200C870;    // "PlayerMode"
constexpr uint32_t kGateKey = 0x8204C630;    // the state 6 gate's third term

// Parsed form of the registry_override cvar: key -> replacement value.
const std::vector<std::pair<std::string, std::string>>& RegistryOverrides() {
  static const std::vector<std::pair<std::string, std::string>> parsed = [] {
    std::vector<std::pair<std::string, std::string>> out;
    const std::string& spec = REXCVAR_GET(registry_override);
    for (size_t pos = 0; pos < spec.size();) {
      size_t comma = spec.find(',', pos);
      if (comma == std::string::npos) comma = spec.size();
      std::string item = spec.substr(pos, comma - pos);
      pos = comma + 1;
      size_t eq = item.find('=');
      if (eq == std::string::npos || eq == 0) continue;
      out.emplace_back(item.substr(0, eq), item.substr(eq + 1));
    }
    return out;
  }();
  return parsed;
}

}  // namespace

// sub_825487C8 — the registry string getter.
REX_IMPORT(__imp__sub_825487C8, orig_RegistryGetString, void());
extern "C" REX_FUNC(sub_825487C8) {
  const char* tag = mx::native::g_plugin_mode ? "plugin" : "native";

  // The original clobbers r4/r5 (it moves the key into r3 to hash it), so the
  // arguments have to be captured before it runs. `lr` goes with them: the call
  // overwrites it, and the log line below runs after.
  const uint32_t key_addr = ctx.r4.u32;
  const uint32_t out = ctx.r5.u32;
  const uint32_t size = ctx.r6.u32;
  const uint32_t from = static_cast<uint32_t>(ctx.lr);
  const std::string key = GuestString(base, key_addr, 64);

  // One-shot: prove the two key addresses and the mode vocabulary against the
  // running image instead of leaving them inferred from .rdata spacing.
  static bool s_dumped = false;
  if (!s_dumped) {
    s_dumped = true;
    REXLOG_INFO("{}: registry — key(0x{:08X})=\"{}\" key(0x{:08X})=\"{}\" key(0x{:08X})=\"{}\"",
                tag, kSceneKey, GuestString(base, kSceneKey, 64), kModeKey,
                GuestString(base, kModeKey, 64), kGateKey,
                GuestString(base, kGateKey, 64));
    for (uint32_t i = 0; i < 5; ++i) {
      uint32_t p = REX_LOAD_U32(kModeTable + i * 4);
      REXLOG_INFO("{}: registry — mode[{}] ptr=0x{:08X} \"{}\"", tag, i, p,
                  GuestString(base, p, 64));
    }
  }

  orig_RegistryGetString(ctx, base);

  // First sighting per distinct key, capped so a per-frame reader cannot flood
  // the log. `out` is only meaningful when the lookup succeeded.
  //
  // `lr` is the point of this line as much as the value is. Native reads exactly
  // one string key in a whole run and the plugin reads four, so the callers of
  // the three extra ones are the boundary native stops short of — and naming
  // them off the return address beats inferring them from which function happens
  // to mention the key in .rdata.
  static std::vector<std::string> s_seen;
  if (s_seen.size() < 40 && std::find(s_seen.begin(), s_seen.end(), key) == s_seen.end()) {
    s_seen.push_back(key);
    REXLOG_INFO("{}: registry get \"{}\" size={} -> r3={} value=\"{}\" from lr=0x{:08X}",
                tag, key, size, ctx.r3.u32,
                ctx.r3.u32 ? GuestString(base, out, size) : std::string(), from);
  }

  for (const auto& [k, v] : RegistryOverrides()) {
    if (k != key) continue;
    if (!out || size < 2) break;
    const size_t n = std::min<size_t>(v.size(), size - 1);
    for (size_t i = 0; i < n; ++i)
      REX_STORE_U8(out + static_cast<uint32_t>(i), static_cast<uint8_t>(v[i]));
    REX_STORE_U8(out + static_cast<uint32_t>(n), 0);
    // Report found even if the key was absent, so an override can introduce a
    // setting the shipped registry does not carry.
    ctx.r3.u32 = 1;
    static std::vector<std::string> s_logged;
    if (std::find(s_logged.begin(), s_logged.end(), key) == s_logged.end()) {
      s_logged.push_back(key);
      REXLOG_INFO("{}: registry OVERRIDE \"{}\" -> \"{}\" (size={})", tag, key, v, size);
    }
    break;
  }
}

// sub_82548758 — the registry int getter, (registry, key, out, 0). It writes the
// value through `out` rather than returning it; the gate's third term reads it
// this way, and the gate's passing branch writes 1 back through sub_82548EA8,
// which makes the setting sticky once anything satisfies it.
REX_IMPORT(__imp__sub_82548758, orig_RegistryGetInt, void());
extern "C" REX_FUNC(sub_82548758) {
  const char* tag = mx::native::g_plugin_mode ? "plugin" : "native";
  const uint32_t key_addr = ctx.r4.u32;
  const uint32_t out = ctx.r5.u32;
  const uint32_t from = static_cast<uint32_t>(ctx.lr);
  const std::string key = GuestString(base, key_addr, 64);

  orig_RegistryGetInt(ctx, base);

  for (const auto& [k, v] : RegistryOverrides()) {
    if (k != key || !out) continue;
    REX_STORE_U32(out, static_cast<uint32_t>(std::strtol(v.c_str(), nullptr, 0)));
    static std::vector<std::string> s_logged;
    if (std::find(s_logged.begin(), s_logged.end(), key) == s_logged.end()) {
      s_logged.push_back(key);
      REXLOG_INFO("{}: registry OVERRIDE int \"{}\" -> {}", tag, key, v);
    }
    break;
  }

  static std::vector<std::string> s_seen;
  if (s_seen.size() < 40 && std::find(s_seen.begin(), s_seen.end(), key) == s_seen.end()) {
    s_seen.push_back(key);
    REXLOG_INFO("{}: registry getint \"{}\" -> {} from lr=0x{:08X}", tag, key,
                out ? REX_LOAD_U32(out) : 0xFFFFFFFFu, from);
  }
}

//=============================================================================
// The state 6 gate
//
// State 6 polls (*(AssetDB+110788))->vt[2] and takes an early return whenever it
// answers 0, which is why the selector never reaches 7. That slot resolves to
// sub_8253CF80 (dumped at runtime, mx_027.log), and its body is:
//
//   mode = sub_82536250(*(0x830577C0));   // maps a registry string to an enum
//   if (mode == 2 || mode == 3) return 1;
//   if (*(0x83057900) != 0)     return 1;
//   tmp = 0; sub_82548758(registry, <key>, &tmp, 0); return tmp;
//
// (`lis r11,-31995` is 0x83050000 — so the first global is the familiar AssetDB
// pointer dword_830577C0, confirmed at runtime by GateMode logging a1=0x407F2190.)
//
// Note state 1 clears *(0x83057900) on the way past (`stw r25,30976(r8)` with
// r25 = 0), so boot itself closes the second escape. These hooks report which
// term is actually deciding.
//=============================================================================

// sub_82536250 — registry-string -> mode enum, the gate's first term.
REX_IMPORT(__imp__sub_82536250, orig_GateMode, void());
extern "C" REX_FUNC(sub_82536250) {
  uint32_t a1 = ctx.r3.u32;
  orig_GateMode(ctx, base);
  static int n = 0;
  if (++n <= 3 || (n % 500) == 0)
    REXLOG_INFO("{}: GateMode(sub_82536250) #{} a1=0x{:08X} -> {}",
                mx::native::g_plugin_mode ? "plugin" : "native", n, a1, ctx.r3.u32);
}

// sub_8253CF80 — the gate itself, (*(AssetDB+110788))->vt[2].
REX_IMPORT(__imp__sub_8253CF80, orig_Gate, void());
extern "C" REX_FUNC(sub_8253CF80) {
  orig_Gate(ctx, base);
  static int n = 0;
  static uint32_t s_last = 0xFFFFFFFF;
  uint32_t ret = ctx.r3.u32;
  if (++n <= 3 || ret != s_last || (n % 500) == 0) {
    REXLOG_INFO("{}: state6 gate(sub_8253CF80) #{} -> {}  assetdb(0x830577C0)=0x{:08X} "
                "flag(0x83057900)=0x{:08X}",
                mx::native::g_plugin_mode ? "plugin" : "native", n, ret,
                REX_LOAD_U32(0x830577C0), REX_LOAD_U32(0x83057900));
    s_last = ret;
  }
}

//=============================================================================
// The front end is script-driven, not C++ virtual dispatch (2026-08-05)
//
// Tracing the load-request chain upward through the recompiled sources runs out
// of static call sites: four of the five functions above `sub_82352AE0` have
// zero direct callers. They are not virtual methods either. They are entries in
// a **name -> function binding table** at `0x8203F2E0`, 228 pairs of
// `const char*` and code pointer, registered from `MXRavage_Xenon_00cb`
// (0x824F1E1C). The vocabulary is a scripting API:
//
//   [  6] LoadAssetDB          [ 10] ExecuteScriptAsset
//   [ 40] GetUIState           [ 50] LoadUIAssetPackage
//   [ 53] LoadUIAssetDatabasePackage
//   [ 66] StartWorldLoad       [ 67] EnableWorld
//   [110] SwitchToUIWorld
//
// `StartWorldLoad` is `sub_824CD280` — the function that reaches
// `sub_82534980`, the load-request API. So nothing requests a scene because
// **no script ever calls StartWorldLoad**, and the question is whether the
// script environment runs at all.
//
// These four probes answer that. They deliberately sit at different depths:
// ExecuteScriptAsset is "does any script run", the two UI loaders are "does the
// front end's content arrive", and StartWorldLoad is the endpoint we already
// know is silent. Whichever is the last to fire names the break.
//
// Note the standing suspicion this connects to: AGENTS.md lists the binary
// `.xenon.package` heaps as encrypted (entropy ~7.98, routine unknown). If the
// UI scripts live in those heaps, the encryption is not a side issue — it is
// the reason there is no menu.
//=============================================================================

// The script call's Nth argument, when it is a Lua string.
//
// Every script probe logged only `a1`, which is the lua_State -- the SAME
// pointer for every call in the run, so `LoadUIAssetPackage #1..#4` could not
// say WHICH packages were asked for. That mattered the moment the garage's bink
// asset came back null: the movie exists in UIAnimations.xenon.package, and
// whether that package is ever requested is the whole question.
//
// The layout is not guessed. `sub_82AA7638` (luaD_precall) is already decoded
// at the bottom of this file and reads a StkId as `tt` at +8 and the GC pointer
// at +0, so TValue is {Value(8), int tt} with the 4-byte pointer at the front
// of the union -- big-endian, so offset 0 -- and a 16-byte stride. `L->base` is
// lua_State+12 (see the lua-state-layout note). LUA_TSTRING is 4, and a Lua 5.1
// TString on this target is next(4) tt(1) marked(1) reserved(1) pad(1) hash(4)
// len(4) = 16 bytes of header with the characters immediately after.
//
// Every read is range-checked against the real mapping. A script argument is
// guest data of whatever type the script happened to pass, and this file has
// already paid once for treating a plausible-looking value as a pointer: a
// range test admitted the ASCII "Litl" and the process died dereferencing it.
std::string ScriptArgString(uint8_t* base, uint32_t L, uint32_t index) {
  constexpr uint32_t kTValueStride = 16;
  constexpr uint32_t kTValueType = 8;
  constexpr uint32_t kLuaTString = 4;
  constexpr uint32_t kTStringLen = 12;
  constexpr uint32_t kTStringChars = 16;
  if (!L || !GuestRangeReadable(base, L + 12, 4)) return {};
  const uint32_t stack = REX_LOAD_U32(L + 12);
  const uint32_t slot = stack + index * kTValueStride;
  if (!stack || !GuestRangeReadable(base, slot, kTValueStride)) return {};
  if (REX_LOAD_U32(slot + kTValueType) != kLuaTString) return {};
  const uint32_t ts = REX_LOAD_U32(slot);
  if (!ts || !GuestRangeReadable(base, ts, kTStringChars)) return {};
  uint32_t len = REX_LOAD_U32(ts + kTStringLen);
  // A length is data too. Cap it before it is used as a size.
  if (len > 256u) len = 256u;
  if (!len || !GuestRangeReadable(base, ts + kTStringChars, len)) return {};
  std::string out;
  out.reserve(len);
  for (uint32_t i = 0; i < len; ++i) {
    const char c = char(REX_LOAD_U8(ts + kTStringChars + i));
    if (!c) break;
    out.push_back(c);
  }
  return out;
}

// Shared reporting: first few calls in full, then time-limited. A fixed budget
// is what hid the UV measurements — it spends itself on the boot phase and goes
// quiet exactly when the interesting work starts.
void ReportScriptProbe(const char* name, uint32_t a1, uint32_t lr,
                       uint64_t& count, const std::string& arg) {
  static std::chrono::steady_clock::time_point s_last[8]{};
  const size_t slot = size_t(reinterpret_cast<uintptr_t>(name)) % 8;
  const auto now = std::chrono::steady_clock::now();
  const bool due = (now - s_last[slot]) >= std::chrono::seconds(5);
  if (++count <= 4 || due) {
    if (due) s_last[slot] = now;
    REXLOG_INFO("{}: script {} #{} arg1=\"{}\" a1=0x{:08X} from lr=0x{:08X}",
                mx::native::g_plugin_mode ? "plugin" : "native", name, count,
                arg, a1, lr);
  }
  // EVERY DISTINCT ARGUMENT, uncapped and independent of the rate limit above.
  //
  // The throttle exists so a per-frame binding cannot flood the log, and it is
  // exactly wrong for this question: package loads happen in a burst during
  // boot, so a 5-second window collapses them to one line and the rest are lost.
  // A new name is new information and always prints; a repeat never does.
  if (!arg.empty()) {
    static std::mutex s_mu;
    static std::set<std::string> s_seen;
    std::lock_guard<std::mutex> lk(s_mu);
    if (s_seen.size() < 256 && s_seen.insert(std::string(name) + "|" + arg).second)
      REXLOG_INFO("{}: script {} FIRST SAW \"{}\"",
                  mx::native::g_plugin_mode ? "plugin" : "native", name, arg);
  }
}

#define MX_SCRIPT_PROBE(addr, orig, label)                       \
  REX_IMPORT(__imp__##addr, orig, void());                       \
  extern "C" REX_FUNC(addr) {                                    \
    static uint64_t s_count = 0;                                 \
    const uint32_t a1 = ctx.r3.u32;                              \
    const uint32_t lr = uint32_t(ctx.lr);                        \
    ReportScriptProbe(label, a1, lr, s_count,                    \
                      ScriptArgString(base, a1, 0));             \
    orig(ctx, base);                                             \
  }

MX_SCRIPT_PROBE(sub_824AF838, orig_ExecuteScriptAsset, "ExecuteScriptAsset")
MX_SCRIPT_PROBE(sub_824CBF90, orig_LoadUIAssetPackage, "LoadUIAssetPackage")
MX_SCRIPT_PROBE(sub_824CC218, orig_LoadUIAssetDbPackage,
                "LoadUIAssetDatabasePackage")
MX_SCRIPT_PROBE(sub_824D0F18, orig_SwitchToUIWorld, "SwitchToUIWorld")

//=============================================================================
// Engine.LoadAssetDB / Engine.LoadAssetPackage -- the OTHER load bindings.
//
// These are NOT LoadUIAssetPackage. UI_Helper (extracted out of
// MXUI.xenon.package) loads the front end with two different families:
//
//     function LoadFrontEndUIPackages( )
//        local isUILoaded = g_UIVariables:GetVariableInt( "UILoaded" );
//        if( isUILoaded == FALSE ) then
//           Engine.LoadAssetDB( "UIAnimations" );
//           Engine.LoadAssetPackage( "UIAnimations", "Rider" );
//           ...
//           Engine.LoadUIAssetPackage( "FrontEndShared" );
//           Engine.LoadUIAssetPackage( "FrontEnd" );
//
// Only the LoadUIAssetPackage family was probed, so a whole run looked like
// "UIAnimations is never requested" -- and it was concluded, wrongly, from an
// instrument that could not see the call. FrontEndShared and FrontEnd DO appear
// in the log and sit inside the same if-block, three lines below, which is the
// proof the block runs and the UIAnimations lines with it.
//
// UIAnimations holds exactly three assets (Base_Rider_Posed_A,
// SC_RU_HC_Anim_Gloves and RiderUI_Final_C_350), and RiderUI_Final_C_350 is the
// movie whose null asset is the 0x8234CE20 crash. So the question is no longer
// "is it asked for" but "what does asking return".
//
// Both args and the return value, because a load binding that fails quietly is
// exactly what this looks like. LoadAssetPackage takes (db, package).
//=============================================================================
REX_IMPORT(__imp__sub_824AF3C0, orig_LoadAssetDB, void());
extern "C" REX_FUNC(sub_824AF3C0) {
  const uint32_t L = ctx.r3.u32;
  const std::string db = ScriptArgString(base, L, 0);
  orig_LoadAssetDB(ctx, base);
  static uint64_t s_n = 0;
  REXLOG_INFO("{}: script LoadAssetDB #{} db=\"{}\" -> r3=0x{:08X}",
              mx::native::g_plugin_mode ? "plugin" : "native", ++s_n, db,
              ctx.r3.u32);
}

//=============================================================================
// sub_824F8E20 -- AssetDB_LoadPackage(dbName, packageName), the worker behind
// Engine.LoadAssetPackage. THIS is where the answer is.
//
//     if (!assetMgr->vt[36](assetMgr, db))                        // DB loaded?
//         assetMgr->vt[28](assetMgr, "Database\\", db, 1, 0,0,0);  // load it
//     return assetMgr->vt[72](assetMgr, db, package, 0,0,0);      // load pkg
//
// The return value IS vt[72]'s, so hooking this one function reports whether
// the package load succeeded without having to resolve a single vtable slot --
// which matters, because deriving them statically already failed once: the
// AssetDB ctor comment names "vtable off_8214518C", but off_8214518C+0x78 holds
// an address in the MIDDLE of sub_82BA8D08, so that premise is wrong.
//
// Everything upstream of here is confirmed good. UI_Helper asks for
// LoadAssetDB("UIAnimations") then LoadAssetPackage("UIAnimations", "Rider"),
// both bindings fire with exactly those arguments, and
// UIAnimations.xenon.database declares a package "Rider" holding
// RiderUI_Final_C_350 (bink, LZX) -- the movie whose null asset is the
// 0x8234CE20 crash. So either the DB open under "Database\" fails, or the
// package lookup inside it does.
//
// Note the two load families use DIFFERENT asset-manager methods: the working
// UI path (sub_824FB0F0 -> sub_823802D0) goes through vt[44], this one through
// vt[36]/vt[28]/vt[72]. "The UI packages load fine" says nothing about this
// path.
//=============================================================================
// 0x82BA91C0 -- AssetManager::Find(type, name), assetMgr->vt[0x78].
//
// Hooked for ONE reason: to capture the manager `this`. AcquirePlayer's
// re-resolve below needs it and cannot afford a plausible-but-wrong value.
//
// It IS derivable -- engine = *(0x830BE400), manager = *(engine + 8) -- but
// that walk has already been got wrong once in this file (dword_830BE400 read
// as an address instead of a pointer variable, printing zeros). Any call at all
// hands us the correct value, and there are thousands before the one that
// matters.
//
// The address had to come from the running game. Reading the IDB at
// off_8214518C+0x78 gave 0x82BA8D40, which is the middle of sub_82BA8D08; the
// live vtable holds 0x82BA91C0 there, and that same value appears as `ctr` in
// the crash register dump. Static reads of that table are not to be trusted.
REX_IMPORT(__imp__sub_82BA91C0, orig_AssetFind, void());
std::atomic<uint32_t> g_assetManager{0};

extern "C" REX_FUNC(sub_82BA91C0) {
  if (ctx.r3.u32) g_assetManager.store(ctx.r3.u32, std::memory_order_relaxed);
  orig_AssetFind(ctx, base);
}

//=============================================================================
// UIVideoComponent::AcquirePlayer - sub_8234CE20
//
// THE 0x8234CE20 CRASH, FIXED AT THE POINT THE STALE NULL IS USED.
//
// The guest caches its bink asset at component+0x94 in BinkVideoComponent's
// property init (sub_8234CBB8) and never re-resolves it. When the database
// worker constructs the component before the script has asked for the package
// that holds the movie, the lookup misses, a NULL is cached, and AcquirePlayer
// later writes through it at +0x58. Main PC, run mx_010, every link traced in
// one log: the worker resolved "RiderUI_Final_C_350" and missed at 20:03:21.682,
// the script asked for the UIAnimations database 0.227s LATER, and the access
// violation writing guest 0x58 with r3=0x21F9DB60 -- the same component --
// landed at 20:03:58.139.
//
// It is a RACE, and it has been "fixed" by winning it before: async shader
// compilation bought the script 0.9s on this VM and the crash went away here.
// It came straight back on a machine 4x faster. Any fix that works by being
// early enough is a fix with an expiry date, so this one does not.
//
// THE KEY FACT: the crash is 36 SECONDS after the null was cached, and the
// package loaded 0.2s after it. So at the moment of use the asset is present
// and simply is not looked at again. Re-resolving here asks the same question
// the guest asked, with the same manager, name and type, at a time when the
// answer exists.
//
// WHY NOT SKIP THE CALL. That was tried (2026-08-28) and made things worse:
// returning early left this->player null and the fault moved downstream to
// RVA 0x10CDCFD, correlated 3-for-3. Nothing is skipped here. On the path
// where the repair fails, the original runs exactly as it does today -- so
// this hook can restore behaviour, never degrade it.
//
// Reads and writes ONE dword of guest state, the one the guest wrote itself.
REX_IMPORT(__imp__sub_8234CE20, orig_AcquirePlayer, void());
extern "C" REX_FUNC(sub_8234CE20) {
  const uint32_t self = ctx.r3.u32;
  static std::atomic<uint64_t> s_repaired{0}, s_stillNull{0}, s_seenNull{0};
  // Offsets are sub_8234CBB8's, which writes the same two fields.
  if (self && GuestRangeReadable(base, self + 0x94u, 4) &&
      REX_LOAD_U32(self + 0x94u) == 0) {
    const uint64_t seen = s_seenNull.fetch_add(1, std::memory_order_relaxed) + 1;
    const uint32_t manager = g_assetManager.load(std::memory_order_relaxed);
    // The movie name is stored INLINE at +0x98, so it is its own name pointer;
    // no copy into guest memory is needed to make the call.
    const bool have_name =
        GuestRangeReadable(base, self + 0x98u, 2) && REX_LOAD_U8(self + 0x98u);
    uint32_t found = 0;
    if (manager && have_name) {
      // An isolated register context, so the outer call's state is untouched.
      // r1 is moved down by a frame the way the SDK's own auto-isolating path
      // does -- the callee will use it.
      rex::CallFrame frame(ctx);
      frame.ctx.r1.u32 -= 0x70;
      frame.ctx.r3.u64 = manager;
      frame.ctx.r4.u64 = self + 0x98u;
      // Type tag, big-endian in the HIGH half -- the layout the AssetFind
      // probe above already decodes ("bink" << 32).
      frame.ctx.r5.u64 = 0x62696E6Bull << 32;
      orig_AssetFind(frame, base);
      found = frame.ctx.r3.u32;
    }
    if (found) {
      REX_STORE_U32(self + 0x94u, found);
      const uint64_t n = s_repaired.fetch_add(1, std::memory_order_relaxed) + 1;
      REXLOG_INFO("{}: AcquirePlayer REPAIRED comp=0x{:08X} movie=\"{}\" -- "
                  "cached bink was NULL, re-resolved to 0x{:08X} ({} repaired, "
                  "{} nulls seen)",
                  mx::native::g_plugin_mode ? "plugin" : "native", self,
                  GuestString(base, self + 0x98u, 128), found, n, seen);
    } else {
      // SAID OUT LOUD. This is the branch that still faults, and a silent one
      // would look identical to a fix from the outside.
      const uint64_t n = s_stillNull.fetch_add(1, std::memory_order_relaxed) + 1;
      if (n <= 8)
        REXLOG_ERROR("{}: AcquirePlayer comp=0x{:08X} movie=\"{}\" -- cached "
                     "bink is NULL and re-resolve FAILED (manager 0x{:08X}, "
                     "name {}); calling the original anyway, expect the "
                     "0x8234CE20 fault ({} so far)",
                     mx::native::g_plugin_mode ? "plugin" : "native", self,
                     have_name ? GuestString(base, self + 0x98u, 128)
                               : std::string("<unreadable>"),
                     manager, have_name ? "ok" : "missing", n);
    }
  }
  orig_AcquirePlayer(ctx, base);
}

//=============================================================================
// D3DDevice_SetVertexShaderConstantF - sub_82550320
//
// MAKE THE CALLER NAME ITSELF, for the bike's world transform.
//
// The floating bike is 1.9 +/- 0.3 world units above the terrain, measured by
// unprojecting the wheel's lowest pixel and the ground through the real
// view-projection. The graphics path is exonerated: the terrain height reaching
// the renderer is correct, the props are correctly seated on that same terrain,
// and the bike's own transform reads out sane. What is left is a ~2-unit
// constant in the bike's RESTING HEIGHT, which is guest logic.
//
// IDA could not reach it. There are no named terrain/height/suspension
// functions in the IDB, the terrain manager's xrefs are all the virtual-texture
// streaming code, and a constant search for the 3000.0 height range landed in
// the save-game string pool. Hunting it by guessing at constants is what the
// rest of that session did.
//
// So do what worked for the UI render list, where static hunting produced 40+
// candidates and none of them right: make the caller name itself. The skinned
// vehicle shader indexes its bone palette as cb0[bone + 85/86/87], so the root
// bone lands in vertex constant registers 85..87, and whoever writes them is
// the vehicle's render path. From there the vehicle object and its position
// field are one structure walk away.
//
// NOT HOOKED BEFORE, DELIBERATELY, and that reasoning still stands for its own
// purpose -- hooks_d3d9.cpp reads device+0x780 rather than hooking this setter,
// because the device holds the live value whichever path wrote it, including a
// state-block path that bypasses every hook. That is about READING STATE. This
// wants the CALLER, which the device field cannot give at any price. If the
// bike's transform turns out to arrive by the state-block path, this probe sees
// nothing -- and a silent probe here means "not this path", not "no writes".
//
// Cheap on the hot path: one range compare before anything else happens.
namespace {

// ONE SERIES PER OBJECT, because the first cut of this trace did not have one.
//
// It printed a single interleaved stream and I read an oscillation out of it
// that was never there. Within 40 units of the camera there are SEVERAL skinned
// objects -- the bike, its rider, and whatever else the level parks nearby --
// written from four guest threads in an order that is not stable run to run.
// Consecutive samples were different OBJECTS, so the "1.84-unit swing" was the
// distance between two of them, not one thing moving. The amplitude agreed with
// the 1.9-unit float measured by unprojection, which is what made it credible
// and is a coincidence.
//
// TAGGED BY THE SOURCE BUFFER the caller supplied (r5), not by position.
// Position cannot be the tag: the bike and its rider sit about a unit apart,
// which is exactly where nearest-previous-position matching swaps them, and a
// tag that swaps is WORSE than no tag -- the swap reads as motion, which is the
// very thing being measured.
//
// AND THE TAG CHECKS ITSELF, which is the part that matters. If r5 is
// per-object storage it is an exact identity; if it is a scratch or ring buffer
// the engine reuses across objects, it is not. Those two cases are told apart
// by the STEP between consecutive samples of one series -- a real object moves
// a little per frame, a reused buffer teleports between unrelated objects -- so
// every series reports its mean and max step and its thread. The log says
// whether the tag held instead of leaving me to assume it did.
struct BikeTrack {
  uint32_t obj = 0;         // r27, the tag the hunt picked
  uint32_t src = 0;         // r5, the per-thread scratch arena it came through
  uint32_t lr = 0;
  uint32_t tid = 0;
  bool multi_lr = false;
  bool multi_tid = false;
  uint64_t samples = 0;
  uint64_t reported = 0;
  float x = 0.f, y = 0.f, z = 0.f;
  float y_min = 0.f, y_max = 0.f;
  float step_max = 0.f;
  double step_sum = 0.0;
};

// 48: the hunt says more than 8 objects pass through the 40-unit radius, and a
// table that drops the bike because it arrived ninth would fail silently.
constexpr uint32_t kMaxBikeTracks = 48;
std::mutex g_bikeMu;
BikeTrack g_bikeTracks[kMaxBikeTracks];
uint32_t g_bikeTrackCount = 0;
uint64_t g_bikeTrackOverflow = 0;
uint64_t g_bikeSamples = 0;

// THE TAG HUNT -- let the run pick the object identity instead of me guessing
// a third time.
//
// r5 was the second guess and the census killed it: three addresses 0x60000
// apart, one per thread, and positions that CYCLE within one of them (sample
// #209 repeats #205 exactly while the camera stands still). It tags the
// per-thread scratch arena, and six objects round-robin through it.
//
// But the immediate caller is sub_82596620, a 0x50-byte thunk that computes a
// dirty-register mask and tail-calls the setter. It touches r11 and r12 and
// NOTHING ELSE -- so every non-volatile register still holds the SUBMITTER's
// live values when we are entered, and a skinned draw almost certainly has its
// object in one of them.
//
// Which one is not worth guessing, so this scores all eighteen at once, with
// the same self-check the src tag failed: group positions by that register's
// value and measure the step between consecutive samples of a group. The
// object pointer is the register whose groups move CONTINUOUSLY -- a few
// distinct values, each tracing a smooth path. Everything else scores badly in
// one of two ways, and the report keeps them apart:
//
//   too few values, big steps   not an identity (a device, a flag, a constant
//                               shared by every object)
//   more values than the table  a counter, a scratch pointer, or a per-call
//                               temporary -- marked "+" and disqualified, and
//                               that mark is what stops a register that scores
//                               a perfect 0.000 step by giving every sample
//                               its own group from winning.
constexpr uint32_t kTagRegs = 18;  // r14..r31
// 64, NOT 8. The first hunt (run 1722) capped at 8 and every plausible
// candidate came back "8+", including the winner: r27 scored a step of 0.097
// against 2.5-8.1 for everything else. Eight was simply fewer than the number
// of skinned objects within 40 units, so the cap disqualified the answer.
//
// Note WHY r27 is not the degenerate case this table's "+" mark exists to
// reject. A register that is unique per call puts every sample in its own
// group, no group ever gets a second sample, and the score comes back "--"
// rather than a small number. 0.097 means those groups were revisited AND
// moved smoothly between visits, which is the definition of an object.
constexpr uint32_t kTagValues = 64;

struct TagSeries {
  uint32_t value = 0;
  float x = 0.f, y = 0.f, z = 0.f;
  uint64_t n = 0;
  double step_sum = 0.0;
};

struct TagCandidate {
  TagSeries s[kTagValues];
  uint32_t count = 0;
  uint64_t overflow = 0;
};

TagCandidate g_tagCands[kTagRegs];

void NoteTagCandidates(const uint32_t* nv, float x, float y, float z) {
  for (uint32_t i = 0; i < kTagRegs; ++i) {
    TagCandidate& c = g_tagCands[i];
    uint32_t k = 0;
    for (; k < c.count; ++k)
      if (c.s[k].value == nv[i]) break;
    if (k == c.count) {
      if (c.count >= kTagValues) {
        ++c.overflow;
        continue;
      }
      TagSeries& fresh = c.s[c.count++];
      fresh.value = nv[i];
      fresh.x = x;
      fresh.y = y;
      fresh.z = z;
      fresh.n = 1;
      continue;
    }
    TagSeries& s = c.s[k];
    const float dx = x - s.x, dy = y - s.y, dz = z - s.z;
    s.step_sum += std::sqrt(dx * dx + dy * dy + dz * dz);
    s.x = x;
    s.y = y;
    s.z = z;
    ++s.n;
  }
}

// A dense-numbered thread index rather than the OS id: the question is only
// "does one series come from one thread", and 0..3 answers it without dragging
// windows.h into this file. The log prefix still carries the real tid.
uint32_t BikeThreadIndex() {
  static std::atomic<uint32_t> s_next{0};
  static thread_local uint32_t s_idx =
      s_next.fetch_add(1, std::memory_order_relaxed);
  return s_idx;
}

// WHERE THE POSITION LIVES INSIDE THE OBJECT.
//
// r27 is a stable guest object pointer, so the transform we read out of the
// scratch palette must also exist somewhere in the object itself. Finding that
// offset is the step that turns a render-side observation into a handle on the
// PHYSICS: a field address can be write-watched, and whoever writes it is the
// code that decides the bike's height.
//
// Searched rather than assumed, because the layout is not knowable from here.
// Two candidate layouts are tested at every 4-byte offset:
//
//   packed    [o]=x [o+4]=y [o+8]=z        a plain vec3 or a matrix's 4th row
//   strided   [o]=x [o+16]=y [o+32]=z      translation in the .w of three rows,
//                                          which is how the palette carries it
//
// All three components must match, which is what keeps this from firing on any
// float that happens to equal y. The report is a census over offsets: a field
// recurs at ONE offset across objects and frames, and a coincidence does not.
struct FieldHit {
  uint32_t off = 0;      // kVia packs the outer offset low, the inner high
  uint8_t mode = 0;
  uint64_t hits = 0;
};
constexpr uint32_t kMaxFieldHits = 64;
FieldHit g_fieldHits[kMaxFieldHits];
uint32_t g_fieldHitCount = 0;
uint64_t g_fieldScans = 0;
uint64_t g_fieldMisses = 0;
uint64_t g_fieldOverflow = 0;

// 0x600 IN EITHER LAYOUT FOUND NOTHING -- 90 scans, 90 misses, run 1724. That
// is a real negative and it narrows the search rather than ending it. Three
// things produce it, and this pass separates them instead of picking one:
//
//   the span was too small        a vehicle object is not 1.5 KB; widened to
//                                 0x2000.
//   the layout was not guessed    both guesses required x FIRST. A component
//                                 order we did not think of (x, z, y), or a
//                                 matrix whose translation is not where we
//                                 assumed, defeats both. Mode 2 matches Y
//                                 ALONE and lets recurrence do the proving.
//   the object points AT it       a component or owner holding the transform
//                                 out of line. One level of pointer follow.
//
// WHY Y-ALONE IS NOT A LOOSE MATCH. In any single scan it will hit several
// offsets by chance. It cannot hit the SAME offset scan after scan while y
// changes, unless that offset holds y. So the census counts hits per offset
// against the scan total, and the field is the one whose rate approaches 1.0 --
// the coincidences scatter and stay at one or two.
enum FieldMode : uint8_t { kPacked = 0, kStrided = 1, kYOnly = 2, kVia = 3 };

void ScanObjectForPosition(uint8_t* base, uint32_t obj, float x, float y,
                           float z) {
  constexpr uint32_t kSpan = 0x2000;
  if (!GuestRangeReadable(base, obj, kSpan + 0x24u)) return;
  ++g_fieldScans;
  const auto at = [&](uint32_t a) {
    const uint32_t bits = REX_LOAD_U32(a);
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
  };
  // Tight, because the palette value and the object field should be the SAME
  // float, not merely close. A loose epsilon here would let the bike's six
  // parts match each other's fields and turn one offset into six.
  const auto same = [](float a, float b) { return std::fabs(a - b) < 1e-4f; };
  bool any = false;
  const auto note = [&](uint32_t off, FieldMode mode) {
    any = true;
    uint32_t i = 0;
    for (; i < g_fieldHitCount; ++i)
      if (g_fieldHits[i].off == off && g_fieldHits[i].mode == mode) break;
    if (i == g_fieldHitCount) {
      if (g_fieldHitCount >= kMaxFieldHits) {
        ++g_fieldOverflow;
        return;
      }
      g_fieldHits[g_fieldHitCount++] = {off, mode, 1};
    } else {
      ++g_fieldHits[i].hits;
    }
  };

  for (uint32_t o = 0; o <= kSpan; o += 4) {
    const float f = at(obj + o);
    if (same(f, x)) {
      if (same(at(obj + o + 4), y) && same(at(obj + o + 8), z))
        note(o, kPacked);
      if (same(at(obj + o + 16), y) && same(at(obj + o + 32), z))
        note(o, kStrided);
    }
    if (same(f, y)) note(o, kYOnly);
  }

  // ONE LEVEL OF POINTER FOLLOW, over the first 0x100 only. A component or
  // owner pointer sits near the head of an object; chasing every plausible
  // pointer in 8 KB would be thousands of scans per sample and would find a
  // match somewhere by sheer volume, which is the opposite of evidence.
  for (uint32_t o = 0; o <= 0x100u; o += 4) {
    const uint32_t p = REX_LOAD_U32(obj + o);
    if (p < 0x20000000u || p >= 0x40000000u) continue;  // the guest heap band
    if (!GuestRangeReadable(base, p, 0x200u + 0x24u)) continue;
    for (uint32_t q = 0; q <= 0x200u; q += 4)
      if (same(at(p + q), y)) note(o | (q << 16), kVia);
  }

  // A scan that matched NOTHING is the interesting negative: it means the
  // object does not hold this transform at all and the render path was handed
  // it from somewhere else. Counted so that case cannot look like "no scans".
  if (!any) ++g_fieldMisses;
}

// GROUND TRUTH FOR THE FLOATING BIKE -- the terrain clipmap constants.
//
// The bike is 1.9 +/- 0.3 units above the terrain, measured by unprojecting
// pixels. What has been missing is the TERRAIN's own height at the bike's XZ,
// from the same data the GPU draws: without it, "the bike is too high" and "the
// ground is too low" cannot be told apart.
//
// The mapping is not guessed. It is stated in the terrain mesh vertex shader
// (HFT_Helper, instruction 12), read out of the asset by tools/shader_code.py:
//
//     mad r0.xz, r1.yzzz, c204.xxxx, c201.xyyy
//     max/min r0.xz, -+c201.zwww
//
//   world.x = grid.x * gMeshResolution.x + gVertexOffset.x
//   world.z = grid.z * gMeshResolution.x + gVertexOffset.y   clamped +-c201.zw
//
// So grid = (world - gVertexOffset.xy) / gMeshResolution.x, exactly, with no
// inference on my part. That is the half of the problem that was blocking:
// [[floating-bike-is-two-units]] records considering this measurement and
// dropping it for want of the clipmap origin and scale.
//
// THIS STAGE CAPTURES AND REPORTS, IT DOES NOT YET SAMPLE. The height lookup
// itself depends on where the terrain height actually lives -- HFB_1tex's
// vertex shader has 2 vfetch and NO height sampler, so the block meshes carry
// height in their VERTEX DATA rather than in a texture, and which buffer that
// is cannot be known until these constants say where the bike sits in the grid.
// Writing the sampler before that is exactly the guess-first pattern that has
// cost this investigation three retractions.
//
// READ FROM THE PM4 ALU CONSTANT FILE, NOT THE DEVICE SHADOW. The first cut
// read the shadow the way the camera at c8 is read, and got 0.000 for all seven
// registers. That was not a bug in the read: a probe placed before the
// register-85 early-out counted the calls to D3DDevice_SetVertexShaderConstantF
// covering c204 and saw ZERO across a whole run, while the same segments logged
// 2256 bike samples -- so the terrain simply never sets these through that API,
// and the shadow could not have held them.
//
// They arrive by Type-0 PM4 instead, which is exactly the case
// `mx::gpu::alu` exists for. FileValues reports the LIVE file and marks
// components the stream never published, which matters here: it is explicitly
// the accessor that CAN report a zero, where WouldFillValues cannot.
std::string TerrainConstReport() {
  // Vertex bank: guest cN is index N. Two controls ride along on purpose --
  // gWaterModifiers (c220) from the same terrain shader set, and c252..c255,
  // which xenos_gpu_state documents as the screen-space scale/bias the D3D9
  // load table carries. If those come back looking like (0.5 -0.5 0 0) the
  // file is sane and a zero elsewhere is a real zero; if they are garbage,
  // NOTHING here should be acted on.
  // The DRAW-SCOPED snapshot, not the live file. The live file is global and
  // returns whatever shader wrote last -- c217 read back as a colour, which is
  // what made the census-time read untrustworthy in the first place.
  return mx::gpu::alu::TerrainValues();
}

// gMeshResolution.x and gVertexOffset.xy, or false when PM4 has not published
// them. Separated from the report so the grid maths cannot quietly run on
// zeros, and typed rather than parsed out of the report string -- that string
// marks unpublished components with `unpub:`, and a parser that ignored the
// marker would read an unwritten register as a published 0.
// The FINEST ring covering the bike, not the last packet's. A ring is chosen
// per position because the clipmap is a ladder and the coarsest ring is the one
// that happens to be written last.
bool TerrainGrid(float bx, float bz, float* mesh_res_x, float* off_x,
                 float* off_z) {
  return mx::gpu::alu::TerrainFinestRing(bx, bz, mesh_res_x, off_x, off_z);
}

void NoteBikeSample(uint8_t* base, uint32_t device, uint32_t src,
                    uint32_t lr, float x, float y,
                    float z, const float cam[4], float dist,
                    const uint32_t* nv) {
  const uint32_t tid = BikeThreadIndex();
  // THE TAG IS r27, chosen by the hunt rather than by me: step 0.097 against
  // 2.5-8.1 for every other non-volatile register. r5 stays in the row as
  // `src` because it identifies the per-thread scratch arena, which is worth
  // seeing next to the object -- but it is no longer what a series means.
  const uint32_t obj = nv[27 - 14];
  std::string line, census;
  {
    std::lock_guard<std::mutex> lk(g_bikeMu);
    uint32_t slot = 0;
    for (; slot < g_bikeTrackCount; ++slot)
      if (g_bikeTracks[slot].obj == obj) break;
    bool fresh = false;
    if (slot == g_bikeTrackCount) {
      if (g_bikeTrackCount >= kMaxBikeTracks) {
        ++g_bikeTrackOverflow;
        return;
      }
      ++g_bikeTrackCount;
      g_bikeTracks[slot] = BikeTrack{};
      fresh = true;
    }
    BikeTrack& t = g_bikeTracks[slot];
    float step = 0.f;
    if (fresh) {
      t.obj = obj;
      t.src = src;
      t.lr = lr;
      t.tid = tid;
      t.y_min = t.y_max = y;
    } else {
      const float dx = x - t.x, dy = y - t.y, dz = z - t.z;
      step = std::sqrt(dx * dx + dy * dy + dz * dz);
      t.step_sum += step;
      if (step > t.step_max) t.step_max = step;
      if (y < t.y_min) t.y_min = y;
      if (y > t.y_max) t.y_max = y;
      if (lr != t.lr) t.multi_lr = true;
      if (tid != t.tid) t.multi_tid = true;
    }
    t.x = x;
    t.y = y;
    t.z = z;
    ++t.samples;
    ++g_bikeSamples;
    NoteTagCandidates(nv, x, y, z);
    // Throttled: the answer is one offset, and 1-in-40 finds it in seconds
    // while keeping a 1.5 KB scan off every skinned draw.
    if ((g_bikeSamples % 40) == 0) ScanObjectForPosition(base, obj, x, y, z);

    // PER SERIES, not global: a budget shared across series is spent by
    // whichever object is drawn most and the one being chased goes unsampled.
    // 200 per series rather than 600: with up to 48 series the old budget is
    // 29k lines, which drowns the census that has to be read.
    const bool dense = t.reported < 200;
    if (dense || (t.samples % 120) == 0) {
      ++t.reported;
      line = fmt::format(
          "native: BIKE TRACE S{} obj=0x{:08X} #{} world ({:.3f}, {:.3f}, "
          "{:.3f}) step {:.3f} | camera ({:.3f}, {:.3f}, {:.3f}) is {:.3f} "
          "above it, {:.3f} away (arena 0x{:08X})",
          slot, obj, t.samples, x, y, z, step, cam[0], cam[1], cam[2],
          cam[1] - y, dist, src);
    }

    // THE CENSUS IS THE POINT. One line that shows every series side by side is
    // what the interleaved stream could never give, and its step column is the
    // verdict on the tag itself.
    if ((g_bikeSamples % 600) == 0) {
      census = fmt::format("native: BIKE TRACE CENSUS after {} samples -- {} "
                           "series:",
                           g_bikeSamples, g_bikeTrackCount);
      uint32_t thin = 0;
      for (uint32_t i = 0; i < g_bikeTrackCount; ++i) {
        const BikeTrack& e = g_bikeTracks[i];
        // A series with almost no samples cannot say anything about motion and
        // would push the ones that can off the end of the line. Counted, not
        // dropped silently.
        if (e.samples < 4) { ++thin; continue; }
        const double avg =
            e.samples > 1 ? e.step_sum / double(e.samples - 1) : 0.0;
        census += fmt::format(
            " [S{} obj=0x{:08X} n={} y {:.2f}..{:.2f} (span {:.2f}) step avg "
            "{:.3f} max {:.3f} lr=0x{:08X}{} t{}{}]",
            i, e.obj, e.samples, e.y_min, e.y_max, e.y_max - e.y_min, avg,
            e.step_max, e.lr, e.multi_lr ? "+" : "", e.tid,
            e.multi_tid ? "+" : "");
      }
      census += fmt::format(
          " || FIELD SCAN {} scans, {} matched nothing, {} distinct "
          "(offset, layout):",
          g_fieldScans, g_fieldMisses, g_fieldHitCount);
      // Ranked by HIT RATE against the scan total, best first, and only the
      // top eight: a field sits near 1.0 and the coincidences sit near 0.
      // Printing all 64 unsorted is how a 1.0 gets lost in a wall of 0.01.
      {
        uint32_t ord[kMaxFieldHits];
        for (uint32_t i = 0; i < g_fieldHitCount; ++i) ord[i] = i;
        for (uint32_t a = 0; a < g_fieldHitCount; ++a)
          for (uint32_t b = a + 1; b < g_fieldHitCount; ++b)
            if (g_fieldHits[ord[b]].hits > g_fieldHits[ord[a]].hits)
              std::swap(ord[a], ord[b]);
        const uint32_t show = g_fieldHitCount < 8 ? g_fieldHitCount : 8;
        for (uint32_t k = 0; k < show; ++k) {
          const FieldHit& h = g_fieldHits[ord[k]];
          const char* mode = h.mode == 0   ? "packed"
                             : h.mode == 1 ? "strided"
                             : h.mode == 2 ? "y-only"
                                           : "via";
          const double rate =
              g_fieldScans ? double(h.hits) / double(g_fieldScans) : 0.0;
          if (h.mode == 3)
            census += fmt::format(" [+0x{:X}->+0x{:X} via x{} rate {:.2f}]",
                                  h.off & 0xFFFFu, h.off >> 16, h.hits, rate);
          else
            census += fmt::format(" [+0x{:X} {} x{} rate {:.2f}]", h.off, mode,
                                  h.hits, rate);
        }
        if (g_fieldHitCount > show)
          census += fmt::format(" (+{} lower-rate offsets)",
                                g_fieldHitCount - show);
      }
      if (g_fieldOverflow)
        census += fmt::format(" (+{} dropped, table full)", g_fieldOverflow);
      // GROUND TRUTH, stage 1: the clipmap constants, from the PM4 file.
      census += " || TERRAIN " + TerrainConstReport();
      census += " || RINGS" + mx::gpu::alu::TerrainRings();
      float mres = 0.f, ox = 0.f, oz = 0.f;
      if (TerrainGrid(x, z, &mres, &ox, &oz))
        census += fmt::format(
            " || BIKE in ring res {:g}: grid ({:.3f}, {:.3f}) from world "
            "({:.3f}, {:.3f})",
            mres, (x - ox) / mres, (z - oz) / mres, x, z);
      else
        census += " || BIKE GRID -- no ring covers the bike (or the terrain has"
                  " not drawn)";
      if (thin) census += fmt::format(" (+{} series under 4 samples)", thin);
      if (g_bikeTrackOverflow)
        census += fmt::format(" (+{} samples dropped, table full)",
                              g_bikeTrackOverflow);

      // The tag hunt, ordered worst step LAST so the candidate to read is the
      // one at the front. "values" is how many distinct values that register
      // took; "+" means it took more than the table holds, which disqualifies
      // it however good its step looks.
      census += "\nnative: BIKE TAG HUNT (object = few values, small step):";
      uint32_t order[kTagRegs];
      for (uint32_t i = 0; i < kTagRegs; ++i) order[i] = i;
      const auto score = [](const TagCandidate& c) {
        double sum = 0.0;
        uint64_t n = 0;
        for (uint32_t k = 0; k < c.count; ++k) {
          sum += c.s[k].step_sum;
          n += c.s[k].n > 1 ? c.s[k].n - 1 : 0;
        }
        // No movement measured at all sorts LAST, not first: a register that
        // never grouped anything is the absence of evidence, and letting it
        // score 0.000 would put it at the top of the very list meant to name
        // the winner.
        return n ? sum / double(n) : 1e30;
      };
      for (uint32_t a = 0; a < kTagRegs; ++a)
        for (uint32_t b = a + 1; b < kTagRegs; ++b) {
          const bool a_bad = g_tagCands[order[a]].overflow != 0;
          const bool b_bad = g_tagCands[order[b]].overflow != 0;
          const bool swap = (a_bad && !b_bad) ||
                            (a_bad == b_bad && score(g_tagCands[order[b]]) <
                                                   score(g_tagCands[order[a]]));
          if (swap) std::swap(order[a], order[b]);
        }
      for (uint32_t o = 0; o < kTagRegs; ++o) {
        const uint32_t i = order[o];
        const TagCandidate& c = g_tagCands[i];
        const double s = score(c);
        census += fmt::format(" [r{} {}{} values step {}]", 14 + i, c.count,
                              c.overflow ? "+" : "",
                              s >= 1e29 ? std::string("--")
                                        : fmt::format("{:.3f}", s));
      }
    }
  }
  if (!line.empty()) REXLOG_INFO("{}", line);
  if (!census.empty()) REXLOG_INFO("{}", census);
}

}  // namespace

REX_IMPORT(__imp__sub_82550320, orig_SetVertexShaderConstantF, void());
extern "C" REX_FUNC(sub_82550320) {
  const uint32_t start = ctx.r4.u32;
  const uint32_t count = ctx.r6.u32;
  const uint32_t src = ctx.r5.u32;
  const uint32_t lr = uint32_t(ctx.lr);
  // BEFORE the original, which is free to clobber r3.
  const uint32_t device = ctx.r3.u32;
  // r14..r31 as the caller left them -- the tag hunt's candidates. Captured
  // before the original for the same reason as r3, though the ABI says these
  // survive it: a probe that depends on a callee honouring the ABI is a probe
  // that fails silently the day one does not.
  const uint32_t nv[18] = {
      ctx.r14.u32, ctx.r15.u32, ctx.r16.u32, ctx.r17.u32, ctx.r18.u32,
      ctx.r19.u32, ctx.r20.u32, ctx.r21.u32, ctx.r22.u32, ctx.r23.u32,
      ctx.r24.u32, ctx.r25.u32, ctx.r26.u32, ctx.r27.u32, ctx.r28.u32,
      ctx.r29.u32, ctx.r30.u32, ctx.r31.u32};
  orig_SetVertexShaderConstantF(ctx, base);
  // DOES ANYTHING SET THE TERRAIN CONSTANTS THROUGH THIS API AT ALL?
  //
  // Reading c200..c218 out of the device shadow returned 0.000 for all seven
  // registers while the camera at c8 reads correctly from the same shadow, so
  // the shadow is fine and these are simply not in it. Two known mechanisms
  // would do that -- shaders DMA their own ALU constants, and constants
  // published only by Type-0 PM4 never reach the shadow -- and both mean the
  // value has to be sourced somewhere else entirely.
  //
  // Before chasing either, establish whether this API carries them. A count of
  // ZERO here says the terrain never sets them this way and the shadow was
  // never going to work; a non-zero count with a device pointer different from
  // the bike's says the value is real but on another device, which is a much
  // easier fix (device state is per-device here).
  //
  // Placed BEFORE the register-85 early-out, which would otherwise reject every
  // one of these calls unseen.
  if (count && start <= 204u && 204u < start + count) {
    static std::atomic<uint64_t> s_hits{0};
    const uint64_t n = s_hits.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 4 || (n % 2048) == 0) {
      const uint32_t at = src + (204u - start) * 16u;
      float v[4] = {0, 0, 0, 0};
      if (GuestRangeReadable(base, at, 16u))
        for (uint32_t c = 0; c < 4; ++c) {
          const uint32_t bits = REX_LOAD_U32(at + c * 4u);
          std::memcpy(&v[c], &bits, 4);
        }
      REXLOG_INFO(
          "native: TERRAIN CONST SET #{} c204 = ({:.3f} {:.3f} {:.3f} {:.3f}) "
          "by lr=0x{:08X} device=0x{:08X} (start {} count {})",
          n, v[0], v[1], v[2], v[3], lr, device, start, count);
    }
  }

  // Register 85 is the first row of bone 0. One compare, and it rejects the
  // overwhelming majority of calls.
  if (!count || start > 85u || 85u >= start + count) return;

  static std::mutex s_mu;
  struct Site { uint32_t lr = 0; uint64_t calls = 0; float ty = 0; };
  static Site s_sites[12];
  static uint32_t s_siteCount = 0;
  static uint64_t s_overflow = 0;

  // The three rows of bone 0, read from the SOURCE buffer rather than the
  // device file: the source is what this caller supplied, which is the thing
  // being attributed. Guest floats are big-endian.
  const uint32_t at = src + (85u - start) * 16u;
  if (!GuestRangeReadable(base, at, 48u)) return;
  float row[3][4];
  for (uint32_t r = 0; r < 3; ++r)
    for (uint32_t c = 0; c < 4; ++c) {
      const uint32_t bits = REX_LOAD_U32(at + r * 16u + c * 4u);
      std::memcpy(&row[r][c], &bits, 4);
    }

  // THE BIKE'S HEIGHT OVER TIME, one series per object (see NoteBikeSample).
  //
  // The float is INTERMITTENT and random between runs, which kills the reading
  // it had before: a wheel-radius or suspension-rest-length constant would
  // float every time. Something is sampled once and sometimes gets a wrong
  // value, so what matters is WHEN the bike's Y goes wrong -- wrong from the
  // first frame is spawn placement, drifting into wrongness while riding is
  // the per-frame ground query. A capture cannot show that; a trace can.
  //
  // xe_c[8] is the camera in world space and lives at vertex constant register
  // 8, so it is readable from the device shadow at the moment of this write.
  // The bone translation is the `.w` of each row -- confirmed live at
  // (290.4, 558.3, 1332.5), a world-scale position. The camera sits ~7.8 units
  // above the ground and ~17 from the wheel, so a skinned object within 40
  // units is the bike or its rider and everything else in the level is not.
  {
    const uint32_t cam_at = device + 0x780u + 8u * 16u;
    if (GuestRangeReadable(base, cam_at, 16u)) {
      float cam[4];
      for (uint32_t c = 0; c < 4; ++c) {
        const uint32_t bits = REX_LOAD_U32(cam_at + c * 4u);
        std::memcpy(&cam[c], &bits, 4);
      }
      const float tx = row[0][3], ty = row[1][3], tz = row[2][3];
      const float dx = tx - cam[0], dy = ty - cam[1], dz = tz - cam[2];
      const float d2 = dx * dx + dy * dy + dz * dz;
      // The 40-unit gate stays: it is what keeps the level's other skinned
      // objects out, and the tag sorts out what is left inside it. The shape
      // being looked for is per-series -- alternating frame to frame is a
      // ping-pong read of the ground height (the terrain height buffers ARE a
      // ping-pong pair), a smooth wave is suspension resonance in the guest's
      // integration, and irregular motion tracking the terrain is just riding
      // over dunes and was never a defect.
      if (d2 < 40.0f * 40.0f && d2 > 0.0f)
        NoteBikeSample(base, device, src, lr, row[0][3], row[1][3], row[2][3],
                       cam,
                       std::sqrt(d2), nv);
    }
  }

  std::lock_guard<std::mutex> lk(s_mu);
  bool report = true;
  uint32_t idx = 0;
  for (; idx < s_siteCount; ++idx) {
    if (s_sites[idx].lr != lr) continue;
    // RE-REPORTED PERIODICALLY, not once. First-sighting only is what the
    // first cut did, and every line it produced came from the menu at startup:
    // translations near 97.8 while the bike in a level sits at world Y ~611.6.
    // A caller that writes every frame needs sampling over the run, or the one
    // value it prints is the least interesting one it ever wrote.
    report = (++s_sites[idx].calls % 4096u) == 0;
    break;
  }
  if (idx == s_siteCount) {
    if (s_siteCount >= 12) { ++s_overflow; return; }
    Site& e = s_sites[s_siteCount++];
    e.lr = lr;
    e.calls = 1;
  }
  if (!report) return;
  // Printed in full the first time each caller appears. Which component is the
  // translation is not assumed -- the shader reads these swizzled (.wzxy) -- so
  // all twelve floats go out and the one near the bike's world Y names itself.
  REXLOG_INFO(
      "native: VS CONST c85..c87 by lr=0x{:08X} (start {} count {}) -- "
      "[{:.3f} {:.3f} {:.3f} {:.3f}] [{:.3f} {:.3f} {:.3f} {:.3f}] "
      "[{:.3f} {:.3f} {:.3f} {:.3f}] (call {})",
      lr, start, count, row[0][0], row[0][1], row[0][2], row[0][3], row[1][0],
      row[1][1], row[1][2], row[1][3], row[2][0], row[2][1], row[2][2],
      row[2][3], s_sites[idx < s_siteCount ? idx : 0].calls);
}

REX_IMPORT(__imp__sub_824F8E20, orig_AssetDbLoadPackage, void());
extern "C" REX_FUNC(sub_824F8E20) {
  // Both args are plain char*, captured before the call in case it clobbers.
  const std::string db = GuestString(base, ctx.r3.u32, 64);
  const std::string pkg = GuestString(base, ctx.r4.u32, 64);
  orig_AssetDbLoadPackage(ctx, base);
  // ONE-TIME: the asset manager's vtable, so the slots this path uses can be
  // hooked by ADDRESS next build. Deriving them from the AssetDB ctor comment
  // ("vtable off_8214518C") gave a mid-function address for +0x78, so that
  // premise is wrong and the object has to answer for itself.
  //
  // vt[0x1C] load-DB-from-"Database\\", vt[0x24] is-DB-loaded,
  // vt[0x48] load-package, vt[0x78] the asset LOOKUP that returns NULL for
  // RiderUI_Final_C_350. A real guest function lives in 0x82xxxxxx; anything
  // else in a slot is data and must never be called.
  {
    static bool s_dumped = false;
    if (!s_dumped) {
      s_dumped = true;
      // dword_830BE400 is a POINTER VARIABLE, not the object. IDA renders the
      // engine as `dword_830BE400` and the manager as
      // `*(dword_830BE400 + 8)`, which reads as an offset off the symbol's
      // ADDRESS and is not: the engine is *(0x830BE400) and the manager is
      // *(engine + 8). Reading 0x830BE408 directly gave 0, which is what the
      // first version of this dump printed. SetupRenderer in hooks_loading.cpp
      // already does it the right way.
      const uint32_t eng = GuestRangeReadable(base, 0x830BE400u, 4)
                               ? REX_LOAD_U32(0x830BE400u)
                               : 0;
      const uint32_t mgr =
          (eng && GuestRangeReadable(base, eng + 8u, 4)) ? REX_LOAD_U32(eng + 8u)
                                                        : 0;
      const uint32_t vt =
          (mgr && GuestRangeReadable(base, mgr, 4)) ? REX_LOAD_U32(mgr) : 0;
      if (vt && GuestRangeReadable(base, vt, 0x7Cu)) {
        REXLOG_INFO("native: AssetMgr 0x{:08X} vtable 0x{:08X} -- "
                    "vt[0x1C]=0x{:08X} vt[0x24]=0x{:08X} vt[0x48]=0x{:08X} "
                    "vt[0x78]=0x{:08X}",
                    mgr, vt, REX_LOAD_U32(vt + 0x1C), REX_LOAD_U32(vt + 0x24),
                    REX_LOAD_U32(vt + 0x48), REX_LOAD_U32(vt + 0x78));
      } else {
        REXLOG_INFO("native: AssetMgr eng=0x{:08X} mgr=0x{:08X} vtable=0x{:08X}"
                    " -- not readable",
                    eng, mgr, vt);
      }
    }
  }
  static uint64_t s_n = 0;
  REXLOG_INFO("{}: AssetDB_LoadPackage #{} db=\"{}\" package=\"{}\" -> {} "
              "(0 = the package did NOT load)",
              mx::native::g_plugin_mode ? "plugin" : "native", ++s_n, db, pkg,
              ctx.r3.u32);
}

REX_IMPORT(__imp__sub_824AF488, orig_LoadAssetPackage, void());
extern "C" REX_FUNC(sub_824AF488) {
  const uint32_t L = ctx.r3.u32;
  const std::string db = ScriptArgString(base, L, 0);
  const std::string pkg = ScriptArgString(base, L, 1);
  orig_LoadAssetPackage(ctx, base);
  static uint64_t s_n = 0;
  REXLOG_INFO("{}: script LoadAssetPackage #{} db=\"{}\" package=\"{}\" -> "
              "r3=0x{:08X}",
              mx::native::g_plugin_mode ? "plugin" : "native", ++s_n, db, pkg,
              ctx.r3.u32);
}
MX_SCRIPT_PROBE(sub_824CD280, orig_StartWorldLoad, "StartWorldLoad")
MX_SCRIPT_PROBE(rex_MXRavage_Xenon_00cb, orig_ScriptBindingRegister, "BindingRegister")

// The script VM's native-call dispatcher, `sub_82AA7638`, used to be probed
// here with MX_SCRIPT_PROBE. It answered its original question — the VM fires a
// handful of times and goes idle, so the VM itself stops rather than looping
// without loading a world — but it only logged the lua_State, which cannot say
// *which* call was the last one. It now has a dedicated hook at the bottom of
// this file that decodes the callee off the Lua stack.

// The script layer is a Lua VM, so ask it directly whether it threw.
//
// `sub_82AA7638` is the call handler and carries the usual strings — it calls
// `sub_82AA9D48(L, "stack overflow")`. `sub_82A9F4F8` is the luaL_error-style
// reporter, used by ExecuteScriptAsset itself for argument mismatches
// ("Error in %s expected %d..%d args, got %d"), so its r4 is a format string.
//
// A root script that dies on its third statement looks, from outside, exactly
// like the two-libraries-then-silence pattern that is actually observed. These
// two hooks distinguish "stopped" from "crashed", which is the whole question.
REX_IMPORT(__imp__sub_82A9F4F8, orig_LuaError, void());
extern "C" REX_FUNC(sub_82A9F4F8) {
  static uint64_t s_count = 0;
  REXLOG_INFO("{}: lua error #{} fmt=\"{}\" (L=0x{:08X}) from lr=0x{:08X}",
              mx::native::g_plugin_mode ? "plugin" : "native", ++s_count,
              GuestString(base, ctx.r4.u32, 160), ctx.r3.u32,
              uint32_t(ctx.lr));
  orig_LuaError(ctx, base);
}

REX_IMPORT(__imp__sub_82AA9D48, orig_LuaRunError, void());
extern "C" REX_FUNC(sub_82AA9D48) {
  static uint64_t s_count = 0;
  REXLOG_INFO("{}: lua runerror #{} msg=\"{}\" (L=0x{:08X}) from lr=0x{:08X}",
              mx::native::g_plugin_mode ? "plugin" : "native", ++s_count,
              GuestString(base, ctx.r4.u32, 160), ctx.r3.u32,
              uint32_t(ctx.lr));
  orig_LuaRunError(ctx, base);
}

// The asset names themselves. `ExecuteScriptAsset` (sub_824AF838) validates
// that it got exactly one `char const*`, resolves it with the VM's string
// accessor, and passes the result to `sub_824F91E8` — so that function's r3 is
// the script asset name, in plain guest memory, before any VM indirection.
// Read out of the binding's own body, not inferred from the call shape.
REX_IMPORT(__imp__sub_824F91E8, orig_RunScriptAsset, void());
extern "C" REX_FUNC(sub_824F91E8) {
  const uint32_t name_ptr = ctx.r3.u32;
  const std::string name = GuestString(base, name_ptr, 128);
  static uint64_t s_count = 0;
  REXLOG_INFO("{}: script asset #{} \"{}\" (ptr=0x{:08X}) from lr=0x{:08X}",
              mx::native::g_plugin_mode ? "plugin" : "native", ++s_count, name,
              name_ptr, uint32_t(ctx.lr));
  orig_RunScriptAsset(ctx, base);
}

//=============================================================================
// Does the guest ever ask for input or audio?
//=============================================================================
// ReXGlue's own audio and input systems come up in native mode — the log shows
// "Input system initialized", "Audio system initialized", and the pad added at
// index 0 — yet neither works. The untested half is the guest: nothing in any
// log shows it calling in, because those are high-frequency kernel calls.
// `--log_high_frequency_kernel_calls=true` changed nothing (mx_386.log has the
// same 15 [krnl] lines as mx_385.log without it), so it does not gate these and
// the question has to be asked directly.
//
// These are the XDK wrappers around the import thunks, not the thunks — the
// thunks are defined in the runtime library and cannot be redefined here, but
// the wrappers are ordinary recompiled functions. Read out of the generated
// sources rather than inferred: each one's body is a register shuffle followed
// by a tail branch to the import (e.g. sub_82C08EC0 is `mr r5,r4; li r4,1;
// b __imp__XamInputGetState`, mx_recomp.90.cpp:16730).
//
// If all five stay silent, audio and input are downstream of "there is no menu"
// and are not a second bug.

REX_IMPORT(__imp__sub_82C08EC0, orig_XInputGetState, void());
extern "C" REX_FUNC(sub_82C08EC0) {
  static uint64_t s_count = 0;
  const uint32_t user = ctx.r3.u32;
  const uint32_t state_ptr = ctx.r4.u32;  // wrapper moves this to r5
  const uint32_t lr = uint32_t(ctx.lr);
  orig_XInputGetState(ctx, base);
  // Log the packet number after the call. The verification that matters is that
  // it changes while a button is held — a constant packet number means the break
  // is between SDL and the SDK, not in the guest.
  static std::chrono::steady_clock::time_point s_last{};
  const auto now = std::chrono::steady_clock::now();
  const bool due = (now - s_last) >= std::chrono::seconds(5);
  if (++s_count <= 4 || due) {
    if (due) s_last = now;
    const uint32_t packet = state_ptr ? REX_LOAD_U32(state_ptr) : 0;
    const uint32_t buttons = state_ptr ? REX_LOAD_U32(state_ptr + 4) : 0;
    REXLOG_INFO(
        "{}: XamInputGetState #{} user={} r3=0x{:08X} packet={} buttons=0x{:08X}"
        " from lr=0x{:08X}",
        mx::native::g_plugin_mode ? "plugin" : "native", s_count, user,
        ctx.r3.u32, packet, buttons, lr);
  }
}

// Boot-time pad enumeration. Fires a handful of times or not at all, so no
// throttle.
#define MX_IO_PROBE(addr, orig, label)                                      \
  REX_IMPORT(__imp__##addr, orig, void());                                  \
  extern "C" REX_FUNC(addr) {                                               \
    static uint64_t s_count = 0;                                            \
    const uint32_t a1 = ctx.r3.u32;                                         \
    const uint32_t lr = uint32_t(ctx.lr);                                   \
    ++s_count;                                                              \
    orig(ctx, base);                                                        \
    if (s_count <= 8)                                                       \
      REXLOG_INFO("{}: {} #{} a1=0x{:08X} -> r3=0x{:08X} from lr=0x{:08X}",  \
                  mx::native::g_plugin_mode ? "plugin" : "native", label,   \
                  s_count, a1, ctx.r3.u32, lr);                             \
  }

MX_IO_PROBE(sub_82C08ED0, orig_XInputGetCaps, "XamInputGetCapabilities")
MX_IO_PROBE(sub_82C87F78, orig_XAudioRegister, "XAudioRegisterRenderDriverClient")
MX_IO_PROBE(sub_82C4C268, orig_XMACreateContext, "XMACreateContext")

// The PCM submit path. Per-frame if audio is alive at all, so time-limited.
REX_IMPORT(__imp__sub_82C87B98, orig_XAudioSubmit, void());
extern "C" REX_FUNC(sub_82C87B98) {
  static uint64_t s_count = 0;
  const uint32_t a1 = ctx.r3.u32;
  const uint32_t lr = uint32_t(ctx.lr);
  static std::chrono::steady_clock::time_point s_last{};
  const auto now = std::chrono::steady_clock::now();
  const bool due = (now - s_last) >= std::chrono::seconds(5);
  orig_XAudioSubmit(ctx, base);
  if (++s_count <= 4 || due) {
    if (due) s_last = now;
    REXLOG_INFO(
        "{}: XAudioSubmitRenderDriverFrame #{} a1=0x{:08X} -> r3=0x{:08X}"
        " from lr=0x{:08X}",
        mx::native::g_plugin_mode ? "plugin" : "native", s_count, a1,
        ctx.r3.u32, lr);
  }
}

// Is the guest submitting real audio, or silence?
//
// `sub_82C87B98` is not a thin wrapper — it is the XDK mixer. It stack-allocates
// 8064 bytes, has `sub_82C87950` fill a buffer at r1+1888, and passes that to
// `__imp__XAudioSubmitRenderDriverFrame` (mx_recomp.94.cpp:31435). 8064-1888 =
// 6176 bytes of room, and a 360 render-driver frame is 256 samples x 6 channels
// x float32 = 6144, so the fill target is the frame buffer itself.
//
// Hooking the fill is the only way to see the samples: the import thunk is
// defined in the runtime library and cannot be redefined here. This separates
// "the SDK is dropping our audio" from "the game is playing nothing", which the
// submit count alone cannot.
REX_IMPORT(__imp__sub_82C87950, orig_AudioMixFrame, void());
extern "C" REX_FUNC(sub_82C87950) {
  const uint32_t dst = ctx.r4.u32;
  orig_AudioMixFrame(ctx, base);
  static uint64_t s_count = 0;
  static uint64_t s_nonsilent = 0;
  float peak = 0.0f;
  if (dst) {
    for (uint32_t i = 0; i < 256 * 6; ++i) {
      const uint32_t bits = REX_LOAD_U32(dst + i * 4);
      float v;
      std::memcpy(&v, &bits, sizeof(v));
      const float a = v < 0.0f ? -v : v;
      if (a > peak) peak = a;
    }
  }
  if (peak > 0.0f) ++s_nonsilent;
  static std::chrono::steady_clock::time_point s_last{};
  const auto now = std::chrono::steady_clock::now();
  const bool due = (now - s_last) >= std::chrono::seconds(5);
  if (++s_count <= 4 || due) {
    if (due) s_last = now;
    REXLOG_INFO("{}: audio mix #{} dst=0x{:08X} peak={:.6f} non-silent={}/{}",
                mx::native::g_plugin_mode ? "plugin" : "native", s_count, dst,
                peak, s_nonsilent, s_count);
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

//=============================================================================
// What is the last statement the native script layer runs?
//=============================================================================
// The VM stops about 1.6s into boot and everything downstream of it — the front
// end, the videos, the registry reads — is idle because of that. Counting script
// assets (2 native vs 4 plugin) is too coarse to say where it stops: two assets
// are libraries, and a library can load and then the caller die on its next
// statement.
//
// sub_82AA7638 is Lua's precall. Its r4 is the `func` StkId, so the callee is
// readable before it runs:
//
//   *(func + 8)  == 6      TValue.tt, LUA_TFUNCTION
//   *(func + 0)            the Closure
//   *(closure + 6)         isC — a C binding rather than Lua bytecode
//   *(closure + 16)        the C function pointer, for isC closures
//
// The offsets are Lua 5.1's and they are confirmed against this binary rather
// than assumed: the function's own Lua-closure branch reads Proto fields at
// +73 numparams, +74 is_vararg, +75 maxstacksize and +12 code, and it reports
// "stack overflow" through sub_82AA9D48 at exactly the 20000 limit.
//
// Every dispatch is logged up to a generous cap, so in native mode — where the
// whole run produces a handful — the last line of the log is literally the last
// statement the script layer reached.

namespace {

// ---- Naming the C function -------------------------------------------------
//
// The script layer is SWIG, not a hand-rolled binding table: the module init
// registers `swig_type` and `swig_equals`, and sub_824A8998 is
// SWIG_Lua_set_immutable ("This variable is immutable"). So the 228-entry table
// is one of five populations of lua_CFunction, and a miss in it never meant
// "not a binding" — 0x824A8AC0 reached a census line as a bare address for
// exactly that reason. Re-read in IDA 2026-08-28:
//
//   MXRavage_Xenon_00cb (0x824F1D80) is the module init. At 0x824F1E10 it calls
//   sub_824A8BA0(L, "Engine"), which builds the global `Engine` table and hangs
//   __index / __newindex / .get / .set off its metatable, and only then walks:
//
//     1. module functions    0x8203F2E0  {name, func}      via sub_824A8DC0
//     2. module attributes   0x82D1C858  {name, get, set}  via sub_824A8CE0
//     3. every class reachable through swig_types[], registered by sub_824A9580
//        (SWIG_Lua_class_register) and its member pass sub_824A9358
//
//   Populations 4 and 5 are the metamethods. They are INSTALLED, so no table
//   holds them and no scan can ever reach them: they are the fixed list below.
//
// Every SWIG array is NUL-terminated, never counted. The 228 is the measured
// distance to the terminator at 0x8203FA00 — (0x8203FA00 - 0x8203F2E0) / 8 —
// kept as a runaway bound rather than as the loop condition, so a table that
// grows cannot silently truncate.
//
// Layouts taken field for field out of sub_824A9358 (members) and sub_824A9500
// (bases), then checked against the live struct at 0x82D1B260: name
// "VariableCollection", methods 0x82D1B208. That array is what
// docs/guest_binary.md used to call "a second table around 0x82D1B21C" of
// unestablished bounds. It is not a second module table — it is one class's
// methods, and its bound is the terminator like every other SWIG array.
//
//   swig_type_info      +16 clientdata -> swig_lua_class*
//   swig_lua_class      +0 name  +8 constructor  +16 methods  +20 attributes
//                       +24 bases  +28 base_names
//   swig_lua_method      8 bytes {name, func}
//   swig_lua_attribute  12 bytes {name, getter, setter}
constexpr uint32_t kBindingTable = 0x8203F2E0;  // module functions
constexpr uint32_t kBindingCap = 228;           // terminator at 0x8203FA00
constexpr uint32_t kModuleAttrs = 0x82D1C858;   // module attributes
constexpr uint32_t kSwigTypes = 0x83016900;     // swig_type_info*[], .bss

// Runaway bounds for the arrays whose length is not statically known. These are
// NOT counts — the NUL entry ends every loop — they only stop a walk over
// uninitialised memory from running away.
constexpr uint32_t kScanCap = 512;
constexpr uint32_t kTypeCap = 4096;

struct SwigHelper {
  uint32_t fn;
  const char* name;
};
constexpr SwigHelper kSwigHelpers[] = {
    {0x824A89E8, "Engine.__index"},
    {0x824A8AC0, "Engine.__newindex"},
    {0x824A9A98, "swig_type"},
    {0x824A9AD8, "swig_equals"},
    {0x824A8998, "SWIG_Lua_set_immutable"},
    {0x824A8E18, "class.__index"},
    {0x824A8FC8, "class.__newindex"},
    {0x824A9120, "class.__gc"},
};

// {name, func} x 8 bytes. Both the module table and a class's `.fn` table.
std::string SwigMethodName(uint8_t* base, uint32_t table, uint32_t fn,
                           uint32_t cap) {
  if (!table) return {};
  for (uint32_t i = 0; i < cap; ++i) {
    const uint32_t e = table + i * 8;
    const uint32_t name = REX_LOAD_U32(e);
    if (!name) break;
    if (REX_LOAD_U32(e + 4) == fn) return GuestString(base, name, 64);
  }
  return {};
}

// {name, getter, setter} x 12 bytes. Which half ran is part of the name: the
// two can be the same function, and SWIG_Lua_set_immutable is the setter of
// every read-only variable in the module.
std::string SwigAttributeName(uint8_t* base, uint32_t table, uint32_t fn,
                              uint32_t cap) {
  if (!table) return {};
  for (uint32_t i = 0; i < cap; ++i) {
    const uint32_t e = table + i * 12;
    const uint32_t name = REX_LOAD_U32(e);
    if (!name) break;
    if (REX_LOAD_U32(e + 4) == fn) return GuestString(base, name, 64) + ".get";
    if (REX_LOAD_U32(e + 8) == fn) return GuestString(base, name, 64) + ".set";
  }
  return {};
}

// One caveat the wider search makes worse rather than better: identical-code
// folding means several trivial bindings share one address (0x829E8FA8 is a
// bare `return 0` with hundreds of xrefs), and a first-match scan will hand
// back whichever of them it reaches first. A name here is a name for the
// ADDRESS, not proof of which binding ran — see the Traps section of
// docs/guest_binary.md. Do not treat a folded hit as an identification.
std::string BindingName(uint8_t* base, uint32_t fn) {
  if (!fn) return {};
  for (const SwigHelper& h : kSwigHelpers) {
    if (h.fn == fn) return h.name;
  }
  std::string n = SwigMethodName(base, kBindingTable, fn, kBindingCap);
  if (!n.empty()) return n;
  n = SwigAttributeName(base, kModuleAttrs, fn, kScanCap);
  if (!n.empty()) return n;
  // The classes. swig_types[] lives in .bss and is filled by the module init,
  // which runs before any script does — and if it has not, the zero first entry
  // ends the walk rather than inventing a name.
  for (uint32_t i = 0; i < kTypeCap; ++i) {
    const uint32_t ti = REX_LOAD_U32(kSwigTypes + i * 4);
    if (!ti) break;
    const uint32_t cls = REX_LOAD_U32(ti + 16);  // clientdata
    if (!cls) continue;
    if (REX_LOAD_U32(cls + 8) == fn) {
      return GuestString(base, REX_LOAD_U32(cls), 64) + ".new";
    }
    n = SwigMethodName(base, REX_LOAD_U32(cls + 16), fn, kScanCap);
    if (n.empty()) {
      n = SwigAttributeName(base, REX_LOAD_U32(cls + 20), fn, kScanCap);
    }
    if (!n.empty()) return GuestString(base, REX_LOAD_U32(cls), 64) + ":" + n;
  }
  return {};
}

// ---- Who CALLED it ---------------------------------------------------------
//
// The dispatch lines used to report `lr` and nothing else, and that number is
// worthless as a caller: decompiled 2026-08-16, 0x82AABA74 is the `bl` to this
// function inside luaV_execute (sub_82AAAFD0), so lr=0x82AABA78 is the
// interpreter's OP_CALL and EVERY script-level call in the game reports it.
// luaD_precall has four call sites in total -- OP_CALL, OP_TAILCALL
// (0x82AABAC0, nresults = -1), and two C entry paths -- so the only thing an lr
// distinguishes here is "from a script" versus "from C".
//
// The CallInfo can name the caller, and the offsets are not carried over from
// the Lua 5.1 headers: sub_82AA9B70 is this title's `addinfo`, and it performs
// exactly the sequence below, field for field, to build its "%s:%d: %s" error
// prefix. Every load here is one the game itself does.
//
//   L+20   ci                        ci+4   func      func+8  TValue.tt (6 = fn)
//   *func  Closure                   +6     isC       +16     Proto (Lua only)
//   L+24   savedpc                   p+12   code      p+20    lineinfo
//   p+32   source (TString)          getstr = TString + 16
//
// currentline is `lineinfo[((savedpc - code) >> 2) - 1]`, and addinfo guards it
// with nothing but `pc >= 0` and `lineinfo != 0`. Matched rather than
// improved on: a bounds check against p+48 would rest on an offset the game
// never reads here, which is the kind of extrapolation this file keeps paying
// for.
// ---- Which CHUNKS execute at all -------------------------------------------
//
// The binding census can only name a script that calls a C binding, so
// "FE_Background is not in the list" was never evidence that it did not run.
// This closes that: executing a Lua chunk means CALLING its main function, so
// every chunk that runs at all passes through precall as a Lua callee.
//
// The population to compare against is the shipped manifest --
// `assets/Database/MXUI.xenon.database` declares 539 script assets over 82
// distinct names, readable with tools/bxml_full_decoder.py. Anything in that 82
// and absent here never executed.
//
// Keyed by PROTO address, not by name: one chunk has many Protos and the name
// resolution is a guest string walk, so keying by proto makes it one map lookup
// per dispatch and one string build per distinct function per run -- the same
// shape as the binding census. Names collapse in the report.
std::mutex g_chunkCensusMu;
std::map<uint32_t, std::string> g_chunkCensus;

std::string ProtoSource(uint8_t* base, uint32_t p) {
  if (!p) return {};
  const uint32_t source = REX_LOAD_U32(p + 32);
  // getstr(): the chars follow the TString header at +16. Confirmed against
  // sub_82AA9B70 -- see LuaCallerSite.
  if (!source) return {};
  std::string s = GuestString(base, source + 16, 96);
  // A chunk name is NOT always a name. Lua uses the loaded string itself as the
  // chunkname for a loadstring() chunk, and this title leans on that: the UI
  // instantiates every page with `FE_Background_33 = FE_Background:New()` and
  // that whole snippet arrives here as the "source". Some carry comment text
  // and EMBEDDED NEWLINES, which put a single log entry across many lines and
  // break every grep the log is read with -- the first census printed 134
  // chunks across ~70 physical lines and looked empty.
  for (char& c : s)
    if (c == '\n' || c == '\r' || c == '\t') c = ' ';
  if (s.size() > 72) s = s.substr(0, 69) + "...";
  return s;
}

void NoteChunk(uint8_t* base, uint32_t proto) {
  if (!proto) return;
  {
    std::lock_guard<std::mutex> lk(g_chunkCensusMu);
    if (g_chunkCensus.count(proto)) return;
  }
  // Resolved outside the lock; a duplicate resolve on a race is harmless and
  // costs one string.
  std::string s = ProtoSource(base, proto);
  if (s.empty()) return;
  std::lock_guard<std::mutex> lk(g_chunkCensusMu);
  g_chunkCensus.emplace(proto, std::move(s));
}

// ---- Which chunks are ACTIVE -----------------------------------------------
//
// `first_caller` on the binding census answers "who called this binding FIRST",
// and that is not the same question as "which chunks call bindings". A chunk
// that only ever calls bindings some earlier chunk already touched never
// appears in it.
//
// That distinction is not academic; it produced a wrong reading on 2026-08-16.
// The first-caller list was read as "FE_Background loads and instantiates but
// calls no binding, unlike FE_Home / FE_Home_Cameras / FE_Smoke", and that
// asymmetry was an ARTIFACT. Playing the credits proves it: FE_Credits is
// plainly active and on screen, and it is absent from the first-caller list too
// -- because everything it calls (SendEvent, PlayUISound, ...) was already
// first-seen elsewhere. Same shape as `counter-that-cannot-fire`.
//
// So count binding calls PER CALLING CHUNK. Keyed by the caller's Proto so the
// hot path is 5 guest loads and one map lookup, with the name resolved once
// through the chunk cache.
std::mutex g_callerCensusMu;
std::map<uint32_t, uint64_t> g_callerCensus;  // caller Proto -> binding calls

// The caller's Proto, or 0 for a C caller / no frame. See LuaCallerSite for why
// L->ci is the CALLER's frame here and where the offsets come from.
uint32_t LuaCallerProto(uint8_t* base, uint32_t L) {
  if (!L) return 0;
  const uint32_t ci = REX_LOAD_U32(L + 20);
  if (!ci) return 0;
  const uint32_t func = REX_LOAD_U32(ci + 4);
  if (!func || REX_LOAD_U32(func + 8) != 6) return 0;
  const uint32_t closure = REX_LOAD_U32(func);
  if (!closure || REX_LOAD_U8(closure + 6)) return 0;
  return REX_LOAD_U32(closure + 16);
}

std::string LuaCallerSite(uint8_t* base, uint32_t L) {
  if (!L) return {};
  // L->ci is still the CALLER's frame -- precall pushes the callee's, and this
  // runs before the original. For the same reason savedpc is read off L and not
  // off the CallInfo: precall's first statement copies one to the other, so the
  // CallInfo's copy is a statement stale until it has run.
  const uint32_t ci = REX_LOAD_U32(L + 20);
  if (!ci) return {};
  const uint32_t func = REX_LOAD_U32(ci + 4);
  if (!func || REX_LOAD_U32(func + 8) != 6) return {};
  const uint32_t closure = REX_LOAD_U32(func);
  // A C caller has no source and no line. That is the boot path and the
  // engine's own lua_call sites, and reporting it as such is the point: it
  // separates "a script asked for this" from "the engine did".
  if (!closure || REX_LOAD_U8(closure + 6)) return "[C]";
  const uint32_t p = REX_LOAD_U32(closure + 16);
  if (!p) return {};
  const int32_t pc =
      static_cast<int32_t>((REX_LOAD_U32(L + 24) - REX_LOAD_U32(p + 12)) >> 2) -
      1;
  const uint32_t lineinfo = REX_LOAD_U32(p + 20);
  const int32_t line =
      (pc >= 0 && lineinfo)
          ? static_cast<int32_t>(REX_LOAD_U32(lineinfo + 4 * uint32_t(pc)))
          : -1;
  const uint32_t source = REX_LOAD_U32(p + 32);
  std::string name = source ? GuestString(base, source + 16, 96) : std::string();
  if (name.empty()) name = fmt::format("proto 0x{:08X}", p);
  return fmt::format("{}:{}", name, line);
}

// ---- Binding census --------------------------------------------------------
//
// The 5-second sampler below cannot answer "did this binding ever fire". A
// one-shot call like SwitchToUIWorld [110] fires once in a run and has no
// meaningful chance of landing on a sampling boundary, so its absence from the
// log has never been evidence of anything. This is the census that makes the
// absence mean something — the same mistake as `counter-that-cannot-fire` and
// the two one-sided probes that hid the frame-pacing bug.
//
// One line the first time each distinct C binding is seen, plus a periodic
// roll-up with counts. `std::map` keyed by guest function address so the
// roll-up is ordered and stable between runs.
//
// Cost: one map lookup per dispatch (~700/min in the front end). BindingName's
// scan runs only on a miss, so at most once per distinct binding per run — it
// now walks the class tables too, which is a few thousand guest loads on the
// worst path (an unresolvable address) instead of 228. That is off the dispatch
// path but NOT off the roll-up path: ReportBindingCensus names every distinct
// binding again on each report. Bounded by how many bindings exist, so it stays
// a log-time cost, but do not move BindingName onto the per-call path.
//
// The precall fires on at least three guest threads (t11624, t17392, t20020 in
// mx_1196), so the map needs the lock. Do not "optimise" it away.
std::mutex g_bindingCensusMu;
// The call site is kept from the FIRST sighting only. A binding called from two
// places would hide the second, and that is the deliberate trade: the open
// question is which script reaches a binding at all, and a per-site breakdown
// costs a set per binding on the dispatch path. Widen it when a binding is
// known to have two callers, not before.
struct BindingCensusEntry {
  uint64_t count = 0;
  std::string first_caller;
};
std::map<uint32_t, BindingCensusEntry> g_bindingCensus;

// Bindings whose absence is the current open question, reported explicitly so a
// zero is stated rather than inferred from a name missing off a list. Addresses
// from docs/guest_binary.md "Binding tables".
struct WatchedBinding {
  uint32_t addr;
  const char* name;
};
constexpr WatchedBinding kWatchedBindings[] = {
    {0x824D0F18, "SwitchToUIWorld"},
    {0x824CD308, "EnableWorld"},
    {0x824CD280, "StartWorldLoad"},
    {0x824CBF90, "LoadUIAssetPackage"},
    {0x824CC218, "LoadUIAssetDatabasePackage"},
    {0x824CC120, "IsUIAssetPackageLoaded"},
    {0x824AF3C0, "LoadAssetDB"},
};

void ReportBindingCensus(uint8_t* base, const char* tag) {
  std::map<uint32_t, BindingCensusEntry> snapshot;
  {
    std::lock_guard<std::mutex> lk(g_bindingCensusMu);
    snapshot = g_bindingCensus;
  }
  REXLOG_INFO("{}: BINDING CENSUS — {} distinct C bindings called", tag,
              snapshot.size());
  for (const auto& [fn, e] : snapshot) {
    const std::string name = BindingName(base, fn);
    REXLOG_INFO("{}:   0x{:08X} x{} {} <- {}", tag, fn, e.count,
                name.empty() ? "(unresolved — no SWIG table names it)" : name,
                e.first_caller.empty() ? "?" : e.first_caller);
  }
  // Every Lua chunk that has executed, collapsed by name. Compare against the
  // 82 distinct script names in MXUI.xenon.database: a name shipped there and
  // missing here never ran, and FE_Background is the one that question was
  // asked for.
  {
    std::map<uint32_t, std::string> chunks;
    {
      std::lock_guard<std::mutex> lk(g_chunkCensusMu);
      chunks = g_chunkCensus;
    }
    std::map<std::string, uint32_t> by_name;
    for (const auto& [proto, name] : chunks) ++by_name[name];
    std::string list;
    for (const auto& [name, protos] : by_name)
      list += fmt::format(" {}({})", name, protos);
    REXLOG_INFO("{}: CHUNK CENSUS — {} distinct Lua chunks executed, {} protos:{}",
                tag, by_name.size(), chunks.size(), list);
  }
  // Binding calls per calling chunk, worst first. THIS is the "which scripts
  // are doing anything" list; the `<-` column on the census above is only ever
  // the first caller of each binding.
  {
    std::map<uint32_t, uint64_t> callers;
    std::map<uint32_t, std::string> chunks;
    {
      std::lock_guard<std::mutex> lk(g_callerCensusMu);
      callers = g_callerCensus;
    }
    {
      std::lock_guard<std::mutex> lk(g_chunkCensusMu);
      chunks = g_chunkCensus;
    }
    std::map<std::string, uint64_t> by_name;
    for (const auto& [proto, count] : callers) {
      const auto it = chunks.find(proto);
      by_name[it == chunks.end() ? "(unnamed)" : it->second] += count;
    }
    std::vector<std::pair<std::string, uint64_t>> sorted(by_name.begin(),
                                                         by_name.end());
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    std::string list;
    for (const auto& [name, count] : sorted)
      list += fmt::format(" {}={}", name, count);
    REXLOG_INFO("{}: CALLER CENSUS — {} chunks made binding calls:{}", tag,
                sorted.size(), list);
  }
  // State the zeroes. This is the half that a "which bindings fired" list
  // cannot give you, and it is the half the UI_World question needs.
  for (const auto& w : kWatchedBindings) {
    const auto it = snapshot.find(w.addr);
    REXLOG_INFO("{}:   WATCHED {:<28} 0x{:08X} x{} <- {}", tag, w.name, w.addr,
                it == snapshot.end() ? 0 : it->second.count,
                it == snapshot.end() ? "(never called)"
                                     : (it->second.first_caller.empty()
                                            ? "?"
                                            : it->second.first_caller.c_str()));
  }
}

}  // namespace

// Replaces the MX_SCRIPT_PROBE that used to sit next to the other script probes
// above. Same function, same "is the VM idle" answer, but it also names the
// callee, which is what turns a count into a last statement.
REX_IMPORT(__imp__sub_82AA7638, orig_ScriptDispatch, void());
extern "C" REX_FUNC(sub_82AA7638) {
  const uint32_t func = ctx.r4.u32;
  const uint32_t from = static_cast<uint32_t>(ctx.lr);

  uint32_t closure = 0;
  uint32_t cfunc = 0;
  bool is_c = false;
  bool is_fn = false;
  if (func && REX_LOAD_U32(func + 8) == 6) {
    is_fn = true;
    closure = REX_LOAD_U32(func);
    if (closure) {
      is_c = REX_LOAD_U8(closure + 6) != 0;
      // +16 is the C function for a C closure and the Proto for a Lua one --
      // the union the reference calls `c.f` / `l.p`.
      if (is_c) cfunc = REX_LOAD_U32(closure + 16);
      else NoteChunk(base, REX_LOAD_U32(closure + 16));
    }
  }

  // Census first, before the sampler, because this is the part that has to see
  // every call. `first_sight` is what catches a binding that fires once.
  //
  // The caller walk runs only on a binding's FIRST sighting, so it costs a
  // handful of guest loads per RUN rather than per dispatch. Resolved before
  // the lock -- it reads guest memory and takes no lock of its own, and holding
  // the census mutex across it would put the walk on three threads' critical
  // path for no reason.
  bool first_sight = false;
  if (is_c && cfunc) {
    bool need_caller = false;
    {
      std::lock_guard<std::mutex> lk(g_bindingCensusMu);
      auto [it, inserted] = g_bindingCensus.try_emplace(cfunc);
      ++it->second.count;
      first_sight = inserted;
      need_caller = it->second.first_caller.empty();
    }
    if (need_caller) {
      std::string site = LuaCallerSite(base, ctx.r3.u32);
      if (!site.empty()) {
        std::lock_guard<std::mutex> lk(g_bindingCensusMu);
        auto& e = g_bindingCensus[cfunc];
        if (e.first_caller.empty()) e.first_caller = std::move(site);
      }
    }
    // EVERY binding call, attributed to its calling chunk -- the population the
    // first-caller list above cannot give. See the note on g_callerCensus.
    if (const uint32_t cp = LuaCallerProto(base, ctx.r3.u32)) {
      NoteChunk(base, cp);
      std::lock_guard<std::mutex> lk(g_callerCensusMu);
      ++g_callerCensus[cp];
    }
  }

  static uint64_t s_count = 0;
  static std::chrono::steady_clock::time_point s_last{};
  static std::chrono::steady_clock::time_point s_last_census{};
  const uint64_t n = ++s_count;
  // Was 200 while this was the instrument hunting the frame-pacing bug. Now
  // that both modes run the front end it fires ~700 times a minute, so the head
  // is small and the rest is sampled. It stays because it is the cheapest
  // "is the front end actually running" signal there is.
  const char* tag = mx::native::g_plugin_mode ? "plugin" : "native";

  // A binding seen for the first time always logs, whatever the sampler says.
  // `lr` is kept only to separate a script caller from a C one -- see
  // LuaCallerSite for why it can say nothing more than that -- and the script
  // site is what the line is actually for.
  if (first_sight) {
    const std::string name = BindingName(base, cfunc);
    std::string site;
    {
      std::lock_guard<std::mutex> lk(g_bindingCensusMu);
      site = g_bindingCensus[cfunc].first_caller;
    }
    REXLOG_INFO(
        "{}: BINDING FIRST CALL 0x{:08X} {} at dispatch #{} from {} (lr=0x{:08X})",
        tag, cfunc, name.empty() ? "(unresolved — no SWIG table names it)" : name, n,
        site.empty() ? "?" : site, from);
  }

  bool due = n <= 8;
  if (!due) {
    const auto now = std::chrono::steady_clock::now();
    if ((now - s_last) >= std::chrono::seconds(5)) {
      s_last = now;
      due = true;
    }
  }
  if (due) {
    const std::string name = is_c ? BindingName(base, cfunc) : std::string();
    // The sampler is the "is the front end alive" signal, and a sampled line
    // naming the calling script says where it is alive -- which is the whole
    // question while the post-composite draw list is short. It runs at most
    // once every 5s, so the walk is free here.
    REXLOG_INFO("{}: vm dispatch #{} {} cfunc=0x{:08X}{}{} from {}", tag, n,
                !is_fn ? "non-function" : (is_c ? "C" : "lua"), cfunc,
                name.empty() ? "" : " name=", name,
                LuaCallerSite(base, ctx.r3.u32));
  }

  // Roll-up every 30s. Timestamped, so a binding that fires on a menu
  // transition can be placed against what was on screen at the time.
  {
    const auto now = std::chrono::steady_clock::now();
    if (s_last_census.time_since_epoch().count() == 0) {
      s_last_census = now;
    } else if ((now - s_last_census) >= std::chrono::seconds(30)) {
      s_last_census = now;
      ReportBindingCensus(base, tag);
    }
  }

  orig_ScriptDispatch(ctx, base);
}
