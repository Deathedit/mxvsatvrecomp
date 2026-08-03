#include "gpu/pm4_translator.h"

#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <system_error>

#include <rex/cvar.h>
#include <rex/logging.h>

// Pick the vertex fetch slot the bound shader names, instead of the lowest
// validated one. Off restores the pre-2026-08-02 tie-break, so the two can be
// compared on the same build.
// Rewrite guest vertices into the PSO's fixed pos3+color4 layout. Off leaves
// the guest layout alone, so the renderer's stride-28 gate keeps rejecting
// everything else, as it did before.
REXCVAR_DEFINE_BOOL(vertex_transcode, true, "Debug",
                    "Convert guest vertices into the POSITION float3 + COLOR "
                    "float4 layout the game PSO declares");

// Applies only to positions picked by the offset/format *guess*. Transcoding
// every format that rule accepts raised submitted draws from 97 to 282 and
// turned the window entirely white: the rule is wrong for the formats it was
// never confirmed on, and k_32_32_32_FLOAT is the only one a real vertex dump
// ever confirmed.
//
// A position the shader itself identified — traced from the fetch destination
// to the register 62 export — is not a guess and is not restricted by this. That
// is the whole point of the export decode; see the gate in TranscodeVertices.
REXCVAR_DEFINE_BOOL(transcode_confirmed_formats_only, true, "Debug",
                    "For guessed positions only, transcode only k_32_32_32_FLOAT "
                    "— the one format confirmed against a real vertex dump");

// Default OFF until the probe histogram says the interpreter handles the
// shaders this game actually submits. With it off the interpreter still runs on
// a 1-in-64 sample of draws, read-only, purely to produce that histogram — so
// one run answers "can it execute these shaders" and "what would it draw"
// without the second question contaminating the first.
REXCVAR_DEFINE_BOOL(alu_execute, false, "Debug",
                    "Compute the vertex position by executing the shader's ALU "
                    "instead of passing the fetched attribute through the "
                    "viewport inverse");

// A MITIGATION, NOT A FIX. These draws are still transformed wrongly; this only
// stops them being drawn on top of the ones that come out right. Nothing here
// repairs the underlying defect, and a screenshot taken with this on must be
// read with that in mind.
//
// Worth doing because of the ratio: the draws it removes are 28.1% of all draws
// but only 1.9% of all vertices. The mixed-origin class dominates it — 2999
// draws averaging 3.0 vertices each, i.e. single triangles with one corner
// pinned at the origin, which is precisely the streak fan that smears the frame
// white. The bulk terrain (2.3M vertices) is untouched.
REXCVAR_DEFINE_BOOL(skip_untransformable_draws, false, "Debug",
                    "Do not submit draws whose transcoded positions come out "
                    "degenerate, entirely outside the clip volume, or with only "
                    "some vertices collapsed to the origin. A mitigation, not a "
                    "fix: these draws are wrong and this stops them painting "
                    "over the ones that are right");

// The A/B knob for the export decode. Off reverts to the pure guess, so the
// two can be compared on one build rather than across two.
REXCVAR_DEFINE_BOOL(transcode_trust_export, true, "Debug",
                    "Take the position attribute from the shader's register-62 "
                    "export trace, in whatever format it declares");

REXCVAR_DEFINE_BOOL(vfetch_use_shader_slot, true, "Debug",
                    "Choose the vertex fetch slot from the shader's vfetch "
                    "instruction rather than taking the lowest one that "
                    "validated");

namespace mx::pm4 {

#if defined(_WIN32)
// Probe the host-side page state — ReXGlue reserves 512MB up-front but only
// commits pages that guest code explicitly Alloc()'d. The plugin's GPU
// command processor keeps its own IB allocations in Xenos physical RAM
// (0x1xxxxxxx range) that are reserved-but-not-committed in our host view.
// We commit on-demand so memcpy can read them.
extern "C" __declspec(dllimport) void* __stdcall VirtualAlloc(
    void* addr, size_t size, unsigned int allocation_type, unsigned int protect);
extern "C" __declspec(dllimport) int __stdcall VirtualQuery(
    const void* addr, void* buffer, unsigned long length);
struct MemoryBasicInformationLite {
  void*  base_address;
  void*  allocation_base;
  unsigned int allocation_protect;
  unsigned __int64 region_size;
  unsigned int state;
  unsigned int protect;
  unsigned int type;
};
// MEM_COMMIT=0x1000, MEM_RESERVE=0x2000, PAGE_READWRITE=0x04
#endif

namespace {

// Xbox 360 guest physical memory maps 1:1 from 0..0x20000000 (512MB main
// RAM). GPU-side DRAW_INDX external index buffer addresses are guest
// physical addresses in this range (or above, into physical heaps). The
// translation to a host pointer is therefore just `guest_mem_base + addr`
// (no masking, no GpuAlloc-windowing — the game is free to place IBs
// anywhere in main RAM). ReXGlue's `REX_LOAD_U16/U32` macros already
// apply big-endian → host-LE byte swap, but raw memcpy from base+addr
// yields big-endian bytes — so we manually byteswap 16/32-bit indices
// after the copy. See AGENTS.md "PM4 Translator GPU memory translation".
constexpr uint32_t kGuestPhysMax = 0x20000000;  // 512MB Xbox 360 main RAM.

// Per Xenia's packet_disassembler.cc PM4_DRAW_INDX_2 / 2_BIN header layout:
//   body[0] bits[31:16] = index_count
//   body[0] bit  [11]    = index_32bit (1=32-bit indices, 0=16-bit)
//   body[0] bits[10:6]   = src_sel (must == 2 "AutoIndex" for inline indices)
//   body[0] bits [5:0]   = prim_type (xenos::PrimitiveType)
struct DrawIndx2Header {
  uint32_t index_count;
  uint32_t prim_type;
  uint32_t src_sel;
  bool index_32bit;
  bool valid;
};

DrawIndx2Header ParseDrawIndx2Header(uint32_t dword0) {
  DrawIndx2Header h{};
  h.index_count  = dword0 >> 16;
  h.prim_type    = dword0 & 0x3F;
  h.src_sel      = (dword0 >> 6) & 0x3;
  h.index_32bit  = (dword0 >> 11) & 0x1;
  h.valid        = (h.index_count > 0 && h.index_count < 0x100000 &&
                    (h.src_sel == 0x2 || h.src_sel == 0x0));
  return h;
}

}  // namespace

namespace {

// Copy the overlap of a Type0 write [reg_base, reg_base+len) with a shadowed
// register file [file_base, file_base+file_len). Writes arrive with counts of
// 6 (a single fetch slot), 21 (the viewport block) and 186 (the whole texture
// block), so a write that only partly overlaps a file is normal, not an error.
// Returns true if anything was copied.
bool ClipWriteInto(uint32_t* file, uint32_t file_base, uint32_t file_len,
                   uint32_t reg_base, const std::vector<uint32_t>& body) {
  const uint32_t file_end = file_base + file_len;
  if (reg_base >= file_end) return false;
  const uint32_t write_end = reg_base + static_cast<uint32_t>(body.size());
  if (write_end <= file_base) return false;

  const uint32_t first = reg_base < file_base ? file_base : reg_base;
  const uint32_t last = write_end < file_end ? write_end : file_end;
  for (uint32_t reg = first; reg < last; ++reg)
    file[reg - file_base] = body[reg - reg_base];
  return true;
}

}  // namespace

void Pm4Translator::ApplyType0Write(uint32_t reg_base,
                                    const std::vector<uint32_t>& body) {
  ClipWriteInto(m_fetchConsts, kFetchConstBase, kFetchConstCount, reg_base, body);
  if (ClipWriteInto(m_ctxRegs, kCtxRegBase, kCtxRegCount, reg_base, body))
    m_ctxWritten = true;
  // The ALU constant file, 0x4000..0x47FF. This used to be dropped on the
  // floor, and AGENTS.md's "the game never writes the ALU constant file" was
  // partly an artifact of that: the check behind it covered 0x4000..0x41FF —
  // the first 128 vec4 of 512 — so a write anywhere above vec4 127 was invisible
  // to it *and* discarded here. Type0 is how this game delivers its vertex fetch
  // constants, so it is the most likely door for these too.
  if (ClipWriteInto(m_aluConsts, kAluConstBase, kAluConstDwords, reg_base, body))
    NoteAluConstWrite(kAluSourceType0, reg_base,
                      static_cast<uint32_t>(body.size()));
}

// One place to count what actually lands in the ALU constant file, from each of
// the three doors into it, so "the file is empty" can be told apart from "we
// were not watching the door it comes through".
void Pm4Translator::NoteAluConstWrite(int source, uint32_t reg_base,
                                      uint32_t dwords) {
  static const char* kNames[] = {"Type0", "SET_CONSTANT", "LOAD_ALU_CONSTANT"};
  static uint64_t s_writes[3] = {};
  static uint64_t s_nonzero[3] = {};
  ++s_writes[source];
  m_aluWritten = true;

  // Count the vec4 slots that currently hold anything non-zero. A file written
  // 54000 times that still reads as zeros is a broken read, not a quiet game.
  uint32_t live = 0;
  for (uint32_t v = 0; v < kAluConstDwords / 4; ++v) {
    if (m_aluConsts[v * 4 + 0] || m_aluConsts[v * 4 + 1] ||
        m_aluConsts[v * 4 + 2] || m_aluConsts[v * 4 + 3]) {
      ++live;
    }
  }
  if (live) ++s_nonzero[source];

  static uint64_t s_total = 0;
  if (++s_total <= 8 || (s_total % 4000) == 0) {
    REXLOG_INFO("aluconst: {} write reg=0x{:X} dwords={} — live vec4 slots {} of "
                "{} — writes by door Type0 {}/{} SET_CONSTANT {}/{} "
                "LOAD_ALU_CONSTANT {}/{} (non-zero-after/total)",
                kNames[source], reg_base, dwords, live, kAluConstDwords / 4,
                s_nonzero[0], s_writes[0], s_nonzero[1], s_writes[1],
                s_nonzero[2], s_writes[2]);
  }
}

float Pm4Translator::CtxFloat(uint32_t reg, float fallback) const {
  if (!m_ctxWritten || reg < kCtxRegBase || reg >= kCtxRegBase + kCtxRegCount)
    return fallback;
  float f;
  std::memcpy(&f, &m_ctxRegs[reg - kCtxRegBase], 4);
  return f;
}

uint32_t Pm4Translator::CtxDword(uint32_t reg, uint32_t fallback) const {
  if (!m_ctxWritten || reg < kCtxRegBase || reg >= kCtxRegBase + kCtxRegCount)
    return fallback;
  return m_ctxRegs[reg - kCtxRegBase];
}

// Reports the region of the ALU constant file each shader stage addresses.
//
// Why this matters: LOAD_ALU_CONSTANT only ever writes four destinations —
// dword 0x0, 0x3E0, 0x3F0 and 0x7F0. 0x3F0 is vec4 252 and 0x7F0 is vec4 508,
// i.e. the last four vec4 of each 256-vec4 bank, which is where a per-object
// matrix normally goes, and they arrive at roughly one of each per draw. The
// interpreter meanwhile indexes the file absolutely. If the vertex stage is
// based at 256 then a shader reading c252..255 wants vec4 508..511, and reading
// 252 instead would hand it whatever is there — plausibly zeros, which is
// exactly the 19% of executions exporting (0,0,0,w=0).
//
// The bit layout is Xenia's, not the SDK's: register_table.inc names both
// registers but carries no bitfield for them. So this logs the raw dword too —
// a base above 512 or a zero size means the layout is wrong, and that is the
// finding rather than something to force a plausible value through.
void Pm4Translator::LogShaderConstBases() const {
  const uint32_t vs = CtxDword(kRegVsConst);
  const uint32_t ps = CtxDword(kRegPsConst);
  static uint32_t s_lastVs = 0xFFFFFFFF, s_lastPs = 0xFFFFFFFF;
  if (vs == s_lastVs && ps == s_lastPs) return;
  s_lastVs = vs;
  s_lastPs = ps;
  REXLOG_INFO("translator: SQ_VS_CONST raw=0x{:08X} base={} size={} | "
              "SQ_PS_CONST raw=0x{:08X} base={} size={} (vec4 units)",
              vs, vs & 0x1FF, (vs >> 12) & 0x1FF,
              ps, ps & 0x1FF, (ps >> 12) & 0x1FF);
}

void Pm4Translator::BuildViewportMvp(float out[16]) const {
  static const float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0,
                                      0, 0, 1, 0, 0, 0, 0, 1};
  std::memcpy(out, kIdentity, sizeof(kIdentity));

  const float xs = CtxFloat(kRegVportXScale, 0.0f);
  const float xo = CtxFloat(kRegVportXOffset, 0.0f);
  const float ys = CtxFloat(kRegVportYScale, 0.0f);
  const float yo = CtxFloat(kRegVportYOffset, 0.0f);
  float zs = CtxFloat(kRegVportZScale, 1.0f);
  const float zo = CtxFloat(kRegVportZOffset, 0.0f);
  if (zs == 0.0f) zs = 1.0f;  // z is commonly left at scale 1 / offset 0

