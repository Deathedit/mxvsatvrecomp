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
// The exec accessors. Address/count/sequence sit at the same bit offsets in
// all three exec structs but are distinct union members, so the dispatch is
// copied from shader_alu.cpp deliberately rather than shared: these three are
// the load-bearing part of the walk, and a shared helper that drifted would
// desynchronise the emitter from the interpreter silently.
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
// it: emitting a straight-line body for a shader that branches or loops
// produces confidently wrong pixels. Every shader captured from this game is a
// straight run of execs (kUnsupportedCf measures 0), so this costs nothing today
// and stays honest if that changes.
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
  Emitter(HlslStage stage, uint32_t interpolator_count)
      : stage_(stage), interpolators_(interpolator_count) {}

  HlslStatus status = HlslStatus::kOk;
  uint32_t blocking_opcode = 0;
  uint32_t input_mask = 0;
  uint32_t written_mask = 0;   // temps this shader has written so far
  uint32_t export_mask = 0;
  uint32_t color_mask = 0;
  bool writes_position = false;
  bool writes_depth = false;
  uint32_t max_const_index = 0;
  bool reads_constants = false;
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
  // device+0x1780 — proven from D3DDevice_DrawVertices' own flush, which passes
  // Xenos register base 0x4000 for the first and 0x4400 for the second
  // (0x4400 = 0x4000 + 1024 dwords = constant 256). The shader indexes its bank
  // from 0 either way, so the base is the caller's job at upload time and this
  // emits a direct index.
  std::string Const(uint32_t index) {
    reads_constants = true;
    if (index > max_const_index) max_const_index = index;
    return "xe_c[" + std::to_string(index) + "]";
  }
  std::string ConstRelative(uint32_t index) {
    reads_constants = true;
    max_const_index = 255;  // any slot is reachable once a0 is involved
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
      case Op::kMax: r = "max(" + a + ", " + b + ")"; break;
      case Op::kMin: r = "min(" + a + ", " + b + ")"; break;
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
        r = "max(" + a + ", " + b + ")";
        break;
      case Op::kMad:
        r = "(XeMul(" + a + ", " + b + ") + " + Src(alu, 3) + ")";
        break;
      // lerp with a selector of exactly 0 or 1 is an exact select, and unlike a
      // vector ternary it is unambiguous about being component-wise.
      case Op::kCndEq:
        r = "lerp(" + Src(alu, 3) + ", " + b + ", float4(" + a + " == 0.0))";
        break;
      case Op::kCndGe:
        r = "lerp(" + Src(alu, 3) + ", " + b + ", float4(" + a + " >= 0.0))";
        break;
      case Op::kCndGt:
        r = "lerp(" + Src(alu, 3) + ", " + b + ", float4(" + a + " > 0.0))";
        break;
      case Op::kDp4: r = "XeDot4(" + a + ", " + b + ").xxxx"; break;
      case Op::kDp3: r = "XeDot3((" + a + ").xyz, (" + b + ").xyz).xxxx"; break;
      case Op::kDp2Add:
        r = "(XeDot2((" + a + ").xy, (" + b + ").xy) + (" + Src(alu, 3) +
            ").x).xxxx";
        break;
      case Op::kMax4: {
        r = "max(max((" + a + ").x, (" + a + ").y), max((" + a + ").z, (" + a +
            ").w)).xxxx";
        break;
      }
      case Op::kDst:
        r = "float4(1.0, XeMul((" + a + ").y, (" + b + ").y), (" + a + ").z, (" +
            b + ").w)";
        break;
      // Cube map coordinate generation. Transcribed from the hardware
      // definition in ucode.h (kCube), not approximated:
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
      // is still evaluated, because the hardware reads it and because an
      // operand skipped here would corrupt the constant-read accounting.
      //
      // Emitted as statements rather than one expression: the three-way major
      // axis selection does not fold into anything readable, and readability is
      // what lets this be checked against the hardware doc line by line.
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
      default:
        // The setp_*_push family. Has not appeared in a shader captured here.
        // Reported by opcode, not guessed. (kCube used to be listed here too;
        // it is implemented above.)
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
  // register is scattered, with one bit in the opcode field itself — which is
  // why each operation has a _0 and a _1 form. Both operands are scalars,
  // selected by the low two bits of src3_swiz. The constant's negate bit
  // applies; the temp's does not, there being no field for it.
  bool ConstRegScalarOp(const uc::AluInstruction& alu, std::string& out) {
    using Op = uc::AluScalarOpcode;
    const Op op = alu.scalar_opcode();
    if (op < Op::kMulsc0 || op > Op::kSubsc1) return false;

    const uint32_t comp = alu.src_swizzle(3) & 3;
    std::string a = Const(alu.src_reg(3) & 0xFF) + "." + kComponent[comp];
    if (alu.abs_constants()) a = "abs(" + a + ")";
    if (alu.src_negate(3)) a = "-(" + a + ")";
    const std::string b =
        Temp(alu.scalar_const_reg_op_src_temp_reg()) + "." + kComponent[comp];

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
    const std::string a = s + ".x", b = s + ".y", w = s + ".w";
    std::string r;
    switch (op) {
      case Op::kAdds: r = "(" + a + " + " + b + ")"; break;
      case Op::kMuls: r = "XeMul(" + a + ", " + b + ")"; break;
      case Op::kSubs: r = "(" + a + " - " + b + ")"; break;
      case Op::kMaxs: r = "max(" + a + ", " + b + ")"; break;
      case Op::kMins: r = "min(" + a + ", " + b + ")"; break;
      case Op::kSeqs: r = "float(" + a + " == " + b + ")"; break;
      case Op::kSgts: r = "float(" + a + " > " + b + ")"; break;
      case Op::kSges: r = "float(" + a + " >= " + b + ")"; break;
      case Op::kSnes: r = "float(" + a + " != " + b + ")"; break;
      case Op::kAddsPrev: r = "(" + a + " + xe_ps)"; break;
      case Op::kMulsPrev: r = "XeMul(" + a + ", xe_ps)"; break;
      case Op::kSubsPrev: r = "(" + a + " - xe_ps)"; break;
      case Op::kFrcs: r = "frac(" + a + ")"; break;
      case Op::kTruncs: r = "trunc(" + a + ")"; break;
      case Op::kFloors: r = "floor(" + a + ")"; break;
      case Op::kExp: r = "exp2(" + a + ")"; break;
      // log(0) is -inf on the hardware; the clamped form saturates to -FLT_MAX.
      case Op::kLog: r = "log2(abs(" + a + "))"; break;
      case Op::kLogc: r = "max(log2(abs(" + a + ")), -3.402823466e+38)"; break;
      // The three reciprocal forms differ ONLY on what they do with an
      // infinity, and that difference is the whole of this game's black main
      // menu. From the SDK (ucode.h:1082-1114):
      //
      //   RECIP_IEEE  (kRcp)   1/0 = +INF
      //   RECIP_CLAMP (kRcpc)  +INF -> +FLT_MAX, -INF -> -FLT_MAX
      //   RECIP_FF    (kRcpf)  +INF ->  0.0,     -INF -> -0.0
      //
      // kRcpf was emitted as a plain rcp, so it produced +Inf where the console
      // produces zero. Traced in a RenderDoc capture of the menu: the rider's
      // material samples a texture, takes its Rec.709 luminance, and divides by
      // it. The luminance is 0, RECIP_FF should hand back 0 and the term
      // vanishes; instead we handed back +Inf, the next instruction computed
      // 0 * Inf = NaN, and every pixel of the rider and bike came out NaN. NaN
      // then poisoned the composite and blacked out the whole 3D layer.
      //
      // "Fast-forward" is the fixed-function-emulation form and it exists
      // precisely so a shader can divide by a possibly-zero quantity without
      // guarding it. A title relies on that.
      case Op::kRcp: r = "rcp(" + a + ")"; break;
      case Op::kRcpf: r = "XeFlushInf(rcp(" + a + "))"; break;
      case Op::kRcpc:
        r = "clamp(rcp(" + a + "), -3.402823466e+38, 3.402823466e+38)";
        break;
      case Op::kRsq: r = "rsqrt(abs(" + a + "))"; break;
      case Op::kRsqf: r = "XeFlushInf(rsqrt(abs(" + a + ")))"; break;
      case Op::kRsqc:
        r = "min(rsqrt(abs(" + a + ")), 3.402823466e+38)";
        break;
      case Op::kSqrt: r = "sqrt(" + a + ")"; break;
      case Op::kSin: r = "sin(" + a + ")"; break;
      case Op::kCos: r = "cos(" + a + ")"; break;
      case Op::kMaxAs:
        // As kMaxA, but the address source is src0.x rather than src0.w.
        Line("xe_a0 = (int)clamp(floor(" + a + " + 0.5), -256.0, 255.0);");
        r = "max(" + a + ", " + w + ")";
        break;
      case Op::kMaxAsf:
        // The floor variant: truncates toward negative infinity, same clamp.
        Line("xe_a0 = (int)clamp(floor(" + a + "), -256.0, 255.0);");
        r = "max(" + a + ", " + w + ")";
        break;
      case Op::kRetainPrev: r = "xe_ps"; break;
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
        // The setp family — predicate machinery, none of it seen here.
        status = HlslStatus::kUnsupportedScalarOp;
        blocking_opcode = uint32_t(op);
        return "0.0";
    }
    if (alu.scalar_clamp()) r = "saturate(" + r + ")";
    return r;
  }

  void EmitAlu(const uc::AluInstruction& alu) {
    // The raw vector/scalar mask fields overlap with the export constant 0/1
    // encoding, so the canonical decoded masks are used for both temp and
    // export writes. Treating the raw bits as ordinary masks preserved position
    // in many shaders but turned interpolator exports into zero.
    const uint32_t vmask = alu.GetVectorOpResultWriteMask();
    const uint32_t smask = alu.GetScalarOpResultWriteMask();
    const bool is_export = alu.is_export();
    // VectorOp still runs when vmask is 0 and this is an export, because
    // kMaxA's address-register side effect depends on it; nothing consumes the
    // value in that case.
    const bool has_vector = vmask != 0 || is_export;

    std::string vexpr, sexpr;
    if (has_vector) vexpr = VectorOp(alu, vmask != 0);
    if (status != HlslStatus::kOk) return;
    if (smask != 0) sexpr = ScalarOp(alu);
    if (status != HlslStatus::kOk) return;

    // Both halves read the register file before either writes — the co-issue
    // semantics. Evaluating into locals first is what preserves that; assigning
    // the vector result straight into r[] would let the scalar half read the
    // new value.
    if (vmask != 0) Line("xe_v = " + vexpr + ";");
    if (smask != 0) Line("xe_s = " + sexpr + "; xe_ps = xe_s;");

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
        // interpolator past the agreed linkage width. Dropped, not faked — but
        // the ALU side effects above have already been emitted.
        return;
      }
      const uint32_t zero_mask = alu.GetConstant0WriteMask();
      const uint32_t one_mask = alu.GetConstant1WriteMask();
      if (!(vmask | smask | zero_mask | one_mask)) return;
      // Priority matches the interpreter: vector, then scalar, then the
      // constant 1 and constant 0 encodings.
      uint32_t taken = 0;
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

    // Destinations name temp registers, so a relative destination is
    // aL-relative for the same reason a relative temp source is. (In the export
    // path above, is_scalar_dest_relative is not an index at all — it is the
    // "write unwritten components as 0" flag — which is why this check lives
    // here and not at the top.)
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
    // 1D joins 2D: a single-row texture samples correctly through an ordinary
    // 2D binding, so it needs no new resource type -- only the v coordinate
    // pinned to the row, below.
    //
    // CUBE joins them through a Texture2DArray, because the guest has already
    // done the cube projection in software before it gets here. The hardware
    // sequence is
    //
    //     cube       r0, source.zzxy, source.yxz   // (T, S, 2*majoraxis, face)
    //     rcp        r0.z, r0_abs.z
    //     mad        r0.xy, r0, r0.zzzw, 1.5f      // ST biased into [1,2)
    //     tfetchCube r0, r0.yxw, tf0               // samples (S, T, face)
    //
    // so tfetchCube receives three scalars, never a direction vector -- which is
    // the whole reason `cube` exists as an ALU opcode. Binding a real TextureCube
    // would mean UNDOING that projection so the hardware could redo it; an array
    // consumes the guest's output as it stands. Faces are six ordinary 2D images
    // at 4 KB-aligned strides in guest memory, so the decode is the 2D one run
    // six times. Xenos does not filter across face edges either, so the one thing
    // this gives up costs nothing against the console.
    //
    // 3D/stacked still needs a real Texture3D or a stack-sized array and is
    // refused by name.
    using Dim = rex::graphics::xenos::FetchOpDimension;
    const bool is_cube = tf.dimension() == Dim::kCube;
    if (!is_cube && tf.dimension() != Dim::k2D && tf.dimension() != Dim::k1D) {
      status = HlslStatus::kFetch3D;
      blocking_opcode = uint32_t(tf.opcode());
      return;
    }
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
    // match both of its fetches.
    const uint32_t slot_bit = 1u << slot;
    if ((slot_fetched_mask & slot_bit) &&
        bool(slot_array_mask & slot_bit) != is_cube) {
      status = HlslStatus::kFetchDimensionConflict;
      blocking_opcode = uint32_t(tf.opcode());
      return;
    }
    slot_fetched_mask |= slot_bit;
    if (is_cube) {
      slot_array_mask |= slot_bit;
      if (!uses_cube) cube_fetch_without_cube_op = true;
    }

    // The fetch source swizzle is ABSOLUTE, two bits per component — unlike an
    // ALU swizzle, which is component-relative. Reading it the ALU way sends
    // every fetch to the wrong coordinate pair.
    const uint32_t swiz = tf.src_swizzle();
    const bool is_1d = tf.dimension() == rex::graphics::xenos::FetchOpDimension::k1D;
    std::string uv;
    uv += kComponent[swiz & 3];
    if (!is_1d) uv += kComponent[(swiz >> 2) & 3];

    if (is_cube) {
      // The third source component is the face index the `cube` op produced,
      // 0-5 in D3DCUBEMAP_FACES order — which is also the array slice order the
      // guest stores the faces in, so it indexes the array directly.
      //
      // ST arrive biased into [1,2) by the `mad ..., 1.5` above: that bias is
      // what the hardware fetch expects, not a shader idiom, so subtracting one
      // is decoding the operand rather than guessing at a convention.
      //
      // xe_texinv is deliberately NOT applied. It normalises TEXEL coordinates,
      // and these are already face-relative.
      const std::string src = Temp(tf.src());
      const std::string face = std::string(1, kComponent[(swiz >> 4) & 3]);
      const std::string s3 = std::to_string(slot);
      Line("xe_v = xe_tex" + s3 + ".Sample(xe_smp" + s3 + ", float3(" + src +
           "." + uv + " - 1.0, " + src + "." + face + "));");
      EmitFetchDestination(tf);
      return;
    }

    std::string coord = Temp(tf.src()) + "." + uv;
    if (tf.unnormalized_coordinates()) {
      // `tx_coord_denorm` means the guest addresses this texture in TEXELS;
      // the host sampler wants normalized coordinates, so this divides by the
      // extent. It arrives pre-reciprocated as xe_texinv, both to keep the
      // divide off the per-pixel path and so that "no texture bound" can be
      // encoded as 0 — which reads texel 0, rather than the infinity a
      // division by a zero extent would produce.
      //
      // This used to MULTIPLY by the extent, leaving every unnormalized fetch
      // wrong by the square of the texture size.
      coord = "(" + coord + " * xe_texinv[" + std::to_string(slot) +
              (is_1d ? "].x)" : "].xy)");
    }
    if (is_1d) {
      // A Xenos 1D texture is one row, described to the host as width x 1, so
      // the row centre is v = 0.5 in normalized space whatever the width.
      coord = "float2(" + coord + ", 0.5)";
    }
    const std::string s = std::to_string(slot);
    Line("xe_v = xe_tex" + s + ".Sample(xe_smp" + s + ", " + coord + ");");
    EmitFetchDestination(tf);
  }

  // Write xe_v out through the fetch's destination swizzle. Shared by the 1D/2D
  // and cube paths, which differ only in how they build the coordinate.
  void EmitFetchDestination(const uc::TextureFetchInstruction& tf) {
    // The destination swizzle is three bits per component: 0-3 select x/y/z/w
    // of the fetched value, 4 is 0.0, 5 is 1.0, and 7 means keep — which is what
    // lets two fetches share a destination register, so ignoring it would
    // clobber.
    const uint32_t dswiz = tf.dest_swizzle();
    const std::string dst = Temp(tf.dest());
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
    MarkWritten(tf.dest());
  }

 private:
  HlslStage stage_;
  uint32_t interpolators_;
};

}  // namespace

