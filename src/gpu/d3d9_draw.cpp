#include "gpu/d3d9_draw.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <rex/logging.h>

#include "gpu/shader_ucode.h"   // ApplyFetchEndian

namespace mx::hle {

const char* HleSkipName(HleSkip s) {
  switch (s) {
    case HleSkip::kNone:             return "built";
    case HleSkip::kNoLayout:         return "no declaration bound";
    case HleSkip::kNoPosition:       return "declaration has no POSITION 0";
    case HleSkip::kStreamUnbound:    return "a needed stream was never set";
    case HleSkip::kZeroStride:       return "stream stride is 0";
    case HleSkip::kBadTopology:      return "primitive type has no host form";
    case HleSkip::kEmpty:            return "zero vertices or indices";
    case HleSkip::kVertexOutOfRange: return "vertex range outside the buffer";
    case HleSkip::kIndexOutOfRange:  return "index range outside the buffer";
    case HleSkip::kUnreadableFormat: return "format the decoder refuses";
    case HleSkip::kTooManyVertices:  return "vertex range beyond the cap";
    default:                         return "?";
  }
}

namespace {

// Guest bytes are big-endian; the index buffer is not covered by the vertex
// fetch constant's endian field, and DrawIndexedVertices reads it as plain
// big-endian halfwords.
inline uint32_t RdIndex16(const uint8_t* p) {
  return (uint32_t(p[0]) << 8) | p[1];
}
inline uint32_t RdIndex32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
         (uint32_t(p[2]) << 8) | p[3];
}

// Copy one vertex out of a stream, in guest byte order.
//
// This used to undo the byte order here, once for the whole vertex. It cannot
// be done here: the correct swap width is the format's packed unit, and one
// vertex mixes 16-bit positions with 32-bit colours — a blanket 8in32 over a
// 16-bit position exchanges its components as well as their bytes. The swap now
// happens inside ReadHleElement, which knows the format. Pass `s.endian` there.
bool CopyVertex(const HleStream& s, uint32_t index, uint8_t* dst,
                uint32_t dst_bytes) {
  const uint64_t byte_off =
      uint64_t(index) * s.stride + s.offset_bytes;
  if (byte_off + s.stride > s.size_bytes) return false;
  const uint32_t n = s.stride < dst_bytes ? s.stride : dst_bytes;
  std::memcpy(dst, s.host + byte_off, n);
  return true;
}

}  // namespace

