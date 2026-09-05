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

#include "gpu/d3d9_draw.h"              // kMaxStreams
#include "hooks/hooks_d3d9_internal.h"  // kDrawGapCount, kMaxTrackedDecls

namespace mx::hooks::d3d9 {

// ---- constants the reports read ------------------------------------------

// A draw report prints on whichever comes first, so a quiet period still gets
// one and a busy one does not print per draw.
constexpr uint64_t kDrawReportEvery = 2500;      // minimum draw delta
constexpr int64_t kDrawReportPeriodMs = 2000;    // and at most this often

constexpr uint32_t kMaxUnknownDecls = 16;
constexpr uint32_t kDeviceScanBytes = 0x4000;
constexpr uint32_t kDeviceScanDwords = kDeviceScanBytes / 4;

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

// WHICH declaration pointers were never seen created, not just how many: a bare
// count cannot name the draw. A table with its own overflow tally, bounded at
// 16 distinct pointers.
struct UnknownDeclTable {
  uint32_t ptr[kMaxUnknownDecls] = {};
  uint64_t draws[kMaxUnknownDecls] = {};
  uint32_t distinct = 0;
  uint64_t overflow = 0;  // draws whose pointer did not fit the table
};

extern UnknownDeclTable g_unknownDecls;

// Does the geometry the guest bound actually fit the draw it asked for?
//
// One object because these are one question asked four ways -- stride, vertex
// buffer, index buffer, index range -- and reading any one of them without the
// others has been misleading before: `checked` is the denominator for all of
// them, and a fit count without it says nothing.
struct DrawFitCensus {
  uint64_t gaps[kDrawGapCount] = {};
  uint64_t complete = 0;
  uint64_t checked = 0;   // the denominator. Read the rest against this.

  uint64_t strideOk = 0;
  uint64_t strideTooSmall = 0;
  uint64_t strideMismatch = 0;  // bound stride larger than the layout needs

  uint64_t vbFits = 0, vbTooSmall = 0;
  uint64_t ibFits = 0, ibTooSmall = 0;
  uint64_t idxRangeFits = 0, idxRangeFails = 0, idxRangeUnread = 0;
};

extern DrawFitCensus g_drawFit;

// The same questions again, but PER STREAM rather than totalled -- which is the
// point: a total hides that one stream is responsible. Every member is indexed
// by stream, so they share a denominator and can be read across.
struct PerStreamCensus {
  uint64_t bindAgeFitSum[mx::hle::kMaxStreams] = {};
  uint64_t bindAgeFailSum[mx::hle::kMaxStreams] = {};
  uint64_t bindAgeFailMax[mx::hle::kMaxStreams] = {};
  uint64_t vbFits[mx::hle::kMaxStreams] = {};
  uint64_t vbFails[mx::hle::kMaxStreams] = {};
  uint64_t fileAgree[mx::hle::kMaxStreams] = {};
  uint64_t fileDiffer[mx::hle::kMaxStreams] = {};
  uint64_t fileRescues[mx::hle::kMaxStreams] = {};
};

extern PerStreamCensus g_perStream;

// Which device dwords could be a fetch-constant file, narrowed across samples.
// `reached` is the lowest end-of-readable seen, so a candidate past it was
// never actually tested rather than tested and rejected.
struct FetchConstScan {
  bool cand0[kDeviceScanDwords];
  bool cand1[kDeviceScanDwords];
  bool primed = false;
  uint32_t samples = 0;
  uint32_t reached = kDeviceScanBytes;
};

extern FetchConstScan g_fcScan;

}  // namespace mx::hooks::d3d9
