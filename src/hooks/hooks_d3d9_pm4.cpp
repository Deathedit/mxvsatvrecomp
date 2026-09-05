// PM4 command-buffer scanning -- what a recorded buffer contains.
//
// Split verbatim out of hooks_d3d9_entry.cpp, which is a file of guest entry
// points and had ~450 lines of packet walking in the middle of it. Nothing here
// is a hook: it is the type-0/type-3 decode behind two of them.
//
// Two subjects, both about the same buffers:
//
//   the CONSTANTS a recorded buffer writes, paired to its draws, so a replayed
//   draw gets the constants that were live when it was recorded rather than one
//   flat list applied to all of them; and
//
//   the command-buffer REPLAY census -- sub_825605D8 splices a recorded buffer
//   into the ring as INDIRECT_BUFFER packets, so those draws never touch a D3D9
//   draw entry point and were never ours to drop.
//
// Publishes exactly one symbol that was not already published:
// NoteCommandBufferExec. Everything else is either in the anonymous namespace
// below or was already declared in hooks_d3d9_internal.h.

#include "hooks/hook_common.h"

#include "hooks/hooks_d3d9_internal.h"
#include "hooks/hooks_d3d9_pm4.h"

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

using namespace mx::hooks::d3d9;

// GuestRangeReadable is declared in hooks_d3d9.h and defined at the bottom of
// hooks_d3d9.cpp, both at global scope. Forward-declared rather than including
// that header: it needs <string>, and this file's project includes come first.
bool GuestRangeReadable(uint8_t* base, uint32_t addr, uint32_t bytes);
uint32_t ResolveGuestRange(uint8_t* base, uint32_t addr, uint32_t bytes);

