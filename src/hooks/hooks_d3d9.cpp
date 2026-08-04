// D3D9 entry-point hooks — observation only.
//
// The title statically links D3D9 v2.0.20209.3, so these functions are in the
// XEX but were nameless until an XDK d3d9.lib was matched against it by bytes
// (see "The D3D9 entry points are located" in AGENTS.md, and
// tools/match_d3d9.py). The control for that match was D3DDevice_Swap, whose
// COMDAT is 0x684 bytes — the exact size of the already-confirmed
// sub_82566B58, and its pattern matched that address and nothing else.
//
// Why this is worth hooking at all: every colour round so far has inferred
// vertex layout from PM4 and shader microcode, because on Xenos a vfetch
// carries format and offset but not semantic. The semantics exist one layer
// up, in the D3DVERTEXELEMENT9 arrays the game hands to
// D3DDevice_CreateVertexDeclaration. They are not static data — four different
// 12-byte D3DDECL_END sentinels return zero matches binary-wide — so the only
// way to see them is to catch them being built.
//
// Everything here passes through to the original and changes no guest state.
// The point of this round is two numbers and one table:
//
//   1. What the declarations actually are.
//   2. Whether the D3D9 draw count matches the translator's transcoded draw
//      count. 53 static call sites are consistent with thousands of draws per
//      frame, but that is an inference; these counters test it.

#include "hooks/hook_common.h"

#include <rex/cvar.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include <fstream>

#include "gpu/d3d9_draw.h"
#include "gpu/d3d9_layout.h"
#include "gpu/shader_ucode.h"   // DecodeVertexShaderFetches, VertexAttribute
#include "gpu/shader_alu.h"     // ExecuteVertexShader
#include <cmath>
#include "gpu/d3d9_state.h"

// Defined in src/app/graphics_system.cpp with the rest of the Debug cvars.
REXCVAR_DECLARE(bool, hle_capture);
REXCVAR_DECLARE(bool, hle_render);
REXCVAR_DECLARE(uint32_t, hle_shader_exec);
REXCVAR_DECLARE(uint32_t, hle_shader_verts);

