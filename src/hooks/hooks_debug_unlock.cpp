// UNLOCKING THE GUEST'S OWN DEBUG FACILITIES.
//
// Split out of hooks_plugin_diag.cpp 2026-08-30. Everything here has the same
// shape and it is not the shape of that file: these hooks do not observe the
// guest, they CHANGE an answer it gives itself, to switch on code the shipping
// build compiled in and then gated off. Diagnostics belong next to the
// subsystem they measure; these belong together.
//
// The game ships a complete developer build behind three separate gates, and
// they are independent -- flipping one does not reach the others:
//
//   debug_menu    the LUA gate. DEFINE_BuildConfig is pushed as "RELEASE";
//                 MXUI/UI_Helper.lua tests it for inequality in
//                 AllowDebugMenu(), and FE_Title.lua puts the dev menu on the
//                 title screen from the same test. Reaches the UI scripts and
//                 nothing else.
//   debug_native  a `return 0;` the engine tests in places. NOT a single
//                 predicate -- see the correction at its cvar. Per call site,
//                 and each site needs checking before it is flipped.
//   debug_binds   the INPUT gate, which is really an absence: the engine
//                 registers a full debug action set that the shipped
//                 ControllerPresets.bxml binds to nothing.
//
// What each unlocks, and what it does not, is documented at each cvar. The
// short version: the dev menu works and loads levels; the debug camera has no
// "enter" action and its native gate has not been found; DebugOverlay is the
// ASSERT SCREEN rather than a HUD, so it has no toggle to find.

#include "hooks/hook_common.h"
#include "hooks/hooks_d3d9.h"  // GuestRangeReadable

#include <rex/cvar.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>

// debug_native -- the engine's own "is this a debug build" predicate, which
// ships compiled to a constant false.
//
//     int sub_829E8FA8() { return 0; }
//
// It has 40+ call sites across asset loading, UI, render and audio, and it is
// the NATIVE counterpart of the Lua DEFINE_BuildConfig that debug_menu flips.
// The two are unrelated: debug_menu only reaches Lua and the UI scripts, so it
// cannot switch on anything the C++ gates -- which is why the dev menu appears
// but the debug camera does not.
//
// Two known consumers, both found while chasing the debug overlay:
//
//   sub_82AB6300  the DebugOverlay constructor. It loads the
//                 EngineDependencies database and package, resolves the
//                 DebugOverlay material, lays out seven HUD cells, and then
//                 sets its enabled field from this predicate:
//                     a1[217] = sub_829E8FA8();
//                 so the overlay builds itself and then marks itself off.
//   sub_82AB58F8  a screen/state poller, whose second branch is
//                     if (sub_829E8FA8() && sub_82B6F070(&unk_830C1140, 1, i))
//                 -- though with the folding below, that call may be a
//                 different stub that merely shares the address.
//
// CORRECTION, 2026-08-30: sub_829E8FA8 IS NOT ONE FUNCTION.
//
// It is the shared body of every `return 0;` in the image, folded together by
// the linker (identical COMDAT folding). The proof is that its callers cannot
// all be the same function -- one address cannot simultaneously be:
//
//     a Lua C function, registered as `print` in sub_82500760:
//         push "print"; push C closure sub_829E8FA8; set global
//     a boolean predicate:      if (sub_829E8FA8() && ...)
//     a stored flag:            a1[217] = sub_829E8FA8();
//
// Those are three incompatible signatures. So the "40+ call sites" the census
// reports are every STUBBED-OUT function in the binary sharing one body, not
// forty debug checks.
//
// This also corrects why `all` crashes. The first explanation here was
// "40 paths retail never executes" -- wrong. It crashes because it returns 1
// to callers that expect 0 from unrelated stubs, Lua's `print` among them.
//
// A consequence worth knowing: the guest's `print()` is that stub, so every
// print() in the shipped Lua discards its output. Nothing is being lost on our
// side. The same is true of the debug printing the dev menu leans on --
// Engine.DebugPrintTable (sub_824B31B0) and Engine.DebugPrintRows
// (sub_824B2700) fully validate their arguments (`Rdb::Table *`, `RdbRows *`,
// arg counts, type-mismatch messages) and then call nullsub_1. Measured: 7
// calls in one run, zero output. They are stubs, not a TTY channel we fail to
// capture.
//
// PER CALL SITE, and CHECK THE SITE FIRST. "census" reports which sites ask
// without changing an answer; decompile a site's caller before flipping it,
// because the census cannot tell a debug check from any other stub.
// 0x82AB6638 is verified: it is `a1[217] = sub_829E8FA8()` inside the
// DebugOverlay constructor.
//
//   debug_native=census        answer FALSE, report the distinct callers
//   debug_native=0x82AB6638    answer TRUE for that site only (verified)
//   debug_native=all           answer TRUE everywhere; crashes, see above
REXCVAR_DEFINE_STRING(debug_native, "", "Debug",
                      "The engine's is-debug-build predicate, per call site. "
                      "'census' lists the callers without changing anything; a "
                      "comma-separated list of hex return addresses answers "
                      "true for just those; 'all' answers true everywhere and "
                      "crashes");

namespace {
std::mutex g_dbgNativeMu;
std::map<uint32_t, uint64_t> g_dbgNativeAsks;   // lr -> times asked
uint64_t g_dbgNativeTrue = 0;
std::chrono::steady_clock::time_point g_dbgNativeLast{};
}  // namespace

