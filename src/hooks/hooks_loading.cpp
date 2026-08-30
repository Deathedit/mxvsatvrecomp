// Loading-path hooks — SetupRenderer, Transition, LoaderTick.
//
// This is the part of the boot sequence mid-ASM hooks used to carve into.
// **NO MID-ASM HOOK IS ACTIVE ANY MORE**, checked 2026-08-28: mx_asm.toml is
// 107 of 116 lines commented with nothing live, and mx_config.toml has no
// mid-ASM section at all. The stubs in midasm_stubs.cpp are referenced only
// from commented mx_asm.toml lines.
//
// This header used to say "only hook #6 is left, and it skips exactly one
// instruction — the `bl sub_82B34998` renderer dispatch at 0x82B70EF4". That is
// no longer true and had not been for some time: nothing is skipped, the
// dispatch RUNS, and it is observed by a plain function hook on sub_82B34998 in
// hooks_plugin_diag.cpp which calls its original in both modes.
//
// Comments in this file below still describe hook #6's behaviour in the past
// tense; read them as history, not as the current configuration.

#include "hooks/hook_common.h"

#include <mutex>
#include <set>

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

  // DORMANT fallback. SetupRenderer's vt[17] (sub_82B43AC8 @ 0x82B71310) writes
  // `*(eng+8) = assetdb_block` — the 545KB block allocated at 0x82B712D8 by
  // sub_82AB73C0(0x85280) and initialized by AssetDB_InnerCtor_VtableInstall at
  // 0x82B712EC. This block existed because mid-ASM hook #4 used to skip vt[17],
  // leaving eng+8 NULL.
  //
  // Hook #4 is disabled, so vt[17] runs and eng+8 is populated for real — the
  // branch below is not taken (log line: "eng+8 already populated"). Kept as a
  // fallback in case #4 is ever re-enabled.
  //
  // If it does run it re-allocates the block and calls the inner ctor to install
  // the same vtable, but SKIPS vt[17]'s secondary sub_82526D10 call (18-subsystem
  // AssetDB registration) — so it is not a faithful substitute.
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
  // access-violates when mid-ASM hook #7 is disabled. The two candidates are
  // the indirect calls at 0x82B70E28 (engine->vt[2]() -> scene manager) and
  // 0x82B70E3C (sceneMgr->vt[32](dt)). Log the slots read-only — do NOT call
  // them — so we can see which resolves to garbage. Sane targets are 0x82XXXXXX.
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
    // LoaderTick's first instruction is Wait(*(tr+0x194), -1). Now that the
    // fake INFINITE-wait success is gone, that wait is real and the Transition
    // thread parks in it. Log the handle so it can be matched against the
    // NtSetEvent handles actually being signalled.
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
    // wholesale, then narrowed to skipping only the `bl sub_82B34998` dispatch
    // at 0x82B70EF4. It is disabled now along with every other mid-ASM hook, so
    // the whole band runs including that dispatch. These reads established that
    // the band's inputs were real before the narrowing; they stay as a
    // regression check.

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
  // The `if (r3 != 0 && lt > 100) r3 = 0` cap is REMOVED (2026-08-02). It was a
  // FABRICATED completion from when LoaderTick's body was deleted by mid-ASM
  // hooks #7/#8 and the loop had to be broken artificially — nothing was ever
  // loaded. The body now runs for real and AssetDB_LoadStateMachine ticks each
  // iteration, so forcing r3=0 would kill the Transition loop mid-load.
  if (lt <= 5 || lt % 500 == 0)
    REXLOG_INFO("native: LoaderTick #{} r3={}", lt, ctx.r3.u32);
}

