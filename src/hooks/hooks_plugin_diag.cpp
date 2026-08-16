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
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

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

// Every string setting the guest reads comes from MXRegistry.bxml through one
// function, sub_825487C8(registry, key, out, size, 0). `registry_override` takes
// comma-separated key=value pairs and substitutes the value for a matching key
// as it is read, which is what shipping a different MXRegistry.bxml would do —
// tools/ has bxml decoders but no encoder, so this is the only way to change one.
// Empty means off. Diagnostic only. See AGENTS.md "the registry chokepoint".
REXCVAR_DEFINE_STRING(registry_override, "", "Debug",
                      "Comma-separated key=value overrides for guest registry string reads");

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
  const float dt = *(float*)&ctx.f1;
  orig_RendererDispatch(ctx, base);
  if (loud) {
    REXLOG_INFO("{}: RendererDispatch #{} a1=0x{:08X} f1={:.2f} -> r3=0x{:08X}", tag,
                rd, a1, dt, ctx.r3.u32);
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
  orig_Timing(ctx, base);
  if ((tm <= 5 || (tm % 1000) == 0) && a1) {
    REXLOG_INFO("{}: Timing #{} dt={:.6f} total={:.3f} a1=0x{:08X}",
                mx::native::g_plugin_mode ? "plugin" : "native", tm,
                std::bit_cast<float>(REX_LOAD_U32(a1 + 24)),
                std::bit_cast<float>(REX_LOAD_U32(a1 + 60)), a1);
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
  static int ed = 0;
  ++ed;
  bool loud = ed <= 5;
  if (loud) REXLOG_INFO("native: sub_82B6D230 #{} ENTER", ed);
  orig_EntityDt(ctx, base);
  if (loud) REXLOG_INFO("native: sub_82B6D230 #{} RETURNED", ed);
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
        // sub_82352AE0 reads its AssetDB from dword_830577C0, the same global our
        // hooks use — confirmed once the `lis r11,-31995` base was computed
        // correctly (0x83050000, not 0x830A0000 as first recorded).
        REXLOG_INFO("native: force_load \"{}\" at call #{} — a1=0x{:08X} "
                    "*(0x830577C0)=0x{:08X} state={}",
                    scene, sm, a1, REX_LOAD_U32(0x830577C0), state_out);

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

// Shared reporting: first few calls in full, then time-limited. A fixed budget
// is what hid the UV measurements — it spends itself on the boot phase and goes
// quiet exactly when the interesting work starts.
void ReportScriptProbe(const char* name, uint32_t a1, uint32_t lr,
                       uint64_t& count) {
  static std::chrono::steady_clock::time_point s_last[8]{};
  const size_t slot = size_t(reinterpret_cast<uintptr_t>(name)) % 8;
  const auto now = std::chrono::steady_clock::now();
  const bool due = (now - s_last[slot]) >= std::chrono::seconds(5);
  if (++count <= 4 || due) {
    if (due) s_last[slot] = now;
    REXLOG_INFO("{}: script {} #{} a1=0x{:08X} from lr=0x{:08X}",
                mx::native::g_plugin_mode ? "plugin" : "native", name, count,
                a1, lr);
  }
}

#define MX_SCRIPT_PROBE(addr, orig, label)                       \
  REX_IMPORT(__imp__##addr, orig, void());                       \
  extern "C" REX_FUNC(addr) {                                    \
    static uint64_t s_count = 0;                                 \
    const uint32_t a1 = ctx.r3.u32;                              \
    const uint32_t lr = uint32_t(ctx.lr);                        \
    ReportScriptProbe(label, a1, lr, s_count);                   \
    orig(ctx, base);                                             \
  }

MX_SCRIPT_PROBE(sub_824AF838, orig_ExecuteScriptAsset, "ExecuteScriptAsset")
MX_SCRIPT_PROBE(sub_824CBF90, orig_LoadUIAssetPackage, "LoadUIAssetPackage")
MX_SCRIPT_PROBE(sub_824CC218, orig_LoadUIAssetDbPackage,
                "LoadUIAssetDatabasePackage")
MX_SCRIPT_PROBE(sub_824D0F18, orig_SwitchToUIWorld, "SwitchToUIWorld")
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

// The 228 (name, func) pairs at 0x8203F2E0 that sub_824F1C98 registers. There is
// a second table of the same shape in .data around 0x82D1B21C, holding the
// VariableCollection bindings, whose bounds have not been established — so a
// miss here means "not in the table scanned", not "not a binding". Log the
// address either way and resolve the rest in IDA.
constexpr uint32_t kBindingTable = 0x8203F2E0;
constexpr uint32_t kBindingCount = 228;

std::string BindingName(uint8_t* base, uint32_t fn) {
  if (!fn) return {};
  for (uint32_t i = 0; i < kBindingCount; ++i) {
    const uint32_t e = kBindingTable + i * 8;
    if (REX_LOAD_U32(e + 4) == fn) return GuestString(base, REX_LOAD_U32(e), 64);
  }
  return {};
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
// 228-entry linear scan runs only on a miss, so at most once per distinct
// binding per run.
//
// The precall fires on at least three guest threads (t11624, t17392, t20020 in
// mx_1196), so the map needs the lock. Do not "optimise" it away.
std::mutex g_bindingCensusMu;
std::map<uint32_t, uint64_t> g_bindingCensus;

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
  std::map<uint32_t, uint64_t> snapshot;
  {
    std::lock_guard<std::mutex> lk(g_bindingCensusMu);
    snapshot = g_bindingCensus;
  }
  REXLOG_INFO("{}: BINDING CENSUS — {} distinct C bindings called", tag,
              snapshot.size());
  for (const auto& [fn, count] : snapshot) {
    const std::string name = BindingName(base, fn);
    REXLOG_INFO("{}:   0x{:08X} x{} {}", tag, fn, count,
                name.empty() ? "(not in the 228-entry table)" : name);
  }
  // State the zeroes. This is the half that a "which bindings fired" list
  // cannot give you, and it is the half the UI_World question needs.
  for (const auto& w : kWatchedBindings) {
    const auto it = snapshot.find(w.addr);
    REXLOG_INFO("{}:   WATCHED {:<28} 0x{:08X} x{}", tag, w.name, w.addr,
                it == snapshot.end() ? 0 : it->second);
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
      if (is_c) cfunc = REX_LOAD_U32(closure + 16);
    }
  }

  // Census first, before the sampler, because this is the part that has to see
  // every call. `first_sight` is what catches a binding that fires once.
  bool first_sight = false;
  if (is_c && cfunc) {
    std::lock_guard<std::mutex> lk(g_bindingCensusMu);
    auto [it, inserted] = g_bindingCensus.try_emplace(cfunc, 0);
    ++it->second;
    first_sight = inserted;
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
  if (first_sight) {
    const std::string name = BindingName(base, cfunc);
    REXLOG_INFO("{}: BINDING FIRST CALL 0x{:08X} {} at dispatch #{} from lr=0x{:08X}",
                tag, cfunc,
                name.empty() ? "(not in the 228-entry table)" : name, n, from);
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
    REXLOG_INFO("{}: vm dispatch #{} {} cfunc=0x{:08X}{}{} from lr=0x{:08X}", tag, n,
                !is_fn ? "non-function" : (is_c ? "C" : "lua"), cfunc,
                name.empty() ? "" : " name=", name, from);
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