bool BuildHleDraw(const HleDrawInputs& in, DrawCall& out, HleSkip& skip) {
  out = DrawCall{};
  skip = HleSkip::kNone;

  if (!in.layout || in.layout->elements.empty()) {
    skip = HleSkip::kNoLayout;
    return false;
  }
  if (in.count == 0) {
    skip = HleSkip::kEmpty;
    return false;
  }

  // RectangleList (8) and QuadList (13) map to kUndefined on purpose: they are
  // not host topologies, they are expansions, and both are rewritten to
  // triangle lists after the vertices exist. Measured, not assumed — the first
  // run refused 62% of draws for topology and the histogram named exactly these
  // two: type 8 x8,310 and type 13 x62,347, the whole non-indexed population.
  const bool expand_rects =
      in.prim_type == uint32_t(PrimitiveType::kRectangleList);
  const bool expand_quads = in.prim_type == uint32_t(PrimitiveType::kQuadList);

  const HostTopology topo = MapTopology(in.prim_type);
  if (topo == HostTopology::kUndefined && !expand_rects && !expand_quads) {
    skip = HleSkip::kBadTopology;
    return false;
  }

  // The declaration states which element is the position. No inference, no
  // fallback — this is the single biggest difference from the PM4 path, where
  // PickPositionAttribute has to trace the microcode to the position export and
  // guesses when that fails.
  const HleInputElement* pos = FindUsage(*in.layout, kUsagePosition, 0);
  if (!pos) {
    skip = HleSkip::kNoPosition;
    return false;
  }
  // Colour is optional: a declaration without one draws opaque white, the same
  // convention the PM4 path uses, and it is recorded so a screenshot can be
  // read against the count.
  const HleInputElement* col = FindUsage(*in.layout, kUsageColor, 0);
  // Likewise the first texcoord set. Optional for the same reason, and it
  // defaults to (0,0) — but only untextured draws should ever see that default.
  // This used to be hardcoded to (0,0) for every vertex of every draw, so every
  // textured draw sampled one corner texel and came out a flat colour.
  const HleInputElement* tex = FindUsage(*in.layout, kUsageTexcoord, 0);

  auto stream_ok = [&](const HleInputElement* e) -> HleSkip {
    if (!e) return HleSkip::kNone;
    if (e->stream >= kMaxStreams) return HleSkip::kStreamUnbound;
    const HleStream& s = in.streams[e->stream];
    if (!s.bound || !s.host) return HleSkip::kStreamUnbound;
    if (s.stride == 0) return HleSkip::kZeroStride;
    return HleSkip::kNone;
  };
  if ((skip = stream_ok(pos)) != HleSkip::kNone) return false;
  if ((skip = stream_ok(col)) != HleSkip::kNone) {
    // A missing colour stream is not worth losing the geometry over.
    col = nullptr;
    skip = HleSkip::kNone;
  }
  if ((skip = stream_ok(tex)) != HleSkip::kNone) {
    tex = nullptr;
    skip = HleSkip::kNone;
  }

  // --- the index buffer, and the vertex range it implies -------------------
  std::vector<uint8_t> indices;
  uint32_t lo = 0, hi = 0;   // inclusive vertex range actually referenced

  if (in.indexed) {
    if (!in.index.bound || !in.index.host) {
      skip = HleSkip::kIndexOutOfRange;
      return false;
    }
    const uint32_t w = in.index.is_32bit ? 4 : 2;
    const uint64_t end = uint64_t(in.first + in.count) * w;
    if (end > in.index.size_bytes) {
      skip = HleSkip::kIndexOutOfRange;
      return false;
    }
    // Read once, find the referenced range, and rebase — so a draw that
    // indexes a small window of a large shared buffer uploads that window and
    // not the whole thing.
    uint32_t vmin = 0xFFFFFFFFu, vmax = 0;
    std::vector<uint32_t> raw(in.count);
    for (uint32_t i = 0; i < in.count; ++i) {
      const uint8_t* p = in.index.host + uint64_t(in.first + i) * w;
      uint32_t v = (w == 4) ? RdIndex32(p) : RdIndex16(p);
      const int64_t adjusted = int64_t(v) + in.base_vertex;
      if (adjusted < 0) { skip = HleSkip::kVertexOutOfRange; return false; }
      v = uint32_t(adjusted);
      raw[i] = v;
      if (v < vmin) vmin = v;
      if (v > vmax) vmax = v;
    }
    lo = vmin;
    hi = vmax;
    if (uint64_t(hi - lo) + 1 > kMaxHleVertices) {
      skip = HleSkip::kTooManyVertices;
      return false;
    }
    indices.resize(size_t(in.count) * 2);
    for (uint32_t i = 0; i < in.count; ++i) {
      const uint32_t r = raw[i] - lo;
      if (r > 0xFFFF) {
        // The renderer's index view is 16-bit here; a window wider than that
        // is counted rather than silently truncated.
        skip = HleSkip::kTooManyVertices;
        return false;
      }
      indices[i * 2 + 0] = uint8_t(r & 0xFF);
      indices[i * 2 + 1] = uint8_t(r >> 8);
    }
    out.index_16bit = true;
    out.index_count = in.count;
  } else {
    lo = in.first;
    hi = in.first + in.count - 1;
    if (uint64_t(in.count) > kMaxHleVertices) {
      skip = HleSkip::kTooManyVertices;
      return false;
    }
    // Non-indexed draws still go through the renderer's indexed path, so a
    // trivial identity index buffer is generated rather than adding a second
    // submission route.
    indices.resize(size_t(in.count) * 2);
    for (uint32_t i = 0; i < in.count; ++i) {
      indices[i * 2 + 0] = uint8_t(i & 0xFF);
      indices[i * 2 + 1] = uint8_t(i >> 8);
    }
    if (in.count > 0xFFFF) { skip = HleSkip::kTooManyVertices; return false; }
    out.index_16bit = true;
    out.index_count = in.count;
  }

  const uint32_t nverts = hi - lo + 1;
  out.first_vertex = lo;

  // --- the vertices --------------------------------------------------------
  //
  // Bounded by each stream's own size. Stage 0 established that a range not
  // fitting is *not* proof the description is wrong — the shader issues its own
  // vfetch and need not index every stream alike — so this refuses the draw and
  // counts it rather than clamping into whatever follows the buffer.
  out.vertices.resize(size_t(nverts) * kHostVertexStride);

  uint8_t vtx[256];   // one guest vertex, larger than any observed stride
  for (uint32_t i = 0; i < nverts; ++i) {
    const uint32_t src_index = lo + i;
    uint8_t* dst = out.vertices.data() + size_t(i) * kHostVertexStride;

    float p[4] = {0, 0, 0, 1};
    {
      const HleStream& s = in.streams[pos->stream];
      if (s.stride > sizeof(vtx)) { skip = HleSkip::kZeroStride; return false; }
      if (!CopyVertex(s, src_index, vtx, sizeof(vtx))) {
        skip = HleSkip::kVertexOutOfRange;
        return false;
      }
      if (!ReadHleElement(vtx, s.stride, *pos, s.endian, p)) {
        skip = HleSkip::kUnreadableFormat;
        return false;
      }
    }

    float c[4] = {1, 1, 1, 1};
    if (col) {
      const HleStream& s = in.streams[col->stream];
      if (s.stride <= sizeof(vtx) && CopyVertex(s, src_index, vtx, sizeof(vtx))) {
        if (!ReadHleElement(vtx, s.stride, *col, s.endian, c)) {
          c[0] = c[1] = c[2] = c[3] = 1.0f;
        }
      }
    }

    float t[4] = {0, 0, 0, 0};
    if (tex) {
      const HleStream& s = in.streams[tex->stream];
      if (s.stride <= sizeof(vtx) && CopyVertex(s, src_index, vtx, sizeof(vtx))) {
        if (!ReadHleElement(vtx, s.stride, *tex, s.endian, t)) {
          t[0] = t[1] = 0.0f;
        }
      }
    }

    std::memcpy(dst + 0, p, 12);       // float3 POSITION
    std::memcpy(dst + 12, c, 16);      // float4 COLOR
    std::memcpy(dst + 28, t, 8);       // float2 TEXCOORD0
  }

  if (in.mvp) {
    std::memcpy(out.mvp, in.mvp, sizeof(out.mvp));
  } else {
    static const float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0,
                                        0, 0, 1, 0, 0, 0, 0, 1};
    std::memcpy(out.mvp, kIdentity, sizeof(out.mvp));
  }

  out.vertex_count = nverts;
  out.vertex_stride = kHostVertexStride;
  out.indices = std::move(indices);
  out.prim_type = in.prim_type;
  out.topology = topo;

  // The same two expansions the PM4 path uses, shared rather than reimplemented
  // — a second copy could drift from the one the renderer already agrees with.
  out.valid = true;
  out.color_source = col ? DrawCall::ColorSource::kPacked
                         : DrawCall::ColorSource::kNone;
  return true;
}

