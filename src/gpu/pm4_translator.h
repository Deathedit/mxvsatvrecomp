#pragma once

#include "gpu/pm4_parser.h"

#include <cstdint>
#include <cstring>
#include <vector>

namespace mx::pm4 {

// Mimics PrimitiveType from xenos.h (subset — only values seen in MX vs ATV).
enum class PrimitiveType : uint8_t {
  kPointList        = 0x01,
  kLineList         = 0x02,
  kLineStrip        = 0x03,
  kTriangleList    = 0x04,
  kTriangleStrip   = 0x05,
  kTriangleFan     = 0x06,
  kRectangleList   = 0x08,
  kUnknown         = 0xFF,
};

struct DrawCall {
  std::vector<uint8_t> vertices;      // optional; filled only when a vertex fetch const is known
  std::vector<uint8_t> indices;       // packed index buffer (2 or 4 bytes per index)
  uint32_t vertex_count = 0;
  uint32_t index_count = 0;
  uint32_t vertex_stride = 0;
  uint32_t prim_type = 0;            // xenos::PrimitiveType (raw 6-bit value)
  bool index_16bit = true;
  bool binned = false;                // true for DRAW_INDX_*_BIN variants
  float mvp[16] = {};
  bool valid = false;                 // set once index buffer is populated
};

class Pm4Translator {
 public:
  Pm4Translator() = default;

  void TranslatePackets(const std::vector<Pm4Packet>& packets,
                        uint8_t* guest_base, uint32_t gpu_phys_base);

  const std::vector<DrawCall>& DrawCalls() const { return m_drawCalls; }
  void Clear() {
    m_drawCalls.clear();
    m_vtxBufAddr = 0;
    m_vtxStride = 0;
    m_indexType16 = true;
    std::memset(m_mvp, 0, sizeof(m_mvp));
    m_binMaskLo = 0xFFFFFFFF;
    m_binMaskHi = 0x00000000;
    m_binSelectLo = 0xFFFFFFFF;
    m_binSelectHi = 0x00000000;
  }

 private:
  // Real Xenos draw handlers (see Xenia packet_disassembler.cc).
  // DRAW_INDX_2_BIN and DRAW_INDX_2 share the same dword0 layout:
  //   bits[31:16]=index_count, [11]=index_32bit, [10:6]=src_sel,
  //   [5:0]=prim_type. Inline indices start at body[1].
  void HandleDrawIndx2(const Pm4Packet& pkt, bool binned);
  // DRAW_INDX uses body[0]=viz_query_info, body[1]=dword0 (draw header),
  // then body[2]=guest_base + body[3]=index_size|endianness for src_sel==0.
  void HandleDrawIndx(const Pm4Packet& pkt, uint8_t* guest_base, bool binned);

  // SET_CONSTANT (0x2D) — sub-dispatch via type field in body[0]:
  //   type=(body[0]>>16)&0xFF, index = body[0] & 0x7FF
  //   type=0 → ALU float constants  (0x4000 + index)
  //   type=1 → FETCH vertex fetch consts (0x4800 + index) — sets vtx buffer
  //   type=2 → BOOL constants           (0x4900 + index)
  //   type=3 → LOOP constants           (0x4908 + index)
  //   type=4 → REGISTERS                (0x2000 + index) — context regs
  void HandleSetConstant(const Pm4Packet& pkt);

  // SET_SHADER_CONSTANTS (0x56) — incremental shader constant update.
  // body[0] = base index (16-bit), body[1..] = float4 constants.
  void HandleSetShaderConstants(const Pm4Packet& pkt);

  // Empty stubs for state-tracking opcodes we want to log/skip:
  void HandleBinMaskLo(const Pm4Packet& pkt);
  void HandleBinMaskHi(const Pm4Packet& pkt);
  void HandleBinSelectLo(const Pm4Packet& pkt);
  void HandleBinSelectHi(const Pm4Packet& pkt);

  uint32_t m_vtxBufAddr = 0;
  uint32_t m_vtxStride = 0;
  bool m_indexType16 = true;
  float m_mvp[16] = {};
  // Bin mask/select registers — written by SET_BIN_MASK_LO/HI + SET_BIN_SELECT_LO/HI.
  uint32_t m_binMaskLo = 0xFFFFFFFF;
  uint32_t m_binMaskHi = 0x00000000;
  uint32_t m_binSelectLo = 0xFFFFFFFF;
  uint32_t m_binSelectHi = 0x00000000;
  std::vector<DrawCall> m_drawCalls;
};

}  // namespace mx::pm4