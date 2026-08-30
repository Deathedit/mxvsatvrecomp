#include "gpu/d3d9_draw.h"

#include "gpu/guard_census.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <map>
#include <mutex>
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
//
// ---------------------------------------------------------------------------
// A stream that ends before the vertex does is NOT a reason to lose the draw.
// This used to `return false`, which became kVertexOutOfRange and discarded the
// whole draw -- 27713 of 172500, 16% of every frame, and the single largest
// reason draws never reached the renderer at all. It is also why those draws
// were invisible in a GPU capture: they were thrown away before one existed.
// Zero-filling instead is what brought the ground back.
//
// The guest binds streams whose fetch-constant size is smaller than the range a
// draw indexes, and our snapshot of that constant AGREES with the device's, so
// the size is not misread -- the hardware simply tolerates the overrun and
// returns zero, which is what the reference does too. Zero-filling from the end
// of the stream reproduces that; the position then reads 0 and the primitive
// collapses, exactly as it would on hardware, instead of the draw vanishing.
//
// `stream_index` exists only for the census below. The overrun is far larger
// than "a few vertices at the tail" -- 304 million fills in one run -- and the
// terrain came back streaked, so the open question is WHICH stream is short and
// by how much. Three hypotheses died to reasoning about that without data
// (instance-data streams, stale bindings, a measurement artifact); this is the
// measurement that replaces them.
bool CopyVertex(const HleStream& s, uint32_t index, uint8_t* dst,
                uint32_t dst_bytes, uint32_t stream_index) {
  const uint64_t byte_off =
      uint64_t(index) * s.stride + s.offset_bytes;
  const uint32_t n = s.stride < dst_bytes ? s.stride : dst_bytes;
  const uint64_t avail =
      s.size_bytes > byte_off ? uint64_t(s.size_bytes) - byte_off : 0;
  const uint32_t copy = avail < n ? uint32_t(avail) : n;
  // Population is EVERY vertex copy, fires only the short ones. Noted before
  // the branch so the denominator cannot drift from the numerator -- 58,138
  // zero-fills is unreadable until you know it is out of how many.
  mx::gpu::guard::Note(mx::gpu::guard::Guard::kVertexZeroFill, copy < n);
  if (copy < n) {
    std::memset(dst, 0, n);
    ++HleVertexZeroFillCount();
    auto& c = HleZeroFillCensus();
    if (stream_index < HleZeroFillCensusStreams) {
      auto& st = c.stream[stream_index];
      ++st.fills;
      // How far past the end, in whole vertices. A tail of 1 is a buffer that
      // is simply one vertex short; a number in the thousands means the index
      // has no relationship to this stream's contents at all, and those two
      // want completely different fixes.
      const uint64_t over = byte_off + n > s.size_bytes
                                ? (byte_off + n) - s.size_bytes : 0;
      const uint64_t over_verts = s.stride ? over / s.stride : 0;
      if (over_verts > st.worst_vertices_past_end)
        st.worst_vertices_past_end = over_verts;
      if (st.first.fills == 0) {
        st.first.fills = 1;
        st.first.stride = s.stride;
        st.first.size_bytes = s.size_bytes;
        st.first.offset_bytes = s.offset_bytes;
        st.first.index = index;
        st.first.byte_off = byte_off;
      }
    }
  }
  // byte_off past the end leaves copy == 0, so s.host is never touched there.
  if (copy) std::memcpy(dst, s.host + byte_off, copy);
  return true;
}

