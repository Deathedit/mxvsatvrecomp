#pragma once

#include "gpu/pm4_parser.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace mx::gpu {

class XenosGpuState {
 public:
  XenosGpuState() = default;

  void ApplyType0Write(uint32_t reg_base, const uint32_t* data, uint32_t count);
  void ApplyType3Packet(const pm4::Pm4Packet& pkt);
  void ApplyPackets(const std::vector<pm4::Pm4Packet>& packets);

  uint32_t ReadRegister(uint32_t reg) const;
  void WriteRegister(uint32_t reg, uint32_t val);

  void Snapshot();
  std::string DumpDiff() const;

  const auto& Registers() const { return regs_; }

  // Shared register-name lookup. Single source of truth across the codebase
  // (also called by Pm4Parser::DumpPackets so the dump doesn't show ??? for
  // registers that exist but aren't duplicated in the parser's local table).
  static const char* RegisterName(uint32_t reg);

 private:
  std::unordered_map<uint32_t, uint32_t> regs_;
  std::unordered_map<uint32_t, uint32_t> prev_regs_;
};

}  // namespace mx::gpu
