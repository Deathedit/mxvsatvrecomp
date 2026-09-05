// The periodic reports -- what the D3D9 layer prints about itself.
//
// Split verbatim out of hooks_d3d9.cpp. Every function here reads counters and
// formats them; none of them decides anything, and nothing on a draw path calls
// into this file.
//
// THIS SPLIT WAS NOT POSSIBLE BEFORE THE COUNTERS WERE GROUPED. Measured on the
// same block: it exports four names, all of which were already published, so by
// that count it looked free. But it IMPORTED FIFTY-THREE names defined
// elsewhere in the layer, and moving it then would have published fifty-three
// mutable globals -- the exact condition hooks_d3d9_internal.h exists to
// prevent, and the state its own comment blames for the original file being
// hard to reason about.
//
// Nine structs later it imports five, and all five are functions or constants,
// which a header carries as ordinary. That is the whole reason the grouping
// commits came first, and why they are separate commits: a rename the compiler
// proves complete, then a move whose diff is empty.

#include "hooks/hook_common.h"

#include <rex/cvar.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "gpu/guard_census.h"
#include "gpu/health.h"
#include "gpu/d3d9_draw.h"
#include "gpu/d3d9_layout.h"
#include "gpu/hle_types.h"
#include "gpu/shader_ucode.h"
#include "hooks/hooks_d3d9_census.h"
#include "hooks/hooks_d3d9_shared.h"

// hle_diag is defined at global scope in graphics_system.cpp, so it is declared
// here at global scope.
REXCVAR_DECLARE(bool, hle_diag);

namespace mx::hooks::d3d9 {

// d3d9_diag_row_heartbeat is NOT: hooks_d3d9.cpp defines it inside this
// namespace, so REXCVAR_DECLARE has to sit inside it too or it names a
// different symbol and fails at link rather than at compile. hooks_d3d9.cpp
// already carries a note about the same trap with an anonymous namespace.
REXCVAR_DECLARE(int32_t, d3d9_diag_row_heartbeat);

void ReportCoverage(uint8_t* base) {
  const auto& st = DeviceState();
  if (g_drawFit.checked == 0) {
    REXLOG_INFO("d3d9: hle -- no draws scored");
    return;
  }
  REXLOG_INFO("d3d9: hle -- {} of {} draws fully described ({}%)",
              g_drawFit.complete, g_drawFit.checked,
              (g_drawFit.complete * 100) / g_drawFit.checked);
  for (uint32_t g = 0; g < kDrawGapCount; ++g) {
    if (g_drawFit.gaps[g]) {
      REXLOG_INFO("d3d9: hle   missing: {:<28} x{}", DrawGapName(g), g_drawFit.gaps[g]);
    }
  }
  //-------------------------------------------------------------------------
  // Stage 2: what was actually built, and why the rest was not. Every skip is
  // named -- a bare total cannot separate "the decoder refuses this format" from
  // "this stream is not indexed the way we model it".
  //-------------------------------------------------------------------------
  {
    const uint64_t built = mx::hle::HleBuiltCount();
    const uint64_t* counts = mx::hle::HleSkipCounts();
    uint64_t attempted = built;
    for (uint32_t i = 1; i < uint32_t(mx::hle::HleSkip::kCount); ++i)
      attempted += counts[i];
    if (attempted) {
      REXLOG_INFO("d3d9: hle-render -- {} of {} draws built ({}%)", built,
                  attempted, (built * 100) / attempted);
      for (uint32_t i = 1; i < uint32_t(mx::hle::HleSkip::kCount); ++i) {
        if (!counts[i]) continue;
        REXLOG_INFO("d3d9: hle-render   skipped: {:<34} x{}",
                    mx::hle::HleSkipName(mx::hle::HleSkip(i)), counts[i]);
      }
      std::string prims;
      for (uint32_t i = 0; i < 64; ++i) {
        if (!g_drawOutcome.badPrimType[i]) continue;
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%u:%llu ", i,
                      (unsigned long long)g_drawOutcome.badPrimType[i]);
        prims += buf;
      }
      if (!prims.empty())
        REXLOG_INFO("d3d9: hle-render   refused prim types (type:count) {}",
                    prims);
    }
    mx::hle::ReportHleTransform();
  }

  ReportPatchRule();

