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
#include <map>
#include <mutex>
#include <string>
#include <utility>

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

// What happened to a draw, and why it did not become one.
//
// accepted and refused are the totals the hooks_d3d9.h shims expose; the rest
// are the reasons. They belong in one object because the reasons only mean
// anything against the totals -- a refusal count is not a rate until you know
// how many draws there were.
struct DrawOutcomeCensus {
  uint64_t accepted = 0;
  uint64_t refused = 0;

  uint64_t badPrimType[64] = {};   // indexed by the guest primitive type
  uint64_t noViewport = 0;
  uint64_t shaderFailed = 0;        // ApplyShaderOutputs returned kFailed
  uint64_t shaderNoCodeFull = 0;    // kNoCode, and the pending queue was full
  uint64_t shaderConstOverlays = 0;
};

extern DrawOutcomeCensus g_drawOutcome;

// Stencil sizing. MEASUREMENT ONLY -- nothing branches on any of this.
//
// The pairs matter more than the members: drawsSeen against drawsUnreadable
// says whether a zero is "no stencil" or "could not tell", and bitSet against
// effective says whether the guest asking for stencil is the same as getting
// it. Either read alone has misled before.
struct StencilCensus {
  uint64_t drawsSeen = 0;        // reached the RB_DEPTHCONTROL read
  uint64_t drawsUnreadable = 0;  // ...and could not read it
  uint64_t bitSet = 0;           // stencil_enable, mode ignored
  uint64_t effective = 0;        // stencil_enable AND edram_mode says so

  uint64_t plumbedSeen = 0;
  uint64_t plumbedUnreadable = 0;
  uint64_t plumbedEffective = 0;

  uint64_t bfWindowDraws = 0;

  // The per-configuration breakdowns behind those totals, and the locks that
  // guard them. In the struct because they are the same census: a total without
  // its configurations cannot say WHICH state was set, which is the question
  // the stencil work kept needing to answer.
  std::mutex mu;
  std::map<uint32_t, uint64_t> edramModes;  // edram_mode -> draws
  std::map<std::pair<uint32_t, uint32_t>, uint64_t> configs;

  std::mutex plumbedMu;
  std::map<std::pair<uint32_t, uint32_t>, uint64_t> plumbedConfigs;

  std::mutex bfWindowMu;
  std::map<uint32_t, std::map<uint32_t, uint64_t>> bfWindow;
};

extern StencilCensus g_stencil;

// Draws that took the GPU vertex-fetch path.
struct GpuFetchCensus {
  uint64_t draws = 0;
  uint64_t rectList = 0;
  uint64_t ordinalMismatch = 0;
};

extern GpuFetchCensus g_gpuFetch;

// SHADER NAMES -- how much of the frame we can say the guest's own name for.
//
// Step 0 of replacing a Xenos subsystem is being able to identify a draw
// semantically, and the names come from tools/shader_manifest.py joining
// microcode to the .shader assets by content.
//
// THE DENOMINATOR IS EVERY DRAW, deliberately. Counting only draws whose
// shaders translated would measure the shaders we already knew and report a
// flattering number for a frame we cannot name -- the exact shape of a counter
// whose population is not the failure's. A draw with no translated shader at
// all is an unnamed draw here, because for this question it is.
//
// `unnamedVs` / `unnamedPs` hold the code_keys that missed, which is what makes
// the miss actionable: dump those blobs, re-run the tool, and the map grows.
struct ShaderNameCensus {
  // The six buckets are exclusive and sum to `draws`, so nothing hides in a
  // remainder. `generated` and `unknown` used to be one bucket called
  // `neither`, which conflated two opposite things: a shader the guest built at
  // runtime has no name to find and is not a gap, while one missing from the
  // map is. Reporting them together made step 0 look 48% short when most of
  // that was work that does not exist.
  uint64_t draws = 0;        // every draw that reached the census
  uint64_t bothNamed = 0;    // vertex AND pixel resolved
  uint64_t vsOnly = 0;
  uint64_t psOnly = 0;
  uint64_t generated = 0;    // no named stage, and every stage present is
                             // positively identified as runtime-generated
  uint64_t unknown = 0;      // no named stage, and at least one is simply
                             // missing from the map -- the real gap
  uint64_t noShader = 0;     // nothing translated at all, so nothing to name
  std::mutex mu;
  // A cap so a pathological run cannot grow these without bound. It is 512
  // rather than 64 because 64 SATURATED on the first real run -- the vertex
  // side reported "64 distinct" when the true count was unknown and 64 was
  // only a floor. The report says so explicitly when a list is at the cap; a
  // number that quietly stops climbing is worse than no number.
  static constexpr size_t kMaxUnnamed = 512;
  std::map<uint64_t, uint64_t> unnamedVs;  // code_key -> draws
  std::map<uint64_t, uint64_t> unnamedPs;
};

extern ShaderNameCensus g_shaderNames;

// ---- what the reports call ------------------------------------------------
//
// Three functions, all defined in hooks_d3d9.cpp beside the state they read.
// Functions in a header are ordinary; it is loose mutable globals that are the
// smell, which is what the structs above are for.

// Change-or-heartbeat gate for a periodic row dump. The caller owns `last` and
// `since`; returns true when the rows should print.
bool RowDumpDue(uint64_t population, uint64_t& last, uint32_t& since);

// Names a DrawGap reason code for the report.
const char* DrawGapName(uint32_t g);

// One line describing every depth surface seen, for the stencil report.
std::string DepthSurfaceReport();

}  // namespace mx::hooks::d3d9
