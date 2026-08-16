#include "gpu/xenos_gpu_state.h"

#include <cstdio>
#include <algorithm>
#include <array>
#include <mutex>

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

namespace alu {
namespace {

constexpr uint32_t kFileDwords = kAluConstants * 4;

std::mutex g_mu;
uint32_t g_file[kFileDwords] = {};
// One bit per dword. Without it "never written" and "written as 0.0" are the
// same value, and repairing a register nobody published would be inventing one.
uint32_t g_have[kFileDwords / 32] = {};
uint64_t g_written = 0;
uint64_t g_repaired = 0;
uint64_t g_zeroed = 0;

bool NonFinite(uint32_t bits) {
  // IEEE-754: exponent all ones is Inf (mantissa 0) or NaN (mantissa non-zero).
  return (bits & 0x7F800000u) == 0x7F800000u;
}

}  // namespace

void NoteType0Write(uint32_t reg_base, const uint32_t* data, uint32_t count) {
  if (reg_base >= kAluRegEnd || reg_base + count <= kAluRegBase) return;
  std::lock_guard<std::mutex> lk(g_mu);
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t reg = reg_base + i;
    if (reg < kAluRegBase || reg >= kAluRegEnd) continue;
    const uint32_t d = reg - kAluRegBase;
    if (d >= kFileDwords) continue;
    // A NaN never counts as PUBLISHED. Two reasons, and the second is the one
    // that bit:
    //
    //  - no guest ever means NaN as a constant, so recording one as
    //    authoritative can only ever suppress a better answer;
    //  - the frame-range walk covers [prev_after, write_before) of the ring,
    //    which can include bytes the guest has not written this frame. Garbage
    //    there decodes as a plausible Type0 write and would otherwise stamp a
    //    `have` bit over a register nothing really published.
    //
    // Measured 2026-08-16: with NaN allowed to publish, the file claimed 362
    // constants and 4.2M dwords — far more than the ALU-range Type0 writes
    // present in any pm4_dump_native_frame_*.txt — and c136..c139 stayed NaN
    // because OverlayNonFinite saw them as published and declined to substitute
    // the power-on 0.0.
    if ((data[i] & 0x7F800000u) == 0x7F800000u && (data[i] & 0x007FFFFFu) != 0)
      continue;
    g_file[d] = data[i];
    g_have[d >> 5] |= 1u << (d & 31);
    ++g_written;
  }
}

uint32_t OverlayNonFinite(uint32_t first_reg, uint32_t* bank,
                          uint32_t reg_count) {
  if (!bank || first_reg >= kAluConstants) return 0;
  const uint32_t regs = std::min(reg_count, kAluConstants - first_reg);
  uint32_t fixed = 0;
  std::lock_guard<std::mutex> lk(g_mu);
  for (uint32_t i = 0; i < regs * 4; ++i) {
    const uint32_t cur = bank[i];
    if (!NonFinite(cur)) continue;
    const uint32_t d = first_reg * 4 + i;
    const bool published = (g_have[d >> 5] & (1u << (d & 31))) != 0;
    if (published && !NonFinite(g_file[d])) {
      bank[i] = g_file[d];
      ++fixed;
      continue;
    }
    // Nothing published it. On hardware the constant is not garbage — the Xenos
    // register file POWERS ON ZEROED, and a title reading a constant it never
    // wrote gets 0.0. Xenia models exactly that: RegisterFile::RegisterFile()
    // is `memset(values, 0, sizeof(values))` with non-zero reset defaults for a
    // handful of context registers, none of them ALU constants
    // (register_file.cc:18). We rebuild the bank per draw out of a device
    // shadow whose backing memory the guest has not written yet, so we hand the
    // shader dirty heap instead.
    //
    // Measured: c136..c139 are NaN for a BOUNDED PREFIX of each shader's draws
    // and finite forever after — the NaN counts freeze while the finite counts
    // climb — so this is startup order, not a missing publisher. The legal,
    // loading and start screens all live in that prefix, which is why they have
    // no background: a NaN interpolator saturates the backdrop draw to white.
    //
    // NaN ONLY, never Inf, and that limit is the whole difference from
    // `hle_sanitize_constants`, which was retired for zeroing every non-finite
    // constant on every draw forever. A guest can legitimately compute +Inf and
    // mean it (see the divide-by-zero exposure path); no guest ever means NaN.
    if (!published && (cur & 0x007FFFFFu) != 0) {
      bank[i] = 0;
      ++g_zeroed;
    }
  }
  g_repaired += fixed;
  return fixed;
}

void Stats(uint64_t& written, uint64_t& repaired, uint32_t& constants_seen,
           uint64_t& zeroed) {
  std::lock_guard<std::mutex> lk(g_mu);
  written = g_written;
  repaired = g_repaired;
  zeroed = g_zeroed;
  uint32_t seen = 0;
  for (uint32_t c = 0; c < kAluConstants; ++c) {
    const uint32_t d = c * 4;
    if (g_have[d >> 5] & (1u << (d & 31))) ++seen;
  }
  constants_seen = seen;
}

}  // namespace alu

void XenosGpuState::ApplyType0Write(uint32_t reg_base, const uint32_t* data,
                                     uint32_t count) {
  alu::NoteType0Write(reg_base, data, count);
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
