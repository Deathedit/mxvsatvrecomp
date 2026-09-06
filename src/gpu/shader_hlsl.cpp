#include "gpu/shader_hlsl.h"

#include <cstring>

#include <rex/graphics/format/ucode.h>

namespace uc = rex::graphics::ucode;

namespace mx::hle {

const char* HlslStatusName(HlslStatus s) {
  switch (s) {
    case HlslStatus::kOk: return "ok";
    case HlslStatus::kMalformed: return "malformed";
    case HlslStatus::kNoExec: return "no exec instruction";
    case HlslStatus::kUnsupportedCf: return "unsupported control flow";
    case HlslStatus::kUnsupportedVectorOp: return "unsupported vector op";
    case HlslStatus::kUnsupportedScalarOp: return "unsupported scalar op";
    case HlslStatus::kUnsupportedFetch: return "unsupported texture fetch";
    case HlslStatus::kFetchRelative: return "relative texture fetch";
    case HlslStatus::kFetchCube: return "cube texture fetch";
    case HlslStatus::kFetch1D: return "1D texture fetch";
    case HlslStatus::kFetch3D: return "3D/stacked texture fetch";
    case HlslStatus::kVertexFetchFormat:
      return "vertex fetch format";
    case HlslStatus::kVertexFetchExpAdjust:
      return "vertex fetch exp_adjust";
    case HlslStatus::kVertexFetchIndex:
      return "vertex fetch index operand";
    case HlslStatus::kFetchDimensionConflict:
      return "one sampler fetched at two dimensions";
    case HlslStatus::kTooManySamplers: return "too many distinct samplers";
    case HlslStatus::kLoopRelative: return "aL-relative (loop)";
    case HlslStatus::kInstructionCap: return "instruction cap";
    case HlslStatus::kNoOutput: return "no position/colour export";
  }
  return "?";
}

namespace {

constexpr uint32_t kMaxInstructions = 2048;
constexpr uint32_t kNumTemps = 64;

// Xenos export destinations that are not interpolators. A vertex shader writes
// its clip-space position to 62; a pixel shader writes depth to 61 and colour
// to 0..3. Linking either as an interpolator would be a signature mismatch, so
// they are flags on HlslShader rather than bits in export_mask.
constexpr uint32_t kPixelDepthExportRegister = 61;
constexpr uint32_t kMaxColorTargets = 4;

//===========================================================================
// The exec accessors. Address/count/sequence sit at the same bit offsets in all
// three exec structs but are distinct union members, so the dispatch is copied
// from shader_alu.cpp deliberately rather than shared: a shared helper that
// drifted would desynchronise the emitter from the interpreter silently.
//===========================================================================
uint32_t ExecAddress(const uc::ControlFlowInstruction& cf) {
  switch (cf.opcode()) {
    case uc::ControlFlowOpcode::kExec:
    case uc::ControlFlowOpcode::kExecEnd:
      return cf.exec.address();
    case uc::ControlFlowOpcode::kCondExec:
    case uc::ControlFlowOpcode::kCondExecEnd:
    case uc::ControlFlowOpcode::kCondExecPredClean:
    case uc::ControlFlowOpcode::kCondExecPredCleanEnd:
      return cf.cond_exec.address();
    case uc::ControlFlowOpcode::kCondExecPred:
    case uc::ControlFlowOpcode::kCondExecPredEnd:
      return cf.cond_exec_pred.address();
    default:
      return 0;
  }
}
uint32_t ExecCount(const uc::ControlFlowInstruction& cf) {
  switch (cf.opcode()) {
    case uc::ControlFlowOpcode::kExec:
    case uc::ControlFlowOpcode::kExecEnd:
      return cf.exec.count();
    case uc::ControlFlowOpcode::kCondExec:
    case uc::ControlFlowOpcode::kCondExecEnd:
    case uc::ControlFlowOpcode::kCondExecPredClean:
    case uc::ControlFlowOpcode::kCondExecPredCleanEnd:
      return cf.cond_exec.count();
    case uc::ControlFlowOpcode::kCondExecPred:
    case uc::ControlFlowOpcode::kCondExecPredEnd:
      return cf.cond_exec_pred.count();
    default:
      return 0;
  }
}
uint32_t ExecSequence(const uc::ControlFlowInstruction& cf) {
  switch (cf.opcode()) {
    case uc::ControlFlowOpcode::kExec:
    case uc::ControlFlowOpcode::kExecEnd:
      return cf.exec.sequence();
    case uc::ControlFlowOpcode::kCondExec:
    case uc::ControlFlowOpcode::kCondExecEnd:
    case uc::ControlFlowOpcode::kCondExecPredClean:
    case uc::ControlFlowOpcode::kCondExecPredCleanEnd:
      return cf.cond_exec.sequence();
    case uc::ControlFlowOpcode::kCondExecPred:
    case uc::ControlFlowOpcode::kCondExecPredEnd:
      return cf.cond_exec_pred.sequence();
    default:
      return 0;
  }
}

// Refused rather than approximated, for the same reason the interpreter refuses
// it: emitting a straight-line body for a shader that branches or loops produces
// confidently wrong pixels. Every shader captured from this game is a straight
// run of execs (kUnsupportedCf measures 0).
bool IsUnsupportedCf(uc::ControlFlowOpcode op) {
  switch (op) {
    case uc::ControlFlowOpcode::kCondJmp:
    case uc::ControlFlowOpcode::kCondCall:
    case uc::ControlFlowOpcode::kReturn:
    case uc::ControlFlowOpcode::kLoopStart:
    case uc::ControlFlowOpcode::kLoopEnd:
      return true;
    default:
      return false;
  }
}

const char kComponent[5] = "xyzw";

// ".xzw" for mask 0b1101. Empty for mask 0 — callers must not emit an
// assignment in that case, since HLSL has no empty write mask.
std::string MaskSwizzle(uint32_t mask) {
  std::string s;
  for (uint32_t c = 0; c < 4; ++c) {
    if (mask & (1u << c)) s += kComponent[c];
  }
  return s.empty() ? s : ("." + s);
}

class Emitter {
 public:
  Emitter(HlslStage stage, uint32_t interpolator_count, bool vertex_fetch)
      : stage_(stage),
        interpolators_(interpolator_count),
        vertex_fetch_(vertex_fetch) {}

  HlslStatus status = HlslStatus::kOk;
  uint32_t blocking_opcode = 0;
  uint32_t input_mask = 0;
  uint32_t written_mask = 0;   // temps this shader has written so far
  uint32_t export_mask = 0;
  uint32_t dropped_export_mask = 0;
  uint32_t memexport_count = 0;
  uint32_t color_mask = 0;
  // Colour targets that received an ACTUAL assignment, as opposed to merely
  // being named as an export destination.
  //
  // These two masks are set in different places on purpose. `color_mask` is set
  // as soon as an export names a colour target, BEFORE the write-mask check --
  // and an export whose vector, scalar, constant-0 and constant-1 masks are all
  // empty legitimately assigns nothing (ucode.h:2053).
  //
  // What is NOT obviously correct is still reporting the shader as producing
  // that target: if every export to colour 0 has an empty mask, `xe_color0`
  // keeps its float4(0,0,0,0) initialiser and we emit `o0 = xe_color0`, which
  // the compiler folds to `mov o0.xyzw, l(0,0,0,0)` -- the exact body of the
  // pixel shader on the 35-index draw that erases the menu background. Measured,
  // not acted on: "unchanged" may also be the guest's intent.
  uint32_t color_assigned_mask = 0;
  bool writes_position = false;
  bool writes_depth = false;
  uint32_t max_const_index = 0;
  uint64_t const_mask[4] = {};   // which xe_c[] slots this shader reads
  bool const_relative = false;   // reads xe_c[] through a0; mask is saturated
  bool reads_constants = false;
  // setp_* instructions -- the ops that WRITE p0. The name is historical and the
  // field is deliberately not renamed; see HlslShader::unhonoured_predicate_ops.
  // Since per-instruction predication landed, p0 written here IS obeyed: by
  // every following ALU instruction carrying the `(p0)` bit, by predicated
  // fetches, and in the vertex stage by cond_exec_pred as well.
  uint32_t unhonoured_predicate_ops = 0;
  // ALU instructions carrying their own predicate bits, now emitted inside
  // `if (xe_p0 == ...)`. Seen and obeyed are the same number here -- unlike the
  // exec-level blocks, there is no stage this is refused for.
  uint32_t predicated_alu_ops = 0;
  // Texture fetches carrying predicate bits, HONOURED as of 2026-08-26 by
  // gating the destination write rather than the sample. See the long note at
  // EmitFetchDestination for why that is legal here and not for exec blocks.
  uint32_t predicated_fetches = 0;
  // Fetch opcodes at 16 and above that this emitter skips. See
  // HlslShader::unhonoured_fetch_ops for why they are skipped and not refused.
  uint32_t unhonoured_fetch_ops = 0;
  // Does any instruction touch the register LOD -- setTexLOD writing it, or a
  // tfetch reading it back? Gates the `xe_lod` declaration so that every shader
  // which does not use one emits byte-identical HLSL to before.
  bool uses_reg_lod = false;
  std::vector<PixelTextureBinding> fetches;
  uint32_t sampler_mask = 0;
  // Guest sampler -> compact register slot, assigned in order of first fetch.
  // See the note on HlslShader::sampler_slot_guest for why the guest index
  // cannot be the register number.
  uint32_t sampler_count = 0;
  uint32_t slot_guest[HlslShader::kMaxSamplerSlots] = {};
  // Slots whose fetches are cube, and so are declared Texture2DArray rather
  // than Texture2D. One bit per COMPACT slot, mirrored into
  // HlslShader::sampler_array_mask for the binder, which must create a matching
  // SRV dimension or the descriptor contradicts the declaration.
  uint32_t slot_array_mask = 0;
  // Slots that have been fetched at all, so a second fetch at a different
  // dimension can be told apart from the first fetch of a slot.
  uint32_t slot_fetched_mask = 0;
  // kCube needs scratch that does not fold into an expression. Declared only
  // when used, so the common shader carries none.
  bool uses_cube = false;
  // A cube FETCH in a shader with no cube ALU op. The (S, T, face) form this
  // emitter assumes is produced by that op; without it, something else computed
  // the fetch coordinate and the assumption is unverified. Reported rather than
  // refused -- it is the hardware's own operand form either way.
  bool cube_fetch_without_cube_op = false;
  // Fetches carrying a non-zero offset_z, which has no host equivalent while a
  // stacked texture's slice axis is an array index. Counted so that "nothing
  // uses it" remains something measured rather than assumed.
  uint32_t fetch_offset_z_dropped = 0;
  // Vertex fetches emitted into the body, in program order. The ordinal is the
  // index into xe_vf[] the host must fill, and it matches the order
  // DecodeVertexShaderFetches pushes attributes in -- both walk the same stream
  // the same way, so the host can pair them positionally.
  uint32_t vertex_fetch_count = 0;
  std::string body;

  // Returns the compact slot for a guest sampler, allocating one on first use.
  // Sets status and returns 0 when the shader needs more distinct samplers than
  // a descriptor table is sized for.
  uint32_t SamplerSlot(uint32_t guest) {
    for (uint32_t i = 0; i < sampler_count; ++i)
      if (slot_guest[i] == guest) return i;
    if (sampler_count >= HlslShader::kMaxSamplerSlots) {
      status = HlslStatus::kTooManySamplers;
      return 0;
    }
    slot_guest[sampler_count] = guest;
    return sampler_count++;
  }

  bool pixel() const { return stage_ == HlslStage::kPixel; }

  // How this stage is allowed to spell a texture fetch. `Sample` needs implicit
  // derivatives, and only a pixel shader has them -- FXC answers it in vs_5_0
  // with "X4532: cannot map expression to vs_5_0 instruction set". SampleLevel
  // at an explicit LOD of 0 is the vertex-stage form; the Xenos fetch carries no
  // gradient either way. A VERTEX shader that samples is a real shape in this
  // game: the engine binds the bone-matrix palette as a texture.
  //
  // A fetch that names its LOD in a register (`use_reg_lod`, written by a
  // preceding setTexLOD) needs SampleLevel in EITHER stage: the point of the
  // instruction is that the shader chooses the level.
  bool ExplicitLod(const uc::TextureFetchInstruction& tf) const {
    return tf.use_register_lod() || !pixel();
  }
  // kBaseMap means the shader never minifies past level 0, so there is no level
  // to bias between. The reference skips the whole bias computation on it
  // (dxbc_shader_translator_fetch.cc:1568) and so does this.
  bool BiasedLod(const uc::TextureFetchInstruction& tf) const {
    return !(tf.has_mip_filter() &&
             tf.mip_filter() == rex::graphics::xenos::TextureFilter::kBaseMap);
  }
  const char* SampleOp(const uc::TextureFetchInstruction& tf) const {
    if (ExplicitLod(tf)) return ".SampleLevel(";
    // SampleBias rather than Sample so the guest's per-texture LOD bias can be
    // applied. With a bias of 0.0 -- which is what all but a handful of this
    // title's textures carry -- it is the same sample.
    return BiasedLod(tf) ? ".SampleBias(" : ".Sample(";
  }
  // Paired with SampleOp: the extra argument SampleLevel takes and Sample does
  // not, emitted immediately before the closing paren of the fetch.
  //
  // The level is the register value plus the instruction's own bias, in the
  // D3D11.3 accumulation order the reference uses
  // (dxbc_shader_translator_fetch.cc:1541). lod_bias is a 7-bit signed field
  // scaled by 1/16, so std::to_string reproduces it exactly.
  //
  // ConstantLodBias is the third term: the FETCH CONSTANT's bias, per slot, in
  // LOD units, arriving in the spare .w of xe_texinv. It was missing on the
  // grounds that "no fetch constant reaches this emitter", and it cost the
  // terrain -- the virtual-texture page tables carry +7.0, the guest populates
  // only the levels that bias makes it read, and without it every lookup landed
  // seven levels too fine and missed.
  std::string ConstantLodBias(uint32_t slot) const {
    return "xe_texinv[" + std::to_string(slot) + "].w";
  }
  std::string SampleLod(const uc::TextureFetchInstruction& tf, uint32_t slot) {
    // The D3D11.3 accumulation order the reference uses: specified LOD, then
    // the sampler (fetch constant) bias, then the instruction bias.
    const float instr_bias = tf.lod_bias();
    if (!ExplicitLod(tf)) {
      // Implicit derivatives: the whole bias is SampleBias's argument.
      if (!BiasedLod(tf)) return "";
      std::string b = ConstantLodBias(slot);
      if (instr_bias != 0.0f) b += " + " + std::to_string(instr_bias);
      return ", (" + b + ")";
    }
    std::string base = "0";
    if (tf.use_register_lod()) {
      uses_reg_lod = true;
      base = "xe_lod";
    }
    if (!BiasedLod(tf)) {
      if (instr_bias == 0.0f) return ", " + base;
      return ", (" + base + " + " + std::to_string(instr_bias) + ")";
    }
    std::string expr = base + " + " + ConstantLodBias(slot);
    if (instr_bias != 0.0f) expr += " + " + std::to_string(instr_bias);
    return ", (" + expr + ")";
  }

