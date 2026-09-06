// State shared between hooks_d3d9.cpp and hooks_d3d9_texture.cpp.
//
// hooks_d3d9.cpp was 9386 lines and the most-edited file in the project (16 of
// the last 40 commits), so every edit recompiled all of it.
//
// This surface is deliberately small in the part that matters. The file holds
// 224 mutable globals, and a naive split published NINETEEN of them here.
// Grouping the families that cross this boundary into five structs brought that
// to six objects: types, functions and constants in a header are ordinary; loose
// mutable globals are the smell, and six is the number to watch.

#pragma once

#include <rex/cvar.h>
#include <rex/graphics/format/ucode.h>

#include "gpu/d3d9_draw.h"
#include "gpu/d3d9_layout.h"
#include "gpu/d3d9_state.h"
#include "gpu/d3d9_texture.h"
#include "gpu/hle_types.h"
#include "gpu/shader_hlsl.h"
#include "gpu/shader_ucode.h"
#include "gpu/xenos_gpu_state.h"
#include "hooks/hooks_d3d9_internal.h"

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

// Both TUs read these. REXCVAR_DECLARE expands to a storage accessor at global
// scope; hooks_d3d9.cpp had the same two lines above its namespace.
REXCVAR_DECLARE(bool, hle_diag);

namespace mx::hooks::d3d9 {

namespace xn = rex::graphics::xenos;

// ---- types -----------------------------------------------------------------

// Why PrepareBinkPlanes refused. Every one of these used to be a bare
// `return false` -- no counter, no log -- so a run in which the composite never
// happened looked identical to one in which it was never asked for: 54,000
// calls, 0 successes, and not one line to say which of the five walls they hit.
//
// `no_fetch` is the one to read first: it is the only refusal that does not come
// from the texture itself but from the DEVICE's live fetch registers, a
// different source from the DeviceState texture bindings a draw probe prints.
struct BinkPlaneRefusals {
  uint64_t calls = 0;
  uint64_t ok = 0;
  uint64_t no_fetch = 0;    // ReadLiveTextureFetch failed at slot s
  uint64_t describe = 0;    // fetch constant unusable
  uint64_t copy = 0;        // guest memory unreadable
  uint64_t decode = 0;      // bytes there, decode refused
  uint64_t too_few = 0;     // fewer than three planes survived
  uint32_t first_fail_slot = 0xFFFFFFFFu;  // where no_fetch first broke
};

struct PatchedCode {
  std::vector<uint32_t> code;   // host-endian, from dest - kPatchWindowBack*4
  uint32_t expect_fetches = 0;  // what the binding table said
  uint32_t variant = 0;
  uint32_t code_off = 0;        // dwords into `code` where the CF section is
  uint32_t code_len_dwords = 0;  // real program length, 0 = unknown
  bool     resolved = false;    // code_off was found by decoding, not assumed
};

struct PhaseTimer {
  uint64_t& sink;
  std::chrono::steady_clock::time_point t0;
  explicit PhaseTimer(uint64_t& s)
      : sink(s), t0(std::chrono::steady_clock::now()) {}
  ~PhaseTimer() {
    sink += uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                         std::chrono::steady_clock::now() - t0)
                         .count());
  }
};

struct TexDecodeSite {
  uint64_t decodes = 0;
  uint64_t bytes = 0;
  uint32_t width = 0, height = 0, format = 0;
  uint64_t by_reason[3] = {0, 0, 0};
  uint64_t distinct_keys = 0;
};

// The emitted source, kept per shader handle. Shared with every draw that binds
// the shader, so a frame's ~158 draws across a few dozen shaders copy a pointer
// rather than a few kilobytes of text each. Defined here rather than beside the
// emitter probe that fills it because ApplyShaderOutputs now reads the vertex
// stage's input_mask to build the GPU vertex layout.
// The guest's own names for a shader's constant registers and sampler slots,
// out of the D3DXSHADER_CONSTANTTABLE in its .shader asset. 19,967 constants
// and 5,124 samplers across the corpus; 294 and 180 distinct names.
//
// This is the difference between `xe_c[4]` and `gViewProjection`, and between
// `s0` and `usr_sampler_diffuse_texture`. Nothing consumes it yet -- it is the
// input the later steps need, and worth noting that the transform probe in
// d3d9_draw.h has been SCORING CANDIDATES for a matrix register whose name is
// sitting in here (`gViewProjection`, in 2396 of the entry points).
//
// Ordered vectors rather than maps: a table holds a handful of entries, no
// lookup is on the draw path, and this way the whole thing is two allocations.
struct ShaderConstantTable {
  // (register or slot, name), sorted by the first.
  std::vector<std::pair<uint32_t, std::string>> constants;
  std::vector<std::pair<uint32_t, std::string>> samplers;

