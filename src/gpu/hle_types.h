#pragma once

#include "gpu/shader_alu.h"
#include "gpu/shader_ucode.h"

#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <vector>

namespace mx::hle {

enum class HostTextureFormat : uint8_t {
  kRgba8 = 0,
  kBc1,
  kBc2,
  kBc3,
  kBc5,
  kR16Float,
  kRgba16Float,
  // Added from measurement, not from the enum: these are the only guest
  // formats the decoder was seen to turn down across four runs. kBgra4 is the
  // front end's own format (3/3 runs, the sole rejection there); kR8 and kR16
  // appear only under --force_load=NAT_Farm.
  kBgra4,
  kR8,
  kR16,
  // Only became visible once kR8/kR16 stopped failing: those rejections were
  // ending the binding scan before it reached this sampler. Measured 71/90/79
  // across three farm runs.
  kR32Float,
};

// Immutable CPU-side base mip shared by all draws that reference it. The hook
// owns guest-memory access; the renderer only sees validated host bytes.
struct HleTexturePayload {
  uint64_t key = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t row_pitch = 0;
  HostTextureFormat format = HostTextureFormat::kRgba8;
  uint32_t swizzle = 0;
  uint8_t clamp_x = 0;
  uint8_t clamp_y = 0;
  bool linear_filter = true;
  std::vector<uint8_t> data;
};

// Mimics PrimitiveType from xenos.h (subset — only values seen in MX vs ATV).
// Fan is 5 and strip is 6, per Xenia's xenos::PrimitiveType. This enum had them
// the other way round until 2026-08-02, which would have drawn every fan as a
// strip once topology started being honoured.
enum class PrimitiveType : uint8_t {
  kPointList        = 0x01,
  kLineList         = 0x02,
  kLineStrip        = 0x03,
  kTriangleList    = 0x04,
  kTriangleFan     = 0x05,
  kTriangleStrip   = 0x06,
  kRectangleList   = 0x08,
  // 0x0C kLineLoop, 0x0E kQuadStrip and 0x0F kPolygon exist in xenos.h too but
  // are omitted here because this game emits none of them. QuadList it does
  // emit, in greater volume than any other type.
  kQuadList        = 0x0D,
  kUnknown         = 0xFF,
};

// Host topology, carried on DrawCall so the renderer stays a dumb consumer.
// The values are deliberately the D3D_PRIMITIVE_TOPOLOGY ones so the renderer
// can cast rather than translate; d3d12_game.cpp static_asserts that they still
// match. Declaring them here rather than including <d3dcommon.h> keeps this
// header usable from translator_test.cpp.
enum class HostTopology : uint32_t {
  kUndefined     = 0,
  kPointList     = 1,
  kLineList      = 2,
  kLineStrip     = 3,
  kTriangleList  = 4,
  kTriangleStrip = 5,
};

struct DrawCall {
  std::vector<uint8_t> vertices;      // optional; filled only when a vertex fetch const is known
  std::vector<uint8_t> indices;       // packed index buffer (2 or 4 bytes per index)
  uint32_t vertex_count = 0;
  uint32_t index_count = 0;
  uint32_t vertex_stride = 0;
  // The fetch constant's endian swap mode for `vertices` (0 none, 1 8in16,
  // 2 8in32). The bytes are left in guest order and the swap is applied per
  // attribute at read time, because its correct width is the format's packed
  // unit and one vertex mixes 16-bit positions with 32-bit colours. Swapping
  // the whole buffer up front is what read every (x, y, z, w=1) position as
  // (y, x, w=1, z).
  uint32_t vertex_endian = 0;
  uint32_t prim_type = 0;            // xenos::PrimitiveType (raw 6-bit value)
  HostTopology topology = HostTopology::kUndefined;  // mapped from prim_type
  bool index_16bit = true;
  bool binned = false;                // true for DRAW_INDX_*_BIN variants
  float mvp[16] = {};
  bool valid = false;                 // set once index buffer is populated
  // HLE path only: the stream vertex index that `vertices[0]` was built from.
  // BuildHleDraw packs the *referenced* range, so element i is stream vertex
  // first_vertex + i, and a draw whose indices start at 4000 has vertices[0] =
  // stream vertex 4000. Anything wanting to re-read the guest bytes behind a
  // built vertex — the shader-execution probe does — needs this or it silently
  // reads a different vertex and compares two unrelated things.
  uint32_t first_vertex = 0;
  // Set by the transcode when skip_untransformable_draws is on and this draw's
  // positions came out degenerate, entirely out of clip, or with only some
  // vertices collapsed to the origin. Its own flag rather than reusing
  // topology == kUndefined (which would misreport the topology) or valid=false
  // (which the renderer skips without counting) — the reason and the count both
  // need to stay honest. A MITIGATION: the draw is still wrong, it is just no
  // longer drawn over the ones that are right.
  bool untransformable = false;
  // Which guest colour surface this draw targeted, from RB_COLOR_INFO /
  // RB_SURFACE_INFO at the time it was translated. A frame touches ~16 distinct
  // surfaces and the renderer has exactly one target, so without this every
  // off-screen pass overpaints the main scene. See LogSurface.
  uint32_t surface_base = 0;          // RB_COLOR_INFO[11:0], in 4KB tiles
  uint32_t surface_pitch = 0;         // RB_SURFACE_INFO[13:0]
  // HLE path: resolved D3D9 viewport extent at draw time. This separates
  // shadow/off-screen passes when render-target identity is not yet modelled.
  uint32_t viewport_width = 0;
  uint32_t viewport_height = 0;
  // HLE path: exact D3D9 render-target snapshot. `render_target_object` is the
  // stable API identity; the raw Xenos words are retained for matching a later
  // SetTexture binding to render-to-texture storage without guessing by size.
  uint32_t render_target_object = 0;
  uint32_t render_target_surface_info = 0;
  uint32_t render_target_color_info = 0;
  uint32_t render_target_width = 0;
  uint32_t render_target_height = 0;
  // HLE path: if the sampled D3D9 texture was most recently populated by
  // D3DDevice_Resolve, this is the source render-target object. It is separate
  // from the CPU texture payload because a resolve is an ordered GPU operation,
  // not immutable guest-memory content.
  uint32_t sampled_render_target_object = 0;
  // Extent of the D3D9 texture object selected by the pixel fetch. This is
  // needed even for resolved render-target samples, which deliberately have no
  // CPU texture payload, because an unnormalized tfetch still needs conversion
  // to the normalized coordinates consumed by the host sampler.
  uint32_t sampled_texture_width = 0;
  uint32_t sampled_texture_height = 0;
  std::shared_ptr<const HleTexturePayload> texture;

