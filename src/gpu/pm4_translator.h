#pragma once

#include "gpu/pm4_parser.h"
#include "gpu/shader_alu.h"
#include "gpu/shader_ucode.h"

#include <cstdint>
#include <cstring>
#include <map>
#include <vector>

namespace mx::pm4 {

// Mimics PrimitiveType from xenos.h (subset — only values seen in MX vs ATV).
// Fan is 5 and strip is 6, per Xenia's xenos::PrimitiveType. This enum had them
// the other way round until 2026-08-02, which would have drawn every fan as a
// strip once topology started being honoured.
enum class PrimitiveType : uint8_t {
  kPointList        = 0x01,
  kLineList         = 0x02,
  kLineStrip        = 0x03,
  kTriangleList    = 0x04,
  kTriangleFan     = 0x05,
  kTriangleStrip   = 0x06,
  kRectangleList   = 0x08,
  // 0x0C kLineLoop, 0x0E kQuadStrip and 0x0F kPolygon exist in xenos.h too but
  // are omitted here because this game emits none of them. QuadList it does
  // emit, in greater volume than any other type.
  kQuadList        = 0x0D,
  kUnknown         = 0xFF,
};

// Host topology, carried on DrawCall so the renderer stays a dumb consumer.
// The values are deliberately the D3D_PRIMITIVE_TOPOLOGY ones so the renderer
// can cast rather than translate; d3d12_game.cpp static_asserts that they still
// match. Declaring them here rather than including <d3dcommon.h> keeps this
// header usable from translator_test.cpp.
enum class HostTopology : uint32_t {
  kUndefined     = 0,
  kPointList     = 1,
  kLineList      = 2,
  kLineStrip     = 3,
  kTriangleList  = 4,
  kTriangleStrip = 5,
};

struct DrawCall {
  std::vector<uint8_t> vertices;      // optional; filled only when a vertex fetch const is known
  std::vector<uint8_t> indices;       // packed index buffer (2 or 4 bytes per index)
  uint32_t vertex_count = 0;
  uint32_t index_count = 0;
  uint32_t vertex_stride = 0;
  uint32_t prim_type = 0;            // xenos::PrimitiveType (raw 6-bit value)
  HostTopology topology = HostTopology::kUndefined;  // mapped from prim_type
  bool index_16bit = true;
  bool binned = false;                // true for DRAW_INDX_*_BIN variants
  float mvp[16] = {};
  bool valid = false;                 // set once index buffer is populated
  // Set by the transcode when skip_untransformable_draws is on and this draw's
  // positions came out degenerate, entirely out of clip, or with only some
  // vertices collapsed to the origin. Its own flag rather than reusing
  // topology == kUndefined (which would misreport the topology) or valid=false
  // (which the renderer skips without counting) — the reason and the count both
  // need to stay honest. A MITIGATION: the draw is still wrong, it is just no
  // longer drawn over the ones that are right.
  bool untransformable = false;
  // Which guest colour surface this draw targeted, from RB_COLOR_INFO /
  // RB_SURFACE_INFO at the time it was translated. A frame touches ~16 distinct
  // surfaces and the renderer has exactly one target, so without this every
  // off-screen pass overpaints the main scene. See LogSurface.
  uint32_t surface_base = 0;          // RB_COLOR_INFO[11:0], in 4KB tiles
  uint32_t surface_pitch = 0;         // RB_SURFACE_INFO[13:0]

  // Where TranscodeVertices got this draw's vertex colour, so LogSurface can
  // cross-tabulate it against the surface. Carried on the draw rather than
  // counted in place because the two facts are established at different points:
  // the colour during transcode, the surface in FinalizeDraw afterwards.
  //
  // Worth the field because draw and vertex counts proved unable to answer the
  // question they were asked. The counters said 95.7% of vertices carry a real
  // packed colour; the tint screenshot said colourless draws cover 100% of the
  // pixels. Both were right. Which surface each population lives on is the next
  // thing that can separate them.
  enum class ColorSource : uint8_t {
    // The default, and deliberately NOT kNone: TranscodeVertices has several
    // early exits (no position attribute, an unconfirmed position format) that
    // return before any colour is resolved. Folding those into kNone would
    // inflate the population this round is trying to measure. These draws are
    // never rewritten, so they keep the guest stride and the renderer's
    // stride-28 gate drops them — they reach no pixels either way.
    kNotTranscoded = 0,
    kNone,          // no colour attribute found — written opaque white
    kPacked,        // format 6 or 7, a real packed colour
    kFallback,      // first 4-component non-position attribute, i.e. a guess
  };
  ColorSource color_source = ColorSource::kNotTranscoded;

