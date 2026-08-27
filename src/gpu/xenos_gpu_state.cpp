#include "gpu/xenos_gpu_state.h"

#include <cstdio>
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
// The VALUE we declined to write, per dword. Counting how often a constant
// could be filled says nothing about whether the thing we would have filled it
// with is sane -- and the last attempt at this substitution was reverted for
// spraying end-of-frame garbage. Recording the value makes that checkable
// BEFORE turning anything on.
uint32_t g_wouldFillVal[kFileDwords] = {};
// Substitutions actually APPLIED in the narrow material-gate window, as opposed
// to g_filledZero which counts the whole dry-run population. Separate because
// "the window never fires" and "the window fires and changes nothing" are
// different outcomes and a shared counter would hide the first.
uint64_t g_materialGateFilled = 0;

// The pixel ALU bank starts at guest constant 256; callers pass first_reg=256
// for it and 0 for the vertex bank.
constexpr uint32_t kPixelBankFirstReg = 256;
// Guest c340..c343 == PIXEL xe_c[84..87]: the material block PM4 publishes as a
// unit via SHADER_CONSTANT_340_X. See the substitution site for why this window
// and no wider.
constexpr uint32_t kMaterialGateFirstConst = 256 + 84;
constexpr uint32_t kMaterialGateEndConst = 256 + 88;

// VERTEX c32 -- the tint the UI/logo draws multiply their sampled texel by.
// Proven in legal.rdc: the texture samples white, xe_c[32] arrives (0,0,0,1),
// and LegacyMul turns white x 0 into +0. PM4 publishes (1,1,1,1) for it.
//
// ONE constant wide, on purpose. The precedent in this file is that a fill from
// the frame-global g_file is safe exactly when its window is not an ARRAY: the
// blanket version tore a stride-6 vertex matrix palette apart with end-of-frame
// values. A single tint register cannot be a palette entry.
constexpr uint32_t kVertexTintFirstConst = 32;
constexpr uint32_t kVertexTintEndConst = 33;

// Vertex-tint fill outcomes. Three counters, not one, because "the window never
// validated" and "the window validated and had nothing to fill" are different
// findings and a single total would print the same number for both.
uint64_t g_vertexTintFilled = 0;    // dwords actually substituted
uint64_t g_vertexTintApplied = 0;   // float4s that passed validation
uint64_t g_vertexTintDenormal = 0;  // float4s rejected for a denormal component
uint64_t g_vertexTintUnpub = 0;     // float4s rejected as not fully published

bool NonFinite(uint32_t bits) {
  // IEEE-754: exponent all ones is Inf (mantissa 0) or NaN (mantissa non-zero).
  return (bits & 0x7F800000u) == 0x7F800000u;
}