REX_IMPORT(__imp__sub_829E8FA8, orig_IsDebugBuild, void());
extern "C" REX_FUNC(sub_829E8FA8) {
  const uint32_t lr = uint32_t(ctx.lr);
  orig_IsDebugBuild(ctx, base);
  const std::string spec = REXCVAR_GET(debug_native);
  if (spec.empty()) return;

  bool want = false;
  if (spec == "all") {
    want = true;
  } else if (spec != "census") {
    // Exact return addresses, comma separated. Matching on lr rather than on
    // the enclosing function keeps this to the one call the log named.
    size_t pos = 0;
    while (pos < spec.size() && !want) {
      size_t comma = spec.find(',', pos);
      if (comma == std::string::npos) comma = spec.size();
      const std::string one = spec.substr(pos, comma - pos);
      pos = comma + 1;
      if (one.empty()) continue;
      const uint32_t v = uint32_t(std::strtoul(one.c_str(), nullptr, 0));
      if (v && v == lr) want = true;
    }
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
  REXLOG_INFO("{}: debug_native CALLERS ({}) -- {} distinct site(s), {} answer(s) "
              "flipped to true:{}",
              mx::native::g_plugin_mode ? "plugin" : "native", spec,
              g_dbgNativeAsks.size(), g_dbgNativeTrue,
              rows.empty() ? " (none)" : rows);
}

// debug_binds -- put the engine's debug actions on the buttons its own shipped
// presets leave empty.
//
// sub_8230EB48 registers a full set of debug input ACTIONS by name --
// DebugCamera{Forward,Backward,Up,Down,ZoomIn,ZoomOut,EnableAccel,
// EnableTracking,EnableWorldAlignedMovement}, DebugEject{Active,
// AirGroundToggle}, DebugBarBangModifier1-4, DebugWreckAvoidanceModifier1-3,
// plus Spec{Next,Prev}Player, SwapToyVehicle, PreviewPerformance,
// QuitUIActivity, Replay{PlayPause,Rewind,FastForward} and
// UserCamQuick{Back,Right}.
//
// NONE of them is bound. assets/ControllerPresets.bxml ships six presets that
// between them reference only 20 gameplay actions, and eight inputs are empty
// in ALL six: Button7, Button14, Button15, Button16 and the four Sensor axes.
// The actions survived in code; the bindings were stripped from the data.
//
// THE TABLE, from the parser at sub_82308300:
//
//     v6 = sub_82B69BA8(buttonName);                       // button index
//     v9 = &unk_82DAA740 + 780 * (25 * presetIndex + v6);  // the entry
//     ... copy the action string to v9 + 260 * slot
//
// so entry(preset, button) = 0x82DAA740 + 780*(25*preset + button), holding
// three 260-byte action strings. Written AFTER the parse rather than by
// intercepting it, because the table is the parser's whole output and a
// post-pass cannot get the element walk wrong.
//
// Button indices are LEARNED, not assumed: sub_82B69BA8 maps a button name to
// its index and the parser calls it for every Input element, so hooking it
// records the real mapping. Guessing indices from XML order would be an
// assumption about a table this file cannot see.
//
// Two safety properties. A slot is written ONLY if the guest left it empty, so
// a real binding can never be clobbered. And the layout is VERIFIED before any
// write: a known shipped binding is read back and must match, or nothing is
// written and the log says so. Getting 780/25/260 wrong would otherwise
// scribble over the input table.
REXCVAR_DEFINE_STRING(debug_binds, "", "Debug",
                      "Comma-separated button=action pairs to add to every "
                      "controller preset, e.g. "
                      "Button7=DebugCameraForward,Button14=DebugCameraBackward. "
                      "Only slots the shipped presets leave empty are written");

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
  const std::string spec = REXCVAR_GET(debug_binds);
  if (spec.empty()) return;
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
  size_t pos = 0;
  while (pos < spec.size()) {
    size_t comma = spec.find(',', pos);
    if (comma == std::string::npos) comma = spec.size();
    std::string pair = spec.substr(pos, comma - pos);
    pos = comma + 1;
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
  REXLOG_INFO("{}: debug_binds applied {} binding(s) across {} presets, {} slot(s) "
              "left alone because the guest already bound them, {} unknown "
              "button name(s); {} button names learned",
              tag, applied, kPresetCount, skipped_taken, unknown,
              g_buttonIndex.size());
}

// DEFINE_BuildConfig -- the guest's own debug gate, flipped at its single
// source.
//
// sub_82500760 registers the engine's Lua globals. In disassembly (Hex-Rays
// renders this function as a __noreturn stub and truncates the rest, so it has
// to be read as asm):
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
// FE_Title.lua enables the dev menu on the same test, and RSLibrary.lua
// installs DebugPrintTable on it. The assets are all still in the packages:
// MXUI has DB_Menu (.lua/.layer.xml/.swfx), DB_UnitTests.lua and
// DB_GraphicsTest.swfx, EngineDependencies has DebugOverlay and DebugGraphics.
//
// SWAPPED AT THE PUSH, not by rewriting the global afterwards. "RELEASE"
// (0x820468E0) has exactly ONE xref in the binary -- this push -- so an
// equality test on the pointer cannot touch anything else, and it lands before
// the setfield rather than racing whatever reads the global first. "DEBUG"
// (0x8204E1E4) is an existing guest string, so no memory has to be written
// into the guest to supply the value. The scripts test for inequality against
// "RELEASE", so any other value would do.
REXCVAR_DEFINE_BOOL(debug_menu, false, "Debug",
                    "Set the guest's DEFINE_BuildConfig Lua global to DEBUG "
                    "instead of RELEASE, which is what its own scripts gate "
                    "the developer menu and debug printing on");

REX_IMPORT(__imp__sub_82A9F468, orig_LuaPushString, void());
extern "C" REX_FUNC(sub_82A9F468) {
  if (REXCVAR_GET(debug_menu) && ctx.r4.u32 == 0x820468E0u) {
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
