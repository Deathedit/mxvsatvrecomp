// The registry chokepoint -- guest settings reads, and the override lever.
//
// Split verbatim out of hooks_plugin_diag.cpp, now hooks_script_diag.cpp.
// Self-contained: the four guest addresses and the parsed-override table below
// are named nowhere else, so this
// cut publishes no symbol at all. That is why it is its own translation unit
// rather than a section of a larger one.

#include "hooks/hook_common.h"

#include <rex/cvar.h>

#include <string>
#include <utility>
#include <vector>

using mx::hooks::GuestString;

// Every string setting the guest reads comes from MXRegistry.bxml through one
// function, sub_825487C8(registry, key, out, size, 0). `registry_override` takes
// comma-separated key=value pairs and substitutes the value for a matching key
// as it is read, which is what shipping a different MXRegistry.bxml would do --
// tools/ has bxml decoders but no encoder. Empty means off. Diagnostic only.
REXCVAR_DEFINE_STRING(registry_override, "", "Debug",
                      "Comma-separated key=value overrides for guest registry string reads");

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
    REXLOG_INFO("native: registry — key(0x{:08X})=\"{}\" key(0x{:08X})=\"{}\" key(0x{:08X})=\"{}\"",
                kSceneKey, GuestString(base, kSceneKey, 64), kModeKey,
                GuestString(base, kModeKey, 64), kGateKey,
                GuestString(base, kGateKey, 64));
    for (uint32_t i = 0; i < 5; ++i) {
      uint32_t p = REX_LOAD_U32(kModeTable + i * 4);
      REXLOG_INFO("native: registry — mode[{}] ptr=0x{:08X} \"{}\"", i, p,
                  GuestString(base, p, 64));
    }
  }

  orig_RegistryGetString(ctx, base);

  // First sighting per distinct key, capped so a per-frame reader cannot flood
  // the log. `out` is only meaningful when the lookup succeeded. `lr` is the
  // point of this line as much as the value is: native reads exactly one string
  // key in a whole run and the plugin reads four, so the callers of the three
  // extra ones are the boundary native stops short of.
  static std::vector<std::string> s_seen;
  if (s_seen.size() < 40 && std::find(s_seen.begin(), s_seen.end(), key) == s_seen.end()) {
    s_seen.push_back(key);
    REXLOG_INFO("native: registry get \"{}\" size={} -> r3={} value=\"{}\" from lr=0x{:08X}",
                key, size, ctx.r3.u32,
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
      REXLOG_INFO("native: registry OVERRIDE \"{}\" -> \"{}\" (size={})", key, v, size);
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
      REXLOG_INFO("native: registry OVERRIDE int \"{}\" -> {}", key, v);
    }
    break;
  }

  static std::vector<std::string> s_seen;
  if (s_seen.size() < 40 && std::find(s_seen.begin(), s_seen.end(), key) == s_seen.end()) {
    s_seen.push_back(key);
    REXLOG_INFO("native: registry getint \"{}\" -> {} from lr=0x{:08X}", key,
                out ? REX_LOAD_U32(out) : 0xFFFFFFFFu, from);
  }
}