namespace {

using mx::pm4::DeviceState;

// Declarations are built during load, and the rotating log (3 x 5MB) only
// retains the last ~50 seconds of a 165s run — the first attempt at this probe
// logged every declaration and then lost all of them. Anything created early
// has to go somewhere that does not rotate, so this follows the pm4_dump_*.txt
// convention and writes next to the executable.
std::ofstream& DeclFile() {
  static std::ofstream f("d3d9_dump_decls.txt", std::ios::trunc);
  return f;
}

// D3DVERTEXELEMENT9 is *12 bytes on Xenon*, not the 8 of the PC struct. This
// is not an assumption: both D3DDevice_CreateVertexDeclaration (0x82550B80)
// and XGSetVertexDeclaration (0x82550A90) walk the array with `lhzu r9, 0xC`,
// and XGSetVertexDeclaration copies each element as three dwords. An earlier
// round searched the XEX for the 8-byte PC sentinel, which is one reason it
// found nothing.
//
// Stream is the halfword at offset 0 and terminates the array at 0xFF — that
// much is read directly by both functions. The remaining ten bytes are dumped
// raw rather than decoded, because nothing observed so far pins their layout.
// Both now live in gpu/d3d9_layout.h, which the decoder and its test share.
using mx::pm4::kElementSize;
using mx::pm4::kMaxElements;   // refuses to walk a runaway array
// A first run hit 23 of a 24 cap, which says nothing about how many exist.
// The dump is a few hundred bytes per declaration and does not rotate, so the
// cap is only here to bound a runaway.
constexpr int kMaxDeclsLogged = 512;
constexpr int kMaxDrawsLogged = 16;
constexpr uint64_t kDrawReportEvery = 2500;  // see the om1 trap in AGENTS.md

uint64_t g_indexed_draws = 0;
uint64_t g_draws = 0;
uint64_t g_decls = 0;
uint64_t g_patchCalls = 0;

//---------------------------------------------------------------------------
// Finding the active vertex declaration at draw time.
//
// The draw entry points take D3DDevice* in r3 but not the declaration, so it
// has to be read off the device. D3DDevice_SetVertexDeclaration would be the
// obvious hook instead, but it is 20 bytes and under 128 bytes a byte match is
// not evidence — hence reading the device.
//
// **Nothing here dereferences an unknown pointer.** An earlier version walked
// the device treating each dword as a pointer and checking the target for
// XGSetVertexDeclaration's 0x00100005 magic. That crashed the guest with an
// access violation at 0x030013A0: the arena is *not* fully mapped, so a
// speculative read of a garbage value faults. Every identification below is a
// comparison against an object we watched being created.
//---------------------------------------------------------------------------

// Every declaration seen by CreateVertexDeclaration, with what matters about
// it. Ids are creation order.
constexpr int kMaxTrackedDecls = 256;
uint32_t g_declPtr[kMaxTrackedDecls] = {};
uint32_t g_declElems[kMaxTrackedDecls] = {};
bool g_declHasColour[kMaxTrackedDecls] = {};
uint64_t g_declDraws[kMaxTrackedDecls] = {};
int g_declCount = 0;
uint64_t g_drawsNoDecl = 0;      // draws whose declaration we never saw created

// The host input layout each declaration decodes to, built once at creation.
// A declaration that fails to decode keeps `layout_ok = false` and its failure
// reason, so the coverage report can name it rather than count it.
mx::pm4::HleInputLayout g_declLayout[kMaxTrackedDecls];
bool g_declLayoutOk[kMaxTrackedDecls] = {};
mx::pm4::LayoutError g_declLayoutErr[kMaxTrackedDecls] = {};

int KnownDeclId(uint32_t p) {
  if (!p) return -1;
  for (int i = 0; i < g_declCount; ++i) {
    if (g_declPtr[i] == p) return i;
  }
  return -1;
}

// Called from the CreateVertexDeclaration hook, where both pointers are valid.
// Returns the id, or -1 if the table is full.
int RecordDeclaration(uint32_t decl, bool has_colour, uint32_t elems,
                      const mx::pm4::D3D9Element* parsed) {
  if (!decl || g_declCount >= kMaxTrackedDecls) return -1;
  const int existing = KnownDeclId(decl);
  if (existing >= 0) return existing;   // pointer reuse after a free
  const int id = g_declCount++;
  g_declPtr[id] = decl;
  g_declElems[id] = elems;
  g_declHasColour[id] = has_colour;

  g_declLayoutOk[id] = mx::pm4::BuildInputLayout(parsed, elems, g_declLayout[id],
                                                 g_declLayoutErr[id]);
  return id;
}

// The current declaration lives at device + 0x2ED8.
//
// **This offset is read out of the library, not searched for.**
// D3DDevice_SetVertexDeclaration is 20 bytes and does nothing but this:
//
//     stw   r4, 0x2ed8(r3)      device->pVertexDeclaration = pDecl
//     ld    r11, 0x10(r3)
//     oris  r11, r11, 0x8       mark the lazy state dirty
//     std   r11, 0x10(r3)
//     blr
//
// D3DDevice_GetVertexDeclaration reads the same field back (`lwz r31,
// 0x2ed8(r3)`), which settles it independently of how the store is read.
//
// Two earlier scans "proved" the declaration was not on the device struct. Both
// covered device + 0..0x2000, and 0x2ED8 is outside that — the scans were
// under-scoped, not the conclusion sound. Scoping a scan by what the struct
// actually spans (SetStreamSource writes +0x3480) was the missing step both
// times, and reading the offset from the code that writes it makes the scan
// unnecessary altogether.
constexpr uint32_t kDeviceVertexDeclaration = 0x2ED8;

// Reading device + 0x2ED8 is safe in a way that dereferencing its *value* is
// not: the device pointer arrives as the draw's own r3, D3D9 is reading the
// same struct on either side of this hook, and the offset is well inside it.
// The value read is only ever compared against declarations we watched
// CreateVertexDeclaration build — never followed.
int g_currentDecl = -1;

// What PatchVertexShaderToMatchVertexDeclaration last saw. Kept only to measure
// how far it lags: it fires on the lazy-state path, ~1 update per 66 draws, and
// the previous round mistook attribution-to-a-stale-value for attribution.
int g_patchDecl = -1;

uint64_t g_declDeviceNull = 0;      // field is 0 — no declaration bound yet
uint64_t g_declDeviceUnknown = 0;   // non-zero, but never seen created
uint64_t g_declAgree = 0;           // device field == the patch hook's value
uint64_t g_declDisagree = 0;        // it does not, i.e. the patch value is stale

// Called from both draw hooks.
void NoteDrawDeclaration(uint32_t device, uint8_t* base) {
  (void)base;
  g_currentDecl = -1;
  if (device) {
    const uint32_t p = REX_LOAD_U32(device + kDeviceVertexDeclaration);
    if (!p) {
      ++g_declDeviceNull;
    } else {
      g_currentDecl = KnownDeclId(p);
      if (g_currentDecl < 0) ++g_declDeviceUnknown;
    }
  }
  if (g_currentDecl >= 0) {
    if (g_currentDecl == g_patchDecl) {
      ++g_declAgree;
    } else {
      ++g_declDisagree;
    }
  }

  DeviceState().current_decl = g_currentDecl;
  if (g_currentDecl < 0) {
    ++g_drawsNoDecl;
    return;
  }
  ++g_declDraws[g_currentDecl];
}

//---------------------------------------------------------------------------
// HleDraw coverage.
//
// The question this round has to answer in writing: at each draw, is the
// description complete? Anything missing is counted under the field that was
// missing, never folded into one "incomplete" total — a renderer built on a
// partial description fails in ways that look like rendering bugs, and by then
// the reason is three layers away.
//
// Nothing here reads guest memory. Every value was captured by the hook that
// set it, at the moment D3D9 was reading the same bytes.
//---------------------------------------------------------------------------
enum DrawGap : uint32_t {
  kGapDeclaration = 0,  // no declaration bound yet
  kGapLayout,           // the declaration bound does not decode
  kGapStream,           // a stream the layout uses was never set
  kGapStreamStride,     // that stream's stride is 0
  kGapIndexBuffer,      // an indexed draw with no index buffer
  kGapVertexShader,
  kGapPixelShader,
  kGapViewport,
  kGapRenderState,      // one of the eight output-merger states never set
  kDrawGapCount,
};

const char* DrawGapName(uint32_t g) {
  switch (g) {
    case kGapDeclaration:  return "no declaration";
    case kGapLayout:       return "declaration does not decode";
    case kGapStream:       return "stream never set";
    case kGapStreamStride: return "stream stride is 0";
    case kGapIndexBuffer:  return "no index buffer";
    case kGapVertexShader: return "no vertex shader";
    case kGapPixelShader:  return "no pixel shader";
    case kGapViewport:     return "no viewport";
    case kGapRenderState:  return "render state never set";
    default:               return "?";
  }
}

uint64_t g_drawGaps[kDrawGapCount] = {};
uint64_t g_drawsComplete = 0;
uint64_t g_drawsChecked = 0;

// Stride disagreements between the declaration and SetStreamSource. The
// layout's own minimum stride cannot exceed the stride the game bound, or the
// last attribute reads past the end of each vertex.
uint64_t g_strideOk = 0;
uint64_t g_strideTooSmall = 0;
uint64_t g_strideMismatch = 0;   // bound stride larger than the layout needs
constexpr int kMaxStrideReports = 8;
int g_strideTooSmallNamed = 0;
int g_vbTooSmallNamed = 0;

// Does the buffer actually hold the vertices the draw asks for? The fetch
// constant's size and the draw's vertex count come from opposite ends of the
// API, so agreeing is evidence that both were read correctly — and this is the
// number the PM4 path had to *infer* the stride from.
uint64_t g_vbFits = 0;
uint64_t g_vbTooSmall = 0;
uint64_t g_ibFits = 0;
uint64_t g_ibTooSmall = 0;

//---------------------------------------------------------------------------
// Stage 0 — why does the vertex range check fail?
//
// 20,125 of 210,799 stream-checks pass, while every one of 66,726 index buffers
// holds its range. Two candidate causes, and this probe separates them.
//
// (a) A second binding path. `?SetStreamSource@D3DDevice@@QAAJIPAUD3DVertexBuffer@@II@Z`
//     exists in blocks.obj beside the `D3DDevice_SetStreamSource` hooked here,
//     as do state-block variants of SetTexture/SetRenderState/SetVertexDeclaration
//     plus D3DStateBlock_Apply. Binds arriving that way never reach the hook and
//     the shadow keeps an older bind. `draws since the last bind` measures it.
//
// (b) Streams are not indexed by a common vertex index. Xenos vertex shaders
//     issue their own vfetch, so a four-entry stream read by something like
//     `index % 4` is legal — and would make the check, not the game, wrong.
//     A per-stream split shows whether the failures are confined to small
//     auxiliary streams.
//
// The device holds the answer either way, because D3D9 writes the bound fetch
// constant into a file on the device. **The offset was meant to be read out of
// SetStreamSource's arithmetic, as `0x2ED8` was.** That failed: the unlinked
// object decodes to `device + StreamNumber*8` for dword0, which collides with
// the lazy-state qword at `+0x10` that SetVertexDeclaration provably uses, so a
// register is being misread and the result must not be built on.
//
// Located empirically instead, by a method that carries its own proof: at
// SetStreamSource we know the exact dwords, so every device offset holding one
// of them is a candidate, and intersecting the candidate sets across many
// different binds leaves only offsets that track the binding. Comparison only —
// no value read out of the device is ever dereferenced.
//---------------------------------------------------------------------------

// **The scan asks the OS whether a page is readable instead of guessing where
// the struct ends.** Two guesses were tried and both faulted at guest
// 0x1D00B000: first 0x4000, then 0x3484 — the latter chosen because
// SetStreamSource writes +0x3480, which proves that offset is mapped for *some*
// device and proves nothing about this one. The arena is sparse; a bound picked
// from a different object is not a bound.
//
// VirtualQuery per 4 KiB page costs one call per page per sample and removes
// the question entirely. The scan stops at the first page that is not
// committed and readable, so it reads exactly as far as memory exists.
constexpr uint32_t kDeviceScanBytes = 0x4000;
constexpr uint32_t kDeviceScanDwords = kDeviceScanBytes / 4;
constexpr uint32_t kHostPageSize = 4096;

bool HostPageReadable(const void* p) {
  MEMORY_BASIC_INFORMATION mbi = {};
  if (VirtualQuery(p, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
  if (mbi.State != MEM_COMMIT) return false;
  constexpr DWORD kNoRead = PAGE_NOACCESS | PAGE_GUARD;
  return mbi.Protect != 0 && (mbi.Protect & kNoRead) == 0;
}

// One bit per dword offset. Starts all-set and is intersected; anything still
// set after many samples held the just-bound value every single time.
bool g_fcCand0[kDeviceScanDwords];
bool g_fcCand1[kDeviceScanDwords];
bool g_fcPrimed = false;
uint32_t g_fcSamples = 0;
uint32_t g_fcReached = kDeviceScanBytes;   // lowest end-of-readable across samples
constexpr uint32_t kFcMaxSamples = 64;

// What the last SetStreamSource for stream 0 bound, kept raw so the scan can
// try the maskings D3D9 might apply rather than assuming one.
uint32_t g_lastBindD0 = 0;
uint32_t g_lastBindD1 = 0;
uint32_t g_lastBindOffset = 0;
bool g_haveBind = false;

// Draws since the last SetStreamSource that touched each stream. If binds are
// arriving through a path this file does not hook, the failing draws sit at
// large values here while the passing ones sit near zero.
uint64_t g_drawsSinceBind[mx::pm4::kMaxStreams] = {};
uint64_t g_bindAgeFitSum[mx::pm4::kMaxStreams] = {};
uint64_t g_bindAgeFailSum[mx::pm4::kMaxStreams] = {};
uint64_t g_bindAgeFailMax[mx::pm4::kMaxStreams] = {};

// The vertex range check, split by stream. A bare total cannot distinguish
// "stream 0 geometry is wrong" from "small auxiliary streams are modelled
// wrong", and those need completely different fixes.
uint64_t g_vbFitStream[mx::pm4::kMaxStreams] = {};

// Does the device's own fetch constant match what SetStreamSource recorded,
// and where it does not, does the device's value explain a draw the snapshot
// could not? `rescues` is the number that matters: it is how much of the
// shortfall reading the device would recover.
uint64_t g_fileAgree[mx::pm4::kMaxStreams] = {};
uint64_t g_fileDiffer[mx::pm4::kMaxStreams] = {};
uint64_t g_fileRescues[mx::pm4::kMaxStreams] = {};
uint64_t g_vbFailStream[mx::pm4::kMaxStreams] = {};

// Indexed draws were never range-checked on the vertex side, because the range
// depends on the index values. Reading them looked safe — the index buffer
// "holds its range" 66,726/66,726 — and it is **off**, because it faults.
//
// Three runs took an access violation at guest 0x1D00B000, and a VirtualQuery
// guard on the device scan did not stop it, which is what identified this read
// rather than that one as the source.
//
// The reason is almost certainly the address decode: SetIndices records
// `address = REX_LOAD_U32(buffer + 0x18) & 0x1FFFFFFF`, and that mask is the
// same one already found wrong for vertex buffers — it clears the top three
// bits rather than the bottom two, so it silently relocates any buffer whose
// address has them set. The 66,726/66,726 result does not contradict this: it
// compares a count against a size and never dereferences the address, so a
// wrong address passes it every time.
//
// Left in place behind this flag rather than deleted: the check is worth having
// once the decode is read out of D3DDevice_SetIndices the way the vertex side
// was. It is not needed for the question Stage 0 is actually asking, because
// only non-indexed draws were ever in the 20,125/210,799 denominator.
constexpr bool kProbeIndexRange = false;
uint64_t g_idxRangeFits = 0;
uint64_t g_idxRangeFails = 0;
uint64_t g_idxRangeUnread = 0;

// Scan the device for dwords matching what was last bound, intersecting into
// the candidate sets. Read-only, bounded, and every read is inside a struct
// D3D9 is itself using on both sides of this hook.
void SampleFetchConstantFile(uint32_t device, uint8_t* base) {
  if (!device || !g_haveBind || g_fcSamples >= kFcMaxSamples) return;

  // The maskings D3D9 might have applied on the way in. Trying several is the
  // point: it avoids assuming which one, and an offset only survives if it
  // matched on every sample.
  const uint32_t want0[4] = {g_lastBindD0, g_lastBindD0 & 0x1FFFFFFFu,
                             g_lastBindD0 + g_lastBindOffset,
                             (g_lastBindD0 + g_lastBindOffset) & 0x1FFFFFFFu};
  const uint32_t want1[2] = {g_lastBindD1, g_lastBindD1 - g_lastBindOffset};

  // How far the scan actually got, so the report can say whether a null result
  // means "not there" or "the struct ended before we looked".
  uint32_t reached = 0;
  for (uint32_t i = 0; i < kDeviceScanDwords; ++i) {
    const uint32_t off = i * 4;
    if ((off & (kHostPageSize - 1)) == 0 || i == 0) {
      if (!HostPageReadable(REX_RAW_ADDR(device + off))) break;
    }
    reached = off + 4;
    const uint32_t v = REX_LOAD_U32(device + off);
    bool hit0 = false;
    for (uint32_t k = 0; k < 4; ++k) hit0 = hit0 || v == want0[k];
    bool hit1 = false;
    for (uint32_t k = 0; k < 2; ++k) hit1 = hit1 || v == want1[k];
    if (!g_fcPrimed) {
      g_fcCand0[i] = hit0;
      g_fcCand1[i] = hit1;
    } else {
      g_fcCand0[i] = g_fcCand0[i] && hit0;
      g_fcCand1[i] = g_fcCand1[i] && hit1;
    }
  }
  // Anything past where this sample could read is not a candidate — leaving it
  // set would let an offset survive on samples that never actually checked it.
  for (uint32_t i = reached / 4; i < kDeviceScanDwords; ++i) {
    g_fcCand0[i] = false;
    g_fcCand1[i] = false;
  }
  if (reached < g_fcReached) g_fcReached = reached;

  g_fcPrimed = true;
  ++g_fcSamples;

  // The scan pinned dword1 to exactly one offset, 0x77C, and that retro-fits
  // SetStreamSource's own arithmetic: `subfic r11, r4, 0x11` — which a first
  // reading dismissed as dead — gives (0x11 - stream) * 8 + 0x6F4 = 0x77C for
  // stream 0. Two independent methods agreeing is what makes this an offset
  // rather than a coincidence.
  //
  // dword0 had no survivor because D3D9 ORs a flag bit in after masking
  // (`rlwinm r11, r11, 0, 19, 19` then `add`), which none of the candidate
  // forms included. Dumping the neighbourhood settles the pair by inspection
  // instead of by another round of guessing at the masking.
  if (g_fcSamples <= 8) {
    auto& f = DeclFile();
    f << "FETCH FILE sample " << g_fcSamples << ": last bind d0=0x" << std::hex
      << g_lastBindD0 << " d1=0x" << g_lastBindD1 << " offset=0x"
      << g_lastBindOffset << "\n           device+0x760..0x790:";
    for (uint32_t o = 0x760; o <= 0x790; o += 4) {
      f << " [" << o << "]=0x" << REX_LOAD_U32(device + o);
    }
    f << std::dec << "\n";
    f.flush();
  }
}

// Where SetStreamSource puts each stream's fetch constant, from the scan above.
// Only dword1 is confirmed; the dump names dword0's slot.
uint32_t FetchFileDword1Offset(uint32_t stream) {
  return 0x6F4 + (0x11 - stream) * 8;
}

//---------------------------------------------------------------------------
// Stage 3 — the vertex shader float constant file.
//
// Read out of D3DDevice_SetVertexShaderConstantFN's own arithmetic
// (shader.obj, and 0x82550320 in the XEX), which is four instructions long
// before it starts storing:
//
//     addi   r10, r4, 0x78          ; StartRegister + 0x78
//     rlwinm r10, r10, 4, 0, 27     ; * 16 — one vec4 per register
//     add    r10, r10, r3           ; + the device
//
// so register N lives at `device + 0x780 + N * 16`. The pixel-shader twin at
// 0x825503F8 is the same function with 0x178 in place of 0x78, giving 0x1780 —
// two 256-register files, 0x1000 bytes each, and they land exactly between the
// vertex fetch constants (which end at 0x780) and the declaration at 0x2ED8.
// Three independently-derived offsets tiling the struct with no overlap is what
// makes this a layout rather than three lucky guesses.
//
// **Not hooked, deliberately.** The device holds the live value whichever path
// wrote it — including the state-block path in blocks.obj that bypasses every
// hook in this file. That is the third time reading the field has beaten
// hooking the setter, after the declaration and the fetch constants.
//---------------------------------------------------------------------------
constexpr uint32_t kDeviceVsConstFile = 0x780;

// Reads kHleProbeRegs vec4s into host order. Bounded by the same page guard the
// device scan uses, and entirely inside a struct D3D9 is using on both sides of
// this hook.
bool ReadVsConstants(uint32_t device, uint8_t* base,
                     float out[mx::pm4::kHleProbeRegs * 4]) {
  (void)base;
  if (!device) return false;
  const uint32_t bytes = mx::pm4::kHleProbeRegs * 16;
  if (!HostPageReadable(REX_RAW_ADDR(device + kDeviceVsConstFile)) ||
      !HostPageReadable(REX_RAW_ADDR(device + kDeviceVsConstFile + bytes - 4)))
    return false;
  for (uint32_t i = 0; i < mx::pm4::kHleProbeRegs * 4; ++i) {
    const uint32_t bits =
        REX_LOAD_U32(device + kDeviceVsConstFile + i * 4);
    std::memcpy(&out[i], &bits, 4);
  }
  return true;
}

//---------------------------------------------------------------------------
// The live viewport, off the device.
//
// `D3DDevice_SetViewport` (0x8254BF50) forwards to sub_8254BCE8, which stores
// six floats and, crucially, **clamps Width and Height against the render
// target** first — the surface extent it reads from `0x24(r9)` bounds
// `X + Width` and `Y + Height` before the store:
//
//   stfs f31, 0x3218(r31)   X
//   stfs f30, 0x321C(r31)   Y
//   stfs f26, 0x3220(r31)   Width    (clamped)
//   stfs f27, 0x3224(r31)   Height   (clamped)
//   stfs f29, 0x3228(r31)   MinZ
//   stfs f28, 0x322C(r31)   MaxZ
//
// That clamp is the whole fix. The argument shadow recorded `65535x65535` on
// 9,130 of ~15,500 calls — a full-surface reset — and last-write-wins meant
// most draws inherited it, so BuildViewportMvp divided by 32767 and collapsed
// every position toward the origin. The device holds what D3D9 actually uses.
//
// Sixth time reading the field has beaten shadowing the call. Same reason each
// time: the device holds the resolved value, whatever path produced it.
//---------------------------------------------------------------------------
constexpr uint32_t kDeviceViewport = 0x3218;

uint64_t g_vpFromDevice = 0, g_vpFromShadow = 0, g_vpDisagreed = 0;

bool ReadDeviceViewport(uint32_t device, uint8_t* base, float out[6]) {
  if (!device) return false;
  if (!HostPageReadable(REX_RAW_ADDR(device + kDeviceViewport)) ||
      !HostPageReadable(REX_RAW_ADDR(device + kDeviceViewport + 20)))
    return false;
  for (uint32_t i = 0; i < 6; ++i) {
    const uint32_t bits = REX_LOAD_U32(device + kDeviceViewport + i * 4);
    std::memcpy(&out[i], &bits, 4);
  }
  // Width and height are the only two this is used for; a zero or non-finite
  // extent is not a viewport and must not become a divide.
  for (uint32_t i = 0; i < 6; ++i)
    if (!std::isfinite(out[i])) return false;
  return out[2] > 0.0f && out[3] > 0.0f;
}

// The transform the PM4 path applies today, built from the D3D9 viewport
// instead of from the Xenos context registers. It maps window coordinates to
// clip space.
//
// D3D9's own scale/offset: xs = width/2, xo = x + width/2, and y is flipped.
//
// Prefers the device's clamped copy and falls back to the argument shadow only
// when the device cannot be read, counting which was used — a silent fallback
// to the value that caused the bug would be the worst of both.
bool BuildViewportMvp(uint32_t device, uint8_t* base, float out[16]) {
  float dv[6];
  float vx, vy, vw, vh, vminz, vmaxz;
  if (ReadDeviceViewport(device, base, dv)) {
    vx = dv[0]; vy = dv[1]; vw = dv[2]; vh = dv[3];
    vminz = dv[4]; vmaxz = dv[5];
    ++g_vpFromDevice;
    const auto& s = DeviceState().viewport;
    if (s.seen && (float(s.width) != vw || float(s.height) != vh))
      ++g_vpDisagreed;
  } else {
    const auto& v = DeviceState().viewport;
    if (!v.seen || v.width == 0 || v.height == 0) return false;
    vx = float(v.x); vy = float(v.y);
    vw = float(v.width); vh = float(v.height);
    vminz = v.min_z; vmaxz = v.max_z;
    ++g_vpFromShadow;
  }

  const float xs = vw * 0.5f;
  const float xo = vx + xs;
  const float ys = -vh * 0.5f;
  const float yo = vy + vh * 0.5f;
  float zs = vmaxz - vminz;
  const float zo = vminz;
  if (zs == 0.0f) zs = 1.0f;

  static const float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0,
                                      0, 0, 1, 0, 0, 0, 0, 1};
  std::memcpy(out, kIdentity, sizeof(kIdentity));
  out[0]  = 1.0f / xs;  out[3]  = -xo / xs;
  out[5]  = 1.0f / ys;  out[7]  = -yo / ys;
  out[10] = 1.0f / zs;  out[11] = -zo / zs;
  return true;
}

//---------------------------------------------------------------------------
// Stage 2 — build a renderable draw from the description.
//
// The hook owns guest access, so it resolves each buffer to a host pointer and
// hands plain pointers to d3d9_draw.cpp, which stays free of the recompiler
// macros. Every range is bounded by the size D3D9 itself recorded on the
// object.
//---------------------------------------------------------------------------
uint64_t g_badPrimType[64] = {};

// (width << 32) | height -> how many SetViewport calls used it.
std::map<uint64_t, uint64_t> g_viewportExtents;

// Stage C — run the guest's own vertex shader over this draw's vertices.
// Defined further down, next to the microcode it needs; declared here because
// the draw builder is the only place that has the vertices and the streams
// together.
void ProbeShaderExecution(const mx::pm4::DrawCall& dc, uint32_t handle,
                          const mx::pm4::HleStream* streams, uint32_t device,
                          uint8_t* base);

void BuildAndQueueDraw(bool indexed, uint32_t prim_type, uint32_t first,
                       uint32_t count, int32_t base_vertex, uint32_t device,
                       uint8_t* base) {
  using namespace mx::pm4;
  const auto& st = DeviceState();

  HleDrawInputs in;
  in.indexed = indexed;
  in.prim_type = prim_type;
  in.first = first;
  in.count = count;
  in.base_vertex = base_vertex;

  const int id = g_currentDecl;
  if (id >= 0 && g_declLayoutOk[id]) in.layout = &g_declLayout[id];

  HleStream streams[kMaxStreams];
  for (uint32_t i = 0; i < kMaxStreams; ++i) {
    const auto& b = st.stream[i];
    if (!b.bound || !b.address || !b.size_bytes) continue;
    streams[i].host = reinterpret_cast<const uint8_t*>(REX_RAW_ADDR(b.address));
    streams[i].size_bytes = b.size_bytes;
    streams[i].stride = b.stride;
    streams[i].offset_bytes = b.offset_bytes;
    streams[i].endian = b.endian;
    streams[i].bound = true;
  }
  in.streams = streams;

  if (indexed && st.index.bound && st.index.address && st.index.size_bytes) {
    in.index.host =
        reinterpret_cast<const uint8_t*>(REX_RAW_ADDR(st.index.address));
    in.index.size_bytes = st.index.size_bytes;
    in.index.is_32bit = st.index.is_32bit;
    in.index.bound = true;
  }

  // The transform the draw is *rendered* with, this stage, is the viewport
  // inverse — the same one the PM4 path uses. That is not a claim that it is
  // right; it is the only transform with evidence behind it today, and it makes
  // the HLE picture directly comparable to the PM4 one on screen. What the
  // constant file says is measured beside it, below, and acted on afterwards.
  float vp[16];
  const bool have_vp = BuildViewportMvp(device, base, vp);
  if (have_vp) in.mvp = vp;

  DrawCall dc;
  HleSkip skip = HleSkip::kNone;
  if (!BuildHleDraw(in, dc, skip)) {
    ++HleSkipCounts()[uint32_t(skip)];
    // Which primitive types are being refused, rather than how many. The bare
    // count says 62% of draws fail and nothing about whether that is one type
    // needing expansion or a wrong prim-type argument.
    if (skip == HleSkip::kBadTopology && prim_type < 64) {
      ++g_badPrimType[prim_type];
    }
    return;
  }
  ++HleBuiltCount();

  // Stage 3's measurement, on the built positions rather than on raw bytes: the
  // vertices are already decoded and in host order here, so the probe scores the
  // same numbers the renderer would receive.
  {
    static float consts[kHleProbeRegs * 4];
    if (ReadVsConstants(device, base, consts)) {
      ScoreHleTransform(dc, consts, have_vp ? vp : nullptr,
                        st.vs_seen ? st.vertex_shader : 0);
      // The first few register files in full. A ranking with no numbers behind
      // it cannot be checked by eye, and "c3 col-major, 94%" is worth much less
      // than seeing that c3..c6 look like a projection matrix.
      static uint32_t s_dumped = 0;
      if (s_dumped < 4) {
        ++s_dumped;
        auto& f = DeclFile();
        f << "VS CONSTANTS (draw " << g_draws << ", device+0x780):\n";
        for (uint32_t r = 0; r < 12; ++r) {
          f << "    c" << r << " = " << consts[r * 4 + 0] << " "
            << consts[r * 4 + 1] << " " << consts[r * 4 + 2] << " "
            << consts[r * 4 + 3] << "\n";
        }
        f << "    first host position = " << *(const float*)dc.vertices.data()
          << " " << *((const float*)dc.vertices.data() + 1) << " "
          << *((const float*)dc.vertices.data() + 2) << "\n";
        f.flush();
      }
    }
  }

  ProbeShaderExecution(dc, st.vs_seen ? st.vertex_shader : 0, streams, device,
                       base);

  HleFrameDraws().push_back(std::move(dc));
}

// Records every gap this draw has, rather than stopping at the first, so the
// report says which fields are actually missing across the population instead
// of which one happens to be checked earliest.
void ScoreDraw(bool indexed, uint32_t first, uint32_t count,
               uint32_t device, uint8_t* base) {
  const auto& st = DeviceState();
  ++g_drawsChecked;
  for (uint32_t s = 0; s < mx::pm4::kMaxStreams; ++s) ++g_drawsSinceBind[s];
  bool complete = true;
  auto gap = [&](uint32_t g) { ++g_drawGaps[g]; complete = false; };

  const int id = g_currentDecl;
  if (id < 0) {
    gap(kGapDeclaration);
  } else if (!g_declLayoutOk[id]) {
    gap(kGapLayout);
  } else {
    const auto& layout = g_declLayout[id];
    // Only the streams this layout actually reads from matter. A declaration
    // using stream 0 alone says nothing about stream 1 being unset.
    bool used[mx::pm4::kMaxStreams] = {};
    for (const auto& e : layout.elements) used[e.stream] = true;
    for (uint32_t s = 0; s < mx::pm4::kMaxStreams; ++s) {
      if (!used[s]) continue;
      const auto& b = st.stream[s];
      if (!b.seen || !b.bound) {
        gap(kGapStream);
      } else if (b.stride == 0) {
        gap(kGapStreamStride);
      } else if (b.stride < layout.min_stride[s]) {
        // The layout needs more bytes per vertex than the game bound: the last
        // attribute would read past the end of each vertex. Either the decode
        // is wrong or the stream was bound for a different declaration than the
        // one in force. Counted separately, and the first few are named — a
        // bare count would say nothing about which of the two it is.
        ++g_strideTooSmall;
        complete = false;
        if (g_strideTooSmallNamed < kMaxStrideReports) {
          ++g_strideTooSmallNamed;
          auto& f = DeclFile();
          f << "STRIDE TOO SMALL: declaration id " << id << " stream " << s
            << " needs " << layout.min_stride[s] << " bytes, bound stride is "
            << b.stride << " (vb addr=0x" << std::hex << b.address << std::dec
            << " size=" << b.size_bytes << ")\n";
          for (const auto& e : layout.elements) {
            if (e.stream != s) continue;
            f << "    " << e.semantic_name << e.semantic_index
              << " off=" << e.offset << " size=" << e.size_bytes << "\n";
          }
          f.flush();
        }
      } else if (b.stride != layout.min_stride[s]) {
        ++g_strideMismatch;   // padding at the end of the vertex; legal
      } else {
        ++g_strideOk;
      }

      // For a non-indexed draw the vertex range is known exactly. For an
      // indexed one it depends on the index values — which are readable, since
      // the index buffer provably holds its own range, so the real highest
      // index is used rather than skipping the check.
      uint32_t hi_vertex = 0;
      bool have_range = false;
      if (!indexed) {
        hi_vertex = first + count;
        have_range = true;
      } else if (kProbeIndexRange && st.index.bound && st.index.address &&
                 !st.index.is_32bit) {
        // Bounded by the index buffer's own size, which the previous round
        // verified holds for every indexed draw.
        const uint64_t end = static_cast<uint64_t>(first + count) * 2;
        if (end <= st.index.size_bytes) {
          uint32_t hi = 0;
          for (uint32_t i = 0; i < count; ++i) {
            const uint32_t v = REX_LOAD_U16(st.index.address + (first + i) * 2);
            if (v > hi) hi = v;
          }
          hi_vertex = hi + 1;
          have_range = true;
        }
      }

      if (indexed && !have_range) ++g_idxRangeUnread;

      // **The decisive comparison.** If binds are reaching the device through a
      // path this file does not hook, the device's own fetch constant will
      // differ from the snapshot SetStreamSource recorded — and the size is the
      // field the range check actually depends on.
      {
        const uint32_t d1 = REX_LOAD_U32(device + FetchFileDword1Offset(s));
        const uint32_t live = ((d1 >> 2) & 0xFFFFFF) * 4;
        if (live == b.size_bytes) {
          ++g_fileAgree[s];
        } else {
          ++g_fileDiffer[s];
          // Does the device's size explain a draw the snapshot could not?
          if (b.stride && static_cast<uint64_t>(hi_vertex) * b.stride <= live) {
            ++g_fileRescues[s];
          }
        }
      }

      if (have_range && b.stride) {
        const uint64_t need = static_cast<uint64_t>(hi_vertex) * b.stride;
        const bool fits = need <= b.size_bytes;
        if (indexed) {
          (fits ? g_idxRangeFits : g_idxRangeFails) += 1;
        }
        // Bind age, split the same way: a stale shadow shows up as failing
        // draws sitting far from their last bind while passing ones sit near it.
        const uint64_t age = g_drawsSinceBind[s];
        if (fits) {
          g_bindAgeFitSum[s] += age;
        } else {
          g_bindAgeFailSum[s] += age;
          if (age > g_bindAgeFailMax[s]) g_bindAgeFailMax[s] = age;
        }
        (fits ? g_vbFitStream[s] : g_vbFailStream[s]) += 1;

        if (!indexed && fits) {
          ++g_vbFits;
        } else if (!indexed) {
          ++g_vbTooSmall;
          if (g_vbTooSmallNamed < kMaxStrideReports) {
            ++g_vbTooSmallNamed;
            auto& f = DeclFile();
            f << "VB DOES NOT HOLD RANGE: declaration id " << id << " stream "
              << s << " start_vertex=" << first << " count=" << count
              << " stride=" << b.stride << " needs " << need << "B, buffer is "
              << b.size_bytes << "B (addr=0x" << std::hex << b.address
              << std::dec << " offset=" << b.offset_bytes
              << " endian=" << b.endian << ")\n";
            f.flush();
          }
        }
      }
    }
  }

  if (indexed) {
    if (!st.index.seen || !st.index.bound) {
      gap(kGapIndexBuffer);
    } else {
      const uint64_t need =
          static_cast<uint64_t>(first + count) * (st.index.is_32bit ? 4 : 2);
      (need <= st.index.size_bytes ? g_ibFits : g_ibTooSmall) += 1;
    }
  }
  if (!st.vs_seen) gap(kGapVertexShader);
  if (!st.ps_seen) gap(kGapPixelShader);
  if (!st.viewport.seen) gap(kGapViewport);

  // BlendFactor has zero call sites in this title, so requiring it would mark
  // every draw incomplete for a state the game never uses. The other seven are
  // required.
  for (uint32_t r = 0; r < mx::pm4::kRenderStateCount; ++r) {
    if (r == mx::pm4::kRsBlendFactor) continue;
    if (!st.render_state.Seen(r)) { gap(kGapRenderState); break; }
  }

  if (complete) ++g_drawsComplete;
}

//---------------------------------------------------------------------------
// Stage A — locate the microcode *inside* the blob, by comparison.
//
// `DecodeVertexShaderFetches` wants an array that starts at the control-flow
// section ("the blob carries no header saying so", shader_ucode.cpp:396), and
// no UCODE header parser exists anywhere — not in this tree, and not in the SDK
// at rex/graphics/format/ucode.h. So the code's offset within the blob has to
// be established before anything can be decoded.
//
// **Not by guessing which header dword is an offset.** The ring already carried
// this exact microcode and CapturedShaders() holds it, so the offset is found
// by searching the blob for what PM4 decoded. Both sides are host-endian —
// REX_LOAD_U32 byteswaps on the way in, and the 0x2B path is host-endian
// already (pm4_translator.cpp:622) — so the dwords compare directly.
//
// A match also settles which variant is bound, for free: GetUCode(i) says the
// object holds several patched microcodes, and the ring carried the one the
// hardware actually ran.
//
// **Blobs are collected at bind time and compared at report time.** A first
// version compared on the spot and reported 0 of 48 matching — against "0
// captured shaders" for the first blob and "3" for the next few, because the
// game binds a shader long before the ring loads it and each handle was only
// searched once. That comparison ran against an almost-empty captured set and
// said nothing about the blob. Same class of mistake as the transform report
// eating its own counters: the instrument, not the data.
//---------------------------------------------------------------------------
constexpr uint32_t kVsBlobOffset = 0x368;   // CreateVertexShader copies here
constexpr uint32_t kVsBlobSizeAt = 0x36C;
constexpr uint32_t kMaxBlobDwords = 4096;   // 16 KB ceiling on one blob

std::map<uint32_t, std::vector<uint32_t>> g_vsBlobs;
uint64_t g_vsBlobUnreadable = 0;

// SH_pPhysical per handle, for the address-key test below.
std::map<uint32_t, uint32_t> g_vsPhys;

void CollectVertexShaderBlob(uint32_t handle, uint8_t* base) {
  (void)base;
  if (!handle || g_vsBlobs.count(handle)) return;
  if (!HostPageReadable(REX_RAW_ADDR(handle + kVsBlobOffset)) ||
      !HostPageReadable(REX_RAW_ADDR(handle + kVsBlobSizeAt))) {
    ++g_vsBlobUnreadable;
    return;
  }
  const uint32_t size_bytes = REX_LOAD_U32(handle + kVsBlobSizeAt);
  if (size_bytes == 0 || size_bytes > kMaxBlobDwords * 4) {
    ++g_vsBlobUnreadable;
    return;
  }
  const uint32_t n = size_bytes / 4;

  std::vector<uint32_t> blob(n);
  for (uint32_t i = 0; i < n; ++i) {
    const uint32_t off = kVsBlobOffset + i * 4;
    if ((off & (kHostPageSize - 1)) == 0 &&
        !HostPageReadable(REX_RAW_ADDR(handle + off))) {
      blob.resize(i);
      break;
    }
    blob[i] = REX_LOAD_U32(handle + off);
  }
  g_vsBlobs.emplace(handle, std::move(blob));
  if (HostPageReadable(REX_RAW_ADDR(handle + 0x20)))
    g_vsPhys.emplace(handle, REX_LOAD_U32(handle + 0x20));
}

//---------------------------------------------------------------------------
// Does the handle already name its own ring key?
//
// HandleImLoad (0x27) keys the shader cache by guest physical address,
// `pkt.body[0] & ~3` (pm4_translator.cpp:638). SH_pPhysical is
// `*(handle + 0x20)`. If D3D9 issues IM_LOAD for the shader it binds, then the
// handle *is* the key and there is nothing to extract from the blob at all.
//
// Three forms are tried because D3D9 masks addresses on the way to the GPU and
// the raw field is the unmasked one — the distinction that cost four access
// violations. Three forms is not a fishing expedition: they are the raw value,
// the physical mask D3D9 itself applies (`rlwinm r11, r11, 0, 3, 31`), and the
// dword alignment HandleImLoad applies on top.
//
// Note the other door: IM_LOAD_IMMEDIATE (0x2B) carries no address, so its keys
// are content hashes and no address can ever match them. How many keys are of
// each kind is therefore part of the answer, not background — a miss means
// nothing if every key in the cache is a hash.
//---------------------------------------------------------------------------
void ReportAddressKeyTest(uint8_t* base) {
  const auto& captured = mx::pm4::CapturedShaders();
  if (captured.empty() || g_vsPhys.empty()) {
    REXLOG_INFO("d3d9: stageA  address-key test: nothing to compare ({} keys, "
                "{} handles)", captured.size(), g_vsPhys.size());
    return;
  }

  // A key that fits in 32 bits and lands in the guest's address range is an
  // address; anything else is a content hash. Counting them says whether the
  // test could have succeeded at all.
  uint32_t addr_keys = 0, hash_keys = 0;
  for (const auto& [key, cs] : captured) {
    (void)cs;
    if (key <= 0xFFFFFFFFull) ++addr_keys; else ++hash_keys;
  }

  uint32_t hit_raw = 0, hit_phys = 0, hit_aligned = 0;
  auto& f = DeclFile();
  uint32_t named = 0;
  for (const auto& [handle, phys] : g_vsPhys) {
    const uint64_t raw = phys;
    const uint64_t masked = phys & 0x1FFFFFFFu;
    const uint64_t aligned = masked & ~3u;
    const bool a = captured.count(raw) != 0;
    const bool b = captured.count(masked) != 0;
    const bool c = captured.count(aligned) != 0;
    hit_raw += a; hit_phys += b; hit_aligned += c;
    if ((a || b || c) && named < 8) {
      ++named;
      f << "ADDRESS KEY HIT: shader 0x" << std::hex << handle
        << " SH_pPhysical=0x" << phys << " matched as"
        << (a ? " raw" : "") << (b ? " physical" : "")
        << (c ? " physical-aligned" : "") << std::dec << "\n";
    }
  }
  f.flush();

  REXLOG_INFO(
      "d3d9: stageA  address-key test over {} handles vs {} ring keys ({} look "
      "like addresses, {} are content hashes): raw {} physical {} "
      "physical-aligned {}",
      g_vsPhys.size(), captured.size(), addr_keys, hash_keys, hit_raw, hit_phys,
      hit_aligned);
  // The exact test missed but most keys are addresses, so the two are probably
  // the same region at a fixed offset — GetPhysicalMicrocode's shape is
  // `*(variant + 0x368) + SH_pPhysical`, i.e. base plus an offset out of the
  // header. Reporting the nearest key per handle turns "no" into a distance,
  // and a delta that repeats is the offset.
  if (addr_keys) {
    std::map<int64_t, uint32_t> deltas;
    for (const auto& [handle, phys] : g_vsPhys) {
      (void)handle;
      const int64_t p = int64_t(phys & 0x1FFFFFFFu);
      int64_t best = 0;
      bool have = false;
      for (const auto& [key, cs] : captured) {
        (void)cs;
        if (key > 0xFFFFFFFFull) continue;
        const int64_t d = int64_t(key) - p;
        if (!have || (d < 0 ? -d : d) < (best < 0 ? -best : best)) {
          best = d;
          have = true;
        }
      }
      if (have) ++deltas[best];
    }
    std::string top;
    uint32_t shown = 0;
    for (const auto& [d, n] : deltas) {
      if (n < 2 || shown >= 8) continue;   // a delta seen once is a coincidence
      ++shown;
      char buf[48];
      std::snprintf(buf, sizeof(buf), "%+lld:%u ", (long long)d, n);
      top += buf;
    }
    REXLOG_INFO(
        "d3d9: stageA  nearest ring key to each SH_pPhysical — {} distinct "
        "deltas over {} handles; repeated ones: {}",
        deltas.size(), g_vsPhys.size(), top.empty() ? "none" : top);

    // **Confirm by content, not by arithmetic.** A nearest-neighbour delta can
    // repeat for reasons that have nothing to do with the shader — if handles
    // share a physical base, or if the allocator spaces every shader a page
    // apart, the same delta falls out of the geometry alone. So take the
    // candidate deltas the histogram named, read the guest at that address, and
    // compare the dwords against the microcode the ring loaded under that key.
    // Matching bytes are the claim; a matching subtraction is not.
    static const int64_t kCandidateDeltas[] = {0x1040, 0x1000, -0xFC0, -0x1000};
    for (int64_t d : kCandidateDeltas) {
      uint32_t tried = 0, hit = 0, unreadable = 0;
      for (const auto& [handle, phys] : g_vsPhys) {
        (void)handle;
        const uint64_t key = uint64_t(int64_t(phys & 0x1FFFFFFFu) + d);
        auto it = captured.find(key);
        if (it == captured.end() || it->second.code.empty()) continue;
        ++tried;
        // Read at the *unmasked* address: the guest's virtual space is where
        // this side reads, which is the distinction that cost four AVs.
        const uint32_t at = uint32_t(int64_t(phys) + d);
        const uint32_t bytes = uint32_t(it->second.code.size() * 4);
        if (!HostPageReadable(REX_RAW_ADDR(at)) ||
            !HostPageReadable(REX_RAW_ADDR(at + bytes - 4))) {
          ++unreadable;
          continue;
        }
        bool same = true;
        for (size_t i = 0; i < it->second.code.size() && same; ++i)
          same = REX_LOAD_U32(at + uint32_t(i * 4)) == it->second.code[i];
        hit += same ? 1 : 0;
      }
      if (tried) {
        REXLOG_INFO(
            "d3d9: stageA    delta {:+#x}: {} handles land on a ring key, {} of "
            "them read back that exact microcode ({} unreadable)",
            d, tried, hit, unreadable);
      }
    }
  }

  if (!hit_raw && !hit_phys && !hit_aligned && addr_keys == 0) {
    REXLOG_INFO(
        "d3d9: stageA  every ring key is a content hash — this title loads "
        "shaders through IM_LOAD_IMMEDIATE, which carries no address, so no "
        "address could have matched and the miss says nothing about the handle");
  }
}

// Route 1: SH_pPhysical read at *draw* time rather than bind time. It was
// sixteen zero dwords at bind; if D3D9 fills it lazily, the draw is when it
// would be filled. Sampled once per handle so the cost is bounded.
std::map<uint32_t, uint32_t> g_physNonzeroAtDraw;   // handle -> nonzero dwords
// The bytes themselves, so the "is this the microcode" question is answered by
// content and not by "it is no longer zero" — which a page of anything at all
// would satisfy.
constexpr uint32_t kPhysProbeDwords = 256;
std::map<uint32_t, std::vector<uint32_t>> g_physDumpAtDraw;

// handle -> {ring key, dword offset of the code in the dump, agreement %}.
struct BestMatch { uint64_t key; uint32_t off_dwords; uint32_t pct; };
std::map<uint32_t, BestMatch> g_bestKeyAtDraw;

void ProbePhysicalAtDrawTime(uint32_t handle, uint8_t* base) {
  (void)base;
  if (!handle || g_physNonzeroAtDraw.count(handle)) return;
  if (!HostPageReadable(REX_RAW_ADDR(handle + 0x20))) return;
  const uint32_t phys = REX_LOAD_U32(handle + 0x20);
  if (!phys || !HostPageReadable(REX_RAW_ADDR(phys)) ||
      !HostPageReadable(REX_RAW_ADDR(phys + 0x7C))) {
    g_physNonzeroAtDraw[handle] = 0;
    return;
  }
  uint32_t nonzero = 0;
  for (uint32_t i = 0; i < 32; ++i)
    if (REX_LOAD_U32(phys + i * 4) != 0) ++nonzero;
  g_physNonzeroAtDraw[handle] = nonzero;

  std::vector<uint32_t> dump;
  dump.reserve(kPhysProbeDwords);
  for (uint32_t i = 0; i < kPhysProbeDwords; ++i) {
    const uint32_t at = phys + i * 4;
    if ((at & (kHostPageSize - 1)) == 0 && !HostPageReadable(REX_RAW_ADDR(at)))
      break;
    dump.push_back(REX_LOAD_U32(at));
  }
  g_physDumpAtDraw.emplace(handle, std::move(dump));
}

//---------------------------------------------------------------------------
// Stage C — execute the shader and see where the position lands.
//
// Everything needed is now located: the microcode at SH_pPhysical + 0x40, the
// constants at device + 0x780, and an interpreter that is already validated.
// The number this exists to produce is the in-clip fraction from *running the
// guest's code*, against the 55% the best scored constant register managed and
// the 0% the viewport inverse did.
//
// **Two honest bridges, both temporary and both stated.**
//
// 1. The attributes come from PM4's decode of the same shader, not from the
//    declaration. The copy at +0x40 is the unpatched template — its format,
//    offset and stride are blank (Stage B) — and pairing declaration elements
//    to vfetch instructions is a rule this has not read out of
//    PatchVertexShaderToMatchVertexDeclaration yet. Guessing that pairing here
//    would put a second unknown inside the one measurement meant to settle the
//    first.
// 2. Attribute values are read from stream 0. PM4's fetch_slot is a Xenos
//    fetch constant index (95), not a D3D9 stream number, and that mapping is
//    also unread. Draws whose bound stride disagrees with the shader's are
//    skipped and counted rather than read anyway.
//
// Neither bridge affects what the measurement can conclude: if executing the
// shader puts positions in the clip volume, the microcode and the constants are
// right, because nothing else would produce that.
//---------------------------------------------------------------------------
//---------------------------------------------------------------------------
// Stage D2 — where the exported positions land, as a distribution.
//
// Stage C reported one number: 35% inside the clip volume. That number cannot
// be recorded as a result, because a single cutoff cannot tell "the transform
// is right and this geometry is off-screen" from "the transform is wrong by a
// factor of a thousand". Both are simply "not in clip".
//
// So bucket by how far outside it lands. A pile at 1-2 says the first; a pile
// past 100 says the second; an even spread across every bucket says it is not
// a transform at all. The buckets are on x and y only — z has its own near
// plane convention and folding it in would blur the one axis being read.
//
// The viewport inverse gets the identical treatment on the identical vertices.
// Without a reference the buckets are just numbers: it scored 0% under the
// Stage 3 threshold, so what it looks like as a *distribution* is what says
// whether the shader's output is different in kind or merely in degree.
//---------------------------------------------------------------------------
enum ClipBucket : uint32_t {
  kClipIn = 0,      // <= 1: inside, on x and y
  kClipJustOut,     // 1-2:   off-screen, but the same order of magnitude
  kClipOut,         // 2-10
  kClipFarOut,      // 10-100
  kClipWild,        // > 100: the scale is wrong, not the framing
  kClipBehind,      // w <= 0: behind the eye, no meaningful projection
  kClipNonFinite,   // inf/nan
  kClipBucketCount,
};

const char* const kClipBucketName[kClipBucketCount] = {
    "<=1", "1-2", "2-10", "10-100", ">100", "w<=0", "nonfinite"};

uint32_t ClassifyClip(const float p[4]) {
  if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2]) ||
      !std::isfinite(p[3]))
    return kClipNonFinite;
  if (p[3] <= 0.0f) return kClipBehind;
  const float x = std::fabs(p[0] / p[3]);
  const float y = std::fabs(p[1] / p[3]);
  const float d = x > y ? x : y;
  if (!std::isfinite(d)) return kClipNonFinite;
  if (d <= 1.0f)   return kClipIn;
  if (d <= 2.0f)   return kClipJustOut;
  if (d <= 10.0f)  return kClipOut;
  if (d <= 100.0f) return kClipFarOut;
  return kClipWild;
}

