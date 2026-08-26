// The texture and pixel-binding half of the D3D9 HLE: the guest texture cache,
// the glyph-cache special case, blank/swizzle/sign/mip censuses, the Bink plane
// path, pixel slot resolution and PrepareDrawTexture.
//
// Split verbatim out of hooks_d3d9.cpp. Nothing was renamed or reordered. The
// shared surface is hooks_d3d9_shared.h, and it is small on purpose -- see the
// note there about why the counters were grouped into structs first.

#include "hooks/hook_common.h"

#include <rex/cvar.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>
#include <fstream>

// For the emitter coverage probe only: emitting HLSL the compiler then rejects
// is exactly as useless as refusing to emit, so the probe compiles what it
// emits. Nothing else in this file touches D3D.
#include <d3dcompiler.h>
#include <wrl/client.h>

// For the vfetch destination swizzle, so the GPU vertex path merges attributes
// into registers by exactly the rule shader_alu.cpp seeds its register file
// with. Two decoders of the same field that disagree is the bug that decoder
// exists to prevent.
#include <rex/graphics/format/ucode.h>

#include "gpu/d3d9_draw.h"
#include "gpu/d3d9_layout.h"
#include "gpu/d3d9_texture.h"
#include "gpu/shader_ucode.h"   // DecodeVertexShaderFetches, VertexAttribute
#include "gpu/shader_alu.h"     // ExecuteVertexShader
#include "gpu/shader_hlsl.h"    // EmitShaderHlsl
#include <cmath>
#include "gpu/d3d9_state.h"
#include "gpu/hle_types.h"      // g_luminanceReadbackBits/Seq
#include "gpu/xenos_gpu_state.h"  // mx::gpu::alu â€” the PM4 ALU constant file
#include "hooks/hooks_d3d9_shared.h"
#include "hooks/texture_dump.h"         // --texture_dump=true, logs/texdump
namespace mx::hooks::d3d9 {

struct ResolvedPixelBinding {
  std::vector<mx::hle::PixelTextureBinding> bindings;
  const char* fail = nullptr;
  uint32_t code_offset_dwords = 0;
  bool decoded = false;
};
std::map<uint32_t, ResolvedPixelBinding> g_resolvedPixelBindings;
std::map<uint64_t, std::shared_ptr<const mx::hle::HleTexturePayload>>
    g_hleCpuTextures;
// Keys whose decode came out entirely zero. This used to be a set-and-forget
// flag, which made "blank" permanent: a texture sampled once while the guest
// was still streaming into it could never be reconsidered, because the key
// hashes the six fetch dwords -- where the texture lives and what shape it is
// -- and never its contents.
//
// Retrying is what lets a streamed texture appear, but it cannot be free.
// Measured on the attract sequence, the blank set is three FMT_8_8_8_8
// surfaces, one 2048x2048 and two at 1280x720 -- the game's render resolution,
// so they are surfaces the GPU rendered into whose guest copy is legitimately
// empty and will never fill in. Re-untiling ~9 MB of those every frame forever
// is pure waste, so each retry that comes back blank doubles the wait before
// the next one, up to a cap. A texture that is about to arrive is retried
// almost immediately; one that never arrives settles into costing nothing.
struct BlankState {
  uint64_t last_frame = 0;
  uint32_t strikes = 0;
};
std::map<uint64_t, BlankState> g_hleEmptyTextures;

// How many frames to wait after `strikes` consecutive blank decodes.
uint64_t BlankRetryDelay(uint32_t strikes) {
  constexpr uint32_t kMaxShift = 7;  // 128 frames, ~2s at 60fps
  return uint64_t(1) << std::min(strikes, kMaxShift);
}

// True when a key found blank before is due another look. Unknown keys are due
// by definition -- they have not been tried.
bool BlankRetryDue(uint64_t key) {
  auto it = g_hleEmptyTextures.find(key);
  if (it == g_hleEmptyTextures.end()) return true;
  const uint64_t now = mx::hle::D3D9FrameCount();
  return now >= it->second.last_frame + BlankRetryDelay(it->second.strikes);
}


//===========================================================================
// Scaleform's raster glyph cache, and why a texture cache keyed on the fetch
// constant cannot see it change.
//
// The UI is Scaleform GFx 3.x ("Warning: Increase raster glyph cache capacity
// - TextureConfig." at 0x820D98D8). It keeps ONE 512x512 FMT_8 atlas per font
// and repacks it at runtime as strings appear and disappear -- dumping the
// decoded payload three times in one run caught it holding "Loading" /
// "Press START", then "PHOENIX...", then nearly empty mid-rewrite.
//
// The guest side, from the IDB:
//
//   sub_8293E720  rasterises one glyph into the cache. It writes rows straight
//                 into the cache buffer -- `sub_82BDB3C0(row, 0, w)` to clear
//                 and `sub_82BDAAF0(dst, src, w)` to copy, addressed as
//                 `(y)*tex[5] + tex[6]` where tex = *(cache+696), [5] is the
//                 pitch and [6] the base -- then records a dirty rect through
//                 sub_8293DA08.
//   sub_8293C778  FLUSHES those rects: it walks the texture slots at +56
//                 (stride 5, matching sub_8293A888's `cache[5*i + 14]`),
//                 gathers the rects belonging to each, calls the texture's
//                 vtable slot 3 -- GTexture::Update(level, n, rects, image) --
//                 and then clears the count at +28.
//
// So sub_8293C778 is the exact moment the atlas contents change, and the
// pending-rect count at +28 says whether this call will change anything. That
// is the signal, and it costs nothing on frames where no glyph moved -- which
// is why it is worth decompiling for rather than hashing every texture every
// frame.
//
// It does NOT say which host texture changed, only that the glyph atlases did,
// so the invalidation has to name them some other way.
//
// It used to name them by FORMAT alone -- every cached kR8 texture -- on the
// stated grounds that "the only other kR8 textures in a run are two 32x32
// ones". Measured 2026-08-16, that is wrong by three orders of magnitude. The
// kR8 population in a loaded pause frame is 5.00 MB: four 512x512 glyph atlases
// and one 2048x2048 that is not a glyph atlas at all (30 binds, swizzle
// 0o05000). In the menu and event captures it also sweeps in the Bink Y/U/V
// planes (640x216, 320x108 x2), a 1024x512 and two 512x256. Every one of those
// re-decoded on every flush, and mx_1189 alone logged 8 flushes.
//
// Worse than the waste: routing a texture here ALSO routes it away from
// GuestTextureFingerprint, so those same non-glyph textures were never
// content-checked at all. A 2048x2048 R8 restreamed without a glyph flush was
// invisible to us.
//
// So name them by GEOMETRY, learned from the cache object rather than assumed.
// sub_8293A888 creates each atlas with InitTexture(cache[0], cache[1], ...), so
// the flush hook reads those two dwords and registers the pair here. A kR8
// texture is a glyph atlas only if its extent matches one the guest actually
// built; everything else falls through to the fingerprint, which is the right
// test for it and the test it should have been getting all along.
//
// Before the first flush the set is empty, so a glyph atlas decoded that early
// stores a fingerprint. Once the geometry registers it compares against the
// generation instead, mismatches once, and re-decodes into the right regime.
// Self-correcting, and it costs one decode.
//===========================================================================
// Counted in both modes -- see the note in hooks_d3d9_internal.h.
std::atomic<uint64_t> g_guestDrawCalls{0};

uint32_t g_glyphCacheGeneration = 1;
uint64_t g_glyphCacheFlushes = 0;

// g_glyphCacheFlushes only moves when the flush carried rects, so on its own it
// cannot tell "the flush never ran" from "it ran with nothing pending". Those
// are completely different findings -- the first says the guest never reaches
// the upload at all, the second says the atlas is simply already warm -- and a
// freeroam run showing zero flushes is unreadable without this pair.
// See [[counter-that-cannot-fire]]: a counter you cannot distinguish a zero in
// is not a measurement.
uint64_t g_glyphFlushCalls = 0;   // EVERY call, before the pending test
uint64_t g_glyphFlushEmpty = 0;   // of those, the ones with 0 rects pending
uint64_t g_glyphFlushRects = 0;   // total rects uploaded

// sub_8293A888 is GetTexture: it hands back the atlas texture for a slot,
// creating it through OUR renderer's vtable on first use. It is the one refusal
// point in the glyph chain that runs through our code rather than the guest's.
//
// A failure there is not recoverable and not retried. sub_8293C778 clears the
// slot's dirty flag OUTSIDE the success test:
//
//     if (v5[4]) { if (sub_8293A888(...)) { ...Update... } v5[4] = 0; }
//
// so a failed create silently DISCARDS that slot's pending rects for the life
// of the cache. That is the exact shape of "some letters never appear".
uint64_t g_glyphGetTextureCalls = 0;
uint64_t g_glyphGetTextureFailed = 0;

// Tiny -- one entry per distinct atlas geometry, which is one or two. The
// atomic is the fast path: the flush hook runs once per guest DrawText, and the
// geometry is the same on essentially every call, so the lock is taken only
// when a genuinely new one appears.
std::mutex g_glyphGeometryMu;
std::set<uint64_t> g_glyphGeometries;
std::atomic<uint64_t> g_glyphGeometryLast{0};

uint64_t GlyphGeometryKey(uint32_t width, uint32_t height) {
  return (uint64_t(width) << 32) | height;
}

void NoteGlyphCacheGeometry(uint32_t width, uint32_t height) {
  if (!width || !height || width > 8192 || height > 8192) return;
  const uint64_t key = GlyphGeometryKey(width, height);
  if (g_glyphGeometryLast.load(std::memory_order_relaxed) == key) return;
  {
    std::lock_guard<std::mutex> lk(g_glyphGeometryMu);
    g_glyphGeometries.insert(key);
  }
  g_glyphGeometryLast.store(key, std::memory_order_relaxed);
}

bool IsGlyphCacheTexture(mx::hle::HostTextureFormat format, uint32_t width,
                         uint32_t height) {
  if (format != mx::hle::HostTextureFormat::kR8) return false;
  const uint64_t key = GlyphGeometryKey(width, height);
  if (g_glyphGeometryLast.load(std::memory_order_relaxed) == key) return true;
  std::lock_guard<std::mutex> lk(g_glyphGeometryMu);
  return g_glyphGeometries.contains(key);
}

// Fingerprint of the GUEST BYTES behind a texture, so the caches can notice
// that an address has been refilled with different artwork.
//
// The cache key is FNV-1a over the six fetch dwords -- where the texture lives
// and what shape it is, never what it contains. Swapping riders streams new
// gear into the SAME allocation at the same dimensions and format, so the key
// does not change and BOTH caches keep serving the previous rider: the decoded
// payload in g_hleCpuTextures (whose emplace never overwrites) and the GPU
// resource in m_gameTextures. That is the wrong-livery and wrong-gear defect,
// and it is why it looked order-dependent -- the only thing that ever
// invalidated anything was a Scaleform font repack, which is unrelated and
// happened to fire sometimes.
//
// Bounded so it can run on every bind. Textures of 4 KB or less are hashed
// WHOLE; larger ones are sampled at 32 fixed offsets, ~2 KB against the ~580
// binds a frame this title makes. Hashing everything in full would be ~100 MB
// a frame. The sampled form could in principle miss artwork that is
// byte-identical at all 32 offsets; the whole-hash cutoff covers the small
// textures where that is most plausible, and for real art it does not happen.
//
// Returns 0 for memory it cannot read, which callers treat as "no opinion"
// rather than as a change -- a texture mid-stream must not be invalidated on
// the strength of a failed read.
uint32_t GuestTextureFingerprint(const mx::hle::HleTextureSource& source,
                                 uint8_t* base) {
  const uint32_t bytes = source.source_bytes;
  if (!source.address || !bytes) return 0;

  // The bare address is often not the readable one; walk the same mirrors
  // CopyTexturePhysical does.
  uint32_t addr = 0;
  for (uint32_t m : {0u, 0xA0000000u, 0xC0000000u, 0xE0000000u}) {
    const uint32_t candidate = source.address | m;
    if (HostPageReadable(REX_RAW_ADDR(candidate))) {
      addr = candidate;
      break;
    }
  }
  if (!addr) return 0;

  uint64_t h = 1469598103934665603ull;
  // Length participates, so the same address resized is a different
  // fingerprint even when its opening bytes agree.
  h ^= bytes;
  h *= 1099511628211ull;
  // So does where the mip chain points. Only the base level's bytes are
  // sampled below -- that is the discriminator for a rider swap, which streams
  // new artwork into the same slot -- but a texture that keeps its base and
  // repoints its chain has still changed, and this catches it for free.
  h ^= source.mip_address;
  h *= 1099511628211ull;

  bool ok = true;
  const auto eat = [&](uint32_t offset, uint32_t n) {
    if (!ok) return;
    // Checked per slice rather than once at each end: the pages between are
    // not guaranteed mapped, and a fingerprint is not worth a fault.
    if (!HostPageReadable(REX_RAW_ADDR(addr + offset)) ||
        !HostPageReadable(REX_RAW_ADDR(addr + offset + n - 1))) {
      ok = false;
      return;
    }
    const auto* q = reinterpret_cast<const uint8_t*>(REX_RAW_ADDR(addr + offset));
    for (uint32_t i = 0; i < n; ++i) {
      h ^= q[i];
      h *= 1099511628211ull;
    }
  };

  constexpr uint32_t kWholeHashLimit = 4096;
  constexpr uint32_t kSlices = 32, kSliceBytes = 64;
  if (bytes <= kWholeHashLimit) {
    eat(0, bytes);
  } else {
    for (uint32_t i = 0; i < kSlices && ok; ++i)
      eat(uint32_t((uint64_t(bytes - kSliceBytes) * i) / (kSlices - 1)),
          kSliceBytes);
  }
  if (!ok) return 0;
  const uint32_t folded = uint32_t(h ^ (h >> 32));
  return folded ? folded : 1u;  // 0 is reserved for "could not read".
}

// What a payload's content_version should hold, and what it is later compared
// against. One function so the store and the test cannot drift apart -- storing
// a fingerprint and comparing it to a generation would invalidate that texture
// on every single bind.
//
// The glyph atlas KEEPS the guest's own flush generation rather than moving to
// the fingerprint. That fix was hard won, the guest tells us outright when the
// atlas is repacked, and an explicit signal beats a sampled read of the same
// memory -- the fingerprint samples 2 KB of a 256 KB atlas, so a localised
// glyph write lands between its sample points and reads as unchanged. That is
// what broke the pause HUD once already.
//
// The fingerprint covers everything else, which until now was covered by
// nothing at all: GlyphCacheStale was gated on IsGlyphCacheFormat, so every
// BC1/BC3/BC5/RGBA8 texture in the game -- all the rider and vehicle art -- was
// never tested for staleness in the first place. As of the geometry test above
// that "everything else" correctly includes the single-channel textures that
// are NOT glyph atlases, which the format-only gate had also been excluding.
uint32_t TextureContentVersion(const mx::hle::HleTextureSource& source,
                               uint8_t* base,
                               mx::hle::HostTextureFormat format) {
  if (IsGlyphCacheTexture(format, source.width, source.height))
    return g_glyphCacheGeneration;
  return GuestTextureFingerprint(source, base);
}

// True when a cached payload no longer matches what the guest memory holds.
bool TextureContentStale(const mx::hle::HleTextureSource& source,
                         uint8_t* base,
                         const mx::hle::HleTexturePayload& payload) {
  const uint32_t now = TextureContentVersion(source, base, payload.format);
  // A fingerprint of 0 means the memory could not be read. Not evidence of a
  // change, so the cached copy stands; the generation is never 0.
  if (!IsGlyphCacheTexture(payload.format, source.width, source.height) && !now)
    return false;
  return now != payload.content_version;
}

void NoteBlankDecode(uint64_t key) {
  BlankState& s = g_hleEmptyTextures[key];
  s.last_frame = mx::hle::D3D9FrameCount();
  ++s.strikes;
}
// The blank payload itself, so the draws that sample a still-blank key within
// one frame share a decode instead of repeating it.
std::map<uint64_t, std::shared_ptr<const mx::hle::HleTexturePayload>>
    g_hleBlankPayloads;

const ResolvedPixelBinding* ResolvePixelProfile(uint32_t handle) {
  // This used to search every PM4-captured pixel shader for a byte match inside
  // the D3D9 allocation, to locate where the CF stream began. CollectPixelShaderBlob
  // now reads that offset out of the shader object itself, the same field
  // sub_82565928 reads when it programs the hardware, so the search has nothing
  // left to find and the resolve is final on the first try.
  auto known = g_resolvedPixelBindings.find(handle);
  if (known != g_resolvedPixelBindings.end()) return &known->second;
  auto bi = g_patch.psBlobs.find(handle);
  if (bi == g_patch.psBlobs.end()) return nullptr;

  ResolvedPixelBinding resolved;
  const uint32_t* code = bi->second.data();
  uint32_t code_count = uint32_t(bi->second.size());
  // Applied here rather than at startup: this is the first point every run
  // reaches before any shader is emitted, and the emitter has no cvar access.
  ReportHlslCoverage(mx::hle::HlslStage::kPixel, handle, code, code_count);
  resolved.decoded = mx::hle::DecodePixelTextureFetches(
      code, code_count, resolved.bindings, &resolved.fail);
  if (!resolved.decoded) {
    // Pixel shader allocations with literal constants place those values in
    // front of the CF stream (the loaded main-pass shaders consistently begin
    // at dword 16). Do not hardcode that observation: try a bounded set of
    // suffixes and accept only a unique valid decode. A second valid alignment
    // makes the blob ambiguous and leaves the draw on the colour fallback.
    uint32_t valid_offsets = 0;
    std::vector<mx::hle::PixelTextureBinding> unique_bindings;
    uint32_t unique_offset = 0;
    const uint32_t limit =
        std::min<uint32_t>(64, uint32_t(bi->second.size()));
    for (uint32_t offset = 1; offset + 3 <= limit; ++offset) {
      std::vector<mx::hle::PixelTextureBinding> candidate;
      const char* candidate_fail = nullptr;
      if (!mx::hle::DecodePixelTextureFetches(
              bi->second.data() + offset,
              uint32_t(bi->second.size()) - offset, candidate,
              &candidate_fail))
        continue;
      ++valid_offsets;
      unique_offset = offset;
      unique_bindings = std::move(candidate);
    }
    if (valid_offsets == 1) {
      resolved.decoded = true;
      resolved.fail = nullptr;
      resolved.code_offset_dwords = unique_offset;
      resolved.bindings = std::move(unique_bindings);
    } else if (valid_offsets > 1) {
      resolved.fail = "ambiguous CF offset in D3D9 allocation";
    }
  }
  g_resolvedPixelBindings[handle] = std::move(resolved);
  auto& profile = g_resolvedPixelBindings[handle];
  std::string linkage;
  for (const auto& b : profile.bindings) {
    linkage += fmt::format(" s{}<-r{}{}", b.sampler, b.src_reg,
                           b.unnormalized ? "(unnorm)" : "");
  }
  REXLOG_INFO("d3d9: pixel shader 0x{:08X} texture profile: {}{}{}; source {}",
              handle,
              profile.decoded
                  ? fmt::format("{} 2D fetch(es)", profile.bindings.size())
                  : "rejected",
              linkage,
              profile.decoded
                  ? ""
                  : fmt::format(" ({})", profile.fail ? profile.fail : "?"),
              profile.code_offset_dwords
                  ? fmt::format("unique CF suffix at blob+0x{:X}",
                                profile.code_offset_dwords * 4)
                  : fmt::format("whole {}-dword D3D9 allocation",
                                bi->second.size()));
  if (!profile.decoded) {
    static std::map<uint32_t, bool> s_dumped_rejected;
    if (s_dumped_rejected.size() < 16 &&
        s_dumped_rejected.emplace(handle, true).second) {
      std::string words;
      uint32_t shown = 0;
      for (uint32_t i = 0; i < bi->second.size() && shown < 16; ++i) {
        if (!bi->second[i]) continue;
        words += fmt::format(" [{}]={:08X}", i, bi->second[i]);
        ++shown;
      }
      REXLOG_INFO("d3d9: rejected pixel shader 0x{:08X} first nonzero "
                  "allocation dwords:{}",
                  handle, words.empty() ? " none" : words);
    }
  }
  return &profile;
}

bool ResolvePixelBinding(uint32_t handle,
                         mx::hle::PixelTextureBinding& out) {
  const ResolvedPixelBinding* profile = ResolvePixelProfile(handle);
  if (!profile || !profile->decoded || profile->bindings.empty())
    return false;
  if (profile->bindings.size() == 1) {
    out = profile->bindings.front();
    return true;
  }

  // Multiple fetch instructions may still describe one host texture. Blur
  // passes in ST_Southwest issue 3 or 9 taps of s0 from the same interpolator;
  // base-mip HLE cannot reproduce their offsets/ALU yet, but one s0 sample is
  // the explicit approximation this milestone permits.
  const auto& first = profile->bindings.front();
  bool same_linkage = true;
  bool same_interpolator = true;
  bool same_sampler = true;
  for (const auto& b : profile->bindings) {
    same_interpolator = same_interpolator && b.src_reg == first.src_reg;
    same_linkage = same_linkage && b.src_reg == first.src_reg &&
                   b.src_swizzle == first.src_swizzle &&
                   b.unnormalized == first.unnormalized;
    same_sampler = same_sampler && b.sampler == first.sampler;
  }
  if (same_linkage && same_sampler) {
    out = first;
    return true;
  }

  // Evidence-selected final compositor profile, measured on the 1280x720
  // ST_Southwest draw: s0 is the resolved 1280x720 scene, s1 is 160x90, s2 is
  // 1x1 and s3 is another full-size input. Select only the base scene and only
  // for this exact fetch order/linkage; do not generalise "sampler zero wins"
  // to unrelated shaders.
  static constexpr uint32_t kFinalSamplers[4] = {3, 1, 2, 0};
  if (same_interpolator &&
      profile->bindings.size() == std::size(kFinalSamplers)) {
    bool exact = true;
    for (uint32_t i = 0; i < std::size(kFinalSamplers); ++i)
      exact = exact && profile->bindings[i].sampler == kFinalSamplers[i];
    if (exact) {
      out = profile->bindings.back();  // s0, the resolved 1280x720 scene.
      static bool s_logged = false;
      if (!s_logged) {
        s_logged = true;
        REXLOG_INFO("d3d9: selected s0 base scene for observed "
                    "four-input final compositor shader 0x{:08X}", handle);
      }
      return true;
    }
  }
  return false;
}

// Texture fetch constants embedded in a shader object's state-patch list.
//
// SetPixelShader and SetVertexShader both walk the same three-part block. The
// first list emits LOAD_ALU_CONSTANT packets. The SECOND list is different:
// each entry is `(u16 byte_offset, u16 dword_count, inline payload)` and the
// guest copies that payload to `device + 0x480 + byte_offset`. The first 0xC0
// bytes of that device block are the 32 six-dword texture fetch constants.
//
// This distinction is verified directly in the recompiled guest functions
// sub_825506E8 and sub_825508A8. An earlier implementation searched the first
// list for ALU register indexes reaching 0x4800; no shader published such an
// entry, so the runtime correctly reported zero captured descriptors.
//
// Cached per pixel-shader handle rather than re-walked per draw. The pixel and
// vertex shader patch lists are merged because either may carry state for the
// shared device constants block used by the draw.
struct ShaderFetchConstants {
  static constexpr uint32_t kDwords = 6;
  uint32_t words[mx::hle::kMaxSamplers * kDwords] = {};
  // Which of the six dwords of each sampler have arrived. A descriptor can be
  // split across entries, and the two halves are only usable together.
  uint8_t partial[mx::hle::kMaxSamplers] = {};
  // Bit s set = ALL SIX dwords of sampler s arrived. A partial publish is not
  // usable -- DescribeHleTexture2D reads all six and would describe a texture
  // out of half a descriptor and half zeros, which is worse than reporting the
  // slot unbound.
  uint32_t complete_mask = 0;
};
std::mutex g_shaderFetchMu;
std::map<uint32_t, ShaderFetchConstants> g_shaderFetch;
// Slot fills served from a shader-embedded descriptor rather than the device
// shadow. Reported beside the unbound-sampler counts, because these two are the
// same population before and after the fix and only mean something together.
std::atomic<uint64_t> g_shaderFetchServed{0};
std::atomic<uint64_t> g_shaderFetchPublished{0};

// Copy sampler `sampler`'s published fetch constant, if this shader published a
// complete one. Returns false otherwise, leaving `out` untouched.
bool ShaderPublishedFetch(uint32_t shader, uint32_t sampler, uint32_t out[6]) {
  if (!shader || sampler >= mx::hle::kMaxSamplers) return false;
  std::lock_guard<std::mutex> lock(g_shaderFetchMu);
  const auto it = g_shaderFetch.find(shader);
  if (it == g_shaderFetch.end()) return false;
  if (!(it->second.complete_mask & (1u << sampler))) return false;
  std::memcpy(out, &it->second.words[sampler * ShaderFetchConstants::kDwords],
              sizeof(uint32_t) * 6);
  return true;
}

// `ps_handle`, when given, is the pixel shader whose load table may carry this
// sampler's descriptor. Pass the PER-DEVICE handle (PixelShaderForDeviceStrict),
// never the thread-local one: DeviceState() is thread_local and a worker-thread
// draw would name the wrong shader, which here would mean binding another
// draw's texture rather than merely missing one.
bool ReadLiveTextureFetch(uint32_t device, uint8_t* base, uint32_t sampler,
                          uint32_t out[6], uint32_t ps_handle = 0) {
  if (!out || sampler >= mx::hle::kMaxSamplers) return false;
  std::memset(out, 0, sizeof(uint32_t) * 6);
  const uint32_t fetch_at = device + 0x480 + sampler * 24;
  if (device && HostPageReadable(REX_RAW_ADDR(fetch_at)) &&
      HostPageReadable(REX_RAW_ADDR(fetch_at + 20))) {
    for (uint32_t i = 0; i < 6; ++i)
      out[i] = REX_LOAD_U32(fetch_at + i * 4);
    // FetchConstantType::kTexture == 2 (SDK rex/graphics/xenos.h:1093-1098).
    if ((out[0] & 3u) == 2u) return true;
  }
  // Second, and only when the device shadow has nothing: the descriptor the
  // shader published for itself. A live SetTexture must still win, so this sits
  // below the shadow rather than above it.
  if (ps_handle) {
    uint32_t published[6] = {};
    if (ShaderPublishedFetch(ps_handle, sampler, published) &&
        (published[0] & 3u) == 2u) {
      std::memcpy(out, published, sizeof(uint32_t) * 6);
      ++g_shaderFetchServed;
      return true;
    }
  }
  const auto& tb = DeviceState().texture[sampler];
  if (!tb.bound || !tb.valid) return false;
  std::memcpy(out, tb.fetch, sizeof(uint32_t) * 6);
  return true;
}

// The milestone can sample one texture even when the guest shader uses many.
// Pick from evidence in the live descriptors: normalized colour storage is a
// closer approximation to the shader's visible base colour than BC5 normal
// maps, float intermediates, or unnormalized render-target inputs. Ties retain
// shader instruction order; no sampler number is treated as a semantic.
// Per-guest-format tally of descriptors the HLE decoder turned down, shared by
// both rejection sites. This replaced a flat "log the first 12" cap, which
// could spend its whole budget on one format and leave every other one
// invisible — the reason "unsupported texture format" has never once told us
// which format to add. Keyed by the base format index; the value counts
// sightings and the first of each is logged in full.
std::map<uint32_t, uint64_t> g_hleRejectedFormats;

void NoteRejectedTextureFormat(const char* site, uint32_t sampler,
                               const mx::hle::HleTextureSource& source,
                               const char* why, const uint32_t fetch[6]) {
  const uint32_t fmt = source.guest_format;
  ++g_hleRejectedFormats[fmt];
  // Logged once per (format, REASON), not once per format. The tally above
  // stays keyed on format alone because that is what RejectedFormatSummary
  // ranks, but the gate cannot: a format already turned down for one reason
  // would silently swallow every later reason for the same format, and the
  // reason is the only part that says what work would fix it.
  //
  // This matters right now for "texture is a 3D volume". tfetch3D shaders used
  // to be refused whole by the HLSL emitter, so their textures were never
  // described and that reason had never once been reachable. Now that the
  // stacked case translates, a volume is the one remaining refusal, and
  // whether it ever fires decides whether a real Texture3D decode is worth
  // building.
  static std::set<std::pair<uint32_t, std::string>> s_seen;
  if (!s_seen.emplace(fmt, why ? why : "?").second) return;
  REXLOG_INFO("d3d9: HLE texture reject [{}]: sampler {} format {} ({}) — {}; "
              "words {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}",
              site, sampler, fmt, mx::hle::GuestTextureFormatName(fmt),
              why ? why : "?", fetch[0], fetch[1], fetch[2], fetch[3],
              fetch[4], fetch[5]);
}

// Renders the tally as "4:FMT_5_6_5=1832 15:FMT_4_4_4_4=97", ranked by count,
// for the periodic summary. Empty string when nothing has been rejected.
std::string RejectedFormatSummary() {
  std::vector<std::pair<uint64_t, uint32_t>> ranked;
  ranked.reserve(g_hleRejectedFormats.size());
  for (const auto& [fmt, count] : g_hleRejectedFormats)
    ranked.emplace_back(count, fmt);
  std::sort(ranked.begin(), ranked.end(), std::greater<>());
  std::string out;
  for (const auto& [count, fmt] : ranked) {
    if (!out.empty()) out += ' ';
    out += std::to_string(fmt);
    out += ':';
    out += mx::hle::GuestTextureFormatName(fmt);
    out += '=';
    out += std::to_string(count);
  }
  return out;
}

// The guest's Bink frame composite, identified from the binary rather than by
// heuristic. sub_8234D630 (XenonBinkVideo vtable [8]) clears a render target,
// calls sub_8234C7C0, then Resolves into a texture. sub_8234C7C0 binds three
// plane textures to samplers 0/1/2 — Y, Cr, Cb — plus an optional alpha plane
// on sampler 3 whose presence selects the second pixel shader. The guest keeps
// all three shader handles in these globals, so a draw can be matched exactly.
constexpr uint32_t kBinkPixelShaderYuv = 0x82DD7130;
constexpr uint32_t kBinkPixelShaderYuvAlpha = 0x82DD7134;
constexpr uint32_t kBinkVertexShader = 0x82DD7138;

uint32_t ReadGuestGlobalPtr(uint8_t* base, uint32_t addr) {
  return HostPageReadable(REX_RAW_ADDR(addr)) ? REX_LOAD_U32(addr) : 0;
}

// Exact identity: the guest's own two Bink composite pixel shaders, read from
// its globals. Not a heuristic on texture count or draw shape.
REXCVAR_DEFINE_BOOL(d3d9_bink_disable, false, "Debug",
                    "Disconnect the Bink decode path. The guest's video "
                    "composite draws are then built like any other draw, with "
                    "no YUV planes prepared, so videos render as whatever "
                    "their bound textures happen to be. Diagnostic A/B only");

// The single gate for the whole Bink path: both routing sites (the draw
// builder and the deferred rebuild) call this before touching planes, so
// returning false here disconnects decode, upload and the plane budget in one
// place rather than stubbing three.
//
// Disconnecting is a DIAGNOSTIC, not a fix. The path is measured healthy --
// `BINK PLANES 1886 calls = 1886 ok` and `yuv plane gate: 1876 prepared, 0
// refused` -- so anything that changes when it is off is a change in what the
// video draws do to the frame, not a repair of the decoder. With the cvar set,
// the BINK PLANES line reports 0 calls, which is how the log shows the switch
// actually took effect rather than the path merely being quiet.
bool IsBinkCompositeDraw(uint32_t pixel_shader, uint8_t* base) {
  if (!pixel_shader) return false;
  if (REXCVAR_GET(d3d9_bink_disable)) return false;
  return pixel_shader == ReadGuestGlobalPtr(base, kBinkPixelShaderYuv) ||
         pixel_shader == ReadGuestGlobalPtr(base, kBinkPixelShaderYuvAlpha);
}

void ProbeBinkComposite(uint32_t pixel_shader, uint32_t vertex_shader,
                        uint32_t device, uint8_t* base, uint32_t vertex_count) {
  const uint32_t ps_yuv = ReadGuestGlobalPtr(base, kBinkPixelShaderYuv);
  const uint32_t ps_yuv_alpha =
      ReadGuestGlobalPtr(base, kBinkPixelShaderYuvAlpha);
  const uint32_t vs_bink = ReadGuestGlobalPtr(base, kBinkVertexShader);

  // Report the handles themselves whether or not a draw ever matches. All
  // three zero means the guest never created its Bink shaders — a different
  // problem from a plane format we cannot decode, and otherwise identical from
  // the outside, since both produce a probe that never fires.
  static bool s_reported_live = false;
  if (!s_reported_live && (ps_yuv || ps_yuv_alpha || vs_bink)) {
    s_reported_live = true;
    REXLOG_INFO("d3d9: Bink composite shaders created: ps_yuv=0x{:08X} "
                "ps_yuv_alpha=0x{:08X} vs=0x{:08X}",
                ps_yuv, ps_yuv_alpha, vs_bink);
  }
  if (!pixel_shader ||
      (pixel_shader != ps_yuv && pixel_shader != ps_yuv_alpha)) {
    return;
  }

  static std::map<uint32_t, uint64_t> s_hits;
  const uint64_t n = ++s_hits[pixel_shader];
  if (n != 1 && (n % 600) != 0) return;
  REXLOG_INFO("d3d9: Bink composite draw #{} ps=0x{:08X} ({}) vs=0x{:08X}{} "
              "verts={}",
              n, pixel_shader,
              pixel_shader == ps_yuv_alpha ? "YUV+alpha" : "YUV",
              vertex_shader,
              vertex_shader == vs_bink ? "" : " <-- not the Bink VS",
              vertex_count);
  // All four samplers, not just whichever one the binding selector would pick:
  // the whole point is that this draw needs several at once.
  for (uint32_t s = 0; s < 4 && s < mx::hle::kMaxSamplers; ++s) {
    uint32_t fetch[6] = {};
    if (!ReadLiveTextureFetch(device, base, s, fetch)) {
      REXLOG_INFO("d3d9:   Bink sampler {}: no live fetch", s);
      continue;
    }
    mx::hle::HleTextureSource src;
    const char* why = nullptr;
    if (mx::hle::DescribeHleTexture2D(fetch, src, &why)) {
      REXLOG_INFO("d3d9:   Bink sampler {}: format {} ({}) {}x{} tiled={} "
                  "pitch_blocks={} bpb={} decodes",
                  s, src.guest_format,
                  mx::hle::GuestTextureFormatName(src.guest_format), src.width,
                  src.height, src.tiled, src.pitch_blocks, src.bytes_per_block);
    } else {
      REXLOG_INFO("d3d9:   Bink sampler {}: REJECTED ({}); format {} ({}); "
                  "words {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}",
                  s, why ? why : "?", src.guest_format,
                  mx::hle::GuestTextureFormatName(src.guest_format), fetch[0],
                  fetch[1], fetch[2], fetch[3], fetch[4], fetch[5]);
    }
  }
}

// Decode the Bink composite's plane set into the DrawCall. Deliberately
// separate from PrepareDrawTexture rather than folded into it:
//
//  - it must bind *several* textures, which the single-winner binding contest
//    in ResolvePixelBindingForDraw cannot express;
//  - the planes are k_8, which the semantic gate correctly refuses as base
//    colour for a mask but wrongly for a luma plane. Here the guest's own
//    shader identity says what they are, so the gate is not consulted;
//  - it must not touch g_hleCpuTextures. The planes are new guest memory every
//    video frame, so caching them by payload key would grow the cache without
//    bound; at 30 fps that is ~90 dead entries a second.
BinkPlaneRefusals g_binkRefusals;
std::mutex g_binkRefusalsMu;

BinkPlaneRefusals BinkPlaneRefusalStats() {
  std::lock_guard<std::mutex> lk(g_binkRefusalsMu);
  return g_binkRefusals;
}

bool PrepareBinkPlanes(mx::hle::DrawCall& dc, uint32_t device, uint8_t* base) {
  using namespace mx::hle;
  // Charged exactly once per call, at whichever wall it hits.
  auto charge = [](uint64_t BinkPlaneRefusals::*field, uint32_t slot) {
    std::lock_guard<std::mutex> lk(g_binkRefusalsMu);
    ++(g_binkRefusals.*field);
    if (slot != 0xFFFFFFFFu &&
        g_binkRefusals.first_fail_slot == 0xFFFFFFFFu)
      g_binkRefusals.first_fail_slot = slot;
  };
  {
    std::lock_guard<std::mutex> lk(g_binkRefusalsMu);
    ++g_binkRefusals.calls;
  }
  uint32_t decoded = 0;
  for (uint32_t s = 0; s < DrawCall::kMaxPlanes && s < kMaxSamplers; ++s) {
    uint32_t fetch[6] = {};
    if (!ReadLiveTextureFetch(device, base, s, fetch)) {
      // NOT a refusal by itself. A three-plane video has no alpha at slot 3, so
      // this break is how the loop terminates normally -- the first cut charged
      // it unconditionally and reported 646 "no-fetch" against 1886 calls that
      // all succeeded. Only a break that leaves too few planes is a failure,
      // and `too_few` below already counts that.
      if (decoded < 3) charge(&BinkPlaneRefusals::no_fetch, s);
      break;
    }
    HleTextureSource source;
    const char* why = nullptr;
    if (!DescribeHleTexture2D(fetch, source, &why)) {
      NoteRejectedTextureFormat("bink", s, source, why, fetch);
      charge(&BinkPlaneRefusals::describe, s);
      return false;
    }
    std::vector<uint8_t> guest;
    if (!CopyTexturePhysical(source, base, guest)) {
      charge(&BinkPlaneRefusals::copy, s);
      return false;
    }
    auto payload = std::make_shared<HleTexturePayload>();
    if (!DecodeHleTexture2D(source, guest.data(), guest.size(), *payload,
                            &why)) {
      charge(&BinkPlaneRefusals::decode, s);
      return false;
    }
    // An all-zero plane is normal for a video that has not decoded its first
    // frame yet, so unlike the immutable path this is not memoised as empty —
    // the same descriptor will carry real pixels a frame later.
    payload->key = HleTextureKey(fetch);
    // Crop the chroma planes to their logical extent, while the payload is
    // still local and mutable.
    //
    // The guest allocates them with the dimensions rounded up, so half of a
    // 216-row luma arrives as a 320x112 descriptor rather than 320x108. The
    // composite shader samples every plane with the same normalized uv and
    // leaves the half-size difference to the sampler, which is only correct
    // when chroma is *exactly* half: with four rows of padding, uv.y = 1.0
    // reads past the image into zeros, and zero chroma over white luma decodes
    // through BT.601 to (0.29, 1.0, 0.08) — the saturated green line seen
    // across the bottom edge of the video. Measured on the 640x216 overlay;
    // the luma plane itself has no padding (137888 of 138240 bytes nonzero,
    // under one row).
    //
    // Cropping rather than scaling uv in the shader keeps the sampler's
    // normalized mapping right by construction and costs no constant-buffer
    // plumbing. Only ever shrinks, so a plane already at or under the logical
    // size is left alone. Planes 1 and 2 are Cr and Cb; plane 0 is luma and
    // plane 3 the alpha, both full resolution.
    if ((s == 1 || s == 2) && dc.planes[0]) {
      const uint32_t chroma_w = (dc.planes[0]->width + 1) / 2;
      const uint32_t chroma_h = (dc.planes[0]->height + 1) / 2;
      if (chroma_w && payload->width > chroma_w) payload->width = chroma_w;
      if (chroma_h && payload->height > chroma_h) {
        payload->height = chroma_h;
        if (payload->row_pitch) {
          const size_t used = size_t(chroma_h) * payload->row_pitch;
          if (used < payload->data.size()) payload->data.resize(used);
        }
      }
    }
    dc.planes[decoded++] = std::move(payload);
  }
  // Y, Cr and Cb are always present; the fourth is the alpha plane and its
  // presence is what selects the guest's alpha-capable pixel shader.
  if (decoded < 3) {
    charge(&BinkPlaneRefusals::too_few, 0xFFFFFFFFu);
    return false;
  }
  dc.plane_count = decoded;
  dc.yuv_has_alpha = decoded >= 4;
  dc.yuv_composite = true;
  // The composite samples the full frame, so its logical extent is the luma
  // plane's; the chroma planes are half-size and the shader normalises.
  dc.sampled_texture_width = dc.planes[0]->width;
  dc.sampled_texture_height = dc.planes[0]->height;
  {
    std::lock_guard<std::mutex> lk(g_binkRefusalsMu);
    ++g_binkRefusals.ok;
  }
  // WHICH SURFACE the composite actually targets, reported once per distinct
  // target. This is the last inference standing in the EDRAM-aliasing case: the
  // videos were attributed to 0x2175DC60 only because that surface takes
  // exactly 2 draws a frame, matching the 2 Bink composites. Resemblance, not
  // evidence -- and dc.render_target_object is assigned 45 lines above this, so
  // the binding can simply be stated instead.
  //
  // The comparison it settles: the 1280x430 FE_Smoke resolve names 0x2123C1D8
  // as its source. If the composite targets a DIFFERENT object at the same
  // EDRAM base, the resolve is copying a surface the video was never drawn into.
  {
    static std::mutex s_tmu;
    static std::set<uint32_t> s_targets;
    bool fresh = false;
    {
      std::lock_guard<std::mutex> lk(s_tmu);
      fresh = s_targets.size() < 16 && s_targets.insert(dc.render_target_object).second;
    }
    if (fresh) {
      REXLOG_INFO("d3d9: BINK COMPOSITE TARGET object 0x{:08X} {}x{} edram base "
                  "0x{:X} pitch {} -- {} planes, luma {}x{}, alpha {}",
                  dc.render_target_object, dc.render_target_width,
                  dc.render_target_height, dc.surface_base, dc.surface_pitch,
                  decoded, dc.planes[0]->width, dc.planes[0]->height,
                  dc.yuv_has_alpha);
    }
  }
  static uint64_t s_ok = 0;
  if (++s_ok <= 4 || (s_ok % 600) == 0) {
    // Nonzero byte counts per plane. Green output from the YUV shader is what
    // all-zero planes produce, so "did the guest actually decode a frame" and
    // "did our upload work" have to be told apart here rather than guessed at
    // from the colour on screen.
    size_t nz[DrawCall::kMaxPlanes] = {};
    for (uint32_t i = 0; i < decoded; ++i)
      HleTextureHasNonzeroData(*dc.planes[i], &nz[i]);
    REXLOG_INFO("d3d9: Bink planes ready #{}: {} planes, luma {}x{}, alpha {};"
                " nonzero bytes Y={} Cr={} Cb={} A={}",
                s_ok, decoded, dc.planes[0]->width, dc.planes[0]->height,
                dc.yuv_has_alpha, nz[0], nz[1], nz[2], nz[3]);
  }
  return true;
}

// FINDING 2026-08-17 — THIS FUNCTION'S RESULT IS SAMPLED BY NO DRAW.
//
// It picks the ONE texture a tex*col stand-in samples, by scoring the shader's
// candidate fetches against each other. That mattered when untranslated shaders
// were common. Measured over mx_1282..mx_1285, it no longer reaches anything:
//
//   stand-in gate: reached 314000, will_stand_in 56138, pixel_shader==0 56138
//
// The two are IDENTICAL, so every stand-in draw is a no-handle draw -- and for
// those this function is never called at all: there is no shader on the draw
// and none at device+0x3244 either, so ReadBoundPixelShader returns at
// `if (!candidate)` before reaching it. Meanwhile `no-hlsl` is 0 in every run,
// so a draw that HAS a shader always translates, and a translated draw carries
// its textures in pixelTextures and binds them itself (d3d12_game.cpp:4059).
//
// A consumer would have to have a shader (so this runs) AND fail to translate
// (so it samples d.texture). That set is empty.
//
// A grading instrument lived here briefly and confirmed the picks are often
// junk -- a 1x1 kR16Float, a 129x129 terrain clipmap, a 1280x720 kR32Float, all
// scored as colour sources. **Real, and inert.** Removed rather than kept,
// because a counter that can only ever read zero is the thing this codebase
// keeps being bitten by. Do not rebuild it without first re-checking the gate
// numbers above.
//
// The last way it could still have mattered is CLOSED, also negative.
// d.texture selects the PSO SAMPLER VARIANT (point/linear, mip mode) at
// d3d12_game.cpp:4390, which looked like a path a bad pick could reach even on
// a translated draw. It cannot: the translated branch at d3d12_game.cpp:4223
// binds its own root signature, heaps and samplers (BindTranslatedSamplers) and
// ends in `continue` at :4301 — everything below, `++m_standInDraws` included,
// is stand-in only. So the variant is computed from d.texture exclusively for
// draws that never called this function.
//
// Net: nothing this function returns is sampled, and nothing it returns selects
// a sampler. It is vestigial in full.
bool ResolvePixelBindingForDraw(uint32_t handle, uint32_t device,
                                uint8_t* base,
                                mx::hle::PixelTextureBinding& out) {
  if (ResolvePixelBinding(handle, out)) return true;
  const ResolvedPixelBinding* profile = ResolvePixelProfile(handle);
  if (!profile || !profile->decoded || profile->bindings.empty()) return false;

  int best_score = -1;
  uint64_t best_texels = 0;
  mx::hle::HleTextureSource best_source;
  bool found = false;
  for (const auto& candidate : profile->bindings) {
    if (candidate.sampler >= mx::hle::kMaxSamplers) continue;
    uint32_t fetch[6];
    if (!ReadLiveTextureFetch(device, base, candidate.sampler, fetch)) continue;
    mx::hle::HleTextureSource source;
    const char* candidate_why = nullptr;
    if (!mx::hle::DescribeHleTexture2D(fetch, source, &candidate_why)) {
      // Selection used to discard this reason entirely, so a format rejected
      // while choosing a binding produced no log line at all — half the
      // rejections in any run were invisible.
      NoteRejectedTextureFormat("select", candidate.sampler, source,
                                candidate_why, fetch);
      continue;
    }

    // A D3D9 Resolve establishes an ordered host render-target dependency.
    // Its guest backing may legitimately be all zero in native mode because
    // the skipped Xenos dispatch never populated that memory, so this identity
    // is stronger evidence than the descriptor's storage format.
    const auto& texture_state = DeviceState().texture[candidate.sampler];
    const bool mapped_render_target =
        texture_state.object &&
        g_resolvedTextureTargets.contains(texture_state.object);
    const uint64_t candidate_key = mx::hle::HleTextureKey(fetch);
    if (!mapped_render_target && g_hleEmptyTextures.contains(candidate_key))
      continue;
    if (!mapped_render_target &&
        (source.host_format == mx::hle::HostTextureFormat::kBc5 ||
         source.host_format == mx::hle::HostTextureFormat::kR16Float ||
         source.host_format == mx::hle::HostTextureFormat::kRgba16Float ||
         source.host_format == mx::hle::HostTextureFormat::kR8 ||
         source.host_format == mx::hle::HostTextureFormat::kR16 ||
         source.host_format == mx::hle::HostTextureFormat::kR32Float ||
         source.host_format == mx::hle::HostTextureFormat::kRg8 ||
         // The G-buffer formats join the list on the strongest version of the
         // same reasoning: they are not merely unlikely to be base colour,
         // they are render-target storage by construction -- the guest's own
         // format table only ever asks for them with D3DUSAGE_RENDERTARGET.
         // An unmapped copy of one is guest memory the skipped dispatch never
         // wrote.
         source.host_format == mx::hle::HostTextureFormat::kRg16Float ||
         source.host_format == mx::hle::HostTextureFormat::kRg16Unorm ||
         source.host_format == mx::hle::HostTextureFormat::kRg16Snorm ||
         source.host_format == mx::hle::HostTextureFormat::kRgba16Unorm ||
         source.host_format == mx::hle::HostTextureFormat::kRgba16Snorm ||
         source.host_format == mx::hle::HostTextureFormat::kRg32Float))
      continue;
    // An 8x8 immutable texture is a lookup table, not a material. Both that
    // this front end owns are ordered-dither matrices the guest thresholds
    // against for stipple transparency (RenderDoc texture 1316, 8x8 BC1, and
    // the 8x8 k_4_4_4_4 at s12 of shader 0x216A8C20), and the generic host
    // pixel shader has no threshold step — it samples whatever wins and shows
    // it, which is how a Bayer checkerboard ended up painted across the main
    // menu. The tie-break below covers the case where a real texture is also
    // present; this covers the case where it is not, and falling through to
    // the colour-only pipeline is the honest answer. Cut measured, not
    // guessed: across a front-end run the smallest immutable winner other
    // than these two is 64x8.
    if (!mapped_render_target && uint64_t(source.width) * source.height <= 64)
      continue;
    // A mapped render target is authoritative storage, but it is not normally
    // the visible base colour of a material. Multi-input world shaders often
    // combine one or more scene/intermediate targets with immutable colour
    // atlases. Giving mapped targets absolute priority made those shaders
    // sample a black native-mode intermediate instead of their BC1 diffuse
    // texture. Prefer normalized immutable colour assets when both kinds are
    // present. The observed final compositor is handled explicitly above and
    // still selects its mapped s0 scene input.
    int score = mapped_render_target ? 40 :
                (candidate.unnormalized ? 0 : 200);
    switch (source.host_format) {
      case mx::hle::HostTextureFormat::kRgba8:
      case mx::hle::HostTextureFormat::kBc1:
      case mx::hle::HostTextureFormat::kBc2:
      case mx::hle::HostTextureFormat::kBc3:
      // k_4_4_4_4 is a four-channel colour format and, in three front-end
      // runs, the only format the front end asked for at all. It scores with
      // the other colour assets or it would never win a binding.
      case mx::hle::HostTextureFormat::kBgra4:
        score += mapped_render_target ? 40 : 200;
        break;
      case mx::hle::HostTextureFormat::kR8:
      case mx::hle::HostTextureFormat::kR16:
      case mx::hle::HostTextureFormat::kR16Snorm:
      case mx::hle::HostTextureFormat::kR32Float:
        // Single-channel; decodable, but not base colour. Same rationale as
        // the semantic gate in PrepareDrawTexture.
        score += mapped_render_target ? 10 : 0;
        break;
      case mx::hle::HostTextureFormat::kR16Float:
      case mx::hle::HostTextureFormat::kRgba16Float:
      // The four-channel G-buffer formats score with the other render/resolve
      // intermediates: same origin, same reason they can only be selected
      // through a mapped host target.
      case mx::hle::HostTextureFormat::kRgba16Unorm:
      case mx::hle::HostTextureFormat::kRgba16Snorm:
      // k_2_10_10_10 scores here rather than with the colour assets above for
      // the same reason: every sighting of it so far is a full-screen 1280x720
      // render target, not a material. Scoring it as base colour would let a
      // scene intermediate outrank a BC1 diffuse, which is the regression the
      // comment above this switch describes.
      case mx::hle::HostTextureFormat::kRgb10A2Unorm:
        // Float descriptors observed in ST_Southwest are render/resolve
        // intermediates. Only the mapped host-target path above may select
        // them; immutable guest copies are black while GPU dispatch is skipped.
        score += mapped_render_target ? 30 : 0;
        break;
      case mx::hle::HostTextureFormat::kBc5:
      // k_8_8 is the uncompressed two-channel format and lands here for the
      // same reason as BC5: two channels is a normal map, a mask pair or a
      // flow field, never the visible base colour of a material.
      case mx::hle::HostTextureFormat::kRg8:
      // The two-channel G-buffer formats, for the same reason as kRg8: two
      // channels is never the visible base colour of a material.
      case mx::hle::HostTextureFormat::kRg16Float:
      case mx::hle::HostTextureFormat::kRg16Unorm:
      case mx::hle::HostTextureFormat::kRg16Snorm:
      case mx::hle::HostTextureFormat::kRg32Float:
        // DXN/BC5 is a normal map. Keep support for inspection and future
        // shader translation, but never prefer it as visible base colour.
        score += mapped_render_target ? 10 : 0;
        break;
    }
    // Ties were previously broken by fetch program order, which is not
    // evidence of anything, and it lost a 2048x2048 colour atlas to an 8x8
    // ordered-dither matrix that happened to be fetched first (shader
    // 0x216A8C20: s12 8x8 k_4_4_4_4 and s11 2048x2048 RGBA8, both scoring
    // 400). Between two candidates the descriptor cannot otherwise separate,
    // the larger one is the material and the smaller one is a lookup table.
    const uint64_t texels = uint64_t(source.width) * source.height;
    if (score < best_score || (score == best_score && texels <= best_texels))
      continue;
    best_score = score;
    best_texels = texels;
    out = candidate;
    best_source = source;
    found = true;
  }
  if (!found) return false;

  static std::map<uint32_t, bool> s_logged;
  if (s_logged.size() < 32 && s_logged.emplace(handle, true).second) {
    const auto& selected_state = DeviceState().texture[out.sampler];
    const bool mapped = selected_state.object &&
                        g_resolvedTextureTargets.contains(selected_state.object);
    REXLOG_INFO("d3d9: selected s{} r{} {}x{} format {} from {}-fetch "
                "pixel shader 0x{:08X} (descriptor score {}, mapped {})",
                out.sampler, out.src_reg, best_source.width,
                best_source.height, uint32_t(best_source.host_format),
                profile->bindings.size(), handle, best_score, mapped);
  }
  return true;
}

void ProbePixelProfileForDraw(uint32_t pixel_shader, uint32_t device,
                              uint8_t* base,
                              const mx::hle::DrawCall& dc) {
  if (!pixel_shader && device &&
      HostPageReadable(REX_RAW_ADDR(device + 0x3244))) {
    pixel_shader = REX_LOAD_U32(device + 0x3244);
  }
  if (!pixel_shader) return;
  CollectPixelShaderBlob(pixel_shader, base);
  const ResolvedPixelBinding* profile = ResolvePixelProfile(pixel_shader);
  if (!profile) return;

  // One line per shader/target pairing is enough to identify the profile used
  // by the present-sized pass without flooding a frame with repeated draws.
  const uint64_t key = (uint64_t(pixel_shader) << 32) |
                       uint64_t(dc.render_target_object);
  static std::map<uint64_t, bool> s_seen;
  if (s_seen.size() >= 64 || !s_seen.emplace(key, true).second) return;
  REXLOG_INFO("d3d9: pixel profile draw ps=0x{:08X}, target=0x{:08X} "
              "{}x{}, viewport={}x{}, fetches={}{}",
              pixel_shader, dc.render_target_object, dc.render_target_width,
              dc.render_target_height, dc.viewport_width, dc.viewport_height,
              profile->bindings.size(),
              profile->decoded ? "" : " (unsupported)");
  if (profile->decoded) {
    std::string inputs;
    for (const auto& binding : profile->bindings) {
      if (binding.sampler >= mx::hle::kMaxSamplers) continue;
      const auto& tb = DeviceState().texture[binding.sampler];
      mx::hle::HleTextureSource source;
      const char* why = nullptr;
      const bool described = tb.valid &&
          mx::hle::DescribeHleTexture2D(tb.fetch, source, &why);
      uint32_t resolved = 0;
      if (const auto it = g_resolvedTextureTargets.find(tb.object);
          it != g_resolvedTextureTargets.end())
        resolved = it->second;
      inputs += fmt::format(" s{}=tex0x{:08X}", binding.sampler, tb.object);
      if (described)
        inputs += fmt::format("({}x{})", source.width, source.height);
      if (resolved) inputs += fmt::format("->rt0x{:08X}", resolved);
    }
    REXLOG_INFO("d3d9: pixel profile inputs ps=0x{:08X}:{}", pixel_shader,
                inputs.empty() ? " none" : inputs);
  }
}

// IDA proves D3DDevice_SetPixelShader stores the live shader at device+0x3244
// (the adjacent vertex shader is device+0x3248). State-block application may
// bypass our public setter hook, so read the authoritative D3D9 device field at
// draw time and still require exact microcode agreement with a captured PM4 PS.
bool ReadBoundPixelShader(uint32_t device, uint8_t* base, uint32_t& handle,
                          mx::hle::PixelTextureBinding& binding) {
  constexpr uint32_t kDevicePixelShaderOffset = 0x3244;
  if (!device ||
      !HostPageReadable(REX_RAW_ADDR(device + kDevicePixelShaderOffset)))
    return false;
  const uint32_t candidate =
      REX_LOAD_U32(device + kDevicePixelShaderOffset);
  if (!candidate) return false;
  CollectPixelShaderBlob(candidate, base);
  if (!ResolvePixelBindingForDraw(candidate, device, base, binding))
    return false;
  handle = candidate;
  static uint32_t s_logged = 0;
  if (s_logged++ < 8) {
    REXLOG_INFO("d3d9: active pixel shader 0x{:08X} read from "
                "device+0x3244 (sampler {}, UV r{})",
                handle, binding.sampler, binding.src_reg);
  }
  return true;
}

// Copy one guest allocation into `dst` at `at`, trying each address mirror in
// turn and refusing any that is not resident for its whole extent. Returns the
// mirror that worked, or 0.
//
// `base` looks unused and is not: REX_RAW_ADDR expands to reference a variable
// of that name in scope.
uint32_t CopyGuestExtent(uint32_t address, uint32_t bytes, uint8_t* base,
                         std::vector<uint8_t>& dst, size_t at) {
  if (!address || !bytes) return 0;
  const uint32_t candidates[] = {address, address | 0xA0000000u,
                                 address | 0xC0000000u, address | 0xE0000000u};
  for (uint32_t candidate : candidates) {
    bool readable = true;
    for (uint64_t o = 0; o < bytes; o += kHostPageSize) {
      if (!HostPageReadable(REX_RAW_ADDR(candidate + uint32_t(o)))) {
        readable = false;
        break;
      }
    }
    if (!readable || !HostPageReadable(REX_RAW_ADDR(candidate + bytes - 1)))
      continue;
    std::memcpy(dst.data() + at, REX_RAW_ADDR(candidate), bytes);
    return candidate;
  }
  return 0;
}

uint64_t g_mipCopyFailed = 0;

// The base level, then the mip chain appended straight after it.
//
// The two are SEPARATE guest allocations at unrelated addresses, so each is
// resolved through the mirrors independently -- they need not agree on which
// one is mapped. Concatenating them here rather than handing the decoder two
// buffers is what keeps DecodeHleTexture2D's signature, and its three call
// sites, unchanged: the level plan already carries offsets into this blob.
//
// A mip allocation that will not resolve is not fatal. The base is copied
// regardless and the decoder truncates the chain to what it can read, so an
// unmapped chain costs mip levels rather than the texture.
bool CopyTexturePhysical(const mx::hle::HleTextureSource& source, uint8_t* base,
                         std::vector<uint8_t>& out) {
  const uint32_t mip_bytes =
      source.level_count > 1 ? source.mip_source_bytes : 0;
  out.resize(size_t(source.source_bytes) + mip_bytes);
  if (!CopyGuestExtent(source.address, source.source_bytes, base, out, 0))
    return false;
  if (mip_bytes &&
      !CopyGuestExtent(source.mip_address, mip_bytes, base, out,
                       source.source_bytes)) {
    out.resize(source.source_bytes);
    ++g_mipCopyFailed;
  }
  return true;
}

// Resolve the texture a translated shader reads at one compact sampler slot.
//
// Deliberately NOT PrepareDrawTexture's logic. That function applies a policy
// gate — kR8, kR16, kBc5 and the float formats are decoded and then refused as
// "not an immutable colour asset" — which is correct for the stand-in shader,
// where a single-channel texture bound as base colour would paint the surface
// grey. It is wrong here. The guest's own shader knows that channel is a
// coverage mask or a normal map and says so in its arithmetic; refusing to bind
// it leaves the shader sampling nothing.
//
// FMT_8 is the format fonts use, which is why glyph quads came out as filled
// blocks: the texture decoded fine and was then withheld from the shader that
// knew what to do with it.
//
// Fills exactly one of the two per-slot outputs: a resolved render target if
// the guest bound one there, otherwise a decoded CPU payload. Returns false
// when neither could be produced, and the caller decides whether that is fatal.
// Why a sampler slot could not be filled. A slot that fails sends the WHOLE
// draw back to the tex*col stand-in, so these are the draws the guest's own
// pixel shader was translated for and then not used on -- 26,844 of them in
// mx_705, which is essentially every stand-in draw in that run. Six of the
// seven exits below were previously silent, and the one that logged fired once.
uint64_t g_slotFailRange = 0, g_slotFailFetch = 0, g_slotFailDescribe = 0;
uint64_t g_slotFailCopy = 0, g_slotFailDecode = 0;
uint64_t g_slotBoundZero = 0;   // all-zero, and bound anyway -- see below
uint64_t g_slotBoundUnbound = 0;  // sampler the guest never bound; sampled zero
// Which guest sampler had no readable fetch constant. The open question this
// answers is whether the shaders' samplers 8-15 index the bank the same way
// 0-7 do: a failure spread evenly over low samplers means genuinely unbound
// slots, whereas one concentrated at and above 8 means the indexing is wrong.
std::map<uint32_t, uint64_t> g_slotFailFetchBySampler;
// Which guest sampler tripped the range check, and which half of it. `range`
// was the largest slot-fill failure in mx_1108 with nothing saying why, and the
// two conditions want opposite fixes: a compact slot at or above 16 is our
// bookkeeping, a guest sampler at or above kMaxSamplers was the file being read
// at half its width.
std::map<uint32_t, uint64_t> g_slotFailRangeBySampler;
uint64_t g_slotFailRangeSlot = 0;

void ReportSlotFailures() {
  const uint64_t total = g_slotFailRange + g_slotFailFetch +
                         g_slotFailDescribe + g_slotFailCopy + g_slotFailDecode;
  // Every 5000 AND on the first one. `(total % 5000) != 0` alone meant a run
  // with fewer than 5000 failures printed NOTHING -- mx_1110 had 2389 short
  // draws and not one outcome line, which reads exactly like zero failures.
  // The same trap as the unreachable stand-in counter; see the note there.
  if (!total || (total != 1 && (total % 5000) != 0)) return;
  std::string by;
  for (const auto& [sampler, n] : g_slotFailFetchBySampler)
    by += fmt::format(" s{}={}", sampler, n);
  std::string rby;
  for (const auto& [sampler, n] : g_slotFailRangeBySampler)
    rby += fmt::format(" s{}={}", sampler, n);
  REXLOG_INFO("d3d9: slot fill outcomes {}: range {} (slot-too-wide {}, guest "
              "sampler:{}) describe {} copy {} decode {} (these still fail the "
              "draw); unbound by sampler:{}",
              total, g_slotFailRange, g_slotFailRangeSlot,
              rby.empty() ? " none" : rby, g_slotFailDescribe, g_slotFailCopy,
              g_slotFailDecode, by.empty() ? " none" : by);
}

// A texture that DECODED but is entirely zero, and a sampler the guest never
// bound, are both bound rather than refused -- deliberately, see the notes in
// ResolvePixelSlotTexture. Their counters used to be printed only inside
// ReportSlotFailures, which returns early when the hard-failure total is zero.
// In mx_736 nothing failed, so neither number was ever printed once: draws
// painting solid black were invisible in the log. They get their own line,
// fired off their own total.
uint64_t g_boundZeroReported = 0;

// One entry per distinct all-zero texture, so the offenders can be named rather
// than counted. The set is small -- nine keys covered 10,890 draws in mx_706 --
// so it is printed in full. `recovered` is the question the log has to answer:
// a key that later decodes non-blank was a texture sampled before the guest
// finished streaming it, which is a caching defect; one that never recovers is
// a genuinely blank guest texture and a different investigation.
// Distinguishes the host upload of a blank decode from the host upload of the
// same texture once it has real contents. Both would otherwise share the
// fetch-word key that EnsureGameTexture caches on, and the recovered texture
// would hit the black resource uploaded before it.
// The value an unbound Xenos sampler actually returns: one black texel. Shared
// by every path that has to bind SOMETHING for a slot the guest did not supply,
// so those paths cannot drift into fabricating different placeholders.
std::shared_ptr<mx::hle::HleTexturePayload> UnboundTexturePayload() {
  static const auto s_unbound = [] {
    auto p = std::make_shared<mx::hle::HleTexturePayload>();
    p->width = p->height = 1;
    p->row_pitch = 4;
    p->format = mx::hle::HostTextureFormat::kRgba8;
    p->linear_filter = false;
    p->data.assign(4, 0);
    return p;
  }();
  return s_unbound;
}

constexpr uint64_t kBlankTextureKeyMarker = 0x8000000000000000ull;

struct BlankTexture {
  uint32_t sampler = 0;
  uint32_t address = 0;
  uint32_t width = 0, height = 0;
  uint32_t guest_format = 0;
  uint32_t swizzle = 0;
  uint64_t first_frame = 0;
  uint64_t draws = 0;
  bool recovered = false;
};
std::map<uint64_t, BlankTexture> g_blankTextures;

void NoteBlankTexture(uint64_t key, uint32_t sampler,
                      const mx::hle::HleTextureSource& source) {
  auto [it, inserted] = g_blankTextures.emplace(key, BlankTexture{});
  BlankTexture& b = it->second;
  ++b.draws;
  if (!inserted) return;
  b.sampler = sampler;
  b.address = source.address;
  b.width = source.width;
  b.height = source.height;
  b.guest_format = source.guest_format;
  b.swizzle = source.swizzle;
  b.first_frame = mx::hle::D3D9FrameCount();
  REXLOG_INFO("d3d9: bound-zero texture {:#x}: sampler {} addr {:#x} {}x{} "
              "guest format {} swizzle {:#o} first seen frame {}",
              key, sampler, source.address, source.width, source.height,
              source.guest_format, source.swizzle, b.first_frame);
}

// Called when a key that was previously all-zero decodes to real data. This is
// the line that convicts or clears the cache: it can only ever print once the
// blank decode stops being cached forever.
void NoteBlankRecovered(uint64_t key) {
  // Both judgements were made when the texture was blank and are now wrong, so
  // they are withdrawn rather than left standing: the stand-in path refuses any
  // key in the empty set as a poor representative of the draw, and the blank
  // payload is what the within-frame retry would hand back.
  g_hleEmptyTextures.erase(key);
  g_hleBlankPayloads.erase(key);
  auto it = g_blankTextures.find(key);
  if (it == g_blankTextures.end() || it->second.recovered) return;
  it->second.recovered = true;
  REXLOG_INFO("d3d9: bound-zero texture {:#x} RECOVERED on frame {} (was blank "
              "from frame {}, {} draws sampled black)",
              key, mx::hle::D3D9FrameCount(), it->second.first_frame,
              it->second.draws);
}

void ReportBoundZero() {
  const uint64_t total = g_slotBoundZero + g_slotBoundUnbound;
  if (!total || total == g_boundZeroReported ||
      (total % 2500) != 0)
    return;
  g_boundZeroReported = total;
  size_t outstanding = 0;
  for (const auto& [key, b] : g_blankTextures)
    if (!b.recovered) ++outstanding;
  std::string by;
  for (const auto& [sampler, n] : g_slotFailFetchBySampler)
    by += fmt::format(" s{}={}", sampler, n);
  REXLOG_INFO("d3d9: draws sampling BLACK {}: all-zero texture {}, "
              "unbound sampler {}; {} distinct blank textures, {} still blank; "
              "unbound by sampler:{}; resolve address matches {} (extent "
              "mismatches {}), resolves dropped for no source {}; "
              "shader-published fetch: {} shaders, {} slots served",
              total, g_slotBoundZero, g_slotBoundUnbound,
              g_blankTextures.size(), outstanding,
              by.empty() ? " none" : by, g_resolveAddr.matches,
              g_resolveAddr.extentMiss, g_resolveDroppedNoSource,
              g_shaderFetchPublished.load(std::memory_order_relaxed),
              g_shaderFetchServed.load(std::memory_order_relaxed));
}

// Every distinct (guest format, swizzle) pair that reaches a binding, printed
// once. Two open questions read straight off it: whether any swizzle component
// is 6 or 7 -- values D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING does not define,
// so the channel would be undefined rather than wrong -- and whether the
// k_8_8_8_8 -> R8G8B8A8 mapping is relying on a swizzle that actually performs
// the BGRA rotation.
// Also censused here: TEX_FORMAT_COMP / GPUSIGN, which the describe step reads
// but nothing acts on. Three of its four values change what a fetch returns --
// kSigned is two's complement (an SNORM host format), kUnsignedBiased is
// 2*c-1, and kGamma is sRGB linearized on sample -- so any of them appearing
// means we hand the shader the wrong numbers, quietly. This says whether the
// game uses them at all before any of it is built.
void NoteSwizzleCensus(const mx::hle::HleTextureSource& source) {
  static std::set<uint64_t> s_seen;
  const uint64_t pair = (uint64_t(source.guest_format) << 32) |
                        (uint64_t(source.signs) << 16) | source.swizzle;
  if (!s_seen.insert(pair).second) return;
  const uint32_t c[4] = {(source.swizzle >> 0) & 7u, (source.swizzle >> 3) & 7u,
                         (source.swizzle >> 6) & 7u, (source.swizzle >> 9) & 7u};
  const bool undefined = c[0] > 5 || c[1] > 5 || c[2] > 5 || c[3] > 5;
  static constexpr const char* kSignName[4] = {"unsigned", "signed", "biased",
                                               "gamma"};
  const uint32_t s[4] = {(source.signs >> 0) & 3u, (source.signs >> 2) & 3u,
                         (source.signs >> 4) & 3u, (source.signs >> 6) & 3u};
  REXLOG_INFO("d3d9: swizzle census: guest format {} swizzle {:#o} "
              "components [{} {} {} {}]{} signs [{} {} {} {}]{}",
              source.guest_format, source.swizzle, c[0], c[1], c[2], c[3],
              undefined ? "  <-- UNDEFINED in D3D12 (>5)" : "", kSignName[s[0]],
              kSignName[s[1]], kSignName[s[2]], kSignName[s[3]],
              source.signs ? "  <-- NOT PLAIN UNSIGNED, we ignore this" : "");
}

// How many slot binds actually carry a sign mode we do not honour, split by
// guest format. The census above says which formats do it; this says whether it
// is one decorative texture or the whole scene, which is the difference between
// closing the question and building a signed decode path.
//
// Float formats are excluded deliberately. A TextureSign on k_*_FLOAT is a
// no-op -- the data is already signed float, and the reference cache only needs
// a separate host texture when a FIXED-POINT format has no signed host
// equivalent (cache.h:488, IsSignedVersionSeparateForFormat). Counting them
// would inflate the number with binds that need nothing done.
bool IsFloatGuestFormat(uint32_t guest_format) {
  switch (xn::TextureFormat(guest_format)) {
    case xn::TextureFormat::k_16_FLOAT:
    case xn::TextureFormat::k_16_16_FLOAT:
    case xn::TextureFormat::k_16_16_16_16_FLOAT:
    case xn::TextureFormat::k_32_FLOAT:
    case xn::TextureFormat::k_32_32_FLOAT:
    case xn::TextureFormat::k_32_32_32_32_FLOAT:
      return true;
    default:
      return false;
  }
}

void NoteSignedBind(const mx::hle::HleTextureSource& source) {
  if (!source.signs || IsFloatGuestFormat(source.guest_format)) return;
  static std::map<uint32_t, uint64_t> s_binds;
  const uint64_t n = ++s_binds[(source.guest_format << 8) | source.signs];
  if ((n % 5000) != 0) return;
  std::string by;
  for (const auto& [k, v] : s_binds)
    by += fmt::format(" fmt{}/signs{:#04x}={}", k >> 8, k & 0xFF, v);
  REXLOG_INFO("d3d9: binds of a non-float texture with an unhonoured sign mode:{}",
              by);
}

// Did we read the mip chain from the RIGHT PLACE?
//
// A wrong offset, pitch or packed-tail displacement does not fail: it returns
// plausible bytes from somewhere else in the allocation, and the result is only
// visible on minified surfaces at a distance, which is exactly where nobody
// looks closely. Neither the blank-texture counters nor the decode's own bounds
// check can see it -- the same blind spot that let the packed base level read
// another texture's bytes for months.
//
// So measure it instead. The guest's mips are a reduction of their parent, so
// box-filtering level n-1 down by two should land close to level n. Small mean
// absolute difference (call it under ~12 of 255) means the addressing is right;
// two uncorrelated images average about 85. Uncompressed formats only -- block
// compression cannot be averaged without decoding it -- but the addressing
// maths is parameterised by block size rather than special-cased per format, so
// what holds here holds for BC too. The BC formats are checked in RenderDoc.
// The mean colour of one texel or one compressed block, as an RGB triple in
// 0..255, or false when this format is not sampled by the check.
//
// Block-compressed formats have to be included or the check is close to
// worthless: this game's art is overwhelmingly BC1/BC3/BC5, and a check that
// only understands kRgba8 would have reported nothing at all while the very
// textures the mip chain was built for went unverified.
//
// A block is not decoded, only averaged. Every BC variant stores two endpoints
// at a known offset and interpolates between them, so the midpoint of the
// endpoints approximates the block's mean well enough to tell "a smaller
// version of its parent" from "bytes belonging to something else" -- which is
// the only question being asked.
bool BlockMeanColor(mx::hle::HostTextureFormat format, const uint8_t* p,
                    uint32_t out[3]) {
  using F = mx::hle::HostTextureFormat;
  auto rgb565 = [](const uint8_t* q, uint32_t acc[3]) {
    const uint32_t v = uint32_t(q[0]) | (uint32_t(q[1]) << 8);
    acc[0] += ((v >> 11) & 31) * 255 / 31;
    acc[1] += ((v >> 5) & 63) * 255 / 63;
    acc[2] += (v & 31) * 255 / 31;
  };
  uint32_t acc[3] = {0, 0, 0};
  switch (format) {
    case F::kRgba8:
      out[0] = p[0]; out[1] = p[1]; out[2] = p[2];
      return true;
    case F::kR8:
      out[0] = out[1] = out[2] = p[0];
      return true;
    // BC1 colour endpoints at +0; BC3 the same after 8 bytes of alpha.
    case F::kBc1:
    case F::kBc2:
    case F::kBc3: {
      const uint8_t* c = p + (format == F::kBc1 ? 0 : 8);
      rgb565(c, acc);
      rgb565(c + 2, acc);
      for (uint32_t i = 0; i < 3; ++i) out[i] = acc[i] / 2;
      return true;
    }
    // BC5 is two BC4 blocks, each an 8-bit endpoint pair.
    case F::kBc5:
      out[0] = (uint32_t(p[0]) + p[1]) / 2;
      out[1] = (uint32_t(p[8]) + p[9]) / 2;
      out[2] = 0;
      return true;
    default:
      return false;
  }
}

void NoteMipLevelAgreement(const mx::hle::HleTexturePayload& payload) {
  if (payload.level_count < 2) return;
  uint32_t probe[3];
  if (payload.data.empty() ||
      !BlockMeanColor(payload.format, payload.data.data(), probe))
    return;
  static std::atomic<uint32_t> s_checked{0};
  if (s_checked.fetch_add(1, std::memory_order_relaxed) >= 16) return;

  // Block units, so compressed and uncompressed walk the same loop.
  const uint32_t bpb =
      payload.format == mx::hle::HostTextureFormat::kRgba8 ? 4
      : payload.format == mx::hle::HostTextureFormat::kR8 ? 1
      : payload.format == mx::hle::HostTextureFormat::kBc1 ? 8
                                                           : 16;
  std::string report;
  double worst = 0.0;
  for (uint32_t l = 1; l < payload.level_count; ++l) {
    const auto& prev = payload.levels[l - 1];
    const auto& cur = payload.levels[l];
    const uint32_t prev_cols = prev.row_pitch / bpb, cur_cols = cur.row_pitch / bpb;
    if (!cur_cols || !cur.rows || prev_cols < 2 || prev.rows < 2) break;
    if (size_t(prev.offset) + size_t(prev.row_pitch) * prev.rows >
            payload.data.size() ||
        size_t(cur.offset) + size_t(cur.row_pitch) * cur.rows >
            payload.data.size())
      break;
    // Measured twice: once against the parent region this level should have
    // reduced, and once against a region half the texture away.
    //
    // An absolute threshold cannot do this job. The guest does not box-filter
    // its mips, block endpoints only approximate a block's mean, and small
    // levels are a small sample -- so a perfectly correct level can score 30
    // while another correct one scores 3. The first version of this check
    // called half the textures SUSPECT on exactly that basis, including two
    // 512x256 BC1 textures whose addressing maths is necessarily identical.
    //
    // The CONTROL is what settles it, with no magic number: whatever the
    // content does to the aligned score, it does to the misaligned one too.
    // Aligned much lower than control means this level really is its parent
    // reduced. The two being equal is the signature of reading someone else's
    // bytes.
    uint64_t sum = 0, control_sum = 0, n = 0;
    for (uint32_t y = 0; y < cur.rows; ++y) {
      for (uint32_t x = 0; x < cur_cols; ++x) {
        uint32_t want[3] = {0, 0, 0}, control[3] = {0, 0, 0};
        for (uint32_t dy = 0; dy < 2; ++dy) {
          for (uint32_t dx = 0; dx < 2; ++dx) {
            const uint32_t sy = std::min(y * 2 + dy, prev.rows - 1);
            const uint32_t sx = std::min(x * 2 + dx, prev_cols - 1);
            uint32_t c[3];
            BlockMeanColor(payload.format,
                           payload.data.data() + prev.offset +
                               size_t(sy) * prev.row_pitch + size_t(sx) * bpb,
                           c);
            for (uint32_t i = 0; i < 3; ++i) want[i] += c[i];
            const uint32_t oy = (sy + prev.rows / 2) % prev.rows;
            const uint32_t ox = (sx + prev_cols / 2) % prev_cols;
            BlockMeanColor(payload.format,
                           payload.data.data() + prev.offset +
                               size_t(oy) * prev.row_pitch + size_t(ox) * bpb,
                           c);
            for (uint32_t i = 0; i < 3; ++i) control[i] += c[i];
          }
        }
        uint32_t got[3];
        BlockMeanColor(payload.format,
                       payload.data.data() + cur.offset +
                           size_t(y) * cur.row_pitch + size_t(x) * bpb,
                       got);
        for (uint32_t i = 0; i < 3; ++i) {
          sum += uint64_t(std::abs(int32_t(want[i] / 4) - int32_t(got[i])));
          control_sum +=
              uint64_t(std::abs(int32_t(control[i] / 4) - int32_t(got[i])));
          ++n;
        }
      }
    }
    if (!n) continue;
    const double mad = double(sum) / double(n);
    const double control_mad = double(control_sum) / double(n);
    // Ratio, not difference: a flat texture scores low on both and a busy one
    // high on both, and only their relationship carries the signal. Guarded
    // against a genuinely uniform level, where both are ~0 and neither says
    // anything.
    const double ratio = control_mad > 1.0 ? mad / control_mad : 0.0;
    worst = std::max(worst, ratio);
    report += fmt::format(" L{}({}x{})={:.1f}/{:.1f}", l, cur.width, cur.height,
                          mad, control_mad);
  }
  if (report.empty()) return;
  REXLOG_INFO("d3d9: MIP AGREEMENT {}x{} fmt{} {} levels: {} -- aligned/control"
              " mean |box(n-1) - n|, worst ratio {:.2f}:{}",
              payload.width, payload.height, uint32_t(payload.format),
              payload.level_count,
              worst < 0.7 ? "ALIGNED" : "SUSPECT (aligned is no better than a"
                                        " half-texture offset)",
              worst, report);
}

// The chain census, and the deliberate gaps in it.
//
// Called per BIND, deliberately. The first version of this hung off the decode
// path, which only runs on a cache miss -- so in a menu-only run it printed
// once, three seconds in, and never again. The numbers it did print (74
// textures described, none with a chain) said nothing about the run at all.
void NoteMipCensus() {
  static std::atomic<uint64_t> s_binds{0};
  const uint64_t n = s_binds.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n != 1 && (n % 20000) != 0) return;
  // The absolute check on the tiled addressing, printed once. It answers a
  // question the mip self-check structurally cannot -- see HleTiledAddressCheck
  // -- and it prints even when it passes, because "no line" is how a check that
  // never ran looks, and that has cost this project twice today.
  {
    static std::atomic<bool> s_reported{false};
    if (!s_reported.exchange(true)) {
      const mx::hle::HleTiledAddressCheck t = mx::hle::HleTiledAddressStats();
      if (t.mismatched) {
        REXLOG_ERROR(
            "d3d9: TILED ADDRESSING DISAGREES with xenia-edge: {} of {} "
            "coordinates; first at ({},{}) pitch {} bpb_log2 {} -- SDK {} vs "
            "reference {}. Every tiled texture is being read from the wrong "
            "bytes.",
            t.mismatched, t.checked, t.first_x, t.first_y, t.first_pitch,
            t.first_bytes_per_block_log2, t.first_sdk, t.first_reference);
      } else {
        REXLOG_INFO("d3d9: tiled addressing self-check: {} coordinates agree "
                    "with xenia-edge across every block size",
                    t.checked);
      }
    }
  }
  // Printed with every census tick, including when every field is zero.
  // "This title binds no 1D textures"
  // is a finding, and it is the finding that decides whether the wide-1D remap
  // is worth writing -- but only if the line appears at all. A census that
  // stays silent when it counts nothing is indistinguishable from one that was
  // never wired up.
  {
    const mx::hle::HleOneDCensus d = mx::hle::HleOneDStats();
    REXLOG_INFO("d3d9: 1D textures: {} described (refused: tiled {}, packed "
                "mips {}, wider than 16384 {}); over 8192 and accepted {}",
                d.seen, d.tiled, d.packed, d.too_wide, d.wide);
  }
  const mx::hle::HleMipCensus c = mx::hle::HleMipChainStats();
  std::string levels;
  for (uint32_t i = 0; i < 16; ++i)
    if (c.by_max_level[i]) levels += fmt::format(" max{}={}", i,
                                                 c.by_max_level[i]);
  REXLOG_INFO(
      "d3d9: MIP CHAIN over {} binds: {} carry one ({} levels total, mean"
      " {:.1f}); mip_address set {}; declared but no address {}; suppressed"
      " base-map {} min-level {}; layout empty {}; truncated at decode {};"
      " chain copy failed {}; deferred: mip_filter=point {} lod_bias {};"
      " raw mip_max_level:{}",
      c.described, c.with_chain, c.levels_planned,
      c.with_chain ? double(c.levels_planned) / double(c.with_chain) : 0.0,
      c.raw_mip_address_set, c.no_address, c.suppressed_base_map,
      c.suppressed_min_level, c.layout_empty, c.truncated, g_mipCopyFailed,
      c.mip_filter_point, c.lod_bias_set, levels);
}