  //-------------------------------------------------------------------------
  // Stage 0 verdict.
  //-------------------------------------------------------------------------
  for (uint32_t s = 0; s < mx::hle::kMaxStreams; ++s) {
    const uint64_t fit = g_perStream.vbFits[s], fail = g_perStream.vbFails[s];
    if (!fit && !fail) continue;
    REXLOG_INFO(
        "d3d9: stage0  stream {}: holds the range {}/{} | mean draws since "
        "bind: fits {} fails {} (worst {})",
        s, fit, fit + fail, fit ? g_perStream.bindAgeFitSum[s] / fit : 0,
        fail ? g_perStream.bindAgeFailSum[s] / fail : 0, g_perStream.bindAgeFailMax[s]);
  }
  for (uint32_t s = 0; s < mx::hle::kMaxStreams; ++s) {
    if (!g_perStream.fileAgree[s] && !g_perStream.fileDiffer[s]) continue;
    REXLOG_INFO(
        "d3d9: stage0  stream {}: device fetch constant vs our snapshot -- "
        "same {} differ {}, and the device's size explains {} of the failures",
        s, g_perStream.fileAgree[s], g_perStream.fileDiffer[s], g_perStream.fileRescues[s]);
  }
  REXLOG_INFO(
      "d3d9: stage0  indexed draws, by real max index: holds {}/{} (unread {})",
      g_drawFit.idxRangeFits, g_drawFit.idxRangeFits + g_drawFit.idxRangeFails, g_drawFit.idxRangeUnread);

  // Offsets that held the just-bound value on every one of the samples. One
  // surviving pair is the fetch constant file; none means D3D9 does not keep
  // the value verbatim and the snapshot is the only source available.
  if (g_fcScan.primed) {
    uint32_t n0 = 0, n1 = 0;
    std::string o0, o1;
    for (uint32_t i = 0; i < kDeviceScanDwords; ++i) {
      char buf[16];
      if (g_fcScan.cand0[i]) {
        ++n0;
        if (n0 <= 8) { std::snprintf(buf, sizeof(buf), "0x%X ", i * 4); o0 += buf; }
      }
      if (g_fcScan.cand1[i]) {
        ++n1;
        if (n1 <= 8) { std::snprintf(buf, sizeof(buf), "0x%X ", i * 4); o1 += buf; }
      }
    }
    REXLOG_INFO(
        "d3d9: stage0  fetch constant file after {} samples (device readable to "
        "0x{:X}): dword0 offsets={} [{}] dword1 offsets={} [{}]",
        g_fcScan.samples, g_fcScan.reached, n0, o0, n1, o1);
  } else {
    REXLOG_INFO("d3d9: stage0  fetch constant file: never sampled");
  }

  REXLOG_INFO(
      "d3d9: hle   stride exact={} padded={} TOO SMALL={} (too small means the "
      "layout decode is wrong)",
      g_drawFit.strideOk, g_drawFit.strideMismatch, g_drawFit.strideTooSmall);
  REXLOG_INFO(
      "d3d9: hle   buffer holds the range: vb {}/{} ib {}/{} (denominator is "
      "draws checked for that buffer)",
      g_drawFit.vbFits, g_drawFit.vbFits + g_drawFit.vbTooSmall, g_drawFit.ibFits, g_drawFit.ibFits + g_drawFit.ibTooSmall);
  REXLOG_INFO(
      "d3d9: hle   vs=0x{:08X} ps=0x{:08X} ib=0x{:08X} ({} bit) vp={}x{} "
      "distinct devices={}",
      st.vertex_shader, st.pixel_shader, st.index.address,
      st.index.is_32bit ? 32 : 16, st.viewport.width, st.viewport.height,
      st.device_count);
  for (uint32_t i = 0; i < st.device_count; ++i) {
    std::string who;
    for (uint32_t e = 0; e < mx::hle::kEntryPointCount; ++e) {
      if (!(st.device_call_mask[i] & (1u << e))) continue;
      if (!who.empty()) who += " ";
      who += mx::hle::EntryPointName(e);
    }
    REXLOG_INFO("d3d9: hle   device 0x{:08X} x{} calls from: {}",
                st.device_ptr[i], st.device_calls[i], who);
  }
  for (uint32_t s = 0; s < mx::hle::kMaxStreams; ++s) {
    const auto& b = st.stream[s];
    if (!b.seen) continue;
    REXLOG_INFO(
        "d3d9: hle   stream {}: addr=0x{:08X} size={}B endian={} offset={} "
        "stride={}{}",
        s, b.address, b.size_bytes, b.endian, b.offset_bytes, b.stride,
        b.bound ? "" : " (unbound)");
  }
}

// The fully-resolved draw, written out for the first few of each kind so the
// description can be read and checked by eye rather than only counted. Goes to
// the non-rotating dump: these happen at load and the rotating log has already
// lost two probes this effort.
constexpr uint64_t kMaxHleDumped = 12;

