// The texture and pixel-binding half of the D3D9 HLE: the guest texture cache,
// the glyph-cache special case, blank/swizzle/sign/mip censuses, the Bink plane
// path, pixel slot resolution and PrepareDrawTexture. Split verbatim out of
// hooks_d3d9.cpp. The shared surface is hooks_d3d9_shared.h.

#include "hooks/hook_common.h"

#include <rex/cvar.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>
#include <atomic>
#include <cstdlib>
#include <fstream>
#include <unordered_map>

// For the emitter coverage probe only: emitting HLSL the compiler then rejects
// is exactly as useless as refusing to emit, so the probe compiles what it
// emits. Nothing else in this file touches D3D.
#include <d3dcompiler.h>
#include <wrl/client.h>

// For the vfetch destination swizzle: the GPU vertex path must merge attributes
// into registers by the same rule shader_alu.cpp seeds its register file with.
#include <rex/graphics/format/ucode.h>

#include "gpu/guard_census.h"
#include "gpu/d3d9_draw.h"
#include "gpu/d3d9_layout.h"
#include "gpu/d3d9_texture.h"
#include "gpu/shader_ucode.h"   // DecodeVertexShaderFetches, VertexAttribute
#include "gpu/shader_alu.h"     // ExecuteVertexShader
#include "gpu/shader_hlsl.h"    // EmitShaderHlsl
#include <cmath>
#include "gpu/d3d9_state.h"
#include "gpu/hle_types.h"      // g_luminanceReadbackBits/Seq
#include "gpu/xenos_gpu_state.h"  // mx::gpu::alu -- the PM4 ALU constant file
#include "hooks/hooks_d3d9_shared.h"
#include "hooks/texture_dump.h"         // --texture_dump=true, logs/texdump
namespace mx::hooks::d3d9 {

struct ResolvedPixelBinding {
  std::vector<mx::hle::PixelTextureBinding> bindings;
  const char* fail = nullptr;
  uint32_t code_offset_dwords = 0;
  // Non-2D fetches passed over rather than rejected. Kept and reported so the
  // population stays visible: these are the slots the draw still does not get,
  // and a silent skip would look exactly like a shader that never wanted them.
  uint32_t skipped_fetches = 0;
  const char* skipped_kind = nullptr;
  bool decoded = false;
};
std::map<uint32_t, ResolvedPixelBinding> g_resolvedPixelBindings;
std::map<uint64_t, std::shared_ptr<const mx::hle::HleTexturePayload>>
    g_hleCpuTextures;
// Keys whose decode came out entirely zero. Not a set-and-forget flag, which
// made "blank" permanent: the key hashes the six fetch dwords -- where the
// texture lives and what shape it is -- never its contents, so a texture sampled
// while the guest was still streaming into it could never be reconsidered.
//
// Retrying cannot be free. The blank set is three FMT_8_8_8_8 surfaces at the
// game's render resolution, whose guest copy is legitimately empty and never
// fills in, so re-untiling ~9 MB every frame is pure waste. Each blank retry
// doubles the wait, up to a cap.
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
// The UI is Scaleform GFx 3.x. It keeps ONE 512x512 FMT_8 atlas per font and
// repacks it at runtime as strings appear and disappear. From the IDB:
//
//   sub_8293E720  rasterises one glyph into the cache, writing rows straight
//                 into the cache buffer addressed as `(y)*tex[5] + tex[6]`
//                 (tex = *(cache+696), [5] pitch, [6] base), then records a
//                 dirty rect through sub_8293DA08.
//   sub_8293C778  FLUSHES those rects: walks the texture slots at +56 (stride
//                 5), gathers each one's rects, calls the texture's vtable slot
//                 3 -- GTexture::Update -- and clears the count at +28.
//
// So sub_8293C778 is the exact moment the atlas contents change, and the
// pending-rect count at +28 says whether this call will change anything. It does
// NOT say which host texture changed, so the invalidation names them by
// GEOMETRY: sub_8293A888 creates each atlas with InitTexture(cache[0],
// cache[1], ...), and a kR8 texture is a glyph atlas only if its extent matches
// one the guest actually built.
//
// Naming them by FORMAT alone was wrong by three orders of magnitude -- the kR8
// population in a loaded frame is 5 MB, including a 2048x2048 that is not an
// atlas and the Bink Y/U/V planes -- and routing a texture here ALSO routes it
// away from GuestTextureFingerprint.
//===========================================================================
std::atomic<uint64_t> g_guestDrawCalls{0};

// Atomic for the reason spelled out at the extern in hooks_d3d9_internal.h: the
// guest guards this cache with its own critical section, so the flush hook can
// run on more than one thread. The generation is published with RELEASE and read
// with ACQUIRE -- the bump has to publish the atlas bytes orig_GlyphCacheFlush
// just wrote, or a reader can observe the new generation and re-decode the old
// pixels.
std::atomic<uint32_t> g_glyphCacheGeneration{1};


// sub_8293A888 is GetTexture: it hands back the atlas texture for a slot,
// creating it through OUR renderer's vtable on first use -- the one refusal
// point in the glyph chain that runs through our code, and a failure there is
// not recoverable and not retried, because sub_8293C778 clears the slot's dirty
// flag OUTSIDE the success test:
//
//     if (v5[4]) { if (sub_8293A888(...)) { ...Update... } v5[4] = 0; }
//
// so a failed create silently DISCARDS that slot's pending rects for the life of
// the cache -- the exact shape of "some letters never appear".

// PIN-MODE CENSUS -- see the read site in hooks_d3d9_entry.cpp for the offset
// derivation. BOTH arms are counted on purpose: a census with only the "held"
// arm cannot tell "the pin is always held" from "we never read the byte".


// sub_828A8C40 returning 0 is a glyph the guest asked for and did not get -- the
// failure itself rather than a proxy, so REFUSED at zero exonerates the raster
// cache outright, and calls is the denominator that makes that zero readable.
//
// Also counted: sub_828A8C40 returning SUCCESS but leaving out+20 (the atlas
// texture) null, so sub_828AC620 emits no quad; sub_828AC620's own per-line
// DROPPED verdict, the last measurable point before the vertex buffer, with
// UNREAD keeping a zero in it honest; and font loads with the truncation latch.





// Calls where out+20 could not be read, so SILENT == 0 means "did not happen"
// and not "could not look". See [[a-total-without-a-denominator]].

// Tiny -- one entry per distinct atlas geometry, which is one or two. The atomic
// is the fast path: the flush hook runs once per guest DrawText and the geometry
// is the same on essentially every call, so the lock is taken only for a new one.
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

// Fingerprint of the GUEST BYTES behind a texture, so the caches can notice that
// an address has been refilled with different artwork.
//
// The cache key is FNV-1a over the six fetch dwords, never what the texture
// contains. Swapping riders streams new gear into the SAME allocation at the
// same dimensions and format, so BOTH caches keep serving the previous rider --
// the decoded payload in g_hleCpuTextures (whose emplace never overwrites) and
// the GPU resource in m_gameTextures.
//
// Bounded so it can run on every bind: textures of 4 KB or less are hashed
// WHOLE, larger ones sampled at 32 fixed offsets, ~2 KB against the ~580 binds a
// frame this title makes. Returns 0 for memory it cannot read, which callers
// treat as "no opinion".
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
  // So does where the mip chain points. Only the base level's bytes are sampled
  // below -- that is the discriminator for a rider swap -- but a texture that
  // keeps its base and repoints its chain has still changed.
  h ^= source.mip_address;
  h *= 1099511628211ull;

  bool ok = true;
  // The readability probe is memoised PER PAGE. Every slice used to cost two
  // HostPageReadable calls, and that function is not free (an atomic plus a
  // linear scan of a 64-entry region cache): 32 slices x 2 probes x ~2800 slot
  // calls is ~180k calls a frame. Still probes every DISTINCT page, so it is no
  // weaker -- readability cannot vary inside one page.
  uint32_t last_ok_page = 0xFFFFFFFFu;
  const auto page_ok = [&](uint32_t a) {
    const uint32_t page = a & ~0xFFFu;
    if (page == last_ok_page) return true;
    if (!HostPageReadable(REX_RAW_ADDR(a))) return false;
    last_ok_page = page;
    return true;
  };
  const auto eat = [&](uint32_t offset, uint32_t n) {
    if (!ok) return;
    // Checked per slice rather than once at each end: the pages between are
    // not guaranteed mapped, and a fingerprint is not worth a fault.
    if (!page_ok(addr + offset) || !page_ok(addr + offset + n - 1)) {
      ok = false;
      return;
    }
    const auto* q = reinterpret_cast<const uint8_t*>(REX_RAW_ADDR(addr + offset));
    // EIGHT BYTES PER MULTIPLY, not one. FNV-1a's multiply is a serial
    // dependency, so a byte-at-a-time loop runs at the multiply's latency per
    // byte however wide the machine is: 2 KB per slot call over ~2800 calls is
    // 5.7 MB a frame down that chain, which is most of `stale-check 11ms`.
    // Consuming a whole word gives a DIFFERENT hash value, which matters not at
    // all -- this is a change detector whose outputs live only in memory.
    uint32_t i = 0;
    for (; i + 8 <= n; i += 8) {
      uint64_t w;
      std::memcpy(&w, q + i, sizeof(w));
      h ^= w;
      h *= 1099511628211ull;
    }
    for (; i < n; ++i) {
      h ^= q[i];
      h *= 1099511628211ull;
    }
  };