constexpr uint32_t kD3d9ConstRegs = 256;

uint64_t g_aluRuns = 0, g_aluInClip = 0, g_aluDegenerate = 0;
uint64_t g_aluNoShader = 0, g_aluNoAttrs = 0, g_aluStrideMismatch = 0;
// fetch_slot did not invert to a stream in [0, kMaxStreams). Its own counter:
// "the stream mapping does not hold here" and "the bound stride disagrees" are
// different failures and folding them together would hide either one.
uint64_t g_aluBadStream = 0;

//---------------------------------------------------------------------------
// Stage G — execute the shader that was actually bound.
//
// Draws are matched to microcode by >=90% content similarity against PM4's
// cache (g_bestKeyAtDraw). That is a heuristic on two counts: it can pick a
// near-identical wrong variant, and it fails outright on ~63% of draws, so
// every number so far comes from a 37% minority.
//
// The patch hook has the real thing. r4 is where D3D9 writes the patched
// microcode and r3 names the shader — an exact key, no similarity involved.
//
// **The window's start is checked, not assumed.** Vfetch triples land at
// dest + 12*index, so dest is the instruction section and the CF section
// precedes it; Stage A found that gap to be 0x40 bytes. Rather than trust
// that, the capture records the binding table's own vfetch count and the
// decode has to produce exactly that many attributes. A wrong window start
// decodes into plausible nonsense, and this makes that countable instead.
//---------------------------------------------------------------------------
// A first attempt assumed the CF section sat 0x40 bytes before dest, the gap
// Stage A found inside SH_pPhysical. It does not: every decode refused with
// "exec target at address 0", which is the self-check earning its place — a
// wrong start would otherwise have decoded into plausible nonsense.
//
// So the start is *searched* rather than assumed, and the search has a
// verifiable answer: the binding table says how many vfetches this shader has,
// and only the true CF start decodes to exactly that many. Resolved once per
// shader handle and reused, because the offset is a property of the layout.
constexpr uint32_t kPatchWindowBack = 128;   // dwords captured before dest

