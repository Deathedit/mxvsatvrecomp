#pragma once

#include <cstddef>
#include <cstdint>

#include "gpu/pm4_translator.h"

namespace mx::pm4 {

struct HleTextureSource {
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
  bool tiled = false;
  bool linear_filter = true;
  HostTextureFormat host_format = HostTextureFormat::kRgba8;
};

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

}  // namespace mx::pm4
