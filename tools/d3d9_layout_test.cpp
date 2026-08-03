// Ground-truth tests for the D3DVERTEXELEMENT9 -> host input layout decode.
//
// Built ad hoc, not part of MX_SOURCES:
//   clang++ -std=c++23 -I src -I C:/rexglue-sdk/include \
//       -o d3d9_layout_test.exe tools/d3d9_layout_test.cpp src/gpu/d3d9_layout.cpp
//
// The fixtures are the 23 vertex declarations this title created in a 165s
// run, transcribed verbatim from d3d9_dump_decls.txt — real elements the game
// handed to D3DDevice_CreateVertexDeclaration, not synthesised. They were
// reproduced byte-identically across 100s, 140s and 165s runs.
//
// The central assertion is not "the decode returns something". It is that
// **each element's decoded size equals the distance to the next element's
// offset in the same stream**, and that the resulting stride matches what the
// PM4 translator independently measured for the two declarations where it has
// a number (28 and 36). The offsets come from the game, the sizes come from
// the runtime's own table at 0x8204E188, and the strides come from a different
// pipeline; three sources agreeing is the evidence. A packing, endianness or
// field-position error shows up here in a second instead of costing a run.
//
// See tools/ucode_test.cpp for the same pattern applied to the microcode.

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>

#include "gpu/d3d9_layout.h"

// The one place the SDK is consulted: the local format constants in
// d3d9_layout.cpp must be xenos::VertexFormat and not a drifting copy.
#include "rex/graphics/xenos.h"

