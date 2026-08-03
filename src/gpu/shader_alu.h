#pragma once

// Executes a Xenos vertex shader's ALU on the CPU, far enough to produce the
// clip-space position it exports to register 62.
//
// Why an interpreter and not a translation to HLSL: everything needed is
// already in hand on the CPU side — the microcode (IM_LOAD_IMMEDIATE/IM_LOAD),
// the fetched attributes (shader_ucode.h), and the ALU constant file — and the
// transcode already walks every vertex on the CPU. Emitting HLSL would mean a
// runtime compiler, a PSO per shader, and a rewrite of the draw path, none of
// which is needed to answer whether the positions come out right.
//
// The export-62 decode established *which* attribute is the position. It did
// not help, because for the majority of draws that attribute is compressed
// model space which the shader expands with a per-object scale and bias. This
// is the code that applies that expansion.
//
// SDK-free header, same as shader_ucode.h, so the unit test needs no SDK.

#include <array>
#include <cstdint>
#include <vector>

#include "gpu/shader_ucode.h"

namespace mx::pm4 {

// Why a shader could not be executed. Anything but kOk means the caller should
// count the gap rather than use the result.
enum class AluStatus {
  kOk = 0,
  kMalformed,          // blob did not decode
  kNoPositionExport,   // never wrote register 62
  kUnsupportedCf,      // a jump, call or loop — see the note in the .cpp
  kUnsupportedVectorOp,
  kUnsupportedScalarOp,
  // aL-relative constant index. aL is the loop counter, and we deliberately walk
  // every exec block once rather than unrolling loops, so there is no value to
  // give it. Distinct from a0-relative addressing, which *is* modelled — keeping
  // them apart is what lets the failure histogram show whether that work landed.
  kLoopRelative,
  kInstructionCap,
};

const char* AluStatusName(AluStatus s);

struct AluResult {
  AluStatus status = AluStatus::kMalformed;
  float position[4] = {0, 0, 0, 1};  // the register 62 export
  // The opcode that stopped us, when status names an unsupported op. Lets the
  // caller build a histogram of exactly which instructions are missing instead
  // of a single opaque failure count.
  uint32_t blocking_opcode = 0;
};

// Inputs to one execution. The register file is seeded from the decoded vertex
// fetches; everything else is shader state.
struct AluInputs {
  // 512 vec4 of ALU constants, dword-indexed exactly as the translator shadows
  // them (register 0x4000 + i). Never null.
  const uint32_t* alu_consts = nullptr;
  uint32_t alu_const_dwords = 0;
};

// Run the vertex shader over one vertex.
//
// `attrs` and `attr_values` are parallel: attr_values[i] is the four floats
// already read out of the vertex buffer for attrs[i]. Each attribute is placed
// in the register its vfetch names (dest_reg), which is what the shader's ALU
// then reads.
//
// Does not throw and does not loop unbounded.
AluResult ExecuteVertexShader(const uint32_t* dwords, uint32_t dword_count,
                              const std::vector<VertexAttribute>& attrs,
                              const std::vector<std::array<float, 4>>& attr_values,
                              const AluInputs& inputs);

}  // namespace mx::pm4