// Exponent zero with a non-zero mantissa: a DENORMAL. Its own tiny magnitude is
// not the problem -- what matters is that shader_alu.cpp's LegacyMul treats
// anything under kSmallestNormal (1.175e-38) as an exact zero, because that is
// what D3D9 fixed-function multiply does on Xenos. So a denormal reaching a
// tint constant is indistinguishable from black at the point of use.
//
// It is also, on this title, a reliable junk MARKER. The one value PM4 has ever
// been seen publishing into a colour slot is 2.074e-42, whose bit pattern is
// 1480 -- a small INTEGER read as a float, seen at both ps c85.y (see
// FillMaterialGate) and vs c32.y. A shader constant does not look like that.
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
                          uint32_t reg_count, bool count_finite_zeros) {
  if (!bank || first_reg >= kAluConstants) return 0;
  const uint32_t regs = std::min(reg_count, kAluConstants - first_reg);
  uint32_t fixed = 0;
  std::lock_guard<std::mutex> lk(g_mu);
  for (uint32_t i = 0; i < regs * 4; ++i) {
    const uint32_t cur = bank[i];
    // Noted BEFORE the non-finite filter. Placed after it, the population was
    // "non-finite components examined" and the rate came out 90.8% -- which
    // sounds catastrophic and is unreadable, because it is a fraction of the
    // NaNs rather than of the bank. The question is what share of the CONSTANTS
    // we overwrite, so every component examined is an opportunity.
    {
      const uint32_t dd = first_reg * 4 + i;
      const bool pub = (g_have[dd >> 5] & (1u << (dd & 31))) != 0;
      // Fires only when we ACTUALLY substitute, so the census tracks the
      // guard rather than the opportunity -- with strict on it should fall to
      // zero, and a non-zero reading would mean the switch is not reaching
      // here.
      const bool would_repair =
          NonFinite(cur) && !pub && (cur & 0x007FFFFFu) != 0 &&
          !mx::gpu::guard::Strict(mx::gpu::guard::Guard::kConstantNanToZero);
      mx::gpu::guard::Note(mx::gpu::guard::Guard::kConstantNanToZero,
                           would_repair);
      // The backdrop block on its own: guest c392..c395 = xe_c[136..139].
      // Population is every component of those four constants examined, so the
      // row reads "of the backdrop constants we looked at, how many were NaN we
      // repaired" -- not diluted by the other 252 registers.
      const uint32_t guest_const = dd >> 2;
      if (guest_const >= 392 && guest_const < 396)
        mx::gpu::guard::Note(mx::gpu::guard::Guard::kConstantNanBackdrop,
                             would_repair);
    }
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
    // THE SUBSTITUTION IS GONE, 2026-08-27. The NaN stays in the bank and
    // reaches the shader. The measurement above stays, so the population is
    // still known -- the same move made for FillMaterialGate.
    //
    // Its justification was the comment two screens up: c392..c395 (xe_c[136..
    // 139]) NaN for a bounded startup prefix, "which is why the legal, loading
    // and start screens have no background: a NaN interpolator saturates the
    // backdrop draw to white". Both halves of that are now falsified.
    //
    //   THE TIMING. Censused on its own, that block reads 0/32 at the first
    //   report and 540360/2510272 (21.5%) at the last. It does not freeze, it
    //   CLIMBS -- starting clean and becoming NaN all run. Not a prefix.
    //
    //   THE SYMPTOM. Run with hle_strict=8, i.e. every NaN repair suppressed
    //   and 1.76M NaNs reaching the shaders: the logo and intro are FINE, and a
    //   level is unchanged. The white backdrop this was built to prevent does
    //   not come back. Whatever caused it was fixed elsewhere, and this has
    //   been masking NaNs ever since.
    //
    // So it was a workaround for a defect that no longer exists, and its cost
    // is that a genuine NaN -- ours or the guest's -- is silently turned into a
    // zero, which is indistinguishable from a constant the guest meant to be
    // zero. That is the exact shape docs/strict_mode.md exists to remove.
    //
    // If a NaN ever does matter again it will now be VISIBLE, and the census
    // row says how many there are. That is the trade, taken deliberately.
    //
    // g_zeroed is REPURPOSED rather than deleted: it now counts NaNs LEFT IN
    // PLACE, which is the same population it used to count substituting. A
    // counter whose increment site disappears prints a permanent zero that
    // reads as a measurement -- the trap g_psFromDeviceRecord set earlier
    // today. Its log line is reworded to match.
    if (!published && (cur & 0x007FFFFFu) != 0) ++g_zeroed;
  }

  // SECOND PASS: a component our sources left at a FINITE ZERO, which PM4
  // published a non-zero value for. The pass above cannot reach these -- it
  // opens with `if (!NonFinite(cur)) continue;` and a zero is finite -- so a
  // constant whose ONLY publisher is Type-0 PM4 stayed 0.0 forever.
  //
  // `SHADER_CONSTANT_340_X(0x4550)` is written 330 times across the pm4 dumps
  // (182 x cnt=16 -> guest c340-343, 148 x cnt=32 -> c340-347) = PIXEL c84-c87,
  // and the menu rider's material gates a lighting term on c85.w:
  //
  //     saturate(tex1.x + c85.w - 1)     w=1 -> tex1.x ;  w=0 -> 0
  //
  // Guards keep this away from the retired `hle_sanitize_constants`: exact
  // zeros only, only where PM4 genuinely published, never substituting a zero
  // or a non-finite. Callers apply the shader load tables AFTER this, so those
  // still override it.
  //
  // MEASURED CONSEQUENCE, 2026-08-26 (mx_1367): 6,705,127 substitutions in a
  // 1020-frame menu run, 1.87% of the bank dwords rebuilt. It did NOT brighten
  // the scene, and the menu developed a new visual fault. So on this title the
  // population it reaches is dominated by zeros the guest MEANT, and the
  // residual risk documented below is not residual -- it is the common case.
  // Kept at the user's instruction; treat a non-zero `filled_zero` as a warning
  // rather than a repair until that fault is understood.
  //
  // Both banks now, because it no longer mutates. The histogram is why it must
  // not: with substitution on, the vertex top was With it on everywhere the
  // top of the fill was c175/c176/c177 and a run at c155, c161, c167, c173,
  // c179, c185 -- STRIDE 6, counts decaying smoothly -- a matrix palette being
  // filled entry by entry with end-of-frame values. Corrupted skinning is
  // exactly what the menu "trying to do a shader it can't" looked like. 207
  // distinct constants, so this is not a range to narrow; the frame-global file
  // is simply the wrong authority for a mid-frame VERTEX draw.
  for (uint32_t i = 0; count_finite_zeros && i < regs * 4; ++i) {
    if (bank[i] != 0) continue;
    const uint32_t d = first_reg * 4 + i;
    if (!(g_have[d >> 5] & (1u << (d & 31)))) continue;
    const uint32_t v = g_file[d];
    if (v == 0 || NonFinite(v)) continue;
    // DRY RUN. This used to assign `bank[i] = v`, and that was wrong in BOTH
    // banks: vertex, where it sprayed a stride-6 matrix palette (c155, c161,
    // c167 ...) with end-of-frame values and tore the geometry apart; and
    // pixel, where 84 constants including the runs ps c68-c71 and ps c231-c236
    // still produced flashing and no brightening.
    //
    // The cause is that `g_file` is FRAME-GLOBAL last-write-wins: it has no
    // notion of when in the frame a value was written, so a mid-frame draw gets
    // the frame's final constants. Harmless for NaN repair, which is rare and
    // startup-bounded; destructive the moment it reaches an array.
    //
    // Kept as a COUNT for the bank at large, because the question it answers is
    // still open -- which constants are published only by Type-0 PM4 and left at
    // zero by our two modelled sources.
    ++g_filledZero;
    ++g_filledByConst[d >> 2];
    g_wouldFillVal[d] = v;

    // The substitution that used to live here has MOVED to FillMaterialGate,
    // which runs after the shader load table instead of before it. Doing it
    // here was wrong for a reason this file already documents two screens up:
    // "Callers apply the shader load tables AFTER this, so those still override
    // it." Measured 2026-08-26 (mx_1447): 368,313 substitutions fired and the
    // terrain did not change, because the terrain shader's own load table wrote
    // c85 straight back to zero -- while materials with NO load-table entry in
    // that window kept the value, which is why the BIKE AND RIDER changed
    // appearance and the ground did not. The fix landed on exactly the shaders
    // it was not aimed at.
    //
    // Kept below for the record: the window and the proof.
    //
    // The blanket version failed for a specific reason: g_file is FRAME-GLOBAL
    // last-write-wins, so a mid-frame draw gets the frame's FINAL value. That
    // is destructive the moment it reaches an ARRAY -- the vertex bank's
    // stride-6 matrix palette (c155, c161, c167 ...) tore the geometry apart.
    // The window below is not an array. It is a four-constant material block
    // that PM4 publishes as a unit: SHADER_CONSTANT_340_X (0x4550) is written
    // 330 times across the pm4 dumps, 182 x cnt=16 -> guest c340-343 and
    // 148 x cnt=32 -> c340-347, which is PIXEL c84-c87.
    //
    // PROVEN, capture3.rdc draw 19725 (terrain, 1741 indices), pixel (640,640),
    // full PS trace:
    //
    //   193: add r7.x, r1.w, xe_c[85].w   0.774069 + 0         = 0.774069
    //   204: add r6.xy, r7.ywyy, r7.xzxx  0.774069 + -0.018824 = 0.755246
    //   205: add_sat r0.x, r6.x, c255.w   saturate(0.755246 - 0.8) = 0.0
    //   209: mul r1.xyz, r0.xxxx, r1.xyzx 0 x (0.070, 0.034, -0.014) = 0
    //
    // c85.w = 1 makes that saturate(1.755 - 0.8) = 0.955 and the terrain
    // diffuse survives. It misses the threshold by 0.045 and the constant is
    // worth exactly 1.0. With the diffuse zeroed all that reaches the output is
    // the ambient interpolator 0.00048 x the gain c43 = 15.18 = 0.034, which is
    // the flat dark grey on screen. Everything else in that draw is correct:
    // world Y 611.4 is real terrain height, xe_tex1 -> 0.113 and xe_tex2 ->
    // 0.848 sample fine, and c86/c87 (the sand tint) are populated. c85 is the
    // only zero among populated neighbours -- unwritten, not wrong.
    //
    // PIXEL BANK ONLY. The vertex bank is where the palette damage happened and
    // nothing here needs it.
    //
    // The frame-global risk is not gone, it is bounded: within this window the
    // worst case is a material gated OFF this frame being lit as though it were
    // ON. That is visible and reversible; a sprayed matrix palette was neither.
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
  // Runs AFTER the shader's load table, and never overwrites it. Two guards,
  // and both matter:
  //
  //   load_written[d]  the shader published this dword itself. Its value is
  //                    authoritative even when it is zero, so we do not touch
  //                    it. This is the guard that was missing when the fill
  //                    lived inside OverlayNonFinite: there it ran BEFORE the
  //                    load table and was simply overwritten for terrain, while
  //                    surviving on materials the table did not cover.
  //   bank[d] != 0     something already supplied a value. We only ever fill a
  //                    hole, never replace.
  //
  // The window is pixel c84..c87 -- guest c340..c343, the material block PM4
  // publishes as a unit via SHADER_CONSTANT_340_X (written 330 times across the
  // pm4 dumps). It is NOT an array, which is why the frame-global staleness of
  // g_file that tore the vertex matrix palette apart does not apply here.
  if (!bank || !load_written) return 0;
  uint32_t filled = 0;
  std::lock_guard<std::mutex> lk(g_mu);
  // WHAT DOES PM4 ACTUALLY PUBLISH HERE? Never checked, and the whole theory
  // rests on it: "c85.w should be 1" came from reading the shader arithmetic
  // backwards, not from observing a published value. If PM4's own c85.w is 0 or
  // unpublished then this fill can never set it, ordering or no ordering -- the
  // loop skips `v == 0` and skips unpublished -- and every run so far is
  // consistent with that. One line, once, so the assumption is either confirmed
  // or killed instead of being reasoned about again.
  {
    // SAMPLED LATE, NOT ONCE. The first cut fired on the first call to this
    // function and reported the whole window `unpub:0` -- which is true at
    // startup and says nothing, because PM4 has not published anything yet.
    // The guard census showed 209,629 fills out of 2,663,216 window dwords in
    // the same run, so the window IS published later and the snapshot was
    // simply taken before it. A one-shot probe on a value that arrives over
    // time reports the arrival order, not the value.
    //
    // Now: report the first time anything in the window is actually published,
    // and again every 200k calls so a later change is visible too.
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
      // SUBSTITUTION REMOVED 2026-08-26. Not because it "did not work" -- the
      // measurement retired the SOURCE. Run 1451, sampled at first publication
      // rather than at startup:
      //
      //   ps c85 = [ unpub:0   2.074e-42   unpub:0   unpub:0 ]
      //
      // c85.w -- the component the terrain shader gates on -- is UNPUBLISHED.
      // PM4 never carries it, at any point in the frame, so no amount of
      // reordering against the shader load table could ever have supplied it.
      // Three changes to delivery were all downstream of a source that does not
      // hold the value.
      //
      // Worse, the ONE dword PM4 does publish in this window is a denormal
      // whose bit pattern is ~1481 -- an INTEGER read as a float. That is not a
      // shader constant; the Type-0 capture is recording a register that is not
      // an ALU constant. With the substitution live it injected that denormal
      // into c85.y on ~94,000 draws a run (94265/2197648 in the guard census).
      // An inventing guard firing 94k times with junk is the exact shape
      // docs/strict_mode.md was written about, committed two hours after
      // writing it.
      //
      // The Note() above STAYS: the window keeps reporting, so if PM4 ever does
      // publish here the census says so without anyone re-deriving this. Do not
      // restore the assignment without first showing c85.w published with a
      // plausible value.
      if (!eligible) continue;
      ++filled;
      ++g_materialGateFilled;
    }
  }
  return filled;
}

