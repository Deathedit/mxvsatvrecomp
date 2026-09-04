#include "gpu/xenos_gpu_state.h"

#include <cstdio>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <array>
#include <mutex>
#include <string>
#include <vector>

#include <rex/logging.h>

#include "gpu/guard_census.h"

namespace mx::gpu {

namespace {

// Register indices are Xenos *dword* indices -- the same units a PM4 Type0
// packet's reg_base is in. This table used to be hand-built with every entry
// from 0x2000 up as a BYTE offset, so each one named a register four slots away
// from the one it labelled; those entries were deleted rather than rescaled, and
// the ten names confirmed one at a time against observed values then matched the
// SDK's own table exactly.
//
// The SDK table is Xenia's, dword-indexed, 3434 live entries spanning
// 0x0000..0x5002, and it is NOT sorted -- there are two out-of-order pairs. A
// binary search would silently miss entries, so the lookup is a direct-indexed
// table instead: one pointer per register index, built once.
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

// The terrain clipmap block, captured at the moment the terrain's own Type-0
// packet writes it. c200..c220 covers gOffsetAndScale through gWaterModifiers.
constexpr uint32_t kTerrainFirst = 200;
constexpr uint32_t kTerrainLast = 220;
constexpr uint32_t kTerrainDwords = (kTerrainLast - kTerrainFirst + 1) * 4;
uint32_t g_terrain[kTerrainDwords] = {};
uint8_t g_terrainHave[kTerrainDwords] = {};
uint64_t g_terrainSnaps = 0;

// THE CLIPMAP LADDER. gMeshResolution read 32 in one run and 256 in the next,
// with different origins: the terrain draws SEVERAL rings per frame, each with
// its own resolution and origin, and a snapshot that keeps only the last packet
// keeps the coarsest one. A height sampled at 256 units per cell says nothing
// useful about a bike.
struct TerrainRing {
  float mesh_res = 0.f;
  float origin_x = 0.f, origin_z = 0.f;
  float extent_x = 0.f, extent_z = 0.f;
  uint64_t packets = 0;
};
constexpr uint32_t kMaxRings = 12;
TerrainRing g_rings[kMaxRings];
uint32_t g_ringCount = 0;
uint64_t g_ringOverflow = 0;
// One bit per dword. Without it "never written" and "written as 0.0" are the
// same value, and repairing a register nobody published would be inventing one.
uint32_t g_have[kFileDwords / 32] = {};
uint64_t g_written = 0;
uint64_t g_repaired = 0;
uint64_t g_zeroed = 0;
// Components substituted from the PM4 file where our own bank held a finite
// ZERO. Separate from g_repaired on purpose: that one replays a value over a
// NaN nobody could have meant, this one overrides a zero the guest might have
// meant. Different claims, different counters.
uint64_t g_filledZero = 0;
// WHICH constants the zero-fill touches, per guest constant index. The total
// alone cannot say whether the fill is hitting a handful of registers or
// spraying the bank, and those want opposite fixes: the first narrows to a
// range, the second means the PM4 file is the wrong authority per draw.
uint64_t g_filledByConst[kAluConstants] = {};
// The VALUE we declined to write, per dword. Counting how often a constant could
// be filled says nothing about whether the thing we would have filled it with is
// sane -- and the last attempt at this substitution was reverted for spraying
// end-of-frame garbage.
//
// g_materialGateFilled counts substitutions actually APPLIED in the narrow
// material-gate window, separate from g_filledZero's whole dry-run population,
// because "the window never fires" and "the window fires and changes nothing"
// are different outcomes.
uint64_t g_materialGateFilled = 0;

// The pixel ALU bank starts at guest constant 256; callers pass first_reg=256
// for it and 0 for the vertex bank.
constexpr uint32_t kPixelBankFirstReg = 256;
// Guest c340..c343 == PIXEL xe_c[84..87]: the material block PM4 publishes as a
// unit via SHADER_CONSTANT_340_X. See the substitution site for why this window
// and no wider.
constexpr uint32_t kMaterialGateFirstConst = 256 + 84;
constexpr uint32_t kMaterialGateEndConst = 256 + 88;

// vs c32 IS NOT A TINT, and the fill that assumed it was is gone:
//
//   - PM4 publishes vs c32 as a plain (0,0,0,0) for 97.2% of a run (468,445
//     all-zero float4s against 13,520 non-zero). The zero is the guest's own
//     value, not a hole we failed to fill.
//   - the (1,1,1,1) that made it look like a white tint is an early-boot value
//     that g_file's last-write-wins never let go of. The WOULD-FILL VALUES
//     report kept printing it because it snapshotted declined NON-ZERO values
//     and structurally could not show a zero -- a diagnostic that cannot express
//     the state you are looking for is what cost the time.
//   - no shader in legal.rdc reads xe_c[32] at all.
//   - the premise underneath was wrong anyway: the legal-screen logo and the
//     intro are NOT missing. They render.
//
// Substituting it changed nothing except to add a flashing line at the bottom of
// those screens -- the third time a fill from this frame-global file has
// produced flashing and no repair.


bool NonFinite(uint32_t bits) {
  // IEEE-754: exponent all ones is Inf (mantissa 0) or NaN (mantissa non-zero).
  return (bits & 0x7F800000u) == 0x7F800000u;
}

// Exponent zero with a non-zero mantissa: a DENORMAL. Its own tiny magnitude is
// not the problem -- shader_alu.cpp's LegacyMul treats anything under
// kSmallestNormal as an exact zero, because that is what D3D9 fixed-function
// multiply does on Xenos.
//
// It is also a reliable junk MARKER on this title. The one value PM4 has ever
// been seen publishing into a colour slot is 2.074e-42, whose bit pattern is
// 1480 -- a small INTEGER read as a float, seen at both ps c85.y and vs c32.y.
bool Denormal(uint32_t bits) {
  return (bits & 0x7F800000u) == 0 && (bits & 0x007FFFFFu) != 0;
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
    //    authoritative can only suppress a better answer;
    //  - the frame-range walk covers [prev_after, write_before) of the ring,
    //    which can include bytes the guest has not written this frame. Garbage
    //    there decodes as a plausible Type0 write and would otherwise stamp a
    //    `have` bit over a register nothing really published.
    //
    // Measured: with NaN allowed to publish, the file claimed 362 constants and
    // 4.2M dwords -- far more than the ALU-range Type0 writes present in any pm4
    // dump -- and c136..c139 stayed NaN because OverlayNonFinite saw them as
    // published.
    if ((data[i] & 0x7F800000u) == 0x7F800000u && (data[i] & 0x007FFFFFu) != 0)
      continue;
    // TARGETED TRACE: the PIXEL bank's c140..c143, i.e. guest ALU constants
    // 396..399, dwords 1584..1599. Watching dwords 560..575 instead compares
    // different registers -- pixel constants 256..511 map into the shader's bank
    // at reg-256, so the mask pass's xe_c[140] is guest constant 396 -- and any
    // conclusion drawn from that disagreement is void.
    //
    // shadows--.rdc shows those four registers holding c100..c103 with a
    // corrupted THIRD COLUMN -- c14x.z equals the negated .y of c128..c131 --
    // which is the shape of "player shadow correct, distant shadow tracking the
    // camera". This says whether the GUEST publishes that column or we assemble
    // it.
    if (d >= 1584u && d < 1600u) {
      // NON-ZERO ONLY, and the cap counts what it PRINTS. Logging every write
      // spends all 48 lines on the bulk zero-fill plus an early all-zero block;
      // the values worth seeing arrive later.
      //
      // The block itself is already informative: base 0x4200 count 64 is
      // constant 128 for 16 constants, so the guest writes c128..c143 in ONE
      // packet -- exactly the span where c14x.z appears to take c12x.y.
      static uint32_t s_traced = 0;
      if (data[i] != 0u && s_traced++ < 64) {
        float f;
        std::memcpy(&f, &data[i], sizeof(f));
        REXLOG_INFO(
            "gpu: TYPE0 guest c{} = PIXEL c{}.{} = {} (0x{:08X}) [reg 0x{:X}, "
            "dword {}, base 0x{:X} count {}]",
            d / 4u, (d / 4u) - 256u, "xyzw"[d & 3u], f, data[i], reg, d,
            reg_base, count);
      }
    }
    g_file[d] = data[i];
    g_have[d >> 5] |= 1u << (d & 31);
    ++g_written;
  }