//===========================================================================
// THE CONSTANTS A RECORDED BUFFER WRITES, PAIRED TO ITS DRAWS.
//
// A recorded buffer carries only the writes the guest made while recording, as
// PM4 type-0 packets whose payload is the data; on the console the replay runs
// against whatever the GPU's constant state already is, so the replay's base is
// the LIVE bank and these are layered on top.
//
// Register index 4 * (first_reg + 4096) puts constant 64 at 0x4100, and 0x4000
// is the vertex ALU constant base, so a type-0 packet in [0x4000, 0x4400) is a
// vertex constant write.
//
// ORDERED, one entry per DRAW_INDX: a constant written before draw N applies to
// draw N and every draw after it. One flat list applied to every draw gave draw
// 1 draw 27's transform and smeared the scene into shards.
//===========================================================================
namespace {

constexpr uint32_t kCbTypeOff = 0x00;    // low nibble 0xA = early-out
constexpr uint32_t kCbBlocksOff = 0x74;  // head of the block list

// The four draw opcodes, from the SDK. Declared here because the census
// block that also names them sits further down this file.
constexpr uint32_t kDrawIndx = 0x22;
constexpr uint32_t kDrawIndx2 = 0x36;
constexpr uint32_t kDrawIndxBin = 0x34;
constexpr uint32_t kDrawIndx2Bin = 0x35;

constexpr uint32_t kAluVertexConstBase = 0x4000;   // constant 0
constexpr uint32_t kAluPixelConstBase = 0x4400;    // constant 256
constexpr uint32_t kAluConstEnd = 0x4800;          // past constant 511
constexpr uint32_t kFetchConstBase = 0x4800;       // texture fetch const 0
constexpr uint32_t kFetchConstEnd = 0x4A00;        // past fetch const 31
constexpr uint32_t kSqProgramCntl = 0x2180;
constexpr uint32_t kSqContextMisc = 0x2181;

using ConstBlock = mx::hooks::d3d9::CmdBufConstOverlay;

// Running state while walking one buffer: constant index -> the latest value written.
// Snapshotted at each draw.
void CollectConstsFromIb(uint32_t phys, uint32_t size_dwords, uint8_t* base,
                         std::map<uint32_t, std::vector<uint32_t>>& live,
                         std::map<uint32_t, std::vector<uint32_t>>& live_fetch,
                         std::map<uint32_t, uint32_t>& live_reg,
                         std::vector<std::vector<ConstBlock>>& out) {
  constexpr uint32_t kMaxScanDwords = 256u * 1024u;
  const uint32_t dwords =
      size_dwords > kMaxScanDwords ? kMaxScanDwords : size_dwords;
  if (!dwords) return;
  const uint32_t at = ResolveGuestRange(base, phys, dwords * 4);
  if (!at) return;
  for (uint32_t i = 0; i < dwords;) {
    const uint32_t hdr = REX_LOAD_U32(at + i * 4);
    const uint32_t type = hdr >> 30;
    uint32_t advance;
    if (type == 0) {
      const uint32_t count = ((hdr >> 16) & 0x3FFF) + 1;
      advance = 1 + count;
      const uint32_t index = hdr & 0x7FFF;
      // BOTH BANKS. 0x4000 is vertex constant 0 and 0x4400 is pixel constant 256
      // -- the guest's own D3D9 flush passes those two bases. Collecting only
      // the vertex range left every recorded pixel constant on the floor.
      //
      // ALSO COUNTED, not acted on: texture fetch constants (Xenos 0x4800, six
      // dwords each) and SQ_PROGRAM_CNTL / SQ_CONTEXT_MISC (0x2180/0x2181, bit
      // 18 of the first enables PARAM_GEN and the second says which interpolator
      // it lands in). The constants proved to be live state and the textures
      // buffer state, so which one this is has to be measured.
      //
      // A type-0 packet writes `count` CONSECUTIVE registers from `index`, so
      // one packet starting at 0x2180 can carry both -- testing only the start
      // index would miss every such write.
      if (index <= kSqContextMisc && index + count > kSqProgramCntl &&
          advance <= dwords - i) {
        for (uint32_t reg = kSqProgramCntl; reg <= kSqContextMisc; ++reg) {
          if (reg < index || reg >= index + count) continue;
          const uint32_t v = REX_LOAD_U32(at + (i + 1 + (reg - index)) * 4);
          live_reg[reg] = v;
        }
      }
      // TEXTURE BINDINGS. Six dwords each from 0x4800; a write that is not
      // six-aligned is a partial update this does not model, and is counted
      // rather than guessed at.
      if (index >= kFetchConstBase && index < kFetchConstEnd) {
        const uint32_t off = index - kFetchConstBase;
        if ((off % 6) == 0 && count >= 6 && advance <= dwords - i) {
          std::vector<uint32_t> f;
          f.reserve(6);
          for (uint32_t k = 0; k < 6; ++k)
            f.push_back(REX_LOAD_U32(at + (i + 1 + k) * 4));
          live_fetch[off / 6] = std::move(f);
        } else {
        }
      }
      if (index >= kAluVertexConstBase && index < kAluConstEnd &&
          ((index - kAluVertexConstBase) & 3) == 0 && count >= 4 &&
          advance <= dwords - i) {
        std::vector<uint32_t> data;
        data.reserve(count);
        for (uint32_t k = 0; k < count; ++k)
          data.push_back(REX_LOAD_U32(at + (i + 1 + k) * 4));
        live[(index - kAluVertexConstBase) / 4] = std::move(data);
      }
    } else if (type == 1) {
      advance = 3;
    } else if (type == 2) {
      advance = 1;
    } else {
      advance = 1 + (((hdr >> 16) & 0x3FFF) + 1);
      const uint32_t opcode = (hdr >> 8) & 0x7F;
      if (opcode == kDrawIndx || opcode == kDrawIndx2 ||
          opcode == kDrawIndxBin || opcode == kDrawIndx2Bin) {
        // Snapshot everything written so far: this is the state this draw
        // sees, and later writes must not reach back into it.
        std::vector<ConstBlock> snap;
        snap.reserve(live.size() + live_fetch.size());
        for (const auto& [reg, data] : live) {
          ConstBlock b;
          b.first_const = reg;
          b.dwords = data;
          snap.push_back(std::move(b));
        }
        for (const auto& [sampler, data] : live_fetch) {
          ConstBlock b;
          b.first_const = sampler;
          b.is_fetch = true;
          b.dwords = data;
          snap.push_back(std::move(b));
        }
        for (const auto& [reg, value] : live_reg) {
          ConstBlock b;
          b.first_const = reg;
          b.is_reg = true;
          b.dwords.push_back(value);
          snap.push_back(std::move(b));
        }
        out.push_back(std::move(snap));
      }
    }
    if (advance > dwords - i) break;   // desync: stop, do not guess
    i += advance;
  }
}

}  // namespace