  if (!m_ctxWritten || xs == 0.0f || ys == 0.0f) {
    static bool s_logged = false;
    if (!s_logged) {
      s_logged = true;
      REXLOG_WARN("translator: no usable viewport (written={} xs={} ys={}) — "
                  "falling back to identity, geometry will be in NDC units",
                  m_ctxWritten, xs, ys);
    }
    return;
  }

  out[0]  = 1.0f / xs;  out[3]  = -xo / xs;
  out[5]  = 1.0f / ys;  out[7]  = -yo / ys;
  out[10] = 1.0f / zs;  out[11] = -zo / zs;

  static float s_lastXs = 0.0f, s_lastYs = 0.0f;
  if (xs != s_lastXs || ys != s_lastYs) {
    s_lastXs = xs;
    s_lastYs = ys;
    REXLOG_INFO("translator: viewport xs={} xo={} ys={} yo={} zs={} zo={} "
                "-> {}x{} target", xs, xo, ys, yo, zs, zo,
                xs * 2.0f, ys < 0 ? -ys * 2.0f : ys * 2.0f);
  }
}

HostTopology Pm4Translator::MapTopology(uint32_t prim_type) {
  switch (static_cast<PrimitiveType>(prim_type)) {
    case PrimitiveType::kPointList:     return HostTopology::kPointList;
    case PrimitiveType::kLineList:      return HostTopology::kLineList;
    case PrimitiveType::kLineStrip:     return HostTopology::kLineStrip;
    case PrimitiveType::kTriangleList:  return HostTopology::kTriangleList;
    case PrimitiveType::kTriangleStrip: return HostTopology::kTriangleStrip;
    // RectangleList and QuadList are not topologies, they are expansions —
    // ExpandRectangleList and ExpandQuadList rewrite them to triangle lists and
    // set the topology themselves.
    // TriangleFan has no D3D12 equivalent either, and is still not handled: it
    // is dropped and counted. Unlike the other two that is not a backlog item
    // for now — it does not appear once in a prim_type histogram of a full run,
    // so there is no measured population to justify the code.
    default:                            return HostTopology::kUndefined;
  }
}

uint32_t Pm4Translator::ExpandRectangleList(DrawCall& dc) const {
  const uint32_t stride = dc.vertex_stride;
  if (stride < 12 || dc.vertices.size() < size_t(dc.vertex_count) * stride)
    return 0;
  const uint32_t rects = dc.vertex_count / 3;
  if (rects == 0) return 0;

  // One extra vertex per rect, six indices per rect. Indices are rewritten
  // wholesale — a rectangle list is always an auto-draw in this game, so the
  // incoming indices are the sequential ones we synthesized.
  std::vector<uint8_t> verts;
  verts.reserve(size_t(rects) * 4 * stride);
  std::vector<uint32_t> idx;
  idx.reserve(size_t(rects) * 6);

  for (uint32_t r = 0; r < rects; ++r) {
    const uint8_t* src = dc.vertices.data() + size_t(r) * 3 * stride;
    const uint32_t base = r * 4;
    verts.insert(verts.end(), src, src + size_t(3) * stride);

    // v3 = v0 + v2 - v1, on position only; everything else comes from v2, which
    // is the corner v3 shares an edge with in both directions.
    verts.insert(verts.end(), src + size_t(2) * stride,
                 src + size_t(3) * stride);
    uint8_t* v3 = verts.data() + (size_t(base) + 3) * stride;
    for (uint32_t c = 0; c < 3; ++c) {
      float p0, p1, p2;
      std::memcpy(&p0, src + c * 4, 4);
      std::memcpy(&p1, src + stride + c * 4, 4);
      std::memcpy(&p2, src + size_t(2) * stride + c * 4, 4);
      const float p3 = p0 + p2 - p1;
      std::memcpy(v3 + c * 4, &p3, 4);
    }

    const uint32_t order[6] = {0, 1, 2, 0, 2, 3};
    for (uint32_t i = 0; i < 6; ++i) idx.push_back(base + order[i]);
  }

  dc.vertices = std::move(verts);
  dc.vertex_count = rects * 4;
  dc.index_count = rects * 6;
  // Stay 16-bit while the counts allow it; the renderer reads index_16bit.
  dc.index_16bit = dc.vertex_count <= 0xFFFF;
  dc.indices.resize(size_t(idx.size()) * (dc.index_16bit ? 2 : 4));
  if (dc.index_16bit) {
    auto* p = reinterpret_cast<uint16_t*>(dc.indices.data());
    for (size_t i = 0; i < idx.size(); ++i) p[i] = uint16_t(idx[i]);
  } else {
    auto* p = reinterpret_cast<uint32_t*>(dc.indices.data());
    for (size_t i = 0; i < idx.size(); ++i) p[i] = idx[i];
  }
  dc.topology = HostTopology::kTriangleList;
  return rects;
}

uint32_t Pm4Translator::ExpandQuadList(DrawCall& dc) const {
  const uint32_t istride = dc.index_16bit ? 2u : 4u;
  const uint32_t have = uint32_t(dc.indices.size() / istride);
  const uint32_t quads = have / 4;
  if (quads == 0) return 0;

  // Unlike a rectangle, a quad has all four of its corners present, so nothing
  // is synthesized: the vertex buffer is untouched and only the index buffer is
  // rewritten. That also means this maps *through* the incoming indices rather
  // than assuming they are the sequential ones an auto-draw synthesizes, so it
  // is correct for a real DRAW_INDX with its own index buffer as well.
  auto read = [&](uint32_t i) -> uint32_t {
    if (dc.index_16bit) {
      uint16_t v;
      std::memcpy(&v, dc.indices.data() + size_t(i) * 2, 2);
      return v;
    }
    uint32_t v;
    std::memcpy(&v, dc.indices.data() + size_t(i) * 4, 4);
    return v;
  };

  std::vector<uint32_t> idx;
  idx.reserve(size_t(quads) * 6);
  for (uint32_t q = 0; q < quads; ++q) {
    const uint32_t c[4] = {read(q * 4 + 0), read(q * 4 + 1), read(q * 4 + 2),
                           read(q * 4 + 3)};
    // The four corners come round the perimeter, so the two triangles share the
    // v0-v2 diagonal. Splitting on v1-v3 instead gives the same silhouette for a
    // planar convex quad but the wrong interpolation across it, and is visibly
    // wrong the moment the quad is not planar — the plausible-but-wrong class,
    // so it is written down rather than left to the reader.
    const uint32_t order[6] = {0, 1, 2, 0, 2, 3};
    for (uint32_t i = 0; i < 6; ++i) idx.push_back(c[order[i]]);
  }

  dc.index_count = quads * 6;
  // Indices address the untouched vertex buffer, so the width is decided by
  // vertex_count exactly as in the rectangle path.
  dc.index_16bit = dc.vertex_count <= 0xFFFF;
  dc.indices.resize(size_t(idx.size()) * (dc.index_16bit ? 2 : 4));
  if (dc.index_16bit) {
    auto* p = reinterpret_cast<uint16_t*>(dc.indices.data());
    for (size_t i = 0; i < idx.size(); ++i) p[i] = uint16_t(idx[i]);
  } else {
    auto* p = reinterpret_cast<uint32_t*>(dc.indices.data());
    for (size_t i = 0; i < idx.size(); ++i) p[i] = idx[i];
  }
  dc.topology = HostTopology::kTriangleList;
  return quads;
}

std::vector<Pm4Translator::VertexFetch> Pm4Translator::CollectVertexFetches(
    uint32_t vertex_count) const {
  // Xenos xe_gpu_vertex_fetch_t, two dwords:
  //   dword0: type [1:0] (3 = vertex, 2 = texture), address [31:2] in dwords
  //   dword1: endian [1:0], size [25:2] in dwords
  std::vector<VertexFetch> out;
  if (vertex_count == 0) return out;
  for (uint32_t i = 0; i + 1 < kFetchConstCount; i += 2) {
    const uint32_t d0 = m_fetchConsts[i];
    const uint32_t d1 = m_fetchConsts[i + 1];
    if ((d0 & 0x3) != 0x3 || d0 == 0) continue;  // not a live vertex fetch

    VertexFetch vf;
    vf.slot = i / 2;
    vf.address = d0 & ~0x3u;
    vf.endian = d1 & 0x3;
    vf.size_bytes = ((d1 >> 2) & 0xFFFFFF) * 4;
    if (vf.address == 0 || vf.size_bytes == 0) continue;

    // Stride is not in the constant — infer it, and only trust an exact
    // division landing in a plausible range. A buffer shared by several draws
    // yields a multiple of the true stride, which is why the hex dump of the
    // resulting vertices is the real verdict and not this arithmetic.
    if (vf.size_bytes % vertex_count != 0) {
      vf.reject = "not divisible";
    } else {
      const uint32_t stride = vf.size_bytes / vertex_count;
      if (stride < kStrideMin || stride > kStrideMax) vf.reject = "stride out of range";
      else vf.stride = stride;
    }
    out.push_back(vf);
  }
  return out;
}

bool Pm4Translator::ReadGuestRange(uint8_t* guest_base, uint32_t addr,
                                    uint32_t bytes, std::vector<uint8_t>& out,
                                    const char* what) {
  if (!guest_base || bytes == 0) return false;
  const uint32_t phys = addr & (kGuestPhysMax - 1);
  if (phys + bytes > kGuestPhysMax) {
    REXLOG_WARN("translator: {} addr=0x{:08X} size={} out of guest RAM", what,
                addr, bytes);
    return false;
  }

  // GPU-side addresses are *physical*. The guest allocates through the Xbox 360
  // physical-aliasing windows (0xA0000000 / 0xC0000000 / 0xE0000000), and it is
  // the windowed address that ReXGlue actually commits — the bare physical page
  // is reserved-but-uncommitted, so reading `guest_base + phys` fails for every
  // buffer the GPU is pointed at. Measured: fetch constants name 0x1EBB02BC
  // while the committed region holding it is 0xBEBB02BC, exactly
  // phys | 0xA0000000, and the same span the PM4 ring itself lives in.
  //
  // Try the bare address first (it is right for anything the CPU allocated),
  // then each window. Above 0xE0000000 ReXGlue applies a +0x1000 offset — see
  // rex/system/xmemory.h PhysicalHostOffset.
  const uint32_t candidates[] = {phys, phys | 0xA0000000u, phys | 0xC0000000u,
                                 phys | 0xE0000000u};
  for (uint32_t i = 0; i < 4; ++i) {
    const uint32_t host_addr =
        candidates[i] + (candidates[i] >= 0xE0000000u ? 0x1000u : 0u);
#if defined(_WIN32)
    // Probe the host-side page state — ReXGlue's guest memory view reserves the
    // address space up front but only commits what was allocated. A memcpy from
    // an uncommitted page is silently swallowed by REX_FUNC's SEH frame,
    // leaving the host thread stuck indefinitely. We CANNOT VirtualAlloc-commit
    // pages ourselves: that breaks the plugin's SharedMemory page tracking,
    // which keeps its own commit state. Probe and move to the next window.
    MemoryBasicInformationLite mbi{};
    if (VirtualQuery(guest_base + host_addr, &mbi, sizeof(mbi)) == 0 ||
        !(mbi.state & 0x1000 /* MEM_COMMIT */) ||
        !(mbi.protect & 0x06 /* PAGE_READONLY|PAGE_READWRITE */)) {
      continue;
    }
#endif
    static int s_loggedWindow[4] = {};
    if (s_loggedWindow[i]++ == 0) {
      REXLOG_INFO("translator: {} phys=0x{:08X} resolved via window 0x{:08X}",
                  what, phys, candidates[i] & 0xE0000000u);
    }
    out.resize(bytes);
    std::memcpy(out.data(), guest_base + host_addr, bytes);
    return true;
  }

  static int s_loggedMiss = 0;
  if (s_loggedMiss++ < 8) {
    REXLOG_INFO("translator: {} phys=0x{:08X} skip — uncommitted in every "
                "aliasing window", what, phys);
  }
  return false;
}