//=============================================================================
// WHY NO TREES DRAW — a three-way probe on the guest's own ForestSystem.
//
// The renderer is not dropping them: FRAME DRAWS reads `guest N accepted N
// refused 0` every frame, and the palm's leaf atlas, bark and the FR_DU_Eco
// billboard atlas are never bound in a capture. The guest is not SUBMITTING
// tree draws, so the question is which link of its own chain is cold.
//
// The chain, read out of the IDB (ForestSystem, ctor at 0x823FB5B8, which
// spawns the cull thread sub_823F9260 and creates the ForestSystemStart/End/
// Kill events):
//
//   sub_823E70C8   asset callback: looks "ForestAsset" up by type name,
//                  stores it at this+156, then calls ->
//   sub_823E6EA8   init: sets THE GATE at this+16024 to 1, reads the tree-type
//                  count from the asset and computes the two LOD distance
//                  thresholds at this+12404 / this+12408.
//   sub_823F9798   per frame: `if (*(this+15880))` -- the same gate, reached
//                  through the IRenderable subobject at this+144, so +15880
//                  here IS +16024 there -- then builds the frustum and SetEvents
//                  ForestSystemStart, releasing the cull thread.
//   sub_823EDF00   called once per tree that survives the frustum and both
//                  distance tests.
//
// The gate is written in exactly ONE place in the whole image (0x823E6EE0,
// inside the init), and the constructor does NOT initialise it. So if the asset
// callback never fires, the gate is whatever the allocation left behind, the
// cull thread blocks on ForestSystemStart forever, and no tree is ever
// submitted -- with no error anywhere, which is what we see.
//
// THREE OUTCOMES, and they point at different code:
//   init 0                  -> the ForestAsset callback never fires. An asset
//                              registration problem, the shape of
//                              [[asset-load-is-an-async-queue]].
//   init > 0, gate-open 0   -> it initialised and the gate is still closed.
//   gate-open > 0, added 0  -> the cull runs and rejects every tree; the
//                              thresholds or the frustum are wrong.
//
// Counted on EVERY call, not sampled, so a zero means "never happened" and not
// "not caught" ([[guard-census-api]]). Reported off the init/kick hooks
// themselves so the line cannot appear before the system exists.
//=============================================================================

// Declared rather than including hooks_d3d9_internal.h, which would drag the
// whole D3D9 hook surface into a loading-path translation unit for one
// predicate. This is the RIGHT guard: PlausibleGuestPtr is only a range test
// and ASCII passes it ([[plausible-is-not-readable]]).
namespace mx::hooks::d3d9 {
bool HostPageReadable(const void* p);
}  // namespace mx::hooks::d3d9
// Cumulative accepted guest draws. GLOBAL SCOPE, not mx::hle: hooks_d3d9.h
// closes that namespace before declaring it, and hooks_frame.cpp calls it
// unqualified. Declared rather than including the header, which would pull
// the whole D3D9 surface into a loading TU.
uint64_t HleDrawsAccepted();

namespace {

using mx::hooks::d3d9::HostPageReadable;

// The distinct (renderer, caller) pairs the batches came from, printed as part
// of the REPEATING census rather than once on first sighting.
//
// The reason recorded here first was WRONG and is worth keeping straight. I
// read "2952 batches emitted, zero BATCH lines" as the log having rotated the
// lines away ([[logs-rotate-under-you]]) and rewrote the probe for it. The
// actual cause was a STALE BINARY: the game runs mx.exe from the REPO ROOT, the
// build writes out/build/win-amd64-release/mx.exe, and nothing copies it. Runs
// 1775 and 1776 executed a 02:42 binary that predated the BATCH line entirely.
//
// Reporting from the repeating census is still the better shape -- it is the
// same lesson the two probes before this one taught -- but it did not fix what
// I said it fixed, and rotation was never involved.
std::mutex g_forestBatchMu;
std::set<uint64_t> g_forestBatchSites;   // (renderer << 32) | caller
std::atomic<uint64_t> g_forestDrawsProduced{0};
std::atomic<uint64_t> g_forestDrawCalls{0};
std::atomic<uint64_t> g_forestBatches{0};
std::atomic<uint64_t> g_guestThreads{0};
std::atomic<uint64_t> g_forestThreadBody{0};
std::atomic<uint64_t> g_forestInit{0};
std::atomic<uint64_t> g_forestKickEnter{0};
std::atomic<uint64_t> g_forestKickReturn{0};
std::atomic<uint64_t> g_forestKickGateOpen{0};
std::atomic<uint64_t> g_forestTreeAdded{0};
std::atomic<uint32_t> g_forestAsset{0};

// ENTERED and RETURNED are separate on purpose, and the first cut of this got
// it wrong. It reported from inside the kick AFTER calling the original, so a
// kick that blocks -- which this one can, it does
// WaitForSingleObject(ForestSystemEnd, INFINITE) before releasing the cull
// thread -- produced no line at all and read as "the kick stopped happening".
// entered > returned is a STALL and has to be visible as one.
void ReportForest(const char* why) {
  REXLOG_INFO("forest: {} -- init {}, kicks entered {} returned {} (gate OPEN "
              "{}), trees added {}; ForestAsset handle 0x{:08X}",
              why, g_forestInit.load(std::memory_order_relaxed),
              g_forestKickEnter.load(std::memory_order_relaxed),
              g_forestKickReturn.load(std::memory_order_relaxed),
              g_forestKickGateOpen.load(std::memory_order_relaxed),
              g_forestTreeAdded.load(std::memory_order_relaxed),
              g_forestAsset.load(std::memory_order_relaxed));
  std::string sites;
  {
    std::lock_guard<std::mutex> lk(g_forestBatchMu);
    for (uint64_t k : g_forestBatchSites)
      sites += fmt::format(" r0x{:08X}/lr0x{:08X}", uint32_t(k >> 32),
                           uint32_t(k));
  }
  REXLOG_INFO("forest:   guest threads created {}, cull-thread body entered {} "
              "| DRAW consumer called {}, batches emitted {}, GUEST DRAWS PRODUCED {} | batch sites{}",
              g_guestThreads.load(std::memory_order_relaxed),
              g_forestThreadBody.load(std::memory_order_relaxed),
              g_forestDrawCalls.load(std::memory_order_relaxed),
              g_forestBatches.load(std::memory_order_relaxed),
              g_forestDrawsProduced.load(std::memory_order_relaxed),
              sites.empty() ? " (none)" : sites);
}

}  // namespace

