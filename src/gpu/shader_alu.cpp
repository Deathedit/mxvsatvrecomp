#include "gpu/shader_alu.h"

#include <cmath>
#include <cstring>

#include <rex/graphics/format/ucode.h>

namespace uc = rex::graphics::ucode;

namespace mx::hle {

const char* AluStatusName(AluStatus s) {
  switch (s) {
    case AluStatus::kOk: return "ok";
    case AluStatus::kMalformed: return "malformed";
    case AluStatus::kNoPositionExport: return "no position export";
    case AluStatus::kUnsupportedCf: return "unsupported control flow";
    case AluStatus::kUnsupportedVectorOp: return "unsupported vector op";
    case AluStatus::kUnsupportedScalarOp: return "unsupported scalar op";
    case AluStatus::kLoopRelative: return "aL-relative (loop)";
    case AluStatus::kInstructionCap: return "instruction cap";
  }
  return "?";
}

namespace {

constexpr uint32_t kMaxInstructions = 2048;
constexpr uint32_t kNumTemps = 64;

struct Vec4 {
  float v[4] = {0, 0, 0, 0};
  float& operator[](size_t i) { return v[i]; }
  float operator[](size_t i) const { return v[i]; }
};

// Same exec-accessor dispatch as the decoder: address/count/sequence sit at the
// same bit offsets in all three exec structs but are distinct union members.
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

// Control flow we refuse rather than approximate. The decoder can afford to
// walk every exec once and over-approximate the attribute set, because a
// superset of the fetches is still a correct layout. Execution cannot: taking
// a branch that the real shader would not, or running a loop body once when it
// runs eight times, produces a confidently wrong position. Every shader
// captured from this game so far is a straight run of execs, so refusing costs
// nothing today and stays honest if that changes.
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

class Interpreter {
 public:
  Interpreter(const AluInputs& in) : in_(in) {}

  Vec4 temps[kNumTemps];
  Vec4 position;
  bool wrote_position = false;
  std::array<Vec4, AluResult::kMaxInterpolators> exports = {};
  uint32_t export_mask = 0;

  // The address register. Written by the maxa family, read by every relative
  // constant, source and destination index. Zero until something writes it,
  // which matches the hardware — a shader that indexes relatively without
  // writing a0 first reads c[0], it does not fault.
  int32_t a0_ = 0;

  AluStatus status = AluStatus::kOk;
  uint32_t blocking_opcode = 0;

  // Every constant read in the shader passes through here — plain indexed
  // reads, a0-relative ones, and the mulsc/addsc/subsc family alike — so it is
  // the one place that can say what the shader asked the constant file for.
  //
  // The index is relative to whatever region the vertex stage is based at.
  // SQ_VS_CONST names that base and the caller is responsible for applying it
  // before handing `alu_consts` over; this reads from the pointer it is given.
  // Out of range reads zero rather than failing: an unwritten constant really
  // is zero on the hardware, and a shader that indexes past its region reads
  // zero rather than faulting.
  //
  // The counters exist because a shader can execute perfectly and still export
  // (0,0,0,w=0) — 19% of them do — and that is computing nothing *from
  // something*. Whether the something was there is the question, and which
  // slots it wanted is what names the gap if it was not. Counters are mutable
  // so this stays a read accessor.
  Vec4 Const(uint32_t index) const {
    Vec4 r;
    const uint32_t base = index * 4;
    const bool out_of_range = !in_.alu_consts || base + 4 > in_.alu_const_dwords;
    if (!out_of_range) {
      for (int i = 0; i < 4; ++i)
        std::memcpy(&r[i], &in_.alu_consts[base + i], 4);
    }
    // The value is produced either way; only the accounting is conditional.
    if (counting_) {
      ++const_reads;
      if (index < const_min_index) const_min_index = index;
      if (index > const_max_index) const_max_index = index;
      if (out_of_range ||
          (r[0] == 0.0f && r[1] == 0.0f && r[2] == 0.0f && r[3] == 0.0f))
        ++const_zero_reads;
    }
    return r;
  }