// Blast radius of the packed mip tail. A texture whose base is packed used to
// be read from the origin of the tail rather than from its own offset within
// it, so every one of these was returning another texture's bytes. Counted by
// extent and format so the population is visible rather than inferred from the
// one 8x8 DXT1 lookup that made it findable -- that one multiplies the menu's
// deferred lighting, which is why the whole scene came out black.
void NotePackedBase(const mx::hle::HleTextureSource& source) {
  if (!source.packed_offset_x_blocks && !source.packed_offset_y_blocks) return;
  static std::map<uint64_t, uint64_t> s_seen;
  const uint64_t k = (uint64_t(source.guest_format) << 32) |
                     (uint64_t(source.width) << 16) | source.height;
  const uint64_t n = ++s_seen[k];
  if (n != 1 && (n % 20000) != 0) return;
  std::string by;
  for (const auto& [key, v] : s_seen)
    by += fmt::format(" {}x{}/fmt{}={}", (key >> 16) & 0xFFFF, key & 0xFFFF,
                      uint32_t(key >> 32), v);
  REXLOG_INFO("d3d9: textures read from a PACKED MIP TAIL, {} distinct:{}",
              s_seen.size(), by);
}

// A sign mode that reaches a bind and is NOT applied. kSigned needs the
// texture's bits reinterpreted into a signed host format (a decode change, task
// #43); kGamma needs an sRGB curve. Neither is approximated here -- an
// unimplemented mode that silently behaves as unsigned is at least visible in
// this line.
void NoteUnhandledSign(uint32_t guest_format, uint32_t mode) {
  static std::map<uint32_t, uint64_t> s_seen;
  const uint64_t n = ++s_seen[(guest_format << 4) | mode];
  if (n != 1 && (n % 20000) != 0) return;
  static constexpr const char* kName[4] = {"unsigned", "signed", "biased",
                                           "gamma"};
  REXLOG_INFO("d3d9: texture sign mode NOT applied: guest format {} mode {} x{}",
              guest_format, kName[mode & 3], n);
}