  // THE TERRAIN SNAPSHOT.
  //
  // The ALU file is GLOBAL: a read at an arbitrary moment returns whatever
  // shader wrote last, which is not a theory -- c217, labelled g_HFMapSize, was
  // measured reading [0.133 0.149 0.180 1], a colour with alpha 1. So the
  // terrain's clipmap constants have to be captured while the TERRAIN's write is
  // happening.
  //
  // Keyed on c204 (gMeshResolution). No other shader in the corpus declares that
  // register, so a Type-0 packet covering it is the terrain's, and the whole
  // c200..c220 block is copied out at that instant -- draw-scoped by
  // construction and needing no shader identification, which is the point:
  // identifying the draw by hashing its microcode against the static assets does
  // NOT work, because runtime blobs carry trailing padding the static decoder
  // excludes and only 1 of 72 runtime dumps matched.
  const uint32_t c204_reg = kAluRegBase + 204 * 4;
  if (reg_base <= c204_reg && c204_reg < reg_base + count) {
    for (uint32_t c = kTerrainFirst; c <= kTerrainLast; ++c) {
      for (uint32_t i = 0; i < 4; ++i) {
        const uint32_t d = c * 4 + i;
        g_terrain[(c - kTerrainFirst) * 4 + i] = g_file[d];
        // Published state is carried across too. A register the terrain packet
        // did not cover keeps whatever the file had, and the caller must be
        // able to tell that from a value the terrain actually set.
        const bool pub = (g_have[d >> 5] & (1u << (d & 31))) != 0;
        g_terrainHave[(c - kTerrainFirst) * 4 + i] = pub ? 1 : 0;
      }
    }
    ++g_terrainSnaps;

    // Record this packet's ring. Keyed on gMeshResolution, which is what
    // distinguishes the rings; the origin travels with the viewer so it is
    // stored as the latest value rather than as part of the key.
    float mres, voff[4];
    std::memcpy(&mres, &g_file[204 * 4], sizeof(mres));
    for (uint32_t i = 0; i < 4; ++i)
      std::memcpy(&voff[i], &g_file[201 * 4 + i], sizeof(float));
    uint32_t r = 0;
    for (; r < g_ringCount; ++r)
      if (g_rings[r].mesh_res == mres) break;
    if (r == g_ringCount && g_ringCount < kMaxRings) {
      g_rings[g_ringCount++] = TerrainRing{};
      g_rings[r].mesh_res = mres;
    }
    if (r < g_ringCount) {
      g_rings[r].origin_x = voff[0];
      g_rings[r].origin_z = voff[1];
      g_rings[r].extent_x = voff[2];
      g_rings[r].extent_z = voff[3];
      ++g_rings[r].packets;
    } else {
      ++g_ringOverflow;
    }
  }
}

// A REAL ring, not a stray packet. The measured ladder is res 1,2,4,...,256 with
// 386-706 packets each; alongside it were three entries with 1, 3 and 8 packets
// and values like 3.66e-42 -- Type-0 packets that covered c204 without being the
// terrain's. Both tests are needed: the count separates hundreds from single
// digits, a two-orders-of-magnitude gap rather than a tuned threshold, and a
// clipmap resolution is a positive power of two.
bool RealRing(const TerrainRing& e) {
  if (!(e.mesh_res > 0.f) || e.packets < 16) return false;
  const float r = e.mesh_res;
  if (r != std::floor(r) || r > 4096.f) return false;
  const uint32_t i = static_cast<uint32_t>(r);
  return (i & (i - 1)) == 0;  // power of two
}

bool TerrainFinestRing(float bike_x, float bike_z, float* mesh_res,
                       float* origin_x, float* origin_z) {
  std::lock_guard<std::mutex> lk(g_mu);
  // The FINEST ring whose extent covers the point. Extent is gVertexOffset.zw,
  // the clamp the terrain vertex shader applies to world XZ, so "covers" is the
  // shader's own test rather than a guess about ring size.
  const TerrainRing* best = nullptr;
  for (uint32_t r = 0; r < g_ringCount; ++r) {
    const TerrainRing& e = g_rings[r];
    if (!RealRing(e)) continue;
    if (std::fabs(bike_x - e.origin_x) > e.extent_x) continue;
    if (std::fabs(bike_z - e.origin_z) > e.extent_z) continue;
    if (!best || e.mesh_res < best->mesh_res) best = &e;
  }
  if (!best) return false;
  *mesh_res = best->mesh_res;
  *origin_x = best->origin_x;
  *origin_z = best->origin_z;
  return true;
}

std::string TerrainRings() {
  std::lock_guard<std::mutex> lk(g_mu);
  if (!g_ringCount) return std::string(" (no terrain packet seen)");
  std::string out;
  for (uint32_t r = 0; r < g_ringCount; ++r) {
    const TerrainRing& e = g_rings[r];
    if (!RealRing(e)) {
      // Counted, not hidden: a rejected ring is evidence about the keying, and
      // if the count ever climbs it means c204 is not terrain-exclusive after
      // all and the whole snapshot is built on sand.
      out += fmt::format(" [REJECTED res {:g} x{}]", e.mesh_res, e.packets);
      continue;
    }
    out += fmt::format(" [res {:g} origin ({:g}, {:g}) extent ({:g}, {:g}) x{}]",
                       e.mesh_res, e.origin_x, e.origin_z, e.extent_x,
                       e.extent_z, e.packets);
  }
  if (g_ringOverflow) out += fmt::format(" (+{} rings dropped)", g_ringOverflow);
  return out;
}

bool TerrainFloat4(uint32_t c, float* out4) {
  if (c < kTerrainFirst || c > kTerrainLast || !out4) return false;
  std::lock_guard<std::mutex> lk(g_mu);
  if (!g_terrainSnaps) return false;
  const uint32_t base = (c - kTerrainFirst) * 4;
  for (uint32_t i = 0; i < 4; ++i) {
    if (!g_terrainHave[base + i]) return false;
    std::memcpy(&out4[i], &g_terrain[base + i], sizeof(float));
  }
  return true;
}

uint64_t TerrainSnapshots() {
  std::lock_guard<std::mutex> lk(g_mu);
  return g_terrainSnaps;
}

std::string TerrainValues() {
  std::lock_guard<std::mutex> lk(g_mu);
  if (!g_terrainSnaps) return std::string(" (no terrain packet seen)");
  std::string out = fmt::format(" (from {} terrain packets)", g_terrainSnaps);
  for (uint32_t c = kTerrainFirst; c <= kTerrainLast; ++c) {
    const uint32_t b = (c - kTerrainFirst) * 4;
    bool any = false;
    for (uint32_t i = 0; i < 4; ++i) any = any || g_terrainHave[b + i];
    if (!any) continue;
    out += fmt::format(" c{}=[", c);
    for (uint32_t i = 0; i < 4; ++i) {
      float f;
      std::memcpy(&f, &g_terrain[b + i], sizeof(f));
      out += fmt::format("{}{}{:g}", i ? " " : "",
                         g_terrainHave[b + i] ? "" : "unpub:", f);
    }
    out += "]";
  }
  return out;
}

uint32_t OverlayNonFinite(uint32_t first_reg, uint32_t* bank,
                          uint32_t reg_count, bool count_finite_zeros) {
  if (!bank || first_reg >= kAluConstants) return 0;
  const uint32_t regs = std::min(reg_count, kAluConstants - first_reg);
  uint32_t fixed = 0;
  // Guard-census tallies, flushed once after the loop. See the note inside.
  uint64_t n_seen = 0, n_fired = 0, n_bdSeen = 0, n_bdFired = 0;
  std::lock_guard<std::mutex> lk(g_mu);
  // THE DENOMINATORS ARE STRUCTURAL, SO THEY ARE COUNTED STRUCTURALLY. The
  // population is "every component examined", and this loop examines every
  // component unconditionally, so the count is `regs * 4`. Accumulating it with
  // a ++ inside the loop, alongside an unconditional g_have load and a range
  // test ahead of the `if (!NonFinite(cur)) continue;` that discards ~all of it,
  // was the cost: batching the atomics took this phase 8ms -> 3.5ms. The census
  // is IDENTICAL, because a `fired` can only happen where NonFinite already
  // holds.
  n_seen = uint64_t(regs) * 4;
  // Same closed form for the backdrop block: guest c392..c395 are the guest
  // constants in [392, 396), this call covers [first_reg, first_reg + regs),
  // and each constant is four components.
  {
    const uint32_t lo = std::max(first_reg, 392u);
    const uint32_t hi = std::min(first_reg + regs, 396u);
    n_bdSeen = hi > lo ? uint64_t(hi - lo) * 4 : 0;
  }
  for (uint32_t i = 0; i < regs * 4; ++i) {
    const uint32_t cur = bank[i];
    if (!NonFinite(cur)) continue;
    const uint32_t d = first_reg * 4 + i;
    const bool published = (g_have[d >> 5] & (1u << (d & 31))) != 0;
    // Fires only when we ACTUALLY substitute, so the census tracks the guard
    // rather than the opportunity -- with strict on it should fall to zero, and
    // a non-zero reading would mean the switch is not reaching here.
    if (!published && (cur & 0x007FFFFFu) != 0 &&
        !mx::gpu::guard::Strict(mx::gpu::guard::Guard::kConstantNanToZero)) {
      ++n_fired;
      const uint32_t guest_const = d >> 2;
      if (guest_const >= 392 && guest_const < 396) ++n_bdFired;
    }
    if (published && !NonFinite(g_file[d])) {
      bank[i] = g_file[d];
      ++fixed;
      continue;
    }
    // Nothing published it. On hardware the constant is not garbage -- the Xenos
    // register file POWERS ON ZEROED, and a title reading a constant it never
    // wrote gets 0.0 (register_file.cc:18). We rebuild the bank per draw out of
    // a device shadow whose backing memory the guest has not written yet, so we
    // hand the shader dirty heap instead.
    //
    // THE SUBSTITUTION IS GONE. The NaN stays in the bank and reaches the
    // shader; the measurement stays, so the population is still known.
    //
    // Its justification was that c392..c395 are NaN for a bounded startup prefix
    // "which is why the legal, loading and start screens have no background".
    // Both halves are falsified: censused on its own that block reads 0/32 at
    // the first report and 21.5% at the last, so it CLIMBS rather than freezing;
    // and with hle_strict=8 and 1.76M NaNs reaching the shaders, the logo and
    // intro are FINE and a level is unchanged.
    //
    // g_zeroed is REPURPOSED rather than deleted: it now counts NaNs LEFT IN
    // PLACE, the same population it used to count substituting. A counter whose
    // increment site disappears prints a permanent zero that reads as a
    // measurement.
    if (!published && (cur & 0x007FFFFFu) != 0) ++g_zeroed;
  }

  // SECOND PASS: a component our sources left at a FINITE ZERO, which PM4
  // published a non-zero value for. The pass above cannot reach these -- it
  // opens with `if (!NonFinite(cur)) continue;` -- so a constant whose ONLY
  // publisher is Type-0 PM4 stayed 0.0 forever.
  //
  // SHADER_CONSTANT_340_X (0x4550) is written 330 times across the pm4 dumps =
  // PIXEL c84-c87, and the menu rider's material gates a lighting term on c85.w:
  //
  //     saturate(tex1.x + c85.w - 1)     w=1 -> tex1.x ;  w=0 -> 0
  //
  // Guards: exact zeros only, only where PM4 genuinely published, never
  // substituting a zero or a non-finite. Callers apply the shader load tables
  // AFTER this.
  //
  // MEASURED CONSEQUENCE: 6,705,127 substitutions in a 1020-frame menu run. It
  // did NOT brighten the scene and the menu developed a new visual fault, so the
  // population it reaches is dominated by zeros the guest MEANT -- treat a
  // non-zero `filled_zero` as a warning rather than a repair. The histogram is
  // why it must not mutate: with substitution on, the vertex top was c155, c161,
  // c167, c173, c179, c185, STRIDE 6 with counts decaying smoothly, a matrix
  // palette being filled entry by entry with end-of-frame values.
  //
  // BLOCK SKIP. Every iteration ends at `if (!(g_have[...] & bit)) continue;`
  // and g_have is a bitmap 32 dwords to the word, so a zero word means none of
  // its 32 dwords can pass. Exact, not sampled.
  for (uint32_t i = 0; count_finite_zeros && i < regs * 4; ++i) {
    const uint32_t d = first_reg * 4 + i;
    if ((d & 31) == 0 && g_have[d >> 5] == 0) {
      i += 31;  // the ++i finishes the block
      continue;
    }
    if (bank[i] != 0) continue;
    if (!(g_have[d >> 5] & (1u << (d & 31)))) continue;
    const uint32_t v = g_file[d];
    if (v == 0 || NonFinite(v)) continue;
    // DRY RUN. This used to assign `bank[i] = v`, and that was wrong in BOTH
    // banks: vertex, where it sprayed a stride-6 matrix palette with
    // end-of-frame values and tore the geometry apart; and pixel, where 84
    // constants still produced flashing and no brightening. The cause is that
    // `g_file` is FRAME-GLOBAL last-write-wins, so a mid-frame draw gets the
    // frame's final constants -- harmless for NaN repair, destructive the moment
    // it reaches an array.
    //
    // Kept as a COUNT, because the question it answers is still open: which
    // constants are published only by Type-0 PM4 and left at zero by our two
    // modelled sources.
    ++g_filledZero;
    ++g_filledByConst[d >> 2];

    // The substitution that used to live here has MOVED to FillMaterialGate,
    // which runs after the shader load table instead of before it. Doing it here
    // was wrong for a reason this file already documents: "Callers apply the
    // shader load tables AFTER this, so those still override it." Measured:
    // 368,313 substitutions fired and the terrain did not change, because the
    // terrain shader's own load table wrote c85 straight back to zero -- while
    // materials with NO load-table entry kept the value, which is why the BIKE
    // AND RIDER changed appearance and the ground did not.
    //
    // The window is not an array: it is a four-constant material block PM4
    // publishes as a unit (SHADER_CONSTANT_340_X, 330 writes across the dumps =
    // PIXEL c84-c87).
    //
    // PROVEN, capture3.rdc draw 19725 (terrain, 1741 indices), pixel (640,640):
    //
    //   193: add r7.x, r1.w, xe_c[85].w   0.774069 + 0         = 0.774069
    //   204: add r6.xy, r7.ywyy, r7.xzxx  0.774069 + -0.018824 = 0.755246
    //   205: add_sat r0.x, r6.x, c255.w   saturate(0.755246 - 0.8) = 0.0
    //   209: mul r1.xyz, r0.xxxx, r1.xyzx 0 x (0.070, 0.034, -0.014) = 0
    //
    // c85.w = 1 makes that saturate(1.755 - 0.8) = 0.955 and the terrain diffuse
    // survives. Everything else in that draw is correct: world Y 611.4 is real
    // terrain height, both textures sample fine, and c86/c87 are populated. c85
    // is the only zero among populated neighbours -- unwritten, not wrong.
    //
    // PIXEL BANK ONLY. The frame-global risk is bounded there: the worst case is
    // a material gated OFF this frame being lit as though it were ON, which is
    // visible and reversible. A sprayed matrix palette was neither.
  }
  // The census, flushed once. Two calls reproduce exactly what 1024 per-dword
  // calls produced: population = clean + fired, fires = fired.
  if (n_seen) {
    mx::gpu::guard::Note(mx::gpu::guard::Guard::kConstantNanToZero, false,
                         n_seen - n_fired);
    if (n_fired)
      mx::gpu::guard::Note(mx::gpu::guard::Guard::kConstantNanToZero, true,
                           n_fired);
  }
  if (n_bdSeen) {
    mx::gpu::guard::Note(mx::gpu::guard::Guard::kConstantNanBackdrop, false,
                         n_bdSeen - n_bdFired);
    if (n_bdFired)
      mx::gpu::guard::Note(mx::gpu::guard::Guard::kConstantNanBackdrop, true,
                           n_bdFired);
  }
  g_repaired += fixed;
  return fixed;
}

uint64_t MaterialGateFilled() {
  std::lock_guard<std::mutex> lk(g_mu);
  return g_materialGateFilled;
}

uint32_t FillMaterialGate(uint32_t* bank, uint32_t bank_regs,
                          const uint8_t* load_written) {
  // Runs AFTER the shader's load table, and never overwrites it. Two guards, and
  // both matter:
  //
  //   load_written[d]  the shader published this dword itself. Its value is
  //                    authoritative even when it is zero. This is the guard
  //                    that was missing when the fill lived inside
  //                    OverlayNonFinite: there it ran BEFORE the load table.
  //   bank[d] != 0     something already supplied a value. We only ever fill a
  //                    hole, never replace.
  //
  // The window is pixel c84..c87 -- guest c340..c343, the material block PM4
  // publishes as a unit. It is NOT an array, which is why the frame-global
  // staleness that tore the vertex matrix palette apart does not apply.
  if (!bank || !load_written) return 0;
  uint32_t filled = 0;
  std::lock_guard<std::mutex> lk(g_mu);
  // WHAT DOES PM4 ACTUALLY PUBLISH HERE? Never checked, and the whole theory
  // rests on it: "c85.w should be 1" came from reading the shader arithmetic
  // backwards, not from observing a published value. If PM4's own c85.w is 0 or
  // unpublished then this fill can never set it, ordering or no ordering.
  {
    // SAMPLED LATE, NOT ONCE. Firing on the first call reports the whole window
    // `unpub:0` -- true at startup and saying nothing, because PM4 has not
    // published anything yet -- while the guard census showed 209,629 fills out
    // of 2,663,216 window dwords in the same run. Now: report the first time
    // anything in the window is actually published, and again every 200k calls.
    static uint64_t s_calls = 0;
    static bool s_sawPublished = false;
    bool any_published = false;
    for (uint32_t c = kMaterialGateFirstConst;
         c < kMaterialGateEndConst && !any_published; ++c)
      for (uint32_t comp = 0; comp < 4; ++comp) {
        const uint32_t d = c * 4 + comp;
        if (g_have[d >> 5] & (1u << (d & 31))) { any_published = true; break; }
      }
    const bool first_publish = any_published && !s_sawPublished;
    if (first_publish) s_sawPublished = true;
    if (first_publish || (++s_calls % 200000) == 0) {
      std::string w;
      for (uint32_t c = kMaterialGateFirstConst; c < kMaterialGateEndConst; ++c) {
        w += fmt::format(" ps c{}=[", c - kPixelBankFirstReg);
        for (uint32_t comp = 0; comp < 4; ++comp) {
          const uint32_t d = c * 4 + comp;
          const bool pub = (g_have[d >> 5] & (1u << (d & 31))) != 0;
          float f;
          std::memcpy(&f, &g_file[d], sizeof(f));
          w += fmt::format("{}{}{}", comp ? " " : "", pub ? "" : "unpub:", f);
        }
        w += "]";
      }
      REXLOG_INFO("gpu: MATERIAL GATE PM4 STATE --{}", w);
    }
  }
  for (uint32_t c = kMaterialGateFirstConst; c < kMaterialGateEndConst; ++c) {
    const uint32_t reg = c - kPixelBankFirstReg;  // bank is pixel-relative
    if (reg >= bank_regs) continue;
    for (uint32_t comp = 0; comp < 4; ++comp) {
      const uint32_t b = reg * 4 + comp;
      const uint32_t d = c * 4 + comp;  // g_file is indexed by guest dword
      const bool eligible =
          !load_written[b] && bank[b] == 0 &&
          (g_have[d >> 5] & (1u << (d & 31))) != 0 && g_file[d] != 0 &&
          !NonFinite(g_file[d]);
      // Every dword in the window is an opportunity, so the census can say
      // whether this fills 4 of 16 or 16 of 16 -- and 0 of 16 would mean PM4
      // never publishes here, which kills the theory outright.
      mx::gpu::guard::Note(mx::gpu::guard::Guard::kMaterialGateFill, eligible);
      // SUBSTITUTION REMOVED. Not because it "did not work" -- the measurement
      // retired the SOURCE. Sampled at first publication rather than at startup:
      //
      //   ps c85 = [ unpub:0   2.074e-42   unpub:0   unpub:0 ]
      //
      // c85.w -- the component the terrain shader gates on -- is UNPUBLISHED.
      // PM4 never carries it, at any point in the frame, so no amount of
      // reordering against the shader load table could have supplied it.
      //
      // Worse, the ONE dword PM4 does publish in this window is a denormal whose
      // bit pattern is ~1481 -- an INTEGER read as a float, so the Type-0 capture
      // is recording a register that is not an ALU constant, and the live
      // substitution injected it into c85.y on ~94,000 draws a run.
      //
      // The Note() above STAYS, so if PM4 ever does publish here the census says
      // so. Do not restore the assignment without first showing c85.w published
      // with a plausible value.
      if (!eligible) continue;
      ++filled;
      ++g_materialGateFilled;
    }
  }
  return filled;
}

// The zero-fill population, worst first. `guest` is the ALU constant index;
// subtract 256 for the pixel bank's xe_c[] numbering.
//
// FileValues reports what we WOULD have filled a constant with. Read it with the
// control in mind: c252..c255 are the screen-space scale/bias the D3D9 shader
// load table is measured to carry -- values like (0.5, -0.5, 0, 0). If those
// come back looking like that the constant file is sane; if they come back as
// garbage, the file is stale or misindexed and NOTHING here should be acted on.
std::string FileValues(const uint32_t* consts, size_t n) {
  // The LIVE contents of g_file, deliberately not the shape the removed
  // WouldFillValues report had: that one snapshotted values the zero-fill
  // DECLINED and skipped `v == 0`, so it structurally could not report a zero
  // and went on saying c32 = (1,1,1,1) for thirty seconds after the fill had
  // stopped firing. `unpub:` marks a component PM4 has never written, which is a
  // different state from a published 0.0.
  std::lock_guard<std::mutex> lk(g_mu);
  std::string out;
  for (size_t k = 0; k < n; ++k) {
    const uint32_t c = consts[k];
    if (c >= kAluConstants) continue;
    out += c >= 256 ? fmt::format(" c{}(ps c{})=[", c, c - 256)
                    : fmt::format(" c{}(vs)=[", c);
    for (uint32_t i = 0; i < 4; ++i) {
      const uint32_t d = c * 4 + i;
      const bool pub = (g_have[d >> 5] & (1u << (d & 31))) != 0;
      float f;
      std::memcpy(&f, &g_file[d], sizeof(f));
      out += fmt::format("{}{}{:g}", i ? " " : "", pub ? "" : "unpub:", f);
    }
    out += "]";
  }
  return out;
}

bool FileFloat4(uint32_t c, float* out4) {
  if (c >= kAluConstants || !out4) return false;
  std::lock_guard<std::mutex> lk(g_mu);
  for (uint32_t i = 0; i < 4; ++i) {
    const uint32_t d = c * 4 + i;
    if ((g_have[d >> 5] & (1u << (d & 31))) == 0) return false;
    std::memcpy(&out4[i], &g_file[d], sizeof(float));
  }
  return true;
}

std::string FilledHistogram(uint32_t top) {
  std::lock_guard<std::mutex> lk(g_mu);
  std::vector<std::pair<uint64_t, uint32_t>> v;
  v.reserve(kAluConstants);
  for (uint32_t c = 0; c < kAluConstants; ++c)
    if (g_filledByConst[c]) v.emplace_back(g_filledByConst[c], c);
  std::sort(v.rbegin(), v.rend());
  std::string out;
  char one[64];
  for (size_t i = 0; i < v.size() && i < top; ++i) {
    const uint32_t c = v[i].second;
    if (c >= 256)
      std::snprintf(one, sizeof(one), " c%u(ps c%u)x%llu", c, c - 256,
                    static_cast<unsigned long long>(v[i].first));
    else
      std::snprintf(one, sizeof(one), " c%u(vs)x%llu", c,
                    static_cast<unsigned long long>(v[i].first));
    out += one;
  }
  if (v.size() > top) out += " ...";
  std::snprintf(one, sizeof(one), " [%zu distinct]", v.size());
  out += one;
  return out;
}

void Stats(uint64_t& written, uint64_t& repaired, uint32_t& constants_seen,
           uint64_t& zeroed, uint64_t& filled_zero) {
  std::lock_guard<std::mutex> lk(g_mu);
  written = g_written;
  repaired = g_repaired;
  zeroed = g_zeroed;
  filled_zero = g_filledZero;
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

    // PM4_REG_RMW (xenos.h:1571) -- read-modify-write of ONE register. Body is
    // three dwords: (rmw_info, and-operand, or-operand), with the target
    // register in the low bits of rmw_info.
    //
    // This used to treat rmw_info as a base and the two operands as a RUN of
    // register values, and the comment that stood here called the case dead --
    // "the Type3 histogram has no 0x21". It fires 23 times in a menu run.
    //
    // The damage was nil, which is why it survived: every register it reaches is
    // scanout, not 3D (D1GRPH_CONTROL, D1GRPH_LUT_10BIT_BYPASS_CNTL,
    // XDVO_FORCE_OUTPUT_CNTL, XDVO_FORCE_DATA). This fix should change no pixel
    // -- if a frame moves, something reads a display register and THAT is the
    // finding.
    case 0x21: {
      if (pkt.body.size() < 3) break;
      const uint32_t rmw_info = pkt.body[0];
      const uint32_t and_operand = pkt.body[1];
      const uint32_t or_operand = pkt.body[2];

      // Encoding verified against Xenia's pm4_command_processor_implement.h:925,
      // which carries implementations the SDK headers do not:
      //
      //   reg = rmw_info & 0x1FFF
      //   bit 31 set -> AND operand is a register index, else an immediate
      //   bit 30 set -> OR  operand is a register index, else an immediate
      //
      // The address field really is 13 bits, and that is NOT an array clamp:
      // Xenia's own register file is 0x5003 entries, so it could address more
      // and deliberately does not. An earlier mask of 0xFFFF agrees with this on
      // all observed traffic, which is exactly why it would not have shown up.
      const uint32_t reg = rmw_info & 0x1FFFu;
      const uint32_t and_mask = ((rmw_info >> 31) & 1u)
                                    ? ReadRegister(and_operand & 0x1FFFu)
                                    : and_operand;
      const uint32_t or_mask = ((rmw_info >> 30) & 1u)
                                   ? ReadRegister(or_operand & 0x1FFFu)
                                   : or_operand;
      const uint32_t before = ReadRegister(reg);
      const uint32_t after = (before & and_mask) | or_mask;
      WriteRegister(reg, after);

      static int s_logged = 0;
      if (++s_logged <= 8) {
        REXLOG_INFO("gpu_state: REG_RMW #{} reg=0x{:04X} and=0x{:08X} "
                    "or=0x{:08X} (indirect and={} or={}) : 0x{:08X} -> 0x{:08X}",
                    s_logged, reg, and_mask, or_mask, (rmw_info >> 31) & 1u,
                    (rmw_info >> 30) & 1u, before, after);
      }
      break;
    }

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

// Snapshot() / DumpDiff() / prev_regs_ removed 2026-08-17. A frame-over-frame
// register diff, with no caller since the PM4 translator was retired — the
// register shadow is now read directly by the D3D9 HLE path, which asks for
// specific registers rather than for what changed. RegisterName stays: it is
// live through Pm4Parser::RegisterName.

}  // namespace mx::gpu