struct PatchedCode {
  std::vector<uint32_t> code;   // host-endian, from dest - kPatchWindowBack*4
  uint32_t expect_fetches = 0;  // what the binding table said
  uint32_t variant = 0;
  uint32_t code_off = 0;        // dwords into `code` where the CF section is
  bool     resolved = false;    // code_off was found by decoding, not assumed
};

// Winning start, as a signed dword distance from dest. The histogram is the
// finding: one value across every shader means a fixed layout.
std::map<int32_t, uint64_t> g_patchCodeOffsets;
std::map<uint32_t, PatchedCode> g_patchedCode;   // shader handle -> latest

uint64_t g_srcPatchHook = 0;    // draws whose code came from the patch hook
uint64_t g_srcHeuristic = 0;    // ... from the >=90% content match
uint64_t g_srcNone = 0;
uint64_t g_patchDecodeOk = 0;     // decoded, and the count matched the table
uint64_t g_patchDecodeCount = 0;  // decoded, count disagreed with the table
uint64_t g_patchDecodeFail = 0;   // refused outright
std::map<std::string, uint64_t> g_patchDecodeFailWhy;
// Independent third reading: our decode of D3D9's patched output against
// PM4's decode of the ring's copy. Same bytes by two routes.
uint64_t g_attrAgree = 0, g_attrDisagree = 0, g_attrNoPeer = 0;
uint64_t g_aluConstReads = 0, g_aluConstZero = 0;
std::map<int, uint64_t> g_aluStatus;
std::map<uint32_t, uint64_t> g_aluBlocking;

// Stage D — cost. Draws entered, not draws offered: the difference between the
// two is every named skip below, and a rate quoted against the wrong
// denominator is how "35% of draws" turns into a claim about the whole title.
uint64_t g_aluDrawsEntered = 0;
uint64_t g_aluNanos = 0;

// Stage F — which space the exported position is in.
//
// Everything before this assumed clip space, and never tested it. The PM4 path
// established on the *same shaders* that the ring's exports read like window
// coordinates, which is the entire reason the renderer applies the viewport
// inverse. If that holds here too then the clip-volume test has been the wrong
// yardstick and the numbers it produced measure the wrong thing.
uint64_t g_spaceCount[4] = {};        // indexed by ExportSpace
// The same histogram as the raw one, but after the viewport inverse. If these
// positions are window coordinates, this is where they collapse into <=1.
uint64_t g_clipExecVp[kClipBucketCount] = {};
uint64_t g_vpApplied = 0;             // executions the inverse could be applied to

// Stage D2 — the two histograms, and the control's own degenerate count.
uint64_t g_clipExec[kClipBucketCount] = {};
uint64_t g_clipCtl[kClipBucketCount] = {};
uint64_t g_ctlVerts = 0, g_ctlDegenerate = 0, g_aluNoViewportDraws = 0;
// The guard the Stage 3 probe needed (d3d9_draw.cpp, kSpreadEpsilon): a
// transform that collapses distinct inputs to a point lands them all in one
// bucket and looks like agreement. Tracked per draw for the control, because a
// degenerate control is not a reference — it is a second way of saying nothing.
uint64_t g_ctlCollapsedDraws = 0;
constexpr float kCtlSpreadEpsilon = 1e-4f;

void ProbeShaderExecution(const mx::pm4::DrawCall& dc, uint32_t handle,
                          const mx::pm4::HleStream* streams, uint32_t device,
                          uint8_t* base) {
  using namespace mx::pm4;
  if (!REXCVAR_GET(hle_capture) || !handle || !device) return;
  // Stage D: the sampling rate is the measurement, so it is a cvar and not a
  // constant. 0 is off, N runs one draw in N, 1 runs every draw — and only the
  // last of those says what using the interpreter would actually cost.
  const uint32_t every = REXCVAR_GET(hle_shader_exec);
  if (every == 0) return;
  static uint64_t s_draws = 0;
  if ((++s_draws % every) != 0) return;

  // Timed from here, so the cost includes the lookups and the 1,024-word
  // constant copy below and not merely the interpreter. Charging the frame only
  // for ExecuteVertexShader would understate it by exactly the part that is
  // easiest to forget.
  const auto t0 = std::chrono::steady_clock::now();
  struct ChargeTime {
    std::chrono::steady_clock::time_point t;
    ~ChargeTime() {
      g_aluNanos += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::steady_clock::now() - t)
                                 .count());
    }
  } charge{t0};
  ++g_aluDrawsEntered;

  // Stage G: the exact code first, the heuristic only as a fallback, and each
  // counted separately so "coverage improved" is a measurement rather than a
  // hope. Both paths are kept because the patch hook fires on the lazy-state
  // path — a shader bound but never re-patched has no entry, and swapping one
  // heuristic for one assumption would not be progress.
  const std::vector<uint32_t>* codep = nullptr;
  uint32_t off = 0;
  static std::vector<VertexAttribute> decoded;
  const std::vector<VertexAttribute>* attrsp = nullptr;
  const std::vector<VertexAttribute>* peer = nullptr;   // PM4's, for comparison

  auto pi = g_patchedCode.find(handle);
  if (pi != g_patchedCode.end() && pi->second.resolved) {
    decoded.clear();
    const char* why = nullptr;
    const uint32_t s = pi->second.code_off;
    if (DecodeVertexShaderFetches(pi->second.code.data() + s,
                                  uint32_t(pi->second.code.size() - s), decoded,
                                  &why)) {
      // The binding table said how many vfetches this shader has. If the decode
      // disagrees, the captured window did not start where it was assumed to —
      // a wrong start would otherwise decode into plausible nonsense.
      if (decoded.size() == pi->second.expect_fetches) {
        ++g_patchDecodeOk;
        codep = &pi->second.code;
        off = s;
        attrsp = &decoded;
      } else {
        ++g_patchDecodeCount;
      }
    } else {
      ++g_patchDecodeFail;
      ++g_patchDecodeFailWhy[why ? why : "?"];
    }
  }

  auto bi = g_bestKeyAtDraw.find(handle);
  auto di = g_physDumpAtDraw.find(handle);
  auto ci = (bi != g_bestKeyAtDraw.end())
                ? mx::pm4::CapturedShaders().find(bi->second.key)
                : mx::pm4::CapturedShaders().end();
  const bool heuristic_ok =
      bi != g_bestKeyAtDraw.end() && bi->second.pct >= 90 &&
      di != g_physDumpAtDraw.end() &&
      ci != mx::pm4::CapturedShaders().end() && !ci->second.attrs.empty() &&
      bi->second.off_dwords < di->second.size();
  if (heuristic_ok) peer = &ci->second.attrs;

  if (codep) {
    ++g_srcPatchHook;
    if (peer) {
      // Third independent reading of the same fact: our decode of D3D9's
      // patched output against PM4's decode of the ring's copy. Same bytes by
      // two routes, so they should agree.
      bool same = peer->size() == attrsp->size();
      for (size_t a = 0; same && a < attrsp->size(); ++a) {
        same = (*peer)[a].fetch_slot == (*attrsp)[a].fetch_slot &&
               (*peer)[a].format == (*attrsp)[a].format &&
               (*peer)[a].offset_bytes == (*attrsp)[a].offset_bytes &&
               (*peer)[a].stride_bytes == (*attrsp)[a].stride_bytes;
      }
      if (same) ++g_attrAgree; else ++g_attrDisagree;
    } else {
      ++g_attrNoPeer;
    }
  } else if (heuristic_ok) {
    ++g_srcHeuristic;
    codep = &di->second;
    off = bi->second.off_dwords;
    attrsp = &ci->second.attrs;
  } else {
    ++g_srcNone;
    ++g_aluNoShader;
    return;
  }

  const std::vector<VertexAttribute>& attrs = *attrsp;
  const std::vector<uint32_t>& code = *codep;
  if (attrs.empty()) { ++g_aluNoAttrs; return; }

  // Which stream each attribute fetches from.
  //
  // No longer a guess. PatchVertexShaderToMatchVertexDeclaration writes
  // `95 - element.stream` into the vfetch's constant field (subfic r20, r5,
  // 0x5F at 0x82564E30), so inverting it gives the D3D9 stream number. The
  // prediction probe agreed with what D3D9 actually wrote on 52 of 53 slots.
  //
  // This also explains why every observed fetch_slot was 95: it is stream 0.
  // The value is ambiguous — the no-match path writes 95 too — but an unmatched
  // vfetch is left with a canned format (0x60000) and swizzle (0x9250), and the
  // measurement found 0 unmatched slots in this title.
  static std::vector<uint32_t> astream;
  astream.assign(attrs.size(), 0);
  for (size_t a = 0; a < attrs.size(); ++a) {
    const uint32_t fs = attrs[a].fetch_slot;
    if (fs > 95 || (95u - fs) >= kMaxStreams) { ++g_aluBadStream; return; }
    astream[a] = 95u - fs;
    const HleStream& sa = streams[astream[a]];
    if (!sa.bound || !sa.host || sa.stride == 0 ||
        sa.stride != attrs[a].stride_bytes) {
      ++g_aluStrideMismatch;
      return;
    }
  }

  // The constant file, straight from the device. Const(i) reads
  // alu_consts[i*4], and D3D9 register N lives at +0x780 + N*16, so the two are
  // the same indexing and no rebase is needed — the API applied the base.
  static std::vector<uint32_t> consts;
  consts.assign(kD3d9ConstRegs * 4, 0);
  if (!HostPageReadable(REX_RAW_ADDR(device + 0x780)) ||
      !HostPageReadable(REX_RAW_ADDR(device + 0x780 + kD3d9ConstRegs * 16 - 4)))
    return;
  for (uint32_t i = 0; i < kD3d9ConstRegs * 4; ++i) {
    const uint32_t bits = REX_LOAD_U32(device + 0x780 + i * 4);
    consts[i] = bits;
  }
  AluInputs in;
  in.alu_consts = consts.data();
  in.alu_const_dwords = uint32_t(consts.size());

  const uint32_t want = REXCVAR_GET(hle_shader_verts);
  const uint32_t n = dc.vertex_count < want ? dc.vertex_count : want;

  // The control's transform: the viewport inverse, which is what this draw is
  // actually rendered with today and what scored 0% under Stage 3's threshold.
  // Built once per draw; a draw with no viewport yet gets no control rather
  // than an identity standing in for one.
  float ctl[16];
  const bool have_ctl = BuildViewportMvp(device, base, ctl);
  if (!have_ctl) ++g_aluNoViewportDraws;
  float ctl_lo[2] = {1e30f, 1e30f}, ctl_hi[2] = {-1e30f, -1e30f};
  uint32_t ctl_scored = 0;

  uint8_t vtx[kMaxStreams][256];
  std::vector<std::array<float, 4>> values(attrs.size());
  for (uint32_t v = 0; v < n; ++v) {
    // The stream index this built vertex came from. dc.vertices packs the
    // referenced range starting at first_vertex, so using v alone would run the
    // shader on one vertex and the control on a different one, and the two
    // histograms would not be comparable — which is the whole point of having
    // a control.
    const uint64_t src = uint64_t(dc.first_vertex) + v;

    // One decoded vertex per stream this shader actually reads, fetched once
    // and shared by every attribute that comes from it.
    bool have[kMaxStreams] = {};
    bool ranged = true;
    for (size_t a = 0; a < attrs.size() && ranged; ++a) {
      const uint32_t si = astream[a];
      if (have[si]) continue;
      const HleStream& sa = streams[si];
      const uint64_t byte_off = src * sa.stride + sa.offset_bytes;
      if (byte_off + sa.stride > sa.size_bytes ||
          sa.stride > sizeof(vtx[0])) {
        ranged = false;
        break;
      }
      std::memcpy(vtx[si], sa.host + byte_off, sa.stride);
      ApplyFetchEndian(vtx[si], sa.stride, sa.endian);
      have[si] = true;
    }
    if (!ranged) break;

    for (size_t a = 0; a < attrs.size(); ++a) {
      float o[4] = {0, 0, 0, 1};
      ReadVertexAttribute(vtx[astream[a]], streams[astream[a]].stride, attrs[a],
                          o);
      values[a] = {o[0], o[1], o[2], o[3]};
    }

    const AluResult r = ExecuteVertexShader(code.data() + off,
                                            uint32_t(code.size() - off), attrs,
                                            values, in);
    ++g_aluRuns;
    ++g_aluStatus[int(r.status)];
    if (r.blocking_opcode) ++g_aluBlocking[r.blocking_opcode];
    g_aluConstReads += r.const_reads;
    g_aluConstZero += r.const_zero_reads;

    const float* p = r.position;
    const bool finite = std::isfinite(p[0]) && std::isfinite(p[1]) &&
                        std::isfinite(p[2]) && std::isfinite(p[3]);
    // The same guard the transform probe needed: an export of (0,0,0,w=0) sits
    // inside any volume and means nothing.
    if (p[0] == 0.0f && p[1] == 0.0f && p[2] == 0.0f) {
      ++g_aluDegenerate;
    } else {
      // Kept exactly as Stage C measured it — full volume, z included — so the
      // 35% stays reproducible beside the histogram rather than being quietly
      // redefined into a different number with the same name.
      if (finite && p[3] > 0.0f && p[0] >= -p[3] && p[0] <= p[3] &&
          p[1] >= -p[3] && p[1] <= p[3] && p[2] >= 0.0f && p[2] <= p[3]) {
        ++g_aluInClip;
      }
      ++g_clipExec[ClassifyClip(p)];
    }

    // Stage F. Scored on every execution including the degenerate ones —
    // ClassifyExportSpace takes those out itself, and it has to, because the
    // origin is inside both regions.
    {
      float dv[6];
      if (ReadDeviceViewport(device, base, dv)) {
        const float xs = dv[2] * 0.5f;
        const float xo = dv[0] + xs;
        const float ys = -dv[3] * 0.5f;
        const float yo = dv[1] + dv[3] * 0.5f;
        ++g_spaceCount[uint32_t(
            ClassifyExportSpace(p[0], p[1], p[3], xs, xo, ys, yo))];

        // The same buckets after the viewport inverse. Done on the divided
        // position and rewrapped with w=1, so the bucket function sees exactly
        // what the renderer would put on screen.
        if (p[3] != 0.0f && std::isfinite(p[3])) {
          const float dx = p[0] / p[3], dy = p[1] / p[3];
          const float q[4] = {(dx - xo) / xs, (dy - yo) / ys, 0.0f, 1.0f};
          ++g_clipExecVp[ClassifyClip(q)];
          ++g_vpApplied;
        }
      }
    }

    // The first few exports in full. A bucket count cannot show that every
    // position reads (640, 0, 1, 1) — which is how the ring's space was
    // identified in the first place.
    {
      static uint32_t s_dumped = 0;
      if (s_dumped < 8) {
        ++s_dumped;
        auto& f = DeclFile();
        f << "HLE EXPORT " << s_dumped << ": pos = " << p[0] << " " << p[1]
          << " " << p[2] << " w=" << p[3];
        float dv[6];
        if (ReadDeviceViewport(device, base, dv))
          f << "   device viewport " << dv[0] << "," << dv[1] << " " << dv[2]
            << "x" << dv[3];
        const auto& sv = DeviceState().viewport;
        if (sv.seen)
          f << "   arg shadow " << sv.width << "x" << sv.height;
        f << "\n";
        f.flush();
      }
    }

    // The control, on the same vertex: the host position BuildHleDraw decoded
    // for it, through the viewport inverse.
    if (have_ctl &&
        (size_t(v) + 1) * kHostVertexStride <= dc.vertices.size()) {
      const float* hp =
          reinterpret_cast<const float*>(dc.vertices.data() +
                                         size_t(v) * kHostVertexStride);
      if (hp[0] == 0.0f && hp[1] == 0.0f && hp[2] == 0.0f) {
        ++g_ctlDegenerate;
      } else {
        float o[4];
        for (uint32_t r4 = 0; r4 < 4; ++r4) {
          o[r4] = ctl[r4 * 4 + 0] * hp[0] + ctl[r4 * 4 + 1] * hp[1] +
                  ctl[r4 * 4 + 2] * hp[2] + ctl[r4 * 4 + 3];
        }
        ++g_ctlVerts;
        ++g_clipCtl[ClassifyClip(o)];
        if (o[3] != 0.0f) {
          const float nx = o[0] / o[3], ny = o[1] / o[3];
          if (nx < ctl_lo[0]) ctl_lo[0] = nx;
          if (nx > ctl_hi[0]) ctl_hi[0] = nx;
          if (ny < ctl_lo[1]) ctl_lo[1] = ny;
          if (ny > ctl_hi[1]) ctl_hi[1] = ny;
          ++ctl_scored;
        }
      }
    }
  }

  // Did the control collapse this draw's distinct vertices onto one point? A
  // transform that does lands every vertex in one bucket and reads as a strong
  // signal while meaning nothing — the failure the Stage 3 probe hit and had to
  // guard against (kSpreadEpsilon, d3d9_draw.cpp). Counted, not discarded: how
  // often the reference degenerates is itself part of how much it is worth.
  if (ctl_scored > 1 && (ctl_hi[0] - ctl_lo[0]) < kCtlSpreadEpsilon &&
      (ctl_hi[1] - ctl_lo[1]) < kCtlSpreadEpsilon) {
    ++g_ctlCollapsedDraws;
  }
}