// `vertex` selects which stage's slot arrays the result lands in. Everything
// else -- the fetch-constant read, the resolve-snapshot match, the blank-decode
// retry, every counter -- is identical for the two stages, because the question
// "what texture is bound to guest sampler N" does not depend on who is asking.
// Splitting this into two functions would have meant two copies of 250 lines
// that must agree.
//
// dc.pixel_shader_handle is still read for the blank-texture key regardless of
// stage: that key identifies the DRAW's material, and a vertex fetch of the
// same guest memory wants the same memoisation.
// `stage_handle` names the shader whose slot this is, for diagnostics only.
// The vertex caller must pass it: `dc.vertex_shader_handle` is assigned in a
// different function that has not necessarily run yet, so reading it here
// printed `SLOT MAP vs 0x00000000` and left every vertex shader's slots hashing
// to the same dedupe key -- the third variant of the same mistake in one
// session, after the renderer census and the uniform-decode line.
bool ResolvePixelSlotTexture(mx::hle::DrawCall& dc, uint32_t slot,
                             uint32_t guest_sampler, uint32_t device,
                             uint8_t* base, bool vertex = false,
                             uint32_t stage_handle_hint = 0) {
  using namespace mx::hle;
  auto& out_textures = vertex ? dc.vertex_textures : dc.pixel_textures;
  auto& out_objects =
      vertex ? dc.vertex_sampled_objects : dc.pixel_sampled_objects;
  auto& out_swizzles =
      vertex ? dc.vertex_sampled_swizzles : dc.pixel_sampled_swizzles;
  const uint32_t stage_handle =
      stage_handle_hint ? stage_handle_hint
                        : (vertex ? dc.vertex_shader_handle
                                  : dc.pixel_shader_handle);
  auto& out_signs =
      vertex ? dc.vertex_sampler_signs : dc.pixel_sampler_signs;
  if (slot >= DrawCall::kMaxPixelTextures || guest_sampler >= kMaxSamplers) {
    ++g_slotFailRange;
    if (slot >= DrawCall::kMaxPixelTextures) ++g_slotFailRangeSlot;
    else ++g_slotFailRangeBySampler[guest_sampler];
    ReportSlotFailures();
    return false;
  }

  // A slot the guest points at a resolve result: the renderer samples the live
  // host target, exactly as the single-texture path already does.
  // A destination the GPU wrote whole is a render target: its guest memory is
  // meaningless and the snapshot is the only truthful answer, so take it before
  // spending a decode. 25 of the 26 destinations in mx_780 are this case.
  //
  // A destination the GPU wrote only part of is the interesting one, and it is
  // NOT a choice between a good answer and a bad one -- both sources are
  // partial. The 2048x2048 menu atlas has three 256x256 tiles of real rendered
  // content along its top edge and nothing else, while guest memory for it
  // decodes to zeros. Refusing the snapshot outright, as the first version of
  // this rule did, threw those three tiles away 2272 times and bound zeros
  // instead. So: try memory first, and fall back to the snapshot when memory
  // has nothing -- which also keeps the blank-retry path alive, so the day the
  // guest fills that memory it wins on its own.
  //
  // "Has nothing" meant all-zero until 2026-08-14, and that read the terrain
  // heightmap as real data for months: nothing CPU-writes it, so its memory
  // decodes to a uniform 0xFF rather than to zeros. A UNIFORM decode counts as
  // nothing too, but only here, where a partly-written snapshot is standing by.
  uint32_t partial_snapshot_object = 0;
  const auto& texture_state = DeviceState().texture[guest_sampler];
  // Unconditional, and BEFORE the resolve-destination branch. The material's
  // texture need never have been resolved into, so gating this the way
  // slot_seen is gated would make it a counter that cannot fire for exactly
  // the case it exists to measure.
  NoteVideoShapeSlot(texture_state.fetch, texture_state.valid);
  ResolvedTargetByAddress* resolve_entry = nullptr;
  if (texture_state.object &&
      g_resolvedTextureTargets.contains(texture_state.object)) {
    // Counted BEFORE the coverage gate, so "reached the draw path" and "was
    // allowed to be a snapshot" stay separable. The SLOT MAP line below cannot
    // answer this: it dedupes on (shader, slot), so a slot that logged once
    // with a different texture never logs again however many other textures
    // pass through it.
    resolve_entry = ResolveEntryForObject(texture_state.object);
    if (resolve_entry) ++resolve_entry->slot_seen;
    if (ResolvedDestinationIsMostlyWritten(texture_state.object)) {
      if (resolve_entry) ++resolve_entry->slot_snapshot;
      // Logged HERE as well as at the decode below, because this path RETURNS.
      // The first cut of the SLOT MAP diagnostic sat only after this point and
      // so reported resolved=0 on every line it printed -- blind to precisely
      // the slots that bind a snapshot, which are the ones worth seeing. A slot
      // simply went missing from the table instead, which reads like it was
      // never bound. See the note at the other call for why this matters.
      static std::mutex s_mu;
      static std::set<uint64_t> s_seen;
      // Stage-qualified, for the reason spelled out at the function header.
      const uint64_t key = (uint64_t(stage_handle) << 9) |
                           (uint64_t(vertex ? 1u : 0u) << 8) | slot;
      bool fresh = false;
      {
        std::lock_guard<std::mutex> lk(s_mu);
        // Bounded by the dedupe -- one line per distinct (shader, slot), which
        // is tens of shaders times a handful of slots. A tighter cap filled up
        // on early menu shaders and cut off before the material under
        // investigation ever bound.
        // Raised with the renderer's census cap, and for the same
        // reason: 632 of these 1024 were spent in the menu, so a level's
        // bindings arrive against a nearly full budget.
        fresh = s_seen.size() < 4096 && s_seen.insert(key).second;
      }
      // Read regardless of whether this line is fresh: the swizzle is no
      // longer only a diagnostic, it is what the renderer binds the snapshot
      // with. Gating it on the log's dedupe would leave every slot after the
      // first with an identity mapping again.
      uint32_t sfetch[6] = {};
      uint32_t swz = 0;
      bool have_swz = false;
      if (ReadLiveTextureFetch(device, base, guest_sampler, sfetch,
                               dc.pixel_shader_handle)) {
        // dword 3: num_format:1 then swizzle:12, per the fetch constant layout
        // (xenos.h) -- so the swizzle starts at bit 1.
        swz = (sfetch[3] >> 1) & 0xFFFu;
        have_swz = true;
      }
      if (fresh) {
        REXLOG_INFO(
            "d3d9: SLOT MAP {} 0x{:08X} slot {} (guest sampler {}): object "
            "0x{:08X} -> SNAPSHOT of a resolve destination (no guest-memory "
            "decode); guest swizzle {}",
            vertex ? "vs" : "ps", stage_handle, slot, guest_sampler,
            texture_state.object,
            have_swz ? fmt::format("{:#o}", swz) : std::string("unreadable"));
      }
      out_objects[slot] = texture_state.object;
      // The renderer has no fetch constant of its own for a snapshot slot; this
      // is the only place the guest swizzle is in hand. See the field's note.
      out_swizzles[slot] = have_swz ? uint16_t(swz) : uint16_t(0);
      return true;
    }
    if (resolve_entry) ++resolve_entry->slot_partial;
    partial_snapshot_object = texture_state.object;
  }

  uint32_t fetch[6] = {};
  // dc.pixel_shader_handle is the handle AttachTranslatedPixelShader resolved
  // for this DEVICE, not the thread-local one -- see the resolution above it --
  // which is the handle whose load table may carry this sampler's descriptor.
  if (!ReadLiveTextureFetch(device, base, guest_sampler, fetch,
                            dc.pixel_shader_handle)) {
    ++g_slotFailFetch;
    ++g_slotFailFetchBySampler[guest_sampler];
    ReportSlotFailures();
    // The guest never bound anything to this sampler, and the shader reads it
    // anyway. Measured over mx_710 the failures fall s5=2420 s6=304 s7=2420
    // s8=2429 s9=2420 s13=4, with samplers 0-4 never failing once -- one shader
    // family reading four slots this title does not bind. It is NOT the
    // thread-local device state losing a binding, which would fail every
    // sampler on the affected thread together rather than the same four.
    //
    // Zero is what the hardware returns for a fetch constant whose type is not
    // kTexture, so an unbound slot samples zero. Refusing instead sent the
    // whole draw to the tex*col stand-in, discarding every OTHER slot's real
    // shading over a slot whose value the shader may not even use -- the same
    // trade already settled for all-zero textures, which cost 10,890 draws.
    //
    // This is a bound zero, not a fabricated colour: nothing here invents a
    // plausible texture, it supplies the value an unbound fetch actually has.
    out_textures[slot] = UnboundTexturePayload();
    ++g_slotBoundUnbound;
    ReportBoundZero();
    return true;
  }
  HleTextureSource source;
  const char* why = nullptr;
  ++g_tex.slotCalls;
  bool described = false;
  {
    PhaseTimer t(g_tex.describeUs);
    described = DescribeHleTexture2D(fetch, source, &why);
  }
  if (!described) {
    NoteRejectedTextureFormat("slot", guest_sampler, source, why, fetch);
    // A fetch constant the reference calls invalid rather than unsupported.
    // Xenia drops the BINDING and keeps drawing -- its key stays invalid, the
    // sampler reads zero, and the guest's own shader still runs. Failing the
    // draw here would be a strictly worse answer than the reference's:
    // the stand-in discards every other slot's real shading over one slot the
    // shader may not even use. Same trade as the unbound-sampler path above,
    // for the same reason.
    if (source.sample_as_zero) {
      out_textures[slot] = UnboundTexturePayload();
      ++g_slotBoundUnbound;
      ReportBoundZero();
      return true;
    }
    ++g_slotFailDescribe;
    ReportSlotFailures();
    return false;
  }
  NoteSignedBind(source);
  NotePackedBase(source);
  NoteMipCensus();
  // WHICH guest surface does each sampler slot actually ask for?
  //
  // Traced from the rider's gear rendering green. Its material computes
  // saturate(tex5.y + rcp(luminance(tex4))) and the saturate pins at 1, which
  // zeroes the red channel. Red survives only if that luminance exceeds ~1.03.
  //
  // tex4 resolves to the pre-pass band snapshot, whose content is written by
  // the full-screen ambient lighting draw. That pass sums six directional
  // lights whose colours are c149/151/153/155/157/159 -- measured, sane, and
  // identical across captures -- and their red channels total 0.619. That is a
  // hard ceiling with every dot product at 1.0 simultaneously, which opposing
  // directions make impossible; the measured value is 0.109.
  //
  // So with that surface as tex4 the red channel can NEVER survive, on any
  // hardware, with correct constants. The arithmetic does not merely say the
  // input is dark -- it says it is the WRONG SURFACE. The gained main-pass
  // scene holds 32.6 at the same pixel, and feeding that in yields
  // saturate(0.033 + 0.029) = 0.062, a red multiplier of 0.938: yellow gear.
  //
  // Binding is by guest OBJECT (DeviceState().texture[sampler].object looked up
  // in g_resolvedTextureTargets), so we follow whatever the guest bound. This
  // says what that is: the object, whether a resolve ever named it, and the
  // fetch constant's own address and extent -- enough to tell "the guest asked
  // for the pre-pass" from "the guest asked for the scene and we handed it the
  // pre-pass".
  //
  // The ADDRESS fallback, resolved BEFORE the log rather than after it.
  //
  // `resolved=` below reports only the OBJECT lookup, and this used to run
  // afterwards -- so a slot whose guest object is not a registered resolve
  // destination printed resolved=0 whether or not the address match then
  // rescued it. Two very different outcomes, one field, and the field named the
  // one that does not decide anything.
  //
  // That is exactly the composite's case: ps 0x215F8620 slot 0 binds object
  // 0x7010F7F0 while three other shaders bind object 0x2123C2A4 for the SAME
  // guest address 0x1EDA0000 -- one guest texture object aliasing another's
  // resolve destination. Whether that slot samples the depth snapshot or a
  // zero-decoding guest allocation was unanswerable from the log.
  const ResolvedTargetByAddress* addr_match = ResolvedTargetForAddress(source);

  // Deduplicated per (shader, slot) and capped: one line per distinct binding,
  // not per draw.
  {
    static std::mutex s_mu;
    static std::set<uint64_t> s_seen;
    static uint32_t s_lines = 0;
    // Keyed on the handle of the stage this slot belongs to, and tagged with
    // the stage. Keying both stages on `dc.pixel_shader_handle` hid the one
    // binding under investigation: the terrain depth prepass runs the depth-only
    // pixel stand-in and carries no pixel handle, so its VERTEX slot hashed to
    // (0 << 8) | 0 and was deduped away against the first pixel slot 0 ever
    // seen. Three runs went by with the terrain's heightmap address unprinted.
    const uint64_t key = (uint64_t(stage_handle) << 9) |
                         (uint64_t(vertex ? 1u : 0u) << 8) | slot;
    bool fresh = false;
    {
      std::lock_guard<std::mutex> lk(s_mu);
      fresh = s_lines < 4096 && s_seen.insert(key).second;
      if (fresh) ++s_lines;
    }
    if (fresh) {
      REXLOG_INFO(
          "d3d9: SLOT MAP {} 0x{:08X} slot {} (guest sampler {}): object "
          "0x{:08X} resolved={} mostly_written={} addr_match={} (dest 0x{:08X})"
          " | fetch addr 0x{:08X} {}x{} fmt {} bytes {} swizzle {:#o} signs"
          " {:#x}",
          vertex ? "vs" : "ps", stage_handle, slot, guest_sampler,
          texture_state.object,
          texture_state.object &&
                  g_resolvedTextureTargets.contains(texture_state.object)
              ? 1
              : 0,
          partial_snapshot_object ? 0 : 1, addr_match ? 1 : 0,
          addr_match ? addr_match->dest_object : 0u, source.address,
          source.width, source.height, source.guest_format,
          source.source_bytes, source.swizzle, source.signs);
    }
  }
  // Permuted into host component order here, at the bind, because this is
  // per-binding state: the same guest memory is sampled with different sign
  // modes by different draws. Applied by the shader after the fetch, which is
  // where it has to happen -- see the note in EmitTextureFetch.
  //
  // Only kUnsignedBiased rides this. kSigned would need the texture's bits
  // reinterpreted into a signed host format, and kGamma is a curve rather than
  // a scale; both are counted by NoteUnhandledSign and left alone rather than
  // approximated.
  //
  // "the census over a full menu run finds no kGamma at all and kSigned on one
  // FMT_4_4_4_4 texture" used to stand here, and it is wrong the way
  // measure-with-a-level-loaded is always wrong: with a LEVEL up the counter
  // reads `guest format 24 mode signed x20000`, and guest format 24 is k_16 --
  // the terrain heightmap. k_16 now picks an SNORM host view the way k_16_16
  // already did, so it no longer arrives here.
  {
    const uint8_t swizzled =
        mx::hle::SwizzleTextureSigns(source.signs, source.swizzle);
    uint8_t biased = 0;
    for (uint32_t c = 0; c < 4; ++c) {
      const uint32_t mode = (swizzled >> (c * 2)) & 3u;
      if (mode == uint32_t(xn::TextureSign::kUnsignedBiased))
        biased |= uint8_t(1u << c);
      else if (mode != uint32_t(xn::TextureSign::kUnsigned) &&
               !IsFloatGuestFormat(source.guest_format))
        NoteUnhandledSign(source.guest_format, mode);
    }
    out_signs[slot] = biased;
  }

  // The same memory a resolve wrote into, reached through a different texture
  // object than the one the resolve named -- so the object test above missed
  // it. Sample the snapshot rather than decoding guest memory the GPU wrote and
  // the emulator never populated, which reads as zeros and paints black.
  //
  // Placed after the describe because the address and extent it matches on come
  // out of it, and before the decode because the decode is precisely what has
  // to be skipped.
  // Decided from the same lookup the log above reported, not a second call:
  // one question, one answer, so the line cannot say something the binding then
  // contradicts.
  if (addr_match) {
    out_objects[slot] = addr_match->dest_object;
    ++g_resolveAddr.matches;
    return true;
  }

  // NOTE the empty-texture set is deliberately NOT consulted here.
  //
  // It is consulted by the single-texture path, which CHOOSES one sampler to
  // represent the draw -- there, skipping a blank candidate is right, because
  // a blank one is a poor representative of a shader that reads several.
  //
  // This path does not choose. The translated shader NAMES this sampler, and
  // the guest bound a texture to it that decodes, from readable memory, to
  // zeros. Then zero is the value the guest's own shader samples, and black is
  // the correct answer. Refusing it reverted the whole draw to the tex*col
  // stand-in -- discarding every other slot's real shading to avoid a black
  // sample that was never wrong. That cost 10,890 draws in mx_706, from just
  // nine distinct all-zero textures, because the refusal is cached per key.
  const uint64_t key = HleTextureKey(fetch);
  TexMissReason miss_reason = TexMissReason::kNotInCache;
  if (auto cached = g_hleCpuTextures.find(key);
      cached != g_hleCpuTextures.end()) {
    // A glyph atlas the guest has repacked since this was decoded falls
    // through to a fresh decode; everything else is served from the cache.
    bool stale = false;
    {
      PhaseTimer t(g_tex.staleUs);
      stale = TextureContentStale(source, base, *cached->second);
    }
    if (!stale) {
      ++g_tex.cacheHits;
      out_textures[slot] = cached->second;
      return true;
    }
    // Counted apart from a plain miss. A key that is present but keeps testing
    // stale is a re-decode every bind, which costs the same as having no cache
    // at all while looking like a working one from the outside.
    ++g_tex.staleEvicts;
    miss_reason = TexMissReason::kStaleEvicted;
    g_hleCpuTextures.erase(cached);
  } else if (g_hleEmptyTextures.count(key)) {
    // Never entered the cache because it decoded blank; the backoff above let
    // it through for another look.
    miss_reason = TexMissReason::kBlankRetry;
  }
  // A key found blank recently and not yet due another look: bind the decode
  // the earlier draw made rather than repeating it.
  if (!BlankRetryDue(key)) {
    // Known blank and not due a re-read: the snapshot's tiles are the best
    // available answer until the retry says otherwise.
    //
    // Gated on the BLANK SET, not on g_hleBlankPayloads. It used to require a
    // blank payload, and that made this branch unreachable for precisely the
    // textures it exists to serve: the blank path below returns as soon as it
    // has a snapshot to bind, so a key with a snapshot never gets a payload
    // recorded, so `contains` was never true, so every bind fell through and
    // re-decoded. Measured on 0x1A2E3000 -- 2 x 16 MB every frame, 21 GB over a
    // 100-frame run, and forcing BlankRetryDue to false changed nothing at all,
    // which is what proved the guard rather than the backoff was the problem.
    //
    // The payload was never needed here in the first place: this binds an
    // OBJECT and does not read one.
    if (partial_snapshot_object && g_hleEmptyTextures.count(key)) {
      out_objects[slot] = partial_snapshot_object;
      return true;
    }
    if (auto payload = g_hleBlankPayloads.find(key);
        payload != g_hleBlankPayloads.end()) {
      out_textures[slot] = payload->second;
      ++g_slotBoundZero;
      NoteBlankTexture(key, guest_sampler, source);
      ReportBoundZero();
      return true;
    }
  }
  std::vector<uint8_t> guest;
  bool copied = false;
  {
    PhaseTimer t(g_tex.copyUs);
    copied = CopyTexturePhysical(source, base, guest);
  }
  if (!copied) {
    ++g_slotFailCopy;
    ReportSlotFailures();
    return false;
  }
  auto payload = std::make_shared<HleTexturePayload>();
  bool decoded_ok = false;
  {
    PhaseTimer t(g_tex.decodeUs);
    decoded_ok =
        DecodeHleTexture2D(source, guest.data(), guest.size(), *payload, &why);
  }
  ++g_tex.decodes;
  g_tex.decodedBytes += guest.size();
  {
    TexDecodeSite& site = g_texIndex.sites[source.address];
    ++site.decodes;
    site.bytes += guest.size();
    site.width = source.width;
    site.height = source.height;
    site.format = source.guest_format;
    ++site.by_reason[size_t(miss_reason)];
    // How many DISTINCT cache keys one guest address has produced. Greater
    // than one means the same bytes are being decoded under several keys --
    // the fetch-constant hash splitting on sampler state -- which is a
    // different fix from a texture whose content genuinely changes.
    if (g_texIndex.keys.emplace(source.address, key).second)
      ++site.distinct_keys;
  }
  if (!decoded_ok) {
    ++g_slotFailDecode;
    ReportSlotFailures();
    return false;
  }
  NoteSwizzleCensus(source);
  NoteMipLevelAgreement(*payload);
  // Still recorded, so the single-texture path above keeps skipping it as a
  // representative and the count stays visible -- but no longer a refusal here.
  // A decode that carries no information. Two forms, and until 2026-08-14 only
  // the first was recognised.
  //
  //   all zero  -- the long-known case: storage the guest has not filled yet.
  //   UNIFORM   -- every byte the same non-zero value. Guest memory that the CPU
  //                never writes at all reads back whatever is there, and for the
  //                terrain heightmap that is 0xFF: a uniformly white 2048x2048
  //                (gameplay-9.rdc ResourceId::733). It passes the nonzero test
  //                as real data, so no `bound-zero` line was ever printed for it
  //                and this fallback never fired. The vertex stage read a
  //                constant 1.0, the terrain came out flat at world Y = 1 under
  //                a camera at Y = 616, and the whole ground sat 615 units below
  //                the view at far-plane depth.
  //
  // The uniform form is only treated as empty when `partial_snapshot_object` is
  // set -- i.e. this surface IS a resolve destination the GPU has written part
  // of, so a better source demonstrably exists. A flat texture with no snapshot
  // behind it is legal and is left alone; that restraint is what keeps this from
  // becoming the blanket substitution the notes above record as a regression.
  const bool decode_is_blank = [&] {
    PhaseTimer t(g_tex.scanUs);
    return !HleTextureHasNonzeroData(*payload);
  }();
  uint8_t uniform_value = 0;
  // Detected UNCONDITIONALLY, acted on only below. The first cut of this
  // computed `decode_is_uniform` as `partial_snapshot_object && ...` and put its
  // log line inside the `if (partial_snapshot_object)` branch -- so when the
  // question was "is partial_snapshot_object even set for this slot?", the
  // diagnostic that would answer it could not print. mx_1148 reached freeroam
  // with the terrain still flat and not one line to say which half had failed.
  // Exactly the shape of [[counter-that-cannot-fire]], committed twice in one
  // session. Measure first, gate second.
  const bool decode_is_uniform = !decode_is_blank && [&] {
    PhaseTimer t(g_tex.scanUs);
    return HleTextureIsConstant(*payload, &uniform_value);
  }();
  if (decode_is_uniform) {
    static std::set<uint64_t> s_uniform;
    if (s_uniform.insert(key).second && s_uniform.size() <= 32) {
      REXLOG_INFO(
          "d3d9: uniform decode 0x{:02X} for sampler {} {}x{} guest format {} "
          "addr 0x{:08X} -- carries no data; snapshot to fall back on: {}",
          uniform_value, guest_sampler, source.width, source.height,
          source.guest_format, source.address,
          partial_snapshot_object
              ? fmt::format("0x{:08X}", partial_snapshot_object)
              : std::string("NONE, binding the constant anyway"));
    }
  }
  // REVERTED 2026-08-14: `decode_is_uniform` used to be admitted here alongside
  // the blank case, on the theory that a uniformly-0xFF decode was as empty as
  // an all-zero one. The texture that motivated it was MISIDENTIFIED -- it was
  // picked out of a RenderDoc resource list by size and format, and the real
  // sampler read a different resource entirely. Every uniform decode measured
  // since has reported no snapshot to fall back on, so the branch never once
  // fired; it is removed rather than left in as a plausible-looking no-op.
  //
  // The DETECTION stays, and its log line with it: a uniform decode is worth
  // knowing about, and the line now says outright whether a fallback exists.
  if (decode_is_blank) {
    NoteBlankDecode(key);
    // Memory had nothing and a partly-written snapshot exists: its resolved
    // tiles beat a constant everywhere, and the blank is still recorded above so
    // the retry keeps looking.
    if (partial_snapshot_object) {
      out_objects[slot] = partial_snapshot_object;
      return true;
    }
    ++g_slotBoundZero;
    NoteBlankTexture(key, guest_sampler, source);
    ReportBoundZero();
    // Bind the zero for THIS draw -- that trade is settled -- but do not cache
    // it. The cache key is FNV-1a over the six fetch dwords alone, so it hashes
    // where the texture lives and what shape it is, never what it contains;
    // inserting a blank decode under that key froze the texture black for the
    // rest of the run, because nothing in the tree ever erases or versions an
    // entry. A texture sampled once while the guest was still streaming into it
    // could therefore never appear. Leaving it out means the next draw re-reads
    // guest memory and the texture shows up the frame its upload completes.
    //
    // The host cache in EnsureGameTexture keys on payload->key too, so the
    // blank upload must not sit on the key the recovered texture will use. It
    // is given a marked key of its own; the real decode arrives under the
    // unmarked key and uploads as a new resource rather than hitting the black
    // one.
    payload->key = key ^ kBlankTextureKeyMarker;
    g_hleBlankPayloads[key] = payload;
    out_textures[slot] = std::move(payload);
    return true;
  }
  NoteBlankRecovered(key);
  payload->key = key;
  payload->content_version =
      TextureContentVersion(source, base, payload->format);
  mx::diag::DumpDecodedTexture(source, *payload, "slot", guest_sampler);
  out_textures[slot] = payload;
  g_hleCpuTextures.emplace(key, std::move(payload));
  return true;
}