  // Where TranscodeVertices got this draw's vertex colour, so LogSurface can
  // cross-tabulate it against the surface. Carried on the draw rather than
  // counted in place because the two facts are established at different points:
  // the colour during transcode, the surface in FinalizeDraw afterwards.
  //
  // Worth the field because draw and vertex counts proved unable to answer the
  // question they were asked. The counters said 95.7% of vertices carry a real
  // packed colour; the tint screenshot said colourless draws cover 100% of the
  // pixels. Both were right. Which surface each population lives on is the next
  // thing that can separate them.
  enum class ColorSource : uint8_t {
    // The default, and deliberately NOT kNone: TranscodeVertices has several
    // early exits (no position attribute, an unconfirmed position format) that
    // return before any colour is resolved. Folding those into kNone would
    // inflate the population this round is trying to measure. These draws are
    // never rewritten, so they keep the guest stride and the renderer's
    // stride-28 gate drops them — they reach no pixels either way.
    kNotTranscoded = 0,
    kNone,          // no colour attribute found — written opaque white
    kPacked,        // format 6 or 7, a real packed colour
    kFallback,      // first 4-component non-position attribute, i.e. a guess
  };
  ColorSource color_source = ColorSource::kNotTranscoded;

  // The guest's output-merger state as of this draw, captured raw.
  //
  // Nothing in this renderer has ever read one of these. CreateGamePipeline
  // sets a write mask and a rasterizer state and stops: BlendEnable is FALSE,
  // DepthStencilState is zeroed, and D3D12 has no alpha test to leave off. So
  // every guest draw is composited fully opaque, in draw order, writing all
  // four channels — the entire back end of the guest pipeline is missing, and
  // it has stayed invisible because it still produces plausible geometry.
  //
  // Captured rather than acted on. A draw whose shader exports only a position
  // has no colour attribute, so the transcode writes {1,1,1,1} opaque white —
  // and a depth pre-pass is exactly that draw, covering the world while writing
  // no colour at all on the guest. If that is what the 12760 colourless draws
  // are, colour_mask reads zero for them and NoteOutputMerger will say so.
  //
  // Raw dwords, not decoded fields: the decode belongs next to the counters
  // that report it, and storing raw keeps a misread visible.
  uint32_t colour_mask = 0;    // RB_COLOR_MASK    0x2104, bits 0-3 = RGBA of RT0
  uint32_t depth_control = 0;  // RB_DEPTHCONTROL  0x2200
  uint32_t blend_control = 0;  // RB_BLENDCONTROL0 0x2201
  uint32_t colour_control = 0; // RB_COLORCONTROL  0x2202, bit 3 = alpha test
  uint32_t mode_control = 0;   // RB_MODECONTROL   0x2208, bits 0-2 = edram_mode

