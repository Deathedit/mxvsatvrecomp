#pragma once

#include "gpu/shader_alu.h"
#include "gpu/shader_ucode.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
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
  kBgra4,
  kR8,
  kR16,
  kR32Float,
  // Appended rather than filed next to kR8, deliberately: these values are
  // printed raw by the texture-fallback log line, so renumbering the existing
  // ones would silently reinterpret every log already written.
  kRg8,
  // Appended, never inserted: these values cross the hooks/renderer boundary by
  // value and are printed raw by the texture-fallback log.
  //
  // k_16_16 and k_16_16_16_16 get TWO entries each because the guest's
  // TEX_FORMAT_COMP decides how identical bytes are read. The reference uses one
  // typeless resource with a UNORM and an SNORM view; we create the resource
  // with a concrete format, so the choice moves here. Safe per-texture only
  // because HleTextureKey hashes the sign fields too.
  kRg16Float,
  kRg16Unorm,
  kRg16Snorm,
  kRgba16Unorm,
  kRgba16Snorm,
  kRg32Float,
  // k_2_10_10_10 (and k_2_10_10_10_AS_16_16_16_16, which GetBaseFormat folds
  // onto it). Three 10-bit channels and a 2-bit alpha in one dword — the same
  // 32bpp passthrough as kRgba8, so nothing in the decode path needs to know
  // about it beyond the DXGI format.
  kRgb10A2Unorm,
  // k_16 signed. This is the TERRAIN HEIGHTMAP: read as UNORM its small negative
  // heights come back near 1.0.
  kR16Snorm,
};

// xenos::TextureFilter::kBaseMap -- "sample level 0 and never minify past it".
// Named here so the renderer can test for it without comparing against a bare 2
// and without including the SDK's xenos.h.
constexpr uint8_t kMipFilterBaseMap = 2;
// xenos::TextureFilter::kPoint -- pick the nearest mip rather than blending the
// two. Zero, which is also what a payload that never set the field carries, so
// test it alongside level_count > 1 rather than on its own.
constexpr uint8_t kMipFilterPoint = 0;

// One decoded mip level inside HleTexturePayload::data. The geometry is stated
// rather than derived arithmetically from data.size(), which is only correct
// while the buffer holds one level and would silently corrupt every array
// texture once a mip chain is appended.
struct HleTextureLevelData {
  uint32_t offset = 0;     // byte offset of slice 0 of this level in `data`
  uint32_t row_pitch = 0;  // tightly packed: width_in_blocks * bytes_per_block
  uint32_t rows = 0;       // block rows per slice
  uint32_t width = 0;      // texels
  uint32_t height = 0;
};