  // setTexLOD: store the level this shader wants its later fetches to sample at.
  // The operand is one component, selected by the source swizzle's first slot --
  // the same absolute two-bits-per-component encoding a tfetch coordinate uses,
  // not the component-relative ALU form (dxbc_shader_translator_fetch.cc:608).
  //
  // getGradients: the screen-space derivatives of a 2-component coordinate.
  //
  // THIS WAS THE VIRTUAL-TEXTURE FEEDBACK PASS. It sat in the not-implemented
  // bucket, its one caller dismissed as "compiles fine without it". It compiles,
  // and it computes a constant. ps_hft_fback is the terrain's VT feedback
  // shader, and its whole job is
  //
  //     getGradients r1, r0.wz, tf8
  //     mul r1, r1.zxyw, c196.yxxy      scale by the feedback/screen ratio
  //     mul r1, r1.xywz, r1.xywz        square
  //     add r0.__zw, r1.yyyx, r1.wwwz   two squared lengths
  //     sqrt / max / log                -> the LOD it is asking for
  //
  // With the op skipped, r1 kept the INTERPOLATOR it happened to hold, whose zw
  // are the constant 2048, so every pixel requested LOD 8, the guest populated
  // only mips 6-8 of its page table, and the composite read the not-available
  // sentinel everywhere -- the terrain banding.
  //
  // LAYOUT IS THE REFERENCE'S, NOT INFERRED (ucode.h:634 -- "Source is
  // 2-component. XZ = ddx(source.xy), YW = ddy(source.xy)"). Reading the guest's
  // own use of the result suggests xy=ddx / zw=ddy instead, and that reading is
  // self-consistent too, which is why it is not evidence.
  //
  // ddx/ddy rather than the _coarse forms: the same header notes the texture
  // unit computes this per quad and that FXC lowers ddx/ddy to _coarse at SM5.
  // Pixel stage only -- HLSL rejects a derivative in a vertex shader.
  void EmitGetGradients(const uc::TextureFetchInstruction& tf) {
    if (!pixel()) {
      ++unhonoured_fetch_ops;
      return;
    }
    // The fetch source swizzle is ABSOLUTE, two bits per component, the same
    // encoding a tfetch coordinate uses -- not the component-relative ALU form.
    const uint32_t swiz = tf.src_swizzle();
    const std::string src = Temp(tf.src());
    std::string uv;
    uv += kComponent[swiz & 3];
    uv += kComponent[(swiz >> 2) & 3];
    Line("xe_v.xz = ddx(" + src + "." + uv + ");");
    Line("xe_v.yw = ddy(" + src + "." + uv + ");");
    // Through the same destination path every fetch uses, so the dest swizzle's
    // keep/0/1 encodings behave identically here.
    EmitFetchDestination(tf);
  }

  void EmitSetTextureLod(const uc::TextureFetchInstruction& tf) {
    uses_reg_lod = true;
    Line("xe_lod = " + Temp(tf.src()) + "." +
         std::string(1, kComponent[tf.src_swizzle() & 3]) + ";");
  }

  void Line(const std::string& s) { body += "  " + s + "\n"; }

  // A temp register read. Recording it as an input the first time it is read
  // before being written is what produces input_mask — the interpolator set a
  // pixel shader needs, and the vfetch destination set a vertex shader needs.
  std::string Temp(uint32_t reg) {
    const uint32_t r = reg & (kNumTemps - 1);
    if (!(written_mask & (1u << (r & 31))) && r < 32) input_mask |= 1u << r;
    return "r[" + std::to_string(r) + "]";
  }
  void MarkWritten(uint32_t reg) {
    const uint32_t r = reg & (kNumTemps - 1);
    if (r < 32) written_mask |= 1u << r;
  }

  // A constant read, in the shader's OWN stage bank. The vertex bank is ALU
  // constants 0-255 at device+0x780 and the pixel bank is 256-511 at
  // device+0x1780 -- proven from D3DDevice_DrawVertices' own flush, which passes
  // Xenos register base 0x4000 for the first and 0x4400 for the second. The
  // shader indexes its bank from 0 either way.
  std::string Const(uint32_t index) {
    reads_constants = true;
    if (index > max_const_index) max_const_index = index;
    const_mask[(index & 255u) >> 6] |= 1ull << (index & 63u);
    return "xe_c[" + std::to_string(index) + "]";
  }
  std::string ConstRelative(uint32_t index) {
    reads_constants = true;
    max_const_index = 255;  // any slot is reachable once a0 is involved
    // Every slot really is reachable, so the mask says so rather than naming
    // the one literal index -- a mask that under-reports is worse than none.
    // But an all-ones mask cannot IDENTIFY anything, so the fact that it is
    // all-ones by saturation rather than by real use is recorded separately.
    const_relative = true;
    for (auto& w : const_mask) w = ~0ull;
    return "xe_c[(" + std::to_string(index) + " + xe_a0) & 255]";
  }

  // One source operand as a float4 expression. Swizzles are component-relative
  // on Xenos — component c reads ((swizzle >> 2c) + c) & 3 — which is why a
  // swizzle of 0 is identity rather than xxxx.
  std::string Src(const uc::AluInstruction& alu, size_t i) {
    const uint32_t reg = alu.src_reg(i);
    const uint32_t swiz = alu.src_swizzle(i);
    std::string base;
    bool absolute = false;

    if (alu.src_is_temp(i)) {
      // A relative temp index is aL-relative, never a0-relative: the register
      // file has no a0 addressing mode. Modelling a0 does not help, so this
      // stays refused, exactly as in the interpreter.
      if (uc::AluInstruction::is_src_temp_relative(reg)) {
        status = HlslStatus::kLoopRelative;
        return "float4(0,0,0,0)";
      }
      base = Temp(uc::AluInstruction::src_temp_reg(reg));
      absolute = uc::AluInstruction::is_src_temp_value_absolute(reg);
    } else {
      const uint32_t index = reg & 0xFF;
      if (alu.src_const_is_addressed(i)) {
        // kAbsolute = c[a0 + n], kRelative = c[aL + n] (ucode.h:191-199). aL is
        // the loop counter and we do not unroll, so there is no honest value for
        // it: refused, not guessed.
        if (!alu.is_const_address_register_relative()) {
          status = HlslStatus::kLoopRelative;
          return "float4(0,0,0,0)";
        }
        base = ConstRelative(index);
      } else {
        base = Const(index);
      }
      absolute = alu.abs_constants();
    }

    std::string sw = ".";
    for (uint32_t c = 0; c < 4; ++c)
      sw += kComponent[uc::AluInstruction::GetSwizzledComponentIndex(swiz, c)];
    std::string e = base + sw;
    if (absolute) e = "abs(" + e + ")";
    if (alu.src_negate(i)) e = "-(" + e + ")";
    return e;
  }

  // Vector opcodes reading src1 only. src2 must not be emitted for these — not
  // for correctness of the result, which discards it, but because emitting it
  // would record a constant read the shader does not perform and corrupt
  // max_const_index.
  static bool VectorOpUsesSrc2(uc::AluVectorOpcode op) {
    using Op = uc::AluVectorOpcode;
    switch (op) {
      case Op::kFrc:
      case Op::kTrunc:
      case Op::kFloor:
      case Op::kMax4:
        return false;
      default:
        return true;
    }
  }

  std::string VectorOp(const uc::AluInstruction& alu, bool need_result) {
    using Op = uc::AluVectorOpcode;
    const Op op = alu.vector_opcode();
    const std::string a = Src(alu, 1);
    const std::string b =
        VectorOpUsesSrc2(op) ? Src(alu, 2) : std::string("float4(0,0,0,0)");
    std::string r;
    switch (op) {
      case Op::kAdd: r = "(" + a + " + " + b + ")"; break;
      case Op::kMul: r = "XeMul(" + a + ", " + b + ")"; break;
      case Op::kMax: r = "XeMax(" + a + ", " + b + ")"; break;
      case Op::kMin: r = "XeMin(" + a + ", " + b + ")"; break;
      case Op::kSeq: r = "float4(" + a + " == " + b + ")"; break;
      case Op::kSgt: r = "float4(" + a + " > " + b + ")"; break;
      case Op::kSge: r = "float4(" + a + " >= " + b + ")"; break;
      case Op::kSne: r = "float4(" + a + " != " + b + ")"; break;
      case Op::kFrc: r = "frac(" + a + ")"; break;
      case Op::kTrunc: r = "trunc(" + a + ")"; break;
      case Op::kFloor: r = "floor(" + a + ")"; break;
      case Op::kMaxA:
        // max, plus the address-register side effect: a0 = clamp(floor(w+0.5)).
        // The clamp is the hardware's signed 9-bit range.
        Line("xe_a0 = (int)clamp(floor((" + a + ").w + 0.5), -256.0, 255.0);");
        r = "XeMax(" + a + ", " + b + ")";
        break;
      case Op::kMad:
        r = "(XeMul(" + a + ", " + b + ") + " + Src(alu, 3) + ")";
        break;
      // A true select. This was a lerp, on the reasoning that a selector of
      // exactly 0 or 1 makes lerp exact -- which holds only for finite operands.
      // FXC expands lerp(x, y, s) to x + s * (y - x), so the UNSELECTED operand
      // stays in the arithmetic: with s = 0 and y = +INF that is 0 * INF = NaN,
      // and kRcp legitimately produces +INF for 1/0.
      //
      // Both Xenia backends select and so does our own interpreter, so the lerp
      // was also an emitter/interpreter split. HLSL's ?: on vectors is
      // component-wise and FXC compiles it to a single movc.
      case Op::kCndEq:
        r = "(" + a + " == 0.0 ? " + b + " : " + Src(alu, 3) + ")";
        break;
      case Op::kCndGe:
        r = "(" + a + " >= 0.0 ? " + b + " : " + Src(alu, 3) + ")";
        break;
      case Op::kCndGt:
        r = "(" + a + " > 0.0 ? " + b + " : " + Src(alu, 3) + ")";
        break;
      case Op::kDp4: r = "XeDot4(" + a + ", " + b + ").xxxx"; break;
      case Op::kDp3: r = "XeDot3((" + a + ").xyz, (" + b + ").xyz).xxxx"; break;
      case Op::kDp2Add:
        r = "(XeDot2((" + a + ").xy, (" + b + ").xy) + (" + Src(alu, 3) +
            ").x).xxxx";
        break;
      case Op::kMax4: {
        r = "XeMax(XeMax((" + a + ").x, (" + a + ").y), XeMax((" + a + ").z, (" + a +
            ").w)).xxxx";
        break;
      }
      case Op::kDst:
        r = "float4(1.0, XeMul((" + a + ").y, (" + b + ").y), (" + a + ").z, (" +
            b + ").w)";
        break;
      // Cube map coordinate generation. Transcribed from the hardware definition
      // in ucode.h (kCube), not approximated:
      //
      //   dest.x = T, dest.y = S, dest.z = 2 * major axis, dest.w = FaceID
      //
      // The direction vector is NOT src0 verbatim. The instruction expects the
      // shader to have swizzled src0 to .zzxy (and src1 to .yxzz), so after our
      // Src() applies that swizzle the components land as
      //
      //   z = src0.x   x = src0.z   y = src0.w
      //
      // which is where the indices below come from. src1 carries the same three
      // values in a different order and is redundant once src0 is unpacked; it
      // is still evaluated, because the hardware reads it and because an operand
      // skipped here would corrupt the constant-read accounting.
      //
      // Emitted as statements rather than one expression, so the three-way major
      // axis selection can be checked against the hardware doc line by line.
      case Op::kCube: {
        uses_cube = true;
        Line("xe_cube = float3((" + a + ").z, (" + a + ").w, (" + a + ").x);");
        Line("xe_cube_a = abs(xe_cube);");
        Line("if (xe_cube_a.z >= xe_cube_a.x && xe_cube_a.z >= xe_cube_a.y) {");
        Line("  xe_cube_r = float4(-xe_cube.y,");
        Line("                     xe_cube.z < 0.0 ? -xe_cube.x : xe_cube.x,");
        Line("                     2.0 * xe_cube.z,");
        Line("                     xe_cube.z < 0.0 ? 5.0 : 4.0);");
        Line("} else if (xe_cube_a.y >= xe_cube_a.x) {");
        Line("  xe_cube_r = float4(xe_cube.y < 0.0 ? -xe_cube.z : xe_cube.z,");
        Line("                     xe_cube.x,");
        Line("                     2.0 * xe_cube.y,");
        Line("                     xe_cube.y < 0.0 ? 3.0 : 2.0);");
        Line("} else {");
        Line("  xe_cube_r = float4(-xe_cube.y,");
        Line("                     xe_cube.x < 0.0 ? xe_cube.z : -xe_cube.z,");
        Line("                     2.0 * xe_cube.x,");
        Line("                     xe_cube.x < 0.0 ? 1.0 : 0.0);");
        Line("}");
        r = "xe_cube_r";
        break;
      }
      // The kill family is a pixel-shader operation and has no vertex meaning.
      // Emitting discard in a vertex shader would not compile, so it is refused
      // there rather than silently dropped.
      case Op::kKillEq:
      case Op::kKillGt:
      case Op::kKillGe:
      case Op::kKillNe: {
        if (!pixel()) {
          status = HlslStatus::kUnsupportedVectorOp;
          blocking_opcode = uint32_t(op);
          return "float4(0,0,0,0)";
        }
        const char* cmp = op == Op::kKillEq   ? "=="
                          : op == Op::kKillGt ? ">"
                          : op == Op::kKillGe ? ">="
                                              : "!=";
        Line("if (any(" + a + " " + std::string(cmp) + " " + b +
             ")) { discard; }");
        r = "float4(any(" + a + " " + std::string(cmp) + " " + b +
            ") ? 1.0 : 0.0)";
        break;
      }
      // The setp_*_push family, opcodes 20..23. The comment that used to sit in
      // `default` said these "have not appeared in a shader captured here". One
      // pixel shader is the ONLY untranslated one in a run, refused for exactly
      // this opcode, and aimed at the 1280x720 scene target 1785 times.
      //
      // Transcribed from ucode.h (kSetpEqPush..kSetpGePush):
      //
      //   p0   = (src0.w == 0.0 && src1.w CMP 0.0)
      //   dest = (src0.x == 0.0 && src1.x CMP 0.0) ? 0.0 : src0.x + 1.0
      //
      // CMP is ==, !=, > or >= per opcode. Note the asymmetry: the src0 side is
      // ALWAYS "== 0.0" and only the src1 comparison varies -- these count a
      // predicate chain rather than compare two operands.
      case Op::kSetpEqPush:
      case Op::kSetpNePush:
      case Op::kSetpGtPush:
      case Op::kSetpGePush: {
        const char* cmp = op == Op::kSetpEqPush   ? "=="
                          : op == Op::kSetpNePush ? "!="
                          : op == Op::kSetpGtPush ? ">"
                                                  : ">=";
        const std::string c = std::string(" ") + cmp + " 0.0";
        // The predicate write is emitted as a statement, and FIRST, because it
        // is the side effect: it has to survive an empty write mask. The SDK's
        // operand table agrees, still reading .w for these when the result is
        // unused (`components = used_result_components ? 0b1001 : 0b1000`).
        Line("xe_p0 = (((" + a + ").w == 0.0) && ((" + b + ").w" + c + "));");
        ++unhonoured_predicate_ops;
        r = "(((" + a + ").x == 0.0) && ((" + b + ").x" + c + ") ? 0.0 : (" + a +
            ").x + 1.0).xxxx";
        break;
      }
      default:
        // Reported by opcode, not guessed. (kCube and the setp_*_push family
        // used to be listed here; both are implemented above.)
        status = HlslStatus::kUnsupportedVectorOp;
        blocking_opcode = uint32_t(op);
        return "float4(0,0,0,0)";
    }
    if (!need_result) return r;
    if (alu.vector_clamp()) r = "saturate(" + r + ")";
    return r;
  }