  const std::string* Constant(uint32_t reg) const {
    for (const auto& [r, n] : constants)
      if (r == reg) return &n;
    return nullptr;
  }
  const std::string* Sampler(uint32_t slot) const {
    for (const auto& [r, n] : samplers)
      if (r == slot) return &n;
    return nullptr;
  }
};

// Texture content-name join; see TextureNames() in hooks_d3d9_texture.cpp.
// Published because the report that prints them lives in another TU, and
// because they must appear on a line that prints EVERY segment: decodes are
// rare once the cache is warm -- one reporting window showed 2287 cache hits
// against 1 decode -- so a counter that reports every N decodes never fires,
// and the startup burst rotates out of the log before anyone reads it.
struct TextureNameCensus {
  uint64_t seen = 0;   // every texture the runtime decoded
  uint64_t named = 0;  // resolved to a .texture asset by content
  uint64_t noKey = 0;  // no measurable level 0, so nothing to join on
  // Of `noKey`, how many had a guest buffer SHORTER than the described level
  // 0. Split out because that is a disagreement between the allocation and the
  // extent -- a real signal -- while the rest are simply textures with no
  // describable block layout, which a render target legitimately is.
  uint64_t shortBuffer = 0;
  // Of `named`, how many the 4096-byte prefix key found. Reported because the
  // full key cannot reach the textures whose described extent exceeds their
  // buffer, and that is most of what was unnamed.
  uint64_t byPrefix = 0;
  // HAD A KEY AND MATCHED NOTHING. The bucket that was missing, and the only
  // one that can distinguish "these textures have no asset" from "the asset
  // bytes are not the guest bytes". Without it the three counters did not sum
  // to `seen` and the interesting number had to be got by subtraction.
  uint64_t unmatched = 0;
  // Unmatched and NAMED, both broken down by guest texture format.
  //
  // This is what closes the question. The asset corpus is six formats -- DXT1
  // 4779, DXT4_5 1236, DXN 623, 16_16_16_16 110, 8_8_8_8 93, 16 55 -- and
  // nothing else. A texture in FMT_4_4_4_4 or FMT_8 cannot match because no
  // asset is in that format, so it is not a miss, it is a texture the game
  // generated. Counting by format separates "has no asset" from "has an asset
  // and we failed to find it" without guessing from a sample of twelve.
  //
  // 64 formats; the field is six bits.
  uint64_t unmatchedByFormat[64] = {};
  uint64_t namedByFormat[64] = {};
};
extern TextureNameCensus g_texNames;

struct TranslatedShader {
  std::shared_ptr<const std::string> source;  // null unless emitted AND compiled
  uint32_t input_mask = 0;
  // Which interpolator slots a VERTEX shader actually exports. Needed to pair
  // against a pixel shader's input_mask at draw time: a slot the PS reads and
  // the VS never exports arrives as a literal zero.
  uint32_t export_mask = 0;
  uint32_t sampler_mask = 0;
  uint32_t sampler_count = 0;
  uint32_t sampler_array_mask = 0;
  uint32_t slot_guest[mx::hle::HlslShader::kMaxSamplerSlots] = {};
  uint32_t max_const_index = 0;
  // The compiled DXBC for `source`, when the persisted cache held it or the
  // first compile wrote it. Null falls back to the renderer compiling the
  // source itself, which is the pre-cache behaviour.
  std::shared_ptr<const std::vector<uint8_t>> dxbc;

