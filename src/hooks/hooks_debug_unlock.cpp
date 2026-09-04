// UNLOCKING THE GUEST'S OWN DEBUG FACILITIES.
//
// Split out of hooks_plugin_diag.cpp because these hooks do not observe the
// guest, they CHANGE an answer it gives itself, to switch on code the shipping
// build compiled in and then gated off.
//
// The game ships a complete developer build behind three independent gates --
// flipping one does not reach the others:
//
//   debug_menu    the LUA gate. DEFINE_BuildConfig is pushed as "RELEASE";
//                 MXUI/UI_Helper.lua tests it for inequality in
//                 AllowDebugMenu(), and FE_Title.lua puts the dev menu on the
//                 title screen from the same test. Reaches the UI scripts only.
//   debug_native  a `return 0;` the engine tests in places. NOT a single
//                 predicate -- see the correction at its cvar. Per call site.
//   debug_binds   the INPUT gate, which is really an absence: the engine
//                 registers a full debug action set that the shipped
//                 ControllerPresets.bxml binds to nothing.
//
// What each unlocks: the dev menu works and loads levels; the debug camera has
// no "enter" action and its native gate has not been found; DebugOverlay is the
// ASSERT SCREEN rather than a HUD, so it has no toggle.

#include "hooks/hook_common.h"
#include "hooks/hooks_d3d9.h"  // GuestRangeReadable

#include <rex/cvar.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <map>
#include <mutex>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// debug_native -- the engine's own "is this a debug build" predicate, which
// ships compiled to a constant false:
//
//     int sub_829E8FA8() { return 0; }
//
// It is the NATIVE counterpart of the Lua DEFINE_BuildConfig that debug_menu
// flips. The two are unrelated, which is why the dev menu appears but the debug
// camera does not. Two known consumers, both found while chasing the overlay:
//
//   sub_82AB6300  the DebugOverlay constructor. It loads its database and
//                 package, resolves the material, lays out seven HUD cells, and
//                 then sets its enabled field from this predicate.
//   sub_82AB58F8  a screen/state poller whose second branch tests it.
//
// CORRECTION: sub_829E8FA8 IS NOT ONE FUNCTION. It is the shared body of every
// `return 0;` in the image, folded together by the linker -- one address cannot
// simultaneously be a Lua C function registered as `print`, a boolean predicate,
// and a stored flag. So the "40+ call sites" the census reports are every
// STUBBED-OUT function in the binary sharing one body, and that is also why
// `all` crashes: it returns 1 to callers of unrelated stubs, Lua's `print`
// among them.
//
// A consequence worth knowing: the guest's `print()` is that stub, so every
// print() in the shipped Lua discards its output. Engine.DebugPrintTable and
// Engine.DebugPrintRows likewise validate their arguments and call nullsub_1.
//
// PER CALL SITE, and CHECK THE SITE FIRST -- the census cannot tell a debug
// check from any other stub. 0x82AB6638 is verified.
//
// ONE SWITCH FOR ALL OF IT. `dev` takes comma-separated tokens:
//
//   menu                     the Lua gate: DEFINE_BuildConfig -> "DEBUG"
//   print                    capture the guest print() to logs/guest_print.log
//   native:census            report the stub callers, change nothing
//   native:<hex>             answer 1 for that ONE return address. REPEATABLE
//   native:all               answer 1 everywhere. Crashes; see above
//   bind:<Button>=<Action>   add one debug binding. REPEATABLE, and OFF unless
//                            asked for -- the debug camera has no way to be
//                            entered yet
//
//   --dev=menu,print,native:0x82AB6638
//
// native: and bind: are repeatable rather than comma-lists of their own, because
// the token separator is already a comma.
REXCVAR_DEFINE_STRING(dev, "", "Debug",
                      "Developer switches, comma separated: menu, print, "
                      "native:census|<hex>|all. See hooks_debug_unlock.cpp");

namespace {

// Split `dev` per call. It is read on cold paths only -- a Lua global
// registration, a preset parse, a stub call -- so it never needs caching.
bool DevFlag(const char* name) {
  const std::string spec = REXCVAR_GET(dev);
  size_t pos = 0;
  while (pos < spec.size()) {
    size_t comma = spec.find(',', pos);
    if (comma == std::string::npos) comma = spec.size();
    if (spec.compare(pos, comma - pos, name) == 0) return true;
    pos = comma + 1;
  }
  return false;
}

// Every value given as `name:value`, in order.
std::vector<std::string> DevOptions(const char* name) {
  std::vector<std::string> out;
  const std::string spec = REXCVAR_GET(dev);
  const std::string prefix = std::string(name) + ":";
  size_t pos = 0;
  while (pos < spec.size()) {
    size_t comma = spec.find(',', pos);
    if (comma == std::string::npos) comma = spec.size();
    const std::string tok = spec.substr(pos, comma - pos);
    if (tok.rfind(prefix, 0) == 0) out.push_back(tok.substr(prefix.size()));
    pos = comma + 1;
  }
  return out;
}

}  // namespace