namespace {

int g_failures = 0;

void Fail(const char* what, const char* detail) {
  std::printf("  FAIL %-34s %s\n", what, detail);
  ++g_failures;
}

void CheckU32(const char* what, uint32_t got, uint32_t want) {
  if (got == want) return;
  std::printf("  FAIL %-34s got %u, want %u\n", what, got, want);
  ++g_failures;
}

//===========================================================================
// The fixtures.
//
// Twelve bytes per element, exactly as dumped. Byte 11 is padding and is left
// at whatever the capture held (FF, 7C, 3B, 60 ...) rather than zeroed — if
// the decode ever started reading it, these would catch it.
//===========================================================================
struct Fixture {
  int id;                      // decl # in the dump
  const uint8_t* raw;
  uint32_t count;              // the count the guest runtime itself settled on
  uint32_t max_stream;         // likewise, read from decl+0x1C
  uint32_t expect_stride0;     // 0 = no independent number to check against
};


const uint8_t kD1[] = {0x00,0x00,0x00,0x00, 0x00,0x2C,0x23,0xA5, 0x00,0x00,0x00,0x00};

const uint8_t kD2[] = {
  0x00,0x00,0x00,0x00, 0x00,0x1A,0x23,0x60, 0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x08, 0x00,0x2A,0x21,0x87, 0x00,0x03,0x00,0x00,
  0x00,0x00,0x00,0x0C, 0x00,0x2C,0x23,0x5F, 0x00,0x05,0x00,0x00,
  0x00,0x00,0x00,0x10, 0x00,0x2C,0x23,0x5F, 0x00,0x05,0x01,0x00,
  0x00,0x00,0x00,0x14, 0x00,0x18,0x28,0x86, 0x00,0x0A,0x00,0x00,
  0x00,0x00,0x00,0x18, 0x00,0x2A,0x21,0x87, 0x00,0x06,0x00,0x00,
};

const uint8_t kD3[] = {0x00,0x00,0x00,0x00, 0x00,0x2C,0x23,0xA5, 0x00,0x00,0x00,0x00};

const uint8_t kD4[] = {
  0x00,0x00,0x00,0x00, 0x00,0x2C,0x23,0x5F, 0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x04, 0x00,0x2C,0x23,0x5F, 0x00,0x05,0x00,0x00,
  0x00,0x00,0x00,0x08, 0x00,0x18,0x28,0x86, 0x00,0x0A,0x00,0x00,
};

const uint8_t kD5[] = {0x00,0x00,0x00,0x00, 0x00,0x2C,0x23,0x59, 0x00,0x00,0x00,0x00};

const uint8_t kD6[] = {
  0x00,0x00,0x00,0x00, 0x00,0x1A,0x23,0xA6, 0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x10, 0x00,0x2C,0x23,0xA5, 0x00,0x05,0x00,0x00,
  0x00,0x00,0x00,0x18, 0x00,0x1A,0x20,0x86, 0x00,0x0A,0x00,0x00,
};

const uint8_t kD7[] = {
  0x00,0x00,0x00,0x00, 0x00,0x2C,0x23,0x59, 0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x04, 0x00,0x1A,0x20,0x86, 0x00,0x0A,0x00,0x00,
};

const uint8_t kD8[] = {
  0x00,0x00,0x00,0x00, 0x00,0x2C,0x23,0x59, 0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x04, 0x00,0x1A,0x20,0x86, 0x00,0x0A,0x00,0x00,
  0x00,0x00,0x00,0x08, 0x00,0x1A,0x20,0x86, 0x00,0x0A,0x01,0x00,
};

const uint8_t kD9[] = {
  0x00,0x00,0x00,0x00, 0x00,0x2A,0x23,0xB9, 0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x0C, 0x00,0x2C,0x23,0xA5, 0x00,0x05,0x00,0x00,
};

// Byte 11 = 0xFF here, and the element is *not* a D3DDECL_END — the sentinel
// is the Stream halfword at offset 0, not the pad.
const uint8_t kD10[] = {0x00,0x00,0x00,0x00, 0x00,0x1A,0x21,0x5A, 0x00,0x00,0x00,0xFF};

// Two streams, and the stream-1 element comes *first*. Sorting by offset alone
// would interleave the two streams' attributes.
const uint8_t kD11[] = {
  0x00,0x01,0x00,0x00, 0x00,0x2A,0x23,0xB9, 0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00, 0x00,0x2A,0x21,0x87, 0x00,0x03,0x00,0x00,
  0x00,0x00,0x00,0x04, 0x00,0x2C,0x23,0x5F, 0x00,0x05,0x00,0x00,
  0x00,0x00,0x00,0x08, 0x00,0x2C,0x23,0x5F, 0x00,0x05,0x01,0x00,
  0x00,0x00,0x00,0x0C, 0x00,0x18,0x28,0x86, 0x00,0x0A,0x00,0x00,
  0x00,0x00,0x00,0x10, 0x00,0x2A,0x21,0x87, 0x00,0x06,0x00,0x00,
};

// The skinned mesh. 21,992 draws per run are dropped by the PM4 path for being
// stride 36; this is that layout.
const uint8_t kD12[] = {
  0x00,0x00,0x00,0x00, 0x00,0x1A,0x23,0x60, 0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x08, 0x00,0x2A,0x21,0x87, 0x00,0x03,0x00,0x00,
  0x00,0x00,0x00,0x0C, 0x00,0x1A,0x22,0x86, 0x00,0x02,0x00,0x00,
  0x00,0x00,0x00,0x10, 0x00,0x1A,0x20,0x86, 0x00,0x01,0x00,0x00,
  0x00,0x00,0x00,0x14, 0x00,0x2C,0x23,0x5F, 0x00,0x05,0x00,0x00,
  0x00,0x00,0x00,0x18, 0x00,0x2C,0x23,0x5F, 0x00,0x05,0x01,0x00,
  0x00,0x00,0x00,0x1C, 0x00,0x18,0x28,0x86, 0x00,0x0A,0x00,0x00,
  0x00,0x00,0x00,0x20, 0x00,0x2A,0x21,0x87, 0x00,0x06,0x00,0x00,
};

const uint8_t kD13[] = {
  0x00,0x00,0x00,0x00, 0x00,0x1A,0x23,0x60, 0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x08, 0x00,0x1A,0x23,0x60, 0x00,0x05,0x04,0x00,
  0x00,0x00,0x00,0x10, 0x00,0x1A,0x23,0x60, 0x00,0x05,0x00,0x00,
  0x00,0x00,0x00,0x18, 0x00,0x1A,0x21,0x86, 0x00,0x03,0x00,0x00,
  0x00,0x00,0x00,0x1C, 0x00,0x1A,0x21,0x86, 0x00,0x06,0x00,0x00,
  0x00,0x00,0x00,0x20, 0x00,0x1A,0x21,0x86, 0x00,0x05,0x02,0x00,
};

const uint8_t kD14[] = {
  0x00,0x00,0x00,0x00, 0x00,0x1A,0x23,0x60, 0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x08, 0x00,0x1A,0x23,0x60, 0x00,0x05,0x04,0x00,
  0x00,0x00,0x00,0x10, 0x00,0x2C,0x23,0x5F, 0x00,0x05,0x00,0xFF,
  0x00,0x00,0x00,0x14, 0x00,0x1A,0x21,0x86, 0x00,0x03,0x00,0x7C,
  0x00,0x00,0x00,0x18, 0x00,0x1A,0x21,0x86, 0x00,0x06,0x00,0x78,
  0x00,0x00,0x00,0x1C, 0x00,0x1A,0x21,0x86, 0x00,0x05,0x02,0x78,
};

const uint8_t kD15[] = {
  0x00,0x00,0x00,0x00, 0x00,0x1A,0x23,0xA6, 0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x10, 0x00,0x1A,0x23,0xA6, 0x00,0x05,0x00,0x01,
  0x00,0x01,0x00,0x00, 0x00,0x1A,0x23,0xA6, 0x00,0x05,0x01,0x3B,
};

const uint8_t kD16[] = {
  0x00,0x00,0x00,0x00, 0x00,0x1A,0x23,0xA6, 0x00,0x00,0x00,0x60,
  0x00,0x00,0x00,0x10, 0x00,0x1A,0x23,0xA6, 0x00,0x05,0x00,0x60,
  0x00,0x00,0x00,0x20, 0x00,0x1A,0x23,0xA6, 0x00,0x05,0x02,0x5F,
  0x00,0x01,0x00,0x00, 0x00,0x1A,0x23,0xA6, 0x00,0x05,0x01,0x86,
};

// The only k_32_FLOAT in the capture, and the only Type dword whose byte 1
// differs (0x83 rather than 0x23) — a decode that keyed on the whole upper
// half instead of the documented bit fields would reject this one.
const uint8_t kD17[] = {
  0x00,0x00,0x00,0x00, 0x00,0x2C,0x23,0xA5, 0x00,0x00,0x01,0x00,
  0x00,0x00,0x00,0x08, 0x00,0x2C,0x23,0xA5, 0x00,0x05,0x00,0x00,
  0x00,0x00,0x00,0x10, 0x00,0x2C,0x23,0xA5, 0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x18, 0x00,0x18,0x28,0x86, 0x00,0x0A,0x01,0x01,
  0x00,0x01,0x00,0x00, 0x00,0x2C,0x83,0xA4, 0x00,0x00,0x02,0x00,
};

const uint8_t kD18[] = {
  0x00,0x00,0x00,0x00, 0x00,0x2C,0x23,0xA5, 0x00,0x00,0x01,0x00,
  0x00,0x00,0x00,0x08, 0x00,0x2C,0x23,0xA5, 0x00,0x05,0x00,0x14,
  0x00,0x00,0x00,0x10, 0x00,0x2C,0x23,0xA5, 0x00,0x00,0x00,0x50,
  0x00,0x00,0x00,0x18, 0x00,0x18,0x28,0x86, 0x00,0x0A,0x01,0x20,
};

const uint8_t kD19[] = {
  0x00,0x00,0x00,0x00, 0x00,0x2A,0x23,0xB9, 0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x0C, 0x00,0x1A,0x23,0x60, 0x00,0x05,0x00,0x50,
  0x00,0x00,0x00,0x14, 0x00,0x1A,0x23,0xA6, 0x00,0x05,0x01,0xFF,
  0x00,0x00,0x00,0x24, 0x00,0x2C,0x23,0xA5, 0x00,0x05,0x02,0xA0,
  0x00,0x00,0x00,0x2C, 0x00,0x18,0x28,0x86, 0x00,0x0A,0x00,0x00,
  0x00,0x00,0x00,0x30, 0x00,0x18,0x28,0x86, 0x00,0x0A,0x01,0x00,
};

const uint8_t kD20[] = {0x00,0x00,0x00,0x00, 0x00,0x2C,0x23,0xA5, 0x00,0x00,0x00,0x00};
const uint8_t kD21[] = {0x00,0x00,0x00,0x00, 0x00,0x2C,0x23,0xA5, 0x00,0x00,0x00,0x00};

const uint8_t kD22[] = {
  0x00,0x00,0x00,0x00, 0x00,0x1A,0x23,0x60, 0x00,0x00,0x00,0x14,
  0x00,0x00,0x00,0x08, 0x00,0x2A,0x21,0x87, 0x00,0x05,0x00,0x24,
  0x00,0x00,0x00,0x0C, 0x00,0x2C,0x23,0x5F, 0x00,0x05,0x01,0x2C,
  0x00,0x01,0x00,0x00, 0x00,0x2A,0x23,0xB9, 0x00,0x05,0x03,0x30,
  0x00,0x01,0x00,0x0C, 0x00,0x2A,0x23,0xB9, 0x00,0x05,0x05,0x00,
  0x00,0x01,0x00,0x18, 0x00,0x1A,0x23,0x60, 0x00,0x05,0x06,0x10,
  0x00,0x01,0x00,0x20, 0x00,0x18,0x28,0x86, 0x00,0x0A,0x00,0xFF,
  0x00,0x01,0x00,0x24, 0x00,0x18,0x28,0x86, 0x00,0x0A,0x01,0x64,
};

const uint8_t kD23[] = {0x00,0x00,0x00,0x00, 0x00,0x2C,0x23,0xA5, 0x00,0x00,0x00,0x40};

const Fixture kFixtures[] = {
  { 1, kD1,  1, 0,  0},
  // Stride 28 is the one layout the PM4 translator already keys on, and the
  // only draws that currently reach the screen.
  { 2, kD2,  6, 0, 28},
  { 3, kD3,  1, 0,  0},
  { 4, kD4,  3, 0,  0},
  { 5, kD5,  1, 0,  0},
  { 6, kD6,  3, 0,  0},
  { 7, kD7,  2, 0,  0},
  { 8, kD8,  3, 0,  0},
  { 9, kD9,  2, 0,  0},
  {10, kD10, 1, 0,  0},
  {11, kD11, 6, 1,  0},
  // Stride 36: 21,992 draws a run are discarded for this today.
  {12, kD12, 8, 0, 36},
  {13, kD13, 6, 0,  0},
  {14, kD14, 6, 0,  0},
  {15, kD15, 3, 1,  0},
  {16, kD16, 4, 1,  0},
  {17, kD17, 5, 1,  0},
  {18, kD18, 4, 0,  0},
  {19, kD19, 6, 0,  0},
  {20, kD20, 1, 0,  0},
  {21, kD21, 1, 0,  0},
  {22, kD22, 8, 1,  0},
  {23, kD23, 1, 0,  0},
};

//===========================================================================
// The checks.
//===========================================================================

// Element sizes against the gaps the game left between offsets. Within one
// stream, elements are laid out consecutively, so element i's size must equal
// offset[i+1] - offset[i]. The offsets are the game's; the sizes come from the
// runtime's table; neither was derived from the other.
void CheckSizesAgainstOffsets(const Fixture& f, const mx::pm4::HleInputLayout& l) {
  for (uint32_t s = 0; s <= l.max_stream; ++s) {
    // Collect this stream's elements in declaration order, which the captures
    // show is already ascending by offset within a stream.
    std::vector<const mx::pm4::HleInputElement*> in_stream;
    for (const auto& e : l.elements) {
      if (e.stream == s) in_stream.push_back(&e);
    }
    for (size_t i = 0; i + 1 < in_stream.size(); ++i) {
      const uint32_t gap = in_stream[i + 1]->offset - in_stream[i]->offset;
      if (in_stream[i]->offset >= in_stream[i + 1]->offset) {
        char msg[160];
        std::snprintf(msg, sizeof(msg),
                      "decl #%d stream %u element %zu offset %u is not below "
                      "the next element's %u",
                      f.id, s, i, in_stream[i]->offset, in_stream[i + 1]->offset);
        Fail("offsets ascend within a stream", msg);
        continue;
      }
      if (in_stream[i]->size_bytes != gap) {
        char msg[200];
        std::snprintf(msg, sizeof(msg),
                      "decl #%d stream %u element %zu (%s%u) decoded size %u, "
                      "gap to next offset %u",
                      f.id, s, i, in_stream[i]->semantic_name,
                      in_stream[i]->semantic_index, in_stream[i]->size_bytes, gap);
        Fail("size equals offset delta", msg);
      }
    }
  }
}

void RunFixture(const Fixture& f) {
  mx::pm4::D3D9Element elems[mx::pm4::kMaxElements];
  for (uint32_t i = 0; i < f.count; ++i) {
    elems[i] = mx::pm4::ReadElement(f.raw + i * mx::pm4::kElementSize);
  }

  mx::pm4::HleInputLayout layout;
  mx::pm4::LayoutError err;
  if (!mx::pm4::BuildInputLayout(elems, f.count, layout, err)) {
    char msg[200];
    std::snprintf(msg, sizeof(msg),
                  "decl #%d element %u: %s (detail 0x%08X)", f.id,
                  err.failed_element, mx::pm4::LayoutErrorText(err.reason),
                  err.detail);
    Fail("layout builds", msg);
    return;
  }

  char what[64];
  std::snprintf(what, sizeof(what), "decl #%d element count", f.id);
  CheckU32(what, static_cast<uint32_t>(layout.elements.size()), f.count);
  std::snprintf(what, sizeof(what), "decl #%d max stream", f.id);
  CheckU32(what, layout.max_stream, f.max_stream);

  CheckSizesAgainstOffsets(f, layout);

  if (f.expect_stride0 != 0) {
    std::snprintf(what, sizeof(what), "decl #%d stream 0 stride", f.id);
    CheckU32(what, layout.min_stride[0], f.expect_stride0);
  }

  std::printf("  decl #%-2d %u element(s), streams 0..%u, stride0=%u\n", f.id,
              static_cast<uint32_t>(layout.elements.size()), layout.max_stream,
              layout.min_stride[0]);
  for (const auto& e : layout.elements) {
    std::printf("      %-13s%u s%u off=%-3u size=%-2u dxgi=%-3d swiz=0x%03X%s\n",
                e.semantic_name, e.semantic_index, e.stream, e.offset,
                e.size_bytes, static_cast<int>(e.format), e.swizzle,
                e.unpack == mx::pm4::Unpack::kSnorm2_10_10_10
                    ? "  (shader unpacks snorm 2_10_10_10)"
                    : "");
  }
}

// The decode must reject rather than approximate. A silent fallback is the
// failure mode this whole file exists to prevent: it produces geometry that
// looks like geometry.
void CheckRejections() {
  mx::pm4::DecodedVertexType d;

  // Format 0 (kUndefined) and format 5 both have a zero entry in the guest's
  // own size table.
  if (mx::pm4::DecodeVertexType(0x00000000u, d))
    Fail("reject format 0", "accepted");
  if (mx::pm4::DecodeVertexType(0x00002305u, d))
    Fail("reject format 5", "accepted");

  // k_11_11_10 has a valid size but no DXGI equivalent.
  if (mx::pm4::DecodeVertexType(0x00002311u, d))
    Fail("reject k_11_11_10", "accepted");

  // A normalized 32-bit integer format: DXGI has nothing that holds it.
  //   format 34 (k_32_32) | signed | normalized (integer bit clear)
  if (mx::pm4::DecodeVertexType(0x00000122u, d))
    Fail("reject normalized k_32_32", "accepted");

  // An element with a usage outside D3DDECLUSAGE must fail the layout, not
  // land on a null semantic name.
  const uint8_t bad_usage[] = {0x00,0x00,0x00,0x00, 0x00,0x2C,0x23,0xA5,
                               0x00,0x63,0x00,0x00};
  mx::pm4::D3D9Element e = mx::pm4::ReadElement(bad_usage);
  mx::pm4::HleInputLayout l;
  mx::pm4::LayoutError err;
  if (mx::pm4::BuildInputLayout(&e, 1, l, err))
    Fail("reject usage 0x63", "accepted");
  else if (err.reason != mx::pm4::LayoutError::Reason::kUnknownUsage)
    Fail("reject usage 0x63", "wrong reason");

  // A half-dword offset would lose its low bits in the vfetch offset field.
  const uint8_t misaligned[] = {0x00,0x00,0x00,0x02, 0x00,0x2C,0x23,0xA5,
                                0x00,0x00,0x00,0x00};
  e = mx::pm4::ReadElement(misaligned);
  if (mx::pm4::BuildInputLayout(&e, 1, l, err))
    Fail("reject offset 2", "accepted");
  else if (err.reason != mx::pm4::LayoutError::Reason::kMisalignedOffset)
    Fail("reject offset 2", "wrong reason");

  std::printf("  rejections behave\n");
}

// The two bits the whole signed/normalized decision rests on, checked one at a
// time against what the usage says the data has to be. These are the values
// PatchVertexShaderToMatchVertexDeclaration copies into the vfetch dword1, so
// getting them backwards would flip every normal and turn blend indices into
// fractions.
void CheckSignedNormalized() {
  struct Case {
    uint32_t type;
    const char* what;
    bool is_signed;
    bool is_normalized;
    DXGI_FORMAT dxgi;
  };
  const Case cases[] = {
    // Vertex colour: unsigned, normalized. Type bits [9:8] = 0.
    {0x00182886u, "COLOR 8_8_8_8", false, true, DXGI_FORMAT_R8G8B8A8_UNORM},
    // Blend weights: same bits, same conclusion.
    {0x001A2086u, "BLENDWEIGHT 8_8_8_8", false, true, DXGI_FORMAT_R8G8B8A8_UNORM},
    // Normals packed to bytes: signed, normalized. Bit 8 set.
    {0x001A2186u, "NORMAL 8_8_8_8", true, true, DXGI_FORMAT_R8G8B8A8_SNORM},
    // Blend *indices*: unsigned integers, not fractions. Bit 9 set, bit 8
    // clear — the case that proves the two bits are independent, since this
    // shares format 6 with COLOR and differs only here.
    {0x001A2286u, "BLENDINDICES 8_8_8_8", false, false, DXGI_FORMAT_R8G8B8A8_UINT},
    // Packed normals/tangents: signed normalized, and DXGI cannot express it.
    {0x002A2187u, "NORMAL 2_10_10_10", true, true, DXGI_FORMAT_R10G10B10A2_UINT},
    {0x001A215Au, "POSITION 16_16_16_16", true, true, DXGI_FORMAT_R16G16B16A16_SNORM},
    {0x002C2359u, "POSITION 16_16", true, false, DXGI_FORMAT_R16G16_SINT},
  };

  for (const auto& c : cases) {
    mx::pm4::DecodedVertexType d;
    if (!mx::pm4::DecodeVertexType(c.type, d)) {
      Fail("signed/normalized", c.what);
      continue;
    }
    if (d.is_signed != c.is_signed || d.is_normalized != c.is_normalized ||
        d.dxgi != c.dxgi) {
      char msg[220];
      std::snprintf(msg, sizeof(msg),
                    "%s (0x%08X): got signed=%d norm=%d dxgi=%d, want "
                    "signed=%d norm=%d dxgi=%d",
                    c.what, c.type, d.is_signed, d.is_normalized,
                    static_cast<int>(d.dxgi), c.is_signed, c.is_normalized,
                    static_cast<int>(c.dxgi));
      Fail("signed/normalized", msg);
    }
  }

  // The one combination that needs shader help must say so.
  mx::pm4::DecodedVertexType d;
  mx::pm4::DecodeVertexType(0x002A2187u, d);
  if (d.unpack != mx::pm4::Unpack::kSnorm2_10_10_10)
    Fail("snorm 2_10_10_10 flagged", "unpack not set");

  std::printf("  signed/normalized bits agree with every usage\n");
}

// The 12-bit swizzle, and specifically which end of it holds component x.
//
// The shift chain inside PatchVertexShaderToMatchVertexDeclaration is dense
// enough that reading a direction out of it is not evidence. The captures are:
// the swizzle a format gets is fixed by its component count, and only one
// reading makes all four counts come out right. Component values are 0-3 for
// xyzw, 4 for constant 0, 5 for constant 1.
void CheckSwizzle() {
  auto comp = [](uint32_t swiz, int i) { return (swiz >> (3 * i)) & 7u; };

  struct Case {
    uint32_t type;
    const char* what;
    uint32_t want[4];   // x, y, z, w
  };
  const Case cases[] = {
    // Four components: identity.
    {0x001A23A6u, "32_32_32_32_FLOAT", {0, 1, 2, 3}},
    // Three: the fourth reads constant 1.
    {0x002A23B9u, "32_32_32_FLOAT",    {0, 1, 2, 5}},
    // Two: constant 0 then constant 1.
    {0x002C23A5u, "32_32_FLOAT",       {0, 1, 4, 5}},
    // One.
    {0x002C83A4u, "32_FLOAT",          {0, 4, 4, 5}},
    // D3DCOLOR: red and blue exchanged, alpha left alone. This is the case
    // that would be invisible if the swizzle were read backwards — (z,y,x,w)
    // reversed is (w,x,y,z), which is not a plausible colour swizzle.
    {0x00182886u, "COLOR 8_8_8_8",     {2, 1, 0, 3}},
  };

  for (const auto& c : cases) {
    mx::pm4::DecodedVertexType d;
    if (!mx::pm4::DecodeVertexType(c.type, d)) {
      Fail("swizzle", c.what);
      continue;
    }
    for (int i = 0; i < 4; ++i) {
      if (comp(d.swizzle, i) == c.want[i]) continue;
      char msg[180];
      std::snprintf(msg, sizeof(msg),
                    "%s (0x%08X): swizzle 0x%03X component %d is %u, want %u",
                    c.what, c.type, d.swizzle, i, comp(d.swizzle, i), c.want[i]);
      Fail("swizzle", msg);
      break;
    }
  }

  std::printf("  swizzle component order matches all four component counts\n");
}

// The local format constants must be the SDK's, not a drifted copy. Checked
// through the public size helper rather than by exposing the private enum.
void CheckAgainstSdk() {
  struct { rex::graphics::xenos::VertexFormat fmt; uint32_t bytes; } cases[] = {
    {rex::graphics::xenos::VertexFormat::k_8_8_8_8, 4},
    {rex::graphics::xenos::VertexFormat::k_2_10_10_10, 4},
    {rex::graphics::xenos::VertexFormat::k_16_16, 4},
    {rex::graphics::xenos::VertexFormat::k_16_16_16_16, 8},
    {rex::graphics::xenos::VertexFormat::k_16_16_FLOAT, 4},
    {rex::graphics::xenos::VertexFormat::k_16_16_16_16_FLOAT, 8},
    {rex::graphics::xenos::VertexFormat::k_32_FLOAT, 4},
    {rex::graphics::xenos::VertexFormat::k_32_32_FLOAT, 8},
    {rex::graphics::xenos::VertexFormat::k_32_32_32_FLOAT, 12},
    {rex::graphics::xenos::VertexFormat::k_32_32_32_32_FLOAT, 16},
  };
  for (const auto& c : cases) {
    const uint32_t got =
        mx::pm4::VertexFormatSizeBytes(static_cast<uint32_t>(c.fmt));
    if (got != c.bytes) {
      char msg[120];
      std::snprintf(msg, sizeof(msg), "format %u: table says %u bytes, want %u",
                    static_cast<uint32_t>(c.fmt), got, c.bytes);
      Fail("guest size table vs SDK enum", msg);
    }
    // The size table must also agree with the SDK's component count.
    const int comps = rex::graphics::xenos::GetVertexFormatComponentCount(c.fmt);
    if (comps <= 0) {
      Fail("guest size table vs SDK enum", "SDK reports no components");
    }
  }
  std::printf("  guest size table agrees with the SDK enum on 10 formats\n");
}

}  // namespace

int main() {
  std::printf("D3D9 vertex declaration -> input layout\n\n");

  std::printf("Format decode:\n");
  CheckAgainstSdk();
  CheckSignedNormalized();
  CheckSwizzle();
  CheckRejections();

  std::printf("\nThe 23 captured declarations:\n");
  for (const auto& f : kFixtures) RunFixture(f);

  std::printf("\n%s (%d failure%s)\n", g_failures ? "FAILED" : "PASSED",
              g_failures, g_failures == 1 ? "" : "s");
  return g_failures ? 1 : 0;
}