  // The same vertex shader emitted a second way: performing its own vfetches out
  // of the raw guest vertex buffer, indexed by SV_VertexID. Null when that
  // variant refused or did not compile, in which case the draw stays on the CPU
  // vertex path. Both are kept because they are not interchangeable -- the fetch
  // variant has an empty input layout and needs xe_vf[], the other needs an input
  // layout built from input_mask.
  std::shared_ptr<const std::string> fetch_source;
  uint32_t vertex_fetch_count = 0;
  uint32_t vertex_fetch_slot[mx::hle::HlslShader::kMaxVertexFetches] = {};
  // Streams whose fetch is indexed by a register the shader computed rather than
  // by the vertex index. The CPU vertex path cannot reproduce those indices at
  // all, so it zero-fills them instead of reading an unrelated row.
  uint32_t computed_index_streams = 0;
  // See HlslShader::const_mask.
  uint64_t const_mask[4] = {};
  bool const_relative = false;
  // Per fetch ordinal; see HlslShader::computed_index_fetches.
  uint32_t computed_index_fetches = 0;
  // The fetch variant's compiled DXBC, same contract as `dxbc`.
  std::shared_ptr<const std::vector<uint8_t>> fetch_dxbc;
  // "<asset>::<EntryPoint>" from userdata/shader_names.txt, or null when this
  // shader's microcode is not in the map. A BARE POINTER on purpose: it aims
  // into a table loaded once and never erased, so it outlives every draw, and
  // a draw reading it pays a deref rather than a refcount.
  //
  // Null is a real answer, not a default. Two distinct reasons produce it and
  // `runtime_generated` separates them.
  const std::string* name = nullptr;
  // The manifest positively identified this shader as built by the guest at
  // runtime -- a clear, a blit, a depth-only pass -- rather than loaded from a
  // .shader asset. Measured offline: no window of even half the blob appears
  // anywhere in the assets. There is no name to find, so a draw running one is
  // NOT a coverage failure and must not be reported as one.
  bool runtime_generated = false;
  // The constant/sampler names for this shader, or null when the manifest has
  // no table for it -- normal for a runtime-generated shader and for one the
  // assets could not name. Bare pointer for the same reason `name` is: it aims
  // into a table loaded once and never erased.
  const ShaderConstantTable* constant_table = nullptr;
};

// INSIDE the texture bucket. A finer breakdown is worth having only once one
// bucket is known to dominate -- it now is: texture runs 156-182ms against
// 29-35ms for the whole vertex path, about 80% of a steady frame.
//
// Split so the answer cannot be argued: a hit that is expensive points at the
// staleness fingerprint, a miss that is expensive points at copy + decode.
// `scan` is the pair of whole-buffer passes that follow every decode, counted
// separately because they are OURS, not the guest's.
//
// Grouped into one object rather than eleven loose globals, so that splitting
// this file publishes ONE symbol instead of eleven.
struct TextureStats {
  uint64_t describeUs = 0, staleUs = 0, copyUs = 0;
  uint64_t decodeUs = 0, scanUs = 0;
  uint64_t slotCalls = 0, cacheHits = 0, staleEvicts = 0;
  uint64_t decodes = 0, decodedBytes = 0;
  uint64_t phaseUs = 0;  // was g_tex.phaseUs
};

// One object rather than seven loose counters, so a split publishes one symbol.
// Values and meanings unchanged.
struct VsWindowCensus {
  uint64_t agree = 0, disagree = 0, noField = 0;
  uint64_t atDest = 0, early = 0, late = 0;
  uint64_t lenRejected = 0;
};

// One object rather than three loose counters, so a split publishes one symbol.
struct ResolveAddressCensus {
  uint64_t matches = 0;      // blanks rescued by the address match
  uint64_t extentMiss = 0;   // same address, different extent
  uint64_t partial = 0;      // matched, but barely written by the GPU
};

// Winning start, as a signed dword distance from dest. The histogram is the
// finding: one value across every shader means a fixed layout. One object rather
// than three containers, so all three live together and the struct can be
// declared after PatchedCode.
struct ShaderPatchState {
  std::map<uint32_t, std::vector<uint32_t>> psBlobs;
  std::map<int32_t, uint64_t> codeOffsets;
  std::map<uint32_t, PatchedCode> patched;   // shader handle -> latest
};

// Cumulative, NOT per frame: the question is whether the same texture repeats
// across frames, which a per-frame counter cannot show.
// One object rather than two containers, for the same reason as the counters.
struct TexDecodeIndex {
  std::map<uint32_t, TexDecodeSite> sites;
  std::set<std::pair<uint32_t, uint64_t>> keys;
};

// One object rather than three loose counters, so a split publishes one symbol.
//
// Flat-decode retry backoff: keys marked, and retries actually taken. Both,
// because `marked N retried 0` and `marked N retried M` are different bugs.
struct FlatDecodeCensus {
  uint64_t notCached = 0;   // was g_flatNotCached
  uint64_t retriesDue = 0;  // was g_flatRetriesDue
  uint64_t volatileKeys = 0;  // was g_flatVolatile -- decode sites and keys
};

enum class TexMissReason : uint8_t {
  kNotInCache,   // key never seen
  kStaleEvicted, // present, but the content fingerprint changed
  kBlankRetry,   // decoded blank before; never cached, retried on a backoff
};