// POSITION 0 is required; COLOR 0 and TEXCOORD 0 are optional and are dropped
// rather than fatal when their stream is unusable. Shared by BuildHleDraw and
// the deferred transcode so the two cannot resolve a declaration differently.
bool ResolveTranscodeElements(const HleDrawInputs& in,
                              const HleInputElement*& pos,
                              const HleInputElement*& col,
                              const HleInputElement*& tex, HleSkip& skip) {
  skip = HleSkip::kNone;
  pos = col = tex = nullptr;
  if (!in.layout || in.layout->elements.empty()) {
    skip = HleSkip::kNoLayout;
    return false;
  }
  pos = FindUsage(*in.layout, kUsagePosition, 0);
  if (!pos) {
    skip = HleSkip::kNoPosition;
    return false;
  }
  col = FindUsage(*in.layout, kUsageColor, 0);
  tex = FindUsage(*in.layout, kUsageTexcoord, 0);

  auto stream_ok = [&](const HleInputElement* e) -> HleSkip {
    if (!e) return HleSkip::kNone;
    if (e->stream >= kMaxStreams) return HleSkip::kStreamUnbound;
    const HleStream& s = in.streams[e->stream];
    if (!s.bound || !s.host) return HleSkip::kStreamUnbound;
    if (s.stride == 0) return HleSkip::kZeroStride;
    return HleSkip::kNone;
  };
  if ((skip = stream_ok(pos)) != HleSkip::kNone) return false;
  if (stream_ok(col) != HleSkip::kNone) col = nullptr;
  if (stream_ok(tex) != HleSkip::kNone) tex = nullptr;
  skip = HleSkip::kNone;
  return true;
}

}  // namespace

