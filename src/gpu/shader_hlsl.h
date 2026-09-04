#pragma once

// Emits HLSL from Xenos shader microcode, so the guest's own shaders run on the
// GPU instead of being approximated.
//
// Why this exists: the pixel side was never translated at all. A guest pixel
// shader was decoded far enough to find its texture fetches, one binding was
// picked, the rest was discarded, and the draw rendered `tex * vertexColour`.
// Real shaders in this game carry up to 16 fetches across four samplers, and
// rendering a 16-tap compositor pass as one tap times a colour is what collapses
// the intro chain to a uniform field.
//
// Why an emitter and not the SDK's DxbcShaderTranslator: that translator is
// Xenia's, and using it means adopting Xenia's binding model wholesale -- EDRAM
// ROV, a shared-memory SRV, bindless descriptor heaps, its system-constant
// layout. That is a different renderer. This emits HLSL the existing
// CompileShader path compiles and the existing root signature can feed.
//
// The walk is deliberately the same one shader_alu.cpp interprets and
// shader_ucode.cpp decodes: bound the CF section at the lowest exec target, then
// run the exec blocks in order. Two walks that disagree would be worse than one
// walk that is wrong, so where this file makes a semantic choice it matches the
// interpreter's, and where it cannot it refuses.
//
// SDK-free header, same rule as shader_ucode.h and shader_alu.h.

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

