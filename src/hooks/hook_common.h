#pragma once

// Shared internals for the guest hook translation units (hooks_*.cpp).
// Not part of any public interface — do not include outside src/hooks/.
//
// Every hook TU needs the same two things: the ReXGlue hook macros
// (REX_FUNC / REX_IMPORT / REX_LOAD_U32 / …), and the generated guest symbol
// declarations from mx_init.h.

#include "hooks/native_bridge.h"

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/thread_state.h>

#include "mx_init.h"

#include <cstddef>
#include <string>

namespace mx::hooks {

// Read a NUL-terminated guest string for logging. Bounded at 260 because that
// is the buffer size sub_82534980 copies into.
//
// Lives here rather than in one TU's anonymous namespace because three of them
// need it -- the load-request chain, the registry getters and the script
// probes. Pure: it reads guest memory and allocates, and touches no state.
inline std::string GuestString(uint8_t* base, uint32_t addr, size_t max = 260) {
  std::string s;
  if (!addr) return s;
  for (size_t i = 0; i < max; ++i) {
    uint8_t c = REX_LOAD_U8(addr + static_cast<uint32_t>(i));
    if (!c) break;
    s.push_back(static_cast<char>(c));
  }
  return s;
}

}  // namespace mx::hooks

// The guest-stall watchdog declarations (GuestWaitEnter / GuestWaitLeave /
// GuestTick) are REMOVED with their definitions in hooks_wait.cpp. It was armed
// by GuestTick(), which only the MainLoop hook called, so once that hook went
// the watchdog thread was never started -- and the entry/leave pair cost a
// process-wide mutex on every guest wait to feed it.


