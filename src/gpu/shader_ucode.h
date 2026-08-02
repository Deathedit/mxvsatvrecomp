#pragma once

// Decodes Xenos vertex shader microcode far enough to recover the vertex
// layout: which fetch slot each attribute comes from, at what offset, in what
// format, and — the reason this exists — the real stride.
//
// Until now the stride was inferred by dividing the vertex buffer size by the
// vertex count and accepting exact divisions in 8..64, because the vertex fetch
// constant does not carry it. That guess is why only stride-28 draws are drawn
// and ~230 draws per frame are skipped. The shader has always carried the
// answer; the microcode arrives inline in the PM4 ring as IM_LOAD_IMMEDIATE
// (0x2B), 68 per frame.
//
// This header deliberately includes no SDK header — it exposes plain data, so a
// test can include it without the SDK on its path. The bit layouts live in
// shader_ucode.cpp, which uses the SDK's own structs rather than hand-rolled
// shifts; their static_assert_size means a packing disagreement fails the build
// loudly instead of yielding silently wrong strides.

#include <cstdint>
#include <vector>

namespace mx::pm4 {

// One attribute a vertex shader fetches. Raw values throughout; `format` is the
// 6-bit xenos::VertexFormat value rather than the enum so this header stays
// SDK-free.
struct VertexAttribute {
  uint32_t fetch_slot    = 0;   // 0..95, from vfetch fetch_constant_index()
  uint32_t offset_bytes  = 0;   // vfetch offset() * 4
  uint32_t stride_bytes  = 0;   // vfetch stride() * 4, from the owning full fetch
  uint32_t format        = 0;   // xenos::VertexFormat, e.g. 57 = k_32_32_32_FLOAT
  uint32_t components    = 0;   // 0 if the format is not one we know
  uint32_t size_bytes    = 0;   // 0 if the format is not one we know
  uint32_t dest_reg      = 0;
  uint32_t dest_swizzle  = 0;
  bool     is_signed     = false;
  bool     is_normalized = false;
  int      exp_adjust    = 0;
  bool     from_mini     = false;  // inherited slot/stride from a preceding full
};

// Walk the control flow of a vertex shader blob and enumerate every vertex
// fetch, in program order. Appends to `out` (does not clear it).
//
// Returns false and sets *fail to a static reason string on a malformed blob.
// Never throws, never loops unbounded, never reads outside [dwords, +count).
// `fail` may be null.
bool DecodeVertexShaderFetches(const uint32_t* dwords, uint32_t dword_count,
                               std::vector<VertexAttribute>& out,
                               const char** fail);

// Byte size of a xenos::VertexFormat, or 0 if unrecognised. Exposed for the
// same reason it is hand-written rather than taken from the SDK: the SDK's
// GetVertexFormatComponentCount asserts on an unhandled case, and a 6-bit field
// out of a malformed blob can hold any of 64 values.
uint32_t VertexFormatSizeBytes(uint32_t format, uint32_t* out_components);

}  // namespace mx::pm4
