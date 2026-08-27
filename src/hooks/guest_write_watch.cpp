#include "hooks/guest_write_watch.h"

#include <rex/logging.h>

#include "hooks/native_bridge.h"

#include <atomic>

#include <windows.h>

namespace mx::hooks {
namespace {

// Fixed storage, no container. The last thing this file did with a std::map on
// guest threads was corrupt the heap and take the game down with an access
// violation half a subsystem away, so nothing here allocates, rehashes, or
// takes a lock. See the note in hooks_d3d9_texture.cpp's flat probe.
constexpr uint32_t kMaxRanges = 16;

struct Range {
  std::atomic<uint32_t> base{0};   // guest address, page-aligned
  std::atomic<uint32_t> bytes{0};  // 0 = slot free / disarmed
  std::atomic<uint64_t> host{0};   // host page-aligned start
  std::atomic<DWORD> restore{0};   // protection to put back on a hit
  // Set once by whichever thread reports first. The range itself is NEVER
  // cleared, and that is a correctness point rather than laziness: if thread A
  // faults, disarms and unprotects while thread B is already inside its own
  // handler for the same page, B would find no match, fall through to the
  // crash reporter and take the process down over a fault we caused on
  // purpose. Keeping the range means B still matches, still continues, and
  // only the reporting is one-shot.
  std::atomic<uint32_t> reported{0};
  const char* why = "";
};

Range g_ranges[kMaxRanges];
std::atomic<uint32_t> g_armed{0};

// Guest memory may be mapped executable; clearing write must not also clear
// execute, or the next guest call into that page faults for the wrong reason.
DWORD WithoutWrite(DWORD protect) {
  switch (protect & 0xFFu) {
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:
      return PAGE_READONLY;
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
      return PAGE_EXECUTE_READ;
    default:
      return 0;  // already unwritable, or something we should not touch
  }
}

}  // namespace

bool ArmGuestWriteWatch(uint32_t guest_addr, uint32_t bytes, const char* why) {
  if (!bytes) return false;
  uint8_t* gbase = mx::native::NativeGraphics::Get().GetGuestMemory();
  if (!gbase) return false;

  // Page-align outward so the whole requested span is covered.
  SYSTEM_INFO si{};
  GetSystemInfo(&si);
  const uint32_t page = si.dwPageSize ? si.dwPageSize : 4096u;
  const uint32_t base = guest_addr & ~(page - 1u);
  const uint32_t end = (guest_addr + bytes + page - 1u) & ~(page - 1u);
  const uint32_t span = end - base;

  for (uint32_t i = 0; i < kMaxRanges; ++i) {
    if (g_ranges[i].bytes.load(std::memory_order_acquire) &&
        g_ranges[i].base.load(std::memory_order_acquire) == base) {
      return false;  // already watching this one
    }
  }

  // THE MIRRORS, resolved here so every caller does not have to. A bare
  // physical guest address is MEM_RESERVE with protect 0 -- run 1475 refused
  // all three arms for exactly that reason -- and the same bytes are live at
  // one of four mirrored addresses. CopyGuestExtent in hooks_d3d9_texture.cpp
  // is the source of truth for this list; keep them in step.
  const uint32_t kMirrors[] = {0u, 0xA0000000u, 0xC0000000u, 0xE0000000u};
  void* host = nullptr;
  MEMORY_BASIC_INFORMATION mbi{};
  DWORD stripped = 0;
  uint32_t chosen = 0;
  for (uint32_t m : kMirrors) {
    void* candidate = gbase + (base | m);
    MEMORY_BASIC_INFORMATION probe{};
    if (!VirtualQuery(candidate, &probe, sizeof(probe))) continue;
    if (probe.State != MEM_COMMIT) continue;
    const DWORD s2 = WithoutWrite(probe.Protect);
    if (!s2) continue;
    host = candidate;
    mbi = probe;
    stripped = s2;
    chosen = base | m;
    break;
  }
  if (!host) {
    REXLOG_INFO(
        "d3d9: WRITE WATCH refused 0x{:08X}: no writable mirror "
        "(bare/A0/C0/E0 all uncommitted or read-only)",
        base);
    return false;
  }
  // VirtualProtect FAILS OUTRIGHT if the span crosses a region boundary, and a
  // 2 MB texture very easily does. Clamp to what this region actually covers
  // rather than asking for the whole span and getting nothing -- a partial
  // watch on the first pages still names the writer, and silently refusing
  // would leave the log looking as though the probe had simply not fired.
  const uint64_t region_end =
      reinterpret_cast<uint64_t>(mbi.BaseAddress) + mbi.RegionSize;
  (void)chosen;
  uint32_t protect_span = span;
  if (reinterpret_cast<uint64_t>(host) + span > region_end)
    protect_span = uint32_t(region_end - reinterpret_cast<uint64_t>(host));
  if (!protect_span) {
    REXLOG_INFO("d3d9: WRITE WATCH refused 0x{:08X}: zero-length region", base);
    return false;
  }

  for (uint32_t i = 0; i < kMaxRanges; ++i) {
    uint32_t expected = 0;
    if (!g_ranges[i].bytes.compare_exchange_strong(expected, protect_span,
                                                   std::memory_order_acq_rel)) {
      continue;
    }
    DWORD old = 0;
    if (!VirtualProtect(host, protect_span, stripped, &old)) {
      REXLOG_INFO(
          "d3d9: WRITE WATCH refused 0x{:08X}: VirtualProtect({} KB, 0x{:X}) "
          "failed ({})",
          base, protect_span / 1024, stripped, GetLastError());
      g_ranges[i].bytes.store(0, std::memory_order_release);
      return false;
    }
    g_ranges[i].base.store(chosen, std::memory_order_release);
    g_ranges[i].host.store(reinterpret_cast<uint64_t>(host),
                           std::memory_order_release);
    g_ranges[i].restore.store(old, std::memory_order_release);
    g_ranges[i].why = why ? why : "";
    ++g_armed;
    REXLOG_INFO(
        "d3d9: WRITE WATCH armed on guest 0x{:08X}..0x{:08X} ({} KB, host "
        "0x{:016X}, protect 0x{:X} -> 0x{:X}) -- {}",
        chosen, chosen + protect_span, protect_span / 1024,
        reinterpret_cast<uint64_t>(host), old, stripped, g_ranges[i].why);
    if (protect_span < span) {
      REXLOG_INFO(
          "d3d9: WRITE WATCH   clamped from {} KB to the region boundary; the "
          "tail 0x{:08X}..0x{:08X} is NOT watched",
          span / 1024, base + protect_span, end);
    }
    return true;
  }
  REXLOG_INFO("d3d9: WRITE WATCH refused 0x{:08X}: all {} slots in use", base,
              kMaxRanges);
  return false;
}

WriteWatchHit NoteGuestWrite(uint64_t host_fault, uint64_t guest_base) {
  WriteWatchHit hit;
  if (!guest_base || !g_armed.load(std::memory_order_acquire)) return hit;
  for (uint32_t i = 0; i < kMaxRanges; ++i) {
    const uint32_t bytes = g_ranges[i].bytes.load(std::memory_order_acquire);
    if (!bytes) continue;
    const uint64_t host = g_ranges[i].host.load(std::memory_order_acquire);
    if (host_fault < host || host_fault >= host + bytes) continue;

    // Restore write access unconditionally -- idempotent, and it is what lets
    // the faulting instruction re-execute and succeed.
    DWORD old = 0;
    VirtualProtect(reinterpret_cast<void*>(host), bytes,
                   g_ranges[i].restore.load(std::memory_order_acquire), &old);
    // Matched either way, so a second thread in the same page continues
    // instead of crashing; only the FIRST one reports.
    hit.matched = true;
    uint32_t unreported = 0;
    hit.first = g_ranges[i].reported.compare_exchange_strong(
        unreported, 1, std::memory_order_acq_rel);
    hit.guest_addr = static_cast<uint32_t>(host_fault - guest_base);
    hit.range_base = g_ranges[i].base.load(std::memory_order_acquire);
    hit.range_bytes = bytes;
    hit.why = g_ranges[i].why;
    return hit;
  }
  return hit;
}

}  // namespace mx::hooks