namespace mx::hooks {
// Called from the frame census, which runs whether or not the forest is stuck.
void ReportForestCensus() { ReportForest("census"); }
}  // namespace mx::hooks

// ForestSystem::Init -- the only writer of the gate.
REX_EXTERN(__imp__sub_823E6EA8);
REX_HOOK_RAW(sub_823E6EA8) {
  const uint32_t self = ctx.r3.u32;
  // The asset the callback just stored, read BEFORE the original runs so a
  // zero here is what init was handed rather than what it left behind.
  if (self && HostPageReadable(REX_RAW_ADDR(self + 156)))
    g_forestAsset.store(REX_LOAD_U32(self + 156), std::memory_order_relaxed);
  g_forestInit.fetch_add(1, std::memory_order_relaxed);
  __imp__sub_823E6EA8(ctx, base);
  ReportForest("init ran");
}

// The per-frame kick. r3 is the IRenderable subobject (this+144), so the gate
// is at +15880 from it -- the same dword init writes as this+16024.
REX_EXTERN(__imp__sub_823F9798);
REX_HOOK_RAW(sub_823F9798) {
  const uint32_t renderable = ctx.r3.u32;
  const uint64_t n = g_forestKickEnter.fetch_add(1, std::memory_order_relaxed) + 1;
  bool open = false;
  if (renderable && HostPageReadable(REX_RAW_ADDR(renderable + 15880))) {
    open = REX_LOAD_U32(renderable + 15880) != 0;
    if (open) g_forestKickGateOpen.fetch_add(1, std::memory_order_relaxed);
  }
  if (n == 1) ReportForest(open ? "first kick, gate OPEN"
                                : "first kick, GATE CLOSED");
  __imp__sub_823F9798(ctx, base);
  g_forestKickReturn.fetch_add(1, std::memory_order_relaxed);
}

// One call per tree that survived the frustum and both distance tests.
REX_EXTERN(__imp__sub_823EDF00);
REX_HOOK_RAW(sub_823EDF00) {
  g_forestTreeAdded.fetch_add(1, std::memory_order_relaxed);
  __imp__sub_823EDF00(ctx, base);
}

//=============================================================================
// GUEST THREAD CREATION, and whether each thread's body ever starts.
//
// sub_82BFC370 is the guest's create-thread, and nothing logged a CREATION --
// only two thread BODIES were instrumented.
//
// CORRECTION, and the reason this comment is worth reading: an earlier version
// of it said the AssetDB DatabaseThread "does not appear to run" and that
// whether it runs was "never answered". Both are false, and the log had said so
// all along:
//
//     DatabaseThread ENTER assetDb=0x212B28E0 event(+0x688)=0xF800011C
//                          gate(0x82D57950)=0x00000001
//
// That thread is already hooked in hooks_plugin_diag.cpp, it enters, and the
// gate it was suspected of failing reads ONE, so its body runs. The level loads
// 403 assets and they render -- which on its own refutes "the worker never
// registers the assets" ([[asset-load-is-an-async-queue]] states that too
// broadly; whatever failed for the garage bink, it was not the worker being
// dead). Do not go looking for a dead AssetDB thread again.
//
// What is genuinely missing is creation-side coverage: which threads exist at
// all, and their ENTRY POINTS. That is what this adds, for every guest thread
// rather than the two that happened to be suspected.
//
// The ENTRY POINT is the identity worth printing: thread handles vary per run
// but the entry is a fixed guest address, so a line here can be matched against
// the IDB directly.
//=============================================================================

REX_EXTERN(__imp__sub_82BFC370);
REX_HOOK_RAW(sub_82BFC370) {
  // r5 is the entry point, r6 the parameter -- the argument order the AssetDB
  // constructor's disassembly shows (`addi r5, r5, entry@l` then `mr r6, r31`).
  const uint32_t entry = ctx.r5.u32;
  const uint32_t param = ctx.r6.u32;
  const uint64_t n = g_guestThreads.fetch_add(1, std::memory_order_relaxed) + 1;
  __imp__sub_82BFC370(ctx, base);
  // Logged AFTER, so the handle in r3 is the real one. Bounded: a run creates a
  // handful, and an unbounded line here would be a log flood if that changed.
  if (n <= 32)
    REXLOG_INFO("guest thread #{}: entry 0x{:08X} param 0x{:08X} -> handle {}",
                n, entry, param, ctx.r3.u32);
}

