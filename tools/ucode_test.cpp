// Ground-truth tests for the Xenos vertex shader microcode decoder.
//
// Built ad hoc, not part of MX_SOURCES:
//   clang++ -std=c++23 -I src -I C:/rexglue-sdk/include \
//       -o ucode_test.exe tools/ucode_test.cpp src/gpu/shader_ucode.cpp
//
// The fixtures are verbatim IM_LOAD_IMMEDIATE (0x2B) payloads captured in
// pm4_dump_native_frame_600.txt — real microcode this game submitted, not
// synthesised. Their expected decodes were worked out by hand against the bit
// layouts in rex/graphics/format/ucode.h before any of this was written, which
// is the whole point: it catches packing, endianness and bit-offset errors in
// seconds instead of costing a 75-second game run each.
//
// translator_test.cpp is deliberately not extended — its includes still name
// the pre-gpu/ file layout and it does not build.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <iterator>
#include <vector>

#include "gpu/shader_alu.h"
#include "gpu/shader_ucode.h"

namespace {

int g_failures = 0;

void CheckU32(const char* what, uint32_t got, uint32_t want) {
  if (got == want) return;
  std::printf("  FAIL %-28s got %u, want %u\n", what, got, want);
  ++g_failures;
}

// Print what was actually decoded. A test whose passing output is silence
// cannot be told apart from a test that never ran its assertions.
void Dump(const std::vector<mx::pm4::VertexAttribute>& attrs) {
  for (size_t i = 0; i < attrs.size(); ++i) {
    const auto& a = attrs[i];
    std::printf(
        "  attr[%zu] slot=%-2u off=%-3u stride=%-3u fmt=%-2u comps=%u "
        "size=%-2u dest=r%u swiz=0x%03X%s%s exp=%d%s%s\n",
        i, a.fetch_slot, a.offset_bytes, a.stride_bytes, a.format,
        a.components, a.size_bytes, a.dest_reg, a.dest_swizzle,
        a.is_signed ? " signed" : "", a.is_normalized ? " norm" : "",
        a.exp_adjust, a.from_mini ? " (mini)" : " (full)",
        a.feeds_position ? " POSITION" : "");
  }
}

void CheckBool(const char* what, bool got, bool want) {
  if (got == want) return;
  std::printf("  FAIL %-28s got %s, want %s\n", what, got ? "true" : "false",
              want ? "true" : "false");
  ++g_failures;
}

// PickPositionAttribute must land on `want_index`, and must say it got there
// from the shader's export trace rather than the offset/format fallback.
void CheckPickedPosition(const std::vector<mx::pm4::VertexAttribute>& attrs,
                         size_t want_index, bool want_from_export) {
  bool from_export = false;
  const mx::pm4::VertexAttribute* got =
      mx::pm4::PickPositionAttribute(attrs, &from_export);
  if (got != &attrs[want_index]) {
    std::printf("  FAIL %-28s picked %s, want attr[%zu]\n", "position pick",
                got ? "a different attribute" : "nothing", want_index);
    ++g_failures;
    return;
  }
  CheckBool("position pick from export", from_export, want_from_export);
}

// ---------------------------------------------------------------------------
// Fixture 1 — the cnt=29 blob. body[0]=0 (vertex shader), body[1]=0x1B (27
// dwords), then the microcode below. Hand decode:
//
//   30052003 00001200 C4000000  kExec addr=3 count=2 seq=0x005 | kAlloc
//   00001005 00001200 C2000000  kExec addr=5 count=1 seq=0     | kAlloc
//   00001006 10071200 22000000  kExec addr=6 count=1           | kNop
//   30081000 00393A88 00000007  vfetch_full slot 0 fmt 57 off 0  stride 7 dw
//   00080000 40263688 00000300  vfetch_mini        fmt 38 off 3
//   C80F8000 00000000 E2000000  ALU export
//   ...
//
// Lowest exec address is 3, so the CF section is dwords [0, 9) — three lines,
// six CF instructions. Sequence 0x005 marks instructions 3 and 4 as fetches.
//
// That is pos3 @0 + colour4 @12, stride 28 — byte for byte the layout the raw
// vertex hex dump confirmed empirically a round ago, now read from the shader
// instead of guessed. Note position lands in dest_reg 1 and colour in dest_reg
// 0: anything picking the position attribute must key on offset and format,
// never on destination register.
constexpr uint32_t kFixturePosColor28[] = {
    0x30052003, 0x00001200, 0xC4000000,
    0x00001005, 0x00001200, 0xC2000000,
    0x00001006, 0x10071200, 0x22000000,
    0x30081000, 0x00393A88, 0x00000007,
    0x00080000, 0x40263688, 0x00000300,
    0xC80F8000, 0x00000000, 0xE2000000,
    0xC80F803E, 0x00000000, 0xE2010100,
    0xC8000000, 0x00000000, 0xE2000000,
    0x00000000, 0x00000000, 0x00000000,
};

// ---------------------------------------------------------------------------
// Fixture 2 — the cnt=26 blob, body[1]=0x18 (24 dwords). Same CF shape, a
// different layout: slot 95 (const_index 31, sel 2 -> 31*3+2), stride 5 dwords
// = 20 bytes, pos3 float @0 plus a float2 @12. 20 is one of the strides the
// renderer currently *skips*, which makes this the more valuable fixture of the
// two — it is the case the heuristic gets no chance to validate.
//
// Its vfetch_mini carries stride 5 in its own field rather than 0, unlike
// fixture 1's, which reads 0. Inheriting from the preceding full is correct
// either way and agrees with the populated value here.
constexpr uint32_t kFixturePosUv20[] = {
    0x30052003, 0x00001200, 0xC2000000,
    0x00001005, 0x00001200, 0xC4000000,
    0x00001006, 0x00002200, 0x00000000,
    0x25F81000, 0x00393A88, 0x00000005,
    0x05F80000, 0x40253FC8, 0x00000305,
    0xC80F803E, 0x00000000, 0xE2010100,
    0xC8038000, 0x00B0B000, 0xE2000000,
    0x00000000, 0x00000000, 0x00000000,
};

void TestPosColor28() {
  std::printf("fixture 1: pos3@0 + color4@12, stride 28\n");
  std::vector<mx::pm4::VertexAttribute> attrs;
  const char* fail = nullptr;
  bool saw_export = false;
  if (!mx::pm4::DecodeVertexShaderFetches(kFixturePosColor28,
                                          std::size(kFixturePosColor28), attrs,
                                          &fail, &saw_export)) {
    std::printf("  FAIL decode returned false: %s\n", fail ? fail : "(none)");
    ++g_failures;
    return;
  }
  Dump(attrs);
  CheckU32("attribute count", (uint32_t)attrs.size(), 2);
  if (attrs.size() != 2) return;

  CheckU32("attrs[0].fetch_slot", attrs[0].fetch_slot, 0);
  CheckU32("attrs[0].offset_bytes", attrs[0].offset_bytes, 0);
  CheckU32("attrs[0].stride_bytes", attrs[0].stride_bytes, 28);
  CheckU32("attrs[0].format", attrs[0].format, 57);  // k_32_32_32_FLOAT
  CheckU32("attrs[0].components", attrs[0].components, 3);
  CheckU32("attrs[0].size_bytes", attrs[0].size_bytes, 12);
  CheckU32("attrs[0].dest_reg", attrs[0].dest_reg, 1);
  CheckBool("attrs[0].from_mini", attrs[0].from_mini, false);

  CheckU32("attrs[1].fetch_slot", attrs[1].fetch_slot, 0);
  CheckU32("attrs[1].offset_bytes", attrs[1].offset_bytes, 12);
  CheckU32("attrs[1].stride_bytes", attrs[1].stride_bytes, 28);
  CheckU32("attrs[1].format", attrs[1].format, 38);  // k_32_32_32_32_FLOAT
  CheckU32("attrs[1].components", attrs[1].components, 4);
  CheckU32("attrs[1].size_bytes", attrs[1].size_bytes, 16);
  CheckU32("attrs[1].dest_reg", attrs[1].dest_reg, 0);
  CheckBool("attrs[1].from_mini", attrs[1].from_mini, true);

  // The export trace. Instruction 6 is `C80F803E 00000000 E2010100`: bits[5:0]
  // of word 0 are 62 (the position export) with export_data set, and word 2's
  // src1 names temp register 1 — the register attrs[0] was fetched into. So the
  // shader itself says attribute 0 is the position, and attribute 1 (dest_reg
  // 0, exported to register 0 by the preceding instruction) is not.
  CheckBool("saw position export", saw_export, true);
  CheckBool("attrs[0].feeds_position", attrs[0].feeds_position, true);
  CheckBool("attrs[1].feeds_position", attrs[1].feeds_position, false);
  CheckPickedPosition(attrs, 0, /*want_from_export=*/true);
}

void TestPosUv20() {
  std::printf("fixture 2: pos3@0 + float2@12, stride 20, slot 95\n");
  std::vector<mx::pm4::VertexAttribute> attrs;
  const char* fail = nullptr;
  bool saw_export = false;
  if (!mx::pm4::DecodeVertexShaderFetches(kFixturePosUv20,
                                          std::size(kFixturePosUv20), attrs,
                                          &fail, &saw_export)) {
    std::printf("  FAIL decode returned false: %s\n", fail ? fail : "(none)");
    ++g_failures;
    return;
  }
  Dump(attrs);
  CheckU32("attribute count", (uint32_t)attrs.size(), 2);
  if (attrs.size() != 2) return;

  CheckU32("attrs[0].fetch_slot", attrs[0].fetch_slot, 95);
  CheckU32("attrs[0].offset_bytes", attrs[0].offset_bytes, 0);
  CheckU32("attrs[0].stride_bytes", attrs[0].stride_bytes, 20);
  CheckU32("attrs[0].format", attrs[0].format, 57);  // k_32_32_32_FLOAT
  CheckU32("attrs[0].dest_reg", attrs[0].dest_reg, 1);
  CheckBool("attrs[0].from_mini", attrs[0].from_mini, false);

  CheckU32("attrs[1].fetch_slot", attrs[1].fetch_slot, 95);  // inherited
  CheckU32("attrs[1].offset_bytes", attrs[1].offset_bytes, 12);
  CheckU32("attrs[1].stride_bytes", attrs[1].stride_bytes, 20);
  CheckU32("attrs[1].format", attrs[1].format, 37);  // k_32_32_FLOAT
  CheckU32("attrs[1].components", attrs[1].components, 2);
  CheckU32("attrs[1].size_bytes", attrs[1].size_bytes, 8);
  CheckU32("attrs[1].dest_reg", attrs[1].dest_reg, 0);
  CheckBool("attrs[1].from_mini", attrs[1].from_mini, true);

  // Same export instruction as fixture 1, in a shader with a different layout:
  // `C80F803E 00000000 E2010100` at instruction 5. Independent confirmation that
  // the position register is not a constant of one shader.
  CheckBool("saw position export", saw_export, true);
  CheckBool("attrs[0].feeds_position", attrs[0].feeds_position, true);
  CheckBool("attrs[1].feeds_position", attrs[1].feeds_position, false);
  CheckPickedPosition(attrs, 0, /*want_from_export=*/true);
}

// A blob with fetches but no export to register 62 must leave every attribute
// unmarked and fall back to the offset/format guess — not silently claim the
// shader confirmed something. This is fixture 1 with the two export
// destinations changed from 62 and 0 to interpolators 1 and 0; everything else,
// including both fetches, is untouched.
void TestNoPositionExport() {
  std::printf("no export to register 62 falls back to the guess\n");
  uint32_t blob[std::size(kFixturePosColor28)];
  std::copy(std::begin(kFixturePosColor28), std::end(kFixturePosColor28), blob);
  blob[18] = 0xC80F8001;  // was 0xC80F803E — export to interpolator 1

  std::vector<mx::pm4::VertexAttribute> attrs;
  const char* fail = nullptr;
  bool saw_export = true;
  if (!mx::pm4::DecodeVertexShaderFetches(blob, std::size(blob), attrs, &fail,
                                          &saw_export)) {
    std::printf("  FAIL decode returned false: %s\n", fail ? fail : "(none)");
    ++g_failures;
    return;
  }
  Dump(attrs);
  CheckU32("attribute count", (uint32_t)attrs.size(), 2);
  if (attrs.size() != 2) return;
  CheckBool("saw position export", saw_export, false);
  CheckBool("attrs[0].feeds_position", attrs[0].feeds_position, false);
  CheckBool("attrs[1].feeds_position", attrs[1].feeds_position, false);
  // The fallback still answers, and on this layout it answers correctly — but
  // it must report that it guessed.
  CheckPickedPosition(attrs, 0, /*want_from_export=*/false);
}

// Executes fixture 1's ALU on the CPU and checks the position it exports.
//
// That shader's export to 62 is `C80F803E 00000000 E2010100`: vector opcode 2
// (kMax) with src1 and src2 both naming temp register 1 and both swizzles zero
// — max(r1, r1), the idiomatic Xenos move. Its vector write mask is 0xF and its
// scalar write mask is 0, so all four exported components come from the vector
// half. So whatever is seeded into r1 must come back out unchanged, which is a
// weak-looking assertion that in fact exercises the whole chain: CF bounding,
// sequence decoding, operand swizzling, the co-issue evaluation and the export
// write-mask scheme.
void TestAluPassthrough() {
  std::printf("fixture 1 executes: export 62 = max(r1,r1) = the fetched pos\n");
  std::vector<mx::pm4::VertexAttribute> attrs;
  const char* fail = nullptr;
  if (!mx::pm4::DecodeVertexShaderFetches(kFixturePosColor28,
                                          std::size(kFixturePosColor28), attrs,
                                          &fail)) {
    std::printf("  FAIL decode returned false: %s\n", fail ? fail : "(none)");
    ++g_failures;
    return;
  }
  // attrs[0] is the float3 position in r1, attrs[1] the colour in r0.
  std::vector<std::array<float, 4>> values = {{1.0f, 2.0f, 3.0f, 1.0f},
                                              {0.25f, 0.5f, 0.75f, 1.0f}};
  uint32_t consts[2048] = {};
  mx::pm4::AluInputs in;
  in.alu_consts = consts;
  in.alu_const_dwords = 2048;

  mx::pm4::AluResult r = mx::pm4::ExecuteVertexShader(
      kFixturePosColor28, std::size(kFixturePosColor28), attrs, values, in);
  std::printf("  status=%s position=(%.3f %.3f %.3f %.3f)\n",
              mx::pm4::AluStatusName(r.status), r.position[0], r.position[1],
              r.position[2], r.position[3]);
  if (r.status != mx::pm4::AluStatus::kOk) {
    std::printf("  FAIL expected ok, got %s (opcode %u)\n",
                mx::pm4::AluStatusName(r.status), r.blocking_opcode);
    ++g_failures;
    return;
  }
  const float want[4] = {1.0f, 2.0f, 3.0f, 1.0f};
  for (int c = 0; c < 4; ++c) {
    if (std::fabs(r.position[c] - want[c]) > 1e-6f) {
      std::printf("  FAIL position[%d] got %.6f, want %.6f\n", c, r.position[c],
                  want[c]);
      ++g_failures;
    }
  }
}

// The interpreter must refuse a blob it cannot execute rather than return a
// confident zero. Fixture 1 with its position export destination changed to an
// interpolator has no export to 62 at all.
void TestAluNoPosition() {
  std::printf("a shader with no export to 62 is refused, not answered\n");
  uint32_t blob[std::size(kFixturePosColor28)];
  std::copy(std::begin(kFixturePosColor28), std::end(kFixturePosColor28), blob);
  blob[18] = 0xC80F8001;

  std::vector<mx::pm4::VertexAttribute> attrs;
  const char* fail = nullptr;
  mx::pm4::DecodeVertexShaderFetches(blob, std::size(blob), attrs, &fail);
  std::vector<std::array<float, 4>> values = {{1, 2, 3, 1}, {0, 0, 0, 1}};
  uint32_t consts[2048] = {};
  mx::pm4::AluInputs in;
  in.alu_consts = consts;
  in.alu_const_dwords = 2048;

  mx::pm4::AluResult r =
      mx::pm4::ExecuteVertexShader(blob, std::size(blob), attrs, values, in);
  std::printf("  status=%s\n", mx::pm4::AluStatusName(r.status));
  CheckBool("refused with no-position-export",
            r.status == mx::pm4::AluStatus::kNoPositionExport, true);

  // And it must not crash or hang on the malformed blobs either.
  const uint32_t garbage[] = {0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF,
                              0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF};
  mx::pm4::AluResult g =
      mx::pm4::ExecuteVertexShader(garbage, std::size(garbage), attrs, values, in);
  std::printf("  garbage blob -> %s\n", mx::pm4::AluStatusName(g.status));
  CheckBool("garbage is not reported ok", g.status == mx::pm4::AluStatus::kOk,
            false);
}

// ---------------------------------------------------------------------------
// Synthetic ALU fixtures.
//
// The two blobs above are real microcode, but neither uses a0-relative
// addressing or the const-register scalar ops, so neither can test them. These
// are hand-assembled against the bit layout in rex/graphics/format/ucode.h:
//
//   word0: vector_dest:6 vector_dest_rel:1 abs_constants:1 scalar_dest:6
//          scalar_dest_rel:1 export_data:1 vector_write_mask:4
//          scalar_write_mask:4 vector_clamp:1 scalar_clamp:1 scalar_opc:6
//   word1: src3_swiz:8 src2_swiz:8 src1_swiz:8 src{3,2,1}_reg_negate:1 each
//          pred_condition:1 is_predicated:1 const_address_register_relative:1
//          const_1_rel_abs:1 const_0_rel_abs:1
//   word2: src3_reg:8 src2_reg:8 src1_reg:8 vector_opc:5
//          src{3,2,1}_sel:1 each   (sel 1 = temp register, 0 = constant)
//
// They reuse fixture 1's control flow verbatim — exec 3 count 2 (the two
// vfetches), then three single-instruction execs at 5, 6 and 7 — and only
// replace the three ALU slots. That keeps the CF encoding, which is already
// proven by the real fixtures, out of the thing under test.
constexpr size_t kAluSlot0 = 15;  // instruction index 5
constexpr size_t kAluSlot1 = 18;  // instruction index 6, the position export
constexpr size_t kAluSlot2 = 21;  // instruction index 7

// Assemble one ALU instruction. Only the fields these fixtures vary are
// parameters; everything else is zero, and scalar_opc defaults to kRetainPrev
// (50) so an instruction with no scalar half does nothing scalar.
struct Alu {
  uint32_t vector_dest = 0, vector_write_mask = 0, scalar_dest = 0,
           scalar_write_mask = 0;
  bool is_export = false;
  uint32_t scalar_opc = 50;  // kRetainPrev
  uint32_t vector_opc = 0;   // kAdd
  uint32_t src1_swiz = 0, src2_swiz = 0, src3_swiz = 0;
  uint32_t src1_reg = 0, src2_reg = 0, src3_reg = 0;
  bool src1_temp = true, src2_temp = true, src3_temp = true;
  bool const_0_rel_abs = false, const_addr_reg_relative = false;

