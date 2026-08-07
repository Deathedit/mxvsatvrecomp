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

namespace mx::hle {

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

// Walk the executable clauses of a pixel shader and enumerate every 2D
// texture fetch in program order. This is the evidence/diagnostic decoder: it
// does not impose the renderer's current one-texture limit. Relative or non-2D
// fetches still reject the blob because their sampler/coordinate linkage cannot
// be represented by PixelTextureBinding without guessing.
//
// Appends to `out` (does not clear it). Returns false for malformed or
// unsupported fetch instructions and sets *fail to a static reason string.
bool DecodePixelTextureFetches(const uint32_t* dwords, uint32_t dword_count,
                               std::vector<PixelTextureBinding>& out,
                               const char** fail = nullptr);

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

// Exact vertex-shader identity comparison across the two representations D3D9
// creates: an unpatched template and the PM4-ready copy after
// PatchVertexShaderToMatchVertexDeclaration. Control flow, ALU instructions and
// every non-patched vfetch bit must agree. Only fields proven to be rewritten
// by that D3D9 routine (fetch slot/coalescing, format/number interpretation,
// swizzle, offset and stride) are ignored. Returns false for malformed code.
struct VertexShaderStructureStats {
  uint32_t equal_dwords = 0;       // after masking proven patch fields
  uint32_t fetch_mismatches = 0;   // retained bits differ in a vfetch
  uint32_t other_mismatches = 0;   // CF, ALU, texture fetch, or padding
  uint32_t fetch_xor[3] = {};      // all raw differing vfetch bits, per word
};

bool VertexShaderStructureMatches(
    const uint32_t* shader_template, const uint32_t* patched_shader,
    uint32_t dword_count, VertexShaderStructureStats* stats = nullptr);

// Byte size of a xenos::VertexFormat, or 0 if unrecognised. Exposed for the
// same reason it is hand-written rather than taken from the SDK: the SDK's
// GetVertexFormatComponentCount asserts on an unhandled case, and a 6-bit field
// out of a malformed blob can hold any of 64 values.
uint32_t VertexFormatSizeBytes(uint32_t format, uint32_t* out_components);

// IEEE half -> float.
float HalfToFloat(uint16_t h);

// Apply the vertex fetch constant's endian swap in place, turning guest bytes
// into GPU-native little-endian.
//   endian: 0 = none, 1 = 8in16, 2 = 8in32
//
// The mode is applied literally, and the format is deliberately not consulted.
// Reversing all four bytes of a dword that holds *two* 16-bit components does
// byte-swap each component **and exchange the pair** — but that exchange is the
// hardware's real behaviour, not a defect, and the shader compiler already
// compensates for it in the vfetch destination swizzle.
//
// This once narrowed the unit to 2 for the 16-bit formats to suppress the
// exchange. That was wrong, and cost several rounds. The narrowing was derived
// from staring at the decoded attribute — which does read (y, x, 1, z) for a
// (x, y, z, 1) position — without following it through the swizzle that comes
// next. What settles it is the swizzle each format actually carries, counted
// over a front-end run:
//
//   16-bit units:  fmt 32 -> 0x4C1 (x935), fmt 26 -> 0x4C1, fmt 31 -> 0xFC1
//                  (x913). All pairwise exchanges.
//   32-bit units:  fmt 37 -> 0xB08 (x2322), fmt 7 -> 0xE88 (x1718),
//                  fmt 57 -> 0xA88, fmt 38 -> 0x688. All identity.
//
// 1851 fetches, no exceptions: every 16-bit format asks for the pair to be
// exchanged back, and no 32-bit format does. The compiler emits that permutation
// for one reason. Suppress the swap and the swizzle scrambles good data instead
// of restoring it — which is what put the homogeneous 1 in slot z and left the
// model matrix's w row reading the model-space z.
//
// Applied per attribute rather than once over the vertex only because the read
// works on one attribute at a time; the width no longer depends on the format.
void ApplyFetchEndianFor(uint8_t* data, size_t bytes, uint32_t endian);

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

}  // namespace mx::hle