// Stage D — the cost, stated against a frame rather than as a bare total.
// Reported even when nothing ran, because "the interpreter was off and the
// frame took N ms" is the baseline every other row is compared to.
//
// **Windowed, not cumulative.** A first run reported the run-wide mean and it
// climbed monotonically — 219 ms/frame at frame 146, 808 ms at frame 186 —
// because this title genuinely degrades as it runs. A run-wide mean therefore
// measures mostly how long the run had been going, and comparing two configs on
// it compares their durations. The delta since the previous report is the
// number that can be compared; the cumulative figures stay beside it so the
// drift remains visible rather than hidden by the fix.
void ReportShaderExecutionCost() {
  static uint64_t s_frames = 0, s_frameNs = 0, s_aluNs = 0, s_draws = 0,
                  s_runs = 0;
  const uint64_t frames = mx::pm4::D3D9FrameCount();
  const uint64_t frame_ns = mx::pm4::D3D9FrameNanos();

  const uint64_t d_frames = frames - s_frames;
  const uint64_t d_frame_ns = frame_ns - s_frameNs;
  const uint64_t d_alu_ns = g_aluNanos - s_aluNs;
  const uint64_t d_draws = g_aluDrawsEntered - s_draws;
  const uint64_t d_runs = g_aluRuns - s_runs;
  s_frames = frames; s_frameNs = frame_ns; s_aluNs = g_aluNanos;
  s_draws = g_aluDrawsEntered; s_runs = g_aluRuns;

  const double win_frame_ms =
      d_frames ? double(d_frame_ns) / double(d_frames) / 1e6 : 0.0;
  const double win_alu_ms =
      d_frames ? double(d_alu_ns) / double(d_frames) / 1e6 : 0.0;
  REXLOG_INFO(
      "d3d9: stageD  cost — exec={} verts={} | window: {} frames, {:.1f} "
      "ms/frame, interpreter {:.3f} ms/frame ({:.2f}% of a frame), {} draws "
      "entered, {} vertices | run total: {} frames, {:.1f}s, interpreter "
      "{:.1f} ms, {} vertices",
      REXCVAR_GET(hle_shader_exec), REXCVAR_GET(hle_shader_verts), d_frames,
      win_frame_ms, win_alu_ms,
      win_frame_ms > 0.0 ? (win_alu_ms / win_frame_ms) * 100.0 : 0.0, d_draws,
      d_runs, frames, double(frame_ns) / 1e9, double(g_aluNanos) / 1e6,
      g_aluRuns);
}

// Stage D2 — the two distributions, side by side, as counts.
void ReportClipHistogram() {
  if (!g_aluRuns) return;
  for (uint32_t b = 0; b < kClipBucketCount; ++b) {
    REXLOG_INFO("d3d9: stageD2 clip {:>9} : executed {:>7}   viewport-inverse {:>7}",
                kClipBucketName[b], g_clipExec[b], g_clipCtl[b]);
  }
  REXLOG_INFO(
      "d3d9: stageD2 control — {} vertices transformed, {} skipped as "
      "degenerate input, {} draws had no viewport yet, {} draws collapsed to a "
      "point (a collapsed control is not a reference)",
      g_ctlVerts, g_ctlDegenerate, g_aluNoViewportDraws, g_ctlCollapsedDraws);
  REXLOG_INFO(
      "d3d9: stageD2 buckets are max(|x/w|,|y/w|); the '<=1' row is NOT the "
      "same test as stageC's in-clip count, which also bounds z");

  // Stage F. The question every number above assumed an answer to.
  const uint64_t sp_total = g_spaceCount[0] + g_spaceCount[1] + g_spaceCount[2] +
                            g_spaceCount[3];
  if (sp_total) {
    REXLOG_INFO(
        "d3d9: stageF  export space — clip-like {} ({}%), window-like {} ({}%), "
        "neither {}, degenerate {} — of {} scored (clip wins ties, degenerate "
        "removed first)",
        g_spaceCount[uint32_t(mx::pm4::ExportSpace::kClipLike)],
        (g_spaceCount[uint32_t(mx::pm4::ExportSpace::kClipLike)] * 100) /
            sp_total,
        g_spaceCount[uint32_t(mx::pm4::ExportSpace::kWindowLike)],
        (g_spaceCount[uint32_t(mx::pm4::ExportSpace::kWindowLike)] * 100) /
            sp_total,
        g_spaceCount[uint32_t(mx::pm4::ExportSpace::kNeither)],
        g_spaceCount[uint32_t(mx::pm4::ExportSpace::kDegenerate)], sp_total);
    for (uint32_t b = 0; b < kClipBucketCount; ++b) {
      REXLOG_INFO(
          "d3d9: stageF  clip {:>9} : raw {:>7}   after viewport inverse {:>7}",
          kClipBucketName[b], g_clipExec[b], g_clipExecVp[b]);
    }
    REXLOG_INFO(
        "d3d9: stageF  viewport inverse applied to {} of {} executions; if "
        "these positions are window coordinates this is where they collapse "
        "into <=1",
        g_vpApplied, sp_total);
  }
  if (!g_viewportExtents.empty()) {
    std::string ve;
    for (const auto& [k, n] : g_viewportExtents) {
      char buf[48];
      std::snprintf(buf, sizeof(buf), "%ux%u:%llu ", uint32_t(k >> 32),
                    uint32_t(k), (unsigned long long)n);
      ve += buf;
    }
    REXLOG_INFO(
        "d3d9: stageF  SetViewport *argument* extents seen — {}(the argument "
        "shadow keeps only the last; the transform now reads the device's "
        "clamped copy at +0x3218 instead)",
        ve);
  }
  if (g_vpFromDevice || g_vpFromShadow) {
    REXLOG_INFO(
        "d3d9: stageF  viewport source — device +0x3218 {}, argument shadow "
        "fallback {}; device disagreed with the shadow's extent on {} of them "
        "(that difference is the clamp, and the bug)",
        g_vpFromDevice, g_vpFromShadow, g_vpDisagreed);
  }
}

void ReportShaderExecution() {
  ReportShaderExecutionCost();
  if (!g_aluRuns) {
    REXLOG_INFO(
        "d3d9: stageC  shader execution — nothing ran (no located shader {}, "
        "no attrs {}, stride mismatch {})",
        g_aluNoShader, g_aluNoAttrs, g_aluStrideMismatch);
    return;
  }
  const uint64_t scored = g_aluRuns - g_aluDegenerate;
  std::string st;
  for (const auto& [k, n] : g_aluStatus) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d:%llu ", k, (unsigned long long)n);
    st += buf;
  }
  std::string bl;
  for (const auto& [k, n] : g_aluBlocking) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%X:%llu ", k, (unsigned long long)n);
    bl += buf;
  }
  REXLOG_INFO(
      "d3d9: stageC  shader execution — {} vertices run, {} exported a "
      "degenerate position, {} of the remaining {} landed in the clip volume "
      "({}%); status {}; blocking opcodes {}",
      g_aluRuns, g_aluDegenerate, g_aluInClip, scored,
      scored ? (g_aluInClip * 100) / scored : 0, st, bl.empty() ? "none" : bl);
  REXLOG_INFO(
      "d3d9: stageC  constant reads {} of which {} read zero — a shader "
      "computing from an empty file is the failure that still looks like "
      "success",
      g_aluConstReads, g_aluConstZero);
  REXLOG_INFO(
      "d3d9: stageC  skipped: no located shader {}, no attrs {}, bound stride "
      "disagrees with the shader's {}, fetch_slot did not invert to a stream "
      "{} — of {} draws entered",
      g_aluNoShader, g_aluNoAttrs, g_aluStrideMismatch, g_aluBadStream,
      g_aluDrawsEntered);
  ReportClipHistogram();

  // Stage G — where the executed code came from. The headline is coverage:
  // the heuristic left 63% of draws with no shader at all, and every number
  // above was computed on the remaining minority.
  const uint64_t src_total = g_srcPatchHook + g_srcHeuristic + g_srcNone;
  if (src_total) {
    REXLOG_INFO(
        "d3d9: stageG  shader source — patch hook (exact) {} ({}%), content "
        "match >=90% {} ({}%), none {} ({}%) of {} draws",
        g_srcPatchHook, (g_srcPatchHook * 100) / src_total, g_srcHeuristic,
        (g_srcHeuristic * 100) / src_total, g_srcNone,
        (g_srcNone * 100) / src_total, src_total);
    std::string why;
    for (const auto& [w, n] : g_patchDecodeFailWhy) {
      char buf[96];
      std::snprintf(buf, sizeof(buf), "%s:%llu ", w.c_str(),
                    (unsigned long long)n);
      why += buf;
    }
    REXLOG_INFO(
        "d3d9: stageG  patched decode — {} matched the binding table's fetch "
        "count, {} decoded a different count (wrong window start), {} refused "
        "[{}]",
        g_patchDecodeOk, g_patchDecodeCount, g_patchDecodeFail,
        why.empty() ? "none" : why);
    std::string offs;
    for (const auto& [o, n] : g_patchCodeOffsets) {
      char buf[48];
      std::snprintf(buf, sizeof(buf), "%+d:%llu ", o, (unsigned long long)n);
      offs += buf;
    }
    REXLOG_INFO(
        "d3d9: stageG  CF section found at dword offsets from dest — {}(one "
        "value across every shader means a fixed layout; assuming -16 was "
        "wrong and the decode said so)",
        offs.empty() ? "none resolved " : offs);
    REXLOG_INFO(
        "d3d9: stageG  attributes, our decode of D3D9's patched output vs "
        "PM4's decode of the ring's copy — {} agree, {} disagree, {} had no "
        "PM4 peer to compare against",
        g_attrAgree, g_attrDisagree, g_attrNoPeer);
  }
}