  constexpr uint32_t kWholeHashLimit = 4096;
  // STILL 32 SLICES, measured rather than left at a default. Coverage does
  // collapse as size grows -- 25% of an 8 KB texture, 0.1% of a 2 MB one -- but
  // scaling to one point per 4 KB was REVERTED: stale-check went 8ms -> 23ms per
  // interval and frame time ~118ms -> ~146ms, and it bought nothing, because
  // sampling cannot see a SPARSE write at any density worth paying for. The
  // texture it was meant to fix is handled at the cache insert instead, by not
  // caching a flat decode at all.
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
// against. One function so the store and the test cannot drift.
//
// The glyph atlas KEEPS the guest's own flush generation: an explicit signal
// beats a sampled read of the same memory, since the fingerprint samples 2 KB of
// a 256 KB atlas and a localised glyph write lands between its sample points.
//
// The fingerprint covers everything else, which until now was covered by nothing
// at all: GlyphCacheStale was gated on IsGlyphCacheFormat, so every
// BC1/BC3/BC5/RGBA8 texture in the game was never tested for staleness.
uint32_t TextureContentVersion(const mx::hle::HleTextureSource& source,
                               uint8_t* base,
                               mx::hle::HostTextureFormat format) {
  if (IsGlyphCacheTexture(format, source.width, source.height))
    return g_glyphCacheGeneration.load(std::memory_order_acquire);
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


// The FULL-HASH PROBE that lived here is GONE, and its answer is why: all eleven
// 1024x1024 BC3 atlases read FULL 1 over a freeroam level, up to 7 whole-buffer
// reads and 3,103 binds each, every read identical. Nothing there was ever stale
// and the missing UI art is absent at the source. One finding worth keeping:
//
//   0x104F2000 1024x1024 fmt15 2MB -- FULL 3 distinct of 6 reads
//                                     SAMPLED 1 distinct of 351 binds
//
// That is the terrain virtual-texture index map, where the sampled fingerprint
// is provably blind to real writes -- papered over by the flat-decode retry
// rather than fixed.

void NoteBlankDecode(uint64_t key) {
  BlankState& s = g_hleEmptyTextures[key];
  s.last_frame = mx::hle::D3D9FrameCount();
  ++s.strikes;
}

// FLAT-DECODE PROBE. MEASUREMENT ONLY -- nothing acts on this, and nothing
// should until it has said which side the flatness comes from:
//
//   guest flat, decode flat   -- the game's own data. Not our bug; stop here.
//   guest varied, decode flat -- the untile/copy/upload path lost it. Ours.
//
// It measures the DOMINANT ELEMENT'S SHARE rather than demanding every byte be
// identical, which is what made HleTextureIsConstant miss the texture the black
// ground hung on: a 1024x1024 B4G4R4A4 tile-index map uniform on every texel
// except (0,0).
//
// PER TEXEL, NOT PER BYTE. A byte histogram can only see byte-uniform constants
// -- 0x00, 0x80, 0xFF -- and the constant word 0x0AF0 is `0A F0 0A F0`, a
// dominant share of exactly 0.5. Boyer-Moore majority over `element_bytes`-wide
// elements: O(1) memory, exact whenever the dominant element is over half, which
// is the only region the 99.9% threshold cares about.
struct FlatScan {
  // Element-level: what the threshold reads.
  uint64_t dominant = 0;
  uint32_t element_bytes = 1;
  size_t total = 0;            // elements, not bytes
  size_t dominant_count = 0;
  // Byte-level, kept alongside because "how many distinct byte values" is still
  // the cheapest way to say whether a buffer carries any variety at all.
  uint32_t distinct_bytes = 0;
  double share() const {
    return total ? double(dominant_count) / double(total) : 0.0;
  }
};

FlatScan ScanFlatness(const uint8_t* data, size_t bytes, uint32_t element_bytes) {
  FlatScan s;
  if (!data || !bytes) return s;
  // 1, 2, 4 or 8 only. A BC block is 8 or 16 bytes; 16 folds to 8, which still
  // reads a constant block as constant.
  uint32_t w = element_bytes ? element_bytes : 1;
  if (w >= 8) w = 8;
  else if (w >= 4) w = 4;
  else if (w >= 2) w = 2;
  else w = 1;
  s.element_bytes = w;

  // A 256-BIT SET, not a 256-ENTRY HISTOGRAM, stopping once every value has been
  // seen. `distinct_bytes` is used by three log lines and nothing else, and the
  // old form counted every byte into a 2 KB array -- 4 million dependent
  // read-modify-writes for a 4 MB texture, measured at 76ms of scan for THREE
  // decoded textures on a ~160ms frame. The count is still EXACT.
  {
    uint64_t seen[4] = {};
    uint32_t found = 0;
    for (size_t i = 0; i < bytes && found < 256; ++i) {
      const uint8_t b = data[i];
      const uint64_t bit = 1ull << (b & 63u);
      uint64_t& word = seen[b >> 6];
      if (!(word & bit)) {
        word |= bit;
        ++found;
      }
    }
    s.distinct_bytes = found;
  }

  const size_t n = bytes / w;
  if (!n) return s;

  // WIDTH-SPECIALISED, because the generic version could not vectorise. These
  // two passes are NOT diagnostics -- `dominant` decides residency and the
  // flat-retry backoff -- so neither can be sampled or dropped. A `memcpy` of a
  // RUNTIME width into a uint64 per element is a call-shaped load the compiler
  // cannot widen: at w=1 that is one per byte of a 2.7 MB texture, twice, 9ms of
  // scan for TWO decodes on a 30ms frame. Hoisting the width into the type makes
  // it a plain aligned load of a fixed size, and memcpy with a COMPILE-TIME size
  // is well defined for any alignment and folds to that single load.
  const auto scan_w = [&]<typename T>(T) {
    const auto load = [data](size_t i) {
      T v;
      std::memcpy(&v, data + i * sizeof(T), sizeof(T));
      return v;
    };
    T cand = 0;
    size_t votes = 0;
    for (size_t i = 0; i < n; ++i) {
      const T v = load(i);
      if (!votes) { cand = v; votes = 1; }
      else if (v == cand) ++votes;
      else --votes;
    }
    size_t count = 0;
    for (size_t i = 0; i < n; ++i) count += (load(i) == cand);
    s.dominant = uint64_t(cand);
    s.dominant_count = count;
  };
  switch (w) {
    case 8: scan_w(uint64_t{}); break;
    case 4: scan_w(uint32_t{}); break;
    case 2: scan_w(uint16_t{}); break;
    default: scan_w(uint8_t{}); break;
  }
  s.total = n;
  return s;
}

// The renderer's staleness key: a hash of the bytes we just decoded. Separate
// from content_version on purpose -- that one is a 2 KB SAMPLE of guest memory
// and must stay one, so it cannot see a sparse write, and without this the fresh
// bytes stopped at the GPU boundary because EnsureGameTexture compared the
// sample and saw no change. Runs once per DECODE, not per bind.
//
// Below: the snapshot slot's sampler word -- clamp in bits 12-13, POINT in 14,
// 15 saying the word was filled in at all, and the guest swizzle in the low 12,
// which a snapshot slot deliberately does NOT apply (applying it turned the
// rider cyan).
//
// TWO producers, and that is the point of this function: the full-snapshot
// branch reads the fetch dwords directly, while the PARTIAL-snapshot binds have
// only `source`. Before this, every partially resolved snapshot reached the
// sampler with a zero word and took the hardcoded clamped POINT.
uint16_t PackSnapshotSamplerWord(uint32_t swizzle, uint32_t clamp_x,
                                 uint32_t clamp_y, bool linear_filter) {
  uint16_t packed = uint16_t(swizzle & 0xFFFu);
  // SamplerVariantFor's rule: kRepeat (0) and kMirroredRepeat (1) wrap,
  // everything at or above 2 clamps. Applied here so no site can drift on it.
  if (clamp_x >= 2u) packed |= uint16_t(1u << 12);
  if (clamp_y >= 2u) packed |= uint16_t(1u << 13);
  if (!linear_filter) packed |= uint16_t(1u << 14);
  packed |= uint16_t(1u << 15);
  return packed;
}

// The PARTIAL-snapshot binds: carry the guest's FILTER and deliberately NOT its
// clamp. The terrain tile atlas binds through here -- an atlas is sparse by
// design, so it fails the coverage gate on every bind -- and its fetch constant
// says CLAMP/CLAMP while the atlas is sampled at U = 1.34: clamped that pins to
// the right edge and reads an empty tile, which is the BLACK GROUND; wrapped it
// is tile 2, which holds sand.
//
// Why the guest can say clamp and mean wrap is not settled: the Xenos sampler
// has more modes than the two this maps onto, and the shader's own address
// arithmetic may already fold the wrap in.
uint16_t PartialSnapshotSamplerWord(const mx::hle::HleTextureSource& source) {
  return PackSnapshotSamplerWord(source.swizzle, /*clamp_x=*/0, /*clamp_y=*/0,
                                 source.linear_filter);
}


uint32_t PayloadUploadVersion(const mx::hle::HleTexturePayload& payload) {
  uint64_t h = 1469598103934665603ull;
  const uint8_t* p = payload.data.data();
  size_t n = payload.data.size();
  while (n >= 8) {
    uint64_t word;
    std::memcpy(&word, p, 8);
    h = (h ^ word) * 1099511628211ull;
    p += 8;
    n -= 8;
  }
  while (n--) h = (h ^ *p++) * 1099511628211ull;
  const uint32_t folded = uint32_t(h ^ (h >> 32));
  // 0 is reserved for "never computed", which is what a blank or Bink payload
  // carries and what makes those upload once and stay put.
  return folded ? folded : 1u;
}

// Computes it, stores it, and says out loud the first time a re-decode of one
// address produces DIFFERENT bytes. `TEXTURE REPEATS ... 98stale` cannot answer
// that: the miss reason is kStaleEvicted both when the fingerprint changed AND
// when the flat-retry backoff forced a re-read, so a texture the guest never
// touches and one it rewrites constantly print the same number.
void SetPayloadUploadVersion(const mx::hle::HleTextureSource& source,
                             mx::hle::HleTexturePayload& payload) {
  uint32_t previous = 0;
  {
    PhaseTimer t(g_tex.scanUs);
    payload.upload_version = PayloadUploadVersion(payload);
  }
  {
    // Its own lock rather than g_flatMutex. The rule is the same one and it is
    // not negotiable: this runs on GUEST THREADS from every decode site, and an
    // unguarded container insert from several of them corrupted the heap once
    // already -- the fault surfaced as a bad pointer in recompiled guest code
    // nowhere near here. A diagnostic is not exempt.
    static std::mutex s_uploadMutex;
    std::lock_guard<std::mutex> upload_lock(s_uploadMutex);
    static std::map<uint32_t, uint32_t> s_lastUpload;
    static std::map<uint32_t, uint32_t> s_printed;
    auto& slot = s_lastUpload[source.address];
    previous = slot;
    slot = payload.upload_version;
    if (!previous || previous == payload.upload_version) return;
    // EIGHT per address, not one. "The bytes changed" is not the question -- the
    // question is WHAT THEY NOW HOLD, and a single first-change line cannot
    // distinguish a virtual-texture map that has come alive from one constant
    // word being replaced by another.
    auto& printed = s_printed[source.address];
    // 40 rather than 8: the per-LEVEL coverage below is a TIME SERIES, and the
    // question it answers -- does the guest's page-table coverage climb toward
    // full or plateau -- cannot be read from a handful of samples. Changes
    // arrive about once a second, so 40 is roughly the first minute in a level.
    if (printed >= 40 || s_printed.size() > 48) return;
    ++printed;
  }
  // The CONTENT itself, on the base level, in the two readings that separate
  // "alive" from "still one word": the dominant element's share, and three
  // spread texels. A virtual-texture index map doing its job has a LOW dominant
  // share and three different texels.
  FlatScan scan{};
  std::string probe;
  {
    PhaseTimer t(g_tex.scanUs);
    const size_t base_bytes =
        payload.level_count > 1
            ? std::min<size_t>(payload.levels[1].offset, payload.data.size())
            : payload.data.size();
    scan = ScanFlatness(payload.data.data(), base_bytes, source.bytes_per_block);
    const uint32_t bpp =
        payload.width ? uint32_t(payload.row_pitch / payload.width) : 0;
    if (bpp && bpp <= 16) {
      const uint32_t pts[3][2] = {{payload.width / 2, payload.height / 2},
                                  {payload.width / 4, payload.height / 4},
                                  {0, 0}};
      for (const auto& pt : pts) {
        const size_t off =
            size_t(pt[1]) * payload.row_pitch + size_t(pt[0]) * bpp;
        if (off + bpp > base_bytes) continue;
        probe += fmt::format(" ({},{})=", pt[0], pt[1]);
        for (uint32_t b = 0; b < bpp; ++b)
          probe += fmt::format("{:02X}", payload.data[off + b]);
      }
    }
  }
  // PER-LEVEL COVERAGE, for a texture that carries a mip chain. The terrain's
  // virtual-texture page table is a pyramid, and "resident" for it means "not
  // the 0xF00A not-available sentinel"; a single base-level share cannot say
  // what is wrong with it, since mip 0 was 0.2% resident, mip 4 21.5% and mip 6
  // 50%, and the visible defect came from the composite sampling a COARSE level
  // at an unwritten texel. Counted against the BASE level's dominant element.
  std::string levels;
  if (payload.level_count > 1 && scan.element_bytes) {
    PhaseTimer t(g_tex.scanUs);
    for (uint32_t l = 0; l < payload.level_count && l < 14; ++l) {
      const size_t begin = payload.levels[l].offset;
      const size_t end = (l + 1 < payload.level_count)
                             ? payload.levels[l + 1].offset
                             : payload.data.size();
      if (begin >= end || end > payload.data.size()) continue;
      const size_t width = scan.element_bytes;
      size_t total = 0, resident = 0;
      for (size_t off = begin; off + width <= end; off += width) {
        uint64_t v = 0;
        std::memcpy(&v, payload.data.data() + off, width);
        ++total;
        if (v != scan.dominant) ++resident;
      }
      if (!total) continue;
      levels += fmt::format(" L{}:{}/{}", l, resident, total);

      // THE SHAPE OF THE COARSE LEVELS, not just their ratio. L5, L6 and L7 all
      // read EXACTLY 50% resident -- 512/1024, 128/256, 32/64. Three consecutive
      // levels at precisely one half is not a streaming curve (the levels above
      // are ragged: 0.3%, 0.4%, 2.0%), it is a factor of two in how pages are
      // marked or indexed, and the coarse levels are the fallback.
      //
      // A ratio cannot say WHICH half -- checkerboard, half-plane and
      // alternating rows all read 50% with completely different causes -- so the
      // smallest levels are printed verbatim.
      if (total <= 64) {
        std::string raw;
        uint32_t col = 0;
        const uint32_t side = uint32_t(std::lround(std::sqrt(double(total))));
        for (size_t off = begin; off + width <= end; off += width) {
          uint64_t v = 0;
          std::memcpy(&v, payload.data.data() + off, width);
          // Row breaks so a 2D pattern is visible as one, rather than having
          // to be reconstructed from a flat run of 64 values.
          if (side && col && (col % side) == 0) raw += " /";
          raw += fmt::format(" {:0{}X}", v, width * 2);
          ++col;
        }
        levels += fmt::format(" [L{} raw{}]", l, raw);
      }
    }
  }
  REXLOG_INFO(
      "d3d9: TEXTURE CONTENT CHANGE addr 0x{:08X} {}x{} fmt {} -- decoded bytes "
      "differ from the previous decode (upload version 0x{:08X} -> 0x{:08X}); "
      "base level dominant 0x{:X} share {:.5f} of {} elems, {} distinct bytes |"
      " probe{} | resident per level{}",
      source.address, source.width, source.height, source.guest_format,
      previous, payload.upload_version, scan.dominant, scan.share(), scan.total,
      scan.distinct_bytes, probe.empty() ? std::string(" (none)") : probe,
      levels.empty() ? std::string(" (no chain)") : levels);
}

// Fires AND population, printed on every line the probe emits, so the rate
// travels with the evidence instead of having to be looked up separately.
struct FlatProbeCounts {
  uint64_t decodes = 0;
  uint64_t flat = 0;
  // What the per-address cap threw away. See the note at the cap.
  uint64_t suppressed = 0;
};
FlatProbeCounts g_flatProbe;
// Keys whose last decode came out flat, with the frame it happened on. A flat
// texture is re-read on a BACKOFF rather than every bind.
// A watched key: when it was last re-read, and whether its content has been seen
// to CHANGE. The two get different intervals -- see kVolatileRetryFrames.
struct FlatWatch {
  uint32_t last_frame = 0;
  bool volatile_content = false;
};
std::map<uint64_t, FlatWatch> g_flatRetryKeys;
// Still uniform: nothing has changed yet, so re-read it rarely.
constexpr uint32_t kFlatRetryFrames = 30;
// PROVEN TO CHANGE: re-read it often. The terrain page table is rewritten by the
// guest every frame, so at 30 frames we render from a mapping up to half a
// second old.
//
// What makes 4 affordable is skipping the flatness scan, not raw budget: the
// measurement that killed "re-read on every bind" was `decode 359ms, scan 651ms
// | 50 decodes over 36880 KB`, and ScanFlatness makes three full passes. On a
// key already known to change that scan asks a question whose answer we have.
constexpr uint32_t kVolatileRetryFrames = 4;
FlatDecodeCensus g_flat;
// Watched keys whose re-read came back with CONTENT -- i.e. the texture really
// does change under us, and dropping it from the watch set would pin it. See
// the note at the cache insert for what dropping the page table cost.

// THE PROBE CRASHED THE GAME AND THIS IS WHY. The census below inserts into a
// std::map and a std::set, and once NoteDecodedTexture was called from all three
// decode sites those inserts happened on SEVERAL GUEST THREADS at once.
// Concurrent std::map::insert corrupts the heap, and the fault that surfaced was
// nowhere near here: an access violation on the UI thread inside recompiled
// guest code, reading 0x4C69746C -- ASCII "Litl", a string dereferenced as a
// pointer. A diagnostic is not exempt from the locking rule. The lock is
// uncontended in practice and covers the counters too, so the printed rate is
// not a torn read.
std::mutex g_flatMutex;

// DECODE CENSUS BY SHAPE. The per-address lines answer "is this texture flat";
// this answers "was it decoded AT ALL", because absence and health look
// identical without the population -- one run decoded 248 textures and printed
// eight flat ones, with no way to tell whether the 1024x1024 FMT_4_4_4_4 the
// black ground hangs on was ABSENT or merely healthy.
//
// Keyed on {guest format, width, height} rather than address, since the question
// is about a kind of texture. The SITE is carried too -- `slot` is a translated
// draw, `standin` the fallback route, `bink` video -- because a real textured
// draw and the stand-in path picking a texture up have opposite consequences.
struct DecodeShape {
  uint64_t decodes = 0;
  uint64_t flat = 0;
  uint32_t site_mask = 0;  // bit per kSiteNames index
};
constexpr const char* kSiteNames[] = {"slot", "standin", "bink"};

uint32_t DecodeSiteBit(const char* site) {
  for (uint32_t i = 0; i < std::size(kSiteNames); ++i)
    if (std::strcmp(site, kSiteNames[i]) == 0) return 1u << i;
  return 1u << 31;  // unknown site, still visible in the line
}

std::string DecodeSiteNames(uint32_t mask) {
  std::string out;
  for (uint32_t i = 0; i < std::size(kSiteNames); ++i)
    if (mask & (1u << i)) out += (out.empty() ? "" : "+") + std::string(kSiteNames[i]);
  if (mask & (1u << 31)) out += (out.empty() ? "" : "+") + std::string("?");
  return out.empty() ? "none" : out;
}

std::map<std::array<uint32_t, 3>, DecodeShape> g_flatShapes;

void ReportDecodeShapes() {
  std::vector<std::pair<std::array<uint32_t, 3>, DecodeShape>> ranked(
      g_flatShapes.begin(), g_flatShapes.end());
  std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
    if (a.second.flat != b.second.flat) return a.second.flat > b.second.flat;
    return a.second.decodes > b.second.decodes;
  });
  // EVERY shape, not a top-N. The top-20 cut was the THIRD truncation in this
  // one instrument to hide the case it was built for: a 1024x1024 FMT_4_4_4_4
  // that decodes with REAL content sorts to the bottom (flat=0, n small), gets
  // cut, and prints no FLAT DECODE line either. The list is ~74 entries.
  std::string top;
  for (size_t i = 0; i < ranked.size(); ++i) {
    top += fmt::format(" [fmt{}({}) {}x{} n={} flat={} via {}]",
                       ranked[i].first[0],
                       mx::hle::GuestTextureFormatName(ranked[i].first[0]),
                       ranked[i].first[1], ranked[i].first[2],
                       ranked[i].second.decodes, ranked[i].second.flat,
                       DecodeSiteNames(ranked[i].second.site_mask));
  }
  REXLOG_INFO("d3d9: DECODE SHAPES {} distinct over {} decodes, {} flat:{}",
              g_flatShapes.size(), g_flatProbe.decodes, g_flatProbe.flat,
              top.empty() ? " (none)" : top);
}

