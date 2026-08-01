#include "pm4_translator.h"

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

void Pm4Translator::HandleDrawIndx2(const Pm4Packet& pkt, bool binned) {
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
  std::memcpy(dc.mvp, m_mvp, sizeof(m_mvp));

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
    REXLOG_INFO("translator: DRAW_INDX_2{} auto draw idx={} prim={} vtcs={} stride={}",
                binned ? "_BIN" : "", h.index_count, h.prim_type,
                dc.vertex_count, dc.vertex_stride);
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

  // Vertex buffer fetch (optional — if we have a known VTG fetch constant).
  if (m_vtxBufAddr != 0) {
    uint32_t va = m_vtxBufAddr;
    if (va < kGuestPhysMax) {
      uint32_t vb_bytes = dc.vertex_count * dc.vertex_stride;
      // (Vertex copy is deferred; the translator needs the guest base.)
      (void)vb_bytes;
    }
  }
  dc.valid = true;

  REXLOG_INFO("translator: DRAW_INDX_2{} idx={} prim={} i32={} vtcs={} stride={} binMsLo=0x{:08X}",
              binned ? "_BIN" : "", h.index_count, h.prim_type,
              h.index_32bit ? 1 : 0, dc.vertex_count, dc.vertex_stride,
              m_binSelectLo);
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
    std::memcpy(dc.mvp, m_mvp, sizeof(m_mvp));
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
    REXLOG_INFO("translator: DRAW_INDX{} auto draw idx={} prim={} vtcs={} stride={}",
                binned ? "_BIN" : "", h.index_count, h.prim_type,
                dc.vertex_count, dc.vertex_stride);
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
  // body[2] is a guest physical address (NOT masked) — game placed the
  // index buffer anywhere in main RAM (typical IXB addrs: 0x13_xxx_xxx).
  // Translation: host_ptr = guest_mem_base + ib_guest_base (no offset for
  // addrs < 0xE0000000 — see REX_LOAD macros / PhysicalHostOffset).
  if (ib_guest_base >= kGuestPhysMax) {
    REXLOG_WARN("translator: DRAW_INDX{} ib_guest_base=0x{:08X} out-of-range",
                binned ? "_BIN" : "", ib_guest_base);
    return;
  }
  // Conservative bounds: ensure the entire IB read stays within guest RAM.
  if (ib_guest_base + ib_size > kGuestPhysMax) {
    REXLOG_WARN("translator: DRAW_INDX{} IB read overruns guest RAM "
                "(ib_addr=0x{:08X} ib_size={})", binned ? "_BIN" : "",
                ib_guest_base, ib_size);
    return;
  }
#if defined(_WIN32)
  // Probe the host-side page state — ReXGlue's guest memory view reserves
  // 512MB but may not commit every page. A memcpy into an uncommitted page
  // is silently swallowed by REX_FUNC's SEH frame, leaving the host thread
  // indefinitely stuck. We CAN'T VirtualAlloc-commit pages ourselves — that
  // breaks the plugin's SharedMemory page tracking (it has its own commit
  // state and becomes inconsistent if we mutate the underlying pages). Just
  // probe + skip memcpy if the page is uncommitted (the IB data lives in
  // the plugin's host GPU memory copy, not accessible from here).
  MemoryBasicInformationLite mbi{};
  const void* probe = guest_base + ib_guest_base;
  if (VirtualQuery(probe, &mbi, sizeof(mbi)) == 0 ||
      !(mbi.state & 0x1000 /* MEM_COMMIT */) ||
      !(mbi.protect & 0x06 /* PAGE_READONLY|PAGE_READWRITE */)) {
    // Page not accessible from host — bail. Don't touch VirtualAlloc.
    REXLOG_INFO("translator: DRAW_INDX{} ib_addr=0x{:08X} skip (page not committed "
                "in host view — lives in plugin's own GPU memory copy)",
                binned ? "_BIN" : "", ib_guest_base);
    return;
  }
#endif

  DrawCall dc;
  dc.prim_type   = h.prim_type;
  dc.index_count = h.index_count;
  dc.index_16bit = !h.index_32bit;
  dc.binned      = binned;
  dc.vertex_stride = m_vtxStride > 0 ? m_vtxStride : 32;
  std::memcpy(dc.mvp, m_mvp, sizeof(m_mvp));

  // Copy raw bytes (big-endian) from guest memory into dc.indices, then
  // byteswap each u16/u32 to host LE so the host renderer can use them
  // directly.
  dc.indices.resize(ib_size);
  std::memcpy(dc.indices.data(), guest_base + ib_guest_base, ib_size);
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

  REXLOG_INFO("translator: DRAW_INDX{} ib_addr=0x{:08X} ib_size={} idx={} "
              "prim={} vtcs={} endian={}",
              binned ? "_BIN" : "", ib_guest_base, ib_size,
              h.index_count, h.prim_type, dc.vertex_count, ib_endianness);
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
  if (type == 0 && index < 16) {
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
      case 0x35: HandleDrawIndx2(pkt, true);  break;                   // DRAW_INDX_2_BIN ← gameplay
      case 0x36: HandleDrawIndx2(pkt, false); break;                   // DRAW_INDX_2

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