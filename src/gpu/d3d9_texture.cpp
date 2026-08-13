#include "gpu/d3d9_texture.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstring>
#include <mutex>

#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/xenos.h>

namespace mx::hle {
namespace {
namespace xenos = rex::graphics::xenos;
namespace tu = rex::graphics::texture_util;

// The 3D tiled address, TRANSCRIBED from xenia-edge
// (src/xenia/gpu/texture_util.cc:473, itself "reconstructed from disassembly of
// XGRAPHICS::TileVolume"), not re-derived and not taken from the rexglue SDK.
//
// The SDK declares GetTiledOffset3D and would link, but its xenos layer is
// known to lag xenia-edge, and there is no way to read its implementation from
// here to check. A wrong address function does not fail loudly -- it produces a
// plausible garbage image -- so the version that can be diffed against its
// source wins.
//
// A tile is 32x32x4 BLOCKS. Note xenos.h:1204: "3D tiled texture slices 0:3 and
// 4:7 are stored separately in memory, in non-overlapping ranges, but
// addressing in 4:7 is different than in 0:3" -- that asymmetry is what
// `offset_outer` and the odd/even split below encode, and it is exactly the
// part a hand-rolled "z * slice_size" guess gets wrong.
int32_t TiledOffset3D(int32_t x, int32_t y, int32_t z, uint32_t pitch,
                      uint32_t height, uint32_t bytes_per_block_log2) {
  constexpr uint32_t kTile = 32;
  pitch = (pitch + (kTile - 1)) & ~(kTile - 1);
  height = (height + (kTile - 1)) & ~(kTile - 1);
  const int32_t macro_outer =
      ((y >> 4) + (z >> 2) * int32_t(height >> 4)) * int32_t(pitch >> 5);
  const int32_t macro =
      ((((x >> 5) + macro_outer) << (bytes_per_block_log2 + 6)) & 0xFFFFFFF)
      << 1;
  const int32_t micro =
      (((x & 7) + ((y & 6) << 2)) << (bytes_per_block_log2 + 6)) >> 6;
  const int32_t offset_outer = ((y >> 3) + (z >> 2)) & 1;
  const int32_t offset1 =
      offset_outer + ((((x >> 3) + (offset_outer << 1)) & 3) << 1);
  const int32_t offset2 = ((macro + (micro & ~15)) << 1) + (micro & 15) +
                          ((z & 3) << (bytes_per_block_log2 + 6)) +
                          ((y & 1) << 4);
  int32_t address = (offset1 & 1) << 3;
  address += (offset2 >> 6) & 7;
  address <<= 3;
  address += offset1 & ~1;
  address <<= 2;
  address += offset2 & ~511;
  address <<= 3;
  address += offset2 & 63;
  return address;
}

// Xenia-edge's own GetTiledOffset2D (src/xenia/gpu/texture_util.cc:455),
// transcribed so the SDK's version can be DIFFED against it rather than trusted.
//
// Every tiled 2D texture in the game -- which is nearly all of them -- is
// addressed by the SDK's tu::GetTiledOffset2D, and nothing has ever checked it
// against anything. The mip self-check cannot: it compares level n against a
// box-filtered level n-1, so an addressing error applied CONSISTENTLY to both
// levels scrambles them identically and still passes. It is a relative check,
// and this is the absolute one it cannot be.
int32_t XeniaTiledOffset2D(int32_t x, int32_t y, uint32_t pitch,
                           uint32_t bytes_per_block_log2) {
  constexpr uint32_t kTile = 32;
  pitch = (pitch + (kTile - 1)) & ~(kTile - 1);
  const int32_t macro = ((x >> 5) + (y >> 5) * int32_t(pitch >> 5))
                        << (bytes_per_block_log2 + 7);
  const int32_t micro = ((x & 7) + ((y & 0xE) << 2)) << bytes_per_block_log2;
  const int32_t offset =
      macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4);
  return ((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) +
         (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F);
}

// One sweep, once per run, comparing the SDK's tiled addressing against the
// transcription above. Silent when they agree; a single line naming the first
// disagreement when they do not.
//
// Cheap enough to be unconditional (a few thousand integer ops at startup, not
// per draw), and it must be unconditional: a mismatch here would mean every
// tiled texture in the game is being read from the wrong bytes, which is not a
// thing to leave behind a diagnostic flag that defaults off.
HleTiledAddressCheck g_tiledCheck;
HleOneDCensus g_oneDCensus;

void VerifyTiledAddressing() {
  static std::once_flag once;
  std::call_once(once, [] {
    uint64_t checked = 0, mismatched = 0;
    int32_t first_x = 0, first_y = 0, first_sdk = 0, first_xenia = 0;
    uint32_t first_pitch = 0, first_bpb = 0;
    // Pitches that are and are not tile-aligned, since internal alignment is
    // exactly the kind of contract that drifts between versions -- xenia-edge's
    // newer texture_address::Tiled2D asserts the caller pre-aligned it, while
    // this entry point still aligns internally.
    for (uint32_t bpb_log2 = 0; bpb_log2 <= 4; ++bpb_log2) {
      for (uint32_t pitch : {32u, 64u, 96u, 128u, 260u, 512u}) {
        for (int32_t y = 0; y < 72; ++y) {
          for (int32_t x = 0; x < 72; ++x) {
            const int32_t sdk = tu::GetTiledOffset2D(x, y, pitch, bpb_log2);
            const int32_t ref = XeniaTiledOffset2D(x, y, pitch, bpb_log2);
            ++checked;
            if (sdk == ref) continue;
            if (!mismatched) {
              first_x = x; first_y = y; first_sdk = sdk; first_xenia = ref;
              first_pitch = pitch; first_bpb = bpb_log2;
            }
            ++mismatched;
          }
        }
      }
    }
    g_tiledCheck.checked = checked;
    g_tiledCheck.mismatched = mismatched;
    g_tiledCheck.first_x = first_x;
    g_tiledCheck.first_y = first_y;
    g_tiledCheck.first_sdk = first_sdk;
    g_tiledCheck.first_reference = first_xenia;
    g_tiledCheck.first_pitch = first_pitch;
    g_tiledCheck.first_bytes_per_block_log2 = first_bpb;
  });
}

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

std::atomic<uint64_t> g_mipDescribed{0};
std::atomic<uint64_t> g_mipWithChain{0};
std::atomic<uint64_t> g_mipLevelsPlanned{0};
std::atomic<uint64_t> g_mipNoAddress{0};
std::atomic<uint64_t> g_mipBaseMap{0};
std::atomic<uint64_t> g_mipMinLevel{0};
std::atomic<uint64_t> g_mipLayoutEmpty{0};
std::atomic<uint64_t> g_mipFilterPoint{0};
std::atomic<uint64_t> g_mipLodBias{0};
std::atomic<uint64_t> g_mipTruncated{0};
std::atomic<uint64_t> g_mipRawAddressSet{0};
std::atomic<uint64_t> g_mipByMaxLevel[16];

// Plan levels 1.. of the guest's mip chain onto out.levels.
//
// The chain is a SEPARATE ALLOCATION at fetch.mip_address, and it is not laid
// out like the base level. Three rules from the reference
// (xenia/gpu/texture_util.cc, the only implementation of the SDK's
// texture_util.h on this machine) that a "halve everything" guess gets wrong:
//
//  - A mip's pitch ignores the fetch constant's pitch entirely. It is
//    max(next_pow2(width) >> level, 1) -- NEXT_POW2, so an 80x260 texture's
//    level 3 derives from 512 >> 3, not from 260 >> 3 -- then aligned to 32
//    blocks, and for linear textures to 256 bytes on top of that.
//  - A level's EXTENT uses a different rule from its stride: plain
//    max(width >> level, 1) on the raw width, no rounding. Aligning the extent
//    up can fault, because titles allocate exactly what they use.
//  - At and beyond the packed level, several levels share ONE image and are
//    sub-rected out of it. GetPackedMipOffset takes the texture's BASE
//    dimensions, never the level's, and it does not reject levels below the
//    tail -- it underflows -- so the caller does the gating.
//
// None of that is derived here: GetGuestTextureLayout returns all of it, and
// this walks its output.
void DescribeHleMipChain(const xenos::xe_gpu_texture_fetch_t& fetch,
                         xenos::TextureFormat format, HleTextureSource& out) {
  g_mipDescribed.fetch_add(1, std::memory_order_relaxed);
  out.mip_address = fetch.mip_address << 12;
  out.mip_min_level = fetch.mip_min_level;
  out.mip_max_level = fetch.mip_max_level;
  out.mip_filter = uint8_t(fetch.mip_filter);
  out.packed_mips = fetch.packed_mips != 0;
  if (fetch.lod_bias) g_mipLodBias.fetch_add(1, std::memory_order_relaxed);
  if (fetch.mip_address)
    g_mipRawAddressSet.fetch_add(1, std::memory_order_relaxed);
  g_mipByMaxLevel[fetch.mip_max_level & 15].fetch_add(
      1, std::memory_order_relaxed);

  // The SDK's own normalisation of the two addresses against the two level
  // bounds. It is the authority on the awkward encodings -- notably that a zero
  // mip_address WINS over a non-zero mip_max_level, which is how this title
  // spells "no chain" for 82,000 binds a run.
  uint32_t base_page = 0, mip_page = 0, mip_min = 0, mip_max = 0;
  tu::GetSubresourcesFromFetchConstant(fetch, nullptr, nullptr, nullptr,
                                       &base_page, &mip_page, &mip_min,
                                       &mip_max);
  if (!mip_page || !mip_max) {
    if (fetch.mip_max_level)
      g_mipNoAddress.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  // kBaseMap is the guest saying it never wants to minify past level 0, so the
  // cheapest way to honour it is to not decode a chain it will not sample.
  //
  // The reference keeps the chain and clamps the SAMPLER instead, because a
  // shader's own tfetch can override the fetch constant's mip filter. Our HLSL
  // emitter does not model that override at all, so the two are equivalent
  // today -- but if it ever does, this has to move back into the sampler.
  static_assert(uint8_t(xenos::TextureFilter::kBaseMap) == kMipFilterBaseMap,
                "the renderer's copy of kBaseMap has drifted from the SDK's");
  if (fetch.mip_filter == xenos::TextureFilter::kBaseMap) {
    g_mipBaseMap.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  if (fetch.mip_filter == xenos::TextureFilter::kPoint)
    g_mipFilterPoint.fetch_add(1, std::memory_order_relaxed);
  // mip_min_level != 0 means the base level is not meant to be sampled at all,
  // and the reference responds by zeroing base_page and turning the level into
  // the sampler's MinLOD. We do neither yet, so the honest thing is to leave
  // such a texture exactly as it decoded before this function existed and count
  // it -- if the counter stays at zero the case does not arise, and if it does
  // not, it gets handled with a real population to size the fix against.
  if (mip_min) {
    g_mipMinLevel.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  const tu::TextureGuestLayout layout = tu::GetGuestTextureLayout(
      fetch.dimension, fetch.pitch, out.width, out.height,
      std::max(out.array_size, 1u), out.tiled, format,
      /*has_packed_levels=*/out.packed_mips, /*has_base=*/true,
      /*max_level=*/mip_max);
  if (!layout.mips_total_extent_bytes) {
    g_mipLayoutEmpty.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  // UINT32_MAX when the texture has no tail, which makes every `level >=
  // packed_level` test below false without a special case.
  const uint32_t packed_level = layout.packed_level;
  uint32_t planned = 1;
  for (uint32_t level = 1; level <= mip_max && level < kMaxTextureLevels;
       ++level) {
    // Levels at or past the tail all live in the tail's storage, so they read
    // the tail's stride and offset and differ only in where inside it they sit.
    const uint32_t storage = std::min(level, packed_level);
    if (storage >= kMaxTextureLevels) break;
    const tu::TextureGuestLayout::Level& src = layout.mips[storage];
    if (!src.row_pitch_bytes) break;

    HleTextureLevel& dst = out.levels[level];
    dst.offset_bytes = out.source_bytes + layout.mip_offsets_bytes[storage];
    dst.pitch_blocks = src.row_pitch_bytes / out.bytes_per_block;
    dst.slice_stride_bytes = src.array_slice_stride_bytes;
    dst.width = std::max(out.width >> level, 1u);
    dst.height = std::max(out.height >> level, 1u);
    dst.width_blocks =
        (dst.width + out.block_width - 1) / out.block_width;
    dst.height_blocks =
        (dst.height + out.block_height - 1) / out.block_height;
    if (level >= packed_level) {
      uint32_t px = 0, py = 0, pz = 0;
      if (tu::GetPackedMipOffset(out.width, out.height, 1, format, level, px,
                                 py, pz)) {
        dst.packed_offset_x_blocks = px;
        dst.packed_offset_y_blocks = py;
      }
    }
    ++planned;
  }
  if (planned <= 1) return;
  out.level_count = planned;
  out.mip_source_bytes = layout.mips_total_extent_bytes;
  g_mipWithChain.fetch_add(1, std::memory_order_relaxed);
  g_mipLevelsPlanned.fetch_add(planned, std::memory_order_relaxed);
}
}  // namespace

HleMipCensus HleMipChainStats() {
  HleMipCensus c;
  c.described = g_mipDescribed.load(std::memory_order_relaxed);
  c.with_chain = g_mipWithChain.load(std::memory_order_relaxed);
  c.levels_planned = g_mipLevelsPlanned.load(std::memory_order_relaxed);
  c.no_address = g_mipNoAddress.load(std::memory_order_relaxed);
  c.suppressed_base_map = g_mipBaseMap.load(std::memory_order_relaxed);
  c.suppressed_min_level = g_mipMinLevel.load(std::memory_order_relaxed);
  c.layout_empty = g_mipLayoutEmpty.load(std::memory_order_relaxed);
  c.mip_filter_point = g_mipFilterPoint.load(std::memory_order_relaxed);
  c.lod_bias_set = g_mipLodBias.load(std::memory_order_relaxed);
  c.truncated = g_mipTruncated.load(std::memory_order_relaxed);
  c.raw_mip_address_set = g_mipRawAddressSet.load(std::memory_order_relaxed);
  for (uint32_t i = 0; i < 16; ++i)
    c.by_max_level[i] = g_mipByMaxLevel[i].load(std::memory_order_relaxed);
  return c;
}

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
  // Level 0 only. This asks whether the guest's backing store was ever written,
  // and the base level answers that; scanning the mip chain as well would cost
  // a third more per bind to reach the same conclusion.
  const size_t base_bytes =
      texture.level_count > 1
          ? std::min<size_t>(texture.levels[1].offset, texture.data.size())
          : texture.data.size();
  const size_t count = std::count_if(
      texture.data.begin(), texture.data.begin() + base_bytes,
      [](uint8_t value) { return value != 0; });
  if (nonzero_bytes) *nonzero_bytes = count;
  return count != 0;
}

HleTiledAddressCheck HleTiledAddressStats() {
  VerifyTiledAddressing();
  return g_tiledCheck;
}

HleOneDCensus HleOneDStats() { return g_oneDCensus; }

bool DescribeHleTexture2D(const uint32_t fetch_words[6],
                          HleTextureSource& out, const char** fail) {
  VerifyTiledAddressing();
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
  if (one_d) {
    ++g_oneDCensus.seen;
    // xenia-edge treats both of these as "completely wrong" for a 1D texture
    // and drops the binding entirely (`texture_cache.cc:1067`, `:1078`), which
    // makes the sampler read zero. We used to carry both flags into the 2D
    // path instead, and neither is harmless there:
    //
    //   tiled -- 2D tiling arranges 32x32 blocks. Applied to a single row it
    //     scatters the row across tiles that hold no data for it.
    //   packed_mips -- GetPackedMipLevel(w, 1) is log2_ceil(1) = 0, so level 0
    //     itself counts as inside the tail and the base level gets a packed
    //     sub-rect offset applied to it. Nothing about a one-row texture has a
    //     packed tail to offset into.
    //
    // The reference's LOG says "ignoring", but its code sets is_invalid_1d and
    // returns with the key still invalid — so the binding is dropped, not the
    // flag. Matching the code, not the message: sample_as_zero tells the caller
    // to bind zero and keep the draw, which is what an invalid key does there.
    // Reading the wrong bytes would be the worse answer, and dropping the whole
    // draw would be worse still.
    if (fetch.tiled) {
      ++g_oneDCensus.tiled;
      out.sample_as_zero = true;
      return reject("1D texture claims tiled storage");
    }
    if (fetch.packed_mips) {
      ++g_oneDCensus.packed;
      out.sample_as_zero = true;
      return reject("1D texture claims packed mips");
    }
    // Not fixed here, only counted. The reference remaps a 1D texture wider
    // than 8192 onto a 2D grid of 8192-wide rows and has the shader convert
    // the coordinate back, because its own key packs width in 13 bits. Our
    // limit is D3D12's, 16384, so the range 8193..16384 works here and is
    // remapped there; only past 16384 are we actually short, and the remap
    // needs a shader-side change to go with it. Counted so the decision is
    // measured rather than argued.
    if (fetch.size_1d.width + 1u > 16384u) {
      ++g_oneDCensus.too_wide;
      // Zero rather than a dropped draw for the same reason as above. This one
      // IS a divergence from the reference, which would render it; we do not
      // have the remap yet, so the honest failure is the defined value.
      out.sample_as_zero = true;
      return reject("1D texture wider than the host limit");
    }
    if (fetch.size_1d.width + 1u > 8192u) ++g_oneDCensus.wide;
  }
  // A TRUE VOLUME. Its slices interleave inside the tile rather than sitting a
  // stride apart, which is the one thing the paragraph above says needs a
  // resource type we do not build -- but only the ADDRESSING differs. Decoded
  // slice by slice into the same tightly-packed output and bound as the same
  // Texture2DArray, so nothing downstream changes; see HleTextureSource::volume.
  //
  // Refusing this was the red gameplay screen. Guest pixel shader 0x216012A0 is
  // a full-screen pass with three samplers, one of them a volume; the refusal
  // failed its slot fill, which reset an already-translated shader, which sent
  // the draw to the vertex-colour stand-in, which painted flat red over a
  // correctly tonemapped frame ([[red-screen-is-standin-overpaint]]).
  out.volume = fetch.dimension == xenos::DataDimension::k3D;
  if (cube) {
    // size_2d.stack_depth is documented as 5 for a cube but "not very
    // meaningful"; a cube has six faces by definition, so do not read it.
    out.array_size = 6;
  } else if (out.volume) {
    // A volume's extents live in their OWN union member -- size_3d is 11/11/10
    // bits where size_2d is 13/13/6 -- so reading it the 2D way would truncate
    // the width and read the depth out of the height's high bits. The same trap
    // size_1d already documents above.
    out.array_size = fetch.size_3d.depth + 1;
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

  out.width = (one_d      ? fetch.size_1d.width
               : out.volume ? fetch.size_3d.width
                            : fetch.size_2d.width) +
              1;
  out.height = one_d        ? 1u
               : out.volume ? fetch.size_3d.height + 1
                            : fetch.size_2d.height + 1;
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
    if (!layout.base.level_data_extent_bytes)
      return reject("array texture layout is empty");
    // A VOLUME has no slice stride to demand -- its slices interleave inside
    // the tile, so the layout reports one extent covering all of them and
    // array_slice_stride_bytes is meaningless. Requiring it here was what made
    // the whole branch reject volumes even after the dimension gate opened.
    if (!out.volume) {
      if (!layout.base.array_slice_stride_bytes)
        return reject("array texture layout is empty");
      out.slice_stride_bytes = layout.base.array_slice_stride_bytes;
    }
    out.source_bytes = layout.base.level_data_extent_bytes;
  }
  if (!out.source_bytes || out.source_bytes > 256u * 1024u * 1024u)
    return reject("texture extent out of range");

  DescribeHleMipChain(fetch, format, out);
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
  const uint32_t slices = std::max(source.array_size, 1u);
  const uint32_t planned = std::clamp(source.level_count, 1u,
                                      kMaxTextureLevels);

  // Where each level lands in the output. The host wants every subresource
  // TIGHTLY packed -- that is what GetCopyableFootprints will be handed -- which
  // is not how the guest stores anything: guest slices are 4 KB-aligned, levels
  // sit at their own offsets in a second allocation, and both may be tiled. So
  // the gaps are source-side strides on the way in and nothing on the way out.
  //
  // Level-major, slices inside each level, matching the guest's own nesting.
  // D3D12 nests the other way round (subresource = mip + slice * MipLevels), so
  // the upload walks this table rather than striding through the buffer.
  //
  // Level 0 is assembled from the flat fields, which are the only description
  // of it -- source.levels[0] is unused on purpose, so a source built by hand
  // still decodes and the base geometry has exactly one statement.
  HleTextureLevel geo[kMaxTextureLevels] = {};
  geo[0].pitch_blocks = source.pitch_blocks;
  geo[0].slice_stride_bytes = source.slice_stride_bytes;
  geo[0].width = source.width;
  geo[0].height = source.height;
  geo[0].width_blocks =
      (source.width + source.block_width - 1) / source.block_width;
  geo[0].height_blocks =
      (source.height + source.block_height - 1) / source.block_height;
  geo[0].packed_offset_x_blocks = source.packed_offset_x_blocks;
  geo[0].packed_offset_y_blocks = source.packed_offset_y_blocks;
  for (uint32_t l = 1; l < planned; ++l) geo[l] = source.levels[l];

  HleTextureLevelData plan[kMaxTextureLevels] = {};
  uint64_t tight = 0;
  for (uint32_t l = 0; l < planned; ++l) {
    const HleTextureLevel& lv = geo[l];
    if (!lv.width_blocks || !lv.height_blocks)
      return reject("texture level is empty");
    plan[l].offset = uint32_t(tight);
    plan[l].row_pitch = lv.width_blocks * source.bytes_per_block;
    plan[l].rows = lv.height_blocks;
    plan[l].width = lv.width;
    plan[l].height = lv.height;
    tight += uint64_t(plan[l].row_pitch) * lv.height_blocks * slices;
    if (tight > UINT32_MAX) return reject("decoded extent overflow");
  }
  if (!tight) return reject("decoded extent overflow");

  out = {};
  out.width = source.width;
  out.height = source.height;
  out.array_size = slices;
  out.row_pitch = plan[0].row_pitch;
  out.format = source.host_format;
  out.swizzle = source.swizzle;
  out.clamp_x = uint8_t(source.clamp_x);
  out.clamp_y = uint8_t(source.clamp_y);
  out.linear_filter = source.linear_filter;
  out.mip_filter = source.mip_filter;
  out.data.resize(size_t(tight));

  const auto endian = static_cast<xenos::Endian>(source.endian);
  const uint32_t bpb_log2 = std::bit_width(source.bytes_per_block) - 1;
  uint32_t decoded = planned;
  for (uint32_t l = 0; l < planned; ++l) {
    // Every address rule that differs between the base level and a mip is
    // already resolved into this entry, so the loop below is the one it always
    // was: pitch, packed displacement and extent just come from the level
    // rather than from the texture.
    const HleTextureLevel& lv = geo[l];
    const uint64_t slice_tight = uint64_t(plan[l].row_pitch) * plan[l].rows;
    bool level_ok = true;
    for (uint32_t slice = 0; slice < slices && level_ok; ++slice) {
      // A TILED volume interleaves its slices inside the tile, so z goes into
      // the address function and the base must NOT advance -- its
      // slice_stride_bytes is 0 for exactly that reason. UNTILED, there is no
      // interleaving and the slices are plain consecutive planes, which the
      // linear formula below cannot express because it never sees z.
      const uint64_t volume_plane =
          (source.volume && !source.tiled)
              ? uint64_t(lv.pitch_blocks) * lv.height_blocks *
                    source.bytes_per_block
              : 0;
      const uint64_t slice_base = uint64_t(lv.offset_bytes) +
                                  uint64_t(slice) * lv.slice_stride_bytes +
                                  uint64_t(slice) * volume_plane;
      uint8_t* slice_out =
          out.data.data() + plan[l].offset + uint64_t(slice) * slice_tight;
      for (uint32_t y = 0; y < lv.height_blocks && level_ok; ++y) {
        for (uint32_t x = 0; x < lv.width_blocks; ++x) {
          // Read coordinates, which are the write coordinates displaced into a
          // packed mip tail. Both zero unless this level is packed -- the base
          // level of a texture 16 texels or smaller, or any level at or beyond
          // the packed level, all of which share one tail image.
          const uint32_t sx = x + lv.packed_offset_x_blocks;
          const uint32_t sy = y + lv.packed_offset_y_blocks;
          // A volume addresses by (x, y, z) into one interleaved allocation; a
          // stack or cube addresses by (x, y) into a slice that slice_base has
          // already displaced. Untiled is the same linear formula either way,
          // with the slice reached through slice_base as before.
          const uint64_t src =
              slice_base +
              (source.tiled
                   ? (source.volume
                          ? uint64_t(TiledOffset3D(int32_t(sx), int32_t(sy),
                                                   int32_t(slice),
                                                   lv.pitch_blocks,
                                                   lv.height_blocks, bpb_log2))
                          : uint64_t(tu::GetTiledOffset2D(sx, sy,
                                                          lv.pitch_blocks,
                                                          bpb_log2)))
                   : (uint64_t(sy) * lv.pitch_blocks + sx) *
                         source.bytes_per_block);
          if (src + source.bytes_per_block > guest_bytes) {
            level_ok = false;
            break;
          }
          uint8_t* dst = slice_out +
              (uint64_t(y) * lv.width_blocks + x) * source.bytes_per_block;
          std::memcpy(dst, guest + src, source.bytes_per_block);
          SwapBlock(dst, source.bytes_per_block, endian);
        }
      }
    }
    if (level_ok) continue;
    // Level 0 is the texture; without it there is nothing to bind. A mip level
    // that runs off the end is a different matter -- the chain lives in its own
    // allocation, which may be shorter than the layout says or only partly
    // resident, and serving fewer levels beats serving none.
    if (l == 0) return reject("tiled block outside source");
    decoded = l;
    out.data.resize(plan[l].offset);
    g_mipTruncated.fetch_add(1, std::memory_order_relaxed);
    break;
  }
  out.level_count = decoded;
  for (uint32_t l = 0; l < decoded; ++l) out.levels[l] = plan[l];

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