void ReportPhysicalAtDrawTime() {
  if (g_physNonzeroAtDraw.empty()) return;
  uint32_t any = 0;
  for (const auto& [h, n] : g_physNonzeroAtDraw) { (void)h; any += n ? 1 : 0; }
  REXLOG_INFO(
      "d3d9: stageA  SH_pPhysical at draw time: {} of {} handles had any "
      "non-zero dword in the first 32 (it was all zeros at bind time)",
      any, g_physNonzeroAtDraw.size());

  // Same scoring as the blob search, and for the same reason: an exact compare
  // would fail on a patched vfetch even when the code is right there, so this
  // reports best agreement and only calls it found at 90%.
  const auto& captured = mx::pm4::CapturedShaders();
  uint32_t found = 0, scored = 0;
  uint64_t pct_sum = 0;
  std::map<uint32_t, uint32_t> offsets;
  // Where each handle's code was located, so Stage B decodes the same bytes
  // this scored rather than re-deriving the offset.
  std::map<uint32_t, uint32_t> agreement_bands;   // band floor -> handles
  auto& f = DeclFile();
  uint32_t named = 0;
  for (const auto& [handle, dump] : g_physDumpAtDraw) {
    size_t best_hits = 0, best_len = 0, best_at = 0;
    uint64_t best_key = 0;
    for (const auto& [key, cs] : captured) {
      if (cs.code.empty() || cs.code.size() > dump.size()) continue;
      for (size_t at = 0; at + cs.code.size() <= dump.size(); ++at) {
        size_t hits = 0;
        for (size_t i = 0; i < cs.code.size(); ++i)
          hits += dump[at + i] == cs.code[i] ? 1 : 0;
        if (hits > best_hits) {
          best_hits = hits; best_len = cs.code.size();
          best_at = at; best_key = key;
        }
      }
    }
    if (!best_len) continue;
    ++scored;
    const uint32_t pct = uint32_t(best_hits * 100 / best_len);
    pct_sum += pct;
    ++agreement_bands[(pct / 20) * 20];
    g_bestKeyAtDraw[handle] = {best_key, uint32_t(best_at), pct};
    if (pct >= 90) {
      ++found;
      ++offsets[uint32_t(best_at * 4)];
    }
    if (named < 10) {
      ++named;
      f << "DRAWTIME UCODE: shader 0x" << std::hex << handle << std::dec
        << " best " << best_hits << "/" << best_len << " at offset 0x"
        << std::hex << uint32_t(best_at * 4) << " vs ring key 0x" << best_key
        << std::dec << "\n";
    }
  }
  f.flush();
  if (scored) {
    std::string offs;
    for (const auto& [off, n] : offsets) {
      char buf[32];
      std::snprintf(buf, sizeof(buf), "0x%X:%u ", off, n);
      offs += buf;
    }
    REXLOG_INFO(
        "d3d9: stageA  draw-time SH_pPhysical vs the ring's microcode: {} of {} "
        "handles matched at 90%+ (mean best {}%), at offsets {}",
        found, scored, pct_sum / scored, offs.empty() ? "none" : offs);
    // The shortfall, named rather than left as "19 of 33". A handle at ~0%
    // is a shader the ring never loaded in this window; one in the middle is
    // a real disagreement and would matter.
    std::string spread;
    for (const auto& [lo, n] : agreement_bands) {
      char buf[32];
      std::snprintf(buf, sizeof(buf), "%u-%u%%:%u ", lo, lo + 19, n);
      spread += buf;
    }
    REXLOG_INFO("d3d9: stageA  agreement spread across handles: {}", spread);
  }

  //-------------------------------------------------------------------------
  // Stage B — decode what was found, and check it against PM4's decode.
  //
  // This is the cross-check the plan promised and Stage 3's could not deliver:
  // two independent paths to the same shader's vertex attributes. PM4 decoded
  // the copy the ring carried; this decodes the copy sitting at
  // SH_pPhysical + 0x40. If they name the same fetch slots, offsets, strides
  // and formats, the D3D9 side can reach the shader — and that is the whole
  // question this milestone exists to answer.
  //-------------------------------------------------------------------------
  {
    uint32_t decoded = 0, decode_failed = 0, agreed = 0, disagreed = 0;
    uint64_t attrs_same = 0, attrs_diff = 0;
    std::map<std::string, uint32_t> fails;
    uint32_t named_b = 0;
    for (const auto& [handle, dump] : g_physDumpAtDraw) {
      auto bi = g_bestKeyAtDraw.find(handle);
      if (bi == g_bestKeyAtDraw.end()) continue;
      const uint64_t key = bi->second.key;
      const uint32_t off_dwords = bi->second.off_dwords;
      const uint32_t pct = bi->second.pct;
      if (pct < 90) continue;   // only where the code was actually located
      auto ci = captured.find(key);
      if (ci == captured.end()) continue;
      if (off_dwords >= dump.size()) continue;

      std::vector<mx::pm4::VertexAttribute> mine;
      const char* fail = nullptr;
      const bool ok = mx::pm4::DecodeVertexShaderFetches(
          dump.data() + off_dwords, uint32_t(dump.size() - off_dwords), mine,
          &fail);
      if (!ok) {
        ++decode_failed;
        ++fails[fail ? fail : "?"];
        continue;
      }
      ++decoded;

      const auto& theirs = ci->second.attrs;
      bool all_same = mine.size() == theirs.size();
      for (size_t i = 0; i < mine.size() && i < theirs.size(); ++i) {
        const auto& a = mine[i];
        const auto& b = theirs[i];
        const bool same = a.fetch_slot == b.fetch_slot &&
                          a.offset_bytes == b.offset_bytes &&
                          a.stride_bytes == b.stride_bytes &&
                          a.format == b.format && a.dest_reg == b.dest_reg;
        (same ? attrs_same : attrs_diff) += 1;
        all_same = all_same && same;
      }
      (all_same ? agreed : disagreed) += 1;
      if (!all_same && named_b < 8) {
        ++named_b;
        f << "ATTR DISAGREE: shader 0x" << std::hex << handle << " key 0x"
          << key << std::dec << " — HLE " << mine.size() << " attrs, PM4 "
          << theirs.size() << "\n";
        for (size_t i = 0; i < mine.size() || i < theirs.size(); ++i) {
          f << "    [" << i << "] HLE ";
          if (i < mine.size())
            f << "slot=" << mine[i].fetch_slot << " off=" << mine[i].offset_bytes
              << " stride=" << mine[i].stride_bytes << " fmt=" << mine[i].format
              << " dest=" << mine[i].dest_reg;
          else
            f << "(none)";
          f << " | PM4 ";
          if (i < theirs.size())
            f << "slot=" << theirs[i].fetch_slot << " off="
              << theirs[i].offset_bytes << " stride=" << theirs[i].stride_bytes
              << " fmt=" << theirs[i].format << " dest=" << theirs[i].dest_reg;
          else
            f << "(none)";
          f << "\n";
        }
      }
    }
    f.flush();
    if (decoded || decode_failed) {
      std::string why;
      for (const auto& [k, n] : fails) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "%s:%u ", k.c_str(), n);
        why += buf;
      }
      REXLOG_INFO(
          "d3d9: stageB  decoded {} of {} located shaders ({} refused: {}) — "
          "attribute lists identical to PM4's for {} shaders, differing for "
          "{}; per attribute {} same {} different",
          decoded, decoded + decode_failed, decode_failed,
          why.empty() ? "none" : why, agreed, disagreed, attrs_same, attrs_diff);
    }
  }
}

void ReportBlobSearch() {
  const auto& captured = mx::pm4::CapturedShaders();
  uint64_t matched = 0, unmatched = 0;
  std::map<uint32_t, uint32_t> offsets;
  auto& f = DeclFile();
  uint32_t named = 0;

  // **Partial agreement, not memcmp.** An exact compare answers the wrong
  // question here: PatchVertexShaderToMatchVertexDeclaration rewrites vfetch
  // dwords, so the copy the ring carries is the *patched* one while the object
  // may hold the original. Those two are the same shader and would never be
  // byte-identical. Scoring the best alignment turns "no match" from a verdict
  // into a measurement — 90% agreement at one offset says the code is there and
  // patched; 25% everywhere says it is not there at all.
  //
  // The byteswapped score is a control. If it wins, the two sides simply
  // disagree about endianness and nothing deeper is wrong.
  uint64_t best_pct_sum = 0;
  uint32_t scored = 0;
  for (const auto& [handle, blob] : g_vsBlobs) {
    size_t best_hits = 0, best_len = 0, best_at = 0, best_swapped_hits = 0;
    uint64_t best_key = 0;
    bool best_ok = false;
    for (const auto& [key, cs] : captured) {
      if (cs.code.empty() || cs.code.size() > blob.size()) continue;
      for (size_t at = 0; at + cs.code.size() <= blob.size(); ++at) {
        size_t hits = 0, swapped = 0;
        for (size_t i = 0; i < cs.code.size(); ++i) {
          const uint32_t b = blob[at + i];
          if (b == cs.code[i]) ++hits;
          if (__builtin_bswap32(b) == cs.code[i]) ++swapped;
        }
        if (hits > best_hits) {
          best_hits = hits;
          best_len = cs.code.size();
          best_at = at;
          best_key = key;
          best_ok = cs.ok;
        }
        if (swapped > best_swapped_hits) best_swapped_hits = swapped;
      }
    }
    if (!best_len) continue;
    ++scored;
    const uint32_t pct = uint32_t(best_hits * 100 / best_len);
    best_pct_sum += pct;
    // Treated as found only when nearly all of it agrees. A patched vfetch is a
    // handful of dwords; a different shader is most of them.
    if (best_hits * 100 >= best_len * 90) {
      ++matched;
      ++offsets[uint32_t(best_at * 4)];
    } else {
      ++unmatched;
    }
    if (named < 12) {
      ++named;
      f << "UCODE BEST: shader 0x" << std::hex << handle << std::dec
        << " blob " << blob.size() << " dwords — best " << best_hits << "/"
        << best_len << " (" << pct << "%) at blob byte offset 0x" << std::hex
        << uint32_t(best_at * 4) << " vs ring key 0x" << best_key << std::dec
        << " (decode " << (best_ok ? "ok" : "FAILED")
        << "), byteswapped control best " << best_swapped_hits << "\n";
    }
  }
  f.flush();
  if (scored) {
    REXLOG_INFO(
        "d3d9: stageA  mean best agreement across {} blobs: {}% — a shader that "
        "is present but patched scores high, one that is absent scores low",
        scored, best_pct_sum / scored);
  }

  REXLOG_INFO(
      "d3d9: stageA  shader blobs: {} of {} contained microcode the ring "
      "loaded, against {} distinct shaders the ring captured ({} blobs "
      "unreadable)",
      matched, g_vsBlobs.size(), captured.size(), g_vsBlobUnreadable);
  if (offsets.empty()) {
    REXLOG_INFO(
        "d3d9: stageA  NO blob contained any captured microcode — the blob at "
        "+0x368 is not raw ucode, and nothing downstream should be built on it");
  } else {
    std::string offs;
    for (const auto& [off, n] : offsets) {
      char buf[32];
      std::snprintf(buf, sizeof(buf), "0x%X:%u ", off, n);
      offs += buf;
    }
    REXLOG_INFO("d3d9: stageA  microcode found at blob byte offsets {}", offs);
  }
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

struct PatchPrediction {
  uint32_t dest_addr = 0;
  uint32_t pred[3] = {};
  bool     bound = false;
};


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
void CapturePatchedCode(uint32_t self, uint32_t dest, uint32_t variant,
                        uint32_t expect_fetches, uint8_t* base) {
  if (!self || !dest || dest < kPatchWindowBack * 4) return;
  const uint32_t start = dest - kPatchWindowBack * 4;

  auto it = g_patchedCode.find(self);
  const bool known = it != g_patchedCode.end() && it->second.resolved;
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
  static std::vector<mx::pm4::VertexAttribute> probe;
  auto decodes_at = [&](uint32_t s) {
    if (s >= pc.code.size()) return false;
    probe.clear();
    return mx::pm4::DecodeVertexShaderFetches(pc.code.data() + s,
                                              uint32_t(pc.code.size() - s),
                                              probe, nullptr) &&
           probe.size() == expect_fetches;
  };

  if (known && decodes_at(known_off)) {
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
      ++g_patchCodeOffsets[int32_t(s) - int32_t(kPatchWindowBack)];
      break;
    }
  }
  g_patchedCode[self] = std::move(pc);
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
    const char* nm = mx::pm4::UsageSemanticName(uint8_t(usage));
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

void ReportCoverage(uint8_t* base) {
  const auto& st = DeviceState();
  if (g_drawsChecked == 0) {
    REXLOG_INFO("d3d9: hle — no draws scored");
    return;
  }
  REXLOG_INFO("d3d9: hle — {} of {} draws fully described ({}%)",
              g_drawsComplete, g_drawsChecked,
              (g_drawsComplete * 100) / g_drawsChecked);
  for (uint32_t g = 0; g < kDrawGapCount; ++g) {
    if (g_drawGaps[g]) {
      REXLOG_INFO("d3d9: hle   missing: {:<28} x{}", DrawGapName(g), g_drawGaps[g]);
    }
  }
  //-------------------------------------------------------------------------
  // Stage 2: what was actually built, and why the rest was not. Every skip is
  // named — a bare total cannot separate "the decoder refuses this format"
  // from "this stream is not indexed the way we model it", and those need
  // opposite fixes.
  //-------------------------------------------------------------------------
  {
    const uint64_t built = mx::pm4::HleBuiltCount();
    const uint64_t* counts = mx::pm4::HleSkipCounts();
    uint64_t attempted = built;
    for (uint32_t i = 1; i < uint32_t(mx::pm4::HleSkip::kCount); ++i)
      attempted += counts[i];
    if (attempted) {
      REXLOG_INFO("d3d9: hle-render — {} of {} draws built ({}%)", built,
                  attempted, (built * 100) / attempted);
      for (uint32_t i = 1; i < uint32_t(mx::pm4::HleSkip::kCount); ++i) {
        if (!counts[i]) continue;
        REXLOG_INFO("d3d9: hle-render   skipped: {:<34} x{}",
                    mx::pm4::HleSkipName(mx::pm4::HleSkip(i)), counts[i]);
      }
      std::string prims;
      for (uint32_t i = 0; i < 64; ++i) {
        if (!g_badPrimType[i]) continue;
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%u:%llu ", i,
                      (unsigned long long)g_badPrimType[i]);
        prims += buf;
      }
      if (!prims.empty())
        REXLOG_INFO("d3d9: hle-render   refused prim types (type:count) {}",
                    prims);
    }
    mx::pm4::ReportHleTransform();
  }

  ReportBlobSearch();
  ReportPhysicalAtDrawTime();
  ReportShaderExecution();
  ReportPatchRule();
  ReportAddressKeyTest(base);

  //-------------------------------------------------------------------------
  // Stage 0 verdict.
  //-------------------------------------------------------------------------
  for (uint32_t s = 0; s < mx::pm4::kMaxStreams; ++s) {
    const uint64_t fit = g_vbFitStream[s], fail = g_vbFailStream[s];
    if (!fit && !fail) continue;
    REXLOG_INFO(
        "d3d9: stage0  stream {}: holds the range {}/{} | mean draws since "
        "bind: fits {} fails {} (worst {})",
        s, fit, fit + fail, fit ? g_bindAgeFitSum[s] / fit : 0,
        fail ? g_bindAgeFailSum[s] / fail : 0, g_bindAgeFailMax[s]);
  }
  for (uint32_t s = 0; s < mx::pm4::kMaxStreams; ++s) {
    if (!g_fileAgree[s] && !g_fileDiffer[s]) continue;
    REXLOG_INFO(
        "d3d9: stage0  stream {}: device fetch constant vs our snapshot — "
        "same {} differ {}, and the device's size explains {} of the failures",
        s, g_fileAgree[s], g_fileDiffer[s], g_fileRescues[s]);
  }
  REXLOG_INFO(
      "d3d9: stage0  indexed draws, by real max index: holds {}/{} (unread {})",
      g_idxRangeFits, g_idxRangeFits + g_idxRangeFails, g_idxRangeUnread);

  // Offsets that held the just-bound value on every one of the samples. One
  // surviving pair is the fetch constant file; none means D3D9 does not keep
  // the value verbatim and the snapshot is the only source available.
  if (g_fcPrimed) {
    uint32_t n0 = 0, n1 = 0;
    std::string o0, o1;
    for (uint32_t i = 0; i < kDeviceScanDwords; ++i) {
      char buf[16];
      if (g_fcCand0[i]) {
        ++n0;
        if (n0 <= 8) { std::snprintf(buf, sizeof(buf), "0x%X ", i * 4); o0 += buf; }
      }
      if (g_fcCand1[i]) {
        ++n1;
        if (n1 <= 8) { std::snprintf(buf, sizeof(buf), "0x%X ", i * 4); o1 += buf; }
      }
    }
    REXLOG_INFO(
        "d3d9: stage0  fetch constant file after {} samples (device readable to "
        "0x{:X}): dword0 offsets={} [{}] dword1 offsets={} [{}]",
        g_fcSamples, g_fcReached, n0, o0, n1, o1);
  } else {
    REXLOG_INFO("d3d9: stage0  fetch constant file: never sampled");
  }

  REXLOG_INFO(
      "d3d9: hle   stride exact={} padded={} TOO SMALL={} (too small means the "
      "layout decode is wrong)",
      g_strideOk, g_strideMismatch, g_strideTooSmall);
  REXLOG_INFO(
      "d3d9: hle   buffer holds the range: vb {}/{} ib {}/{} (denominator is "
      "draws checked for that buffer)",
      g_vbFits, g_vbFits + g_vbTooSmall, g_ibFits, g_ibFits + g_ibTooSmall);
  REXLOG_INFO(
      "d3d9: hle   vs=0x{:08X} ps=0x{:08X} ib=0x{:08X} ({} bit) vp={}x{} "
      "distinct devices={}",
      st.vertex_shader, st.pixel_shader, st.index.address,
      st.index.is_32bit ? 32 : 16, st.viewport.width, st.viewport.height,
      st.device_count);
  for (uint32_t i = 0; i < st.device_count; ++i) {
    std::string who;
    for (uint32_t e = 0; e < mx::pm4::kEntryPointCount; ++e) {
      if (!(st.device_call_mask[i] & (1u << e))) continue;
      if (!who.empty()) who += " ";
      who += mx::pm4::EntryPointName(e);
    }
    REXLOG_INFO("d3d9: hle   device 0x{:08X} x{} calls from: {}",
                st.device_ptr[i], st.device_calls[i], who);
  }
  for (uint32_t s = 0; s < mx::pm4::kMaxStreams; ++s) {
    const auto& b = st.stream[s];
    if (!b.seen) continue;
    REXLOG_INFO(
        "d3d9: hle   stream {}: addr=0x{:08X} size={}B endian={} offset={} "
        "stride={}{}",
        s, b.address, b.size_bytes, b.endian, b.offset_bytes, b.stride,
        b.bound ? "" : " (unbound)");
  }
}

