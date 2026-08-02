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
