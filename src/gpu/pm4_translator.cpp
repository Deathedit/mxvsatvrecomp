#include "gpu/pm4_translator.h"

#include <cstring>
#include <system_error>

#include <rex/logging.h>

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
}

float Pm4Translator::CtxFloat(uint32_t reg, float fallback) const {
  if (!m_ctxWritten || reg < kCtxRegBase || reg >= kCtxRegBase + kCtxRegCount)
    return fallback;
  float f;
  std::memcpy(&f, &m_ctxRegs[reg - kCtxRegBase], 4);
  return f;
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
    // RectangleList is not a topology, it is an expansion — ExpandRectangleList
    // rewrites it to a triangle list and sets the topology itself.
    // TriangleFan has no D3D12 equivalent either, but unlike rectangles it is
    // not handled this round: it is dropped and counted.
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

  const uint32_t bytes = dc.vertex_count * pick->stride;
  if (bytes > pick->size_bytes) return;
  if (!ReadGuestRange(guest_base, pick->address, bytes, dc.vertices, "vertex buffer")) {
    dc.vertices.clear();
    return;
  }
  dc.vertex_stride = pick->stride;

  // Guest memory is big-endian. endian == 2 is the usual 8-in-32 swap; treat
  // anything non-zero as a 32-bit swap, which is right for float and packed
  // 4-byte attributes and wrong only for 16-bit ones we cannot identify without
  // the shader's vfetch format anyway.
  if (pick->endian != 0) {
    const size_t words = dc.vertices.size() / 4;
    auto* p = reinterpret_cast<uint32_t*>(dc.vertices.data());
    for (size_t i = 0; i < words; ++i) p[i] = __builtin_bswap32(p[i]);
  }

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

  LogNdc(dc);
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