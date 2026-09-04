#include "gpu/d3d9_layout.h"

// For ReadVertexAttributeAs — the format decode has one implementation, and it
// is the one the PM4 path has been using all along.
#include "gpu/shader_ucode.h"

namespace mx::hle {
namespace {

//---------------------------------------------------------------------------
// The guest's own size table, lifted verbatim from the XEX at 0x8204E188.
//
// PatchVertexShaderToMatchVertexDeclaration indexes it by the vfetch format
// field -- `byte_8204E188[*(_WORD *)(instr + 4) & 0x3F]`, where that halfword is
// the big-endian top of dword1 and the mask picks out bits [21:16] -- and uses
// the result to compare attribute extents. **The entries are dwords**, so the
// byte size is 4x. Every entry not listed below is zero in the XEX, which is the
// runtime saying that format is not a legal vertex format.
//
// This replaces a size derivation done by hand, which happened to agree on all
// ten formats the captures contain; this is the runtime's answer for all 64.
//---------------------------------------------------------------------------
constexpr uint8_t kFormatSizeDwords[64] = {
    0, 0, 0, 0, 0, 0, 1, 1,   //  0..7   ( 6 k_8_8_8_8, 7 k_2_10_10_10)
    0, 0, 0, 0, 0, 0, 0, 0,   //  8..15
    1, 1, 0, 0, 0, 0, 0, 0,   // 16..23  (16 k_10_11_11, 17 k_11_11_10)
    0, 1, 2, 0, 0, 0, 0, 1,   // 24..31  (25 k_16_16, 26 k_16_16_16_16,
                              //          31 k_16_16_FLOAT)
    2, 1, 2, 4, 1, 2, 4, 0,   // 32..39  (32 k_16_16_16_16_FLOAT, 33 k_32,
                              //          34 k_32_32, 35 k_32_32_32_32,
                              //          36 k_32_FLOAT, 37 k_32_32_FLOAT,
                              //          38 k_32_32_32_32_FLOAT)
    0, 0, 0, 0, 0, 0, 0, 0,   // 40..47
    0, 0, 0, 0, 0, 0, 0, 0,   // 48..55
    0, 3, 0, 0, 0, 0, 0, 0,   // 56..63  (57 k_32_32_32_FLOAT)
};

// xenos::VertexFormat, spelled out locally so this header stays SDK-free (the
// same arrangement shader_ucode.h uses). The values are checked against the
// SDK enum by a static_assert in the test.
enum : uint32_t {
  k_8_8_8_8           = 6,
  k_2_10_10_10        = 7,
  k_10_11_11          = 16,
  k_11_11_10          = 17,
  k_16_16             = 25,
  k_16_16_16_16       = 26,
  k_16_16_FLOAT       = 31,
  k_16_16_16_16_FLOAT = 32,
  k_32                = 33,
  k_32_32             = 34,
  k_32_32_32_32       = 35,
  k_32_FLOAT          = 36,
  k_32_32_FLOAT       = 37,
  k_32_32_32_32_FLOAT = 38,
  k_32_32_32_FLOAT    = 57,
};

// Picks the host format for an integer Xenon format from the two bits the
// runtime itself keys on. Returns UNKNOWN where DXGI has no format that holds
// the same values -- the caller turns that into a reported failure, never a
// substitution.
DXGI_FORMAT IntegerFormat(uint32_t format, bool is_signed, bool is_normalized) {
  switch (format) {
    case k_8_8_8_8:
      if (is_normalized)
        return is_signed ? DXGI_FORMAT_R8G8B8A8_SNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
      return is_signed ? DXGI_FORMAT_R8G8B8A8_SINT : DXGI_FORMAT_R8G8B8A8_UINT;

    case k_16_16:
      if (is_normalized)
        return is_signed ? DXGI_FORMAT_R16G16_SNORM : DXGI_FORMAT_R16G16_UNORM;
      return is_signed ? DXGI_FORMAT_R16G16_SINT : DXGI_FORMAT_R16G16_UINT;

    case k_16_16_16_16:
      if (is_normalized)
        return is_signed ? DXGI_FORMAT_R16G16B16A16_SNORM
                         : DXGI_FORMAT_R16G16B16A16_UNORM;
      return is_signed ? DXGI_FORMAT_R16G16B16A16_SINT
                       : DXGI_FORMAT_R16G16B16A16_UINT;

    // DXGI has no normalized 32-bit integer format. Nothing in the captures
    // asks for one; if something does, it must be reported, not rounded off.
    case k_32:
      if (is_normalized) return DXGI_FORMAT_UNKNOWN;
      return is_signed ? DXGI_FORMAT_R32_SINT : DXGI_FORMAT_R32_UINT;
    case k_32_32:
      if (is_normalized) return DXGI_FORMAT_UNKNOWN;
      return is_signed ? DXGI_FORMAT_R32G32_SINT : DXGI_FORMAT_R32G32_UINT;
    case k_32_32_32_32:
      if (is_normalized) return DXGI_FORMAT_UNKNOWN;
      return is_signed ? DXGI_FORMAT_R32G32B32A32_SINT
                       : DXGI_FORMAT_R32G32B32A32_UINT;

    default:
      return DXGI_FORMAT_UNKNOWN;
  }
}

}  // namespace

D3D9Element ReadElement(const uint8_t raw[kElementSize]) {
  // Guest bytes are big-endian; the field boundaries are the ones both
  // CreateVertexDeclaration and PatchVertexShaderToMatchVertexDeclaration use
  // (usage at +9 and usage index at +10 appear literally in the latter's
  // element-matching loop).
  D3D9Element e;
  e.stream = static_cast<uint16_t>((raw[0] << 8) | raw[1]);
  e.offset = static_cast<uint16_t>((raw[2] << 8) | raw[3]);
  e.type = (static_cast<uint32_t>(raw[4]) << 24) |
           (static_cast<uint32_t>(raw[5]) << 16) |
           (static_cast<uint32_t>(raw[6]) << 8) | raw[7];
  e.method = raw[8];
  e.usage = raw[9];
  e.usage_index = raw[10];
  // raw[11] is padding the runtime never reads.
  return e;
}

const char* UsageSemanticName(uint8_t usage) {
  switch (usage) {
    case kUsagePosition:     return "POSITION";
    case kUsageBlendWeight:  return "BLENDWEIGHT";
    case kUsageBlendIndices: return "BLENDINDICES";
    case kUsageNormal:       return "NORMAL";
    case kUsagePSize:        return "PSIZE";
    case kUsageTexcoord:     return "TEXCOORD";
    case kUsageTangent:      return "TANGENT";
    case kUsageBinormal:     return "BINORMAL";
    case kUsageTessFactor:   return "TESSFACTOR";
    case kUsagePositionT:    return "POSITIONT";
    case kUsageColor:        return "COLOR";
    case kUsageFog:          return "FOG";
    case kUsageDepth:        return "DEPTH";
    case kUsageSample:       return "SAMPLE";
    default:                 return nullptr;
  }
}

uint32_t VertexFormatSizeBytes(uint32_t format) {
  if (format >= 64) return 0;
  return kFormatSizeDwords[format] * 4u;
}

bool DecodeVertexType(uint32_t type, DecodedVertexType& out) {
  out = {};
  out.format = type & kTypeFormatMask;
  out.is_signed = (type & kTypeSignedBit) != 0;
  // num_format_all is 0 for normalized — the inverted sense is the runtime's,
  // via VertexFetchInstruction::is_normalized() in ucode.h.
  out.is_normalized = (type & kTypeIntegerBit) == 0;
  out.swizzle = (type >> kTypeSwizzleShift) & kTypeSwizzleMask;
  out.size_bytes = VertexFormatSizeBytes(out.format);
  if (out.size_bytes == 0) return false;   // the guest's table says unusable

  switch (out.format) {
    // Float formats: the signed and normalized bits carry no meaning, and the
    // captures set them inconsistently across float elements, which is why
    // they are not consulted here.
    case k_16_16_FLOAT:       out.dxgi = DXGI_FORMAT_R16G16_FLOAT; break;
    case k_16_16_16_16_FLOAT: out.dxgi = DXGI_FORMAT_R16G16B16A16_FLOAT; break;
    case k_32_FLOAT:          out.dxgi = DXGI_FORMAT_R32_FLOAT; break;
    case k_32_32_FLOAT:       out.dxgi = DXGI_FORMAT_R32G32_FLOAT; break;
    case k_32_32_32_FLOAT:    out.dxgi = DXGI_FORMAT_R32G32B32_FLOAT; break;
    case k_32_32_32_32_FLOAT: out.dxgi = DXGI_FORMAT_R32G32B32A32_FLOAT; break;

    case k_2_10_10_10:
      // The unsigned normalized case maps exactly. The signed normalized one --
      // which is what this title uses for NORMAL and TANGENT -- has no DXGI
      // equivalent, so the raw bits are passed through as UINT and the shader is
      // told to finish the job. R10G10B10A2_SNORM does not exist; picking UNORM
      // instead would halve and bias every normal, and the result would still
      // look like geometry.
      if (!out.is_normalized) {
        out.dxgi = DXGI_FORMAT_R10G10B10A2_UINT;
      } else if (!out.is_signed) {
        out.dxgi = DXGI_FORMAT_R10G10B10A2_UNORM;
      } else {
        out.dxgi = DXGI_FORMAT_R10G10B10A2_UINT;
        out.unpack = Unpack::kSnorm2_10_10_10;
      }
      break;

    // 3-component packed formats. Neither appears in the 23 captures and
    // neither has a DXGI equivalent, so they are refused rather than
    // approximated; if one shows up the report will name it.
    case k_10_11_11:
    case k_11_11_10:
      return false;

    default:
      out.dxgi = IntegerFormat(out.format, out.is_signed, out.is_normalized);
      break;
  }

  return out.dxgi != DXGI_FORMAT_UNKNOWN;
}

const char* LayoutErrorText(LayoutError::Reason r) {
  switch (r) {
    case LayoutError::Reason::kNone:              return "ok";
    case LayoutError::Reason::kUnknownType:       return "unknown Type dword";
    case LayoutError::Reason::kUnknownUsage:      return "usage outside D3DDECLUSAGE";
    case LayoutError::Reason::kUnsupportedMethod: return "D3DDECLMETHOD not DEFAULT";
    case LayoutError::Reason::kStreamOutOfRange:  return "stream >= 4";
    case LayoutError::Reason::kMisalignedOffset:  return "offset not dword-aligned";
    case LayoutError::Reason::kEmpty:             return "no elements";
  }
  return "?";
}

bool BuildInputLayout(const D3D9Element* elements, uint32_t count,
                      HleInputLayout& out, LayoutError& err) {
  out = {};
  err = {};

  if (!elements || count == 0) {
    err.reason = LayoutError::Reason::kEmpty;
    return false;
  }

  out.elements.reserve(count);
  err.offered = count;

  for (uint32_t i = 0; i < count; ++i) {
    const D3D9Element& e = elements[i];

    // Whichever check below fails first, with the value that failed it. Nothing
    // returns from inside the checks any more: the decision of what a failure
    // COSTS is made once, after them, and depends on which element this is.
    LayoutError::Reason bad = LayoutError::Reason::kNone;
    uint32_t detail = 0;
    DecodedVertexType decoded;
    const char* semantic = nullptr;

    // Four streams because SetStreamSource takes 0..3 on this hardware and the
    // captures reach stream 1 at most. A higher index means the element was
    // misread, not that the game used stream 9.
    if (e.stream >= 4) {
      bad = LayoutError::Reason::kStreamOutOfRange;
      detail = e.stream;
    // The runtime stores offset >> 2 into the vfetch dword offset field, so a
    // non-multiple of 4 would silently lose its low bits there.
    } else if ((e.offset & 3) != 0) {
      bad = LayoutError::Reason::kMisalignedOffset;
      detail = e.offset;
    } else if (e.method != 0) {
      bad = LayoutError::Reason::kUnsupportedMethod;
      detail = e.method;
    } else if ((semantic = UsageSemanticName(e.usage)) == nullptr) {
      bad = LayoutError::Reason::kUnknownUsage;
      detail = e.usage;
    } else if (!DecodeVertexType(e.type, decoded)) {
      bad = LayoutError::Reason::kUnknownType;
      detail = e.type;
    }

    if (bad != LayoutError::Reason::kNone) {
      // POSITION0 is the one element the transcode cannot do without, so it is
      // still fatal -- and it is identified by the usage byte, which is
      // readable whether or not the rest of the element decoded.
      const bool is_position =
          e.usage == kUsagePosition && e.usage_index == 0;
      if (is_position) {
        err.reason = bad;
        err.failed_element = i;
        err.detail = detail;
        return false;
      }

      if (err.skipped++ == 0) {
        err.skip_reason = bad;
        err.skip_element = i;
        err.skip_detail = detail;
      }

      // The element is dropped from the layout but NOT from the stride. It still
      // occupies its bytes in the guest's vertex, and min_stride is what the
      // bound stride is checked against -- shrinking it here would turn a real
      // short-stride binding into a silent pass. VertexFormatSizeBytes reads the
      // guest's own table and is valid even where the DXGI mapping is not.
      if (e.stream < 4) {
        if (e.stream > out.max_stream) out.max_stream = e.stream;
        const uint32_t skipped_end =
            e.offset + VertexFormatSizeBytes(e.type & kTypeFormatMask);
        if (skipped_end > out.min_stride[e.stream])
          out.min_stride[e.stream] = skipped_end;
      }
      continue;
    }

    HleInputElement h;
    h.semantic_name = semantic;
    h.semantic_index = e.usage_index;
    h.format = decoded.dxgi;
    h.stream = e.stream;
    h.offset = e.offset;
    h.size_bytes = decoded.size_bytes;
    h.unpack = decoded.unpack;
    h.swizzle = decoded.swizzle;
    h.usage = e.usage;
    h.xenos_format = decoded.format;
    h.is_signed = decoded.is_signed;
    h.is_normalized = decoded.is_normalized;
    out.elements.push_back(h);

    if (e.stream > out.max_stream) out.max_stream = e.stream;
    const uint32_t end = e.offset + decoded.size_bytes;
    if (end > out.min_stride[e.stream]) out.min_stride[e.stream] = end;
  }

  // Every element was dropped. There is no layout to hand back, and the reason to
  // report is the first one that fired rather than a bare "empty" -- which is
  // also what keeps a single-element declaration failing with the reason its one
  // element failed for.
  if (out.elements.empty()) {
    err.reason = err.skipped ? err.skip_reason : LayoutError::Reason::kEmpty;
    err.failed_element = err.skipped ? err.skip_element : 0;
    err.detail = err.skip_detail;
    return false;
  }

  err.failed_element = 0;
  return true;
}

const HleInputElement* FindUsage(const HleInputLayout& layout, uint8_t usage,
                                 uint32_t usage_index) {
  for (const auto& e : layout.elements) {
    if (e.usage == usage && e.semantic_index == usage_index) return &e;
  }
  return nullptr;
}

bool ReadHleElement(const uint8_t* vertex_base, uint32_t vertex_bytes,
                    const HleInputElement& element, uint32_t endian,
                    float out[4]) {
  // The two Type bits pick the interpretation. This is the whole reason the D3D9
  // route exists: COLOR and BLENDINDICES are both k_8_8_8_8 and differ only here,
  // and a decode that ignored them would turn every bone index into a fraction
  // while still producing something that looks like geometry.
  NumFormat num;
  if (element.is_normalized) {
    num = element.is_signed ? NumFormat::kSnorm : NumFormat::kUnorm;
  } else {
    num = element.is_signed ? NumFormat::kSint : NumFormat::kUint;
  }

  float raw[4];
  if (!ReadVertexAttributeAs(vertex_base, vertex_bytes, element.xenos_format,
                             element.offset, element.size_bytes, num, endian,
                             raw)) {
    return false;
  }

  // Apply the swizzle. On the host-layout path it is left for the shader,
  // because that is where D3D9 puts it and a DXGI format that reordered
  // components would apply it twice -- but a CPU read has no shader downstream.
  // Values are 0-3 for xyzw, 4 for constant 0 and 5 for constant 1.
  for (int i = 0; i < 4; ++i) {
    const uint32_t sel = (element.swizzle >> (i * 3)) & 0x7;
    switch (sel) {
      case 0: case 1: case 2: case 3: out[i] = raw[sel]; break;
      case 4: out[i] = 0.0f; break;
      case 5: out[i] = 1.0f; break;
      default:
        // 6 and 7 are not swizzle values any capture produces. Failing is the
        // point: a silent 0 here would be indistinguishable from a real one.
        return false;
    }
  }
  return true;
}

}  // namespace mx::hle