void DumpHleDraw(bool indexed, uint64_t n, uint32_t prim, int32_t base_vertex,
                 uint32_t start, uint32_t count) {
  if (n > kMaxHleDumped) return;
  const auto& st = DeviceState();
  auto& f = DeclFile();

  f << "\nHleDraw " << (indexed ? "indexed" : "non-indexed") << " #" << n
    << " prim=" << prim << (indexed ? " base_vertex=" : " start_vertex=")
    << base_vertex;
  if (indexed) f << " start_index=" << start;
  f << (indexed ? " index_count=" : " vertex_count=") << count << "\n";

  const int id = st.current_decl;
  if (id < 0) {
    f << "  declaration: NONE\n";
  } else if (!g_declTable.layoutOk[id]) {
    f << "  declaration id " << id << ": DOES NOT DECODE ("
      << mx::hle::LayoutErrorText(g_declTable.layoutErr[id].reason) << ")\n";
  } else {
    const auto& layout = g_declTable.layout[id];
    f << "  declaration id " << id << ", " << layout.elements.size()
      << " element(s):\n";
    for (const auto& e : layout.elements) {
      f << "    " << e.semantic_name << e.semantic_index << " s" << e.stream
        << " off=" << e.offset << " size=" << e.size_bytes
        << " dxgi=" << static_cast<int>(e.format);
      if (e.unpack == mx::hle::Unpack::kSnorm2_10_10_10)
        f << " (shader unpacks snorm 2_10_10_10)";
      f << "\n";
    }
    for (uint32_t s = 0; s <= layout.max_stream; ++s) {
      const auto& b = st.stream[s];
      f << "    stream " << s << ": ";
      if (!b.seen) {
        f << "NEVER SET\n";
        continue;
      }
      f << "addr=0x" << std::hex << b.address << std::dec
        << " size=" << b.size_bytes << " offset=" << b.offset_bytes
        << " stride=" << b.stride << " (layout needs " << layout.min_stride[s]
        << ")" << (b.bound ? "" : " UNBOUND") << "\n";
    }
  }

  if (indexed) {
    f << "  index buffer: ";
    if (!st.index.seen || !st.index.bound) {
      f << "NONE\n";
    } else {
      f << "addr=0x" << std::hex << st.index.address << std::dec
        << " size=" << st.index.size_bytes << " "
        << (st.index.is_32bit ? 32 : 16) << "-bit\n";
    }
  }

  f << "  vs=0x" << std::hex << st.vertex_shader << " ps=0x" << st.pixel_shader
    << std::dec << (st.vs_seen ? "" : " (vs NEVER SET)")
    << (st.ps_seen ? "" : " (ps NEVER SET)") << "\n";
  f << "  viewport: ";
  if (!st.viewport.seen) {
    f << "NEVER SET\n";
  } else {
    f << st.viewport.x << "," << st.viewport.y << " " << st.viewport.width
      << "x" << st.viewport.height << " z=[" << st.viewport.min_z << ","
      << st.viewport.max_z << "]\n";
  }
  f << "  render state:";
  for (uint32_t r = 0; r < mx::hle::kRenderStateCount; ++r) {
    f << " " << mx::hle::RenderStateName(r) << "=";
    if (st.render_state.Seen(r)) {
      f << st.render_state.value[r];
    } else {
      f << "unset";
    }
  }
  f << "\n";
  f.flush();
}

