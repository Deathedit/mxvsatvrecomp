// Quick standalone test for the Pm4Translator's DRAW_INDX_2_BIN handling.
// Build: clang++ -std=c++20 -I src -I C:\rexglue-sdk\include -o translator_test.exe translator_test.cpp src/pm4_parser.cpp src/pm4_translator.cpp src/xenos_gpu_state.cpp
// (Skips host-only Windows APIs by stubbing them out below.)
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>

// Stubs to satisfy pm4_parser.cpp + xenos_gpu_state.cpp link
// (the only usage in the translator is indirect — we replace XenosGpuState
// with a no-op).

// Minimal REXLOG stubs
#define REXLOG_INFO(fmt, ...) printf("[INFO] " fmt "\n", ##__VA_ARGS__)
#define REXLOG_WARN(fmt, ...) printf("[WARN] " fmt "\n", ##__VA_ARGS__)

// Stub out the Windows-specific helpers from pm4_parser.cpp
extern "C" {
  void* GetModuleFileNameA(...) { return nullptr; }
  // fopen_s is already declared by UCRT; use it as-is
}

// Include the full parser / translator
#include "src/pm4_parser.h"
#include "src/pm4_translator.h"

int main() {
  using namespace mx::pm4;

  // -- Synthetic DRAW_INDX_2_BIN packet per Xenia's format --
  // dword0 (header): tt cccc cccc cccc cccc ?ooo oooo ??????p
  //   tt=3, count=4-1=3 (4 body dwords follow), opcode=0x35 (DRAW_INDX_2_BIN), p=0
  // Body[0] (draw header): bits[31:16]=index_count=6, [11]=index_32bit=0,
  //                       [10:6]=src_sel=2 (AutoIndex/inline), [5:0]=prim_type=4 (TriangleList)
  // Body[1..]: 3 16-bit indices packed 2-per-dword: 0,1,2, 3,4,5, pad = {0x00010000, 0x00030002, 0x00050004}
  uint32_t pkts_be[5];
  // Header (BE-stored in our parser's input; parser byteswaps to LE)
  // LE form: (3<<30) | ((4-1)<<16) | (0x35<<8) | 0  = 0xC0003503
  uint32_t hdr_le = (3u << 30) | (((4 - 1) & 0x3FFF) << 16) | ((0x35 & 0x7F) << 8) | 0;
  pkts_be[0] = _byteswap_ulong(hdr_le);
  // Draw header: index_count=6, src_sel=2, prim_type=4, index_32bit=0
  // LE form: (6<<16) | (2<<6) | 4 = 0x00060084
  uint32_t dh_le = (6u << 16) | (2u << 6) | 4u;
  pkts_be[1] = _byteswap_ulong(dh_le);
  // 16-bit indices: (0, 1), (2, 3), (4, 5) packed 2-per-dword, low-then-high
  pkts_be[2] = _byteswap_ulong(0x00010000u);  // lo=0, hi=1
  pkts_be[3] = _byteswap_ulong(0x00030002u);  // lo=2, hi=3
  pkts_be[4] = _byteswap_ulong(0x00050004u);  // lo=4, hi=5

  Pm4Parser parser;
  parser.ParseRange(pkts_be, 5);

  printf("Decoded %zu packets\n", parser.Packets().size());
  for (const auto& p : parser.Packets()) {
    printf("  type=%d op=0x%02X count=%u body_words=%zu\n",
           int(p.type), p.opcode, p.body_word_count, p.body.size());
  }

  Pm4Translator translator;
  uint8_t guest_mem[64] = {};  // unused by inline indices
  translator.TranslatePackets(parser.Packets(), guest_mem, 0);
  printf(" translator produced %zu draw calls\n", translator.DrawCalls().size());
  for (const auto& dc : translator.DrawCalls()) {
    printf("   idx_count=%u prim=%u 16bit=%d binned=%d valid=%d indices=",
           dc.index_count, dc.prim_type, dc.index_16bit, dc.binned, dc.valid);
    for (size_t i = 0; i < 6 && i < dc.indices.size() / 2; ++i) {
      uint16_t idx;
      memcpy(&idx, dc.indices.data() + i * 2, 2);
      printf(" %u", idx);
    }
    printf("\n");
  }
  return 0;
}