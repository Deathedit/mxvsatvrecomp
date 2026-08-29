#include "hooks/guest_coherency.h"

#include <atomic>
#include <mutex>

namespace mx::coherency {
namespace {

struct Range {
  uint32_t base = 0;
  uint32_t size = 0;
  uint64_t claims = 0;
};

// Small and fixed. The live census saw five distinct TC ranges in two minutes,
// so this is generous; overflow is counted rather than silently dropping a
// claim, because a missed claim reads as "the guest does not own this" and that
// is the wrong direction to fail in.
constexpr uint32_t kMaxRanges = 16;
std::mutex g_mu;
Range g_ranges[kMaxRanges];
uint32_t g_rangeCount = 0;
std::atomic<uint64_t> g_overflow{0};
std::atomic<uint64_t> g_considered{0};
std::atomic<uint64_t> g_suppressed{0};

// The guest arena's mirrors, matching REX_PHYS_HOST_OFFSET: everything at or
// above 0xE0000000 is the +0x1000 physical window. Kept here rather than at the
// call site so the two consumers cannot disagree about it -- the log line
// "resolve dest addr 0xFCF89000 (phys 0x1CF8A000)" is the check that this is
// right, and 0xFA2DC000 -> 0x1A2DD000 is the case that matters.
uint32_t ToPhysical(uint32_t addr) {
  const uint32_t masked = addr & 0x1FFFFFFFu;
  return addr >= 0xE0000000u ? masked + 0x1000u : masked;
}

}  // namespace

void NoteGuestWroteRange(uint32_t phys_base, uint32_t size) {
  if (!size) return;  // the degenerate base 0x1000 size 0 request
  std::lock_guard<std::mutex> lk(g_mu);
  for (uint32_t i = 0; i < g_rangeCount; ++i) {
    if (g_ranges[i].base == phys_base && g_ranges[i].size == size) {
      ++g_ranges[i].claims;
      return;
    }
  }
  if (g_rangeCount >= kMaxRanges) {
    g_overflow.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  g_ranges[g_rangeCount++] = {phys_base, size, 1};
}

bool GuestOwnsRange(uint32_t addr, uint32_t bytes) {
  if (!bytes) return false;
  const uint32_t lo = ToPhysical(addr);
  const uint64_t hi = uint64_t(lo) + bytes;
  std::lock_guard<std::mutex> lk(g_mu);
  for (uint32_t i = 0; i < g_rangeCount; ++i) {
    const uint64_t rlo = g_ranges[i].base;
    const uint64_t rhi = rlo + g_ranges[i].size;
    if (lo >= rlo && hi <= rhi) return true;
  }
  return false;
}

void NoteClaimedWriteback(bool claimed) {
  g_considered.fetch_add(1, std::memory_order_relaxed);
  if (claimed) g_suppressed.fetch_add(1, std::memory_order_relaxed);
}

uint64_t ClaimedWritebacks() {
  return g_suppressed.load(std::memory_order_relaxed);
}

uint64_t ConsideredWritebacks() {
  return g_considered.load(std::memory_order_relaxed);
}

uint32_t ClaimedRangeCount() {
  std::lock_guard<std::mutex> lk(g_mu);
  return g_rangeCount;
}

}  // namespace mx::coherency