// Overlay a pixel shader's EMBEDDED constants onto the bank read from the
// device shadow.
//
// The shadow at device+0x1780 is not the whole story, and reading it alone is
// why every 3D material in this title rendered black. A shader object carries
// its own constant-load table, and the draw-time flush publishes it straight to
// the GPU by address, never through the shadow:
//
//   sub_82565928, the draw flush, does
//       v8 = *(device + 12868);              // 0x3244, the PIXEL shader
//       sub_825656A0(device, v8 + 10, v8[6]) // table at ps+0x28, data *(ps+0x18)
//       v6 = *(device + 12872);              // 0x3248, the VERTEX shader
//       sub_825656A0(device, v6 + 218, v6[8])// table at vs+0x368, data *(vs+0x20)
//
//   and sub_825656A0 walks, from `rel = *(u32*)(table + 0x14)`, a block at
//   `table + rel` whose entry list starts at +0x14 and runs *(u32*)(+0x10)
//   bytes:
//       entry: u16 reg_index, u16 dword_count, u32 data_offset   (8 bytes)
//       terminated by dword_count == 0
//       source  = data_offset + data_base
//       emitted as PM4 0xC0022F00 (LOAD_ALU_CONSTANT) with body
//       [physical(source), 4 * reg_index, dword_count]
//
// The vertex half of this was already understood -- see
// ProbeVertexShaderConstantPatch, which recorded that the data "never passes
// through device + 0x780". What was missed is that the PIXEL shader has the
// same mechanism at DIFFERENT offsets, so the pixel bank was left with only the
// registers SetPixelShaderConstantF happens to write. Measured over 264 dumped
// shaders: 242 read constants above c217, which the shadow never contains, and
// the 22 that read only written registers are exactly the UI shaders that
// always looked correct.
//
// `reg` is an ALU float4 constant index. The emitted packet uses `4 * reg` as
// its dword offset from register 0x4000, so pixel constants 256..511 map to this
// bank at reg-256. Texture fetch constants do NOT live in this list; they are
// in the second, inline state-patch list walked by ApplyShaderFetchPatchTable.
void ApplyShaderLoadTable(uint32_t shader, uint32_t table_at, uint32_t data_at,
                          uint8_t* base, std::vector<uint32_t>& bank) {
  if (!shader || bank.size() < 256 * 4) return;
  const uint32_t kPsLoadTableAt = table_at;
  const uint32_t kPsDataBaseAt = data_at;
  const uint32_t table = shader + kPsLoadTableAt;
  if (!HostPageReadable(REX_RAW_ADDR(table + 0x14)) ||
      !HostPageReadable(REX_RAW_ADDR(shader + kPsDataBaseAt)))
    return;
  const uint32_t rel = REX_LOAD_U32(table + 0x14);
  if (!rel || rel >= 0x10000) return;
  const uint32_t block = table + rel;
  if (!HostPageReadable(REX_RAW_ADDR(block + 0x10))) return;
  const uint32_t bytes = REX_LOAD_U32(block + 0x10);
  if (!bytes || bytes >= 0x10000) return;
  const uint32_t data_base = REX_LOAD_U32(shader + kPsDataBaseAt);
  if (!data_base) return;

  uint32_t at = block + 0x14;
  const uint32_t end = at + bytes;
  uint32_t applied = 0, entries = 0;
  std::string first;
  while (at + 8 <= end && HostPageReadable(REX_RAW_ADDR(at + 4))) {
    const uint32_t hdr = REX_LOAD_U32(at);
    const uint32_t reg = hdr >> 16;
    const uint32_t dwords = hdr & 0xFFFF;
    if (!dwords) break;
    const uint32_t data_off = REX_LOAD_U32(at + 4);
    at += 8;
    ++entries;
    const uint32_t src = data_base + data_off;
    for (uint32_t j = 0; j < dwords; ++j) {
      const uint32_t abs_reg = reg + j / 4;
      if (abs_reg < 256 || abs_reg >= 512) continue;
      const uint32_t dst = (abs_reg - 256) * 4 + (j % 4);
      if (dst >= bank.size()) continue;
      if (!HostPageReadable(REX_RAW_ADDR(src + j * 4))) continue;
      bank[dst] = REX_LOAD_U32(src + j * 4);
      ++applied;
    }
    if (entries <= 4)
      first += fmt::format(" c{}..c{}", reg, reg + (dwords + 3) / 4 - 1);
  }
}

