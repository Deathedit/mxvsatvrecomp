// Standalone tests for the SDK-independent HLE texture payload contract.
// Build with:
//   clang++ -std=c++23 -I src -I C:/rexglue-sdk/include -o d3d9_texture_test.exe \
//     tools/d3d9_texture_test.cpp src/gpu/d3d9_texture.cpp

#include "gpu/d3d9_texture.h"

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
  using namespace mx::pm4;
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