  void Emit(uint32_t* out) const {
    out[0] = (vector_dest & 0x3F) | ((scalar_dest & 0x3F) << 8) |
             (uint32_t(is_export) << 15) | ((vector_write_mask & 0xF) << 16) |
             ((scalar_write_mask & 0xF) << 20) | ((scalar_opc & 0x3F) << 26);
    out[1] = (src3_swiz & 0xFF) | ((src2_swiz & 0xFF) << 8) |
             ((src1_swiz & 0xFF) << 16) |
             (uint32_t(const_addr_reg_relative) << 29) |
             (uint32_t(const_0_rel_abs) << 31);
    out[2] = (src3_reg & 0xFF) | ((src2_reg & 0xFF) << 8) |
             ((src1_reg & 0xFF) << 16) | ((vector_opc & 0x1F) << 24) |
             (uint32_t(src3_temp) << 29) | (uint32_t(src2_temp) << 30) |
             (uint32_t(src1_temp) << 31);
  }
};

// Run a blob built from fixture 1's CF with these three ALU slots.
mx::pm4::AluResult RunSynthetic(const Alu& a0, const Alu& a1, const Alu& a2,
                                const uint32_t* consts) {
  uint32_t blob[std::size(kFixturePosColor28)];
  std::copy(std::begin(kFixturePosColor28), std::end(kFixturePosColor28), blob);
  a0.Emit(blob + kAluSlot0);
  a1.Emit(blob + kAluSlot1);
  a2.Emit(blob + kAluSlot2);

  std::vector<mx::pm4::VertexAttribute> attrs;
  const char* fail = nullptr;
  mx::pm4::DecodeVertexShaderFetches(blob, std::size(blob), attrs, &fail);
  // attrs[0] -> r1 (position), attrs[1] -> r0 (colour).
  std::vector<std::array<float, 4>> values = {{4.0f, 5.0f, 6.0f, 1.0f},
                                              {2.0f, 0.0f, 0.0f, 0.0f}};
  mx::pm4::AluInputs in;
  in.alu_consts = consts;
  in.alu_const_dwords = 2048;
  return mx::pm4::ExecuteVertexShader(blob, std::size(blob), attrs, values, in);
}

void CheckPos(const char* what, const mx::pm4::AluResult& r, float x, float y,
              float z, float w) {
  std::printf("  %-34s status=%s pos=(%.3f %.3f %.3f %.3f)\n", what,
              mx::pm4::AluStatusName(r.status), r.position[0], r.position[1],
              r.position[2], r.position[3]);
  if (r.status != mx::pm4::AluStatus::kOk) {
    std::printf("  FAIL %s: expected ok, got %s (opcode %u)\n", what,
                mx::pm4::AluStatusName(r.status), r.blocking_opcode);
    ++g_failures;
    return;
  }
  const float want[4] = {x, y, z, w};
  for (int c = 0; c < 4; ++c) {
    if (std::fabs(r.position[c] - want[c]) > 1e-5f) {
      std::printf("  FAIL %s: position[%d] got %.6f, want %.6f\n", what, c,
                  r.position[c], want[c]);
      ++g_failures;
    }
  }
}

// Fill c[index] with four floats.
void SetConst(uint32_t* consts, uint32_t index, float x, float y, float z,
              float w) {
  const float v[4] = {x, y, z, w};
  std::memcpy(&consts[index * 4], v, sizeof(v));
}

// a0 must be written by the maxa family and consumed by relative constant
// reads. This is the assertion that proves both halves: get the sign, the
// rounding or the offset wrong and it reads a different constant.
void TestAddressRegister() {
  std::printf("a0 is written by maxa and consumed by relative constant reads\n");
  uint32_t consts[2048] = {};
  SetConst(consts, 0, -1, -1, -1, -1);
  SetConst(consts, 3, 10, 20, 30, 40);   // what a0 = +3 should reach
  SetConst(consts, 5, 50, 60, 70, 80);   // what a0 = -3 from c[8] should reach
  SetConst(consts, 8, -9, -9, -9, -9);

  // r0 was seeded (2,0,0,0), so r0.w is 0 and r0.x is 2. maxa takes its address
  // from src0.w, so to get a0 = 3 the source must have 3 in w: swizzle r1
  // (4,5,6,1) is no good, so use a constant instead. Simpler: mad-free path —
  // set a0 from a constant whose w is 3.
  SetConst(consts, 1, 0, 0, 0, 3.0f);    // c1.w = 3
  SetConst(consts, 2, 0, 0, 0, -3.0f);   // c2.w = -3

  // slot0: maxa r2, c1, c1  -> a0 = floor(c1.w + 0.5) = 3
  Alu maxa;
  maxa.vector_opc = 29;  // kMaxA
  maxa.vector_dest = 2;
  maxa.vector_write_mask = 0xF;
  maxa.src1_reg = 1; maxa.src1_temp = false;
  maxa.src2_reg = 1; maxa.src2_temp = false;

  // slot1: export 62 = add(c[0 + a0], c[0 + a0]) / 2 is not available, so just
  // read the relative constant through max, which returns it unchanged.
  Alu exp;
  exp.is_export = true;
  exp.vector_dest = 62;
  exp.vector_write_mask = 0xF;
  exp.vector_opc = 2;  // kMax
  exp.src1_reg = 0; exp.src1_temp = false;
  exp.src2_reg = 0; exp.src2_temp = false;
  exp.const_0_rel_abs = true;  // src1/src2 constant index is a0-relative

  Alu nop;  // scalar_opc kRetainPrev, no writes

  mx::pm4::AluResult r = RunSynthetic(maxa, exp, nop, consts);
  CheckPos("a0 = +3 reads c[3]", r, 10, 20, 30, 40);

  // Negative a0. c[8] with a0 = -3 must reach c[5]; a sign error reads c[11],
  // which is zero, and an off-by-one reads c[4] or c[6] — also zero. Only the
  // right answer is non-zero, so this cannot pass by accident.
  maxa.src1_reg = 2; maxa.src2_reg = 2;  // c2.w = -3
  exp.src1_reg = 8; exp.src2_reg = 8;
  mx::pm4::AluResult n = RunSynthetic(maxa, exp, nop, consts);
  CheckPos("a0 = -3 reads c[8-3] = c[5]", n, 50, 60, 70, 80);

  // aL-relative must still be refused. Same instruction, plus the bit that
  // says "the relative register is aL, not a0".
  exp.src1_reg = 0; exp.src2_reg = 0;
  exp.const_addr_reg_relative = true;
  mx::pm4::AluResult l = RunSynthetic(maxa, exp, nop, consts);
  std::printf("  %-34s status=%s\n", "aL-relative stays refused",
              mx::pm4::AluStatusName(l.status));
  CheckBool("aL-relative refused", l.status == mx::pm4::AluStatus::kLoopRelative,
            true);
}

// The mulsc/addsc/subsc family: one constant operand named directly by src3,
// one temp whose index is scattered across the opcode's low bit, src3_sel and
// src3_swiz. Reconstruct that index backwards and the test reads register n^1.
void TestConstRegScalarOps() {
  std::printf("mulsc/addsc/subsc read one constant and one temp\n");
  uint32_t consts[2048] = {};
  SetConst(consts, 7, 3.0f, 0, 0, 0);  // the constant operand: c7.x = 3

  // The temp operand must be a register we seeded. r0 holds the colour
  // (2,0,0,0), so r0.x = 2. Encoding r0 means
  // scalar_const_reg_op_src_temp_reg() == 0, i.e. opcode low bit 0, src3_sel 0
  // and src3_swiz[2:5] 0 — so use the _0 form of each opcode with src3_sel
  // clear. src3_swiz low 2 bits select component x.
  struct Case { const char* name; uint32_t opc; float want; };
  const Case cases[] = {
      {"mulsc0  c7.x * r0.x", 42, 6.0f},
      {"addsc0  c7.x + r0.x", 44, 5.0f},
      {"subsc0  c7.x - r0.x", 46, 1.0f},
  };

  for (const Case& c : cases) {
    // slot1 exports to 62: vector half writes nothing (mask 0), scalar half
    // writes all four components with the scalar result.
    Alu exp;
    exp.is_export = true;
    exp.vector_dest = 62;
    exp.vector_write_mask = 0x0;
    exp.scalar_write_mask = 0xF;
    exp.scalar_opc = c.opc;
    exp.src3_reg = 7;         // the constant index
    exp.src3_temp = false;    // src3_sel 0 -> temp reg bit 1 clear
    exp.src3_swiz = 0;        // component x, and temp index bits [2:5] zero
    // The vector half must not fault: with vector_write_mask 0 on an export it
    // still evaluates, so give it something harmless reading temps.
    exp.vector_opc = 2;  // kMax
    exp.src1_reg = 0; exp.src2_reg = 0;

    Alu nop;
    mx::pm4::AluResult r = RunSynthetic(nop, exp, nop, consts);
    CheckPos(c.name, r, c.want, c.want, c.want, c.want);
  }

  // And the temp index really is reconstructed, not assumed zero: the _1 form
  // sets the opcode's low bit, which selects r1 — the position (4,5,6,1), so
  // r1.x = 4 and mulsc1 must give 12 rather than 6.
  Alu exp;
  exp.is_export = true;
  exp.vector_dest = 62;
  exp.vector_write_mask = 0x0;
  exp.scalar_write_mask = 0xF;
  exp.scalar_opc = 43;  // kMulsc1 -> temp register 1
  exp.src3_reg = 7;
  exp.src3_temp = false;
  exp.src3_swiz = 0;
  exp.vector_opc = 2;
  exp.src1_reg = 0; exp.src2_reg = 0;
  Alu nop;
  mx::pm4::AluResult r = RunSynthetic(nop, exp, nop, consts);
  CheckPos("mulsc1  c7.x * r1.x", r, 12, 12, 12, 12);
}

// Malformed input must be rejected, not crash and not spin. Each case is a
// blob the decoder could plausibly be handed by a desynced parser.
void TestMalformed() {
  std::printf("malformed blobs are rejected, not crashed on\n");
  struct Case {
    const char* name;
    std::vector<uint32_t> dwords;
  };
  const Case cases[] = {
      {"empty", {}},
      {"one dword", {0x30052003}},
      {"two dwords (partial CF)", {0x30052003, 0x00001200}},
      // exec target 0xFFF is far past the end of the blob.
      {"exec address past end", {0x30052FFF, 0x00001200, 0xC4000000}},
      // All ones: opcode nibbles decode to kMarkVsFetchDone, huge addresses.
      {"all ones", {0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF}},
      {"all zero", {0, 0, 0, 0, 0, 0}},
      // Claims two fetch instructions at address 3 but stops at dword 9.
      {"truncated after CF",
       {0x30052003, 0x00001200, 0xC4000000, 0x00001005, 0x00001200, 0xC2000000,
        0x00001006, 0x10071200, 0x22000000}},
  };

  for (const auto& c : cases) {
    std::vector<mx::pm4::VertexAttribute> attrs;
    const char* fail = nullptr;
    const bool ok = mx::pm4::DecodeVertexShaderFetches(
        c.dwords.empty() ? nullptr : c.dwords.data(),
        (uint32_t)c.dwords.size(), attrs, &fail);
    // Either outcome is acceptable as long as it terminates and reports a
    // reason when it fails; what must never happen is a crash, a hang, or a
    // bogus attribute. A blob with no fetches legitimately decodes to zero.
    if (ok && !attrs.empty()) {
      std::printf("  FAIL %-24s produced %zu attributes from garbage\n",
                  c.name, attrs.size());
      ++g_failures;
    } else if (!ok && !fail) {
      std::printf("  FAIL %-24s returned false with no reason\n", c.name);
      ++g_failures;
    } else {
      std::printf("  ok   %-24s %s\n", c.name,
                  ok ? "decoded, 0 attributes" : fail);
    }
  }
}

}  // namespace

int main() {
  TestPosColor28();
  TestPosUv20();
  TestNoPositionExport();
  TestAluPassthrough();
  TestAluNoPosition();
  TestAddressRegister();
  TestConstRegScalarOps();
  TestMalformed();
  if (g_failures) {
    std::printf("\n%d check(s) FAILED\n", g_failures);
    return 1;
  }
  std::printf("\nall checks passed\n");
  return 0;
}