// Read the second list in the shader patch block -- the one SetPixelShader and
// SetVertexShader memcpy directly into `device + 0x480`.
void ApplyShaderFetchPatchTable(uint32_t shader, uint32_t table_at,
                                uint8_t* base, ShaderFetchConstants& fetch) {
  if (!shader) return;
  const uint32_t table = shader + table_at;
  if (!HostPageReadable(REX_RAW_ADDR(table + 0x14))) return;
  const uint32_t rel = REX_LOAD_U32(table + 0x14);
  if (!rel || rel >= 0x10000) return;
  const uint32_t block = table + rel;
  if (!HostPageReadable(REX_RAW_ADDR(block + 0x10))) return;
  const uint32_t bytes = REX_LOAD_U32(block + 0x10);
  if (!bytes || bytes >= 0x10000) return;

  uint32_t at = block + 0x14;
  const uint32_t end = at + bytes;

  // First list: `(u16 reg, u16 dwords, u32 data_offset)`, terminated by a
  // zero dword count. This is the LOAD_ALU_CONSTANT list handled above.
  while (at + 4 <= end && HostPageReadable(REX_RAW_ADDR(at + 2))) {
    const uint32_t dwords = REX_LOAD_U16(at + 2);
    at += 4;
    if (!dwords) break;
    if (at + 4 > end) return;
    at += 4;
  }

  // Second list: inline device-shadow copies. Offset zero is fetch constant 0;
  // six dwords later begins fetch constant 1. This retained only the first 16
  // while noting that "the guest block itself contains all 32" — kMaxSamplers is
  // now 32 and the truncation is gone. See the note on that constant.
  constexpr uint32_t kFetchBytes = mx::hle::kMaxSamplers *
                                   ShaderFetchConstants::kDwords * 4;
  while (at + 4 <= end && HostPageReadable(REX_RAW_ADDR(at + 2))) {
    const uint32_t byte_offset = REX_LOAD_U16(at);
    const uint32_t dwords = REX_LOAD_U16(at + 2);
    at += 4;
    if (!dwords) break;
    const uint64_t payload_bytes = uint64_t(dwords) * 4;
    if (payload_bytes > uint64_t(end - at)) return;
    for (uint32_t j = 0; j < dwords; ++j) {
      const uint64_t dst_byte = uint64_t(byte_offset) + uint64_t(j) * 4;
      if (dst_byte >= kFetchBytes) continue;
      const uint32_t src = at + j * 4;
      if (!HostPageReadable(REX_RAW_ADDR(src))) continue;
      const uint32_t fetch_dword = uint32_t(dst_byte / 4);
      const uint32_t sampler =
          fetch_dword / ShaderFetchConstants::kDwords;
      const uint32_t component =
          fetch_dword % ShaderFetchConstants::kDwords;
      fetch.words[fetch_dword] = REX_LOAD_U32(src);
      fetch.partial[sampler] |= uint8_t(1u << component);
      if (fetch.partial[sampler] == 0x3F)
        fetch.complete_mask |= 1u << sampler;
    }
    at += uint32_t(payload_bytes);
  }
}