  // The mulsc/addsc/subsc family, opcodes 42..47. These do not use the normal
  // operand encoding: src3 names a constant register directly and the temp
  // register is scattered, with one bit in the opcode field itself -- which is
  // why each operation has a _0 and a _1 form. The constant is selected with the
  // W-relative src3 swizzle and the temporary with the X-relative one (the Xenos
  // AB = WX scalar convention). abs_constants and src3 negate apply to both.
  bool ConstRegScalarOp(const uc::AluInstruction& alu, std::string& out) {
    using Op = uc::AluScalarOpcode;
    const Op op = alu.scalar_opcode();
    if (op < Op::kMulsc0 || op > Op::kSubsc1) return false;

    const uint32_t swizzle = alu.src_swizzle(3);
    const uint32_t const_comp =
        uc::AluInstruction::GetSwizzledComponentIndex(swizzle, 3);
    const uint32_t temp_comp =
        uc::AluInstruction::GetSwizzledComponentIndex(swizzle, 0);
    std::string a =
        Const(alu.src_reg(3) & 0xFF) + "." + kComponent[const_comp];
    if (alu.abs_constants()) a = "abs(" + a + ")";
    if (alu.src_negate(3)) a = "-(" + a + ")";
    std::string b = Temp(alu.scalar_const_reg_op_src_temp_reg()) + "." +
                    kComponent[temp_comp];
    if (alu.abs_constants()) b = "abs(" + b + ")";
    if (alu.src_negate(3)) b = "-(" + b + ")";

    switch (op) {
      case Op::kMulsc0: case Op::kMulsc1: out = "XeMul(" + a + ", " + b + ")"; break;
      case Op::kAddsc0: case Op::kAddsc1: out = "(" + a + " + " + b + ")"; break;
      default:                            out = "(" + a + " - " + b + ")"; break;
    }
    return true;
  }

  std::string ScalarOp(const uc::AluInstruction& alu) {
    using Op = uc::AluScalarOpcode;
    const Op op = alu.scalar_opcode();

    if (std::string cr; ConstRegScalarOp(alu, cr)) {
      if (alu.scalar_clamp()) cr = "saturate(" + cr + ")";
      return cr;
    }

    const std::string s = "(" + Src(alu, 3) + ")";
    // Xenos scalar operands use AB = WX, not XY. Src() has already applied
    // the component-relative swizzle, so the W-relative left operand is .w
    // and the X-relative right operand is .x. One-component scalar opcodes
    // consume only a; the two-component forms consume both a and b.
    const std::string a = s + ".w", b = s + ".x";
    std::string r;
    switch (op) {
      case Op::kAdds: r = "(" + a + " + " + b + ")"; break;
      case Op::kMuls: r = "XeMul(" + a + ", " + b + ")"; break;
      case Op::kSubs: r = "(" + a + " - " + b + ")"; break;
      case Op::kMaxs: r = "XeMax(" + a + ", " + b + ")"; break;
      case Op::kMins: r = "XeMin(" + a + ", " + b + ")"; break;
      case Op::kSeqs: r = "float(" + a + " == 0.0)"; break;
      case Op::kSgts: r = "float(" + a + " > 0.0)"; break;
      case Op::kSges: r = "float(" + a + " >= 0.0)"; break;
      case Op::kSnes: r = "float(" + a + " != 0.0)"; break;
      case Op::kAddsPrev: r = "(" + a + " + xe_ps)"; break;
      case Op::kMulsPrev: r = "XeMul(" + a + ", xe_ps)"; break;
      // The LIT-emulation form (ucode.h:kMulsPrev2): guards the specular term so
      // a non-positive or non-finite exponent collapses to -FLT_MAX rather than
      // producing a NaN. Not seen in this title's shaders, but it is the last
      // scalar opcode without a handler and `default:` refuses the whole shader.
      case Op::kMulsPrev2:
        r = "((xe_ps == -3.402823466e+38 || !isfinite(xe_ps) || !isfinite(" +
            b + ") || " + b + " <= 0.0) ? -3.402823466e+38 : XeMul(" + a +
            ", xe_ps))";
        break;
      case Op::kSubsPrev: r = "(" + a + " - xe_ps)"; break;
      case Op::kFrcs: r = "frac(" + a + ")"; break;
      case Op::kTruncs: r = "trunc(" + a + ")"; break;
      case Op::kFloors: r = "floor(" + a + ")"; break;
      case Op::kExp: r = "exp2(" + a + ")"; break;
      // No abs() on the operand. Direct3D 9 defines ITS log/rsq as operating on
      // |src|, but Xenos does not: the operand carries an abs MODIFIER, which
      // Src() already honours, and the Xbox 360 compiler sets that bit itself
      // wherever the D3D9 semantic calls for it. All 115 rsq sites across the 150
      // dumped shaders arrive with it already set, so a hardcoded abs was a
      // second unconditional application -- and it masked the three log sites
      // that deliberately have it clear.
      //
      // logc saturates -INF to -FLT_MAX and ONLY -INF: log2(+INF) stays +INF on
      // the hardware and a NaN stays NaN. The max() folded both.
      case Op::kLog: r = "log2(" + a + ")"; break;
      case Op::kLogc: r = "XeClampNegInf(log2(" + a + "))"; break;
      // The three reciprocal forms differ ONLY on what they do with an infinity,
      // and that difference is the whole of this game's black main menu. From
      // the SDK (ucode.h:1082):
      //
      //   RECIP_IEEE  (kRcp)   1/0 = +INF
      //   RECIP_CLAMP (kRcpc)  +INF -> +FLT_MAX, -INF -> -FLT_MAX
      //   RECIP_FF    (kRcpf)  +INF ->  0.0,     -INF -> -0.0
      //
      // kRcpf was emitted as a plain rcp, so it produced +Inf where the console
      // produces zero: the rider's material takes a texture's Rec.709 luminance
      // and divides by it, the luminance is 0, and the next instruction computed
      // 0 * Inf = NaN, which poisoned the composite. "Fast-forward" is the
      // fixed-function-emulation form and exists precisely so a shader can
      // divide by a possibly-zero quantity without guarding it.
      case Op::kRcp: r = "rcp(" + a + ")"; break;
      case Op::kRcpf: r = "XeFlushInf(rcp(" + a + "))"; break;
      case Op::kRcpc: r = "XeClampInf(rcp(" + a + "))"; break;
      case Op::kRsq: r = "rsqrt(" + a + ")"; break;
      case Op::kRsqf: r = "XeFlushInf(rsqrt(" + a + "))"; break;
      case Op::kRsqc: r = "XeClampInf(rsqrt(" + a + "))"; break;
      case Op::kSqrt: r = "sqrt(" + a + ")"; break;
      case Op::kSin: r = "sin(" + a + ")"; break;
      case Op::kCos: r = "cos(" + a + ")"; break;
      case Op::kMaxAs:
        // The address source and left maximum operand are both scalar a.
        Line("xe_a0 = (int)clamp(floor(" + a + " + 0.5), -256.0, 255.0);");
        r = "XeMax(" + a + ", " + b + ")";
        break;
      case Op::kMaxAsf:
        // The floor variant: truncates toward negative infinity, same clamp.
        Line("xe_a0 = (int)clamp(floor(" + a + "), -256.0, 255.0);");
        r = "XeMax(" + a + ", " + b + ")";
        break;
      case Op::kRetainPrev: r = "xe_ps"; break;
      // The predicate-set family. BOTH halves matter and both are honoured.
      //
      // p0 is WRITTEN here and READ downstream: a following ALU instruction
      // carrying the `(p0)` bit is emitted inside `if (xe_p0 == ...)` by
      // EmitAlu's PredicateBlock, a predicated fetch gates its destination
      // write, and in the vertex stage cond_exec_pred gates a whole block. The
      // second half is the VALUE, which lands in ps for a following *_prev to
      // read.
      //
      // Note the polarity, from the SDK (ucode.h:1140): the predicate being TRUE
      // writes 0.0 to the destination, not 1.0.
      case Op::kSetpEq: case Op::kSetpNe: case Op::kSetpGt: case Op::kSetpGe: {
        const char* cmp = op == Op::kSetpEq   ? "=="
                          : op == Op::kSetpNe ? "!="
                          : op == Op::kSetpGt ? ">"
                                              : ">=";
        Line("xe_p0 = (" + a + " " + std::string(cmp) + " 0.0);");
        r = "float(!(" + a + " " + std::string(cmp) + " 0.0))";
        ++unhonoured_predicate_ops;
        break;
      }
      case Op::kSetpInv:
        Line("xe_p0 = (" + a + " == 1.0);");
        r = "(" + a + " == 1.0 ? 0.0 : (" + a + " == 0.0 ? 1.0 : " + a + "))";
        ++unhonoured_predicate_ops;
        break;
      case Op::kSetpPop:
        Line("xe_p0 = ((" + a + " - 1.0) <= 0.0);");
        // A select, matching Xenia's OpGE + OpMovC, so this reads the way the
        // interpreter does. It does NOT change the compiled DXBC: FXC folds
        // `(x <= 0) ? 0 : x` straight back to `max(x, 0)`. The NaN divergence
        // therefore survives here -- and is unobservable, because setp_pop
        // appears in 0 of this title's 150 shaders.
        r = "((" + a + " - 1.0) <= 0.0 ? 0.0 : " + a + " - 1.0)";
        ++unhonoured_predicate_ops;
        break;
      case Op::kSetpClr:
        Line("xe_p0 = false;");
        r = "3.402823466e+38";
        ++unhonoured_predicate_ops;
        break;
      case Op::kSetpRstr:
        Line("xe_p0 = (" + a + " == 0.0);");
        r = a;
        ++unhonoured_predicate_ops;
        break;
      case Op::kKillsEq: case Op::kKillsGt: case Op::kKillsGe:
      case Op::kKillsNe: case Op::kKillsOne: {
        if (!pixel()) {
          status = HlslStatus::kUnsupportedScalarOp;
          blocking_opcode = uint32_t(op);
          return "0.0";
        }
        const char* cmp = op == Op::kKillsEq   ? "=="
                          : op == Op::kKillsGt ? ">"
                          : op == Op::kKillsGe ? ">="
                          : op == Op::kKillsNe ? "!="
                                               : "==";
        const char* rhs = op == Op::kKillsOne ? "1.0" : "0.0";
        Line("if (" + a + " " + std::string(cmp) + " " + rhs +
             ") { discard; }");
        r = "float(" + a + " " + std::string(cmp) + " " + rhs + ")";
        break;
      }
      default:
        status = HlslStatus::kUnsupportedScalarOp;
        blocking_opcode = uint32_t(op);
        return "0.0";
    }
    if (alu.scalar_clamp()) r = "saturate(" + r + ")";
    return r;
  }

  // Opens `if (xe_p0 == ...) {` and closes it however EmitAlu returns -- and it
  // returns from a dozen places, several of them error paths. A brace left
  // unclosed produces HLSL that fails to compile and drops the draw to a
  // stand-in, so this is scoped rather than closed by hand at each exit.
  struct PredicateBlock {
    Emitter* em = nullptr;
    PredicateBlock(Emitter* e, bool predicated, bool condition,
                   uint32_t* counter) {
      if (!predicated) return;
      em = e;
      ++*counter;
      em->Line(std::string("if (xe_p0 == ") + (condition ? "true" : "false") +
               ") {");
    }
    ~PredicateBlock() {
      if (em) em->Line("}");
    }
    PredicateBlock(const PredicateBlock&) = delete;
    PredicateBlock& operator=(const PredicateBlock&) = delete;
  };

