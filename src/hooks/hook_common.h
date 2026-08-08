#pragma once

// Shared internals for the guest hook translation units (hooks_*.cpp).
// Not part of any public interface — do not include outside src/hooks/.
//
// Every hook TU needs the same three things: the ReXGlue hook macros
// (REX_FUNC / REX_IMPORT / REX_LOAD_U32 / …), the generated guest symbol
// declarations from mx_init.h, and the plugin-mode flag they all branch on.

#include "hooks/native_bridge.h"

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/thread_state.h>

#include "mx_init.h"

namespace mx::native {

// Guest-stall watchdog. The slow-wait detector in hooks_wait.cpp logs only
// AFTER the wait returns, so a thread parked forever is invisible to it — which
// is precisely the case we need to see. These record waits at entry instead,
// and a watchdog thread reports what is still outstanding once the guest's
// MainLoop stops ticking.
void GuestWaitEnter(uint32_t handle, uint32_t timeout, uint32_t lr);
void GuestWaitLeave();
void GuestTick();  // MainLoop calls this once per iteration.

}  // namespace mx::native

namespace {

// Helper: log the current value of `*(dword_830BE400 + 8)` (the engine's
// "slot 2"). In plugin mode this is populated to a real object (0x40BCF740);
// in native mode Bootstrap leaves it NULL. Triangulating which SetupRenderer
// vtable call writes it requires dumping at multiple hook points.
inline void LogEngSlot8(uint8_t* base, const char* where) {
  uint32_t eng = REX_LOAD_U32(0x830BE400);
  uint32_t slot8 = eng ? REX_LOAD_U32(eng + 8) : 0;
  REXLOG_INFO("plugin: [{}] eng+8 = 0x{:08X} (eng=0x{:08X})", where, slot8, eng);
}

}  // namespace
