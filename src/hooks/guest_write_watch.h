#pragma once

// GUEST WRITE WATCH -- name the guest function that fills a buffer.
//
// Some guest allocations are runtime addresses with no static identity, so
// there is nothing in the XEX to search for: IDA can show code that writes
// SOME texture, never code that writes THIS one. The terrain's 1024x1024
// k_4_4_4_4 tile-index map is the case that forced this -- it decodes to one
// repeated word (0x0AF0) straight out of guest memory, and an hour of static
// analysis could not say who put it there. RTTI has no xrefs in this IDB and a
// whole-image immediate search times out.
//
// Watching the pages answers it directly: strip write access, take the fault,
// resolve the host RIP through PPCFuncMappings, and print the guest function
// and its LR. The crash reporter in app/mx_app.cpp already does every one of
// those steps on the way down; this reuses them and continues execution
// instead.
//
// ONE SHOT PER RANGE, deliberately. Re-arming would turn a buffer the guest
// writes in a loop into a fault storm, and the first writer is the answer.
//
// MEASUREMENT ONLY. Nothing acts on a hit, and a range that is never written
// is a result too: it says the fill happened before the watch was armed, or
// does not happen at all.

#include <cstdint>

namespace mx::hooks {

// Strip write access from the host pages backing [guest_addr, +bytes).
// `why` is a static string printed with the hit; it is NOT copied.
// Returns false when the range is full, already armed, or unprotectable.
bool ArmGuestWriteWatch(uint32_t guest_addr, uint32_t bytes, const char* why);

struct WriteWatchHit {
  bool matched = false;
  // True only for the FIRST thread to hit this range. Callers log on `first`
  // and continue on `matched` -- see the note on Range::reported.
  bool first = false;
  uint32_t guest_addr = 0;    // the exact address written
  uint32_t range_base = 0;    // the armed range it fell in
  uint32_t range_bytes = 0;
  const char* why = "";
};

// Called from the vectored exception handler on a WRITE fault. If the address
// is inside an armed range this restores the original protection, disarms the
// range, and reports it so the caller can log and continue. Allocation-free
// and lock-free: it runs inside an exception handler, where taking a std::mutex
// the faulting thread might already hold would deadlock rather than report.
WriteWatchHit NoteGuestWrite(uint64_t host_fault, uint64_t guest_base);

}  // namespace mx::hooks