// Immutable CPU-side texture shared by all draws that reference it. The hook
// owns guest-memory access; the renderer only sees validated host bytes.
struct HleTexturePayload {
  uint64_t key = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t row_pitch = 0;
  // Slices in `data`, tightly packed one after another, each row_pitch *
  // height_in_blocks bytes. 1 for an ordinary texture, 6 for a cube. Anything
  // above 1 is uploaded as that many D3D12 subresources and bound as a
  // Texture2DArray.
  uint32_t array_size = 1;
  HostTextureFormat format = HostTextureFormat::kRgba8;
  uint32_t swizzle = 0;
  uint8_t clamp_x = 0;
  uint8_t clamp_y = 0;
  bool linear_filter = true;
  // The guest's per-texture LOD bias, already scaled into LOD units. Applied in
  // the SHADER via xe_texinv[slot].w rather than on the sampler, as the
  // reference does (xenia d3d12_texture_cache.cc:1043). Not cosmetic: the
  // terrain's virtual-texture page tables carry +7.0, levels 5-9 are fully
  // populated and 0-4 sparse, so without the bias the lookup misses and reads
  // the not-available sentinel -- one atlas tile repeated 1024x.
  float lod_bias = 0.0f;
  // Bumped when the same guest texture is decoded again because its CONTENTS
  // changed under a stable fetch constant. The key hashes only the six fetch
  // dwords, so a texture the guest rewrites in place keeps its key -- which is
  // exactly what Scaleform's glyph atlas does. Zero means never rewritten.
  uint32_t content_version = 0;
  // What the RENDERER compares to decide its uploaded copy is stale, and
  // deliberately NOT content_version.
  //
  // content_version has to stay a 2 KB fingerprint SAMPLE, because the cache-hit
  // path compares a freshly sampled fingerprint against it. The consequence is
  // that a SPARSE guest write leaves it identical, so re-decodes forced by the
  // flat-retry backoff were computed, cached, and then discarded at the GPU
  // boundary. This is a hash of the DECODED BYTES, so it changes exactly when
  // the uploaded copy would differ. Zero means "not computed".
  uint32_t upload_version = 0;
  // The guest's mip chain, decoded from its own allocation. 1 means base level
  // only. `data` is LEVEL-MAJOR with the slices inside each level, matching the
  // guest; D3D12 nests the other way round (mip + slice * MipLevels), so the
  // upload walks this table rather than striding through the buffer.
  uint32_t level_count = 1;
  // TextureFilter, from the fetch constant. Carried for the sampler; kBaseMap
  // never reaches here with level_count > 1, because a chain the guest does not
  // want is not decoded in the first place.
  uint8_t mip_filter = 0;
  // DIAGNOSTIC ONLY. The level-0, slice-0 centre block as it sat in guest memory
  // and as it ended up after SwapBlock, plus the endian applied. Carried on the
  // payload because the decoder has no logger and because the two halves only
  // mean anything if they come from the SAME decode.
  uint8_t probe_raw[16] = {};
  uint8_t probe_swapped[16] = {};
  uint32_t probe_bytes = 0;
  uint32_t probe_endian = 0;
  HleTextureLevelData levels[14] = {};
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

// Whether a primitive has front and back faces, and so is subject to face
// culling at all. Points, lines and RECTANGLE LISTS are always "front".
//
// kRectangleList is deliberately NOT in this set, matching the reference
// (draw_util.h:71), which records that culling rectangles breaks a title. It
// broke one here too: the guest submits its post-process passes as rectangle
// lists, we expand each into two triangles with a winding we choose ourselves,
// and honouring cull_back against that synthesised winding drops the whole quad
// -- including the luminance downsample, whose zeroed target makes the guest's
// exposure a divide by zero.
constexpr bool IsPrimitivePolygonal(uint32_t prim_type) {
  switch (static_cast<PrimitiveType>(prim_type)) {
    case PrimitiveType::kTriangleList:
    case PrimitiveType::kTriangleFan:
    case PrimitiveType::kTriangleStrip:
    case PrimitiveType::kQuadList:
      return true;
    default:
      return false;
  }
}

// Host topology, carried on DrawCall so the renderer stays a dumb consumer. The
// values are deliberately the D3D_PRIMITIVE_TOPOLOGY ones so the renderer can
// cast rather than translate; d3d12_game.cpp static_asserts that they still
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
  // RULE: `vertices` stays in GUEST byte order and the fetch constant's endian
  // swap is applied PER ATTRIBUTE at read time, never to the whole buffer. The
  // correct swap width is the format's packed unit, and one vertex mixes 16-bit
  // positions with 32-bit colours; swapping the buffer up front is what read
  // every (x, y, z, w=1) position as (y, x, w=1, z).
  uint32_t prim_type = 0;            // xenos::PrimitiveType (raw 6-bit value)
  HostTopology topology = HostTopology::kUndefined;  // mapped from prim_type
  bool index_16bit = true;
  bool binned = false;                // true for DRAW_INDX_*_BIN variants
  float mvp[16] = {};
  bool valid = false;                 // set once index buffer is populated
  // HLE path only: the stream vertex index that `vertices[0]` was built from.
  // BuildHleDraw packs the *referenced* range, so element i is stream vertex
  // first_vertex + i. Anything re-reading the guest bytes behind a built vertex
  // needs this or it silently compares two unrelated vertices.
  uint32_t first_vertex = 0;
  // Set by the transcode when skip_untransformable_draws is on and this draw's
  // positions came out degenerate or out of clip. Its own flag rather than
  // reusing topology == kUndefined (which would misreport the topology) or
  // valid=false (which the renderer skips without counting). A MITIGATION: the
  // draw is still wrong, it is just no longer drawn over the ones that are right.
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
  // The viewport the GUEST programmed, decoded from PA_CL_VPORT_XSCALE/XOFFSET/
  // YSCALE/YOFFSET (0x210F..0x2112, shadow base device+0x28CC). Distinct from
  // viewport_width/height above, which is the D3D9 struct: this is what the
  // hardware actually transforms into, and the renderer hands D3D12 the render
  // TARGET's extent instead of consulting it. Zero means unreadable.
  uint32_t guest_vp_width = 0;
  uint32_t guest_vp_height = 0;
  // HLE path: exact D3D9 render-target snapshot. `render_target_object` is the
  // stable API identity; the raw Xenos words are retained so a later SetTexture
  // bind can be matched to render-to-texture storage without guessing by size.
  //
  // MRT SLOT 1, and only slot 1. The guest's terrain tile pass binds two 256x256
  // targets and resolves the SECOND into the texture its tile shader samples;
  // rendering only slot 0 left that resolve without a source. Slots 2 and 3
  // never appear in this title.
  uint32_t render_target1_object = 0;
  uint32_t render_target1_color_info = 0;
  uint32_t render_target1_width = 0;
  uint32_t render_target1_height = 0;
  uint32_t render_target_object = 0;
  uint32_t render_target_surface_info = 0;
  uint32_t render_target_color_info = 0;
  uint32_t render_target_width = 0;
  uint32_t render_target_height = 0;
  // A full-surface D3DDevice_Clear event rather than a geometry draw. Clears
  // share the ordered draw/resolve stream because a scratch target may be
  // cleared and resolved without any draw between the two.
  bool clear_color_target = false;
  uint32_t clear_color = 0;  // D3DCOLOR A8R8G8B8.
  // A full-surface D3D9 DEPTH clear, carried as an ordered event for the same
  // reason the colour one is: it has to land between the draws it separates.
  //
  // The renderer used to clear depth exactly once per frame, on the depth
  // target's first use, and the Clear hook tested only bit 0 (COLOUR) so the
  // guest's own clears were dropped. Six passes then shared one target with no
  // clear between them and the ground draw was discarded depthTestFailed against
  // depth it never wrote.
  bool clear_depth_target = false;
  float clear_depth = 1.0f;
  // Stencil is INDEPENDENT of depth, and that independence is the point. From
  // the guest's own clear emitter (sub_8255A510): Flags & 0x10 sets the depth
  // bit, Flags & 0x20 sets the stencil bit AND writes RB_STENCILREFMASK. All
  // three combinations are issued -- 0x1F colour+depth with no stencil, 0x20
  // stencil alone, 0x30 both.
  bool clear_stencil_target = false;
  uint8_t clear_stencil = 0;
  // Resolve flag 0x100 clears the source after copying and takes a float4
  // colour, unlike D3DDevice_Clear's packed D3DCOLOR. Keeping the two forms
  // distinct avoids quantizing HDR clear values.
  bool clear_color_is_float = false;
  std::array<float, 4> clear_color_float = {};
  // A SURFACE BIND event rather than a draw, a clear or a resolve. The guest
  // named this surface as a render target or depth-stencil attachment; nothing
  // has necessarily been drawn into it yet.
  //
  // Host storage used to be created on the first DRAW that named a surface,
  // which silently loses every pass that binds and resolves without a draw we
  // route -- the menu's shadow atlas is bound depth-only with no colour target
  // and resolved every frame, but instantiated by nothing. Identify such
  // surfaces by extent, EDRAM base and pitch: guest surface objects are pointers
  // and differ every run.
  //
  // Rides the ordered queue for the same reason a resolve does: a bind that
  // arrives after a resolve must not create the surface the resolve wanted.
  bool surface_bind = false;
  bool surface_bind_is_depth = false;
  uint32_t surface_bind_object = 0;
  uint32_t surface_bind_width = 0;
  uint32_t surface_bind_height = 0;
  uint32_t surface_bind_base = 0;
  // Colour format nibble, as render_target_color_info >> 16. Unused for depth.
  uint32_t surface_bind_color_format = 0;
  // The bound DEPTH-STENCIL surface, carried for the same reason as the colour
  // one: it is a guest surface with its own object identity, and Resolve names
  // it by that identity (source slot 4). Offscreen colour targets used to be
  // rendered with no depth attachment at all, so a depth resolve had nothing to
  // copy -- 224 misses on one surface in a single menu run.
  uint32_t depth_target_object = 0;
  uint32_t depth_target_width = 0;
  uint32_t depth_target_height = 0;
  // EDRAM tile base of that depth surface. See resolve_source_base below.
  uint32_t depth_target_base = 0;
  // HLE path: if the sampled D3D9 texture was most recently populated by
  // D3DDevice_Resolve, this is the source render-target object. It is separate
  // from the CPU texture payload because a resolve is an ordered GPU operation,
  // not immutable guest-memory content.
  uint32_t sampled_render_target_object = 0;
  // The D3D9 texture object the pixel fetch selected, when that texture was
  // populated by a resolve. The source object alone cannot identify WHICH
  // resolve result to bind: one guest surface is shared scratch that the scene
  // and every video render into in turn, so keying host storage by the source
  // target makes them all alias one resource.
  uint32_t sampled_texture_object = 0;
  // A resolve event rather than a draw. Carries no geometry: `dest_texture` is
  // the D3D9 texture the guest resolved into and `source_object` the render
  // target it copied out of. It rides the same ordered queue as draws because a
  // resolve is only meaningful in sequence -- it snapshots the target as of that
  // moment, and every draw recorded after it must see the snapshot.
  uint32_t resolve_dest_texture = 0;
  uint32_t resolve_source_object = 0;
  // The source was the DEPTH-STENCIL surface (Resolve source slot 4), not a
  // colour target. Carried because one guest object can be bound as both --
  // 0x218C1FE0 is a 640x360 depth surface in one binding and a colour target
  // in another -- so the object alone cannot say which pool to look in.
  bool resolve_source_is_depth = false;
  // The EDRAM tile base of the resolve source surface. A banded depth pass
  // renders into two surfaces and then resolves the whole thing through a third
  // object that aliases band 0's base -- an object no draw ever binds. Matching
  // the pool by object identity misses every one of those.
  uint32_t resolve_source_base = 0;
  // The source surface's OWN extent. Matching an aliased source on the
  // DESTINATION texture's extent fails whenever that extent could not be
  // decoded, and the source is the thing being identified anyway.
  uint32_t resolve_source_width = 0;
  uint32_t resolve_source_height = 0;
  // Where in the destination texture this resolve lands, and which part of the
  // source it takes -- D3DDevice_Resolve's pDestPoint (r7) and pSourceRect (r5).
  //
  // Not optional detail: a 1280x720 colour+depth surface does not fit in 10MB of
  // EDRAM, so the guest splits the scene into two bands that resolve into one
  // texture. Discarding these two arguments made every band look like a whole
  // surface, so the second replaced the first and the scene snapshot became an
  // 80-line strip stretched over the screen.
  //
  // `resolve_src_x2 == 0` means no rectangle was supplied: take the whole source.
  int32_t resolve_dest_x = 0;
  int32_t resolve_dest_y = 0;
  int32_t resolve_src_x1 = 0;
  int32_t resolve_src_y1 = 0;
  int32_t resolve_src_x2 = 0;
  int32_t resolve_src_y2 = 0;
  // The DESTINATION TEXTURE's own declared extent, off its fetch constant -- not
  // the extent of the region this particular resolve covers.
  //
  // The host snapshot must be this size. Sizing it to the covered region is
  // right for a banded resolve and wrong for an ATLAS: a 2048x2048 atlas filled
  // by repeated 256x256 sub-rect resolves would be created at 256x256, and the
  // shader's normalized UVs map [0,1] across that.
  uint32_t resolve_dest_width = 0;
  uint32_t resolve_dest_height = 0;
  // The guest's texture address mode for the sampled texture, straight off the
  // fetch constant. Carried for BOTH resource paths -- a resolved render-target
  // sample has no CPU payload to hang it on, and those are exactly the
  // fullscreen passes where wrapping shows up as a seam at the surface edge.
  // Xenos numbering: 0 repeat, 1 mirrored repeat, everything above clamps.
  //
  // sampled_texture_* below is that texture's extent, needed even for resolved
  // samples because an unnormalized tfetch still has to be converted to the
  // normalized coordinates the host sampler consumes.
  uint8_t clamp_x = 0;
  uint8_t clamp_y = 0;
  uint32_t sampled_texture_width = 0;
  uint32_t sampled_texture_height = 0;
  std::shared_ptr<const HleTexturePayload> texture;