  // The guest's output-merger state as of this draw, captured raw.
  //
  // Nothing in this renderer has ever read one of these. CreateGamePipeline
  // sets a write mask and a rasterizer state and stops: BlendEnable is FALSE,
  // DepthStencilState is zeroed, and D3D12 has no alpha test to leave off. So
  // every guest draw is composited fully opaque, in draw order, writing all
  // four channels — the entire back end of the guest pipeline is missing, and
  // it has stayed invisible because it still produces plausible geometry.
  //
  // Captured rather than acted on. A draw whose shader exports only a position
  // has no colour attribute, so the transcode writes {1,1,1,1} opaque white —
  // and a depth pre-pass is exactly that draw, covering the world while writing
  // no colour at all on the guest. If that is what the 12760 colourless draws
  // are, colour_mask reads zero for them and NoteOutputMerger will say so.
  //
  // Raw dwords, not decoded fields: the decode belongs next to the counters
  // that report it, and storing raw keeps a misread visible.
  uint32_t colour_mask = 0;    // RB_COLOR_MASK    0x2104, bits 0-3 = RGBA of RT0
  uint32_t depth_control = 0;  // RB_DEPTHCONTROL  0x2200
  uint32_t blend_control = 0;  // RB_BLENDCONTROL0 0x2201
  uint32_t colour_control = 0; // RB_COLORCONTROL  0x2202, bit 3 = alpha test
  uint32_t mode_control = 0;   // RB_MODECONTROL   0x2208, bits 0-2 = edram_mode

  // Which of the five above had actually been written this frame, at the time
  // this draw was finalized. Bit i corresponds to register i in the order
  // listed. WITHOUT THIS THE VALUES ABOVE ARE UNREADABLE.
  //
  // The translator is Clear()ed once per frame (hooks_frame.cpp:212) and the
  // context shadow is memset to zero, then the frame's packets are replayed. A
  // register the guest set once at init and never re-set therefore reads zero
  // in every frame we ever look at — and a colour mask of zero is exactly the
  // finding this round is hunting for. Reading it without knowing whether
  // anyone wrote it would let the instrument manufacture its own conclusion.
  uint32_t om_seen = 0;
};

class Pm4Translator {
 public:
  Pm4Translator() = default;

  void TranslatePackets(const std::vector<Pm4Packet>& packets,
                        uint8_t* guest_base, uint32_t gpu_phys_base);

  const std::vector<DrawCall>& DrawCalls() const { return m_drawCalls; }
  void Clear() {
    m_drawCalls.clear();
    std::memset(m_fetchConsts, 0, sizeof(m_fetchConsts));
    std::memset(m_ctxRegs, 0, sizeof(m_ctxRegs));
    m_ctxWritten = false;
    m_omSeen = 0;
    m_vtxBufAddr = 0;
    m_vtxStride = 0;
    m_indexType16 = true;
    std::memset(m_mvp, 0, sizeof(m_mvp));
    m_binMaskLo = 0xFFFFFFFF;
    m_binMaskHi = 0x00000000;
    m_binSelectLo = 0xFFFFFFFF;
    m_binSelectHi = 0x00000000;
  }

 private:
  // Real Xenos draw handlers (see Xenia packet_disassembler.cc).
  // DRAW_INDX_2_BIN and DRAW_INDX_2 share the same dword0 layout:
  //   bits[31:16]=index_count, [11]=index_32bit, [10:6]=src_sel,
  //   [5:0]=prim_type. Inline indices start at body[1].
  void HandleDrawIndx2(const Pm4Packet& pkt, uint8_t* guest_base, bool binned);
  // DRAW_INDX uses body[0]=viz_query_info, body[1]=dword0 (draw header),
  // then body[2]=guest_base + body[3]=index_size|endianness for src_sel==0.
  void HandleDrawIndx(const Pm4Packet& pkt, uint8_t* guest_base, bool binned);

  // SET_CONSTANT (0x2D) — sub-dispatch via type field in body[0]:
  //   type=(body[0]>>16)&0xFF, index = body[0] & 0x7FF
  //   type=0 → ALU float constants  (0x4000 + index)
  //   type=1 → FETCH vertex fetch consts (0x4800 + index) — sets vtx buffer
  //   type=2 → BOOL constants           (0x4900 + index)
  //   type=3 → LOOP constants           (0x4908 + index)
  //   type=4 → REGISTERS                (0x2000 + index) — context regs
  void HandleSetConstant(const Pm4Packet& pkt);

