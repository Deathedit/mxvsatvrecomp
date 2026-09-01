#pragma once

// HEALTH CHECKS -- the expectation lives next to the number, not in a comment.
//
// THE PROBLEM THIS SOLVES. A run emits ~250 log lines and every one of them is
// [info]. 393 REXLOG_INFO call sites against 3 REXLOG_WARN, so severity says
// nothing and there is no way to ask "is anything wrong". Meanwhile 33 places
// in this tree state an expectation in a COMMENT beside the line that prints
// the number -- "must stay at 0", "both must read 0", "must match". The log
// prints the fact; the expectation is three lines up in the source; and the
// join happens in the reader's head, every time, for every line.
//
// It does not survive contact. `decl-source ... unknown=N` carries the comment
// "must stay at 0 or the field is not what SetVertexDeclaration writes". It has
// never been 0 in any run anyone looked at, and nothing ever said so.
//
// So: state the expectation HERE, in code, and let the line classify itself.
//
// THREE VERDICTS, AND THE THIRD IS THE POINT.
//
//   ok          measured against a real population, and it holds
//   BAD         measured against a real population, and it does not
//   UNMEASURED  the population was zero -- nothing was asked, so nothing is
//               known
//
// UNMEASURED IS NEVER ok. That distinction is the whole reason this exists.
// This project has lost multiple sessions to a zero that meant "never had the
// chance to fire" being read as "healthy":
//
//   - a reason-code chain ordered so that one branch was unreachable; its 0
//     was read as a measurement and cost a session
//   - menu-only runs reporting every resource cap healthy, because a level was
//     never loaded; the cap was 64 and a level needs 67
//   - "VFETCH coverage: 30573 of 42", a count divided by a population it was
//     never counting
//
// A fourth state, STALE, catches the other half of the same failure: a check
// that stopped being evaluated at all. Report() runs on a fixed cadence and
// stamps a cycle number; a check that did not update this cycle is reported as
// STALE rather than repeating its last verdict forever. A site that has been
// compiled out, gated off, or moved behind an early return goes loud instead of
// freezing at its last good answer.
//
// WHAT IS NOT HERE. No thresholds invented for lines that have no defensible
// expectation. A check exists only where the source already asserted one in
// prose; converting a merely informational counter into a check would mean
// choosing a bound nobody has justified, which is how a report starts lying.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mx::gpu::health {

enum class Verdict : uint8_t {
  kOk = 0,
  kBad,
  kUnmeasured,
  kStale,
};

// "ok" / "BAD" / "??" / "stale" -- short because these are meant to be read
// inline, at the end of the line that carries the number.
const char* Tag(Verdict v);

// The four shapes that cover every expectation currently written in a comment
// in this tree. Each returns its verdict so the caller can print it on its own
// line as well as have it counted.
//
// `name` must be stable and greppable: "decl-source.unknown", not a sentence.
// It is the key, so reusing one name for two quantities silently merges them.

// "this must read 0". `population` is what it was counted OUT OF -- the number
// of opportunities the zero is a claim about. Pass it honestly: a zero out of
// zero is UNMEASURED, and that is usually the finding.
Verdict Zero(const char* name, uint64_t value, uint64_t population);

// "this must NOT sit at zero" -- a counter whose zero means the thing feeding
// it is broken rather than merely quiet. g_indexCondRead is the example the
// tree already spells out: "if read is 0 the offsets are wrong and every index
// is unconditioned -- which is the state that lost the ground -- so this must
// not be allowed to sit silently at zero."
Verdict NonZero(const char* name, uint64_t value, uint64_t population);

// "these two must be equal" -- applied vs attempted, accepted vs submitted.
// `want` doubles as the population, so want == 0 is UNMEASURED.
Verdict Equal(const char* name, uint64_t got, uint64_t want);

// "no more than this fraction". `max_percent` is the inclusive bound.
Verdict AtMost(const char* name, uint64_t value, uint64_t population,
               double max_percent);

// One line for the whole census, the same discipline the guard census uses:
// every check, zero included, counts beside the names, and the BAD ones spelled
// out so the line is actionable without opening anything else.
//
// Calling this advances the cycle counter, so it must be called once per report
// pass and from one place.
std::string Report();

// Checks that turned BAD since the last call, rendered ready to log, and
// cleared by the call. This module deliberately does not log: keeping it free
// of the SDK's logging dependency is what lets it be built and tested on its
// own, the same arrangement guard_census.cpp uses. The caller emits these at
// WARN so severity finally distinguishes a finding from a fact.
//
// Only the TRANSITION into BAD is reported, not every cycle a check stays bad:
// reports run several times a minute and a stuck check would bury everything
// else. Nothing is hidden by that -- Report() carries every check's current
// verdict on every pass.
std::vector<std::string> DrainNewlyBad();

// How many checks this build declares. The report's total is this plus any
// name a call site used that is not on the declared list. Exposed so a test can
// reason about the totals without hard-coding a number that changes every time
// a check is added.
size_t DeclaredCount();

// True if anything is currently BAD. For callers that want to raise their own
// alarm without parsing Report().
bool AnyBad();

}  // namespace mx::gpu::health