void mx::hooks::d3d9::CollectCmdBufConstants(
    uint32_t cmdbuf, uint8_t* base,
    std::vector<std::vector<ConstBlock>>& out) {
  if (!cmdbuf || !GuestRangeReadable(base, cmdbuf, 0x78)) return;
  if ((REX_LOAD_U32(cmdbuf + kCbTypeOff) & 0xF) == 0xA) return;
  std::map<uint32_t, std::vector<uint32_t>> live;
  std::map<uint32_t, std::vector<uint32_t>> live_fetch;
  std::map<uint32_t, uint32_t> live_reg;
  uint32_t node = REX_LOAD_U32(cmdbuf + kCbBlocksOff);
  for (uint32_t blocks = 0; node && blocks < 64; ++blocks) {
    const uint32_t at = ResolveGuestRange(base, node, 8);
    if (!at) break;
    const uint32_t n = REX_LOAD_U32(at + 4);
    if (n > 4096) break;
    for (uint32_t i = 1; i <= n; ++i) {
      const uint32_t pair = at + 8 * i;
      if (!GuestRangeReadable(base, pair, 8)) break;
      const uint32_t sz = REX_LOAD_U32(pair) & 0xFFFFFF;
      const uint32_t ib = REX_LOAD_U32(pair + 4);
      if (ib)
        CollectConstsFromIb(ib, sz, base, live, live_fetch, live_reg, out);
    }
    node = REX_LOAD_U32(at);
  }
}