  // The guest pixel shader, translated to HLSL, when EmitShaderHlsl accepted it.
  // Null means it did not, and the draw keeps the `tex * col` stand-in.
  //
  // A shared_ptr to one cached string rather than by value: the source is
  // emitted once per shader handle but a frame issues ~158 draws. The renderer
  // compiles it once per handle and keys its PSO cache on `pixel_shader_handle`.
  uint32_t pixel_shader_handle = 0;
  std::shared_ptr<const std::string> pixel_shader_hlsl;
  // The compiled DXBC of pixel_shader_hlsl, when the persisted content-keyed
  // cache held it (or the first compile wrote it). Carried so the renderer can
  // build the PSO without an FXC compile of its own — FXC is the expensive part,
  // 18-145ms per shader at O0. Null means "compile from hlsl".
  std::shared_ptr<const std::vector<uint8_t>> pixel_shader_dxbc;
  // Zero when SQ_PROGRAM_CNTL.param_gen is disabled; otherwise one plus
  // SQ_CONTEXT_MISC.param_gen_pos. The bias leaves zero as the disabled value
  // while still allowing the hardware destination r0.
  uint32_t pixel_param_gen = 0;
  // How many distinct samplers the translated shader reads, and the guest
  // sampler each compact slot was assigned. See HlslShader::sampler_slot_guest.
  uint32_t pixel_sampler_count = 0;
  // Bit i set = slot i is declared Texture2DArray by the emitted shader and so
  // needs a TEXTURE2DARRAY SRV. See HlslShader::sampler_array_mask; the
  // descriptor's dimension must agree with the declaration.
  uint32_t pixel_sampler_array_mask = 0;