  void EmitAlu(const uc::AluInstruction& alu) {
    // PER-INSTRUCTION PREDICATION. Separate from the exec-level cond_exec_pred
    // handled in the CF walk: every ALU instruction carries its own
    // is_predicated/pred_condition pair, so
    //
    //     17  setp_ne_push r0.x, r0.xxxx, r0.yyyy
    //     18  (p0) sgts r2.w, -|r0|.x
    //     21  (p0) mul  r2.w, r2.wwww, r0.xxxx
    //
    // runs 18 and 21 only where p0 holds, and elsewhere r2.w keeps the alpha
    // instruction 10 put there. Running them regardless makes alpha the constant
    // 0 -- `-|x| > 0` is never true -- so the intro logo's quad rasterised
    // correctly, output white, and blended to nothing.
    //
    // Emitted as real flow control, which the exec-level path deliberately
    // refuses in the pixel stage. That refusal does not apply here: an ALU
    // instruction never samples, so nothing inside this block can want a
    // gradient. Predicated FETCHES are counted, not honoured.
    //
    // Skipping the whole instruction, rather than selecting over its result, is
    // also what gets the side effects right: a0, p0, ps and the kill discard are
    // suppressed exactly where the console suppressed them.
    PredicateBlock pred(this, alu.is_predicated(),
                        alu.predicate_condition(), &predicated_alu_ops);

    // The raw vector/scalar mask fields overlap with the export constant 0/1
    // encoding, so the canonical decoded masks are used for both temp and
    // export writes. Treating the raw bits as ordinary masks preserved position
    // in many shaders but turned interpolator exports into zero.
    const uint32_t vmask = alu.GetVectorOpResultWriteMask();
    const uint32_t smask = alu.GetScalarOpResultWriteMask();
    const bool is_export = alu.is_export();

    // maxa/maxas/maxasf exist to load the address register, and a shader that
    // wants only that side effect leaves the write mask empty. A mask test is
    // therefore the wrong gate: it skipped the instruction outright and with it
    // the a0 assignment, leaving xe_a0 at 0 for the whole invocation.
    //
    // That is what un-posed the rider. Four-influence skinning is
    //   mova a0, index.z ; r = weight.z * c[85+a0]
    //   mova a0, index.y ; r += weight.y * c[85+a0]   ... and so on
    // so with a0 pinned at 0 all four influences read the SAME matrix.
    //
    // The SDK tags exactly these with kAluOpChangedStateAddressRegister, but its
    // opcode-info tables are declared extern in the header and defined in a
    // translation unit the runtime does not ship, so naming the opcodes is the
    // only way to ask the question and still link. Keep this list in step with
    // that flag: an a0-setting opcode missing here is silently dropped.
    const bool vector_sets_a0 =
        alu.vector_opcode() == uc::AluVectorOpcode::kMaxA;
    const bool scalar_sets_a0 =
        alu.scalar_opcode() == uc::AluScalarOpcode::kMaxAs ||
        alu.scalar_opcode() == uc::AluScalarOpcode::kMaxAsf;

    // a0 is NOT the only side effect that outlives an empty write mask. The SDK's
    // operand table states the other two outright, by keeping operand components
    // live when the result is unused:
    //
    //   setp_*_push  components = used_result_components ? 0b1001 : 0b1000
    //                             -- .w still read with no result: it feeds p0
    //   kill_*       components = 0b1111
    //                             -- unconditional: the side effect is discard
    const bool vector_sets_p0 =
        alu.vector_opcode() == uc::AluVectorOpcode::kSetpEqPush ||
        alu.vector_opcode() == uc::AluVectorOpcode::kSetpNePush ||
        alu.vector_opcode() == uc::AluVectorOpcode::kSetpGtPush ||
        alu.vector_opcode() == uc::AluVectorOpcode::kSetpGePush;
    const bool vector_discards =
        alu.vector_opcode() == uc::AluVectorOpcode::kKillEq ||
        alu.vector_opcode() == uc::AluVectorOpcode::kKillGt ||
        alu.vector_opcode() == uc::AluVectorOpcode::kKillGe ||
        alu.vector_opcode() == uc::AluVectorOpcode::kKillNe;

    // VectorOp still runs when vmask is 0 and this is an export, because
    // kMaxA's address-register side effect depends on it; nothing consumes the
    // value in that case. Same for the p0 and discard families beside it.
    const bool has_vector = vmask != 0 || is_export || vector_sets_a0 ||
                            vector_sets_p0 || vector_discards;

    std::string vexpr, sexpr;
    if (has_vector) vexpr = VectorOp(alu, vmask != 0);
    if (status != HlslStatus::kOk) return;
    // The scalar half ALWAYS runs. a0 was the first side effect found to survive
    // an empty write mask, but it is not the only one and gating on the mask was
    // never the right shape: `ps` is written by every scalar issue, and `p0` and
    // the kill discard likewise. See the xe_ps note below.
    sexpr = ScalarOp(alu);
    if (status != HlslStatus::kOk) return;

    // Both halves read the register file before either writes — the co-issue
    // semantics. Evaluating into locals first is what preserves that; assigning
    // the vector result straight into r[] would let the scalar half read the
    // new value.
    if (vmask != 0) Line("xe_v = " + vexpr + ";");
    // xe_ps is the Xenos PS register, and the hardware writes it on EVERY scalar
    // issue. The write mask governs only whether the result is ALSO committed to
    // the destination register, so gating the ps update on it silently deletes
    // the one thing a mask-less scalar op exists to produce.
    //
    // Shaders lean on that. Xenia's dump of this title has 47 such instructions
    // across 20 shaders, every one immediately consumed by an adds_prev/muls_prev:
    //
    //     15   dp3_sat r1._y__, r3.wyzz, c158.zxyy
    //      +   maxs r0._, -r5.yy          <- empty mask, issued only for ps
    //     16   dp3 r5._y__, r2.wyzz, r3.wyzz
    //      +   muls_prev r5.__z_, c255.y  <- reads that ps
    //
    // We emitted no line for 15, so 16 multiplied by instruction 14's leftover
    // ps. Same error as the maxa/maxas/maxasf one above, one register over: a
    // write mask is not a statement about side effects.
    //
    // kRetainPrev exists precisely to NOT disturb ps, and stays correct because
    // it yields xe_ps: the assignment is a self-assignment.
    Line("xe_s = " + sexpr + "; xe_ps = xe_s;");

    if (is_export) {
      const uint32_t dest = alu.vector_dest();
      std::string target;
      if (!pixel() && dest == kPositionExportRegister) {
        writes_position = true;
        target = "xe_pos";
      } else if (pixel() && dest < kMaxColorTargets) {
        color_mask |= 1u << dest;
        target = "xe_color" + std::to_string(dest);
      } else if (pixel() && dest == kPixelDepthExportRegister) {
        writes_depth = true;
        target = "xe_depth";
      } else if (!pixel() && dest < interpolators_) {
        export_mask |= 1u << dest;
        target = "xe_o" + std::to_string(dest);
      } else {
        // An export we do not link: a point size, a misc output, or an
        // interpolator past the agreed linkage width. Dropped, not faked -- but
        // the ALU side effects above have already been emitted. Recorded rather
        // than discarded silently, because from outside the emitter there is
        // otherwise no way to tell a rejected export from one never reached.
        if (dest < 32) dropped_export_mask |= 1u << dest;
        // MEMORY EXPORT (dest 32..37) is dropped like the rest, but counted --
        // the mask above cannot hold it, so without this a shader that writes
        // guest memory is indistinguishable from one that exports nothing.
        else if (dest <= 37) ++memexport_count;
        return;
      }
      const uint32_t zero_mask = alu.GetConstant0WriteMask();
      const uint32_t one_mask = alu.GetConstant1WriteMask();
      if (!(vmask | smask | zero_mask | one_mask)) return;
      // Priority matches the interpreter: vector, then scalar, then the
      // constant 1 and constant 0 encodings.
      uint32_t taken = 0;
      if (pixel() && dest < kMaxColorTargets) color_assigned_mask |= 1u << dest;
      if (vmask) {
        Line(target + MaskSwizzle(vmask) + " = xe_v" + MaskSwizzle(vmask) +
             ";");
        taken |= vmask;
      }
      if (const uint32_t m = smask & ~taken) {
        Line(target + MaskSwizzle(m) + " = xe_s;");
        taken |= m;
      }
      if (const uint32_t m = one_mask & ~taken) {
        Line(target + MaskSwizzle(m) + " = 1.0;");
        taken |= m;
      }
      if (const uint32_t m = zero_mask & ~taken) {
        Line(target + MaskSwizzle(m) + " = 0.0;");
      }
      return;
    }

    // Destinations name temp registers, so a relative destination is aL-relative
    // for the same reason a relative temp source is. (In the export path above,
    // is_scalar_dest_relative is not an index at all — it is the "write unwritten
    // components as 0" flag — which is why this check lives here.)
    if (alu.is_vector_dest_relative() || alu.is_scalar_dest_relative()) {
      status = HlslStatus::kLoopRelative;
      return;
    }
    if (vmask) {
      const uint32_t d = alu.vector_dest();
      Line(Temp(d) + MaskSwizzle(vmask) + " = xe_v" + MaskSwizzle(vmask) + ";");
      MarkWritten(d);
    }
    if (smask) {
      const uint32_t d = alu.scalar_dest();
      Line(Temp(d) + MaskSwizzle(smask) + " = xe_s;");
      MarkWritten(d);
    }
  }

  void EmitTextureFetch(const uc::TextureFetchInstruction& tf) {
    if (tf.is_src_relative() || tf.is_dest_relative()) {
      status = HlslStatus::kFetchRelative;
      blocking_opcode = uint32_t(tf.opcode());
      return;
    }
    // 1D joins 2D: a single-row texture samples correctly through an ordinary 2D
    // binding, so it needs no new resource type -- only the v coordinate pinned
    // to the row, below.
    //
    // CUBE joins them through a Texture2DArray, because the guest has already
    // done the cube projection in software:
    //
    //     cube       r0, source.zzxy, source.yxz   // (T, S, 2*majoraxis, face)
    //     rcp        r0.z, r0_abs.z
    //     mad        r0.xy, r0, r0.zzzw, 1.5f      // ST biased into [1,2)
    //     tfetchCube r0, r0.yxw, tf0               // samples (S, T, face)
    //
    // so tfetchCube receives three scalars, never a direction vector -- which is
    // the whole reason `cube` exists as an ALU opcode. Binding a real TextureCube
    // would mean UNDOING that projection so the hardware could redo it. Faces are
    // six ordinary 2D images at 4 KB-aligned strides, and Xenos does not filter
    // across face edges either.
    //
    // 3D/STACKED joins the array path for the same reason. One opcode serves two
    // storage layouts (ucode.h: "3D (used for both 3D and stacked 2D texture)")
    // and they are told apart by the FETCH CONSTANT's DataDimension, not by the
    // instruction. A stacked texture is stack_depth+1 ordinary 2D images at
    // 4 KB-aligned strides, which DescribeHleTexture2D already produces.
    //
    // A true 3D volume interleaves its slices INSIDE a tile and does need a
    // resource type we do not build; DescribeHleTexture2D refuses those by name
    // and the draw falls back. Refusing at the texture, where the dimension is
    // known, rather than at the instruction, where it is not, is the whole
    // change.
    //
    // The reference reaches the same two destinations by declaring BOTH a
    // Texture3D and a Texture2DArray and branching per-fetch on fetch constant
    // word 5 bits 9:10; it needs the branch because its shaders are cached
    // against fetch constants it has not resolved at translate time.
    using Dim = rex::graphics::xenos::FetchOpDimension;
    const bool is_cube = tf.dimension() == Dim::kCube;
    const bool is_3d = tf.dimension() == Dim::k3DOrStacked;
    if (!is_cube && !is_3d && tf.dimension() != Dim::k2D &&
        tf.dimension() != Dim::k1D) {
      status = HlslStatus::kFetch3D;
      blocking_opcode = uint32_t(tf.opcode());
      return;
    }
    // Both spellings that need a third coordinate share one declaration.
    const bool is_array = is_cube || is_3d;
    const uint32_t sampler = tf.fetch_constant_index();
    PixelTextureBinding binding;
    binding.sampler = sampler;
    binding.src_reg = tf.src();
    binding.src_swizzle = tf.src_swizzle();
    binding.unnormalized = tf.unnormalized_coordinates();
    fetches.push_back(binding);
    sampler_mask |= 1u << (sampler & 31);
    const uint32_t slot = SamplerSlot(sampler);
    if (status != HlslStatus::kOk) return;

    // One slot, one resource type. A second fetch on the same slot at the other
    // dimension would contradict whichever declaration the preamble writes, so
    // the shader is refused rather than emitted with a descriptor that cannot
    // match both of its fetches. Compared against is_ARRAY, not is_cube: cube
    // and 3D/stacked both declare Texture2DArray, so only array-versus-plain-2D
    // is a real contradiction.
    const uint32_t slot_bit = 1u << slot;
    if ((slot_fetched_mask & slot_bit) &&
        bool(slot_array_mask & slot_bit) != is_array) {
      status = HlslStatus::kFetchDimensionConflict;
      blocking_opcode = uint32_t(tf.opcode());
      return;
    }
    slot_fetched_mask |= slot_bit;
    if (is_array) slot_array_mask |= slot_bit;
    if (is_cube && !uses_cube) cube_fetch_without_cube_op = true;

    // The fetch source swizzle is ABSOLUTE, two bits per component — unlike an
    // ALU swizzle, which is component-relative. Reading it the ALU way sends
    // every fetch to the wrong coordinate pair.
    const uint32_t swiz = tf.src_swizzle();
    const bool is_1d = tf.dimension() == rex::graphics::xenos::FetchOpDimension::k1D;
    std::string uv;
    uv += kComponent[swiz & 3];
    if (!is_1d) uv += kComponent[(swiz >> 2) & 3];

    if (is_cube) {
      // The third source component is the face index the `cube` op produced, 0-5
      // in D3DCUBEMAP_FACES order -- which is also the array slice order the
      // guest stores the faces in. ST arrive biased into [1,2) by the `mad ...,
      // 1.5` above: that bias is what the hardware fetch expects, so subtracting
      // one is decoding the operand rather than guessing at a convention.
      // xe_texinv is deliberately NOT applied: these are already face-relative.
      const std::string src = Temp(tf.src());
      const std::string face = std::string(1, kComponent[(swiz >> 4) & 3]);
      const std::string s3 = std::to_string(slot);
      Line("xe_v = xe_tex" + s3 + SampleOp(tf) + "xe_smp" + s3 + ", float3(" +
           src + "." + uv + " - 1.0, " + src + "." + face + ")" +
           SampleLod(tf, slot) + ");");
      EmitFetchDestination(tf);
      return;
    }

    // TEXEL OFFSETS. The instruction carries offset_x/y/z and this emitter used
    // to drop them, so every offset fetch in the game sampled the coordinate
    // unmodified. bloom.shader::BlurHPS is three tfetch2D from the same
    // register and the same fetch constant, differing ONLY by these fields:
    // without them its blur is a weighted copy of one texel.
    //
    // HALF-TEXEL UNITS, from the SDK -- `offset_x() { return data_.offset_x *
    // 0.5f; }` (ucode.h:816). So the accessor already returns texels and the
    // raw field is twice that. That also rules out HLSL's integer offset
    // argument, which takes an immediate texel count: bloom's taps are at
    // +/-1.5 texels and are not representable there. Applied to the coordinate
    // instead, scaled into normalized space by xe_texinv.
    //
    // Applied AFTER the unnormalized divide below, so one expression covers
    // both cases: a denormalized coordinate becomes raw*texinv + off*texinv,
    // which is (raw + off) * texinv, the texel-space sum.
    const float off_x = tf.offset_x();
    const float off_y = tf.offset_y();
    const float off_z = tf.offset_z();

    std::string coord = Temp(tf.src()) + "." + uv;
    if (tf.unnormalized_coordinates()) {
      // `tx_coord_denorm` means the guest addresses this texture in TEXELS; the
      // host sampler wants normalized coordinates, so this divides by the
      // extent. It arrives pre-reciprocated as xe_texinv, both to keep the
      // divide off the per-pixel path and so that "no texture bound" can be
      // encoded as 0 -- which reads texel 0, rather than the infinity a division
      // by a zero extent would produce. This used to MULTIPLY by the extent.
      coord = "(" + coord + " * xe_texinv[" + std::to_string(slot) +
              (is_1d ? "].x)" : "].xy)");
    }
    // Now, while the coordinate is still the bare 1 or 2 normalized components
    // and before the 1D/3D wrapping below turns it into a float2/float3.
    if (off_x != 0.0f || off_y != 0.0f) {
      const std::string ti = "xe_texinv[" + std::to_string(slot) + "]";
      if (is_1d) {
        coord = "(" + coord + " + " + std::to_string(off_x) + " * " + ti + ".x)";
      } else {
        coord = "(" + coord + " + float2(" + std::to_string(off_x) + ", " + std::to_string(off_y) +
                ") * " + ti + ".xy)";
      }
    }
    if (is_1d) {
      // A Xenos 1D texture is one row, described to the host as width x 1, so
      // the row centre is v = 0.5 in normalized space whatever the width.
      coord = "float2(" + coord + ", 0.5)";
    }
    if (is_3d) {
      // W selects the slice. Which units it arrives in is the SAME question the
      // reference answers with a runtime branch, and it has a static answer:
      //
      //   unnormalized -- W is already a layer index (the layer axis has no
      //     sub-texel meaning for a stack), so it passes through.
      //   normalized -- W is a fraction of the stack, so it scales by the layer
      //     COUNT, which rides in xe_texinv[slot].z.
      //
      // Scaling by a count rather than count-1 matches the hardware's treatment
      // of the stack as a texel axis, and the sampler clamps the top slice. A
      // zero .z (no texture bound) collapses every fetch to slice 0.
      //
      // KNOWN DIVERGENCE, and the reason a real Texture3D is still worth
      // building later: Xenos filters BETWEEN stack layers under VolMagFilter /
      // VolMinFilter, and ucode.h names colour correction as the use for it. A
      // Texture2DArray slice index does not filter, so a graded frame comes out
      // with correct hues and visible banding along the LUT's third axis.
      const std::string src = Temp(tf.src());
      const std::string w =
          src + "." + std::string(1, kComponent[(swiz >> 4) & 3]);
      coord = "float3(" + coord + ", " +
              (tf.unnormalized_coordinates()
                   ? w
                   : "(" + w + " * xe_texinv[" + std::to_string(slot) + "].z)") +
              ")";
    }
    // offset_z is NOT applied. The slice axis of a stacked texture is an array
    // index here, not a filtered coordinate, so a half-texel offset along it
    // has no host equivalent -- the same divergence the 3D note above records.
    // Counted rather than ignored, so "no shader uses it" stays a measurement.
    if (off_z != 0.0f) ++fetch_offset_z_dropped;

    const std::string s = std::to_string(slot);
    Line("xe_v = xe_tex" + s + SampleOp(tf) + "xe_smp" + s + ", " + coord +
         SampleLod(tf, slot) + ");");
    // TEX_FORMAT_COMP / GPUSIGN, applied here because it is per-BINDING state,
    // not per-texture: the same guest memory is bound with different sign modes
    // by different draws, so baking it into the decode would poison a cache
    // keyed on content. 2*c-1 is also unrepresentable in the UNORM8 the decode
    // produces. The reference does the same.
    //
    // xe_texsign carries a per-component SCALE, already permuted into host
    // component order by SwizzleTextureSigns because the SRV applies the fetch
    // swizzle before the shader sees a texel. The offset is not carried: for the
    // two modes that reach here it is exactly 1-scale. kSigned is applied
    // host-side by picking a SNORM view and arrives as an identity scale; kGamma
    // cannot ride a scale, so 3.0 is a SELECTOR and the decode below applies the
    // piecewise-linear curve instead.
    //
    // THE BRANCH DOES NOT ACTUALLY SKIP THE CURVE, and this is measured.
    // `any(xe_gam)` reads a constant buffer, so it is uniform across the wave and
    // should be a real branch -- but fxc flattens it, and `[branch]` altered the
    // output of ZERO of the 81 shaders in logs/hlsldump. What that costs, fxc
    // /O3, counting only instructions outside any branch:
    //
    //     51 of 81 shaders   no change at all
    //     aggregate          6194 -> 6977 slots, +12.6%
    //     worst, ps_215F0020    41 ->  284, +243   (19 fetch sites, tiny base)
    //
    // So ~13 slots per fetch SITE, paid whether or not gamma is bound. The
    // alternative is a shader permutation keyed on the sign modes, which the
    // cache-by-handle design deliberately avoids.
    Line("{");
    Line("  float4 xe_sgn = xe_texsign[" + s + "];");
    Line("  float4 xe_gam = saturate(xe_sgn - 2.0);");
    Line("  float4 xe_scl = xe_sgn - xe_gam * 2.0;");
    Line("  xe_v = xe_v * xe_scl + (1.0 - xe_scl);");
    Line("  if (any(xe_gam))");
    Line("    xe_v = lerp(xe_v, XePWLGammaToLinear(xe_v), xe_gam);");
    Line("}");
    EmitFetchDestination(tf);
  }

