// Ground-truth tests for the Xenos vertex shader microcode decoder.
//
// Built ad hoc, not part of MX_SOURCES:
//   clang++ -std=c++23 -I src -I C:/rexglue-sdk/include \
//       -o ucode_test.exe tools/ucode_test.cpp src/gpu/shader_ucode.cpp
//
// The fixtures are verbatim IM_LOAD_IMMEDIATE (0x2B) payloads captured from a
// real frame. Their expected decodes were worked out by hand against the bit
// layouts in ucode.h before any of this was written, which is the point: it
// catches packing, endianness and bit-offset errors in seconds instead of
// costing a 75-second game run each.

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
void Dump(const std::vector<mx::hle::VertexAttribute>& attrs) {
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
void CheckPickedPosition(const std::vector<mx::hle::VertexAttribute>& attrs,
                         size_t want_index, bool want_from_export) {
  bool from_export = false;
  const mx::hle::VertexAttribute* got =
      mx::hle::PickPositionAttribute(attrs, &from_export);
  if (got != &attrs[want_index]) {
    std::printf("  FAIL %-28s picked %s, want attr[%zu]\n", "position pick",
                got ? "a different attribute" : "nothing", want_index);
    ++g_failures;
    return;
  }
  CheckBool("position pick from export", from_export, want_from_export);
}

// ---------------------------------------------------------------------------
// Fixture 1 -- the cnt=29 blob. body[0]=0 (vertex shader), body[1]=0x1B (27
// dwords), then the microcode below. Hand decode:
//
//   30052003 00001200 C4000000  kExec addr=3 count=2 seq=0x005 | kAlloc
//   00001005 00001200 C2000000  kExec addr=5 count=1 seq=0     | kAlloc
//   00001006 10071200 22000000  kExec addr=6 count=1           | kNop
//   30081000 00393A88 00000007  vfetch_full slot 0 fmt 57 off 0  stride 7 dw
//   00080000 40263688 00000300  vfetch_mini        fmt 38 off 3
//   C80F8000 00000000 E2000000  ALU export
//
// Lowest exec address is 3, so the CF section is dwords [0, 9). Sequence 0x005
// marks instructions 3 and 4 as fetches.
//
// That is pos3 @0 + colour4 @12, stride 28. Note position lands in dest_reg 1
// and colour in dest_reg 0: anything picking the position attribute must key on
// offset and format, never on destination register.
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
// Fixture 2 -- the cnt=26 blob, body[1]=0x18 (24 dwords). Same CF shape, a
// different layout: slot 95, stride 5 dwords = 20 bytes, pos3 float @0 plus a
// float2 @12. 20 is one of the strides the renderer *skips*, which makes this
// the more valuable fixture. Its vfetch_mini carries stride 5 in its own field
// rather than 0, unlike fixture 1's; inheriting from the preceding full is
// correct either way.
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
  std::vector<mx::hle::VertexAttribute> attrs;
  const char* fail = nullptr;
  bool saw_export = false;
  if (!mx::hle::DecodeVertexShaderFetches(kFixturePosColor28,
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
  // shader itself says attribute 0 is the position.
  CheckBool("saw position export", saw_export, true);
  CheckBool("attrs[0].feeds_position", attrs[0].feeds_position, true);
  CheckBool("attrs[1].feeds_position", attrs[1].feeds_position, false);
  CheckPickedPosition(attrs, 0, /*want_from_export=*/true);
}

void TestPosUv20() {
  std::printf("fixture 2: pos3@0 + float2@12, stride 20, slot 95\n");
  std::vector<mx::hle::VertexAttribute> attrs;
  const char* fail = nullptr;
  bool saw_export = false;
  if (!mx::hle::DecodeVertexShaderFetches(kFixturePosUv20,
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

void TestVertexShaderStructureMatch() {
  std::printf("vertex template matches only across proven patch fields\n");
  uint32_t shader_template[std::size(kFixturePosColor28)];
  std::copy(std::begin(kFixturePosColor28), std::end(kFixturePosColor28),
            shader_template);

  // Both executable vfetches may have their declaration-dependent fields
  // rewritten and must retain the same structural identity.
  for (uint32_t at : {9u, 12u}) {
    shader_template[at + 0] ^= 0x3FF00000u;
    shader_template[at + 1] ^= 0x003F3FFFu;
    shader_template[at + 2] ^= 0x7FFFFFFFu;
  }
  CheckBool("patched vfetch fields ignored",
            mx::hle::VertexShaderStructureMatches(
                shader_template, kFixturePosColor28,
                std::size(kFixturePosColor28)),
            true);

  // The opcode is not patched by the D3D9 routine. A one-bit difference there
  // must reject even though every control-flow and ALU dword still agrees.
  shader_template[9] ^= 1u;
  CheckBool("non-patch vfetch bit retained",
            mx::hle::VertexShaderStructureMatches(
                shader_template, kFixturePosColor28,
                std::size(kFixturePosColor28)),
            false);
}

// A blob with fetches but no export to register 62 must leave every attribute
// unmarked and fall back to the offset/format guess -- not silently claim the
// shader confirmed something. This is fixture 1 with the two export destinations
// changed from 62 and 0 to interpolators 1 and 0.
void TestNoPositionExport() {
  std::printf("no export to register 62 falls back to the guess\n");
  uint32_t blob[std::size(kFixturePosColor28)];
  std::copy(std::begin(kFixturePosColor28), std::end(kFixturePosColor28), blob);
  blob[18] = 0xC80F8001;  // was 0xC80F803E — export to interpolator 1

  std::vector<mx::hle::VertexAttribute> attrs;
  const char* fail = nullptr;
  bool saw_export = true;
  if (!mx::hle::DecodeVertexShaderFetches(blob, std::size(blob), attrs, &fail,
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
// -- max(r1, r1), the idiomatic Xenos move. Its vector write mask is 0xF and its
// scalar write mask 0, so all four exported components come from the vector
// half. Whatever is seeded into r1 must come back unchanged, which is a
// weak-looking assertion that in fact exercises CF bounding, sequence decoding,
// operand swizzling, co-issue evaluation and the export write-mask scheme.
void TestAluPassthrough() {
  std::printf("fixture 1 executes: export 62 = max(r1,r1) = the fetched pos\n");
  std::vector<mx::hle::VertexAttribute> attrs;
  const char* fail = nullptr;
  if (!mx::hle::DecodeVertexShaderFetches(kFixturePosColor28,
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
  mx::hle::AluInputs in;
  in.alu_consts = consts;
  in.alu_const_dwords = 2048;

  mx::hle::AluResult r = mx::hle::ExecuteVertexShader(
      kFixturePosColor28, std::size(kFixturePosColor28), attrs, values, in);
  std::printf("  status=%s position=(%.3f %.3f %.3f %.3f)\n",
              mx::hle::AluStatusName(r.status), r.position[0], r.position[1],
              r.position[2], r.position[3]);
  if (r.status != mx::hle::AluStatus::kOk) {
    std::printf("  FAIL expected ok, got %s (opcode %u)\n",
                mx::hle::AluStatusName(r.status), r.blocking_opcode);
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

  std::vector<mx::hle::VertexAttribute> attrs;
  const char* fail = nullptr;
  mx::hle::DecodeVertexShaderFetches(blob, std::size(blob), attrs, &fail);
  std::vector<std::array<float, 4>> values = {{1, 2, 3, 1}, {0, 0, 0, 1}};
  uint32_t consts[2048] = {};
  mx::hle::AluInputs in;
  in.alu_consts = consts;
  in.alu_const_dwords = 2048;

  mx::hle::AluResult r =
      mx::hle::ExecuteVertexShader(blob, std::size(blob), attrs, values, in);
  std::printf("  status=%s\n", mx::hle::AluStatusName(r.status));
  CheckBool("refused with no-position-export",
            r.status == mx::hle::AluStatus::kNoPositionExport, true);
  CheckBool("general export 1 retained", (r.export_mask & (1u << 1)) != 0,
            true);
  CheckBool("general export 1 x", std::fabs(r.exports[1][0] - 1.0f) < 1e-6f,
            true);

  // And it must not crash or hang on the malformed blobs either.
  const uint32_t garbage[] = {0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF,
                              0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF};
  mx::hle::AluResult g =
      mx::hle::ExecuteVertexShader(garbage, std::size(garbage), attrs, values, in);
  std::printf("  garbage blob -> %s\n", mx::hle::AluStatusName(g.status));
  CheckBool("garbage is not reported ok", g.status == mx::hle::AluStatus::kOk,
            false);
}

// ---------------------------------------------------------------------------
// Synthetic ALU fixtures.
//
// The two blobs above are real microcode, but neither uses a0-relative
// addressing or the const-register scalar ops. These are hand-assembled against
// the bit layout in ucode.h:
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
// They reuse fixture 1's control flow verbatim and replace only the three ALU
// slots, which keeps the CF encoding out of the thing under test.
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
mx::hle::AluResult RunSynthetic(const Alu& a0, const Alu& a1, const Alu& a2,
                                const uint32_t* consts) {
  uint32_t blob[std::size(kFixturePosColor28)];
  std::copy(std::begin(kFixturePosColor28), std::end(kFixturePosColor28), blob);
  a0.Emit(blob + kAluSlot0);
  a1.Emit(blob + kAluSlot1);
  a2.Emit(blob + kAluSlot2);

  std::vector<mx::hle::VertexAttribute> attrs;
  const char* fail = nullptr;
  mx::hle::DecodeVertexShaderFetches(blob, std::size(blob), attrs, &fail);
  // attrs[0] -> r1 (position), attrs[1] -> r0 (colour).
  std::vector<std::array<float, 4>> values = {{4.0f, 5.0f, 6.0f, 1.0f},
                                              {2.0f, 0.0f, 0.0f, 0.0f}};
  mx::hle::AluInputs in;
  in.alu_consts = consts;
  in.alu_const_dwords = 2048;
  return mx::hle::ExecuteVertexShader(blob, std::size(blob), attrs, values, in);
}

void CheckPos(const char* what, const mx::hle::AluResult& r, float x, float y,
              float z, float w) {
  std::printf("  %-34s status=%s pos=(%.3f %.3f %.3f %.3f)\n", what,
              mx::hle::AluStatusName(r.status), r.position[0], r.position[1],
              r.position[2], r.position[3]);
  if (r.status != mx::hle::AluStatus::kOk) {
    std::printf("  FAIL %s: expected ok, got %s (opcode %u)\n", what,
                mx::hle::AluStatusName(r.status), r.blocking_opcode);
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
  // from src0.w, so to get a0 = 3 the source must have 3 in w -- use a constant
  // whose w is 3.
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
  exp.const_0_rel_abs = true;  // src1/src2 constant index is relative at all
  // ...and *which* register it is relative to. The polarity is what this test
  // exists to pin, because it reads backwards: the bit is named "address
  // register relative", and the address register is a0, so
  //   true  = c[a0 + n]   (AddressingMode::kAbsolute = 1, ucode.h:196)
  //   false = c[aL + n]   (AddressingMode::kRelative = 0, ucode.h:193)
  // This test previously had both cases the wrong way round, which made it agree
  // with an inverted condition in Src() instead of catching it.
  exp.const_addr_reg_relative = true;

  Alu nop;  // scalar_opc kRetainPrev, no writes

  mx::hle::AluResult r = RunSynthetic(maxa, exp, nop, consts);
  CheckPos("a0 = +3 reads c[3]", r, 10, 20, 30, 40);

  // Negative a0. c[8] with a0 = -3 must reach c[5]; a sign error reads c[11],
  // which is zero, and an off-by-one reads c[4] or c[6] — also zero. Only the
  // right answer is non-zero, so this cannot pass by accident.
  maxa.src1_reg = 2; maxa.src2_reg = 2;  // c2.w = -3
  exp.src1_reg = 8; exp.src2_reg = 8;
  mx::hle::AluResult n = RunSynthetic(maxa, exp, nop, consts);
  CheckPos("a0 = -3 reads c[8-3] = c[5]", n, 50, 60, 70, 80);

  // aL-relative must still be refused. Same instruction, with the selector
  // cleared so the index is relative to the loop counter rather than to a0.
  // Nothing in this game's shaders takes this branch, but it has to stay visible
  // if one ever does.
  exp.src1_reg = 0; exp.src2_reg = 0;
  exp.const_addr_reg_relative = false;
  mx::hle::AluResult l = RunSynthetic(maxa, exp, nop, consts);
  std::printf("  %-34s status=%s\n", "aL-relative stays refused",
              mx::hle::AluStatusName(l.status));
  CheckBool("aL-relative refused", l.status == mx::hle::AluStatus::kLoopRelative,
            true);
}

// The mulsc/addsc/subsc family: one constant operand named directly by src3, one
// temp whose index is scattered across the opcode low bit, src3_sel and
// src3_swiz.
//
// THE SRC3_SWIZ FIELD IS SPLIT THREE WAYS. From the SDK:
//
//   scalar_const_reg_op_src_temp_reg() =
//       (scalar_opc & 1) | (src3_sel << 1) | (src3_swiz & 0x3C)
//
// so bits [5:2] are the TEMP REGISTER, leaving [1:0] and [7:6] as swizzles.
// They are RELATIVE swizzles -- ((swizzle >> (2 * i)) + i) & 3 -- and the scalar
// half uses the Xenos AB = WX convention, so the CONSTANT is selected W-relative
// out of bits [7:6] and the TEMP X-relative out of bits [1:0].
//
// Setting src3_swiz = 0 and calling bits [1:0] the constant component reads c7.w
// instead. The constant now has DISTINCT values in every component, so selecting
// the wrong one is a loud wrong number rather than a zero that reads as absent.
void TestConstRegScalarOps() {
  std::printf("mulsc/addsc/subsc read one constant and one temp\n");
  uint32_t consts[2048] = {};
  SetConst(consts, 7, 3.0f, 70.0f, 80.0f, 90.0f);

  // Constant component x: ((swiz >> 6) + 3) & 3 == 0 needs bits [7:6] == 1.
  // Temp component x: ((swiz >> 0) + 0) & 3 == 0 needs bits [1:0] == 0.
  // Temp register 0: bits [5:2] == 0, with the _0 form of each opcode.
  constexpr uint32_t kConstX_TempX = 0x40u;

  // r0 holds the colour (2,0,0,0) and r1 the position (4,5,6,1).
  struct Case { const char* name; uint32_t opc; uint32_t swiz; float want; };
  const Case cases[] = {
      {"mulsc0  c7.x * r0.x", 42, kConstX_TempX, 6.0f},
      {"addsc0  c7.x + r0.x", 44, kConstX_TempX, 5.0f},
      {"subsc0  c7.x - r0.x", 46, kConstX_TempX, 1.0f},
      // The temp index is reconstructed, not assumed zero: the _1 form sets
      // the opcode low bit, which selects r1. 3 * r1.x = 3 * 4.
      {"mulsc1  c7.x * r1.x", 43, kConstX_TempX, 12.0f},
      // The constant really is W-relative. bits [7:6] == 2 gives
      // ((2) + 3) & 3 == 1, i.e. c7.y == 70, so 70 * r0.x = 140. Reading it
      // X-relative instead would give c7.z and 160.
      {"mulsc0  c7.y * r0.x (W-relative)", 42, 0x80u, 140.0f},
      // And the temp really is X-relative out of bits [1:0]. 1 gives
      // ((1) + 0) & 3 == 1, i.e. r1.y == 5, so 3 * 5 = 15.
      {"mulsc1  c7.x * r1.y (X-relative)", 43, kConstX_TempX | 0x1u, 15.0f},
  };

  for (const Case& c : cases) {
    // slot1 exports to 62: the vector half writes nothing (mask 0), the scalar
    // half writes all four components with the scalar result.
    Alu exp;
    exp.is_export = true;
    exp.vector_dest = 62;
    exp.vector_write_mask = 0x0;
    exp.scalar_write_mask = 0xF;
    exp.scalar_opc = c.opc;
    exp.src3_reg = 7;         // the constant index
    exp.src3_temp = false;    // src3_sel 0 -> temp reg bit 1 clear
    exp.src3_swiz = c.swiz;
    // The vector half must not fault: with vector_write_mask 0 on an export it
    // still evaluates, so give it something harmless reading temps.
    exp.vector_opc = 2;  // kMax
    exp.src1_reg = 0; exp.src2_reg = 0;

    Alu nop;
    mx::hle::AluResult r = RunSynthetic(nop, exp, nop, consts);
    CheckPos(c.name, r, c.want, c.want, c.want, c.want);
  }
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
    std::vector<mx::hle::VertexAttribute> attrs;
    const char* fail = nullptr;
    const bool ok = mx::hle::DecodeVertexShaderFetches(
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

void TestPixelTextureBinding() {
  std::printf("single 2D pixel texture fetch links sampler and UV register\n");
  uint32_t blob[std::size(kFixturePosColor28)];
  std::copy(std::begin(kFixturePosColor28), std::end(kFixturePosColor28), blob);
  // Instruction 3 is selected as a fetch by fixture 1's first exec. Encode a
  // tfetch2D using sampler 5 and coordinates from r7.
  blob[9] = 1u | (7u << 5) | (3u << 12) | (5u << 20) | (1u << 25);
  blob[10] = 0;
  blob[11] = 1u << 14;  // FetchOpDimension::k2D.
  mx::hle::PixelTextureBinding binding;
  const char* fail = nullptr;
  CheckBool("single texture accepted",
            mx::hle::DecodeSingleTexturePixelShader(
                blob, std::size(blob), binding, &fail), true);
  CheckU32("pixel sampler", binding.sampler, 5);
  CheckU32("pixel UV source", binding.src_reg, 7);
  CheckBool("unnormalized coordinates", binding.unnormalized, true);

  // A second tfetch in the same exec is outside the supported profile.
  blob[12] = blob[9];
  blob[13] = blob[10];
  blob[14] = blob[11];
  std::vector<mx::hle::PixelTextureBinding> bindings;
  CheckBool("multiple textures enumerated",
            mx::hle::DecodePixelTextureFetches(
                blob, std::size(blob), bindings, &fail), true);
  CheckU32("pixel fetch count", uint32_t(bindings.size()), 2);
  CheckU32("second pixel sampler", bindings[1].sampler, 5);
  CheckU32("second pixel UV source", bindings[1].src_reg, 7);
  CheckBool("multiple textures rejected",
            mx::hle::DecodeSingleTexturePixelShader(
                blob, std::size(blob), binding, &fail), false);
  blob[12] = kFixturePosColor28[12];
  blob[13] = kFixturePosColor28[13];
  blob[14] = kFixturePosColor28[14];
  blob[11] = 2u << 14;  // 3D/stacked.
  CheckBool("3D texture rejected",
            mx::hle::DecodeSingleTexturePixelShader(
                blob, std::size(blob), binding, &fail), false);
}

}  // namespace

int main() {
  TestPosColor28();
  TestPosUv20();
  TestVertexShaderStructureMatch();
  TestNoPositionExport();
  TestAluPassthrough();
  TestAluNoPosition();
  TestAddressRegister();
  TestConstRegScalarOps();
  TestPixelTextureBinding();
  TestMalformed();
  if (g_failures) {
    std::printf("\n%d check(s) FAILED\n", g_failures);
    return 1;
  }
  std::printf("\nall checks passed\n");
  return 0;
}
