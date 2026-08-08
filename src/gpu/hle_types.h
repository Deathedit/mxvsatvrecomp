#pragma once

#include "gpu/shader_alu.h"
#include "gpu/shader_ucode.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>
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
  // Bumped when the same guest texture is decoded again because its CONTENTS
  // changed underneath a stable fetch constant. The cache key hashes the six
  // fetch dwords -- address, size, format -- so a texture the guest rewrites in
  // place keeps its key, and the renderer, which also caches on that key, would
  // keep serving the bytes it uploaded the first time. Scaleform's raster glyph
  // cache does exactly that: it repacks glyphs into one atlas as strings come
  // and go, which froze the menu text as whatever the atlas held when it was
  // first sampled. Zero means never rewritten.
  uint32_t content_version = 0;
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
  // The D3D9 texture object the pixel fetch selected, when that texture was
  // populated by a resolve.
  //
  // The source object above cannot identify *which* resolve result to bind: one
  // guest surface is a shared scratch buffer that the scene and every video
  // render into in turn, and six distinct textures were measured resolving out
  // of a single target in one run. Keying host storage by the source target
  // makes all six alias one resource, so every draw sampling any of them sees
  // whatever was drawn most recently. This field is the one that tells them
  // apart.
  uint32_t sampled_texture_object = 0;
  // A resolve event rather than a draw. Carries no geometry: `dest_texture` is
  // the D3D9 texture the guest resolved into and `source_object` the render
  // target it copied out of.
  //
  // It rides in the same ordered queue as draws instead of a parallel list
  // because a resolve is only meaningful in sequence — it snapshots the target
  // as of that moment, and every draw recorded after it must see the snapshot,
  // not the surface's later contents. Sharing the queue keeps that ordering for
  // free.
  uint32_t resolve_dest_texture = 0;
  uint32_t resolve_source_object = 0;
  // Where in the destination texture this resolve lands, and which part of the
  // source it takes — D3DDevice_Resolve's pDestPoint (r7) and pSourceRect (r5).
  //
  // Not optional detail: the scene is resolved as two EDRAM *bands*, not once.
  // A 1280x720 colour+depth surface does not fit in 10MB of EDRAM, so the guest
  // splits it — measured as render targets 0x2123CA94 (1280x640) and 0x2123CAC4
  // (1280x80) sharing one surface descriptor 0x14000500, base 0x2D0, pitch
  // 1280, both resolving into the single 1280x720 texture 0x2123CA60, 1432
  // times in one run. Discarding these two arguments made every band look like
  // a whole surface, so the second one replaced the first and the scene
  // snapshot became an 80-line strip stretched over the screen.
  //
  // `resolve_src_x2 == 0` means no rectangle was supplied: take the whole
  // source.
  int32_t resolve_dest_x = 0;
  int32_t resolve_dest_y = 0;
  int32_t resolve_src_x1 = 0;
  int32_t resolve_src_y1 = 0;
  int32_t resolve_src_x2 = 0;
  int32_t resolve_src_y2 = 0;
  // Extent of the D3D9 texture object selected by the pixel fetch. This is
  // needed even for resolved render-target samples, which deliberately have no
  // CPU texture payload, because an unnormalized tfetch still needs conversion
  // to the normalized coordinates consumed by the host sampler.
  // The guest's texture address mode for the sampled texture, straight off the
  // fetch constant. Carried for BOTH resource paths — a resolved render-target
  // sample has no CPU payload to hang it on, and those are exactly the
  // fullscreen passes where wrapping shows up as a seam at the surface edge.
  // Xenos numbering: 0 repeat, 1 mirrored repeat, everything above clamps.
  uint8_t clamp_x = 0;
  uint8_t clamp_y = 0;
  uint32_t sampled_texture_width = 0;
  uint32_t sampled_texture_height = 0;
  std::shared_ptr<const HleTexturePayload> texture;

  // The guest pixel shader, translated to HLSL, when EmitShaderHlsl accepted
  // it. Null means it did not, and the draw keeps the `tex * col` stand-in.
  //
  // Carried as a shared_ptr to one cached string rather than by value: the
  // source is emitted once per shader handle and a frame issues ~158 draws
  // across a few dozen shaders, so copying it per draw would dominate the
  // translation it exists to enable. The renderer compiles it once per handle
  // and keys its PSO cache on `pixel_shader_handle`, so after the first sight
  // the string is never read again — it is kept only so a shader first seen in
  // a later frame can still be compiled.
  uint32_t pixel_shader_handle = 0;
  std::shared_ptr<const std::string> pixel_shader_hlsl;
  // How many distinct samplers the translated shader reads, and the guest
  // sampler each compact slot was assigned. See HlslShader::sampler_slot_guest.
  uint32_t pixel_sampler_count = 0;

  // One texture per compact sampler slot, in the order the emitted shader
  // declares them: slot i is the texture the shader reads as xe_texi.
  //
  // This is what lets a shader with more than one texture run at all. Binding
  // only one meant every multi-sampler shader — which is most of them — fell
  // back to the stand-in, and the stand-in picks a single arbitrary fetch. A
  // rider shader sampling diffuse + normal + detail would render whichever one
  // that happened to be, which is why the jersey looked like a dark misaligned
  // texture rather than yellow: it was the normal map.
  //
  // A slot may instead name a resolved render target, for the post-process
  // chain, which samples resolve results rather than guest memory. The two are
  // parallel arrays because a slot has exactly one or the other.
  // One per sampler slot a translated pixel shader can declare. Tied to
  // HlslShader::kMaxSamplerSlots and the renderer's kTranslatedSamplerSlots.
  static constexpr uint32_t kMaxPixelTextures = 16;
  std::array<std::shared_ptr<const HleTexturePayload>, kMaxPixelTextures>
      pixel_textures;
  std::array<uint32_t, kMaxPixelTextures> pixel_sampled_objects = {};

  // The interpolators the translated pixel shader reads, one float4 per
  // linkage slot per vertex, in a buffer parallel to `vertices`.
  //
  // A second vertex stream rather than a wider vertex: `vertices` is built by
  // the transcode at a fixed stride that the stand-in pipeline depends on, and
  // widening it would change the layout of the path that currently renders the
  // game in order to serve the path that does not yet. Two streams cost one
  // extra buffer and leave the working layout untouched.
  //
  // The data was already being computed and thrown away. ExecuteVertexShader
  // returns all 16 exports for every vertex; only the one the texture profile
  // selected was ever read.
  std::vector<uint8_t> interpolators;

  // The guest VERTEX shader, translated to HLSL, and the raw attribute stream
  // the emitted `XeVsIn` consumes. Null/empty means this draw keeps the CPU
  // interpreter, which is still the path for every draw whose vertex or pixel
  // shader did not translate.
  //
  // Only populated when BOTH stages translate. A GPU vertex stage feeding the
  // stand-in pixel shader would have to also reproduce everything the CPU path
  // derives on the side — the param_gen UV reconstruction above all — so the
  // migration is per draw and takes both stages together or neither.
  uint32_t vertex_shader_handle = 0;
  std::shared_ptr<const std::string> vertex_shader_hlsl;

  // The registers the translated vertex shader reads, ascending. This is the
  // same order `EmitShaderHlsl` walks `input_mask` to declare
  // `float4 vN : TEXCOORDN`, so input element i carries register
  // `vertex_input_regs[i]` at semantic index that register number.
  //
  // Keyed by REGISTER, not by attribute, and that distinction is measured: 816
  // of 15,000 draws (5.4%) have two vfetch attributes writing one register with
  // complementary destination swizzles — a mini-fetch completing a full one.
  // One element per attribute would declare the same register twice and the
  // second would clobber the first, which is the exact bug shader_alu.cpp
  // documents at its own seeding loop.
  static constexpr uint32_t kMaxVertexInputs = 32;
  std::array<uint8_t, kMaxVertexInputs> vertex_input_regs = {};
  uint32_t vertex_input_count = 0;

  // One float4 per declared register per vertex, tightly packed in
  // `vertex_input_regs` order: stride is vertex_input_count * 16.
  std::vector<uint8_t> vertex_inputs;

  // The guest's VERTEX ALU constant bank (constants 0-255 at device+0x780), in
  // the same shape as `pixel_constants`: constant i is vertex_constants[i * 4].
  std::vector<uint32_t> vertex_constants;

  // The guest's PIXEL ALU constant bank, as raw dwords, indexed the way the
  // shader indexes it: constant i is pixel_constants[i * 4].
  //
  // This is ALU constants 256-511, read from device+0x1780 — a different bank
  // from the vertex one at device+0x780, and one nothing has ever read.
  // D3DDevice_DrawVertices flushes them separately, passing Xenos register base
  // 0x4000 for the vertex bank and 0x4400 for this one (0x4400 = 0x4000 + 1024
  // dwords = constant 256). The rebase happens here, at capture, so the emitted
  // shader can index its own bank from 0.
  std::vector<uint32_t> pixel_constants;

  // The Bink frame composite binds several textures at once — Y, Cr and Cb
  // planes plus an optional alpha plane — which the single `texture` above
  // cannot express. Measured shape, 3/3 runs: Y at full resolution, chroma at
  // half, every plane k_8.
  //
  // These deliberately bypass the g_hleCpuTextures cache: the planes are new
  // guest memory every video frame, so caching them by payload key would grow
  // the cache and the renderer's descriptor heap without bound. They are
  // decoded fresh per frame and uploaded into reusable host textures.
  static constexpr uint32_t kMaxPlanes = 4;
  std::array<std::shared_ptr<const HleTexturePayload>, kMaxPlanes> planes;
  uint32_t plane_count = 0;
  // Set only when the bound pixel shader is one of the guest's own two Bink
  // composite shaders, read from its globals — an exact identity, not a guess.
  bool yuv_composite = false;
  // True when the guest bound a fourth plane, which selects its alpha-capable
  // pixel shader.
  bool yuv_has_alpha = false;

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
    kNone,          // no colour attribute — seeded {1,1,1,1}, a modulation
                    // identity for the textured shader, not a colour
    kPacked,        // format 6 or 7, a real packed colour
    kFallback,      // first 4-component non-position attribute, i.e. a guess
  };
  ColorSource color_source = ColorSource::kNotTranscoded;

  // The guest's output-merger state as of this draw, captured raw.
  //
  // These ARE read now, and the note claiming otherwise was stale: since
  // 1007a99 CreateGamePipeline sets BlendEnable TRUE with the guest's own
  // factors, and depth enable/write come from depth_control. What is still
  // missing is the alpha test, which D3D12 has no fixed-function equivalent for.
  //
  // The depth-pre-pass theory this field was added to test is settled and was
  // wrong: only 175 colourless draws have colour_mask 0, against 4024 that want
  // colour. They are fullscreen compositor passes, not pre-passes — 4.3 verts,
  // raw extent 2.01 ndc, 91.5% sampling a render target (51f3c80).
  //
  // Raw dwords, not decoded fields: the decode belongs next to the counters
  // that report it, and storing raw keeps a misread visible.
  uint32_t colour_mask = 0;    // RB_COLOR_MASK    0x2104, bits 0-3 = RGBA of RT0
  uint32_t depth_control = 0;  // RB_DEPTHCONTROL  0x2200

  // Alpha blending, as the guest's own D3DRS_* values. Raw for the same reason
  // as the two above: the translation to host enums belongs in the renderer,
  // and keeping the guest's numbers here means a wrong mapping shows up as a
  // wrong number rather than as a plausible blend nobody asked for.
  //
  // Everything was drawn opaque before this, so anything the guest expected to
  // blend covered what was underneath it — the front end's fullscreen overlays
  // painted flat over the whole scene.
  uint32_t blend_enable = 0;   // D3DRS_ALPHABLENDENABLE
  uint32_t src_blend = 0;      // D3DRS_SRCBLEND,  a D3DBLEND
  uint32_t dest_blend = 0;     // D3DRS_DESTBLEND, a D3DBLEND
  uint32_t blend_op = 0;       // D3DRS_BLENDOP,   a D3DBLENDOP
  uint32_t blend_control = 0;  // RB_BLENDCONTROL0 0x2201
  // RB_COLORCONTROL 0x2202: bits 0-2 the alpha comparison, bit 3 its enable.
  // With the reference value below, this is the alpha test — the one piece of
  // output-merger state D3D12 has no fixed-function equivalent for, and the
  // classic reason glyph quads render as filled blocks.
  //
  // Both are read from the device's register shadow rather than the render
  // state shadow, which covers only eight output-merger leaves and has no alpha
  // entries. Their locations come from D3DDevice_DrawVertices' own flush, which
  // names both block base and register base:
  //
  //     addi r6, r31, 0x2934 ; li r5, 0x2200   m_ControlPacket
  //     addi r6, r31, 0x28CC ; li r5, 0x2100   m_ValuesPacket
  //
  // so 0x2202 sits at device+0x2934 + (0x2202-0x2200)*4 = device+0x293C, and
  // 0x210E at device+0x28CC + (0x210E-0x2100)*4 = device+0x2904. Both offsets
  // fall inside the packet sizes the IDB's D3DDevice type gives (48 and 84
  // bytes), which is the cross-check that they are the right blocks.
  uint32_t colour_control = 0;
  float alpha_ref = 0.0f;      // RB_ALPHA_REF 0x210E
  bool alpha_state_seen = false;
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