namespace {
std::mutex g_dbgNativeMu;
std::map<uint32_t, uint64_t> g_dbgNativeAsks;   // lr -> times asked
uint64_t g_dbgNativeTrue = 0;
std::chrono::steady_clock::time_point g_dbgNativeLast{};
}  // namespace

void NoteGuestPrint(const std::string& text);

// A Lua string argument, read straight off the stack. Deliberately a local copy
// of the reader in hooks_plugin_diag.cpp rather than a shared symbol: it is
// twenty guarded lines, and the alternative is this file depending on the
// internals of the one it was split out of. Every field is range-checked,
// because the argument is guest data of whatever type the script passed.
std::string LuaArgString(uint8_t* base, uint32_t L, uint32_t index) {
  constexpr uint32_t kTValueStride = 16, kTValueType = 8, kLuaTString = 4;
  constexpr uint32_t kTStringLen = 12, kTStringChars = 16;
  if (!L || !GuestRangeReadable(base, L + 12, 4)) return {};
  const uint32_t stack = REX_LOAD_U32(L + 12);
  const uint32_t slot = stack + index * kTValueStride;
  if (!stack || !GuestRangeReadable(base, slot, kTValueStride)) return {};
  if (REX_LOAD_U32(slot + kTValueType) != kLuaTString) return {};
  const uint32_t ts = REX_LOAD_U32(slot);
  if (!ts || !GuestRangeReadable(base, ts, kTStringChars)) return {};
  uint32_t len = REX_LOAD_U32(ts + kTStringLen);
  if (len > 512u) len = 512u;
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

REX_IMPORT(__imp__sub_829E8FA8, orig_IsDebugBuild, void());
extern "C" REX_FUNC(sub_829E8FA8) {
  const uint32_t lr = uint32_t(ctx.lr);
  // THE GUEST'S print(), CAUGHT HERE. Lua binds print to this same folded stub,
  // and Lua calls a C function with r3 = lua_State -- so this is the one call
  // site where the argument list is reachable. The dispatcher is not: its r3 is
  // its own first parameter, not the state, which is why the first attempt's
  // capture came back empty. Every other caller of this stub passes something
  // that is not a lua_State, and LuaArgString range-checks its way to an empty
  // string on all of them.
  NoteGuestPrint(LuaArgString(base, ctx.r3.u32, 0));
  orig_IsDebugBuild(ctx, base);
  const std::vector<std::string> sites = DevOptions("native");
  if (sites.empty()) return;

  bool want = false;
  for (const std::string& one : sites) {
    if (one == "all") { want = true; break; }
    if (one == "census") continue;
    // Exact return address. Matching on lr rather than on the enclosing
    // function keeps this to the one call the log named.
    const uint32_t v = uint32_t(std::strtoul(one.c_str(), nullptr, 0));
    if (v && v == lr) { want = true; break; }
  }
  if (want) ctx.r3.u64 = 1;

  std::lock_guard<std::mutex> lk(g_dbgNativeMu);
  ++g_dbgNativeAsks[lr];
  if (want) ++g_dbgNativeTrue;
  // Census on a timer, not per call: the render and asset paths ask constantly
  // and a line each would drown the log.
  const auto now = std::chrono::steady_clock::now();
  if (g_dbgNativeLast.time_since_epoch().count() != 0 &&
      now - g_dbgNativeLast < std::chrono::seconds(10))
    return;
  g_dbgNativeLast = now;
  std::string rows;
  for (const auto& [site, n] : g_dbgNativeAsks)
    rows += fmt::format(" [lr=0x{:08X} x{}]", site, n);
  REXLOG_INFO("{}: dev native CALLERS ({}) -- {} distinct site(s), {} answer(s) "
              "flipped to true:{}",
              mx::native::g_plugin_mode ? "plugin" : "native", REXCVAR_GET(dev),
              g_dbgNativeAsks.size(), g_dbgNativeTrue,
              rows.empty() ? " (none)" : rows);
}

// debug_binds -- put the engine's debug actions on the buttons its own shipped
// presets leave empty.
//
// sub_8230EB48 registers a full set of debug input ACTIONS by name --
// DebugCamera{Forward,Backward,Up,Down,ZoomIn,ZoomOut,...}, DebugEject*,
// DebugBarBangModifier1-4, Spec{Next,Prev}Player, Replay* and more. NONE is
// bound: ControllerPresets.bxml ships six presets referencing only 20 gameplay
// actions, and eight inputs are empty in ALL six.
//
// THE TABLE, from the parser at sub_82308300:
//
//     v6 = sub_82B69BA8(buttonName);                       // button index
//     v9 = &unk_82DAA740 + 780 * (25 * presetIndex + v6);  // the entry
//     ... copy the action string to v9 + 260 * slot
//
// so entry(preset, button) = 0x82DAA740 + 780*(25*preset + button), holding
// three 260-byte action strings. Written AFTER the parse rather than by
// intercepting it, because the table is the parser's whole output.
//
// Button indices are LEARNED, not assumed: sub_82B69BA8 maps a button name to
// its index and the parser calls it for every Input element.
//
// Two safety properties. A slot is written ONLY if the guest left it empty, so a
// real binding can never be clobbered. And the layout is VERIFIED before any
// write: a known shipped binding is read back and must match.
namespace {

constexpr uint32_t kPresetTable = 0x82DAA740u;
constexpr uint32_t kEntryStride = 780u;    // three action slots
constexpr uint32_t kButtonsPerPreset = 25u;
constexpr uint32_t kActionStride = 260u;
constexpr uint32_t kPresetCount = 6u;      // Regular1-3, Advanced1-3

std::mutex g_bindMu;
std::map<std::string, uint32_t> g_buttonIndex;  // learned from sub_82B69BA8

uint32_t PresetEntry(uint32_t preset, uint32_t button) {
  return kPresetTable + kEntryStride * (kButtonsPerPreset * preset + button);
}

std::string ReadGuestCString(uint8_t* base, uint32_t addr, uint32_t max) {
  std::string out;
  for (uint32_t i = 0; i < max; ++i) {
    const uint8_t c = REX_LOAD_U8(addr + i);
    if (!c) break;
    out.push_back(char(c));
  }
  return out;
}

}  // namespace

// sub_82B69BA8(name) -- button name to index. Hooked to learn the mapping the
// preset parser itself uses.
REX_IMPORT(__imp__sub_82B69BA8, orig_ButtonIndex, void());
extern "C" REX_FUNC(sub_82B69BA8) {
  const uint32_t name_ptr = ctx.r3.u32;
  orig_ButtonIndex(ctx, base);
  if (!name_ptr || !GuestRangeReadable(base, name_ptr, 4)) return;
  const std::string name = ReadGuestCString(base, name_ptr, 64);
  if (name.empty()) return;
  std::lock_guard<std::mutex> lk(g_bindMu);
  g_buttonIndex[name] = ctx.r3.u32;
}

// sub_82308300() -- parses ControllerPresets.bxml into the table above.
REX_IMPORT(__imp__sub_82308300, orig_ParsePresets, void());
extern "C" REX_FUNC(sub_82308300) {
  orig_ParsePresets(ctx, base);
  const std::vector<std::string> pairs = DevOptions("bind");
  if (pairs.empty()) return;
  const char* tag = mx::native::g_plugin_mode ? "plugin" : "native";

  std::lock_guard<std::mutex> lk(g_bindMu);

  // PROVE THE LAYOUT BEFORE WRITING. Throttle is on Button12 in every shipped
  // preset, so if the arithmetic is right that entry reads back as "Throttle".
  // If it does not, the stride or base is wrong and writing would corrupt the
  // input table.
  const auto it_probe = g_buttonIndex.find("VIJoystick_Button12");
  std::string probe;
  if (it_probe != g_buttonIndex.end() &&
      GuestRangeReadable(base, PresetEntry(0, it_probe->second), kActionStride))
    probe = ReadGuestCString(base, PresetEntry(0, it_probe->second), 64);
  if (probe != "Throttle") {
    REXLOG_ERROR("{}: debug_binds REFUSED -- preset table layout check failed. "
                 "Expected Button12 of preset 0 to read \"Throttle\", got "
                 "\"{}\" ({} button names learned). Nothing written.",
                 tag, probe, g_buttonIndex.size());
    return;
  }

  uint32_t applied = 0, skipped_taken = 0, unknown = 0;
  for (const std::string& pair_in : pairs) {
    std::string pair = pair_in;
    const size_t eq = pair.find('=');
    if (eq == std::string::npos) continue;
    std::string btn = pair.substr(0, eq);
    const std::string action = pair.substr(eq + 1);
    if (btn.empty() || action.empty() || action.size() >= kActionStride) continue;
    // Accept the short form as well as the name the data file uses.
    if (btn.rfind("VIJoystick_", 0) != 0) btn = "VIJoystick_" + btn;
    const auto it = g_buttonIndex.find(btn);
    if (it == g_buttonIndex.end()) {
      ++unknown;
      REXLOG_ERROR("{}: debug_binds unknown button \"{}\"", tag, btn);
      continue;
    }
    for (uint32_t preset = 0; preset < kPresetCount; ++preset) {
      const uint32_t at = PresetEntry(preset, it->second);
      if (!GuestRangeReadable(base, at, kActionStride)) continue;
      // Empty slots only: never overwrite a binding the guest made.
      if (REX_LOAD_U8(at) != 0) { ++skipped_taken; continue; }
      for (size_t i = 0; i < action.size(); ++i)
        REX_STORE_U8(at + uint32_t(i), uint8_t(action[i]));
      REX_STORE_U8(at + uint32_t(action.size()), 0);
      ++applied;
    }
  }
  REXLOG_INFO("{}: dev bind applied {} binding(s) across {} presets, {} slot(s) "
              "left alone because the guest already bound them, {} unknown "
              "button name(s); {} button names learned",
              tag, applied, kPresetCount, skipped_taken, unknown,
              g_buttonIndex.size());
}

// DEFINE_BuildConfig -- the guest's own debug gate, flipped at its single source.
//
// sub_82500760 registers the engine's Lua globals. In disassembly (Hex-Rays
// renders this function as a __noreturn stub and truncates the rest):
//
//     addi r4, r10, aRelease@l          ; "RELEASE"
//     bl   sub_82A9F468                 ; lua_pushstring(L, "RELEASE")
//     li   r4, -0x2712                  ; -10002 = LUA_GLOBALSINDEX
//     addi r5, r9, aDefineBuildcon@l    ; "DEFINE_BuildConfig"
//     bl   sub_82A9FA18                 ; lua_setfield(L, GLOBALS, name)
//
// The shipped UI scripts gate the developer build on it and nothing else:
//
//     function AllowDebugMenu()                       -- MXUI/UI_Helper.lua
//        if( DEFINE_BuildConfig ~= "RELEASE" ) then return TRUE else return FALSE end
//
// The assets are all still in the packages: MXUI has DB_Menu, DB_UnitTests.lua
// and DB_GraphicsTest.swfx; EngineDependencies has DebugOverlay and
// DebugGraphics.
//
// SWAPPED AT THE PUSH, not by rewriting the global afterwards. "RELEASE" has
// exactly ONE xref in the binary -- this push -- so an equality test on the
// pointer cannot touch anything else, and it lands before the setfield rather
// than racing whatever reads the global first. "DEBUG" is an existing guest
// string, so no memory has to be written into the guest.
REX_IMPORT(__imp__sub_82A9F468, orig_LuaPushString, void());
extern "C" REX_FUNC(sub_82A9F468) {
  if (DevFlag("menu") && ctx.r4.u32 == 0x820468E0u) {
    ctx.r4.u64 = 0x8204E1E4u;  // "DEBUG"
    static std::atomic<bool> s_said{false};
    bool expected = false;
    if (s_said.compare_exchange_strong(expected, true))
      REXLOG_INFO("{}: DEFINE_BuildConfig pushed as \"DEBUG\" instead of "
                  "\"RELEASE\" -- AllowDebugMenu() will now return TRUE",
                  mx::native::g_plugin_mode ? "plugin" : "native");
  }
  orig_LuaPushString(ctx, base);
}

// guest_print_log -- the guest's own print() output, which the retail build
// throws away. Lua's `print` is registered as a C closure whose function is the
// folded `return 0;` body, so every print() in the shipped scripts formats
// nothing and returns, losing chunk load banners, state transitions and routing
// decisions.
//
// Captured at the SCRIPT DISPATCHER rather than at the stub, because the
// dispatcher is where the lua_State and the argument list are both in hand. The
// cfunc match is on that folded address, so a call to any OTHER stubbed binding
// lands here too -- which is why only calls with a non-empty STRING first
// argument are written. Its own file, so the main log stays readable.
namespace {
std::mutex g_printMu;
std::ofstream g_printFile;
uint64_t g_printLines = 0;
}  // namespace

void NoteGuestPrint(const std::string& text) {
  if (text.empty() || !DevFlag("print")) return;
  std::lock_guard<std::mutex> lk(g_printMu);
  if (!g_printFile.is_open()) {
    std::error_code ec;
    std::filesystem::create_directories("logs", ec);
    g_printFile.open("logs/guest_print.log", std::ios::out | std::ios::trunc);
    if (!g_printFile.is_open()) return;
    g_printFile << "# guest Lua print() output. The retail build binds print to\n"
                   "# sub_829E8FA8, a folded `return 0;`, so none of this is\n"
                   "# printed by the game itself.\n";
  }
  g_printFile << text << '\n';
  // Flushed every line on purpose: this exists to survive a crash, and the
  // volume is a few hundred lines a run rather than a few hundred thousand.
  g_printFile.flush();
  ++g_printLines;
}
