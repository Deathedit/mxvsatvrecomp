#pragma once

// Turns a guest D3DVERTEXELEMENT9 array into a host input layout.
//
// This is the first piece of the D3D9 -> D3D12 high-level path. Every colour
// and stride round so far has inferred the vertex layout from PM4 plus shader
// microcode, because a Xenos vfetch carries format and offset but not semantic.
// The semantics live one layer up, in the arrays the game hands to
// D3DDevice_CreateVertexDeclaration — 23 of them captured in
// d3d9_dump_decls.txt.
//
// **The Type dword's bit layout is not inferred here.** It was read out of
// D3D::PatchVertexShaderToMatchVertexDeclaration (0x82564C50), which is the
// function that consumes it: it takes each element's Type and writes the
// matching fields of the shader's vfetch instruction. Its own arithmetic gives
// the field positions exactly (see kTypeFormat* below), and the size table it
// indexes is lifted verbatim from the XEX at 0x8204E188.
//
// Header stays free of the SDK and of d3d12.h — like shader_ucode.h — so the
// fixture test can include it directly. Only <dxgiformat.h> comes in, which is
// a dependency-free enum header.

#include <cstdint>
#include <vector>

#include <dxgiformat.h>

namespace mx::pm4 {

//===========================================================================
// The guest element, as D3D9 on Xenon lays it out.
//
// **12 bytes, not the PC struct's 8.** Both D3DDevice_CreateVertexDeclaration
// (0x82550B80) and XGSetVertexDeclaration (0x82550A90) walk the array with
// `lhzu r9, 0xC`, and PatchVertexShaderToMatchVertexDeclaration advances its
// element cursor by 6 halfwords. Byte 11 is padding the runtime never reads —
// the captures show it holding leftover garbage (FF, 7C, 78, 3B, 60).
//===========================================================================
struct D3D9Element {
  uint16_t stream      = 0;
  uint16_t offset      = 0;   // bytes into the vertex; always a multiple of 4
  uint32_t type        = 0;   // packed; see DecodeVertexType
  uint8_t  method      = 0;   // D3DDECLMETHOD; 0 (DEFAULT) in every capture
  uint8_t  usage       = 0;   // D3DDECLUSAGE
  uint8_t  usage_index = 0;
};

constexpr uint32_t kElementSize   = 12;
constexpr uint16_t kStreamEnd     = 0xFF;  // D3DDECL_END's Stream field
constexpr uint32_t kMaxElements   = 32;

// Reads one element out of 12 raw big-endian guest bytes. Separate from the
// hooks so the test can build elements without a guest.
D3D9Element ReadElement(const uint8_t raw[kElementSize]);

//===========================================================================
// D3DDECLUSAGE, recovered from D3DX9's declaration-string parser
// (sub_8257A1B0) two rounds before the library match, and unchanged by it.
//===========================================================================
enum : uint8_t {
  kUsagePosition      = 0,
  kUsageBlendWeight   = 1,
  kUsageBlendIndices  = 2,
  kUsageNormal        = 3,
  kUsagePSize         = 4,
  kUsageTexcoord      = 5,
  kUsageTangent       = 6,
  kUsageBinormal      = 7,
  kUsageTessFactor    = 8,
  kUsagePositionT     = 9,
  kUsageColor         = 10,
  kUsageFog           = 11,
  kUsageDepth         = 12,
  kUsageSample        = 13,
};

// nullptr for a usage outside the table above.
const char* UsageSemanticName(uint8_t usage);

//===========================================================================
// The Type dword.
//
// PatchVertexShaderToMatchVertexDeclaration builds the vfetch words like this,
// with `t` the Type dword (decompilation at 0x82564C50, simplified):
//
//   dword1 = (((t << 12) & 0x3F000) | (t & 0x300)) << 4 | dword1 & 0xBFC0CFFF;
//
// The preserved mask clears exactly bits [21:16], [13:12] and [30] of the
// vfetch dword1, which by ucode.h are `format`, `fomat_comp_all`,
// `num_format_all` and `is_mini_fetch`. Equating the two sides:
//
//   t[5:0]   -> format            (xenos::VertexFormat)
//   t[8]     -> fomat_comp_all    (1 = signed)
//   t[9]     -> num_format_all    (0 = normalized, 1 = integer)
//   t[21:10] -> the vfetch destination swizzle, four 3-bit components,
//               **x in [12:10] and w in [21:19]**
//
// The swizzle's component order is not taken on trust from that arithmetic —
// it is confirmed by what the 23 captures decode to, which is only consistent
// one way round: 4-component formats give the identity 0x688 = (x,y,z,w),
// 3-component ones give 0xA88 = (x,y,z,1), 2-component 0xB08 = (x,y,0,1),
// 1-component 0xB20 = (x,0,0,1), and every 8_8_8_8 COLOR gives 0x60A =
// (z,y,x,w), which is D3DCOLOR's BGRA arriving as RGBA. Component values are
// 0-3 for xyzw, 4 for constant 0 and 5 for constant 1.
//
// The swizzle is carried through but deliberately not applied to the host
// format: on this path it belongs in the translated shader, exactly where D3D9
// itself puts it. A DXGI format that silently reordered components would be
// applying it twice.
//===========================================================================
constexpr uint32_t kTypeFormatMask     = 0x0000003Fu;
constexpr uint32_t kTypeSignedBit      = 1u << 8;
constexpr uint32_t kTypeIntegerBit     = 1u << 9;
constexpr uint32_t kTypeSwizzleShift   = 10;
constexpr uint32_t kTypeSwizzleMask    = 0x00000FFFu;

// Some Xenon formats have no DXGI equivalent that carries the same values. For
// those the host format passes the raw bits through unchanged and the shader
// has to finish the conversion. Naming that here is the point: the alternative
// is a near-miss DXGI format that produces plausible, wrong geometry.
enum class Unpack : uint8_t {
  kNone = 0,
  kSnorm2_10_10_10,   // R10G10B10A2_UINT bits -> signed normalized floats
};

struct DecodedVertexType {
  uint32_t format        = 0;      // xenos::VertexFormat
  uint32_t size_bytes    = 0;      // from the guest's own table at 0x8204E188
  bool     is_signed     = false;
  bool     is_normalized = false;
  uint32_t swizzle       = 0;      // 12 bits, x in [2:0] .. w in [11:9]
  DXGI_FORMAT dxgi       = DXGI_FORMAT_UNKNOWN;
  Unpack   unpack        = Unpack::kNone;
};

// False for any format or signed/normalized combination not established. There
// is deliberately no fallback: an unknown type must surface as a failure with
// its dword reported, not as a guess.
bool DecodeVertexType(uint32_t type, DecodedVertexType& out);

// Size in bytes of a xenos::VertexFormat, from the runtime's own table (dwords
// there, bytes here). 0 for every format the guest marks unusable.
uint32_t VertexFormatSizeBytes(uint32_t format);

//===========================================================================
// The host layout.
//
// Not D3D12_INPUT_ELEMENT_DESC, so this stays out of d3d12.h — the gfx layer
// converts. `stream` is carried rather than flattened: 5 of the 23 captured
// declarations use two streams, and collapsing them to stream 0 would read the
// second stream's offsets against the first stream's buffer.
//===========================================================================
struct HleInputElement {
  const char* semantic_name  = nullptr;
  uint32_t    semantic_index = 0;
  DXGI_FORMAT format         = DXGI_FORMAT_UNKNOWN;
  uint32_t    stream         = 0;
  uint32_t    offset         = 0;   // bytes, within that stream's vertex
  uint32_t    size_bytes     = 0;
  Unpack      unpack         = Unpack::kNone;
  uint32_t    swizzle        = 0;
  uint8_t     usage          = 0;
};

struct HleInputLayout {
  std::vector<HleInputElement> elements;
  uint32_t max_stream = 0;
  // Per stream, the highest (offset + size) seen. This is the layout's own
  // minimum stride; SetStreamSource supplies the real one, and the two
  // disagreeing means something is wrong.
  uint32_t min_stride[4] = {};
};

// Failure reasons, so a rejection can be reported precisely rather than as a
// count. `failed_element` indexes the offending element when it applies.
struct LayoutError {
  enum class Reason : uint8_t {
    kNone = 0,
    kUnknownType,        // DecodeVertexType said no
    kUnknownUsage,       // usage outside D3DDECLUSAGE
    kUnsupportedMethod,  // D3DDECLMETHOD other than DEFAULT
    kStreamOutOfRange,   // stream >= 4
    kMisalignedOffset,   // offset not a multiple of 4
    kEmpty,              // zero elements
  } reason = Reason::kNone;
  uint32_t failed_element = 0;
  uint32_t detail         = 0;   // the Type dword, usage, or offset
};

const char* LayoutErrorText(LayoutError::Reason r);

// False on the first element it cannot describe, with `err` filled in.
bool BuildInputLayout(const D3D9Element* elements, uint32_t count,
                      HleInputLayout& out, LayoutError& err);

}  // namespace mx::pm4
