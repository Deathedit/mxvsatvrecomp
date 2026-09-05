// The declaration-to-vfetch pairing rule.
//
// Split verbatim out of hooks_d3d9_texture.cpp, which is a file about resolving
// and decoding textures and had 590 lines of vertex-shader reverse engineering
// at the end of it. Different subject, different guest function: this is what
// D3D::PatchVertexShaderToMatchVertexDeclaration (0x82564C50) does to a shader
// when a declaration is bound, worked out by capturing the code it writes and
// comparing against what the rule predicts.
//
// The seam was chosen by measurement, the way hooks_d3d9_shared.h says to
// choose one. The block defines 30 names; five are named outside it, and all
// five were ALREADY declared in hooks_d3d9_internal.h or hooks_d3d9_shared.h --
// ReadPatchFetchCount, CapturePatchedCode, CheckPatchedFetches,
// PredictPatchedFetches and ReportPatchRule. So this publishes nothing new and
// needs no header of its own. The other 25 names stop being reachable from
// anywhere else, which is the direction that boundary is supposed to move.

#include "hooks/hook_common.h"

#include "hooks/hooks_d3d9_shared.h"

#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace mx::hooks::d3d9 {

//---------------------------------------------------------------------------
// The declaration-to-vfetch pairing rule, read out of
// D3D::PatchVertexShaderToMatchVertexDeclaration (0x82564C50).
//
//   this   = r3   CVertexShader*
//   dest   = r4   the microcode being patched -- where results are written
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
// (byte 9) and `usage_index` (byte 10) equal the key's patches that vfetch -- a
// linear search, first match wins. That is why the template's format/offset/
// stride are blank: they are not defaults, they are unbound.
//
// From the matched element:
//   fetch constant index = 95 - element.stream       (subfic r20, r5, 0x5F)
//   format/signed/integer/swizzle from the Type dword
//   offset field = element.offset / 4                (rlwinm r8, r8, 6, 1, 23)
//   stride field = strides[element.stream]           (lbzx r5, r5, r6)
//
// **No match leaves fetch constant 95 as well**, the same value stream 0
// produces, so a decoded fetch_slot of 95 is ambiguous between "stream 0" and
// "unbound" -- the unbound case is identifiable by its canned format bits
// (0x60000) and swizzle (0x9250) instead.
//
// None of this is believed on the strength of the disassembly: the probe
// predicts all three dwords of the patched vfetch *before* the call and compares
// them against what D3D9 actually wrote *after* it.
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

// Where does D3D9 write the patched microcode? r4 is that destination, and if it
// is the same memory the draw-time probe already reads (SH_pPhysical + 0x40),
// the patched code is directly readable and none of this rule needs
// reimplementing. Measured rather than assumed, because Stage B concluded that
// buffer held the unpatched template.
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
// ring and will be overwritten. The shader OBJECT also carries its own
// microcode, which matters because the patch hook only fires for shaders D3D9
// needs to patch -- everything else was reported as "no-code", 41% of draws in
// one run.
//
// Both transcribed from sub_82565550, which allocates ring space, copies the
// code in, and only THEN calls the patcher on the ring copy:
//
//   v17 = *(*(self + (variant+0x70)*8) + self + 876)   // size in BYTES
//   v23 = *(*(self + (variant+0x70)*8) + self + 872) + *(self + 0x20)
//   v24 = (((v23 >> 20) + 512) & 0x1000) + (v23 & 0x1FFFFFFF) - 0x40000000
//   memcpy(dest, v24, v17)
//
// 872 is kUCodeBlobDelta and the size sits at blob+4, beside the fetch count at
// blob+0x1C this file already reads. The code is at blob + *(self+0x20) through
// an address fixup that clears the 0x40000000 segment bit -- which is why
// searching a window around the blob found NOTHING in 36,000 shaders.
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
  // expression in the patcher uses the same address WITHOUT one. Taking blob
  // itself put the read 0 of 28,000 shaders' first eight dwords.
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
  // agree EXCEPT the vfetch fields the patch then rewrote in the ring. A handful
  // of differing dwords confirms the address; wholesale disagreement means it is
  // the wrong buffer.
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

  // As a SHARE of the shader, not an absolute count. 226 differing dwords is 11%
  // of a 2000-dword program and 95% of a 237-dword one, and those mean opposite
  // things -- the first is the patch rewriting fetches, the second is the wrong
  // buffer.
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
  // version cached the offset and reused it blind, and the draw-time decode then
  // failed on thousands of captures while the report happily said the shader was
  // resolved. A cached answer that is never re-checked is an assumption wearing
  // a measurement's clothes.
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
  // early in 3 of them because it takes the first offset that decodes and a
  // false positive can precede the true start. Still verified, not assumed.
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

  // Does the guest state the answer the search just hunted for? sub_82565928's
  // VS branch computes the program address the GPU is given as *(vs + 0x20) +
  // *(info + 0x368), where info = vs + *(vs + 0x380 + variant*8), with the
  // length in bytes at info + 0x36C; the patcher indexes the identical field.
  // Compared as ABSOLUTE guest addresses, the only common ground.
  //
  // Measurement only -- the search is still what runs. The LENGTH is read
  // unconditionally, because it bounds the code handed to the decoders; it is
  // the canonical program's, taken as applying to the patched copy too, since
  // patching rewrites fetch instructions in place.
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
  // a walk that does not stop on its own continues into the next shader.
  if (pc.resolved && field_len && (field_len & 3) == 0) {
    const size_t want = pc.code_off + field_len / 4;
    if (want >= 8 && want <= pc.code.size()) {
      pc.code.resize(want);
      pc.code_len_dwords = field_len / 4;
    } else {
      ++g_vsWindow.lenRejected;
    }
  }

  if (pc.resolved && REXCVAR_GET(hle_diag)) {
    const uint32_t search_abs = start + pc.code_off * 4;
    // Two independent questions, kept apart because they have different answers.
    // Where the CF starts: dest, in 24 of 24 measured. Whether the shader
    // object's own allocation is the buffer that was patched: only sometimes --
    // 16 of 24 -- so the field is NOT a drop-in source of code.
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
  // A ring-window read is inherently transient: the destination may wrap or be
  // overwritten between the original call and this hook's copy. Never let such a
  // failed observation destroy a previously proven capture for the same shader
  // variant -- this used to turn valid shaders back into "no exact patched code"
  // later in the frame.
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
