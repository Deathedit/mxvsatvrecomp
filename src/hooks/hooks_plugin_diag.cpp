// DIAGNOSTIC HOOKS — log critical functions that fail in native mode. Most
// only log when --gpu_plugin=xenos (g_plugin_mode=true) and otherwise fall
// straight through to the guest original; sub_8253AA40 logs in both modes.
//
// Note the mid-ASM hooks that skip these call sites are unconditional (see
// midasm_stubs.cpp) — they fire in plugin mode too, so a hook being silent
// means its call site is jumped, not that the mode is wrong.

#include "hooks/hook_common.h"
#include "hooks/hooks_ui_probe.h"
#include "hooks/hooks_d3d9.h"  // GuestDrawCalls

#include <rex/cvar.h>

// Defined here, declared in hooks_ui_probe.h: NoteVideoComponent below
// writes it and the probe file reads it.
namespace mx::hooks {
std::atomic<uint32_t> g_videoCompFast[16] = {};
}  // namespace mx::hooks

using namespace mx::hooks;

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
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
// already paid once for treating a plausible-looking value as a pointer --
// see the PlausibleGuestPtr note on the video probe.
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
//=============================================================================
// 0x82BA91C0 -- AssetManager::Find(type, name), resolved at RUNTIME.
//
// This is assetMgr->vt[0x78], the lookup BinkVideoComponent_InitAndOpen uses to
// turn the movie name into an asset, and whose NULL return is stored to
// this+0x94 and then dereferenced at +0x58 by AcquirePlayer -- the 0x8234CE20
// crash.
//
// The address had to come from the running game. Reading the IDB at
// off_8214518C+0x78 gave 0x82BA8D40, which is the middle of sub_82BA8D08; the
// live vtable holds 0x82BA91C0 there, and that same value appears as `ctr` in
// the crash register dump, which is the independent confirmation. Static reads
// of that table are not to be trusted.
//
// The function has TWO failure exits and they mean different things:
//
//     if (!sub_82BA8EE0(this + 1504, &type, out)) return 0;   // no such TYPE
//     RtlEnterCriticalSection(...);
//     if (!sub_82B099C0(&bucket[1], &name, out)) { ... return 0; }  // no NAME
//     return out[0];
//
// So a miss is either "the 'bink' type bucket does not exist" or "the bucket
// exists and RiderUI_Final_C_350 is not in it". The first says the package's
// contents were never registered; the second says they were registered under a
// different key. AssetDB_LoadPackage already reports SUCCESS for
// db="UIAnimations" package="Rider", so one of those two is happening anyway.
//
// ONLY FAILURES ARE LOGGED, deduped by (type, name). This runs for every asset
// lookup in the game and a per-call line would be a flood; a miss is rare and
// is the entire signal. The type is 8 chars packed into one 64-bit register,
// big-endian, so "bink" sits in the HIGH half ("texture\0" fills both).
//=============================================================================
// Which of AssetManager::Find's two exits returns 0 for the bink lookup.
//
// Find does:
//     if (!sub_82BA8EE0(this + 1504, ...)) return 0;      // FIRST  lookup
//     RtlEnterCriticalSection(...);
//     if (!sub_82B099C0(...))  { ... return 0; }          // SECOND lookup
//     return out[0];
//
// One is the type bucket and one is the name within it. The decompiler's
// argument mapping for this function is NOT reliable -- it renders the name and
// the 64-bit type as a2/a13/a14 in a way that contradicts the call sites, and
// only the runtime r4=name / r5=type reading is confirmed (the probe prints
// "bink", "material", "uicmpnt" and sensible names from it). So rather than
// argue about which inner call is which, watch the FIRST one and let the result
// say: if it fails, the miss happens before the critical section and nothing
// about the name matters; if it succeeds, the miss is in the second lookup.
//
// Scoped by a thread-local so this costs a bool test on every other asset
// lookup in the game -- sub_82BA8EE0 is generic and runs constantly.
namespace {
thread_local bool t_inBinkFind = false;
}

REX_IMPORT(__imp__sub_82BA8EE0, orig_AssetFindFirst, void());
extern "C" REX_FUNC(sub_82BA8EE0) {
  orig_AssetFindFirst(ctx, base);
  if (!t_inBinkFind) return;
  static uint64_t s_n = 0;
  if (++s_n <= 4)
    REXLOG_INFO("{}: AssetFind[bink] first lookup (sub_82BA8EE0) -> {}",
                mx::native::g_plugin_mode ? "plugin" : "native",
                ctx.r3.u32 ? "HIT -- so the miss is the SECOND lookup"
                           : "MISS -- the miss is the FIRST lookup");
}