  // A VERTEX fetch, decoded on the GPU out of the raw guest vertex buffer. The
  // CPU used to do this: per vertex, per attribute, a stream memcpy, this format
  // decode and this destination swizzle, at 0.48us a vertex over 289,000
  // vertices a frame. Everything the decode needs -- format, offset, dest and
  // dest swizzle -- comes from the INSTRUCTION, so the generated HLSL stays a
  // pure function of the shader handle; only the buffer base, the stride and the
  // endian mode are runtime state, and those ride in xe_vf[].
  //
  // `src_reg`/`src_swizzle`/`rounded` are the EFFECTIVE index operand, which for
  // a vfetch_mini is the preceding vfetch_full's. The SDK is explicit: "the
  // source is applicable only to vfetch_full (the address from vfetch_full is
  // reused in vfetch_mini)", and the same holds for is_index_rounded and stride,
  // so the caller tracks them across the pair.
  void EmitVertexFetch(const uc::VertexFetchInstruction& vf,
                       uint32_t fetch_ordinal, uint32_t src_reg,
                       uint32_t src_swizzle, bool rounded) {
    if (vf.exp_adjust() != 0) {
      // Decoded by the ucode reader and applied by nothing, on either path. A
      // power-of-two scale dropped silently is wrong geometry that looks
      // plausible, so refuse instead. Censused as always 0 in this game.
      status = HlslStatus::kVertexFetchExpAdjust;
      blocking_opcode = uint32_t(vf.opcode());
      return;
    }
    if (rounded) {
      status = HlslStatus::kVertexFetchIndex;
      blocking_opcode = uint32_t(vf.opcode());
      return;
    }

    const uint32_t fmt = uint32_t(vf.data_format());
    // num_format_all == 0 means normalized -- the sense is inverted, which is
    // why this reads is_normalized() rather than the raw bit. The pairing below
    // matches d3d9_layout.cpp's ReadHleElement, NOT shader_ucode.cpp's
    // ReadVertexAttribute: that PM4 wrapper deliberately ignores both bits, and
    // matching it here would decode signed data as unsigned.
    const bool sign = vf.is_signed();
    const bool norm = vf.is_normalized();

    // Sizes from VertexFormatSizeBytes (shader_ucode.cpp:114-136), in dwords.
    uint32_t dwords = 0;
    switch (fmt) {
      case 6: case 7: case 25: case 31: case 36: dwords = 1; break;  // 4 bytes
      case 26: case 32: case 37: dwords = 2; break;          // 8 bytes
      case 57: dwords = 3; break;                            // 12 bytes
      case 38: dwords = 4; break;                            // 16 bytes
      default:
        // 16/17 (k_10_11_11, k_11_11_10) and the integer 32-bit family never
        // appear in this title's census; refusing keeps the draw on the CPU
        // path rather than guessing at a decode nothing has exercised.
        status = HlslStatus::kVertexFetchFormat;
        blocking_opcode = uint32_t(vf.opcode());
        return;
    }

    // Every fetch gets its own block. A shader with several vfetches would
    // otherwise redeclare xe_vr and the byte address at function scope, and the
    // scratch names below are deliberately fixed rather than uniquified.
    Line("{");
    const std::string n = std::to_string(fetch_ordinal);
    const std::string base = "xe_vf[" + n + "]";
    // offset() is in dwords, like stride().
    //
    // The INDEX REGISTER the guest named, not SV_VertexID. This was `xe_vid`
    // unconditionally, which is only correct while every vfetch indexes by r0.x:
    // three shaders fetch a 48-byte per-object stream with `src r0.y` while
    // fetching a FOUR-entry corner table with `src r0.x`. Xenia writes the vertex
    // index to GPR 0 `.x` ONLY, so r0.y holds a computed value, and substituting
    // the vertex ID addressed that table at 8984..15443 instead of 0..6777.
    //
    // Xenos keeps the index in a float register, so convert here. FLOOR, both
    // ways -- matching Xenia (dxbc_shader_translator_fetch.cc:74): OpRoundNI on
    // the index, and OpAdd 0.5 first when is_index_rounded. NOT trunc and NOT
    // HLSL round(), which is half-to-even and so meaningless for addressing.
    Line("uint xe_vfi = (uint)" +
         std::string(rounded ? "floor(0.5 + " : "floor(") + "r[" +
         std::to_string(src_reg) + "]." +
         std::string(1, "xyzw"[src_swizzle & 3]) + ");");
    Line("uint xe_vfa = " + base + ".x + xe_vfi * " + base + ".y + " +
         std::to_string(uint32_t(vf.offset()) * 4) + "u;");

    const char* load = dwords == 1 ? "Load" : dwords == 2 ? "Load2"
                     : dwords == 3 ? "Load3" : "Load4";
    const std::string uty = dwords == 1 ? "uint" : "uint" + std::to_string(dwords);

    // Bounds. xe_vb is bound as a ROOT SRV, which carries a virtual address and
    // no size, so nothing here is bounds-checked by the hardware: a read past
    // this stream's region takes the next stream's vertices, then another draw's
    // suballocation in the same upload page, then undefined memory.
    //
    // `.w` is one past this stream's valid bytes. Past it the fetch yields zero,
    // which is what the hardware and the reference do with an over-long fetch;
    // this also covers a fetch whose xe_vf[] slot was never filled. The add
    // cannot wrap in practice, and is written this way rather than as `xe_vfa <=
    // .w - N` precisely because THAT form underflows when a stream's region is
    // shorter than one attribute.
    //
    // The ADDRESS is clamped, not just the result. `?:` around the Load itself
    // would not be enough: HLSL does not guarantee short-circuit evaluation and
    // fxc routinely evaluates both sides of a select, and a root SRV has no size
    // for the hardware to clamp against -- so that load can cross a page and
    // fault. Loading from 0 instead is always in bounds.
    std::string zeros = uty + "(0u";
    for (uint32_t i = 1; i < dwords; ++i) zeros += ", 0u";
    zeros += ")";
    Line("bool xe_vin = xe_vfa + " + std::to_string(dwords * 4) + "u <= " +
         base + ".w;");
    Line(uty + " xe_vr = xe_vb." + load + "(xe_vin ? xe_vfa : 0u);");
    Line("if (!xe_vin) xe_vr = " + zeros + ";");

    // Endian, exactly as ApplyFetchEndianFor does it on the CPU: reverse fixed
    // width units across the attribute, 4-byte units for mode 2 and 2-byte for
    // mode 1, WITHOUT consulting the format. A 4-byte reversal of a dword holding
    // two 16-bit components both swaps each component and exchanges the pair,
    // and that is the hardware's real behaviour -- the guest compiler compensates
    // for it in the destination swizzle. Mode 2 is 7335 of 8772 fetches here and
    // mode 1 never occurs, but both are emitted rather than assumed away.
    Line("if (" + base + ".z == 2u) xe_vr = XeSwap8in32(xe_vr);");
    Line("else if (" + base + ".z == 1u) xe_vr = XeSwap8in16(xe_vr);");

    // Unwritten components default to (0,0,0,1), matching ReadVertexAttributeAs.
    Line("xe_v = float4(0.0, 0.0, 0.0, 1.0);");
    EmitVertexFormatDecode(fmt, dwords, sign, norm);
    EmitFetchDestination(vf.dest(), vf.dest_swizzle());
    Line("}");
    ++vertex_fetch_count;
  }

  // Turn the raw dwords in xe_vr into xe_v, per Xenos vertex format. Mirrors
  // ReadVertexAttributeAs (shader_ucode.cpp:214-330) case for case.
  void EmitVertexFormatDecode(uint32_t fmt, uint32_t dwords, bool sign,
                              bool norm) {
    const std::string r = "xe_vr";
    auto dw = [&](uint32_t i) {
      return dwords == 1 ? r : r + "." + std::string(1, kComponent[i]);
    };
    switch (fmt) {
      // --- float formats: no num_format involvement --------------------------
      case 36:  // k_32_FLOAT (not in the census, but free alongside 37/57/38)
        Line("xe_v.x = asfloat(" + dw(0) + ");");
        break;
      case 37:  // k_32_32_FLOAT
        Line("xe_v.xy = asfloat(" + r + ".xy);");
        break;
      case 57:  // k_32_32_32_FLOAT
        Line("xe_v.xyz = asfloat(" + r + ".xyz);");
        break;
      case 38:  // k_32_32_32_32_FLOAT
        Line("xe_v = asfloat(" + r + ");");
        break;
      case 31:  // k_16_16_FLOAT -- two halves packed in one dword
        Line("xe_v.x = f16tof32(" + dw(0) + " & 0xFFFFu);");
        Line("xe_v.y = f16tof32(" + dw(0) + " >> 16);");
        break;
      case 32:  // k_16_16_16_16_FLOAT
        Line("xe_v.x = f16tof32(" + r + ".x & 0xFFFFu);");
        Line("xe_v.y = f16tof32(" + r + ".x >> 16);");
        Line("xe_v.z = f16tof32(" + r + ".y & 0xFFFFu);");
        Line("xe_v.w = f16tof32(" + r + ".y >> 16);");
        break;

      // --- integer formats: num_format decides the scaling -------------------
      case 25:    // k_16_16 (absent from the census; free alongside 26)
        EmitInt16Pair("xe_v.xy", dw(0), sign, norm, 0);
        break;
      case 26:    // k_16_16_16_16
        EmitInt16Pair("xe_v.xy", r + ".x", sign, norm, 0);
        EmitInt16Pair("xe_v.zw", r + ".y", sign, norm, 1);
        break;
      case 6: {  // k_8_8_8_8 -- component 0 is the LOW byte
        for (uint32_t i = 0; i < 4; ++i) {
          const std::string b =
              "((" + dw(0) + " >> " + std::to_string(i * 8) + ") & 0xFFu)";
          const std::string comp = "xe_v." + std::string(1, kComponent[i]);
          if (sign) {
            // Sign-extend the byte, then normalise by 127 as Norm(S8) does.
            Line("int xe_s8_" + std::to_string(i) + " = int(" + b +
                 " ^ 0x80u) - 128;");
            const std::string s = "xe_s8_" + std::to_string(i);
            Line(comp + " = " +
                 (norm ? "max(float(" + s + ") / 127.0, -1.0)" : "float(" + s + ")") +
                 ";");
          } else {
            Line(comp + " = " +
                 (norm ? "float(" + b + ") / 255.0" : "float(" + b + ")") + ";");
          }
        }
        break;
      }
      case 7: {  // k_2_10_10_10 -- three 10-bit then a 2-bit at bits 30-31
        for (uint32_t i = 0; i < 3; ++i) {
          const std::string f =
              "((" + dw(0) + " >> " + std::to_string(i * 10) + ") & 0x3FFu)";
          const std::string comp = "xe_v." + std::string(1, kComponent[i]);
          if (sign) {
            Line("int xe_s10_" + std::to_string(i) + " = int(" + f +
                 " ^ 0x200u) - 512;");
            const std::string s = "xe_s10_" + std::to_string(i);
            Line(comp + " = " +
                 (norm ? "max(float(" + s + ") / 511.0, -1.0)" : "float(" + s + ")") +
                 ";");
          } else {
            Line(comp + " = " +
                 (norm ? "float(" + f + ") / 1023.0" : "float(" + f + ")") + ";");
          }
        }
        {
          const std::string f = "((" + dw(0) + " >> 30) & 0x3u)";
          if (sign) {
            Line("int xe_s2 = int(" + f + " ^ 0x2u) - 2;");
            Line("xe_v.w = " +
                 std::string(norm ? "max(float(xe_s2) / 1.0, -1.0)"
                                  : "float(xe_s2)") + ";");
          } else {
            Line("xe_v.w = " +
                 std::string(norm ? "float(" + f + ") / 3.0" : "float(" + f + ")") +
                 ";");
          }
        }
        break;
      }
      default:
        // Unreachable: EmitVertexFetch refuses anything not listed above.
        status = HlslStatus::kVertexFetchFormat;
        break;
    }
  }

  // Two 16-bit components out of one dword, low half first.
  void EmitInt16Pair(const std::string& dst, const std::string& src, bool sign,
                     bool norm, uint32_t half) {
    const std::string a = "xe_h" + std::to_string(half);
    if (sign) {
      Line("int2 " + a + " = int2(int((" + src + " & 0xFFFFu) ^ 0x8000u) - 32768,"
           " int((" + src + " >> 16) ^ 0x8000u) - 32768);");
      Line(dst + " = " +
           (norm ? "max(float2(" + a + ") / 32767.0, -1.0)" : "float2(" + a + ")") +
           ";");
    } else {
      Line("uint2 " + a + " = uint2(" + src + " & 0xFFFFu, " + src + " >> 16);");
      Line(dst + " = " +
           (norm ? "float2(" + a + ") / 65535.0" : "float2(" + a + ")") + ";");
    }
  }

