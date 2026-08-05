#pragma once

// Decodes Xenos vertex shader microcode far enough to recover the vertex
// layout: which fetch slot each attribute comes from, at what offset, in what
// format, and — the reason this exists — the real stride.
//
// Until now the stride was inferred by dividing the vertex buffer size by the
// vertex count and accepting exact divisions in 8..64, because the vertex fetch
// constant does not carry it. That guess is why only stride-28 draws are drawn
// and ~230 draws per frame are skipped. The shader has always carried the
// answer; the microcode arrives inline in the PM4 ring as IM_LOAD_IMMEDIATE
// (0x2B), 68 per frame.
//
// This header deliberately includes no SDK header — it exposes plain data, so a
// test can include it without the SDK on its path. The bit layouts live in
// shader_ucode.cpp, which uses the SDK's own structs rather than hand-rolled
// shifts; their static_assert_size means a packing disagreement fails the build
// loudly instead of yielding silently wrong strides.

#include <cstdint>
#include <vector>

namespace mx::pm4 {

// One attribute a vertex shader fetches. Raw values throughout; `format` is the
// 6-bit xenos::VertexFormat value rather than the enum so this header stays
// SDK-free.
struct VertexAttribute {
  uint32_t fetch_slot    = 0;   // 0..95, from vfetch fetch_constant_index()
  uint32_t offset_bytes  = 0;   // vfetch offset() * 4
  uint32_t stride_bytes  = 0;   // vfetch stride() * 4, from the owning full fetch
  uint32_t format        = 0;   // xenos::VertexFormat, e.g. 57 = k_32_32_32_FLOAT
  uint32_t components    = 0;   // 0 if the format is not one we know
  uint32_t size_bytes    = 0;   // 0 if the format is not one we know
  uint32_t dest_reg      = 0;
  uint32_t dest_swizzle  = 0;
  bool     is_signed     = false;
  bool     is_normalized = false;
  int      exp_adjust    = 0;
  bool     from_mini     = false;  // inherited slot/stride from a preceding full