// EVERY DECODE SITE, and that is the point. This lived inline in
// ResolvePixelSlotTexture and therefore measured ONE of the three places a
// texture is decoded. DecodeHleTexture2D is called from the Bink plane path,
// from PrepareDrawTexture (the stand-in route) and from ResolvePixelSlotTexture
// (the translated route), and they SHARE A CACHE -- so a texture first decoded
// by one route is a cache hit for the others and never reaches their code. The
// 1024x1024 B4G4R4A4 tile-index map was present as a bound payload while this
// census reported no such decode in the same run.
void NoteDecodedTexture(const mx::hle::HleTextureSource& source,
                        const mx::hle::HleTexturePayload& payload,
                        const uint8_t* guest_bytes, size_t guest_size,
                        uint32_t sampler, const char* site) {
  PhaseTimer t(g_tex.scanUs);
  std::lock_guard<std::mutex> flat_lock(g_flatMutex);
  const size_t flat_base_bytes =
      payload.level_count > 1
          ? std::min<size_t>(payload.levels[1].offset, payload.data.size())
          : payload.data.size();
  const FlatScan decoded_flat = ScanFlatness(payload.data.data(),
                                             flat_base_bytes,
                                             source.bytes_per_block);
  ++g_flatProbe.decodes;
  auto& shape = g_flatShapes[{source.guest_format, source.width,
                              source.height}];
  ++shape.decodes;
  shape.site_mask |= DecodeSiteBit(site);
  // Every 500 rather than every 100: the line now carries every shape, and the
  // Bink planes decode each frame, so 4100 decodes a run would otherwise be 41
  // copies of a 3 KB line.
  if (g_flatProbe.decodes % 500 == 0) ReportDecodeShapes();
  if (decoded_flat.total && decoded_flat.share() >= 0.999) {
    ++shape.flat;
    ++g_flatProbe.flat;
    // THE CAP WAS 24 AND IT HID THE CASE THIS EXISTS FOR. One run reached 24
    // addresses at decode 236 and then rendered 1860 more frames; every texture
    // first bound after that -- the terrain's tile-index map among them -- was
    // dropped without a word. A limit whose effect is invisible is the same
    // defect as a counter that cannot fire, so the cap now counts what it drops.
    static std::set<uint32_t> s_flatSeen;
    const bool flat_first_seen = s_flatSeen.insert(source.address).second;
    if (flat_first_seen && s_flatSeen.size() > 256) ++g_flatProbe.suppressed;
    if (flat_first_seen && s_flatSeen.size() <= 256) {
      // The guest bytes as COPIED, before untiling -- the only reading that
      // can acquit or convict our decode path.
      const FlatScan guest_flat =
          ScanFlatness(guest_bytes, guest_size, source.bytes_per_block);
      std::string head;
      for (size_t i = 0; i < guest_size && i < 16; ++i)
        head += fmt::format("{:02X} ", guest_bytes[i]);
      REXLOG_INFO(
          "d3d9: FLAT DECODE [{}] addr 0x{:08X} fmt {} ({}) {}x{} sampler {} "
          "pitch {} blk {}x{}x{}B endian {} swizzle 0x{:X} tiled {} "
          "packed_mips {} mip_addr 0x{:08X} levels {} | DECODED dom 0x{:X} "
          "{:.5f} of {} x{}B elems, {} distinct bytes | GUEST dom 0x{:X} "
          "{:.5f} of {} x{}B elems, {} distinct bytes | guest head {}| "
          "flat {}/{} decodes, {} addresses, {} suppressed",
          site, source.address, source.guest_format,
          mx::hle::GuestTextureFormatName(source.guest_format), source.width,
          source.height, sampler, source.pitch_blocks,
          source.block_width, source.block_height, source.bytes_per_block,
          source.endian, source.swizzle, source.tiled ? 1 : 0,
          source.packed_mips ? 1 : 0, source.mip_address, source.level_count,
          decoded_flat.dominant, decoded_flat.share(), decoded_flat.total,
          decoded_flat.element_bytes, decoded_flat.distinct_bytes,
          guest_flat.dominant, guest_flat.share(), guest_flat.total,
          guest_flat.element_bytes, guest_flat.distinct_bytes, head,
          g_flatProbe.flat,
          g_flatProbe.decodes, s_flatSeen.size(), g_flatProbe.suppressed);
    }
  }
}

// The blank payload itself, so the draws that sample a still-blank key within
// one frame share a decode instead of repeating it.
std::map<uint64_t, std::shared_ptr<const mx::hle::HleTexturePayload>>
    g_hleBlankPayloads;

