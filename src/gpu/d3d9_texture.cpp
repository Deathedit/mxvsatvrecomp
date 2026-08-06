#include "gpu/d3d9_texture.h"

#include <algorithm>
#include <bit>
#include <cstring>

#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/xenos.h>

namespace mx::hle {
namespace {
namespace xenos = rex::graphics::xenos;
namespace tu = rex::graphics::texture_util;

void SwapBlock(uint8_t* p, uint32_t bytes, xenos::Endian endian) {
  // A 2-byte block never entered the dword loop below, so every 16-bit format
  // was uploaded byte-reversed. That was invisible while k_16_FLOAT was the
  // only such format and the semantic gate refused to bind it; k_4_4_4_4 and
  // k_16 are both 2 bytes and both measured live, so it is load-bearing now.
  if (bytes == 2) {
    if (endian == xenos::Endian::k8in16 || endian == xenos::Endian::k8in32)
      std::swap(p[0], p[1]);
    return;
  }
  for (uint32_t i = 0; i + 4 <= bytes; i += 4) {
    uint32_t v;
    std::memcpy(&v, p + i, 4);
    v = xenos::GpuSwap(v, endian);
    std::memcpy(p + i, &v, 4);
  }
}
}  // namespace

// Transcribed from the guest's own GPUTEXTUREFORMAT name table: a 64-entry
// pointer array at 0x82d24378 (near-duplicate at 0x82d59d00), indexing the
// FMT_* strings in 0x820a9d00-0x820a9fd0 and 0x8205b5ec-0x8205b6d0. Both
// tables belong to the HLSL compiler embedded in the XEX (sub_8263F9C0 and
// sub_82C1BB88), so they are guest diagnostics rather than the runtime texture
// path — but they are authoritative for the enum, and decoding them confirmed
// index-for-index that fetch.format indexes the same ordering xenos.h assumes.
// Spellings are the guest's own, including index 20's truncated "FMT_4_5" for
// DXT4_5 and index 0's lowercase tail. Indices 61-63 are compiler-internal and
// have no GPU meaning.
const char* GuestTextureFormatName(uint32_t guest_format) {
  static constexpr const char* kNames[64] = {
      "FMT_1_reverse", "FMT_1", "FMT_8", "FMT_1_5_5_5",
      "FMT_5_6_5", "FMT_6_5_5", "FMT_8_8_8_8", "FMT_2_10_10_10",
      "FMT_8_A", "FMT_8_B", "FMT_8_8", "FMT_CR",
      "FMT_Y1", "FMT_SHADOW", "FMT_8_8_8_8_A", "FMT_4_4_4_4",
      "FMT_10_11_11", "FMT_11_11_10", "FMT_DXT1", "FMT_DXT2_3",
      "FMT_4_5", "FMT_DXV", "FMT_24_8", "FMT_24_8_FLOAT",
      "FMT_16", "FMT_16_16", "FMT_16_16_16_16", "FMT_16_EXPAND",
      "FMT_16_16_EXPAND", "FMT_16_16_16_16_EXPAND", "FMT_16_FLOAT",
      "FMT_16_16_FLOAT",
      "FMT_16_16_16_16_FLOAT", "FMT_32", "FMT_32_32", "FMT_32_32_32_32",
      "FMT_32_FLOAT", "FMT_32_32_FLOAT", "FMT_32_32_32_32_FLOAT",
      "FMT_32_AS_8",
      "FMT_32_AS_8_8", "FMT_16_MPEG", "FMT_16_16_MPEG", "FMT_8_INTERLACED",
      "FMT_32_AS_8_INTERLACED", "FMT_32_AS_8_8_INTERLACED",
      "FMT_16_INTERLACED", "FMT_16_MPEG_INTERLACED",
      "FMT_16_16_INTERLACED", "FMT_DXN", "FMT_8_8_8_8_AS_16_16_16_16",
      "FMT_DXT1_AS_16_16_16_16",
      "FMT_DXT2_3_AS_16_16_16_16", "FMT_DXT4_5_AS_16_16_16_16",
      "FMT_2_10_10_10_AS_16_16_16_16", "FMT_10_11_11_AS_16_16_16_16",
      "FMT_11_11_10_AS_16_16_16_16", "FMT_32_32_32_FLOAT", "FMT_DXT3A",
      "FMT_DXT5A",
      "FMT_CTX1", "FMT_compiler_61", "FMT_compiler_62", "FMT_compiler_63",
  };
  return guest_format < 64 ? kNames[guest_format] : "FMT_?";
}

uint64_t HleTextureKey(const uint32_t fetch_words[6]) {
  uint64_t h = 1469598103934665603ull;
  for (uint32_t i = 0; i < 6; ++i) {
    h ^= fetch_words[i];
    h *= 1099511628211ull;
  }
  return h;
}

bool HleTextureHasNonzeroData(const HleTexturePayload& texture,
                              size_t* nonzero_bytes) {
  const size_t count = std::count_if(
      texture.data.begin(), texture.data.end(),
      [](uint8_t value) { return value != 0; });
  if (nonzero_bytes) *nonzero_bytes = count;
  return count != 0;
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
  // Recorded before the accept-list so the reject path can name what it hit.
  out.guest_format = uint32_t(format);
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
    // Measured, not assumed. k_4_4_4_4 is the front end's texture format and
    // the only one it was ever seen to ask for; B4G4R4A4_UNORM is the same bit
    // layout, so the existing untile-and-swap path carries it unchanged and
    // the fetch swizzle still resolves component order in the SRV.
    case xenos::TextureFormat::k_4_4_4_4:
      out.host_format = HostTextureFormat::kBgra4;
      break;
    // Both single-channel, and both seen only in the farm scene.
    case xenos::TextureFormat::k_8:
      out.host_format = HostTextureFormat::kR8;
      break;
    case xenos::TextureFormat::k_16:
      out.host_format = HostTextureFormat::kR16;
      break;
    case xenos::TextureFormat::k_32_FLOAT:
      out.host_format = HostTextureFormat::kR32Float;
      break;
    default:
      return reject("unsupported texture format");
  }
  const auto* fi = rex::graphics::FormatInfo::Get(format);
  if (!fi || !fi->bytes_per_block()) return reject("invalid format metadata");
  // Both tiling helpers take a bytes_per_block *log2*, so a block size that is
  // not a power of two (k_32_32_32_FLOAT is 12 bytes) would silently address
  // every block wrongly. Refuse it instead, so the failure is a log line.
  if (fi->bytes_per_block() & (fi->bytes_per_block() - 1))
    return reject("non-power-of-two block size");

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

}  // namespace mx::hle
