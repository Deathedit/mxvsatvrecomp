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

// The guest-stall watchdog declarations (GuestWaitEnter / GuestWaitLeave /
// GuestTick) are REMOVED with their definitions in hooks_wait.cpp. It was armed
// by GuestTick(), which only the MainLoop hook called, so once that hook went
// the watchdog thread was never started -- and the entry/leave pair cost a
// process-wide mutex on every guest wait to feed it.