//===========================================================================
// COMMAND-BUFFER REPLAY -- the submission path with no D3D9 draw call in it.
//
// sub_825605D8 takes a recorded command buffer and splices it into the PM4 ring.
// From its own code the packets it writes are
//
//     v26 = v15 | 0xC0013F00      type 3, count 2, OPCODE 0x3F
//
// and 0x3F is INDIRECT_BUFFER: address and size. So the geometry is not in this
// call at all, it is in buffers this call POINTS AT.
//
// sub_823F82D0, the SpeedTree render, records one command buffer and executes it
// once per tree instance. Every investigation into the missing 3D trees asked
// whether we DROP those draws and correctly answered no -- they were never in
// our pipeline to drop. Third time this shape has been root cause, after
// BeginVertices/EndVertices for the UI and sub_82556110 for GFx shapes.
//
// MEASURING ONLY, deliberately: the open question is what those indirect buffers
// CONTAIN.
//===========================================================================
namespace {

// The fields sub_825605D8 itself reads off the command buffer.
constexpr uint32_t kCmdBufType   = 0x00;   // low nibble 0xA is its early-out
constexpr uint32_t kCmdBufFlags  = 0x6C;   // +108
constexpr uint32_t kCmdBufBlocks = 0x74;   // +116, head of the block list

uint64_t g_cmdExec = 0;          // executions
uint64_t g_cmdExecEarlyOut = 0;  // the (type & 0xF) == 0xA path, submits nothing
uint64_t g_cmdUnreadable = 0;    // buffer pointer not mapped
uint64_t g_cmdEntries = 0;       // indirect-buffer entries walked

// Distinct buffers, so "one buffer replayed 6000 times" and "6000 buffers" are
// not the same number. Bounded, with the overflow counted.
constexpr uint32_t kMaxCmdBufs = 32;
uint32_t g_cmdBufPtr[kMaxCmdBufs] = {};
uint64_t g_cmdBufRuns[kMaxCmdBufs] = {};
uint32_t g_cmdBufBlocks[kMaxCmdBufs] = {};
uint32_t g_cmdBufDraws[kMaxCmdBufs] = {};   // draw packets inside each
uint32_t g_cmdBufDistinct = 0;
uint64_t g_cmdBufOverflow = 0;
std::mutex g_cmdMu;

// WHAT IS ACTUALLY IN THE INDIRECT BUFFERS.
//
// The executor's own loop says how to read a block. `v25` is pre-incremented, so
// the pairs start at node[2], and it writes
//
//     *v32     = v29[1]           -> the ADDRESS, written first
//     v32[1]   = *v29 & 0xFFFFFF  -> the SIZE, masked to 24 bits
//
// under a 0xC0013F00 header. So a block entry is the pair (size, address) at
// node[2 + 2i]. The address is PHYSICAL -- the first sighting printed a list
// head of 0xDDCA8000 -- so ResolveGuestRange tries the segment bases and returns
// the one that is mapped, or 0.
//
// FOUR draw opcodes, not two, verified against the SDK (xenos.h:1584):
//
//   PM4_DRAW_INDX       0x22  fetch index buffer and draw
//   PM4_DRAW_INDX_2     0x36  draw using indices supplied in the packet
//   PM4_DRAW_INDX_BIN   0x34  fetch index buffer and binIDs and draw
//   PM4_DRAW_INDX_2_BIN 0x35  fetch bin IDs and draw with supplied indices
//
// Checking only the first two would count a BINNED draw as "other", i.e. report
// "no draws" for a buffer full of them, and this title has BeginTiling/EndTiling
// in its symbol list. Everything else is counted as "other" rather than named.
constexpr uint32_t kPm4DrawIndx = 0x22;
constexpr uint32_t kPm4DrawIndx2 = 0x36;
constexpr uint32_t kPm4DrawIndxBin = 0x34;
constexpr uint32_t kPm4DrawIndx2Bin = 0x35;

uint64_t g_ibDrawIndx = 0;      // DRAW_INDX packets found
uint64_t g_ibDrawIndx2 = 0;     // DRAW_INDX_2 packets found
uint64_t g_ibOther = 0;         // every other type-3 packet
uint64_t g_ibScanned = 0;       // buffers successfully scanned
uint64_t g_ibUnresolved = 0;    // entries whose address never resolved
uint64_t g_ibDesync = 0;        // walks that lost packet alignment

// Scan one indirect buffer for draw packets. Bounded: a malformed stream must
// not walk off, and this runs on the draw path.
void ScanIndirectBuffer(uint32_t phys, uint32_t size_dwords, uint8_t* base,
                        std::string& out) {
  // THE CAP MUST NOT HIDE THE ANSWER. One run reported 8 of 22 buffers at
  // exactly 4096 dwords -- that was this clamp, not their size, so a draw past
  // 16 KB would have read as "no draws". Raised to 256K dwords because this runs
  // ONCE per distinct buffer, and the true size is printed beside the scanned
  // one so any future truncation is visible.
  constexpr uint32_t kMaxScanDwords = 256u * 1024u;
  const uint32_t dwords =
      size_dwords > kMaxScanDwords ? kMaxScanDwords : size_dwords;
  if (!dwords) return;
  const uint32_t at = ResolveGuestRange(base, phys, dwords * 4);
  if (!at) {
    ++g_ibUnresolved;
    out += fmt::format(" [0x{:08X} x{} dwords UNRESOLVED]", phys, dwords);
    return;
  }
  ++g_ibScanned;
  // WALK ALL FOUR PACKET TYPES. Understanding only type-3 and sliding one dword
  // at a time past everything else walks straight into a type-0 register write's
  // PAYLOAD and reads data as a header; a payload dword whose top two bits are
  // 11 then consumed `count + 1` more dwords, which is how a 6649-dword stream
  // reported TWO packets.
  //
  // Sizes are the SDK's, from MakePacketType0/1/2/3:
  //   type 0   bits 29:16 + 1 payload dwords   (register write)
  //   type 1   exactly 2 payload dwords        (two register writes)
  //   type 2   no payload                      (NOP / filler)
  //   type 3   bits 29:16 + 1 payload dwords   (packet, opcode in 15:8)
  uint32_t draws = 0, draws2 = 0, other = 0;
  uint32_t t0 = 0, t1 = 0, t2 = 0, t3 = 0;
  bool desync = false;
  for (uint32_t i = 0; i < dwords;) {
    const uint32_t hdr = REX_LOAD_U32(at + i * 4);
    const uint32_t type = hdr >> 30;
    uint32_t advance;
    if (type == 0) {
      ++t0;
      advance = 1 + (((hdr >> 16) & 0x3FFF) + 1);
    } else if (type == 1) {
      ++t1;
      advance = 3;
    } else if (type == 2) {
      ++t2;
      advance = 1;
    } else {
      ++t3;
      advance = 1 + (((hdr >> 16) & 0x3FFF) + 1);
      const uint32_t opcode = (hdr >> 8) & 0x7F;
      if (opcode == kPm4DrawIndx || opcode == kPm4DrawIndxBin) {
        ++draws;
      } else if (opcode == kPm4DrawIndx2 || opcode == kPm4DrawIndx2Bin) {
        ++draws2;
      } else {
        ++other;
        // NAME the opcodes rather than only counting them. "0 draws, N other" is
        // indistinguishable from "this is not a packet stream and the scanner is
        // walking data", and a negative result has to be separable from a broken
        // instrument.
      }
    }
    // A packet whose payload runs past the end means the walk lost alignment
    // somewhere behind it. Say so out loud: a desynced walk's zero is not
    // evidence about the buffer, it is evidence about the walk.
    if (advance > dwords - i) {
      desync = true;
      ++g_ibDesync;
      break;
    }
    i += advance;
  }
  g_ibDrawIndx += draws;
  g_ibDrawIndx2 += draws2;
  g_ibOther += other;
  out += fmt::format(" [0x{:08X}->0x{:08X} {} of {} dwords{}{}: t0 {} t1 {} t2 "
                     "{} t3 {} -- DRAW_INDX {} DRAW_INDX_2 {} other {}]",
                     phys, at, dwords, size_dwords,
                     dwords < size_dwords ? " TRUNCATED" : "",
                     desync ? " DESYNC" : "", t0, t1, t2, t3, draws, draws2,
                     other);
}

// Walk one command buffer's block list and scan every indirect buffer it names.
void ScanCmdBufBlocks(uint32_t head, uint8_t* base, std::string& out) {
  uint32_t node = head;
  for (uint32_t blocks = 0; node && blocks < 64; ++blocks) {
    const uint32_t at = ResolveGuestRange(base, node, 8);
    if (!at) {
      out += fmt::format(" [block 0x{:08X} UNRESOLVED]", node);
      break;
    }
    const uint32_t n = REX_LOAD_U32(at + 4);
    if (n > 4096) break;
    for (uint32_t i = 1; i <= n; ++i) {
      const uint32_t pair = at + 8 * i;
      if (!GuestRangeReadable(base, pair, 8)) break;
      const uint32_t sz = REX_LOAD_U32(pair) & 0xFFFFFF;
      const uint32_t ib = REX_LOAD_U32(pair + 4);
      if (ib) ScanIndirectBuffer(ib, sz, base, out);
    }
    node = REX_LOAD_U32(at);
  }
}

// Walk the block list the executor walks: node[0] = next, node[1] = count,
// then `count` pairs. Bounded on both axes -- a malformed list must not spin,
// and this runs on the draw path.
uint32_t CountCmdBufBlocks(uint32_t head, uint8_t* base) {
  uint32_t blocks = 0, entries = 0;
  uint32_t node = head;
  while (node && blocks < 64) {
    if (!GuestRangeReadable(base, node, 8)) break;
    ++blocks;
    const uint32_t n = REX_LOAD_U32(node + 4);
    if (n <= 4096) entries += n;
    node = REX_LOAD_U32(node);
  }
  g_cmdEntries += entries;
  return blocks;
}

}  // namespace

