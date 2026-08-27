#include "gpu/hle_types.h"

#include "gpu/shader_hlsl.h"

#include <algorithm>
#include <cstring>

namespace mx::hle {

HostTopology MapTopology(uint32_t prim_type) {
  switch (static_cast<PrimitiveType>(prim_type)) {
    case PrimitiveType::kPointList:     return HostTopology::kPointList;
    case PrimitiveType::kLineList:      return HostTopology::kLineList;
    case PrimitiveType::kLineStrip:     return HostTopology::kLineStrip;
    case PrimitiveType::kTriangleList:  return HostTopology::kTriangleList;
    case PrimitiveType::kTriangleStrip: return HostTopology::kTriangleStrip;
    // RectangleList and QuadList are not topologies, they are expansions —
    // ExpandRectangleList and ExpandQuadList rewrite them to triangle lists and
    // set the topology themselves.
    // TriangleFan has no D3D12 equivalent either, and is still not handled: it
    // is dropped and counted. Unlike the other two that is not a backlog item
    // for now — it does not appear once in a prim_type histogram of a full run,
    // so there is no measured population to justify the code.
    default:                            return HostTopology::kUndefined;
  }
}

namespace {

// Expand a PER-VERTEX byte array the same way ExpandRectangleList expands the
// vertex buffer: three supplied vertices per rect, permuted so the diagonal is
// first, plus a synthesized fourth.
//
// This exists because the vertex buffer was not the only per-vertex stream.
// The GPU vertex stage's input registers (dc.vertex_inputs) and the CPU path's
// interpolator stream (dc.interpolators) are both built BEFORE topology
// finalisation, sized to the incoming vertex_count of 3 -- so after expansion
// the index buffer referenced a fourth vertex whose registers did not exist.
// The vertex stage read zeros, the position came out (0,0,0,0), and the second
// triangle of every rectangle collapsed to a point at the screen centre.
//
// Measured in capture-ask.rdc at the 320x180 luminance pass: post-transform
// v3 = (0,0) where it should be (1,-1), and the target was covered by a single
// triangle with its bottom-right corner missing. Every full-screen post pass in
// this game is a RECTLIST, so each reduction stage lost half its area and the
// chain drove the average luminance toward zero.
//
// Every register here is affine across the rectangle -- position, colour, UV
// and any other interpolant alike -- so the same rule that gives the fourth
// corner gives the fourth vertex's registers.
//
// `starts` is the per-rect permutation ExpandRectangleList chose from the
// POSITIONS. It has to be passed in rather than recomputed, because this
// stream may not contain a position at all: choosing independently would let
// the interpolators be permuted differently from the vertices they belong to,
// which is worse than either choice made consistently.
void ExpandRectStream(std::vector<uint8_t>& stream, uint32_t stride,
                      uint32_t rects, const std::vector<uint8_t>& starts) {
  if (stream.empty() || !stride) return;
  if (stream.size() < size_t(rects) * 3 * stride) return;
  if (starts.size() < rects) return;
  std::vector<uint8_t> out;
  out.resize(size_t(rects) * 4 * stride);
  const uint32_t floats = stride / 4;
  for (uint32_t r = 0; r < rects; ++r) {
    const uint8_t* src = stream.data() + size_t(r) * 3 * stride;
    uint8_t* dst = out.data() + size_t(r) * 4 * stride;
    const uint32_t i0 = starts[r], i1 = (starts[r] + 1) % 3,
                   i2 = (starts[r] + 2) % 3;
    std::memcpy(dst + size_t(0) * stride, src + size_t(i0) * stride, stride);
    std::memcpy(dst + size_t(1) * stride, src + size_t(i1) * stride, stride);
    std::memcpy(dst + size_t(2) * stride, src + size_t(i2) * stride, stride);
    uint8_t* v3 = dst + size_t(3) * stride;
    for (uint32_t c = 0; c < floats; ++c) {
      float p0, p1, p2;
      std::memcpy(&p0, src + size_t(i0) * stride + c * 4, 4);
      std::memcpy(&p1, src + size_t(i1) * stride + c * 4, 4);
      std::memcpy(&p2, src + size_t(i2) * stride + c * 4, 4);
      const float p3 = p1 + p2 - p0;
      std::memcpy(v3 + c * 4, &p3, 4);
    }
  }
  stream = std::move(out);
}

}  // namespace

// How often each of the three rectangle arrangements was seen, indexed by the
// chosen first vertex. [0] is the case the old code assumed unconditionally,
// so [1] and [2] are exactly the rectangles it built wrong.
std::atomic<uint64_t> g_rectArrangement[3] = {};
std::atomic<uint64_t> g_rectDegenerate{0};

// See the note beside the declaration: the renderer writes these, the D3D9
// layer reads them, and neither can do the other's half of the job.
std::mutex g_luminanceReadbackMutex;
LuminanceReadback g_luminanceReadbacks[kMaxLuminanceReadbacks];
uint32_t g_luminanceReadbackCount = 0;
std::atomic<uint32_t> g_luminanceReadbackSeq{0};

// See the note beside the declaration. One slot, not a set: the population it
// serves is a single destination and a rotating array would be capacity nobody
// asked for.
std::mutex g_surfaceReadbackMutex;
SurfaceReadback g_surfaceReadback;
std::atomic<uint32_t> g_surfaceReadbackSeq{0};

uint32_t ExpandRectangleList(DrawCall& dc) {
  const uint32_t stride = dc.vertex_stride;
  if (stride < 12 || dc.vertices.size() < size_t(dc.vertex_count) * stride)
    return 0;
  const uint32_t rects = dc.vertex_count / 3;
  if (rects == 0) return 0;

  // One extra vertex per rect, six indices per rect. Indices are rewritten
  // wholesale — a rectangle list is always an auto-draw in this game, so the
  // incoming indices are the sequential ones we synthesized.
  std::vector<uint8_t> verts;
  verts.reserve(size_t(rects) * 4 * stride);
  std::vector<uint32_t> idx;
  idx.reserve(size_t(rects) * 6);

  // Which of the three supplied vertices starts the strip, per rect. Chosen
  // from the positions here and reused for every other per-vertex stream.
  std::vector<uint8_t> starts(rects, 0);

  for (uint32_t r = 0; r < rects; ++r) {
    const uint8_t* src = dc.vertices.data() + size_t(r) * 3 * stride;
    const uint32_t base = r * 4;

    // Three corners of a rectangle arrive, and which three is NOT fixed: the
    // right-angle corner can be any of them, so the diagonal can be any edge.
    // Transcribed from xenia-edge `spirv_builtin_geometry_shader.cc:671`,
    // which mirrors a vertex across the LONGEST edge:
    //
    //   0---1        1---2        2---0
    //   |  /|        |  /|        |  /|
    //   | / |        | / |        | / |
    //   |/  |        |/  |        |/  |
    //   2--[3]       0--[3]       1--[3]
    //   12 longest   20 longest   01 longest
    //   strip 0123   strip 1203   strip 2013
    //
    // and in every case the fourth corner is v_i1 + v_i2 - v_i0 over the
    // PERMUTED triple. This code used to assume the first arrangement always,
    // which is why the formula has been flipped once before on the evidence of
    // a single menu quad — a case-dependent rule tuned to one case. The other
    // two arrangements built a quad from the wrong corner: one triangle a
    // wedge, the other folded away off-screen.
    //
    // Squared lengths, x/y only and no perspective divide, exactly as the
    // reference does it. These positions are the guest vertex shader's own
    // clip-space exports (FinalizeHleTopology runs after execution), so this
    // measures the same space the reference's geometry stage measures.
    auto pos = [&](uint32_t v, uint32_t c) {
      float f;
      std::memcpy(&f, src + size_t(v) * stride + c * 4, 4);
      return f;
    };
    float edge[3];
    for (uint32_t i = 0; i < 3; ++i) {
      const uint32_t a = (1 + i) % 3, b = (2 + i) % 3;
      const float dx = pos(b, 0) - pos(a, 0);
      const float dy = pos(b, 1) - pos(a, 1);
      edge[i] = dx * dx + dy * dy;
    }
    const uint32_t start = (edge[0] > edge[1] && edge[0] > edge[2]) ? 0u
                           : (edge[1] > edge[2])                    ? 1u
                                                                   : 2u;
    starts[r] = uint8_t(start);
    ++g_rectArrangement[start];
    // All three edges equal means there is no diagonal to find and the choice
    // above is arbitrary. Counted rather than handled: a degenerate rectangle
    // covers nothing whichever corner is synthesized.
    if (edge[0] == edge[1] && edge[1] == edge[2]) ++g_rectDegenerate;

    const uint32_t i0 = start, i1 = (start + 1) % 3, i2 = (start + 2) % 3;
    for (uint32_t v : {i0, i1, i2})
      verts.insert(verts.end(), src + size_t(v) * stride,
                   src + size_t(v + 1) * stride);

    // Placeholder bytes for the synthesized corner, overwritten component by
    // component below. Every float of the vertex is interpolated, not the
    // first nine: the host vertex is 40 bytes, so a nine-float clamp left
    // TEXCOORD0.y alone and collapsed one edge's V into a smear — the exact
    // failure the old comment here warned about while still committing it.
    verts.insert(verts.end(), src + size_t(i2) * stride,
                 src + size_t(i2 + 1) * stride);
    uint8_t* v3 = verts.data() + (size_t(base) + 3) * stride;
    for (uint32_t c = 0; c < stride / 4; ++c) {
      const float p3 = pos(i1, c) + pos(i2, c) - pos(i0, c);
      std::memcpy(v3 + c * 4, &p3, 4);
    }

    // v0..v3 do not run round the perimeter — v3 is opposite v0 — so the two
    // triangles share the v1-v2 diagonal rather than v0-v2. Both wind the same
    // way, which the {0,1,2, 0,2,3} order did not once v3 moved. This is the
    // triangle-list spelling of the reference's four-vertex strip.
    const uint32_t order[6] = {0, 1, 2, 2, 1, 3};
    for (uint32_t i = 0; i < 6; ++i) idx.push_back(base + order[i]);
  }

  // The other per-vertex streams have to grow with it, under the SAME
  // permutation. See ExpandRectStream.
  ExpandRectStream(dc.vertex_inputs, dc.vertex_input_count * 16u, rects,
                   starts);
  ExpandRectStream(dc.interpolators,
                   kHlslInterpolatorLinkage * 4u * uint32_t(sizeof(float)),
                   rects, starts);

  dc.vertices = std::move(verts);
  dc.vertex_count = rects * 4;
  dc.index_count = rects * 6;
  // Stay 16-bit while the counts allow it; the renderer reads index_16bit.
  dc.index_16bit = dc.vertex_count <= 0xFFFF;
  dc.indices.resize(size_t(idx.size()) * (dc.index_16bit ? 2 : 4));
  if (dc.index_16bit) {
    auto* p = reinterpret_cast<uint16_t*>(dc.indices.data());
    for (size_t i = 0; i < idx.size(); ++i) p[i] = uint16_t(idx[i]);
  } else {
    auto* p = reinterpret_cast<uint32_t*>(dc.indices.data());
    for (size_t i = 0; i < idx.size(); ++i) p[i] = idx[i];
  }
  dc.topology = HostTopology::kTriangleList;
  return rects;
}

uint32_t ExpandQuadList(DrawCall& dc) {
  const uint32_t istride = dc.index_16bit ? 2u : 4u;
  const uint32_t have = uint32_t(dc.indices.size() / istride);
  const uint32_t quads = have / 4;
  if (quads == 0) return 0;

  // Unlike a rectangle, a quad has all four of its corners present, so nothing
  // is synthesized: the vertex buffer is untouched and only the index buffer is
  // rewritten. That also means this maps *through* the incoming indices rather
  // than assuming they are the sequential ones an auto-draw synthesizes, so it
  // is correct for a real DRAW_INDX with its own index buffer as well.
  auto read = [&](uint32_t i) -> uint32_t {
    if (dc.index_16bit) {
      uint16_t v;
      std::memcpy(&v, dc.indices.data() + size_t(i) * 2, 2);
      return v;
    }
    uint32_t v;
    std::memcpy(&v, dc.indices.data() + size_t(i) * 4, 4);
    return v;
  };

  std::vector<uint32_t> idx;
  idx.reserve(size_t(quads) * 6);
  for (uint32_t q = 0; q < quads; ++q) {
    const uint32_t c[4] = {read(q * 4 + 0), read(q * 4 + 1), read(q * 4 + 2),
                           read(q * 4 + 3)};
    // The four corners come round the perimeter, so the two triangles share the
    // v0-v2 diagonal. Splitting on v1-v3 instead gives the same silhouette for a
    // planar convex quad but the wrong interpolation across it, and is visibly
    // wrong the moment the quad is not planar — the plausible-but-wrong class,
    // so it is written down rather than left to the reader.
    const uint32_t order[6] = {0, 1, 2, 0, 2, 3};
    for (uint32_t i = 0; i < 6; ++i) idx.push_back(c[order[i]]);
  }

  dc.index_count = quads * 6;
  // Indices address the untouched vertex buffer, so the width is decided by
  // vertex_count exactly as in the rectangle path.
  dc.index_16bit = dc.vertex_count <= 0xFFFF;
  dc.indices.resize(size_t(idx.size()) * (dc.index_16bit ? 2 : 4));
  if (dc.index_16bit) {
    auto* p = reinterpret_cast<uint16_t*>(dc.indices.data());
    for (size_t i = 0; i < idx.size(); ++i) p[i] = uint16_t(idx[i]);
  } else {
    auto* p = reinterpret_cast<uint32_t*>(dc.indices.data());
    for (size_t i = 0; i < idx.size(); ++i) p[i] = idx[i];
  }
  dc.topology = HostTopology::kTriangleList;
  return quads;
}

}  // namespace mx::hle