const Pm4Translator::ShaderLayout* Pm4Translator::DecodeAndCacheShader(
    uint64_t key, const uint32_t* dwords, uint32_t count, const char* origin) {
  auto it = m_shaderCache.find(key);
  if (it != m_shaderCache.end()) return &it->second;

  ShaderLayout layout;
  layout.code.assign(dwords, dwords + count);
  layout.ok = DecodeVertexShaderFetches(dwords, count, layout.attrs,
                                        &layout.fail,
                                        &layout.saw_position_export);
  if (!layout.ok) layout.attrs.clear();

  auto [pos, inserted] = m_shaderCache.emplace(key, std::move(layout));
  const ShaderLayout& sl = pos->second;

  // Dump the first shaders in full, then only count. Log rotation is at 5MB
  // and this fires 68+357 times a frame, so an uncapped line would bury the
  // measurement it exists to produce.
  static uint32_t s_logged = 0, s_ok = 0, s_failed = 0;
  // How the position was identified, per distinct shader. s_exportTraced is the
  // number whose position we read out of the microcode; s_exportBarren saw the
  // export but found no fetched attribute reaching it; s_noExport never saw one.
  // The last two both fall back to the offset/format guess.
  static uint32_t s_exportTraced = 0, s_exportBarren = 0, s_noExport = 0;
  if (sl.ok) ++s_ok; else ++s_failed;
  bool traced = false;
  for (const auto& a : sl.attrs) traced = traced || a.feeds_position;
  if (sl.ok) {
    if (traced) ++s_exportTraced;
    else if (sl.saw_position_export) ++s_exportBarren;
    else ++s_noExport;
  }
  if (s_logged < 20) {
    ++s_logged;
    if (!sl.ok) {
      REXLOG_INFO("ucode: {} key=0x{:X} decode FAILED — {}", origin, key,
                  sl.fail ? sl.fail : "(no reason)");
    } else {
      REXLOG_INFO("ucode: {} key=0x{:X} decoded {} attribute(s), position "
                  "export {}", origin, key, sl.attrs.size(),
                  traced ? "traced to a fetch"
                         : (sl.saw_position_export ? "seen but fed by no fetch"
                                                   : "NOT SEEN"));
      for (size_t i = 0; i < sl.attrs.size(); ++i) {
        const auto& a = sl.attrs[i];
        REXLOG_INFO("ucode:   attr[{}] slot={} off={} stride={} fmt={} comps={} "
                    "size={} dest=r{} {}{}",
                    i, a.fetch_slot, a.offset_bytes, a.stride_bytes, a.format,
                    a.components, a.size_bytes, a.dest_reg,
                    a.from_mini ? "(mini)" : "(full)",
                    a.feeds_position ? " POSITION" : "");
      }
    }
  }
  static uint32_t s_calls = 0;
  if ((++s_calls % 2000) == 0) {
    REXLOG_INFO("ucode: {} distinct shaders cached, {} decoded, {} failed — "
                "position from export trace {}, export with no fetch {}, no "
                "export {}",
                m_shaderCache.size(), s_ok, s_failed, s_exportTraced,
                s_exportBarren, s_noExport);
  }
  return &sl;
}

void Pm4Translator::HandleImLoadImmediate(const Pm4Packet& pkt) {
  if (pkt.body.size() < 2) return;
  const uint32_t type = pkt.body[0];
  const uint32_t size_dwords = pkt.body[1] & 0xFFFF;
  const uint32_t start = pkt.body[1] >> 16;

  // The high half of body[1] is a start offset into instruction memory. It is 0
  // in every packet captured; if it ever is not, this is a partial patch of an
  // existing shader and decoding the fragment alone would be wrong. Say so
  // loudly rather than producing a confident wrong layout.
  if (start != 0) {
    static int s_warned = 0;
    if (s_warned++ < 4) {
      REXLOG_WARN("ucode: IM_LOAD_IMMEDIATE start offset {} != 0 — partial "
                  "shader patch, not decoding", start);
    }
    return;
  }
  if (type != 0) return;  // 1 = pixel shader, carries no vertex layout
  if (size_dwords == 0 || pkt.body.size() < size_dwords + 2) return;

  const uint32_t* code = pkt.body.data() + 2;

  // Content hash — an immediate load has no address to key on. FNV-1a; the
  // cache is a correctness-neutral optimisation, so a collision would cost a
  // wrong log line, not a wrong render.
  uint64_t key = 1469598103934665603ull;
  for (uint32_t i = 0; i < size_dwords; ++i) {
    key ^= code[i];
    key *= 1099511628211ull;
  }

  m_currentVs = DecodeAndCacheShader(key, code, size_dwords, "IM_LOAD_IMMEDIATE");
}

void Pm4Translator::HandleImLoad(const Pm4Packet& pkt, uint8_t* guest_base) {
  if (pkt.body.size() < 2) return;
  const uint32_t addr = pkt.body[0] & ~0x3u;
  const uint32_t type = pkt.body[0] & 0x3;
  const uint32_t size_dwords = pkt.body[1];
  if (type != 0) return;  // vertex shaders only
  if (addr == 0 || size_dwords == 0 || size_dwords > 4096) return;

  // Reuse by address before touching guest memory — 0x1D5FF040 alone recurs
  // ~40 times a frame, and ReadGuestRange is the expensive part.
  auto it = m_shaderCache.find(addr);
  if (it != m_shaderCache.end()) {
    m_currentVs = &it->second;
    return;
  }

  std::vector<uint8_t> bytes;
  if (!ReadGuestRange(guest_base, addr, size_dwords * 4, bytes, "shader ucode"))
    return;

  // Guest memory is big-endian; the ring came pre-swapped by the parser but
  // this did not.
  std::vector<uint32_t> code(size_dwords);
  std::memcpy(code.data(), bytes.data(), size_dwords * 4);
  for (auto& w : code) w = __builtin_bswap32(w);

  m_currentVs = DecodeAndCacheShader(addr, code.data(), size_dwords, "IM_LOAD");
}

void Pm4Translator::HandleLoadAluConstant(const Pm4Packet& pkt,
                                          uint8_t* guest_base) {
  if (pkt.body.size() < 3) return;
  const uint32_t addr = pkt.body[0];
  const uint32_t index = pkt.body[1] & 0xFFFF;   // dword index into the file
  const uint32_t type = pkt.body[1] >> 16;
  const uint32_t size_dwords = pkt.body[2];

  static std::map<uint32_t, uint32_t> s_indices;
  static std::map<uint32_t, uint32_t> s_sizes;
  ++s_indices[index];
  ++s_sizes[size_dwords];

  // Do not hardcode 16 — a couple of packets per frame carry 0x20.
  if (size_dwords == 0 || index >= kAluConstDwords ||
      index + size_dwords > kAluConstDwords) {
    static int s_warned = 0;
    if (s_warned++ < 4) {
      REXLOG_WARN("alu: LOAD_ALU_CONSTANT index={} size={} type={} out of the "
                  "512-vec4 file — ignoring", index, size_dwords, type);
    }
    return;
  }

  std::vector<uint8_t> bytes;
  if (!ReadGuestRange(guest_base, addr, size_dwords * 4, bytes, "alu constants"))
    return;

  // Guest memory is big-endian and this did not come through the parser's
  // byteswap.
  for (uint32_t i = 0; i < size_dwords; ++i) {
    uint32_t w;
    std::memcpy(&w, bytes.data() + i * 4, 4);
    m_aluConsts[index + i] = __builtin_bswap32(w);
  }
  NoteAluConstWrite(kAluSourceLoad, kAluConstBase + index, size_dwords);

  // Is the source blank, or is our read of it blank? Every one of 54000 loads
  // logged row0 = (0,0,0,0) from many distinct addresses, which is a suspicious
  // shape for real data. Count the loads whose payload was entirely zero.
  {
    bool any = false;
    for (uint32_t i = 0; i < size_dwords && !any; ++i)
      any = m_aluConsts[index + i] != 0;
    static uint64_t s_zero = 0, s_any = 0;
    if (any) ++s_any; else ++s_zero;
    static uint64_t s_n = 0;
    if ((++s_n % 4000) == 0) {
      REXLOG_INFO("alu: loads with any non-zero payload {} / all-zero {} — if "
                  "all-zero dominates the guest read is blank, not the game",
                  s_any, s_zero);
    }
  }

  static uint32_t s_calls = 0;
  if (++s_calls <= 6 || (s_calls % 2000) == 0) {
    std::string ih, sh;
    for (const auto& [k, n] : s_indices) ih += fmt::format("0x{:X}:{} ", k, n);
    for (const auto& [k, n] : s_sizes) sh += fmt::format("{}:{} ", k, n);
    float f[4];
    for (int i = 0; i < 4; ++i) std::memcpy(&f[i], &m_aluConsts[index + i], 4);
    REXLOG_INFO("alu: load #{} addr=0x{:08X} index=0x{:X} size={} type={} "
                "row0=({:.4f} {:.4f} {:.4f} {:.4f}) — indices {}— sizes {}",
                s_calls, addr, index, size_dwords, type, f[0], f[1], f[2], f[3],
                ih, sh);
  }
}

void Pm4Translator::ProbeAluMatrices(const DrawCall& dc) {
  // Read-only. The viewport inverse below is the control: it is the transform
  // the renderer actually uses today, and it is known right for window-space UI
  // rects and expected wrong for world geometry. If one of the ALU matrices
  // puts world vertices in [-1,1] where the viewport transform does not, that
  // identifies both the matrix and its layout in one run.
  if (!m_aluWritten) return;

  // Wait for the post-load state before sampling. The first version of this
  // probed the first 10 draws and every candidate matrix read as zeros — the
  // draws it caught were from the first moments of the run, long before the
  // game had loaded anything into the constant file. The interesting state
  // starts around t+40s, so hold off until the ALU file has been written a few
  // thousand times.
  static uint32_t s_aluLoads = 0;
  if (++s_aluLoads < 5000) return;

  if (!m_currentVs || !m_currentVs->ok) return;
  const VertexAttribute* pos = nullptr;
  for (const auto& a : m_currentVs->attrs) {
    if (a.offset_bytes == 0) { pos = &a; break; }
  }
  if (!pos) return;
  // Position is read according to the format the shader declares, not assumed.
  // The first version only accepted k_32_32_32_FLOAT, which meant every probed
  // draw was a RectangleList UI quad — the geometry already handled correctly
  // by the viewport transform, and precisely not the geometry the world matrix
  // would be for. Half-float positions outnumber float3 six to one here.
  const bool is_f32 = pos->format == 57;  // k_32_32_32_FLOAT
  const bool is_f16 = pos->format == 32;  // k_16_16_16_16_FLOAT
  if (!is_f32 && !is_f16) return;
  const uint32_t need = is_f32 ? 12u : 8u;
  if (dc.vertices.empty() || dc.vertex_stride < need || dc.vertex_count == 0)
    return;

  // Sample a spread of primitive types rather than 10 of whatever comes first,
  // so a verdict for QuadList is not inferred from RectangleList.
  static std::map<uint32_t, int> s_perPrim;
  if (s_perPrim[dc.prim_type] >= 3) return;
  ++s_perPrim[dc.prim_type];
  static int s_logged = 0;
  if (s_logged >= 24) return;
  ++s_logged;

  // IEEE half -> float. Only needed for the k_16_16_16_16_FLOAT case.
  auto half_to_float = [](uint16_t h) -> float {
    const uint32_t sign = uint32_t(h & 0x8000) << 16;
    uint32_t exp = (h >> 10) & 0x1F;
    uint32_t man = h & 0x3FF;
    uint32_t bits;
    if (exp == 0) {
      if (man == 0) {
        bits = sign;  // +-0
      } else {
        // Subnormal: renormalise.
        exp = 1;
        while (!(man & 0x400)) { man <<= 1; --exp; }
        man &= 0x3FF;
        bits = sign | ((exp + 112) << 23) | (man << 13);
      }
    } else if (exp == 0x1F) {
      bits = sign | 0x7F800000u | (man << 13);  // inf / nan
    } else {
      bits = sign | ((exp + 112) << 23) | (man << 13);
    }
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
  };

  auto read_pos = [&](uint32_t v, float out[4]) {
    const uint8_t* p = dc.vertices.data() + size_t(v) * dc.vertex_stride;
    out[0] = out[1] = out[2] = 0.0f;
    out[3] = 1.0f;
    if (is_f32) {
      for (uint32_t c = 0; c < 3; ++c) std::memcpy(&out[c], p + c * 4, 4);
    } else {
      // 4 halves: x, y, z, w. w is genuinely present in this format, so use it
      // rather than forcing 1.0 — a pre-projected position would carry it.
      for (uint32_t c = 0; c < 4; ++c) {
        uint16_t h;
        std::memcpy(&h, p + c * 2, 2);
        out[c] = half_to_float(h);
      }
    }
  };

  // The two constant-file offsets every LOAD_ALU_CONSTANT targets.
  const uint32_t kCandidates[] = {0x3F0, 0x7F0};

  auto probe = [&](const char* label, const float m[16]) {
    const uint32_t show = dc.vertex_count < 3 ? dc.vertex_count : 3;
    uint32_t in_range = 0;
    std::string line;
    for (uint32_t v = 0; v < show; ++v) {
      float in[4];
      read_pos(v, in);
      float o[4];
      for (uint32_t r = 0; r < 4; ++r) {
        o[r] = m[r * 4 + 0] * in[0] + m[r * 4 + 1] * in[1] +
               m[r * 4 + 2] * in[2] + m[r * 4 + 3] * in[3];
      }
      const bool ok = o[3] > 0.0f && o[0] >= -o[3] && o[0] <= o[3] &&
                      o[1] >= -o[3] && o[1] <= o[3];
      if (ok) ++in_range;
      line += fmt::format("({:.3f} {:.3f} {:.3f} {:.3f}){} ", o[0], o[1], o[2],
                          o[3], ok ? "*" : "");
    }
    REXLOG_INFO("alu probe [{}] prim={} {}-> {}/{} in clip", label, dc.prim_type,
                line, in_range, show);
  };

  // Show the source vertices and the raw matrices. Without these a null result
  // is uninterpretable — "nothing landed in clip" could equally mean the
  // matrices are zeros, the positions are garbage, or the layout is wrong, and
  // those want different next steps.
  {
    std::string vs;
    const uint32_t show = dc.vertex_count < 3 ? dc.vertex_count : 3;
    for (uint32_t v = 0; v < show; ++v) {
      float in[4];
      read_pos(v, in);
      vs += fmt::format("({:.3f} {:.3f} {:.3f} {:.3f}) ", in[0], in[1], in[2],
                        in[3]);
    }
    REXLOG_INFO("alu probe: prim={} stride={} posfmt={} ({}) vtcs={} src {}",
                dc.prim_type, dc.vertex_stride, pos->format,
                is_f32 ? "float3" : "half4", dc.vertex_count, vs);
  }

  for (uint32_t base : kCandidates) {
    float raw[16];
    for (int i = 0; i < 16; ++i) std::memcpy(&raw[i], &m_aluConsts[base + i], 4);

    for (int r = 0; r < 4; ++r) {
      REXLOG_INFO("alu probe:   c0x{:X} row{} = {:.4f} {:.4f} {:.4f} {:.4f}",
                  base, r, raw[r * 4 + 0], raw[r * 4 + 1], raw[r * 4 + 2],
                  raw[r * 4 + 3]);
    }

    // Row-major: constant register N is row N — the layout BuildViewportMvp
    // already produces, so this is the like-for-like reading.
    probe(fmt::format("c0x{:X} row-major", base).c_str(), raw);

    // Column-major: constant register N is column N, which is how a D3D-era
    // shader compiler usually packs a matrix into 4 constants.
    float t[16];
    for (int r = 0; r < 4; ++r)
      for (int c = 0; c < 4; ++c) t[r * 4 + c] = raw[c * 4 + r];
    probe(fmt::format("c0x{:X} col-major", base).c_str(), t);
  }

  probe("viewport inverse (control)", dc.mvp);
}