// Both shaders publish into the SAME unified ALU constant file -- the draw
// flush calls sub_825656A0 once for the pixel shader and once for the vertex
// shader -- so an entry in EITHER table whose register lands in 256..511 is a
// pixel constant. Taking only the pixel shader's table left c43 and c85 at
// zero, which is most of a material's shading still missing.
// The fetch patches are gathered from BOTH shader objects because both binding
// calls write the same device constants block. They are keyed by the PIXEL
// shader handle because that is the identity the draw's texture resolver owns.
// Print a shader's constant-load table verbatim, so a register that reaches the
// shader with a suspicious value can be checked against what the table actually
// claims to write. Mirrors ApplyShaderLoadTable's walk exactly rather than
// sharing it, so removing this again cannot disturb the working path.
//
// Recovered by hash from 1078e5f, where it was deleted as dead code once the
// c100 NaN question closed. It is the tool the red-screen question asks for.
void DumpShaderLoadTable(uint32_t shader, uint32_t table_at, uint32_t data_at,
                         uint8_t* base, const char* tag) {
  if (!shader) return;
  const uint32_t table = shader + table_at;
  if (!HostPageReadable(REX_RAW_ADDR(table + 0x14)) ||
      !HostPageReadable(REX_RAW_ADDR(shader + data_at))) {
    REXLOG_INFO("d3d9:   {} table 0x{:08X}: unreadable", tag, shader);
    return;
  }
  const uint32_t rel = REX_LOAD_U32(table + 0x14);
  if (!rel || rel >= 0x10000) {
    REXLOG_INFO("d3d9:   {} table 0x{:08X}: rel=0x{:X} rejected", tag, shader,
                rel);
    return;
  }
  const uint32_t block = table + rel;
  if (!HostPageReadable(REX_RAW_ADDR(block + 0x10))) return;
  const uint32_t bytes = REX_LOAD_U32(block + 0x10);
  const uint32_t data_base = REX_LOAD_U32(shader + data_at);
  std::string entries;
  uint32_t at = block + 0x14;
  const uint32_t end = at + bytes;
  uint32_t n = 0;
  while (at + 8 <= end && HostPageReadable(REX_RAW_ADDR(at + 4))) {
    const uint32_t hdr = REX_LOAD_U32(at);
    const uint32_t reg = hdr >> 16;
    const uint32_t dwords = hdr & 0xFFFF;
    if (!dwords) break;
    const uint32_t data_off = REX_LOAD_U32(at + 4);
    at += 8;
    if (++n <= 24)
      entries += fmt::format(" c{}+{}dw@0x{:X}", reg, dwords,
                             data_base + data_off);
  }
  REXLOG_INFO("d3d9:   {} 0x{:08X} bytes={} data=0x{:08X} {} entries:{}", tag,
              shader, bytes, data_base, n, entries);
}

// THE RED SCREEN. A 256x256 warm radial gradient is multiplied 45.5x into the
// HDR scene target by two scalar broadcasts, and its alpha falloff is killed by
// a third constant reading zero:
//
//   mul r0.xyz, r0.xyz, xe_c[255].xxxx   -> x 3.0          guest c511
//   mul r0.xyz, r0.xyz, xe_c[43].xxxx    -> x 15.178571     guest c299
//   o0.w = r0.w * xe_c[9].x              -> x 0             guest c265
//
// The pixel bank is rebased -- xe_c[N] is guest ALU constant 256+N -- and large
// runs of it read exactly zero, which is the known shape of a constant we
// failed to publish ([[shader-embedded-constants]]: shaders DMA their own ALU
// constants and the device shadow is only half the bank).
//
// So the question is not "is 45.5 too big" but "is 45.5 what the guest asked
// for". A shader's own load table answers it: if the table claims one of these
// registers and the bank disagrees, we are landing in the wrong slot; if it
// does not claim them, these are genuinely the guest's values and the
// over-brightness is downstream, in the tonemap.
//
// Once per distinct pixel shader, because a handle is an address and this is
// looking for a shader it cannot name in advance
// ([[shader-handles-are-not-stable]]).
void NoteRedScreenConstants(uint32_t shader, uint32_t device, uint8_t* base,
                            const std::vector<uint32_t>& bank) {
  if (!shader) return;
  static std::mutex s_mu;
  static std::set<uint32_t> s_seen;
  {
    std::lock_guard<std::mutex> lock(s_mu);
    if (s_seen.size() >= 256 || !s_seen.insert(shader).second) return;
  }
  // Guest register -> index in the bank, and what we ended up with. The bank is
  // REBASED: it holds the pixel half only, 256 registers, so guest c511 lives
  // at index 255 -- the same rebasing ApplyShaderLoadTable applies at its
  // `(abs_reg - 256) * 4`. Indexing it by raw guest register reads off the end.
  const auto at = [&](uint32_t guest_reg) {
    if (guest_reg < 256) return std::string("not-a-pixel-reg");
    const size_t i = size_t(guest_reg - 256) * 4;
    if (i + 3 >= bank.size()) return std::string("out-of-range");
    float v[4];
    for (uint32_t c = 0; c < 4; ++c) std::memcpy(&v[c], &bank[i + c], 4);
    return fmt::format("({:g},{:g},{:g},{:g})", v[0], v[1], v[2], v[3]);
  };
  REXLOG_INFO("d3d9: RED SCREEN constants ps 0x{:08X}: c265={} c299={} c511={}",
              shader, at(265), at(299), at(511));
  DumpShaderLoadTable(shader, 0x28, 0x18, base, "ps");
  constexpr uint32_t kDeviceVertexShaderAt = 0x3248;
  if (device && HostPageReadable(REX_RAW_ADDR(device + kDeviceVertexShaderAt))) {
    const uint32_t vs = REX_LOAD_U32(device + kDeviceVertexShaderAt);
    if (vs) DumpShaderLoadTable(vs, 0x368, 0x20, base, "vs");
  }
}

void ApplyPixelShaderLoadTable(
    uint32_t shader, uint32_t device, uint8_t* base,
    std::vector<uint32_t>& bank) {
  ShaderFetchConstants fetch;
  // Same repair as the vertex side, before the load tables. The pixel bank is
  // guest c256..c511, so first_reg is 256 — this is the one that matters, since
  // the measured NaN block is c392..c395 and lands here as xe_c[136..139].
  if (bank.size() >= 256 * 4)
    mx::gpu::alu::OverlayNonFinite(256, bank.data(), 256,
                                   /*count_finite_zeros=*/true);
  ApplyShaderLoadTable(shader, 0x28, 0x18, base, bank);
  ApplyShaderFetchPatchTable(shader, 0x28, base, fetch);
  // device+0x3248 is the vertex shader object; its table sits at +0x368 with
  // its data base at +0x20.
  constexpr uint32_t kDeviceVertexShaderAt = 0x3248;
  if (device && HostPageReadable(REX_RAW_ADDR(device + kDeviceVertexShaderAt))) {
    const uint32_t vs = REX_LOAD_U32(device + kDeviceVertexShaderAt);
    if (vs) {
      ApplyShaderLoadTable(vs, 0x368, 0x20, base, bank);
      ApplyShaderFetchPatchTable(vs, 0x368, base, fetch);
    }
  }
  // After both tables have been applied, so the bank it reports is the one the
  // shader will actually see.
  NoteRedScreenConstants(shader, device, base, bank);
  if (!shader || !fetch.complete_mask) return;
  bool first = false;
  {
    std::lock_guard<std::mutex> lock(g_shaderFetchMu);
    first = g_shaderFetch.insert_or_assign(shader, fetch).second;
  }
  if (!first) return;
  // Once per shader that publishes any complete descriptor. This is what
  // confirms which shader objects actually embed complete descriptors, so it
  // names the samplers rather than merely counting them.
  ++g_shaderFetchPublished;
  std::string list;
  for (uint32_t s = 0; s < mx::hle::kMaxSamplers; ++s)
    if (fetch.complete_mask & (1u << s)) list += fmt::format(" s{}", s);
  REXLOG_INFO("d3d9: ps 0x{:08X} publishes its own texture fetch constants:{}",
              shader, list);
}

// Attach the guest's own translated pixel shader to a draw: its source, its
// constant bank, and one texture per sampler slot it declares.
//
// Extracted so it can run BEFORE the single-texture binding contest as well
// as after it. The contest answers a different question -- which one texture
// the tex*col stand-in should sample -- and used to gate this, which meant a
// shader fetching no texture at all never got here.
void AttachTranslatedPixelShader(mx::hle::DrawCall& dc, uint32_t handle,
                                 uint32_t device, uint8_t* base) {
  using namespace mx::hle;
  dc.pixel_shader_handle = handle;
  // Census of resolve destinations bound at THIS draw, before any of the slot
  // logic below can filter them out. See the fields' note in the header: this
  // is deliberately outside the sampler loop, because the whole point is to see
  // destinations that loop never reaches.
  {
    NoteDrawThread();
    const TranslatedShader* census_t = TranslatedPixelShader(handle);
    uint32_t declared = 0;
    if (census_t) {
      for (uint32_t s = 0; s < census_t->sampler_count &&
                           s < mx::hle::DrawCall::kMaxPixelTextures; ++s)
        declared |= 1u << (census_t->slot_guest[s] & 31u);
    }
    auto& st = DeviceState();
    for (uint32_t gs = 0; gs < kMaxSamplers; ++gs) {
      const uint32_t obj = st.texture[gs].object;
      if (!obj || !g_resolvedTextureTargets.contains(obj)) continue;
      if (auto* e = ResolveEntryForObject(obj)) {
        ++e->draws_while_bound;
        e->declared_sampler_mask |= declared;
        if (!census_t) ++e->draws_no_translation;
      }
    }
  }
  // Draws whose bound shader has no translation at all -- the other half of the
  // stand-in population, the half slot-filling cannot explain. Counted per
  // HANDLE because coverage is measured per shader and the picture is painted
  // per draw: 23 untranslated shaders out of 256 is a small share of the
  // former and may be a large share of the latter, and only this says which.
  if (!TranslatedPixelShader(handle)) {
    static std::map<uint32_t, uint64_t> s_byHandle;
    static uint64_t s_total = 0;
    ++s_total;
    ++s_byHandle[handle];
    // Every 500, not 5000: at 5000 this printed NOTHING across a 1943-frame run
    // while 25,359 draws took the stand-in -- silence that reads identically to
    // zero, and which sent the search to the wrong place until the arithmetic
    // was checked against the earlier exit.
    if ((s_total % 500) == 0) {
      std::vector<std::pair<uint64_t, uint32_t>> top;
      for (const auto& [h, n] : s_byHandle) top.emplace_back(n, h);
      std::sort(top.rbegin(), top.rend());
      std::string worst;
      for (size_t i = 0; i < top.size() && i < 6; ++i)
        worst += fmt::format(" 0x{:08X}={}", top[i].second, top[i].first);
      REXLOG_INFO("d3d9: draws with an UNTRANSLATED pixel shader: {} over {} "
                  "handles; worst:{}",
                  s_total, s_byHandle.size(), worst);
    }
  }
  if (const TranslatedShader* t = TranslatedPixelShader(handle)) {
    dc.pixel_shader_hlsl = t->source;
    dc.pixel_shader_dxbc = t->dxbc;
    dc.pixel_sampler_count = t->sampler_count;
    dc.pixel_sampler_array_mask = t->sampler_array_mask;
    // The PIXEL constant bank, ALU constants 256-511 at device+0x1780. Captured
    // per draw because the guest rewrites it between draws, and captured only
    // for a shader that will use it. Its base is applied here so the shader can
    // index from 0 — see DrawCall::pixel_constants.
    constexpr uint32_t kPixelConstBase = 0x1780;
    constexpr uint32_t kPixelConstRegs = 256;
    if (HostPageReadable(REX_RAW_ADDR(device + kPixelConstBase)) &&
        HostPageReadable(
            REX_RAW_ADDR(device + kPixelConstBase + kPixelConstRegs * 16 - 4))) {
      dc.pixel_constants.resize(kPixelConstRegs * 4);
      for (uint32_t i = 0; i < kPixelConstRegs * 4; ++i)
        dc.pixel_constants[i] = REX_LOAD_U32(device + kPixelConstBase + i * 4);
      // The shadow is only half the bank. Overlay the shader's own literals.
      ApplyPixelShaderLoadTable(handle, device, base, dc.pixel_constants);

      // One texture per slot the shader declares. A shader whose slots cannot
      // all be filled keeps the stand-in: running it with a missing texture
      // would sample whatever descriptor happened to be at that index, which is
      // a confident wrong answer rather than a visible failure.
      uint32_t filled = 0;
      for (uint32_t s = 0; s < t->sampler_count &&
                           s < mx::hle::DrawCall::kMaxPixelTextures; ++s) {
        if (ResolvePixelSlotTexture(dc, s, t->slot_guest[s], device, base))
          ++filled;
      }
      static uint64_t s_slotOk = 0, s_slotShort = 0;
      if (filled < t->sampler_count) {
        ++s_slotShort;
        dc.pixel_shader_hlsl.reset();
        dc.pixel_shader_dxbc.reset();
        dc.pixel_textures = {};
        dc.pixel_sampled_objects = {};
      } else {
        ++s_slotOk;
      }

      // The VERTEX stage's textures, by exactly the same rule. A vertex shader
      // that samples -- terrain displacement is the case here -- used to be
      // refused the GPU path for having a sampler at all, and the interpreter
      // it fell back to has no texture fetch, so its samples were silent zeros
      // and the positions silently wrong.
      //
      // All-or-nothing like the pixel stage, and for the same reason: a slot
      // left unfilled samples whatever descriptor sits at that index. Falling
      // short here clears the count rather than the shader, which puts the draw
      // back on the CPU path it used to take unconditionally.
      constexpr uint32_t kDeviceVertexShaderAt = 0x3248;
      uint32_t vs_handle = 0;
      if (HostPageReadable(REX_RAW_ADDR(device + kDeviceVertexShaderAt)))
        vs_handle = REX_LOAD_U32(device + kDeviceVertexShaderAt);
      if (const TranslatedShader* vt =
              vs_handle ? TranslatedVertexShader(vs_handle) : nullptr) {
        if (vt->sampler_count) {
          uint32_t vfilled = 0;
          for (uint32_t s = 0; s < vt->sampler_count &&
                               s < mx::hle::DrawCall::kMaxPixelTextures; ++s) {
            if (ResolvePixelSlotTexture(dc, s, vt->slot_guest[s], device, base,
                                        /*vertex=*/true, vs_handle))
              ++vfilled;
          }
          static uint64_t s_vsSlotOk = 0, s_vsSlotShort = 0;
          if (vfilled < vt->sampler_count) {
            ++s_vsSlotShort;
            dc.vertex_textures = {};
            dc.vertex_sampled_objects = {};
            dc.vertex_sampler_count = 0;
          } else {
            ++s_vsSlotOk;
            dc.vertex_sampler_count = vt->sampler_count;
            dc.vertex_sampler_array_mask = vt->sampler_array_mask;
          }
          if (((s_vsSlotOk + s_vsSlotShort) % 5000) == 1) {
            REXLOG_INFO("d3d9: VERTEX texture slots: {} draws bound, {} short",
                        s_vsSlotOk, s_vsSlotShort);
          }
        }
      }
      if (((s_slotOk + s_slotShort) % 5000) == 0) {
        // The resolve-address counters ride here rather than in ReportBoundZero,
        // which only fires every 2500 BLACK draws -- so the moment the black
        // count collapses, the numbers saying whether the address match is
        // doing the work disappear with it. They were invisible in exactly the
        // runs where they mattered most.
        size_t blank_outstanding = 0;
        for (const auto& [key, b] : g_blankTextures)
          if (!b.recovered) ++blank_outstanding;
        REXLOG_INFO("d3d9: translated texture slots: {} draws bound, {} short; "
                    "resolve address matches {} (extent mismatches {}, "
                    "partial-coverage refusals {}), "
                    "blank textures {} still blank of {}",
                    s_slotOk, s_slotShort, g_resolveAddr.matches,
                    g_resolveAddr.extentMiss, g_resolveAddr.partial,
                    blank_outstanding, g_blankTextures.size());
      }
    } else {
      // Without its constants the shader would compute from zeros, which is a
      // confident wrong answer. Drop the translation and keep the stand-in.
      dc.pixel_shader_hlsl.reset();
      dc.pixel_shader_dxbc.reset();
      static uint32_t s_logged = 0;
      if (s_logged++ < 4)
        REXLOG_INFO("d3d9: pixel constant bank unreadable at device+0x{:X}",
                    kPixelConstBase);
    }
  }
}

