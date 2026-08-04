#pragma once

// A shadow of the guest's D3D9 device state, fed by the entry-point hooks.
//
// The point is to arrive at each draw knowing everything the draw needs, from
// the layer that actually has it. The PM4 path reconstructs the same
// information from command-buffer writes and shader microcode; this reads it
// off the API calls that produced those writes.
//
// Two rules this file exists to enforce:
//
//   1. **Nothing here dereferences guest memory.** It is plain host data with
//      plain setters. The hooks read the guest — at the moment D3D9 itself is
//      reading the same bytes, so the pointers are known good — and hand the
//      values over. An earlier round crashed the guest by speculatively
//      dereferencing a device field, and the arena really is sparse.
//
//   2. **Every field records whether it was ever set.** State established
//      before the hooks were reached, or simply never set by this title, must
//      read as unknown rather than as zero. Zero is a legal blend factor and a
//      legal shader handle; "not seen" is not.
//
// Header stays free of the SDK, d3d12.h and dxgi, like shader_ucode.h.

#include <cstdint>

namespace mx::pm4 {

constexpr uint32_t kMaxStreams  = 4;
constexpr uint32_t kMaxSamplers = 16;

//===========================================================================
// Vertex streams.
//
// D3DDevice_SetStreamSource(pDevice, StreamNumber, pStreamData, OffsetInBytes,
// Stride) — signature read off the typed decompilation at 0x8254B7C0.
//
// The D3DVertexBuffer carries a Xenos vertex fetch constant at +0x18 — the
// same two dwords the PM4 path already shadows, so the two descriptions of a
// draw can be compared field by field. Decoded the way the translator decodes
// them (`xe_gpu_vertex_fetch_t`, xenos.h:1104): dword0 is {type[1:0],
// address[31:2]}, dword1 is {endian[1:0], size[25:2] in dwords}.
//
// Captured when SetStreamSource is called, *not* at draw time: the function
// reads those same dwords on its very next instruction, so the pointer is known
// good there. A buffer the game freed without rebinding would still be a live
// pointer in our shadow, and reading it later is exactly the speculative
// dereference that crashed a previous round.
//===========================================================================
struct StreamBinding {
  uint32_t buffer_obj   = 0;   // guest D3DVertexBuffer*
  uint32_t address      = 0;   // dword0 & ~3
  uint32_t size_bytes   = 0;   // ((dword1 >> 2) & 0xFFFFFF) * 4
  uint32_t endian       = 0;   // dword1 & 3
  uint32_t fetch_type   = 0;   // dword0 & 3; 3 = vertex fetch
  uint32_t offset_bytes = 0;   // the OffsetInBytes argument
  uint32_t stride       = 0;   // the Stride argument
  bool     bound        = false;  // false after SetStreamSource(.., nullptr, ..)
  bool     seen         = false;
};

//===========================================================================
// Index buffer.
//
// D3DDevice_SetIndices(pDevice, pIndexData) at 0x8254B8E0 — one argument, no
// BaseVertexIndex, unlike the PC API.
//
// The 16-vs-32-bit choice is **bit 31 of the object's Common dword**, not a
// separate field: DrawIndexedVertices branches on `if (*pIndexBuffer < 0)` and
// takes `4 * StartIndex` on that side against `2 * StartIndex` on the other.
//===========================================================================
struct IndexBinding {
  uint32_t buffer_obj = 0;     // guest D3DIndexBuffer*
  uint32_t address    = 0;     // +0x18
  uint32_t size_bytes = 0;     // +0x1C
  uint32_t common     = 0;     // +0x00, kept whole so the endian bits survive
  bool     is_32bit   = false; // Common bit 31
  bool     bound      = false;
  bool     seen       = false;
};

struct ViewportState {
  uint32_t x = 0, y = 0, width = 0, height = 0;
  float    min_z = 0.0f, max_z = 1.0f;
  bool     seen = false;
};

struct ScissorState {
  int32_t left = 0, top = 0, right = 0, bottom = 0;
  bool    seen = false;
};

//===========================================================================
// Render states.
//
// Only the eight output-merger leaves that were matched uniquely from
// state.obj. There are around a hundred of them and most are 20-56 bytes with
// no relocations, so a byte match on the rest would not be an identification —
// see the caveat in AGENTS.md. These eight are what the renderer needs to stop
// guessing at blending and depth.
//
// SetRenderState_BlendFactor has **zero call sites** in this title. It is
// carried anyway so that the "never set" case is a measured fact rather than a
// gap in the shadow.
//===========================================================================
enum RenderStateId : uint32_t {
  kRsZEnable = 0,
  kRsAlphaBlendEnable,
  kRsSrcBlend,
  kRsDestBlend,
  kRsBlendOp,
  kRsColorWriteEnable,
  kRsSeparateAlphaBlendEnable,
  kRsBlendFactor,
  kRenderStateCount,
};

const char* RenderStateName(uint32_t id);

//===========================================================================
// Which entry point a device pointer was seen at.
//
// A first run found **five** distinct pointers in r3 across these functions,
// where every earlier capture had shown one (0x40BC5F80, across 170,000
// draws). Five pointers folded into one shadow is five sets of state
// interleaved, which is enough on its own to explain draws whose bound stride
// does not match their declaration. Recording which entry points touch which
// pointer is what decides whether these are real devices, per-thread command
// contexts, or something else — a bare count decides nothing.
//===========================================================================
enum EntryPointId : uint32_t {
  kEpDraw = 0,
  kEpSetStreamSource,
  kEpSetIndices,
  kEpSetVertexShader,
  kEpSetPixelShader,
  kEpSetTexture,
  kEpSetViewport,
  kEpSetScissorRect,
  kEpSetRenderState,
  kEntryPointCount,
};

const char* EntryPointName(uint32_t id);

struct RenderStateShadow {
  uint32_t value[kRenderStateCount] = {};
  uint32_t seen_mask = 0;   // bit per RenderStateId