  // SET_SHADER_CONSTANTS (0x56) — incremental shader constant update.
  // body[0] = base index (16-bit), body[1..] = float4 constants.
  void HandleSetShaderConstants(const Pm4Packet& pkt);

  // Type0 writes feed two shadows: the shader fetch constant file at
  // 0x4800..0x48BF, and the context register block at 0x2000..0x2FFF. This game
  // sets its vertex fetch constants in the former and never emits a
  // SET_CONSTANT (0x2D) at all, so without this the translator never learns a
  // vertex buffer address and every draw comes out with no vertices. The latter
  // carries the viewport registers BuildViewportMvp reads. A write is clipped
  // independently against each file.
  void ApplyType0Write(uint32_t reg_base, const std::vector<uint32_t>& body);

  // Read a shadowed context register as a float. Returns `fallback` when the
  // block has never been written.
  float CtxFloat(uint32_t reg, float fallback) const;

  // Build the window-space -> NDC matrix from PA_CL_VPORT_*. The guest's
  // vertices are already in window coordinates, so what we want is the inverse
  // of the hardware viewport transform `window = ndc * SCALE + OFFSET`:
  //
  //   row0 = [1/XSCALE, 0,        0,        -XOFFSET/XSCALE]
  //   row1 = [0,        1/YSCALE, 0,        -YOFFSET/YSCALE]
  //   row2 = [0,        0,        1/ZSCALE, -ZOFFSET/ZSCALE]
  //   row3 = [0,        0,        0,        1              ]
  //
  // YSCALE is negative in every frame observed, and that negation is what flips
  // window-y-down to NDC-y-up — there is no second flip anywhere. Falls back to
  // identity (and logs once) when a scale is zero or the block is unwritten,
  // rather than emitting NaNs into a constant buffer.
  //
  // Row-major. kGameVS declares its cbuffer matrix `row_major` to match; HLSL
  // would otherwise pack a float4x4 column-major and silently transpose this.
  void BuildViewportMvp(float out[16]) const;

 public:
  // Map the raw 6-bit prim_type to a host topology. kUndefined means the
  // renderer must drop the draw — RectangleList maps to kUndefined here because
  // it is not a topology but an expansion, handled by ExpandRectangleList.
  //
  // Public because the D3D9 path needs it too, and needs *this* one: a
  // D3DPRIMITIVETYPE on Xenon is the Xenos value, not a separate enum. That is
  // not an assumption — D3DDevice_DrawVertices writes the argument straight
  // into the draw initiator's low 6 bits, and indexes its own per-primitive
  // table at 0x82002B90 with it. A second mapping here would be a copy that
  // could drift from the one the renderer already agrees with.
  static HostTopology MapTopology(uint32_t prim_type);

 private:

 public:
  // Rewrite a RectangleList draw into a triangle list in place. D3D12 has no
  // rectangle topology. Each group of 3 vertices is a rectangle whose implied
  // 4th corner is v3 = v0 + v2 - v1; the synthesized vertex takes that
  // arithmetic on the leading 3 floats and copies its remaining bytes from v2.
  // Returns the number of rectangles expanded, 0 if the draw could not be.
  // Public and static for the same reason as MapTopology: the D3D9 path
  // needs the identical expansion, and neither touches instance state.
  static uint32_t ExpandRectangleList(DrawCall& dc);

  // Rewrite a QuadList draw into a triangle list in place. D3D12 has no quad
  // topology either, but a quad needs no synthesized corner: all four are
  // present, so the vertices pass through untouched and only the index buffer
  // is rebuilt, six indices per quad on the v0-v2 diagonal. Maps through the
  // incoming indices, so it is correct for auto-draws and real index buffers
  // alike. Returns the number of quads expanded, 0 if the draw could not be.
  static uint32_t ExpandQuadList(DrawCall& dc);

 private:
  // Everything a draw needs once its vertices and indices are in place:
  // topology, the viewport transform, RectangleList expansion, and the NDC log.
  // Called by all three draw paths so none of them can drift.
  void FinalizeDraw(DrawCall& dc);