  // One texture per compact sampler slot, in the order the emitted shader
  // declares them: slot i is the texture the shader reads as xe_texi. This is
  // what lets a shader with more than one texture run at all -- binding only one
  // sent every multi-sampler shader to the stand-in, which picks a single
  // arbitrary fetch.
  //
  // A slot may instead name a resolved render target, for the post-process
  // chain. The two are parallel arrays because a slot has exactly one or the
  // other. Tied to HlslShader::kMaxSamplerSlots and the renderer's
  // kTranslatedSamplerSlots.
  static constexpr uint32_t kMaxPixelTextures = 16;
  std::array<std::shared_ptr<const HleTexturePayload>, kMaxPixelTextures>
      pixel_textures;
  std::array<uint32_t, kMaxPixelTextures> pixel_sampled_objects = {};
  // Per compact slot: the fetch constant's four TextureSign values, already
  // permuted into host component order (SwizzleTextureSigns), two bits per
  // component. 0 means plain unsigned. Per-BINDING state, not per-texture -- the
  // same guest memory is bound with different sign modes by different draws.
  std::array<uint8_t, kMaxPixelTextures> pixel_sampler_signs = {};
  // Per compact slot: the fetch constant's SWIZZLE, for slots served by a
  // resolve SNAPSHOT rather than a decoded texture (the decoded path carries its
  // swizzle on the payload). Without a carrier the renderer bound every snapshot
  // with an identity mapping: slots asking for 03012, the BGRA->RGBA correction,
  // sampled red and blue swapped. 0 means "not a snapshot slot".
  std::array<uint16_t, kMaxPixelTextures> pixel_sampled_swizzles = {};

