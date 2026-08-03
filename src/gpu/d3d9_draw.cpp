#include "gpu/d3d9_draw.h"

#include <cstring>
#include <vector>

#include "gpu/shader_ucode.h"   // ApplyFetchEndian

namespace mx::pm4 {

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

// Copy one vertex out of a stream and undo the guest byte order, so every later
// read is a plain little-endian read at its natural offset — which is what the
// hardware's fetch does and what ReadHleElement expects.
bool CopyVertex(const HleStream& s, uint32_t index, uint8_t* dst,
                uint32_t dst_bytes) {
  const uint64_t byte_off =
      uint64_t(index) * s.stride + s.offset_bytes;
  if (byte_off + s.stride > s.size_bytes) return false;
  const uint32_t n = s.stride < dst_bytes ? s.stride : dst_bytes;
  std::memcpy(dst, s.host + byte_off, n);
  ApplyFetchEndian(dst, n, s.endian);
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

  const HostTopology topo = Pm4Translator::MapTopology(in.prim_type);
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
      if (!ReadHleElement(vtx, s.stride, *pos, p)) {
        skip = HleSkip::kUnreadableFormat;
        return false;
      }
    }

    float c[4] = {1, 1, 1, 1};
    if (col) {
      const HleStream& s = in.streams[col->stream];
      if (s.stride <= sizeof(vtx) && CopyVertex(s, src_index, vtx, sizeof(vtx))) {
        if (!ReadHleElement(vtx, s.stride, *col, c)) {
          c[0] = c[1] = c[2] = c[3] = 1.0f;
        }
      }
    }

    std::memcpy(dst + 0, p, 12);       // float3 POSITION
    std::memcpy(dst + 12, c, 16);      // float4 COLOR
  }

  out.vertex_count = nverts;
  out.vertex_stride = kHostVertexStride;
  out.indices = std::move(indices);
  out.prim_type = in.prim_type;
  out.topology = topo;

  // The same two expansions the PM4 path uses, shared rather than reimplemented
  // — a second copy could drift from the one the renderer already agrees with.
  if (expand_rects) {
    if (Pm4Translator::ExpandRectangleList(out) == 0) {
      skip = HleSkip::kBadTopology;
      return false;
    }
  } else if (expand_quads) {
    if (Pm4Translator::ExpandQuadList(out) == 0) {
      skip = HleSkip::kBadTopology;
      return false;
    }
  }

  out.valid = true;
  out.color_source = col ? DrawCall::ColorSource::kPacked
                         : DrawCall::ColorSource::kNone;
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

}  // namespace mx::pm4