  // Transform the first few vertices by dc.mvp and log the result. This is the
  // instrument that separates "the transform is wrong" from "the D3D12 state is
  // wrong": positions inside [-1,1] mean the matrix is right whatever the
  // screen shows.
  void LogNdc(const DrawCall& dc) const;
  // Read-only: counts the distinct guest colour surfaces draws target. Nothing
  // downstream consumes it — see the definition.
  void LogSurface(DrawCall& dc);
  // Read-only: how much of the viewport each draw's transformed bounding box
  // could cover, split by colour source. An UPPER BOUND — see the definition.
  void NoteDrawCoverage(const DrawCall& dc) const;
  // Read-only: what output-merger state the guest set, cross-tabulated against
  // colour source. Reports the per-register write counts first — see the
  // definition for why a distribution without them means nothing.
  void NoteOutputMerger(const DrawCall& dc) const;

  // ---- Shader microcode capture (read-only this round) --------------------
  //
  // The decoded vertex layout of one shader. Cached because the same shader is
  // reloaded many times per frame — guest address 0x1D5FF040 alone recurs ~40
  // times — and the CF walk should run once per distinct shader, not per load.
  struct ShaderLayout {
    std::vector<VertexAttribute> attrs;
    // The microcode itself, kept so the ALU interpreter can run it. Cached per
    // distinct shader alongside the decode, so this costs one copy per shader
    // rather than one per draw.
    std::vector<uint32_t> code;
    bool ok = false;
    const char* fail = nullptr;
    // Whether the shader exported to register 62 at all. Distinguishes a
    // position built entirely from constants (export seen, nothing tainted)
    // from a blob whose export we never reached.
    bool saw_position_export = false;
  };

  // IM_LOAD_IMMEDIATE (0x2B) — microcode inline in the ring, 68/frame.
  //   body[0] = shader type (0 vertex, 1 pixel)
  //   body[1] = size in dwords; the high half is a start offset into
  //             instruction memory, 0 in every packet observed so far
  //   body[2..] = microcode, already host-endian after the parser's byteswap
  void HandleImLoadImmediate(const Pm4Packet& pkt);

  // IM_LOAD (0x27) — microcode in guest memory, 357/frame.
  //   body[0] = physical address | type in [1:0]
  //   body[1] = size in dwords
  void HandleImLoad(const Pm4Packet& pkt, uint8_t* guest_base);

  // Decode once per distinct shader and remember it. `key` is the guest address
  // for IM_LOAD and a content hash for IM_LOAD_IMMEDIATE.
  const ShaderLayout* DecodeAndCacheShader(uint64_t key, const uint32_t* dwords,
                                           uint32_t count, const char* origin);

  // The heart of this round: report the stride the shader actually declares
  // beside the one AttachVertices guessed by division. Logs only — nothing
  // downstream reads the decoded value yet.
  void LogStrideComparison(const DrawCall& dc, uint32_t heuristic_stride,
                           uint32_t heuristic_slot);


  // LOAD_ALU_CONSTANT (0x2F) — 271/frame post-load, 0 at boot. Every one is a
  // 16-dword (4x4 matrix) load from guest memory into the ALU constant file.
  //   body[0] = physical address
  //   body[1] = (type << 16) | dword index into the constant file
  //   body[2] = size in dwords (0x10 for almost all; a couple carry 0x20)
  // This is the third door: AGENTS.md's "the game never writes the ALU constant
  // file" is true of SET_CONSTANT and of Type0 writes to 0x4000..0x41FF, and
  // wrong about the game. Read-only this round — shadow, probe, decide next.
  void HandleLoadAluConstant(const Pm4Packet& pkt, uint8_t* guest_base);

  // Transform a draw's first vertices by each candidate matrix in each layout
  // and report which, if any, lands in clip space. Logs only.
  void ProbeAluMatrices(const DrawCall& dc);

  static constexpr uint32_t kAluConstDwords = 2048;  // 512 vec4
  static constexpr uint32_t kAluConstBase = 0x4000;
  uint32_t m_aluConsts[kAluConstDwords] = {};
  bool m_aluWritten = false;

  // Which door an ALU constant write came through. There are three and until
  // now only one was watched, which is why the file looked empty.
  enum : int {
    kAluSourceType0 = 0,
    kAluSourceSetConstant = 1,
    kAluSourceLoad = 2,
  };
  void NoteAluConstWrite(int source, uint32_t reg_base, uint32_t dwords);

