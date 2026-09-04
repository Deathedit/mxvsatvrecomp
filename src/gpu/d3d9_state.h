#pragma once

// A shadow of the guest's D3D9 device state, fed by the entry-point hooks, so a
// draw arrives knowing everything it needs from the layer that actually has it.
//
// Two rules this file exists to enforce:
//
//   1. **Nothing here dereferences guest memory.** It is plain host data with
//      plain setters; the hooks read the guest -- at the moment D3D9 itself is
//      reading the same bytes -- and hand the values over. An earlier round
//      crashed the guest by speculatively dereferencing a device field.
//
//   2. **Every field records whether it was ever set.** Zero is a legal blend
//      factor and a legal shader handle; "not seen" is not.
//
// Header stays free of the SDK, d3d12.h and dxgi, like shader_ucode.h.

#include <cstdint>

namespace mx::hle {

constexpr uint32_t kMaxStreams  = 4;
// Xenos has THIRTY-TWO texture fetch constants, not sixteen. A tfetch's
// fetch_constant_index is five bits, so a shader may name any of tf0..tf31, and
// this was silently rejecting every fetch at or above 16 as "out of range" --
// which surfaced as slot-fill failures that discarded an already-translated
// pixel shader.
//
// Confirmed two independent ways rather than assumed:
//   - xenia-edge registers run SHADER_CONSTANT_FETCH_00_0 through _31_5, i.e.
//     32 constants of 6 dwords.
//   - the guest device's own layout agrees exactly. The fetch file is at
//     device+0x480 with a 24-byte stride, and 0x480 + 32*24 = 0x780, which is
//     precisely kDeviceVsConstFile. Sixteen entries would end at 0x600 and leave
//     0x600..0x780 unexplained.
constexpr uint32_t kMaxSamplers = 32;

//===========================================================================
// Vertex streams.
//
// D3DDevice_SetStreamSource(pDevice, StreamNumber, pStreamData, OffsetInBytes,
// Stride) -- signature read off the typed decompilation at 0x8254B7C0.
//
// The D3DVertexBuffer carries a Xenos vertex fetch constant at +0x18 -- the same
// two dwords the PM4 path shadows, so the two descriptions of a draw can be
// compared field by field. Decoded the way the translator decodes them
// (xenos.h:1104): dword0 is {type[1:0], address[31:2]}, dword1 is {endian[1:0],
// size[25:2] in dwords}.
//
// Captured when SetStreamSource is called, *not* at draw time: the function
// reads those same dwords on its very next instruction, so the pointer is known
// good there. A buffer the game freed without rebinding would still be a live
// pointer in our shadow.
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
// D3DDevice_SetIndices(pDevice, pIndexData) at 0x8254B8E0 -- one argument, no
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
// state.obj. There are around a hundred and most are 20-56 bytes with no
// relocations, so a byte match on the rest would not be an identification.
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
// where every earlier capture had shown one across 170,000 draws. Five pointers
// folded into one shadow is five sets of state interleaved, which is enough on
// its own to explain draws whose bound stride does not match their declaration.
//===========================================================================
enum EntryPointId : uint32_t {
  kEpDraw = 0,
  kEpSetStreamSource,
  kEpSetIndices,
  kEpSetVertexShader,
  kEpSetPixelShader,
  kEpSetTexture,
  kEpSetRenderTarget,
  kEpSetDepthStencil,
  kEpResolve,
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

struct TextureBinding {
  uint32_t object = 0;
  uint32_t fetch[6] = {};
  bool bound = false;
  bool valid = false;
};

// Snapshot while SetRenderTarget guarantees the guest surface object is live.
// The offsets come directly from D3DDevice_SetRenderTarget/sub_8254BFD0 in
// default.xex.probe.i64: SurfaceInfo +0x18, ColorInfo +0x1C and packed extent
// +0x24. Keeping the raw words makes later format decoding auditable.
struct RenderTargetBinding {
  uint32_t object = 0;
  uint32_t surface_info = 0;
  uint32_t color_info = 0;
  uint32_t extent = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  bool bound = false;
  bool valid = false;
};

//===========================================================================
// The whole shadow.
//
// One instance. This title creates a single D3DDevice -- every capture so far
// shows the same pointer in r3 across 170,000 draws -- so a per-device map would
// be structure without evidence behind it. If a second device ever appears the
// draw hook will see a different pointer and say so.
//===========================================================================
struct D3D9DeviceState {
  // Every distinct r3 the entry points have been called with. A first run said
  // only "more than one", which is a label rather than a finding -- the pointers
  // themselves say whether this is a second device, a per-thread command
  // context, or D3D9 calling its own entry points with a different `this`.
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
  // The last NON-NULL pixel shader bound on this device.
  //
  // A null pixel shader does not mean "no pixel shader" on this hardware: the
  // guest's PM4 emitter simply emits no pixel IM_LOAD, so the GPU keeps the
  // previously loaded program and the draw inherits it. `pixel_shader` above is
  // overwritten by SetPixelShader(NULL) and cannot answer "which program is
  // actually running".
  //
  // Diagnostic only: it exists to test whether the draws that bind YUV planes
  // with a null pixel shader are inheriting one of the two Bink composite
  // shaders.
  uint32_t last_nonnull_pixel_shader = 0;
  bool     vs_seen = false;
  bool     ps_seen = false;

  TextureBinding texture[kMaxSamplers];
  uint32_t texture_seen_mask = 0;

  RenderTargetBinding render_target[4];
  RenderTargetBinding depth_stencil;
  uint32_t render_target_seen_mask = 0;

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
// How many draws the D3D9 entry points have seen, cumulative. Lives here rather
// than in the hook file so the swap hook can read it without reaching into
// another translation unit's anonymous namespace.
//
// It exists for one question: does the ring carry the same number of draws per
// frame that D3D9 was asked for? If it does, the Nth ring draw is the Nth D3D9
// draw, and the shader the ring bound for it is the shader this handle means --
// the only remaining route from a D3D9 shader handle to its microcode. If it
// does not, saying so is the result.
//===========================================================================
uint64_t& D3D9DrawCounter();

// Indexed draws only (DrawIndexedVertices). Split out because the ring has four
// draw opcodes and a bare total cannot tell "the ring replays draws per bin"
// from "D3D9 issues draws the game never asked for" — opposite conclusions.
uint64_t& D3D9IndexedDrawCounter();

//===========================================================================
// Frame wall time, accumulated by the swap hook. Here for the same reason as the
// draw counters: the swap hook owns the only point that knows where one frame
// ends, and the report that needs the number lives elsewhere.
//
// The pair exists so the cost of running the guest's vertex shader can be stated
// as a fraction of a frame rather than as a bare millisecond total -- 400ms of
// interpreter is irrelevant across 600 frames and fatal across six. Measured
// swap-to-swap, so it is the frame *period*: the question is what the
// interpreter costs against a real frame, not against an idle one.
//===========================================================================
uint64_t& D3D9FrameCount();
uint64_t& D3D9FrameNanos();

}  // namespace mx::hle