  // Whether a constant read should be counted. NOT whether it happens — the
  // value is still produced, this only governs the instrumentation.
  //
  // An operand the opcode does not consume still has a register field, holding
  // whatever the assembler left there, and 0xFF is common. Counting those is
  // what produced a long-running "this shader reads c255 and gets zero" signal:
  // c255 is genuinely unwritten (nothing publishes it, see AGENTS.md), so every
  // unused operand looked like a real read returning zero.
  mutable bool counting_ = true;
  mutable uint32_t const_reads = 0;
  mutable uint32_t const_zero_reads = 0;
  mutable uint32_t const_min_index = 0xFFFFFFFF;
  mutable uint32_t const_max_index = 0;

  // One source operand, swizzled, absolute-valued and negated as the
  // instruction asks. Swizzles are component-relative on Xenos — component c
  // reads ((swizzle >> 2c) + c) & 3 — which is why a swizzle of 0 is identity
  // rather than xxxx.
  Vec4 Src(const uc::AluInstruction& alu, size_t i) {
    const uint32_t reg = alu.src_reg(i);
    const uint32_t swiz = alu.src_swizzle(i);
    const bool is_temp = alu.src_is_temp(i);

    Vec4 base;
    bool absolute = false;
    if (is_temp) {
      // A relative temp index is aL-relative, never a0-relative — the register
      // file simply has no a0 addressing mode. So modelling a0 does not help
      // here and this stays refused.
      if (uc::AluInstruction::is_src_temp_relative(reg)) {
        status = AluStatus::kLoopRelative;
        return base;
      }
      base = temps[uc::AluInstruction::src_temp_reg(reg) & (kNumTemps - 1)];
      absolute = uc::AluInstruction::is_src_temp_value_absolute(reg);
    } else {
      uint32_t index = reg & 0xFF;
      if (alu.src_const_is_addressed(i)) {
        // Relative to a0 or to aL, and the instruction says which. The accessor
        // name is the decoder: "address register relative" means relative to
        // *the address register*, which is a0. AddressingMode spells the
        // mapping out — kAbsolute = 1 = c[a0 + n], kRelative = 0 = c[aL + n]
        // (ucode.h:191-199).
        //
        // This condition used to be written without the '!', which is exactly
        // backwards: it refused every a0 read — the case this interpreter
        // implements — and applied a0_ to every aL read, a case that never
        // occurs in this game. The a0 arithmetic below was therefore never once
        // executed, which is why modelling a0 measured as converting zero
        // failures. That null was the bug, not a fact about the game.
        //
        // aL is the loop counter, and we walk every exec block once rather than
        // unrolling, so there is no honest value for it. Refused, not guessed.
        // Expect this to fire ~0 times here: kUnsupportedCf is 0 across every
        // sample taken, meaning no shader in this game contains a loop at all.
        // The two counts cross-check each other and should move together.
        if (!alu.is_const_address_register_relative()) {
          status = AluStatus::kLoopRelative;
          return base;
        }
        index = (index + uint32_t(a0_)) & 0xFF;
      }
      base = Const(index);
      absolute = alu.abs_constants();
    }

    Vec4 out;
    for (uint32_t c = 0; c < 4; ++c) {
      float f = base[uc::AluInstruction::GetSwizzledComponentIndex(swiz, c)];
      if (absolute) f = std::fabs(f);
      out[c] = f;
    }
    if (alu.src_negate(i)) {
      for (uint32_t c = 0; c < 4; ++c) out[c] = -out[c];
    }
    return out;
  }

  static float Saturate(float f) { return f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f); }

  // a0 is a signed 9-bit register: the hardware clamps to [-256, 255]. A NaN
  // input lands on 0 rather than propagating, since the register is an index.
  static int32_t ClampAddress(float f) {
    if (!(f > -256.0f)) return std::isnan(f) ? 0 : -256;
    if (f > 255.0f) return 255;
    return int32_t(f);
  }
  // maxa / maxas write floor(x + 0.5) — round to nearest, halves upward — and
  // then clamp. maxasf floors instead, and calls ClampAddress directly.
  static int32_t SetAddressRegister(float f) {
    return ClampAddress(std::floor(f + 0.5f));
  }