void Pm4Translator::LogStrideComparison(const DrawCall& dc,
                                        uint32_t heuristic_stride,
                                        uint32_t heuristic_slot) {
  // Counters are cumulative across the run and dumped periodically; the
  // per-draw lines are capped. ~46000 draws per run go through here.
  static uint64_t s_agree = 0, s_disagree = 0, s_noShader = 0, s_noAttrs = 0;
  static uint64_t s_slotMiss = 0;
  static std::map<uint32_t, uint32_t> s_vfetchStrides;
  static std::map<uint32_t, uint32_t> s_posFormats;
  static int s_logged = 0;

  if (!m_currentVs || !m_currentVs->ok) { ++s_noShader; return; }
  if (m_currentVs->attrs.empty()) { ++s_noAttrs; return; }

  // Prefer the attribute group the heuristic actually picked a slot for; a
  // shader can fetch from several slots with different strides, and comparing
  // against the wrong one would manufacture disagreement.
  const VertexAttribute* match = nullptr;
  for (const auto& a : m_currentVs->attrs) {
    if (a.fetch_slot == heuristic_slot) { match = &a; break; }
  }
  const bool slot_matched = match != nullptr;
  if (!match) {
    ++s_slotMiss;
    match = &m_currentVs->attrs[0];
  }

  const uint32_t vfetch_stride = match->stride_bytes;
  ++s_vfetchStrides[vfetch_stride];

  // Position is the attribute at offset 0 with a float format — NOT the one in
  // dest_reg 0. Both ground-truth shaders put position in dest_reg 1 and the
  // second attribute in dest_reg 0.
  for (const auto& a : m_currentVs->attrs) {
    if (a.offset_bytes == 0) { ++s_posFormats[a.format]; break; }
  }

  const bool agree = vfetch_stride == heuristic_stride;
  if (agree) ++s_agree; else ++s_disagree;

  // Split the verdict by whether the slot matched. A slot miss means we are
  // comparing the heuristic against attrs[0] — some other slot's stride — so
  // those disagreements are an artifact of the comparison, not evidence the
  // decoder is wrong. Reporting one number over both would overstate the
  // disagreement.
  static uint64_t s_agreeMatched = 0, s_disagreeMatched = 0;
  static uint64_t s_agreeMissed = 0, s_disagreeMissed = 0;
  if (slot_matched) {
    if (agree) ++s_agreeMatched; else ++s_disagreeMatched;
  } else {
    if (agree) ++s_agreeMissed; else ++s_disagreeMissed;
  }
  // Which (heuristic, vfetch) pairs actually disagree, for the slot-matched
  // cases only — that is the population that says something about the decoder.
  static std::map<uint64_t, uint32_t> s_disagreePairs;
  if (!agree && slot_matched)
    ++s_disagreePairs[(uint64_t(heuristic_stride) << 32) | vfetch_stride];

  if (s_logged < 30) {
    ++s_logged;
    REXLOG_INFO("translator: stride heuristic={} vfetch={} slot h={} v={}{} "
                "prim={} fmt={} vtcs={} {}",
                heuristic_stride, vfetch_stride, heuristic_slot,
                match->fetch_slot, slot_matched ? "" : " (SLOT MISS)",
                dc.prim_type, match->format, dc.vertex_count,
                agree ? "AGREE" : "DISAGREE");
  }

  static uint64_t s_total = 0;
  if ((++s_total % 5000) == 0) {
    std::string sh;
    for (const auto& [s, n] : s_vfetchStrides) sh += fmt::format("{}:{} ", s, n);
    std::string pf;
    for (const auto& [f, n] : s_posFormats) pf += fmt::format("{}:{} ", f, n);
    std::string dp;
    for (const auto& [k, n] : s_disagreePairs)
      dp += fmt::format("h{}/v{}:{} ", uint32_t(k >> 32), uint32_t(k), n);
    REXLOG_INFO("translator: STRIDE agree={} disagree={} (no shader {}, no "
                "attrs {}) — slot MATCHED agree={} disagree={} — slot MISSED "
                "agree={} disagree={} — vfetch strides {}— pos formats {}— "
                "disagreeing pairs (slot-matched only) {}",
                s_agree, s_disagree, s_noShader, s_noAttrs,
                s_agreeMatched, s_disagreeMatched, s_agreeMissed,
                s_disagreeMissed, sh, pf, dp.empty() ? "none " : dp);
  }
}

// Records what the ALU interpreter did with one vertex. The question this has
// to answer before anything trusts it: does it execute these shaders at all,
// and when it does, are the coordinates inside the clip volume? Positions in
// [-1,1] after the perspective divide mean the interpreter and the constants
// are both right — the same instrument that made the viewport transform
// unambiguous two rounds ago.
void Pm4Translator::NoteAluExecution(const AluResult& r, uint32_t pos_format) {
  static std::map<int, uint64_t> s_status;
  static std::map<uint32_t, uint64_t> s_blocking;
  static uint64_t s_inRange = 0, s_outOfRange = 0, s_nonFinite = 0;
  static std::map<uint32_t, uint64_t> s_inByFormat, s_outByFormat;

  // The same positions scored a second way. The transcode asserts the
  // interpreter's answer is already clip space and hands the renderer identity;
  // but every sampled export so far reads (640, 0, 1, w=1), (1280, 0, 0, w=1),
  // (639.5, -0.5, 1, w=1) — 1280x720 window coordinates in the D3D9
  // pixel-centre convention, not clip space. If that generalises, the right
  // transform is the viewport inverse BuildViewportMvp already computes, and
  // the ALU path is the only thing opting out of it.
  //
  // Four log lines are not a population, so this scores both interpretations
  // over every execution rather than acting on the samples. Reuses CtxFloat and
  // the same registers BuildViewportMvp reads, so the two cannot drift apart.
  static uint64_t s_inRangeVp = 0, s_outOfRangeVp = 0;
  static std::map<uint32_t, uint64_t> s_inByFormatVp;
  // And what each position *looks* like, so the verdict is not just "one number
  // is bigger than the other".
  //
  // s_degenerate is the reason this needed a second pass. The most common single
  // export is (0, 0, 0, w=0), which sits inside the unit cube and would be
  // counted as clip-like while being no evidence of anything. It also inflates
  // the viewport-inverse in-range count, because with xo/xs == 1 the origin maps
  // to exactly (-1, +1) — a corner, in range, and meaningless. Counted apart so
  // neither interpretation gets credit for it.
  static uint64_t s_clipLike = 0, s_windowLike = 0, s_neither = 0,
                  s_degenerate = 0;

  // Constant-file reads, indexed [0] = produced a position, [1] = degenerate.
  static uint64_t s_constReads[2] = {}, s_constZero[2] = {}, s_constExecs[2] = {};
  static uint32_t s_constMin[2] = {0xFFFFFFFF, 0xFFFFFFFF}, s_constMax[2] = {};
  // Highest index each execution touched. The distribution of this is what says
  // whether shaders are reaching for c252..255 — the slot LOAD_ALU_CONSTANT
  // fills — or staying in the low bank.
  static std::map<uint32_t, uint64_t> s_topIndex;

  ++s_status[int(r.status)];
  if (r.status != AluStatus::kOk) {
    ++s_blocking[r.blocking_opcode];
  } else {
    const float w = r.position[3];
    float x = r.position[0], y = r.position[1], z = r.position[2];
    if (w != 0.0f) { x /= w; y /= w; z /= w; }
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
      ++s_nonFinite;
    } else if (x >= -1.05f && x <= 1.05f && y >= -1.05f && y <= 1.05f &&
               z >= -1.05f && z <= 1.05f) {
      ++s_inRange;
      ++s_inByFormat[pos_format];
    } else {
      ++s_outOfRange;
      ++s_outByFormat[pos_format];
    }

    const float xs = CtxFloat(kRegVportXScale, 0.0f);
    const float xo = CtxFloat(kRegVportXOffset, 0.0f);
    const float ys = CtxFloat(kRegVportYScale, 0.0f);
    const float yo = CtxFloat(kRegVportYOffset, 0.0f);
    if (std::isfinite(x) && std::isfinite(y) && xs != 0.0f && ys != 0.0f) {
      const float vx = (x - xo) / xs;
      const float vy = (y - yo) / ys;
      if (std::isfinite(vx) && std::isfinite(vy) && vx >= -1.05f &&
          vx <= 1.05f && vy >= -1.05f && vy <= 1.05f) {
        ++s_inRangeVp;
        ++s_inByFormatVp[pos_format];
      } else {
        ++s_outOfRangeVp;
      }

      // Window-like is judged against the viewport's own extent rather than a
      // hardcoded 1280x720: xs is half the width and ys half the height (ys is
      // negative, y growing downward). A half-pixel margin covers the D3D9
      // pixel-centre offsets seen in the samples.
      const float wpx = std::fabs(xs) * 2.0f, wpy = std::fabs(ys) * 2.0f;
      const bool clip_like = x >= -1.05f && x <= 1.05f && y >= -1.05f && y <= 1.05f;
      const bool win_like = x >= -1.0f && x <= wpx + 1.0f &&
                            y >= -1.0f && y <= wpy + 1.0f;
      // Checked in this order because the two overlap near the origin — a point
      // inside the unit cube is also inside the viewport rectangle. Clip wins
      // the tie, which biases against the hypothesis being tested here rather
      // than for it. Degenerate output is taken out first so it cannot be read
      // as support for either.
      const bool degenerate = (w == 0.0f) || (std::fabs(x) < 1e-4f &&
                                              std::fabs(y) < 1e-4f);
      if (degenerate) ++s_degenerate;
      else if (clip_like) ++s_clipLike;
      else if (win_like) ++s_windowLike;
      else ++s_neither;

      // What the shader asked the constant file for, split by whether it went
      // on to compute nothing. If the degenerate executions are exactly the
      // ones whose constant reads came back zero, the diagnosis is closed and
      // the index range says which slots to chase.
      const int b = degenerate ? 1 : 0;
      s_constReads[b] += r.const_reads;
      s_constZero[b] += r.const_zero_reads;
      ++s_constExecs[b];
      if (r.const_reads) {
        if (r.const_min_index < s_constMin[b]) s_constMin[b] = r.const_min_index;
        if (r.const_max_index > s_constMax[b]) s_constMax[b] = r.const_max_index;
        ++s_topIndex[r.const_max_index];
      }
    }
  }

  static uint64_t s_n = 0;
  if (++s_n <= 10 || (s_n % 4000) == 0) {
    if (s_n <= 10) {
      REXLOG_INFO("alu: exec #{} status={} pos=({:.4f} {:.4f} {:.4f} w={:.4f}) "
                  "posfmt={}",
                  s_n, AluStatusName(r.status), r.position[0], r.position[1],
                  r.position[2], r.position[3], pos_format);
      return;
    }
    std::string st, bl, inf, outf, infvp;
    for (const auto& [k, n] : s_status)
      st += fmt::format("{}:{} ", AluStatusName(AluStatus(k)), n);
    for (const auto& [k, n] : s_blocking) bl += fmt::format("{}:{} ", k, n);
    for (const auto& [k, n] : s_inByFormat) inf += fmt::format("{}:{} ", k, n);
    for (const auto& [k, n] : s_outByFormat) outf += fmt::format("{}:{} ", k, n);
    for (const auto& [k, n] : s_inByFormatVp) infvp += fmt::format("{}:{} ", k, n);
    REXLOG_INFO("alu: exec {} — status {}— blocking opcodes {}— clip in-range "
                "{} out-of-range {} non-finite {} — in-range by pos format {}— "
                "out-of-range by pos format {}",
                s_n, st, bl.empty() ? "none " : bl, s_inRange, s_outOfRange,
                s_nonFinite, inf.empty() ? "none " : inf,
                outf.empty() ? "none " : outf);
    REXLOG_INFO("alu space: as-clip in {} out {} | after viewport inverse in {} "
                "out {} — looks like clip {} window {} neither {} degenerate {} "
                "— viewport-inverse in-range by pos format {}",
                s_inRange, s_outOfRange, s_inRangeVp, s_outOfRangeVp,
                s_clipLike, s_windowLike, s_neither, s_degenerate,
                infvp.empty() ? "none " : infvp);

    std::string ti;
    for (const auto& [k, n] : s_topIndex) ti += fmt::format("{}:{} ", k, n);
    auto per = [](uint64_t a, uint64_t b) { return b ? double(a) / double(b) : 0.0; };
    REXLOG_INFO("alu consts: produced-a-position n={} reads/exec {:.1f} "
                "zero-reads/exec {:.1f} index range [{}..{}] | degenerate n={} "
                "reads/exec {:.1f} zero-reads/exec {:.1f} index range [{}..{}] "
                "— highest index touched per exec {}",
                s_constExecs[0], per(s_constReads[0], s_constExecs[0]),
                per(s_constZero[0], s_constExecs[0]),
                s_constMin[0] == 0xFFFFFFFF ? -1 : int(s_constMin[0]),
                int(s_constMax[0]),
                s_constExecs[1], per(s_constReads[1], s_constExecs[1]),
                per(s_constZero[1], s_constExecs[1]),
                s_constMin[1] == 0xFFFFFFFF ? -1 : int(s_constMin[1]),
                int(s_constMax[1]), ti.empty() ? "none " : ti);
  }
}