bool PrepareDrawTexture(mx::hle::DrawCall& dc, uint32_t pixel_shader,
                        uint32_t device, uint8_t* base,
                        mx::hle::PixelTextureBinding& binding) {
  using namespace mx::hle;
  PhaseTimer phase_timer(g_tex.phaseUs);
  static uint64_t s_attempts = 0, s_ready = 0, s_no_shader = 0;
  static uint64_t s_no_binding = 0, s_bad_desc = 0, s_unreadable = 0;
  static uint64_t s_mapped = 0, s_empty = 0, s_semantic_reject = 0;
  static uint64_t s_no_shader_no_setter = 0;
  ++s_attempts;
  // Driven by attempts, not by successes: the summary further down only fires
  // once a texture is ready, so a run in which every descriptor is rejected —
  // precisely the run this tally exists to characterise — would print nothing.
  if ((s_attempts % 2500) == 0 && !g_hleRejectedFormats.empty()) {
    REXLOG_INFO("d3d9: HLE rejected guest texture formats after {} attempts: {}",
                s_attempts, RejectedFormatSummary());
  }
  // WHICH shader is bound is a separate question from WHICH ONE TEXTURE the
  // stand-in should sample, and conflating them cost 63,207 draws in mx_709 --
  // 27.8% of every draw attempted, the largest single cause of stand-in draws
  // by a wide margin.
  //
  // Both resolutions below end in ResolvePixelBindingForDraw, which gives up
  // when `profile->bindings.empty()`. A pixel shader that fetches no texture at
  // all has exactly that -- no bindings -- so it was reported as "no eligible
  // pixel shader" and the draw kept the tex*col stand-in. For a TRANSLATED
  // shader that is precisely backwards: a shader sampling nothing needs no
  // texture, and the stand-in it fell back to is the one thing that does.
  //
  // So resolve the handle first, from the setter or from the device field, and
  // attach the translation on its own terms. The single-binding contest still
  // runs below, unchanged, because the stand-in still needs a winner -- but it
  // can no longer veto a draw that was never going to use its answer.
  uint32_t resolved = pixel_shader;
  if (!resolved && device &&
      HostPageReadable(REX_RAW_ADDR(device + 0x3244)))
    resolved = REX_LOAD_U32(device + 0x3244);
  // Last resort: the device's own last-bound shader, recorded across threads.
  // Both sources above are thread-local in effect -- the argument comes from a
  // thread_local DeviceState, and device+0x3244 read zero for 15,555 draws on a
  // loaded menu. Without this those draws take the tex*col stand-in and paint
  // whatever their first texture happens to be, which for a character material
  // is a packed normal/gloss map.
  //
  // That fallback was WRITTEN AND NEVER CALLED. This comment described it and
  // the code below stopped at device+0x3244, so PixelShaderForDevice sat dead
  // in the file. Measured cost of the omission, mx_890 at the menu: 79,984 of
  // 240,000 draws arrive with no setter handle at all; 133 of them per frame
  // carry 144,097 vertices, and because no translated pixel shader means no GPU
  // vertex path, they run the software interpreter and the CPU attribute decode
  // -- 121ms of a 225ms frame, the single largest item in it.
  // Gated, because it is both the largest speedup this session and a suspected
  // regression, and those have to be separable.
  //
  // It took the menu 4.45 -> 9.88 fps by giving 80,000 draws a translated pixel
  // shader, which is what lets them onto the GPU vertex path instead of the
  // software interpreter. But "unbound by sampler s0/s1/s2" appears in no run
  // before mx_891 and in every run after it, at ~69,000 draws a frame: the
  // shader this attaches declares sampler slots those draws never bound, and
  // each one gets a 1x1 black. Whether that is a wrong shader or a second
  // missing binding is the open question, and one flag answers it.
  // WHOSE device is it? Recorded BEFORE the fallback runs, so it describes the
  // draw as it arrived rather than after being patched up.
  //
  // The 3D layer of the menu is 133 draws a frame with no pixel shader from
  // either source, and both offsets have been confirmed against the guest:
  // D3DDevice_SetPixelShader writes pDevice[1].m_Constants.Fetch[29] —
  // device+0x3244 — which is exactly what was read above. So the field is not
  // being misread; either the shader was never set on THIS device, or the draw
  // is carrying the wrong device pointer.
  //
  // The guest runs three parallel record workers, each driving its own device
  // (dword_830B2C60[0..2]), and this file has twice been caught by state being
  // read from the wrong one of the three. If the draw devices below do not
  // appear among the setter devices, that is the whole defect.
  if (!resolved) {
    // What ARE these draws running? Self-limiting; see ProbeVertexObjectSecondBlob.
    ProbeVertexObjectSecondBlob(device, base);
    // Does a null-pixel-shader draw bind a COLOUR target?
    //
    // The probe above establishes what these draws are: one 48-dword program
    // that writes position and exports no interpolators at all, i.e. a
    // depth-only pass, which is why a null pixel shader is legal for them. The
    // renderer already has a route for that, but it opens only when NO colour
    // target is bound:
    //
    //     depthOnlyPass = !d.targetObject && d.depthObject && ...
    //
    // If these draws carry a colour target too, they miss it, and each one is
    // given a scratch colour target plus the tex*col stand-in -- painting
    // colour the guest never wrote, into the scene buffer the rider's material
    // later samples for its luminance. That would make the stand-in actively
    // harmful here rather than merely a missing feature.
    //
    // Split by which targets are present, and record the colour extents seen,
    // because "binds the 1280x640 scene band" and "binds some small offscreen
    // target" want different answers.
    {
      static std::mutex s_mu;
      static uint64_t s_colour_and_depth = 0, s_depth_only = 0;
      static uint64_t s_colour_only = 0, s_neither = 0;
      static std::set<uint64_t> s_extents;
      // Whether they actually PAINT is a separate question from whether they
      // bind a target, and it is decided by the colour mask, which the renderer
      // already honours:
      //
      //     colorWrite = (om_seen & 1) == 0 || (colour_mask & 0xF) != 0
      //
      // A depth pass that binds the colour target but masks colour off is
      // harmless -- its PSO gets RenderTargetWriteMask 0 and the stand-in
      // writes nothing. The damaging case is narrower: colour bound and the
      // mask permitting writes, so the stand-in paints.
      //
      // Read RB_COLOR_MASK from the device HERE rather than through dc.
      // dc.colour_mask and dc.om_seen are filled further down this same
      // function, ~34 lines AFTER the PrepareDrawTexture call this runs
      // inside, so consulting them reports every draw in the game as
      // "mask never observed" -- which is exactly what the first cut did, and
      // 41844 of 41844 was the tell. Same address and same gate as the
      // assignment below, so the two cannot drift.
      constexpr uint32_t kRbColorMaskAt = 0x28DC;
      static uint64_t s_wouldPaint = 0, s_maskedOff = 0, s_maskUnreadable = 0;
      uint32_t mask = 0;
      bool mask_readable = false;
      if (device && HostPageReadable(REX_RAW_ADDR(device + kRbColorMaskAt))) {
        mask = REX_LOAD_U32(device + kRbColorMaskAt) & 0xFu;
        mask_readable = true;
      }
      std::lock_guard<std::mutex> lk(s_mu);
      const bool has_colour = dc.render_target_object != 0;
      const bool has_depth = dc.depth_target_object != 0;
      if (has_colour) {
        if (!mask_readable) ++s_maskUnreadable;
        else if (mask != 0) ++s_wouldPaint;
        else ++s_maskedOff;
      }
      if (has_colour && has_depth) ++s_colour_and_depth;
      else if (has_depth) ++s_depth_only;
      else if (has_colour) ++s_colour_only;
      else ++s_neither;
      if (has_colour && s_extents.size() < 32) {
        s_extents.insert((uint64_t(dc.render_target_width) << 32) |
                         dc.render_target_height);
      }
      const uint64_t total = s_colour_and_depth + s_depth_only +
                             s_colour_only + s_neither;
      if ((total % 5000) == 0) {
        std::string extents;
        for (uint64_t e : s_extents)
          extents += fmt::format(" {}x{}", uint32_t(e >> 32), uint32_t(e));
        REXLOG_INFO("d3d9: NULL-PS TARGETS over {} draws: colour+depth {}, "
                    "depth only {}, colour only {}, neither {}; of those with "
                    "colour: WOULD PAINT {}, masked off {}, mask unreadable "
                    "{}; colour extents:{}",
                    total, s_colour_and_depth, s_depth_only, s_colour_only,
                    s_neither, s_wouldPaint, s_maskedOff, s_maskUnreadable,
                    extents.empty() ? " none" : extents);
      }
    }
    static std::mutex s_mu;
    static std::map<uint64_t, uint64_t> s_byDeviceThread;
    static uint64_t s_total = 0;
    bool report = false;
    {
      std::lock_guard<std::mutex> lk(s_mu);
      ++s_byDeviceThread[(uint64_t(device) << 32) | GetCurrentThreadId()];
      report = (++s_total % 20000) == 0;
    }
    if (report) {
      std::string draws;
      {
        std::lock_guard<std::mutex> lk(s_mu);
        for (const auto& [key, n] : s_byDeviceThread) {
          draws += fmt::format(" dev=0x{:08X}/t{}={}", uint32_t(key >> 32),
                               uint32_t(key & 0xFFFFFFFFu), n);
        }
      }
      REXLOG_INFO("d3d9: NO-PS DEVICES over {} draws:{}", s_total, draws);
      REXLOG_INFO("d3d9: SETTER DEVICES:{}", PixelShaderDeviceSummary());
      // RECYCLED counts handles the guest reused for DIFFERENT microcode. A
      // non-zero figure means the translation cache would have been serving a
      // previous shader's translation -- see g_hlslReportedVs.
      REXLOG_INFO("d3d9: {}", ShaderTranslationSummary());
    }
  }
  if (!resolved) {
    resolved = PixelShaderForDeviceStrict(device);
    if (resolved) ++g_psFromDeviceRecord;
  }
  if (resolved) {
    // Normally reached via ReadBoundPixelShader, which is below the contest and
    // therefore never ran for these draws -- so their microcode was never
    // collected and they could not have translated even in principle.
    CollectPixelShaderBlob(resolved, base);
    AttachTranslatedPixelShader(dc, resolved, device, base);
  }

  // AttachTranslatedPixelShader ran just above, so dc already knows whether it
  // has a translated shader. A draw that has one renders with it and never
  // samples this pick -- only a stand-in draw does, and only those are graded.
  if ((!pixel_shader ||
       !ResolvePixelBindingForDraw(pixel_shader, device, base, binding)) &&
      !ReadBoundPixelShader(device, base, pixel_shader, binding)) {
    ++s_no_shader;
    // The COUNT, not just a sample. This exit was logged only as "attempt N"
    // with a handle, which said it happens without ever saying how often --
    // and it turns out to be the largest single reason a draw keeps the
    // tex*col stand-in, larger than untranslated shaders and slot-filling put
    // together. A cap of 8 plus a sample every 2500 attempts looked like a
    // quiet failure while it was in fact the dominant one.
    //
    // Split by which of the two resolutions was even possible: no setter
    // handle at all is a different defect from a setter we cannot follow.
    if (!pixel_shader) ++s_no_shader_no_setter;
    if (s_no_shader <= 8 || (s_attempts % 2500) == 0) {
      const uint32_t direct =
          device && HostPageReadable(REX_RAW_ADDR(device + 0x3244))
              ? REX_LOAD_U32(device + 0x3244)
              : 0;
      // Named for what it now means. This used to read "NO ELIGIBLE PIXEL
      // SHADER", which was true when the contest also decided whether the draw
      // could run its guest shader at all -- it no longer does, and a draw
      // reaching here may well be running a translated shader with every slot
      // bound. Left as it was, the next reader would diagnose 80,000 lost
      // draws that are not lost.
      REXLOG_INFO("d3d9: stand-in has no single texture to sample: {} of {} "
                  "attempts ({} with no setter handle at all, {} rescued by "
                  "the per-device record); this one setter=0x{:08X}, "
                  "device+0x3244=0x{:08X}",
                  s_no_shader, s_attempts, s_no_shader_no_setter,
                  g_psFromDeviceRecord, pixel_shader, direct);
    }
    return false;
  }
  // The shader is resolved by here — `pixel_shader` may have been rewritten by
  // ReadBoundPixelShader — so this is the first point at which the draw knows
  // which guest program it is running. Attaching the translation here rather
  // than at the call site keeps the "which shader is really bound" logic in one
  // place; a draw whose shader did not translate simply carries nothing and
  // keeps the tex*col stand-in.
  // The contest may have rewritten `pixel_shader` to the device's own handle,
  // which is the authoritative answer. If it differs from the one attached
  // before the contest, attach again for the shader actually bound.
  if (pixel_shader != resolved)
    AttachTranslatedPixelShader(dc, pixel_shader, device, base);
  dc.pixel_shader_handle = pixel_shader;

  if (binding.sampler >= kMaxSamplers) {
    ++s_no_binding;
    if (s_no_binding <= 8)
      REXLOG_INFO("d3d9: HLE texture fallback: sampler {} out of range",
                  binding.sampler);
    return false;
  }
  // No coverage rule here, deliberately. This path picks ONE texture to
  // represent a draw the translated path could not take, so a partly-written
  // snapshot is still the best thing it has -- there is no second source to
  // prefer over it, which is the only reason the rule exists on the slot path.
  const auto& texture_state = DeviceState().texture[binding.sampler];
  if (texture_state.object &&
      g_resolvedTextureTargets.contains(texture_state.object)) {
    ++s_mapped;
    // The resolved texture still owns a normal fetch descriptor. Capture its
    // logical extent before taking the host-target path so denormalized shader
    // coordinates can be converted without requiring a CPU payload.
    uint32_t mapped_fetch[6] = {};
    HleTextureSource mapped_source;
    const char* mapped_why = nullptr;
    if (ReadLiveTextureFetch(device, base, binding.sampler, mapped_fetch)) {
      if (DescribeHleTexture2D(mapped_fetch, mapped_source, &mapped_why)) {
        dc.sampled_texture_width = mapped_source.width;
        dc.sampled_texture_height = mapped_source.height;
        dc.clamp_x = uint8_t(mapped_source.clamp_x);
        dc.clamp_y = uint8_t(mapped_source.clamp_y);
      } else {
        // This failure used to be discarded outright. Three quarters of all
        // texture attempts take this early return, so an undecodable format
        // arriving on a resolved target produced no log line at all — which
        // is why "no YUV format has ever been rejected" was not evidence of
        // anything. The branch still returns true; only its silence changes.
        NoteRejectedTextureFormat("mapped", binding.sampler, mapped_source,
                                  mapped_why, mapped_fetch);
      }
    }
    // The renderer samples the live host render target identified below. Do
    // not also upload its stale/empty guest storage as an immutable texture.
    dc.texture.reset();
    return true;
  }
  uint32_t fetch[6] = {};
  if (!ReadLiveTextureFetch(device, base, binding.sampler, fetch)) {
    ++s_no_binding;
    if (s_no_binding <= 8) {
      REXLOG_INFO("d3d9: HLE texture fallback: sampler {} has no 2D fetch "
                  "(device words {:08X} {:08X} {:08X} {:08X} {:08X} {:08X})",
                  binding.sampler, fetch[0], fetch[1], fetch[2], fetch[3],
                  fetch[4], fetch[5]);
    }
    return false;
  }
  HleTextureSource source;
  const char* why = nullptr;
  if (!DescribeHleTexture2D(fetch, source, &why)) {
    ++s_bad_desc;
    NoteRejectedTextureFormat("prepare", binding.sampler, source, why, fetch);
    return false;
  }
  dc.sampled_texture_width = source.width;
  dc.sampled_texture_height = source.height;
  dc.clamp_x = uint8_t(source.clamp_x);
  dc.clamp_y = uint8_t(source.clamp_y);

  // The address route, for the same reason as in ResolvePixelSlotTexture: this
  // memory is a resolve destination reached through a texture object the
  // resolve never named, so the object test above missed it.
  //
  // Both routing fields are set here rather than left to PrepareHleDraw, which
  // sets them only when its own object lookup hits. It runs after this and does
  // not clear them on a miss, so these survive.
  if (const ResolvedTargetByAddress* resolved =
          ResolvedTargetForAddress(source)) {
    ++s_mapped;
    ++g_resolveAddr.matches;
    dc.sampled_texture_object = resolved->dest_object;
    dc.sampled_render_target_object = resolved->source_object;
    // The renderer samples the snapshot; uploading the empty guest storage
    // alongside it would just be the black texture again.
    dc.texture.reset();
    return true;
  }

  // kR8 and kR16 join this list on the same reasoning as the others: they are
  // single-channel, so binding one as visible base colour would paint the
  // surface grey. They are decoded rather than rejected so the counters can
  // tell "we cannot read this" apart from "we choose not to show this" — the
  // distinction the old shared "unsupported" string destroyed.
  //
  // kRg8 (k_8_8) is two-channel and joins them: the stand-in shader has no
  // idea what the second channel means, so binding one as base colour paints
  // the surface in red and green. The translated path, which is where these
  // 1273 rejections a run were actually losing draws, goes through
  // ResolvePixelSlotTexture and is deliberately not gated by this.
  if (source.host_format == HostTextureFormat::kBc5 ||
      source.host_format == HostTextureFormat::kR16Float ||
      source.host_format == HostTextureFormat::kRgba16Float ||
      source.host_format == HostTextureFormat::kR8 ||
      source.host_format == HostTextureFormat::kR16 ||
      source.host_format == HostTextureFormat::kR32Float ||
      source.host_format == HostTextureFormat::kRg8) {
    ++s_semantic_reject;
    if (s_semantic_reject <= 12) {
      // Named in guest terms as well: this drop is a policy choice about a
      // format we *can* decode, so telling the two kinds of loss apart in the
      // log matters when deciding what to add next.
      REXLOG_INFO("d3d9: HLE texture fallback: sampler {} guest format {} ({}) "
                  "decodes to host format {} but is not an immutable colour "
                  "asset",
                  binding.sampler, source.guest_format,
                  GuestTextureFormatName(source.guest_format),
                  uint32_t(source.host_format));
    }
    return false;
  }
  const uint64_t key = HleTextureKey(fetch);
  // Blank textures are still a poor representative for the one texture this
  // path picks to stand in for the whole draw -- but only while they ARE blank.
  // The refusal now expires on the same backoff the translated path retries on,
  // so a texture the guest streams in later is reconsidered instead of being
  // written off for the rest of the run.
  if (!BlankRetryDue(key)) {
    ++s_empty;
    return false;
  }
  auto cached = g_hleCpuTextures.find(key);
  if (cached != g_hleCpuTextures.end()) {
    if (!TextureContentStale(source, base, *cached->second)) {
      dc.texture = cached->second;
      ++s_ready;
      return true;
    }
    g_hleCpuTextures.erase(cached);
  }
  std::vector<uint8_t> guest;
  if (!CopyTexturePhysical(source, base, guest)) {
    ++s_unreadable;
    if (s_unreadable <= 8) {
      REXLOG_INFO("d3d9: HLE texture fallback: sampler {} source 0x{:08X} "
                  "(size {}) unreadable",
                  binding.sampler, source.address, source.source_bytes);
    }
    return false;
  }
  auto payload = std::make_shared<HleTexturePayload>();
  if (!DecodeHleTexture2D(source, guest.data(), guest.size(), *payload, &why)) {
    ++s_bad_desc;
    if (s_bad_desc <= 12)
      REXLOG_INFO("d3d9: HLE texture decode rejected ({})",
                  why ? why : "?");
    return false;
  }
  size_t nonzero_bytes = 0;
  if (!HleTextureHasNonzeroData(*payload, &nonzero_bytes)) {
    NoteBlankDecode(key);
    ++s_empty;
    if (s_empty <= 12) {
      REXLOG_INFO("d3d9: HLE texture fallback: sampler {} {}x{} format {} "
                  "decoded to an all-zero guest payload",
                  binding.sampler, source.width, source.height,
                  uint32_t(source.host_format));
    }
    return false;
  }
  NoteBlankRecovered(key);
  payload->key = key;
  payload->content_version =
      TextureContentVersion(source, base, payload->format);
  mx::diag::DumpDecodedTexture(source, *payload, "prepare", binding.sampler);
  dc.texture = payload;
  g_hleCpuTextures.emplace(key, std::move(payload));
  ++s_ready;
  if (s_ready <= 8 || (s_attempts % 2500) == 0) {
    REXLOG_INFO("d3d9: HLE textures attempts {} ready {} mapped {} cached {} "
                "empty {} semantic-reject {} no-shader {} no-binding {} "
                "bad-desc {} unreadable {}; latest {}x{} format {} in "
                "viewport {}x{} ({} nonzero bytes)",
                s_attempts, s_ready, s_mapped, g_hleCpuTextures.size(), s_empty,
                s_semantic_reject, s_no_shader, s_no_binding, s_bad_desc,
                s_unreadable, dc.texture->width,
                dc.texture->height, uint32_t(dc.texture->format),
                dc.viewport_width, dc.viewport_height, nonzero_bytes);
  }
  return true;
}


//---------------------------------------------------------------------------
// The declaration-to-vfetch pairing rule, read out of
// D3D::PatchVertexShaderToMatchVertexDeclaration (0x82564C50).
//
// This was the last bridge in the shader-execution path: attributes were taken
// from PM4's decode because nothing said which declaration element feeds which
// vfetch instruction. The function itself says, and it says it with a table.
//
//   this   = r3   CVertexShader*
//   dest   = r4   the microcode being patched — where results are written
//   decl   = r5   CVertexDeclaration*, count at +0x18, elements at +0x34
//   strides= r6   const BYTE*, indexed by stream, stride in DWORDS
//   variant= r7   which patched variant of the shader this is
//
// The shader carries a **binding table**, one dword per vfetch:
//
//   blob  = this + *(this + (variant + 0x70) * 8) + 0x368     ; GetUCode(variant)
//   count = blob[0x1C]
//   table = blob + 4 * (blob[0x18] + 9)
//
//   key[11:0]  -> vfetch instruction index; the patched triple is written to
//                 dest + 12 * index
//   key[15:12] -> D3DDECLUSAGE
//   key[19:16] -> usage index
//
// **The pairing is by semantic, not by position.** The element whose `usage`
// (byte 9) and `usage_index` (byte 10) equal the key's is the one that patches
// that vfetch — a linear search, first match wins. That is why the template's
// format/offset/stride are blank: they are not defaults, they are unbound.
//
// From the matched element:
//   fetch constant index = 95 - element.stream       (subfic r20, r5, 0x5F)
//   format/signed/integer/swizzle from the Type dword, as kType* already say
//   offset field = element.offset / 4                (rlwinm r8, r8, 6, 1, 23)
//   stride field = strides[element.stream]           (lbzx r5, r5, r6)
//
// **No match leaves fetch constant 95 as well**, which is the same value stream
// 0 produces. So a decoded fetch_slot of 95 is ambiguous between "stream 0" and
// "unbound", and PM4's decode showing 95 everywhere never distinguished them.
// The unbound case is identifiable by its canned format bits (0x60000) and
// swizzle (0x9250) instead.
//
// None of this is believed on the strength of the disassembly. The probe
// predicts all three dwords of the patched vfetch *before* the call and
// compares them against what D3D9 actually wrote *after* it. A rule read wrong
// disagrees; a rule read right agrees on every dword of every vfetch.
//---------------------------------------------------------------------------
constexpr uint32_t kUCodePtrTable  = 0x70;   // this + (variant + 0x70) * 8
constexpr uint32_t kUCodeBlobDelta = 0x368;
constexpr uint32_t kBlobTableOff   = 0x18;   // dword: table start, in dwords - 9
constexpr uint32_t kBlobFetchCount = 0x1C;   // dword: how many vfetches
constexpr uint32_t kTemplateBase   = 0x44;   // this + 0x44 + 416 * variant
constexpr uint32_t kTemplateStride = 0x1A0;
constexpr uint32_t kDeclCountOff   = 0x18;
constexpr uint32_t kDeclElemsOff   = 0x34;
constexpr uint32_t kMaxPatchFetch  = 32;

uint64_t g_patchProbed = 0;        // calls the probe actually examined
uint64_t g_patchFetches = 0;       // vfetch slots predicted
uint64_t g_patchBound = 0;         // ... of which a declaration element matched
uint64_t g_patchUnbound = 0;       // ... of which none did
// Per *field*, not per dword. A whole-dword comparison asks a stricter question
// than the one that matters: two of these fields are written by machinery this
// rule does not model (the second pass that coalesces adjacent fetches, and the
// swizzle chain through word_8204E178), and folding them in would report the
// fields that ARE read correctly as failures.
enum PatchField : uint32_t {
  kPfFetchConst = 0,   // dword0 [26:20] — 95 - stream
  kPfCoalesce,         // dword0 [29:27] — second pass; NOT modelled
  kPfFormat,           // dword1 [21:16] — Type[5:0]
  kPfNumFormat,        // dword1 [13:12] — Type[9:8]
  kPfSwizzle,          // dword1 [11:0]  — swizzle chain; NOT modelled
  kPfOffset,           // dword2 [30:8]  — element.offset / 4
  kPfStride,           // dword2 [7:0]   — strides[stream]
  kPatchFieldCount,
};
const char* const kPatchFieldName[kPatchFieldCount] = {
    "fetch const (95-stream)", "coalesce count [29:27] (unmodelled)",
    "format", "signed/integer", "swizzle [11:0] (unmodelled)",
    "offset (elem.offset/4)", "stride (strides[stream])"};
const uint32_t kPatchFieldDword[kPatchFieldCount] = {0, 0, 1, 1, 1, 2, 2};
const uint32_t kPatchFieldMask[kPatchFieldCount] = {
    0x07F00000u, 0x38000000u, 0x003F0000u, 0x00003000u,
    0x00000FFFu, 0x7FFFFF00u, 0x000000FFu};

uint64_t g_pfAgree[kPatchFieldCount] = {};
uint64_t g_pfDisagree[kPatchFieldCount] = {};

uint64_t g_patchAgree[3] = {};     // per dword, prediction == what D3D9 wrote
uint64_t g_patchDisagree[3] = {};
uint64_t g_patchBadTable = 0;      // count or table offset outside anything sane

// Where does D3D9 write the patched microcode? r4 is that destination, and if
// it is the same memory the draw-time probe already reads (SH_pPhysical +
// 0x40), then the patched code is directly readable and none of this rule needs
// reimplementing — the prediction only ever needed to be a *check*. Measured
// rather than assumed, because Stage B concluded that buffer held the unpatched
// template and that conclusion has to be either confirmed or overturned.
uint64_t g_destIsPhys40 = 0;       // dest == unmasked SH_pPhysical + 0x40
uint64_t g_destIsPhysOther = 0;    // inside that allocation, different offset
uint64_t g_destElsewhere = 0;
uint32_t g_destSample[4] = {};     // self, dest, SH_pPhysical, delta
bool     g_destHaveSample = false;
uint32_t g_patchFirstMismatch[6] = {};  // predicted/actual triple, first miss
bool     g_patchHaveMismatch = false;
// usage -> how many vfetches bound to it, so the report says which semantics
// this title actually feeds its shaders.
std::map<uint32_t, uint64_t> g_patchUsage;