  // A predicated TEXTURE fetch. The `if` goes around the destination write ONLY
  // -- the .Sample() above has already been emitted, outside it.
  //
  // That is what makes this legal where the exec-block form is not. .Sample()
  // needs implicit derivatives and cannot appear in varying flow control, but a
  // fetch's only effect is writing its destination register, so sampling
  // unconditionally and gating the WRITE is observationally identical. Nothing
  // is reordered -- the sample was simply never inside the `if`.
  //
  // The guest agrees, in the encoding: every predicated fetch in this title that
  // carries the attribute sets FetchValidOnly=false, which ucode.h defines as
  // "whether the data should be fetched only for pixels inside the current
  // primitive in a 2x2 quad".
  //
  // Xenia instead wraps the fetch itself and pays for it with explicit gradients
  // everywhere, because it wraps whole exec blocks, where a skipped fetch has to
  // interact with memexport and kill.
  //
  // EXEC-BLOCK predication is NOT solved by this and must not be assumed to be:
  // a fetch inside a p0-gated block would need its coordinate computed outside
  // the block, which nothing guarantees.
  void EmitFetchDestination(const uc::TextureFetchInstruction& tf) {
    PredicateBlock pred(this, tf.is_predicated(), tf.predicate_condition(),
                        &predicated_fetches);
    EmitFetchDestination(tf.dest(), tf.dest_swizzle());
  }

  // Write xe_v out through the fetch's destination swizzle. Shared by the 1D/2D
  // and cube texture paths and by the vertex fetch, which differ only in how
  // they produce xe_v. Takes the two fields rather than an instruction because
  // TextureFetchInstruction and VertexFetchInstruction are unrelated types that
  // happen to spell these identically.
  void EmitFetchDestination(uint32_t dest, uint32_t dswiz) {
    // The destination swizzle is three bits per component: 0-3 select x/y/z/w
    // of the fetched value, 4 is 0.0, 5 is 1.0, and 7 means keep — which is what
    // lets two fetches share a destination register, so ignoring it would
    // clobber.
    const std::string dst = Temp(dest);
    for (uint32_t c = 0; c < 4; ++c) {
      switch (uc::GetFetchDestinationComponentSwizzle(dswiz, c)) {
        case uc::FetchDestinationSwizzle::kX:
        case uc::FetchDestinationSwizzle::kY:
        case uc::FetchDestinationSwizzle::kZ:
        case uc::FetchDestinationSwizzle::kW: {
          const uint32_t src = uint32_t(
              uc::GetFetchDestinationComponentSwizzle(dswiz, c));
          Line(dst + "." + kComponent[c] + " = xe_v." + kComponent[src] + ";");
          break;
        }
        case uc::FetchDestinationSwizzle::k0:
          Line(dst + "." + kComponent[c] + " = 0.0;");
          break;
        case uc::FetchDestinationSwizzle::k1:
          Line(dst + "." + kComponent[c] + " = 1.0;");
          break;
        default:
          break;  // kKeep, and the one undefined encoding
      }
    }
    MarkWritten(dest);
  }