  // Vector opcodes reading src1 only. src2 is still evaluated below — the
  // result is discarded — but must not be counted as a constant read.
  static bool VectorOpUsesSrc2(uc::AluVectorOpcode op) {
    using Op = uc::AluVectorOpcode;
    switch (op) {
      case Op::kFrc:
      case Op::kTrunc:
      case Op::kFloor:
      case Op::kMax4:
        return false;
      default:
        // kMaxA and kDst do read src2, as does everything else handled here.
        return true;
    }
  }

  // Direct3D 9 legacy multiply: a zero multiplicand yields +0 regardless of the
  // other operand, so 0 * INF is +0 and not NaN. Every multiplying operation on
  // this hardware behaves this way -- see the note above AluScalarOpcode in the
  // SDK's ucode.h, and the matching XeMul in shader_hlsl.cpp. Titles rely on it
  // for vector normalisation written as rcp-then-mul.
  static float LegacyMul(float a, float b) {
    return (a == 0.0f || b == 0.0f) ? 0.0f : a * b;
  }

  Vec4 VectorOp(const uc::AluInstruction& alu) {
    Vec4 r;
    using Op = uc::AluVectorOpcode;
    const Op op = alu.vector_opcode();
    const Vec4 a = Src(alu, 1);
    const bool count_src2 = counting_ && VectorOpUsesSrc2(op);
    const bool outer = counting_;
    counting_ = count_src2;
    const Vec4 b = Src(alu, 2);
    counting_ = outer;
    // src3 is only read where the opcode actually has a third operand — the
    // same discipline the export tracer needed, for the same reason.
    switch (op) {
      case Op::kAdd: for (int c = 0; c < 4; ++c) r[c] = a[c] + b[c]; break;
      case Op::kMul: for (int c = 0; c < 4; ++c) r[c] = LegacyMul(a[c], b[c]); break;
      case Op::kMax: for (int c = 0; c < 4; ++c) r[c] = a[c] >= b[c] ? a[c] : b[c]; break;
      case Op::kMin: for (int c = 0; c < 4; ++c) r[c] = a[c] < b[c] ? a[c] : b[c]; break;
      case Op::kSeq: for (int c = 0; c < 4; ++c) r[c] = a[c] == b[c] ? 1.0f : 0.0f; break;
      case Op::kSgt: for (int c = 0; c < 4; ++c) r[c] = a[c] > b[c] ? 1.0f : 0.0f; break;
      case Op::kSge: for (int c = 0; c < 4; ++c) r[c] = a[c] >= b[c] ? 1.0f : 0.0f; break;
      case Op::kSne: for (int c = 0; c < 4; ++c) r[c] = a[c] != b[c] ? 1.0f : 0.0f; break;
      case Op::kFrc: for (int c = 0; c < 4; ++c) r[c] = a[c] - std::floor(a[c]); break;
      case Op::kTrunc: for (int c = 0; c < 4; ++c) r[c] = std::trunc(a[c]); break;
      case Op::kFloor: for (int c = 0; c < 4; ++c) r[c] = std::floor(a[c]); break;
      case Op::kMaxA:
        // max, plus the address-register side effect. Modelling it is what lets
        // Src() resolve relative constant reads instead of refusing them.
        a0_ = SetAddressRegister(a[3]);
        for (int c = 0; c < 4; ++c) r[c] = a[c] >= b[c] ? a[c] : b[c];
        break;
      case Op::kMad: {
        const Vec4 c3 = Src(alu, 3);
        for (int c = 0; c < 4; ++c) r[c] = LegacyMul(a[c], b[c]) + c3[c];
        break;
      }
      case Op::kCndEq: {
        const Vec4 c3 = Src(alu, 3);
        for (int c = 0; c < 4; ++c) r[c] = a[c] == 0.0f ? b[c] : c3[c];
        break;
      }
      case Op::kCndGe: {
        const Vec4 c3 = Src(alu, 3);
        for (int c = 0; c < 4; ++c) r[c] = a[c] >= 0.0f ? b[c] : c3[c];
        break;
      }
      case Op::kCndGt: {
        const Vec4 c3 = Src(alu, 3);
        for (int c = 0; c < 4; ++c) r[c] = a[c] > 0.0f ? b[c] : c3[c];
        break;
      }
      case Op::kDp4: {
        float d = 0;
        for (int c = 0; c < 4; ++c) d += LegacyMul(a[c], b[c]);
        for (int c = 0; c < 4; ++c) r[c] = d;
        break;
      }
      case Op::kDp3: {
        float d = 0;
        for (int c = 0; c < 3; ++c) d += LegacyMul(a[c], b[c]);
        for (int c = 0; c < 4; ++c) r[c] = d;
        break;
      }
      case Op::kDp2Add: {
        const Vec4 c3 = Src(alu, 3);
        const float d = LegacyMul(a[0], b[0]) + LegacyMul(a[1], b[1]) + c3[0];
        for (int c = 0; c < 4; ++c) r[c] = d;
        break;
      }
      case Op::kMax4: {
        float m = a[0];
        for (int c = 1; c < 4; ++c) if (a[c] > m) m = a[c];
        for (int c = 0; c < 4; ++c) r[c] = m;
        break;
      }
      case Op::kDst:
        r[0] = 1.0f; r[1] = LegacyMul(a[1], b[1]); r[2] = a[2]; r[3] = b[3];
        break;
      default:
        // kCube, the setp_*_push family and the kill_* family. The kills are
        // pixel-shader operations and the pushes are predicate machinery; none
        // has appeared in a vertex shader captured here. Reported, not guessed.
        status = AluStatus::kUnsupportedVectorOp;
        blocking_opcode = uint32_t(op);
        break;
    }
    if (alu.vector_clamp()) {
      for (int c = 0; c < 4; ++c) r[c] = Saturate(r[c]);
    }
    return r;
  }