// The linkage width actually used, and the one number both stages MUST agree on:
// the emitted pixel shader declares its input struct with exactly this many
// interpolators, so a vertex stage offering a different count produces two
// signatures that cannot link and CreateGraphicsPipelineState fails with no
// message. Not hypothetical -- emitting at kMaxHlslInterpolators while the
// renderer's vertex shader carried 8 is exactly how it first failed.
//
// Eight because the widest input_mask measured in this game is 0xFF (r0-r7).
// Raising it costs vertex bandwidth on every translated draw; lowering it would
// silently drop interpolators.
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
  // UNUSED: k3DOrStacked translates through a Texture2DArray. The instruction
  // cannot say whether the texture is a stack or a true volume -- only the
  // fetch constant can -- so the volume case is refused where that is known,
  // in DescribeHleTexture2D, and this status has no reachable cause left.
  kFetch3D,
  // Two fetches on one sampler slot at different dimensions. The slot gets one
  // HLSL declaration and one descriptor, so neither choice can serve both.
  kFetchDimensionConflict,
  kTooManySamplers,       // more distinct samplers than kMaxSamplerSlots
  kLoopRelative,          // aL-relative index; no honest value for aL
  kInstructionCap,
  kNoOutput,              // executed, but never wrote position (VS) or colour (PS)
  // Vertex fetch, and only reachable when the caller asked for the fetch to be
  // emitted. Each one means the draw stays on the CPU vertex path, which still
  // works — so these are a cost, not a defect, and are counted apart so it is
  // visible which decode is worth adding.
  kVertexFetchFormat,     // a vertex format the emitter does not decode
  kVertexFetchExpAdjust,  // non-zero exp_adjust, a scale nothing applies
  kVertexFetchIndex,      // index operand this cannot express as SV_VertexID
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
  // guest index would force every table to span t0..t<max guest index> -- the
  // 14-fetch shader binds s8-s12, which would need thirteen descriptor slots to
  // deliver five textures, and one block that size per draw would exhaust the
  // heap within a frame.
  //
  // `sampler_slot_guest[i]` is the guest sampler that slot i was assigned, for
  // the first `sampler_count` slots.
  //
  // Measured at 144 distinct shaders: 8 slots refused 5 of them outright. Must
  // equal DrawCall::kMaxPixelTextures and the renderer's
  // kTranslatedSamplerSlots -- that last is the width of the descriptor table,
  // so the three are one number wearing three names and d3d12_game.cpp
  // static_asserts them together.
  static constexpr uint32_t kMaxSamplerSlots = 16;
  uint32_t sampler_count = 0;
  uint32_t sampler_slot_guest[kMaxSamplerSlots] = {};

  // Base HLSL register for this stage's textures and samplers.
  //
  // BOTH stages use t0/s0, and that is correct rather than a collision. D3D12's
  // ShaderVisibility scopes a register to a stage, so a VERTEX-visible table at
  // s0 and a PIXEL-visible one at s0 are different bind points. The two shaders
  // are translated and cached independently, so their compact slot 0 IS a
  // different guest sampler -- the root signature keeps them apart, not the
  // register number.
  //
  // The first attempt moved the vertex stage to t17/s16 to "avoid" a collision
  // that does not exist. It is also impossible: vs_5_0 has exactly 16 sampler
  // slots, so s16 is not a register and FXC rejects the shader with X4509.
  static constexpr uint32_t kPixelTextureBaseRegister = 0;
  static constexpr uint32_t kPixelSamplerBaseRegister = 0;
  static constexpr uint32_t kVertexTextureBaseRegister = 0;
  static constexpr uint32_t kVertexSamplerBaseRegister = 0;

  // Bit i set = compact slot i is declared Texture2DArray, not Texture2D,
  // because its fetches are cube or 3D/stacked — both address a third
  // coordinate that a Texture2D has no axis for. The binder MUST create a
  // TEXTURE2DARRAY SRV for those slots: an SRV whose dimension contradicts the
  // declaration is undefined, not merely wrong-looking.
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

  // Bit i set = the shader exported to destination i and the emitter DISCARDED
  // it: a point size, a misc output, or an interpolator past the agreed linkage
  // width. Reported rather than dropped in silence, because "the walk saw no
  // exports" and "the walk saw exports and threw them away" are different
  // defects with different fixes. Destinations >= 32 are not represented --
  // those are MEMORY EXPORT and are counted separately below, because the guard
  // that fills this mask (`dest < 32`) excluded them by construction.
  uint32_t dropped_export_mask = 0;

  // Number of ALU exports to a MEMORY EXPORT register: ucode.h names them
  // kExportAddress = 32 and kExportData0..4 = 33..37. Memory export lets a
  // shader write to guest memory rather than to a render target, and we do not
  // implement it -- such an export is dropped.
  //
  // This exists because the drop was not merely unimplemented, it was
  // UNCOUNTABLE. The mask above is written under `if (dest < 32)`, so a
  // memexport skipped the mask and the else-branch alike and left no trace: from
  // outside, a shader that exports to memory and one that exports nothing read
  // identically.
  //
  // A nonzero count here does NOT by itself mean a defect -- it means the
  // question "does this title use memory export?" now has an answer.
  uint32_t memexport_count = 0;

  // PS only. Bit i set = the shader EXPORTED to colour target i, so export_mask
  // reports it as produced, but no export to it ever assigned a channel.
  //
  // An export whose vector, scalar, constant-0 and constant-1 write masks are
  // all empty assigns nothing, and per the reference that is correct (ucode.h:
  // "vector_write_mask 0, scalar_write_mask 0 ... unchanged"). What is not
  // obviously correct is still declaring the target produced, because
  // `xe_colorN` then keeps its float4(0,0,0,0) initialiser and the shader
  // compiles to `mov o0.xyzw, l(0,0,0,0)` -- one that opaquely paints black.
  //
  // That is the exact body of the pixel shader on the 35-index draw which
  // replaces the menu background with (0,0,0,0), and whose guest state is blend
  // ONE/ZERO with colour mask 0xF, so the black is written rather than
  // discarded. Measured, not acted on: "unchanged" may equally be the intent.
  uint32_t color_unassigned_mask = 0;

  // The highest constant index the shader reads within its OWN stage bank, or 0
  // if it reads none. Each stage indexes its bank from 0: the vertex bank is ALU
  // constants 0-255 at device+0x780, the pixel bank is 256-511 at device+0x1780.
  // Proven from D3DDevice_DrawVertices' own flush, which passes Xenos register
  // base 0x4000 for the first and 0x4400 for the second. Applying that base is
  // the caller's job at upload time, so this index is never above 255.
  uint32_t max_const_index = 0;
  // Bitmask of the xe_c[] slots the shader actually reads, one bit per slot.
  // Identity by CONSTANT USE: the SpeedTree 3D vegetation shaders read
  // g_TreeLerps c69, g_TreeFade c70 and gTreeLODParams3 c82, while the billboard
  // shader reads g_BBTreeTypes at c80-c94. That distinguishes the two paths
  // exactly, where geometry shape could not -- the guest repacks .tree data into
  // its own stride.
  uint64_t const_mask[4] = {};
  // True when the shader indexes xe_c[] through a0, which saturates the mask
  // to all-ones. Such a shader cannot be identified by constant use at all --
  // BBVertexShader does `maxas a0, type*3` then c[a0+80/81/82], and skinned
  // meshes index gBoneMatrixVectors the same way. Counting them as "reads c80"
  // silently folds every skinned draw into the billboard bucket.
  bool const_relative = false;
  bool reads_constants = false;

  // Count of setp_* instructions -- the ops that WRITE p0.
  //
  // The name is now historical. It was accurate while nothing read xe_p0 at all;
  // since per-instruction predication landed, p0 written here is obeyed by any
  // `(p0)` ALU instruction that follows, and in the vertex stage by
  // cond_exec_pred as well. What is still unobeyed is narrower and has its own
  // counters below. Kept under the old name so a dump from before the change and
  // one from after still line up on the same field.
  uint32_t unhonoured_predicate_ops = 0;

  // Count of ALU instructions carrying their own predicate bits, emitted as
  // `if (xe_p0 == condition) { ... }`. Seen and obeyed are one number: unlike
  // the exec-level blocks there is no stage where this is skipped, because an
  // ALU instruction never samples and so its body is always legal flow control.
  //
  // This was the intro logo. Its pixel shader ends
  //
  //     (p0) sgts r2.w, -|r0|.x        <- `-|x| > 0`, constant false
  //     (p0) mul  r2.w, r2.wwww, r0.x
  //          max  export0, r2, r2
  //
  // and run unpredicated it forces alpha to a compile-time 0 -- FXC even
  // dead-strips the tfetch feeding it, which is how a one-texture DXBC came out
  // of a two-texture shader. The quad rasterised, output white, and blended to
  // nothing.
  uint32_t predicated_alu_ops = 0;

  // Count of TEXTURE FETCH instructions carrying predicate bits, all HONOURED by
  // gating the destination WRITE rather than the sample.
  //
  // Previously counted and refused, on the grounds that .Sample() needs
  // derivatives and is illegal in varying flow control. That objection is real
  // but applies only to putting the SAMPLE inside the branch: a fetch's only
  // effect is writing its destination register, so sampling unconditionally and
  // predicating the write is observationally identical.
  //
  // Found in Xenia's dump of this title: 53 predicated fetches across 4 shaders,
  // every one carrying FetchValidOnly=false -- the guest telling the hardware
  // NOT to make them lane-conditional.
  //
  // Does NOT cover p0-gated exec blocks, which remain unhonoured in the pixel
  // stage; see honoured_pred_exec_blocks.
  uint32_t predicated_fetches = 0;

  // Count of PREDICATED EXEC blocks -- kCondExecPred / kCondExecPredClean and
  // their *End forms. The emitter walks them like ordinary execs, so their body
  // runs UNCONDITIONALLY.
  //
  // Counted rather than refused: if the population is zero on this game, the
  // setp_* value translation is harmless and there is nothing to refuse; if it
  // is non-zero, this names the shaders to decide about instead of refusing a
  // working draw on suspicion.
  //
  // SPLIT, because these were one counter conflating two entirely different
  // mechanisms. Confirmed against Xenia's ucode.h:258 (the ReXGlue SDK copy is
  // stale; use the Xenia tree for GPU questions):
  //
  //   kCondExecPred / kCondExecPredEnd
  //       -> ControlFlowCondExecPredInstruction, gated on p0.
  //       FIXABLE NOW: xe_p0 already exists and setp_*_push writes it.
  //
  //   kCondExec / kCondExecEnd / kCondExecPredClean / kCondExecPredCleanEnd
  //       -> ControlFlowCondExecInstruction, gated on a BOOL CONSTANT at
  //       bool_address(). Despite the name, PredClean is NOT p0-gated.
  //       NOT FIXABLE YET: there is no bool constant bank in this translator.
  uint32_t pred_exec_blocks = 0;  // p0-gated, seen
  uint32_t bool_exec_blocks = 0;  // bool-constant-gated, seen

  // Of pred_exec_blocks, how many were emitted as `if (xe_p0 == ...)`. Vertex
  // stage only.
  //
  // A GAP HERE IS NOT A CORRECTNESS GAP, having been described as one here for
  // over a week. The exec-level predicate is a WAVEFRONT branch: "if any of the
  // invocations passes the predicate check, all of them will enter the exec". It
  // never gated a lane. Per-lane correctness is the INSTRUCTION predicates,
  // measured at 194 ALU + 46 fetch inside cond_exec_pred blocks in this title,
  // all 240 individually predicated. This number is a speed figure.
  uint32_t honoured_pred_exec_blocks = 0;

  // Count of fetch instructions skipped because their opcode is not one this
  // emitter implements -- the getTextureWeights / getCompTexLOD / getBCF family
  // and the two setGradients, everything in FetchOpcode at 16 and above except
  // setTexLOD.
  //
  // Skipped and counted rather than refused. Before the three-way classification
  // existed, `!= kTextureFetch` sent every one of these into the VERTEX fetch
  // branch, where a pixel shader dropped it and a vertex shader read its bits as
  // a VertexFetchInstruction and refused the whole shader on the garbage that
  // landed in exp_adjust.
  //
  // getGradients IS NOW HONOURED and is no longer in this bucket. The sentence
  // that used to stand here -- "the one pixel shader that uses getGradients and
  // compiles fine without it" -- was true about compiling and wrong about
  // everything else: that shader is ps_hft_fback, the terrain's virtual-texture
  // feedback pass, and skipping the op left it computing a CONSTANT LOD from a
  // stale interpolator. "Compiles fine without it" is not a test.
  uint32_t unhonoured_fetch_ops = 0;

  // --- vertex fetch, only when the caller asked for it ----------------------
  //
  // The number of vfetches emitted into the body, and for each one the guest
  // fetch slot it reads. The host fills xe_vf[i] for i < vertex_fetch_count with
  // {byte offset of that stream in the merged raw buffer, stride, endian}.
  //
  // Ordinals follow program order, which is the order DecodeVertexShaderFetches
  // pushes its attributes in -- both walk the same stream the same way.
  // `fetch_slot` is still carried explicitly rather than inferred from that
  // pairing, because a silent divergence between the two walks would misaddress
  // geometry with no symptom at the point of the mistake.
  static constexpr uint32_t kMaxVertexFetches = 32;
  uint32_t vertex_fetch_count = 0;
  uint32_t vertex_fetch_slot[kMaxVertexFetches] = {};

  // Streams whose fetch is indexed by a register OTHER than r0.x -- a value the
  // shader COMPUTED, not the vertex index. One bit per stream, using the same
  // 95-minus-slot numbering the host binds by.
  //
  // The GPU fetch path handles these by indexing with the named register and
  // copying the whole stream. The CPU path CANNOT: it is declaration-driven,
  // finds attributes by usage, and never executes the ALU that produces the
  // index -- so for these streams there is no value it could read. This flag
  // exists so it can say so instead of producing confident nonsense.
  uint32_t computed_index_streams = 0;

  // The same fact per FETCH ORDINAL rather than per stream, because the
  // consumer needs it per fetch: a stream can carry both a computed-index
  // fetch and a vertex-indexed one, and they need different bases into the
  // merged buffer. Bit i is set when fetch ordinal i is computed-indexed.
  uint32_t computed_index_fetches = 0;
};

// Emit HLSL for one shader blob.
//
// `interpolator_count` fixes the VS-to-PS linkage width: both stages must be
// emitted with the same value or the signatures will not match. Clamped to
// kMaxHlslInterpolators.
//
// `emit_vertex_fetch` (vertex stage only) makes the shader perform its own
// vfetches out of a raw guest vertex buffer bound as `xe_vb`, indexed by
// SV_VertexID, instead of reading input elements the CPU unpacked. The two forms
// are NOT interchangeable: the fetch form leaves input_mask empty and needs an
// empty input layout plus xe_vf[], the other needs an input layout built from
// input_mask. Emit both and keep both -- the CPU path still needs the second
// whenever the first refuses.
//
// Does not throw and does not loop unbounded. On failure `out.source` is empty
// and `out.status` says why.
bool EmitShaderHlsl(const uint32_t* dwords, uint32_t dword_count,
                    HlslStage stage, uint32_t interpolator_count,
                    HlslShader& out, bool emit_vertex_fetch = false);

}  // namespace mx::hle