bool FinalizeHleTopology(DrawCall& draw, HleSkip& skip) {
  skip = HleSkip::kNone;
  const auto prim = static_cast<PrimitiveType>(draw.prim_type);
  if (prim == PrimitiveType::kRectangleList) {
    if (ExpandRectangleList(draw) == 0) {
      skip = HleSkip::kBadTopology;
      return false;
    }
  } else if (prim == PrimitiveType::kQuadList) {
    if (ExpandQuadList(draw) == 0) {
      skip = HleSkip::kBadTopology;
      return false;
    }
  }
  return true;
}

std::vector<DrawCall>& HleFrameDraws() {
  static std::vector<DrawCall> v;
  return v;
}

uint64_t* HleSkipCounts() {
  static uint64_t counts[size_t(HleSkip::kCount)] = {};
  return counts;
}

uint64_t& HleBuiltCount() {
  static uint64_t n = 0;
  return n;
}

//===========================================================================
// Stage 3 — scoring the candidate transforms.
//
// One candidate per (base register, layout), plus two controls: identity, and
// the viewport inverse the PM4 path uses today. A vertex counts as in-clip when
// |x| <= w, |y| <= w, 0 <= z <= w with w > 0 and everything finite.
//
// **Scored per draw, not per run.** The first version pooled every position in
// the run and ranked candidates over the total, which asks "which single matrix
// transforms this game" — and the constant dump says that question has no
// answer. Draw 1 already carries positions at (-1, 1, 0) with c0..c3 identity,
// while c4..c7 hold a perspective projection: the population is a mix of
// pre-transformed 2D geometry and 3D geometry, and no one register can win over
// both. Pooling produced a top candidate at 45% that changed between reports,
// which is what "no single answer" looks like when you insist on one.
//
// So each draw votes for its own best candidate, and the report is a histogram
// of winners plus how many draws any candidate could explain at all. That
// distinguishes "the matrix is somewhere we are not looking" from "there are
// several matrices" — the pooled version could not.
//
// The trap this is built around, learned the last time positions were scored
// this way: **(0,0,0) passes every candidate**. A degenerate position sits
// inside the clip volume under any matrix whatsoever, so a draw full of them
// would score 100% for all 128 candidates and mean nothing. Degenerate inputs
// are excluded from the denominator and counted separately, so a run whose
// positions are mostly zero says so instead of producing a confident ranking.
//===========================================================================
namespace {

constexpr uint32_t kCandidateBases = kHleProbeRegs - 3;   // need 4 registers
constexpr uint32_t kCandidates = kCandidateBases * 2 + 2; // + identity, viewport
constexpr uint32_t kIdentityCandidate = kCandidates - 2;
constexpr uint32_t kViewportCandidate = kCandidates - 1;

// Vertices sampled per draw. Enough to be a population per draw without making
// the probe the most expensive thing in the frame.
constexpr uint32_t kProbeVertsPerDraw = 16;

// Draws each candidate won outright, and draws it would have explained on its
// own. "Wins" and "explains" differ: several candidates can explain the same
// draw when the geometry is small enough to sit inside the volume under more
// than one matrix, and a winner-take-all histogram would hide that.
uint64_t g_candWins[kCandidates] = {};
uint64_t g_candExplains[kCandidates] = {};
uint64_t g_probeDraws = 0;         // draws with any non-degenerate position
uint64_t g_probeExplained = 0;     // draws some candidate put fully in clip
uint64_t g_probeUnexplained = 0;
uint64_t g_probeDegenerate = 0;    // positions rejected as (0,0,0)
uint64_t g_probeAllDegenerate = 0; // draws with nothing else in them
bool     g_probeSawViewport = false;

// Winner per bound vertex shader, keyed (handle << 32 | candidate). The
// question a run-wide histogram cannot answer: is the register a property of
// the shader? If it is, each handle has one dominant winner and the spread
// across the run is just the shader changing.
std::map<uint64_t, uint32_t> g_shaderWins;
std::map<uint32_t, uint32_t> g_shaderDraws;

// A candidate "explains" a draw when it puts at least this much of it in the
// clip volume. Not 100%: real geometry is legitimately clipped at the screen
// edge, and demanding perfection would reject the right matrix.
constexpr double kExplainFraction = 0.95;

inline bool Transform(const float m[16], const float p[3], float o[4]) {
  for (int r = 0; r < 4; ++r) {
    o[r] = m[r * 4 + 0] * p[0] + m[r * 4 + 1] * p[1] + m[r * 4 + 2] * p[2] +
           m[r * 4 + 3];
  }
  for (int i = 0; i < 4; ++i) {
    if (!std::isfinite(o[i])) return false;
  }
  return true;
}

inline bool InClip(const float o[4]) {
  if (o[3] <= 0.0f) return false;
  return o[0] >= -o[3] && o[0] <= o[3] && o[1] >= -o[3] && o[1] <= o[3] &&
         o[2] >= 0.0f && o[2] <= o[3];
}

// Distinct positions must stay distinct. **A matrix that collapses everything
// onto one point sits inside the clip volume no matter what you feed it**, so
// without this the ranking is won by whichever candidate is closest to rank 1 —
// which is the (0,0,0) trap moved from the input to the transform, and it is
// exactly what the first per-draw run produced: one candidate explaining all
// 10,266 draws while both controls explained none.
constexpr float kSpreadEpsilon = 1e-4f;

const char* CandidateName(uint32_t c, char* buf, size_t n) {
  if (c == kIdentityCandidate) return "identity (control)";
  if (c == kViewportCandidate) return "viewport inverse (control, = PM4 today)";
  std::snprintf(buf, n, "c%u %s", c / 2,
                (c & 1) ? "col-major" : "row-major");
  return buf;
}

}  // namespace

