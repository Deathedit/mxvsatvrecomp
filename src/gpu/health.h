#pragma once

// HEALTH CHECKS -- the expectation lives next to the number, not in a comment.
//
// THE PROBLEM THIS SOLVES. A run emits ~250 log lines and every one is [info]:
// 393 REXLOG_INFO call sites against 3 REXLOG_WARN, so severity says nothing and
// there is no way to ask "is anything wrong". Meanwhile 33 places in this tree
// state an expectation in a COMMENT beside the line that prints the number, and
// the join happens in the reader's head every time -- `decl-source ... unknown=N`
// carries "must stay at 0", has never been 0 in any run anyone looked at, and
// nothing ever said so.
//
// THREE VERDICTS, AND THE THIRD IS THE POINT.
//
//   ok          measured against a real population, and it holds
//   BAD         measured against a real population, and it does not
//   UNMEASURED  the population was zero -- nothing was asked, so nothing is
//               known
//
// UNMEASURED IS NEVER ok. This project has lost multiple sessions to a zero that
// meant "never had the chance to fire" being read as "healthy": a reason-code
// chain with an unreachable branch; menu-only runs reporting every resource cap
// healthy when a level needs 67 of a 64 cap; "VFETCH coverage: 30573 of 42".
//
// A fourth state, STALE, catches the other half: a check that stopped being
// evaluated at all. Report() runs on a fixed cadence and stamps a cycle number,
// so a site compiled out, gated off or moved behind an early return goes loud
// instead of freezing at its last good answer.
//
// WHAT IS NOT HERE. No thresholds invented for lines that have no defensible
// expectation. A check exists only where the source already asserted one in
// prose.

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

// The four shapes that cover every expectation currently written in a comment in
// this tree. Each returns its verdict so the caller can print it on its own line
// as well as have it counted. `name` must be stable and greppable --
// "decl-source.unknown", not a sentence -- because it is the key, and reusing
// one name for two quantities silently merges them.

// "this must read 0". `population` is what it was counted OUT OF -- the number
// of opportunities the zero is a claim about. Pass it honestly: a zero out of
// zero is UNMEASURED, and that is usually the finding.
Verdict Zero(const char* name, uint64_t value, uint64_t population);

// "this must NOT sit at zero" -- a counter whose zero means the thing feeding it
// is broken rather than merely quiet. g_indexCondRead is the example the tree
// already spells out: if read is 0 the offsets are wrong and every index is
// unconditioned, which is the state that lost the ground.
Verdict NonZero(const char* name, uint64_t value, uint64_t population);

// "these two must be equal" -- applied vs attempted, accepted vs submitted.
// `want` doubles as the population, so want == 0 is UNMEASURED.
Verdict Equal(const char* name, uint64_t got, uint64_t want);

// "no more than this fraction". `max_percent` is the inclusive bound.
Verdict AtMost(const char* name, uint64_t value, uint64_t population,
               double max_percent);

// One line for the whole census, the same discipline the guard census uses:
// every check, zero included, counts beside the names, and the BAD ones spelled
// out. Calling this advances the cycle counter, so it must be called once per
// report pass and from one place.
std::string Report();

// Checks that turned BAD since the last call, rendered ready to log, and cleared
// by the call. This module deliberately does not log: keeping it free of the
// SDK's logging dependency is what lets it be built and tested on its own.
//
// Only the TRANSITION into BAD is reported, not every cycle a check stays bad:
// reports run several times a minute and a stuck check would bury everything
// else. Nothing is hidden -- Report() carries every check's current verdict.
std::vector<std::string> DrainNewlyBad();

// How many checks this build declares. The report's total is this plus any name
// a call site used that is not on the declared list. Exposed so a test can
// reason about the totals without hard-coding a number that changes every time a
// check is added.
size_t DeclaredCount();

// True if anything is currently BAD. For callers that want to raise their own
// alarm without parsing Report().
bool AnyBad();

}  // namespace mx::gpu::health