  // True when this attribute's destination register reaches the position export
  // (register 62) through the shader's ALU instructions. This is the shader
  // saying which of its inputs is the position, rather than us guessing from
  // offset and format — see kPositionExportRegister in the .cpp.
  bool     feeds_position = false;
};

// The single sampled-texture profile understood by the HLE renderer. This is
// decoded from the pixel shader's tfetch instruction, so UV linkage is based
// on the register the shader reads rather than a declaration-order guess.
struct PixelTextureBinding {
  uint32_t sampler = 0;
  uint32_t src_reg = 0;
  uint32_t src_swizzle = 0;
  bool unnormalized = false;
};

// Accepts only a pixel shader containing exactly one 2D texture fetch. Other
// fetch profiles deliberately fall back to the colour-only host pipeline.
bool DecodeSingleTexturePixelShader(const uint32_t* dwords,
                                    uint32_t dword_count,
                                    PixelTextureBinding& out,
                                    const char** fail = nullptr);

// The Xenos vertex position export destination. A vertex shader writes its
// clip-space position by exporting to this register; every other export is an
// interpolator, a point size or a misc output.
inline constexpr uint32_t kPositionExportRegister = 62;

// Walk the control flow of a vertex shader blob and enumerate every vertex
// fetch, in program order. Appends to `out` (does not clear it).
//
// Also tracks, through the shader's ALU instructions, which fetched attributes
// reach the position export — setting `feeds_position` on those. If
// `out_saw_position_export` is non-null it receives whether an export to
// register 62 was seen at all, which distinguishes "the shader exports a
// position that no fetch feeds" (computed from constants) from "this blob has
// no position export" (not a vertex shader, or decode stopped early).
//
// Returns false and sets *fail to a static reason string on a malformed blob.
// Never throws, never loops unbounded, never reads outside [dwords, +count).
// `fail` and `out_saw_position_export` may be null.
bool DecodeVertexShaderFetches(const uint32_t* dwords, uint32_t dword_count,
                               std::vector<VertexAttribute>& out,
                               const char** fail,
                               bool* out_saw_position_export = nullptr);

// Byte size of a xenos::VertexFormat, or 0 if unrecognised. Exposed for the
// same reason it is hand-written rather than taken from the SDK: the SDK's
// GetVertexFormatComponentCount asserts on an unhandled case, and a 6-bit field
// out of a malformed blob can hold any of 64 values.
uint32_t VertexFormatSizeBytes(uint32_t format, uint32_t* out_components);

// IEEE half -> float.
float HalfToFloat(uint16_t h);

// The width of the unit a format's bits are packed into, which is the unit the
// endian swap has to operate on — 2 for the 16-bit-component formats, 4 for
// everything else (a k_8_8_8_8 colour is one big-endian dword, not four
// independent bytes). 0 for a format not recognised.
//
// This is not the same as the per-component width and must not be confused with
// it: k_8_8_8_8 has 1-byte components packed in a 4-byte unit.
uint32_t VertexFormatUnitBytes(uint32_t format);

// Apply the vertex fetch constant's endian swap in place, turning guest bytes
// into GPU-native little-endian.
//   endian: 0 = none, 1 = 8in16, 2 = 8in32
//   format: the xenos::VertexFormat those bytes hold
//
// The mode alone does not determine the swap width. Reversing all four bytes of
// a dword that holds *two* 16-bit components byte-swaps each component **and
// exchanges the pair** — so an 8in32 request over a 16-bit format has to be
// performed as a 2-byte reversal. Applying the mode literally is what turned
// every (x, y, z, w=1) position into (y, x, w=1, z) and left the interpreter
// reading a constant 1.0 as the z coordinate. See AGENTS.md, "The position input
// is (x, y, z, 1) read as (x, y, 1, z)".
//
// Because the right width depends on the format, this cannot be applied once
// over a whole vertex: a single vertex mixes 16-bit positions with 32-bit
// colours. It is applied per attribute, inside the read.
void ApplyFetchEndianFor(uint8_t* data, size_t bytes, uint32_t endian,
                         uint32_t format);

// How a format's raw bits are interpreted. The layout of a format says how many
// bits each component gets; this says what they mean, and the two are
// independent — k_8_8_8_8 is D3DCOLOR under kUnorm and a bone index under
// kUint, with identical bits.
//
// On the PM4 path this is not selectable: a vfetch does carry the two bits, but
// ReadVertexAttribute was written before they were decoded and hardcodes one
// choice per format. The D3D9 path does have them, from the declaration's Type
// dword, and losing that distinction is exactly the failure the HLE route
// exists to avoid.
enum class NumFormat : uint8_t {
  kUnorm = 0,   // and every float format, where it does not apply
  kSnorm,
  kUint,
  kSint,
};

// Decode one attribute of one vertex to up to 4 floats. `vertex_base` points at
// the start of the vertex **in guest byte order**; `endian` is the fetch
// constant's swap mode and is applied here, at the format's own unit width,
// because only here is the format known. Pass 0 for bytes already in host
// order. Unwritten components are left as (0,0,0,1). Returns false for a format
// not handled, leaving `out` untouched, so callers can count the gap instead of
// rendering a guess.
bool ReadVertexAttributeAs(const uint8_t* vertex_base, uint32_t vertex_bytes,
                           uint32_t format, uint32_t offset_bytes,
                           uint32_t size_bytes, NumFormat num, uint32_t endian,
                           float out[4]);

// The PM4 path's entry point. Delegates to the above with the interpretation
// this function has always used per format, so its results are unchanged.
bool ReadVertexAttribute(const uint8_t* vertex_base, uint32_t vertex_bytes,
                         const VertexAttribute& attr, uint32_t endian,
                         float out[4]);

// Pick the position attribute. Prefers an attribute the decoder proved feeds
// the position export; falls back, only when none does, to the old guess — the
// lowest-offset attribute carrying at least two components in a format that
// could hold coordinates. Returns null if nothing qualifies.
//
// If `out_from_export` is non-null it receives whether the answer came from the
// shader or from the fallback guess, so callers can gate on the difference
// rather than trusting both equally.
//
// Explicitly NOT by destination register alone — both ground-truth shaders put
// position in dest_reg 1 and the second attribute in dest_reg 0, so keying on
// dest_reg 0 would pick colour every time. The export trace is what says *which*
// register the position is in.
const VertexAttribute* PickPositionAttribute(
    const std::vector<VertexAttribute>& attrs,
    bool* out_from_export = nullptr);

// Pick an attribute usable as vertex colour, or null. Prefers a packed 8888.
const VertexAttribute* PickColorAttribute(
    const std::vector<VertexAttribute>& attrs);

}  // namespace mx::pm4
