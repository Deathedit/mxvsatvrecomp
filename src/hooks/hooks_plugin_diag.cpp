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
// MADE MODE-NEUTRAL 2026-08-06. It used to log only under the plugin and call
// the original silently in native, which hid the one number that matters: the
// plugin enters the Lua VM from this thread immediately after this function
// returns, and it returns a non-null r3 (0x21293D44) every time. Natively the
// Transition thread never enters the VM at all. Whether that is because this
// returns something different has never been observable — the probe was
// one-sided.
//
// Beware of reading any *other* Transition-thread line difference between the
// two modes: most of the probes on that thread are still plugin-only, so their
// absence in a native log means nothing.
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

// The registration site is 0x824F1E1C; its enclosing function is
// `sub_824F1C98` — resolved from the recompiled sources, where the next
// definition after it is `sub_824F1F78`, not guessed from the site address. If
// this never runs, the script environment was never set up at all, which is a
// different failure from "set up but never fed".
MX_SCRIPT_PROBE(sub_824F1C98, orig_ScriptBindingRegister, "BindingRegister")

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
// Does the guest load Bink itself?
//=============================================================================
// Bink is statically linked into the XEX, the same way D3D9 is: there is a
// BINKCONS data segment at 0x821CD1D0 and the library code runs from about
// 0x82CEB650 to 0x82CF0508. The guest has a complete decoder and needs nothing
// from the host player in src/gfx/bink_player.cpp.
//
// It also demonstrably uses it. The intro plays under --gpu_plugin, and in
// plugin mode MxApp::OnPreSetup returns before creating D3D12GraphicsSystem, so
// RenderThreadFunc and the FFmpeg BinkPlayer never start at all. Something in
// the guest decoded and presented that video.
//
// Natively, nothing has ever measured whether it even tries. Across 1,035 logs
// the only lines matching /bink/ are mx's own "RenderPipeline #1 — skipped
// (Bink playing)". That is not a negative result — no hook has ever existed on
// the guest path — which is what these four add.
//
// Deliberately mode-neutral: comparing native against plugin is the entire
// point, so every hook logs in both modes and calls the original. Nothing here
// changes guest behaviour.

namespace {

// First `head` hits verbatim, then at most one line per 5s, matching the audio
// and RequestLoad probes above. A looping video would otherwise flood the log.
bool BinkLogDue(uint64_t count, std::chrono::steady_clock::time_point& last,
                uint64_t head) {
  if (count <= head) return true;
  const auto now = std::chrono::steady_clock::now();
  if ((now - last) < std::chrono::seconds(5)) return false;
  last = now;
  return true;
}

const char* BinkTag() { return mx::native::g_plugin_mode ? "plugin" : "native"; }

}  // namespace

// sub_82CEB7C8 — BinkOpen(path, flags). Named from its own error strings, not
// from the call shape: "Not a Bink file." (0x82144B9C) and "Error reading Bink
// header." (0x82144B28) are referenced from this function and nowhere else. It
// has exactly two callers, both of them the manager opens below, so it is the
// single choke point for every video the guest plays.
//
// The return is the line that matters. It separates "the guest never asked"
// from "the guest asked and was refused" — two completely different problems.
// 0 is failure, so failures get a much larger unrate-limited head.
REX_IMPORT(__imp__sub_82CEB7C8, orig_BinkOpen, void());
extern "C" REX_FUNC(sub_82CEB7C8) {
  const uint32_t flags = ctx.r4.u32;
  const uint32_t from = static_cast<uint32_t>(ctx.lr);
  const std::string path = GuestString(base, ctx.r3.u32, 260);
  orig_BinkOpen(ctx, base);
  const uint32_t hbink = ctx.r3.u32;
  static uint64_t s_count = 0;
  static std::chrono::steady_clock::time_point s_last{};
  if (BinkLogDue(++s_count, s_last, hbink ? 8 : 64)) {
    REXLOG_INFO(
        "{}: BinkOpen #{} \"{}\" flags=0x{:08X} -> hbink=0x{:08X} {} from lr=0x{:08X}",
        BinkTag(), s_count, path, flags, hbink, hbink ? "OK" : "FAILED", from);
  }
}

// sub_8234E0A8 — XenonBinkVideoManager::Open, slot [1] of the vtable at
// 0x82017510. Formats "game:\%s.bik" from a3 and hands it to BinkOpen. a4 picks
// the branch: non-zero passes flags 0x2000|0x100400 straight through, zero
// first calls sub_82CEB3F0(10485760) and then passes 0x1000000|0x100400. That
// 10 MB reserve sits on the path and is a plausible native failure point, so a4
// is logged rather than dropped.
//
// Neither this nor sub_8234E290 has a single code xref — both are reached only
// through the vtable, the same shape as the script bindings. A static call
// graph cannot answer this question; only the runtime can.
REX_IMPORT(__imp__sub_8234E0A8, orig_BinkMgrOpenGame, void());
extern "C" REX_FUNC(sub_8234E0A8) {
  const std::string name = GuestString(base, ctx.r5.u32, 260);
  const uint32_t a4 = ctx.r6.u32;
  const uint32_t from = static_cast<uint32_t>(ctx.lr);
  static uint64_t s_count = 0;
  static std::chrono::steady_clock::time_point s_last{};
  if (BinkLogDue(++s_count, s_last, 8)) {
    REXLOG_INFO("{}: BinkMgr::Open(game:) #{} name=\"{}\" a4=0x{:08X} from lr=0x{:08X}",
                BinkTag(), s_count, name, a4, from);
  }
  orig_BinkMgrOpenGame(ctx, base);
}

