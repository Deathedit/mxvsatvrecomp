#include "gpu/shader_alu.h"

#include <cmath>
#include <cstring>

#include <rex/graphics/format/ucode.h>

namespace uc = rex::graphics::ucode;

namespace mx::pm4 {

const char* AluStatusName(AluStatus s) {
  switch (s) {
    case AluStatus::kOk: return "ok";
    case AluStatus::kMalformed: return "malformed";
    case AluStatus::kNoPositionExport: return "no position export";
    case AluStatus::kUnsupportedCf: return "unsupported control flow";
    case AluStatus::kUnsupportedVectorOp: return "unsupported vector op";
    case AluStatus::kUnsupportedScalarOp: return "unsupported scalar op";
    case AluStatus::kRelativeAddressing: return "relative addressing";
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

  AluStatus status = AluStatus::kOk;
  uint32_t blocking_opcode = 0;

  // Reads constant vec4 `index` from the shadowed file. Vertex shaders address
  // the low bank, c0..c255, which is dwords 0..1023 — the same indexing the
  // translator shadows under register 0x4000 + i. Out of range reads zero
  // rather than failing: an unwritten constant really is zero on the hardware.
  Vec4 Const(uint32_t index) const {
    Vec4 r;
    const uint32_t base = index * 4;
    if (!in_.alu_consts || base + 4 > in_.alu_const_dwords) return r;
    for (int i = 0; i < 4; ++i) std::memcpy(&r[i], &in_.alu_consts[base + i], 4);
    return r;
  }

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
      if (uc::AluInstruction::is_src_temp_relative(reg)) {
        status = AluStatus::kRelativeAddressing;
        return base;
      }
      base = temps[uc::AluInstruction::src_temp_reg(reg) & (kNumTemps - 1)];
      absolute = uc::AluInstruction::is_src_temp_value_absolute(reg);
    } else {
      if (alu.src_const_is_addressed(i) ||
          alu.is_const_address_register_relative()) {
        status = AluStatus::kRelativeAddressing;
        return base;
      }
      base = Const(reg & 0xFF);
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

  Vec4 VectorOp(const uc::AluInstruction& alu) {
    Vec4 r;
    const Vec4 a = Src(alu, 1);
    const Vec4 b = Src(alu, 2);
    using Op = uc::AluVectorOpcode;
    const Op op = alu.vector_opcode();
    // src3 is only read where the opcode actually has a third operand — the
    // same discipline the export tracer needed, for the same reason.
    switch (op) {
      case Op::kAdd: for (int c = 0; c < 4; ++c) r[c] = a[c] + b[c]; break;
      case Op::kMul: for (int c = 0; c < 4; ++c) r[c] = a[c] * b[c]; break;
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
        // max plus a copy of a.w into the address register, which we do not
        // model — any instruction that then uses it relatively is refused by
        // Src(), so ignoring the side effect cannot silently mislead.
        for (int c = 0; c < 4; ++c) r[c] = a[c] >= b[c] ? a[c] : b[c];
        break;
      case Op::kMad: {
        const Vec4 c3 = Src(alu, 3);
        for (int c = 0; c < 4; ++c) r[c] = a[c] * b[c] + c3[c];
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
        for (int c = 0; c < 4; ++c) d += a[c] * b[c];
        for (int c = 0; c < 4; ++c) r[c] = d;
        break;
      }
      case Op::kDp3: {
        float d = 0;
        for (int c = 0; c < 3; ++c) d += a[c] * b[c];
        for (int c = 0; c < 4; ++c) r[c] = d;
        break;
      }
      case Op::kDp2Add: {
        const Vec4 c3 = Src(alu, 3);
        const float d = a[0] * b[0] + a[1] * b[1] + c3[0];
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
        r[0] = 1.0f; r[1] = a[1] * b[1]; r[2] = a[2]; r[3] = b[3];
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

  float ScalarOp(const uc::AluInstruction& alu) {
    using Op = uc::AluScalarOpcode;
    const Op op = alu.scalar_opcode();
    const Vec4 s = Src(alu, 3);
    // Two-operand scalar ops take x and y of the swizzled operand; one-operand
    // ops take x, and a few also read w.
    const float a = s[0], b = s[1], w = s[3];
    float r = 0.0f;
    switch (op) {
      case Op::kAdds: r = a + b; break;
      case Op::kMuls: r = a * b; break;
      case Op::kSubs: r = a - b; break;
      case Op::kMaxs: r = a >= b ? a : b; break;
      case Op::kMins: r = a < b ? a : b; break;
      case Op::kSeqs: r = a == b ? 1.0f : 0.0f; break;
      case Op::kSgts: r = a > b ? 1.0f : 0.0f; break;
      case Op::kSges: r = a >= b ? 1.0f : 0.0f; break;
      case Op::kSnes: r = a != b ? 1.0f : 0.0f; break;
      case Op::kAddsPrev: r = a + ps_; break;
      case Op::kMulsPrev: r = a * ps_; break;
      case Op::kSubsPrev: r = a - ps_; break;
      case Op::kFrcs: r = a - std::floor(a); break;
      case Op::kTruncs: r = std::trunc(a); break;
      case Op::kFloors: r = std::floor(a); break;
      case Op::kExp: r = std::exp2(a); break;
      case Op::kLog: case Op::kLogc:
        r = a == 0.0f ? -INFINITY : std::log2(std::fabs(a));
        if (op == Op::kLogc && std::isinf(r)) r = -3.402823466e+38f;
        break;
      case Op::kRcp: case Op::kRcpc: case Op::kRcpf:
        r = a == 0.0f ? INFINITY : 1.0f / a;
        if (op == Op::kRcpc && std::isinf(r)) r = r > 0 ? 3.402823466e+38f : -3.402823466e+38f;
        break;
      case Op::kRsq: case Op::kRsqc: case Op::kRsqf:
        r = a == 0.0f ? INFINITY : 1.0f / std::sqrt(std::fabs(a));
        if (op == Op::kRsqc && std::isinf(r)) r = 3.402823466e+38f;
        break;
      case Op::kSqrt: r = std::sqrt(a); break;
      case Op::kSin: r = std::sin(a); break;
      case Op::kCos: r = std::cos(a); break;
      case Op::kMaxAs: case Op::kMaxAsf:
        // As kMaxA: the address-register side effect is not modelled, and any
        // later relative use is refused rather than approximated.
        r = a >= w ? a : w;
        break;
      case Op::kRetainPrev: r = ps_; break;
      default:
        // The setp/kill families and the mulsc/addsc/subsc constant-operand
        // family, whose operand encoding is a special case worth implementing
        // only once something needs it.
        status = AluStatus::kUnsupportedScalarOp;
        blocking_opcode = uint32_t(op);
        return ps_;
    }
    if (alu.scalar_clamp()) r = Saturate(r);
    ps_ = r;
    return r;
  }

  void Execute(const uc::AluInstruction& alu) {
    const uint32_t vmask = alu.vector_write_mask();
    const uint32_t smask = alu.scalar_write_mask();

    // Evaluate both halves. They read the register file before either writes,
    // which is the co-issue semantics.
    Vec4 vres;
    float sres = ps_;
    const bool has_vector = vmask != 0 || alu.is_export();
    if (has_vector) vres = VectorOp(alu);
    if (smask != 0) sres = ScalarOp(alu);
    if (status != AluStatus::kOk) return;

    if (alu.is_export()) {
      if (alu.vector_dest() != kPositionExportRegister) return;
      wrote_position = true;
      // Export write masking is its own scheme, per component:
      //   v=1 s=0 -> vector result     v=0 s=1 -> scalar result
      //   v=1 s=1 -> constant 1
      //   v=0 s=0 -> unchanged, or constant 0 if scalar_dest_rel is set
      for (uint32_t c = 0; c < 4; ++c) {
        const bool vb = (vmask >> c) & 1, sb = (smask >> c) & 1;
        if (vb && sb) position[c] = 1.0f;
        else if (vb) position[c] = vres[c];
        else if (sb) position[c] = sres;
        else if (alu.is_scalar_dest_relative()) position[c] = 0.0f;
      }
      return;
    }

    if (alu.is_vector_dest_relative() || alu.is_scalar_dest_relative()) {
      status = AluStatus::kRelativeAddressing;
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

  // Seed the register file from the vertex fetches. This is what makes the
  // interpreter worth running: the shader reads its inputs from exactly the
  // registers its vfetch instructions named, and the export-62 decode already
  // proved those are the registers the position is built from.
  for (size_t i = 0; i < attrs.size() && i < attr_values.size(); ++i) {
    Vec4& d = in.temps[attrs[i].dest_reg & (kNumTemps - 1)];
    for (int c = 0; c < 4; ++c) d[c] = attr_values[i][c];
  }

  uint32_t executed = 0;
  for (uint32_t i = 0; i + 2 < max_cf_dword; i += 3) {
    uc::ControlFlowInstruction cf[2];
    uc::UnpackControlFlowInstructions(dwords + i, cf);
    for (int j = 0; j < 2; ++j) {
      if (IsUnsupportedCf(cf[j].opcode())) {
        out.status = AluStatus::kUnsupportedCf;
        out.blocking_opcode = uint32_t(cf[j].opcode());
        return out;
      }
      if (!uc::IsControlFlowOpcodeExec(cf[j].opcode())) continue;

      const uint32_t addr = ExecAddress(cf[j]);
      const uint32_t count = ExecCount(cf[j]);
      const uint32_t seq = ExecSequence(cf[j]);
      for (uint32_t n = 0; n < count; ++n) {
        if (++executed > kMaxInstructions) {
          out.status = AluStatus::kInstructionCap;
          return out;
        }
        const uint64_t at = (uint64_t(addr) + n) * 3;
        if (at + 3 > dword_count) {
          out.status = AluStatus::kMalformed;
          return out;
        }
        // Fetches were already performed by the caller and seeded above.
        if ((seq >> (n * 2)) & 0x1) continue;

        uc::AluInstruction alu{};
        std::memcpy(&alu, dwords + at, sizeof(alu));
        in.Execute(alu);
        if (in.status != AluStatus::kOk) {
          out.status = in.status;
          out.blocking_opcode = in.blocking_opcode;
          return out;
        }
      }
    }
  }

  if (!in.wrote_position) {
    out.status = AluStatus::kNoPositionExport;
    return out;
  }
  out.status = AluStatus::kOk;
  for (int c = 0; c < 4; ++c) out.position[c] = in.position[c];
  return out;
}

}  // namespace mx::pm4
