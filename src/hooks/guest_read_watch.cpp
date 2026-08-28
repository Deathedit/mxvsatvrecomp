#include "hooks/guest_read_watch.h"

#include <rex/logging.h>

#include <atomic>
#include <mutex>
#include <string>

#include <fmt/format.h>

#include <windows.h>  // WIN32_LEAN_AND_MEAN comes from the build definitions

namespace mx::watch {
namespace {

struct Range {
  uint8_t* host = nullptr;   // page-aligned start
  size_t bytes = 0;          // page-rounded length
  uint32_t guest = 0;        // guest address of the ORIGINAL request
  const char* what = "";
};

std::mutex g_mu;
Range g_range;  // one range is enough for the question being asked
bool g_armed = false;

// BUDGETS, because a guard page in a hot buffer is not free and the answer is
// binary. Once these are spent the watch disarms permanently: it has either
// seen a guest-code access or it has not, and re-arming forever would only add
// cost to a question already answered.
std::atomic<uint64_t> g_arms{0};
std::atomic<uint64_t> g_hitsTotal{0};
std::atomic<uint64_t> g_hitsGuestRead{0};
std::atomic<uint64_t> g_hitsGuestWrite{0};
std::atomic<uint64_t> g_hitsHost{0};
constexpr uint64_t kMaxArms = 400;

// Distinct host-code sites that touched the buffer, with a read/write split per
// site. Eight is plenty: the expectation is ONE (our writeback), and the whole
// point is to notice when it is two.
struct HostSite {
  uint64_t rva = 0;
  uint64_t reads = 0;
  uint64_t writes = 0;
};
constexpr size_t kMaxHostSites = 8;
HostSite g_hostSites[kMaxHostSites];
size_t g_hostSiteCount = 0;
uint64_t g_hostSiteOverflow = 0;

}  // namespace

void ArmGuestReadWatch(void* host_addr, size_t bytes, uint32_t guest_addr,
                       const char* what) {
  if (!host_addr || !bytes) return;
  if (g_arms.load(std::memory_order_relaxed) >= kMaxArms) return;

  // THROTTLED. The buffer is rewritten every frame and arming every time would
  // put a guard fault in the path of every access forever. One arm per 30
  // writebacks samples it at ~2 Hz, which is far more than enough to catch a
  // reader that runs per frame, and cheap enough to leave in.
  static uint64_t s_calls = 0;
  if ((s_calls++ % 30) != 0) return;

  auto* p = static_cast<uint8_t*>(host_addr);
  SYSTEM_INFO si = {};
  GetSystemInfo(&si);
  const uintptr_t page = si.dwPageSize ? si.dwPageSize : 4096;
  auto* start = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(p) &
                                           ~(page - 1));
  const size_t span =
      ((reinterpret_cast<uintptr_t>(p) + bytes) - reinterpret_cast<uintptr_t>(start)
       + page - 1) & ~(page - 1);

  // Preserve whatever protection the arena already has and only add the guard
  // bit -- the guest heap is not necessarily plain PAGE_READWRITE, and forcing
  // it would be a silent behaviour change far outside this probe's remit.
  MEMORY_BASIC_INFORMATION mbi = {};
  if (VirtualQuery(start, &mbi, sizeof(mbi)) != sizeof(mbi)) return;
  if (mbi.State != MEM_COMMIT) return;
  if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return;  // already armed

  DWORD old = 0;
  if (!VirtualProtect(start, span, mbi.Protect | PAGE_GUARD, &old)) return;

  {
    std::lock_guard<std::mutex> lk(g_mu);
    g_range = {start, span, guest_addr, what};
    g_armed = true;
  }
  const uint64_t n = g_arms.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n == 1)
    REXLOG_INFO(
        "native: READ WATCH armed on {} -- guest 0x{:08X}, {} bytes ({} pages). "
        "Reports every access until the budget of {} arms is spent",
        what, guest_addr, span, span / page, kMaxArms);
}

