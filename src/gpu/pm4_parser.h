#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mx::pm4 {

enum class PacketType : uint8_t { Type0, Type2, Type3, Unknown };

// Xenos Type3 opcodes — see C:\rexglue-sdk\include\rex\graphics\xenos.h::Type3Opcode
// These are 7-bit values per the Xenos PM4 ring format.
enum class Pm4Opcode : uint8_t {
  NOP                   = 0x10,  // skip N 32-bit words
  INDIRECT_BUFFER       = 0x3F,  // indirect buffer dispatch
  INDIRECT_BUFFER_PFD   = 0x37,  // indirect buffer with pipelined init
  WAIT_FOR_IDLE         = 0x26,
  WAIT_REG_MEM          = 0x3C,
  REG_RMW               = 0x21,
  REG_TO_MEM            = 0x3E,
  MEM_WRITE             = 0x3D,
  MEM_WRITE_CNTR        = 0x4F,
  COND_EXEC             = 0x44,
  COND_WRITE            = 0x45,
  EVENT_WRITE           = 0x46,
  EVENT_WRITE_SHD       = 0x58,
  EVENT_WRITE_CFL       = 0x59,
  EVENT_WRITE_EXT       = 0x5A,
  EVENT_WRITE_ZPD       = 0x5B,
  DRAW_INDX             = 0x22,  // initiate fetch of index buffer and draw
  DRAW_INDX_2           = 0x36,  // draw using supplied indices in packet
  DRAW_INDX_BIN         = 0x34,
  DRAW_INDX_2_BIN       = 0x35,
  VIZ_QUERY             = 0x23,
  SET_STATE             = 0x25,  // fetch state sub-blocks + shader code DMAs
  SET_CONSTANT          = 0x2D,
  SET_SHADER_CONSTANTS  = 0x56,
  LOAD_ALU_CONSTANT     = 0x2F,
  IM_LOAD               = 0x27,  // load sequencer instruction memory
  ME_INIT               = 0x48,
  // Back-compat aliases (legacy names preserved for callers)
  SET_CONFIG_REG        = 0x60,  // legacy alias, not Xenos-native
  SET_CONTEXT_REG       = 0x61,
  SET_ALU_CONST         = 0x62,
  SET_BOOL_CONST        = 0x63,
  SET_LOOP_CONST        = 0x64,
  SET_RESOURCE          = 0x65,
  SET_SAMPLER           = 0x66,
  SET_CTL_CONST         = 0x67,
  SET_VTX_RESOURCE      = 0x68,
  SET_VTX_SAMPLER       = 0x69,
  INDEX_TYPE            = 0x6A,
  STRMOUT_BUFFER_UPDATE = 0x6C,
  NUM_INSTANCES         = 0x6F,
};

struct Pm4Packet {
  PacketType type = PacketType::Unknown;
  uint32_t raw_header = 0;
  uint32_t guest_addr = 0;

  uint32_t reg_base = 0;
  uint32_t reg_count = 0;

  uint8_t opcode = 0;
  uint8_t sub_op = 0;  // sub-op byte [15:8] retained for diagnostic output
  uint16_t body_word_count = 0;
  bool predicate = false;

  std::vector<uint32_t> body;
};

class Pm4Parser {
 public:
  Pm4Parser() = default;

  void ParseRange(const uint32_t* dwords, uint32_t count,
                  uint32_t base_guest_addr = 0);

  const std::vector<Pm4Packet>& Packets() const { return packets_; }
  void Clear() { packets_.clear(); }

  static const char* OpcodeName(uint32_t opcode);
  static const char* RegisterName(uint32_t reg);

  static void DumpPackets(const std::vector<Pm4Packet>& packets,
                           const char* filename = "pm4_dump.txt");

 private:
  Pm4Packet DecodePacket(const uint32_t* data, uint32_t max_dwords,
                         uint32_t& consumed, uint32_t guest_addr);

  std::vector<Pm4Packet> packets_;
};

}  // namespace mx::pm4
