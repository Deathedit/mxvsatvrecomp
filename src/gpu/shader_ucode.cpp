#include "gpu/shader_ucode.h"

#include <algorithm>
#include <cstring>

// The SDK ships Xenia's GPU layer, and these two headers are header-only â€” no
// linking (rexgpu-xenos.lib is a 2-symbol plugin import stub). We get the bit
// layouts free and write the control-flow walk ourselves, because the things
// that would do it for us â€” PacketDisassembler, Shader::AnalyzeUcode,
// ParseVertexFetchInstruction â€” are all sealed inside the plugin DLL.
#include <rex/graphics/format/ucode.h>
#include <rex/graphics/xenos.h>

namespace uc = rex::graphics::ucode;

namespace mx::pm4 {

namespace {

// Guard rails. Both are far above anything this game emits (the largest
// captured vertex shader is 27 dwords, i.e. 9 instructions) and exist so a
// desynced parser handing us garbage produces a `fail` reason rather than a
// hang or a runaway vector.
constexpr uint32_t kMaxCfInstructions = 512;
constexpr uint32_t kMaxAttributes = 32;

// address(), count() and sequence() sit at identical bit offsets in all three
// exec structs, but they are distinct union members, so reaching them requires
// a switch on the opcode rather than picking one arbitrarily. Getting this
// wrong would compile silently and read the right bits by luck â€” until someone
// changed a field.
bool IsExec(uc::ControlFlowOpcode op) { return uc::IsControlFlowOpcodeExec(op); }

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

// How many source operands each vector opcode actually reads. The SDK knows â€”
// kAluVectorOpcodeInfos::GetOperandCount â€” but that table is `extern const` and
// defined inside the sealed plugin DLL, so it cannot be linked against. This is
// transcribed from the per-opcode signatures documented in the enum itself
// (AluVectorOpcode in ucode.h), which give the operand list for every one.
//
// Reading a source the opcode does not use is not harmless here. The position
// export in both ground-truth shaders is a two-operand op whose unused src3
// field still names temp register 0 â€” the colour register â€” so consulting all
// three marked colour as feeding the position. That is the exact false positive
// this table exists to prevent.
//
// Opcodes 30 and 31 are undefined; 3 is the conservative answer for them.
constexpr uint8_t kVectorOperandCount[32] = {
    2, 2, 2, 2, 2, 2, 2, 2,  // add mul max min seq sgt sge sne
    1, 1, 1,                 // frc trunc floor
    3, 3, 3, 3,              // mad cndeq cndge cndgt
    2, 2, 3, 2, 1,           // dp4 dp3 dp2add cube max4
    2, 2, 2, 2,              // setp_{eq,ne,gt,ge}_push
    2, 2, 2, 2,              // kill_{eq,gt,ge,ne}
    2, 2,                    // dst maxa
    3, 3,                    // undefined
};

}  // namespace

uint32_t VertexFormatSizeBytes(uint32_t format, uint32_t* out_components) {
  uint32_t components = 0, bytes = 0;
  switch (format) {
    case 6:  components = 4; bytes = 4;  break;  // k_8_8_8_8
    case 7:  components = 4; bytes = 4;  break;  // k_2_10_10_10
    case 16: components = 3; bytes = 4;  break;  // k_10_11_11
    case 17: components = 3; bytes = 4;  break;  // k_11_11_10
    case 25: components = 2; bytes = 4;  break;  // k_16_16
    case 26: components = 4; bytes = 8;  break;  // k_16_16_16_16
    case 31: components = 2; bytes = 4;  break;  // k_16_16_FLOAT
    case 32: components = 4; bytes = 8;  break;  // k_16_16_16_16_FLOAT
    case 33: components = 1; bytes = 4;  break;  // k_32
    case 34: components = 2; bytes = 8;  break;  // k_32_32
    case 35: components = 4; bytes = 16; break;  // k_32_32_32_32
    case 36: components = 1; bytes = 4;  break;  // k_32_FLOAT
    case 37: components = 2; bytes = 8;  break;  // k_32_32_FLOAT
    case 38: components = 4; bytes = 16; break;  // k_32_32_32_32_FLOAT
    case 57: components = 3; bytes = 12; break;  // k_32_32_32_FLOAT
    default: break;  // unrecognised â€” report 0 rather than assert
  }
  if (out_components) *out_components = components;
  return bytes;
}

float HalfToFloat(uint16_t h) {
  const uint32_t sign = uint32_t(h & 0x8000) << 16;
  uint32_t exp = (h >> 10) & 0x1F;
  uint32_t man = h & 0x3FF;
  uint32_t bits;
  if (exp == 0) {
    if (man == 0) {
      bits = sign;  // +-0
    } else {
      exp = 1;      // subnormal: renormalise
      while (!(man & 0x400)) { man <<= 1; --exp; }
      man &= 0x3FF;
      bits = sign | ((exp + 112) << 23) | (man << 13);
    }
  } else if (exp == 0x1F) {
    bits = sign | 0x7F800000u | (man << 13);  // inf / nan
  } else {
    bits = sign | ((exp + 112) << 23) | (man << 13);
  }
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}

uint32_t VertexFormatUnitBytes(uint32_t format) {
  switch (format) {
    // The 16-bit-component formats: two components share every dword, so the
    // dword is not the unit.
    case 25:  // k_16_16
    case 26:  // k_16_16_16_16
    case 31:  // k_16_16_FLOAT
    case 32:  // k_16_16_16_16_FLOAT
      return 2;
    // Everything else this decoder handles is packed in dwords, including the
    // byte and sub-byte formats â€” a k_8_8_8_8 is one big-endian dword.
    case 6: case 7: case 16: case 17: case 33: case 34: case 35:
    case 36: case 37: case 38: case 57:
      return 4;
    default:
      return 0;  // unrecognised â€” the caller refuses the format anyway
  }
}

void ApplyFetchEndianFor(uint8_t* data, size_t bytes, uint32_t endian,
                         uint32_t format) {
  if (!data || endian == 0) return;
  // The mode's nominal width, narrowed to the format's own packed unit. This
  // narrowing is the fix: an 8in32 swap over 16-bit components exchanges the
  // components as well as their bytes.
  uint32_t unit = (endian == 1) ? 2u : 4u;
  const uint32_t packed = VertexFormatUnitBytes(format);
  if (packed && packed < unit) unit = packed;
  if (unit < 2) return;
  for (size_t i = 0; i + unit <= bytes; i += unit) {
    for (uint32_t a = 0, b = unit - 1; a < b; ++a, --b) {
      std::swap(data[i + a], data[i + b]);
    }
  }
}

namespace {

inline uint16_t Rd16(const uint8_t* p) {
  uint16_t v; std::memcpy(&v, p, 2); return v;
}
inline uint32_t Rd32(const uint8_t* p) {
  uint32_t v; std::memcpy(&v, p, 4); return v;
}
inline float RdF32(const uint8_t* p) {
  float v; std::memcpy(&v, p, 4); return v;
}
// Signed normalised 16-bit. Xenos clamps the -32768 case to -1.
inline float Snorm16(uint16_t u) {
  const int16_t s = int16_t(u);
  return s <= -32767 ? -1.0f : float(s) / 32767.0f;
}

}  // namespace

namespace {

// Signed reads of the sub-byte widths the packed formats use.
inline float S8(uint32_t v)  { return float(int8_t(v & 0xFF)); }
inline float S16(uint32_t v) { return float(int16_t(v & 0xFFFF)); }
inline int32_t S10(uint32_t v) {
  const int32_t x = int32_t(v & 0x3FF);
  return (x & 0x200) ? x - 0x400 : x;
}
inline int32_t S2(uint32_t v) {
  const int32_t x = int32_t(v & 0x3);
  return (x & 0x2) ? x - 0x4 : x;
}
// D3D's signed-normalized rule: the most negative value maps to -1, not below.
inline float Norm(float v, float scale) {
  const float f = v / scale;
  return f < -1.0f ? -1.0f : f;
}

}  // namespace

bool ReadVertexAttributeAs(const uint8_t* vertex_base, uint32_t vertex_bytes,
                           uint32_t format, uint32_t offset_bytes,
                           uint32_t size_bytes, NumFormat num, uint32_t endian,
                           float out[4]) {
  if (!vertex_base) return false;
  if (size_bytes == 0) return false;
  if (offset_bytes + size_bytes > vertex_bytes) return false;

  // The endian swap happens here rather than once over the whole vertex,
  // because its width depends on the format and one vertex mixes formats. The
  // copy is bounded by the largest format this decoder handles (16 bytes).
  uint8_t swapped[16];
  const uint8_t* p = vertex_base + offset_bytes;
  if (endian != 0 && size_bytes <= sizeof(swapped)) {
    std::memcpy(swapped, p, size_bytes);
    ApplyFetchEndianFor(swapped, size_bytes, endian, format);
    p = swapped;
  }

  out[0] = out[1] = out[2] = 0.0f;
  out[3] = 1.0f;

  switch (format) {
    // Float formats: the signed/normalized bits do not apply.
    case 36:  // k_32_FLOAT
      out[0] = RdF32(p);
      return true;
    case 37:  // k_32_32_FLOAT
      out[0] = RdF32(p); out[1] = RdF32(p + 4);
      return true;
    case 57:  // k_32_32_32_FLOAT
      out[0] = RdF32(p); out[1] = RdF32(p + 4); out[2] = RdF32(p + 8);
      return true;
    case 38:  // k_32_32_32_32_FLOAT
      for (int i = 0; i < 4; ++i) out[i] = RdF32(p + i * 4);
      return true;
    case 31:  // k_16_16_FLOAT
      out[0] = HalfToFloat(Rd16(p)); out[1] = HalfToFloat(Rd16(p + 2));
      return true;
    case 32:  // k_16_16_16_16_FLOAT
      for (int i = 0; i < 4; ++i) out[i] = HalfToFloat(Rd16(p + i * 2));
      return true;

    case 25:    // k_16_16
    case 26: {  // k_16_16_16_16
      const int n = (format == 25) ? 2 : 4;
      for (int i = 0; i < n; ++i) {
        const uint16_t u = Rd16(p + i * 2);
        switch (num) {
          case NumFormat::kSnorm: out[i] = Norm(S16(u), 32767.0f); break;
          case NumFormat::kUnorm: out[i] = float(u) / 65535.0f;    break;
          case NumFormat::kSint:  out[i] = S16(u);                 break;
          case NumFormat::kUint:  out[i] = float(u);               break;
        }
      }
      return true;
    }

    case 6: {  // k_8_8_8_8 â€” low byte is component 0
      const uint32_t v = Rd32(p);
      for (int i = 0; i < 4; ++i) {
        const uint32_t b = (v >> (i * 8)) & 0xFF;
        switch (num) {
          case NumFormat::kUnorm: out[i] = float(b) / 255.0f;    break;
          case NumFormat::kSnorm: out[i] = Norm(S8(b), 127.0f);  break;
          case NumFormat::kUint:  out[i] = float(b);             break;
          case NumFormat::kSint:  out[i] = S8(b);                break;
        }
      }
      return true;
    }

    case 7: {  // k_2_10_10_10 â€” three 10-bit components then a 2-bit one
      const uint32_t v = Rd32(p);
      const uint32_t raw[4] = {v & 0x3FF, (v >> 10) & 0x3FF, (v >> 20) & 0x3FF,
                               (v >> 30) & 0x3};
      for (int i = 0; i < 4; ++i) {
        const bool two = (i == 3);
        switch (num) {
          case NumFormat::kUnorm:
            out[i] = float(raw[i]) / (two ? 3.0f : 1023.0f);
            break;
          case NumFormat::kSnorm:
            out[i] = two ? Norm(float(S2(raw[i])), 1.0f)
                         : Norm(float(S10(raw[i])), 511.0f);
            break;
          case NumFormat::kUint:
            out[i] = float(raw[i]);
            break;
          case NumFormat::kSint:
            out[i] = two ? float(S2(raw[i])) : float(S10(raw[i]));
            break;
        }
      }
      return true;
    }

    case 16: {  // k_10_11_11 â€” x:11 y:11 z:10, low to high
      const uint32_t v = Rd32(p);
      out[0] = float(v & 0x7FF) / 2047.0f;
      out[1] = float((v >> 11) & 0x7FF) / 2047.0f;
      out[2] = float((v >> 22) & 0x3FF) / 1023.0f;
      return true;
    }
    case 17: {  // k_11_11_10 â€” x:10 y:11 z:11
      const uint32_t v = Rd32(p);
      out[0] = float(v & 0x3FF) / 1023.0f;
      out[1] = float((v >> 10) & 0x7FF) / 2047.0f;
      out[2] = float((v >> 21) & 0x7FF) / 2047.0f;
      return true;
    }
    default:
      // 33/34/35 (integer k_32 family) and anything unrecognised. Reporting
      // false lets the caller count what it cannot handle rather than draw a
      // plausible-looking guess.
      return false;
  }
}

bool ReadVertexAttribute(const uint8_t* vertex_base, uint32_t vertex_bytes,
                         const VertexAttribute& attr, uint32_t endian,
                         float out[4]) {
  // The interpretation this function has always applied, kept exactly so the
  // PM4 path's output does not move. attr.is_signed / attr.is_normalized are
  // deliberately *not* consulted here: they were never consulted before, and
  // honouring them would change PM4 geometry as a side effect of a D3D9 change.
  NumFormat num = NumFormat::kUnorm;
  if (attr.format == 25 || attr.format == 26) num = NumFormat::kSnorm;
  return ReadVertexAttributeAs(vertex_base, vertex_bytes, attr.format,
                               attr.offset_bytes, attr.size_bytes, num, endian,
                               out);
}

const VertexAttribute* PickPositionAttribute(
    const std::vector<VertexAttribute>& attrs, bool* out_from_export) {
  if (out_from_export) *out_from_export = false;

  // The shader's own answer, when the ALU trace produced one. Several
  // attributes can legitimately reach the export â€” a skinned mesh exports a
  // position built from bone weights and indices as well as the point â€” so
  // among the contributors still prefer the one that looks most like a
  // coordinate, lowest offset breaking the tie.
  const VertexAttribute* best = nullptr;
  for (const auto& a : attrs) {
    if (!a.feeds_position || a.components < 2) continue;
    if (!best || a.offset_bytes < best->offset_bytes) best = &a;
  }
  if (best) {
    if (out_from_export) *out_from_export = true;
    return best;
  }

  // Fallback: the guess this used to make unconditionally. Kept because a
  // shader whose ALU we could not follow is better served by a guess than by
  // nothing, but the caller is now told which it got.
  for (const auto& a : attrs) {
    if (a.components < 2) continue;
    switch (a.format) {
      case 57: case 38: case 32: case 37: case 31:
      case 16: case 17: case 7: case 25: case 26:
        break;
      default:
        continue;
    }
    if (!best || a.offset_bytes < best->offset_bytes) best = &a;
  }
  return best;
}

const VertexAttribute* PickColorAttribute(
    const std::vector<VertexAttribute>& attrs) {
  const VertexAttribute* pos = PickPositionAttribute(attrs);
  const VertexAttribute* fallback = nullptr;
  for (const auto& a : attrs) {
    if (&a == pos) continue;
    if (a.format == 6 || a.format == 7) return &a;  // packed 8888 / 2_10_10_10
    if (a.components == 4 && !fallback) fallback = &a;
  }
  return fallback;
}

bool DecodeVertexShaderFetches(const uint32_t* dwords, uint32_t dword_count,
                               std::vector<VertexAttribute>& out,
                               const char** fail,
                               bool* out_saw_position_export) {
  auto reject = [&](const char* why) {
    if (fail) *fail = why;
    return false;
  };
  if (fail) *fail = nullptr;
  if (out_saw_position_export) *out_saw_position_export = false;

  if (!dwords) return reject("null blob");
  if (dword_count < 3) return reject("blob shorter than one CF pair");

  // Pass 1 â€” find where the control flow section ends.
  //
  // The blob carries no header saying so. CF instructions are 48-bit, packed
  // two per three dwords, and the section runs until the first instruction the
  // control flow jumps to. So the end is the lowest exec target address, and
  // the bound is re-read each iteration so it shrinks as execs are seen. That
  // is sound because a compiler never places an exec target ahead of the CF
  // that references it.
  uint32_t max_cf_dword = dword_count - (dword_count % 3);
  bool saw_exec = false;
  for (uint32_t i = 0; i + 2 < max_cf_dword; i += 3) {
    uc::ControlFlowInstruction cf[2];
    uc::UnpackControlFlowInstructions(dwords + i, cf);
    for (int j = 0; j < 2; ++j) {
      if (!IsExec(cf[j].opcode())) continue;
      saw_exec = true;
      const uint64_t target = uint64_t(ExecAddress(cf[j])) * 3;
      if (target < max_cf_dword) max_cf_dword = uint32_t(target);
    }
  }
  if (!saw_exec) return reject("no exec instruction");
  if (max_cf_dword == 0) return reject("exec target at address 0");

  // Pass 2 â€” walk every exec block in program order and pull out the fetches.
  //
  // Branches are deliberately not followed and loops are not unrolled: each
  // exec is visited exactly once. That over-approximates the attribute set for
  // a shader with alternative paths, which is the right error for gathering a
  // layout â€” every path fetches from the same buffer. A shader with two genuine
  // layouts would show up as two attributes at one offset with different
  // formats, and that should be reported, not smoothed over by a heuristic.
  //
  // last_full persists across exec blocks within the shader: a vfetch_full in
  // one exec followed by minis in another is legal.
  uc::VertexFetchInstruction last_full{};
  bool have_full = false;
  uint32_t cf_seen = 0;

  // Which fetched attributes reach the position export.
  //
  // taint[r] is a bitmask of indices into the attributes this call appends, one
  // bit per attribute, recording which of them the value currently in GPR r was
  // built from. A vfetch writing r resets it to just that attribute; an ALU
  // instruction unions its source registers' masks into its destination; an
  // export to register 62 unions them into pos_taint. kMaxAttributes is 32, so
  // one uint32_t per register is exactly enough.
  //
  // Only the operands the opcode actually reads count â€” see
  // kVectorOperandCount, and the false positive that made it necessary.
  const size_t out_base = out.size();
  uint32_t taint[64] = {};
  uint32_t pos_taint = 0;
  bool saw_position_export = false;

  for (uint32_t i = 0; i + 2 < max_cf_dword; i += 3) {
    uc::ControlFlowInstruction cf[2];
    uc::UnpackControlFlowInstructions(dwords + i, cf);
    for (int j = 0; j < 2; ++j) {
      if (++cf_seen > kMaxCfInstructions) return reject("CF instruction cap");
      if (!IsExec(cf[j].opcode())) continue;

      const uint32_t addr = ExecAddress(cf[j]);
      const uint32_t count = ExecCount(cf[j]);
      const uint32_t seq = ExecSequence(cf[j]);

      for (uint32_t n = 0; n < count; ++n) {
        const uint64_t at = (uint64_t(addr) + n) * 3;
        if (at + 3 > dword_count) return reject("instruction out of range");
        const uint32_t* ins = dwords + at;

        // Sequence bits, 2 per instruction: bit[2n] selects fetch (1) over ALU
        // (0), bit[2n+1] is serialize, which does not concern us.
        if (!((seq >> (n * 2)) & 0x1)) {
          // An ALU instruction. We do not evaluate it â€” only follow which
          // registers its result depends on, which is all that is needed to
          // learn what feeds the position export.
          uc::AluInstruction alu{};
          std::memcpy(&alu, ins, sizeof(alu));

          // A source only contributes if it is a temp register (a constant
          // carries no fetch) and the opcode actually reads it.
          auto temp_taint = [&](size_t s) -> uint32_t {
            if (!alu.src_is_temp(s)) return 0;
            return taint[uc::AluInstruction::src_temp_reg(alu.src_reg(s)) & 63];
          };
          uint32_t vec_taint = 0;
          const uint32_t operands =
              kVectorOperandCount[uint32_t(alu.vector_opcode()) & 31];
          for (size_t s = 1; s <= operands; ++s) vec_taint |= temp_taint(s);
          // The scalar operand always occupies the src3 slot.
          const uint32_t sca_taint = temp_taint(3);

          if (alu.is_export()) {
            // On an export both the vector and the scalar operation write to
            // vector_dest, so one destination check covers the instruction â€”
            // but only the halves that actually write anything contribute.
            if (alu.vector_dest() == kPositionExportRegister) {
              saw_position_export = true;
              if (alu.vector_write_mask()) pos_taint |= vec_taint;
              if (alu.scalar_write_mask()) pos_taint |= sca_taint;
            }
            continue;
          }

          // A full write mask replaces the register's provenance; a partial one
          // leaves the untouched components carrying whatever they had, so it
          // has to union. Relative destination addressing is not resolved â€”
          // the index lives in a register we do not evaluate â€” so such a write
          // lands on the base register and is a known imprecision.
          if (alu.vector_write_mask()) {
            uint32_t& t = taint[alu.vector_dest() & 63];
            t = (alu.vector_write_mask() == 0xF) ? vec_taint : (t | vec_taint);
          }
          if (alu.scalar_write_mask()) {
            taint[alu.scalar_dest() & 63] |= sca_taint;
          }
          continue;
        }

        // FetchOpcode is the low 5 bits of word 0. Anything but kVertexFetch
        // here is a texture fetch, which carries no vertex layout.
        if ((ins[0] & 0x1F) != uint32_t(uc::FetchOpcode::kVertexFetch)) continue;

        // VertexFetchInstruction is standard-layout with no public constructor,
        // so it is filled by memcpy. Its static_assert_size means a packing
        // disagreement fails the build rather than producing a wrong stride.
        uc::VertexFetchInstruction vf{};
        std::memcpy(&vf, ins, sizeof(vf));

        if (out.size() >= kMaxAttributes) return reject("attribute cap");

        VertexAttribute a;
        a.from_mini = vf.is_mini_fetch();
        // A vfetch_mini inherits fetch_constant_index, stride, src and
        // is_index_rounded from the preceding vfetch_full, and carries its own
        // format, offset, dest and modifiers. Its stride field usually reads 0
        // and its const_index reads garbage, so consuming them unconditionally
        // would silently yield a stride-0 attribute. (Not always zero, though:
        // one captured shader's mini carries the same stride as its full. The
        // inherited value is correct in both cases.)
        if (a.from_mini) {
          if (!have_full) return reject("vfetch_mini with no preceding full");
          a.fetch_slot = last_full.fetch_constant_index();
          a.stride_bytes = last_full.stride() * 4;
        } else {
          a.fetch_slot = vf.fetch_constant_index();
          a.stride_bytes = vf.stride() * 4;
          last_full = vf;
          have_full = true;
        }

        const int32_t off = vf.offset();
        if (off < 0) return reject("negative vfetch offset");
        a.offset_bytes = uint32_t(off) * 4;
        a.format = uint32_t(vf.data_format());
        a.size_bytes = VertexFormatSizeBytes(a.format, &a.components);
        a.dest_reg = vf.dest();
        a.dest_swizzle = vf.dest_swizzle();
        a.is_signed = vf.is_signed();
        a.is_normalized = vf.is_normalized();
        a.exp_adjust = vf.exp_adjust();

        // A fetch defines its destination outright, discarding whatever the
        // register held. Attributes past the 32nd cannot be represented in the
        // mask, but kMaxAttributes caps the list at 32 above, so the index is
        // always in range.
        const size_t index = out.size() - out_base;
        taint[a.dest_reg & 63] = uint32_t(1) << index;
        out.push_back(a);
      }
    }
  }

  for (size_t i = out_base; i < out.size(); ++i) {
    if (pos_taint & (uint32_t(1) << (i - out_base))) out[i].feeds_position = true;
  }
  if (out_saw_position_export) *out_saw_position_export = saw_position_export;

  return true;
}

bool VertexShaderStructureMatches(const uint32_t* shader_template,
                                  const uint32_t* patched_shader,
                                  uint32_t dword_count,
                                  VertexShaderStructureStats* stats) {
  VertexShaderStructureStats local;
  if (!stats) stats = &local;
  *stats = {};
  if (!shader_template || !patched_shader || dword_count < 3) return false;

  // First establish the executable fetch locations from the patched copy. The
  // control-flow words are compared verbatim below, so the template cannot
  // redirect execution to different instructions and still match.
  uint32_t max_cf_dword = dword_count - (dword_count % 3);
  bool saw_exec = false;
  for (uint32_t i = 0; i + 2 < max_cf_dword; i += 3) {
    uc::ControlFlowInstruction cf[2];
    uc::UnpackControlFlowInstructions(patched_shader + i, cf);
    for (int j = 0; j < 2; ++j) {
      if (!IsExec(cf[j].opcode())) continue;
      saw_exec = true;
      const uint64_t target = uint64_t(ExecAddress(cf[j])) * 3;
      if (target < max_cf_dword) max_cf_dword = uint32_t(target);
    }
  }
  if (!saw_exec || max_cf_dword == 0) return false;

  std::vector<uint8_t> patched_fetch(dword_count, 0);
  for (uint32_t i = 0; i + 2 < max_cf_dword; i += 3) {
    uc::ControlFlowInstruction cf[2];
    uc::UnpackControlFlowInstructions(patched_shader + i, cf);
    for (int j = 0; j < 2; ++j) {
      if (!IsExec(cf[j].opcode())) continue;
      const uint32_t addr = ExecAddress(cf[j]);
      const uint32_t count = ExecCount(cf[j]);
      const uint32_t seq = ExecSequence(cf[j]);
      for (uint32_t n = 0; n < count; ++n) {
        const uint64_t at64 = (uint64_t(addr) + n) * 3;
        if (at64 + 3 > dword_count) return false;
        const uint32_t at = uint32_t(at64);
        if (!((seq >> (n * 2)) & 1u)) continue;
        if ((patched_shader[at] & 0x1Fu) !=
            uint32_t(uc::FetchOpcode::kVertexFetch))
          continue;
        patched_fetch[at] = 1;
      }
    }
  }

  // These are precisely the fields checked by the live prediction around
  // PatchVertexShaderToMatchVertexDeclaration in hooks_d3d9.cpp. Keeping every
  // other bit makes this an identity comparison, not a similarity score.
  constexpr uint32_t kKeep[3] = {
      ~0x3FF00000u,  // fetch constant and coalescing count
      ~0x003F3FFFu,  // data/number format and destination swizzle
      0x80000000u,   // offset and stride; retain the mini/sign bit
  };
  for (uint32_t i = 0; i < dword_count; ++i) {
    uint32_t a = shader_template[i];
    uint32_t b = patched_shader[i];
    const uint32_t base = i - (i % 3);
    if (base < patched_fetch.size() && patched_fetch[base]) {
      const uint32_t word = i % 3;
      a &= kKeep[word];
      b &= kKeep[word];
    }
    if (base < patched_fetch.size() && patched_fetch[base]) {
      stats->fetch_xor[i % 3] |= shader_template[i] ^ patched_shader[i];
    }
    if (a == b) {
      ++stats->equal_dwords;
    } else if (base < patched_fetch.size() && patched_fetch[base]) {
      ++stats->fetch_mismatches;
    } else {
      ++stats->other_mismatches;
    }
  }
  return stats->equal_dwords == dword_count;
}

bool DecodePixelTextureFetches(const uint32_t* dwords, uint32_t dword_count,
                               std::vector<PixelTextureBinding>& out,
                               const char** fail) {
  auto reject = [&](const char* why) {
    if (fail) *fail = why;
    return false;
  };
  if (fail) *fail = nullptr;
  if (!dwords || dword_count < 3) return reject("blob too short");
  const size_t out_base = out.size();

  uint32_t max_cf_dword = dword_count - (dword_count % 3);
  bool saw_exec = false;
  for (uint32_t i = 0; i + 2 < max_cf_dword; i += 3) {
    uc::ControlFlowInstruction cf[2];
    uc::UnpackControlFlowInstructions(dwords + i, cf);
    for (int j = 0; j < 2; ++j) {
      if (!IsExec(cf[j].opcode())) continue;
      saw_exec = true;
      const uint64_t target = uint64_t(ExecAddress(cf[j])) * 3;
      if (target < max_cf_dword) max_cf_dword = uint32_t(target);
    }
  }
  if (!saw_exec || max_cf_dword == 0) return reject("no exec instruction");

  for (uint32_t i = 0; i + 2 < max_cf_dword; i += 3) {
    uc::ControlFlowInstruction cf[2];
    uc::UnpackControlFlowInstructions(dwords + i, cf);
    for (int j = 0; j < 2; ++j) {
      if (!IsExec(cf[j].opcode())) continue;
      const uint32_t addr = ExecAddress(cf[j]);
      const uint32_t count = ExecCount(cf[j]);
      const uint32_t seq = ExecSequence(cf[j]);
      for (uint32_t n = 0; n < count; ++n) {
        if (!((seq >> (n * 2)) & 1)) continue;
        const uint64_t at = (uint64_t(addr) + n) * 3;
        if (at + 3 > dword_count) return reject("instruction out of range");
        if ((dwords[at] & 0x1F) !=
            uint32_t(uc::FetchOpcode::kTextureFetch))
          continue;
        uc::TextureFetchInstruction tf{};
        std::memcpy(&tf, dwords + at, sizeof(tf));
        if (tf.is_src_relative() || tf.is_dest_relative())
          return reject("relative texture fetch");
        if (tf.dimension() != rex::graphics::xenos::FetchOpDimension::k2D)
          return reject("non-2D texture fetch");
        PixelTextureBinding binding;
        binding.sampler = tf.fetch_constant_index();
        binding.src_reg = tf.src();
        binding.src_swizzle = tf.src_swizzle();
        binding.unnormalized = tf.unnormalized_coordinates();
        out.push_back(binding);
      }
    }
  }
  return out.size() != out_base ? true : reject("no texture fetch");
}

bool DecodeSingleTexturePixelShader(const uint32_t* dwords,
                                    uint32_t dword_count,
                                    PixelTextureBinding& out,
                                    const char** fail) {
  out = {};
  std::vector<PixelTextureBinding> bindings;
  if (!DecodePixelTextureFetches(dwords, dword_count, bindings, fail))
    return false;
  if (bindings.size() != 1) {
    if (fail) *fail = "multiple texture fetches";
    return false;
  }
  out = bindings.front();
  return true;
}

}  // namespace mx::pm4
