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

#include <cstdio>
#include <fstream>

namespace {

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
constexpr uint32_t kElementSize = 12;
constexpr uint32_t kMaxElements = 32;   // refuse to walk a runaway array
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

int KnownDeclId(uint32_t p) {
  if (!p) return -1;
  for (int i = 0; i < g_declCount; ++i) {
    if (g_declPtr[i] == p) return i;
  }
  return -1;
}

// Called from the CreateVertexDeclaration hook, where both pointers are valid.
void RecordDeclaration(uint32_t decl, bool has_colour, uint32_t elems) {
  if (!decl || g_declCount >= kMaxTrackedDecls) return;
  if (KnownDeclId(decl) >= 0) return;   // pointer reuse after a free
  const int id = g_declCount++;
  g_declPtr[id] = decl;
  g_declElems[id] = elems;
  g_declHasColour[id] = has_colour;
}

// The device-struct route is a dead end, confirmed twice: scanning the whole
// 0x2000 struct finds no offset holding a known declaration, and sub_82565550
// (a caller of PatchVertexShaderToMatchVertexDeclaration) reaches its state
// through an indexed relative-offset table rather than a fixed field.
//
// So the declaration is taken from PatchVertexShaderToMatchVertexDeclaration
// instead, which *receives* it as an argument on the lazy-state path. That hook
// sets g_currentDecl; draws attribute themselves to whatever it last held.
int g_currentDecl = -1;

// Called from both draw hooks.
void NoteDrawDeclaration(uint32_t device, uint8_t* base) {
  (void)device;
  (void)base;
  if (g_currentDecl < 0) {
    ++g_drawsNoDecl;
    return;
  }
  ++g_declDraws[g_currentDecl];
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
void ReportDrawCounts() {
  const uint64_t total = g_indexed_draws + g_draws;
  if ((total % kDrawReportEvery) != 0) return;
  REXLOG_INFO("d3d9: draws — DrawIndexedVertices={} DrawVertices={} total={}",
              g_indexed_draws, g_draws, total);
  ReportDeclHistogram();
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
  if (elements) {
    for (uint32_t i = 0; i < kMaxElements; ++i) {
      const uint32_t p = elements + i * kElementSize;
      if (REX_LOAD_U16(p) == 0xFF) break;
      ++n_elems;
      if (REX_LOAD_U8(p + 9) == 10) has_colour = true;  // D3DDECLUSAGE_COLOR
    }
  }
  RecordDeclaration(decl, has_colour, n_elems);

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

  if (found >= 0) g_currentDecl = found;
  ++g_patchCalls;
  orig_PatchVertexShader(ctx, base);
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
  ReportDrawCounts();
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
  NoteDrawDeclaration(ctx.r3.u32, base);
  if (n <= kMaxDrawsLogged) {
    auto& f = DeclFile();
    f << "DrawVertices #" << n << " dev=0x" << std::hex << ctx.r3.u32
      << std::dec << " prim=" << ctx.r4.u32 << " start_vertex=" << ctx.r5.u32
      << " vertex_count=" << ctx.r6.u32 << "\n";
    f.flush();
  }
  ReportDrawCounts();
  orig_DrawVertices(ctx, base);
}