// Runs the interpreter on a sample of vertices, read-only, and feeds
// NoteAluExecution. Called before the format gate so it sees every draw the
// transcode could ever handle, not just the ones it already does.
void Pm4Translator::ProbeAluExecution(const DrawCall& dc,
                                      const VertexAttribute& pos) {
  if (!m_currentVs || m_currentVs->code.empty()) return;
  static uint64_t s_draws = 0;
  if ((++s_draws % 16) != 1) return;
  const uint32_t src_stride = dc.vertex_stride;
  if (src_stride == 0 || dc.vertex_count == 0) return;
  if (uint64_t(dc.vertex_count) * src_stride > dc.vertices.size()) return;

  AluInputs in;
  in.alu_consts = m_aluConsts;
  in.alu_const_dwords = kAluConstDwords;
  std::vector<std::array<float, 4>> values(m_currentVs->attrs.size());

  const uint32_t n = dc.vertex_count < 2 ? dc.vertex_count : 2;
  for (uint32_t v = 0; v < n; ++v) {
    const uint8_t* src = dc.vertices.data() + size_t(v) * src_stride;
    for (size_t i = 0; i < m_currentVs->attrs.size(); ++i) {
      float f[4] = {0, 0, 0, 1};
      ReadVertexAttribute(src, src_stride, m_currentVs->attrs[i], f);
      for (int c = 0; c < 4; ++c) values[i][c] = f[c];
    }
    NoteAluExecution(ExecuteVertexShader(
                         m_currentVs->code.data(),
                         static_cast<uint32_t>(m_currentVs->code.size()),
                         m_currentVs->attrs, values, in),
                     pos.format);
  }
}

Pm4Translator::DrawClass Pm4Translator::ClassifyTransformedDraw(
    const std::vector<uint8_t>& verts, uint32_t count, uint32_t stride) const {
  if (count == 0 || stride < 12 || verts.size() < size_t(count) * stride)
    return DrawClass::kPartial;  // nothing to judge; do not condemn it

  float mvp[16];
  BuildViewportMvp(mvp);

  bool all_origin = true;
  bool any_origin = false;
  bool any_near = false;
  for (uint32_t v = 0; v < count; ++v) {
    float in[4] = {0, 0, 0, 1};
    std::memcpy(in, verts.data() + size_t(v) * stride, 12);
    if (std::fabs(in[0]) > 1e-6f || std::fabs(in[1]) > 1e-6f ||
        std::fabs(in[2]) > 1e-6f) {
      all_origin = false;
    } else {
      any_origin = true;
    }
    float o[4];
    for (uint32_t r = 0; r < 4; ++r) {
      o[r] = mvp[r * 4 + 0] * in[0] + mvp[r * 4 + 1] * in[1] +
             mvp[r * 4 + 2] * in[2] + mvp[r * 4 + 3] * in[3];
    }
    // Deliberately generous — four times the frustum. The question is not "is
    // this vertex visible" but "could this draw possibly be right", and a draw
    // is only hopeless when every vertex is wildly out. A tight bound here
    // would delete real geometry crossing the frustum edge and make the screen
    // look cleaner while showing less, which is the failure mode that flatters
    // itself.
    if (std::isfinite(o[0]) && std::isfinite(o[1]) && std::isfinite(o[3]) &&
        o[3] != 0.0f) {
      const float lim = std::fabs(o[3]) * 4.0f;
      if (std::fabs(o[0]) <= lim && std::fabs(o[1]) <= lim) any_near = true;
    }
  }

  // Origin-collapsed is reported ahead of out-of-range because it is the more
  // specific fact: the ALU writes xyz = 0 exactly when it exports w == 0, and w
  // does not survive into the transcoded buffer to be tested directly.
  if (all_origin) return DrawClass::kDegenerate;
  if (!any_near) return DrawClass::kOutOfRange;
  // Checked last so it only claims draws that would otherwise have passed as
  // ordinary. A draw with some vertices at exactly the origin and some not is
  // a transform that failed for part of its vertices, and it is the shape that
  // stretches one triangle from the corner across the whole frame.
  if (any_origin) return DrawClass::kMixedOrigin;
  return DrawClass::kPartial;
}

void Pm4Translator::TranscodeVertices(DrawCall& dc, const VertexFetch& fetch) {
  static uint64_t s_done = 0, s_noShader = 0, s_noPos = 0, s_readFail = 0;
  static uint64_t s_passthrough = 0;
  static uint64_t s_fromExport = 0, s_fromGuess = 0;
  static std::map<uint32_t, uint32_t> s_posFormatsUsed, s_unhandled;
  // Split the position-format histogram by how the position was identified.
  // Pooled they cannot answer the question the export decode exists to settle:
  // whether the formats the guess got wrong are the ones the shader disagrees
  // about.
  static std::map<uint32_t, uint32_t> s_exportFormats;
  // Draw classification, indexed by DrawClass: partial / degenerate / out of
  // range. Kept beside the transcode counters so one log line says both what
  // was produced and how much of it could not possibly draw.
  static uint64_t s_class[kDrawClassCount] = {},
                  s_classVerts[kDrawClassCount] = {};
  static std::map<uint32_t, uint32_t> s_classByFormat[kDrawClassCount];
  static uint64_t s_skippedClass[kDrawClassCount] = {};

  // Leave the guest layout and the stride the caller already set alone. The
  // renderer's stride-28 gate then decides, exactly as before this existed.
  auto passthrough = [&] { ++s_passthrough; };
  (void)fetch;

  if (!REXCVAR_GET(vertex_transcode)) { passthrough(); return; }
  if (!m_currentVs || !m_currentVs->ok || m_currentVs->attrs.empty()) {
    ++s_noShader; passthrough(); return;
  }

  bool from_export = false;
  const VertexAttribute* pos =
      PickPositionAttribute(m_currentVs->attrs, &from_export);
  if (!pos) { ++s_noPos; passthrough(); return; }
  if (!REXCVAR_GET(transcode_trust_export)) from_export = false;
  if (from_export) ++s_fromExport; else ++s_fromGuess;

  // The format restriction applies to export-traced positions too, and the
  // measurement is why.
  //
  // Trusting the export in whatever format it declares was tried, and it does
  // raise submitted draws from 100 to 253 — but the window goes white with
  // degenerate triangles fanning off the top-left corner, identical at t+80s
  // and t+105s. That is not the wrong attribute any more: the trace says
  // k_16_16_16_16_FLOAT genuinely is what the position export reads, in 35655
  // of 65000 transcoded draws. It is the wrong *space*. Half-float positions
  // are compressed model space that the shader expands with a per-object scale
  // and bias, and Stage 3 already established this game computes that transform
  // in the shader rather than supplying a matrix we could read. Feeding raw
  // half-floats through the viewport inverse produces coordinates far outside
  // the frustum, which is exactly the corner spray on screen.
  //
  // So the export decode buys the right answer to "which attribute", and the
  // remaining blocker is "in what space" — a shader ALU translation, not
  // another identification heuristic. Until then only k_32_32_32_FLOAT, which
  // needs no expansion, is transcoded.
  // The ALU probe runs BEFORE this gate, deliberately. Placed after it, the
  // probe only ever saw the k_32_32_32_FLOAT draws that already work — the
  // format-32 majority, which is the entire reason the interpreter exists,
  // was filtered out before it could be measured. An instrument downstream of
  // the filter it is meant to justify removing measures nothing.
  ProbeAluExecution(dc, *pos);

  if (REXCVAR_GET(transcode_confirmed_formats_only) && pos->format != 57) {
    ++s_noPos; passthrough(); return;
  }
  const VertexAttribute* col = PickColorAttribute(m_currentVs->attrs);

  // The shader's stride is authoritative; the division guess is not. Where they
  // disagree the shader wins, but only if the buffer actually holds that many
  // vertices — a wrong stride here reads off the end.
  // dc.vertex_stride is what the buffer was actually read with — the caller
  // already prefers the shader's stride where it fits. Never read past what was
  // read, whatever the shader claims.
  const uint32_t src_stride = dc.vertex_stride;
  if (src_stride == 0) { ++s_noPos; passthrough(); return; }
  if (uint64_t(dc.vertex_count) * src_stride > dc.vertices.size()) {
    ++s_readFail; passthrough(); return;
  }

  static constexpr uint32_t kOutStride = 28;  // float3 position + float4 colour
  std::vector<uint8_t> out(size_t(dc.vertex_count) * kOutStride);
  bool any_read_failed = false;

  // The ALU interpreter. When `alu_execute` is on it replaces the raw attribute
  // as the position; either way the first vertices of sampled draws are run
  // through it read-only so the status histogram says what it can and cannot
  // handle before anything depends on it.
  const bool alu_on = REXCVAR_GET(alu_execute);
  AluInputs alu_in;
  alu_in.alu_consts = m_aluConsts;
  alu_in.alu_const_dwords = kAluConstDwords;
  std::vector<std::array<float, 4>> attr_values(m_currentVs->attrs.size());

  for (uint32_t v = 0; v < dc.vertex_count; ++v) {
    const uint8_t* src = dc.vertices.data() + size_t(v) * src_stride;
    float p[4] = {0, 0, 0, 1};
    if (!ReadVertexAttribute(src, src_stride, *pos, p)) {
      any_read_failed = true;
      ++s_unhandled[pos->format];
      break;
    }

    if (alu_on) {
      // Every attribute the shader fetches, not just the position: the ALU
      // reads whichever registers it likes, and an attribute we skipped reads
      // as zero rather than as what the shader would have seen.
      for (size_t i = 0; i < m_currentVs->attrs.size(); ++i) {
        float f[4] = {0, 0, 0, 1};
        ReadVertexAttribute(src, src_stride, m_currentVs->attrs[i], f);
        for (int c = 0; c < 4; ++c) attr_values[i][c] = f[c];
      }
      const AluResult r = ExecuteVertexShader(
          m_currentVs->code.data(),
          static_cast<uint32_t>(m_currentVs->code.size()),
          m_currentVs->attrs, attr_values, alu_in);
      if (alu_on && r.status == AluStatus::kOk) {
        // This used to say "the interpreter's answer is already clip space, so
        // it must not then be run through the viewport inverse", and handed the
        // renderer identity on that basis. It is not clip space. These shaders
        // do the viewport transform themselves and export window coordinates in
        // the D3D9 pixel-centre convention — (640, 0, 1, w=1), (1280, 0, 0,
        // w=1), (639.5, -0.5, 1, w=1) against a 1280x720 target. So the answer
        // wants exactly the same viewport inverse every fetched position gets,
        // and the special case below was the only thing preventing it.
        //
        // Scored over 4000 executions before removing it. In-range by position
        // format, as-clip against after-the-inverse: 31 44%/78%, 32 32%/74%,
        // 37 70%/87%, 38 65%/74%, 57 34%/88%. Every format improves and format
        // 32 — the 35,655-draw majority this exists for — more than doubles.
        //
        // The space buckets are genuinely mixed (clip 1129, window 864, neither
        // 1130, degenerate 784), so this is the better of two interpretations
        // rather than a clean sweep. It is chosen on the per-format numbers,
        // which are what the geometry actually depends on.
        //
        // The divide stays: with w == 1, which is what every sampled export
        // carries, it is a no-op, and it is the right thing for any shader that
        // does export a genuine projective position.
        const float w = r.position[3];
        if (w != 0.0f) {
          p[0] = r.position[0] / w;
          p[1] = r.position[1] / w;
          p[2] = r.position[2] / w;
        } else {
          p[0] = r.position[0]; p[1] = r.position[1]; p[2] = r.position[2];
        }
        p[3] = 1.0f;
      }
    }
    // A position carrying its own w is already projected; divide through so the
    // downstream transform sees a consistent object-space point. w == 0 is left
    // alone rather than producing an infinity.
    if (pos->components >= 4 && p[3] != 0.0f && p[3] != 1.0f) {
      p[0] /= p[3]; p[1] /= p[3]; p[2] /= p[3];
    }

    float c[4] = {1, 1, 1, 1};
    if (col) ReadVertexAttribute(src, src_stride, *col, c);

    uint8_t* dst = out.data() + size_t(v) * kOutStride;
    std::memcpy(dst + 0, p, 12);
    std::memcpy(dst + 12, c, 16);
  }

  if (any_read_failed) { ++s_readFail; passthrough(); return; }

  dc.vertices = std::move(out);
  dc.vertex_stride = kOutStride;
  // There used to be an `if (used_alu)` override here forcing dc.mvp to
  // identity, on the belief that the ALU had already produced clip space. It
  // had not — see the note at the interpreter call above. FinalizeDraw's
  // BuildViewportMvp is the transform the ALU output actually wants, and
  // letting it stand is the whole fix. The ALU path and the fetched-position
  // path now agree about what space they are in.
  // How this draw's positions came out. Counted only — nothing is skipped yet,
  // so the numbers describe current behaviour and a later run with the skip on
  // can be compared against them rather than against run variance.
  {
    const DrawClass cls =
        ClassifyTransformedDraw(dc.vertices, dc.vertex_count, kOutStride);
    const int ci = int(cls);
    ++s_class[ci];
    s_classVerts[ci] += dc.vertex_count;
    if (cls != DrawClass::kPartial) ++s_classByFormat[ci][pos->format];
    if (cls != DrawClass::kPartial &&
        REXCVAR_GET(skip_untransformable_draws)) {
      dc.untransformable = true;
      ++s_skippedClass[ci];
    }
  }

  ++s_done;
  ++s_posFormatsUsed[pos->format];
  if (from_export) ++s_exportFormats[pos->format];

  static uint64_t s_total = 0;
  if ((++s_total % 5000) == 0) {
    std::string pf, uh, ef;
    for (const auto& [f, n] : s_posFormatsUsed) pf += fmt::format("{}:{} ", f, n);
    for (const auto& [f, n] : s_unhandled) uh += fmt::format("{}:{} ", f, n);
    for (const auto& [f, n] : s_exportFormats) ef += fmt::format("{}:{} ", f, n);
    REXLOG_INFO("transcode: done {} passthrough {} (no shader {}, no position "
                "{}, read failed {}) — position from export {} / guess {} — "
                "position formats {}— of those, export-traced {}— unhandled {}",
                s_done, s_passthrough, s_noShader, s_noPos, s_readFail,
                s_fromExport, s_fromGuess, pf, ef.empty() ? "none " : ef,
                uh.empty() ? "none " : uh);

    std::string dgf, oof, mxf;
    for (const auto& [f, n] : s_classByFormat[int(DrawClass::kDegenerate)])
      dgf += fmt::format("{}:{} ", f, n);
    for (const auto& [f, n] : s_classByFormat[int(DrawClass::kOutOfRange)])
      oof += fmt::format("{}:{} ", f, n);
    for (const auto& [f, n] : s_classByFormat[int(DrawClass::kMixedOrigin)])
      mxf += fmt::format("{}:{} ", f, n);
    uint64_t tot = 0, vtot = 0;
    for (int i = 0; i < kDrawClassCount; ++i) { tot += s_class[i]; vtot += s_classVerts[i]; }
    const uint64_t bad = s_class[1] + s_class[2] + s_class[3];
    const uint64_t badv = s_classVerts[1] + s_classVerts[2] + s_classVerts[3];
    REXLOG_INFO("transcode class: partial {} ({} vtx) | degenerate {} ({} vtx) "
                "by format {}| out-of-range {} ({} vtx) by format {}| "
                "mixed-origin {} ({} vtx) by format {}— {:.1f}% of draws and "
                "{:.1f}% of vertices could not draw correctly",
                s_class[0], s_classVerts[0], s_class[1], s_classVerts[1],
                dgf.empty() ? "none " : dgf, s_class[2], s_classVerts[2],
                oof.empty() ? "none " : oof, s_class[3], s_classVerts[3],
                mxf.empty() ? "none " : mxf,
                tot ? 100.0 * double(bad) / double(tot) : 0.0,
                vtot ? 100.0 * double(badv) / double(vtot) : 0.0);
    REXLOG_INFO("transcode skip: {} (degenerate {} out-of-range {} "
                "mixed-origin {}) — mitigation only, these draws are still "
                "wrong and are merely not drawn",
                s_skippedClass[1] + s_skippedClass[2] + s_skippedClass[3],
                s_skippedClass[1], s_skippedClass[2], s_skippedClass[3]);
  }
}

