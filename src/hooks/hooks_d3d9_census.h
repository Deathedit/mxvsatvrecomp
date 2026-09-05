#pragma once

// Diagnostic censuses of the D3D9 layer -- the tallies the periodic reports
// read, grouped by subject.
//
// This is the seam between hooks_d3d9.cpp and hooks_d3d9_report.cpp. It exists
// because the reporting block reads counters defined all over the layer, and
// moving it while those were loose globals would have published forty of them.
// hooks_d3d9_shared.h states the rule this follows: types, functions and
// constants in a header are ordinary; loose mutable globals are the smell.
//
// Everything here is MEASUREMENT. Nothing in these structs is read by code that
// decides what to draw -- if something here ever gains a reader on the hot path
// it belongs somewhere else.

#include <cstdint>

namespace mx::hooks::d3d9 {

// Tallies ABOUT the declaration table, which lives in hooks_d3d9_internal.h.
//
// Deliberately a different object from DeclTable. They shared a `g_decl` prefix
// and read as one twelve-member family, but the table is live state that draws
// depend on and this is a set of counters that nothing branches on. Grouping
// them together would have written that confusion down as structure.
struct DeclCensus {
  // CreateVertexDeclaration calls. This was a loose `g_decls`, which is the
  // name the TABLE wanted -- the collision is what surfaced that a counter and
  // a data structure had been sharing one idea of what "the declarations" are.
  uint64_t created = 0;
  uint64_t drawsNoDecl = 0;    // draws whose declaration we never saw created
  uint64_t deviceNull = 0;     // device field is 0 -- no declaration bound yet
  uint64_t deviceUnknown = 0;  // non-zero, but never seen created
  uint64_t agree = 0;          // device field == the patch hook's value
  uint64_t disagree = 0;       // it does not, i.e. the patch value is stale
  // Adopted straight off the device because we never saw them created, and the
  // attempts that could not be trusted enough to adopt.
  uint64_t adopted = 0;
  uint64_t adoptRefused = 0;
};

extern DeclCensus g_declCensus;

}  // namespace mx::hooks::d3d9
