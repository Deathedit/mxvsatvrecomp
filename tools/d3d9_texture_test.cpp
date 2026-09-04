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
  // FMT_8_8, the format the reject tally has been naming: 1273 descriptors in
  // one run, every one of them a translated shader's slot that then failed the
  // whole draw back to the stand-in. 1x1 blocks of 2 bytes, matching the
  // reference's own R8G8_UNORM / 16bpb entry.
  fetch16.format = rex::graphics::xenos::TextureFormat::k_8_8;
  Check(DescribeHleTexture2D(fetch16Words, expanded, &why) &&
            expanded.host_format == HostTextureFormat::kRg8,
        "k_8_8 maps to RG8 storage");
  Check(expanded.bytes_per_block == 2 && expanded.block_width == 1 &&
            expanded.block_height == 1,
        "k_8_8 is a 2-byte 1x1 block");

  // THE G-BUFFER FORMATS. Four entries out of the guest's own render-target
  // format table (0x82D54414) that used to hit the accept-list default and drop
  // the draw. Each is checked for its host format AND its block geometry,
  // because a wrong bytes_per_block does not fail -- it reads plausible bytes
  // from the wrong place, which is the failure mode the packed-mip work already
  // paid for once.
  fetch16.format = rex::graphics::xenos::TextureFormat::k_16_16_FLOAT;
  Check(DescribeHleTexture2D(fetch16Words, expanded, &why) &&
            expanded.host_format == HostTextureFormat::kRg16Float,
        "k_16_16_FLOAT maps to RG16 float storage");
  Check(expanded.bytes_per_block == 4 && expanded.block_width == 1 &&
            expanded.block_height == 1,
        "k_16_16_FLOAT is a 4-byte 1x1 block");
  // The EXPAND spelling of the same thing, which is what the guest's table
  // actually stores (0x2D22AB9C). It must fold to the identical host format --
  // if GetBaseFormat ever stops folding it, this catches it here rather than as
  // a rejection in a run.
  fetch16.format = rex::graphics::xenos::TextureFormat::k_16_16_EXPAND;
  Check(DescribeHleTexture2D(fetch16Words, expanded, &why) &&
            expanded.host_format == HostTextureFormat::kRg16Float,
        "k_16_16_EXPAND folds onto the same RG16 float storage");

  fetch16.format = rex::graphics::xenos::TextureFormat::k_32_32_FLOAT;
  Check(DescribeHleTexture2D(fetch16Words, expanded, &why) &&
            expanded.host_format == HostTextureFormat::kRg32Float,
        "k_32_32_FLOAT maps to RG32 float storage");
  Check(expanded.bytes_per_block == 8, "k_32_32_FLOAT is an 8-byte block");

  // The two fixed-point ones, whose host format depends on TEX_FORMAT_COMP.
  // Both directions are asserted: a mapping that ignored the sign bits would
  // still pass the unsigned half.
  fetch16.format = rex::graphics::xenos::TextureFormat::k_16_16;
  Check(DescribeHleTexture2D(fetch16Words, expanded, &why) &&
            expanded.host_format == HostTextureFormat::kRg16Unorm,
        "k_16_16 with unsigned components maps to RG16 UNORM");
  Check(expanded.bytes_per_block == 4, "k_16_16 is a 4-byte block");
  fetch16.format = rex::graphics::xenos::TextureFormat::k_16_16_16_16;
  Check(DescribeHleTexture2D(fetch16Words, expanded, &why) &&
            expanded.host_format == HostTextureFormat::kRgba16Unorm,
        "k_16_16_16_16 with unsigned components maps to RGBA16 UNORM");
  Check(expanded.bytes_per_block == 8, "k_16_16_16_16 is an 8-byte block");

  // ONE signed component is enough, per the reference's IsAnySignSigned: the
  // storage is two's complement or it is not, and a format cannot be half of
  // each. Set only y, so a test that happened to read sign_x would fail.
  fetch16.sign_y = rex::graphics::xenos::TextureSign::kSigned;
  Check(DescribeHleTexture2D(fetch16Words, expanded, &why) &&
            expanded.host_format == HostTextureFormat::kRgba16Snorm,
        "k_16_16_16_16 with one signed component maps to RGBA16 SNORM");
  fetch16.format = rex::graphics::xenos::TextureFormat::k_16_16;
  Check(DescribeHleTexture2D(fetch16Words, expanded, &why) &&
            expanded.host_format == HostTextureFormat::kRg16Snorm,
        "k_16_16 with one signed component maps to RG16 SNORM");
  // kUnsignedBiased is NOT kSigned. It rides the shader's xe_texsign scale and
  // must leave the storage unsigned; treating "not plain unsigned" as "signed"
  // would reinterpret every biased texel as two's complement.
  fetch16.sign_y = rex::graphics::xenos::TextureSign::kUnsignedBiased;
  Check(DescribeHleTexture2D(fetch16Words, expanded, &why) &&
            expanded.host_format == HostTextureFormat::kRg16Unorm,
        "kUnsignedBiased does not select the signed storage");
  fetch16.sign_y = rex::graphics::xenos::TextureSign::kUnsigned;
  Check(DescribeHleTexture2D(fetch16Words, expanded, &why) &&
            expanded.signs == 0,
        "signs are carried out of the descriptor");

  // The 8-byte block claim the accept-list comment makes: SwapBlock's dword
  // loop runs TWICE over one block and each dword is swapped on its own. An
  // implementation that reversed all eight bytes would pass a 4-byte test and
  // fail here, which is exactly why this is checked at 8 and not at 4.
  {
    HleTextureSource wide{};
    wide.source_bytes = 8;
    wide.width = wide.height = wide.pitch_blocks = 1;
    wide.block_width = wide.block_height = 1;
    wide.bytes_per_block = 8;
    wide.host_format = HostTextureFormat::kRgba16Unorm;
    wide.endian = uint32_t(rex::graphics::xenos::Endian::k8in32);
    const uint8_t src64[8] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    HleTexturePayload out64;
    Check(DecodeHleTexture2D(wide, src64, sizeof(src64), out64, &why),
          "8-byte block decode");
    const uint8_t want[8] = {0x44, 0x33, 0x22, 0x11, 0x88, 0x77, 0x66, 0x55};
    Check(out64.data.size() == 8 &&
              std::memcmp(out64.data.data(), want, 8) == 0,
          "8-byte blocks swap as two independent dwords");
  }

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

    // The same swap, stated as the claim that matters for k_8_8: the guest
    // stores the texel big-endian and component 0 is its LOW half, so after
    // the swap host R must be component 0 and host G component 1. Get this
    // backwards and every two-channel texture samples with its channels
    // exchanged -- which for a normal map is a sign flip nothing else catches.
    narrow.host_format = HostTextureFormat::kRg8;
    const uint8_t rg[4] = {0xAA, 0xBB, 0xCC, 0xDD};
    Check(DecodeHleTexture2D(narrow, rg, sizeof(rg), out16, &why),
          "k_8_8 decode");
    Check(out16.data[0] == 0xBB && out16.data[1] == 0xAA,
          "k_8_8 host R is guest component 0");
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

  // THE MIP CHAIN. A 256x256 k_8_8_8_8 with packed mips, the shape that matters:
  // log2_ceil(256) - 4 == 4, so levels 1..3 have their own storage and levels
  // 4..8 share the packed tail. Both addressing paths in one case.
  //
  // The guest side is placed using GetGuestTextureLayout -- the same function
  // the describe path reads -- so this does not re-verify the SDK's layout. It
  // verifies OUR use of it: the per-level offset, the per-level pitch (which is
  // NOT the fetch pitch), the packed displacement, the level count, and the
  // level-major packing of the output.
  //
  // The dimensions are chosen so the test can FAIL. Two ways this went wrong
  // while being written, both found by mutating the decode and watching the test
  // still pass:
  //
  //  - At 64 texels wide, LINEAR proves nothing. A linear mip's pitch is rounded
  //    up to 256 bytes, which for a 64-wide RGBA8 texture lands exactly on the
  //    base pitch -- so a decode using the base pitch for every level was
  //    indistinguishable from a correct one.
  //  - At 64 texels wide, TILED proves nothing either. Level 1 is then 32x32, a
  //    single tile, and GetTiledOffset2D only consults the pitch across tile
  //    boundaries. 256 makes level 1 a 4x4 grid of tiles.
  for (int tiled_case = 0; tiled_case < 2; ++tiled_case) {
    namespace xg = rex::graphics::xenos;
    namespace tu = rex::graphics::texture_util;
    const bool is_tiled = tiled_case != 0;
    const char* tag = is_tiled ? "tiled" : "linear";
    constexpr uint32_t kDim = 256, kMaxLevel = 8;
    xg::xe_gpu_texture_fetch_t f{};
    f.type = xg::FetchConstantType::kTexture;
    f.dimension = xg::DataDimension::k2DOrStacked;
    f.format = xg::TextureFormat::k_8_8_8_8;
    f.base_address = 1;
    f.mip_address = 2;
    f.pitch = kDim / 32;
    f.size_2d.width = kDim - 1;
    f.size_2d.height = kDim - 1;
    f.tiled = is_tiled ? 1 : 0;
    f.packed_mips = 1;
    f.mip_max_level = kMaxLevel;
    f.mip_filter = xg::TextureFilter::kLinear;

    HleTextureSource src{};
    Check(DescribeHleTexture2D(reinterpret_cast<const uint32_t*>(&f), src, &why),
          "mip chain descriptor accepted");
    Check(src.level_count == kMaxLevel + 1, "every declared level is planned");
    Check(src.mip_source_bytes != 0, "the chain has an extent");

    const tu::TextureGuestLayout layout = tu::GetGuestTextureLayout(
        f.dimension, f.pitch, kDim, kDim, 1, is_tiled,
        xg::TextureFormat::k_8_8_8_8, /*has_packed_levels=*/true,
        /*has_base=*/true, kMaxLevel);
    // The whole point of the tiled case: these must not be equal, or the case
    // proves nothing.
    if (is_tiled)
      Check(layout.mips[1].row_pitch_bytes / 4 != src.pitch_blocks,
            "the tiled case distinguishes level pitch from base pitch");

    // Paint level n's texels with the value n, where the guest would store them.
    std::vector<uint8_t> guest(size_t(src.source_bytes) + src.mip_source_bytes,
                               0xEE);
    const uint32_t packed_level = layout.packed_level;
    for (uint32_t l = 0; l <= kMaxLevel; ++l) {
      const uint32_t w = std::max(kDim >> l, 1u), h = std::max(kDim >> l, 1u);
      uint32_t px = 0, py = 0, pz = 0;
      size_t at = 0;
      uint32_t pitch_blocks = src.pitch_blocks;
      if (l) {
        const uint32_t storage = std::min(l, packed_level);
        at = size_t(src.source_bytes) + layout.mip_offsets_bytes[storage];
        pitch_blocks = layout.mips[storage].row_pitch_bytes / 4;
        if (l >= packed_level)
          tu::GetPackedMipOffset(kDim, kDim, 1, xg::TextureFormat::k_8_8_8_8, l,
                                 px, py, pz);
      }
      for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
          const size_t texel =
              is_tiled ? size_t(tu::GetTiledOffset2D(px + x, py + y,
                                                     pitch_blocks, 2))
                       : (size_t(py + y) * pitch_blocks + px + x) * 4;
          for (uint32_t c = 0; c < 4; ++c) guest[at + texel + c] = uint8_t(l);
        }
      }
    }

    HleTexturePayload mips;
    Check(DecodeHleTexture2D(src, guest.data(), guest.size(), mips, &why),
          "mip chain decode");
    Check(mips.level_count == kMaxLevel + 1, "every level survived the decode");
    bool geometry = true, content = true;
    for (uint32_t l = 0; l <= kMaxLevel && l < mips.level_count; ++l) {
      const uint32_t w = std::max(kDim >> l, 1u), h = std::max(kDim >> l, 1u);
      const auto& lv = mips.levels[l];
      if (lv.width != w || lv.height != h || lv.rows != h ||
          lv.row_pitch != w * 4)
        geometry = false;
      for (uint32_t y = 0; y < h && content; ++y)
        for (uint32_t x = 0; x < w * 4; ++x)
          if (mips.data[lv.offset + size_t(y) * lv.row_pitch + x] != l) {
            content = false;
            break;
          }
    }
    char msg[96];
    std::snprintf(msg, sizeof(msg), "[%s] each level halves its geometry", tag);
    Check(geometry, msg);
    std::snprintf(msg, sizeof(msg),
                  "[%s] each level reads the bytes stored for it", tag);
    Check(content, msg);
    Check(mips.width == kDim && mips.row_pitch == kDim * 4,
          "the payload still presents level 0 to old consumers");

    // mip_address zero is the guest's "no chain", and it WINS over a non-zero
    // mip_max_level -- 82,000 binds a run take this path.
    xg::xe_gpu_texture_fetch_t none = f;
    none.mip_address = 0;
    HleTextureSource flat{};
    Check(DescribeHleTexture2D(reinterpret_cast<const uint32_t*>(&none), flat,
                               &why) &&
              flat.level_count == 1 && flat.mip_source_bytes == 0,
          "no mip_address means no chain whatever mip_max_level says");

    // kBaseMap is the guest declining the chain it allocated.
    xg::xe_gpu_texture_fetch_t base_map = f;
    base_map.mip_filter = xg::TextureFilter::kBaseMap;
    HleTextureSource bm{};
    Check(DescribeHleTexture2D(reinterpret_cast<const uint32_t*>(&base_map), bm,
                               &why) && bm.level_count == 1,
          "mip_filter kBaseMap suppresses the chain");

    // A short read costs levels, not the texture.
    HleTexturePayload partial;
    Check(DecodeHleTexture2D(src, guest.data(), src.source_bytes + 1, partial,
                             &why) &&
              partial.level_count == 1,
          "an unreadable chain degrades to the base level");
  }

  // A CUBE WITH MIPS. Array slices live INSIDE a level on the guest and outside
  // it on the host, and each level has its own slice stride -- so a decode that
  // reused the base level's stride for the chain would return face 0's bytes
  // for five of six faces, at every level but the first. Nothing in the
  // single-slice case above can see that: it was written after mutating the
  // stride and watching the test pass anyway.
  {
    namespace xg = rex::graphics::xenos;
    namespace tu = rex::graphics::texture_util;
    constexpr uint32_t kDim = 64, kMaxLevel = 6, kFaces = 6;
    xg::xe_gpu_texture_fetch_t f{};
    f.type = xg::FetchConstantType::kTexture;
    f.dimension = xg::DataDimension::kCube;
    f.format = xg::TextureFormat::k_8_8_8_8;
    f.base_address = 1;
    f.mip_address = 2;
    f.pitch = kDim / 32;
    f.size_2d.width = kDim - 1;
    f.size_2d.height = kDim - 1;
    f.size_2d.stack_depth = 5;
    f.tiled = 1;
    f.packed_mips = 1;
    f.mip_max_level = kMaxLevel;
    f.mip_filter = xg::TextureFilter::kLinear;

    HleTextureSource src{};
    Check(DescribeHleTexture2D(reinterpret_cast<const uint32_t*>(&f), src, &why),
          "cube mip descriptor accepted");
    Check(src.array_size == kFaces, "a cube is six faces");
    Check(src.level_count == kMaxLevel + 1, "cube chain is planned");

    const tu::TextureGuestLayout layout = tu::GetGuestTextureLayout(
        f.dimension, f.pitch, kDim, kDim, kFaces, true,
        xg::TextureFormat::k_8_8_8_8, true, true, kMaxLevel);
    Check(layout.mips[1].array_slice_stride_bytes !=
              src.slice_stride_bytes,
          "a level's slice stride differs from the base's");

    // Value encodes BOTH level and face, so a mix-up in either is visible.
    std::vector<uint8_t> guest(size_t(src.source_bytes) + src.mip_source_bytes,
                               0xEE);
    const uint32_t packed_level = layout.packed_level;
    for (uint32_t l = 0; l <= kMaxLevel; ++l) {
      const uint32_t w = std::max(kDim >> l, 1u), h = w;
      uint32_t px = 0, py = 0, pz = 0;
      size_t at = 0, stride = src.slice_stride_bytes;
      uint32_t pitch_blocks = src.pitch_blocks;
      if (l) {
        const uint32_t storage = std::min(l, packed_level);
        at = size_t(src.source_bytes) + layout.mip_offsets_bytes[storage];
        pitch_blocks = layout.mips[storage].row_pitch_bytes / 4;
        stride = layout.mips[storage].array_slice_stride_bytes;
        if (l >= packed_level)
          tu::GetPackedMipOffset(kDim, kDim, 1, xg::TextureFormat::k_8_8_8_8, l,
                                 px, py, pz);
      }
      for (uint32_t s = 0; s < kFaces; ++s)
        for (uint32_t y = 0; y < h; ++y)
          for (uint32_t x = 0; x < w; ++x) {
            const size_t texel = size_t(tu::GetTiledOffset2D(
                px + x, py + y, pitch_blocks, 2));
            for (uint32_t c = 0; c < 4; ++c)
              guest[at + s * stride + texel + c] = uint8_t(l * 16 + s);
          }
    }

    HleTexturePayload cube;
    Check(DecodeHleTexture2D(src, guest.data(), guest.size(), cube, &why),
          "cube mip decode");
    Check(cube.level_count == kMaxLevel + 1,
          "every cube level survived the decode");
    bool faces = true;
    for (uint32_t l = 0; l < cube.level_count && faces; ++l) {
      const auto& lv = cube.levels[l];
      const size_t slice_bytes = size_t(lv.row_pitch) * lv.rows;
      for (uint32_t s = 0; s < kFaces && faces; ++s)
        for (uint32_t y = 0; y < lv.rows && faces; ++y)
          for (uint32_t x = 0; x < lv.row_pitch; ++x)
            if (cube.data[lv.offset + s * slice_bytes +
                          size_t(y) * lv.row_pitch + x] != l * 16 + s) {
              faces = false;
              break;
            }
    }
    Check(faces, "every face of every level reads its own bytes");
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
