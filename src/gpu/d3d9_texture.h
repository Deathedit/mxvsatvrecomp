#pragma once

#include <cstddef>
#include <cstdint>

#include "gpu/hle_types.h"

namespace mx::hle {

struct HleTextureSource {
  // The raw 6-bit format field, already folded through GetBaseFormat. Set
  // before the accept-list switch runs, so it is valid even when
  // DescribeHleTexture2D returns false — that is the whole point of it, since
  // "unsupported texture format" is useless without naming the format.
  uint32_t guest_format = 0;
  uint32_t address = 0;
  uint32_t source_bytes = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t pitch_blocks = 0;
  uint32_t block_width = 0;
  uint32_t block_height = 0;
  uint32_t bytes_per_block = 0;
  uint32_t endian = 0;
  uint32_t swizzle = 0;
  uint32_t clamp_x = 0;
  uint32_t clamp_y = 0;
  // TEX_FORMAT_COMP / GPUSIGN, one xenos::TextureSign per component packed two
  // bits each in XYZW order -- kUnsigned(0), kSigned(1), kUnsignedBiased(2,
  // meaning 2*c-1), kGamma(3, sRGB linearized when sampled). Decoded so the
  // census can say whether this game uses anything but kUnsigned; nothing acts
  // on it yet.
  uint8_t signs = 0;
  // 1 for a plain 2D or 1D texture; 6 for a cube; stack_depth+1 for a stacked
  // 2D. Each slice is a whole 2D image of the width/height above, and
  // slice_stride_bytes is the distance between their bases in guest memory
  // (0 when array_size is 1). Both come from GetGuestTextureLayout.
  uint32_t array_size = 1;
  uint32_t slice_stride_bytes = 0;
  bool tiled = false;
  bool linear_filter = true;
  HostTextureFormat host_format = HostTextureFormat::kRgba8;
};

// Name of a guest texture format, transcribed from the game's own 64-entry
// table. Never null; unknown indices return "FMT_?".
const char* GuestTextureFormatName(uint32_t guest_format);

// The fetch constant's four TextureSign values, permuted into HOST component
// order so a shader can apply them to an already-swizzled sample.
//
// This has to be permuted because the swizzle is applied by the SRV's component
// mapping, not in the shader: by the time the shader sees a texel, component c
// holds guest component swizzle[c], so it needs guest component swizzle[c]'s
// sign. A swizzle entry of 4 or 5 forces a literal 0 or 1, which carries no
// guest component and is therefore unsigned. Mirrors the reference's
// SwizzleSigns (pipeline/texture/util.h:309).
//
// Returns two bits per host component in XYZW order, same packing as
// HleTextureSource::signs.
uint8_t SwizzleTextureSigns(uint8_t signs, uint32_t swizzle);

bool DescribeHleTexture2D(const uint32_t fetch_words[6],
                          HleTextureSource& out, const char** fail = nullptr);
bool DecodeHleTexture2D(const HleTextureSource& source,
                        const uint8_t* guest, size_t guest_bytes,
                        HleTexturePayload& out, const char** fail = nullptr);
uint64_t HleTextureKey(const uint32_t fetch_words[6]);
// Returns true when the decoded base mip contains more than an empty/cleared
// backing store. This is intentionally format-agnostic: it is a guard against
// publishing guest render-target storage that is all zero because the original
// GPU dispatch is skipped, not an attempt to classify texture semantics.
bool HleTextureHasNonzeroData(const HleTexturePayload& texture,
                              size_t* nonzero_bytes = nullptr);

}  // namespace mx::hle