void ScoreHleTransform(const DrawCall& dc, const float* consts,
                       const float* viewport_mvp, uint32_t vertex_shader) {
  if (!consts || dc.vertex_count == 0 || dc.vertex_stride != kHostVertexStride)
    return;
  if (viewport_mvp) g_probeSawViewport = true;

  static const float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0,
                                      0, 0, 1, 0, 0, 0, 0, 1};

  // Stride the sample across the draw rather than taking the first N: the first
  // vertices of a strip are not representative of the whole.
  const uint32_t n = dc.vertex_count;
  const uint32_t step = n > kProbeVertsPerDraw ? n / kProbeVertsPerDraw : 1;

  float pts[kProbeVertsPerDraw + 1][3];
  uint32_t npts = 0;
  for (uint32_t i = 0; i < n && npts <= kProbeVertsPerDraw; i += step) {
    float p[3];
    std::memcpy(p, dc.vertices.data() + size_t(i) * kHostVertexStride, 12);
    if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2]))
      continue;
    if (p[0] == 0.0f && p[1] == 0.0f && p[2] == 0.0f) {
      ++g_probeDegenerate;
      continue;
    }
    std::memcpy(pts[npts++], p, sizeof(p));
  }
  if (npts == 0) { ++g_probeAllDegenerate; return; }
  ++g_probeDraws;

  const uint32_t need = uint32_t(double(npts) * kExplainFraction + 0.999);

  // Whether the *inputs* are distinct at all. A draw whose sampled positions
  // are genuinely one repeated point cannot be used to reject a collapsing
  // matrix, so the spread test is only applied where it means something.
  bool inputs_spread = false;
  for (uint32_t i = 1; i < npts && !inputs_spread; ++i) {
    for (int k = 0; k < 3; ++k)
      inputs_spread = inputs_spread || pts[i][k] != pts[0][k];
  }

  uint32_t best = kCandidates;
  uint32_t best_in = 0;
  auto score = [&](uint32_t c, const float m[16]) {
    uint32_t in = 0;
    float lo[2] = {1e30f, 1e30f}, hi[2] = {-1e30f, -1e30f};
    for (uint32_t i = 0; i < npts; ++i) {
      float o[4];
      if (!Transform(m, pts[i], o)) continue;
      if (!InClip(o)) continue;
      ++in;
      for (int k = 0; k < 2; ++k) {
        const float v = o[k] / o[3];
        if (v < lo[k]) lo[k] = v;
        if (v > hi[k]) hi[k] = v;
      }
    }
    if (inputs_spread && in > 1 &&
        (hi[0] - lo[0]) < kSpreadEpsilon && (hi[1] - lo[1]) < kSpreadEpsilon) {
      return;   // collapses distinct geometry to a point: not a transform
    }
    if (in >= need) ++g_candExplains[c];
    // Ties go to the lower candidate index, which is the lower register and then
    // row-major — an arbitrary rule, but a fixed one, so a tie cannot drift
    // between runs and look like a change in the data.
    if (in > best_in) { best_in = in; best = c; }
  };

  for (uint32_t b = 0; b < kCandidateBases; ++b) {
    const float* r = consts + size_t(b) * 4;
    score(b * 2 + 0, r);   // row-major: register b+k is row k
    // Column-major: register b+k is column k, which is how a D3D-era compiler
    // usually packs a matrix into four constants.
    float t[16];
    for (int rr = 0; rr < 4; ++rr)
      for (int cc = 0; cc < 4; ++cc) t[rr * 4 + cc] = r[cc * 4 + rr];
    score(b * 2 + 1, t);
  }
  score(kIdentityCandidate, kIdentity);
  if (viewport_mvp) score(kViewportCandidate, viewport_mvp);

  if (best != kCandidates && best_in >= need) {
    ++g_candWins[best];
    ++g_probeExplained;
    ++g_shaderWins[(uint64_t(vertex_shader) << 32) | best];
    ++g_shaderDraws[vertex_shader];
    // The first few winners in full. The last round's ranking was won by a
    // matrix that turned out to collapse everything to a point — visible
    // immediately in its numbers, and not at all in its score.
    static uint32_t s_dumped = 0;
    if (s_dumped < 8 && best < kCandidates - 2) {
      ++s_dumped;
      const uint32_t b = best / 2;
      const float* r = consts + size_t(b) * 4;
      char nm[64];
      REXLOG_INFO(
          "d3d9: stage3  winner {} on a draw of {} verts ({}/{} in clip): "
          "[{:.4f} {:.4f} {:.4f} {:.4f}] [{:.4f} {:.4f} {:.4f} {:.4f}] "
          "[{:.4f} {:.4f} {:.4f} {:.4f}] [{:.4f} {:.4f} {:.4f} {:.4f}], "
          "first pos ({:.3f} {:.3f} {:.3f})",
          CandidateName(best, nm, sizeof(nm)), dc.vertex_count, best_in, npts,
          r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8], r[9], r[10],
          r[11], r[12], r[13], r[14], r[15], pts[0][0], pts[0][1], pts[0][2]);
    }
  } else {
    ++g_probeUnexplained;
  }
}