uint64_t g_transcodeUs = 0;
uint64_t g_transcodeVerts = 0;

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
  // What the renderer is finally told. It differs from `topo` only where a
  // primitive restart forces a strip to be walked into a list below, which is a
  // decision that cannot be made until the indices have been read.
  HostTopology final_topo = topo;

  // The declaration states which element is the position. No inference, no
  // fallback — this is the single biggest difference from the PM4 path, where
  // PickPositionAttribute has to trace the microcode to the position export and
  // guesses when that fails.
  //
  // Colour and the first texcoord set are optional: a declaration without a
  // colour seeds the modulation identity (see the seed in the transcode — it is
  // a factor, not a colour), and a missing texcoord defaults to (0,0). Both are
  // dropped rather than fatal when their stream is unusable, because neither is
  // worth losing the geometry over.
  const HleInputElement *pos = nullptr, *col = nullptr, *tex = nullptr;
  if (!ResolveTranscodeElements(in, pos, col, tex, skip)) return false;

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
    // Condition each index exactly as the hardware does before it reaches the
    // vertex fetch -- see HleDrawInputs' index_* fields for the reference this
    // transcribes. Doing none of this is what lost the terrain: a single
    // 0xFFFF restart index put vmax at 65535, which made the referenced range
    // 65536 vertices wide, which ran past the stream and refused the draw.
    // Not a valid 24-bit index, so it cannot collide with a real one.
    constexpr uint32_t kRestartSlot = 0xFFFFFFFFu;
    uint32_t vmin = 0xFFFFFFFFu, vmax = 0;
    std::vector<uint32_t> raw(in.count);
    bool any_vertex = false;
    bool any_restart = false;
    for (uint32_t i = 0; i < in.count; ++i) {
      const uint8_t* p = in.index.host + uint64_t(in.first + i) * w;
      uint32_t v = ((w == 4) ? RdIndex32(p) : RdIndex16(p)) & 0xFFFFFFu;
      // A restart is not a vertex. It keeps a slot in `raw` so the primitive
      // walk below can see WHERE the cut is; it is never turned into an index.
      if (in.index_reset_enabled && v == in.index_reset) {
        raw[i] = kRestartSlot;
        any_restart = true;
        ++HleRestartCutCount();
        continue;
      }
      // base_vertex ONLY. D3D9's BaseVertexIndex is what the runtime programs
      // into VGT_INDX_OFFSET, so they are the same number arriving by two
      // routes -- adding both double-counts it, pushes every index past the
      // real range, and the clamp below then squashes the draw onto index_max.
      // That is what erased the terrain a second time after the restart fix.
      // index_offset is read and reported so the equality can be checked
      // rather than assumed, but it is deliberately NOT added here.
      const int64_t adjusted = int64_t(v) + in.base_vertex;
      if (adjusted < 0) { skip = HleSkip::kVertexOutOfRange; return false; }
      v = uint32_t(adjusted) & 0xFFFFFFu;
      if (v < in.index_min) v = in.index_min;
      if (v > in.index_max) v = in.index_max;
      raw[i] = v;
      if (v < vmin) vmin = v;
      if (v > vmax) vmax = v;
      any_vertex = true;
    }
    if (!any_vertex) { skip = HleSkip::kEmpty; return false; }
    lo = vmin;
    hi = vmax;
    if (uint64_t(hi - lo) + 1 > kMaxHleVertices) {
      skip = HleSkip::kTooManyVertices;
      return false;
    }
    // The renderer's index view is 16-bit here; a window wider than that is
    // counted rather than silently truncated. Checked once against the window
    // rather than per index, which is the same test: every index kept below
    // lies in [lo, hi] by construction, because lo and hi ARE their extremes.
    if (uint64_t(hi - lo) > 0xFFFFu) {
      skip = HleSkip::kTooManyVertices;
      return false;
    }
    // Absolute indices are not rebased, so it is the TOP of the range that
    // must fit the 16-bit index view, not its width.
    if (in.absolute_indices && hi > 0xFFFFu) {
      skip = HleSkip::kTooManyVertices;
      return false;
    }
    indices.reserve(size_t(in.count) * 2);
    auto emit = [&](uint32_t v) {
      const uint32_t r = in.absolute_indices ? v : (v - lo);
      indices.push_back(uint8_t(r & 0xFF));
      indices.push_back(uint8_t(r >> 8));
    };

    if (!any_restart) {
      for (uint32_t i = 0; i < in.count; ++i) emit(raw[i]);
    } else {
      // A restart cannot be expressed AS an index, and substituting one -- which
      // is what this did first -- does not end a strip. It welds the last
      // vertices of one patch to the first of the next, and since consecutive
      // patches sit anywhere on the map, those welds were the flat sheets
      // stretching across the terrain and up over the horizon.
      //
      // D3D12 can cut a strip in the IA (IBStripCutValue), but that is a PSO
      // property, and these indices are rebased onto the draw's own vertex
      // window where 0xFFFF is a legal vertex -- the cut value would collide
      // with real geometry. So the cut is resolved here instead: the strip is
      // walked into a list, a form no marker has to survive into.
      ++HleRestartCutDraws();
      switch (topo) {
        case HostTopology::kTriangleStrip:
        case HostTopology::kLineStrip: {
          const bool tri = topo == HostTopology::kTriangleStrip;
          const uint32_t need = tri ? 2u : 1u;
          uint32_t run = 0;   // first vertex of the strip currently being walked
          for (uint32_t i = 0; i < in.count; ++i) {
            if (raw[i] == kRestartSlot) { run = i + 1; continue; }
            const uint32_t at = i - run;
            if (at < need) continue;     // this strip has no primitive yet
            if (!tri) { emit(raw[i - 1]); emit(raw[i]); continue; }
            // Strip winding alternates, and it alternates from the start of THIS
            // strip: a cut resets the parity along with the vertices. Getting
            // that wrong flips every other triangle in every patch after the
            // first cut, which back-face culling then removes.
            if (at & 1) { emit(raw[i - 1]); emit(raw[i - 2]); }
            else        { emit(raw[i - 2]); emit(raw[i - 1]); }
            emit(raw[i]);
          }
          final_topo = tri ? HostTopology::kTriangleList
                           : HostTopology::kLineList;
          break;
        }
        default: {
          if (topo == HostTopology::kUndefined) {
            // RectangleList and QuadList, which are not topologies yet. Their
            // index stream has to keep its shape for the expansion that rewrites
            // it later, so a marker degenerates against the draw's own lowest
            // vertex here instead of removing a slot.
            for (uint32_t i = 0; i < in.count; ++i)
              emit(raw[i] == kRestartSlot ? vmin : raw[i]);
            break;
          }
          // A list topology with a marker in it. The marker is not a vertex, so
          // the primitive containing it is short a corner and is dropped whole
          // rather than closed with a substitute.
          const uint32_t n = (topo == HostTopology::kTriangleList) ? 3u
                           : (topo == HostTopology::kLineList)     ? 2u
                                                                   : 1u;
          for (uint32_t i = 0; i + n <= in.count; i += n) {
            bool whole = true;
            for (uint32_t k = 0; k < n; ++k)
              if (raw[i + k] == kRestartSlot) whole = false;
            if (!whole) continue;
            for (uint32_t k = 0; k < n; ++k) emit(raw[i + k]);
          }
          break;
        }
      }
    }
    if (indices.empty()) { skip = HleSkip::kEmpty; return false; }
    out.index_16bit = true;
    out.index_count = uint32_t(indices.size() / 2);
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
    // The non-indexed twin of `emit` above, and it has to agree with it or a
    // DrawVertices call lands on the wrong instances.
    const uint32_t id_base = in.absolute_indices ? in.first : 0;
    for (uint32_t i = 0; i < in.count; ++i) {
      const uint32_t v = id_base + i;
      indices[i * 2 + 0] = uint8_t(v & 0xFF);
      indices[i * 2 + 1] = uint8_t(v >> 8);
    }
    if (id_base + in.count > 0xFFFF) {
      skip = HleSkip::kTooManyVertices;
      return false;
    }
    out.index_16bit = true;
    out.index_count = in.count;
  }

  const uint32_t nverts = hi - lo + 1;
  out.first_vertex = lo;
  out.vertex_count = nverts;

  if (in.defer_transcode) {
    // The caller predicts this draw fetches on the GPU. Leave the host vertices
    // unbuilt — but reproduce, in O(1), every condition under which the loop
    // below would have REFUSED the draw, so a deferred draw is dropped in
    // exactly the cases an eager one is. Otherwise turning the prediction on
    // would silently start rendering geometry that used to be discarded.
    //
    // All three are constant or monotone across the range: the stride bound and
    // the position format do not vary per vertex, and the buffer overrun is
    // worst at the highest index.
    const HleStream& s = in.streams[pos->stream];
    if (s.stride > 256) {
      skip = HleSkip::kZeroStride;
      return false;
    }
    uint8_t probe[256];
    float pp[4] = {0, 0, 0, 1};
    if (!CopyVertex(s, lo, probe, sizeof(probe), pos->stream) ||
        !CopyVertex(s, hi, probe, sizeof(probe), pos->stream)) {
      skip = HleSkip::kVertexOutOfRange;
      return false;
    }
    if (!ReadHleElement(probe, s.stride, *pos, s.endian, pp)) {
      skip = HleSkip::kUnreadableFormat;
      return false;
    }
    out.vertex_stride = 0;
  } else if (!TranscodeHleVertices(in, out, skip)) {
    return false;
  }

  if (in.mvp) {
    std::memcpy(out.mvp, in.mvp, sizeof(out.mvp));
  } else {
    static const float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0,
                                        0, 0, 1, 0, 0, 0, 0, 1};
    std::memcpy(out.mvp, kIdentity, sizeof(out.mvp));
  }

  out.indices = std::move(indices);
  out.prim_type = in.prim_type;
  out.topology = final_topo;

  // The same two expansions the PM4 path uses, shared rather than reimplemented
  // — a second copy could drift from the one the renderer already agrees with.
  out.valid = true;
  out.color_source = col ? DrawCall::ColorSource::kPacked
                         : DrawCall::ColorSource::kNone;
  return true;
}

