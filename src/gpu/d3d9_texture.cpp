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
uint8_t SwizzleTextureSigns(uint8_t signs, uint32_t swizzle) {
  if (!signs) return 0;
  uint8_t out = 0;
  for (uint32_t c = 0; c < 4; ++c) {
    const uint32_t src = (swizzle >> (c * 3)) & 7u;
    if (src > 3) continue;  // forced 0 or 1: no guest component, so unsigned.
    out |= uint8_t(((signs >> (src * 2)) & 3u) << (c * 2));
  }
  return out;
}

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
  // A 1D texture is one row, and every size/pitch/extent computation below
  // generalises to height 1 unchanged — so it is described as a width x 1 2D
  // texture and binds through the ordinary 2D path. The shader side pins v to
  // the row centre. Its extent lives in a different union member (size_1d has
  // 24 bits of width, size_2d only 13), so reading it the 2D way would truncate
  // any row wider than 8192.
  //
  // CUBE and STACKED textures are the same thing in memory as several 2D ones:
  // six faces, or stack_depth+1 slices, each an ordinary 2D image, each starting
  // 4 KB-aligned from the last (see the layout note in
  // rex/graphics/pipeline/texture/util.h). So they are described here as a 2D
  // texture plus a slice count and stride, decoded by running the 2D untile once
  // per slice, and bound as a Texture2DArray. Only true 3D volumes, whose slices
  // interleave INSIDE a tile, need a resource type we do not build.
  const bool one_d = fetch.dimension == xenos::DataDimension::k1D;
  const bool cube = fetch.dimension == xenos::DataDimension::kCube;
  if (fetch.dimension == xenos::DataDimension::k3D)
    return reject("texture is a 3D volume");
  if (cube) {
    // size_2d.stack_depth is documented as 5 for a cube but "not very
    // meaningful"; a cube has six faces by definition, so do not read it.
    out.array_size = 6;
  } else if (!one_d && fetch.stacked) {
    out.array_size = fetch.size_2d.stack_depth + 1;
  }
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
    // Depth sampled as a texture. This was the ONLY format rejected across
    // whole runs while it was rejected once per run -- a shader read it, gave
    // up, and the draw fell back. Once the pixel shaders that sample depth
    // started translating it became 29,240 rejections in a single run
    // (mx_716), the largest texture rejection by far and the last remaining
    // one of any size.
    //
    // Carried as R32Float rather than a depth format: the guest samples this
    // as ordinary colour data, and a host DSV-backed SRV would drag format
    // typelessness through the whole binding path for no gain. The 24-bit
    // integer depth is converted to float in DecodeHleTexture2D, which is the
    // one place that knows the guest format.
    case xenos::TextureFormat::k_24_8:
    case xenos::TextureFormat::k_24_8_FLOAT:
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

  out.width = (one_d ? fetch.size_1d.width : fetch.size_2d.width) + 1;
  out.height = one_d ? 1u : fetch.size_2d.height + 1;
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
  out.signs = uint8_t((uint32_t(fetch.sign_x) & 3u) |
                      ((uint32_t(fetch.sign_y) & 3u) << 2) |
                      ((uint32_t(fetch.sign_z) & 3u) << 4) |
                      ((uint32_t(fetch.sign_w) & 3u) << 6));
  out.tiled = fetch.tiled != 0;
  out.linear_filter = fetch.min_filter != xenos::TextureFilter::kPoint &&
                      fetch.mag_filter != xenos::TextureFilter::kPoint;

  // The base level of a small texture is inside the packed mip tail, not at the
  // origin. GetPackedMipOffset reports false for anything whose base is stored
  // plainly, so this is self-gating: only textures with a dimension of 16 or
  // less come back packed, and everything larger keeps a zero offset.
  //
  // Gated on the fetch constant's flag as well, because the tail only exists if
  // the guest asked for one -- see xenos.h:1265, packed_mips at bit +11.
  if (fetch.packed_mips) {
    uint32_t px = 0, py = 0, pz = 0;
    if (tu::GetPackedMipOffset(out.width, out.height, 1, format, /*mip=*/0, px,
                               py, pz)) {
      out.packed_offset_x_blocks = px;
      out.packed_offset_y_blocks = py;
    }
  }
  // The extent has to cover the offset too, or the bounds check in the decode
  // rejects every block of a packed texture as "outside source".
  const uint32_t reach_x_blocks = width_blocks + out.packed_offset_x_blocks;
  const uint32_t reach_y_blocks = height_blocks + out.packed_offset_y_blocks;

  if (out.tiled) {
    const uint32_t bpb_log2 = std::bit_width(out.bytes_per_block) - 1;
    out.source_bytes = tu::GetTiledAddressUpperBound2D(
        reach_x_blocks, reach_y_blocks, out.pitch_blocks, bpb_log2);
  } else {
    const uint64_t row = uint64_t(out.pitch_blocks) * out.bytes_per_block;
    const uint64_t extent = row * (reach_y_blocks - 1) +
                            uint64_t(reach_x_blocks) * out.bytes_per_block;
    if (extent > UINT32_MAX) return reject("texture extent overflow");
    out.source_bytes = uint32_t(extent);
  }
  // Multi-slice: the distance between slices, and the extent covering all of
  // them, come from the SDK's own layout calculation rather than being derived
  // here. The 4 KB alignment interacts with tile padding and the packed-mip
  // rules in ways the single-image arithmetic above does not model, and this is
  // the same computation the reference emulator validated against real titles.
  if (out.array_size > 1) {
    const tu::TextureGuestLayout layout = tu::GetGuestTextureLayout(
        fetch.dimension, fetch.pitch, out.width, out.height, out.array_size,
        out.tiled, format, /*has_packed_levels=*/false, /*has_base=*/true,
        /*max_level=*/0);
    if (!layout.base.array_slice_stride_bytes ||
        !layout.base.level_data_extent_bytes)
      return reject("array texture layout is empty");
    out.slice_stride_bytes = layout.base.array_slice_stride_bytes;
    out.source_bytes = layout.base.level_data_extent_bytes;
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
  // One slice's worth, then multiplied up: the host wants the slices TIGHTLY
  // packed one after another (that is what GetCopyableFootprints will be handed
  // per subresource), which is not how the guest stores them -- guest slices are
  // 4 KB-aligned and may be tiled, so the gap between them is
  // source.slice_stride_bytes on the way in and nothing on the way out.
  const uint64_t slice_tight = uint64_t(wb) * hb * source.bytes_per_block;
  const uint32_t slices = std::max(source.array_size, 1u);
  const uint64_t tight = slice_tight * slices;
  if (!tight || tight > UINT32_MAX) return reject("decoded extent overflow");

  out = {};
  out.width = source.width;
  out.height = source.height;
  out.array_size = slices;
  out.row_pitch = wb * source.bytes_per_block;
  out.format = source.host_format;
  out.swizzle = source.swizzle;
  out.clamp_x = uint8_t(source.clamp_x);
  out.clamp_y = uint8_t(source.clamp_y);
  out.linear_filter = source.linear_filter;
  out.data.resize(size_t(tight));

  const auto endian = static_cast<xenos::Endian>(source.endian);
  const uint32_t bpb_log2 = std::bit_width(source.bytes_per_block) - 1;
  for (uint32_t slice = 0; slice < slices; ++slice) {
    const uint64_t slice_base = uint64_t(slice) * source.slice_stride_bytes;
    uint8_t* slice_out = out.data.data() + uint64_t(slice) * slice_tight;
    for (uint32_t y = 0; y < hb; ++y) {
      for (uint32_t x = 0; x < wb; ++x) {
        // Read coordinates, which are the write coordinates displaced into the
        // packed mip tail. Both zero for any texture bigger than 16 texels, so
        // this is the identity for the overwhelming majority of textures.
        const uint32_t sx = x + source.packed_offset_x_blocks;
        const uint32_t sy = y + source.packed_offset_y_blocks;
        const uint64_t src = slice_base +
            (source.tiled
                 ? uint64_t(tu::GetTiledOffset2D(sx, sy, source.pitch_blocks,
                                                 bpb_log2))
                 : (uint64_t(sy) * source.pitch_blocks + sx) *
                       source.bytes_per_block);
        if (src + source.bytes_per_block > guest_bytes)
          return reject("tiled block outside source");
        uint8_t* dst =
            slice_out + (uint64_t(y) * wb + x) * source.bytes_per_block;
        std::memcpy(dst, guest + src, source.bytes_per_block);
        SwapBlock(dst, source.bytes_per_block, endian);
      }
    }
  }

  // Depth is the one accepted format whose BYTES are not already the host's.
  // Every other entry is a straight copy plus an endian swap, so this is the
  // only place a value conversion happens, and it is done after the swap so it
  // reads host-order words.
  //
  // k_24_8 packs 24 bits of depth above 8 bits of stencil. The shader wants the
  // depth as it wrote it, in [0,1], so the stencil is dropped and the integer
  // is normalised -- NOT reinterpreted as a float, which would produce
  // denormals and NaNs from perfectly ordinary depth values.
  //
  // k_24_8_FLOAT shares the layout but stores a float depth; it is converted
  // the same way here, which is wrong in the last bits of precision and right
  // in range. Called out rather than hidden: it has never been observed in this
  // title, and doing it properly needs a sample to check against.
  if (source.guest_format == uint32_t(xenos::TextureFormat::k_24_8) ||
      source.guest_format == uint32_t(xenos::TextureFormat::k_24_8_FLOAT)) {
    if (source.bytes_per_block != 4) return reject("24_8 block is not 4 bytes");
    const size_t texels = out.data.size() / 4;
    for (size_t i = 0; i < texels; ++i) {
      uint32_t raw = 0;
      std::memcpy(&raw, out.data.data() + i * 4, 4);
      const float depth = float(raw >> 8) * (1.0f / 16777215.0f);
      std::memcpy(out.data.data() + i * 4, &depth, 4);
    }
  }
  return true;
}

}  // namespace mx::hle