// sub_8234E290 — XenonBinkVideoManager::Open, slot [2]. Same structure, but it
// formats "%s.bik" and passes a2 — an already-built path — to BinkOpen with
// flags 0x04100400. Both arguments are logged because a2 and a3 need not agree.
REX_IMPORT(__imp__sub_8234E290, orig_BinkMgrOpenPath, void());
extern "C" REX_FUNC(sub_8234E290) {
  const std::string path = GuestString(base, ctx.r4.u32, 260);
  const std::string name = GuestString(base, ctx.r5.u32, 260);
  const uint32_t from = static_cast<uint32_t>(ctx.lr);
  static uint64_t s_count = 0;
  static std::chrono::steady_clock::time_point s_last{};
  if (BinkLogDue(++s_count, s_last, 8)) {
    REXLOG_INFO("{}: BinkMgr::Open(path) #{} path=\"{}\" name=\"{}\" from lr=0x{:08X}",
                BinkTag(), s_count, path, name, from);
  }
  orig_BinkMgrOpenPath(ctx, base);
}

// sub_8234CBB8 — the Bink video component's init. It reads a "Texture To
// Override" property off the descriptor in a2, then resolves the resource named
// by "Bink Video Asset" (0x82016DF0) and requests it by fourcc 1651076715,
// which is 0x62696E6B — 'bink'. Results land at a1[36] and a1[37].
//
// This fires when a video component is *created*, which is upstream of the
// manager open. If it fires and the opens do not, the gap is between the
// component and the manager rather than in Bink itself.
//
// The other "Bink Video Asset" reference, at 0x8234D090, is inside sub_8234CF80
// and is deliberately not hooked. Hex-Rays gives up on that function after one
// line — it opens with a call to the __noreturn sub_82ABAB98 — so what the rest
// of it does has not actually been read, and naming a hook after a guess is the
// mistake this file keeps re-learning. Add it once it has been decompiled.
REX_IMPORT(__imp__sub_8234CBB8, orig_BinkAssetInit, void());
extern "C" REX_FUNC(sub_8234CBB8) {
  const uint32_t a1 = ctx.r3.u32;
  const uint32_t from = static_cast<uint32_t>(ctx.lr);
  static uint64_t s_count = 0;
  static std::chrono::steady_clock::time_point s_last{};
  const bool due = BinkLogDue(++s_count, s_last, 8);
  orig_BinkAssetInit(ctx, base);
  if (due) {
    REXLOG_INFO("{}: BinkAsset::Init #{} a1=0x{:08X} tex=0x{:08X} asset=0x{:08X} from lr=0x{:08X}",
                BinkTag(), s_count, a1, a1 ? REX_LOAD_U32(a1 + 36 * 4) : 0,
                a1 ? REX_LOAD_U32(a1 + 37 * 4) : 0, from);
  }
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

  static uint64_t s_count = 0;
  static std::chrono::steady_clock::time_point s_last{};
  const uint64_t n = ++s_count;
  // Native produces a handful in a whole run, so nothing is dropped there. The
  // plugin runs the front end and produces many, hence the cap.
  bool due = n <= 200;
  if (!due) {
    const auto now = std::chrono::steady_clock::now();
    if ((now - s_last) >= std::chrono::seconds(5)) {
      s_last = now;
      due = true;
    }
  }
  if (due) {
    const std::string name = is_c ? BindingName(base, cfunc) : std::string();
    REXLOG_INFO("{}: vm dispatch #{} {} cfunc=0x{:08X}{}{} from lr=0x{:08X}",
                mx::native::g_plugin_mode ? "plugin" : "native", n,
                !is_fn ? "non-function" : (is_c ? "C" : "lua"), cfunc,
                name.empty() ? "" : " name=", name, from);
  }

  orig_ScriptDispatch(ctx, base);
}

// The two VariableCollection bindings the plugin uses and native never reaches.
// Named from the (name, func) table in .data near 0x82D1B21C, which agrees with
// each function's own error strings ("GetVariableString" /
// "VariableCollection_GetVariableString" in sub_824B1C20). Note the earlier
// reading that put these in a table at 0x821A1740/0x821A1750 was wrong — that is
// .pdata, function address plus unwind flags.
//
// r3 is the lua_State; the key is arg 2 on the Lua stack, which is why these log
// only the fact of the call. The value and key already come out of the registry
// getter probes above, with lr pointing back here.
REX_IMPORT(__imp__sub_824B1C20, orig_GetVariableString, void());
extern "C" REX_FUNC(sub_824B1C20) {
  static uint64_t s_count = 0;
  static std::chrono::steady_clock::time_point s_last{};
  if (BinkLogDue(++s_count, s_last, 20)) {
    REXLOG_INFO("{}: GetVariableString #{} L=0x{:08X} from lr=0x{:08X}",
                mx::native::g_plugin_mode ? "plugin" : "native", s_count,
                ctx.r3.u32, uint32_t(ctx.lr));
  }
  orig_GetVariableString(ctx, base);
}

REX_IMPORT(__imp__sub_824B1788, orig_GetVariableInt, void());
extern "C" REX_FUNC(sub_824B1788) {
  static uint64_t s_count = 0;
  static std::chrono::steady_clock::time_point s_last{};
  if (BinkLogDue(++s_count, s_last, 20)) {
    REXLOG_INFO("{}: GetVariableInt #{} L=0x{:08X} from lr=0x{:08X}",
                mx::native::g_plugin_mode ? "plugin" : "native", s_count,
                ctx.r3.u32, uint32_t(ctx.lr));
  }
  orig_GetVariableInt(ctx, base);
}
