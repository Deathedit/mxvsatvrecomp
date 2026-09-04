#pragma once

#include "gpu/pm4_parser.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace mx::gpu {

// ---- The ALU constant file, as the GPU actually receives it ----------------
//
// Some guest ALU constants never pass through the D3D9 device shadow OR a
// shader's constant-load table: the runtime writes them straight into the
// register file as Type-0 PM4, and on hardware that file is PERSISTENT. We
// rebuild the bank per draw from the two sources we do model, so any register
// published only that way is left as whatever was in the upload buffer.
//
// Measured across every pm4_dump_native_frame_*.txt:
//
//     122 x  Type0 reg=SHADER_CONSTANT_384_X(0x4600) cnt=48
//
// 0x4600 is ALU dword 1536 = constant c384, and 48 dwords is 12 float4s, so that
// one packet publishes c384..c395. Guest c394 is xe_c[138] in the rebased pixel
// bank, and it read as NaN -- which saturated a full-screen draw to white and is
// why the legal, start and main-menu screens have no background.
//
// This is NOT the LOAD_ALU_CONSTANT path. That one only ever targets index
// 0x3F0/0x7F0 (c252-255, c508-511) in this title and ApplyShaderLoadTable
// already handles it correctly, verified against sub_825656A0 in IDA.
namespace alu {

// Registers are Xenos register indices, i.e. 0x4000 + 4*constant. Writes
// outside [0x4000, 0x4800) are ignored; this file holds ALU constants only.
constexpr uint32_t kAluRegBase = 0x4000;
constexpr uint32_t kAluRegEnd = 0x4800;   // fetch constants start here
constexpr uint32_t kAluConstants = 512;   // float4s, vertex 0-255 / pixel 256-511

void NoteType0Write(uint32_t reg_base, const uint32_t* data, uint32_t count);

// Fill non-finite entries of `bank` from the file, for the `reg_count` float4
// constants starting at `first_reg`. Returns how many dwords were replaced.
//
// Deliberately narrow: it only touches components our own sources left as NaN or
// Inf, so a register the device shadow or the load table set correctly can never
// be clobbered by an end-of-frame PM4 value. It also does NOT invent a value --
// a register the PM4 stream never wrote either is left alone, which is the line
// `hle_sanitize_constants` crossed when it zeroed non-finites and was retired.
//
// `count_finite_zeros` enables a second pass that MEASURES ONLY: it counts
// components our sources left at a finite zero that Type-0 PM4 published a value
// for, and changes nothing. Both banks, since it is now harmless.
//
// It briefly SUBSTITUTED those values and that was wrong in both banks: vertex
// sprayed a stride-6 matrix palette and tore the geometry apart, pixel still
// flashed and never brightened. `g_file` is frame-global last-write-wins, so a
// mid-frame draw gets the frame's final constants -- fine for rare NaN repair,
// destructive the moment it reaches an array. Read `filled_zero` as a WARNING,
// not as a repair count.
uint32_t OverlayNonFinite(uint32_t first_reg, uint32_t* bank,
                          uint32_t reg_count, bool count_finite_zeros);

// Dwords written by PM4, dwords repaired, and how many distinct constants the
// file has ever seen written. `zeroed` counts NaN components nothing ever
// published, set to the register file's power-on 0.0 -- reported separately from
// `repaired` because the two are different claims: one replays a value the GPU
// was given, the other supplies the hardware default.
void Stats(uint64_t& written, uint64_t& repaired, uint32_t& constants_seen,
           uint64_t& zeroed, uint64_t& filled_zero);

// Which constants the zero-fill actually touched, worst first. The total alone
// cannot distinguish "a handful of registers" from "spraying the whole bank",
// and those want opposite fixes — narrow the range, or stop treating a
// frame-global PM4 file as authoritative for a mid-frame draw.
std::string FilledHistogram(uint32_t top);
// The LIVE file contents for the given constants, with unpublished components
// marked. Read a live value here rather than reintroducing a snapshot of
// REFUSALS: the WouldFillValues report that used to sit beside this could not
// express a zero at all, because its pass skipped `v == 0`, and it therefore
// went on printing a stale non-zero after the file had changed underneath it.
std::string FileValues(const uint32_t* consts, size_t n);

// One constant as floats, for callers that need the VALUE rather than a line of
// text. Returns false unless PM4 has published all four components, so an
// unpublished register cannot be mistaken for a published zero -- the same
// distinction FileValues draws with its `unpub:` marker, in a form arithmetic
// can use. Parsing FileValues' string instead would silently swallow that
// marker, which is how a probe ends up dividing by an imaginary zero.
bool FileFloat4(uint32_t c, float* out4);

// THE SAME, BUT DRAW-SCOPED TO THE TERRAIN. The ALU file is global, so
// FileFloat4 returns whatever shader wrote a register last -- measured, not
// feared: c217 (g_HFMapSize) read back as a colour. These read a snapshot of
// c200..c220 taken at the instant a Type-0 packet covering c204
// (gMeshResolution, declared by no other shader in the corpus) writes it.
//
// TerrainFloat4 returns false unless that register was published at snapshot
// time; TerrainSnapshots is 0 until the terrain has drawn, which is a different
// fact from a zero value and must not read the same.
bool TerrainFloat4(uint32_t c, float* out4);
uint64_t TerrainSnapshots();
std::string TerrainValues();

// The clipmap is a LADDER of rings, one gMeshResolution each, all drawn per
// frame -- measured at 32 in one run and 256 in the next. A height read at 256
// units per cell says nothing about a bike, so the ring must be chosen, not
// taken as whichever packet happened to be last.
//
// TerrainFinestRing picks the finest ring whose extent covers the point, where
// "extent" is gVertexOffset.zw -- the clamp the terrain vertex shader itself
// applies to world XZ, so the coverage test is the shader's, not a guess.
bool TerrainFinestRing(float bike_x, float bike_z, float* mesh_res,
                       float* origin_x, float* origin_z);
std::string TerrainRings();

// Substitutions APPLIED in the narrow material-gate window (pixel c84..c87).
// Distinct from the dry-run zero-fill count: a zero here means the window never
// fired, which is a different outcome from firing and changing nothing.
uint64_t MaterialGateFilled();

// Fill the narrow material-gate window (pixel c84..c87) from the Type-0 PM4
// file, for dwords the shader's own load table did NOT publish and that are
// still zero. `bank` is the PIXEL bank (guest c256 at index 0), `load_written`
// is one byte per bank dword, non-zero where the load table wrote. Call AFTER
// ApplyShaderLoadTable -- before it, the table overwrites the fill.
uint32_t FillMaterialGate(uint32_t* bank, uint32_t bank_regs,
                          const uint8_t* load_written);

}  // namespace alu

class XenosGpuState {
 public:
  XenosGpuState() = default;

  void ApplyType0Write(uint32_t reg_base, const uint32_t* data, uint32_t count);
  void ApplyType3Packet(const pm4::Pm4Packet& pkt);
  void ApplyPackets(const std::vector<pm4::Pm4Packet>& packets);

  uint32_t ReadRegister(uint32_t reg) const;
  void WriteRegister(uint32_t reg, uint32_t val);

  const auto& Registers() const { return regs_; }

  // Shared register-name lookup. Single source of truth across the codebase
  // (also called by Pm4Parser::DumpPackets so the dump doesn't show ??? for
  // registers that exist but aren't duplicated in the parser's local table).
  static const char* RegisterName(uint32_t reg);

 private:
  std::unordered_map<uint32_t, uint32_t> regs_;
};

}  // namespace mx::gpu