bool EmitShaderHlsl(const uint32_t* dwords, uint32_t dword_count,
                    HlslStage stage, uint32_t interpolator_count,
                    HlslShader& out) {
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

  Emitter em(stage, interpolator_count);
  uint32_t executed = 0;

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
          if ((dwords[at] & 0x1F) != uint32_t(uc::FetchOpcode::kTextureFetch))
            continue;  // a vertex fetch; the layout path owns those
          uc::TextureFetchInstruction tf{};
          std::memcpy(&tf, dwords + at, sizeof(tf));
          em.EmitTextureFetch(tf);
        } else {
          uc::AluInstruction alu{};
          std::memcpy(&alu, dwords + at, sizeof(alu));
          em.EmitAlu(alu);
        }
        if (em.status != HlslStatus::kOk) {
          out.status = em.status;
          out.blocking_opcode = em.blocking_opcode;
          return false;
        }
      }
    }
  }

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
  // Emitting both at b1 would mean the vertex stage reading the pixel bank, or
  // the two stages fighting over one binding.
  src += "cbuffer XeShaderConstants : register(";
  src += stage == HlslStage::kPixel ? "b1" : "b0";
  src += ") {\n";
  src += "  float4 xe_c[256];\n";
  // Indexed by compact slot, not by guest sampler — same remap as the textures.
  // Holds the RECIPROCAL of the bound texture's extent (0 when none); see the
  // unnormalized-coordinate note in EmitTextureFetch.
  src += "  float4 xe_texinv[" + std::to_string(HlslShader::kMaxSamplerSlots) +
         "];\n";
  src += "};\n";
  // RECIP_FF / RECIPSQ_FF: an infinity becomes a signed zero. See the note at
  // the kRcpf case -- these are the "fast-forward" forms a title uses when it
  // wants to divide by a possibly-zero quantity and have the term vanish
  // instead of poisoning the result. Emitted unconditionally: it is four lines,
  // and gating it on whether the shader happens to use one is a way for the two
  // to drift apart.
  //
  // `isinf` rather than a comparison against FLT_MAX: rcp of a denormal can
  // overflow to infinity without the input being zero, and the hardware flushes
  // that case too.
  src +=
      "float XeFlushInf(float v) {\n"
      "  return isinf(v) ? (v < 0.0 ? -0.0 : 0.0) : v;\n"
      "}\n";
  // Direct3D 9 "legacy" multiply, which is what every multiplying operation on
  // this hardware does -- mul, mad, the dot products, and their scalar forms.
  // Quoting the SDK (ucode.h, above AluScalarOpcode):
  //
  //   Direct3D 9 rules (like in GCN v_*_legacy_f32 instructions) for
  //   multiplication (+-0 or denormal * anything = +0) wherever it's present
  //   (mul, mad, dp, etc.) [...] It's very important to respect this rule for
  //   multiplication, as games often rely on it in vector normalization (rcp
  //   and mul), Infinity * 0 resulting in NaN breaks a lot of things in games
  //   - causes white screen [...], white specular on characters [...]. The
  //   result is always positive zero in this case, no matter what the signs of
  //   the other operands are.
  //
  // That is this game's menu exactly. The light-prepass materials sample the
  // screen-space light buffer, take its Rec.709 luminance, `rcp` it, and scale
  // colour*colour by the result -- the standard light-prepass specular
  // reconstruction. Where the light buffer is black the hardware computes
  // 0 * INF = +0 and the specular term simply vanishes; we computed NaN and
  // every pixel of the rider and the bike came out NaN.
  //
  // Only exact zero is tested, not denormals: the hardware flushes denormal
  // inputs to zero before the multiply, and so does every host GPU we target.
  //
  // mad stays unfused and is emitted as XeMul(a, b) + c rather than as a select
  // on c -- per the same note, +0 + -0 is +0, so a zero multiplicand must still
  // go through the add.
  src +=
      "float XeMul(float a, float b) {\n"
      "  return (a == 0.0 || b == 0.0) ? 0.0 : a * b;\n"
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
  // Declared for EVERY slot up to sampler_count, contiguously from t0/s0, so
  // the registers match a descriptor table of exactly that width.
  //
  // A slot fetched as a cube is declared Texture2DArray: the guest projects the
  // direction to (S, T, face) itself, so an array indexed by face is what its
  // operands already describe. BindTranslatedTextures must give that slot a
  // TEXTURE2DARRAY SRV to match — see HlslShader::sampler_array_mask.
  for (uint32_t s = 0; s < em.sampler_count; ++s) {
    const std::string n = std::to_string(s);
    const bool array = (em.slot_array_mask >> s) & 1u;
    src += array ? "Texture2DArray<float4> xe_tex" : "Texture2D<float4> xe_tex";
    src += n + " : register(t" + n + ");\n";
    src += "SamplerState xe_smp" + n + " : register(s" + n + ");\n";
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
    src += "XePsOut main(XeInterpolants xe_in) {\n";
  } else {
    // A vertex shader's inputs are its vfetch destinations. The fetches
    // themselves are not emitted here — the host input assembler performs them,
    // driven by the layout DecodeVertexShaderFetches already recovers — so each
    // register the body reads before writing becomes one input element. The
    // caller must build its input layout from input_mask, in this same order,
    // or the semantics will not match.
    src += "struct XeVsIn {\n";
    for (uint32_t i = 0; i < kNumTemps && i < 32; ++i) {
      if (!(em.input_mask & (1u << i))) continue;
      const std::string n = std::to_string(i);
      src += "  float4 v" + n + " : TEXCOORD" + n + ";\n";
    }
    src += "};\n";
    src += "XeInterpolants main(XeVsIn xe_in) {\n";
  }

  src += "  float4 r[" + std::to_string(kNumTemps) + "];\n";
  src += "  [unroll] for (int xe_i = 0; xe_i < " + std::to_string(kNumTemps) +
         "; ++xe_i) r[xe_i] = float4(0, 0, 0, 0);\n";
  src += "  float4 xe_v = float4(0, 0, 0, 0);\n";
  src += "  float xe_s = 0.0, xe_ps = 0.0;\n";
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
    for (uint32_t t = 0; t < kMaxColorTargets; ++t) {
      if (!(em.color_mask & (1u << t))) continue;
      src += "  float4 xe_color" + std::to_string(t) + " = float4(0,0,0,0);\n";
    }
    if (em.writes_depth) src += "  float xe_depth = 0.0;\n";
  } else {
    for (uint32_t i = 0; i < kNumTemps && i < 32; ++i) {
      if (!(em.input_mask & (1u << i))) continue;
      src += "  r[" + std::to_string(i) + "] = xe_in.v" + std::to_string(i) +
             ";\n";
    }
    src += "  float4 xe_pos = float4(0, 0, 0, 1);\n";
    for (uint32_t i = 0; i < link; ++i)
      src += "  float4 xe_o" + std::to_string(i) + " = float4(0, 0, 0, 0);\n";
  }

  src += em.body;

  if (stage == HlslStage::kPixel) {
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
  out.writes_position = em.writes_position;
  out.writes_depth = em.writes_depth;
  out.max_const_index = em.max_const_index;
  out.reads_constants = em.reads_constants;
  return true;
}

}  // namespace mx::hle
