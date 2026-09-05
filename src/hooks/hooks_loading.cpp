// Loading-path hooks -- SetupRenderer, Transition, LoaderTick.
//
// This is the part of the boot sequence mid-ASM hooks used to carve into. **NO
// MID-ASM HOOK IS ACTIVE ANY MORE**: mx_asm.toml is 107 of 116 lines commented
// with nothing live, mx_config.toml has no mid-ASM section, and midasm_stubs.cpp
// -- which held the stubs they jumped to -- was deleted in 8ee7d8e once nothing
// referenced it from a live line. Nothing is skipped, the dispatch RUNS, and it
// is observed by a plain function hook on sub_82B34998.
//
// Comments below still describe hook #6's behaviour in the past tense; read them
// as history, not as the current configuration.

#include "hooks/hook_common.h"

#include <rex/cvar.h>

#include <bit>
#include <string>
#include <vector>

using mx::hooks::GuestString;

// The guest has its own frame limiter, and the hook below switches it ON.
//
// sub_82B70370, the pacer LoaderTick calls first, spins while dt is below
// engine+0x14 -- a minimum frame time in seconds, with FLT_MAX as the "no cap"
// sentinel the Engine constructor installs. The only thing that replaces the
// sentinel is SetupRenderer reading the "Game"/"VSync" setting: "30HZ" calls
// engine->vt[0] (SetMaxFrameRate, `engine+0x14 = 1.0f/fps`) with 30.0f, "60HZ"
// with 60.0f, and anything else -- including a missing key -- leaves the cap
// alone. The shipped data reads as neither, which is why this cvar exists.
//
// 30 is the rate the engine is built around: the constructor seeds all five
// slots of the dt smoothing ring with 0.033333 and their sum with 0.166665.
//
// The log says which state a run was in, and the transition is visible across
// three consecutive runs: mx_1984 logs `Timing guards +20=0x7F7FFFFF
// (FLT_MAX=true)`, and mx_1985/mx_1986 log `guest frame cap engine+0x14
// 0x7F7FFFFF -> 30 fps` followed by `+20=0x3D088889 (FLT_MAX=false)`.
//
// Two things to know before reading a result. The spin is no longer a busy
// wait: PaceFrame in hooks_frame.cpp sleeps until 1.2ms short of the deadline
// and leaves the last of it to the guest, so the loop confirms the edge instead
// of burning a core for a frame. And the cap is a floor on frame time, so it
// can only ever slow a frame down: where we already run slower than it, it does
// nothing at all, which is most of a level.
REXCVAR_DEFINE_INT32(guest_frame_cap_fps, 30, "Debug",
                     "Force the guest's own frame cap (engine+0x14) to this "
                     "rate after SetupRenderer. 0 keeps the guest's value");

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
  // `mr r31, r3` at 0x82B7115C: r3 is the Engine, the object every +0x14 /
  // +0x190 / +0x2DC store in this function writes to. Captured before the
  // original runs, which is free to clobber r3.
  const uint32_t engine = ctx.r3.u32;
  REXLOG_INFO("native: SetupRenderer ENTER (0x82B71148) engine=0x{:08X}", engine);
  orig_SetupRenderer(ctx, base);
  REXLOG_INFO("native: SetupRenderer RETURNED");

  const int cap_fps = REXCVAR_GET(guest_frame_cap_fps);
  if (cap_fps > 0 && engine) {
    const uint32_t before = REX_LOAD_U32(engine + 0x14u);
    const float min_frame = 1.0f / static_cast<float>(cap_fps);
    REX_STORE_U32(engine + 0x14u, std::bit_cast<uint32_t>(min_frame));
    REXLOG_INFO("native: guest frame cap engine+0x14 0x{:08X} -> {} fps "
                "({:.6f}s), read back 0x{:08X}",
                before, cap_fps, min_frame, REX_LOAD_U32(engine + 0x14u));
  }

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
    // last disabled execution stalled right here (in the mid-ASM stub that
    // midasm_stubs.cpp:33 held, before 8ee7d8e deleted the file) -- but
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

//=============================================================================
// The asset load path -- the state machine, the request chain, and the gate
//
// Moved verbatim from hooks_plugin_diag.cpp, now hooks_script_diag.cpp. This
// is the same subject as the rest of the file: LoaderTick drives the state machine below, and the state 6
// gate is what decides whether it advances past 6 or parks. They were three
// blocks in a diagnostics file with the registry chokepoint wedged between two
// of them.
//=============================================================================