// QUALIFIED, and moved out of the anonymous namespace above: the
// ExecuteCommandBuffer hook in hooks_d3d9_entry.cpp calls this, so it is the
// one symbol this translation unit publishes. Declared in hooks_d3d9_pm4.h.
void mx::hooks::d3d9::NoteCommandBufferExec(uint32_t cmdbuf, uint8_t* base) {
  std::lock_guard<std::mutex> lk(g_cmdMu);
  ++g_cmdExec;
  if (!cmdbuf || !GuestRangeReadable(base, cmdbuf, 0x78)) {
    ++g_cmdUnreadable;
    return;
  }
  if ((REX_LOAD_U32(cmdbuf + kCmdBufType) & 0xF) == 0xA) {
    // The executor returns immediately on this, so it submits nothing and must
    // not be counted as geometry we are missing.
    ++g_cmdExecEarlyOut;
    return;
  }
  for (uint32_t i = 0; i < g_cmdBufDistinct; ++i) {
    if (g_cmdBufPtr[i] == cmdbuf) {
      ++g_cmdBufRuns[i];
      return;
    }
  }
  if (g_cmdBufDistinct >= kMaxCmdBufs) {
    ++g_cmdBufOverflow;
    return;
  }
  const uint32_t i = g_cmdBufDistinct++;
  g_cmdBufPtr[i] = cmdbuf;
  g_cmdBufRuns[i] = 1;
  const uint32_t head = REX_LOAD_U32(cmdbuf + kCmdBufBlocks);
  g_cmdBufBlocks[i] = CountCmdBufBlocks(head, base);
  // ONCE per distinct buffer. The contents do not change between replays
  // -- that is the whole point of recording one -- so scanning on every
  // execution would cost the draw path thousands of walks for one answer.
  std::string ib;
  const uint64_t draws_before = g_ibDrawIndx + g_ibDrawIndx2;
  ScanCmdBufBlocks(head, base, ib);
  g_cmdBufDraws[i] =
      uint32_t(g_ibDrawIndx + g_ibDrawIndx2 - draws_before);
  REXLOG_INFO("d3d9: CMDBUF 0x{:08X} first execution -- type 0x{:08X} flags "
              "0x{:08X} blocks {} (list head 0x{:08X})",
              cmdbuf, REX_LOAD_U32(cmdbuf + kCmdBufType),
              REX_LOAD_U32(cmdbuf + kCmdBufFlags), g_cmdBufBlocks[i], head);
  REXLOG_INFO("d3d9: CMDBUF 0x{:08X} indirect buffers:{}", cmdbuf,
              ib.empty() ? " none" : ib);
}

