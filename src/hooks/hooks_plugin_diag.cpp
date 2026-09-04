// DIAGNOSTIC HOOKS -- log critical functions that fail in native mode. Most only
// log when --gpu_plugin=xenos and otherwise fall straight through to the guest
// original; sub_8253AA40 logs in both modes.
//
// Note the mid-ASM hooks that skip these call sites are unconditional, so a hook
// being silent means its call site is jumped, not that the mode is wrong.

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

// force_load is GONE, cvar and implementation both. It named a scene to request
// from the AssetDB once the loader went idle, by calling the guest's own
// sub_82534980(AssetDB, name, flags) from the LoadStateMachine hook below.
//
// The observation behind it still stands and is still unexplained: the loader
// reaches state 2 (IdleClearRenderBusy) and parks, because nothing in the game
// ever calls sub_82534980. That same idle AssetDB is why UI_World never loads,
// and it is the root of the 0x8234CE20 crash. Neither lever ever explained WHY
// nothing asks.

// Every string setting the guest reads comes from MXRegistry.bxml through one
// function, sub_825487C8(registry, key, out, size, 0). `registry_override` takes
// comma-separated key=value pairs and substitutes the value for a matching key
// as it is read, which is what shipping a different MXRegistry.bxml would do --
// tools/ has bxml decoders but no encoder. Empty means off. Diagnostic only.
REXCVAR_DEFINE_STRING(registry_override, "", "Debug",
                      "Comma-separated key=value overrides for guest registry string reads");

// sub_82B34998 -- RendererDispatchBlock, called from LoaderTick on the
// Transition thread.
//
// MADE MODE-NEUTRAL, and keep it that way. It used to log only under the plugin
// and call the original silently in native, which hid the number that found the
// frame-pacing bug: `f1` arriving here was exactly 0.00 in native and varied
// under the plugin. **A one-sided probe was read as evidence that native behaved
// the same.** Most Transition-thread probes in this file are still plugin-only,
// so their absence from a native log means nothing.
REX_IMPORT(__imp__sub_82B34998, orig_RendererDispatch, void());
extern "C" REX_FUNC(sub_82B34998) {
  const char* tag = mx::native::g_plugin_mode ? "plugin" : "native";
  static int rd = 0;
  ++rd;
  const bool loud = rd <= 20 || (rd % 500) == 0;
  const uint32_t a1 = ctx.r3.u32;
  const double dt = ctx.f1.f64;
  orig_RendererDispatch(ctx, base);
  if (loud) {
    REXLOG_INFO("{}: RendererDispatch #{} a1=0x{:08X} f1={:.6f} -> r3=0x{:08X}", tag,
                rd, a1, dt, ctx.r3.u32);
  }
}

// sub_82B3C7D0 -- accessor for dword_830BE190, with a null assert.
//
//   result = dword_830BE190;
//   if (!dword_830BE190) sub_82AB73C0(61568);   // assert helper, line number
//   return result;
//
// It was labelled "lazy-init alloc (hangs in Transition thread in native)". Both
// halves are wrong: it allocates nothing, and a load / branch / return cannot
// hang. The label predates the D3D9 HLE layer and was never re-derived. Measured
// as well as decompiled: called twice, returns 0x212859A0 both times, assert
// path never taken.
//
// Kept, and mode-neutral, for the one thing worth watching: whether
// dword_830BE190 is ever null.
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

