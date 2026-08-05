#include "gpu/d3d9_texture.h"

#include <algorithm>
#include <bit>
#include <cstring>

#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/xenos.h>

namespace mx::pm4 {
namespace {
namespace xenos = rex::graphics::xenos;
namespace tu = rex::graphics::texture_util;

void SwapBlock(uint8_t* p, uint32_t bytes, xenos::Endian endian) {
  for (uint32_t i = 0; i + 4 <= bytes; i += 4) {
    uint32_t v;
    std::memcpy(&v, p + i, 4);
    v = xenos::GpuSwap(v, endian);
    std::memcpy(p + i, &v, 4);
  }
}
}  // namespace

uint64_t HleTextureKey(const uint32_t fetch_words[6]) {
  uint64_t h = 1469598103934665603ull;
  for (uint32_t i = 0; i < 6; ++i) {
    h ^= fetch_words[i];
    h *= 1099511628211ull;
  }
  return h;
}

bool DescribeHleTexture2D(const uint32_t fetch_words[6],
                          HleTextureSource& out, const char** fail) {
  auto reject = [&](const char* why) {
    if (fail) *fail = why;
    return false;
  };
  if (fail) *fail = nullptr;
  out = {};
  if (!fetch_words) return reject("null fetch constant");
  xenos::xe_gpu_texture_fetch_t fetch{};
  std::memcpy(&fetch, fetch_words, sizeof(fetch));
  if (fetch.type != xenos::FetchConstantType::kTexture)
    return reject("fetch constant is not a texture");
  if (fetch.dimension != xenos::DataDimension::k2DOrStacked || fetch.stacked)
    return reject("texture is not plain 2D");
  if (!fetch.base_address) return reject("texture has no base level");

  const auto format = rex::graphics::GetBaseFormat(fetch.format);
  switch (format) {
    case xenos::TextureFormat::k_8_8_8_8:
      out.host_format = HostTextureFormat::kRgba8;
      break;
    case xenos::TextureFormat::k_DXT1:
      out.host_format = HostTextureFormat::kBc1;
      break;
    case xenos::TextureFormat::k_DXT2_3:
      out.host_format = HostTextureFormat::kBc2;
      break;
    case xenos::TextureFormat::k_DXT4_5:
      out.host_format = HostTextureFormat::kBc3;
      break;
    // DXN is the Xbox 360 two-channel block-compressed normal-map format.
    // Its 4x4 / 16-byte storage is host-compatible with BC5_UNORM after the
    // same fetch-endian conversion and untile pass used by DXT textures.
    case xenos::TextureFormat::k_DXN:
      out.host_format = HostTextureFormat::kBc5;
      break;
    // ReXGlue's canonical mapping folds the EXPAND variants observed in
    // ST_Southwest into their corresponding half-float formats.
    case xenos::TextureFormat::k_16_FLOAT:
      out.host_format = HostTextureFormat::kR16Float;
      break;
    case xenos::TextureFormat::k_16_16_16_16_FLOAT:
      out.host_format = HostTextureFormat::kRgba16Float;
      break;
    default:
      return reject("unsupported texture format");
  }
  const auto* fi = rex::graphics::FormatInfo::Get(format);
  if (!fi || !fi->bytes_per_block()) return reject("invalid format metadata");

  out.width = fetch.size_2d.width + 1;
  out.height = fetch.size_2d.height + 1;
  out.block_width = fi->block_width;
  out.block_height = fi->block_height;
  out.bytes_per_block = fi->bytes_per_block();
  const uint32_t width_blocks =
      (out.width + out.block_width - 1) / out.block_width;
  const uint32_t height_blocks =
      (out.height + out.block_height - 1) / out.block_height;
  const uint32_t pitch_texels = std::max(fetch.pitch * 32u, out.width);
  out.pitch_blocks =
      std::max((pitch_texels + out.block_width - 1) / out.block_width,
               width_blocks);
  out.address = fetch.base_address << 12;
  out.endian = uint32_t(fetch.endianness);
  out.swizzle = fetch.swizzle;
  out.clamp_x = uint32_t(fetch.clamp_x);
  out.clamp_y = uint32_t(fetch.clamp_y);
  out.tiled = fetch.tiled != 0;
  out.linear_filter = fetch.min_filter != xenos::TextureFilter::kPoint &&
                      fetch.mag_filter != xenos::TextureFilter::kPoint;

  if (out.tiled) {
    const uint32_t bpb_log2 = std::bit_width(out.bytes_per_block) - 1;
    out.source_bytes = tu::GetTiledAddressUpperBound2D(
        width_blocks, height_blocks, out.pitch_blocks, bpb_log2);
  } else {
    const uint64_t row = uint64_t(out.pitch_blocks) * out.bytes_per_block;
    const uint64_t extent = row * (height_blocks - 1) +
                            uint64_t(width_blocks) * out.bytes_per_block;
    if (extent > UINT32_MAX) return reject("texture extent overflow");
    out.source_bytes = uint32_t(extent);
  }
  if (!out.source_bytes || out.source_bytes > 256u * 1024u * 1024u)
    return reject("texture extent out of range");
  return true;
}

bool DecodeHleTexture2D(const HleTextureSource& source,
                        const uint8_t* guest, size_t guest_bytes,
                        HleTexturePayload& out, const char** fail) {
  auto reject = [&](const char* why) {
    if (fail) *fail = why;
    return false;
  };
  if (fail) *fail = nullptr;
  if (!guest || guest_bytes < source.source_bytes)
    return reject("texture source is truncated");
  const uint32_t wb =
      (source.width + source.block_width - 1) / source.block_width;
  const uint32_t hb =
      (source.height + source.block_height - 1) / source.block_height;
  const uint64_t tight = uint64_t(wb) * hb * source.bytes_per_block;
  if (!tight || tight > UINT32_MAX) return reject("decoded extent overflow");

  out = {};
  out.width = source.width;
  out.height = source.height;
  out.row_pitch = wb * source.bytes_per_block;
  out.format = source.host_format;
  out.swizzle = source.swizzle;
  out.clamp_x = uint8_t(source.clamp_x);
  out.clamp_y = uint8_t(source.clamp_y);
  out.linear_filter = source.linear_filter;
  out.data.resize(size_t(tight));

  const auto endian = static_cast<xenos::Endian>(source.endian);
  const uint32_t bpb_log2 = std::bit_width(source.bytes_per_block) - 1;
  for (uint32_t y = 0; y < hb; ++y) {
    for (uint32_t x = 0; x < wb; ++x) {
      const uint64_t src = source.tiled
          ? uint64_t(tu::GetTiledOffset2D(x, y, source.pitch_blocks,
                                         bpb_log2))
          : (uint64_t(y) * source.pitch_blocks + x) *
                source.bytes_per_block;
      if (src + source.bytes_per_block > guest_bytes)
        return reject("tiled block outside source");
      uint8_t* dst = out.data.data() +
                     (uint64_t(y) * wb + x) * source.bytes_per_block;
      std::memcpy(dst, guest + src, source.bytes_per_block);
      SwapBlock(dst, source.bytes_per_block, endian);
    }
  }
  return true;
}

}  // namespace mx::pm4