  // The VERTEX stage's samplers, in the same shape as the pixel ones above and
  // resolved by the same code. A vertex shader that fetches a texture -- terrain
  // displacement is the case that made this necessary -- used to be refused the
  // GPU vertex path outright, and the CPU interpreter it fell back to has no
  // texture fetch at all.
  //
  // A SEPARATE descriptor range from the pixel ones (t17+/s16+, see
  // HlslShader::kVertexTextureBaseRegister): the two stages are translated and
  // cached independently, so their compact slot 0 means different guest samplers.
  //
  // FREEROAM content only. Every --force_load=NAT_Farm run measured has ZERO
  // vertex shaders with samplers.
  uint32_t vertex_sampler_count = 0;
  uint32_t vertex_sampler_array_mask = 0;
  std::array<std::shared_ptr<const HleTexturePayload>, kMaxPixelTextures>
      vertex_textures;
  std::array<uint32_t, kMaxPixelTextures> vertex_sampled_objects = {};
  std::array<uint8_t, kMaxPixelTextures> vertex_sampler_signs = {};
  // The vertex stage's half of pixel_sampled_swizzles above.
  std::array<uint16_t, kMaxPixelTextures> vertex_sampled_swizzles = {};
  // The interpolators the translated pixel shader reads, one float4 per linkage
  // slot per vertex, in a buffer parallel to `vertices`. A second vertex stream
  // rather than a wider vertex: `vertices` is built at a fixed stride the
  // stand-in pipeline depends on, and widening it would change the layout of the
  // path that currently renders the game.
  std::vector<uint8_t> interpolators;

  // The guest VERTEX shader, translated to HLSL, and the raw attribute stream
  // the emitted `XeVsIn` consumes. Null/empty means this draw keeps the CPU
  // interpreter. Only populated when BOTH stages translate: a GPU vertex stage
  // feeding the stand-in pixel shader would also have to reproduce everything
  // the CPU path derives on the side, the param_gen UV reconstruction above all.
  uint32_t vertex_shader_handle = 0;
  std::shared_ptr<const std::string> vertex_shader_hlsl;
  // The compiled DXBC of vertex_shader_hlsl — same contract as
  // pixel_shader_dxbc, and the fetch variant of the shader carries the fetch
  // variant's bytecode here.
  std::shared_ptr<const std::vector<uint8_t>> vertex_shader_dxbc;

  // The registers the translated vertex shader reads, ascending -- the same
  // order EmitShaderHlsl walks `input_mask` to declare `float4 vN : TEXCOORDN`,
  // so input element i carries register vertex_input_regs[i].
  //
  // Keyed by REGISTER, not by attribute: 5.4% of draws have two vfetch
  // attributes writing one register with complementary destination swizzles, and
  // one element per attribute would declare that register twice.
  static constexpr uint32_t kMaxVertexInputs = 32;
  std::array<uint8_t, kMaxVertexInputs> vertex_input_regs = {};
  uint32_t vertex_input_count = 0;

  // One float4 per declared register per vertex, tightly packed in
  // vertex_input_regs order: stride is vertex_input_count * 16. Empty when the
  // draw is on the GPU FETCH path below, which is the point of that path -- this
  // is the per-vertex CPU pass that cost 145ms of a 159ms frame.
  std::vector<uint8_t> vertex_inputs;

  // --- the GPU vertex fetch path -------------------------------------------
  //
  // The guest's own vertex streams, raw and still big-endian, concatenated into
  // one buffer for the translated vertex shader to decode for itself. Merged
  // rather than bound separately so a shader reading up to kMaxStreams of them
  // still needs one root SRV and no descriptor at all. A copy rather than a
  // pointer: the guest may overwrite its vertex buffer before the render thread
  // submits this draw.
  std::vector<uint8_t> raw_vertex_bytes;

