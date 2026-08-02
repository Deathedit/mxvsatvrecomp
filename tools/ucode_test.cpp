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

#include <cstdio>
#include <cstdint>
#include <vector>

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
        "size=%-2u dest=r%u swiz=0x%03X%s%s exp=%d%s\n",
        i, a.fetch_slot, a.offset_bytes, a.stride_bytes, a.format,
        a.components, a.size_bytes, a.dest_reg, a.dest_swizzle,
        a.is_signed ? " signed" : "", a.is_normalized ? " norm" : "",
        a.exp_adjust, a.from_mini ? " (mini)" : " (full)");
  }
}

void CheckBool(const char* what, bool got, bool want) {
  if (got == want) return;
  std::printf("  FAIL %-28s got %s, want %s\n", what, got ? "true" : "false",
              want ? "true" : "false");
  ++g_failures;
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
  if (!mx::pm4::DecodeVertexShaderFetches(kFixturePosColor28,
                                          std::size(kFixturePosColor28), attrs,
                                          &fail)) {
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
}

void TestPosUv20() {
  std::printf("fixture 2: pos3@0 + float2@12, stride 20, slot 95\n");
  std::vector<mx::pm4::VertexAttribute> attrs;
  const char* fail = nullptr;
  if (!mx::pm4::DecodeVertexShaderFetches(kFixturePosUv20,
                                          std::size(kFixturePosUv20), attrs,
                                          &fail)) {
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
  TestMalformed();
  if (g_failures) {
    std::printf("\n%d check(s) FAILED\n", g_failures);
    return 1;
  }
  std::printf("\nall checks passed\n");
  return 0;
}