  // The mulsc/addsc/subsc family, opcodes 42..47. These do not use the normal
  // operand encoding, so they must not go through Src(): src3 names a constant
  // *register* directly, and the temp register it multiplies against is
  // scattered — one bit lives in the opcode field itself, which is why each
  // operation has a _0 and a _1 form. The SDK reassembles it for us in
  // scalar_const_reg_op_src_temp_reg().
  //
  // Both operands are scalars, selected by the low two bits of src3_swiz. The
  // constant's negate bit still applies; the temp's does not, there being no
  // field for it.
  bool ConstRegScalarOp(const uc::AluInstruction& alu, float& out) {
    using Op = uc::AluScalarOpcode;
    const Op op = alu.scalar_opcode();
    if (op < Op::kMulsc0 || op > Op::kSubsc1) return false;

    const uint32_t comp = alu.src_swizzle(3) & 3;
    Vec4 cv = Const(alu.src_reg(3) & 0xFF);
    float a = cv[comp];
    if (alu.abs_constants()) a = std::fabs(a);
    if (alu.src_negate(3)) a = -a;

    const float b =
        temps[alu.scalar_const_reg_op_src_temp_reg() & (kNumTemps - 1)][comp];

    switch (op) {
      case Op::kMulsc0: case Op::kMulsc1: out = LegacyMul(a, b); break;
      case Op::kAddsc0: case Op::kAddsc1: out = a + b; break;
      default:                            out = a - b; break;  // subsc0/1
    }
    return true;
  }

