#include "gpu/guard_census.h"

#include <atomic>
#include <array>

#include <fmt/format.h>

namespace mx::gpu::guard {

namespace {

// Relaxed atomics, not a mutex. These sit on the draw path -- the stand-in site
// alone reached 225,000 opportunities in one run -- and the census only ever
// needs to be right to within a draw. A lock here would be measuring the
// measurement.
struct Counters {
  std::atomic<uint64_t> fires{0};
  std::atomic<uint64_t> population{0};
};

std::array<Counters, size_t(Guard::kCount)> g_counters;

constexpr const char* kNames[size_t(Guard::kCount)] = {
    "stand-in-draw",        "scratch-colour-target",
    "blank-texture",        "constant-nan-to-zero",
    "material-gate-fill",   "vertex-zero-fill",
    "interpolator-zero-fill",
};

}  // namespace

const char* Name(Guard g) {
  const size_t i = size_t(g);
  return i < size_t(Guard::kCount) ? kNames[i] : "?";
}

void Note(Guard g, bool fired, uint64_t weight) {
  const size_t i = size_t(g);
  if (i >= size_t(Guard::kCount) || !weight) return;
  g_counters[i].population.fetch_add(weight, std::memory_order_relaxed);
  if (fired) g_counters[i].fires.fetch_add(weight, std::memory_order_relaxed);
}

std::string Report() {
  std::string out;
  for (size_t i = 0; i < size_t(Guard::kCount); ++i) {
    const uint64_t f = g_counters[i].fires.load(std::memory_order_relaxed);
    const uint64_t p = g_counters[i].population.load(std::memory_order_relaxed);
    // EVERY guard, zero included, and the population always beside the fires.
    // A guard at 0/0 has never been reached and is a different finding from one
    // at 0/225000, which is reached constantly and never needed -- that second
    // one is a guard that can simply be deleted, and it is the cheapest win
    // this census can produce.
    // "NOT WIRED" is not the same finding as "reached and never fired", and
    // with both printed as 0/0 they were indistinguishable -- the exact defect
    // this census exists to prevent, committed inside the census itself. A site
    // that has never called Note() at all cannot be told from one that is
    // called constantly and declines, so say which.
    out += fmt::format(" [{} {}/{}{}]", kNames[i], f, p,
                       p ? fmt::format(" {:.1f}%", double(f) * 100.0 / double(p))
                         : " NOT WIRED");
  }
  return out;
}

}  // namespace mx::gpu::guard