// The fully-resolved draw, written out for the first few of each kind so the
// description can be read and checked by eye rather than only counted. Goes to
// the non-rotating dump: these happen at load and the rotating log has already
// lost two probes this effort.
constexpr uint64_t kMaxHleDumped = 12;

void DumpHleDraw(bool indexed, uint64_t n, uint32_t prim, int32_t base_vertex,
                 uint32_t start, uint32_t count) {
  if (n > kMaxHleDumped) return;
  const auto& st = DeviceState();
  auto& f = DeclFile();

  f << "\nHleDraw " << (indexed ? "indexed" : "non-indexed") << " #" << n
    << " prim=" << prim << (indexed ? " base_vertex=" : " start_vertex=")
    << base_vertex;
  if (indexed) f << " start_index=" << start;
  f << (indexed ? " index_count=" : " vertex_count=") << count << "\n";

  const int id = st.current_decl;
  if (id < 0) {
    f << "  declaration: NONE\n";
  } else if (!g_declLayoutOk[id]) {
    f << "  declaration id " << id << ": DOES NOT DECODE ("
      << mx::pm4::LayoutErrorText(g_declLayoutErr[id].reason) << ")\n";
  } else {
    const auto& layout = g_declLayout[id];
    f << "  declaration id " << id << ", " << layout.elements.size()
      << " element(s):\n";
    for (const auto& e : layout.elements) {
      f << "    " << e.semantic_name << e.semantic_index << " s" << e.stream
        << " off=" << e.offset << " size=" << e.size_bytes
        << " dxgi=" << static_cast<int>(e.format);
      if (e.unpack == mx::pm4::Unpack::kSnorm2_10_10_10)
        f << " (shader unpacks snorm 2_10_10_10)";
      f << "\n";
    }
    for (uint32_t s = 0; s <= layout.max_stream; ++s) {
      const auto& b = st.stream[s];
      f << "    stream " << s << ": ";
      if (!b.seen) {
        f << "NEVER SET\n";
        continue;
      }
      f << "addr=0x" << std::hex << b.address << std::dec
        << " size=" << b.size_bytes << " offset=" << b.offset_bytes
        << " stride=" << b.stride << " (layout needs " << layout.min_stride[s]
        << ")" << (b.bound ? "" : " UNBOUND") << "\n";
    }
  }

  if (indexed) {
    f << "  index buffer: ";
    if (!st.index.seen || !st.index.bound) {
      f << "NONE\n";
    } else {
      f << "addr=0x" << std::hex << st.index.address << std::dec
        << " size=" << st.index.size_bytes << " "
        << (st.index.is_32bit ? 32 : 16) << "-bit\n";
    }
  }

  f << "  vs=0x" << std::hex << st.vertex_shader << " ps=0x" << st.pixel_shader
    << std::dec << (st.vs_seen ? "" : " (vs NEVER SET)")
    << (st.ps_seen ? "" : " (ps NEVER SET)") << "\n";
  f << "  viewport: ";
  if (!st.viewport.seen) {
    f << "NEVER SET\n";
  } else {
    f << st.viewport.x << "," << st.viewport.y << " " << st.viewport.width
      << "x" << st.viewport.height << " z=[" << st.viewport.min_z << ","
      << st.viewport.max_z << "]\n";
  }
  f << "  render state:";
  for (uint32_t r = 0; r < mx::pm4::kRenderStateCount; ++r) {
    f << " " << mx::pm4::RenderStateName(r) << "=";
    if (st.render_state.Seen(r)) {
      f << st.render_state.value[r];
    } else {
      f << "unset";
    }
  }
  f << "\n";
  f.flush();
}

// The two histograms the round exists to produce.
void ReportDeclHistogram() {
  uint64_t with = 0, without = 0;
  for (int i = 0; i < g_declCount; ++i) {
    (g_declHasColour[i] ? with : without) += g_declDraws[i];
  }
  REXLOG_INFO(
      "d3d9: decl-draws — {} declarations known; COLOUR={} NO-COLOUR={} "
      "unattributed={} patch_calls={}",
      g_declCount, with, without, g_drawsNoDecl, g_patchCalls);
  // The declaration now comes from device + 0x2ED8, per draw. These four say
  // whether that source is sound and how badly the old one lagged: `unknown`
  // must stay at 0 or the field is not what SetVertexDeclaration writes, and a
  // large `stale` is the 2508-calls-per-165000-draws problem, measured.
  REXLOG_INFO(
      "d3d9: decl-source — from device+0x2ED8: null={} unknown={} | vs the "
      "patch hook: same={} stale={}",
      g_declDeviceNull, g_declDeviceUnknown, g_declAgree, g_declDisagree);
  for (int i = 0; i < g_declCount; ++i) {
    REXLOG_INFO("d3d9: decl-draws   id={} ptr=0x{:08X} elems={} colour={} x{}",
                i, g_declPtr[i], g_declElems[i],
                g_declHasColour[i] ? "yes" : "no", g_declDraws[i]);
  }
}

// Both draw entry points report through here so the two counters are always
// read together. A 150s run reaches 5000-10000 transcoded draws, so a coarser
// cadence than 2500 reports nothing at all — the first output-merger probe was
// lost to exactly that.
void ReportDrawCounts(uint8_t* base) {
  const uint64_t total = g_indexed_draws + g_draws;
  if ((total % kDrawReportEvery) != 0) return;
  REXLOG_INFO("d3d9: draws — DrawIndexedVertices={} DrawVertices={} total={}",
              g_indexed_draws, g_draws, total);
  ReportDeclHistogram();
  if (REXCVAR_GET(hle_capture)) ReportCoverage(base);
}

}  // namespace

//=============================================================================
// 0x82550B80 — D3DVertexDeclaration* D3DDevice_CreateVertexDeclaration(
//                  const D3DVERTEXELEMENT9* pVertexElements)
//
// One argument, and the declaration object comes back in r3. 18 call sites in
// game code.
//
// The object it returns is laid out by XGSetVertexDeclaration: +0x00 magic
// 0x00100005, +0x04 refcount, +0x18 element count, +0x1C highest stream index,
// +0x20 a 16-byte per-stream usage map, and **+0x34 the copied element array**.
// Recorded here because a later round hooking SetVertexDeclaration will need
// it to get from a declaration pointer back to the elements.
//=============================================================================

REX_IMPORT(__imp__sub_82550B80, orig_CreateVertexDeclaration, void());
extern "C" REX_FUNC(sub_82550B80) {
  const uint32_t elements = ctx.r3.u32;
  const uint64_t n = ++g_decls;

  orig_CreateVertexDeclaration(ctx, base);

  const uint32_t decl = ctx.r3.u32;

  // Record it for the draw-time correlation before anything else. This is the
  // only place the element array can be read safely — the runtime has just
  // walked it — and it is what makes the draw-side lookup a comparison rather
  // than a speculative dereference.
  bool has_colour = false;
  uint32_t n_elems = 0;
  mx::pm4::D3D9Element parsed[kMaxElements] = {};
  if (elements) {
    for (uint32_t i = 0; i < kMaxElements; ++i) {
      const uint32_t p = elements + i * kElementSize;
      if (REX_LOAD_U16(p) == 0xFF) break;
      uint8_t raw[kElementSize];
      for (uint32_t b = 0; b < kElementSize; ++b) raw[b] = REX_LOAD_U8(p + b);
      parsed[n_elems] = mx::pm4::ReadElement(raw);
      ++n_elems;
      if (raw[9] == mx::pm4::kUsageColor) has_colour = true;
    }
  }
  const int decl_id = RecordDeclaration(decl, has_colour, n_elems, parsed);

  // Report a declaration the layout decoder cannot describe immediately and by
  // name. The whole HLE path rests on this decode; a silent miss here would
  // surface much later as geometry that looks almost right.
  if (decl_id >= 0 && !g_declLayoutOk[decl_id]) {
    const auto& e = g_declLayoutErr[decl_id];
    REXLOG_WARN(
        "d3d9: declaration id {} does NOT decode — element {}: {} "
        "(detail 0x{:08X})",
        decl_id, e.failed_element, mx::pm4::LayoutErrorText(e.reason), e.detail);
    DeclFile() << "  LAYOUT FAILED element " << e.failed_element << ": "
               << mx::pm4::LayoutErrorText(e.reason) << " detail 0x" << std::hex
               << e.detail << std::dec << "\n";
  }

  if (n > kMaxDeclsLogged) return;

  auto& f = DeclFile();
  f << "decl #" << n << " elements=0x" << std::hex << elements << " -> decl=0x"
    << decl << std::dec << "\n";
  if (!elements) {
    f.flush();
    return;
  }

  for (uint32_t i = 0; i < kMaxElements; ++i) {
    const uint32_t p = elements + i * kElementSize;
    const uint16_t stream = REX_LOAD_U16(p);
    if (stream == 0xFF) {
      f << "  [" << i << "] END (" << i << " elements)\n";
      break;
    }
    // Raw, in guest byte order. Decoding is deliberately left to the reader:
    // the field layout past Stream is not established, and a wrong decode here
    // would be indistinguishable from a right one in the dump.
    f << "  [" << i << "] stream=" << stream << " raw=";
    for (uint32_t b = 0; b < kElementSize; ++b) {
      char hex[4];
      std::snprintf(hex, sizeof(hex), "%02X ", REX_LOAD_U8(p + b));
      f << hex;
    }
    f << "\n";
  }

  // The count the runtime itself settled on, as a cross-check against the walk
  // above. If these disagree, the element stride is wrong.
  if (decl) {
    f << "  runtime count=" << REX_LOAD_U32(decl + 0x18)
      << " max_stream=" << REX_LOAD_U32(decl + 0x1C) << "\n";
  }
  f.flush();

  // One line in the log too, so a run with no dump file is distinguishable
  // from a run where the hook was never reached.
  REXLOG_INFO("d3d9: decl #{} written to d3d9_dump_decls.txt", n);
}

//=============================================================================
// 0x82564C50 — void D3D::PatchVertexShaderToMatchVertexDeclaration(
//                  CVertexShader*, ULONG*, const CVertexDeclaration*,
//                  const BYTE*, ULONG)
//
// This is where semantics get bound to shader inputs at runtime — the reason
// they do not survive into the microcode. Only 3 xrefs, all D3D9-internal,
// because it is reached from the lazy-state path at draw time rather than
// called by the game. That is exactly what makes it the right place to read
// the current declaration from.
//
// **Which register holds the declaration is determined by comparison, not by
// reading the mangled signature.** Every argument register is checked against
// declarations we watched CreateVertexDeclaration build; whichever matches is
// the declaration. A signature misread would be invisible in the output,
// whereas a mismatch here is loud.
//=============================================================================

REX_IMPORT(__imp__sub_82564C50, orig_PatchVertexShader, void());
extern "C" REX_FUNC(sub_82564C50) {
  const uint32_t args[5] = {ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32,
                            ctx.r7.u32};
  int found = -1;
  int arg_index = -1;
  for (int a = 0; a < 5; ++a) {
    const int id = KnownDeclId(args[a]);
    if (id >= 0) {
      found = id;
      arg_index = a;
      break;
    }
  }

  static bool s_reported = false;
  if (!s_reported) {
    s_reported = true;
    auto& f = DeclFile();
    if (found >= 0) {
      f << "PatchVertexShader: declaration is argument r" << (3 + arg_index)
        << " (matched declaration id " << found << ")\n";
    } else {
      f << "PatchVertexShader: NO argument register matches a known "
           "declaration — r3..r7 = ";
      for (int a = 0; a < 5; ++a) {
        f << "0x" << std::hex << args[a] << std::dec << " ";
      }
      f << "\n";
    }
    f.flush();
    REXLOG_INFO("d3d9: PatchVertexShader declaration arg = {}",
                found >= 0 ? 3 + arg_index : -1);
  }

  if (found >= 0) g_patchDecl = found;
  ++g_patchCalls;

  // Predict before, compare after. The arguments are only guaranteed good
  // across this call, and the point of the test is what the *original* writes.
  // Sampled: this fires on the lazy-state path, and the rule either holds on
  // every slot or it does not hold at all.
  static std::vector<PatchPrediction> s_pred;
  const bool probe = REXCVAR_GET(hle_capture) && (g_patchCalls % 16) == 0;
  if (probe) {
    PredictPatchedFetches(args[0], args[1], args[2], args[3], args[4], base,
                          s_pred);
  }

  const uint32_t nfetch =
      REXCVAR_GET(hle_capture) ? ReadPatchFetchCount(args[0], args[4], base) : 0;

  orig_PatchVertexShader(ctx, base);

  // Every call, not just the sampled ones: this is the coverage fix, and the
  // destination is in the command ring so there is no second chance at it.
  if (nfetch) CapturePatchedCode(args[0], args[1], args[4], nfetch, base);

  if (probe) CheckPatchedFetches(s_pred, base);
}

//=============================================================================
// 0x825565C8 — void D3DDevice_DrawIndexedVertices(
//                  D3DDevice*, D3DPRIMITIVETYPE, INT BaseVertexIndex,
//                  UINT StartIndex, UINT IndexCount)
//
// Note IndexCount, not PrimitiveCount — the 360 variant differs from the PC
// API here. 19 call sites in game code.
//=============================================================================

REX_IMPORT(__imp__sub_825565C8, orig_DrawIndexedVertices, void());
extern "C" REX_FUNC(sub_825565C8) {
  const uint64_t n = ++g_indexed_draws;
  ++mx::pm4::D3D9DrawCounter();
  if (REXCVAR_GET(hle_capture))
    ProbePhysicalAtDrawTime(DeviceState().vertex_shader, base);
  ++mx::pm4::D3D9IndexedDrawCounter();
  NoteDrawDeclaration(ctx.r3.u32, base);
  if (n <= kMaxDrawsLogged) {
    // Same rotation problem as the declarations: the first draws happen at
    // load and would not survive a long run's log.
    auto& f = DeclFile();
    f << "DrawIndexedVertices #" << n << " dev=0x" << std::hex << ctx.r3.u32
      << std::dec << " prim=" << ctx.r4.u32 << " base_vertex=" << ctx.r5.s32
      << " start_index=" << ctx.r6.u32 << " index_count=" << ctx.r7.u32 << "\n";
    f.flush();
  }
  if (REXCVAR_GET(hle_render)) {
    // r4 PrimitiveType, r5 BaseVertexIndex, r6 StartIndex, r7 IndexCount.
    BuildAndQueueDraw(/*indexed=*/true, ctx.r4.u32, ctx.r6.u32, ctx.r7.u32,
                      ctx.r5.s32, ctx.r3.u32, base);
  }
  if (REXCVAR_GET(hle_capture)) {
    DeviceState().NoteDevice(ctx.r3.u32, mx::pm4::kEpDraw);
    SampleFetchConstantFile(ctx.r3.u32, base);
    ScoreDraw(/*indexed=*/true, ctx.r6.u32, ctx.r7.u32, ctx.r3.u32, base);
    DumpHleDraw(/*indexed=*/true, n, ctx.r4.u32, ctx.r5.s32, ctx.r6.u32,
                ctx.r7.u32);
  }
  ReportDrawCounts(base);
  orig_DrawIndexedVertices(ctx, base);
}

//=============================================================================
// 0x825561B0 — void D3DDevice_DrawVertices(
//                  D3DDevice*, D3DPRIMITIVETYPE, UINT StartVertex,
//                  UINT VertexCount)
//
// 34 call sites in game code.
//=============================================================================

REX_IMPORT(__imp__sub_825561B0, orig_DrawVertices, void());
extern "C" REX_FUNC(sub_825561B0) {
  const uint64_t n = ++g_draws;
  ++mx::pm4::D3D9DrawCounter();
  if (REXCVAR_GET(hle_capture))
    ProbePhysicalAtDrawTime(DeviceState().vertex_shader, base);
  NoteDrawDeclaration(ctx.r3.u32, base);
  if (n <= kMaxDrawsLogged) {
    auto& f = DeclFile();
    f << "DrawVertices #" << n << " dev=0x" << std::hex << ctx.r3.u32
      << std::dec << " prim=" << ctx.r4.u32 << " start_vertex=" << ctx.r5.u32
      << " vertex_count=" << ctx.r6.u32 << "\n";
    f.flush();
  }
  if (REXCVAR_GET(hle_render)) {
    // r4 PrimitiveType, r5 StartVertex, r6 VertexCount.
    BuildAndQueueDraw(/*indexed=*/false, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, 0,
                      ctx.r3.u32, base);
  }
  if (REXCVAR_GET(hle_capture)) {
    DeviceState().NoteDevice(ctx.r3.u32, mx::pm4::kEpDraw);
    SampleFetchConstantFile(ctx.r3.u32, base);
    ScoreDraw(/*indexed=*/false, ctx.r5.u32, ctx.r6.u32, ctx.r3.u32, base);
    DumpHleDraw(/*indexed=*/false, n, ctx.r4.u32, 0, ctx.r5.u32, ctx.r6.u32);
  }
  ReportDrawCounts(base);
  orig_DrawVertices(ctx, base);
}