void Pm4Translator::AttachVertices(DrawCall& dc, uint8_t* guest_base) {
  auto fetches = CollectVertexFetches(dc.vertex_count);

  // Log the whole candidate field for the first draws. The selection rule below
  // is only defensible if what it is choosing between is on the record.
  static int s_logged = 0;
  const bool log_this = s_logged < 20;
  if (log_this) {
    ++s_logged;
    if (fetches.empty()) {
      REXLOG_INFO("translator: vfetch — no live vertex fetch for vtcs={}",
                  dc.vertex_count);
    }
    for (const auto& vf : fetches) {
      REXLOG_INFO("translator: vfetch slot {} addr=0x{:08X} size={} B endian={} "
                  "-> stride {} for vtcs={} [{}]",
                  vf.slot, vf.address, vf.size_bytes, vf.endian, vf.stride,
                  dc.vertex_count, vf.reject ? vf.reject : "ok");
    }
  }

  // Lowest-indexed slot that validated. Name the losers so an ambiguous pick is
  // visible rather than silent.
  const VertexFetch* pick = nullptr;
  uint32_t accepted = 0;
  for (const auto& vf : fetches) {
    if (vf.reject) continue;
    ++accepted;
    if (!pick) pick = &vf;
  }
  if (!pick) return;  // leave vertices empty — the renderer will skip this draw
  if (accepted > 1 && log_this) {
    REXLOG_INFO("translator: vfetch AMBIGUOUS — {} slots validated, took slot {}",
                accepted, pick->slot);
  }

  // Ask the shader which slot it actually fetches from.
  //
  // "Lowest-indexed slot that validated" is a tie-break with nothing behind it,
  // and it was measurably wrong: 14% of draws — ~12000 a run — were reading a
  // buffer the bound shader never fetches. The vfetch instruction names its
  // fetch constant index, so the tie-break can be replaced by the answer.
  //
  // Matching is done against the *set* of slots the shader uses rather than
  // "the position attribute's slot", deliberately. Identifying position needs a
  // rule for picking it out of the attribute list, and the one available —
  // "the attribute at offset 0" — is not yet justified: for QuadList shaders
  // that attribute decodes to values with w=0 and a range of about +-1.75,
  // which may not be a position at all. Slot membership needs no such rule.
  const VertexFetch* heuristic_pick = pick;
  if (REXCVAR_GET(vfetch_use_shader_slot) && m_currentVs && m_currentVs->ok &&
      !m_currentVs->attrs.empty()) {
    const VertexFetch* by_shader = nullptr;
    for (const auto& vf : fetches) {
      if (vf.reject) continue;
      for (const auto& a : m_currentVs->attrs) {
        if (a.fetch_slot == vf.slot) { by_shader = &vf; break; }
      }
      if (by_shader) break;
    }

    static uint64_t s_agreed = 0, s_corrected = 0, s_noMatch = 0;
    if (by_shader == heuristic_pick) {
      ++s_agreed;
    } else if (by_shader) {
      ++s_corrected;
      pick = by_shader;
    } else {
      // The shader names a slot, but no *validated* fetch carries it. The old
      // behaviour reads some other slot's buffer, which is the bug. Keeping
      // that fallback is still the lesser evil than dropping the draw — it is
      // what shipped in every previous run, so leaving it keeps this change to
      // one variable — but it is counted, because these draws are the ones
      // whose pixels cannot be trusted.
      ++s_noMatch;
    }
    static uint64_t s_total = 0;
    if ((++s_total % 5000) == 0) {
      REXLOG_INFO("translator: SLOT heuristic already right {}, corrected {}, "
                  "shader slot not among validated fetches {}",
                  s_agreed, s_corrected, s_noMatch);
    }
    if (by_shader && by_shader != heuristic_pick && log_this) {
      REXLOG_INFO("translator: vfetch slot CORRECTED {} -> {} (shader names it)",
                  heuristic_pick->slot, by_shader->slot);
    }
  }

  // Measure the guess against the shader's own answer. Deliberately placed
  // before the size check and the read below, so draws that go on to be
  // rejected still contribute to the histogram — those are exactly the
  // populations (strides 12, 16, 20, 36) the heuristic never gets validated on.
  //
  // Nothing below this line consumes the decoded stride. dc.vertex_stride is
  // still pick->stride, the division guess, and the renderer's stride-28 gate
  // sees exactly what it saw before. That is the point: this round measures.
  LogStrideComparison(dc, pick->stride, pick->slot);

  // Read using the stride the shader declares when we have it. The division
  // guess is what sized this read before, and where the two disagree — 2600
  // draws a run, nearly all of them the guess saying 8 where the shader says
  // 16 — sizing by the guess reads only half the buffer and the transcode then
  // has to fall back. Clamped to what the fetch constant says is there.
  uint32_t read_stride = pick->stride;
  if (REXCVAR_GET(vertex_transcode) && m_currentVs && m_currentVs->ok) {
    for (const auto& a : m_currentVs->attrs) {
      if (a.fetch_slot == pick->slot && a.stride_bytes) {
        read_stride = a.stride_bytes;
        break;
      }
    }
  }
  if (read_stride == 0) return;
  uint32_t bytes = dc.vertex_count * read_stride;
  if (bytes > pick->size_bytes) {
    // The declared stride overruns the buffer the fetch constant describes.
    // Prefer the guess in that case rather than reading short or over.
    read_stride = pick->stride;
    bytes = dc.vertex_count * read_stride;
    if (read_stride == 0 || bytes > pick->size_bytes) return;
  }
  if (!ReadGuestRange(guest_base, pick->address, bytes, dc.vertices, "vertex buffer")) {
    dc.vertices.clear();
    return;
  }
  dc.vertex_stride = read_stride;

  // Apply the fetch constant's endian swap, per mode. This used to apply a
  // 32-bit swap for any non-zero mode, on the grounds that the 16-bit formats
  // could not be identified without the shader — which is no longer true, and
  // the assumption was wrong anyway: both 8in16 (1) and 8in32 (2) occur in this
  // game's fetch constants, 13 and 26 of the first 52 logged.
  ApplyFetchEndian(dc.vertices.data(), dc.vertices.size(), pick->endian);

  // Transcode into the one layout the game PSO describes: POSITION float3 at 0,
  // COLOR float4 at 12, stride 28. Everything above this point produced the
  // guest's own layout, whatever it happens to be, and the renderer then threw
  // away every draw that was not already 28 bytes — 230 of ~320 per frame.
  TranscodeVertices(dc, *pick);

  // Hex dump the first accepted buffer. This is the verdict for the round:
  // plausible float positions mean the fetch path works, noise means the slot
  // or the stride is wrong however good the counts look.
  static bool s_dumped = false;
  if (!s_dumped) {
    s_dumped = true;
    REXLOG_INFO("translator: FIRST VERTEX BUFFER slot {} addr=0x{:08X} "
                "stride={} vtcs={} ({} B of {} B)",
                pick->slot, pick->address, pick->stride, dc.vertex_count, bytes,
                pick->size_bytes);
    const uint32_t show = dc.vertex_count < 4 ? dc.vertex_count : 4;
    for (uint32_t v = 0; v < show; ++v) {
      std::string hex, flt;
      for (uint32_t b = 0; b < pick->stride && b + 3 < pick->stride; b += 4) {
        uint32_t w;
        std::memcpy(&w, dc.vertices.data() + v * pick->stride + b, 4);
        float f;
        std::memcpy(&f, &w, 4);
        hex += fmt::format("{:08X} ", w);
        flt += fmt::format("{:.3f} ", f);
      }
      REXLOG_INFO("translator:   v[{}] {}| {}", v, hex, flt);
    }
  }
}

void Pm4Translator::LogNdc(const DrawCall& dc) const {
  static int s_logged = 0;
  if (s_logged >= 10 || dc.vertices.empty() || dc.vertex_stride < 12) return;
  ++s_logged;

  const uint32_t show = dc.vertex_count < 3 ? dc.vertex_count : 3;
  for (uint32_t v = 0; v < show; ++v) {
    const uint8_t* p = dc.vertices.data() + size_t(v) * dc.vertex_stride;
    float in[4] = {0, 0, 0, 1};
    for (uint32_t c = 0; c < 3; ++c) std::memcpy(&in[c], p + c * 4, 4);
    float o[4];
    for (uint32_t r = 0; r < 4; ++r) {
      o[r] = dc.mvp[r * 4 + 0] * in[0] + dc.mvp[r * 4 + 1] * in[1] +
             dc.mvp[r * 4 + 2] * in[2] + dc.mvp[r * 4 + 3] * in[3];
    }
    const bool on_screen = o[3] != 0.0f && o[0] >= -o[3] && o[0] <= o[3] &&
                           o[1] >= -o[3] && o[1] <= o[3];
    REXLOG_INFO("translator: NDC prim={} v[{}] window({:.2f} {:.2f} {:.2f}) "
                "-> clip({:.4f} {:.4f} {:.4f} {:.4f}) [{}]",
                dc.prim_type, v, in[0], in[1], in[2], o[0], o[1], o[2], o[3],
                on_screen ? "on screen" : "OFF SCREEN");
  }
}