const ResolvedPixelBinding* ResolvePixelProfile(uint32_t handle) {
  // This used to search every PM4-captured pixel shader for a byte match inside
  // the D3D9 allocation, to locate where the CF stream began. CollectPixelShaderBlob
  // now reads that offset out of the shader object itself, the same field
  // sub_82565928 reads when it programs the hardware, so the resolve is final on
  // the first try.
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
      code, code_count, resolved.bindings, &resolved.fail,
      &resolved.skipped_fetches);
  if (resolved.skipped_fetches) resolved.skipped_kind = resolved.fail;
  if (!resolved.decoded) {
    // Pixel shader allocations with literal constants place those values in
    // front of the CF stream (the loaded main-pass shaders consistently begin at
    // dword 16). Do not hardcode that: try a bounded set of suffixes and accept
    // only a unique valid decode; a second valid alignment leaves the draw on
    // the colour fallback.
    //
    // WATCH THIS COUNT. The acceptance test is "DecodePixelTextureFetches
    // returned true", which became more permissive when a non-2D fetch started
    // being skipped rather than rejecting the blob, so alignments once
    // disqualified by one odd fetch can come back valid. If the ambiguous count
    // climbs, this loop -- not the skip -- needs tightening.
    uint32_t valid_offsets = 0;
    std::vector<mx::hle::PixelTextureBinding> unique_bindings;
    uint32_t unique_offset = 0;
    uint32_t unique_skipped = 0;
    const char* unique_kind = nullptr;
    const uint32_t limit =
        std::min<uint32_t>(64, uint32_t(bi->second.size()));
    for (uint32_t offset = 1; offset + 3 <= limit; ++offset) {
      std::vector<mx::hle::PixelTextureBinding> candidate;
      const char* candidate_fail = nullptr;
      uint32_t candidate_skipped = 0;
      if (!mx::hle::DecodePixelTextureFetches(
              bi->second.data() + offset,
              uint32_t(bi->second.size()) - offset, candidate,
              &candidate_fail, &candidate_skipped))
        continue;
      ++valid_offsets;
      unique_offset = offset;
      unique_skipped = candidate_skipped;
      unique_kind = candidate_fail;
      unique_bindings = std::move(candidate);
    }
    if (valid_offsets == 1) {
      resolved.decoded = true;
      resolved.fail = nullptr;
      resolved.code_offset_dwords = unique_offset;
      resolved.bindings = std::move(unique_bindings);
      // Carried through the retry as well, or a shader resolved at a suffix
      // would silently under-report what it lost.
      resolved.skipped_fetches = unique_skipped;
      resolved.skipped_kind = unique_skipped ? unique_kind : nullptr;
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
  REXLOG_INFO("d3d9: pixel shader 0x{:08X} texture profile: {}{}{}{}; source {}",
              handle,
              profile.decoded
                  ? fmt::format("{} 2D fetch(es)", profile.bindings.size())
                  : "rejected",
              profile.skipped_fetches
                  ? fmt::format(" (+{} SKIPPED: {})", profile.skipped_fetches,
                                profile.skipped_kind ? profile.skipped_kind
                                                     : "?")
                  : "",
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
    // CAP WAS 16, and 16 is not a population. One run rejected 75 of 178
    // distinct pixel shaders here -- 42% -- and this dump showed the first 16,
    // which reads exactly like "16 shaders are rejected". The cap now clears a
    // level's whole shader set, and stays a cap only so a handle-recycling run
    // cannot get the log back by this route.
    //
    // The REASON rides on this line too: it is already on the profile line
    // above, but the point of the dwords is to read them against the reason that
    // refused them.
    static std::map<uint32_t, bool> s_dumped_rejected;
    if (s_dumped_rejected.size() < 256 &&
        s_dumped_rejected.emplace(handle, true).second) {
      std::string words;
      uint32_t shown = 0;
      for (uint32_t i = 0; i < bi->second.size() && shown < 16; ++i) {
        if (!bi->second[i]) continue;
        words += fmt::format(" [{}]={:08X}", i, bi->second[i]);
        ++shown;
      }
      REXLOG_INFO("d3d9: rejected pixel shader 0x{:08X} ({}) first nonzero "
                  "allocation dwords:{}",
                  handle, profile.fail ? profile.fail : "?",
                  words.empty() ? " none" : words);
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

  // Multiple fetch instructions may still describe one host texture. Blur passes
  // in ST_Southwest issue 3 or 9 taps of s0 from the same interpolator; base-mip
  // HLE cannot reproduce their offsets/ALU yet, but one s0 sample is the explicit
  // approximation this milestone permits.
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
  // for this exact fetch order/linkage; do not generalise "sampler zero wins".
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
// first list emits LOAD_ALU_CONSTANT packets. The SECOND list is different: each
// entry is `(u16 byte_offset, u16 dword_count, inline payload)` and the guest
// copies that payload to `device + 0x480 + byte_offset`, whose first 0xC0 bytes
// are the 32 six-dword texture fetch constants. Verified in sub_825506E8 and
// sub_825508A8; searching the FIRST list for ALU register indexes reaching
// 0x4800 finds nothing, because no shader publishes such an entry.
//
// Cached per pixel-shader handle rather than re-walked per draw. The two
// shaders' patch lists are merged because either may carry state for the shared
// device constants block.
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
// never the thread-local one: a worker-thread draw would name the wrong shader,
// which here means binding another draw's texture rather than merely missing one.
//
// Below: WHY a fetch could not be found. This used to return a bare `false` for
// six different situations, and its caller asserted one of them -- "the guest
// never bound anything to this sampler" -- which is true for exactly ONE; the
// other five are us failing to find a binding that exists, indistinguishable
// from the outside while the result is a black texel either way.
//
// Ordered MOST-SPECIFIC-FIRST, and every path terminates in exactly one code. An
// earlier reason-code chain in this tree was ordered the other way, made its
// last entry unreachable, and printed a permanent zero that read as data.
enum class FetchMiss : uint32_t {
  kNoDevice,          // device pointer is 0 -- OURS
  kShadowUnreadable,  // device shadow page would fault -- OURS
  kNotATexture,       // shadow read fine, type != kTexture -- GUEST, black is
                      // the hardware's own answer and nothing is wrong
  kPublishedNoType,   // shader published a descriptor, but not a texture one
  kThreadLocalUnset,  // no thread-local binding either -- see
                      // [[device-state-is-thread-local]]: a worker thread can
                      // legitimately have none while the drawing thread does
  kThreadLocalInvalid,  // bound but flagged invalid
  kCount
};
const char* FetchMissName(FetchMiss m) {
  switch (m) {
    case FetchMiss::kNoDevice: return "no-device(OURS)";
    case FetchMiss::kShadowUnreadable: return "shadow-unreadable(OURS)";
    case FetchMiss::kNotATexture: return "not-a-texture(GUEST)";
    case FetchMiss::kPublishedNoType: return "published-not-texture";
    case FetchMiss::kThreadLocalUnset: return "thread-local-unset";
    case FetchMiss::kThreadLocalInvalid: return "thread-local-invalid";
    default: return "?";
  }
}
uint64_t g_fetchMiss[size_t(FetchMiss::kCount)] = {};
// Per sampler AND per reason. The aggregate cannot say whether s1's failures
// are the same kind as s5's, and the whole question is whether one family of
// slots is legitimately unbound while another is being lost.
std::map<std::pair<uint32_t, uint32_t>, uint64_t> g_fetchMissBySampler;

bool ReadLiveTextureFetch(uint32_t device, uint8_t* base, uint32_t sampler,
                          uint32_t out[6], uint32_t ps_handle = 0,
                          FetchMiss* out_miss = nullptr) {
  const auto miss = [&](FetchMiss m) {
    ++g_fetchMiss[size_t(m)];
    ++g_fetchMissBySampler[{sampler, uint32_t(m)}];
    if (out_miss) *out_miss = m;
    return false;
  };
  if (!out || sampler >= mx::hle::kMaxSamplers) return false;
  std::memset(out, 0, sizeof(uint32_t) * 6);
  const uint32_t fetch_at = device + 0x480 + sampler * 24;
  bool shadow_readable = false;
  if (device) {
    shadow_readable = HostPageReadable(REX_RAW_ADDR(fetch_at)) &&
                      HostPageReadable(REX_RAW_ADDR(fetch_at + 20));
  }
  if (shadow_readable) {
    for (uint32_t i = 0; i < 6; ++i)
      out[i] = REX_LOAD_U32(fetch_at + i * 4);
    // FetchConstantType::kTexture == 2 (SDK rex/graphics/xenos.h:1093-1098).
    if ((out[0] & 3u) == 2u) return true;
  }
  // Second, and only when the device shadow has nothing: the descriptor the
  // shader published for itself. A live SetTexture must still win, so this sits
  // below the shadow rather than above it.
  bool published_seen = false;
  if (ps_handle) {
    uint32_t published[6] = {};
    if (ShaderPublishedFetch(ps_handle, sampler, published)) {
      published_seen = true;
      if ((published[0] & 3u) == 2u) {
        std::memcpy(out, published, sizeof(uint32_t) * 6);
        ++g_shaderFetchServed;
        return true;
      }
    }
  }
  const auto& tb = DeviceState().texture[sampler];
  if (tb.bound && tb.valid) {
    std::memcpy(out, tb.fetch, sizeof(uint32_t) * 6);
    return true;
  }
  // Nothing produced a binding. Attribute it to the most specific thing that
  // went wrong, hardest evidence first: a shadow we could READ and that said
  // "not a texture" is the guest's own answer and outranks every downstream
  // miss, because the later sources are fallbacks for a shadow we could not
  // consult at all.
  if (!device) return miss(FetchMiss::kNoDevice);
  if (!shadow_readable) return miss(FetchMiss::kShadowUnreadable);
  if ((out[0] & 3u) != 2u) return miss(FetchMiss::kNotATexture);
  if (published_seen) return miss(FetchMiss::kPublishedNoType);
  if (tb.bound) return miss(FetchMiss::kThreadLocalInvalid);
  return miss(FetchMiss::kThreadLocalUnset);
}

// One line, every reason, zeros included -- and the per-sampler split beside
// it, because "s1 is a different failure from s5" is the finding this exists to
// make visible.
std::string FetchMissReport() {
  std::string out;
  uint64_t total = 0;
  for (uint64_t n : g_fetchMiss) total += n;
  for (uint32_t i = 0; i < uint32_t(FetchMiss::kCount); ++i)
    out += fmt::format(" {}={}", FetchMissName(FetchMiss(i)), g_fetchMiss[i]);
  out += fmt::format(" | total {}", total);
  std::string by;
  for (const auto& [k, n] : g_fetchMissBySampler)
    by += fmt::format(" s{}:{}={}", k.first, FetchMissName(FetchMiss(k.second)),
                      n);
  if (!by.empty()) out += "\n  by sampler:" + by;
  return out;
}

// The milestone can sample one texture even when the guest shader uses many.
// Pick from evidence in the live descriptors: normalized colour storage is a
// closer approximation to the shader's visible base colour than BC5 normal maps,
// float intermediates, or unnormalized render-target inputs. Ties retain shader
// instruction order; no sampler number is treated as a semantic.
//
// Below: per-guest-format tally of descriptors the HLE decoder turned down,
// shared by both rejection sites. This replaced a flat "log the first 12" cap,
// which could spend its whole budget on one format.
std::map<uint32_t, uint64_t> g_hleRejectedFormats;

void NoteRejectedTextureFormat(const char* site, uint32_t sampler,
                               const mx::hle::HleTextureSource& source,
                               const char* why, const uint32_t fetch[6]) {
  const uint32_t fmt = source.guest_format;
  ++g_hleRejectedFormats[fmt];
  // Logged once per (format, REASON), not once per format. The tally above stays
  // keyed on format alone because that is what RejectedFormatSummary ranks, but
  // the gate cannot: a format already turned down for one reason would swallow
  // every later reason for the same format, and the reason is the only part that
  // says what work would fix it. It matters for "texture is a 3D volume", which
  // was unreachable while tfetch3D shaders were refused whole by the emitter.
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
// plane textures to samplers 0/1/2 -- Y, Cr, Cb -- plus an optional alpha plane
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

// The single gate for the whole Bink path: both routing sites call this before
// touching planes, so returning false disconnects decode, upload and the plane
// budget in one place rather than stubbing three.
//
// Disconnecting is a DIAGNOSTIC, not a fix. The path is measured healthy, so
// anything that changes when it is off is a change in what the video draws do to
// the frame, not a repair of the decoder. With the cvar set, the BINK PLANES
// line reports 0 calls, which is how the log shows the switch took effect rather
// than the path merely being quiet.
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

  // Report the handles themselves whether or not a draw ever matches. All three
  // zero means the guest never created its Bink shaders — a different problem
  // from a plane format we cannot decode, and otherwise identical from the
  // outside, since both produce a probe that never fires.
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

// Decode the Bink composite's plane set into the DrawCall. Deliberately separate
// from PrepareDrawTexture:
//
//  - it must bind *several* textures, which the single-winner binding contest in
//    ResolvePixelBindingForDraw cannot express;
//  - the planes are k_8, which the semantic gate correctly refuses as base
//    colour for a mask but wrongly for a luma plane; here the guest's own shader
//    identity says what they are, so the gate is not consulted;
//  - it must not touch g_hleCpuTextures. The planes are new guest memory every
//    video frame, so caching them by payload key would grow without bound.
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
      // this break is how the loop terminates normally -- charged
      // unconditionally it reported 646 "no-fetch" against 1886 calls that all
      // succeeded. Only a break leaving too few planes is a failure, and
      // `too_few` below counts that.
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
    NoteDecodedTexture(source, *payload, guest.data(), guest.size(), s, "bink");
    // An all-zero plane is normal for a video that has not decoded its first
    // frame yet, so unlike the immutable path this is not memoised as empty —
    // the same descriptor will carry real pixels a frame later.
    payload->key = HleTextureKey(fetch);
    // Crop the chroma planes to their logical extent, while the payload is still
    // local and mutable.
    //
    // The guest allocates them with the dimensions rounded up, so half of a
    // 216-row luma arrives as a 320x112 descriptor rather than 320x108. The
    // composite shader samples every plane with the same normalized uv and
    // leaves the half-size difference to the sampler, which is only correct when
    // chroma is *exactly* half: with four rows of padding, uv.y = 1.0 reads past
    // the image into zeros, and zero chroma over white luma decodes through
    // BT.601 to (0.29, 1.0, 0.08) -- the green line along the bottom edge.
    //
    // Cropping rather than scaling uv keeps the sampler's normalized mapping
    // right by construction, and only ever shrinks. Planes 1 and 2 are Cr and
    // Cb; 0 is luma and 3 the alpha, both full resolution.
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
  // videos were attributed to 0x2175DC60 only because that surface takes exactly
  // 2 draws a frame, matching the 2 Bink composites -- resemblance, not evidence
  // -- and dc.render_target_object is assigned 45 lines above.
  //
  // The comparison it settles: the 1280x430 FE_Smoke resolve names 0x2123C1D8 as
  // its source. If the composite targets a DIFFERENT object at the same EDRAM
  // base, the resolve is copying a surface the video was never drawn into.
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

// FINDING -- THIS FUNCTION'S RESULT IS SAMPLED BY NO DRAW.
//
// It picks the ONE texture a tex*col stand-in samples, which mattered when
// untranslated shaders were common. Measured, it no longer reaches anything:
//
//   stand-in gate: reached 314000, will_stand_in 56138, pixel_shader==0 56138
//
// The two are IDENTICAL, so every stand-in draw is a no-handle draw -- and for
// those this function is never called, ReadBoundPixelShader returning at
// `if (!candidate)` first. Meanwhile `no-hlsl` is 0 in every run, so a consumer
// would have to have a shader AND fail to translate; that set is empty. A
// grading instrument confirmed the picks are often junk (a 1x1 kR16Float, a
// 129x129 terrain clipmap, all scored as colour sources). Do not rebuild it
// without re-checking the gate numbers above.
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

    // A D3D9 Resolve establishes an ordered host render-target dependency. Its
    // guest backing may legitimately be all zero in native mode because the
    // skipped Xenos dispatch never populated that memory, so this identity is
    // stronger evidence than the descriptor's storage format.
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
         // same reasoning: they are not merely unlikely to be base colour, they
         // are render-target storage by construction -- the guest's own format
         // table only ever asks for them with D3DUSAGE_RENDERTARGET.
         source.host_format == mx::hle::HostTextureFormat::kRg16Float ||
         source.host_format == mx::hle::HostTextureFormat::kRg16Unorm ||
         source.host_format == mx::hle::HostTextureFormat::kRg16Snorm ||
         source.host_format == mx::hle::HostTextureFormat::kRgba16Unorm ||
         source.host_format == mx::hle::HostTextureFormat::kRgba16Snorm ||
         source.host_format == mx::hle::HostTextureFormat::kRg32Float))
      continue;
    // An 8x8 immutable texture is a lookup table, not a material. Both that this
    // front end owns are ordered-dither matrices the guest thresholds against
    // for stipple transparency, and the generic host pixel shader has no
    // threshold step -- it samples whatever wins and shows it, which is how a
    // Bayer checkerboard ended up painted across the main menu. Cut measured,
    // not guessed: the smallest immutable winner other than these two is 64x8.
    if (!mapped_render_target && uint64_t(source.width) * source.height <= 64)
      continue;
    // A mapped render target is authoritative storage, but not normally the
    // visible base colour of a material. Multi-input world shaders often combine
    // scene/intermediate targets with immutable colour atlases, and giving
    // mapped targets absolute priority made those sample a black native-mode
    // intermediate instead of their BC1 diffuse. The observed final compositor
    // is handled explicitly above and still selects its mapped s0 scene input.
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
      // scene intermediate outrank a BC1 diffuse.
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
    // Ties were previously broken by fetch program order, which is not evidence
    // of anything, and it lost a 2048x2048 colour atlas to an 8x8 ordered-dither
    // matrix that happened to be fetched first (both scoring 400). Between two
    // candidates the descriptor cannot otherwise separate, the larger one is the
    // material and the smaller one is a lookup table.
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
// mirror that worked, or 0. `base` looks unused and is not: REX_RAW_ADDR expands
// to reference a variable of that name in scope.
// THE TEXTURE NAME, joined by CONTENT. HleTextureKey hashes the six fetch
// dwords, so it is an address and an extent, never the picture: the same image
// at a new address is a new key, and the key says nothing about what it shows.
//
// tools/texture_manifest.py hashes the raw, tiled, big-endian level 0 of every
// .texture asset -- the guest's own representation, so neither side decodes and
// there is nothing to disagree about. 3364 of 3458 distinct contents resolve to
// one name; 94 are genuinely two assets sharing a level 0 and are absent.
//
// Level 0 alone because the runtime's mip chain is normalised and the asset's
// is the full authored one; level 0 is what both always hold.
const std::unordered_map<uint64_t, std::string>& TextureNames() {
  static const std::unordered_map<uint64_t, std::string> names = [] {
    std::unordered_map<uint64_t, std::string> m;
    std::ifstream f("userdata/texture_names.txt");
    if (!f) {
      REXLOG_INFO("d3d9: userdata/texture_names.txt absent -- textures will "
                  "report unnamed. Build it with tools/texture_manifest.py");
      return m;
    }
    std::string line;
    while (std::getline(f, line)) {
      const size_t tab = line.find('\t');
      if (tab != 16) continue;
      char* end = nullptr;
      const uint64_t key =
          std::strtoull(line.substr(0, tab).c_str(), &end, 16);
      if (!end || *end) continue;
      std::string name = line.substr(tab + 1);
      while (!name.empty() && (name.back() == '\r' || name.back() == '\n'))
        name.pop_back();
      m.emplace(key, std::move(name));
    }
    REXLOG_INFO("d3d9: texture names loaded: {} entries", m.size());
    return m;
  }();
  return names;
}

// FNV-1a 64 over the level-0 bytes of the blob CopyTexturePhysical built.
// Level 0 always starts at offset 0 there; its size is its own pitch, which is
// NOT the fetch constant's pitch for any level but this one.
//
// Returns 0 when the level cannot be measured, which is also the "no hash"
// value -- a texture with no describable level 0 has no content to join on.
uint64_t TextureContentKey(const mx::hle::HleTextureSource& source,
                           const std::vector<uint8_t>& guest) {
  if (source.level_count == 0 || source.bytes_per_block == 0) return 0;
  const auto& l0 = source.levels[0];
  const uint64_t bytes = uint64_t(l0.pitch_blocks) * source.bytes_per_block *
                         l0.height_blocks;
  if (bytes == 0 || bytes > guest.size()) return 0;
  uint64_t h = 1469598103934665603ull;
  for (uint64_t i = 0; i < bytes; ++i) {
    h ^= guest[size_t(i)];
    h *= 1099511628211ull;
  }
  return h;
}

// Counted on EVERY decode, so the denominator is every texture the runtime
// built rather than the ones that happened to resolve.
std::atomic<uint64_t> g_texNameSeen{0}, g_texNameHit{0}, g_texNameNoKey{0};

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

// The base level, then the mip chain appended straight after it. The two are
// SEPARATE guest allocations at unrelated addresses, so each is resolved through
// the mirrors independently -- they need not agree on which one is mapped.
// Concatenating them here keeps DecodeHleTexture2D's signature unchanged.
//
// A mip allocation that will not resolve is not fatal: the base is copied
// regardless and the decoder truncates the chain to what it can read.
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
// Deliberately NOT PrepareDrawTexture's logic. That function refuses kR8, kR16,
// kBc5 and the float formats as "not an immutable colour asset", correct for the
// stand-in shader where a single-channel texture bound as base colour would
// paint the surface grey. It is wrong here: the guest's own shader knows that
// channel is a coverage mask or a normal map, and FMT_8 is the format fonts use,
// which is why glyph quads came out as filled blocks.
//
// Fills exactly one of the two per-slot outputs: a resolved render target if the
// guest bound one there, otherwise a decoded CPU payload.
//
// Below: why a sampler slot could not be filled. A slot that fails sends the
// WHOLE draw back to the tex*col stand-in, so these are the draws the guest's
// own pixel shader was translated for and then not used on.
uint64_t g_slotFailRange = 0, g_slotFailFetch = 0, g_slotFailDescribe = 0;
uint64_t g_slotFailCopy = 0, g_slotFailDecode = 0;
uint64_t g_slotBoundZero = 0;   // all-zero, and bound anyway -- see below
uint64_t g_slotBoundUnbound = 0;  // sampler the guest never bound; sampled zero
// Which guest sampler had no readable fetch constant. The open question this
// answers is whether the shaders' samplers 8-15 index the bank the same way 0-7
// do: a failure spread evenly over low samplers means genuinely unbound slots,
// whereas one concentrated at and above 8 means the indexing is wrong.
std::map<uint32_t, uint64_t> g_slotFailFetchBySampler;
// Which guest sampler tripped the range check, and which half of it. `range` was
// the largest slot-fill failure in one run with nothing saying why, and the two
// conditions want opposite fixes: a compact slot at or above 16 is our
// bookkeeping, a guest sampler at or above kMaxSamplers was the file being read
// at half its width.
std::map<uint32_t, uint64_t> g_slotFailRangeBySampler;
uint64_t g_slotFailRangeSlot = 0;

void ReportSlotFailures() {
  const uint64_t total = g_slotFailRange + g_slotFailFetch +
                         g_slotFailDescribe + g_slotFailCopy + g_slotFailDecode;
  // Every 5000 AND on the first one. `(total % 5000) != 0` alone meant a run
  // with fewer than 5000 failures printed NOTHING -- 2389 short draws and not
  // one outcome line, which reads exactly like zero failures.
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
// ReportSlotFailures, which returns early when the hard-failure total is zero,
// so in a run where nothing failed draws painting solid black were invisible.
uint64_t g_boundZeroReported = 0;

// One entry per distinct all-zero texture, so the offenders can be named rather
// than counted -- nine keys covered 10,890 draws in one run. `recovered` is the
// question the log has to answer: a key that later decodes non-blank was sampled
// before the guest finished streaming it, which is a caching defect; one that
// never recovers is a genuinely blank guest texture.
//
// The blank upload gets a MARKED key of its own, because EnsureGameTexture caches
// on payload->key too and the recovered texture must not hit the black resource.
//
// UnboundTexturePayload is the value an unbound Xenos sampler actually returns:
// one black texel, shared by every path that has to bind SOMETHING.
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
  // WHY each of those unbound samplers had no fetch. Only `not-a-texture` is the
  // guest's own answer, where black is what the hardware returns and nothing is
  // missing. Everything marked (OURS), and arguably the two thread-local rows,
  // is a binding that exists and we did not find.
  REXLOG_INFO("d3d9: FETCH MISS BY REASON:{}", FetchMissReport());
}

// Every distinct (guest format, swizzle) pair that reaches a binding, printed
// once. Two open questions read straight off it: whether any swizzle component
// is 6 or 7 -- values D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING does not define --
// and whether the k_8_8_8_8 -> R8G8B8A8 mapping relies on a swizzle that
// performs the BGRA rotation. Also censused: TEX_FORMAT_COMP / GPUSIGN, three of
// whose four values change what a fetch returns (kSigned is two's complement,
// kUnsignedBiased is 2*c-1, kGamma is sRGB linearized on sample).
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

// How many slot binds carry a sign mode we do not honour, split by guest format.
// The census above says which formats do it; this says whether it is one
// decorative texture or the whole scene.
//
// Float formats are excluded deliberately: a TextureSign on k_*_FLOAT is a
// no-op, and the reference cache only needs a separate host texture when a
// FIXED-POINT format has no signed host equivalent (cache.h:488).
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

// Formats whose HOST VIEW is already chosen by signedness in
// DescribeHleTexture2D, each `any_component_signed ? *Snorm : *Unorm`. For these
// kSigned IS applied -- by picking a SNORM view rather than converting anything.
//
// Safe because swizzled-signed implies raw-signed: SwizzleTextureSigns only
// routes existing guest components to host channels, or substitutes literal 0/1
// which it calls unsigned.
bool GuestFormatTakesSignedHostView(uint32_t guest_format) {
  switch (xn::TextureFormat(guest_format)) {
    case xn::TextureFormat::k_16:
    case xn::TextureFormat::k_16_16:
    case xn::TextureFormat::k_16_16_16_16:
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

// Did we read the mip chain from the RIGHT PLACE? A wrong offset, pitch or
// packed-tail displacement does not fail: it returns plausible bytes from
// elsewhere in the allocation, visible only on minified surfaces at a distance,
// and neither the blank-texture counters nor the decode's own bounds check can
// see it.
//
// So measure it. The guest's mips are a reduction of their parent, so
// box-filtering level n-1 down by two should land close to level n: small mean
// absolute difference (under ~12 of 255) means the addressing is right, while
// two uncorrelated images average about 85.
//
// Block-compressed formats have to be included or the check is close to
// worthless, since this game's art is overwhelmingly BC1/BC3/BC5. A block is not
// decoded, only averaged -- every BC variant stores two endpoints at a known
// offset, so the midpoint approximates the block's mean well enough here.
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
    // An absolute threshold cannot do this job -- the guest does not box-filter
    // its mips, block endpoints only approximate a block's mean, and small levels
    // are a small sample, so a correct level can score 30 while another correct
    // one scores 3. The CONTROL settles it with no magic number: whatever the
    // content does to the aligned score it does to the misaligned one too, so
    // aligned much lower than control means this level really is its parent
    // reduced, and the two being equal is the signature of someone else's bytes.
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
    // against a genuinely uniform level, where both are ~0.
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

// The chain census, called per BIND, deliberately. The first version hung off
// the decode path, which only runs on a cache miss -- so in a menu-only run it
// printed once, three seconds in, and never again.
void NoteMipCensus() {
  static std::atomic<uint64_t> s_binds{0};
  const uint64_t n = s_binds.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n != 1 && (n % 20000) != 0) return;
  // The absolute check on the tiled addressing, printed once. It answers a
  // question the mip self-check structurally cannot -- see HleTiledAddressCheck
  // -- and it prints even when it passes, because "no line" is how a check that
  // never ran looks.
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
  // Printed with every census tick, including when every field is zero. "This
  // title binds no 1D textures" is a finding, and it is the finding that decides
  // whether the wide-1D remap is worth writing -- but only if the line appears
  // at all.
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
      " chain copy failed {}; guest asks for mip_filter=point {} lod_bias {}"
      " (both APPLIED -- these were labelled `deferred` until 2026-08-30, long"
      " after they landed: mip_filter drives the kSamplerMipPoint/kSamplerBaseMap"
      " variants in d3d12_game.cpp and the bias rides in xe_texinv[slot].w);"
      " raw mip_max_level:{}",
      c.described, c.with_chain, c.levels_planned,
      c.with_chain ? double(c.levels_planned) / double(c.with_chain) : 0.0,
      c.raw_mip_address_set, c.no_address, c.suppressed_base_map,
      c.suppressed_min_level, c.layout_empty, c.truncated, g_mipCopyFailed,
      c.mip_filter_point, c.lod_bias_set, levels);
}

// Blast radius of the packed mip tail. A texture whose base is packed used to be
// read from the origin of the tail rather than from its own offset within it, so
// every one of these was returning another texture's bytes. Counted by extent
// and format so the population is visible rather than inferred from the one 8x8
// DXT1 lookup that made it findable -- that one multiplies the menu's deferred
// lighting, which is why the whole scene came out black.
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
// texture's bits reinterpreted into a signed host format; kGamma needs an sRGB
// curve. Neither is approximated here -- an unimplemented mode that silently
// behaves as unsigned is at least visible in this line.
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
// retry, every counter -- is identical for the two stages, because "what texture
// is bound to guest sampler N" does not depend on who is asking.
// dc.pixel_shader_handle is still read for the blank-texture key regardless of
// stage: that key identifies the DRAW's material.
//
// `stage_handle` names the shader whose slot this is, for diagnostics only. The
// vertex caller must pass it: `dc.vertex_shader_handle` is assigned in a
// different function that has not necessarily run yet, so reading it here
// printed `SLOT MAP vs 0x00000000` and hashed every vertex slot to one key.
bool ResolvePixelSlotTexture(mx::hle::DrawCall& dc, uint32_t slot,
                             uint32_t guest_sampler, uint32_t device,
                             uint8_t* base, bool vertex,
                             uint32_t stage_handle_hint,
                             const uint32_t* fetch_override) {
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
  // host target, exactly as the single-texture path does.
  //
  // A destination the GPU wrote WHOLE is a render target -- its guest memory is
  // meaningless and the snapshot is the only truthful answer -- so take it before
  // spending a decode. A destination the GPU wrote only PART of is not a choice
  // between a good answer and a bad one; both sources are partial. The 2048x2048
  // menu atlas has three 256x256 tiles of real content while its guest memory
  // decodes to zeros, so refusing the snapshot outright threw those tiles away.
  // Try memory first, fall back to the snapshot when memory has nothing.
  //
  // "Has nothing" meant all-zero, and that read the terrain heightmap as real
  // data for months: nothing CPU-writes it, so its memory decodes to a uniform
  // 0xFF. A UNIFORM decode counts as nothing too, but only here, where a
  // partly-written snapshot is standing by.
  uint32_t partial_snapshot_object = 0;
  const auto& texture_state = DeviceState().texture[guest_sampler];
  // Unconditional, and BEFORE the resolve-destination branch. The material's
  // texture need never have been resolved into, so gating this the way slot_seen
  // is gated would make it a counter that cannot fire for exactly the case it
  // exists to measure.
  NoteVideoShapeSlot(texture_state.fetch, texture_state.valid);
  ResolvedTargetByAddress* resolve_entry = nullptr;
  if (texture_state.object &&
      g_resolvedTextureTargets.contains(texture_state.object)) {
    // Counted BEFORE the coverage gate, so "reached the draw path" and "was
    // allowed to be a snapshot" stay separable. The SLOT MAP line below dedupes
    // on (shader, slot), so a slot that logged once with a different texture
    // never logs again however many other textures pass through it.
    resolve_entry = ResolveEntryForObject(texture_state.object);
    if (resolve_entry) ++resolve_entry->slot_seen;
    if (ResolvedDestinationIsMostlyWritten(texture_state.object)) {
      if (resolve_entry) ++resolve_entry->slot_snapshot;
      // Logged HERE as well as at the decode below, because this path RETURNS.
      // The first cut of SLOT MAP sat only after this point and so reported
      // resolved=0 on every line -- blind to precisely the slots that bind a
      // snapshot.
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
        // investigation ever bound: 632 of 1024 were spent in the menu.
        fresh = s_seen.size() < 4096 && s_seen.insert(key).second;
      }
      // Read regardless of whether this line is fresh: the swizzle is no longer
      // only a diagnostic, it is what the renderer binds the snapshot with.
      // Gating it on the log's dedupe would leave every slot after the first
      // with an identity mapping again.
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
        // CLAMP alongside the swizzle. Both come from the same fetch dword, and
        // leaving it out made a run that had just changed the clamp behaviour
        // unable to say what clamp it chose. Prints the raw 3-bit guest modes
        // AND the wrap/clamp they resolve to, because SamplerVariantFor's rule
        // (>= 2 clamps; kRepeat 0 / kMirroredRepeat 1 wrap) is the thing worth
        // checking, not the enum value.
        const uint32_t cx = have_swz ? ((sfetch[0] >> 10) & 7u) : 0u;
        const uint32_t cy = have_swz ? ((sfetch[0] >> 13) & 7u) : 0u;
        const uint32_t mag = have_swz ? ((sfetch[3] >> 19) & 3u) : 0u;
        const uint32_t min = have_swz ? ((sfetch[3] >> 21) & 3u) : 0u;
        REXLOG_INFO(
            "d3d9: SLOT MAP {} 0x{:08X} slot {} (guest sampler {}): object "
            "0x{:08X} -> SNAPSHOT of a resolve destination (no guest-memory "
            "decode); guest swizzle {} clamp {}/{} -> {}/{} filter mag {} "
            "min {} -> {}",
            vertex ? "vs" : "ps", stage_handle, slot, guest_sampler,
            texture_state.object,
            have_swz ? fmt::format("{:#o}", swz) : std::string("unreadable"),
            have_swz ? fmt::format("{}", cx) : std::string("?"),
            have_swz ? fmt::format("{}", cy) : std::string("?"),
            have_swz ? (cx >= 2 ? "CLAMP" : "wrap") : "CLAMP(fallback)",
            have_swz ? (cy >= 2 ? "CLAMP" : "wrap") : "CLAMP(fallback)",
            have_swz ? fmt::format("{}", mag) : std::string("?"),
            have_swz ? fmt::format("{}", min) : std::string("?"),
            have_swz ? ((mag == 0u || min == 0u) ? "POINT" : "LINEAR")
                     : "POINT(fallback)");
      }
      out_objects[slot] = texture_state.object;
      // The renderer has no fetch constant of its own for a snapshot slot; this
      // is the only place the guest swizzle is in hand. AND THE CLAMP MODE,
      // packed into bits 12-13, and THE FILTER into bit 14 -- nothing reads the
      // top nibble of this uint16_t, so that beats threading a parallel array
      // through DrawCall, graphics_system and AddGameDraw.
      //
      // Why both are needed: BindTranslatedSamplers gave every snapshot slot a
      // HARDCODED clamped POINT, on the reasoning that a snapshot "is sampled
      // 1:1" and has "no fetch constant to read a mode off". True for a
      // full-screen post-process copy; false for the terrain ATLAS, sampled with
      // computed UVs, where clamp pins tile index 10 at U = 1.283 into an empty
      // tile (the black ground) and POINT gives every mid-distance dune hard
      // corduroy aliasing.
      //
      // dword 0 (xenia-edge gpu/xenos.h): type:2 sign_xyzw:8, clamp_x:3 at bit
      // 10, clamp_y:3 at 13. dword 3: num_format:1, swizzle:12, exp_adjust:6,
      // mag_filter:2 at 19, min_filter:2 at 21; kPoint is 0. An unreadable fetch
      // keeps the old hardcode rather than guessing.
      out_swizzles[slot] =
          have_swz ? PackSnapshotSamplerWord(
                         swz, (sfetch[0] >> 10) & 7u, (sfetch[0] >> 13) & 7u,
                         ((sfetch[3] >> 19) & 3u) != 0u &&
                             ((sfetch[3] >> 21) & 3u) != 0u)
                   : PackSnapshotSamplerWord(0, 2, 2, false);
      return true;
    }
    if (resolve_entry) ++resolve_entry->slot_partial;
    partial_snapshot_object = texture_state.object;
  }

  uint32_t fetch[6] = {};
  // dc.pixel_shader_handle is the handle AttachTranslatedPixelShader resolved for
  // this DEVICE, not the thread-local one, which is the handle whose load table
  // may carry this sampler's descriptor.
  //
  // A REPLAYED DRAW BRINGS ITS OWN BINDING. The recorded command buffer writes
  // texture fetch constants (Xenos 0x4800 + sampler*6) and the console sets
  // textures from those, so for a replay that is authoritative and the device
  // shadow is not. Reading the live device put a rock texture on the rider;
  // reading the RECORDING device left the palm leaf with a bush atlas.
  if (fetch_override) {
    std::memcpy(fetch, fetch_override, sizeof(uint32_t) * 6);
  } else if (!ReadLiveTextureFetch(device, base, guest_sampler, fetch,
                                   dc.pixel_shader_handle)) {
    ++g_slotFailFetch;
    ++g_slotFailFetchBySampler[guest_sampler];
    ReportSlotFailures();
    // The guest never bound anything to this sampler, and the shader reads it
    // anyway. The failures fall s5=2420 s6=304 s7=2420 s8=2429 s9=2420 s13=4
    // with samplers 0-4 never failing once -- one shader family reading four
    // slots this title does not bind, NOT the thread-local device state losing a
    // binding, which would fail every sampler on the affected thread.
    //
    // Zero is what the hardware returns for a fetch constant whose type is not
    // kTexture. Refusing instead sent the whole draw to the tex*col stand-in,
    // discarding every OTHER slot's real shading.
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
    // sampler reads zero, and the guest's own shader still runs. Same trade as
    // the unbound-sampler path.
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
  // VEGETATION BINDS, by texture SHAPE -- the one identification that has
  // actually held on this problem, after six attempts to fingerprint a
  // vegetation DRAW all over-matched. The texture is established, not guessed: a
  // bush billboard sampled a 2048x1024 BC3, and in FR_Dunes the ONLY 2048x1024
  // DXT4_5 assets are the impostor atlas pair; the fan palm's bark is likewise
  // uniquely 256x1024.
  //
  // So: does anything BIND the palm's bark while the tree is on screen? Driving
  // up to a palm, the impostor atlas took 63998 binds and climbed, the palm bark
  // took 2 and never incremented -- and 2 is the impostor BAKE, once each for
  // diffuse and normal. The bark is a trunk-only material in no impostor, so the
  // palm's 3D material NEVER reaches a scene draw: vegetation in the scene is
  // billboards, exclusively. This is the runtime half of the IDA result that
  // only sub_823F82D0 (the bake) calls the 3D renderer, and unlike the
  // constant-mask censuses it cannot be blind to an entry point.
  //
  // Formats: 18 = FMT_DXT1, 20 = FMT_4_5 (DXT4_5/BC3), 49 = FMT_DXN. Counts
  // BINDS (per draw, per slot), not draws.
  {
    static std::mutex s_vegMu;
    static uint64_t s_vegAtlas = 0, s_vegBark = 0, s_vegBinds = 0;
    static uint64_t s_prevAtlas = 0, s_prevBark = 0;
    const bool atlas = source.width == 2048u && source.height == 1024u &&
                       source.guest_format == 20u;
    const bool bark = source.width == 256u && source.height == 1024u &&
                      (source.guest_format == 18u || source.guest_format == 49u);
    if (atlas || bark) {
      std::lock_guard<std::mutex> veg_lk(s_vegMu);
      if (atlas) ++s_vegAtlas;
      if (bark) ++s_vegBark;
      if ((++s_vegBinds % 2000) == 0) {
        REXLOG_INFO(
            "d3d9: VEGETATION BINDS: impostor atlas (2048x1024 BC3) {} "
            "(+{} since last), palm bark (256x1024 DXT1/DXN) {} (+{}). Bark "
            "binding while a tree is on screen means the 3D material reaches "
            "a draw; bark at 0 with atlas climbing means billboards only.",
            s_vegAtlas, s_vegAtlas - s_prevAtlas, s_vegBark,
            s_vegBark - s_prevBark);
        s_prevAtlas = s_vegAtlas;
        s_prevBark = s_vegBark;
      }
    }
  }
  // WHICH guest surface does each sampler slot actually ask for?
  //
  // Traced from the rider's gear rendering green. Its material computes
  // saturate(tex5.y + rcp(luminance(tex4))) and the saturate pins at 1, zeroing
  // red unless that luminance exceeds ~1.03. tex4 resolves to the pre-pass band
  // snapshot, whose six directional lights' red channels total 0.619 -- a ceiling
  // requiring every dot product at 1.0 at once, which opposing directions make
  // impossible; measured 0.109. So the arithmetic does not merely say the input
  // is dark, it says it is the WRONG SURFACE: the gained main-pass scene holds
  // 32.6 at the same pixel, a red multiplier of 0.938.
  //
  // Binding is by guest OBJECT, so we follow whatever the guest bound, and this
  // says what that is. The ADDRESS fallback is resolved BEFORE the log, because
  // `resolved=` reports only the OBJECT lookup and would print 0 whether or not
  // the address match rescued it -- the composite's case, where one shader binds
  // object 0x7010F7F0 while three others bind 0x2123C2A4 for the same address.
  const ResolvedTargetByAddress* addr_match = ResolvedTargetForAddress(source);

  // Deduplicated per (shader, slot) and capped: one line per distinct binding,
  // not per draw.
  {
    static std::mutex s_mu;
    static std::set<uint64_t> s_seen;
    static uint32_t s_lines = 0;
    // Keyed on the handle of the stage this slot belongs to, and tagged with the
    // stage. Keying both stages on `dc.pixel_shader_handle` hid the one binding
    // under investigation: the terrain depth prepass runs the depth-only pixel
    // stand-in and carries no pixel handle, so its VERTEX slot hashed to
    // (0 << 8) | 0 and was deduped away against the first pixel slot 0 seen.
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
          " {:#x} clamp {}/{} -> {}/{} | levels {} mip_filter {} lod_bias"
          " {:+.4f} (raw {}) | fetch dword4 0x{:08X} dword5 0x{:08X}",
          vertex ? "vs" : "ps", stage_handle, slot, guest_sampler,
          texture_state.object,
          texture_state.object &&
                  g_resolvedTextureTargets.contains(texture_state.object)
              ? 1
              : 0,
          partial_snapshot_object ? 0 : 1, addr_match ? 1 : 0,
          addr_match ? addr_match->dest_object : 0u, source.address,
          source.width, source.height, source.guest_format,
          source.source_bytes, source.swizzle, source.signs, source.clamp_x,
          source.clamp_y, source.clamp_x >= 2 ? "CLAMP" : "wrap",
          source.clamp_y >= 2 ? "CLAMP" : "wrap", source.level_count,
          source.mip_filter, double(source.lod_bias_raw) / 32.0,
          source.lod_bias_raw, fetch[4], fetch[5]);
    }
  }
  // Permuted into host component order here, at the bind, because this is
  // per-binding state: the same guest memory is sampled with different sign
  // modes by different draws. Applied by the shader after the fetch -- see the
  // note in EmitTextureFetch.
  //
  // Only kUnsignedBiased rides this. kSigned would need the bits reinterpreted
  // into a signed host format and kGamma is a curve rather than a scale; both
  // are counted by NoteUnhandledSign and left alone. The gate EXCLUDES formats
  // whose host view is chosen by signedness -- picking the SNORM view happens in
  // DescribeHleTexture2D, and this gate not knowing that made it report a defect
  // that did not exist. What remains in the census is REAL: kGamma on DXT4_5 and
  // kSigned on k_4_4_4_4, for which no signed BGRA4 host view exists.
  {
    // Stored as the RAW 2-bit-per-component modes, which is what
    // TextureSignScale decodes and what FillVertexTextureSigns always expected.
    // This used to be a 1-bit-per-component "is biased" mask, which the vertex
    // stage then misread as modes -- see the note on TextureSignScale.
    static_assert(uint32_t(xn::TextureSign::kUnsignedBiased) == 2u &&
                      uint32_t(xn::TextureSign::kGamma) == 3u,
                  "TextureSignScale spells these numerically");
    const uint8_t swizzled =
        mx::hle::SwizzleTextureSigns(source.signs, source.swizzle);
    for (uint32_t c = 0; c < 4; ++c) {
      const uint32_t mode = (swizzled >> (c * 2)) & 3u;
      // Both of these are applied in the shader out of xe_texsign, so neither
      // is unhandled: kUnsignedBiased as a scale, kGamma as the piecewise
      // linear curve the hardware uses.
      if (mode == uint32_t(xn::TextureSign::kUnsignedBiased) ||
          mode == uint32_t(xn::TextureSign::kGamma))
        continue;
      if (mode != uint32_t(xn::TextureSign::kUnsigned) &&
          !IsFloatGuestFormat(source.guest_format) &&
          !GuestFormatTakesSignedHostView(source.guest_format))
        NoteUnhandledSign(source.guest_format, mode);
    }
    out_signs[slot] = swizzled;
  }

  // The same memory a resolve wrote into, reached through a different texture
  // object than the one the resolve named, so the object test above missed it.
  // Sample the snapshot rather than decoding guest memory the GPU wrote and the
  // emulator never populated, which reads as zeros and paints black.
  //
  // After the describe, because the address and extent it matches on come out of
  // it; before the decode, because the decode is what has to be skipped. Decided
  // from the same lookup the log above reported, so the line cannot say
  // something the binding then contradicts.
  if (addr_match) {
    out_objects[slot] = addr_match->dest_object;
    // The sampler word, which this path did not write. It was the ONLY one of the
    // four object-binding sites that set an object and returned without a word,
    // so every bind through here reached BindTranslatedSamplers with bit 15
    // clear, which that function reads as "no filter to honour" and answers with
    // the snapshot path's clamped POINT -- 43,343 of 1,302,535 snapshot slot
    // binds, at a dead-steady 300 per report.
    //
    // PartialSnapshotSamplerWord, not PackSnapshotSamplerWord: this is a resolve
    // snapshot reached by address, and honouring the stated clamp on an atlas
    // sampled past U=1 is what walks the ground back to black.
    out_swizzles[slot] = PartialSnapshotSamplerWord(source);
    ++g_resolveAddr.matches;
    return true;
  }

  // NOTE the empty-texture set is deliberately NOT consulted here.
  //
  // It is consulted by the single-texture path, which CHOOSES one sampler to
  // represent the draw -- there, skipping a blank candidate is right. This path
  // does not choose: the translated shader NAMES this sampler, and the guest
  // bound a texture to it that decodes, from readable memory, to zeros. Then
  // zero is what the guest's own shader samples and black is correct. Refusing
  // it reverted the whole draw to the stand-in -- 10,890 draws from just nine
  // distinct all-zero textures, because the refusal is cached per key.
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
    // A previously FLAT decode is re-read on a backoff, because the fingerprint
    // above cannot see the sparse write that fills it. Cheap: the map holds only
    // keys that decoded flat, and this is one decode per key per
    // kFlatRetryFrames rather than one per bind.
    if (!stale) {
      if (auto fr = g_flatRetryKeys.find(key); fr != g_flatRetryKeys.end()) {
        const uint32_t now = mx::hle::D3D9FrameCount();
        const uint32_t interval = fr->second.volatile_content
                                      ? kVolatileRetryFrames
                                      : kFlatRetryFrames;
        if (now - fr->second.last_frame >= interval) {
          fr->second.last_frame = now;
          ++g_flat.retriesDue;
          stale = true;
        }
      }
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
    // Gated on the BLANK SET, not on g_hleBlankPayloads. Requiring a blank
    // payload made this branch unreachable for precisely the textures it serves:
    // the blank path below returns as soon as it has a snapshot, so a key with a
    // snapshot never gets a payload recorded, and every bind fell through and
    // re-decoded -- 2 x 16 MB every frame, 21 GB over a 100-frame run.
    if (partial_snapshot_object && g_hleEmptyTextures.count(key)) {
      out_objects[slot] = partial_snapshot_object;
      out_swizzles[slot] = PartialSnapshotSamplerWord(source);
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
  // NAME IT, from the guest bytes, before anything decodes them. Counted on
  // every copy so the denominator is every texture the runtime built -- not
  // the ones that happened to resolve, which would make the rate meaningless.
  {
    ++g_texNameSeen;
    const uint64_t ckey = TextureContentKey(source, guest);
    if (!ckey) {
      ++g_texNameNoKey;
    } else {
      const auto& tn = TextureNames();
      const auto it = tn.find(ckey);
      if (it != tn.end()) {
        ++g_texNameHit;
        // The first few by name, because a percentage cannot be checked by eye
        // and "ATV_Alpha_Combo at 256x256 DXT4_5" can.
        static std::atomic<uint32_t> s_shown{0};
        if (s_shown++ < 12)
          REXLOG_INFO("d3d9: texture named: {} ({}x{} {}) key {:016X}",
                      it->second, source.width, source.height,
                      mx::hle::GuestTextureFormatName(source.guest_format),
                      ckey);
      }
    }
    const uint64_t n = g_texNameSeen.load();
    if ((n % 2000) == 0)
      REXLOG_INFO("d3d9: TEXTURE NAMES -- {} decodes, {} named ({:.1f}%), {} "
                  "had no measurable level 0. Unnamed is a render target, a "
                  "glyph atlas, or an asset whose level 0 is shared",
                  n, g_texNameHit.load(),
                  g_texNameHit.load() * 100.0 / double(n),
                  g_texNameNoKey.load());
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
    // How many DISTINCT cache keys one guest address has produced. Greater than
    // one means the same bytes are being decoded under several keys -- the
    // fetch-constant hash splitting on sampler state -- which is a different fix
    // from a texture whose content genuinely changes.
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
  // A decode that carries no information. Two forms, and only the first used to
  // be recognised:
  //
  //   all zero  -- storage the guest has not filled yet.
  //   UNIFORM   -- every byte the same non-zero value. Guest memory the CPU never
  //                writes reads back whatever is there, and for the terrain
  //                heightmap that is 0xFF, which passes the nonzero test as real
  //                data: the vertex stage read a constant 1.0 and the whole
  //                ground sat 615 units below the view.
  //
  // The uniform form is only treated as empty when `partial_snapshot_object` is
  // set -- i.e. a better source demonstrably exists. A flat texture with no
  // snapshot behind it is legal and left alone.
  const bool decode_is_blank = [&] {
    PhaseTimer t(g_tex.scanUs);
    return !HleTextureHasNonzeroData(*payload);
  }();
  uint8_t uniform_value = 0;
  // Detected UNCONDITIONALLY, acted on only below. The first cut computed
  // `decode_is_uniform` as `partial_snapshot_object && ...` and put its log line
  // inside the `if (partial_snapshot_object)` branch -- so when the question was
  // "is partial_snapshot_object even set for this slot?", the diagnostic that
  // would answer it could not print. Measure first, gate second.
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
  // FLAT-DECODE PROBE. See NoteDecodedTexture -- it is called from ALL THREE
  // decode sites, which is the whole reason it is a function.
  NoteDecodedTexture(source, *payload, guest.data(), guest.size(),
                     guest_sampler, "slot");
  // REVERTED: `decode_is_uniform` used to be admitted here alongside the blank
  // case. The texture that motivated it was MISIDENTIFIED -- picked out of a
  // RenderDoc resource list by size and format, while the real sampler read a
  // different resource -- and every uniform decode measured since has reported
  // no snapshot to fall back on.
  //
  // The DETECTION stays, now saying outright whether a fallback exists. STRICT:
  // do not record the blank, so the decode is retried instead of a uniform
  // payload being cached as the texture.
  const bool blank_strict =
      mx::gpu::guard::Strict(mx::gpu::guard::Guard::kBlankTexturePayload);
  mx::gpu::guard::Note(mx::gpu::guard::Guard::kBlankTexturePayload,
                       decode_is_blank && !blank_strict);
  if (decode_is_blank && !blank_strict) {
    NoteBlankDecode(key);
    // Memory had nothing and a partly-written snapshot exists: its resolved
    // tiles beat a constant everywhere, and the blank is still recorded above so
    // the retry keeps looking.
    if (partial_snapshot_object) {
      out_objects[slot] = partial_snapshot_object;
      out_swizzles[slot] = PartialSnapshotSamplerWord(source);
      return true;
    }
    ++g_slotBoundZero;
    NoteBlankTexture(key, guest_sampler, source);
    ReportBoundZero();
    // Bind the zero for THIS draw -- that trade is settled -- but do not cache
    // it. The cache key hashes where the texture lives and what shape it is,
    // never what it contains, so inserting a blank decode under that key froze
    // the texture black for the rest of the run: nothing in the tree erases or
    // versions an entry. EnsureGameTexture keys on payload->key too, so the
    // blank upload gets a marked key of its own.
    payload->key = key ^ kBlankTextureKeyMarker;
    g_hleBlankPayloads[key] = payload;
    out_textures[slot] = std::move(payload);
    return true;
  }
  NoteBlankRecovered(key);
  payload->key = key;
  payload->content_version =
      TextureContentVersion(source, base, payload->format);
  SetPayloadUploadVersion(source, *payload);
  mx::diag::DumpDecodedTexture(source, *payload, "slot", guest_sampler);
  out_textures[slot] = payload;
  // A FLAT DECODE IS CACHED, AND MARKED FOR PERIODIC RE-READ.
  //
  // GuestTextureFingerprint SAMPLES, so a SPARSE write lands between its sample
  // points however the sampling is tuned. The terrain's virtual-texture INDEX MAP
  // decoded once while still uniform and was never re-read.
  //
  // NOT CACHING IT AT ALL WAS TRIED AND REVERTED -- it looks free and is not:
  //
  //   decode 359ms, scan 651ms | 50 decodes over 36880 KB
  //   FRAMETIME 1746617us     (1.75 SECONDS a frame)
  //
  // 36 MB re-decoded and re-scanned per interval, because a 4 MB cutoff still
  // admits the 2 MB index map on EVERY bind, and it does not self-limit until
  // the guest fills the texture. So: cache as normal and record the key, and let
  // the cache-hit path re-read it every kFlatRetryFrames.
  constexpr size_t kFlatRetryLimit = 4u * 1024u * 1024u;
  const size_t flat_base =
      payload->level_count > 1
          ? std::min<size_t>(payload->levels[1].offset, payload->data.size())
          : payload->data.size();
  // DO NOT RE-SCAN A KEY ALREADY KNOWN TO CHANGE. The scan exists to ask "is this
  // texture still empty?", and for a volatile key that question is already
  // answered. It is also the larger half of a re-read's cost, so skipping it is
  // what makes kVolatileRetryFrames affordable.
  const auto watch_it = g_flatRetryKeys.find(key);
  const bool known_volatile = watch_it != g_flatRetryKeys.end() &&
                              watch_it->second.volatile_content;
  const bool flat_now =
      !known_volatile && flat_base && flat_base <= kFlatRetryLimit &&
      ScanFlatness(payload->data.data(), flat_base, source.bytes_per_block)
              .share() >= 0.999;
  if (flat_now) {
    ++g_flat.notCached;
    g_flatRetryKeys[key] =
        FlatWatch{uint32_t(mx::hle::D3D9FrameCount()), false};
    static std::set<uint64_t> s_flatSeen;
    if (s_flatSeen.insert(key).second && s_flatSeen.size() <= 12) {
      // IS THIS ACTUALLY A GPU SURFACE WE FAILED TO CLAIM? On Xenos a render
      // target lives in EDRAM and only a RESOLVE moves GPU output into guest
      // memory, so a zero here proves the GPU never wrote these bytes and the
      // texture is genuinely uniform. `resolved=0` on the SLOT MAP cannot say
      // this: it is also what an extent mismatch and a coverage refusal print.
      const mx::hooks::d3d9::ResolveRangeProbe rp =
          mx::hooks::d3d9::ProbeResolveRange(source.address,
                                             uint32_t(flat_base));
      REXLOG_INFO(
          "d3d9: FLAT RETRY-MARKED addr 0x{:08X} {}x{} fmt {} ({} KB) -- "
          "re-read every {} frames until it carries data | RESOLVE REACH: "
          "exact {} inside {} of {} destinations{}",
          source.address, source.width, source.height, source.guest_format,
          uint32_t(flat_base / 1024), kFlatRetryFrames, rp.exact, rp.inside,
          rp.total,
          rp.any()
              ? fmt::format(" -- dest 0x{:08X} {}x{}", rp.first_addr,
                            rp.first_width, rp.first_height)
              : (rp.below_addr
                     ? fmt::format(" -- NONE; nearest below 0x{:08X} {}x{} "
                                   "(delta 0x{:X})",
                                   rp.below_addr, rp.below_width,
                                   rp.below_height, rp.below_delta)
                     : std::string(" -- NONE, and none below either")));
    }
  } else if (watch_it != g_flatRetryKeys.end()) {
    // KEEP WATCHING IT. Erasing here was a real bug and it cost the terrain: the
    // backoff was written for a texture that STARTS empty and later fills, so a
    // decode that came back non-flat was read as "done" and the key dropped. But
    // a texture that was uniform and is now not has just proved the opposite --
    // it CHANGES -- and that is precisely the texture GuestTextureFingerprint
    // cannot track. The terrain's PAGE TABLE is exactly this shape: THREE decodes
    // of it in sixteen thousand.
    watch_it->second.last_frame = mx::hle::D3D9FrameCount();
    // Latches on: once a texture has been seen to carry content after being
    // uniform, it is volatile for good and gets the tight interval.
    watch_it->second.volatile_content = true;
    ++g_flat.volatileKeys;
  }
  g_hleCpuTextures.emplace(key, std::move(payload));
  return true;
}

// Overlay a pixel shader's EMBEDDED constants onto the bank read from the device
// shadow.
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
//   `table + rel` whose entry list starts at +0x14 and runs *(u32*)(+0x10) bytes:
//       entry: u16 reg_index, u16 dword_count, u32 data_offset   (8 bytes)
//       terminated by dword_count == 0
//       emitted as PM4 0xC0022F00 (LOAD_ALU_CONSTANT)
//
// The vertex half was already understood; what was missed is that the PIXEL
// shader has the same mechanism at DIFFERENT offsets, so the pixel bank was left
// with only the registers SetPixelShaderConstantF happens to write. Over 264
// dumped shaders, 242 read constants above c217, which the shadow never
// contains, and the 22 that do not are exactly the UI shaders that always looked
// correct.
//
// `reg` is an ALU float4 constant index; the emitted packet uses `4 * reg` from
// register 0x4000, so pixel constants 256..511 map to this bank at reg-256.
// Texture fetch constants do NOT live in this list -- they are in the second,
// inline state-patch list walked by ApplyShaderFetchPatchTable.
//
// `written`, when given, marks the bank dwords this table published, which
// FillMaterialGate needs to distinguish a deliberate zero from an unwritten slot.
void ApplyShaderLoadTable(uint32_t shader, uint32_t table_at, uint32_t data_at,
                          uint8_t* base, std::vector<uint32_t>& bank,
                          std::vector<uint8_t>* written = nullptr) {
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
      // TRACE: the mask pass's xe_c[140..143] = pixel ALU c396..c399.
      //
      // RESULT: PRINTS NOTHING -- neither the load table nor PM4 writes those
      // registers, so they come from the linear copy of the guest's own shadow
      // and the values are the GUEST's. That kills the hypothesis this was built
      // for: c140..c143 equal c100..c103 except in .z, with c14x.z == -c12x.y
      // exactly, which I read as a corrupted column. For an orthonormal rotation
      // the inverse IS the transpose, so that is evidence AGAINST corruption.
      if (dst >= 560u && dst < 576u) {
        static uint32_t s_traced = 0;
        if (s_traced++ < 64) {
          float f;
          std::memcpy(&f, &bank[dst], sizeof(f));
          REXLOG_INFO(
              "d3d9: LOADTABLE pixel c{}.{} = {} (0x{:08X}) [entry reg {} "
              "dwords {}, j {}, src 0x{:08X}, shader 0x{:08X}]",
              (abs_reg - 256u), "xyzw"[j % 4], f, bank[dst], reg, dwords, j,
              src + j * 4, shader);
        }
      }
      if (written && dst < written->size()) (*written)[dst] = 1;
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
  // now 32 and the truncation is gone.
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

// Both shaders publish into the SAME unified ALU constant file -- the draw flush
// calls sub_825656A0 once for each -- so an entry in EITHER table whose register
// lands in 256..511 is a pixel constant. Taking only the pixel shader's table
// left c43 and c85 at zero, which is most of a material's shading still missing.
//
// The fetch patches are gathered from BOTH shader objects because both binding
// calls write the same device constants block. They are keyed by the PIXEL
// shader handle because that is the identity the draw's texture resolver owns.
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
  // Which dwords the shader publishes itself. Collected so the material-gate
  // fill below can leave them alone -- including the ones it sets to zero
  // deliberately, which is the case that makes a "fill every zero" rule wrong.
  std::vector<uint8_t> ps_written(bank.size(), 0);
  ApplyShaderLoadTable(shader, 0x28, 0x18, base, bank, &ps_written);
  // AFTER the load table, not before. The first cut ran inside OverlayNonFinite
  // above and was overwritten for every shader whose table covers c84..c87 --
  // which is the terrain -- while sticking on materials whose table does not.
  // 368,313 substitutions, none of them where it was aimed.
  if (bank.size() >= 256 * 4)
    mx::gpu::alu::FillMaterialGate(bank.data(), 256, ps_written.data());
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
// Extracted so it can run BEFORE the single-texture binding contest as well as
// after it. The contest answers a different question -- which one texture the
// tex*col stand-in should sample -- and used to gate this, which meant a shader
// fetching no texture at all never got here.
void AttachTranslatedPixelShader(mx::hle::DrawCall& dc, uint32_t handle,
                                 uint32_t device, uint8_t* base) {
  using namespace mx::hle;
  dc.pixel_shader_handle = handle;
  // Census of resolve destinations bound at THIS draw, before any of the slot
  // logic below can filter them out. See the fields' note in the header: this is
  // deliberately outside the sampler loop, because the whole point is to see
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
  // per draw: 23 untranslated shaders out of 256 is a small share of the former
  // and may be a large share of the latter.
  if (!TranslatedPixelShader(handle)) {
    static std::map<uint32_t, uint64_t> s_byHandle;
    static uint64_t s_total = 0;
    ++s_total;
    ++s_byHandle[handle];
    // Every 500, not 5000: at 5000 this printed NOTHING across a 1943-frame run
    // while 25,359 draws took the stand-in -- silence that reads identically to
    // zero, and which sent the search to the wrong place.
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
      // that samples -- terrain displacement -- used to be refused the GPU path
      // for having a sampler at all, and the interpreter it fell back to has no
      // texture fetch, so its samples were silent zeros and the positions
      // silently wrong. All-or-nothing like the pixel stage; falling short
      // clears the count rather than the shader, putting the draw back on the
      // CPU path.
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
        // count collapses, the numbers saying whether the address match is doing
        // the work disappear with it.
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
  // stand-in should sample, and conflating them cost 63,207 draws in one run --
  // 27.8% of every draw attempted. Both resolutions below end in
  // ResolvePixelBindingForDraw, which gives up when `profile->bindings.empty()`,
  // and a pixel shader that fetches no texture has exactly that -- so it was
  // reported as "no eligible pixel shader" and the draw kept the stand-in, which
  // is backwards for a TRANSLATED shader that needs no texture while the stand-in
  // it fell back to does.
  //
  // So resolve the handle first and attach the translation on its own terms.
  uint32_t resolved = pixel_shader;
  if (!resolved && device &&
      HostPageReadable(REX_RAW_ADDR(device + 0x3244)))
    resolved = REX_LOAD_U32(device + 0x3244);
  // Last resort: the device's own last-bound shader, recorded across threads.
  // Both sources above are thread-local in effect -- the argument comes from a
  // thread_local DeviceState, and device+0x3244 read zero for 15,555 draws on a
  // loaded menu -- and without this those draws take the tex*col stand-in and
  // paint whatever their first texture happens to be, which for a character
  // material is a packed normal/gloss map. Cost of it having been written and
  // never called: 79,984 of 240,000 draws arrive with no setter handle, and no
  // translated pixel shader means no GPU vertex path, so they run the software
  // interpreter -- 121ms of a 225ms frame.
  //
  // Gated, because it is both the largest speedup this session (menu 4.45 ->
  // 9.88 fps) and a suspected regression ("unbound by sampler s0/s1/s2" appears
  // in no earlier run and in every later one), and those have to be separable.
  //
  // WHOSE device it is, is recorded BEFORE the fallback runs. Both offsets are
  // confirmed against the guest -- D3DDevice_SetPixelShader writes
  // pDevice[1].m_Constants.Fetch[29], which is device+0x3244 -- so either the
  // shader was never set on THIS device, or the draw carries the wrong device.
  if (!resolved) {
    // What ARE these draws running? Self-limiting; see ProbeVertexObjectSecondBlob.
    ProbeVertexObjectSecondBlob(device, base);
    // Does a null-pixel-shader draw bind a COLOUR target?
    //
    // The probe above establishes what these draws are: one 48-dword program
    // that writes position and exports no interpolators, i.e. a depth-only pass.
    // The renderer already has a route for that, but it opens only when NO
    // colour target is bound:
    //
    //     depthOnlyPass = !d.targetObject && d.depthObject && ...
    //
    // If these draws carry a colour target too they miss it, and each is given a
    // scratch colour target plus the tex*col stand-in -- painting colour the
    // guest never wrote, into the scene buffer the rider's material later samples
    // for its luminance.
    {
      static std::mutex s_mu;
      static uint64_t s_colour_and_depth = 0, s_depth_only = 0;
      static uint64_t s_colour_only = 0, s_neither = 0;
      static std::set<uint64_t> s_extents;
      // Whether they actually PAINT is a separate question from whether they bind
      // a target, and it is decided by the colour mask, which the renderer
      // already honours:
      //
      //     colorWrite = (om_seen & 1) == 0 || (colour_mask & 0xF) != 0
      //
      // Read RB_COLOR_MASK from the device HERE rather than through dc:
      // dc.colour_mask and dc.om_seen are filled ~34 lines AFTER the
      // PrepareDrawTexture call this runs inside, so consulting them reports
      // every draw in the game as "mask never observed".
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
  // THE CROSS-THREAD PIXEL SHADER FALLBACK IS GONE -- the second guard removed
  // under docs/strict_mode.md.
  //
  // It called PixelShaderForDeviceStrict(device) for any draw arriving with no
  // shader from either per-device source, and across both scenes it never once
  // supplied one (freeroam 0/36062, menu 0/5804). The population is not draws we
  // are failing: every null-PS draw binds depth only, or binds colour with the
  // write mask OFF -- WOULD PAINT 0 of 30536 and 0 of 2098. It also settles the
  // hypothesis at the probe above: NO-PS DEVICES reports ONE device on ONE
  // thread, so the guest genuinely called SetPixelShader(NULL).
  //
  // THE 4.45 -> 9.88 FPS MENU WIN IS NOT THIS FUNCTION: that came from
  // PixelShaderForDevice, which falls back to the last shader seen on ANY device.
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
    // with a handle, which said it happens without saying how often -- and it is
    // the largest single reason a draw keeps the tex*col stand-in, larger than
    // untranslated shaders and slot-filling together.
    //
    // Split by which of the two resolutions was even possible: no setter handle
    // at all is a different defect from a setter we cannot follow.
    if (!pixel_shader) ++s_no_shader_no_setter;
    if (s_no_shader <= 8 || (s_attempts % 2500) == 0) {
      const uint32_t direct =
          device && HostPageReadable(REX_RAW_ADDR(device + 0x3244))
              ? REX_LOAD_U32(device + 0x3244)
              : 0;
      // Named for what it now means. This used to read "NO ELIGIBLE PIXEL
      // SHADER", which was true when the contest also decided whether the draw
      // could run its guest shader at all; a draw reaching here may well be
      // running a translated shader with every slot bound.
      //
      // The "rescued by the per-device record" figure is GONE with the fallback
      // that produced it: leaving it would print a permanent zero, which reads
      // as a measurement of a mechanism that no longer exists.
      REXLOG_INFO("d3d9: stand-in has no single texture to sample: {} of {} "
                  "attempts ({} with no setter handle at all); this one "
                  "setter=0x{:08X}, device+0x3244=0x{:08X}",
                  s_no_shader, s_attempts, s_no_shader_no_setter, pixel_shader,
                  direct);
    }
    return false;
  }
  // The shader is resolved by here -- `pixel_shader` may have been rewritten by
  // ReadBoundPixelShader -- so this is the first point at which the draw knows
  // which guest program it is running. A draw whose shader did not translate
  // carries nothing and keeps the stand-in. If the contest rewrote
  // `pixel_shader` to the device's own handle, attach again for that shader.
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
        // arriving on a resolved target produced no log line at all — which is
        // why "no YUV format has ever been rejected" was not evidence of
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
  // memory is a resolve destination reached through a texture object the resolve
  // never named. Both routing fields are set here rather than left to
  // PrepareHleDraw, which sets them only when its own object lookup hits, runs
  // after this, and does not clear them on a miss.
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
  // surface grey. They are decoded rather than rejected so the counters can tell
  // "we cannot read this" apart from "we choose not to show this".
  //
  // kRg8 (k_8_8) is two-channel and joins them: the stand-in shader has no idea
  // what the second channel means. The translated path, where these 1273
  // rejections a run were actually losing draws, goes through
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
  // Blank textures are still a poor representative for the one texture this path
  // picks to stand in for the whole draw -- but only while they ARE blank. The
  // refusal expires on the same backoff the translated path retries on, so a
  // texture the guest streams in later is reconsidered.
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
  NoteDecodedTexture(source, *payload, guest.data(), guest.size(),
                     binding.sampler, "standin");
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
  SetPayloadUploadVersion(source, *payload);
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


}  // namespace mx::hooks::d3d9