// The two histograms the round exists to produce.
void ReportDeclHistogram() {
  uint64_t with = 0, without = 0;
  for (int i = 0; i < g_declTable.count; ++i) {
    (g_declTable.hasColour[i] ? with : without) += g_declTable.draws[i];
  }
  REXLOG_INFO(
      "d3d9: decl-draws -- {} declarations known; COLOUR={} NO-COLOUR={} "
      "unattributed={} patch_calls={}",
      g_declTable.count, with, without, g_declCensus.drawsNoDecl, g_patchCalls);
  // The declaration now comes from device + 0x2ED8, per draw. These four say
  // whether that source is sound and how badly the old one lagged: `unknown`
  // must stay at 0 or the field is not what SetVertexDeclaration writes, and a
  // large `stale` is the 2508-calls-per-165000-draws problem, measured.
  REXLOG_INFO(
      "d3d9: decl-source -- from device+0x2ED8: null={} unknown={} [{}] | vs "
      "the patch hook: same={} stale={} | adopted {} refused {}",
      g_declCensus.deviceNull, g_declCensus.deviceUnknown,
      // WHAT IS STILL LOST, not what was merely unfamiliar. This checked
      // g_declCensus.deviceUnknown, which was right while an unknown pointer meant a
      // dropped draw; since those are adopted off the device, an unknown pointer
      // is a FIRST SIGHTING and costs nothing. The population that still loses
      // draws is the adoption REFUSALS.
      mx::gpu::health::Tag(mx::gpu::health::Zero(
          "decl.unknown_ptr", g_declCensus.adoptRefused,
          with + without + g_declCensus.drawsNoDecl)),
      g_declCensus.agree, g_declCensus.disagree, g_declCensus.adopted, g_declCensus.adoptRefused);
  // WHICH pointers, when there are any. A count cannot name the draw, and the
  // shape of the answer decides the next step: one address recurring every frame
  // is a specific draw to go and find, while hundreds of distinct addresses is a
  // lifetime or ordering problem instead.
  if (g_unknownDecls.distinct) {
    std::string u;
    for (uint32_t i = 0; i < g_unknownDecls.distinct; ++i)
      u += fmt::format(" 0x{:08X}x{}", g_unknownDecls.ptr[i], g_unknownDecls.draws[i]);
    REXLOG_INFO("d3d9: decl-unknown -- {} distinct pointer(s){}{}",
                g_unknownDecls.distinct, u,
                g_unknownDecls.overflow
                    ? fmt::format(" (+{} draws on pointers past the {} cap)",
                                  g_unknownDecls.overflow, kMaxUnknownDecls)
                    : "");
  }
  // LAYOUT DECODE, with its denominator. Three outcomes, never folded: clean,
  // decoded-with-elements-dropped, and refused outright. The middle one used to
  // BE the last one -- BuildInputLayout returned false on the first element it
  // could not describe, which nulled in.layout for every draw using that
  // declaration and dropped them all as kNoLayout, over elements the 36-byte
  // transcode never reads. table_full and reused must both read 0.
  {
    uint32_t clean = 0, partial = 0, refused = 0, dropped_elems = 0;
    for (int i = 0; i < g_declTable.count; ++i) {
      if (!g_declTable.layoutOk[i]) { ++refused; continue; }
      if (g_declTable.layoutErr[i].skipped) {
        ++partial;
        dropped_elems += g_declTable.layoutErr[i].skipped;
      } else {
        ++clean;
      }
    }
    // Three expectations, each stated where it is measured. A refused
    // declaration nulls in.layout for every draw that uses it; a full table does
    // the same past the cap; a reused address hands new geometry the previous
    // element list. None can be non-zero without draws being lost or decoded
    // wrong.
    namespace h = mx::gpu::health;
    const char* t_refused =
        h::Tag(h::Zero("decl.layout_refused", refused, uint64_t(g_declTable.count)));
    const char* t_full = h::Tag(h::Zero("decl.table_full", g_declTableFull,
                                        g_declCensus.created));
    const char* t_reuse = h::Tag(h::Zero("decl.addr_reused", g_declRebuilt,
                                         g_declCensus.created));
    REXLOG_INFO(
        "d3d9: decl-layout -- of {} declarations: clean={} partial={} "
        "refused={} [{}] | elements dropped={} | table_full={} [{}] "
        "addr_reused={} [{}]",
        g_declTable.count, clean, partial, refused, t_refused, dropped_elems,
        g_declTableFull, t_full, g_declRebuilt, t_reuse);
  }
  // One row per declaration, held back while no NEW declaration has appeared:
  // 17,987 rows over 783 reports in one run, and the only thing moving between
  // prints is each row's draw count. The two summary lines above always print
  // and already carry g_declTable.count.
  static uint64_t s_lastDecls = 0;
  static uint32_t s_sinceDecls = 0;
  if (!RowDumpDue(uint64_t(g_declTable.count), s_lastDecls, s_sinceDecls)) {
    REXLOG_INFO("d3d9: decl-draws   rows held -- declaration set unchanged at "
                "{} ({} report(s) so far; d3d9_diag_row_heartbeat={})",
                g_declTable.count, s_sinceDecls,
                REXCVAR_GET(d3d9_diag_row_heartbeat));
    return;
  }
  for (int i = 0; i < g_declTable.count; ++i) {
    REXLOG_INFO("d3d9: decl-draws   id={} ptr=0x{:08X} elems={} colour={} x{}",
                i, g_declTable.ptr[i], g_declTable.elems[i],
                g_declTable.hasColour[i] ? "yes" : "no", g_declTable.draws[i]);
  }
}

// All three draw entry points report through here so the counters are always
// read together. A 150s run reaches 5000-10000 transcoded draws, so a coarser
// cadence than 2500 reports nothing at all.
//
// DrawVerticesUP CALLER CENSUS. ~95 of the ~342 draws the guest submits each
// frame come through it, shared by about 30 engine functions -- UI, particles,
// the Bink composite -- and it was unhooked until 2026-08-07, so every earlier
// draw total excluded them. The LINK REGISTER names the call SITE, not just the
// function, so two draws from different points in one function stay
// distinguishable. BOUNDED, with the overflow counted rather than dropped.
namespace {

constexpr size_t kMaxUpCallers = 64;

struct UpCaller {
  uint32_t lr = 0;
  uint32_t kind = 0;
  uint64_t calls = 0;
  uint64_t verts = 0;
  uint32_t min_verts = 0xFFFFFFFFu;
  uint32_t max_verts = 0;
};

std::mutex g_upCallerMu;
std::array<UpCaller, kMaxUpCallers> g_upCallers{};
size_t g_upCallerCount = 0;
uint64_t g_upCallerOverflow = 0;

}  // namespace