// Reads the binding table, predicts every patched vfetch, and returns them for
// comparison after the original runs. Every read is page-guarded; the pointers
// are D3D9's own arguments, which it is about to dereference itself.
void PredictPatchedFetches(uint32_t self, uint32_t dest, uint32_t decl,
                           uint32_t strides, uint32_t variant, uint8_t* base,
                           std::vector<PatchPrediction>& out) {
  out.clear();
  if (!self || !dest || !decl || !strides) return;

  // Is the destination the buffer the draw-time probe already reads?
  if (HostPageReadable(REX_RAW_ADDR(self + 0x20))) {
    const uint32_t phys = REX_LOAD_U32(self + 0x20);
    if (dest == phys + 0x40) {
      ++g_destIsPhys40;
    } else if (phys && dest > phys && dest - phys < 0x1000) {
      ++g_destIsPhysOther;
    } else {
      ++g_destElsewhere;
    }
    if (!g_destHaveSample) {
      g_destHaveSample = true;
      g_destSample[0] = self;
      g_destSample[1] = dest;
      g_destSample[2] = phys;
      g_destSample[3] = dest - phys;
    }
  }

  const uint32_t slot = self + (variant + kUCodePtrTable) * 8;
  if (!HostPageReadable(REX_RAW_ADDR(slot))) return;
  const uint32_t blob = self + REX_LOAD_U32(slot) + kUCodeBlobDelta;
  if (!HostPageReadable(REX_RAW_ADDR(blob + kBlobFetchCount))) return;

  const uint32_t count = REX_LOAD_U32(blob + kBlobFetchCount);
  const uint32_t tbl_off = REX_LOAD_U32(blob + kBlobTableOff);
  if (count == 0 || count > kMaxPatchFetch || tbl_off > 0x10000) {
    ++g_patchBadTable;
    return;
  }
  const uint32_t table = blob + 4 * (tbl_off + 9);

  if (!HostPageReadable(REX_RAW_ADDR(decl + kDeclCountOff))) return;
  const uint32_t nelem = REX_LOAD_U32(decl + kDeclCountOff);
  if (nelem > kMaxElements) { ++g_patchBadTable; return; }

  for (uint32_t i = 0; i < count; ++i) {
    if (!HostPageReadable(REX_RAW_ADDR(table + i * 4))) return;
    const uint32_t key = REX_LOAD_U32(table + i * 4);
    const uint32_t instr = key & 0xFFF;
    const uint32_t usage = (key >> 12) & 0xF;
    const uint32_t uidx = (key >> 16) & 0xF;

    // The template triple this vfetch starts from.
    const uint32_t tmpl = self + kTemplateBase + kTemplateStride * variant +
                          12 * i;
    if (!HostPageReadable(REX_RAW_ADDR(tmpl)) ||
        !HostPageReadable(REX_RAW_ADDR(tmpl + 8)))
      return;
    const uint32_t d0 = REX_LOAD_U32(tmpl);
    const uint32_t d1 = REX_LOAD_U32(tmpl + 4);
    const uint32_t d2 = REX_LOAD_U32(tmpl + 8);

    // The linear search by semantic, exactly as the function does it.
    uint32_t match = nelem;
    for (uint32_t e = 0; e < nelem; ++e) {
      const uint32_t ea = decl + kDeclElemsOff + e * kElementSize;
      if (!HostPageReadable(REX_RAW_ADDR(ea))) return;
      if (REX_LOAD_U8(ea + 9) == usage && REX_LOAD_U8(ea + 10) == uidx) {
        match = e;
        break;
      }
    }

    PatchPrediction p;
    p.dest_addr = dest + 12 * instr;
    if (match < nelem) {
      const uint32_t ea = decl + kDeclElemsOff + match * kElementSize;
      const uint32_t stream = REX_LOAD_U16(ea + 0);
      const uint32_t offset = REX_LOAD_U16(ea + 2);
      const uint32_t type = REX_LOAD_U32(ea + 4);
      if (!HostPageReadable(REX_RAW_ADDR(strides + stream))) return;
      const uint32_t stride = REX_LOAD_U8(strides + stream);

      p.pred[0] = (d0 & 0xC00FFFFFu) | (((95u - stream) & 0x7Fu) << 20);
      p.pred[1] = (d1 & 0xBFC0CFFFu) |
                  ((((type << 12) & 0x3F000u) | (type & 0x300u)) << 4);
      p.pred[2] = (d2 & 0x80000000u) | ((offset << 6) & 0x7FFFFF00u) | stride;
      p.bound = true;
      ++g_patchUsage[usage];
    } else {
      p.pred[0] = (d0 & 0xC00FFFFFu) | 0x5F00000u;
      p.pred[1] = (d1 & 0xBFC0CFFFu) | 0x60000u;
      if (!HostPageReadable(REX_RAW_ADDR(strides))) return;
      p.pred[2] = (d2 & 0x80000000u) | REX_LOAD_U8(strides);
      p.bound = false;
    }
    out.push_back(p);
  }
}

// The binding table's vfetch count on its own, so the capture can run on every
// call while the full prediction stays sampled.
uint32_t ReadPatchFetchCount(uint32_t self, uint32_t variant, uint8_t* base) {
  if (!self) return 0;
  const uint32_t slot = self + (variant + kUCodePtrTable) * 8;
  if (!HostPageReadable(REX_RAW_ADDR(slot))) return 0;
  const uint32_t blob = self + REX_LOAD_U32(slot) + kUCodeBlobDelta;
  if (!HostPageReadable(REX_RAW_ADDR(blob + kBlobFetchCount))) return 0;
  const uint32_t count = REX_LOAD_U32(blob + kBlobFetchCount);
  return count > kMaxPatchFetch ? 0 : count;
}

// Copies the patched microcode out of the destination, keyed by shader handle.
// Must run immediately after the original: the destination is in the command
// ring and will be overwritten.
// Does the shader OBJECT carry its own microcode, and if so where?
//
// This matters because the patch hook is our only source of vertex microcode,
// and it only fires for shaders D3D9 needs to patch. Everything else is
// reported as "no-code": 164,648 of 401,750 draws in mx_711 (41%), which are
// the same draws as the 82,324 of 129,004 dropped before reaching the renderer.
// They are not a rendering fault -- they never had a program to run.
//
// The offset is SEARCHED rather than assumed, against code already proven by
// the patch hook's own decode. If one offset explains every shader, it is a
// property of the layout and can be relied on; if the histogram is spread, the
// premise is wrong and this says so instead of producing plausible garbage.
// Same discipline as the CF-start search this file already documents.
// Where a vertex shader's own microcode lives, and how long it is.
//
// Both transcribed from sub_82565550, the routine that uploads a shader: it
// allocates ring space, copies the code in, and only THEN calls
// PatchVertexShaderToMatchVertexDeclaration on the ring copy.
//
//   v17 = *(*(self + (variant+0x70)*8) + self + 876)   // size in BYTES
//   v23 = *(*(self + (variant+0x70)*8) + self + 872) + *(self + 0x20)
//   v24 = (((v23 >> 20) + 512) & 0x1000) + (v23 & 0x1FFFFFFF) - 0x40000000
//   memcpy(dest, v24, v17)
//
// 872 is kUCodeBlobDelta and 876 is the dword after it, so the size sits at
// blob+4 -- beside the fetch count at blob+0x1C this file already reads. The
// code itself is at blob + *(self+0x20), through an address fixup that clears
// the 0x40000000 segment bit. Searching a window around the blob found NOTHING
// in 36,000 shaders, which is exactly right: the fixup moves it out of range.
uint32_t ShaderObjectBlob(uint32_t self, uint32_t variant, uint8_t* base) {
  const uint32_t slot = self + (variant + kUCodePtrTable) * 8;
  if (!self || !HostPageReadable(REX_RAW_ADDR(slot))) return 0;
  return self + REX_LOAD_U32(slot) + kUCodeBlobDelta;
}

uint32_t ShaderObjectCodeBytes(uint32_t self, uint32_t variant, uint8_t* base) {
  const uint32_t blob = ShaderObjectBlob(self, variant, base);
  if (!blob || !HostPageReadable(REX_RAW_ADDR(blob + 4))) return 0;
  return REX_LOAD_U32(blob + 4);
}

uint32_t ShaderObjectCodeAddress(uint32_t self, uint32_t variant,
                                 uint8_t* base) {
  const uint32_t blob = ShaderObjectBlob(self, variant, base);
  if (!blob || !HostPageReadable(REX_RAW_ADDR(self + 0x20))) return 0;
  // *(blob), not blob. The decompilation reads
  //   v23 = *(_DWORD *)(*(...) + a3 + 872) + *(_DWORD *)(a3 + 32)
  // and that outer dereference is easy to drop, because the very similar
  // expression in PatchVertexShaderToMatchVertexDeclaration uses the same
  // address WITHOUT one (as the base for the fetch table). Taking blob itself
  // put the read 0 of 28,000 shaders' first eight dwords -- caught only
  // because the probe checked alignment separately from content.
  if (!HostPageReadable(REX_RAW_ADDR(blob))) return 0;
  const uint32_t v23 = REX_LOAD_U32(blob) + REX_LOAD_U32(self + 0x20);
  const uint32_t addr =
      (((v23 >> 20) + 512) & 0x1000) + (v23 & 0x1FFFFFFF) - 0x40000000u;
  return HostPageReadable(REX_RAW_ADDR(addr)) ? addr : 0;
}

void ProbeShaderObjectCode(uint32_t self, uint32_t variant,
                           const PatchedCode& known, uint8_t* base) {
  if (!known.resolved || known.code.size() <= known.code_off + 8) return;
  static std::map<int64_t, uint64_t> s_offsets;
  static uint64_t s_probed = 0, s_found = 0;
  ++s_probed;

  const uint32_t src = ShaderObjectCodeAddress(self, variant, base);
  if (!src) return;
  const uint32_t size = ShaderObjectCodeBytes(self, variant, base);
  if (!size || size > 64u * 1024u) return;

  // The ring copy this capture came from IS this memory, byte for byte, at the
  // moment of the copy -- dest was memcpy'd from here. So every dword should
  // agree EXCEPT the vfetch fields the patch then rewrote in the ring. A
  // handful of differing dwords confirms the address; wholesale disagreement
  // means it is the wrong buffer, and the count is what tells them apart.
  const uint32_t have = uint32_t(known.code.size());
  uint32_t compared = 0, differ = 0;
  for (uint32_t i = 0; i < size / 4; ++i) {
    const uint32_t at = kPatchWindowBack + i;
    if (at >= have) break;
    if ((at & (kHostPageSize - 1)) == 0 &&
        !HostPageReadable(REX_RAW_ADDR(src + i * 4)))
      break;
    ++compared;
    if (REX_LOAD_U32(src + i * 4) != known.code[at]) ++differ;
  }
  if (!compared) return;
  ++s_found;

  // As a SHARE of the shader, not an absolute count. 226 differing dwords is
  // 11% of a 2000-dword program and 95% of a 237-dword one, and those mean
  // opposite things -- the first is the patch rewriting fetches, the second is
  // the wrong buffer. The absolute histogram could not tell them apart.
  const uint32_t pct = differ * 100 / compared;
  ++s_offsets[pct < 1 ? 0 : pct < 5 ? 5 : pct < 10 ? 10 : pct < 25 ? 25
              : pct < 50 ? 50 : 100];

  // Independently: does the program START at this address? The copy was
  // byte-for-byte, so a correct address agrees on the leading dwords unless a
  // fetch sits at instruction 0. Alignment is a separate claim from content.
  static uint64_t s_headMatch = 0;
  bool head = true;
  for (uint32_t i = 0; i < 8 && head; ++i)
    head = REX_LOAD_U32(src + i * 4) == known.code[kPatchWindowBack + i];
  if (head) ++s_headMatch;

  if ((s_probed % 2000) == 0) {
    std::string hist;
    for (const auto& [b, n] : s_offsets)
      hist += b == 100   ? fmt::format(" >=50%={}", n)
              : b == 0   ? fmt::format(" 0%={}", n)
                         : fmt::format(" <{}%={}", b, n);
    REXLOG_INFO("d3d9: shader-object code probe: {} of {} readable at "
                "blob+[self+0x20], {} with a matching first 8 dwords; share "
                "of dwords differing from the patched ring copy:{}",
                s_found, s_probed, s_headMatch, hist.empty() ? " none" : hist);
  }
}

void CapturePatchedCode(uint32_t self, uint32_t dest, uint32_t variant,
                        uint32_t expect_fetches, uint8_t* base) {
  if (!self || !dest || dest < kPatchWindowBack * 4) return;
  const uint32_t start = dest - kPatchWindowBack * 4;

  auto it = g_patch.patched.find(self);
  const bool known = it != g_patch.patched.end() && it->second.resolved;
  const uint32_t known_off = known ? it->second.code_off : 0;

  PatchedCode pc;
  pc.expect_fetches = expect_fetches;
  pc.variant = variant;
  pc.code.reserve(kPatchWindowBack + kPhysProbeDwords);
  for (uint32_t i = 0; i < kPatchWindowBack + kPhysProbeDwords; ++i) {
    const uint32_t at = start + i * 4;
    if ((at & (kHostPageSize - 1)) == 0 && !HostPageReadable(REX_RAW_ADDR(at)))
      break;
    pc.code.push_back(REX_LOAD_U32(at));
  }
  if (pc.code.size() < 32) return;

  // Try the known offset first — but *verify* it, do not assume it. An earlier
  // version cached the offset and reused it blind, and the draw-time decode
  // then failed on thousands of captures while the report happily said the
  // shader was resolved. A cached answer that is never re-checked is an
  // assumption wearing a measurement's clothes.
  static std::vector<mx::hle::VertexAttribute> probe;
  auto decodes_at = [&](uint32_t s) {
    if (s >= pc.code.size()) return false;
    probe.clear();
    return mx::hle::DecodeVertexShaderFetches(pc.code.data() + s,
                                              uint32_t(pc.code.size() - s),
                                              probe, nullptr) &&
           probe.size() == expect_fetches;
  };

  // dest first. Measured over 24 distinct shaders: the CF stream starts exactly
  // at the patch destination in 24 of 24, while the upward scan below lands
  // early in 3 of them (off -3, -3, -85) because it takes the first offset that
  // decodes and a false positive can precede the true start. Trying dest before
  // scanning costs one decode and removes that whole failure mode.
  //
  // Still verified, not assumed — same rule as the cached offset below.
  if (decodes_at(kPatchWindowBack)) {
    pc.code_off = kPatchWindowBack;
    pc.resolved = true;
  } else if (known && decodes_at(known_off)) {
    pc.code_off = known_off;
    pc.resolved = true;
  } else {
    // Only the true CF start decodes to the count the binding table states, so
    // this is a search with a checkable answer rather than a guess. Preferring
    // the known offset first also stops a low false positive from winning when
    // the real layout is already established.
    for (uint32_t s = 0; s < pc.code.size(); ++s) {
      if (!decodes_at(s)) continue;
      pc.code_off = s;
      pc.resolved = true;
      ++g_patch.codeOffsets[int32_t(s) - int32_t(kPatchWindowBack)];
      break;
    }
  }

  // Does the guest state the answer the search just hunted for?
  //
  // sub_82565928's VS branch computes the program address the GPU is given as
  // *(vs + 0x20) + *(info + 0x368), where info = vs + *(vs + 0x380 +
  // variant*8), with the length in bytes at info + 0x36C. The patcher
  // (0x82564C50) indexes the identical 0x380 + variant*8 field, so both agree
  // the patched code lives in the shader's own allocation.
  //
  // Compared as ABSOLUTE guest addresses, which is the only common ground: the
  // search's answer is an index into a ring window, the field's is a pointer.
  // Reporting them any other way would compare two different coordinate
  // systems and call the mismatch a finding.
  //
  // Measurement only. Nothing here changes what is captured — if the field is
  // right, the search is still what runs until a separate change says so.
  // The program length, read unconditionally — it bounds the code handed to the
  // decoders and so is not a diagnostic. Only the reporting below is gated.
  //
  // The length is the canonical program's, and 2520 of 2561 captures patch into
  // a buffer other than the shader's own allocation. Taken as applying to the
  // patched copy anyway: patching rewrites fetch instructions in place and
  // cannot change the instruction count. If that were wrong the bound would cut
  // a shader short and the fetch decode would fail loudly rather than silently.
  uint32_t field_abs = 0, field_len = 0;
  {
    const uint32_t info_at = self + kVsInfoOffsetAt + variant * 8;
    if (HostPageReadable(REX_RAW_ADDR(info_at)) &&
        HostPageReadable(REX_RAW_ADDR(self + kVsCodeAllocAt))) {
      const uint32_t info = self + REX_LOAD_U32(info_at);
      if (HostPageReadable(REX_RAW_ADDR(info + kVsInfoCodeSize))) {
        field_abs =
            REX_LOAD_U32(self + kVsCodeAllocAt) + REX_LOAD_U32(info + kVsInfoCodeOffset);
        field_len = REX_LOAD_U32(info + kVsInfoCodeSize);
      }
    }
  }
  // Trim the captured window to the real program. Without this the ALU
  // interpreter and the fetch decoder are handed everything to the end of the
  // capture — 256 dwords past dest — while measured programs run 24 to 174, so
  // a walk that does not stop on its own continues into the next shader in the
  // ring.
  if (pc.resolved && field_len && (field_len & 3) == 0) {
    const size_t want = pc.code_off + field_len / 4;
    if (want >= 8 && want <= pc.code.size()) {
      pc.code.resize(want);
      pc.code_len_dwords = field_len / 4;
    } else {
      ++g_vsWindow.lenRejected;
    }
  }

  if (pc.resolved && REXCVAR_GET(hle_capture)) {
    const uint32_t search_abs = start + pc.code_off * 4;
    // Two independent questions, kept apart because they have different
    // answers. Where the CF starts: dest, in 24 of 24 measured. Whether the
    // shader object's own allocation is the buffer that was patched: only
    // sometimes — 16 of 24 — so the field is NOT a drop-in source of code.
    if (search_abs == dest) ++g_vsWindow.atDest;
    else if (search_abs < dest) ++g_vsWindow.early;
    else ++g_vsWindow.late;
    if (!field_abs) ++g_vsWindow.noField;
    else if (field_abs == dest) ++g_vsWindow.agree;
    else ++g_vsWindow.disagree;
    static std::map<uint64_t, bool> s_logged;
    const uint64_t key = (uint64_t(self) << 32) | variant;
    if (field_abs && s_logged.size() < 24 && s_logged.emplace(key, true).second) {
      REXLOG_INFO(
          "d3d9: vs 0x{:08X} v{} window: search 0x{:08X} (off {}), field "
          "0x{:08X} len {} dwords, dest 0x{:08X} — {}",
          self, variant, search_abs, int32_t(pc.code_off) - int32_t(kPatchWindowBack),
          field_abs, field_len / 4, dest,
          search_abs == dest ? (field_abs == dest ? "at-dest, same buffer"
                                                  : "at-dest, other buffer")
                             : "SEARCH OFF DEST");
    }
    const uint64_t seen = g_vsWindow.atDest + g_vsWindow.early + g_vsWindow.late;
    if ((seen % 512) == 1) {
      REXLOG_INFO(
          "d3d9: vs code window: CF at dest {} early {} late {}; shader alloc "
          "is the patched buffer {} of {} (other buffer {}, unreadable {}); "
          "length rejected {}",
          g_vsWindow.atDest, g_vsWindow.early, g_vsWindow.late, g_vsWindow.agree,
          seen, g_vsWindow.disagree, g_vsWindow.noField, g_vsWindow.lenRejected);
    }
  }
  // A ring-window read is inherently transient: the destination may wrap or
  // be overwritten between the original call and this hook's copy. Never let
  // such a failed observation destroy a previously proven capture for the
  // same shader variant. This used to turn valid shaders back into
  // "no exact patched code" later in the frame.
  const bool same_variant_as_known = known && it->second.variant == variant;
  static uint64_t s_capture_attempts = 0;
  static uint64_t s_capture_resolved = 0;
  static uint64_t s_capture_preserved = 0;
  static uint64_t s_capture_invalidated = 0;
  ++s_capture_attempts;
  const bool capture_resolved = pc.resolved;
  if (capture_resolved) {
    g_patch.patched[self] = std::move(pc);
    // Only ever against a capture the decode already proved, so a match here
    // is evidence about the LAYOUT rather than about this one shader.
    ProbeShaderObjectCode(self, variant, g_patch.patched[self], base);
    ++s_capture_resolved;
  } else if (same_variant_as_known) {
    ++s_capture_preserved;
  } else {
    // A different variant is a different program. Refuse to use stale exact
    // code if the replacement could not itself be captured.
    if (known) {
      g_patch.patched.erase(it);
      ++s_capture_invalidated;
    }
  }
  if (s_capture_attempts <= 24 || (s_capture_attempts % 1000) == 0) {
    REXLOG_INFO(
        "d3d9: patched VS capture {} self=0x{:08X} bound=0x{:08X} "
        "variant={} fetches={} resolved={} totals resolved {} preserved {} "
        "invalidated {}",
        s_capture_attempts, self, DeviceState().vertex_shader, variant,
        expect_fetches, capture_resolved, s_capture_resolved, s_capture_preserved,
        s_capture_invalidated);
  }
}

// After the original ran: did it write what the rule predicts?
void CheckPatchedFetches(const std::vector<PatchPrediction>& pred,
                         uint8_t* base) {
  if (pred.empty()) return;
  ++g_patchProbed;
  for (const auto& p : pred) {
    ++g_patchFetches;
    if (p.bound) ++g_patchBound; else ++g_patchUnbound;
    if (!HostPageReadable(REX_RAW_ADDR(p.dest_addr)) ||
        !HostPageReadable(REX_RAW_ADDR(p.dest_addr + 8)))
      continue;
    uint32_t got[3] = {REX_LOAD_U32(p.dest_addr), REX_LOAD_U32(p.dest_addr + 4),
                       REX_LOAD_U32(p.dest_addr + 8)};
    for (uint32_t f = 0; f < kPatchFieldCount; ++f) {
      const uint32_t m = kPatchFieldMask[f];
      const uint32_t d = kPatchFieldDword[f];
      if ((got[d] & m) == (p.pred[d] & m)) ++g_pfAgree[f];
      else                                 ++g_pfDisagree[f];
    }
    for (uint32_t d = 0; d < 3; ++d) {
      if (got[d] == p.pred[d]) {
        ++g_patchAgree[d];
      } else {
        ++g_patchDisagree[d];
        // Keep the first disagreement in full. A count says the rule is wrong;
        // the two triples say which field of it is.
        if (!g_patchHaveMismatch) {
          g_patchHaveMismatch = true;
          for (uint32_t k = 0; k < 3; ++k) {
            g_patchFirstMismatch[k] = p.pred[k];
            g_patchFirstMismatch[3 + k] = got[k];
          }
        }
      }
    }
  }
}

void ReportPatchRule() {
  if (!g_patchProbed) {
    if (g_patchBadTable)
      REXLOG_INFO(
          "d3d9: pairing — nothing predicted; {} calls had an out-of-range "
          "table or element count, so the header offsets are wrong",
          g_patchBadTable);
    return;
  }
  REXLOG_INFO(
      "d3d9: pairing — {} patch calls, {} vfetch slots: {} bound to a "
      "declaration element by (usage, usage_index), {} unbound; {} calls "
      "rejected for a bad table",
      g_patchProbed, g_patchFetches, g_patchBound, g_patchUnbound,
      g_patchBadTable);
  for (uint32_t f = 0; f < kPatchFieldCount; ++f) {
    const uint64_t tot = g_pfAgree[f] + g_pfDisagree[f];
    REXLOG_INFO("d3d9: pairing — field {:<36} {} of {} agree ({}%)",
                kPatchFieldName[f], g_pfAgree[f], tot,
                tot ? (g_pfAgree[f] * 100) / tot : 0);
  }
  static const char* const kName[3] = {"dword0", "dword1", "dword2"};
  for (uint32_t d = 0; d < 3; ++d) {
    const uint64_t tot = g_patchAgree[d] + g_patchDisagree[d];
    REXLOG_INFO(
        "d3d9: pairing — whole {} : {} of {} ({}%) — stricter than the "
        "question; the unmodelled fields live here",
        kName[d], g_patchAgree[d], tot,
        tot ? (g_patchAgree[d] * 100) / tot : 0);
  }
  if (g_patchHaveMismatch) {
    REXLOG_INFO(
        "d3d9: pairing — first disagreement: predicted {:08X} {:08X} {:08X}, "
        "D3D9 wrote {:08X} {:08X} {:08X}",
        g_patchFirstMismatch[0], g_patchFirstMismatch[1],
        g_patchFirstMismatch[2], g_patchFirstMismatch[3],
        g_patchFirstMismatch[4], g_patchFirstMismatch[5]);
  }
  std::string u;
  for (const auto& [usage, n] : g_patchUsage) {
    const char* nm = mx::hle::UsageSemanticName(uint8_t(usage));
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%s:%llu ", nm ? nm : "?",
                  (unsigned long long)n);
    u += buf;
  }
  REXLOG_INFO("d3d9: pairing — bound semantics {}", u.empty() ? "none" : u);
  REXLOG_INFO(
      "d3d9: pairing — patch destination: {} calls wrote to SH_pPhysical+0x40 "
      "(what the draw-time probe reads), {} elsewhere in that allocation, {} "
      "somewhere else",
      g_destIsPhys40, g_destIsPhysOther, g_destElsewhere);
  if (g_destHaveSample) {
    REXLOG_INFO(
        "d3d9: pairing — first: shader 0x{:08X}, dest 0x{:08X}, SH_pPhysical "
        "0x{:08X}, dest-phys 0x{:X}",
        g_destSample[0], g_destSample[1], g_destSample[2], g_destSample[3]);
  }
}

}  // namespace mx::hooks::d3d9