  // Per emitted vfetch, in the shader's program order, matching
  // HlslShader::vertex_fetch_slot. Uploaded verbatim as the shader's xe_vf[].
  // `base` already includes first_vertex * stride and the stream's own byte
  // offset, so the shader indexes with SV_VertexID and needs no bias. `endian`
  // is applied per attribute, as on the CPU path.
  //
  // `limit` is the exclusive byte offset one past this stream's valid region,
  // and it is what makes reading the merged buffer safe: the buffer reaches the
  // GPU as a ROOT SRV, which carries a virtual address and no size. Per stream
  // rather than per buffer because the regions are packed with no gap. Beyond
  // `limit` the shader reads zero, which is what the hardware does.
  struct RawFetch {
    uint32_t base = 0;
    uint32_t stride = 0;
    uint32_t endian = 0;
    uint32_t limit = 0;
  };
  static constexpr uint32_t kMaxRawFetches = 32;
  std::array<RawFetch, kMaxRawFetches> raw_fetch = {};
  uint32_t raw_fetch_count = 0;

  // The guest's VERTEX ALU constant bank (constants 0-255 at device+0x780), in
  // the same shape as `pixel_constants`: constant i is vertex_constants[i * 4].
  std::vector<uint32_t> vertex_constants;

  // The guest's PIXEL ALU constant bank, as raw dwords, indexed the way the
  // shader indexes it: constant i is pixel_constants[i * 4].
  //
  // ALU constants 256-511, read from device+0x1780 -- a different bank from the
  // vertex one at device+0x780. D3DDevice_DrawVertices flushes them separately,
  // passing Xenos register base 0x4000 for the vertex bank and 0x4400 for this
  // one. The rebase happens here, at capture.
  std::vector<uint32_t> pixel_constants;

  // The Bink frame composite binds several textures at once -- Y at full
  // resolution, Cr and Cb at half, plus an optional alpha plane, every plane k_8
  // -- which the single `texture` above cannot express. These deliberately
  // bypass the g_hleCpuTextures cache: the planes are new guest memory every
  // video frame, so caching them by payload key would grow the cache and the
  // descriptor heap without bound.
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
  enum class ColorSource : uint8_t {
    // The default, and deliberately NOT kNone: TranscodeVertices has several
    // early exits (no position attribute, an unconfirmed position format) that
    // return before any colour is resolved, and folding those into kNone would
    // inflate the population.
    kNotTranscoded = 0,
    kNone,          // no colour attribute — seeded {1,1,1,1}, a modulation
                    // identity for the textured shader, not a colour
    kPacked,        // format 6 or 7, a real packed colour
    kFallback,      // first 4-component non-position attribute, i.e. a guess
  };
  ColorSource color_source = ColorSource::kNotTranscoded;

  // The guest's output-merger state as of this draw, captured raw. Raw dwords
  // rather than decoded fields: the decode belongs next to the counters that
  // report it, and storing raw keeps a misread visible.
  //
  // colour_mask bits 0-3 are per-channel RGBA, but every consumer collapses them
  // -- graphics_system passes `(colour_mask & 0xF) != 0` and the renderer sets
  // RenderTargetWriteMask to ALL or 0, so a guest mask of 0x1 makes us write
  // four channels. A 199,000-draw histogram measured that widening firing
  // exactly once: 0x0 66518 (depth-only passes), 0xF 132481, 0x7 one.
  uint32_t colour_mask = 0;    // RB_COLOR_MASK    0x2104, bits 0-3 = RGBA of RT0
  uint32_t depth_control = 0;  // RB_DEPTHCONTROL  0x2200
  // The other two registers the stencil state needs, captured beside
  // depth_control so all three describe the SAME draw. Reading them later, in
  // the renderer, would read whatever the device holds by then.
  //
  //   RB_STENCILREFMASK  ref bits 0-7, read mask 8-15, write mask 16-23
  //   RB_MODECONTROL     edram_mode is the low 3 bits
  //
  // edram_mode is carried rather than pre-judged because it gates the WHOLE
  // depth-stencil register: outside kColorDepth(4) and kDepthOnly(5) the
  // hardware ignores depth and stencil alike and the reference returns a zeroed
  // RB_DEPTHCONTROL (draw_util.cc:90). 0xFFFFFFFF means the register could not
  // be read, and cannot collide with a real value: edram_mode is 3 bits.
  uint32_t stencil_ref_mask = 0xFFFFFFFFu;  // RB_STENCILREFMASK 0x210D
  uint32_t edram_mode = 0xFFFFFFFFu;        // RB_MODECONTROL    0x2208, low 3

  // PA_SU_SC_MODE_CNTL 0x2205: cull_front +0, cull_back +1, face +2 (0 = front
  // is CCW, 1 = CW). Raw, like the three above; zero means unreadable, which
  // decodes as "cull nothing".
  //
  // Both PSO paths hardcoded D3D12_CULL_MODE_NONE before this existed. Nearly
  // invisible for opaque solids, whose back faces are hidden by the depth test,
  // and catastrophic for a closed volume that CONTAINS the camera: every visible
  // face is then a back face, so the console culls the whole primitive and we
  // rasterise its interior. That is the menu background.
  uint32_t pa_su_sc_mode_cntl = 0;  // PA_SU_SC_MODE_CNTL 0x2205