REX_IMPORT(__imp__sub_82BA91C0, orig_AssetFind, void());
extern "C" REX_FUNC(sub_82BA91C0) {
  const uint32_t name_ptr = ctx.r4.u32;
  const uint64_t type_bits = ctx.r5.u64;
  // Big-endian: "bink" occupies the high half, low half zero.
  const bool is_bink = (type_bits >> 32) == 0x62696E6Bull;
  t_inBinkFind = is_bink;
  orig_AssetFind(ctx, base);
  t_inBinkFind = false;
  const uint32_t found = ctx.r3.u32;

  char type[9] = {};
  for (int i = 0; i < 8; ++i) {
    const char c = char((type_bits >> (56 - i * 8)) & 0xFF);
    type[i] = (c >= 0x20 && c < 0x7F) ? c : '\0';
  }
  const std::string name = GuestString(base, name_ptr, 128);
  const char* tag = mx::native::g_plugin_mode ? "plugin" : "native";

  // EVERY 'bink' LOOKUP, hit or miss, because the two readings of a miss want
  // opposite fixes and only the hits separate them:
  //
  //   no bink lookup EVER succeeds -> the 'bink' TYPE BUCKET is missing or
  //     empty, so the package's contents were never registered under it, and
  //     the first exit of AssetManager::Find is what returns 0;
  //   some succeed and RiderUI_Final_C_350 does not -> the bucket is fine and
  //     this one name is absent or keyed differently.
  //
  // Logging only misses cannot tell those apart -- it has no denominator. The
  // population is tiny (three bink assets exist in the whole game:
  // RiderUI_Final_C_350, and RiderCloth_60fms_FNL in NAT_Farm and ST_Farm), so
  // this is uncapped and unthrottled on purpose.
  if (std::strcmp(type, "bink") == 0) {
    REXLOG_INFO("{}: AssetFind bink \"{}\" -> {}", tag, name,
                found ? "FOUND" : "MISS");
    if (found) return;
  }
  if (found) return;  // a hit of any other type says nothing

  static std::mutex s_mu;
  static std::set<std::string> s_seen;
  const std::string key = std::string(type) + "|" + name;
  {
    std::lock_guard<std::mutex> lk(s_mu);
    if (s_seen.size() >= 64 || !s_seen.insert(key).second) return;
  }
  REXLOG_INFO("{}: AssetFind MISS type=\"{}\" name=\"{}\" (raw type 0x{:016X})",
              tag, type, name, type_bits);
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

//-----------------------------------------------------------------------------
// Defined with the render probes below; declared here because the
// SetTextureAsset hook is what discovers the video components in the first
// place, and it comes first in the file.
void NoteVideoComponent(uint32_t component, const std::string& video,
                        uint32_t texture_asset, uint32_t player);

// UIVideoLayer::SetTextureAsset — sub_8236EB30
//
// The missing link in the menu backdrop. A UIVideoLayer with a `TextureAsset`
// property does NOT draw to the screen: it renders into a named texture and
// something else samples that texture. This function is where that texture is
// created and attached, from the property loader at sub_823911C8:
//
//     layer+664  <- renderer_vtable[120](asset, 0x756B6500, 1)   the texture
//     player+140 <- layer+664                                    render target
//
// Both player-creation sites in sub_8237F320 pass layer+664 into the factory,
// so that object IS the video's output surface — the one a consumer draw would
// bind. Our resolve tracking keys on the RESOLVE's destination object instead.
// If the two are different objects we track one and the guest binds the other,
// which is precisely the shape of the defect: FE_Smoke is produced every frame,
// resolved to a 1280x430 texture, and that texture is sampled by nothing.
//
// Mode-neutral on purpose. A plugin-only probe here would be a one-sided
// measurement, and this project has read the resulting silence as evidence
// before.
REX_IMPORT(__imp__sub_8236EB30, orig_UIVideoSetTextureAsset, void());
extern "C" REX_FUNC(sub_8236EB30) {
  const uint32_t layer = ctx.r3.u32;
  const uint32_t asset = ctx.r4.u32;
  orig_UIVideoSetTextureAsset(ctx, base);

  // Read AFTER the original runs: layer+664 is assigned inside it, and the
  // whole point is which object it ended up holding.
  constexpr uint32_t kLayerVideoName = 608;  // the "Video" property, 48 bytes
  constexpr uint32_t kLayerTexture = 664;    // <- the output surface
  constexpr uint32_t kLayerPlayer = 668;
  // GUARDED, after an access violation here on 2026-08-19: reading at guest
  // 0x3E4F96FC, which is exactly the `texture + 0x1C` below for a texture of
  // 0x3E4F96E0. That run reached further into the menu than any before it
  // (FE_RiderSelect, FE_SplitscreenSetup, POP_ProfileSelection) and found a
  // UIVideoLayer whose +664 is not a live object. `layer` is a guest `this` and
  // has always been sound; +664 is whatever the property loader left there, and
  // this probe reads six dwords off it, so it needs checking before the read.
  //
  // Refusals are COUNTED, not silently skipped: a probe that quietly reads
  // nothing looks exactly like a probe whose subject never appears, and that
  // confusion has cost this project real time.
  if (!PlausibleGuestPtr(layer)) return;
  // PLAUSIBLE IS NOT READABLE, and the gap between them was killing the game.
  //
  // PlausibleGuestPtr is `p >= 0x10000000 && (p & 3) == 0` -- a RANGE test, not
  // a mapping test. The guard above was already written after one access
  // violation here, and it does not do the job: runs 1594, 1618, 1625, 1629,
  // 1630, 1632, 1638 and 1639 all died reading guest 0x4C69746C, which is the
  // ASCII "Litl" -- a fragment of a STRING that satisfies both halves of the
  // test and is then dereferenced. layer+664 is whatever the property loader
  // left there, so on some UI layers it is text, not an object.
  //
  // GuestRangeReadable probes the real mapping at BOTH ends of the span, so a
  // range straddling an unmapped page is not admitted on the strength of its
  // first byte. This hook logs and calls NoteVideoComponent; it has no business
  // taking the process down, whatever the guest left in that field.
  //
  // One span covers everything read off `layer`: the 48-byte name at +608
  // through the player pointer ending at +672.
  if (!GuestRangeReadable(base, layer + kLayerVideoName,
                          (kLayerPlayer + 4u) - kLayerVideoName)) {
    static std::atomic<uint64_t> s_badLayer{0};
    const uint64_t n = s_badLayer.fetch_add(1, std::memory_order_relaxed) + 1;
    // Counted, and said out loud the first few times: a probe that silently
    // reads nothing looks exactly like a probe whose subject never appears.
    if (n <= 4)
      REXLOG_INFO(
          "native: UIVideoLayer::SetTextureAsset layer 0x{:08X} -- +{}..+{} is "
          "not readable guest memory, skipped ({} so far)",
          layer, kLayerVideoName, kLayerPlayer + 4u, n);
    return;
  }
  const uint32_t texture = REX_LOAD_U32(layer + kLayerTexture);
  const uint32_t player = REX_LOAD_U32(layer + kLayerPlayer);
  std::string name;
  for (uint32_t i = 0; i < 48; ++i) {
    const char c = char(REX_LOAD_U8(layer + kLayerVideoName + i));
    if (!c) break;
    name.push_back(c);
  }
  // Is layer+664 a D3D9 TEXTURE, or an asset wrapper holding one?
  //
  // Settled here rather than by decompiling renderer_vtable[120]: that call is
  // resolved at runtime through dword_830BE400+8 and the global has 40+ xrefs,
  // so the static route would be a guess about which store feeds slot 8. A
  // texture object carries its six hardware fetch dwords at +0x1C -- the offsets
  // D3DDevice_SetTexture copies from -- and word 0 has type in bits [1:0]
  // (2 = texture) with base_address in [31:12]. So the object answers for
  // itself, and if it IS a texture its guest address can be compared directly
  // against the 0xFBE94000 the FE_Smoke resolve lands on.
  std::string fetch_desc = " (texture null)";
  if (texture && !PlausibleGuestPtr(texture)) {
    static std::atomic<uint64_t> s_badTexture{0};
    fetch_desc = fmt::format(" (texture 0x{:08X} NOT a plausible guest pointer, "
                             "not dereferenced; {} so far)",
                             texture,
                             s_badTexture.fetch_add(1, std::memory_order_relaxed) + 1);
  } else if (texture && !GuestRangeReadable(base, texture + 0x1Cu, 6u * 4u)) {
    // The same distinction one level down. PlausibleGuestPtr passed this value
    // -- that is exactly how "Litl" got dereferenced -- so the six fetch dwords
    // need the mapping checked, not the range.
    static std::atomic<uint64_t> s_unreadableTexture{0};
    fetch_desc = fmt::format(
        " (texture 0x{:08X} plausible but +0x1C..+0x34 is not readable, not "
        "dereferenced; {} so far)",
        texture,
        s_unreadableTexture.fetch_add(1, std::memory_order_relaxed) + 1);
  } else if (texture) {
    uint32_t fetch[6] = {};
    for (uint32_t i = 0; i < 6; ++i)
      fetch[i] = REX_LOAD_U32(texture + 0x1C + i * 4);
    const uint32_t type = fetch[0] & 3u;
    fetch_desc = fmt::format(
        " fetch0=0x{:08X} type={} {} addr=0x{:08X}"
        " [{:08X} {:08X} {:08X} {:08X} {:08X} {:08X}]",
        fetch[0], type,
        type == 2u ? "IS-A-TEXTURE" : "not a texture fetch",
        fetch[0] & 0xFFFFF000u, fetch[0], fetch[1], fetch[2], fetch[3],
        fetch[4], fetch[5]);
  }
  REXLOG_INFO("native: UIVideoLayer::SetTextureAsset layer 0x{:08X} video "
              "\"{}\" asset 0x{:08X} -> TEXTURE 0x{:08X}, player 0x{:08X};{}",
              layer, name, asset, texture, player, fetch_desc);
  NoteVideoComponent(layer, name, texture, player);
}

//-----------------------------------------------------------------------------
// What does the FE_Smoke QUAD actually carry?
//
// Both video render targets are produced, bound only for the resolve idiom and
// sampled by nothing -- measured, see VIDEO TARGET CONSUMPTION. So the question
// is no longer which host texture we lose, but what the guest's own draw item
// holds. UIVideoComponent, decompiled 2026-08-17:
//
//   this+260  { material, pack, index }   <- what the quad samples
//   this+236  -> ImageIconProperties (this+320); +164 is the installed material
//   this+664  <TextureAsset>              <- where the video decodes
//
//   sub_82388560(this+260, name)   resolves a NAME to a material. The MATERIAL
//                                  branch always fails for these (no material
//                                  named *VideoRenderTarget exists in any of
//                                  the game's 130 databases), so it wraps the
//                                  TEXTURE of that name in a synthesised one.
//   sub_8237ABA8(this)             vtable slot 15, the render prepare:
//                                    sub_82B26778(*(this+236), *(this+260))
//   sub_82B26778(props, material)  installs it at props+164.
//
// Three probes, because each answers a different question and the previous
// round of this investigation was lost to conflating them:
//
//   sub_82388560   which NAME each material handle came from. Without it every
//                  handle below is an unnamed pointer and we are back to
//                  identifying resources by resemblance.
//   sub_8237ABA8   is the video component's render path running AT ALL, and
//                  what does it hold when it runs. `0 calls` and `calls but
//                  the wrong material` are opposite diagnoses.
//   sub_82B26778   whether the material actually reaches the draw item. NOTE:
//                  guarded by `props[41] != material`, so it fires ONLY on
//                  CHANGE -- a low call count here means "stable", NOT "not
//                  drawing". Do not read it as a per-frame counter.
//-----------------------------------------------------------------------------

namespace {

constexpr uint32_t kCompMaterialSlot = 260;
constexpr uint32_t kCompTextureAsset = 664;
// The three fields slot 13 (sub_8236EBB0) actually branches on:
//
//     v2 = this[167];                      // +668 the video player object
//     if (v2) (*(v2->vtbl + 24))(v2);      // begin
//     v3 = this[48];                       // +192 THE SUBMITTER
//     if (v3) {
//         (*(v3->vtbl + 12))(v3, this[60], 0);   // submit the item at +240
//         this[60] = 0;                          // consumed
//     }
//
// So a null +192 means the draw is never submitted no matter how healthy
// everything upstream is -- and upstream is now proven healthy: the
// component is visited, visible, enqueued and drained 1:1 (UI ENQUEUE /
// UI DRAIN). +240 is refilled EVERY frame by sub_8237AB78 (a bump
// allocation from the pool dword_82DD8084), so a zero there would mean the
// allocation step never ran, not that it was consumed once.
// The sort key the emit computes for every UI draw:
//
//     sub_82B26860(*(short*)(item + 248), ...):
//         key = table[idx] * 10000.0 - seq++
//
// where `table` is the float array at 0x830C0000. A layer index that sorts
// the intro's items outside whatever range actually gets drawn would produce
// exactly the observed symptom: every gate passes, the item is queued, and
// no draw comes out.
constexpr uint32_t kItemLayerIndex = 248;
constexpr uint32_t kLayerPriorityTable = 0x830C0000;
// A layer index is a small enum. Anything larger is a misread, and reading
// the table at a wild offset would be a wild guest load - so it is bounded
// and the out-of-range case is COUNTED rather than silently clamped.
constexpr uint32_t kMaxSaneLayerIndex = 256;
constexpr uint32_t kCompSubmitter = 192;
constexpr uint32_t kCompPendingItem = 240;
constexpr uint32_t kCompPlayerObj = 668;

struct VideoComponentProbe {
  uint32_t component = 0;
  std::string video;
  uint32_t texture_asset = 0;
  uint32_t player = 0;
  uint64_t render_calls = 0;
  uint32_t last_material = 0;
  uint32_t last_draw_item = 0;
  // Who called the render prepare. Slot 15 is reached through a virtual call
  // and static hunting for it failed: the `lwz rD, 0x3C(rA)` encodings produce
  // 40+ candidate sites and not one of them is in the UI range, so the caller
  // is not findable by pattern. The link register names it outright.
  uint32_t first_caller = 0;
  uint32_t last_caller = 0;
  // The per-frame step that decides whether the quad rebuilds and draws.
  //
  //   sub_8236DB10(this)   if (this[+176] & 0xC0000000) -> slot 15 rebuild
  //   sub_8236DCA8(this)   return (this[+172] >> 4) & 1  -> then submit
  //
  // Two INDEPENDENT gates, so `slot 15 never ran` has three causes and these
  // separate them:
  //   visit_calls == 0     the component is never traversed at all -- it is
  //                        not in the active layer, and the question moves up
  //                        to whoever walks the tree.
  //   dirty_seen == 0      it is traversed but never marked dirty, so the
  //                        quad is never (re)built.
  //   visible_seen == 0    it is traversed but bit 4 of +172 is clear, so
  //                        nothing is ever submitted.
  uint64_t visit_calls = 0;
  uint64_t dirty_seen = 0;
  uint64_t visible_seen = 0;
  uint32_t last_flags172 = 0;
  uint32_t last_flags176 = 0;
  // Sampled in the per-frame visit. That hook runs at the TOP of the draw
  // step (sub_8237A6D0 / sub_8237B1D0), which sub_8237AB78 tail-calls right
  // after allocating the frame's draw item into +240 -- so at this point
  // +240 is freshly allocated and slot 13 has not yet consumed it. Both
  // ends of its lifetime are therefore visible here.
  //
  // Each carries a non-null COUNT, not just the last value: a single
  // last-seen pointer cannot distinguish "always null" from "null on the
  // frame we happened to sample", and that distinction is the whole point.
  uint32_t last_submitter = 0;     // +192
  uint32_t last_pending_item = 0;  // +240
  uint32_t last_player_obj = 0;    // +668
  uint64_t submitter_nonnull = 0;
  uint64_t pending_nonnull = 0;
  uint64_t player_nonnull = 0;
  uint64_t draw_item_nonnull = 0;  // +236, set by slot 15
  // The submit in slot 13 is a VIRTUAL call on the object at +192:
  //
  //     (*(v3->vtbl + 12))(v3, this[240], 0);   // slot 3 -- the submit
  //     (*(v3->vtbl +  4))(v3, *(UIManager+560));// slot 1 -- the context
  //
  // Its target cannot be read off the vtable statically because the object
  // is chosen at run time, but it is two guest loads once the pointer is in
  // hand. Resolving it here is the same move that named the drain from the
  // link register after a static hunt failed -- let it name itself.
  uint32_t last_submitter_vtbl = 0;
  uint32_t last_submit_fn = 0;   // vtbl + 12
  uint32_t last_slot1_fn = 0;    // vtbl + 4
  // THE LAST GATE. Slot 1 (sub_82B268A8) emits a draw only for an item whose
  // +212 has a non-zero TOP BYTE:
  //
  //     v4 = *(this + 104);                      // the installed item
  //     if ((*(v4 + 212) & 0xFF000000) != 0) { ... emit ... }
  //     return;                                  // otherwise silently nothing
  //
  // Read off the PERSISTENT item at +236, not the per-frame one at +240: the
  // image draw step (sub_8237A6D0) copies +236 into +240 via the item's own
  // vtable slot 2, and that copy happens AFTER this hook runs. +236 is the
  // stable source and is already populated by slot 15.
  uint32_t last_item212 = 0;
  uint64_t item212_top_nonzero = 0;
  uint32_t last_layer248 = 0;       // *(uint16*)(item236 + 248)
  uint32_t last_layer_prio = 0;     // raw bits of table[last_layer248]
  uint64_t layer_out_of_range = 0;
};

std::mutex g_videoProbeMu;
std::vector<VideoComponentProbe> g_videoProbes;   // 6 in the whole game
// The material name table has its OWN mutex, not the probe mutex, because two
// unrelated collectors read it: the video report (under g_videoProbeMu) and the
// UI inventory (under g_uiMu). Lock order is always <collector> -> g_materialMu
// and nothing takes a collector lock while holding this one, so there is no
// cycle. Sharing g_videoProbeMu for it would have made every UI-inventory read
// a data race.
std::mutex g_materialMu;
std::map<uint32_t, std::string> g_materialNames;  // handle -> resolved name
std::atomic<uint64_t> g_materialResolves{0};


// Linear scan: the population is six. Caller holds the lock.
VideoComponentProbe* FindProbe(uint32_t component) {
  for (auto& p : g_videoProbes)
    if (p.component == component) return &p;
  return nullptr;
}

std::string MaterialLabel(uint32_t handle) {
  if (!handle) return "none";
  std::lock_guard<std::mutex> lk(g_materialMu);
  const auto it = g_materialNames.find(handle);
  return it == g_materialNames.end()
             ? fmt::format("0x{:08X} <unnamed>", handle)
             : fmt::format("0x{:08X} \"{}\"", handle, it->second);
}

// The whole population, every time. Caller holds g_videoProbeMu.
//
// Throttled on TIME and on the population changing -- NOT on a render count.
// The first cut fired only at `total == 1 || total % 600 == 0`, which printed
// exactly once, at a moment when two of the six components had registered and
// FE_Smoke had not. Its row never appeared at all, and an absent row is
// indistinguishable from `renders 0`, which is the entire question. A counter
// whose reporting condition can be skipped over is the same defect as one that
// cannot fire.
void ReportVideoComponents(bool force) {
  static std::chrono::steady_clock::time_point s_last{};
  static size_t s_lastCount = 0;
  const auto now = std::chrono::steady_clock::now();
  const bool grew = g_videoProbes.size() != s_lastCount;
  if (!force && !grew &&
      now - s_last < std::chrono::seconds(3))
    return;
  s_last = now;
  s_lastCount = g_videoProbes.size();
  uint64_t total = 0;
  std::string rows;
  for (const auto& q : g_videoProbes) {
    total += q.render_calls;
    rows += fmt::format(
        " [\"{}\" comp0x{:08X} renders{} visits{} dirty{} visible{} "
        "f172=0x{:08X} f176=0x{:08X} material={} item0x{:08X} "
        "texasset0x{:08X} caller0x{:08X} submitter192=0x{:08X}(n{}) "
        "pend240=0x{:08X}(n{}) player668=0x{:08X}(n{}) item236n{} "
        "subvt=0x{:08X} submitFn=0x{:08X} slot1Fn=0x{:08X} "
        "item212=0x{:08X}(topNZ n{}) layer248={} prio=0x{:08X}({:g}) oor{}]",
        q.video, q.component, q.render_calls, q.visit_calls, q.dirty_seen,
        q.visible_seen, q.last_flags172, q.last_flags176,
        MaterialLabel(q.last_material), q.last_draw_item, q.texture_asset,
        q.last_caller, q.last_submitter, q.submitter_nonnull,
        q.last_pending_item, q.pending_nonnull, q.last_player_obj,
        q.player_nonnull, q.draw_item_nonnull, q.last_submitter_vtbl,
        q.last_submit_fn, q.last_slot1_fn, q.last_item212,
        q.item212_top_nonzero, q.last_layer248, q.last_layer_prio,
        std::bit_cast<float>(q.last_layer_prio), q.layer_out_of_range);
  }
  REXLOG_INFO("native: VIDEO COMPONENT RENDER {} components, {} renders "
              "total, {} material resolves --{}",
              g_videoProbes.size(), total, g_materialResolves.load(),
              rows.empty() ? " (none)" : rows);
}

// ---- The full UI inventory -------------------------------------------------

struct UiComponentRow {
  std::string name;       // read once, from +16
  uint64_t visits = 0;
  uint64_t visible = 0;
  uint32_t material = 0;
  uint32_t flags172 = 0;
  // The class, from the vtable pointer at +0.
  //
  // Needed because +260 (material) and +172 (flags) belong to the IMAGE
  // component family and are NOT valid for every UIComponent subclass. The
  // first inventory printed rows like `mat=0x64547269 f172=0x4672655D` -- those
  // are ASCII ("dTri", "Fre]"), i.e. some other member read through the wrong
  // layout. The names at +16 are all sensible, so +16 is right and these two
  // are class-specific. Logging the vtable makes the row self-describing
  // instead of quietly inviting the same misreading the material handles did.
  uint32_t vtable = 0;
  // The draw item's layer index, for components that have a draw item at
  // +236 at all. Recorded here as well as on the video rows because a bare
  // layer number is uninterpretable: the question is whether the intro's
  // components sort differently from the MENU's, which do produce draws.
  uint32_t item = 0;
  uint32_t layer248 = 0;
  bool has_layer = false;
};

std::mutex g_uiMu;
std::map<uint32_t, UiComponentRow> g_uiRows;
uint64_t g_uiVisits = 0;
uint64_t g_uiDropped = 0;

constexpr size_t kMaxUiRows = 768;

// The component's own <Name>, at +16.
//
// The offset comes from sub_8237F320, which passes `a1 + 4` (dwords, so byte
// 16) into the video-player factory alongside `a1 + 152` (byte 608), and 608 is
// confirmed to be the <Video> path by the SetTextureAsset hook. It is read
// through a printable-ASCII filter and reported as `name?` when it does not
// look like a string, so a wrong offset shows up as visibly empty rather than
// as plausible garbage.
std::string ReadComponentName(uint32_t component, uint8_t* base) {
  std::string out;
  for (uint32_t i = 0; i < 48; ++i) {
    const uint8_t c = REX_LOAD_U8(component + 16 + i);
    if (!c) break;
    if (c < 0x20 || c > 0x7E) return std::string();
    out.push_back(char(c));
  }
  return out;
}

}  // namespace

void NoteUiComponentVisit(uint32_t component, uint32_t flags172,
                          uint32_t flags176, uint8_t* base) {
  if (!component) return;
  const uint32_t material = REX_LOAD_U32(component + kCompMaterialSlot);
  std::lock_guard<std::mutex> lk(g_uiMu);
  ++g_uiVisits;
  auto it = g_uiRows.find(component);
  if (it == g_uiRows.end()) {
    if (g_uiRows.size() >= kMaxUiRows) { ++g_uiDropped; return; }
    it = g_uiRows.emplace(component, UiComponentRow{}).first;
    it->second.name = ReadComponentName(component, base);
    it->second.vtable = REX_LOAD_U32(component);
  }
  UiComponentRow& r = it->second;
  ++r.visits;
  if ((flags172 >> 4) & 1u) ++r.visible;
  r.material = material;
  r.flags172 = flags172;
  // The draw item and its layer index, for whatever carries one. This is
  // the comparison population for the video component's own layer248: a
  // bare layer number says nothing, but the MENU's components DO produce
  // draws, so their values are the reference.
  //
  // Guarded, not assumed: +236 is an IMAGE-family field like +260 and +172,
  // and reading it on a class that has no draw item is exactly the misread
  // that produced the ASCII "dTri"/"Fre]" material handles. has_layer says
  // whether the row's value is worth anything at all.
  r.item = REX_LOAD_U32(component + kCompDrawItem);
  if (PlausibleGuestPtr(r.item)) {
    r.layer248 = REX_LOAD_U16(r.item + kItemLayerIndex);
    r.has_layer = true;
  }

  // Report the inventory every 5s, ordered by visits. Bounded to the top rows
  // because a menu walks a few hundred components; the whole count and the
  // dropped count are printed so a truncated list is never mistaken for the
  // whole population.
  static std::chrono::steady_clock::time_point s_last{};
  const auto now = std::chrono::steady_clock::now();
  if (now - s_last < std::chrono::seconds(5)) return;
  s_last = now;
  std::vector<const std::pair<const uint32_t, UiComponentRow>*> order;
  order.reserve(g_uiRows.size());
  for (const auto& kv : g_uiRows) order.push_back(&kv);
  std::sort(order.begin(), order.end(), [](auto* a, auto* b) {
    return a->second.visits > b->second.visits;
  });
  std::string rows;
  size_t shown = 0, drawn = 0;
  for (const auto* kv : order) {
    if (kv->second.visible) ++drawn;
    if (shown >= 48) continue;
    ++shown;
    rows += fmt::format(" [{}0x{:08X} vt0x{:08X} visits{} mat?={} f172?=0x{:08X}"
                        " item0x{:08X} layer{}]",
                        kv->second.name.empty()
                            ? std::string("name? ")
                            : ("\"" + kv->second.name + "\" "),
                        kv->first, kv->second.vtable, kv->second.visits,
                        MaterialLabel(kv->second.material), kv->second.flags172,
                        kv->second.item,
                        kv->second.has_layer
                            ? std::to_string(kv->second.layer248)
                            : std::string("?"));
  }
  // mat? and f172? carry question marks because they are only valid for the
  // IMAGE component family; group by vt (the class vtable) before believing
  // them. `drawn` is derived from f172 and inherits the same doubt, so it is
  // labelled rather than presented as a count of what draws.
  REXLOG_INFO("native: UI INVENTORY {} components, {} visits, {} dropped "
              "(cap {}); showing {} by visits; {} rows had f172 bit4 set "
              "(only meaningful for image-family vt) --{}",
              g_uiRows.size(), g_uiVisits, g_uiDropped, kMaxUiRows, shown, drawn,
              rows.empty() ? " (none)" : rows);
}

void NoteVideoComponent(uint32_t component, const std::string& video,
                        uint32_t texture_asset, uint32_t player) {
  if (!component) return;
  std::lock_guard<std::mutex> lk(g_videoProbeMu);
  if (VideoComponentProbe* p = FindProbe(component)) {
    p->video = video;
    p->texture_asset = texture_asset;
    p->player = player;
    return;
  }
  if (g_videoProbes.size() >= 16) return;
  g_videoCompFast[g_videoProbes.size()].store(component,
                                              std::memory_order_release);
  g_videoProbes.push_back({component, video, texture_asset, player});
  // Report on registration too, so a component that NEVER renders still gets a
  // row. Without this the only reporter is the render hook, and a component
  // that never renders is exactly the one that would never be printed.
  ReportVideoComponents(true);
}

// sub_82388560(slot, name) — the material-name resolver.
REX_IMPORT(__imp__sub_82388560, orig_ResolveMaterialByName, void());
extern "C" REX_FUNC(sub_82388560) {
  const uint32_t slot = ctx.r3.u32;
  const uint32_t name_ptr = ctx.r4.u32;
  std::string name;
  for (uint32_t i = 0; i < 64 && name_ptr; ++i) {
    const char c = char(REX_LOAD_U8(name_ptr + i));
    if (!c) break;
    name.push_back(c);
  }
  orig_ResolveMaterialByName(ctx, base);
  // The slot is written by the original; slot[0] is the material it chose.
  const uint32_t material = slot ? REX_LOAD_U32(slot) : 0;
  std::lock_guard<std::mutex> lk(g_materialMu);
  ++g_materialResolves;
  if (!material || name.empty()) return;
  // The name table is what makes every material handle downstream readable, so
  // it records EVERY handle regardless of logging.
  if (g_materialNames.size() < 4096) g_materialNames.emplace(material, name);
  if (name.find("VideoRenderTarget") == std::string::npos) return;
  // Deduped on (SLOT, material), not on the handle. The handle is shared: the
  // asset system caches materials by name, so both video components resolve
  // "1280_720_VideoRenderTarget" to the same 0x21B0D720 and a handle-keyed
  // dedupe printed one line for the whole run -- hiding WHICH components
  // resolved it, which is the part worth knowing. The slot is component+260,
  // so each line names a distinct component.
  static std::set<uint64_t> s_seen;
  if (s_seen.size() < 64 && s_seen.insert((uint64_t(slot) << 32) | material).second) {
    REXLOG_INFO("native: MATERIAL RESOLVE \"{}\" -> material 0x{:08X} "
                "(slot 0x{:08X} = component 0x{:08X}); {} resolves so far",
                name, material, slot, slot - kCompMaterialSlot,
                g_materialResolves.load());
  }
}

// sub_82B26778(props, material) — installs the material on the draw item.
// Fires only when it CHANGES; see the header note.
REX_IMPORT(__imp__sub_82B26778, orig_InstallDrawItemMaterial, void());
extern "C" REX_FUNC(sub_82B26778) {
  const uint32_t props = ctx.r3.u32;
  const uint32_t material = ctx.r4.u32;
  orig_InstallDrawItemMaterial(ctx, base);
  static std::mutex s_mu;
  static std::set<uint64_t> s_seen;
  static uint64_t s_calls = 0, s_dropped = 0;
  std::string label;
  {
    std::lock_guard<std::mutex> lk(g_materialMu);
    ++s_calls;
    // Only pairs involving a *VideoRenderTarget material. Everything else is
    // the rest of the UI and would bury it.
    const auto it = g_materialNames.find(material);
    if (it == g_materialNames.end() ||
        it->second.find("VideoRenderTarget") == std::string::npos)
      return;
    label = it->second;
  }
  const uint64_t key = (uint64_t(props) << 32) | material;
  {
    std::lock_guard<std::mutex> lk(s_mu);
    if (s_seen.size() >= 64) { ++s_dropped; return; }
    if (!s_seen.insert(key).second) return;
  }
  REXLOG_INFO("native: DRAW ITEM MATERIAL props 0x{:08X} <- material 0x{:08X} "
              "\"{}\"; {} installs total, {} pairs dropped",
              props, material, label, s_calls, s_dropped);
}

// sub_8236DB10(this) — the per-frame visit, called at the top of the component
// draw step (sub_8237A6D0 / sub_8237B1D0) before anything is submitted:
//
//     if (this[+176] & 0xC0000000) { slot15(this); this[+176] &= 0x3FFFFFFF; }
//     this[+176] &= 0xCFFFFFFF;
//
// Runs for every UI component every frame, so it is prefiltered without a lock
// against the five registered video components.
REX_IMPORT(__imp__sub_8236DB10, orig_UIComponentVisit, void());
extern "C" REX_FUNC(sub_8236DB10) {
  const uint32_t self = ctx.r3.u32;
  uint32_t flags172 = 0, flags176 = 0;
  bool watched = false;
  if (self) {
    for (auto& slot : g_videoCompFast) {
      const uint32_t c = slot.load(std::memory_order_acquire);
      if (!c) break;
      if (c == self) { watched = true; break; }
    }
  }
  // Read BEFORE the original: it CLEARS both flag groups on the way out, so
  // sampling afterwards would report every component as clean and the dirty
  // counter could never fire.
  if (self) {
    flags172 = REX_LOAD_U32(self + 172);
    flags176 = REX_LOAD_U32(self + 176);
  }
  // The FULL UI inventory, not just the video components.
  //
  // "What actually paints the menu backdrop" has now been answered wrongly four
  // times by inference, and the direct probe is unavailable: pixel history on a
  // plugin capture cannot work, because Xenia renders into an emulated EDRAM
  // buffer whose target is 80x8192 rather than 1280x720. So name it from the
  // guest instead -- every component the UI walk actually visits, with the
  // material it carries. The guest submits the same work in both modes, so this
  // inventory is the menu's 2D layer by name.
  NoteUiComponentVisit(self, flags172, flags176, base);
  orig_UIComponentVisit(ctx, base);
  // Before the `watched` early-out and while holding no other lock: the UI
  // ENQUEUE line has to appear even when zero video components are registered
  // and even when the render list never receives a push, because "no pushes"
  // is the answer this probe exists to report.
  ReportUiEnqueue(false);
  ReportUiDrain(false);
  if (!watched) return;
  std::lock_guard<std::mutex> lk(g_videoProbeMu);
  VideoComponentProbe* p = FindProbe(self);
  if (!p) return;
  ++p->visit_calls;
  if (flags176 & 0xC0000000u) ++p->dirty_seen;
  if ((flags172 >> 4) & 1u) ++p->visible_seen;
  p->last_flags172 = flags172;
  p->last_flags176 = flags176;
  // The slot-13 branch fields. Read after the original: it only touches the
  // +176 flag group and the slot-15 rebuild, neither of which owns these.
  p->last_submitter = REX_LOAD_U32(self + kCompSubmitter);
  p->last_pending_item = REX_LOAD_U32(self + kCompPendingItem);
  p->last_player_obj = REX_LOAD_U32(self + kCompPlayerObj);
  const uint32_t item236 = REX_LOAD_U32(self + kCompDrawItem);
  if (p->last_submitter) ++p->submitter_nonnull;
  // Resolve the submit target. Two indirections, both guarded, and only for
  // the handful of watched video components -- not on the UI hot path.
  if (PlausibleGuestPtr(p->last_submitter)) {
    const uint32_t vtbl = REX_LOAD_U32(p->last_submitter);
    p->last_submitter_vtbl = vtbl;
    if (PlausibleGuestPtr(vtbl)) {
      p->last_submit_fn = REX_LOAD_U32(vtbl + 12);
      p->last_slot1_fn = REX_LOAD_U32(vtbl + 4);
    }
  }
  if (p->last_pending_item) ++p->pending_nonnull;
  if (p->last_player_obj) ++p->player_nonnull;
  if (item236) ++p->draw_item_nonnull;
  if (PlausibleGuestPtr(item236)) {
    p->last_item212 = REX_LOAD_U32(item236 + 212);
    if (p->last_item212 & 0xFF000000u) ++p->item212_top_nonzero;
    p->last_layer248 = REX_LOAD_U16(item236 + kItemLayerIndex);
    if (p->last_layer248 < kMaxSaneLayerIndex)
      p->last_layer_prio =
          REX_LOAD_U32(kLayerPriorityTable + p->last_layer248 * 4);
    else
      ++p->layer_out_of_range;
  }
  ReportVideoComponents(false);
}

//-----------------------------------------------------------------------------
// sub_8234D630 -- BinkPlayer::DecodeAndBlitFrame, vtable slot 8. THE GATE.
//
// FE_Smoke.bik decodes, composites and resolves every frame into a texture that
// nothing ever samples. Measured, not inferred: 646 VIDEO COMPONENT RENDER rows
// across 101 logs, every one `renders0 visits0`, in native AND plugin mode. The
// counter is not broken -- the same probe reports THQ_Logo_wSound `visits336`
// and Attract `visits10` -- and the front end really was on screen, because
// FE_Background.layer was visited up to 2250 times in 83 of those runs.
//
// The cause is guest architecture: BinkVideoComponent's constructor calls
// AcquirePlayer and BinkPlayer_Start without ever consulting visibility, so from
// front-end init onward the player runs forever regardless of whether anything
// draws it.
//
// GATED ON TRAVERSAL, NOT ON THE MOVIE NAME. A name test would be a lie about
// what we know: the measurement is "this component is never visited", so that is
// what the gate tests. If the game ever does display FE_Smoke, the gate lifts on
// its own.
//
// WHY THIS SKIPS THE DECODE TOO, with only the blit hooked. The decode thread
// (sub_8234D908) does its work only under `*(player+184) == 1`, and +184 is set
// to 1 in exactly one place: the tail of THIS function. The thread clears it
// itself once it has decoded. So the first gated frame lets at most one more
// decode finish, the thread clears +184, and from then on it parks on its 1ms
// sleep having found nothing to do. One hook stops the whole chain.
//
// RECOVERY IS AUTOMATIC AND COSTS ONE FRAME. Nothing here latches: the predicate
// is re-evaluated on every call. The moment the component is visited we stop
// gating, the blit runs against whatever frame the planes still hold, sets
// +184 = 1, and the decode thread picks straight back up.
//
// HOW THE SKIP IS PERFORMED -- read this before changing it. We do NOT return
// early and we do NOT write the state word ourselves. The guest already has an
// idle path (its `playing && !paused && handle && !frameReady` test), and it
// does three things on the way out that are not ours to imitate: it takes and
// releases the player's critical section, it normalises +136, and it returns
// that value to a caller we cannot see -- sub_8234D630 has exactly ONE xref in
// the binary, a vtable slot at 0x82198B18, so the caller is not findable
// statically and we do not know what it does with the result. So instead of
// reproducing that path we make the guest take it: set the player's own pause
// flag, call the original, restore it. The guest's code decides everything; the
// only thing we contribute is the answer to `is it paused`.
//
// +4 is safe to toggle here. sub_8234D908 never reads it -- it reads +144, +184,
// +124, +132, +108, +112 and nothing else -- so the decode thread cannot observe
// the flicker, and the original holds the critical section for the whole window.
//
// Player field offsets, all from the decompilation of this function:
//   +4 paused   +124 playing   +136 state   +140 TextureAsset
//   +144 handle   +156 CRITICAL_SECTION   +184 frameReady   +192 BinkFrame
//-----------------------------------------------------------------------------
namespace {

constexpr uint32_t kPlayerPaused = 4;
constexpr uint32_t kPlayerTexAsset = 140;

// A player is only gated after it has been asking to blit for this long without
// its component ever being traversed. Components are constructed before the
// screen that shows them, so an instant verdict would gate a video during the
// window between its construction and its first visit. The cost of being wrong
// in this direction is a few stale frames at the start of a video; the cost of
// being wrong in the other is a video that never plays.
constexpr auto kGateGrace = std::chrono::seconds(5);

struct GatedPlayer {
  uint32_t player = 0;
  std::chrono::steady_clock::time_point first_blit{};
  uint64_t skipped = 0;
  bool announced = false;
};

// Guarded by g_videoProbeMu, not a lock of its own: every read of this table is
// already inside the probe lock to look at visit_calls, and one lock cannot be
// mis-ordered against itself.
std::vector<GatedPlayer> g_gatedPlayers;

// Caller holds g_videoProbeMu. The reference dies before the lock does.
GatedPlayer& GateEntryFor(uint32_t player) {
  for (auto& e : g_gatedPlayers)
    if (e.player == player) return e;
  g_gatedPlayers.push_back({player, std::chrono::steady_clock::now(), 0, false});
  return g_gatedPlayers.back();
}

}  // namespace

REX_IMPORT(__imp__sub_8234D630, orig_BinkDecodeAndBlit, int());
extern "C" REX_FUNC(sub_8234D630) {
  // The gate was DISABLED as an experiment on 2026-08-28 and is restored: it
  // is not implicated in the 0x8234CE20 crash. With it fully off (0 "BINK GATE"
  // lines in run 1670) the 'bink' lookup for RiderUI_Final_C_350 still missed
  // and the crash still fired, so it is cleared by measurement, not argument.
  const uint32_t player = ctx.r3.u32;
  if (!PlausibleGuestPtr(player)) {
    orig_BinkDecodeAndBlit(ctx, base);
    return;
  }
  // The player names its own TextureAsset at +140, and that is the SAME object
  // the property loader put at component+664 -- which is what the probe records
  // as texture_asset. So the asset is the join between a player and the
  // component(s) that own it, and it is available without the component ever
  // having been visited. component+668 would look like the more direct route and
  // is not: every last_* field on the probe is sampled inside the visit hook, so
  // on a component with visits0 -- precisely the population this gate is about
  // -- they are all still at their initialised zero. An unsampled field is not a
  // measurement.
  const uint32_t asset = REX_LOAD_U32(player + kPlayerTexAsset);
  bool gate = false;
  if (asset) {
    std::lock_guard<std::mutex> lk(g_videoProbeMu);
    bool attributed = false;
    bool visited = false;
    std::string name;
    // OR over every probe sharing the asset, because sharing is real: the
    // Videos\Attract components all share THQ_Logo's 0x21896260. If ANY sharer
    // is being traversed the target is live and nothing may be skipped.
    // (FE_Smoke's 0x21896920 is private to it, so this costs it nothing.)
    for (const auto& p : g_videoProbes) {
      if (p.texture_asset != asset) continue;
      if (!attributed) name = p.video;
      attributed = true;
      if (p.visit_calls) visited = true;
    }
    // Unattributed means we have no evidence either way -- a player whose
    // component never reached NoteVideoComponent. Never skip what we cannot
    // account for.
    if (attributed && !visited) {
      GatedPlayer& e = GateEntryFor(player);
      if (std::chrono::steady_clock::now() - e.first_blit >= kGateGrace) {
        gate = true;
        ++e.skipped;
        if (!e.announced) {
          e.announced = true;
          REXLOG_INFO(
              "native: BINK GATE \"{}\" player 0x{:08X} asset 0x{:08X} -- no "
              "component using this asset has EVER been visited; skipping "
              "decode + composite + resolve until one is",
              name, player, asset);
        } else if ((e.skipped % 1800) == 0) {
          REXLOG_INFO("native: BINK GATE \"{}\" player 0x{:08X}: {} frames "
                      "skipped so far",
                      name, player, e.skipped);
        }
      }
    }
  }
  if (!gate) {
    orig_BinkDecodeAndBlit(ctx, base);
    return;
  }
  const uint32_t paused = REX_LOAD_U32(player + kPlayerPaused);
  REX_STORE_U32(player + kPlayerPaused, 1);
  orig_BinkDecodeAndBlit(ctx, base);
  REX_STORE_U32(player + kPlayerPaused, paused);
}

// sub_8237ABA8(this) — vtable slot 15, the render prepare that binds
// *(this+260) onto the draw item. Called per render, so this is a COUNTER with
// a periodic report rather than a line per call.
REX_IMPORT(__imp__sub_8237ABA8, orig_UIImageRenderPrepare, void());
extern "C" REX_FUNC(sub_8237ABA8) {
  const uint32_t self = ctx.r3.u32;
  // Read BEFORE the original: it makes calls of its own, and the callee is
  // free to leave whatever it likes in lr by the time control comes back.
  const uint32_t caller = uint32_t(ctx.lr);
  orig_UIImageRenderPrepare(ctx, base);
  if (!self) return;

  // Read AFTER the original: it is what assigns the draw item's material, and
  // the whole question is what it ended up holding.
  std::lock_guard<std::mutex> lk(g_videoProbeMu);
  VideoComponentProbe* p = FindProbe(self);
  if (!p) return;  // some other image component; not this probe's population
  ++p->render_calls;
  p->last_material = REX_LOAD_U32(self + kCompMaterialSlot);
  p->last_draw_item = REX_LOAD_U32(self + kCompDrawItem);
  if (!p->first_caller) p->first_caller = caller;
  p->last_caller = caller;
  ReportVideoComponents(false);
}

//-----------------------------------------------------------------------------
// The engine's texture header filler — sub_826295E8
//
// 99.7% of this game's textures are created here, not through
// D3DDevice_CreateTexture. It takes no name, so it cannot be filtered by
// "_VideoRenderTarget" directly; what it does carry is the DESTINATION HEADER
// POINTER, and that is the texture object a consumer would later bind.
//
// Argument layout is from three wrappers' register moves, NOT from Hex-Rays
// (whose 42-argument prototype interleaves real stack slots with junk):
//
//     r3 width, r4 height, r5 depth, r6 levels, r7 usage, r8 format
//     sp+0x74 = D3DRESOURCETYPE,  sp+0x7c = destination header pointer
//
// r1 on entry still points at the CALLER's frame — the outgoing parameter area
// lives there, before the callee's stwu — so the stack args are read at entry.
//
// Filtered to Usage == 1, which is the render-target case: the two parallel
// format tables at 0x82D54414 / 0x82D5448C are read with Usage = 1. That should
// be a handful of creations rather than the ~17,000 total.
//
// The header's six fetch dwords at +0x1C are read AFTER the original runs, when
// they are real: this is a texture header, unlike the UIVideoLayer asset object
// whose +0x1C holds the ASCII name and produced a false `type=2` reading.
// base_address is bits [31:12], so the guest address is fetch0 & 0xFFFFF000 —
// compare it against the 0xFBE94000 that FE_Smoke's 1280x430 resolve lands on.
REX_IMPORT(__imp__sub_826295E8, orig_EngineFillTextureHeader, void());
extern "C" REX_FUNC(sub_826295E8) {
  const uint32_t width = ctx.r3.u32;
  const uint32_t height = ctx.r4.u32;
  const uint32_t depth = ctx.r5.u32;
  const uint32_t levels = ctx.r6.u32;
  const uint32_t usage = ctx.r7.u32;
  const uint32_t format = ctx.r8.u32;
  const uint32_t sp = ctx.r1.u32;
  const uint32_t restype = sp ? REX_LOAD_U32(sp + 0x74) : 0;
  const uint32_t dest = sp ? REX_LOAD_U32(sp + 0x7C) : 0;

  orig_EngineFillTextureHeader(ctx, base);

  // COUNT FIRST, FILTER SECOND. The first cut gated on `usage == 1` and printed
  // nothing at all, which says the same thing as "the function is never called"
  // -- and those are completely different diagnoses. So the population is
  // reported before any filter is applied to it.
  {
    static std::mutex s_cmu;
    static uint64_t s_calls = 0, s_no_dest = 0;
    static std::map<uint32_t, uint64_t> s_byUsage;
    std::lock_guard<std::mutex> lk(s_cmu);
    ++s_calls;
    if (!dest) ++s_no_dest;
    ++s_byUsage[usage];
    if (s_calls <= 6 || (s_calls % 2000) == 0) {
      std::string usages;
      for (const auto& [u, n] : s_byUsage)
        usages += fmt::format(" usage{}={}", u, n);
      REXLOG_INFO("native: ENGINE TEX HEADER {} calls ({} with no dest), "
                  "latest {}x{}x{} levels {} usage {} format {} restype {} "
                  "dest 0x{:08X}; by usage:{}",
                  s_calls, s_no_dest, width, height, depth, levels, usage,
                  format, restype, dest, usages);
    }
  }

  // NO usage filter. Two attempts to guess which usage means "render target"
  // were both wrong:
  //
  //   usage == 1  -- printed nothing; this title never uses 1. The value came
  //                 from the format TABLES being read with Usage = 1, which is
  //                 the table lookup and not this argument.
  //   usage != 0  -- printed the Bink video PLANES (640x216 + 320x112 for
  //                 FE_Smoke, 1280x720 + 640x360 for Attract). usage 4 is the
  //                 decode target, not a render target.
  //
  // The render targets are usage 0: the first three calls of the run are
  // 1280x720 into 0x2123CA60 / 0x2123C238 / 0x2123C26C, and 0x2123C238 is a
  // destination the resolve census already tracks.
  //
  // So: dedupe by SHAPE across everything and let the census show what exists.
  // The size that matters is 1280x430 — FE_Smoke's resolve extent.
  if (!dest) return;
  // One line per distinct shape, so a render target created every frame does
  // not drown the one created once.
  static std::mutex s_mu;
  static std::set<uint64_t> s_seen;
  const uint64_t key = (uint64_t(width) << 40) ^ (uint64_t(height) << 16) ^
                       (uint64_t(format) << 8) ^ uint64_t(levels);
  {
    std::lock_guard<std::mutex> lk(s_mu);
    if (s_seen.size() >= 96 || !s_seen.insert(key).second) return;
  }
  // RANGE-CHECKED. `dest` is a guest value that has only been tested for
  // non-zero, and the six fetch dwords at +0x1C are read straight off it. That
  // is the same shape as the "Litl" crash in the video probe: a field that is
  // usually an object, occasionally not, dereferenced because it looked
  // plausible. This one is diagnostics only and must never be the thing that
  // takes the process down.
  uint32_t fetch[6] = {};
  if (!GuestRangeReadable(base, dest + 0x1Cu, 6u * 4u)) {
    static std::atomic<uint64_t> s_unreadable{0};
    const uint64_t n = s_unreadable.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 4)
      REXLOG_INFO("native: ENGINE TEX SHAPE {}x{}x{} -- header 0x{:08X} +0x1C is "
                  "not readable, fetch constants not dumped ({} so far)",
                  width, height, depth, dest, n);
    return;
  }
  for (uint32_t i = 0; i < 6; ++i) fetch[i] = REX_LOAD_U32(dest + 0x1C + i * 4);
  REXLOG_INFO("native: ENGINE TEX SHAPE {}x{}x{} levels {} usage {} format "
              "0x{:08X} restype {} -> header 0x{:08X}; fetch0=0x{:08X} type={} "
              "addr=0x{:08X}",
              width, height, depth, levels, usage, format, restype, dest,
              fetch[0], fetch[0] & 3u, fetch[0] & 0xFFFFF000u);
}
