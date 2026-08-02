#include "gpu/xenos_gpu_state.h"

#include <cstdio>
#include <algorithm>
#include <array>

#include <rex/logging.h>

namespace mx::gpu {

namespace {

// Register indices are Xenos *dword* indices — the same units a PM4 Type0
// packet's reg_base is in. Until 2026-08-02 this table was hand-built and every
// entry from 0x2000 up was a byte offset (RB_COLOR_INFO at 0x2004 rather than
// 0x2001), so each one named a register four slots away from the one it
// labelled. Those entries were deleted rather than rescaled, leaving ten names
// confirmed one at a time against observed values.
//
// All ten are now superseded by the SDK's own table, and all ten matched it
// exactly — 0x2000 RB_SURFACE_INFO, 0x2080..0x2082 the window offset/scissor
// pair, 0x210F..0x2114 the viewport block. That agreement is worth recording:
// the viewport transform the translator inverts was derived from those names.
//
// The SDK table is Xenia's, dword-indexed, 3434 live entries spanning
// 0x0000..0x5002, and it is NOT sorted — there are two out-of-order pairs
// (0x1DD before 0x1DC, and a section restart). A binary search would have
// silently missed entries, so the lookup is a direct-indexed table instead:
// one pointer per register index, built once, O(1).
constexpr uint32_t kRegIndexCount = 0x5003;  // matches the SDK's kRegisterCount

const char* const* RegNameTable() {
  static const std::array<const char*, kRegIndexCount> table = [] {
    std::array<const char*, kRegIndexCount> t{};  // value-initialized to nullptr
#define XE_GPU_REGISTER(index, type, name) t[index] = #name;
#include <rex/graphics/register_table.inc>
#undef XE_GPU_REGISTER
    return t;
  }();
  return table.data();
}

}  // namespace

const char* XenosGpuState::RegisterName(uint32_t reg) {
  if (reg >= kRegIndexCount) return nullptr;
  return RegNameTable()[reg];
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

    // BUG(wrong-opcode): these two cases are misnamed, and the opcodes are not
    // the ones intended. Per the SDK's PM4_* list, 0x21 is REG_RMW — a
    // read-modify-write of a single register, whose body is (mask, or-value),
    // NOT a base plus a run of values — and 0x20 is not a Xenos opcode at all.
    // Xenos has no SET_CONTEXT_REG/SET_CONFIG_REG; register writes arrive as
    // Type0 packets, which ApplyType0Write already handles correctly.
    //
    // Left as-is deliberately. Neither opcode appears in any captured dump
    // (frame 600's Type3 histogram has no 0x21 and no 0x20), so this is dead
    // in practice, and the round this comment was written in is a
    // measurement round that must not move any number. Deleting these cases,
    // or implementing REG_RMW properly, belongs with the PM4 parser resync
    // work where it can be measured on its own.
    case 0x21:  // labelled SET_CONTEXT_REG; actually REG_RMW — see above
      if (pkt.body.size() >= 2) {
        uint32_t base = pkt.body[0] & 0xFFFF;
        REXLOG_INFO("gpu_state: opcode 0x21 base=0x{:04X} count={}",
                    base, pkt.body.size() - 1);
        for (size_t i = 1; i < pkt.body.size(); ++i) {
          WriteRegister(base + (uint32_t)(i - 1), pkt.body[i]);
        }
      }
      break;

    case 0x20:  // not a Xenos opcode — see above
      if (pkt.body.size() >= 2) {
        uint32_t base = pkt.body[0] & 0xFFFF;
        REXLOG_INFO("gpu_state: opcode 0x20 base=0x{:04X} count={}",
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