  // Alpha blending from RB_BLENDCONTROL0. Raw for the same reason as the two
  // above: keeping the guest's Xenos numbers means a wrong mapping shows up as a
  // wrong number rather than as a plausible blend nobody asked for. Everything
  // was drawn opaque before this.
  uint32_t blend_enable = 0;   // equation differs from ONE/ZERO/ADD
  uint32_t src_blend = 0;      // Xenos color source factor
  uint32_t dest_blend = 0;     // Xenos color destination factor
  uint32_t blend_op = 0;       // Xenos color combine function
  uint32_t blend_control = 0;  // RB_BLENDCONTROL0 0x2201
  // RB_COLORCONTROL 0x2202: bits 0-2 the alpha comparison, bit 3 its enable.
  // With the reference value below, this is the alpha test -- the one piece of
  // output-merger state D3D12 has no fixed-function equivalent for, and the
  // classic reason glyph quads render as filled blocks.
  //
  // Both are read from the device's REGISTER shadow rather than the render-state
  // shadow, which covers eight output-merger leaves and has no alpha entries.
  // D3DDevice_DrawVertices' own flush names both blocks -- m_ControlPacket at
  // device+0x2934 for register base 0x2200, m_ValuesPacket at device+0x28CC for
  // base 0x2100 -- so 0x2202 sits at device+0x293C and 0x210E at device+0x2904.
  uint32_t colour_control = 0;
  float alpha_ref = 0.0f;      // RB_ALPHA_REF 0x210E
  bool alpha_state_seen = false;
  // `mode_control` (RB_MODECONTROL 0x2208, bits 0-2 = edram_mode) sat here and
  // was never written or read — removed 2026-08-17. Nothing consults edram_mode
  // yet; add it back at the point something does, not before.

  // SCISSOR, in guest render-target pixels, already offset and normalised.
  //
  // Read from PA_SC_WINDOW_SCISSOR rather than from D3DDevice_SetScissorRect's
  // arguments, which is not a matter of taste: D3D9 has a separate
  // D3DRS_SCISSORTESTENABLE, so disabling the test makes the runtime write the
  // full surface into the register while the last rect passed to the setter goes
  // stale. The register is what the hardware rasterises against
  // (draw_util.cc:632), which is why there is no enable bit to look for.
  //
  // `seen` false means the register was unreadable, not that there is no
  // scissor: a scissor covering everything is spelled as the full rect.
  int32_t scissor_left = 0, scissor_top = 0;
  int32_t scissor_right = 0, scissor_bottom = 0;
  bool scissor_seen = false;

  // Which of the five above had actually been written this frame at the time
  // this draw was finalized; bit i is register i in the order listed. WITHOUT
  // THIS THE VALUES ABOVE ARE UNREADABLE: the translator is Clear()ed once per
  // frame and the context shadow zeroed, so a register the guest set once at
  // init and never re-set reads zero in every frame we look at.
  uint32_t om_seen = 0;
};

//===========================================================================
// FINDING: exported vertex positions are WINDOW coordinates, not clip space.
// Every sampled export read as (640, 0, 1, w=1), (1280, 0, 0, w=1) or
// (639.5, -0.5, 1, w=1) in a 1280x720 window. That is why BuildViewportMvp
// exists and why the renderer applies the viewport inverse.
//
// The classifier that established it was removed as dead code. Three rules it
// baked in, worth knowing if the question is reopened: test degenerate
// (0,0,0,w=0) FIRST, since it sits inside both regions; on a tie clip wins; and
// the window extent comes from the viewport, not a hardcoded 1280x720.
//===========================================================================

//===========================================================================
// Topology mapping and the two primitive expansions. Free functions rather than
// statics on the retired PM4 translator: nothing about them was PM4-specific,
// since a D3DPRIMITIVETYPE on Xenon *is* the Xenos value.
//===========================================================================

// Map the raw 6-bit prim_type to a host topology. kUndefined means the renderer
// must drop the draw; RectangleList maps to kUndefined because it is not a
// topology but an expansion, handled by ExpandRectangleList. Not an assumption
// -- D3DDevice_DrawVertices writes its argument straight into the draw
// initiator's low 6 bits.
HostTopology MapTopology(uint32_t prim_type);

// Rewrite a RectangleList draw into a triangle list in place. Each group of 3
// vertices gives three corners of a rectangle, and WHICH three is not fixed: the
// longest edge picks the arrangement, the triple is permuted so the diagonal
// runs from the second vertex to the third, and the fourth corner is
// v1 + v2 - v0 over the permuted triple, on every float of the vertex. Returns
// the number of rectangles expanded, 0 if the draw could not be.
uint32_t ExpandRectangleList(DrawCall& dc);