  // Which of the five above had actually been written this frame, at the time
  // this draw was finalized. Bit i corresponds to register i in the order
  // listed. WITHOUT THIS THE VALUES ABOVE ARE UNREADABLE.
  //
  // The translator is Clear()ed once per frame (hooks_frame.cpp:212) and the
  // context shadow is memset to zero, then the frame's packets are replayed. A
  // register the guest set once at init and never re-set therefore reads zero
  // in every frame we ever look at — and a colour mask of zero is exactly the
  // finding this round is hunting for. Reading it without knowing whether
  // anyone wrote it would let the instrument manufacture its own conclusion.
  uint32_t om_seen = 0;
};

//===========================================================================
// Which space an executed vertex shader's exported position lives in.
//
// The PM4 path settled this for the ring's own executions: every sampled export
// read like window coordinates — (640, 0, 1, w=1), (1280, 0, 0, w=1),
// (639.5, -0.5, 1, w=1) in a 1280x720 window — not clip space. That is why
// BuildViewportMvp exists and why the renderer applies the viewport inverse.
//
// A free function because two paths now ask the same question from different
// inputs: PM4 reads the Xenos context registers, the D3D9 path reads the
// D3D9 viewport. Only the *inputs* differ, so only the inputs should be
// duplicated. Two copies of the classification would be free to drift, and a
// drifted control is worse than no control.
//
// Three rules are baked in, and each was paid for:
//
//   1. **Degenerate first.** (0,0,0,w=0) sits inside the unit cube *and*
//      inside the viewport rectangle, so it would otherwise count as evidence
//      for whichever hypothesis is checked first.
//   2. **Clip wins the tie.** The two regions overlap near the origin. The tie
//      goes against the window hypothesis, not for it.
//   3. **The window extent comes from the viewport**, not a hardcoded
//      1280x720: xs is half the width, ys half the height (negative, y growing
//      downward). A half-pixel margin covers D3D9's pixel-centre convention.
//===========================================================================
enum class ExportSpace : uint8_t {
  kDegenerate = 0,  // no evidence either way
  kClipLike,
  kWindowLike,
  kNeither,
};

// x, y, w are the raw exported position; the perspective divide is applied
// here so callers cannot disagree about whether it was already done.
ExportSpace ClassifyExportSpace(float x, float y, float w, float xs, float xo,
                                float ys, float yo);
const char* ExportSpaceName(ExportSpace s);

//===========================================================================
// Topology mapping and the two primitive expansions.
//
// Free functions in the HLE header rather than statics on the retired PM4
// translator, which is where they used to live. Nothing about them was ever
// PM4-specific: a D3DPRIMITIVETYPE on Xenon *is* the Xenos value, so the D3D9
// path needs this exact mapping, not a parallel one that could drift.
//===========================================================================

// Map the raw 6-bit prim_type to a host topology. kUndefined means the
// renderer must drop the draw — RectangleList maps to kUndefined here because
// it is not a topology but an expansion, handled by ExpandRectangleList.
//
// Not an assumption — D3DDevice_DrawVertices writes its argument straight into
// the draw initiator's low 6 bits, and indexes its own per-primitive table at
// 0x82002B90 with it.
HostTopology MapTopology(uint32_t prim_type);

// Rewrite a RectangleList draw into a triangle list in place. D3D12 has no
// rectangle topology. Each group of 3 vertices is a rectangle whose implied
// 4th corner is v3 = v0 + v2 - v1; the synthesized vertex takes that
// arithmetic on the leading 3 floats and copies its remaining bytes from v2.
// Returns the number of rectangles expanded, 0 if the draw could not be.
uint32_t ExpandRectangleList(DrawCall& dc);

// Rewrite a QuadList draw into a triangle list in place. D3D12 has no quad
// topology either, but a quad needs no synthesized corner: all four are
// present, so the vertices pass through untouched and only the index buffer is
// rebuilt, six indices per quad on the v0-v2 diagonal. Maps through the
// incoming indices, so it is correct for auto-draws and real index buffers
// alike. Returns the number of quads expanded, 0 if the draw could not be.
uint32_t ExpandQuadList(DrawCall& dc);

}  // namespace mx::hle
