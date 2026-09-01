#include "gpu/health.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <vector>

#include <fmt/format.h>

namespace mx::gpu::health {
namespace {

struct Check {
  Verdict verdict = Verdict::kUnmeasured;
  uint64_t value = 0;
  uint64_t population = 0;
  std::string expectation;   // rendered, e.g. "expected 0"
  uint64_t cycle = 0;        // the Report() pass that last updated it
  bool announced = false;    // has a WARN already been emitted for this BAD

  // How often this check actually updates, measured rather than assumed.
  //
  // A FIXED TOLERANCE DOES NOT WORK, and shipping one proved it inside an hour:
  // the d3d9 report pass runs ~3x a second and the frame pass every ~4 seconds,
  // so the frame check updates once per ~12 cycles and a 4-cycle rule reported
  // it permanently STALE. That is a false alarm in the one mechanism whose
  // entire job is to be believed, which is worse than not having it.
  //
  // So each check calibrates itself: `max_gap` is the largest interval it has
  // ever been seen to take, and staleness is a multiple of that. The maximum
  // only ever grows, so the bound only ever loosens -- it cannot start crying
  // wolf because one pass ran late, and it still catches a site that has
  // stopped entirely, just later.
  uint64_t updates = 0;      // how many times it has reported
  uint64_t max_gap = 0;      // widest observed cycle gap between updates
};

std::mutex& Mu() {
  static std::mutex m;
  return m;
}

// std::map, not unordered: the report order is the check order, so a line
// cannot reshuffle between two runs and look like a change.
std::map<std::string, Check>& Checks() {
  static std::map<std::string, Check> m;
  return m;
}

uint64_t g_cycle = 0;

// EVERY CHECK THIS BUILD CONTAINS. This array is the DENOMINATOR, and it is the
// reason it exists: a check only appeared in the report once it had been
// evaluated, so one that never ran was ABSENT rather than unmeasured -- exactly
// the "a zero you never took is not a healthy zero" failure this module was
// written to stop, committed inside the module.
//
// It was not hypothetical. Run mx_1900 printed "(of 11)" against twelve wired
// checks: gpu_fetch.address_mismatch sits behind the g_diag gate and needs 400
// qualifying draws, so its self-check line never printed and the check silently
// did not exist. Nothing in the report could have said so.
//
// Report() now materialises every name below, so a check that never ran is
// listed as UNMEASURED and NAMED. Adding a check means adding its name here;
// forgetting to costs nothing but the guarantee, and a name here that no call
// site ever updates is reported as unmeasured forever, which is the correct
// answer for a check that is compiled in and never reached.
constexpr const char* kDeclared[] = {
    "decl.addr_reused",
    "decl.layout_refused",
    "decl.table_full",
    "decl.unknown_ptr",
    "draws.gap_unattributed",
    "gpu_fetch.address_mismatch",
    "gpu_fetch.ordinal_mismatch",
    "index_cond.registers_read",
    "stencil.config_keys_missing",
    "stencil.unreadable_reg",
    "transcode.lost",
    "vt.rowhi_matches_b0",
};

// Checks that turned BAD since the last drain, rendered. The caller logs them;
// see the note in Record().
std::vector<std::string>& NewlyBad() {
  static std::vector<std::string> v;
  return v;
}

// logs/health.txt, truncated once per process and appended thereafter.
//
// A SEPARATE FILE BECAUSE THE MAIN LOG ROTATES. logs/mx_NNNN.log holds only the
// last ~30 seconds and its segments are overwritten while a run is still going
// -- three empty greps over a rotated segment nearly became a false theory
// once. Findings have to outlive the window they were found in, so they go
// somewhere nothing overwrites for the life of the process.
std::ofstream& File() {
  static std::ofstream f = [] {
    // Overridable so the unit test cannot truncate a real run's findings just
    // by being run. The default is the only path anything else looks at.
    const char* env = std::getenv("MX_HEALTH_FILE");
    const std::string path = env && *env ? env : "logs/health.txt";
    std::error_code ec;
    std::filesystem::create_directories(
        std::filesystem::path(path).parent_path(), ec);
    std::ofstream out(path, std::ios::trunc);
    out << "# Health checks that FAILED, in the order they first failed.\n"
        << "# Every line here is an expectation stated in the source that the\n"
        << "# run did not meet. An empty file below this header means nothing\n"
        << "# a check covers went wrong -- NOT that nothing went wrong.\n\n";
    return out;
  }();
  return f;
}

// Shared tail of the three entry points. Records the measurement, and the first
// time a check goes BAD says so once at WARN and once in the file.
//
// ONCE, not every cycle: reports run several times a minute and a check that
// stays bad would otherwise bury everything else. The per-cycle state is never
// hidden, because Report() prints every check's current verdict regardless.
Verdict Record(const char* name, Verdict v, uint64_t value, uint64_t population,
               std::string expectation) {
  std::lock_guard<std::mutex> lock(Mu());
  Check& c = Checks()[name];
  const bool was_bad = c.verdict == Verdict::kBad;
  c.verdict = v;
  c.value = value;
  c.population = population;
  c.expectation = std::move(expectation);
  if (c.updates) {
    const uint64_t gap = g_cycle - c.cycle;
    if (gap > c.max_gap) c.max_gap = gap;
  }
  ++c.updates;
  c.cycle = g_cycle;

  if (v == Verdict::kBad && !was_bad && !c.announced) {
    c.announced = true;
    // The durable half happens here; the WARN does not. This module has no
    // logging dependency ON PURPOSE -- guard_census.cpp is arranged the same
    // way, and it is what lets both be built and tested standalone without
    // reproducing the SDK's spdlog include wiring. The caller drains the list
    // and logs it.
    NewlyBad().push_back(fmt::format("{} = {} of {} ({})", name, value,
                                     population, c.expectation));
    File() << "BAD  " << name << " = " << value << " of " << population << "  ("
           << c.expectation << ")\n";
    File().flush();
  }
  // Recovering re-arms the announcement, so a check that goes bad, is fixed,
  // and regresses says so a second time.
  if (v != Verdict::kBad) c.announced = false;
  return v;
}

}  // namespace

const char* Tag(Verdict v) {
  switch (v) {
    case Verdict::kOk:         return "ok";
    case Verdict::kBad:        return "BAD";
    case Verdict::kUnmeasured: return "??";
    case Verdict::kStale:      return "stale";
  }
  return "?";
}

Verdict Zero(const char* name, uint64_t value, uint64_t population) {
  const Verdict v = population == 0 ? Verdict::kUnmeasured
                    : value == 0    ? Verdict::kOk
                                    : Verdict::kBad;
  return Record(name, v, value, population, "expected 0");
}

Verdict NonZero(const char* name, uint64_t value, uint64_t population) {
  const Verdict v = population == 0 ? Verdict::kUnmeasured
                    : value != 0    ? Verdict::kOk
                                    : Verdict::kBad;
  return Record(name, v, value, population, "expected non-zero");
}

Verdict Equal(const char* name, uint64_t got, uint64_t want) {
  const Verdict v = want == 0  ? Verdict::kUnmeasured
                    : got == want ? Verdict::kOk
                                  : Verdict::kBad;
  return Record(name, v, got, want, fmt::format("expected all {}", want));
}

Verdict AtMost(const char* name, uint64_t value, uint64_t population,
               double max_percent) {
  Verdict v = Verdict::kUnmeasured;
  if (population != 0) {
    const double pct = double(value) * 100.0 / double(population);
    v = pct <= max_percent ? Verdict::kOk : Verdict::kBad;
  }
  return Record(name, v, value, population,
                fmt::format("expected <= {:.1f}%", max_percent));
}

std::vector<std::string> DrainNewlyBad() {
  std::lock_guard<std::mutex> lock(Mu());
  std::vector<std::string> out;
  out.swap(NewlyBad());
  return out;
}

size_t DeclaredCount() { return std::size(kDeclared); }

bool AnyBad() {
  std::lock_guard<std::mutex> lock(Mu());
  for (const auto& [name, c] : Checks()) {
    if (c.verdict == Verdict::kBad) return true;
  }
  return false;
}

std::string Report() {
  std::lock_guard<std::mutex> lock(Mu());
  // OPEN THE FILE EVEN IF NOTHING IS WRONG.
  //
  // File() is a static local, so it used to be created on the first BAD -- and
  // a run with no BAD never opened it at all, which left the PREVIOUS run's
  // failures sitting in logs/health.txt with the previous run's timestamp,
  // reading as current. Run mx_1908 was the first clean run and it inherited
  // mx_1907's line verbatim.
  //
  // That is stale data reading as live, in the one file whose job is to say
  // what went wrong -- the same shape of defect as a zero that was never
  // measured reading as healthy, and it was committed here twice over. Opening
  // on the first report makes a clean run leave a header and nothing else,
  // which is what the header already promises.
  (void)File();
  // Materialise the full set first, so the total is structural rather than
  // "whatever happened to run". A name inserted here and never updated keeps
  // updates == 0, which reads as UNMEASURED and is never called stale.
  for (const char* name : kDeclared) Checks()[name];

  uint32_t ok = 0, bad = 0, unmeasured = 0, stale = 0;
  std::string bad_list, unmeasured_list, stale_list;

  for (auto& [name, c] : Checks()) {
    // A check that has stopped updating is not answering any more. Reporting
    // its last verdict would be reporting a measurement no longer being taken,
    // which is the same defect as a counter that cannot fire -- so it is
    // called out rather than repeated.
    //
    // The bound is the check's OWN measured cadence, not a constant -- see the
    // note on Check::max_gap for why a constant cannot work here.
    //
    // A check is never called stale until it has reported at least twice,
    // because until then there is no cadence to compare against and every
    // slow check would be stale during warm-up. "Not enough information yet"
    // is not a finding, and reporting it as one is the same error as reading a
    // zero out of zero as healthy.
    constexpr uint64_t kStaleSlack = 4;
    const uint64_t tolerance = 2 * c.max_gap + kStaleSlack;
    const bool fresh =
        c.updates < 2 || (g_cycle - c.cycle) <= tolerance;
    const Verdict v = fresh ? c.verdict : Verdict::kStale;
    switch (v) {
      case Verdict::kOk:
        ++ok;
        break;
      case Verdict::kBad:
        ++bad;
        bad_list += fmt::format("{}{}={}/{} ({})", bad_list.empty() ? "" : "; ",
                                name, c.value, c.population, c.expectation);
        break;
      case Verdict::kUnmeasured:
        ++unmeasured;
        unmeasured_list +=
            fmt::format("{}{}", unmeasured_list.empty() ? "" : "; ", name);
        break;
      case Verdict::kStale:
        ++stale;
        stale_list += fmt::format("{}{}", stale_list.empty() ? "" : "; ", name);
        break;
    }
  }

  // The next pass writes into a new cycle. Advanced AFTER the walk so the pass
  // being reported is the one the checks stamped.
  ++g_cycle;

  const uint32_t total = ok + bad + unmeasured + stale;
  // Counts first and always, zero included, so "0 BAD" is a statement rather
  // than an absent line -- and the total beside them, because a shrinking
  // number of checks is itself a finding.
  std::string out = fmt::format("{} ok, {} BAD, {} unmeasured, {} stale (of {})",
                                ok, bad, unmeasured, stale, total);
  if (bad) out += " | BAD: " + bad_list;
  // Named, not just counted. An unmeasured check is an invitation to run the
  // scene that would measure it, and that is only actionable with the name.
  if (unmeasured) out += " | UNMEASURED: " + unmeasured_list;
  if (stale) out += " | STALE: " + stale_list;
  return out;
}

}  // namespace mx::gpu::health