void Pm4Translator::FinalizeDraw(DrawCall& dc) {
  BuildViewportMvp(dc.mvp);
  dc.topology = MapTopology(dc.prim_type);

  if (static_cast<PrimitiveType>(dc.prim_type) == PrimitiveType::kRectangleList) {
    const uint32_t rects = ExpandRectangleList(dc);
    static uint32_t s_rects = 0, s_failed = 0;
    if (rects) s_rects += rects; else ++s_failed;
    static int s_logged = 0;
    if (s_logged < 10) {
      ++s_logged;
      REXLOG_INFO("translator: RectangleList expanded {} rects -> {} verts, {} "
                  "indices (running total {} rects, {} unexpandable)",
                  rects, dc.vertex_count, dc.index_count, s_rects, s_failed);
    }
  } else if (static_cast<PrimitiveType>(dc.prim_type) ==
             PrimitiveType::kQuadList) {
    // The largest single population in a frame — 4786 of 9664 logged draws in
    // mx_118, more than TriangleStrip and RectangleList combined — and all of
    // it was being dropped on MapTopology's default case.
    //
    // Two things counted rather than assumed. `real_idx` is draws whose index
    // buffer is not the sequential one an auto-draw synthesizes: the rectangle
    // path asserts these are always auto in this game, and that claim is worth
    // testing separately rather than inheriting. `no_verts` is draws whose
    // vertex data did not resolve — the sampled QuadList line reads
    // `verts=0 B`, and if that is typical then expansion produces correctly
    // formed empty draws and the submitted count will not move. That is a real
    // outcome, so it gets a number instead of a workaround.
    bool sequential = true;
    {
      const uint32_t istride = dc.index_16bit ? 2u : 4u;
      const uint32_t have = uint32_t(dc.indices.size() / istride);
      for (uint32_t i = 0; i < have && sequential; ++i) {
        uint32_t v;
        if (dc.index_16bit) {
          uint16_t h;
          std::memcpy(&h, dc.indices.data() + size_t(i) * 2, 2);
          v = h;
        } else {
          std::memcpy(&v, dc.indices.data() + size_t(i) * 4, 4);
        }
        if (v != i) sequential = false;
      }
    }
    const bool had_verts = !dc.vertices.empty();

    const uint32_t quads = ExpandQuadList(dc);
    static uint32_t s_quads = 0, s_failed = 0, s_real_idx = 0, s_no_verts = 0;
    if (quads) s_quads += quads; else ++s_failed;
    if (!sequential) ++s_real_idx;
    if (!had_verts) ++s_no_verts;
    static int s_logged = 0;
    if (s_logged < 10) {
      ++s_logged;
      REXLOG_INFO("translator: QuadList expanded {} quads -> {} verts, {} "
                  "indices (running total {} quads, {} unexpandable, {} with a "
                  "real index buffer, {} with no vertex data)",
                  quads, dc.vertex_count, dc.index_count, s_quads, s_failed,
                  s_real_idx, s_no_verts);
    }
  } else if (dc.topology == HostTopology::kUndefined) {
    // Nothing maps this — TriangleFan and the exotic quad/polygon types. The
    // renderer drops it; count them so the next round knows the size of it.
    static uint32_t s_dropped[64] = {};
    const uint32_t t = dc.prim_type & 0x3F;
    if (s_dropped[t]++ == 0) {
      REXLOG_INFO("translator: no host topology for prim_type={} — dropping "
                  "these (first occurrence)", dc.prim_type);
    }
  }

  LogShaderConstBases();
  LogSurface(dc);
  LogNdc(dc);
  ProbeAluMatrices(dc);
}

// Read-only probe. Nothing about which guest surface a draw targets reaches the
// renderer — every translated draw is submitted into the one host render target
// — so if the guest renders several passes per frame (shadow map, reflection,
// the main scene, post chains) they are all being flattened into one image and
// whichever pass paints last wins. The per-pass viewport sizes already recorded
// in AGENTS.md (1280x720 down to 1x1) say it does. This counts the distinct
// colour surfaces actually seen so that stops being an inference.
//
// RB_SURFACE_INFO (0x2000) low 14 bits are the surface pitch; RB_COLOR_INFO
// (0x2001) low 12 bits are the colour base in 4KB tiles and bits [15:12] the
// format. Both are already in the wholesale 0x2000..0x2FFF shadow.
void Pm4Translator::LogSurface(DrawCall& dc) {
  if (!m_ctxWritten) return;
  const uint32_t surface_info = m_ctxRegs[0x2000 - kCtxRegBase];
  const uint32_t color_info   = m_ctxRegs[0x2001 - kCtxRegBase];
  const uint32_t pitch = surface_info & 0x3FFF;
  const uint32_t base  = color_info & 0xFFF;
  const uint32_t fmt   = (color_info >> 12) & 0xF;
  dc.surface_base = base;
  dc.surface_pitch = pitch;

  // One entry per distinct (base, pitch, format) triple, so the count is the
  // number of colour surfaces the frame actually touched.
  const uint64_t key = (uint64_t(base) << 32) | (uint64_t(pitch) << 8) | fmt;
  static std::map<uint64_t, uint32_t> s_surfaces;
  const bool is_new = s_surfaces.find(key) == s_surfaces.end();
  ++s_surfaces[key];

  if (is_new) {
    REXLOG_INFO("translator: NEW colour surface base=0x{:03X} pitch={} fmt={} "
                "(prim={} verts={}) — {} distinct surfaces so far",
                base, pitch, fmt, dc.prim_type, dc.vertex_count,
                s_surfaces.size());
  }
  static uint32_t s_calls = 0;
  if ((++s_calls % 20000) == 0) {
    std::string hist;
    for (const auto& [k, n] : s_surfaces)
      hist += fmt::format("{:03X}/{}:{} ", uint32_t(k >> 32),
                          uint32_t((k >> 8) & 0xFFFFFF), n);
    REXLOG_INFO("translator: colour surfaces (base/pitch:draws) {}", hist);
  }
}

void Pm4Translator::HandleDrawIndx2(const Pm4Packet& pkt, uint8_t* guest_base,
                                    bool binned) {
  if (pkt.body.empty()) {
    REXLOG_WARN("translator: DRAW_INDX_2{} empty body", binned ? "_BIN" : "");
    return;
  }
  DrawIndx2Header h = ParseDrawIndx2Header(pkt.body[0]);
  if (!h.valid) {
    REXLOG_WARN("translator: DRAW_INDX_2{} invalid header dword0=0x{:08X} "
                "(idx={} prim={} src={} i32={}{})",
                binned ? "_BIN" : "", pkt.body[0],
                h.index_count, h.prim_type, h.src_sel, h.index_32bit ? 1 : 0,
                h.src_sel != 0x2 ? " (src_sel!=2)" : "");
    return;
  }

  DrawCall dc;
  dc.prim_type   = h.prim_type;
  dc.index_count = h.index_count;
  dc.index_16bit = !h.index_32bit;
  dc.binned      = binned;
  dc.vertex_stride = m_vtxStride > 0 ? m_vtxStride : 32;
  // dc.mvp is filled by FinalizeDraw, not from m_mvp — see the note there.

  if (h.src_sel == 0x2) {
    // Auto draw — GPU generates sequential indices 0..N-1. No inline index
    // data in the packet. Produce a draw call with synthetic sequential
    // indices so the host renderer can issue it.
    dc.vertex_count = h.index_count;
    uint32_t idx_size = h.index_32bit ? 4 : 2;
    dc.indices.resize(h.index_count * idx_size);
    if (h.index_32bit) {
      auto* p = reinterpret_cast<uint32_t*>(dc.indices.data());
      for (uint32_t i = 0; i < h.index_count; ++i) p[i] = i;
    } else {
      auto* p = reinterpret_cast<uint16_t*>(dc.indices.data());
      for (uint32_t i = 0; i < h.index_count; ++i) p[i] = uint16_t(i);
    }
    dc.valid = true;
    AttachVertices(dc, guest_base);
    FinalizeDraw(dc);
    REXLOG_INFO("translator: DRAW_INDX_2{} auto draw idx={} prim={} vtcs={} stride={} verts={} B",
                binned ? "_BIN" : "", h.index_count, h.prim_type,
                dc.vertex_count, dc.vertex_stride, dc.vertices.size());
    m_drawCalls.push_back(std::move(dc));
    return;
  }

  // src_sel == 0: inline indices in body[1..].
  // (DrawCall dc already declared above for the auto-draw path — reuse it.)
  uint32_t idx_size = h.index_32bit ? 4 : 2;
  uint32_t ib_bytes = h.index_count * idx_size;
  uint32_t ib_dwords = h.index_32bit
                           ? h.index_count
                           : (h.index_count + 1) / 2;  // 16-bit packed 2/dword

  if (ib_dwords + 1 > pkt.body.size()) {
    REXLOG_WARN("translator: DRAW_INDX_2{} body too small (have {} words, need {})",
                binned ? "_BIN" : "", pkt.body.size(), ib_dwords + 1);
    return;
  }

  // (DrawCall dc already declared + fields set above — reuse for inline path)

  dc.indices.resize(ib_bytes);
  // Inline indices are byteswapped LE in the packet's BE dwords. On Xenos, the
  // CPU's host-side format writes 16-bit indices in pairs to BE dwords; after
  // our parser's _byteswap_ulong on each dword, the resulting body[i] is in
  // host-LE order — so indices stored starting from the low 16 bits of each
  // dword are accessible directly.
  if (h.index_32bit) {
    std::memcpy(dc.indices.data(), pkt.body.data() + 1, ib_bytes);
  } else {
    // 16-bit indices — two per dword
    auto* out = reinterpret_cast<uint16_t*>(dc.indices.data());
    for (uint32_t i = 0; i < h.index_count; ++i) {
      // dword layout: low 16 bits = index[2k], high 16 bits = index[2k+1]
      uint32_t dword = pkt.body[1 + i / 2];
      out[i] = (i & 1) ? uint16_t(dword >> 16) : uint16_t(dword & 0xFFFF);
    }
  }

  // Compute max index → vertex count estimate
  uint32_t max_idx = 0;
  if (h.index_32bit) {
    auto* p = reinterpret_cast<const uint32_t*>(dc.indices.data());
    for (uint32_t i = 0; i < h.index_count; ++i)
      if (p[i] > max_idx) max_idx = p[i];
  } else {
    auto* p = reinterpret_cast<const uint16_t*>(dc.indices.data());
    for (uint32_t i = 0; i < h.index_count; ++i)
      if (p[i] > max_idx) max_idx = p[i];
  }
  dc.vertex_count = max_idx + 1;

  dc.valid = true;
  // This path used to stop here with a dead m_vtxBufAddr bounds check and a
  // deferred copy that never landed. AttachVertices does the real work now, and
  // it is the same call the two auto-draw paths make.
  AttachVertices(dc, guest_base);
  FinalizeDraw(dc);

  REXLOG_INFO("translator: DRAW_INDX_2{} idx={} prim={} i32={} vtcs={} stride={} verts={} B binMsLo=0x{:08X}",
              binned ? "_BIN" : "", h.index_count, h.prim_type,
              h.index_32bit ? 1 : 0, dc.vertex_count, dc.vertex_stride,
              dc.vertices.size(), m_binSelectLo);
  m_drawCalls.push_back(std::move(dc));
}