  // Histogram of what the ALU interpreter managed on real draws — status,
  // blocking opcodes, and whether the clip coordinates land inside the volume.
  void NoteAluExecution(const AluResult& r, uint32_t pos_format);
  void ProbeAluExecution(const DrawCall& dc, const VertexAttribute& pos);

  std::map<uint64_t, ShaderLayout> m_shaderCache;
  // The last vertex shader loaded before the current draw. That is how the
  // hardware binds, and the dump agrees: every 0x2B/0x27 is followed by an
  // SQ_PROGRAM_CNTL write and then draw state. Pointers into a std::map stay
  // valid across later inserts, so holding one is safe.
  const ShaderLayout* m_currentVs = nullptr;

  // One vertex fetch slot, decoded from a dword pair in the fetch file.
  struct VertexFetch {
    uint32_t slot = 0;
    uint32_t address = 0;   // guest physical byte address
    uint32_t size_bytes = 0;
    uint32_t endian = 0;
    uint32_t stride = 0;    // inferred, 0 if it failed validation
    const char* reject = nullptr;  // nullptr when accepted
  };

  // Rewrite a draw's vertices from the guest's own layout into the single
  // layout the game PSO declares — POSITION float3 @0, COLOR float4 @12,
  // stride 28 — using the layout decoded from the shader. Falls back to leaving
  // the guest layout in place, and counts why, whenever it cannot. Declared
  // after VertexFetch because a parameter type must already be visible.
  void TranscodeVertices(DrawCall& dc, const VertexFetch& fetch);

  // Walk the fetch file and decode every live vertex fetch (type == 3),
  // inferring a stride for `vertex_count` vertices. Stride is NOT in the fetch
  // constant — on Xenos it lives in the shader's vfetch instruction — so it is
  // derived as size_bytes / vertex_count and only accepted when that divides
  // exactly and lands in kStrideMin..kStrideMax.
  std::vector<VertexFetch> CollectVertexFetches(uint32_t vertex_count) const;

  // Raw-copy `bytes` from guest physical `addr` into `out` — no byteswap, the
  // caller knows its element width. Returns false (and logs why) when the
  // address is out of range or the host page is not committed. Shared by the
  // index-buffer and vertex-buffer paths — the commit probe is the reason
  // neither hangs on an uncommitted page.
  static bool ReadGuestRange(uint8_t* guest_base, uint32_t addr, uint32_t bytes,
                             std::vector<uint8_t>& out, const char* what);

  // Fill dc.vertices from the best candidate slot, or leave it empty. Logs the
  // candidate field for the first draws so the selection rule can be judged
  // against what the game actually writes.
  void AttachVertices(DrawCall& dc, uint8_t* guest_base);

  // Empty stubs for state-tracking opcodes we want to log/skip:
  void HandleBinMaskLo(const Pm4Packet& pkt);
  void HandleBinMaskHi(const Pm4Packet& pkt);
  void HandleBinSelectLo(const Pm4Packet& pkt);
  void HandleBinSelectHi(const Pm4Packet& pkt);

  // Shader fetch constant file, registers 0x4800..0x48BF. Vertex fetches
  // occupy 2 dwords, texture fetches 6; the type field in dword0[1:0]
  // distinguishes them (3 = vertex, 2 = texture).
  static constexpr uint32_t kFetchConstBase = 0x4800;
  static constexpr uint32_t kFetchConstCount = 192;
  static constexpr uint32_t kStrideMin = 8;
  static constexpr uint32_t kStrideMax = 64;
  uint32_t m_fetchConsts[kFetchConstCount] = {};

  // Context register block 0x2000..0x2FFF. 16KB, shadowed wholesale rather than
  // cherry-picked because the interesting registers keep turning out to be ones
  // we had not thought to keep.
  static constexpr uint32_t kCtxRegBase = 0x2000;
  static constexpr uint32_t kCtxRegCount = 0x1000;
  // PA_CL_VPORT_* — the tail of the cnt=21 Type0 write to 0x2100.
  static constexpr uint32_t kRegVportXScale  = 0x210F;
  static constexpr uint32_t kRegVportXOffset = 0x2110;
  static constexpr uint32_t kRegVportYScale  = 0x2111;
  static constexpr uint32_t kRegVportYOffset = 0x2112;
  // SQ_VS_CONST / SQ_PS_CONST — the base and size, in vec4, of the region of
  // the 512-vec4 ALU constant file that a shader's c[n] is relative to. Both
  // are inside the shadowed context range and nothing has ever read them;
  // Const() in shader_alu.cpp indexes the file absolutely. If the base is
  // non-zero every constant read in every shader is off by it.
  static constexpr uint32_t kRegVsConst = 0x2307;
  static constexpr uint32_t kRegPsConst = 0x2308;

