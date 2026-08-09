#pragma once

// Emits HLSL from Xenos shader microcode, so the guest's own shaders run on the
// GPU instead of being approximated.
//
// Why this exists: the pixel side was never translated at all. A guest pixel
// shader was decoded far enough to find its texture fetches
// (DecodePixelTextureFetches), one binding was picked, the rest of the shader
// was discarded, and the draw rendered `tex * vertexColour`. Measured against
// logs/mx_665, real shaders in this game carry up to 16 fetches across four
// samplers; rendering a 16-tap compositor pass as one tap times a colour is what
// collapses the intro chain to a uniform field.
//
// Why an emitter and not the SDK's DxbcShaderTranslator: that translator is
// Xenia's, and using it means adopting Xenia's binding model wholesale — EDRAM
// ROV, a shared-memory SRV, bindless descriptor heaps, its system-constant
// layout. That is a different renderer. This emits HLSL that the existing
// CompileShader path compiles and the existing root signature can feed.
//
// The walk is deliberately the same one shader_alu.cpp interprets and
// shader_ucode.cpp decodes: bound the CF section at the lowest exec target, then
// run the exec blocks in order. Same structure, same refusals, string output
// instead of float math. Two walks that disagree would be worse than one walk
// that is wrong, so where this file makes a semantic choice it matches the
// interpreter's, and where it cannot it refuses.
//
// SDK-free header, same rule as shader_ucode.h and shader_alu.h, so the unit
// test needs no SDK on its include path.

#include <cstdint>
#include <string>
#include <vector>

#include "gpu/shader_ucode.h"

namespace mx::hle {

enum class HlslStage : uint8_t {
  kVertex,
  kPixel,
};

// The widest VS-to-PS linkage this emitter will build. Deliberately the same
// number as AluResult::kMaxInterpolators, but declared here rather than taken
// from shader_alu.h: the emitter must not depend on the interpreter, since the
// point of the work is to retire it.
inline constexpr uint32_t kMaxHlslInterpolators = 16;

// The linkage width actually used, and the one number both stages MUST agree
// on: the emitted pixel shader declares its input struct with exactly this many
// interpolators, so a vertex stage offering a different count produces two
// signatures that cannot link and CreateGraphicsPipelineState fails with no
// message. That is not hypothetical — emitting at kMaxHlslInterpolators while
// the renderer's vertex shader carried 8 is exactly how it first failed.
//
// Eight because the widest input_mask measured in this game is 0xFF (r0-r7),
// from the 14-fetch shader 0x216A8C20. Raising it costs vertex bandwidth on
// every translated draw; lowering it would silently drop interpolators.
inline constexpr uint32_t kHlslInterpolatorLinkage = 8;

// Why a shader could not be emitted. Mirrors AluStatus, and for the same
// reason: a gap must be countable by cause, not collapse into one opaque
// failure. Anything but kOk means `source` is empty and the caller must fall
// back rather than render a guess.
enum class HlslStatus : uint8_t {
  kOk = 0,
  kMalformed,             // blob did not decode
  kNoExec,                // no exec instruction; not a shader body
  kUnsupportedCf,         // a jump, call or loop — see IsUnsupportedCf
  kUnsupportedVectorOp,
  kUnsupportedScalarOp,
  kUnsupportedFetch,      // relative or non-2D texture fetch
  // The causes the one status above used to hide. They need different fixes --
  // a relative fetch needs an index we do not have, a 3D fetch needs a resource
  // type we do not create -- so a combined count could not say which work would
  // pay. It said exactly that for cube, which turned out to be the largest
  // bucket and is now translated.
  kFetchRelative,         // src or dest indexed by a register
  kFetchCube,             // UNUSED: cube fetches translate, see EmitTextureFetch
  kFetch1D,               // UNUSED: 1D fetches translate through a 2D binding
  kFetch3D,               // dimension k3DOrStacked
  // Two fetches on one sampler slot at different dimensions. The slot gets one
  // HLSL declaration and one descriptor, so neither choice can serve both.
  kFetchDimensionConflict,
  kTooManySamplers,       // more distinct samplers than kMaxSamplerSlots
  kLoopRelative,          // aL-relative index; no honest value for aL
  kInstructionCap,
  kNoOutput,              // executed, but never wrote position (VS) or colour (PS)
};

const char* HlslStatusName(HlslStatus s);

struct HlslShader {
  HlslStatus status = HlslStatus::kMalformed;
  std::string source;