bool TranscodeHleVertices(const HleDrawInputs& in, DrawCall& out,
                          HleSkip& skip) {
  const HleInputElement *pos = nullptr, *col = nullptr, *tex = nullptr;
  if (!ResolveTranscodeElements(in, pos, col, tex, skip)) return false;

  const uint32_t lo = out.first_vertex;
  const uint32_t nverts = out.vertex_count;
  if (nverts == 0) {
    skip = HleSkip::kEmpty;
    return false;
  }

  // --- the vertices --------------------------------------------------------
  //
  // Bounded by each stream's own size. Stage 0 established that a range not
  // fitting is *not* proof the description is wrong — the shader issues its own
  // vfetch and need not index every stream alike — so this refuses the draw and
  // counts it rather than clamping into whatever follows the buffer.
  out.vertices.resize(size_t(nverts) * kHostVertexStride);

  // The second per-vertex CPU pass, measured. ApplyShaderOutputs' loop was the
  // one the FRAME COST buckets caught; this one is upstream of it and was part
  // of the unaccounted remainder. Read by the caller and reset each frame.
  const auto transcode_t0 = std::chrono::steady_clock::now();

  // A stream whose fetch the shader indexes by a computed register carries
  // no value this path can read: `src_index` is the vertex, and the row the
  // shader would have addressed is a different one entirely. Reading it
  // anyway yields an unrelated vertex -- plausible, wrong, and invisible.
  // The attribute keeps its default instead, which is what an out-of-range
  // fetch already produces here, and is counted so the gap is visible
  // rather than inferred.
  const auto unknowable = [&](uint32_t stream) {
    return ((in.computed_index_streams >> stream) & 1u) != 0;
  };

  uint8_t vtx[256];   // one guest vertex, larger than any observed stride
  for (uint32_t i = 0; i < nverts; ++i) {
    const uint32_t src_index = lo + i;
    uint8_t* dst = out.vertices.data() + size_t(i) * kHostVertexStride;

    float p[4] = {0, 0, 0, 1};
    {
      const HleStream& s = in.streams[pos->stream];
      if (s.stride > sizeof(vtx)) { skip = HleSkip::kZeroStride; return false; }
      if (unknowable(pos->stream)) {
        ++HleComputedIndexSkips();
      } else if (!CopyVertex(s, src_index, vtx, sizeof(vtx), pos->stream)) {
        skip = HleSkip::kVertexOutOfRange;
        return false;
      }
      if (!ReadHleElement(vtx, s.stride, *pos, s.endian, p)) {
        skip = HleSkip::kUnreadableFormat;
        return false;
      }
    }

    // One, and it is a modulation identity rather than a colour. The textured
    // pixel shader is `g_tex.Sample(g_smp, uv) * col` (d3d12_shaders.h), so for
    // a declaration with no COLOR element this is what passes the texture
    // through unchanged.
    //
    // Measured, 2026-08-07: setting this to {0,0,0,0} made the logo disappear
    // and left the frame at the clear colour, because it multiplies every
    // texture by zero. Do not do it again. The reasoning that motivated it was
    // sound about the guest and wrong about us — D3D9 really does answer an
    // unmatched interpolator by NOPing instructions (sub_82565278, from the
    // merge join in sub_82565400, writes {0xC8000000, 0, 0x02000000}:
    // kRetainPrev / kMax with both write masks and export_data clear). But that
    // is about a shader we do not run. Here the value is a factor in someone
    // else's multiply, and 1 is its identity.
    //
    // Corrected 2026-08-07: the shader it patches is the VERTEX shader, not the
    // pixel shader as this said. sub_82565928 passes the vertex shader (the one
    // it hands to D3D_PatchVertexShaderToMatchVertexDeclaration) as the patched
    // side, so sub_82565278 eliminates vertex EXPORTS the pixel shader does not
    // read, and sub_82565348 rewrites an export's register to the one the pixel
    // shader expects. The direction matters if anyone revisits interpolator
    // linkage: a remap keyed the other way is a no-op, measured.
    //
    // The white overpaint is therefore NOT this constant. It is the untextured
    // path, where `col` is the output rather than a factor, and the compositor
    // passes whose render target has no content (see AGENTS.md).
    float c[4] = {1, 1, 1, 1};
    if (col) {
      const HleStream& s = in.streams[col->stream];
      if (unknowable(col->stream)) {
        ++HleComputedIndexSkips();
      } else if (s.stride <= sizeof(vtx) &&
                 CopyVertex(s, src_index, vtx, sizeof(vtx), col->stream)) {
        if (!ReadHleElement(vtx, s.stride, *col, s.endian, c)) {
          c[0] = c[1] = c[2] = c[3] = 1.0f;
        }
      }
    }

    float t[4] = {0, 0, 0, 0};
    if (tex) {
      const HleStream& s = in.streams[tex->stream];
      if (unknowable(tex->stream)) {
        ++HleComputedIndexSkips();
      } else if (s.stride <= sizeof(vtx) &&
                 CopyVertex(s, src_index, vtx, sizeof(vtx), tex->stream)) {
        if (!ReadHleElement(vtx, s.stride, *tex, s.endian, t)) {
          t[0] = t[1] = 0.0f;
        }
      }
    }

    // p[3] is 1 for a declaration position, which is what an untransformed or
    // pre-transformed vertex needs. ApplyShaderOutputs overwrites all four
    // components with the shader's own clip-space export where it runs.
    std::memcpy(dst + 0, p, 16);       // float4 POSITION (clip space)
    std::memcpy(dst + 16, c, 16);      // float4 COLOR
    std::memcpy(dst + 32, t, 8);       // float2 TEXCOORD0
  }
  g_transcodeUs +=
      uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                   std::chrono::steady_clock::now() - transcode_t0)
                   .count());
  g_transcodeVerts += nverts;

  out.vertex_stride = kHostVertexStride;
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