  // The output-merger block. Numbers taken from the SDK's own register table,
  // rex/graphics/register_table.inc lines 511 and 542-548, not from memory; bit
  // layouts are the unions in rex/graphics/registers.h (RB_COLOR_MASK:754,
  // RB_BLENDCONTROL:779, RB_COLORCONTROL:681, RB_MODECONTROL:659).
  static constexpr uint32_t kRegColorMask    = 0x2104;
  static constexpr uint32_t kRegDepthControl = 0x2200;
  static constexpr uint32_t kRegBlendControl = 0x2201;
  static constexpr uint32_t kRegColorControl = 0x2202;
  static constexpr uint32_t kRegModeControl  = 0x2208;

  // How many Type0 writes have actually covered each of the five registers
  // above, in the order listed.
  //
  // This is not decoration. CtxDword returns its fallback for a register the
  // game never wrote, and a colour mask reading zero because nothing set it is
  // indistinguishable from one deliberately set to zero — except that the first
  // makes *every* draw look like a depth pre-pass, which is precisely the
  // conclusion this round is trying to reach. So the counts get reported before
  // the distributions, and a zero here voids the table below it.
  static constexpr int kOmRegCount = 5;
  uint64_t m_omRegWrites[kOmRegCount] = {};
  // Bit i set once register i has been written in the current frame. Reset by
  // Clear(), unlike m_omRegWrites which accumulates across the whole run —
  // "has the guest ever set this" and "had it set it before this draw" are
  // different questions and both need answering.
  uint32_t m_omSeen = 0;

  // Raw shadowed context register. CtxFloat reinterprets as float, which is
  // right for the viewport but wrong for a bitfield like SQ_VS_CONST.
  uint32_t CtxDword(uint32_t reg, uint32_t fallback = 0) const;

  // Logs SQ_VS_CONST / SQ_PS_CONST, on change only. Called per draw.
  void LogShaderConstBases() const;

  // How a transcoded draw's positions came out, judged after the viewport
  // transform the renderer will apply. A draw that is degenerate or entirely
  // outside the clip volume cannot render correctly; submitting it anyway is
  // what smears the frame white over the geometry that is right.
  enum class DrawClass {
    kPartial,      // at least one vertex lands, and nothing looks broken
    kDegenerate,   // every vertex at the origin
    kOutOfRange,   // nothing lands anywhere near the clip volume
    // Some vertices collapsed to exactly the origin and others did not. Real
    // geometry does not put a handful of vertices at exactly (0,0,0) and leave
    // the rest alone — this is a transform that failed for part of the draw,
    // and it is the shape that stretches a triangle from the corner across the
    // whole frame. The two clean cases above turned out to be only 7.5% of
    // draws and 1.5% of vertices, far too little to explain a white screen, so
    // this is where the smear actually comes from.
    kMixedOrigin,
  };
  static constexpr int kDrawClassCount = 4;
  // `verts` is the transcoded buffer: kOutStride bytes per vertex, position in
  // the leading 12. Judged against BuildViewportMvp, the same transform
  // FinalizeDraw will attach to the draw.
  DrawClass ClassifyTransformedDraw(const std::vector<uint8_t>& verts,
                                    uint32_t count, uint32_t stride) const;
  static constexpr uint32_t kRegVportZScale  = 0x2113;
  static constexpr uint32_t kRegVportZOffset = 0x2114;
  uint32_t m_ctxRegs[kCtxRegCount] = {};
  bool m_ctxWritten = false;

  uint32_t m_vtxBufAddr = 0;
  uint32_t m_vtxStride = 0;
  bool m_indexType16 = true;
  float m_mvp[16] = {};
  // Bin mask/select registers — written by SET_BIN_MASK_LO/HI + SET_BIN_SELECT_LO/HI.
  uint32_t m_binMaskLo = 0xFFFFFFFF;
  uint32_t m_binMaskHi = 0x00000000;
  uint32_t m_binSelectLo = 0xFFFFFFFF;
  uint32_t m_binSelectHi = 0x00000000;
  std::vector<DrawCall> m_drawCalls;
};

}  // namespace mx::pm4