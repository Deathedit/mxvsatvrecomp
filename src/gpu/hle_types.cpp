#include "gpu/hle_types.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace mx::hle {

ExportSpace ClassifyExportSpace(float x, float y, float w, float xs, float xo,
                                float ys, float yo) {
  (void)xo;
  (void)yo;
  if (w != 0.0f) { x /= w; y /= w; }
  if (!std::isfinite(x) || !std::isfinite(y)) return ExportSpace::kNeither;
  // Degenerate first: (0,0,0,w=0) is inside the unit cube and inside the
  // viewport rectangle both, so it is evidence for nothing.
  if (w == 0.0f || (std::fabs(x) < 1e-4f && std::fabs(y) < 1e-4f))
    return ExportSpace::kDegenerate;
  if (xs == 0.0f || ys == 0.0f) return ExportSpace::kNeither;

  const float wpx = std::fabs(xs) * 2.0f, wpy = std::fabs(ys) * 2.0f;
  const bool clip_like =
      x >= -1.05f && x <= 1.05f && y >= -1.05f && y <= 1.05f;
  const bool win_like =
      x >= -1.0f && x <= wpx + 1.0f && y >= -1.0f && y <= wpy + 1.0f;
  // Clip wins the tie: the regions overlap near the origin, and the tie must
  // go against the hypothesis being tested rather than for it.
  if (clip_like) return ExportSpace::kClipLike;
  if (win_like) return ExportSpace::kWindowLike;
  return ExportSpace::kNeither;
}

const char* ExportSpaceName(ExportSpace s) {
  switch (s) {
    case ExportSpace::kDegenerate: return "degenerate";
    case ExportSpace::kClipLike:   return "clip-like";
    case ExportSpace::kWindowLike: return "window-like";
    default:                       return "neither";
  }
}

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

  for (uint32_t r = 0; r < rects; ++r) {
    const uint8_t* src = dc.vertices.data() + size_t(r) * 3 * stride;
    const uint32_t base = r * 4;
    verts.insert(verts.end(), src, src + size_t(3) * stride);

    // v3 = v0 + v2 - v1 for every host interpolant. Position, colour and UV
    // describe the same affine plane; copying v2's UV makes textured rectangles
    // collapse one edge into a smear.
    verts.insert(verts.end(), src + size_t(2) * stride,
                 src + size_t(3) * stride);
    uint8_t* v3 = verts.data() + (size_t(base) + 3) * stride;
    const uint32_t float_components = std::min(stride / 4, 9u);
    for (uint32_t c = 0; c < float_components; ++c) {
      float p0, p1, p2;
      std::memcpy(&p0, src + c * 4, 4);
      std::memcpy(&p1, src + stride + c * 4, 4);
      std::memcpy(&p2, src + size_t(2) * stride + c * 4, 4);
      const float p3 = p0 + p2 - p1;
      std::memcpy(v3 + c * 4, &p3, 4);
    }

    const uint32_t order[6] = {0, 1, 2, 0, 2, 3};
    for (uint32_t i = 0; i < 6; ++i) idx.push_back(base + order[i]);
  }

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
