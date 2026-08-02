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

// Register indices here are Xenos *dword* indices — the same units a PM4 Type0
// packet's reg_base is in. Until 2026-08-02 every entry from 0x2000 up was a
// byte offset (RB_COLOR_INFO at 0x2004 rather than 0x2001), so each one named a
// register four slots away from the one it labelled: 0x2080 printed as
// "RB_DISP_OUTPUT" when the observed value — 0, 0, 0x02D00500 = 1280x720 — makes
// it PA_SC_WINDOW_OFFSET followed by the window scissor pair. Those entries are
// deleted rather than rescaled: a confidently wrong name is worse than "???",
// and this table is the single source of truth for Pm4Parser's dumps.
//
// Only names confirmed against a value observed in a captured dump are listed.
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
  // Surface state. RB_SURFACE_INFO's low 14 bits are the pitch: observed
  // 0x14000500 = 1280 at boot and 0x0C000320 = 800 post-load, matching the
  // backbuffer and the 768x1024 offscreen pass respectively.
  {0x2000, "RB_SURFACE_INFO"},
  // Scissor block, observed as one cnt=3 write of [0, 0, 0x02D00500] —
  // offset (0,0), TL (0,0), BR 1280x720.
  {0x2080, "PA_SC_WINDOW_OFFSET"},
  {0x2081, "PA_SC_WINDOW_SCISSOR_TL"},
  {0x2082, "PA_SC_WINDOW_SCISSOR_BR"},
  // Viewport block, the tail of the cnt=21 write to 0x2100. Observed at boot
  // as 640, 640, -360, 360, 1.0, 0.0 (a 1280x720 viewport) and post-load as
  // 384, 384, -512, 512 (768x1024). This is the transform Pm4Translator
  // inverts to map the guest's window-space vertices back to NDC.
  {0x210F, "PA_CL_VPORT_XSCALE"},
  {0x2110, "PA_CL_VPORT_XOFFSET"},
  {0x2111, "PA_CL_VPORT_YSCALE"},
  {0x2112, "PA_CL_VPORT_YOFFSET"},
  {0x2113, "PA_CL_VPORT_ZSCALE"},
  {0x2114, "PA_CL_VPORT_ZOFFSET"},
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
        // confirm or fix this formula against kRegNames above — noting those
        // are dword indices, so a byte offset compared against them is off by
        // a factor of four. Logging raw base per packet for now so the first
        // live capture immediately yields the diagnostic.
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