// force_load is GONE, cvar and implementation both. It named a scene to request
// from the AssetDB once the loader went idle, by calling the guest's own
// sub_82534980(AssetDB, name, flags).
//
// The observation behind it still stands and is still unexplained: the loader
// reaches state 2 (IdleClearRenderBusy) and parks, because nothing in the game
// ever calls sub_82534980. That same idle AssetDB is why UI_World never loads,
// and it is the root of the 0x8234CE20 crash.

// sub_8253AA40 — AssetDB_LoadStateMachine (LoaderTick's gate, 12-state)
REX_IMPORT(__imp__sub_8253AA40, orig_LoadStateMachine, void());
// Logs in BOTH modes. The note that used to sit here -- "in native this is
// unreachable, so its absence from the log is itself the signal" -- is STALE:
// every mid-ASM hook in mx_config.toml is commented out, so LoaderTick's vt[6]
// gate runs and this fires continuously. Its absence would now mean something is
// wrong, not something is skipped.
//
// What that run showed: the machine goes 0 -> 1 -> 2 and then stays at 2. State
// 2 is idle-awaiting-a-request -- sub_82534980 is what moves it 2 -> 3, and in a
// front-end-only run nothing calls it.
extern "C" REX_FUNC(sub_8253AA40) {
  static int sm = 0;
  ++sm;
  uint32_t a1 = ctx.r3.u32;
  // The state is *(a1+28) — a 0..11 selector. Derived from the recompiled body:
  // mx_recomp.31.cpp:36836 `lwz r11,28(r31)` (r31 = a1, never reassigned) feeds
  // the 12-entry jump table at :36862. The `+110796` this used to read came from
  // pm4_pipeline.md and is a guest heap pointer, not the enum.
  uint32_t state_in = a1 ? REX_LOAD_U32(a1 + 28) : 0xFFFFFFFF;
  // Every store to *(a1+28) I could find is inside this function, but that was
  // a grep of mx_recomp.31.cpp only and would miss a write through a computed
  // pointer regardless. Settle it with data: if the state changed between our
  // last return and this entry, something outside sub_8253AA40 wrote it.
  static uint32_t s_prev_out = 0xFFFFFFFE;
  if (s_prev_out != 0xFFFFFFFE && state_in != s_prev_out) {
    REXLOG_INFO("native: EXTERNAL WRITE to AssetDB+28: {} -> {} between calls #{} and #{}",
                s_prev_out, state_in, sm - 1, sm);
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
    REXLOG_INFO("native: LoadStateMachine #{} state {} -> {} r3=0x{:08X}{}", sm,
                state_in, state_out, ctx.r3.u32, changed ? "  <-- CHANGED" : "");
  }

  // State 6 parks. Exactly one predicate is responsible: loc_8253B504 reads
  // *(a1+110328) and, finding it zero, goes to loc_8253B560, which reads the
  // listener at *(a1+110788) and calls its vt[2]. A zero return jumps to an
  // early return that leaves the selector at 6.
  //
  // The listener is the same object sub_82534980 notifies via vt[0], and it is
  // assigned once in the AssetDB constructor -- so it is never null and the
  // vt[2] branch is always taken. Dump it once so vt[2] can be resolved.
  if (a1 && state_out == 6) {
    static bool s_dumped = false;
    if (!s_dumped) {
      s_dumped = true;
      uint32_t obj = REX_LOAD_U32(a1 + 110788);
      uint32_t sib = REX_LOAD_U32(a1 + 110792);
      uint32_t vt = obj ? REX_LOAD_U32(obj) : 0;
      REXLOG_INFO("native: state6 gate — listener(+110788)=0x{:08X} sibling(+110792)=0x{:08X} "
                  "vt=0x{:08X} +110328=0x{:08X}",
                  obj, sib, vt, REX_LOAD_U32(a1 + 110328));
      if (vt) {
        // A real guest function pointer lives in 0x82xxxxxx. Anything else in a
        // vtable slot is data — assetdb vt[36] reads 0x53505F45 ("SP_E") — so
        // print the slots raw and judge them by range, never call them blind.
        REXLOG_INFO("native: state6 gate — vt[0]=0x{:08X} vt[1]=0x{:08X} vt[2]=0x{:08X} vt[3]=0x{:08X}",
                    REX_LOAD_U32(vt), REX_LOAD_U32(vt + 4),
                    REX_LOAD_U32(vt + 8), REX_LOAD_U32(vt + 12));
      }
    }
  }
}