GuardHit OnGuardPageHit(uint64_t fault_addr, uint32_t* guest_addr,
                        const char** what) {
  std::lock_guard<std::mutex> lk(g_mu);
  // NOT gated on g_armed. A hit anywhere in a range we ever armed is OURS and
  // must be swallowed, and getting that wrong KILLED THE PROCESS five seconds
  // into run 1700.
  //
  // The guard covers 17 pages and Windows clears the bit for ONE page per
  // fault. Our own writeback walks the whole buffer, so the first page faulted,
  // the old code set g_armed = false, and the remaining 16 pages -- still
  // armed -- reported kNotOurs, fell through to EXCEPTION_CONTINUE_SEARCH, and
  // took the process down with no crash line. The log ends mid-writeback on
  // exactly that.
  //
  // So: the range stays claimed for the life of the process, and the whole
  // span is un-guarded on the FIRST hit rather than one page at a time.
  if (!g_range.host) return GuardHit::kNotOurs;
  const auto addr = reinterpret_cast<uintptr_t>(fault_addr);
  const auto lo = reinterpret_cast<uintptr_t>(g_range.host);
  if (addr < lo || addr >= lo + g_range.bytes) return GuardHit::kNotOurs;
  if (guest_addr)
    *guest_addr = g_range.guest + uint32_t(addr - lo);
  if (what) *what = g_range.what;
  if (g_armed) {
    // Drop the guard bit across the ENTIRE range so no sibling page can fault
    // behind us. VirtualProtect is safe from a vectored handler.
    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(g_range.host, &mbi, sizeof(mbi)) == sizeof(mbi)) {
      DWORD old = 0;
      VirtualProtect(g_range.host, g_range.bytes,
                     mbi.Protect & ~DWORD(PAGE_GUARD), &old);
    }
    g_armed = false;
  }
  return GuardHit::kOurs;
}

bool GuestReadWatchActive() {
  return g_arms.load(std::memory_order_relaxed) < kMaxArms;
}

void NoteGuestReadWatchAccess(bool from_guest_code, bool is_write,
                              uint64_t host_rva) {
  const uint64_t total = g_hitsTotal.fetch_add(1, std::memory_order_relaxed) + 1;
  // Sibling pages of the same access burst are swallowed above but still land
  // here; they are counted, because "one toucher walked 17 pages" and "17
  // separate touchers" are both interesting and the tally shows which.
  std::string sites;
  if (from_guest_code) {
    if (is_write) g_hitsGuestWrite.fetch_add(1, std::memory_order_relaxed);
    else g_hitsGuestRead.fetch_add(1, std::memory_order_relaxed);
  } else {
    g_hitsHost.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(g_mu);
    bool placed = false;
    for (size_t i = 0; i < g_hostSiteCount; ++i) {
      if (g_hostSites[i].rva != host_rva) continue;
      (is_write ? g_hostSites[i].writes : g_hostSites[i].reads)++;
      placed = true;
      break;
    }
    if (!placed) {
      if (g_hostSiteCount < kMaxHostSites) {
        auto& e = g_hostSites[g_hostSiteCount++];
        e.rva = host_rva;
        (is_write ? e.writes : e.reads) = 1;
      } else {
        ++g_hostSiteOverflow;
      }
    }
  }
  {
    std::lock_guard<std::mutex> lk(g_mu);
    for (size_t i = 0; i < g_hostSiteCount; ++i)
      sites += fmt::format(" [RVA 0x{:X} r{} w{}]", g_hostSites[i].rva,
                           g_hostSites[i].reads, g_hostSites[i].writes);
    if (g_hostSiteOverflow)
      sites += fmt::format(" (+{} sites dropped)", g_hostSiteOverflow);
  }
  // A RUNNING TALLY, not just a first sighting. "The guest read it once" and
  // "the guest reads it every frame" want different conclusions, and a single
  // line cannot tell them apart.
  if ((total % 16) == 0 || total <= 4)
    REXLOG_INFO(
        "native: READ WATCH tally -- {} accesses: GUEST-CODE reads {}, "
        "GUEST-CODE writes {}, host-side {} --{}",
        total, g_hitsGuestRead.load(std::memory_order_relaxed),
        g_hitsGuestWrite.load(std::memory_order_relaxed),
        g_hitsHost.load(std::memory_order_relaxed),
        sites.empty() ? std::string(" (no host sites)") : sites);
}

}  // namespace mx::watch