namespace {

// One record list per thread. Registered in a global table so the join can find
// the workers' lists; the table is touched only on first use per thread and at
// merge time, never on the per-draw path.
constexpr uint32_t kNotAWorker = 0xFFFFFFFFu;
struct ThreadDrawList {
  std::vector<DrawCall> draws;
  uint32_t index = kNotAWorker;
};

std::mutex g_listsMutex;
std::vector<ThreadDrawList*> g_lists;
thread_local ThreadDrawList* t_list = nullptr;

ThreadDrawList& LocalList() {
  if (!t_list) {
    // Deliberately leaked: these threads live for the life of the process, and
    // a list outliving its thread is harmless where freeing it under a merge
    // would not be.
    t_list = new ThreadDrawList();
    std::lock_guard<std::mutex> lock(g_listsMutex);
    g_lists.push_back(t_list);
  }
  return *t_list;
}

}  // namespace

std::recursive_mutex& HleGlobalMutex() {
  static std::recursive_mutex m;
  return m;
}

std::vector<DrawCall>& HleFrameDraws() { return LocalList().draws; }

void HleSetThreadRecordIndex(uint32_t index) { LocalList().index = index; }

void HleMergeWorkerDraws() {
  auto& dst = LocalList();
  std::vector<ThreadDrawList*> workers;
  {
    std::lock_guard<std::mutex> lock(g_listsMutex);
    for (auto* l : g_lists)
      if (l != &dst && l->index != kNotAWorker && !l->draws.empty())
        workers.push_back(l);
  }
  std::sort(workers.begin(), workers.end(),
            [](const ThreadDrawList* a, const ThreadDrawList* b) {
              return a->index < b->index;
            });
  for (auto* w : workers) {
    dst.draws.insert(dst.draws.end(), std::make_move_iterator(w->draws.begin()),
                     std::make_move_iterator(w->draws.end()));
    w->draws.clear();
  }
}

uint64_t* HleSkipCounts() {
  static uint64_t counts[size_t(HleSkip::kCount)] = {};
  return counts;
}

uint64_t& HleBuiltCount() {
  static uint64_t n = 0;
  return n;
}

uint64_t& HleVertexZeroFillCount() {
  static uint64_t n = 0;
  return n;
}

uint64_t& HleComputedIndexSkips() {
  static uint64_t n = 0;
  return n;
}

uint64_t& HleRestartCutDraws() {
  static uint64_t n = 0;
  return n;
}

uint64_t& HleRestartCutCount() {
  static uint64_t n = 0;
  return n;
}

HleZeroFillCensusData& HleZeroFillCensus() {
  static HleZeroFillCensusData c;
  return c;
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