void ReportHleTransform() {
  if (g_probeDraws == 0) {
    REXLOG_INFO(
        "d3d9: stage3  transform probe — no draws scored ({} were entirely "
        "degenerate positions)", g_probeAllDegenerate);
    return;
  }
  REXLOG_INFO(
      "d3d9: stage3  transform probe — {} draws scored, {} explained by some "
      "candidate ({}%), {} by none; {} positions and {} whole draws rejected "
      "as (0,0,0){}",
      g_probeDraws, g_probeExplained, (g_probeExplained * 100) / g_probeDraws,
      g_probeUnexplained, g_probeDegenerate, g_probeAllDegenerate,
      g_probeSawViewport ? "" : " — NO VIEWPORT was ever set, so the viewport "
                                "control is absent, not losing");

  char buf[64];
  // Controls first and unconditionally: a candidate that only beats nothing has
  // to be visible as such, and burying the controls in a sorted list hides that.
  for (uint32_t c : {kIdentityCandidate, kViewportCandidate}) {
    REXLOG_INFO("d3d9: stage3    {:<40} won {:>7} draws, explains {:>7}",
                CandidateName(c, buf, sizeof(buf)), g_candWins[c],
                g_candExplains[c]);
  }
  // Then the best constant-file candidates. **Nothing is mutated here.** The
  // first version zeroed each winner as it printed it, on the assumption that
  // the report ran once; it runs every 2,500 draws, so each report ate the
  // previous one's leaders and the winner appeared to change every few seconds.
  // The instability was the instrument, not the data.
  uint32_t shown[6] = {};
  uint32_t nshown = 0;
  for (uint32_t rank = 0; rank < 6; ++rank) {
    uint32_t best = kCandidates;
    uint64_t best_n = 0;
    for (uint32_t c = 0; c < kCandidates - 2; ++c) {
      bool already = false;
      for (uint32_t i = 0; i < nshown; ++i) already = already || shown[i] == c;
      if (already) continue;
      if (g_candWins[c] > best_n) { best_n = g_candWins[c]; best = c; }
    }
    if (best == kCandidates || best_n == 0) break;
    shown[nshown++] = best;
    REXLOG_INFO("d3d9: stage3    {:<40} won {:>7} draws, explains {:>7}",
                CandidateName(best, buf, sizeof(buf)), best_n,
                g_candExplains[best]);
  }

  // And the same winners cut by bound shader. A shader whose draws agree on one
  // register is evidence the register belongs to the shader; a shader whose
  // draws scatter means the scoring is fitting noise, and no table built from it
  // would be worth anything.
  for (const auto& [handle, draws] : g_shaderDraws) {
    if (draws < 32) continue;   // too few to say anything
    uint32_t top = 0, top_n = 0, distinct = 0;
    for (uint32_t c = 0; c < kCandidates; ++c) {
      auto it = g_shaderWins.find((uint64_t(handle) << 32) | c);
      if (it == g_shaderWins.end() || it->second == 0) continue;
      ++distinct;
      if (it->second > top_n) { top_n = it->second; top = c; }
    }
    if (!top_n) continue;
    REXLOG_INFO(
        "d3d9: stage3    vs 0x{:08X}: {} explained draws, top {} on {}/{} "
        "({}%), {} distinct winners",
        handle, draws, CandidateName(top, buf, sizeof(buf)), top_n, draws,
        (top_n * 100) / draws, distinct);
  }
}

}  // namespace mx::hle
