#pragma once

#include <cstddef>
#include <cstdint>

#include "gpu/hle_types.h"

namespace mx::hle {

// The most levels a Xenos texture can have: xenos::kTextureMaxMips, which is
// log2(8192) + 1. Restated here so this header does not drag the SDK in.
constexpr uint32_t kMaxTextureLevels = 14;

// One mip level's geometry, resolved once in DescribeHleTexture2D so nothing
// downstream has to re-derive it. A level is NOT "the base with everything
// halved": its pitch comes from a different rule, and beyond the packed level
// several levels share one image. See the field notes below and the block
// comment on HleTextureSource::levels.
struct HleTextureLevel {
  // Where this level's storage starts in the blob CopyTexturePhysical builds:
  // the base allocation, then the mip allocation appended after it. Level 0 is
  // always 0; levels 1.. are source_bytes + the SDK's mip_offsets_bytes.
  uint32_t offset_bytes = 0;
  // This level's own row pitch in blocks. NOT HleTextureSource::pitch_blocks --
  // that is the fetch constant's pitch, which the hardware uses for the base
  // level and ignores for every other one.
  uint32_t pitch_blocks = 0;
  uint32_t slice_stride_bytes = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  // Rounded up from height by the format's block height, so the decoder does
  // not have to carry the block dimensions into the level loop.
  uint32_t width_blocks = 0;
  uint32_t height_blocks = 0;
  // Displacement into a packed mip tail, in blocks. Non-zero for the base level
  // of a texture 16 texels or smaller, and for every level at or beyond the
  // packed level, which all share one tail image.
  uint32_t packed_offset_x_blocks = 0;
  uint32_t packed_offset_y_blocks = 0;
};

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
  // PACKED MIP TAIL. A texture whose width OR height is 16 or smaller does not
  // store its BASE level plainly at base_address: the level lives inside a mip
  // tail, offset by these block counts. From the SDK,
  // rex/graphics/pipeline/texture/util.h:77 -- "the mip tail can be used both
  // for the base level and mips (1...) if the entire texture has width or
  // height of 16 or smaller", and the game's own tiling routine checks only the
  // packed flag, never the level.
  //
  // The offsets are not decorative. Measured against the SDK for an 8x8 DXT1
  // the base sits at x=4 blocks, and for an 8x8 k_8_8_8_8 at x=16 -- so reading
  // from the origin returns a whole texture's worth of unrelated bytes. Zero
  // for anything larger than 16, and zero when the fetch constant does not set
  // packed_mips.
  uint32_t packed_offset_x_blocks = 0;
  uint32_t packed_offset_y_blocks = 0;
  // THE MIP CHAIN. The guest allocates it SEPARATELY from the base level, at
  // its own address (xenos.h:1249-1266: mip_address is stored >> 12 like
  // base_address, and the levels run mip_min_level..mip_max_level). Measured
  // over 460,000 binds in a level: 29% carry a chain, at a readable address,
  // up to twelve levels deep, and every one of them sets packed_mips.
  //
  // mip_address is zero when there is no chain, and that ZERO WINS over a
  // non-zero mip_max_level -- 81,934 binds in that run declare levels with no
  // address, which the reference collapses to a single level
  // (xenia/gpu/texture_util.cc:82). The fields below are the raw fetch values;
  // level_count is what survived that normalisation.
  uint32_t mip_address = 0;
  uint32_t mip_min_level = 0;
  uint32_t mip_max_level = 0;
  // TextureFilter: kPoint(0), kLinear(1), kBaseMap(2). kBaseMap means the guest
  // never wants to minify past level 0, and we honour it by not building a
  // chain at all -- see the note in DescribeHleTexture2D.
  uint8_t mip_filter = 0;
  bool packed_mips = false;
  // Bytes to copy from mip_address. Zero when there is no chain.
  uint32_t mip_source_bytes = 0;
  uint32_t level_count = 1;
  // Indexed by ABSOLUTE level, matching how the reference indexes its own
  // offsets. **levels[0] is deliberately unused and stays zero**: the base
  // level is described by the flat fields above and by nothing else, so the two
  // cannot drift apart, and a caller that fills this struct by hand -- the
  // decode tests do -- keeps working without knowing the chain exists.
  HleTextureLevel levels[kMaxTextureLevels] = {};
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

// What the mip chain looked like across every texture described so far. Kept
// because "the field exists" is not "the title fills it", and because the two
// suppressed cases below are deliberate gaps that should be visible as numbers
// rather than inferred from the picture.
struct HleMipCensus {
  uint64_t described = 0;
  uint64_t with_chain = 0;      // level_count > 1
  uint64_t levels_planned = 0;  // summed, so the mean depth is recoverable
  // mip_max_level says there are levels, mip_address says there are none. The
  // reference resolves that in favour of no levels; this counts how often the
  // question is asked.
  uint64_t no_address = 0;
  uint64_t suppressed_base_map = 0;   // guest asked for base map only
  uint64_t suppressed_min_level = 0;  // mip_min_level > 0, not handled yet
  uint64_t layout_empty = 0;
  // Two populations that decide whether the deferred work is worth doing:
  // point mip filtering (we always filter linearly between levels) and a
  // non-zero LOD bias (we ignore it).
  uint64_t mip_filter_point = 0;
  uint64_t lod_bias_set = 0;
  // Decode reached a level whose bytes were not there and served the levels it
  // had. Should be zero; anything else means the mip allocation is shorter or
  // less resident than the layout claims.
  uint64_t truncated = 0;
  // The RAW fetch fields, before any normalisation, so these numbers can be
  // compared directly against the probe that motivated this work rather than
  // only against themselves. by_max_level is indexed by fetch.mip_max_level.
  uint64_t raw_mip_address_set = 0;
  uint64_t by_max_level[16] = {};
};
HleMipCensus HleMipChainStats();

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