void VertexTintStats(uint64_t& filled, uint64_t& applied, uint64_t& denormal,
                     uint64_t& unpublished) {
  std::lock_guard<std::mutex> lk(g_mu);
  filled = g_vertexTintFilled;
  applied = g_vertexTintApplied;
  denormal = g_vertexTintDenormal;
  unpublished = g_vertexTintUnpub;
}

uint32_t FillVertexTint(uint32_t* bank, uint32_t bank_regs,
                        const uint8_t* load_written) {
  // The one substitution from g_file that survives its own history, and the
  // history is the design. Everything above this function that tried to fill
  // finite zeros from PM4 was reverted, twice, for the same two reasons:
  //
  //   1. it reached ARRAYS. g_file is frame-global last-write-wins, so a
  //      mid-frame draw reading a matrix palette gets the frame's FINAL rows.
  //      Fixed here by a one-register window that cannot be an array.
  //   2. it injected values that were not shader constants. FillMaterialGate
  //      shipped a 2.074e-42 denormal into ps c85.y on ~94,000 draws a run
  //      before anyone looked at what was being written.
  //
  // (2) is what the validation below exists for, and it is deliberately
  // WHOLE-VECTOR. A per-component guard is worse than none here: measured over
  // one run, PM4 publishes vs c32 as (1,1,1,1) on 12 samples and
  // (1, 2.074e-42, 1, 1) on 32. Skipping only the bad component would leave
  // .y at zero and light the logo MAGENTA -- a plausible-looking wrong answer,
  // which is the exact failure mode docs/strict_mode.md is about. Rejecting the
  // whole float4 leaves it black instead: unchanged, and still obviously
  // broken.
  //
  // So the expected first result is a PARTIAL fix -- the tint appearing on the
  // ~27% of samples that validate. If it never fires, the source is junk and
  // this comes straight back out; if it fires and flickers, the denormal in .y
  // is the next thing to chase and not this window.
  //
  // Runs AFTER the shader's own literal overlay, never over it. Same ordering
  // lesson FillMaterialGate paid for: placed before the table, a fill is simply
  // overwritten for every shader whose table covers the window, and survives
  // only on the shaders it was not aimed at.
  if (!bank || !load_written) return 0;
  uint32_t filled = 0;
  std::lock_guard<std::mutex> lk(g_mu);
  for (uint32_t c = kVertexTintFirstConst; c < kVertexTintEndConst; ++c) {
    if (c >= bank_regs) continue;
    // VALIDATE THE WHOLE FLOAT4 FIRST, decide second.
    bool valid = true, any_nonzero = false, denormal = false, unpub = false;
    for (uint32_t comp = 0; comp < 4; ++comp) {
      const uint32_t d = c * 4 + comp;
      if (!(g_have[d >> 5] & (1u << (d & 31)))) { unpub = true; valid = false; break; }
      const uint32_t v = g_file[d];
      if (NonFinite(v)) { valid = false; break; }
      if (Denormal(v)) { denormal = true; valid = false; break; }
      if (v != 0) any_nonzero = true;
    }
    // An all-zero publish is not worth substituting -- it is what the bank
    // already holds -- but it is not a REJECTION either, so it is counted
    // nowhere rather than being folded into one of the failure rows.
    if (valid && !any_nonzero) valid = false;
    if (denormal) ++g_vertexTintDenormal;
    else if (unpub) ++g_vertexTintUnpub;
    else if (valid) ++g_vertexTintApplied;
    for (uint32_t comp = 0; comp < 4; ++comp) {
      const uint32_t b = c * 4 + comp;
      // Every dword is an opportunity whether or not the vector validated, so
      // the census denominator is structural rather than a count of the times
      // we happened to agree with ourselves.
      const bool fired = valid && !load_written[b] && bank[b] == 0;
      mx::gpu::guard::Note(mx::gpu::guard::Guard::kVertexTintFill, fired);
      // Switchable, unlike the other fills in this file: turning it off leaves
      // the tint at the black the guest bank already holds, which is exactly
      // the pre-change behaviour. That makes the strict bit a one-run A/B on
      // whether this is what changed the screen, with no rebuild.
      if (!fired || mx::gpu::guard::Strict(mx::gpu::guard::Guard::kVertexTintFill))
        continue;
      bank[b] = g_file[c * 4 + comp];
      ++filled;
      ++g_vertexTintFilled;
    }
  }
  return filled;
}