void NoteUpDrawCaller(uint32_t lr, uint32_t verts, uint32_t kind) {
  std::lock_guard<std::mutex> lk(g_upCallerMu);
  for (size_t i = 0; i < g_upCallerCount; ++i) {
    if (g_upCallers[i].lr != lr || g_upCallers[i].kind != kind) continue;
    auto& c = g_upCallers[i];
    ++c.calls;
    c.verts += verts;
    if (verts < c.min_verts) c.min_verts = verts;
    if (verts > c.max_verts) c.max_verts = verts;
    return;
  }
  if (g_upCallerCount >= kMaxUpCallers) {
    ++g_upCallerOverflow;
    return;
  }
  auto& c = g_upCallers[g_upCallerCount++];
  c.lr = lr;
  c.kind = kind;
  c.calls = 1;
  c.verts = verts;
  c.min_verts = verts;
  c.max_verts = verts;
}

void ReportUpDrawCallers() {
  std::array<UpCaller, kMaxUpCallers> snap{};
  size_t n = 0;
  uint64_t overflow = 0;
  {
    std::lock_guard<std::mutex> lk(g_upCallerMu);
    snap = g_upCallers;
    n = g_upCallerCount;
    overflow = g_upCallerOverflow;
  }
  const std::string cap =
      overflow ? fmt::format(", {} DROPPED past the {}-site cap (the rows are "
                             "then not the whole set)",
                             overflow, kMaxUpCallers)
               : std::string();
  // A new call site moves `n`; a first overflow moves the other half. Either is
  // something unseen, so fold both into the population the heartbeat watches.
  static uint64_t s_last = 0;
  static uint32_t s_since = 0;
  if (!RowDumpDue((uint64_t(n) << 1) | (overflow ? 1u : 0u), s_last, s_since)) {
    REXLOG_INFO("d3d9: UP CALLERS {} distinct call sites{} -- unchanged, rows "
                "held ({} report(s) so far; d3d9_diag_row_heartbeat={})",
                n, cap, s_since, REXCVAR_GET(d3d9_diag_row_heartbeat));
    return;
  }
  std::sort(snap.begin(), snap.begin() + n,
            [](const UpCaller& a, const UpCaller& b) { return a.calls > b.calls; });
  std::string rows;
  for (size_t i = 0; i < n; ++i) {
    rows += fmt::format(" [{} lr0x{:08X} x{} verts{}..{} avg{}]",
                        snap[i].kind == 0   ? "IDX"
                        : snap[i].kind == 1 ? "VTX"
                                            : "UP ",
                        snap[i].lr,
                        snap[i].calls, snap[i].min_verts, snap[i].max_verts,
                        snap[i].calls ? snap[i].verts / snap[i].calls : 0);
  }
  REXLOG_INFO("d3d9: UP CALLERS {} distinct call sites{} --{}", n, cap,
              rows.empty() ? " (none)" : rows);
}