  // The opcode that stopped us, when status names an unsupported op or fetch.
  // Lets the caller build a histogram of exactly what is missing instead of a
  // single failure count — the same discipline as AluResult::blocking_opcode,
  // and the reason the interpreter's coverage was measurable at all.
  uint32_t blocking_opcode = 0;

  // Every 2D texture fetch, in program order, with the sampler and coordinate
  // register each names. Same type the existing decoder produces, so the
  // renderer's binding code does not need a second shape to understand.
  std::vector<PixelTextureBinding> fetches;

  // Bit i set = GUEST sampler i is read by this shader. Derived from `fetches`;
  // carried separately because the renderer binds per sampler slot, not per
  // fetch, and one sampler is commonly fetched many times.
  uint32_t sampler_mask = 0;

  // Guest sampler indices are NOT used as shader register numbers. They are
  // remapped to a compact 0-based slot, in order of first fetch, and the shader
  // declares `xe_tex<slot> : register(t<slot>)`.
  //
  // Not cosmetic. A descriptor table is a contiguous range, so emitting at the
  // guest index would force every table to span t0..t<max guest index> — the
  // 14-fetch shader binds s8-s12, which would need thirteen descriptor slots to
  // deliver five textures, and one block that size per draw would exhaust the
  // heap within a frame. Compact slots mean a table only as wide as the shader
  // has distinct samplers, and the overwhelmingly common single-sampler case
  // needs exactly one.
  //
  // `sampler_slot_guest[i]` is the guest sampler that slot i was assigned, for
  // the first `sampler_count` slots. The renderer uses it to decide which
  // texture to place at each descriptor.
  // Measured at 144 distinct shaders: 8 slots refused 5 of them outright
  // ("too many distinct samplers"). Must equal DrawCall::kMaxPixelTextures and
  // the renderer's kTranslatedSamplerSlots -- that last one is the width of the
  // descriptor table, so the three are one number wearing three names and
  // d3d12_game.cpp static_asserts them together.
  static constexpr uint32_t kMaxSamplerSlots = 16;
  uint32_t sampler_count = 0;
  uint32_t sampler_slot_guest[kMaxSamplerSlots] = {};

  // Bit i set = compact slot i is declared Texture2DArray, not Texture2D,
  // because its fetches are cube. The binder MUST create a TEXTURE2DARRAY SRV
  // for those slots: an SRV whose dimension contradicts the declaration is
  // undefined, not merely wrong-looking.
  uint32_t sampler_array_mask = 0;

  // A cube fetch appeared in a shader that never ran the `cube` ALU op, so the
  // (S, T, face) operand form this translation assumes was produced by
  // something we have not seen. Diagnostic only -- reported, not refused.
  bool cube_fetch_without_cube_op = false;

  // Bit i set = register i is read before this shader ever writes it, i.e. it
  // arrives as an input. For a pixel shader these are the interpolators the
  // vertex stage must supply; for a vertex shader they are the vfetch
  // destinations.
  //
  // Reported rather than assumed because the two stages have to agree on a
  // linkage and neither blob states one. The caller owns that decision; the
  // emitter only says what it reads.
  uint32_t input_mask = 0;

  // Bit i set = the shader exports interpolator i (VS), or writes colour
  // target i (PS). Position (VS) and depth (PS) are the flags below, not bits
  // here, because they are not interpolators and must not be linked as such.
  uint32_t export_mask = 0;
  bool writes_position = false;  // VS: exported to register 62
  bool writes_depth = false;     // PS: exported to register 61

  // The highest constant index the shader reads within its OWN stage bank, or
  // 0 if it reads none. Each stage indexes its bank from 0: the vertex bank is
  // ALU constants 0-255 at device+0x780, the pixel bank is 256-511 at
  // device+0x1780. Proven from D3DDevice_DrawVertices' own flush, which passes
  // Xenos register base 0x4000 for the first and 0x4400 for the second
  // (0x4400 = 0x4000 + 1024 dwords = constant 256). Applying that base is the
  // caller's job at upload time, so this index is never above 255.
  uint32_t max_const_index = 0;
  bool reads_constants = false;
};

// Emit HLSL for one shader blob.
//
// `interpolator_count` fixes the VS-to-PS linkage width: both stages must be
// emitted with the same value or the signatures will not match. Clamped to
// kMaxHlslInterpolators.
//
// Does not throw and does not loop unbounded. On failure `out.source` is empty
// and `out.status` says why.
bool EmitShaderHlsl(const uint32_t* dwords, uint32_t dword_count,
                    HlslStage stage, uint32_t interpolator_count,
                    HlslShader& out);

}  // namespace mx::hle