//=============================================================================
// The state entry points.
//
// All pass-through, all recording only, all behind hle_capture except that the
// recording itself is unconditional — a shadow that only starts filling when
// the cvar is read would be missing everything set before the first draw.
//
// **No guest pointer is dereferenced speculatively.** Where a resource object
// is read (SetStreamSource, SetIndices) it is read here, in the same call where
// D3D9 reads the same fields itself, and only the resulting values are kept.
// Reading it later at draw time would be the speculative dereference that
// crashed an earlier round: the game can free a buffer without rebinding, and
// the guest arena is sparse.
//
// Signatures come from the typed decompilation of each function in
// assets/default.xex.probe.i64, not from the PC D3D9 headers — several differ.
//=============================================================================

//-----------------------------------------------------------------------------
// 0x8254B7C0 — D3DDevice_SetStreamSource(D3DDevice*, UINT StreamNumber,
//                  D3DVertexBuffer*, UINT OffsetInBytes, UINT Stride)
//
// D3DVertexBuffer is D3DResource (24 bytes) followed by its two-dword vertex
// fetch constant at +0x18: dword[0] is the base address with flags in the top
// bits, dword[1] the size. SetStreamSource's own first act is to mask that
// dword with 0x1FFFFFFF and write it into the device's fetch constant file, so
// the same mask is applied here.
//-----------------------------------------------------------------------------
REX_IMPORT(__imp__sub_8254B7C0, orig_SetStreamSource, void());
extern "C" REX_FUNC(sub_8254B7C0) {
  const uint32_t stream = ctx.r4.u32;
  const uint32_t buffer = ctx.r5.u32;
  const uint32_t offset = ctx.r6.u32;
  const uint32_t stride = ctx.r7.u32;

  if (stream < mx::pm4::kMaxStreams) {
    auto& st = DeviceState();
    st.NoteDevice(ctx.r3.u32, mx::pm4::kEpSetStreamSource);
    auto& b = st.stream[stream];
    b.seen = true;
    b.buffer_obj = buffer;
    b.offset_bytes = offset;
    b.stride = stride;
    b.bound = buffer != 0;
    if (buffer) {
      // The two dwords are a Xenos vertex fetch constant, decoded exactly as
      // Pm4Translator::CollectVertexFetches already does — dword0 is
      // {type[1:0], address[31:2]} and dword1 is {endian[1:0], size[25:2] in
      // dwords}. That decode is the validated one: it is what produced the
      // stride-28 geometry that currently reaches the screen.
      //
      // A first pass here masked dword0 with 0x1FFFFFFF, copying the mask out
      // of SetStreamSource. That mask is right for what the runtime writes
      // into its fetch constant file, but it leaves the two type bits in the
      // address.
      const uint32_t d0 = REX_LOAD_U32(buffer + 0x18);
      const uint32_t d1 = REX_LOAD_U32(buffer + 0x1C);
      b.fetch_type = d0 & 0x3;
      b.address = d0 & ~0x3u;
      b.endian = d1 & 0x3;
      b.size_bytes = ((d1 >> 2) & 0xFFFFFF) * 4;

      // Stage 0: remember stream 0's raw dwords so the next draw can look for
      // them on the device and locate the fetch constant file.
      if (stream == 0) {
        g_lastBindD0 = d0;
        g_lastBindD1 = d1;
        g_lastBindOffset = offset;
        g_haveBind = true;
      }
    } else {
      b.address = 0;
      b.size_bytes = 0;
      b.endian = 0;
      b.fetch_type = 0;
    }
    g_drawsSinceBind[stream] = 0;
  }

  orig_SetStreamSource(ctx, base);
}

//-----------------------------------------------------------------------------
// 0x8254B8E0 — D3DDevice_SetIndices(D3DDevice*, D3DIndexBuffer*)
//
// One argument; the 360 API has no BaseVertexIndex here. D3DIndexBuffer is
// D3DResource plus Address at +0x18 and Size at +0x1C.
//
// **The index width is bit 31 of Common (+0x00), not a separate field.**
// DrawIndexedVertices branches on `if (*pIndexBuffer < 0)` — a signed test of
// that dword — and multiplies StartIndex by 4 on that side against 2 on the
// other.
//-----------------------------------------------------------------------------
REX_IMPORT(__imp__sub_8254B8E0, orig_SetIndices, void());
extern "C" REX_FUNC(sub_8254B8E0) {
  const uint32_t buffer = ctx.r4.u32;
  auto& st = DeviceState();
  st.NoteDevice(ctx.r3.u32, mx::pm4::kEpSetIndices);
  auto& ib = st.index;
  ib.seen = true;
  ib.buffer_obj = buffer;
  ib.bound = buffer != 0;
  if (buffer) {
    ib.common = REX_LOAD_U32(buffer + 0x00);
    // **Not masked with 0x1FFFFFFF.** D3D9 applies that mask itself
    // (`rlwinm r11, r11, 0, 3, 31` in DrawIndexedVertices) because the GPU
    // needs a *physical* address — but every read on this side goes through the
    // guest's *virtual* space, where the buffer lives at the unmasked address.
    // Masking relocated it: an index buffer at 0xF3B64000 was recorded as
    // 0x13B64000, and reading there faulted at 0x1D00B000 in three separate
    // runs before the cause was found.
    //
    // The vertex path never had this bug because it uses `& ~3` — keeping the
    // high bits and clearing only the fetch constant's two type bits.
    //
    // The "index buffer holds its range 66,726/66,726" result did not catch it,
    // and could not: that check compares a count against Size and never
    // dereferences Address, so a relocated address passes it every time.
    ib.address = REX_LOAD_U32(buffer + 0x18);
    ib.size_bytes = REX_LOAD_U32(buffer + 0x1C);
    ib.is_32bit = (ib.common & 0x80000000u) != 0;
  } else {
    ib.common = 0;
    ib.address = 0;
    ib.size_bytes = 0;
    ib.is_32bit = false;
  }

  orig_SetIndices(ctx, base);
}

//-----------------------------------------------------------------------------
// 0x825508A8 / 0x825506E8 — SetVertexShader / SetPixelShader(D3DDevice*, ptr)
//
// Handles only this round. Translating the microcode behind them is the next
// step, and recording the handle is what makes it possible to tell how many
// distinct shaders the population actually uses.
//-----------------------------------------------------------------------------
// Stage 3b — where a bound shader's microcode lives, read out of the accessors
// in gpu.obj that reach it (dis.py prints addi's operands swapped; these are
// corrected):
//
//   Promote(D3DVertexShader*)   = blr           ; the handle IS the CVertexShader
//   SH_pPhysical(this)          = *(this + 0x20)
//   GetUCodeHeader()            = this + 0x368
//   GetUCode(i)                 = this + *(this + (i + 0x70)*8) + 0x368
//   GetPhysicalMicrocode(i)     = *(variant + 0x368) + *(this + 0x20)
//   GetPhysicalMicrocodeSize(i) = *(variant + 0x36C)
//
// So the object carries a table of *patched* microcode variants — which is what
// PatchVertexShaderToMatchVertexDeclaration has been writing all along — and the
// bytes sit at a physical base plus an offset out of the header.
//
// **The physical base is not dereferenced here.** `SH_pPhysical` is exactly the
// kind of address that cost four access violations: D3D9 keeps it masked for the
// GPU, and every read on this side goes through the guest's virtual space. This
// dumps the object's own fields only, so the next step can be decided from what
// they contain rather than from a guess about which space they are in.
uint32_t g_vsDumped = 0;

void DumpVertexShaderObject(uint32_t handle, uint8_t* base) {
  if (!handle || g_vsDumped >= 6) return;
  if (!HostPageReadable(REX_RAW_ADDR(handle)) ||
      !HostPageReadable(REX_RAW_ADDR(handle + 0x380))) return;
  ++g_vsDumped;
  auto& f = DeclFile();
  f << "VERTEX SHADER 0x" << std::hex << handle << ":\n    +0x00..0x40:";
  for (uint32_t o = 0; o < 0x40; o += 4) {
    f << " [" << o << "]=0x" << REX_LOAD_U32(handle + o);
  }
  f << "\n    header +0x360..0x380:";
  for (uint32_t o = 0x360; o < 0x380; o += 4) {
    f << " [" << o << "]=0x" << REX_LOAD_U32(handle + o);
  }
  f << "\n    SH_pPhysical=0x" << REX_LOAD_U32(handle + 0x20)
    << " ucode_offset=0x" << REX_LOAD_U32(handle + 0x368)
    << " ucode_size=0x" << REX_LOAD_U32(handle + 0x36C) << std::dec << "\n";

  // The field's top bits are set (0xFD62A000), which is the *unmasked* form —
  // the same shape the vertex buffer's 0xFD21C003 has before D3D9 masks it to
  // 0x1D21D003 for the fetch constant. So this should be readable as-is, which
  // is exactly the thing four access violations were caused by getting wrong.
  // Page-guarded, and 16 dwords only: if it is microcode the first words will
  // decode as one, and if it is not, that is the finding.
  const uint32_t phys = REX_LOAD_U32(handle + 0x20);
  if (phys && HostPageReadable(REX_RAW_ADDR(phys)) &&
      HostPageReadable(REX_RAW_ADDR(phys + 0x3C))) {
    f << "    ucode @0x" << std::hex << phys << ":";
    for (uint32_t o = 0; o < 0x40; o += 4) f << " " << REX_LOAD_U32(phys + o);
    f << std::dec << "\n";
  } else {
    f << "    ucode @0x" << std::hex << phys << std::dec
      << " NOT READABLE in guest virtual space\n";
  }
  // The physical base reads as sixteen zero dwords — safe, and empty. So the
  // code is not behind that pointer at bind time. CreateVertexShader copies the
  // token stream to `this + 0x368` for *(source + 4) bytes, and +0x36C here is
  // 0x200, so the object very likely carries the microcode inline. Dumped so the
  // next step can compare it against what the ring carried for the same shader,
  // which is a cross-check that actually exists.
  if (HostPageReadable(REX_RAW_ADDR(handle + 0x468))) {
    f << "    inline +0x368:";
    for (uint32_t o = 0x368; o < 0x3E8; o += 4) f << " " << std::hex
                                                  << REX_LOAD_U32(handle + o);
    f << std::dec << "\n";
  }
  f.flush();
}

REX_IMPORT(__imp__sub_825508A8, orig_SetVertexShader, void());
extern "C" REX_FUNC(sub_825508A8) {
  auto& st = DeviceState();
  st.NoteDevice(ctx.r3.u32, mx::pm4::kEpSetVertexShader);
  st.vertex_shader = ctx.r4.u32;
  st.vs_seen = true;
  if (REXCVAR_GET(hle_capture)) {
    DumpVertexShaderObject(ctx.r4.u32, base);
    CollectVertexShaderBlob(ctx.r4.u32, base);
  }
  orig_SetVertexShader(ctx, base);
}

REX_IMPORT(__imp__sub_825506E8, orig_SetPixelShader, void());
extern "C" REX_FUNC(sub_825506E8) {
  auto& st = DeviceState();
  st.NoteDevice(ctx.r3.u32, mx::pm4::kEpSetPixelShader);
  st.pixel_shader = ctx.r4.u32;
  st.ps_seen = true;
  orig_SetPixelShader(ctx, base);
}

//-----------------------------------------------------------------------------
// 0x8254E748 — D3DDevice_SetTexture(D3DDevice*, DWORD Sampler,
//                                   D3DBaseTexture*)
//
// 307 call sites, the most-called entry point in the set. The texture object
// itself is not read: unlike a vertex buffer it is not two dwords, and nothing
// this round does with it would justify the reads.
//-----------------------------------------------------------------------------
REX_IMPORT(__imp__sub_8254E748, orig_SetTexture, void());
extern "C" REX_FUNC(sub_8254E748) {
  const uint32_t sampler = ctx.r4.u32;
  if (sampler < mx::pm4::kMaxSamplers) {
    auto& st = DeviceState();
    st.NoteDevice(ctx.r3.u32, mx::pm4::kEpSetTexture);
    st.texture[sampler] = ctx.r5.u32;
    st.texture_seen_mask |= 1u << sampler;
  }
  orig_SetTexture(ctx, base);
}

//-----------------------------------------------------------------------------
// 0x8254BF50 — D3DDevice_SetViewport(D3DDevice*, const D3DVIEWPORT9*)
//
// Six dwords: X, Y, Width, Height as integers then MinZ, MaxZ as floats. The
// struct is read here because the function reads all six itself on the next
// instruction.
//-----------------------------------------------------------------------------
REX_IMPORT(__imp__sub_8254BF50, orig_SetViewport, void());
extern "C" REX_FUNC(sub_8254BF50) {
  const uint32_t p = ctx.r4.u32;
  if (p) {
    auto& st = DeviceState();
    st.NoteDevice(ctx.r3.u32, mx::pm4::kEpSetViewport);
    auto& v = st.viewport;
    v.x = REX_LOAD_U32(p + 0);
    v.y = REX_LOAD_U32(p + 4);
    v.width = REX_LOAD_U32(p + 8);
    v.height = REX_LOAD_U32(p + 12);
    const uint32_t min_bits = REX_LOAD_U32(p + 16);
    const uint32_t max_bits = REX_LOAD_U32(p + 20);
    std::memcpy(&v.min_z, &min_bits, 4);
    std::memcpy(&v.max_z, &max_bits, 4);
    v.seen = true;
    // Every distinct extent, not just the last one. The shadow is
    // last-write-wins, and the first Stage F run read 65535x65535 out of it —
    // which built a nonsense viewport inverse and made the "window-like" test
    // accept almost any position. Whether that is the only viewport this title
    // sets or merely the most recent one is the difference between a wrong
    // read and a wrong *model*, and a single value cannot say which.
    ++g_viewportExtents[(uint64_t(v.width) << 32) | v.height];
  }
  orig_SetViewport(ctx, base);
}

//-----------------------------------------------------------------------------
// 0x8254B678 — D3DDevice_SetScissorRect(D3DDevice*, const RECT*)
//-----------------------------------------------------------------------------
REX_IMPORT(__imp__sub_8254B678, orig_SetScissorRect, void());
extern "C" REX_FUNC(sub_8254B678) {
  const uint32_t p = ctx.r4.u32;
  if (p) {
    auto& st = DeviceState();
    st.NoteDevice(ctx.r3.u32, mx::pm4::kEpSetScissorRect);
    auto& s = st.scissor;
    s.left = static_cast<int32_t>(REX_LOAD_U32(p + 0));
    s.top = static_cast<int32_t>(REX_LOAD_U32(p + 4));
    s.right = static_cast<int32_t>(REX_LOAD_U32(p + 8));
    s.bottom = static_cast<int32_t>(REX_LOAD_U32(p + 12));
    s.seen = true;
  }
  orig_SetScissorRect(ctx, base);
}

//-----------------------------------------------------------------------------
// The eight D3DDevice_SetRenderState_* leaves.
//
// All (D3DDevice*, DWORD Value) — confirmed on ZEnable's decompilation, and
// they are generated from one template so the rest follow.
//
// Only these eight were matched uniquely. The other ~90 leaves in state.obj are
// 20-56 bytes with no relocations and several are byte-identical to each other,
// so a byte match on them would not be an identification. These eight are the
// output-merger states the renderer needs.
//
// BlendFactor has **zero call sites** in this title. It is hooked anyway so
// that "never called" stays a measured fact.
//-----------------------------------------------------------------------------
#define MX_RENDER_STATE_HOOK(addr_sym, orig_name, state_id)              \
  REX_IMPORT(__imp__##addr_sym, orig_name, void());                      \
  extern "C" REX_FUNC(addr_sym) {                                        \
    auto& st = DeviceState();                                            \
    st.NoteDevice(ctx.r3.u32, mx::pm4::kEpSetRenderState);             \
    st.render_state.Set(state_id, ctx.r4.u32);                           \
    orig_name(ctx, base);                                                \
  }

MX_RENDER_STATE_HOOK(sub_82549AD8, orig_RsZEnable, mx::pm4::kRsZEnable)
MX_RENDER_STATE_HOOK(sub_82549448, orig_RsAlphaBlendEnable,
                     mx::pm4::kRsAlphaBlendEnable)
MX_RENDER_STATE_HOOK(sub_82549568, orig_RsSrcBlend, mx::pm4::kRsSrcBlend)
MX_RENDER_STATE_HOOK(sub_825495F8, orig_RsDestBlend, mx::pm4::kRsDestBlend)
MX_RENDER_STATE_HOOK(sub_825494D8, orig_RsBlendOp, mx::pm4::kRsBlendOp)
MX_RENDER_STATE_HOOK(sub_8254A078, orig_RsColorWriteEnable,
                     mx::pm4::kRsColorWriteEnable)
MX_RENDER_STATE_HOOK(sub_825497D8, orig_RsSeparateAlphaBlendEnable,
                     mx::pm4::kRsSeparateAlphaBlendEnable)
MX_RENDER_STATE_HOOK(sub_82549900, orig_RsBlendFactor, mx::pm4::kRsBlendFactor)

#undef MX_RENDER_STATE_HOOK