//=============================================================================
// The load-request chain
//
// sub_82534980 is the guest's load-request API. It has exactly one caller,
// sub_82352AE0, which builds the scene name from a registry lookup and is itself
// a method with five callers. Nothing in this chain has ever been observed to
// run in native mode -- the point of these hooks is to find out how far up
// execution actually reaches.
//=============================================================================

// sub_82534980 — AssetDB_RequestLoad(AssetDB, name, flags). Copies up to 260
// bytes of `name` to AssetDB+29540, stores flags at +29800, and moves the
// selector 2 -> 3.
REX_IMPORT(__imp__sub_82534980, orig_RequestLoad, void());
extern "C" REX_FUNC(sub_82534980) {
  uint32_t a1 = ctx.r3.u32;
  std::string name = GuestString(base, ctx.r4.u32);
  uint32_t state_in = a1 ? REX_LOAD_U32(a1 + 28) : 0xFFFFFFFF;
  REXLOG_INFO("native: RequestLoad(sub_82534980) a1=0x{:08X} name=\"{}\" flags=0x{:08X} state={}",
              a1, name, ctx.r5.u32, state_in);
  orig_RequestLoad(ctx, base);
  REXLOG_INFO("native: RequestLoad returned — state {} -> {}", state_in,
              a1 ? REX_LOAD_U32(a1 + 28) : 0xFFFFFFFF);
}

// sub_82352AE0 — the sole caller of RequestLoad; resolves the scene name from
// the registry. Takes a `this` pointer in r3.
REX_IMPORT(__imp__sub_82352AE0, orig_RequestLoadCaller, void());
extern "C" REX_FUNC(sub_82352AE0) {
  REXLOG_INFO("native: sub_82352AE0 (RequestLoad caller) ENTER this=0x{:08X}",
              ctx.r3.u32);
  orig_RequestLoadCaller(ctx, base);
}

// The five callers of sub_82352AE0. Entry-only — one line each is enough to see
// where the chain stops.
#define MX_CHAIN_PROBE(addr, sym)                                             \
  REX_IMPORT(__imp__sub_##addr, orig_chain_##addr, void());                   \
  extern "C" REX_FUNC(sub_##addr) {                                           \
    static int n = 0;                                                         \
    if (++n <= 5)                                                             \
      REXLOG_INFO("native: chain " sym " ENTER #{} r3=0x{:08X}", n,           \
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
// The state 6 gate
//
// State 6 polls (*(AssetDB+110788))->vt[2] and takes an early return whenever it
// answers 0, which is why the selector never reaches 7. That slot resolves to
// sub_8253CF80, whose body is:
//
//   mode = sub_82536250(*(0x830577C0));   // maps a registry string to an enum
//   if (mode == 2 || mode == 3) return 1;
//   if (*(0x83057900) != 0)     return 1;
//   tmp = 0; sub_82548758(registry, <key>, &tmp, 0); return tmp;
//
// Note state 1 clears *(0x83057900) on the way past, so boot itself closes the
// second escape. These hooks report which term is actually deciding.
//=============================================================================

// sub_82536250 — registry-string -> mode enum, the gate's first term.
REX_IMPORT(__imp__sub_82536250, orig_GateMode, void());
extern "C" REX_FUNC(sub_82536250) {
  uint32_t a1 = ctx.r3.u32;
  orig_GateMode(ctx, base);
  static int n = 0;
  if (++n <= 3 || (n % 500) == 0)
    REXLOG_INFO("native: GateMode(sub_82536250) #{} a1=0x{:08X} -> {}",
                n, a1, ctx.r3.u32);
}

// sub_8253CF80 — the gate itself, (*(AssetDB+110788))->vt[2].
REX_IMPORT(__imp__sub_8253CF80, orig_Gate, void());
extern "C" REX_FUNC(sub_8253CF80) {
  orig_Gate(ctx, base);
  static int n = 0;
  static uint32_t s_last = 0xFFFFFFFF;
  uint32_t ret = ctx.r3.u32;
  if (++n <= 3 || ret != s_last || (n % 500) == 0) {
    REXLOG_INFO("native: state6 gate(sub_8253CF80) #{} -> {}  assetdb(0x830577C0)=0x{:08X} "
                "flag(0x83057900)=0x{:08X}", n, ret,
                REX_LOAD_U32(0x830577C0), REX_LOAD_U32(0x83057900));
    s_last = ret;
  }
}
