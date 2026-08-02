#include "gpu/shader_ucode.h"

#include <algorithm>
#include <cstring>

// The SDK ships Xenia's GPU layer, and these two headers are header-only — no
// linking (rexgpu-xenos.lib is a 2-symbol plugin import stub). We get the bit
// layouts free and write the control-flow walk ourselves, because the things
// that would do it for us — PacketDisassembler, Shader::AnalyzeUcode,
// ParseVertexFetchInstruction — are all sealed inside the plugin DLL.
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
// wrong would compile silently and read the right bits by luck — until someone
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
    default: break;  // unrecognised — report 0 rather than assert
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

void ApplyFetchEndian(uint8_t* data, size_t bytes, uint32_t endian) {
  if (!data) return;
  if (endian == 1) {
    for (size_t i = 0; i + 1 < bytes; i += 2) std::swap(data[i], data[i + 1]);
  } else if (endian == 2) {
    for (size_t i = 0; i + 3 < bytes; i += 4) {
      std::swap(data[i], data[i + 3]);
      std::swap(data[i + 1], data[i + 2]);
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

bool ReadVertexAttribute(const uint8_t* vertex_base, uint32_t vertex_bytes,
                         const VertexAttribute& attr, float out[4]) {
  if (!vertex_base) return false;
  const uint32_t size = attr.size_bytes;
  if (size == 0) return false;
  if (attr.offset_bytes + size > vertex_bytes) return false;
  const uint8_t* p = vertex_base + attr.offset_bytes;

  out[0] = out[1] = out[2] = 0.0f;
  out[3] = 1.0f;

  switch (attr.format) {
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
    case 25:  // k_16_16 — signed normalised
      out[0] = Snorm16(Rd16(p)); out[1] = Snorm16(Rd16(p + 2));
      return true;
    case 26:  // k_16_16_16_16
      for (int i = 0; i < 4; ++i) out[i] = Snorm16(Rd16(p + i * 2));
      return true;
    case 6: {  // k_8_8_8_8 — unsigned normalised, low byte is component 0
      const uint32_t v = Rd32(p);
      for (int i = 0; i < 4; ++i)
        out[i] = float((v >> (i * 8)) & 0xFF) / 255.0f;
      return true;
    }
    case 7: {  // k_2_10_10_10 — 10/10/10 then 2
      const uint32_t v = Rd32(p);
      out[0] = float(v & 0x3FF) / 1023.0f;
      out[1] = float((v >> 10) & 0x3FF) / 1023.0f;
      out[2] = float((v >> 20) & 0x3FF) / 1023.0f;
      out[3] = float((v >> 30) & 0x3) / 3.0f;
      return true;
    }
    case 16: {  // k_10_11_11 — x:11 y:11 z:10, low to high
      const uint32_t v = Rd32(p);
      out[0] = float(v & 0x7FF) / 2047.0f;
      out[1] = float((v >> 11) & 0x7FF) / 2047.0f;
      out[2] = float((v >> 22) & 0x3FF) / 1023.0f;
      return true;
    }
    case 17: {  // k_11_11_10 — x:10 y:11 z:11
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

const VertexAttribute* PickPositionAttribute(
    const std::vector<VertexAttribute>& attrs) {
  const VertexAttribute* best = nullptr;
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
                               const char** fail) {
  auto reject = [&](const char* why) {
    if (fail) *fail = why;
    return false;
  };
  if (fail) *fail = nullptr;

  if (!dwords) return reject("null blob");
  if (dword_count < 3) return reject("blob shorter than one CF pair");

  // Pass 1 — find where the control flow section ends.
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

  // Pass 2 — walk every exec block in program order and pull out the fetches.
  //
  // Branches are deliberately not followed and loops are not unrolled: each
  // exec is visited exactly once. That over-approximates the attribute set for
  // a shader with alternative paths, which is the right error for gathering a
  // layout — every path fetches from the same buffer. A shader with two genuine
  // layouts would show up as two attributes at one offset with different
  // formats, and that should be reported, not smoothed over by a heuristic.
  //
  // last_full persists across exec blocks within the shader: a vfetch_full in
  // one exec followed by minis in another is legal.
  uc::VertexFetchInstruction last_full{};
  bool have_full = false;
  uint32_t cf_seen = 0;

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
        // Sequence bits, 2 per instruction: bit[2n] selects fetch (1) over ALU
        // (0), bit[2n+1] is serialize, which does not concern us.
        if (!((seq >> (n * 2)) & 0x1)) continue;

        const uint64_t at = (uint64_t(addr) + n) * 3;
        if (at + 3 > dword_count) return reject("fetch instruction out of range");
        const uint32_t* ins = dwords + at;

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
        out.push_back(a);
      }
    }
  }

  return true;
}

}  // namespace mx::pm4