// The ForestSystem cull thread's BODY. An infinite loop, so this fires exactly
// once if the thread is ever scheduled and never if it is not -- which is the
// difference between "created" and "running" and is the whole question.
//
// Logged BEFORE calling the original, because the original does not return.
REX_EXTERN(__imp__sub_823F9260);
REX_HOOK_RAW(sub_823F9260) {
  g_forestThreadBody.fetch_add(1, std::memory_order_relaxed);
  REXLOG_INFO("forest: CULL THREAD BODY entered (this 0x{:08X}) -- it is "
              "scheduled; anything after this is the wait on ForestSystemStart",
              ctx.r3.u32);
  __imp__sub_823F9260(ctx, base);
}

//=============================================================================
// THE DRAW SIDE. The cull works -- 2337 trees added in mx_1773 -- so whatever
// is wrong is downstream of it.
//
// sub_823F9808 is the consumer, read out of the IDB:
//
//     WaitForSingleObject(this[3974], INFINITE)
//     for each tree TYPE t, for each entry i:
//         sub_823F82D0(renderer, t, i, this[...], this[...])
//     (*(renderer_vtable + 16))(renderer)          <- the flush
//     SetEvent(this[3974])
//
// so there are exactly two ways for 2337 culled trees to produce no draw: the
// consumer never runs, or it runs and its loop bound is zero -- the cull fills
// one set of lists and the draw reads another, or reads them after something
// has cleared them.
//
// Counting both distinguishes those without another IDA session, and the ratio
// is the interesting part: batches per consumer call against trees added.
//=============================================================================

// THE DECISIVE MEASUREMENT. sub_823F82D0 only APPENDS -- the draws come out of
// the `(*(renderer_vtable + 16))(renderer)` flush at the end of this function.
// So counting batches says the guest built tree geometry; it does not say a
// single draw reached D3D.
//
// Bracketing the whole consumer with our own accepted-draw counter does. The
// delta is exactly the guest draws this forest pass produced:
//
//   0        the forest builds 2652 batches a run and issues NO draw. The bug
//            is inside the flush, upstream of anything we hook.
//   ~12/frame the trees ARE drawn and we lose them somewhere after -- but
//            FRAME DRAWS says refused 0, so they would be draws we accept and
//            then fail to make visible.
//
// HleDrawsAccepted is a plain uint64_t written without a lock, which is fine
// here: this is a per-call delta on one thread, not a precise total.
REX_EXTERN(__imp__sub_823F9808);
REX_HOOK_RAW(sub_823F9808) {
  g_forestDrawCalls.fetch_add(1, std::memory_order_relaxed);
  const uint64_t before = HleDrawsAccepted();
  __imp__sub_823F9808(ctx, base);
  const uint64_t after = HleDrawsAccepted();
  if (after > before)
    g_forestDrawsProduced.fetch_add(after - before, std::memory_order_relaxed);
}

// One call per batch the draw loop actually emits.
//
// The batches EXIST -- 2508 of them in mx_1774 against 2419 culled trees -- so
// the remaining question is which pass they belong to. Tree geometry is visible
// in a capture ONLY inside the eight 129x129 terrain clipmap bakes, and the
// FR_DU_Eco billboard atlas is never uploaded at all, so "the forest draws, but
// only into the clipmap" and "the forest draws into the scene and we lose it"
// are both still live and they need different fixes.
//
// The RENDERER OBJECT (r3) separates them: the clipmap bake and the main scene
// are different render targets driven through different renderer instances, so
// a batch's r3 says which one it fed. The caller (lr) names the loop it came
// from. Both are logged for the first few and then the distinct set is counted,
// which is what matters -- one renderer means one pass.
REX_EXTERN(__imp__sub_823F82D0);
REX_HOOK_RAW(sub_823F82D0) {
  const uint64_t n = g_forestBatches.fetch_add(1, std::memory_order_relaxed) + 1;
  const uint32_t renderer = ctx.r3.u32;
  const uint32_t type = ctx.r4.u32;
  const uint32_t caller = uint32_t(ctx.lr);
  {
    std::lock_guard<std::mutex> lk(g_forestBatchMu);
    if (g_forestBatchSites.size() < 32)
      g_forestBatchSites.insert((uint64_t(renderer) << 32) | caller);
  }
  (void)n;
  (void)type;
  __imp__sub_823F82D0(ctx, base);
}