  float ScalarOp(const uc::AluInstruction& alu) {
    using Op = uc::AluScalarOpcode;
    const Op op = alu.scalar_opcode();

    // Handled ahead of the switch because their operands are not Src()-shaped.
    if (float cr = 0.0f; ConstRegScalarOp(alu, cr)) {
      if (alu.scalar_clamp()) cr = Saturate(cr);
      ps_ = cr;
      return cr;
    }

    const Vec4 s = Src(alu, 3);
    // Two-operand scalar ops take x and y of the swizzled operand; one-operand
    // ops take x, and a few also read w.
    const float a = s[0], b = s[1], w = s[3];
    float r = 0.0f;
    switch (op) {
      case Op::kAdds: r = a + b; break;
      case Op::kMuls: r = LegacyMul(a, b); break;
      case Op::kSubs: r = a - b; break;
      case Op::kMaxs: r = a >= b ? a : b; break;
      case Op::kMins: r = a < b ? a : b; break;
      case Op::kSeqs: r = a == b ? 1.0f : 0.0f; break;
      case Op::kSgts: r = a > b ? 1.0f : 0.0f; break;
      case Op::kSges: r = a >= b ? 1.0f : 0.0f; break;
      case Op::kSnes: r = a != b ? 1.0f : 0.0f; break;
      case Op::kAddsPrev: r = a + ps_; break;
      case Op::kMulsPrev: r = LegacyMul(a, ps_); break;
      case Op::kSubsPrev: r = a - ps_; break;
      case Op::kFrcs: r = a - std::floor(a); break;
      case Op::kTruncs: r = std::trunc(a); break;
      case Op::kFloors: r = std::floor(a); break;
      case Op::kExp: r = std::exp2(a); break;
      case Op::kLog: case Op::kLogc:
        r = a == 0.0f ? -INFINITY : std::log2(std::fabs(a));
        if (op == Op::kLogc && std::isinf(r)) r = -3.402823466e+38f;
        break;
      // The three forms differ only on an infinity, and treating the FF form
      // as IEEE is what blacked out the menu on the HLSL side -- see the note
      // beside kRcpf in shader_hlsl.cpp. Corrected here too so the interpreter
      // and the emitter cannot disagree about a shader they both run.
      //   RECIP_IEEE  +INF          RECIP_CLAMP  +/-FLT_MAX   RECIP_FF  +/-0.0
      case Op::kRcp: case Op::kRcpc: case Op::kRcpf:
        r = a == 0.0f ? INFINITY : 1.0f / a;
        if (op == Op::kRcpc && std::isinf(r)) r = r > 0 ? 3.402823466e+38f : -3.402823466e+38f;
        if (op == Op::kRcpf && std::isinf(r)) r = r > 0 ? 0.0f : -0.0f;
        break;
      case Op::kRsq: case Op::kRsqc: case Op::kRsqf:
        r = a == 0.0f ? INFINITY : 1.0f / std::sqrt(std::fabs(a));
        if (op == Op::kRsqc && std::isinf(r)) r = 3.402823466e+38f;
        if (op == Op::kRsqf && std::isinf(r)) r = r > 0 ? 0.0f : -0.0f;
        break;
      case Op::kSqrt: r = std::sqrt(a); break;
      case Op::kSin: r = std::sin(a); break;
      case Op::kCos: r = std::cos(a); break;
      case Op::kMaxAs:
        // As kMaxA, but the address source is src0.x rather than src0.w.
        a0_ = SetAddressRegister(a);
        r = a >= w ? a : w;
        break;
      case Op::kMaxAsf:
        // The "floor" variant: truncates toward negative infinity instead of
        // rounding to nearest. Same clamp.
        a0_ = ClampAddress(std::floor(a));
        r = a >= w ? a : w;
        break;
      case Op::kRetainPrev: r = ps_; break;
      default:
        // The setp and kill families — predicate machinery and pixel-shader
        // operations. The mulsc/addsc/subsc family used to land here too; it is
        // now handled above, ahead of this switch.
        status = AluStatus::kUnsupportedScalarOp;
        blocking_opcode = uint32_t(op);
        return ps_;
    }
    if (alu.scalar_clamp()) r = Saturate(r);
    ps_ = r;
    return r;
  }

