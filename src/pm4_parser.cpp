#include "pm4_parser.h"

#include <cstdio>
#include <cstdlib>
#include <Windows.h>

#include "xenos_gpu_state.h"  // shared register-name catalog (single source of truth)

namespace mx::pm4 {

namespace {

inline uint32_t bswap(uint32_t x) { return _byteswap_ulong(x); }

struct OpcodeEntry {
  uint8_t opcode;
  const char* name;
};

constexpr OpcodeEntry kOpcodeNames[] = {
  // Xenos Type3 opcodes (from C:\rexglue-sdk\include\rex\graphics\xenos.h)
  {0x10, "NOP"},
  {0x21, "REG_RMW"},
  {0x22, "DRAW_INDX"},
  {0x23, "VIZ_QUERY"},
  {0x25, "SET_STATE"},
  {0x26, "WAIT_FOR_IDLE"},
  {0x27, "IM_LOAD"},
  {0x2D, "SET_CONSTANT"},
  {0x2F, "LOAD_ALU_CONSTANT"},
  {0x34, "DRAW_INDX_BIN"},
  {0x35, "DRAW_INDX_2_BIN"},
  {0x36, "DRAW_INDX_2"},
  {0x3C, "WAIT_REG_MEM"},
  {0x3D, "MEM_WRITE"},
  {0x3E, "REG_TO_MEM"},
  {0x3F, "INDIRECT_BUFFER"},
  {0x44, "COND_EXEC"},
  {0x45, "COND_WRITE"},
  {0x46, "EVENT_WRITE"},
  {0x48, "ME_INIT"},
  {0x4F, "MEM_WRITE_CNTR"},
  {0x52, "WAIT_REG_EQ"},
  {0x53, "WAIT_REG_GTE"},
  {0x55, "SET_CONSTANT2"},
  {0x56, "SET_SHADER_CONSTANTS"},
  {0x58, "EVENT_WRITE_SHD"},
  {0x59, "EVENT_WRITE_CFL"},
  {0x5A, "EVENT_WRITE_EXT"},
  {0x5B, "EVENT_WRITE_ZPD"},
  {0x5C, "WAIT_UNTIL_READ"},
  {0x5D, "WAIT_IB_PFD_COMPLETE"},
  // Legacy aliases (AMD R600 naming, kept for back-compat with existing
  // xenos_gpu_state.cpp ApplyType3Packet switch)
  {0x37, "INDIRECT_BUFFER_PFD"},
  {0x60, "SET_CONFIG_REG"},
  {0x61, "SET_CONTEXT_REG"},
  {0x62, "SET_ALU_CONST"},
  {0x63, "SET_BOOL_CONST"},
  {0x64, "SET_LOOP_CONST"},
  {0x65, "SET_RESOURCE"},
  {0x66, "SET_SAMPLER"},
  {0x67, "SET_CTL_CONST"},
  {0x68, "SET_VTX_RESOURCE"},
  {0x69, "SET_VTX_SAMPLER"},
  {0x6A, "INDEX_TYPE"},
  {0x6C, "STRMOUT_BUFFER_UPDATE"},
  {0x6F, "NUM_INSTANCES"},
};

// Note: register-name catalog lives in xenos_gpu_state.cpp kRegNames; the
// previous local duplicate here (which was missing SQ_*/DISPLAY_*/CONTEXT_ID_*
// entries — pm4_dump.txt was showing ??? for those) has been replaced by a
// delegation to mx::gpu::XenosGpuState::RegisterName in the implementation
// below.

}  // namespace

const char* Pm4Parser::OpcodeName(uint32_t opcode) {
  for (const auto& e : kOpcodeNames) {
    if (e.opcode == (uint8_t)opcode) return e.name;
  }
  return nullptr;
}

const char* Pm4Parser::RegisterName(uint32_t reg) {
  // Delegate to the unified catalog in xenos_gpu_state.cpp so the dump
  // resolves register names like SQ_LOAD_CTL / DISPLAY_MODE / RB_COLOR_INFO
  // instead of showing ??? for entries the parser's old local list lacked.
  return mx::gpu::XenosGpuState::RegisterName(reg);
}

Pm4Packet Pm4Parser::DecodePacket(const uint32_t* data, uint32_t max_dwords,
                                   uint32_t& consumed, uint32_t guest_addr) {
  Pm4Packet pkt;
  pkt.guest_addr = guest_addr;
  consumed = 0;

  if (max_dwords == 0) return pkt;

  uint32_t hdr = data[0];
  pkt.raw_header = hdr;

  static constexpr uint32_t kSentinels[] = {
    0xDEADBEEF, 0xFFFFFFFF, 0xFFFFF8FF, 0x77777777,
    0x00000000,
  };
  bool is_sentinel = false;
  for (auto s : kSentinels) {
    if (hdr == s) { is_sentinel = true; break; }
  }
  if (is_sentinel) {
    pkt.type = PacketType::Type2;
    consumed = 1;
    return pkt;
  }

  uint32_t hdr_type = hdr >> 30;

  if (hdr_type == 3) {
    // Xenos Type3 format: tt cccccccc ccccccc ?ooooooo ???????p
    //   bits[31:30] = 3
    //   bits[29:16] = COUNT (14-bit, N-1 data words)
    //   bits[14:8]  = OPCODE (7-bit) — per Xenia/skylane HH:XUK sesoneuref
    //   bit[0]      = predicate
    pkt.type = PacketType::Type3;
    pkt.opcode = (hdr >> 8) & 0x7F;
    pkt.body_word_count = ((hdr >> 16) & 0x3FFF) + 1;
    pkt.predicate = hdr & 1;
    pkt.sub_op = (hdr >> 8) & 0xFF;  // diagnostic: full byte incl unused bit+opcode
    if (pkt.opcode == 0x7F || pkt.body_word_count > 0x4000) {
      pkt.type = PacketType::Type2;
      consumed = 1;
      return pkt;
    }
    consumed = 1 + pkt.body_word_count;
    if (consumed > max_dwords) {
      // Don't discard the entire packet — parse as many body words as are
      // available. This happens with huge packets like DRAW_INDX_2_BIN that
      // carry 16K+ inline indices and may be split across ring buffer wraps.
      consumed = max_dwords;
    }
    for (uint32_t i = 1; i < consumed && i < max_dwords; ++i) {
      pkt.body.push_back(data[i]);
    }
  } else if (hdr_type == 2) {
    pkt.type = PacketType::Type2;
    consumed = 1;
  } else {
    pkt.type = PacketType::Type0;
    pkt.reg_base = hdr & 0xFFFF;
    uint32_t reg_cnt = ((hdr >> 16) & 0x3FFF) + 1;
    if (reg_cnt > 256 || pkt.reg_base * 4 > 0x30000) {
      pkt.type = PacketType::Type2;
      consumed = 1;
      return pkt;
    }
    consumed = 1 + reg_cnt;
    if (consumed > max_dwords) consumed = max_dwords;
    for (uint32_t i = 1; i < consumed; ++i) {
      pkt.body.push_back(data[i]);
    }
    pkt.reg_count = reg_cnt;
  }

  return pkt;
}

void Pm4Parser::ParseRange(const uint32_t* dwords, uint32_t count,
                           uint32_t base_guest_addr) {
  std::vector<uint32_t> swapped(count);
  for (uint32_t i = 0; i < count; ++i) {
    swapped[i] = bswap(dwords[i]);
  }

  uint32_t off = 0;
  while (off < count) {
    uint32_t consumed = 0;
    auto pkt = DecodePacket(swapped.data() + off, count - off, consumed,
                            base_guest_addr + off * 4);
    if (consumed == 0) break;
    off += consumed;
    packets_.push_back(std::move(pkt));
  }
}

void Pm4Parser::DumpPackets(const std::vector<Pm4Packet>& packets,
                              const char* filename) {
  char path[MAX_PATH];
  GetModuleFileNameA(nullptr, path, sizeof(path));
  char* last = strrchr(path, '\\');
  if (last) *(last + 1) = '\0';
  strcat_s(path, filename);

  FILE* f = nullptr;
  fopen_s(&f, path, "w");
  if (!f) return;

  uint32_t type3_count = 0, type0_count = 0, type2_count = 0;
  for (const auto& p : packets) {
    if (p.type == PacketType::Type3) ++type3_count;
    else if (p.type == PacketType::Type0) ++type0_count;
    else if (p.type == PacketType::Type2) ++type2_count;
  }
  fprintf(f, "PM4 Dump: %zu packets (Type3=%u Type0=%u Type2=%u)\n\n",
          packets.size(), type3_count, type0_count, type2_count);

  for (const auto& p : packets) {
    if (p.type == PacketType::Type3) {
      const char* name = OpcodeName(p.opcode);
      fprintf(f, "[0x%08X] Type3 %s(0x%02X) sub=0x%02X pred=%d cnt=%u",
              p.guest_addr, name ? name : "???", p.opcode, p.sub_op,
              p.predicate, p.body_word_count);
      if (!p.body.empty()) {
        fprintf(f, " data=[");
        for (size_t i = 0; i < p.body.size() && i < 64; ++i)
          fprintf(f, "%s0x%08X", i ? " " : "", p.body[i]);
        if (p.body.size() > 64) fprintf(f, " ...(+%zu)", p.body.size() - 64);
        fprintf(f, "]");
      }
      fprintf(f, "\n");
    } else if (p.type == PacketType::Type0) {
      const char* rname = RegisterName(p.reg_base);
      fprintf(f, "[0x%08X] Type0 reg=%s(0x%04X) cnt=%u",
              p.guest_addr, rname ? rname : "???", p.reg_base, p.reg_count);
      if (!p.body.empty()) {
        fprintf(f, " val=[");
        for (size_t i = 0; i < p.body.size() && i < 32; ++i)
          fprintf(f, "%s0x%08X", i ? " " : "", p.body[i]);
        if (p.body.size() > 32) fprintf(f, " ...(+%zu)", p.body.size() - 32);
        fprintf(f, "]");
      }
      fprintf(f, "\n");
    } else if (p.type == PacketType::Type2) {
    }
  }
  fclose(f);
  OutputDebugStringA(path);
  OutputDebugStringA("\n");
}

}  // namespace mx::pm4