// QUALIFIED. This file does `using namespace mx::hooks::d3d9`, which lets it
// CALL into that namespace but would put an unqualified definition at global
// scope -- a different function from the one the header declares.
void mx::hooks::d3d9::ReportCommandBuffers() {
  std::lock_guard<std::mutex> lk(g_cmdMu);
  std::string rows;
  for (uint32_t i = 0; i < g_cmdBufDistinct; ++i)
    rows += fmt::format(" [0x{:08X} x{} blocks {}]", g_cmdBufPtr[i],
                        g_cmdBufRuns[i], g_cmdBufBlocks[i]);
  // Printed even at zero. "This path is never taken" and "this report is not
  // wired" are different findings, and a suppressed line looks like neither.
  //
  // hidden_draws is THE NUMBER THIS WHOLE THREAD IS FOR: draw packets found
  // inside the buffers, multiplied by how often each buffer is replayed.
  uint64_t hidden_draws = 0;
  for (uint32_t i = 0; i < g_cmdBufDistinct; ++i)
    hidden_draws += g_cmdBufRuns[i] * g_cmdBufDraws[i];
  mx::hooks::d3d9::ReportCmdBufReplay();
  REXLOG_INFO("d3d9: CMDBUF REPLAY -- {} executions ({} early-out, {} "
              "unreadable) over {} distinct buffer(s), {} indirect entries; "
              "NONE of these reach a D3D9 draw entry point:{}{}",
              g_cmdExec, g_cmdExecEarlyOut, g_cmdUnreadable, g_cmdBufDistinct,
              g_cmdEntries, rows.empty() ? " none" : rows,
              g_cmdBufOverflow
                  ? fmt::format(" (+{} runs on buffers past the {} cap)",
                                g_cmdBufOverflow, kMaxCmdBufs)
                  : "");
}