  void Execute(const uc::AluInstruction& alu) {
    // The raw vector/scalar mask fields overlap with the export constant 0/1
    // encoding. Use the canonical decoded masks for both temp and export writes.
    // Treating the raw bits as ordinary masks happened to preserve position in
    // many shaders, but turned interpolator exports (including UV r0) into zero.
    const uint32_t vmask = alu.GetVectorOpResultWriteMask();
    const uint32_t smask = alu.GetScalarOpResultWriteMask();

    // Evaluate both halves. They read the register file before either writes,
    // which is the co-issue semantics.
    Vec4 vres;
    bool in_counting = true;
    float sres = ps_;
    const bool has_vector = vmask != 0 || alu.is_export();
    if (has_vector) {
      // VectorOp still runs when vmask is 0 and this is an export — kMaxA's
      // address-register side effect depends on it. But nothing consumes vres
      // in that case (the write loop only takes it under `vmask & bit`), so the
      // operands are not counted. An export writing only constant 0/1, such as
      // position.w = 1, is exactly this shape and is very common.
      in_counting = counting_;
      counting_ = vmask != 0;
      vres = VectorOp(alu);
      counting_ = in_counting;
    }
    if (smask != 0) sres = ScalarOp(alu);
    if (status != AluStatus::kOk) return;

    if (alu.is_export()) {
      const uint32_t dest = alu.vector_dest();
      Vec4* target = nullptr;
      if (dest == kPositionExportRegister) {
        wrote_position = true;
        target = &position;
      } else if (dest < AluResult::kMaxInterpolators) {
        export_mask |= 1u << dest;
        target = &exports[dest];
      }
      if (!target) return;
      const uint32_t zero_mask = alu.GetConstant0WriteMask();
      const uint32_t one_mask = alu.GetConstant1WriteMask();
      const uint32_t written_mask = vmask | smask | zero_mask | one_mask;
      if (!written_mask) return;
      for (uint32_t c = 0; c < 4; ++c) {
        const uint32_t bit = 1u << c;
        if (vmask & bit) (*target)[c] = vres[c];
        else if (smask & bit) (*target)[c] = sres;
        else if (one_mask & bit) (*target)[c] = 1.0f;
        else if (zero_mask & bit) (*target)[c] = 0.0f;
      }
      return;
    }

    // Destinations name temp registers, so a relative destination is
    // aL-relative for the same reason a relative temp source is. Refused, not
    // approximated. (In the export path above, is_scalar_dest_relative is not
    // an index at all — it is the "write unwritten components as 0" flag —
    // which is why this check lives here and not at the top of Execute.)
    if (alu.is_vector_dest_relative() || alu.is_scalar_dest_relative()) {
      status = AluStatus::kLoopRelative;
      return;
    }
    if (vmask) {
      Vec4& d = temps[alu.vector_dest() & (kNumTemps - 1)];
      for (uint32_t c = 0; c < 4; ++c) if ((vmask >> c) & 1) d[c] = vres[c];
    }
    if (smask) {
      Vec4& d = temps[alu.scalar_dest() & (kNumTemps - 1)];
      for (uint32_t c = 0; c < 4; ++c) if ((smask >> c) & 1) d[c] = sres;
    }
  }

 private:
  const AluInputs& in_;
  float ps_ = 0.0f;  // previous scalar result, for the *_prev opcodes
};

}  // namespace