// sub_82B70370 -- frame pacing: QPC delta / perf frequency -> dt at a1+24, then
// a 5-sample smoothing pass and the running totals at a1+56/60/64.
//
// This was stubbed in native mode, and that stub was the reason the front end
// never ran: it wrote a fixed 1/60 to a1+24 and nothing else, so a1+60 never
// advanced and the dt reaching LoaderTick's RendererDispatch was exactly 0.00.
// Unstubbing took the script VM from 28 dispatches to 686 and Bink opens from 0
// to 3 -- plugin mode's numbers exactly, 3/3 runs.
//
// The stub's two stated hazards were both false and neither had been checked
// against the guest code:
//
//   - "a1+20 drives a busy-wait". The guest's own test is
//     `if (*(float*)(a1+20) != 3.4028235e38 && dt < target)`, and a1+20 holds
//     exactly that FLT_MAX sentinel, so the guest disables the spin itself.
//   - "a1+32 is an unbounded store offset". It is a bounded 5-entry ring at
//     a1+36..a1+52, guarded by `if (*(a1+28))`.
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

// sub_82B6D230 -- called from LoaderTick's entity block with f1=dt. It iterates
// a vector calling sub_82B6A448(elem, dt) on each, so it is an entity update
// pass, not the "frontier probe" it was labelled as. That label said execution
// "reaches TexManager then dies" here; it does not, and ENTER and RETURNED
// balance 5/5 across three runs.
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
// Logs in BOTH modes. The note that used to sit here -- "in native this is
// unreachable, so its absence from the log is itself the signal" -- is STALE.
// Every mid-ASM hook in mx_config.toml is commented out, so LoaderTick's vt[6]
// gate runs and this fires continuously (1600 calls in a two-minute native run).
// Its absence would now mean something is wrong, not something is skipped.
//
// What that run showed: the machine goes 0 -> 1 -> 2 and then stays at 2.
// State 2 is idle-awaiting-a-request -- sub_82534980 is what moves it 2 -> 3,
// and in a front-end-only run nothing calls it. The AssetDB is healthy and
// unasked, which is why UI_World never loads.
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
  // listener at *(a1+110788) and calls its vt[2]. A zero return jumps to an
  // early return that leaves the selector at 6. The per-player loop and the
  // `state = 7` write are both downstream of that gate.
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
// sub_82352AE0, which builds the scene name from a registry lookup and is itself
// a method with five callers. Nothing in this chain has ever been observed to
// run in native mode -- the point of these hooks is to find out how far up
// execution actually reaches, so entry-only logging is enough for everything
// except sub_82534980 itself.
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
// sub_82473360 and differ only in the value type. Two matter, both reading the
// same registry global *(0x82D6605C):
//
//   sub_825487C8(registry, key, out, size, 0)   string, non-zero if found
//   sub_82548758(registry, key, out, 0)         int, written through `out`
//
// Their callers:
//
//   sub_82352AE0  key 0x8200C864 = "Location", 256-byte buffer -> the name it
//     hands to sub_82534980. This is the load path we have never seen run.
//   sub_82536250  key 0x8200C870 = "PlayerMode", 30-byte buffer -> mapped by
//     sub_82533D20 to an index in the 5-entry string table at 0x82D1F810.
//
// Measured, not inferred -- an earlier reading guessed the keys were
// "UISceneName"/"startMode" from .rdata spacing. The table is a network mode
// vocabulary rather than a boot mode one:
//
//   0 SplitScreen   1 SinglePlayer   2 Online   3 LAN   4 None
//
// The state 6 gate wants 2 or 3 and the registry says "None", so its first term
// asks "are we in a network session", which offline we legitimately are not --
// the term worth moving is the third one, the int at key 0x8204C630.
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
  // the three extra ones are the boundary native stops short of.
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
// The front end is script-driven, not C++ virtual dispatch
//
// Tracing the load-request chain upward runs out of static call sites: four of
// the five functions above `sub_82352AE0` have zero direct callers, and they are
// not virtual methods either. They are entries in a **name -> function binding
// table** at 0x8203F2E0, 228 pairs of `const char*` and code pointer, registered
// from MXRavage_Xenon_00cb. The vocabulary is a scripting API:
//
//   [  6] LoadAssetDB          [ 10] ExecuteScriptAsset
//   [ 40] GetUIState           [ 50] LoadUIAssetPackage
//   [ 53] LoadUIAssetDatabasePackage
//   [ 66] StartWorldLoad       [ 67] EnableWorld
//   [110] SwitchToUIWorld
//
// `StartWorldLoad` is sub_824CD280 -- the function that reaches sub_82534980.
// So nothing requests a scene because **no script ever calls StartWorldLoad**,
// and the question is whether the script environment runs at all.
//
// These four probes sit at different depths: ExecuteScriptAsset is "does any
// script run", the two UI loaders are "does the front end's content arrive", and
// StartWorldLoad is the endpoint we know is silent. Whichever is the last to
// fire names the break.
//=============================================================================