void Pm4Translator::HandleDrawIndx(const Pm4Packet& pkt, uint8_t* guest_base,
                                    bool binned) {
  // PM4_DRAW_INDX (Xenia): body[0]=viz_query_info, body[1]=draw header,
  //   body[2]=guest_base (when src_sel==0), body[3]=index_size|endianness.
  if (pkt.body.size() < 2) return;
  DrawIndx2Header h = ParseDrawIndx2Header(pkt.body[1]);
  if (!h.valid) {
    REXLOG_WARN("translator: DRAW_INDX{} invalid header dword1=0x{:08X}",
                binned ? "_BIN" : "", pkt.body[1]);
    return;
  }
  if (h.src_sel == 0x2) {
    // Auto draw — GPU generates sequential indices 0..N-1. Produce a
    // draw call with synthetic sequential indices.
    DrawCall dc;
    dc.prim_type   = h.prim_type;
    dc.index_count = h.index_count;
    dc.index_16bit = !h.index_32bit;
    dc.binned      = binned;
    dc.vertex_stride = m_vtxStride > 0 ? m_vtxStride : 32;
    dc.vertex_count = h.index_count;
    uint32_t idx_size = h.index_32bit ? 4 : 2;
    dc.indices.resize(h.index_count * idx_size);
    if (h.index_32bit) {
      auto* p = reinterpret_cast<uint32_t*>(dc.indices.data());
      for (uint32_t i = 0; i < h.index_count; ++i) p[i] = i;
    } else {
      auto* p = reinterpret_cast<uint16_t*>(dc.indices.data());
      for (uint32_t i = 0; i < h.index_count; ++i) p[i] = uint16_t(i);
    }
    dc.valid = true;
    AttachVertices(dc, guest_base);
    FinalizeDraw(dc);
    REXLOG_INFO("translator: DRAW_INDX{} auto draw idx={} prim={} vtcs={} stride={} verts={} B",
                binned ? "_BIN" : "", h.index_count, h.prim_type,
                dc.vertex_count, dc.vertex_stride, dc.vertices.size());
    m_drawCalls.push_back(std::move(dc));
    return;
  }
  if (pkt.body.size() < 4) {
    REXLOG_WARN("translator: DRAW_INDX{} body too small for indexed draw ({})",
                binned ? "_BIN" : "", pkt.body.size());
    return;
  }
  uint32_t ib_guest_base = pkt.body[2];
  uint32_t ib_size_word   = pkt.body[3];
  uint32_t ib_size        = ib_size_word & 0x00FFFFFF;
  uint32_t ib_endianness  = ib_size_word >> 30;
  ib_size *= h.index_32bit ? 4 : 2;

  if (!guest_base) {
    REXLOG_WARN("translator: DRAW_INDX{} no guest_base, indices not capturable",
                binned ? "_BIN" : "");
    return;
  }
  DrawCall dc;
  dc.prim_type   = h.prim_type;
  dc.index_count = h.index_count;
  dc.index_16bit = !h.index_32bit;
  dc.binned      = binned;
  dc.vertex_stride = m_vtxStride > 0 ? m_vtxStride : 32;

  // body[2] is a guest physical address (NOT masked) — the game places the
  // index buffer anywhere in main RAM (typical IXB addrs: 0x13_xxx_xxx).
  // Translation: host_ptr = guest_mem_base + ib_guest_base (no offset for
  // addrs < 0xE0000000 — see REX_LOAD macros / PhysicalHostOffset).
  // ReadGuestRange carries the bounds check and the page-commit probe.
  if (!ReadGuestRange(guest_base, ib_guest_base, ib_size, dc.indices,
                      binned ? "DRAW_INDX_BIN IB" : "DRAW_INDX IB")) {
    return;
  }
  // Guest memory is big-endian — byteswap each index to host LE.
  if (h.index_32bit) {
    auto* p = reinterpret_cast<uint32_t*>(dc.indices.data());
    for (uint32_t i = 0; i < h.index_count; ++i)
      p[i] = __builtin_bswap32(p[i]);
  } else {
    auto* p = reinterpret_cast<uint16_t*>(dc.indices.data());
    for (uint32_t i = 0; i < h.index_count; ++i)
      p[i] = __builtin_bswap16(p[i]);
  }

  // Compute max index → vertex count estimate
  uint32_t max_idx = 0;
  if (h.index_32bit) {
    auto* p = reinterpret_cast<const uint32_t*>(dc.indices.data());
    for (uint32_t i = 0; i < h.index_count; ++i)
      if (p[i] > max_idx) max_idx = p[i];
  } else {
    auto* p = reinterpret_cast<const uint16_t*>(dc.indices.data());
    for (uint32_t i = 0; i < h.index_count; ++i)
      if (p[i] > max_idx) max_idx = p[i];
  }
  dc.vertex_count = max_idx + 1;

  // Debug: dump first few indices for sanity check (zero = bad, sequential = good)
  if (h.index_count >= 1) {
    uint32_t show = h.index_count < 4 ? h.index_count : 4;
    std::string idx_str;
    if (h.index_32bit) {
      auto* p = reinterpret_cast<const uint32_t*>(dc.indices.data());
      for (uint32_t i = 0; i < show; ++i)
        idx_str += fmt::format("0x{:08X} ", p[i]);
    } else {
      auto* p = reinterpret_cast<const uint16_t*>(dc.indices.data());
      for (uint32_t i = 0; i < show; ++i)
        idx_str += fmt::format("0x{:04X} ", p[i]);
    }
    REXLOG_INFO("translator:   indices[0..{}] @0x{:08X}: {}", show, ib_guest_base, idx_str);
  }

  dc.valid = true;
  AttachVertices(dc, guest_base);
  FinalizeDraw(dc);

  REXLOG_INFO("translator: DRAW_INDX{} ib_addr=0x{:08X} ib_size={} idx={} "
              "prim={} vtcs={} endian={} verts={} B",
              binned ? "_BIN" : "", ib_guest_base, ib_size,
              h.index_count, h.prim_type, dc.vertex_count, ib_endianness,
              dc.vertices.size());
  m_drawCalls.push_back(std::move(dc));
}

void Pm4Translator::HandleSetConstant(const Pm4Packet& pkt) {
  if (pkt.body.empty()) return;
  uint32_t offset_type = pkt.body[0];
  uint32_t index = offset_type & 0x7FF;
  uint32_t type  = (offset_type >> 16) & 0xFF;

  // Resolve the actual register index per Xenia's type encoding.
  uint32_t reg_index = 0;
  switch (type) {
    case 0: reg_index = 0x4000 + index; break;  // ALU float constants
    case 1: reg_index = 0x4800 + index; break;  // FETCH (vertex fetch consts)
    case 2: reg_index = 0x4900 + index; break;  // BOOL
    case 3: reg_index = 0x4908 + index; break;  // LOOP
    case 4: reg_index = 0x2000 + index; break;  // REGISTERS (context)
    default: return;  // unknown type — ignore
  }

  // Type 0 is the ALU float constant file. This was computing reg_index and
  // then never using it for anything but fetch constants, so a SET_CONSTANT
  // carrying real ALU constants would have been counted as "the game emits no
  // SET_CONSTANT at all" — true of the packet count, but it would have gone
  // nowhere even if it were not.
  if (type == 0 && pkt.body.size() > 1) {
    const uint32_t n = static_cast<uint32_t>(pkt.body.size()) - 1;
    if (index + n <= kAluConstDwords) {
      for (uint32_t i = 0; i < n; ++i) m_aluConsts[index + i] = pkt.body[i + 1];
      NoteAluConstWrite(kAluSourceSetConstant, reg_index, n);
    }
  }

  // Vertex fetch constants (type=1). Each fetch constant is a 3-dword
  // (12-byte) descriptor: dword0=base+size, dword1=format+stride, dword2=...
  if (type == 1 && pkt.body.size() >= 4) {
    uint32_t dword0 = pkt.body[1];
    uint32_t dword1 = pkt.body[2];
    // Xenos FETCH constant dword0 layout:
    //   [31:0] = base (GPU VA, includes endianness in bits[1:0])
    // dword1:
    //   [31:24] = stride (bytes per vertex), maybe [31:27] for tessellated
    if (dword0 != 0) {
      m_vtxBufAddr = dword0;
      uint32_t stride = (dword1 >> 24) & 0xFF;
      if (stride == 0) stride = 32;
      m_vtxStride = stride;
    }
  }

  // ALU float constants (type=0). Track hfloat index 0..15 as MVP.
  // Per Xenia, ALU consts are 16 float4 registers; first 4 vec4s ~~ MVP.
  //
  // Nothing reads m_mvp any more — DrawCall::mvp comes from the viewport
  // registers (BuildViewportMvp), because this game emits neither SET_CONSTANT
  // nor SET_SHADER_CONSTANTS and so left m_mvp identically zero, which silently
  // collapsed every translated vertex to the origin. If this ever fires, the
  // premise has changed and the transform source is worth revisiting.
  if (type == 0 && index < 16) {
    static bool s_logged = false;
    if (!s_logged) {
      s_logged = true;
      REXLOG_WARN("translator: SET_CONSTANT type=0 seen — the game DOES emit "
                  "ALU constants; reconsider the viewport-only transform");
    }
    for (size_t i = 1; i < pkt.body.size() && index + (i - 1) < 16; ++i) {
      float val;
      std::memcpy(&val, &pkt.body[i], 4);
      m_mvp[index + (i - 1)] = val;
    }
  }
}

void Pm4Translator::HandleSetShaderConstants(const Pm4Packet& pkt) {
  // PM4_SET_SHADER_CONSTANTS (0x56) — incremental shader constant update.
  // body[0] = base index (16-bit), body[1..] = float4 constant updates.
  if (pkt.body.empty()) return;
  uint32_t base_idx = pkt.body[0] & 0xFFFF;
  if (base_idx < 16 && pkt.body.size() > 1) {
    // As in HandleSetConstant: nothing reads m_mvp any more. This opcode does
    // not occur in any captured frame; if it starts to, say so.
    static bool s_logged = false;
    if (!s_logged) {
      s_logged = true;
      REXLOG_WARN("translator: SET_SHADER_CONSTANTS seen — reconsider the "
                  "viewport-only transform");
    }
    for (size_t i = 1; i < pkt.body.size() && base_idx + (i - 1) < 16; ++i) {
      float val;
      std::memcpy(&val, &pkt.body[i], 4);
      m_mvp[base_idx + (i - 1)] = val;
    }
  }
}

void Pm4Translator::HandleBinMaskLo(const Pm4Packet& pkt) {
  if (!pkt.body.empty()) m_binMaskLo = pkt.body[0];
}
void Pm4Translator::HandleBinMaskHi(const Pm4Packet& pkt) {
  if (!pkt.body.empty()) m_binMaskHi = pkt.body[0];
}
void Pm4Translator::HandleBinSelectLo(const Pm4Packet& pkt) {
  if (!pkt.body.empty()) m_binSelectLo = pkt.body[0];
}
void Pm4Translator::HandleBinSelectHi(const Pm4Packet& pkt) {
  if (!pkt.body.empty()) m_binSelectHi = pkt.body[0];
}

void Pm4Translator::TranslatePackets(const std::vector<Pm4Packet>& packets,
                                      uint8_t* guest_base,
                                      uint32_t /*gpu_phys_base*/) {
  if (!guest_base) return;

  for (const auto& pkt : packets) {
    // Type0 register writes carry this game's vertex fetch constants. It emits
    // no SET_CONSTANT (0x2D) at all, so discarding Type0 — as this loop used to
    // — meant no vertex buffer address was ever learned.
    if (pkt.type == PacketType::Type0) {
      ApplyType0Write(pkt.reg_base, pkt.body);
      continue;
    }
    if (pkt.type != PacketType::Type3) continue;

    // Debug: log only indexed draws (skipping the auto-draw spam from Bink quads).
    if (pkt.opcode == 0x22 && pkt.body.size() >= 4) {
      REXLOG_INFO("translator: saw DRAW_INDX body_words={} pred={}",
                  pkt.body.size(), pkt.predicate);
    }

    // Switch on real Xenos Type3 opcodes (see xenos.h::Type3Opcode).
    switch (pkt.opcode) {
      // ---- Draw opcodes ----
      case 0x22: HandleDrawIndx(pkt, guest_base, false); break;       // DRAW_INDX
      case 0x34: HandleDrawIndx(pkt, guest_base, true);  break;        // DRAW_INDX_BIN
      case 0x35: HandleDrawIndx2(pkt, guest_base, true);  break;        // DRAW_INDX_2_BIN ← gameplay
      case 0x36: HandleDrawIndx2(pkt, guest_base, false); break;        // DRAW_INDX_2

      // ---- State-setting opcodes (vertex & shader constants) ----
      case 0x2D: HandleSetConstant(pkt);            break;            // SET_CONSTANT
      case 0x56: HandleSetShaderConstants(pkt);      break;            // SET_SHADER_CONSTANTS

      // ---- Shader microcode ----
      // Both carry the vertex layout — the stride and formats the fetch
      // constant does not hold. Read-only this round: they update m_currentVs
      // and nothing else. Until now 0x2B was not even named and 0x27 fell
      // through the default below.
      case 0x2B: HandleImLoadImmediate(pkt);          break;           // IM_LOAD_IMMEDIATE
      case 0x27: HandleImLoad(pkt, guest_base);        break;           // IM_LOAD

      // ---- ALU constants ----
      // The door this game actually uses. 271/frame post-load, 0 at boot; it
      // emits no SET_CONSTANT at all, which is why the ALU file looked empty
      // and the MVP was zero. Shadowed and probed, not yet consumed.
      case 0x2F: HandleLoadAluConstant(pkt, guest_base); break;         // LOAD_ALU_CONSTANT

      // ---- Binning control opcodes (legacy-alias opcode names fixed) ----
      case 0x60: HandleBinMaskLo(pkt);    break;                       // SET_BIN_MASK_LO
      case 0x61: HandleBinMaskHi(pkt);    break;                       // SET_BIN_MASK_HI
      case 0x62: HandleBinSelectLo(pkt);  break;                       // SET_BIN_SELECT_LO
      case 0x63: HandleBinSelectHi(pkt);  break;                        // SET_BIN_SELECT_HI

      // ---- Ignored/opaque opcodes ----
      // 0x10 NOP / 0x3B INVALIDATE_STATE / 0x3C WAIT_REG_MEM /
      // 0x21 REG_RMW / 0x3E REG_TO_MEM / 0x3D MEM_WRITE / 0x3F INDIRECT_BUFFER /
      // 0x46 EVENT_WRITE / 0x58 EVENT_WRITE_SHD / 0x5B EVENT_WRITE_ZPD /
      // 0x5E CONTEXT_UPDATE / 0x45 COND_WRITE — none contribute draw data.
      default:
        break;
    }
  }
}

}  // namespace mx::pm4