void ReportDrawCounts(uint8_t* base) {
  const uint64_t total = g_indexed_draws + g_draws + g_up_draws + g_indexed_up_draws;
  // Both gates, cheapest first. The draw floor keeps the clock read off the draw
  // path for all but every 2500th call; the clock is what actually bounds the
  // rate. Atomics because this runs on every draw hook and the guest draws from
  // more than one thread.
  if ((total % kDrawReportEvery) != 0) return;
  {
    using namespace std::chrono;
    static std::atomic<int64_t> s_lastMs{
        std::numeric_limits<int64_t>::min() / 2};
    const int64_t now_ms = duration_cast<milliseconds>(
                               steady_clock::now().time_since_epoch())
                               .count();
    int64_t last = s_lastMs.load(std::memory_order_relaxed);
    if (now_ms - last < kDrawReportPeriodMs) return;
    // Whoever wins the exchange prints; the losers return. Without this two
    // threads crossing the floor together both report, which is how a "census"
    // ends up with duplicate rows nobody can explain.
    if (!s_lastMs.compare_exchange_strong(last, now_ms,
                                          std::memory_order_relaxed))
      return;
  }
  REXLOG_INFO("d3d9: draws -- DrawIndexedVertices={} DrawVertices={} "
              "DrawVerticesUP={} DrawIndexedVerticesUP={} (skipped {}) total={}",
              g_indexed_draws, g_draws, g_up_draws, g_indexed_up_draws,
              g_indexed_up_skipped, total);
  ReportUpDrawCallers();
  // Stencil sizing. See the census at its definition for why the effective count
  // is not just the enable bit. Printed here rather than at first sight of each
  // config because sizing needs the TOTALS, and a first-sight line always
  // reports a count of one.
  {
    std::lock_guard<std::mutex> lk(g_stencil.mu);
    std::string modes;
    for (const auto& [mode, n] : g_stencil.edramModes) {
      const char* name = mode == 0   ? "NoOperation"
                         : mode == 4 ? "ColorDepth"
                         : mode == 5 ? "DepthOnly"
                         : mode == 6 ? "Copy"
                         : mode == 0xFFFFFFFFu ? "UNREADABLE"
                                               : "reserved";
      modes += fmt::format(" {}({})={}", name, mode, n);
    }
    REXLOG_INFO("d3d9: STENCIL census -- {} draws reached the read ({} could "
                "not); enable bit set {}, of which {} are in an edram_mode "
                "that honours it; {} distinct configs; edram_mode:{}",
                g_stencil.drawsSeen, g_stencil.drawsUnreadable, g_stencil.bitSet,
                g_stencil.effective, g_stencil.configs.size(), modes);
    // One line per distinct configuration, so the translation work is a
    // countable list rather than an impression. Held back while the config set
    // is unchanged -- the census line above already carries the count.
    static uint64_t s_lastCfgs = 0;
    static uint32_t s_sinceCfgs = 0;
    const bool dump_cfgs =
        RowDumpDue(g_stencil.configs.size(), s_lastCfgs, s_sinceCfgs);
    if (!dump_cfgs) {
      REXLOG_INFO("d3d9:   stencil cfg rows held -- config set unchanged at {} "
                  "({} report(s) so far; d3d9_diag_row_heartbeat={})",
                  g_stencil.configs.size(), s_sinceCfgs,
                  REXCVAR_GET(d3d9_diag_row_heartbeat));
    } else {
      for (const auto& [key, n] : g_stencil.configs) {
        const uint32_t dc_bits = key.first;
        const uint32_t rm = key.second;
        REXLOG_INFO("d3d9:   stencil cfg depthcontrol=0x{:08X} refmask=0x{:08X}"
                    " x{} -- func {} fail {} zpass {} zfail {} backface {}"
                    " (bf func {} fail {} zpass {} zfail {});"
                    " ref {} mask 0x{:02X} writemask 0x{:02X}",
                    dc_bits, rm, n, (dc_bits >> 8) & 7u, (dc_bits >> 11) & 7u,
                    (dc_bits >> 14) & 7u, (dc_bits >> 17) & 7u,
                    (dc_bits >> 7) & 1u, (dc_bits >> 20) & 7u,
                    (dc_bits >> 23) & 7u, (dc_bits >> 26) & 7u,
                    (dc_bits >> 29) & 7u, rm & 0xFFu, (rm >> 8) & 0xFFu,
                    (rm >> 16) & 0xFFu);
      }
    }
  }
  // DEPTH SURFACES BY EDRAM BASE. More than one owner on a base means two D3D12
  // depth textures -- and two independent stencil planes -- stand in for one
  // console allocation, so a mask written through one view is invisible through
  // the other.
  REXLOG_INFO("d3d9: DEPTH SURFACES BY EDRAM BASE (>1 owner means the stencil "
              "plane is split across textures the guest treats as one):{}",
              DepthSurfaceReport());

  // PHASE 1 PASS CONDITION.
  //
  // READ THIS BEFORE CONCLUDING THE COUNTS SHOULD BE EQUAL. They should not:
  // since the check moved to the CONSUMER the two count different populations by
  // construction, the census seeing every draw that reached the register read
  // and this one only those that reached AddGameDraw.
  //
  //   config KEY SET   must be IDENTICAL. A configuration present in the census
  //                    and absent here would be state Phase 2 can never act on.
  //   counts           must differ by exactly the draws that never reached the
  //                    renderer -- cross-check against FRAME DRAWS `guest`
  //                    minus `accepted`.
  //
  // The gap is PRINTED rather than left to be worked out, because a reader who
  // expects equality will otherwise read a correct result as a failure.
  {
    std::lock_guard<std::mutex> lk(g_stencil.plumbedMu);
    // Stated as a check rather than left to a reader diffing two key dumps by
    // eye. Only census-minus-plumbed is a defect: the reverse cannot happen,
    // since the consumer sees a subset.
    //
    // A GRACE PERIOD, because the two sides are not recorded at the same moment:
    // the census records a configuration when the guest programs it, the
    // consumer when a draw carrying it reaches the renderer. If the first draw
    // with a configuration is dropped and a later one is not, the sets differ
    // for a while and then agree.
    constexpr uint64_t kConfigGracePasses = 3;
    static std::map<std::pair<uint32_t, uint32_t>, uint64_t> s_firstSeen;
    static uint64_t s_configPass = 0;
    ++s_configPass;
    uint64_t census_keys_missing = 0;
    std::string missing;
    for (const auto& [key, n] : g_stencil.configs) {
      const auto it = s_firstSeen.emplace(key, s_configPass).first;
      if (g_stencil.plumbedConfigs.count(key)) continue;
      if (s_configPass - it->second < kConfigGracePasses) continue;
      ++census_keys_missing;
      // NAMED, not just counted. Both lines already print "12 distinct configs"
      // and "key set unchanged" while three of those twelve keys differ, so the
      // sets are the same SIZE and different MEMBERS -- precisely what a size
      // comparison cannot see.
      if (census_keys_missing <= 8)
        missing += fmt::format(" depthcontrol=0x{:08X}/refmask=0x{:08X}x{}",
                               key.first, key.second, n);
    }
    if (census_keys_missing) {
      REXLOG_WARN(
          "d3d9: STENCIL {} of {} census config key(s) NEVER reach the "
          "consumer -- state the renderer can never act on:{}{}",
          census_keys_missing, g_stencil.configs.size(), missing,
          census_keys_missing > 8 ? " ..." : "");
    }
    mx::gpu::health::Zero("stencil.config_keys_missing", census_keys_missing,
                          g_stencil.configs.size());
    // The KEY SET is the payload and it is add-only, so its size is a faithful
    // "a configuration you have not seen has reached the consumer". The counts
    // and the gaps stay on the line either way, because the gap is the number a
    // reader comes here for.
    static uint64_t s_lastPlumbed = 0;
    static uint32_t s_sincePlumbed = 0;
    const bool dump_keys =
        RowDumpDue(g_stencil.plumbedConfigs.size(), s_lastPlumbed, s_sincePlumbed);
    std::string cfgs;
    if (dump_keys) {
      for (const auto& [key, n] : g_stencil.plumbedConfigs)
        cfgs += fmt::format(" [{:08X}/{:08X} x{}]", key.first, key.second, n);
    } else {
      cfgs = fmt::format(" held, key set unchanged ({} report(s) so far; "
                         "d3d9_diag_row_heartbeat={})",
                         s_sincePlumbed,
                         REXCVAR_GET(d3d9_diag_row_heartbeat));
    }
    // Signed: the consumer cannot legitimately see MORE than the census, so a
    // negative gap is itself a finding rather than an impossible number.
    const int64_t seen_gap =
        int64_t(g_stencil.drawsSeen) - int64_t(g_stencil.plumbedSeen);
    const int64_t eff_gap =
        int64_t(g_stencil.effective) - int64_t(g_stencil.plumbedEffective);
    REXLOG_INFO("d3d9: STENCIL PLUMBED at the CONSUMER -- {} draws carried the "
                "fields ({} had an unreadable register [{}]), {} effective, {} "
                "distinct configs. Counts are EXPECTED to be lower than the "
                "census: {} draws and {} effective never reached the renderer "
                "(cross-check FRAME DRAWS guest-minus-accepted). The KEY SET "
                "is what must match:{}",
                g_stencil.plumbedSeen, g_stencil.plumbedUnreadable,
                // A draw that carried the fields but whose register could not
                // be read has stencil state we invented rather than observed.
                mx::gpu::health::Tag(mx::gpu::health::Zero(
                    "stencil.unreadable_reg", g_stencil.plumbedUnreadable,
                    g_stencil.plumbedSeen)),
                g_stencil.plumbedEffective,
                g_stencil.plumbedConfigs.size(), seen_gap, eff_gap,
                cfgs.empty() ? " none" : cfgs);
  }
  // The back-face register window. See NoteBackFaceWindow for why this is a
  // scan and not a read of one guessed offset.
  {
    std::lock_guard<std::mutex> lk(g_stencil.bfWindowMu);
    if (g_stencil.bfWindowDraws) {
      // Every distinct (offset, value) pair the scan has ever seen. Both map
      // levels are add-only, so this grows exactly when the window shows
      // something new -- which is the entire question the scan asks.
      uint64_t pairs = 0;
      for (const auto& [off, vals] : g_stencil.bfWindow) pairs += 1 + vals.size();
      static uint64_t s_lastBf = 0;
      static uint32_t s_sinceBf = 0;
      if (!RowDumpDue(pairs, s_lastBf, s_sinceBf)) {
        REXLOG_INFO("d3d9: BACKFACE STENCIL WINDOW -- {} two-sided draws "
                    "sampled, scan held, no new offset or value ({} report(s) "
                    "so far; d3d9_diag_row_heartbeat={})",
                    g_stencil.bfWindowDraws, s_sinceBf,
                    REXCVAR_GET(d3d9_diag_row_heartbeat));
      } else {
        std::string w;
        for (const auto& [off, vals] : g_stencil.bfWindow) {
          w += fmt::format(" [+{:04X}", off);
          // At most four values per offset: a register that takes many values is
          // not the one being looked for, and printing them all would bury the
          // one that does.
          uint32_t shown = 0;
          for (const auto& [v, n] : vals) {
            if (shown++ == 4) {
              w += fmt::format(" +{}more", vals.size() - 4);
              break;
            }
            w += fmt::format(" {:08X}x{}", v, n);
          }
          w += "]";
        }
        REXLOG_INFO("d3d9: BACKFACE STENCIL WINDOW -- {} two-sided draws sampled "
                    "(0x2900 is RB_STENCILREFMASK 0x210D; looking for 0x210E, "
                    "which should be refmask-shaped 0x00rrwwss and NOT a copy of "
                    "+2900):{}",
                    g_stencil.bfWindowDraws, w.empty() ? " none" : w);
      }
    }
  }
  // The ALU constant file. `repaired 0` is only meaningful next to a non-zero
  // `constants seen` -- with zero seen, the PM4 feed is not reaching the file and
  // the repair count says nothing at all.
  {
    uint64_t written = 0, repaired = 0, zeroed = 0, filled_zero = 0;
    uint32_t seen = 0;
    mx::gpu::alu::Stats(written, repaired, seen, zeroed, filled_zero);
    REXLOG_INFO("d3d9: ALU constant file -- {} dwords written by PM4 over {} "
                "distinct constants; {} repaired from PM4, {} NaN LEFT IN "
                "PLACE (the substitution was deleted 2026-08-27), {} finite "
                "zeros PM4 could fill but we do NOT "
                "(measurement only); shader load-table overlays {}",
                written, seen, repaired, zeroed, filled_zero,
                g_drawOutcome.shaderConstOverlays);
    // Which constants the zero-fill hit. A short tail means the fill can be
    // narrowed to a range; a long one means the frame-global PM4 file is simply
    // the wrong authority for a mid-frame draw, and the fix is upstream.
    if (filled_zero)
      REXLOG_INFO("d3d9: ZERO-FILL BY CONSTANT:{}",
                  mx::gpu::alu::FilledHistogram(12));
    // The narrow window that actually substitutes. Printed unconditionally,
    // zero included: "never fired" and "fired and changed nothing" are the two
    // outcomes worth telling apart, and a suppressed line looks like neither.
    REXLOG_INFO("d3d9: MATERIAL GATE FILL {} substitutions (pixel c84-c87, the "
                "PM4-only material block; c85.w is the terrain diffuse gate)",
                mx::gpu::alu::MaterialGateFilled());
    // Every outcome on one line, zeros included, because each says something
    // different and only one means the change worked:
    //   applied 0, denormal 0, unpub N  -> PM4 never publishes c32; remove this
    //   applied 0, denormal N           -> the source is junk; remove this
    //   applied N, filled 0             -> validated, but the slot was never a
    //                                      hole; c32 was a red herring
    //   applied N, filled M             -> the substitution is live
    // vs c32 WAS filled from this file and is not any more -- see the revert.
    // The value stays on the report because PM4 publishes it as a plain zero,
    // which is what killed the substitution.
    static const uint32_t kTint[] = {32};
    REXLOG_INFO("d3d9: ALU FILE SPOT CHECK{}",
                mx::gpu::alu::FileValues(kTint, 1));
  }
  ReportDeclHistogram();
  // The submission path that bypasses every draw hook. Reported next to the
  // declaration census because the two answer the same question from opposite
  // ends: that one says the draws we SEE are described, this one says how many
  // never arrive to be described at all.
  ReportCommandBuffers();
  ReportVegetationLod();
  // LAST, and unconditional. Every line above states a fact; this one states
  // whether any of them broke an expectation the source itself wrote down, so it
  // is the line to read first.
  //
  // Checked here rather than beside the number, which is printed on the
  // per-attempt DRAW REPORTS line: health::Record takes a mutex and that site
  // runs on every draw. A periodic look is just as good for cumulative counters.
  mx::gpu::health::Zero("gpu_fetch.ordinal_mismatch", g_gpuFetch.ordinalMismatch,
                        g_gpuFetch.draws);
  // A check that has just turned bad says so at WARN -- once, on the transition.
  // That is the only thing in this report that is not [info], which is the
  // point: `grep "\[warning\]"` over a run means "show me what broke an
  // expectation". The same entries are appended to logs/health.txt, which does
  // not rotate, so a finding outlives the 30-second window it appeared in.
  for (const std::string& bad : mx::gpu::health::DrainNewlyBad())
    REXLOG_WARN("health: BAD {}", bad);
  REXLOG_INFO("d3d9: HEALTH -- {}", mx::gpu::health::Report());
  if (REXCVAR_GET(hle_diag)) ReportCoverage(base);
}

}  // namespace mx::hooks::d3d9
