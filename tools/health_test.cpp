// Ground-truth tests for the health census.
//
// Run through tools/run_tests.py, or by hand:
//   clang++ -std=c++23 -I src -I C:/rexglue-sdk/include -o health_test.exe \
//     tools/health_test.cpp src/gpu/health.cpp C:/rexglue-sdk/lib/rexruntime.lib
//
// THE ASSERTION THAT MATTERS IS THE ZERO-OUT-OF-ZERO ONE. This module exists to
// stop a zero that means "never had the chance to fire" from reading as
// "healthy", and the way that fails is for kUnmeasured to quietly collapse into
// kOk. Every primitive is therefore checked at all three populations, and the
// report tallies as well -- a verdict that is right per call and miscounted in
// the summary is the same defect one layer along.

#include <cstdio>
#include <cstdlib>
#include <string>

#include "gpu/health.h"

namespace {

int g_failures = 0;

void Fail(const char* what, const std::string& detail) {
  std::printf("  FAIL %-46s %s\n", what, detail.c_str());
  ++g_failures;
}

namespace h = mx::gpu::health;

void CheckVerdict(const char* what, h::Verdict got, h::Verdict want) {
  if (got == want) return;
  Fail(what, std::string("got ") + h::Tag(got) + ", want " + h::Tag(want));
}

void CheckContains(const char* what, const std::string& hay,
                   const std::string& needle) {
  if (hay.find(needle) != std::string::npos) return;
  Fail(what, "\"" + needle + "\" not in: " + hay);
}

void CheckAbsent(const char* what, const std::string& hay,
                 const std::string& needle) {
  if (hay.find(needle) == std::string::npos) return;
  Fail(what, "\"" + needle + "\" unexpectedly in: " + hay);
}

// Each primitive at all three populations. The middle column is the one the
// whole module exists for.
void CheckVerdicts() {
  std::printf("verdicts\n");

  CheckVerdict("Zero: 0 of 100 is ok", h::Zero("t.zero.ok", 0, 100),
               h::Verdict::kOk);
  CheckVerdict("Zero: 5 of 100 is BAD", h::Zero("t.zero.bad", 5, 100),
               h::Verdict::kBad);
  // THE ONE. A zero measured against nothing is not a healthy zero.
  CheckVerdict("Zero: 0 of 0 is UNMEASURED, not ok",
               h::Zero("t.zero.unmeasured", 0, 0), h::Verdict::kUnmeasured);

  CheckVerdict("NonZero: 5 of 100 is ok", h::NonZero("t.nz.ok", 5, 100),
               h::Verdict::kOk);
  CheckVerdict("NonZero: 0 of 100 is BAD", h::NonZero("t.nz.bad", 0, 100),
               h::Verdict::kBad);
  CheckVerdict("NonZero: 0 of 0 is UNMEASURED, not BAD",
               h::NonZero("t.nz.unmeasured", 0, 0), h::Verdict::kUnmeasured);

  CheckVerdict("Equal: 3 of 3 is ok", h::Equal("t.eq.ok", 3, 3),
               h::Verdict::kOk);
  CheckVerdict("Equal: 2 of 3 is BAD", h::Equal("t.eq.bad", 2, 3),
               h::Verdict::kBad);
  CheckVerdict("Equal: 0 of 0 is UNMEASURED",
               h::Equal("t.eq.unmeasured", 0, 0), h::Verdict::kUnmeasured);

  CheckVerdict("AtMost: 1%% under a 5%% bound is ok",
               h::AtMost("t.pct.ok", 1, 100, 5.0), h::Verdict::kOk);
  CheckVerdict("AtMost: 10%% over a 5%% bound is BAD",
               h::AtMost("t.pct.bad", 10, 100, 5.0), h::Verdict::kBad);
  CheckVerdict("AtMost: the bound is INCLUSIVE",
               h::AtMost("t.pct.edge", 5, 100, 5.0), h::Verdict::kOk);
  CheckVerdict("AtMost: 0 of 0 is UNMEASURED",
               h::AtMost("t.pct.unmeasured", 0, 0, 5.0),
               h::Verdict::kUnmeasured);

  if (!h::AnyBad()) Fail("AnyBad sees the bad ones", "returned false");
}

// The summary has to agree with the verdicts, and has to NAME the bad and
// unmeasured ones -- a count alone is not actionable, which is the whole
// complaint that produced this module.
void CheckReport() {
  std::printf("report\n");
  const std::string r = h::Report();

  // 4 ok from CheckVerdicts (zero.ok, nz.ok, eq.ok, pct.ok) plus pct.edge = 5.
  CheckContains("report counts ok", r, "5 ok");
  CheckContains("report counts BAD", r, "4 BAD");
  // 4 unmeasured of this test's own, plus every declared check, none of
  // which this test ever calls. Derived from DeclaredCount() rather than
  // written out, so adding a real check does not redden the test.
  CheckContains("report counts unmeasured", r,
                std::to_string(4 + h::DeclaredCount()) + " unmeasured");
  CheckContains("report carries the total", r,
                "(of " + std::to_string(13 + h::DeclaredCount()) + ")");

  CheckContains("BAD entries are named", r, "t.zero.bad=5/100");
  CheckContains("BAD entries carry the expectation", r, "expected 0");
  CheckContains("UNMEASURED entries are named", r, "t.zero.unmeasured");

  // An ok check must NOT be listed by name: the lists exist so that what is
  // printed is what needs acting on.
  CheckAbsent("ok entries are not listed by name", r, "t.zero.ok=");
}

// A check that stops being evaluated must not keep reporting its last verdict.
// This is the counter-that-cannot-fire failure in its other form: the site went
// away and the number froze.
void CheckStaleness() {
  std::printf("staleness\n");

  // WARM-UP FIRST. A check seen only once has no cadence to compare against,
  // and calling it stale is the false alarm that shipped: the frame report
  // path updates once per ~12 d3d9 cycles, and a fixed 4-cycle rule marked it
  // permanently STALE in the first run after the mechanism landed.
  h::Zero("t.stale.subject", 0, 100);
  CheckAbsent("a just-updated check is not stale", h::Report(), "STALE");
  for (int i = 0; i < 12; ++i) {
    CheckAbsent("a check seen ONCE is never stale, however long",
                h::Report(), "t.stale.subject");
  }

  // Now give it a cadence: a second update establishes max_gap, which is the
  // 13 cycles that just elapsed. Tolerance is 2 * max_gap + 4.
  h::Zero("t.stale.subject", 0, 100);
  // The bound is INCLUSIVE, so a gap of exactly `kTolerance` is still fresh
  // and the check can only trip on the one after it.
  const uint64_t kGap = 13, kTolerance = 2 * kGap + 4;
  for (uint64_t i = 0; i <= kTolerance; ++i) {
    const std::string r = h::Report();
    if (r.find("t.stale.subject") != std::string::npos) {
      Fail("stale only AFTER the measured tolerance",
           "tripped at cycle " + std::to_string(i + 1) + " of " +
               std::to_string(kTolerance));
      break;
    }
  }
  const std::string late = h::Report();
  CheckContains("a check that stopped updating goes STALE", late, "STALE");
  CheckContains("the stale check is named", late, "t.stale.subject");
  // And it is no longer counted as ok, which is the point of the state.
  CheckAbsent("a stale check is not still counted ok", late, "6 ok");
}

// A check the build contains but that never ran must be UNMEASURED and NAMED,
// not absent.
//
// This is the failure the module was written to prevent, committed inside the
// module: checks materialised on first use, so one behind a gate that never
// opened simply did not exist. One run reported "(of 11)" against twelve wired
// checks, because gpu_fetch.address_mismatch sits behind the g_diag gate and
// needs 400 qualifying draws. The declared list is now the denominator.
void CheckDeclaredButNeverRun() {
  std::printf("declared checks\n");
  const std::string r = h::Report();

  // Nothing in this test ever calls the real check names, so every one of them
  // must be present and unmeasured.
  CheckContains("a never-run check is NAMED", r, "gpu_fetch.address_mismatch");
  CheckContains("...and another", r, "decl.unknown_ptr");
  CheckContains("...and it is counted UNMEASURED", r, "unmeasured");

  // And it must never be reported as ok or stale -- both would be a lie
  // about a measurement that was never taken. Checked against the STALE
  // SECTION specifically, not the whole line: t.stale.subject is
  // legitimately stale by now from the test above, and asserting "no
  // STALE anywhere" would fail on that instead of on what is meant.
  if (r.find("BAD: gpu_fetch.address_mismatch") != std::string::npos)
    Fail("a never-run check is not BAD", r);
  const size_t stale_at = r.find("| STALE:");
  if (stale_at != std::string::npos &&
      r.find("gpu_fetch.address_mismatch", stale_at) != std::string::npos)
    Fail("a never-run check is not STALE", r);
}

}  // namespace

int main() {
  // Never truncate a real run's findings just by running the test.
  _putenv_s("MX_HEALTH_FILE",
            (std::string(std::getenv("TEMP") ? std::getenv("TEMP") : ".") +
             "\\mx_health_test.txt")
                .c_str());

  std::printf("health census\n\n");
  CheckVerdicts();
  CheckReport();
  CheckStaleness();
  CheckDeclaredButNeverRun();

  std::printf("\n%s (%d failure%s)\n", g_failures ? "FAILED" : "PASSED",
              g_failures, g_failures == 1 ? "" : "s");
  return g_failures ? 1 : 0;
}
