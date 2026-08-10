// Standalone tests for the SDK-independent HLE texture payload contract.
// Not part of the CMake build; invoke it by hand. The tiling helpers and
// FormatInfo::Get live in rexruntime, not rexgpu-xenos, and the matching DLL
// must be on PATH to run:
//   clang++ -std=c++23 -I src -I C:/rexglue-sdk/include -o d3d9_texture_test.exe \
//     tools/d3d9_texture_test.cpp src/gpu/d3d9_texture.cpp \
//     C:/rexglue-sdk/lib/rexruntime.lib
//   PATH=C:/rexglue-sdk/bin:$PATH ./d3d9_texture_test.exe

#include "gpu/d3d9_texture.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/xenos.h>

namespace {
int failures = 0;
void Check(bool value, const char* what) {
  if (value) return;
  std::printf("FAIL: %s\n", what);
  ++failures;
}
}

int main() {
  using namespace mx::hle;
  HleTextureSource linear{};
  linear.source_bytes = 16;
  linear.width = 2;
  linear.height = 2;
  linear.pitch_blocks = 2;
  linear.block_width = linear.block_height = 1;
  linear.bytes_per_block = 4;
  linear.swizzle = 0x60A;
  linear.host_format = HostTextureFormat::kRgba8;
  const uint8_t rgba[16] = {0,1,2,3, 4,5,6,7, 8,9,10,11, 12,13,14,15};
  HleTexturePayload decoded;
  const char* why = nullptr;
  Check(DecodeHleTexture2D(linear, rgba, sizeof(rgba), decoded, &why),
        "linear RGBA8 decode");
  Check(decoded.row_pitch == 8 && decoded.data.size() == 16,
        "linear RGBA8 pitch and extent");
  Check(decoded.data == std::vector<uint8_t>(rgba, rgba + 16),
        "linear RGBA8 bytes");
  Check(decoded.swizzle == 0x60A, "swizzle metadata retained");
  size_t nonzero = 0;
  Check(HleTextureHasNonzeroData(decoded, &nonzero) && nonzero == 15,
        "RGBA8 activity count");
  HleTexturePayload cleared = decoded;
  std::fill(cleared.data.begin(), cleared.data.end(), 0);
  Check(!HleTextureHasNonzeroData(cleared, &nonzero) && nonzero == 0,
        "cleared texture rejected as empty");

  HleTextureSource swapped = linear;
  swapped.width = swapped.height = swapped.pitch_blocks = 1;
  swapped.source_bytes = 4;
  swapped.endian = uint32_t(rex::graphics::xenos::Endian::k8in32);
  Check(DecodeHleTexture2D(swapped, rgba, 4, decoded, &why),
        "8-in-32 endian decode");
  Check(decoded.data[0] == 3 && decoded.data[1] == 2 &&
        decoded.data[2] == 1 && decoded.data[3] == 0,
        "8-in-32 endian bytes");

  HleTextureSource bc{};
  bc.source_bytes = 32;
  bc.width = 7;
  bc.height = 5;
  bc.pitch_blocks = 2;
  bc.block_width = bc.block_height = 4;
  bc.bytes_per_block = 8;
  bc.host_format = HostTextureFormat::kBc1;
  std::vector<uint8_t> blocks(32, 0x5A);
  Check(DecodeHleTexture2D(bc, blocks.data(), blocks.size(), decoded, &why),
        "BC1 decode");
  Check(decoded.row_pitch == 16 && decoded.data.size() == 32,
        "BC1 block pitch and extent");

  rex::graphics::xenos::xe_gpu_texture_fetch_t fetchBc5{};
  fetchBc5.type = rex::graphics::xenos::FetchConstantType::kTexture;
  fetchBc5.dimension = rex::graphics::xenos::DataDimension::k2DOrStacked;
  fetchBc5.base_address = 1;
  fetchBc5.pitch = 1;
  fetchBc5.size_2d.width = 31;
  fetchBc5.size_2d.height = 31;
  fetchBc5.format = rex::graphics::xenos::TextureFormat::k_DXN;
  HleTextureSource bc5{};
  const auto* fetchBc5Words =
      reinterpret_cast<const uint32_t*>(&fetchBc5);
  Check(DescribeHleTexture2D(fetchBc5Words, bc5, &why),
        "DXN descriptor accepted");
  Check(bc5.host_format == HostTextureFormat::kBc5 &&
            bc5.block_width == 4 && bc5.block_height == 4 &&
            bc5.bytes_per_block == 16,
        "DXN maps to BC5 block storage");

  HleTextureSource tiled = linear;
  tiled.width = tiled.height = 8;
  tiled.pitch_blocks = 32;
  tiled.tiled = true;
  tiled.source_bytes = rex::graphics::texture_util::GetTiledAddressUpperBound2D(
      8, 8, tiled.pitch_blocks, 2);
  std::vector<uint8_t> tiledBytes(tiled.source_bytes);
  for (uint32_t y = 0; y < 8; ++y) {
    for (uint32_t x = 0; x < 8; ++x) {
      const uint32_t at = rex::graphics::texture_util::GetTiledOffset2D(
          x, y, tiled.pitch_blocks, 2);
      const uint32_t value = y * 8 + x;
      std::memcpy(tiledBytes.data() + at, &value, 4);
    }
  }
  Check(DecodeHleTexture2D(tiled, tiledBytes.data(), tiledBytes.size(), decoded,
                           &why), "tiled RGBA8 decode");
  bool ordered = decoded.data.size() == 8 * 8 * 4;
  for (uint32_t i = 0; ordered && i < 64; ++i) {
    uint32_t value = 0;
    std::memcpy(&value, decoded.data.data() + i * 4, 4);
    ordered = value == i;
  }
  Check(ordered, "tiled RGBA8 ordering");

  rex::graphics::xenos::xe_gpu_texture_fetch_t fetch16{};
  fetch16.type = rex::graphics::xenos::FetchConstantType::kTexture;
  fetch16.dimension = rex::graphics::xenos::DataDimension::k2DOrStacked;
  fetch16.base_address = 1;
  fetch16.pitch = 1;
  fetch16.size_2d.width = 15;
  fetch16.size_2d.height = 15;
  fetch16.format =
      rex::graphics::xenos::TextureFormat::k_16_16_16_16_EXPAND;
  HleTextureSource expanded{};
  const auto* fetch16Words = reinterpret_cast<const uint32_t*>(&fetch16);
  Check(DescribeHleTexture2D(fetch16Words, expanded, &why),
        "16_16_16_16_EXPAND descriptor accepted");
  Check(expanded.host_format == HostTextureFormat::kRgba16Float &&
            expanded.bytes_per_block == 8,
        "16_16_16_16_EXPAND maps to RGBA16 float storage");
  fetch16.format = rex::graphics::xenos::TextureFormat::k_16_EXPAND;
  Check(DescribeHleTexture2D(fetch16Words, expanded, &why),
        "16_EXPAND descriptor accepted");
  Check(expanded.host_format == HostTextureFormat::kR16Float &&
            expanded.bytes_per_block == 2,
        "16_EXPAND maps to R16 float storage");

  // The three formats measured live in native runs. k_4_4_4_4 is the front
  // end's own and was the only format it was ever seen to ask for.
  fetch16.format = rex::graphics::xenos::TextureFormat::k_4_4_4_4;
  Check(DescribeHleTexture2D(fetch16Words, expanded, &why),
        "4_4_4_4 descriptor accepted");
  Check(expanded.host_format == HostTextureFormat::kBgra4 &&
            expanded.bytes_per_block == 2,
        "4_4_4_4 maps to BGRA4 storage");
  Check(expanded.guest_format ==
            uint32_t(rex::graphics::xenos::TextureFormat::k_4_4_4_4),
        "guest format index carried out of the descriptor");
  fetch16.format = rex::graphics::xenos::TextureFormat::k_8;
  Check(DescribeHleTexture2D(fetch16Words, expanded, &why) &&
            expanded.host_format == HostTextureFormat::kR8,
        "k_8 maps to R8 storage");
  fetch16.format = rex::graphics::xenos::TextureFormat::k_16;
  Check(DescribeHleTexture2D(fetch16Words, expanded, &why) &&
            expanded.host_format == HostTextureFormat::kR16,
        "k_16 maps to R16 storage");

  // A rejected descriptor must still name its format, or the reject log
  // cannot say what to add next.
  fetch16.format = rex::graphics::xenos::TextureFormat::k_10_11_11;
  Check(!DescribeHleTexture2D(fetch16Words, expanded, &why),
        "unsupported format still rejected");
  Check(expanded.guest_format ==
            uint32_t(rex::graphics::xenos::TextureFormat::k_10_11_11),
        "rejected descriptor still names its guest format");
  Check(std::strcmp(GuestTextureFormatName(15), "FMT_4_4_4_4") == 0 &&
            std::strcmp(GuestTextureFormatName(49), "FMT_DXN") == 0 &&
            std::strcmp(GuestTextureFormatName(6), "FMT_8_8_8_8") == 0,
        "guest format names match the guest's own table at 0x82d24378");

  // Regression: SwapBlock's dword loop skipped 2-byte blocks entirely, so
  // every 16-bit format uploaded byte-reversed.
  {
    HleTextureSource narrow{};
    narrow.source_bytes = 4;
    narrow.width = 2;
    narrow.height = 1;
    narrow.pitch_blocks = 2;
    narrow.block_width = narrow.block_height = 1;
    narrow.bytes_per_block = 2;
    narrow.host_format = HostTextureFormat::kBgra4;
    narrow.endian = uint32_t(rex::graphics::xenos::Endian::k8in16);
    const uint8_t src16[4] = {0x12, 0x34, 0x56, 0x78};
    HleTexturePayload out16;
    Check(DecodeHleTexture2D(narrow, src16, sizeof(src16), out16, &why),
          "2-byte block decode");
    Check(out16.data.size() == 4 && out16.data[0] == 0x34 &&
              out16.data[1] == 0x12 && out16.data[2] == 0x78 &&
              out16.data[3] == 0x56,
          "2-byte blocks are endian-swapped");
  }

  // PACKED MIP TAIL. A texture 16 texels or smaller stores its base level at an
  // offset inside the tail, not at the origin. The offsets come from the SDK's
  // GetPackedMipOffset -- for an 8x8 DXT1 it reports x=4 blocks -- and this
  // checks the decode actually reads from there. Before the fix every such
  // texture returned the bytes at x=0, which belong to a different image.
  {
    HleTextureSource packed{};
    packed.width = 8;
    packed.height = 8;
    packed.block_width = packed.block_height = 4;  // DXT1: 2x2 blocks
    packed.bytes_per_block = 8;
    packed.pitch_blocks = 8;
    packed.packed_offset_x_blocks = 4;
    packed.host_format = HostTextureFormat::kBc1;
    packed.source_bytes = 6 * 8;  // (2 + 4) blocks across one row
    // One byte per block, so the value at a block says where it was read from.
    std::vector<uint8_t> tail(8 * 2 * 8, 0);
    for (uint32_t by = 0; by < 2; ++by)
      for (uint32_t bx = 0; bx < 8; ++bx)
        tail[(by * 8 + bx) * 8] = uint8_t(0x10 * by + bx);
    HleTexturePayload outp;
    Check(DecodeHleTexture2D(packed, tail.data(), tail.size(), outp, &why),
          "packed mip tail decode");
    // Block (0,0) of the image must be block (4,0) of the tail, not (0,0).
    Check(outp.data.size() == 2 * 2 * 8, "packed decode extent is the image");
    Check(outp.data[0] == 0x04 && outp.data[8] == 0x05,
          "packed base reads from the tail offset, not the origin");
    Check(outp.data[16] == 0x14 && outp.data[24] == 0x15,
          "packed base second block row");

    // The identity case: no offset must read from the origin exactly as before.
    HleTextureSource unpacked = packed;
    unpacked.packed_offset_x_blocks = 0;
    Check(DecodeHleTexture2D(unpacked, tail.data(), tail.size(), outp, &why),
          "unpacked decode");
    Check(outp.data[0] == 0x00 && outp.data[8] == 0x01,
          "a zero offset is the identity");
  }

  Check(!DecodeHleTexture2D(linear, rgba, sizeof(rgba) - 1, decoded, &why),
        "truncated source rejected");
  uint32_t fetchA[6] = {1,2,3,4,5,6};
  uint32_t fetchB[6] = {1,2,3,4,5,7};
  Check(HleTextureKey(fetchA) == HleTextureKey(fetchA), "stable cache key");
  Check(HleTextureKey(fetchA) != HleTextureKey(fetchB), "distinct cache key");

  std::printf("%s (%d failure%s)\n", failures ? "FAILED" : "PASSED", failures,
              failures == 1 ? "" : "s");
  return failures ? 1 : 0;
}
