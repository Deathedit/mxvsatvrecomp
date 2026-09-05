// D3D12Renderer -- per-frame telemetry.
//
// Split verbatim out of d3d12_game_frame.cpp, which is about recording and
// submitting a frame. This reads counters and prints; it records nothing and
// decides nothing, and it was 600 of that file's 3663 lines.
//
// Everything it touches is a member or its own function-local statics, plus the
// three frame-scoped values passed in. Nothing else in the renderer calls it.

#include "gfx/d3d12_renderer.h"

#include "gfx/d3d12_game_internal.h"
#include "gfx/d3d12_internal.h"
#include "gpu/guard_census.h"

#include <cstdio>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

using mx::gfx::LogInfo;

// Per-frame telemetry, split out of RenderGameFrame.
//
// This file's own header says splitting a function body is a behaviour risk
// rather than a move, and that rule is right -- so the claim was checked rather
// than asserted, and it was WRONG. The block reads members and its own
// function-local statics, AND three frame-scoped locals of RenderGameFrame:
// fullSizeTargets, fullSizeDraws and fullSizeOrder. The compiler named all
// three on the first build, which is exactly why this rule is worth having.
// They are parameters now, so the dependency is written down instead of
// implied by scope.
// It is called from the same place, at the end of the frame, so the 20-frame
// cadence and every value it prints are unchanged.
void D3D12Renderer::ReportGameFrameTelemetry(
    const std::unordered_set<uint32_t>& fullSizeTargets,
    uint32_t fullSizeDraws,
    const std::vector<std::pair<uint32_t, uint32_t>>& fullSizeOrder) {
  // Cumulative, every 20 frames that drew anything, with distinct live targets
  // beside the cap -- together they say whether the budget is comfortable or
  // about to be exhausted; the count alone does not. Twenty, not 100: frames
  // cost ~0.5s here, so a 100-frame interval produces exactly ONE print in a
  // watched session, from frame 1, before anything has been drawn.
  static uint32_t s_rtFrame = 0;
  if (!m_gameDraws.empty() && (++s_rtFrame % 20) == 1) {
    // 512, not 300: the stand-in line below carries eight counters now, and a
    // snprintf that truncates would drop the three newest — silently losing the
    // numbers this line exists to show.
    char message[512];
    std::snprintf(message, sizeof(message),
                  "game RT routing: offscreen %llu, main %llu, OVERPAINT %llu "
                  "(refused: budget %llu, resized %llu of which fmt-changed "
                  "%llu, white-allowed-offscreen %llu, MRT bound %llu "
                  "missing %llu); live targets %u/%u "
                  "(evicted %llu, sweeps freeing nothing %llu), "
                  "srv %u/%u (cached %zu, free %zu, evicted %llu, "
                  "evict-blocked %llu)",
                  static_cast<unsigned long long>(m_rtDrawsOffscreen),
                  static_cast<unsigned long long>(m_rtDrawsMain),
                  static_cast<unsigned long long>(m_rtDrawsOverpaint),
                  static_cast<unsigned long long>(m_rtRejectBudget),
                  static_cast<unsigned long long>(m_rtRejectResized),
                  // Incremented since the format branch was written and never
                  // printed anywhere. A counter nothing reads cannot answer a
                  // question, and this is the one that says whether a snapshot's
                  // accumulated content was thrown away.
                  static_cast<unsigned long long>(m_snapshotFormatChanged),
                  static_cast<unsigned long long>(m_whiteAllowedOffscreen),
                  static_cast<unsigned long long>(m_mrtDrawsBound),
                  static_cast<unsigned long long>(m_mrtSecondTargetMissing),
                  uint32_t(m_gameRenderTargets.size()), kMaxGameRenderTargets,
                  static_cast<unsigned long long>(m_rtEvictions),
                  static_cast<unsigned long long>(m_rtEvictBlocked),
                  m_nextGameSrvDescriptor, kMaxGameTextures,
                  m_gameTextures.size(), m_freeGameSrvDescriptors.size(),
                  static_cast<unsigned long long>(m_gameTextureEvictions),
                  static_cast<unsigned long long>(m_gameTextureEvictBlocked));
    LogInfo(message);
    // The figure that says whether the guest's own shaders are actually carrying
    // the picture. Translated against stand-in, because the translated count
    // alone cannot distinguish "the frame runs guest shaders" from "four draws
    // in a corner do".
    std::snprintf(message, sizeof(message),
                  "guest shaders: %llu draws TRANSLATED (%llu of them running "
                  "the guest VERTEX shader too, %llu of those fetching their "
                  "own vertices, %llu dropped for want of one), "
                  "%llu stand-in (%.2f/frame); %llu pipelines built, %llu "
                  "failed; vertex-sampled %llu (bind failed %llu); scissor "
                  "clipped %llu, unreadable %llu",
                  static_cast<unsigned long long>(m_translatedDraws),
                  static_cast<unsigned long long>(m_gpuVertexDraws),
                  static_cast<unsigned long long>(m_gpuVertexFetchDraws),
                  static_cast<unsigned long long>(m_gpuVertexDropped),
                  static_cast<unsigned long long>(m_standInDraws),
                  // The stand-in count is the one figure on this line a reader
                  // acts on, and cumulative it is unreadable: 2643 over a
                  // 2800-frame run is one draw a frame, not a defect. Carry the
                  // rate next to it so the total cannot be read alone.
                  m_gameFrame ? double(m_standInDraws) / double(m_gameFrame)
                              : 0.0,
                  static_cast<unsigned long long>(m_translatedOk),
                  static_cast<unsigned long long>(m_translatedFailed),
                  static_cast<unsigned long long>(m_vertexSampledDraws),
                  static_cast<unsigned long long>(m_vertexSampleBindFailed),
                  static_cast<unsigned long long>(m_scissorClipped),
                  static_cast<unsigned long long>(m_scissorUnreadable));
    LogInfo(message);
    // Which of the four bind failures sent a translatable draw to the stand-in.
    // The counts decide the next move: block-exhausted means the ring is
    // undersized or not being reset, no-snapshot means the draw wants a resolve
    // result we never captured, and the texture ones point upstream at the hooks.
    std::snprintf(message, sizeof(message),
                  "translated bind failures: block-exhausted %llu, "
                  "no-snapshot %llu (depth %llu, colour %llu, "
                  "never-resolved %llu), no-texture %llu, "
                  "upload-failed %llu, array-slot-flat %llu; "
                  "surfaces created on bind %llu depth + %llu colour, "
                  "on resolve %llu; "
                  "sampler blocks %zu of %u, exhausted %llu; "
                  "descriptor blocks %u of %u per frame at peak",
                  static_cast<unsigned long long>(m_translatedBlockExhausted),
                  static_cast<unsigned long long>(m_translatedNoSnapshot),
                  static_cast<unsigned long long>(m_noSnapshotDepth),
                  static_cast<unsigned long long>(m_noSnapshotColour),
                  static_cast<unsigned long long>(m_noSnapshotUnknown),
                  static_cast<unsigned long long>(m_translatedNoTexture),
                  static_cast<unsigned long long>(m_translatedUploadFailed),
                  static_cast<unsigned long long>(
                      m_translatedArraySlotNot2DArray),
                  static_cast<unsigned long long>(m_bindCreatedDepth),
                  static_cast<unsigned long long>(m_bindCreatedColour),
                  static_cast<unsigned long long>(m_resolveCreatedSources),
                  m_samplerBlocks.size(), kSamplerBlockCount,
                  static_cast<unsigned long long>(m_samplerBlockExhausted),
                  m_translatedBlockHighWater, m_translatedBlocksPerFrame);
    LogInfo(message);
    // PER-CHUNK CLIPMAP LEVEL. How many distinct 129x129 height snapshots the
    // terrain chunks bound, and how the draws divide between them: one level for
    // every chunk means the seams in the terrain normal buffer are NOT a LOD
    // boundary. Draws-per-level is printed, not just the count, because "8
    // levels, 7 of them with one draw" and "8 levels evenly used" are different
    // situations.
    {
      std::string levels;
      uint32_t shown = 0;
      for (const auto& [object, draws] : m_clipmapLevelDraws) {
        if (shown++ >= 12) break;
        levels += fmt::format(" 0x{:08X}={}", object, draws);
      }
      std::snprintf(message, sizeof(message),
                    "terrain clipmap levels: %llu chunk draws over %zu distinct "
                    "levels%s |%s",
                    static_cast<unsigned long long>(m_clipmapDraws),
                    m_clipmapLevelDraws.size(),
                    m_clipmapCensusOverflow
                        ? " (census full, some levels unlisted)"
                        : "",
                    levels.empty() ? " (none)" : levels.c_str());
      LogInfo(message);
    }
    // SNAPSHOT SLOT FILTERING. Its own line because "how many snapshot slots are
    // silently POINT-sampled" is a question no existing counter answers, and the
    // terrain tile atlas is the one it is asked about: it binds through the
    // PARTIAL-snapshot path by design, and a partial bind that leaves the
    // sampler word zero keeps that path's POINT filter. no-word is the DEFECT
    // column; guest-asked is correct behaviour. Printed even when both are zero.
    std::snprintf(message, sizeof(message),
                  "snapshot slot filtering: %llu binds, %llu with a sampler "
                  "word (%.1f%%) | POINT taken: %llu NO-WORD (silent), %llu "
                  "guest-asked",
                  static_cast<unsigned long long>(m_snapSlotBinds),
                  static_cast<unsigned long long>(m_snapSlotWordFilled),
                  m_snapSlotBinds
                      ? 100.0 * double(m_snapSlotWordFilled) / double(m_snapSlotBinds)
                      : 0.0,
                  static_cast<unsigned long long>(m_snapSlotPointNoWord),
                  static_cast<unsigned long long>(m_snapSlotPointGuest));
    LogInfo(message);
    // Separate line rather than a longer format: the snapshot numbers answer a
    // different question (which resolve result a draw sampled) from the routing
    // ones (where a draw landed), and fallbacks are the figure to watch.
    std::snprintf(message, sizeof(message),
                  "resolve snapshots: copies %llu, hits %llu, FALLBACKS %llu, "
                  "source-not-offscreen %llu, WHITE-SKIPPED %llu, "
                  "BLANK-SOURCE %llu, STALE-REFUSED %llu; live snapshots %u/%u "
                  "(REFUSED-BUDGET %llu, evicted %llu, sweeps freeing "
                  "nothing %llu), "
                  "DEPTH resolves %llu (%llu band-stitched) from %zu depth "
                  "surfaces, stand-in depth refused %llu, "
                  "aliased-source matches %llu (+%llu contained, +%llu "
                  "msaa-partner)",
                  static_cast<unsigned long long>(m_snapshotCopies),
                  static_cast<unsigned long long>(m_snapshotHits),
                  static_cast<unsigned long long>(m_snapshotFallbacks),
                  static_cast<unsigned long long>(m_snapshotMissingSource),
                  static_cast<unsigned long long>(m_sampleMissSkipped),
                  static_cast<unsigned long long>(m_snapshotBlankSource),
                  static_cast<unsigned long long>(m_snapshotStaleRefused),
                  uint32_t(m_gameSnapshots.size()), kMaxGameSnapshots,
                  static_cast<unsigned long long>(m_snapshotRejectBudget),
                  static_cast<unsigned long long>(m_snapshotEvictions),
                  static_cast<unsigned long long>(m_snapshotEvictBlocked),
                  static_cast<unsigned long long>(m_depthResolves),
                  static_cast<unsigned long long>(m_depthBandResolves),
                  m_gameDepthTargets.size(),
                  static_cast<unsigned long long>(
                      m_standInDepthSnapshotRefused),
                  static_cast<unsigned long long>(m_aliasedSourceResolves),
                  static_cast<unsigned long long>(m_containedSourceResolves),
                  static_cast<unsigned long long>(m_msaaPartnerResolves));
    LogInfo(message);

    // EDRAM depth aliasing. Its own line because it answers a question the
    // routing counters cannot: whether two guest objects that ARE the same
    // memory were given the same host surface. `hits 0` on a menu frame means
    // the light pass is back to testing an empty depth buffer, which silently
    // discards every stencil light volume.
    std::snprintf(message, sizeof(message),
                  "depth EDRAM aliasing: %llu binds served from an aliased "
                  "surface, %zu distinct aliases; row-offset bands: %llu depth "
                  "copies, %llu SKIPPED (a skip leaves the band on its creation "
                  "clear)",
                  static_cast<unsigned long long>(m_depthAliasHits),
                  m_gameDepthAliases.size(),
                  static_cast<unsigned long long>(m_depthBandDepthCopies),
                  static_cast<unsigned long long>(m_depthBandCopySkipped));
    LogInfo(message);

    // PER-SLOT TEXINV BRANCH. Only slots that were used at all are printed, and
    // a slot with a non-zero "no-map" column is called out: that is the slot
    // sampling black, and for the terrain height tile it costs exactly 2.008
    // units of world height.
    {
      std::string slots;
      for (uint32_t s = 0; s < 16; ++s) {
        const uint64_t snap = m_texSlotPath[s][0];
        const uint64_t none = m_texSlotPath[s][1];
        const uint64_t pay = m_texSlotPath[s][2];
        if (!snap && !none && !pay) continue;
        char one[160];
        std::snprintf(one, sizeof(one), " [s%u snap %llu, NO-MAP %llu%s, payload %llu]",
                      s, static_cast<unsigned long long>(snap),
                      static_cast<unsigned long long>(none),
                      none ? " <<<" : "",
                      static_cast<unsigned long long>(pay));
        slots += one;
      }
      const std::string line =
          std::string("texinv slot paths, ALL draws:") +
          (slots.empty() ? std::string(" none") : slots);
      LogInfo(line.c_str());

      // The 129x129 height tile alone. This is the population the floating
      // bike lives in; any NO-MAP here is -2.008 world units of terrain.
      std::string tile;
      for (uint32_t s = 0; s < 16; ++s) {
        const uint64_t sn = m_texSlotPathTile[s][0];
        const uint64_t nm = m_texSlotPathTile[s][1];
        const uint64_t pl = m_texSlotPathTile[s][2];
        if (!sn && !nm && !pl) continue;
        char one[160];
        std::snprintf(one, sizeof(one),
                      " [s%u snap %llu, NO-MAP %llu%s, payload %llu]", s,
                      static_cast<unsigned long long>(sn),
                      static_cast<unsigned long long>(nm),
                      nm ? " <<< BLACK, -2.008 units" : "",
                      static_cast<unsigned long long>(pl));
        tile += one;
      }
      const std::string tline =
          std::string("texinv slot paths, 129x129 HEIGHT TILE only:") +
          (tile.empty() ? std::string(" no height tile drawn this run") : tile);
      LogInfo(tline.c_str());

      // The object behind slot 3 on those tile draws.
      std::string objs;
      for (uint32_t k = 0; k < m_tileSlot3Count; ++k) {
        char one[192];
        std::snprintf(one, sizeof(one),
                      " [obj 0x%08X %ux%u: with-entry %llu, NO-ENTRY %llu]",
                      m_tileSlot3[k].object, m_tileSlot3[k].width,
                      m_tileSlot3[k].height,
                      static_cast<unsigned long long>(m_tileSlot3[k].withEntry),
                      static_cast<unsigned long long>(
                          m_tileSlot3[k].withoutEntry));
        objs += one;
      }
      if (m_tileSlot3Overflow) {
        char one[64];
        std::snprintf(one, sizeof(one), " (+%llu dropped, table full)",
                      static_cast<unsigned long long>(m_tileSlot3Overflow));
        objs += one;
      }
      const std::string oline =
          std::string("height-tile slot 3 objects (these sample BLACK and cost"
                      " -2.008 world units):") +
          (objs.empty() ? std::string(" none -- slot 3 was always a payload")
                        : objs);
      LogInfo(oline.c_str());
    }

    // The guest alpha test. STAND-IN is the figure to watch: those draws have
    // an enabled test and took a path with no shader to discard in, so they are
    // still painting the pixels the guest masks away.
    std::snprintf(message, sizeof(message),
                  "alpha test: honoured %llu, STAND-IN %llu; "
                  "fixed16 -32..32 targets %llu draws (scale identity); 7e3 clamped %llu; "
                  "texinv snapshot-shadowed slots %llu; "
                  "half-pixel offset applied %llu, skipped %llu; "
                  "guest viewport vs host target: match %llu, MISMATCH %llu, "
                  "unreadable %llu, taken-from-guest %llu; edram takeover "
                  "transfers %llu (no-source %llu, source-never-drawn %llu); "
                  "targets carrying previous-frame content %llu (all PRESERVED -- the "
                  "first-use clear was deleted 2026-08-26); STRICT skipped %llu "
                  "stand-in draws",
                  static_cast<unsigned long long>(m_alphaTestHonoured),
                  static_cast<unsigned long long>(m_alphaTestStandIn),
                  static_cast<unsigned long long>(m_fixed16Scaled),
                  static_cast<unsigned long long>(m_float7e3Clamped),
                  static_cast<unsigned long long>(m_texinvSlotMismatch),
                  static_cast<unsigned long long>(m_halfPixelDraws),
                  static_cast<unsigned long long>(m_halfPixelSkipped),
                  static_cast<unsigned long long>(m_vpMatch),
                  static_cast<unsigned long long>(m_vpMismatch),
                  static_cast<unsigned long long>(m_vpUnknown),
                  static_cast<unsigned long long>(m_vpTakenFromGuest),
                  static_cast<unsigned long long>(m_edramTransfers),
                  static_cast<unsigned long long>(m_edramTransferNoSource),
                  static_cast<unsigned long long>(m_edramTransferNotDrawn),
                  static_cast<unsigned long long>(m_targetCarriedContent),
                  static_cast<unsigned long long>(m_standInStrictSkipped));
    LogInfo(message);
    // GUARD CENSUS -- phase 1 of docs/strict_mode.md. One line, every class-B
    // guard, fires beside the population they are a fraction of. A guard reading
    // 0/N with N large is reached constantly and never needed: that is a guard
    // that can be deleted.
    std::snprintf(message, sizeof(message), "  GUARD CENSUS --%s",
                  mx::gpu::guard::Report().c_str());
    LogInfo(message);
    // Guest depth clears. Printed unconditionally, zero included: "the guest
    // never asked" and "it asked and we could not place it" are the two outcomes
    // this change exists to tell apart, and a missing line looks like neither.
    std::snprintf(message, sizeof(message),
                  "  GUEST DEPTH CLEARS %llu honoured, %llu with no host "
                  "depth surface",
                  static_cast<unsigned long long>(m_guestDepthClears),
                  static_cast<unsigned long long>(m_guestDepthClearsUnresolved));
    LogInfo(message);
    // PHASE 2 STENCIL. Every number that decides whether this phase is sound, on
    // one line, zeros included.
    //
    //   states       distinct pipeline variants interned. The guest uses 18
    //                configurations and those differing only in ref collapse
    //                here, so a healthy run is well under 20. A number that
    //                climbs run over run means something varying is leaking into
    //                the key.
    //   refused      draws that wanted stencil past the intern cap and rendered
    //                WITHOUT it. Must be 0 -- non-zero is a wrong picture rather
    //                than an error, which is why it is printed.
    //   blend PSOs   occupancy against kMaxBlendPSOs, the sharpest hazard here:
    //                past the cap a blended draw silently falls back to its
    //                opaque pipeline and loses its blending.
    //   by-format    the on-demand opaque cache, same concern.
    //   translated   THE ONE THAT MATTERS NOW. Stencil is in the translated key,
    //                so every stencil state multiplies the variants of every
    //                shader that meets it. `capped` non-zero means pipelines are
    //                no longer being built.
    std::snprintf(message, sizeof(message),
                  "  STENCIL PSOs: %llu draws carried stencil (%llu with a "
                  "comparison that can REJECT -- zero means the test is not "
                  "live), %llu distinct states interned, %llu refused past the "
                  "cap (must be 0); blend PSOs %llu of %llu, by-format PSOs "
                  "%llu, TRANSLATED PSOs %llu of %llu (capped %llu); paths: "
                  "%llu translated, %llu stand-in, %llu WITH NO DSV",
                  static_cast<unsigned long long>(m_stencilDraws),
                  static_cast<unsigned long long>(m_stencilTestingDraws),
                  static_cast<unsigned long long>(m_stencilStates.size()),
                  static_cast<unsigned long long>(m_stencilStatesRefused),
                  static_cast<unsigned long long>(m_blendPSOs.size()),
                  static_cast<unsigned long long>(kMaxBlendPSOs),
                  static_cast<unsigned long long>(m_gamePSOsByFormat.size()),
                  static_cast<unsigned long long>(m_translatedPSOs.size()),
                  static_cast<unsigned long long>(kMaxTranslatedPSOs),
                  static_cast<unsigned long long>(m_translatedPsoCapped),
                  static_cast<unsigned long long>(m_stencilViaTranslated),
                  static_cast<unsigned long long>(m_stencilViaStandIn),
                  static_cast<unsigned long long>(m_stencilNoDsv));
    LogInfo(message);
    // DIAG: what the WHITE-SKIPPED draws were aimed at.
    for (const auto& [extent, e] : m_skipByTarget) {
      std::snprintf(message, sizeof(message),
                    "  WHITE-SKIPPED target %ux%u obj 0x%08X: %llu draws, "
                    "%llu translated, %llu wanted sampler slots",
                    uint32_t(extent >> 32), uint32_t(extent), e.object,
                    static_cast<unsigned long long>(e.count),
                    static_cast<unsigned long long>(e.translated),
                    static_cast<unsigned long long>(e.wantedSlots));
      LogInfo(message);
    }
    // DIAG: the population behind BLANK-SOURCE. A blank snapshot is a compositor
    // quad painting nothing, so a full-screen extent with a frame range that
    // ends early is a boot-time screen whose backdrop never arrived. `rescue`
    // says why the substitution search did not save it: no-cand = nothing else
    // sits at that EDRAM base, all-blank = something does but nothing was ever
    // drawn into it either, n/a = depth source or no base to search from.
    for (const auto& [extent, b] : m_blankSourceByExtent) {
      std::snprintf(message, sizeof(message),
                    "  BLANK-SOURCE %ux%u src 0x%08X base 0x%03X fmt %u -> dest "
                    "0x%08X: %llu resolves, frames %llu..%llu; rescue no-cand "
                    "%llu all-blank %llu n/a %llu",
                    uint32_t(extent >> 32), uint32_t(extent), b.object,
                    b.edramBase, b.format, b.dest,
                    static_cast<unsigned long long>(b.count),
                    static_cast<unsigned long long>(b.firstFrame),
                    static_cast<unsigned long long>(b.lastFrame),
                    static_cast<unsigned long long>(b.rescueNoCandidate),
                    static_cast<unsigned long long>(b.rescueAllBlank),
                    static_cast<unsigned long long>(b.rescueNotAttempted));
      LogInfo(message);
    }
    // Why banded depth resolves did not stitch. See DepthBandRefusal for the
    // reason codes; reason 2 carries the observed total, which says whether the
    // cover UNDERSHOOT (a band missing from the pool) or OVERSHOT (an extra
    // aliasing surface joined the set) -- opposite repairs.
    for (const auto& [reason, r] : m_depthBandRefusals) {
      static const char* kWhy[] = {"accepted", "fewer than 2 bands",
                                   "heights do not cover the destination",
                                   "cover does not start at the source base",
                                   "exact cover but no band drawn yet"};
      std::snprintf(
          message, sizeof(message),
          "  DEPTH-BAND REFUSED (%u: %s) x%llu: dest %ux%u, %u candidates "
          "summing %u, source 0x%08X base 0x%03X, first band base 0x%03X",
          reason, reason < 5 ? kWhy[reason] : "?",
          static_cast<unsigned long long>(r.count), r.destWidth, r.destHeight,
          r.candidates, r.observedTotal, r.source, r.sourceBase, r.firstBase);
      LogInfo(message);
    }
    // DIAG: the COLOUR pool with its EDRAM bases. The 640x360 resolve source
    // (base 0x2D0, pitch 640, 4x MSAA in its surface word) has no host target of
    // its own, while a 640x720 surface sits at the same base and pitch at 1x.
    // Whether that 640x720 is drawn into decides whether the 640x360 resolve
    // should take a region of it or whether the pass that fills it is lost.
    for (const auto& [object, t] : m_gameRenderTargets) {
      std::snprintf(message, sizeof(message),
                    "  COLOUR pool obj 0x%08X %ux%u base 0x%03X fmt %u "
                    "drawn:%s",
                    object, t.width, t.height, t.edramBase,
                    uint32_t(t.format), t.everDrawn ? "Y" : "N");
      LogInfo(message);
    }
    // DIAG: what the depth pool actually holds. The shadow resolve names a
    // 768x1024 depth surface while the pass appears to render two EDRAM bands
    // (768x640 at base 0x580, 768x384 at base 0x710), so the object the resolve
    // asks for may be one no draw ever bound.
    for (const auto& [object, t] : m_gameDepthTargets) {
      std::snprintf(message, sizeof(message),
                    "  DEPTH pool obj 0x%08X %ux%u drawn:%s", object, t.width,
                    t.height, t.everDrawn ? "Y" : "N");
      LogInfo(message);
    }
    // The worst offenders behind source-not-offscreen, with their status read
    // NOW rather than at first sighting. Sorted by how many resolves each one
    // cost, because one source losing a thousand resolves and a thousand losing
    // one are different defects.
    if (!m_missingSourceCounts.empty()) {
      std::vector<std::pair<uint32_t, uint64_t>> worst(
          m_missingSourceCounts.begin(), m_missingSourceCounts.end());
      std::sort(worst.begin(), worst.end(),
                [](const auto& a, const auto& b) { return a.second > b.second; });
      std::string line;
      for (size_t i = 0; i < worst.size() && i < 6; ++i) {
        char one[96];
        std::snprintf(one, sizeof(one), " 0x%08X x%llu(drawn:%s)",
                      worst[i].first,
                      static_cast<unsigned long long>(worst[i].second),
                      m_everDrawTarget.count(worst[i].first) ? "Y" : "N");
        line += one;
      }
      std::snprintf(message, sizeof(message),
                    "missing-source offenders (%zu distinct):%s",
                    m_missingSourceCounts.size(), line.c_str());
      LogInfo(message);
    }
    std::snprintf(message, sizeof(message),
                  "stand-in reasons: no-hlsl %llu, no-handle %llu, "
                  "no-vertex-inputs %llu, no-constants %llu, "
                  "too-many-samplers %llu; AFTER the gate: buffer-failed %llu, "
                  "pso-capped %llu, no-root-sig %llu",
                  static_cast<unsigned long long>(m_standInNoHlsl),
                  static_cast<unsigned long long>(m_standInNoHandle),
                  static_cast<unsigned long long>(m_standInNoVertexInputs),
                  static_cast<unsigned long long>(m_standInNoConstants),
                  static_cast<unsigned long long>(m_standInTooManySamplers),
                  static_cast<unsigned long long>(m_standInBufferFailed),
                  static_cast<unsigned long long>(m_translatedPsoCapped),
                  static_cast<unsigned long long>(m_translatedNoRootSig));
    LogInfo(message);
    // Name the no-handle population. Sorted by count, worst first, and the
    // DISTINCT key count printed alongside so "one shader does all 1906" and
    // "1906 shaders do one each" are distinguishable.
    {
      std::vector<std::pair<uint64_t, uint64_t>> worst;  // count, key
      worst.reserve(m_standInNoHandleBy.size());
      for (const auto& [key, n] : m_standInNoHandleBy)
        worst.emplace_back(n, key);
      std::sort(worst.rbegin(), worst.rend());
      std::string line;
      for (size_t i = 0; i < worst.size() && i < 8; ++i) {
        char one[80];
        std::snprintf(one, sizeof(one), " [vs 0x%08X x%u idx=%u]",
                      uint32_t(worst[i].second >> 32),
                      static_cast<unsigned>(worst[i].first),
                      uint32_t(worst[i].second & 0xFFFFFFFFu));
        line += one;
      }
      // PER FRAME, beside the cumulative total, and that is the point of this
      // line: on a 2800-frame run this printed "2744" and read as a 2744-draw
      // defect. It is ONE DRAW PER FRAME, and that number was carried as an open
      // item for most of a session on the strength of the total alone.
      //
      // What the residual actually is, measured in a capture: the frame's FIRST
      // draw, a screen-space quad over roughly the top-left 40% x 35%, issued
      // before the guest has bound any shader and CLEARED five events later by
      // the frame's own clear. It never reaches the screen.
      const double perFrame =
          m_gameFrame ? double(m_standInNoHandle) / double(m_gameFrame) : 0.0;
      std::snprintf(message, sizeof(message),
                    "no-handle records: %llu total over %llu frames = %.2f per "
                    "frame; = %llu yuv + %llu clear + %llu surface-bind + %llu "
                    "other; %zu distinct (vs, idx):%s",
                    static_cast<unsigned long long>(m_standInNoHandle),
                    static_cast<unsigned long long>(m_gameFrame), perFrame,
                    static_cast<unsigned long long>(m_standInNoHandlePlanes),
                    static_cast<unsigned long long>(m_standInNoHandleClear),
                    static_cast<unsigned long long>(m_standInNoHandleBind),
                    static_cast<unsigned long long>(
                        m_standInNoHandle - m_standInNoHandlePlanes -
                        m_standInNoHandleClear - m_standInNoHandleBind),
                    m_standInNoHandleBy.size(), line.c_str());
    }
    LogInfo(message);
    // The YUV plane gate, whole population and never gated on non-zero: this is
    // the last step before a video draw becomes pixels, and every refusal in it
    // used to be silent.
    std::snprintf(message, sizeof(message),
                  "yuv plane gate: %llu prepared, refused %llu no-heap / "
                  "%llu too-few-planes / %llu OVER BUDGET (cap %u per frame)",
                  static_cast<unsigned long long>(m_yuvPrepared),
                  static_cast<unsigned long long>(m_yuvRefusedNoHeap),
                  static_cast<unsigned long long>(m_yuvRefusedTooFewPlanes),
                  static_cast<unsigned long long>(m_yuvRefusedBudget),
                  kMaxYuvDrawsPerFrame);
    LogInfo(message);
    // EDRAM aliasing, whole population and never gated on non-zero. Sizes the
    // ownership-transfer fix: `same-extent` takeovers are the ones it can
    // repair, and `format-differs` are the subset needing a conversion rather
    // than a plain copy.
    {
      size_t sharedBases = 0;
      for (const auto& [edramBase, owners] : m_edramOwners)
        if (owners.size() >= 2) ++sharedBases;
      std::snprintf(message, sizeof(message),
                    "edram aliasing: %zu bases, %zu shared by >1 object; "
                    "%llu takeovers (%llu same-extent, %llu of those "
                    "format-differs)",
                    m_edramOwners.size(), sharedBases,
                    static_cast<unsigned long long>(m_edramTakeovers),
                    static_cast<unsigned long long>(m_edramTakeoverSameExtent),
                    static_cast<unsigned long long>(m_edramTakeoverFormatDiff));
      LogInfo(message);
      // ONE LINE PER BASE. Packing every base into the single line above
      // truncated at message[512]: base 0x0 has ~15 owners and consumed the
      // whole buffer, so base 0x2D0 -- the one this was built to look at --
      // never printed at all.
      size_t emitted = 0;
      for (const auto& [edramBase, owners] : m_edramOwners) {
        if (owners.size() < 2 || ++emitted > 8) continue;
        std::string named;
        char one[96];
        size_t shown = 0;
        for (const auto& o : owners) {
          if (++shown > 8) break;
          std::snprintf(one, sizeof(one), " 0x%08X(%ux%u fmt%u x%llu)",
                        o.object, o.width, o.height, uint32_t(o.format),
                        static_cast<unsigned long long>(o.binds));
          named += one;
        }
        std::snprintf(message, sizeof(message),
                      "  edram base 0x%X: %zu owners%s%s", edramBase,
                      owners.size(), named.c_str(),
                      owners.size() > 8 ? " ..." : "");
        LogInfo(message);
      }
    }
    // THIS frame, not cumulative: the question is whether the frame on screen
    // was assembled on one surface or several. "presented 1 of 1" means present
    // is showing the finished scene; "1 of 4" means it is showing one layer.
    std::snprintf(message, sizeof(message),
                  "present source: resolve 0x%08X, fallback object 0x%08X, %u "
                  "full-size surfaces drawn this frame, %u draws across them",
                  m_presentResolveTexture, m_presentSourceObject,
                  uint32_t(fullSizeTargets.size()), fullSizeDraws);
    LogInfo(message);
    {
      std::string order;
      char one[64];
      for (const auto& e : fullSizeOrder) {
        std::snprintf(one, sizeof(one), " 0x%08X=%u", e.first, e.second);
        order += one;
      }
      std::snprintf(message, sizeof(message),
                    "full-size surfaces this frame, in draw order:%s "
                    "(presenting the last)",
                    order.empty() ? " none" : order.c_str());
      LogInfo(message);
    }
    // Age of a full-screen snapshot when a draw samples it. The 100+ bucket is
    // the one that matters: those are whole leftover frames being composited.
    std::snprintf(message, sizeof(message),
                  "fullscreen snapshot age at sample: this-frame %llu, "
                  "last %llu, 2-9 %llu, 10-99 %llu, 100+ %llu",
                  static_cast<unsigned long long>(m_snapshotAge[0]),
                  static_cast<unsigned long long>(m_snapshotAge[1]),
                  static_cast<unsigned long long>(m_snapshotAge[2]),
                  static_cast<unsigned long long>(m_snapshotAge[3]),
                  static_cast<unsigned long long>(m_snapshotAge[4]));
    LogInfo(message);
    // Both of these have been non-zero for real reasons — a draw-list cap that
    // dropped resolves, and a descriptor leak that made every creation fail —
    // and both were invisible until they were counted. Kept reported.
    std::snprintf(message, sizeof(message),
                  "resolve budget: dropped-list-full %llu, create-failed %llu",
                  static_cast<unsigned long long>(m_resolvesDroppedFull),
                  static_cast<unsigned long long>(m_snapshotCreateFailed));
    LogInfo(message);
  }
}