// ---- the six shared objects ------------------------------------------------
extern TextureStats g_tex;                  // 11 texture-path counters
extern VsWindowCensus g_vsWindow;           // 7 patch-window counters
extern ResolveAddressCensus g_resolveAddr;  // 3 resolve-address counters
extern ShaderPatchState g_patch;            // psBlobs / codeOffsets / patched
extern TexDecodeIndex g_texIndex;
extern FlatDecodeCensus g_flat;             // 3 flat-decode retry counters

// ---- constants -------------------------------------------------------------
constexpr uint32_t kHostPageSize = 4096;
constexpr uint32_t kPhysProbeDwords = 256;
constexpr uint32_t kPatchWindowBack = 128;     // dwords captured before dest
constexpr uint32_t kVsInfoOffsetAt = 0x380;    // + variant*8 -> info block
constexpr uint32_t kVsInfoCodeOffset = 0x368;  // CF byte offset in the allocation
constexpr uint32_t kVsInfoCodeSize = 0x36C;    // program length in bytes

// ---- functions -------------------------------------------------------------

BinkPlaneRefusals BinkPlaneRefusalStats();
bool CopyTexturePhysical(const mx::hle::HleTextureSource& source, uint8_t* base,
                         std::vector<uint8_t>& out);
bool IsBinkCompositeDraw(uint32_t pixel_shader, uint8_t* base);
void NoteDrawThread();
std::string PixelShaderDeviceSummary();
uint32_t PixelShaderForDevice(uint32_t device, bool* from_fallback);
uint32_t PixelShaderForDeviceStrict(uint32_t device);
std::string ShaderTranslationSummary();
bool PrepareBinkPlanes(mx::hle::DrawCall& dc, uint32_t device, uint8_t* base);
bool PrepareDrawTexture(mx::hle::DrawCall& dc, uint32_t pixel_shader,
                        uint32_t device, uint8_t* base,
                        mx::hle::PixelTextureBinding& binding);
void ProbeBinkComposite(uint32_t pixel_shader, uint32_t vertex_shader,
                        uint32_t device, uint8_t* base, uint32_t vertex_count);
void ProbePixelProfileForDraw(uint32_t pixel_shader, uint32_t device,
                              uint8_t* base, const mx::hle::DrawCall& dc);
void ProbeVertexObjectSecondBlob(uint32_t device, uint8_t* base);
void ReportHlslCoverage(mx::hle::HlslStage stage, uint32_t handle,
                        const uint32_t* code, uint32_t count);
ResolvedTargetByAddress* ResolveEntryForObject(uint32_t dest_object);
bool ResolvedDestinationIsMostlyWritten(uint32_t dest_object);
const TranslatedShader* TranslatedPixelShader(uint32_t handle);
const TranslatedShader* TranslatedVertexShader(uint32_t handle);

// Defined in the texture TU, called from hooks_d3d9.cpp. Found by asking which
// symbols the kept file still references, not by guessing.
void AttachTranslatedPixelShader(mx::hle::DrawCall& dc, uint32_t handle,
                                 uint32_t device, uint8_t* base);
void CapturePatchedCode(uint32_t self, uint32_t dest, uint32_t variant,
                        uint8_t* base);
uint32_t ReadPatchFetchCount(uint32_t self, uint32_t variant, uint8_t* base);
void ReportPatchRule();
// Whether any RESOLVE reaches a guest address range, with the denominator. On
// Xenos a render target lives in EDRAM and only a resolve moves GPU output into
// guest memory, so a zero here proves the GPU never wrote those bytes. See the
// note at the definition for why `resolved=0` could not answer this.
struct ResolveRangeProbe {
  uint32_t total = 0;         // resolve destinations known -- the denominator
  uint32_t exact = 0;         // a destination starts exactly at this address
  uint32_t inside = 0;        // destinations whose base is within the range
  uint32_t first_addr = 0;    // the exact or first inside destination
  uint32_t first_width = 0;
  uint32_t first_height = 0;
  uint32_t below_addr = 0;    // nearest destination base below the range
  uint32_t below_delta = 0;
  uint32_t below_width = 0;
  uint32_t below_height = 0;
  bool any() const { return exact || inside; }
};
ResolveRangeProbe ProbeResolveRange(uint32_t address, uint32_t bytes);

const ResolvedTargetByAddress* ResolvedTargetForAddress(
    const mx::hle::HleTextureSource& described);

}  // namespace mx::hooks::d3d9