 private:
  HlslStage stage_;
  uint32_t interpolators_;
  bool vertex_fetch_ = false;
};

}  // namespace

bool EmitShaderHlsl(const uint32_t* dwords, uint32_t dword_count,
                    HlslStage stage, uint32_t interpolator_count,
                    HlslShader& out, bool emit_vertex_fetch) {
  // Only the vertex stage has vertex fetches. Asking for them on a pixel shader
  // is a caller mistake, and silently honouring it would emit a buffer
  // declaration nothing binds.
  if (stage == HlslStage::kPixel) emit_vertex_fetch = false;
  out = {};
  if (interpolator_count > kMaxHlslInterpolators)
    interpolator_count = kMaxHlslInterpolators;
  if (!dwords || dword_count < 3) {
    out.status = HlslStatus::kMalformed;
    return false;
  }

  // Bound the CF section exactly as the decoder and the interpreter do: it ends
  // at the lowest exec target, and the bound shrinks as execs are seen.
  uint32_t max_cf_dword = dword_count - (dword_count % 3);
  bool saw_exec = false;
  for (uint32_t i = 0; i + 2 < max_cf_dword; i += 3) {
    uc::ControlFlowInstruction cf[2];
    uc::UnpackControlFlowInstructions(dwords + i, cf);
    for (int j = 0; j < 2; ++j) {
      if (!uc::IsControlFlowOpcodeExec(cf[j].opcode())) continue;
      saw_exec = true;
      const uint64_t t = uint64_t(ExecAddress(cf[j])) * 3;
      if (t < max_cf_dword) max_cf_dword = uint32_t(t);
    }
  }
  if (!saw_exec || max_cf_dword == 0) {
    out.status = HlslStatus::kNoExec;
    return false;
  }

  Emitter em(stage, interpolator_count, emit_vertex_fetch);
  uint32_t executed = 0;
  // Vertex fetch bookkeeping, unused unless emit_vertex_fetch. `fetch_slot_of`
  // tells the host which guest stream each xe_vf[] entry addresses.
  uint32_t vfetch_ordinal = 0;
  uint32_t last_full_slot = 0;
  bool have_full = false;
  uint32_t fetch_slot_of[HlslShader::kMaxVertexFetches] = {};
  // The index operand carried across a vfetch_full -> vfetch_mini pair,
  // and the streams a non-r0.x index makes unknowable to the CPU path.
  uint32_t last_full_src = 0, last_full_src_swz = 0;
  bool last_full_rounded = false;
  uint32_t computed_index_streams = 0, computed_index_fetches = 0;
  // Which register COMPONENTS the shader has written before the current
  // instruction. 32 registers x 4 components.
  //
  // r0.x holds the vertex index at ENTRY and nothing else does -- but a shader
  // is free to overwrite it, and the billboard shaders do:
  //
  //   r0.x = vid % 4        corner index, fetched from a 4-entry table
  //   r0.x = floor(vid / 4) instance index, into a 6778-entry table
  //
  // Both fetches then read `src r0.x`, so testing the register NUMBER classifies
  // them as vertex-indexed, windows their streams at first_vertex, and drops
  // every region. What matters is whether that component is still unwritten at
  // the point of the fetch.
  uint32_t written_comp[32] = {};

  for (uint32_t i = 0; i + 2 < max_cf_dword; i += 3) {
    uc::ControlFlowInstruction cf[2];
    uc::UnpackControlFlowInstructions(dwords + i, cf);
    for (int j = 0; j < 2; ++j) {
      if (IsUnsupportedCf(cf[j].opcode())) {
        out.status = HlslStatus::kUnsupportedCf;
        out.blocking_opcode = uint32_t(cf[j].opcode());
        return false;
      }
      if (!uc::IsControlFlowOpcodeExec(cf[j].opcode())) continue;

      // Split by MECHANISM, not by name: kCondExecPredClean rides the
      // bool-constant struct, not the predicate one, so grouping it with
      // kCondExecPred (as one counter used to) hides which fix applies. See
      // HlslShader::pred_exec_blocks.
      bool p0_gated = false;
      bool p0_condition = false;
      switch (cf[j].opcode()) {
        case uc::ControlFlowOpcode::kCondExecPred:
        case uc::ControlFlowOpcode::kCondExecPredEnd:
          ++out.pred_exec_blocks;
          p0_gated = true;
          p0_condition = cf[j].cond_exec_pred.condition();
          break;
        case uc::ControlFlowOpcode::kCondExec:
        case uc::ControlFlowOpcode::kCondExecEnd:
        case uc::ControlFlowOpcode::kCondExecPredClean:
        case uc::ControlFlowOpcode::kCondExecPredCleanEnd:
          ++out.bool_exec_blocks;  // bool constant — needs a bank first
          break;
        default:
          break;
      }

      // HONOUR the predicate where it is safe to, which today means the vertex
      // stage only. Xenia emits a plain `if` on p0 and gets away with it because
      // it emits DXBC directly and its fetches carry an explicit LOD. Ours emit
      // HLSL, and the two stages differ:
      //
      //   VERTEX  every vertex fetch is already SampleLevel, so no implicit
      //           gradient, an `if` around it is legal, and a vertex stage has
      //           no discard to be skipped. SAFE.
      //   PIXEL   the body spells `.Sample()`, illegal in varying flow control;
      //           FXC rejects it and the shader falls to a stand-in, which is a
      //           VISIBLE regression (this is how the water was lost once
      //           already). `[flatten]` is worse: both affected pixel shaders
      //           contain `discard`. UNSAFE until fetches inside a predicated
      //           region are emitted gradient-free.
      //
      // NOT a correctness gap. This `if` is a WAVEFRONT SKIP, not per-lane
      // correctness -- ucode.h on kCondExecPred: "if any of the invocations
      // passes the predicate check, all of them will enter the exec". Per-lane
      // correctness comes from the instruction predicates, and all 240 ALU and
      // fetch instructions inside this title's cond_exec_pred blocks carry their
      // own (p0).
      const bool honour_p0 = p0_gated && stage != HlslStage::kPixel;
      if (honour_p0) {
        ++out.honoured_pred_exec_blocks;
        em.Line(std::string("if (xe_p0 == ") + (p0_condition ? "true" : "false") +
                ") {");
      }

      const uint32_t addr = ExecAddress(cf[j]);
      const uint32_t count = ExecCount(cf[j]);
      const uint32_t seq = ExecSequence(cf[j]);
      for (uint32_t n = 0; n < count; ++n) {
        if (++executed > kMaxInstructions) {
          out.status = HlslStatus::kInstructionCap;
          return false;
        }
        const uint64_t at = (uint64_t(addr) + n) * 3;
        if (at + 3 > dword_count) {
          out.status = HlslStatus::kMalformed;
          return false;
        }
        // Bit 0 of each 2-bit sequence slot says "this is a fetch", exactly as
        // DecodePixelTextureFetches reads it.
        if ((seq >> (n * 2)) & 0x1) {
          // FetchOpcode is NOT two-valued. It is {kVertexFetch=0,
          // kTextureFetch=1, kGetTextureBorderColorFrac=16,
          // kGetTextureComputedLod=17, kGetTextureGradients=18,
          // kGetTextureWeights=19, kSetTextureLod=20,
          // kSetTextureGradientsHorz=21, kSetTextureGradientsVert=22}, so a
          // two-way split on `!= kTextureFetch` sends all seven high opcodes
          // down the VERTEX branch, where they were read as a
          // VertexFetchInstruction: word 1 bits 13:18 landed in exp_adjust as
          // garbage and refused the whole shader -- ~89k-100k draws a run kept
          // off the GPU fetch path over a plain setTexLOD.
          //
          // Test each opcode for what it IS. The high ops are skipped and
          // counted rather than refused.
          const uint32_t fetch_op = dwords[at] & 0x1F;
          if (fetch_op == uint32_t(uc::FetchOpcode::kVertexFetch)) {
            // A vertex fetch. Without emit_vertex_fetch the host input assembler
            // performs it and skipping is what leaves the destination register
            // unwritten, which is precisely how input_mask is produced. With it,
            // the shader reads the raw guest buffer itself.
            if (!emit_vertex_fetch) continue;
            if (vfetch_ordinal >= HlslShader::kMaxVertexFetches) {
              out.status = HlslStatus::kVertexFetchFormat;
              return false;
            }
            uc::VertexFetchInstruction vf{};
            std::memcpy(&vf, dwords + at, sizeof(vf));
            // Mini fetches inherit the fetch constant from the preceding full
            // one, exactly as DecodeVertexShaderFetches does. Their own field
            // reads garbage, so consuming it unconditionally would address the
            // wrong stream.
            if (!vf.is_mini_fetch()) {
              last_full_slot = vf.fetch_constant_index();
              last_full_src = vf.src();
              last_full_src_swz = vf.src_swizzle();
              last_full_rounded = vf.is_index_rounded();
              have_full = true;
            } else if (!have_full) {
              out.status = HlslStatus::kVertexFetchIndex;
              return false;
            }
            em.EmitVertexFetch(vf, vfetch_ordinal, last_full_src,
                               last_full_src_swz, last_full_rounded);
            fetch_slot_of[vfetch_ordinal] = last_full_slot;
            // The index is the vertex index only if it is r0.x AND that
            // component is still untouched. Once the shader has written
            // it, the value is computed and absolute, so the stream cannot
            // be windowed by the draw's vertex range.
            const bool src_written =
                last_full_src < 32 &&
                // Bit 0 is x, per MaskSwizzle: `mask & (1u << c)` with
                // kComponent[c] = "xyzw". Writing this inverted would
                // misclassify silently, which is why it is taken from the
                // emitter rather than assumed.
                (written_comp[last_full_src] &
                 (1u << (last_full_src_swz & 3))) != 0;
            const bool by_vertex = last_full_src == 0 &&
                                   last_full_src_swz == 0 && !src_written;
            if (!by_vertex) {
              if (vfetch_ordinal < 32)
                computed_index_fetches |= 1u << vfetch_ordinal;
              if (last_full_slot <= 95 && (95u - last_full_slot) < 32)
                computed_index_streams |= 1u << (95u - last_full_slot);
            }
            ++vfetch_ordinal;
          } else if (fetch_op == uint32_t(uc::FetchOpcode::kTextureFetch)) {
            uc::TextureFetchInstruction tf{};
            std::memcpy(&tf, dwords + at, sizeof(tf));
            em.EmitTextureFetch(tf);
          } else if (fetch_op == uint32_t(uc::FetchOpcode::kSetTextureLod)) {
            // Emitted in BOTH stages. It writes a shader register, samples
            // nothing and needs no binding, so there is no reason a vertex
            // shader may not have one — and the one shader this whole change
            // is about is exactly that case.
            uc::TextureFetchInstruction tf{};
            std::memcpy(&tf, dwords + at, sizeof(tf));
            em.EmitSetTextureLod(tf);
          } else if (fetch_op ==
                     uint32_t(uc::FetchOpcode::kGetTextureGradients)) {
            // Emitted in the pixel stage; EmitGetGradients counts it as
            // unhonoured in a vertex shader, where a derivative is meaningless.
            uc::TextureFetchInstruction tf{};
            std::memcpy(&tf, dwords + at, sizeof(tf));
            em.EmitGetGradients(tf);
          } else {
            // getCompTexLOD / getWeights / getBCF and the two setGradients. Not
            // implemented; skipped with the destination left holding whatever it
            // had. Counted so the population stays visible rather than being
            // silently approximated -- getGradients WAS in this list and is now
            // honoured above, and it was not harmless.
            ++em.unhonoured_fetch_ops;
          }
        } else {
          uc::AluInstruction alu{};
          std::memcpy(&alu, dwords + at, sizeof(alu));
          em.EmitAlu(alu);
          // Relative addressing can land anywhere, so treat it as writing
          // everything rather than guessing which register it hit.
          if (alu.is_vector_dest_relative() ||
              alu.is_scalar_dest_relative()) {
            for (auto& w : written_comp) w = 0xF;
          } else {
            if (alu.vector_dest() < 32)
              written_comp[alu.vector_dest()] |= alu.vector_write_mask();
            if (alu.scalar_dest() < 32)
              written_comp[alu.scalar_dest()] |= alu.scalar_write_mask();
          }
        }
        if (em.status != HlslStatus::kOk) {
          out.status = em.status;
          out.blocking_opcode = em.blocking_opcode;
          return false;
        }
      }
      if (honour_p0) em.Line("}");
    }
  }

  // Colour targets named by an export but never actually ASSIGNED. See
  // `color_assigned_mask` in the emitter for why the two masks differ: such a
  // target is emitted as its float4(0,0,0,0) initialiser, i.e. opaque black.
  // Surfaced on the result rather than logged here, exactly like
  // dropped_export_mask above -- this file is a pure translation unit.
  out.color_unassigned_mask =
      stage == HlslStage::kPixel ? (em.color_mask & ~em.color_assigned_mask) : 0;

  const bool produced = stage == HlslStage::kPixel ? (em.color_mask != 0)
                                                   : em.writes_position;
  if (!produced) {
    out.status = HlslStatus::kNoOutput;
    return false;
  }

  // ---- Assemble the translation unit -------------------------------------
  std::string src;
  src += "// Generated from Xenos microcode by EmitShaderHlsl.\n";
  // b0 for the vertex stage, b1 for the pixel stage — because the two banks are
  // different memory (ALU constants 0-255 at device+0x780 and 256-511 at
  // device+0x1780) and a root CBV binds one buffer per register per pipeline.
  // Emitting both at b1 would mean the vertex stage reading the pixel bank.
  src += "cbuffer XeShaderConstants : register(";
  src += stage == HlslStage::kPixel ? "b1" : "b0";
  src += ") {\n";
  src += "  float4 xe_c[256];\n";
  // Indexed by compact slot, not by guest sampler — same remap as the textures.
  // Holds the RECIPROCAL of the bound texture's extent (0 when none); see the
  // unnormalized-coordinate note in EmitTextureFetch.
  src += "  float4 xe_texinv[" + std::to_string(HlslShader::kMaxSamplerSlots) +
         "];\n";
  // Per-component TextureSign scale, host component order, 1.0 where the fetch
  // is plain unsigned. See the note at the fetch site.
  if (stage == HlslStage::kPixel) {
    src += "  float4 xe_texsign[" +
           std::to_string(HlslShader::kMaxSamplerSlots) + "];\n";
    // .x is zero when disabled, otherwise one plus the destination register;
    // .y classifies the primitive (0 triangle, 1 point, 2 line). This remains
    // draw state because host shaders are cached by guest shader handle.
    src += "  uint4 xe_param_gen;\n";
    // The guest alpha test: RB_COLORCONTROL's comparison and enable, plus
    // RB_ALPHA_REF. .x is the xenos::CompareFunction, .y the enable, .z the
    // reference as raw float bits (the cbuffer member is uint4, so it is
    // reinterpreted in the shader rather than stored twice).
    //
    // DRAW STATE, not a shader permutation, for the same reason xe_param_gen is:
    // host shaders are cached by guest shader handle, and making the test part
    // of the key would compile the same shader once per alpha state.
    src += "  uint4 xe_alphatest;\n";
    // Colour output scale, .x only. 1/32 for a k_16_16 / k_16_16_16_16 target,
    // 1.0 for everything else.
    //
    // Those two RENDER TARGET formats are signed fixed point -32...32, and a
    // resolve out of them is NOT bitwise equivalent to the texture format
    // (xenos.h:566). We resolve with CopyTextureRegion, which IS bitwise, so the
    // range has to be applied somewhere else -- and the reference puts it here,
    // on the write: "Remap from -32...32 to -1...1", color_exp_bias -= 5. Doing
    // it on the write keeps the host target holding -1...1, which makes both the
    // resolve and any direct sample correct without a scaling blit.
    src += "  float4 xe_colorscale;\n";
  }
  // One entry per emitted vertex fetch: .x the byte offset of this attribute's
  // stream within the merged raw buffer (with first_vertex already folded in),
  // .y the stream stride, .z the endian mode. Everything else the decode needs
  // came from the instruction. Always declared for the vertex stage, even at
  // zero fetches, so the constant buffer has one layout rather than two.
  if (stage != HlslStage::kPixel) {
    src += "  uint4 xe_vf[" + std::to_string(HlslShader::kMaxVertexFetches) +
           "];\n";
    // AFTER xe_vf, deliberately. The renderer writes xe_vf at a FIXED byte
    // offset computed as constDwords*4 + kMaxSamplerSlots*16, so anything
    // inserted between xe_texinv and xe_vf silently moves the vertex fetch table
    // and corrupts every GPU-fetched attribute.
    //
    // The comment this replaces said the vertex stage needed no texture sign
    // "because no vertex shader in this game samples anything". Measured false:
    // 230,720 draws in one run were refused the GPU vertex path for having a
    // sampler, and the interpreter they fell to has no texture fetch at all.
    src += "  float4 xe_texsign[" +
           std::to_string(HlslShader::kMaxSamplerSlots) + "];\n";
    // THE D3D9 HALF-PIXEL OFFSET, in NDC, already scaled for this draw's
    // viewport: .xy is added to the clip-space position times w.
    //
    // D3D9 centres pixels at .0 and the host rasterises at .5, so a guest quad
    // covering the whole target lands half a pixel short and its last row and
    // column clamp -- one anomalous line on the bottom and right edges of every
    // full-screen pass.
    //
    // APPLIED HERE, NOT TO THE HOST VIEWPORT. Shifting viewport.TopLeftX/Y moves
    // rasterisation without moving anything else, so a texel-exact pass then
    // samples BETWEEN texels: that is what blew the 160x90 luminance downsample
    // and drove the whole scene white. The reference folds the same +0.5 into
    // the viewport offset in the GUEST's space and emits it as an NDC offset on
    // the vertex position (draw_util.cc:389).
    //
    // LAST in the cbuffer, for the reason above.
    src += "  float4 xe_ndc_offset;\n";
  }
  src += "};\n";
  // RECIP_FF / RECIPSQ_FF: an infinity becomes a signed zero. See the note at
  // the kRcpf case. Emitted unconditionally: it is four lines, and gating it on
  // whether the shader happens to use one is a way for the two to drift apart.
  //
  // `isinf` rather than a comparison against FLT_MAX: rcp of a denormal can
  // overflow to infinity without the input being zero, and the hardware flushes
  // that case too.
  src +=
      "float XeFlushInf(float v) {\n"
      "  return isinf(v) ? (v < 0.0 ? -0.0 : 0.0) : v;\n"
      "}\n";
  // The clamping forms (rcpc, rsqc, logc) saturate an infinity to the finite
  // extreme and leave EVERYTHING else alone, NaN included. Done the way Xenia
  // does it, on the integer representation: 0x7F800000 - 1 is 0x7F7FFFFF and
  // 0xFF800000 - 1 is 0xFF7FFFFF, exactly +-FLT_MAX. clamp() and min() folded
  // NaN to FLT_MAX.
  src +=
      "float XeClampInf(float v) {\n"
      "  return (asuint(v) & 0x7FFFFFFFu) == 0x7F800000u\n"
      "             ? asfloat(asint(v) - 1)\n"
      "             : v;\n"
      "}\n"
      "float XeClampNegInf(float v) {\n"
      "  return asuint(v) == 0xFF800000u ? -3.402823466e+38 : v;\n"
      "}\n";
  // TextureSign::kGamma. Xenos gamma is a FOUR-PIECE PIECEWISE LINEAR curve, NOT
  // sRGB, so a *_UNORM_SRGB host view is the wrong curve and not merely a
  // cheaper one. Transcribed from the reference's PWLGammaToLinear.
  //
  // step() rather than a ternary chain because step(edge, x) is exactly
  // `x >= edge` and yields a float4 mask directly, and because a NaN input
  // compares false everywhere and so selects the lowest piece -- which the
  // reference documents it relies on. Emitted unconditionally, like XeFlushInf
  // above: the sign mode is per-BINDING, so it is not known at translate time.
  src +=
      "float4 XePWLGammaToLinear(float4 g) {\n"
      "  float4 low = step(64.0 / 255.0, g);\n"
      "  float4 mid = step(96.0 / 255.0, g);\n"
      "  float4 top = step(192.0 / 255.0, g);\n"
      "  float4 scale = lerp(lerp(lerp(1.0 / 1024.0, 2.0 / 1024.0, low),\n"
      "                           4.0 / 1024.0, mid), 8.0 / 1024.0, top);\n"
      "  float4 offset = lerp(lerp(lerp(0.0, -64.0, low), -256.0, mid),\n"
      "                       -1024.0, top);\n"
      "  float4 v = saturate(g) * (255.0 * 1024.0);\n"
      "  v = v * scale + offset;\n"
      "  v += trunc(v * scale);\n"
      "  return v * (1.0 / 1023.0);\n"
      "}\n";
  // Direct3D 9 "legacy" multiply, which is what every multiplying operation on
  // this hardware does -- mul, mad, the dot products, and their scalar forms.
  // Quoting the SDK (ucode.h, above AluScalarOpcode):
  //
  //   Direct3D 9 rules (like in GCN v_*_legacy_f32 instructions) for
  //   multiplication (+-0 or denormal * anything = +0) wherever it's present
  //   (mul, mad, dp, etc.) [...] Infinity * 0 resulting in NaN breaks a lot of
  //   things in games - causes white screen [...], white specular on characters
  //   [...]. The result is always positive zero in this case, no matter what the
  //   signs of the other operands are.
  //
  // That is this game's menu exactly: the light-prepass materials sample the
  // screen-space light buffer, take its Rec.709 luminance, `rcp` it, and scale
  // colour*colour by the result. Where the light buffer is black the hardware
  // computes 0 * INF = +0 and the specular term vanishes; we computed NaN.
  //
  // DENORMALS COUNT AS ZERO, as the SDK states it. Testing exact zero is not
  // enough: the input flush is about how an operand is READ, while this rule is
  // about what the PRODUCT is. The comparison is against FLT_MIN, the smallest
  // NORMAL float, with abs() so a negative denormal is caught, and the returned
  // zero is always positive. NaN is deliberately NOT caught: abs(NaN) < FLT_MIN
  // is false, so a NaN operand still propagates, which is what the hardware does.
  //
  // mad stays unfused and is emitted as XeMul(a, b) + c rather than as a select
  // on c -- +0 + -0 is +0, so a zero multiplicand must still go through the add.
  src +=
      "float XeMul(float a, float b) {\n"
      "  return (abs(a) < 1.175494351e-38 || abs(b) < 1.175494351e-38)\n"
      "             ? 0.0\n"
      "             : a * b;\n"
      "}\n"
      "float2 XeMul(float2 a, float2 b) {\n"
      "  return float2(XeMul(a.x, b.x), XeMul(a.y, b.y));\n"
      "}\n"
      "float3 XeMul(float3 a, float3 b) {\n"
      "  return float3(XeMul(a.x, b.x), XeMul(a.y, b.y), XeMul(a.z, b.z));\n"
      "}\n"
      "float4 XeMul(float4 a, float4 b) {\n"
      "  return float4(XeMul(a.x, b.x), XeMul(a.y, b.y), XeMul(a.z, b.z),\n"
      "                XeMul(a.w, b.w));\n"
      "}\n"
      // ---- Shader Model 3 min/max ------------------------------------------
      // `a op b ? a : b`, NOT the HLSL intrinsics. The difference is NaN and
      // only NaN: D3D min/max return the NON-NaN operand, so max(5, NaN) is 5,
      // while the SM3 form asks `5 >= NaN`, which is false, and yields NaN. The
      // console propagates; the intrinsic swallows. The same reference note that
      // carries the legacy-multiply rule ends "and for NaN in min/max. It is
      // very important to respect" (ucode.h:975).
      //
      // This closes an EMITTER/INTERPRETER DIVERGENCE, the precise hazard the
      // XeMul note warns about: shader_alu.cpp has always used the SM3 form.
      //
      // A helper rather than an inline ternary because the operands are
      // expression STRINGS, some large, and expanding each twice per min/max
      // would bloat every shader against kInstructionCap.
      //
      // Deliberately NOT applied to: kLogc/kRsqc, whose max/min against
      // +-FLT_MAX is part of those opcodes' own clamped semantics; the
      // vertex-fetch SNORM `max(x, -1.0)`, a format rule; or the 7e3 output
      // clamp, which models the ROP -- there D3D NaN->0 IS the hardware
      // behaviour being reproduced.
      "float XeMax(float a, float b) { return a >= b ? a : b; }\n"
      "float XeMin(float a, float b) { return a < b ? a : b; }\n"
      "float2 XeMax(float2 a, float2 b) {\n"
      "  return float2(XeMax(a.x, b.x), XeMax(a.y, b.y));\n"
      "}\n"
      "float2 XeMin(float2 a, float2 b) {\n"
      "  return float2(XeMin(a.x, b.x), XeMin(a.y, b.y));\n"
      "}\n"
      "float3 XeMax(float3 a, float3 b) {\n"
      "  return float3(XeMax(a.x, b.x), XeMax(a.y, b.y), XeMax(a.z, b.z));\n"
      "}\n"
      "float3 XeMin(float3 a, float3 b) {\n"
      "  return float3(XeMin(a.x, b.x), XeMin(a.y, b.y), XeMin(a.z, b.z));\n"
      "}\n"
      "float4 XeMax(float4 a, float4 b) {\n"
      "  return float4(XeMax(a.x, b.x), XeMax(a.y, b.y), XeMax(a.z, b.z),\n"
      "                XeMax(a.w, b.w));\n"
      "}\n"
      "float4 XeMin(float4 a, float4 b) {\n"
      "  return float4(XeMin(a.x, b.x), XeMin(a.y, b.y), XeMin(a.z, b.z),\n"
      "                XeMin(a.w, b.w));\n"
      "}\n"
      "float XeDot2(float2 a, float2 b) {\n"
      "  float2 p = XeMul(a, b);\n"
      "  return p.x + p.y;\n"
      "}\n"
      "float XeDot3(float3 a, float3 b) {\n"
      "  float3 p = XeMul(a, b);\n"
      "  return p.x + p.y + p.z;\n"
      "}\n"
      "float XeDot4(float4 a, float4 b) {\n"
      "  float4 p = XeMul(a, b);\n"
      "  return p.x + p.y + p.z + p.w;\n"
      "}\n";
  if (em.vertex_fetch_count) {
    // t16, not (t0, space1): register spaces need shader model 5.1 and this
    // compiles vs_5_0. The pixel stage's descriptor table covers t0..t15 --
    // kMaxSamplerSlots of them -- so t16 is the first free register and cannot
    // collide with it whatever the visibility.
    static_assert(HlslShader::kMaxSamplerSlots == 16,
                  "xe_vb sits at t16 because the texture table ends at t15");
    src += "ByteAddressBuffer xe_vb : register(t16);\n";
    // The endian shuffles, matching ApplyFetchEndianFor: whole-attribute
    // reversal in fixed units, applied per dword because the CPU loop reverses
    // each 4-byte unit independently.
    src +=
        "uint XeSwap8in32(uint v) {\n"
        "  return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |\n"
        "         ((v & 0x00FF0000u) >> 8)  | ((v & 0xFF000000u) >> 24);\n"
        "}\n"
        "uint XeSwap8in16(uint v) {\n"
        "  return ((v & 0x00FF00FFu) << 8) | ((v & 0xFF00FF00u) >> 8);\n"
        "}\n"
        "uint2 XeSwap8in32(uint2 v) {\n"
        "  return uint2(XeSwap8in32(v.x), XeSwap8in32(v.y));\n"
        "}\n"
        "uint2 XeSwap8in16(uint2 v) {\n"
        "  return uint2(XeSwap8in16(v.x), XeSwap8in16(v.y));\n"
        "}\n"
        "uint3 XeSwap8in32(uint3 v) {\n"
        "  return uint3(XeSwap8in32(v.x), XeSwap8in32(v.y), XeSwap8in32(v.z));\n"
        "}\n"
        "uint3 XeSwap8in16(uint3 v) {\n"
        "  return uint3(XeSwap8in16(v.x), XeSwap8in16(v.y), XeSwap8in16(v.z));\n"
        "}\n"
        "uint4 XeSwap8in32(uint4 v) {\n"
        "  return uint4(XeSwap8in32(v.x), XeSwap8in32(v.y), XeSwap8in32(v.z),\n"
        "               XeSwap8in32(v.w));\n"
        "}\n"
        "uint4 XeSwap8in16(uint4 v) {\n"
        "  return uint4(XeSwap8in16(v.x), XeSwap8in16(v.y), XeSwap8in16(v.z),\n"
        "               XeSwap8in16(v.w));\n"
        "}\n";
  }
  // Declared for EVERY slot up to sampler_count, contiguously from this stage's
  // base register, so the registers match a descriptor table of exactly that
  // width. Both stages base at t0/s0 and are kept apart by the root signature's
  // ShaderVisibility.
  //
  // A slot fetched as a cube is declared Texture2DArray: the guest projects the
  // direction to (S, T, face) itself, so an array indexed by face is what its
  // operands already describe. BindTranslatedTextures must give that slot a
  // TEXTURE2DARRAY SRV to match.
  const uint32_t tex_base = stage == HlslStage::kPixel
                                ? HlslShader::kPixelTextureBaseRegister
                                : HlslShader::kVertexTextureBaseRegister;
  const uint32_t smp_base = stage == HlslStage::kPixel
                                ? HlslShader::kPixelSamplerBaseRegister
                                : HlslShader::kVertexSamplerBaseRegister;
  for (uint32_t s = 0; s < em.sampler_count; ++s) {
    const std::string n = std::to_string(s);
    const bool array = (em.slot_array_mask >> s) & 1u;
    src += array ? "Texture2DArray<float4> xe_tex" : "Texture2D<float4> xe_tex";
    src += n + " : register(t" + std::to_string(tex_base + s) + ");\n";
    src += "SamplerState xe_smp" + n + " : register(s" +
           std::to_string(smp_base + s) + ");\n";
  }

  const uint32_t link = interpolator_count;
  src += "struct XeInterpolants {\n";
  src += "  float4 pos : SV_Position;\n";
  for (uint32_t i = 0; i < link; ++i) {
    const std::string n = std::to_string(i);
    src += "  float4 i" + n + " : TEXCOORD" + n + ";\n";
  }
  src += "};\n";

  if (stage == HlslStage::kPixel) {
    src += "struct XePsOut {\n";
    for (uint32_t t = 0; t < kMaxColorTargets; ++t) {
      if (!(em.color_mask & (1u << t))) continue;
      const std::string n = std::to_string(t);
      src += "  float4 c" + n + " : SV_Target" + n + ";\n";
    }
    if (em.writes_depth) src += "  float d : SV_Depth;\n";
    src += "};\n";
    src +=
        "XePsOut main(XeInterpolants xe_in, bool xe_front : SV_IsFrontFace) {\n";
  } else {
    // Two shapes, and which one this is depends on emit_vertex_fetch.
    //
    // Without it, a vertex shader's inputs are its vfetch destinations: the
    // fetches are skipped here, the host input assembler performs them driven by
    // the layout DecodeVertexShaderFetches recovers, and each register the body
    // reads before writing becomes one input element. The caller must build its
    // input layout from input_mask, in this same order.
    //
    // With it, the shader fetches for itself out of xe_vb and the only input is
    // the vertex ID. input_mask is then normally empty -- a register still in it
    // is one the body reads that no vfetch writes, which reads zero on hardware
    // too.
    if (emit_vertex_fetch) {
      src += "XeInterpolants main(uint xe_vid : SV_VertexID) {\n";
    } else {
      src += "struct XeVsIn {\n";
      for (uint32_t i = 0; i < kNumTemps && i < 32; ++i) {
        if (!(em.input_mask & (1u << i))) continue;
        const std::string n = std::to_string(i);
        src += "  float4 v" + n + " : TEXCOORD" + n + ";\n";
      }
      src += "};\n";
      src += "XeInterpolants main(XeVsIn xe_in) {\n";
    }
  }

  src += "  float4 r[" + std::to_string(kNumTemps) + "];\n";
  src += "  [unroll] for (int xe_i = 0; xe_i < " + std::to_string(kNumTemps) +
         "; ++xe_i) r[xe_i] = float4(0, 0, 0, 0);\n";
  // r0.x IS the vertex index on Xenos, and nothing set it here before. Xenia
  // does exactly this in StartVertexShader_LoadVertexIndex: the index goes to
  // GPR 0 with write mask 0b0001, converted to float. It was invisible while the
  // fetch hard-coded SV_VertexID, but now that each fetch indexes by the
  // register it names, an r0.x fetch reads this -- and any shader reading r0.x
  // for its own arithmetic was silently getting 0 all along.
  if (emit_vertex_fetch) src += "  r[0].x = (float)xe_vid;\n";
  src += "  float4 xe_v = float4(0, 0, 0, 0);\n";
  src += "  float xe_s = 0.0, xe_ps = 0.0;\n";
  // Written by setTexLOD, read by any later tfetch whose use_reg_lod is set.
  // Declared only when one of those exists, so a shader with neither emits
  // exactly the HLSL it emitted before this was added. Zero-initialised because
  // the two can appear in either order.
  if (em.uses_reg_lod) src += "  float xe_lod = 0.0;\n";
  // Written by setp_*, and READ by cond_exec_pred in the vertex stage. Still
  // unread in the pixel stage, where `.Sample()` inside varying flow control is
  // illegal; see the safety note at the emit site and
  // HlslShader::honoured_pred_exec_blocks for the seen-vs-obeyed split. Declared
  // ahead of `src += em.body`, so every emitted block is downstream of it.
  src += "  bool xe_p0 = false;\n";
  src += "  int xe_a0 = 0;\n";
  if (em.uses_cube) {
    src += "  float3 xe_cube = float3(0, 0, 0), xe_cube_a = float3(0, 0, 0);\n";
    src += "  float4 xe_cube_r = float4(0, 0, 0, 0);\n";
  }

  if (stage == HlslStage::kPixel) {
    // Interpolators arrive in the low temp registers, which is the linkage the
    // hardware itself uses.
    for (uint32_t i = 0; i < link; ++i) {
      if (!(em.input_mask & (1u << i))) continue;
      src += "  r[" + std::to_string(i) + "] = xe_in.i" + std::to_string(i) +
             ";\n";
    }
    // PsParamGen.xy is the magnitude of the host pixel position rounded down.
    // Floor before abs so sample-rate positions and derivative helper quads
    // retain the Xenos integer-pixel behavior. The X face flag applies only to
    // polygon primitives; points are always front-facing and lines have their
    // own flag in Z. Point-sprite z/w coordinates need point expansion that this
    // renderer does not yet provide.
    src +=
        "  if (xe_param_gen.x != 0) {\n"
        "    uint xe_pg_reg = xe_param_gen.x - 1;\n"
        "    uint4 xe_pg = asuint(float4(abs(floor(xe_in.pos.xy)), 0.0, 0.0));\n"
        "    if (xe_param_gen.y == 0 && !xe_front) xe_pg.x |= 0x80000000u;\n"
        "    if (xe_param_gen.y == 1) xe_pg.y |= 0x80000000u;\n"
        "    if (xe_param_gen.y == 2) xe_pg.z |= 0x80000000u;\n"
        "    r[xe_pg_reg] = asfloat(xe_pg);\n"
        "  }\n";
    for (uint32_t t = 0; t < kMaxColorTargets; ++t) {
      if (!(em.color_mask & (1u << t))) continue;
      src += "  float4 xe_color" + std::to_string(t) + " = float4(0,0,0,0);\n";
    }
    if (em.writes_depth) src += "  float xe_depth = 0.0;\n";
  } else {
    if (!emit_vertex_fetch) {
      for (uint32_t i = 0; i < kNumTemps && i < 32; ++i) {
        if (!(em.input_mask & (1u << i))) continue;
        src += "  r[" + std::to_string(i) + "] = xe_in.v" + std::to_string(i) +
               ";\n";
      }
    }
    src += "  float4 xe_pos = float4(0, 0, 0, 1);\n";
    for (uint32_t i = 0; i < link; ++i)
      src += "  float4 xe_o" + std::to_string(i) + " = float4(0, 0, 0, 0);\n";
  }

  src += em.body;

  if (stage == HlslStage::kPixel) {
    // The alpha test, as a discard. D3D12 has no fixed-function equivalent -- it
    // is the one piece of Xenos output-merger state with nowhere else to go, and
    // the reference puts it here too (dxbc_shader_translator_om.cc). Ignoring it
    // is why alpha-cutout geometry rendered as filled quads.
    //
    // Against colour target 0's alpha only, which is the only one the hardware
    // tests. kNotEqual is spelled `!=` rather than `<` combined with `>`: it must
    // pass for NaN, and the pair of comparisons does not.
    if (em.color_mask & 1u) {
      src +=
          "  if (xe_alphatest.y != 0) {\n"
          "    float xe_at_a = xe_color0.w;\n"
          "    float xe_at_ref = asfloat(xe_alphatest.z);\n"
          "    bool xe_at_pass;\n"
          "    switch (xe_alphatest.x) {\n"
          "      case 0: xe_at_pass = false; break;\n"
          "      case 1: xe_at_pass = xe_at_a <  xe_at_ref; break;\n"
          "      case 2: xe_at_pass = xe_at_a == xe_at_ref; break;\n"
          "      case 3: xe_at_pass = xe_at_a <= xe_at_ref; break;\n"
          "      case 4: xe_at_pass = xe_at_a >  xe_at_ref; break;\n"
          "      case 5: xe_at_pass = xe_at_a != xe_at_ref; break;\n"
          "      case 6: xe_at_pass = xe_at_a >= xe_at_ref; break;\n"
          "      default: xe_at_pass = true; break;\n"
          "    }\n"
          "    if (!xe_at_pass) discard;\n"
          "  }\n";
    }
    // AFTER the alpha test, never before. RB_ALPHA_REF is in the guest's own
    // colour units, so the test has to compare against the unscaled alpha.
    // Scaling here rather than in the emitted ALU also keeps it linear with
    // respect to blending: every factor the guest uses is linear in the source
    // colour, so a uniform scale on the output commutes with the blend.
    for (uint32_t t = 0; t < kMaxColorTargets; ++t) {
      if (!(em.color_mask & (1u << t))) continue;
      const std::string n = std::to_string(t);
      src += "  xe_color" + n + " *= xe_colorscale.x;\n";
    }
    // Clamp to the range the GUEST colour format can actually represent.
    //
    // Guest format 3 (k_2_10_10_10_FLOAT) is 7e3, and the reference states its
    // range outright: "7e3 [0, 32) RGB, unorm alpha" (xenos.h:301). It is
    // UNSIGNED, and we map it to R16G16B16A16_FLOAT, which is signed, so a
    // shader returning a negative has that negative STORED where the console ROP
    // would have clamped it to 0 -- measured, the scene target holds (0.236,
    // 0.159, -0.071) at one pixel and that negative propagates into the 320x180
    // luminance target that drives auto-exposure.
    //
    // Xenia performs exactly this clamp before packing: max(x, 0.0) then
    // min(x, 31.875). The ORDER matters for NaN: D3D min/max return the non-NaN
    // operand, so NaN becomes 0 instead of propagating -- hardware behaviour,
    // which is why this is min(max(..)) and not clamp(), since clamp(NaN, lo,
    // hi) would yield lo.
    //
    // Gated on xe_colorscale.y, which the renderer sets to the RGB maximum for
    // the clamped formats and leaves 0 for every other one. A uniform branch, so
    // draws on any other format stay byte-identical -- deliberately NOT an
    // unconditional clamp against +-FLT_MAX, which reads like a no-op but would
    // turn NaN into -FLT_MAX on every draw in the game.
    if (em.color_mask) {
      src += "  if (xe_colorscale.y > 0.0) {\n";
      for (uint32_t t = 0; t < kMaxColorTargets; ++t) {
        if (!(em.color_mask & (1u << t))) continue;
        const std::string n = std::to_string(t);
        src += "    xe_color" + n + ".rgb = min(max(xe_color" + n +
               ".rgb, 0.0), xe_colorscale.y);\n";
        src += "    xe_color" + n + ".a = min(max(xe_color" + n +
               ".a, 0.0), xe_colorscale.z);\n";
      }
      src += "  }\n";
    }
    src += "  XePsOut xe_out;\n";
    for (uint32_t t = 0; t < kMaxColorTargets; ++t) {
      if (!(em.color_mask & (1u << t))) continue;
      const std::string n = std::to_string(t);
      src += "  xe_out.c" + n + " = xe_color" + n + ";\n";
    }
    if (em.writes_depth) src += "  xe_out.d = xe_depth;\n";
    src += "  return xe_out;\n";
  } else {
    src += "  XeInterpolants xe_out;\n";
    src += "  xe_out.pos = xe_pos;\n";
    // Times w because this runs before the perspective divide, so the offset
    // is a constant number of PIXELS at any depth rather than shrinking with
    // distance.
    src += "  xe_out.pos.xy += xe_ndc_offset.xy * xe_pos.w;\n";
    for (uint32_t i = 0; i < link; ++i)
      src += "  xe_out.i" + std::to_string(i) + " = xe_o" + std::to_string(i) +
             ";\n";
    src += "  return xe_out;\n";
  }
  src += "}\n";

  out.status = HlslStatus::kOk;
  out.source = std::move(src);
  out.fetches = std::move(em.fetches);
  out.sampler_mask = em.sampler_mask;
  out.sampler_count = em.sampler_count;
  out.sampler_array_mask = em.slot_array_mask;
  out.cube_fetch_without_cube_op = em.cube_fetch_without_cube_op;
  for (uint32_t i = 0; i < em.sampler_count; ++i)
    out.sampler_slot_guest[i] = em.slot_guest[i];
  out.input_mask = em.input_mask;
  out.export_mask = stage == HlslStage::kPixel ? em.color_mask : em.export_mask;
  out.dropped_export_mask = em.dropped_export_mask;
  out.memexport_count = em.memexport_count;
  out.writes_position = em.writes_position;
  out.writes_depth = em.writes_depth;
  out.max_const_index = em.max_const_index;
  for (int i = 0; i < 4; ++i) out.const_mask[i] = em.const_mask[i];
  out.const_relative = em.const_relative;
  out.reads_constants = em.reads_constants;
  out.unhonoured_predicate_ops = em.unhonoured_predicate_ops;
  out.predicated_alu_ops = em.predicated_alu_ops;
  out.predicated_fetches = em.predicated_fetches;
  out.unhonoured_fetch_ops = em.unhonoured_fetch_ops;
  out.vertex_fetch_count = em.vertex_fetch_count;
  for (uint32_t i = 0; i < em.vertex_fetch_count; ++i)
    out.vertex_fetch_slot[i] = fetch_slot_of[i];
  out.computed_index_streams = computed_index_streams;
  out.computed_index_fetches = computed_index_fetches;
  return true;
}

}  // namespace mx::hle