  bool Seen(uint32_t id) const { return (seen_mask & (1u << id)) != 0; }
  void Set(uint32_t id, uint32_t v) {
    value[id] = v;
    seen_mask |= 1u << id;
  }
};

//===========================================================================
// The whole shadow.
//
// One instance. This title creates a single D3DDevice — every capture so far
// shows the same 0x40BC5F80 in r3 across 170,000 draws — so a per-device map
// would be structure without evidence behind it. If a second device ever
// appears the draw hook will see a different pointer and say so.
//===========================================================================
struct D3D9DeviceState {
  // Every distinct r3 the entry points have been called with. A first run said
  // only "more than one", which is a label rather than a finding — the
  // pointers themselves say whether this is a second device, a per-thread
  // command context, or D3D9 calling its own entry points with a different
  // `this`.
  static constexpr uint32_t kMaxDevices = 8;
  uint32_t device_ptr[kMaxDevices] = {};
  uint32_t device_call_mask[kMaxDevices] = {};  // bit per EntryPointId
  uint64_t device_calls[kMaxDevices] = {};
  uint32_t device_count = 0;
  uint32_t device_overflow = 0;   // distinct pointers past kMaxDevices

  StreamBinding stream[kMaxStreams];
  IndexBinding  index;

  uint32_t vertex_shader = 0;
  uint32_t pixel_shader  = 0;
  bool     vs_seen = false;
  bool     ps_seen = false;

  uint32_t texture[kMaxSamplers] = {};
  uint32_t texture_seen_mask = 0;

  ViewportState     viewport;
  ScissorState      scissor;
  RenderStateShadow render_state;

  // Set by the PatchVertexShaderToMatchVertexDeclaration hook; -1 until a
  // declaration this shadow watched being created comes through.
  int current_decl = -1;

  // Notes which device pointer the entry points are being called with, so a
  // second device shows up as a fact instead of as silently mixed state.
  void NoteDevice(uint32_t d, uint32_t entry_point) {
    if (!d) return;
    for (uint32_t i = 0; i < device_count; ++i) {
      if (device_ptr[i] != d) continue;
      device_call_mask[i] |= 1u << entry_point;
      ++device_calls[i];
      return;
    }
    if (device_count < kMaxDevices) {
      const uint32_t i = device_count++;
      device_ptr[i] = d;
      device_call_mask[i] = 1u << entry_point;
      device_calls[i] = 1;
    } else {
      ++device_overflow;
    }
  }
};

// The single shadow instance.
D3D9DeviceState& DeviceState();

//===========================================================================
// How many draws the D3D9 entry points have seen, cumulative.
//
// Lives here rather than in the hook file so the swap hook can read it without
// reaching into another translation unit's anonymous namespace. It exists for
// one question: does the ring carry the same number of draws per frame that
// D3D9 was asked for?
//
// If it does, the Nth ring draw is the Nth D3D9 draw, and the shader the ring
// bound for it is the shader this handle means — which is the only remaining
// route from a D3D9 shader handle to its microcode, every direct one having
// been closed (the blob at +0x368 is not the code, SH_pPhysical is zeros at
// bind time and its address is not the ring key).
//
// If it does not, the correspondence is not there to be used, and saying so is
// the result. This is deliberately a *checkable* correlation rather than an
// assumed one.
//===========================================================================
uint64_t& D3D9DrawCounter();

// Indexed draws only (DrawIndexedVertices). Split out because the ring has four
// draw opcodes and a bare total cannot tell "the ring replays draws per bin"
// from "D3D9 issues draws the game never asked for" — opposite conclusions.
uint64_t& D3D9IndexedDrawCounter();

//===========================================================================
// Frame wall time, accumulated by the swap hook.
//
// Here for the same reason as the draw counters: the swap hook owns the only
// point that knows where one frame ends, and the report that needs the number
// lives in another translation unit.
//
// The pair exists so the cost of running the guest's vertex shader can be
// stated as a fraction of a frame rather than as a bare millisecond total. A
// total says nothing on its own — 400ms of interpreter is irrelevant across
// 600 frames and fatal across six.
//
// Measured swap-to-swap, so it is the frame *period* and includes whatever the
// guest and the host renderer did in between. That is deliberate: the question
// is what the interpreter costs against a real frame, not against an idle one.
//===========================================================================
uint64_t& D3D9FrameCount();
uint64_t& D3D9FrameNanos();

}  // namespace mx::pm4