// The script call's Nth argument, when it is a Lua string.
//
// Every script probe logged only `a1`, which is the lua_State -- the SAME
// pointer for every call in the run, so `LoadUIAssetPackage #1..#4` could not
// say WHICH packages were asked for.
//
// The layout is not guessed. sub_82AA7638 (luaD_precall) is decoded at the
// bottom of this file and reads a StkId as `tt` at +8 and the GC pointer at +0,
// so TValue is {Value(8), int tt} with the 4-byte pointer at the front of the
// union -- big-endian, so offset 0 -- and a 16-byte stride. `L->base` is
// lua_State+12, LUA_TSTRING is 4, and a Lua 5.1 TString on this target is 16
// bytes of header with the characters immediately after.
//
// Every read is range-checked against the real mapping. A script argument is
// guest data of whatever type the script passed, and this file has already paid
// once for treating a plausible-looking value as a pointer: a range test
// admitted the ASCII "Litl" and the process died dereferencing it.
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
  // The throttle exists so a per-frame binding cannot flood the log, and it is
  // exactly wrong for this question: package loads happen in a burst during
  // boot, so a 5-second window collapses them to one line.
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
// These are NOT LoadUIAssetPackage. UI_Helper loads the front end with two
// different families:
//
//     function LoadFrontEndUIPackages( )
//        local isUILoaded = g_UIVariables:GetVariableInt( "UILoaded" );
//        if( isUILoaded == FALSE ) then
//           Engine.LoadAssetDB( "UIAnimations" );
//           Engine.LoadAssetPackage( "UIAnimations", "Rider" );
//           ...
//           Engine.LoadUIAssetPackage( "FrontEndShared" );
//
// Only the LoadUIAssetPackage family was probed, so a whole run looked like
// "UIAnimations is never requested" -- concluded, wrongly, from an instrument
// that could not see the call. FrontEndShared and FrontEnd DO appear in the log
// and sit inside the same if-block three lines below, which is the proof the
// block runs and the UIAnimations lines with it.
//
// UIAnimations holds exactly three assets, and RiderUI_Final_C_350 is the movie
// whose null asset is the 0x8234CE20 crash. So the question is no longer "is it
// asked for" but "what does asking return". Both args and the return value,
// because a load binding that fails quietly is exactly what this looks like.
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
// The return value IS vt[72]'s, so hooking this one function reports whether the
// package load succeeded without having to resolve a single vtable slot -- which
// matters, because deriving them statically already failed once: the AssetDB
// ctor comment names "vtable off_8214518C", but off_8214518C+0x78 holds an
// address in the MIDDLE of sub_82BA8D08.
//
// Everything upstream is confirmed good: both bindings fire with exactly those
// arguments, and UIAnimations.xenon.database declares a package "Rider" holding
// RiderUI_Final_C_350. So either the DB open under "Database\" fails, or the
// package lookup inside it does.
//
// The two load families use DIFFERENT asset-manager methods: the working UI path
// goes through vt[44], this one through vt[36]/vt[28]/vt[72]. "The UI packages
// load fine" says nothing about this path.
//=============================================================================
// 0x82BA91C0 -- AssetManager::Find(type, name), assetMgr->vt[0x78]. Hooked for
// ONE reason: to capture the manager `this`, which AcquirePlayer's re-resolve
// needs and cannot afford a plausible-but-wrong value for. It IS derivable --
// engine = *(0x830BE400), manager = *(engine + 8) -- but that walk has already
// been got wrong once in this file. Any call at all hands us the correct value.
//
// The address had to come from the running game: the IDB gives 0x82BA8D40 for
// that slot, which is the middle of another function, while the live vtable
// holds 0x82BA91C0.
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
// property init and never re-resolves it. When the database worker constructs
// the component before the script has asked for the package holding the movie,
// the lookup misses, a NULL is cached, and AcquirePlayer later writes through it
// at +0x58. Traced end to end in one log: the worker missed, the script asked
// for the database 0.227s LATER, and the access violation landed 36 seconds
// after that.
//
// It is a RACE, and it has been "fixed" by winning it before: async shader
// compilation bought the script 0.9s on this VM and the crash went away, then
// came straight back on a machine 4x faster. Any fix that works by being early
// enough has an expiry date.
//
// THE KEY FACT: at the moment of use the asset is present and simply is not
// looked at again. Re-resolving here asks the same question the guest asked,
// with the same manager, name and type, at a time when the answer exists.
//
// WHY NOT SKIP THE CALL. That was tried and made things worse: returning early
// left this->player null and the fault moved downstream, correlated 3-for-3.
// Nothing is skipped here, and on the path where the repair fails the original
// runs exactly as it does today -- so this hook can restore behaviour, never
// degrade it. Reads and writes ONE dword of guest state, the one the guest
// wrote itself.
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
// IDA could not reach it -- no named terrain/height/suspension functions, the
// terrain manager's xrefs are all VT streaming, and a constant search for the
// height range landed in the save-game string pool.
//
// So make the caller name itself. The skinned vehicle shader indexes its bone
// palette as cb0[bone + 85/86/87], so the root bone lands in vertex constant
// registers 85..87, and whoever writes them is the vehicle's render path.
//
// NOT HOOKED BEFORE, DELIBERATELY, and that reasoning still stands for its own
// purpose -- hooks_d3d9.cpp reads device+0x780 rather than hooking this setter,
// because the device holds the live value whichever path wrote it. That is about
// READING STATE. This wants the CALLER, which the device field cannot give at
// any price. If the bike's transform arrives by the state-block path this probe
// sees nothing -- a silent probe here means "not this path", not "no writes".
//
// Cheap on the hot path: one range compare before anything else happens.

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
  // published only by Type-0 PM4 never reach the shadow.
  //
  // A count of ZERO here says the terrain never sets them this way; a non-zero
  // count with a device pointer different from the bike's says the value is real
  // but on another device, which is a much easier fix.
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
  // gave a mid-function address for +0x78, so the object has to answer for
  // itself.
  //
  // vt[0x1C] load-DB-from-"Database\\", vt[0x24] is-DB-loaded, vt[0x48]
  // load-package, vt[0x78] the asset LOOKUP that returns NULL. A real guest
  // function lives in 0x82xxxxxx; anything else in a slot is data and must never
  // be called.
  {
    static bool s_dumped = false;
    if (!s_dumped) {
      s_dumped = true;
      // dword_830BE400 is a POINTER VARIABLE, not the object. IDA renders the
      // engine as `dword_830BE400` and the manager as `*(dword_830BE400 + 8)`,
      // which reads as an offset off the symbol's ADDRESS and is not: the engine
      // is *(0x830BE400) and the manager is *(engine + 8). Reading 0x830BE408
      // directly gave 0, which is what the first version of this dump printed.
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

// The script VM's native-call dispatcher, sub_82AA7638, used to be probed here
// with MX_SCRIPT_PROBE. It answered its original question -- the VM fires a
// handful of times and goes idle, so the VM itself stops rather than looping --
// but it only logged the lua_State, which cannot say *which* call was the last
// one. It now has a dedicated hook at the bottom of this file.

// The script layer is a Lua VM, so ask it directly whether it threw.
//
// sub_82AA7638 is the call handler and carries the usual strings -- it calls
// sub_82AA9D48(L, "stack overflow"). sub_82A9F4F8 is the luaL_error-style
// reporter, used by ExecuteScriptAsset itself for argument mismatches, so its r4
// is a format string.
//
// A root script that dies on its third statement looks, from outside, exactly
// like the two-libraries-then-silence pattern that is actually observed. These
// two hooks distinguish "stopped" from "crashed".
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
// ReXGlue's own audio and input systems come up in native mode, yet neither
// works. The untested half is the guest: nothing in any log shows it calling in,
// because those are high-frequency kernel calls, and
// --log_high_frequency_kernel_calls=true changed nothing.
//
// These are the XDK wrappers around the import thunks, not the thunks -- the
// thunks are defined in the runtime library and cannot be redefined here, but
// the wrappers are ordinary recompiled functions, each a register shuffle
// followed by a tail branch to the import.
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
// sub_82C87B98 is not a thin wrapper -- it is the XDK mixer. It stack-allocates
// 8064 bytes, has sub_82C87950 fill a buffer at r1+1888, and passes that to
// __imp__XAudioSubmitRenderDriverFrame. 8064-1888 = 6176 bytes of room, and a
// 360 render-driver frame is 256 samples x 6 channels x float32 = 6144, so the
// fill target is the frame buffer itself.
//
// Hooking the fill is the only way to see the samples: the import thunk is
// defined in the runtime library. This separates "the SDK is dropping our audio"
// from "the game is playing nothing".
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
// The VM stops about 1.6s into boot and everything downstream is idle because of
// that. Counting script assets (2 native vs 4 plugin) is too coarse: two assets
// are libraries, and a library can load and then the caller die on its next
// statement.
//
// sub_82AA7638 is Lua's precall. Its r4 is the `func` StkId, so the callee is
// readable before it runs:
//
//   *(func + 8)  == 6      TValue.tt, LUA_TFUNCTION
//   *(func + 0)            the Closure
//   *(closure + 6)         isC -- a C binding rather than Lua bytecode
//   *(closure + 16)        the C function pointer, for isC closures
//
// The offsets are Lua 5.1's and are confirmed against this binary rather than
// assumed: the function's own Lua-closure branch reads Proto fields at +73
// numparams, +74 is_vararg, +75 maxstacksize and +12 code, and it reports "stack
// overflow" at exactly the 20000 limit.
//
// Every dispatch is logged up to a generous cap, so in native mode the last line
// of the log is literally the last statement the script layer reached.

namespace {

// ---- Naming the C function -------------------------------------------------
//
// The script layer is SWIG, not a hand-rolled binding table: the module init
// registers `swig_type` and `swig_equals`, and sub_824A8998 is
// SWIG_Lua_set_immutable. So the 228-entry table is one of five populations of
// lua_CFunction, and a miss in it never meant "not a binding".
//
//   MXRavage_Xenon_00cb (0x824F1D80) is the module init. It calls
//   sub_824A8BA0(L, "Engine"), which builds the global `Engine` table, and then
//   walks:
//
//     1. module functions    0x8203F2E0  {name, func}      via sub_824A8DC0
//     2. module attributes   0x82D1C858  {name, get, set}  via sub_824A8CE0
//     3. every class reachable through swig_types[], registered by sub_824A9580
//        (SWIG_Lua_class_register) and its member pass sub_824A9358
//
//   Populations 4 and 5 are the metamethods. They are INSTALLED, so no table
//   holds them and no scan can reach them: they are the fixed list below.
//
// Every SWIG array is NUL-terminated, never counted. The 228 is the measured
// distance to the terminator, kept as a runaway bound rather than as the loop
// condition, so a table that grows cannot silently truncate.
//
// Layouts taken field for field out of sub_824A9358 (members) and sub_824A9500
// (bases), then checked against the live struct at 0x82D1B260. That array is
// what docs/guest_binary.md used to call "a second table around 0x82D1B21C" of
// unestablished bounds -- it is one class's methods, bounded by a terminator
// like every other SWIG array.
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
// folding means several trivial bindings share one address (0x829E8FA8 is a bare
// `return 0` with hundreds of xrefs), and a first-match scan hands back whichever
// it reaches first. A name here is a name for the ADDRESS, not proof of which
// binding ran. Do not treat a folded hit as an identification.
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
// worthless as a caller: 0x82AABA74 is the `bl` to this function inside
// luaV_execute, so lr=0x82AABA78 is the interpreter's OP_CALL and EVERY
// script-level call in the game reports it. luaD_precall has four call sites, so
// the only thing an lr distinguishes is "from a script" versus "from C".
//
// The CallInfo can name the caller, and the offsets are not carried over from
// the Lua 5.1 headers: sub_82AA9B70 is this title's `addinfo` and performs
// exactly the sequence below, field for field, to build its "%s:%d: %s" prefix.
//
//   L+20   ci                        ci+4   func      func+8  TValue.tt (6 = fn)
//   *func  Closure                   +6     isC       +16     Proto (Lua only)
//   L+24   savedpc                   p+12   code      p+20    lineinfo
//   p+32   source (TString)          getstr = TString + 16
//
// currentline is `lineinfo[((savedpc - code) >> 2) - 1]`, and addinfo guards it
// with nothing but `pc >= 0` and `lineinfo != 0`. Matched rather than improved
// on: a bounds check against p+48 would rest on an offset the game never reads.
//
// ---- Which CHUNKS execute at all -------------------------------------------
//
// The binding census can only name a script that calls a C binding, so
// "FE_Background is not in the list" was never evidence that it did not run.
// Executing a Lua chunk means CALLING its main function, so every chunk that
// runs at all passes through precall as a Lua callee.
//
// The population to compare against is the shipped manifest --
// assets/Database/MXUI.xenon.database declares 539 script assets over 82
// distinct names. Anything in that 82 and absent here never executed.
//
// Keyed by PROTO address, not by name: one chunk has many Protos and the name
// resolution is a guest string walk, so this is one map lookup per dispatch and
// one string build per distinct function per run.
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
  // that whole snippet arrives here as the "source". Some carry comment text and
  // EMBEDDED NEWLINES, which put one log entry across many lines and break every
  // grep the log is read with.
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
// which is not "which chunks call bindings": a chunk that only ever calls
// bindings some earlier chunk already touched never appears in it.
//
// Not academic -- it produced a wrong reading. The first-caller list was read as
// "FE_Background loads and instantiates but calls no binding, unlike FE_Home",
// and that asymmetry was an ARTIFACT. Playing the credits proves it: FE_Credits
// is plainly active and on screen and is absent from the list too, because
// everything it calls was already first-seen elsewhere.
//
// So count binding calls PER CALLING CHUNK, keyed by the caller's Proto so the
// hot path is 5 guest loads and one map lookup.
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
// one-shot call like SwitchToUIWorld fires once in a run and has no meaningful
// chance of landing on a sampling boundary, so its absence has never been
// evidence of anything.
//
// One line the first time each distinct C binding is seen, plus a periodic
// roll-up with counts, keyed by guest function address so the roll-up is ordered
// and stable between runs.
//
// Cost: one map lookup per dispatch (~700/min in the front end). BindingName's
// scan runs only on a miss, so at most once per distinct binding per run -- but
// it is NOT off the roll-up path, since ReportBindingCensus names every distinct
// binding again on each report. Do not move BindingName onto the per-call path.
//
// The precall fires on at least three guest threads, so the map needs the lock.
// Do not "optimise" it away.
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
  // handful of guest loads per RUN. Resolved before the lock -- it reads guest
  // memory and takes no lock of its own, and holding the census mutex across it
  // would put the walk on three threads' critical path for no reason.
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