// How often each arrangement was chosen, indexed by the first vertex of the
// permuted triple. [0] is what the expansion used to assume unconditionally,
// so [1] + [2] counts the rectangles it built from the wrong corner.
extern std::atomic<uint64_t> g_rectArrangement[3];
extern std::atomic<uint64_t> g_rectDegenerate;

// Rewrite a QuadList draw into a triangle list in place. A quad needs no
// synthesized corner: all four are present, so the vertices pass through
// untouched and only the index buffer is rebuilt, six indices per quad on the
// v0-v2 diagonal. Maps through the incoming indices, so it is correct for
// auto-draws and real index buffers alike. Returns quads expanded, 0 if not.
uint32_t ExpandQuadList(DrawCall& dc);

// The 1x1 auto-exposure result, read back from the GPU and handed to the guest.
//
// The guest's adaptation pass (sub_82AFB8A8) does not sample this value, it
// LOADS it: `lwz r4, 0x20(r5); clrrwi r3, r4, 12; lhz r11, 0(r3)` reads the
// destination texture's own bytes out of guest memory as a 16-bit half. Our
// resolves only fill host textures, so the guest read zero and wrote +Infinity
// into pixel constant 100, which turned the composite's output to NaN.
//
// The renderer publishes and the D3D9 layer consumes, because only the renderer
// can read the GPU and only the hooks can address guest memory. `seq` is bumped
// last and read first.
//
// Keyed by DESTINATION TEXTURE OBJECT, not broadcast. The adaptation is
// `new = old + (cur - old) * k`, reading one buffer and writing the other, so
// one value for both makes cur == old and pins the filter at a fixed point.
struct LuminanceReadback {
  uint32_t destObject = 0;  // resolve destination texture object
  uint32_t bits = 0;        // host R16 half, little-endian
};
inline constexpr uint32_t kMaxLuminanceReadbacks = 4;
// Guarded by g_luminanceReadbackMutex; `seq` changes whenever the set does.
extern std::mutex g_luminanceReadbackMutex;
extern LuminanceReadback g_luminanceReadbacks[kMaxLuminanceReadbacks];
extern uint32_t g_luminanceReadbackCount;
extern std::atomic<uint32_t> g_luminanceReadbackSeq;

// SMALL RESOLVE DESTINATIONS THE GUEST READS FROM MEMORY rather than samples --
// see [[guest-reads-resolves-from-memory]]. The luminance 1x1 above is one; the
// terrain's 64x64 virtual-texture FEEDBACK BUFFER is the other, where the GPU
// writes page IDs and the CPU reads them to decide which tiles to stream.
//
// 64x64x4 is the cap: it covers the feedback buffer and excludes the only other
// never-sampled destination in the census, a 1280x720 scene target that reaches
// the screen by another path. Raising it to 128 KB to deliver the 129x129
// terrain height snapshot was tried and reverted -- nothing reads that snapshot,
// and delivering it took the feedback buffer down to 1 win in 2162.
inline constexpr uint32_t kMaxSurfaceReadbackBytes = 64 * 64 * 4;
struct SurfaceReadback {
  uint32_t destObject = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  // Bytes per row IN `bytes`, which is the readback buffer's own pitch and is
  // NOT the guest's -- D3D12 aligns footprint rows to 256.
  uint32_t rowPitch = 0;
  uint32_t bytesPerTexel = 0;
  uint32_t byteCount = 0;
  // Where in the destination this region belongs. The resolve's destpoint --
  // (0,0) for the VT feedback buffer, (768,224) and friends for the terrain
  // deformation, which writes 128x32 tiles into a 2048x2048 accumulation.
  uint32_t destX = 0;
  uint32_t destY = 0;
  // The source's DXGI format, for the conversion the guest's own resolve does.
  uint32_t srcFormat = 0;
  uint8_t bytes[kMaxSurfaceReadbackBytes] = {};
  // NON-ZERO AND MONOTONIC once this slot has ever been filled. A consumer
  // remembers the seq it last acted on PER SLOT, which is what lets several
  // destinations be delivered in the same frame: a single global flag let
  // whichever destination matched first mark the frame consumed.
  uint32_t seq = 0;
};
// MORE THAN ONE READBACK IN FLIGHT. One slot was right while the VT feedback
// buffer was the only destination; with the terrain deformation also landing,
// the renderer put 711 of 1,922 opportunities in lost-busy. Rotating one slot
// would share the starvation instead of ending it: the feedback buffer is
// per-frame state driving page streaming, while the deformation ACCUMULATES and
// tolerates gaps.
inline constexpr uint32_t kSurfaceReadbackSlots = 4;
extern std::mutex g_surfaceReadbackMutex;
extern SurfaceReadback g_surfaceReadback[kSurfaceReadbackSlots];
// Bumped after the slot is filled, so a reader that checks it first can never
// pair a new sequence with half-written bytes.
extern std::atomic<uint32_t> g_surfaceReadbackSeq;

}  // namespace mx::hle