AluResult ExecuteVertexShader(
    const uint32_t* dwords, uint32_t dword_count,
    const std::vector<VertexAttribute>& attrs,
    const std::vector<std::array<float, 4>>& attr_values,
    const AluInputs& inputs) {
  AluResult out;
  if (!dwords || dword_count < 3) return out;

  // Bound the CF section exactly as the decoder does: it ends at the lowest
  // exec target, and the bound shrinks as execs are seen.
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
  if (!saw_exec || max_cf_dword == 0) return out;

  Interpreter in(inputs);

  // Called at every exit from here on, including the failure ones, so a refused
  // execution still reports what it managed to read before it stopped. Written
  // as an explicit call rather than an RAII guard on purpose: without NRVO the
  // return value is copied at the return statement, *before* local destructors
  // run, so a destructor writing into `out` would silently lose the counters.
  auto finish = [&in, &out]() -> AluResult& {
    out.const_reads = in.const_reads;
    out.const_zero_reads = in.const_zero_reads;
    out.const_min_index = in.const_min_index;
    out.const_max_index = in.const_max_index;
    for (size_t r = 0; r < out.exports.size(); ++r) {
      for (size_t c = 0; c < out.exports[r].size(); ++c) {
        out.exports[r][c] = in.exports[r][c];
      }
    }
    out.export_mask = in.export_mask;
    return out;
  };

  // Seed the register file from the vertex fetches. This is what makes the
  // interpreter worth running: the shader reads its inputs from exactly the
  // registers its vfetch instructions named, and the export-62 decode already
  // proved those are the registers the position is built from.
  // Seed the fetched attributes, honouring each vfetch's destination swizzle.
  //
  // Writing all four components unconditionally is wrong, and wrong in a way
  // that hides: two vfetches are allowed to target the SAME register and fill
  // different components of it, which is exactly what a position-plus-texcoord
  // vertex does. The second fetch then overwrote the first outright. Measured
  // in NAT_Farm: attr[1] fmt=31 -> r0 = (22.969, -16.234, 0, 1), a real
  // texcoord, immediately replaced by attr[2] fmt=6 -> r0 = (0, 0, 0, 0). The
  // shader then exported a zero UV and the draw sampled one texel.
  //
  // ucode.h FetchDestinationSwizzle, three bits per destination component:
  //   0..3 = x/y/z/w of the fetched value, 4 = 0.0, 5 = 1.0, 7 = keep (do not
  //   write this component).
  // kKeep is the mechanism that lets two fetches share a register, so ignoring
  // it is what produced the clobber.
  for (size_t i = 0; i < attrs.size() && i < attr_values.size(); ++i) {
    Vec4& d = in.temps[attrs[i].dest_reg & (kNumTemps - 1)];
    const uint32_t swiz = attrs[i].dest_swizzle;
    for (uint32_t c = 0; c < 4; ++c) {
      switch (uc::GetFetchDestinationComponentSwizzle(swiz, c)) {
        case uc::FetchDestinationSwizzle::kX: d[c] = attr_values[i][0]; break;
        case uc::FetchDestinationSwizzle::kY: d[c] = attr_values[i][1]; break;
        case uc::FetchDestinationSwizzle::kZ: d[c] = attr_values[i][2]; break;
        case uc::FetchDestinationSwizzle::kW: d[c] = attr_values[i][3]; break;
        case uc::FetchDestinationSwizzle::k0: d[c] = 0.0f; break;
        case uc::FetchDestinationSwizzle::k1: d[c] = 1.0f; break;
        default: break;  // kKeep, and the one undefined encoding
      }
    }
  }

  uint32_t executed = 0;
  for (uint32_t i = 0; i + 2 < max_cf_dword; i += 3) {
    uc::ControlFlowInstruction cf[2];
    uc::UnpackControlFlowInstructions(dwords + i, cf);
    for (int j = 0; j < 2; ++j) {
      if (IsUnsupportedCf(cf[j].opcode())) {
        out.status = AluStatus::kUnsupportedCf;
        out.blocking_opcode = uint32_t(cf[j].opcode());
        return finish();
      }
      if (!uc::IsControlFlowOpcodeExec(cf[j].opcode())) continue;

      const uint32_t addr = ExecAddress(cf[j]);
      const uint32_t count = ExecCount(cf[j]);
      const uint32_t seq = ExecSequence(cf[j]);
      for (uint32_t n = 0; n < count; ++n) {
        if (++executed > kMaxInstructions) {
          out.status = AluStatus::kInstructionCap;
          return finish();
        }
        const uint64_t at = (uint64_t(addr) + n) * 3;
        if (at + 3 > dword_count) {
          out.status = AluStatus::kMalformed;
          return finish();
        }
        // Fetches were already performed by the caller and seeded above.
        if ((seq >> (n * 2)) & 0x1) continue;

        uc::AluInstruction alu{};
        std::memcpy(&alu, dwords + at, sizeof(alu));
        in.Execute(alu);
        if (in.status != AluStatus::kOk) {
          out.status = in.status;
          out.blocking_opcode = in.blocking_opcode;
          return finish();
        }
      }
    }
  }

  if (!in.wrote_position) {
    out.status = AluStatus::kNoPositionExport;
    return finish();
  }
  out.status = AluStatus::kOk;
  for (int c = 0; c < 4; ++c) out.position[c] = in.position[c];
  return finish();
}

}  // namespace mx::hle