// The zero-fill population, worst first. `guest` is the ALU constant index;
// subtract 256 for the pixel bank's xe_c[] numbering.
// What we WOULD have filled a given constant with, as floats.
//
// Read it with the control in mind. c252..c255 are the screen-space scale/bias
// the D3D9 shader load table is measured to carry -- values like (0.5, -0.5, 0,
// 0) and (0, 1, 0.5, -0.5). If those come back looking like that, the constant
// file is sane and a value read for any other register can be trusted. If they
// come back as garbage, the file is stale or misindexed and NOTHING here should
// be acted on, least of all a blanket substitution.
std::string WouldFillValues(const uint32_t* consts, size_t n) {
  std::lock_guard<std::mutex> lk(g_mu);
  std::string out;
  char one[128];
  for (size_t k = 0; k < n; ++k) {
    const uint32_t c = consts[k];
    if (c >= kAluConstants) continue;
    float f[4];
    bool any = false;
    for (uint32_t i = 0; i < 4; ++i) {
      const uint32_t bits = g_wouldFillVal[c * 4 + i];
      std::memcpy(&f[i], &bits, 4);
      if (bits) any = true;
    }
    if (c >= 256)
      std::snprintf(one, sizeof(one), " c%u(ps c%u)=%s(%.4g,%.4g,%.4g,%.4g)", c,
                    c - 256, any ? "" : "NEVER ", f[0], f[1], f[2], f[3]);
    else
      std::snprintf(one, sizeof(one), " c%u(vs)=%s(%.4g,%.4g,%.4g,%.4g)", c,
                    any ? "" : "NEVER ", f[0], f[1], f[2], f[3]);
    out += one;
  }
  return out;
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

    // PM4_REG_RMW (xenos.h:1571) — read-modify-write of ONE register. Body is
    // three dwords: (rmw_info, and-operand, or-operand), with the target
    // register in the low bits of rmw_info.
    //
    // This used to treat rmw_info as a base and the two operands as a RUN of
    // register values, writing the and-mask to reg and the or-mask to reg+1.
    // The comment that stood here called the case dead — "frame 600's Type3
    // histogram has no 0x21" — and that was wrong: it fires 23 times in a menu
    // run (22x 0x1841, 1x 0x1E4E), in BOTH native and plugin mode, because this
    // parser does not sit behind the D3D9 passthrough. A claim that a branch
    // cannot fire is worth as little here as a counter that cannot fire.
    //
    // The damage was nil, which is why it survived: every register it reaches
    // is scanout, not 3D. 0x1841 D1GRPH_CONTROL, 0x1842
    // D1GRPH_LUT_10BIT_BYPASS_CNTL, 0x1E4E XDVO_FORCE_OUTPUT_CNTL, 0x1E4F
    // XDVO_FORCE_DATA (register_table.inc). Nothing downstream reads any of
    // them — we do not emulate scanout — so this fix corrects the register file
    // and should change no pixel. If a frame moves, something reads a display
    // register and that is the finding, not this.
    case 0x21: {
      if (pkt.body.size() < 3) break;
      const uint32_t rmw_info = pkt.body[0];
      const uint32_t and_operand = pkt.body[1];
      const uint32_t or_operand = pkt.body[2];

      // Encoding verified against the Xenia source at
      // xenia/gpu/pm4_command_processor_implement.h:925 (the tree at
      // C:\Users\VM\Desktop\xenia-edge-edge, which carries implementations the
      // SDK headers do not):
      //
      //   reg = rmw_info & 0x1FFF
      //   bit 31 set -> AND operand is a register index, else an immediate
      //   bit 30 set -> OR  operand is a register index, else an immediate
      //
      // The address field really is 13 bits, and that is NOT an array clamp:
      // Xenia's own register file is 0x5003 entries, so it could address more
      // and deliberately does not. An earlier version of this masked 0xFFFF on
      // the reasoning that the register table runs to 0x8D07; that reasoning
      // was wrong, and the two masks happen to agree on all observed traffic
      // (0x1841, 0x1E4E) which is exactly why it would not have shown up.
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
