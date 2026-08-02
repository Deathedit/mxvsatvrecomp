#include "gpu/xenos_gpu_state.h"

#include <cstdio>
#include <algorithm>

#include <rex/logging.h>

namespace mx::gpu {

namespace {

struct RegName {
  uint32_t offset;
  const char* name;
};

constexpr RegName kRegNames[] = {
  {0x0001, "CONFIG_CTL"},
  {0x0007, "CONTEXT_ID_0"},
  {0x0008, "CONTEXT_ID_1"},
  {0x0009, "CONTEXT_ID_2"},
  {0x000B, "CONTEXT_ID_3"},
  {0x000D, "CONTEXT_ID_4"},
  {0x0500, "DISPLAY_MODE"},
  {0x05C8, "DISPLAY_TIMING"},
  {0x0A2F, "MC_BASE_ADDR"},
  {0x0A31, "MC_CTL"},
  {0x0E00, "DISP_TG_CTL"},
  {0x0E40, "DISP_DITHER"},
  {0x0F01, "DISP_UNKNOWN"},
  {0x1838, "SQ_STATE"},
  {0x1844, "SQ_ADDR_LO"},
  {0x1852, "SQ_PAGE"},
  {0x1921, "SQ_CONST_BASE"},
  {0x1922, "SQ_CONST_INDEX"},
  {0x1925, "SQ_CONST_DATA"},
  {0x1927, "SQ_CONST_COUNT"},
  {0x1930, "SQ_LOAD_CTL"},
  {0x1964, "SQ_LOAD_MODE"},
  {0x1973, "SQ_LOAD_GATE"},
  // Entries below were previously only in pm4_parser.cpp's now-deleted
  // duplicate catalog. Merged here as the single source of truth.
  {0x2000, "RB_SURFACE_INFO"},
  {0x2004, "RB_COLOR_INFO"},
  {0x2008, "RB_DEPTH_INFO"},
  {0x200C, "RB_COLOR0_MASK"},
  {0x2010, "RB_COLOR1_MASK"},
  {0x2014, "RB_COLOR2_MASK"},
  {0x2018, "RB_COLOR3_MASK"},
  {0x2020, "RB_BLEND0_CTL"},
  {0x2024, "RB_BLEND1_CTL"},
  {0x2028, "RB_BLEND2_CTL"},
  {0x202C, "RB_BLEND3_CTL"},
  {0x2030, "RB_BLEND_RED"},
  {0x2034, "RB_BLEND_GREEN"},
  {0x2038, "RB_BLEND_BLUE"},
  {0x203C, "RB_BLEND_ALPHA"},
  {0x2080, "RB_DISP_OUTPUT"},
  {0x22C0, "SQ_PROGRAM_CNTL"},
  {0x22C4, "SQ_CONTEXT_MISC"},
  {0x2304, "SPI_CONFIG_CNTL_1"},
  {0x2310, "SPI_PS_INPUT_CNTL_0"},
  {0x2314, "SPI_PS_INPUT_CNTL_1"},
  {0x2318, "SPI_PS_INPUT_CNTL_2"},
  {0x2400, "VGT_PRIMITIVE_TYPE"},
  {0x2404, "VGT_VTX_CNT"},
  {0x243C, "VGT_DRAW_INITIATOR"},
  {0x2440, "VGT_DMA_BASE"},
  {0x2444, "VGT_DMA_BASE_HI"},
  {0x2448, "VGT_DMA_INDEX_TYPE"},
  {0x244C, "VGT_DMA_NUM_INSTANCES"},
  {0x2800, "PA_SC_WINDOW_SCISSOR_TL"},
  {0x2804, "PA_SC_WINDOW_SCISSOR_BR"},
  {0x2840, "VGT_MIN_VTX_INDX"},
  {0x2844, "VGT_MAX_VTX_INDX"},
  {0x2848, "PA_SC_LINE_STIPPLE"},
  {0x284C, "VGT_INDX_OFFSET"},
  {0x2850, "PA_SC_SCREEN_SCISSOR_TL"},
  {0x2854, "PA_SC_SCREEN_SCISSOR_BR"},
  {0x286C, "PA_SC_WINDOW_OFFSET"},
  {0x2884, "PA_SC_AA_CONFIG"},
  {0x288C, "PA_SC_AA_MASK"},
  {0x28E4, "PA_SU_SC_MODE_CNTL"},
  {0x28E8, "PA_SU_VTX_CNTL"},
  {0x28EC, "PA_CL_VTE_CNTL"},
  {0x2908, "PA_CL_CLIP_CNTL"},
  {0x2910, "PA_CL_VS_OUT_CNTL"},
  {0x2914, "PA_CL_VS_OUT_CLIP_CNTL"},
  // 0x4800..0x48BF is the 192-dword shader fetch constant file, not
  // HW_MODE_TABLE — the old name made every dump of a vertex fetch look like
  // display state. See Pm4Translator's fetch shadow.
  {0x4800, "SHADER_FETCH_CONST"},
};

}  // namespace

const char* XenosGpuState::RegisterName(uint32_t reg) {
  for (const auto& e : kRegNames) {
    if (e.offset == reg) return e.name;
  }
  return nullptr;
}

void XenosGpuState::WriteRegister(uint32_t reg, uint32_t val) {
  regs_[reg] = val;
}

uint32_t XenosGpuState::ReadRegister(uint32_t reg) const {
  auto it = regs_.find(reg);
  return it != regs_.end() ? it->second : 0;
}

void XenosGpuState::ApplyType0Write(uint32_t reg_base, const uint32_t* data,
                                     uint32_t count) {
  for (uint32_t i = 0; i < count; ++i) {
    WriteRegister(reg_base + i, data[i]);
  }
}

void XenosGpuState::ApplyType3Packet(const pm4::Pm4Packet& pkt) {
  if (pkt.type != pm4::PacketType::Type3) return;

  switch (pkt.opcode) {
    case 0x04:  // REG_LOAD — informational, no state mutation needed
    case 0x05:  // REG_UPDATE — informational, no state mutation needed
    case 0x02:  // INDIRECT_BUFFER — chase would recurse; left as no-op
    case 0x03:  // WAIT_REG_MEM — sync only, no state mutation
      break;

    case 0x21:  // SET_CONTEXT_REG
      if (pkt.body.size() >= 2) {
        // TODO(register-indexing): captured PM4 so far contains ZERO
        // SET_CONTEXT_REG packets (renderer is disabled — mid-ASM hook #6
        // skips the entity block where shader/draw setup would emit them).
        // The current `base + (i-1)` write assumes body[0] is a byte-offset
        // register index that walks sequentially. Once the renderer is
        // re-enabled and live SET_CONTEXT_REG packets appear, match each
        // packet's body[0] against documented Xenos R500 register offsets
        // (e.g. RB_COLOR_INFO @ 0x2004, SQ_PROGRAM_CNTL @ 0x22C0) and
        // confirm or fix this formula. Logging raw base per packet for now
        // so the first live capture immediately yields the diagnostic.
        uint32_t base = pkt.body[0] & 0xFFFF;
        REXLOG_INFO("gpu_state: SET_CONTEXT_REG base=0x{:04X} count={}",
                    base, pkt.body.size() - 1);
        for (size_t i = 1; i < pkt.body.size(); ++i) {
          WriteRegister(base + (uint32_t)(i - 1), pkt.body[i]);
        }
      }
      break;

    case 0x20:  // SET_CONFIG_REG
      if (pkt.body.size() >= 2) {
        // Same caveat as SET_CONTEXT_REG above — formula unverified until
        // live packets appear. Log raw base for diagnostic.
        uint32_t base = pkt.body[0] & 0xFFFF;
        REXLOG_INFO("gpu_state: SET_CONFIG_REG base=0x{:04X} count={}",
                    base, pkt.body.size() - 1);
        for (size_t i = 1; i < pkt.body.size(); ++i) {
          WriteRegister(base + (uint32_t)(i - 1), pkt.body[i]);
        }
      }
      break;

    default:
      break;
  }
}

void XenosGpuState::ApplyPackets(const std::vector<pm4::Pm4Packet>& packets) {
  for (const auto& p : packets) {
    if (p.type == pm4::PacketType::Type0) {
      ApplyType0Write(p.reg_base, p.body.data(), p.reg_count);
    } else if (p.type == pm4::PacketType::Type3) {
      ApplyType3Packet(p);
    }
  }
}

void XenosGpuState::Snapshot() {
  prev_regs_ = regs_;
}

std::string XenosGpuState::DumpDiff() const {
  std::string out = "--- GPU State Diff ---\n";

  for (const auto& [reg, val] : regs_) {
    auto it = prev_regs_.find(reg);
    if (it == prev_regs_.end() || it->second != val) {
      const char* name = RegisterName(reg);
      char buf[128];
      uint32_t prev = (it != prev_regs_.end()) ? it->second : 0;
      snprintf(buf, sizeof(buf), "  [0x%04X] %-24s 0x%08X",
               reg, name ? name : "?", val);
      out += buf;
      if (it != prev_regs_.end() && it->second != val) {
        snprintf(buf, sizeof(buf), "  (was 0x%08X)", prev);
        out += buf;
      }
      out += "\n";
    }
  }

  if (out.size() <= 24) out += "  (no changes)\n";
  return out;
}

}  // namespace mx::gpu
