// D3D12Renderer -- per-frame submission: the draw/clear/resolve queue,
// RenderGameFrame, and present.
//
// Split verbatim out of d3d12_game.cpp. RenderGameFrame is ~1900 lines on its
// own and AddGameDraw ~600; both moved WHOLE. Splitting a function body is a
// behaviour risk, not a move, so the size of this file is the floor until
// someone deliberately restructures those two.
#include "gfx/d3d12_renderer.h"

#include "gfx/d3d12_game_internal.h"
#include "gfx/d3d12_internal.h"
#include "gfx/d3d12_shaders.h"
#include "gpu/guard_census.h"
#include "gpu/d3d9_layout.h"
#include "gpu/hle_types.h"
#include "gpu/shader_hlsl.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

using mx::gfx::CompileShader;
using mx::gfx::LogError;
using mx::gfx::LogInfo;

void D3D12Renderer::RenderGameFrame() {
  if (!m_hasGamePipeline) return;
  m_commandList->SetGraphicsRootSignature(m_gameRootSig.Get());
  m_commandList->SetPipelineState(m_gamePSOs[0].Get());

  ID3D12DescriptorHeap* heaps[] = {m_gameSrvHeap.Get(), m_samplerHeap.Get()};
  m_commandList->SetDescriptorHeaps(2, heaps);
  // Composite draws take their plane descriptor blocks in order within a frame.
  m_yuvDrawsThisFrame = 0;
  ++m_gameFrame;
  // Before anything is recorded into this slot: the copy issued the last time
  // round is complete, and the guest is waiting on its exposure.
  DrainLuminanceReadback();
  // The same, for small destinations the guest reads out of memory -- the
  // terrain virtual-texture feedback buffer. See QueueSurfaceReadback.
  DrainSurfaceReadback();
  // This frame in flight takes its own slice of the descriptor blocks, so the
  // window resets every host frame rather than only when the guest hands off a
  // new draw list. See m_translatedBlocksPerFrame.
  // High-water BEFORE the reset, so it measures the frame that just ended.
  // Without it the ring's headroom is unfalsifiable from a log -- which is how
  // "eight times the measured demand of ~125" survived into a scene submitting
  // seven times that.
  //
  // The base comes from m_translatedBlockLimit, NOT from m_frameIndex: at this
  // point m_frameIndex has already advanced to the frame about to be recorded,
  // while m_translatedBlockNext still points into the slice of the frame that
  // just ended. Deriving the base from the new index reads across the wrap and
  // reports nonsense -- the first run of this printed "16726 of 8192 per frame
  // at peak", i.e. 2 * 8192 + 342, when the true figure was 342. A high-water
  // mark that can exceed its own limit is how that got caught; keep the
  // impossible value impossible.
  {
    const uint32_t prevBase = m_translatedBlockLimit >= m_translatedBlocksPerFrame
                                  ? m_translatedBlockLimit -
                                        m_translatedBlocksPerFrame
                                  : 0;
    if (m_translatedBlockNext > prevBase) {
      const uint32_t used = m_translatedBlockNext - prevBase;
      if (used > m_translatedBlockHighWater) m_translatedBlockHighWater = used;
    }
  }
  m_translatedBlockNext = m_frameIndex * m_translatedBlocksPerFrame;
  m_translatedBlockLimit = m_translatedBlockNext + m_translatedBlocksPerFrame;
  for (auto& [object, target] : m_gameRenderTargets)
    target.usedThisFrame = false;
  for (auto& [object, target] : m_gameDepthTargets)
    target.usedThisFrame = false;

  // Which 1x1 resolve of this frame we are looking at, for the rotation in
  // QueueLuminanceReadback.
  uint32_t oneByOneSeen = 0;
  uint32_t boundTargetObject = 0;  // zero is the final m_gameRT.
  // The scissor last handed to the command list. Tracked so the per-draw
  // scissor costs one comparison rather than a state change per draw, and so
  // the target-binding blocks below no longer own the scissor at all -- they
  // set the viewport, this owns the rectangle.
  D3D12_RECT boundScissor = {-1, -1, -1, -1};
  // Zero means "no DSV bound", which is a distinct state from any depth object.
  uint32_t boundDepthObject = 0;
  // MRT slot 1's format for whatever is currently bound, so the PSO key below
  // describes the RTVs that are ACTUALLY set rather than what the draw asked
  // for. UNKNOWN means one target is bound, which is the overwhelming default.
  DXGI_FORMAT boundTarget1Format = DXGI_FORMAT_UNKNOWN;
  static const float kOffscreenClear[4] = {0, 0, 0, 0};
  std::unordered_set<uint32_t> sampledTargets;
  sampledTargets.reserve(m_gameDraws.size());
  std::unordered_set<uint32_t> resolveSources;
  for (const auto& d : m_gameDraws) {
    if (d.sampledTargetObject &&
        d.sampledTargetObject != d.targetObject)
      sampledTargets.insert(d.sampledTargetObject);
    if (d.resolveDest && d.resolveSource) resolveSources.insert(d.resolveSource);
  }
  // Carry both sets forward across frames, and decide routing on the union.
  //
  // The routing decision below is made when a surface is DRAWN INTO, but the
  // facts it needs -- will this be resolved, will a later draw sample it -- are
  // only known when the resolve or the sample arrives, which is often a
  // different frame. Built per frame, the sets answer for the wrong frame: a
  // surface drawn in frame N and resolved in N+1 is not a resolve source in N,
  // so N routes it to m_gameRT, which is cleared every frame, and N+1's resolve
  // finds no offscreen entry and copies nothing. Its snapshot stays at the
  // clear colour, and every draw sampling it paints black.
  //
  // That is the other side of the note below: "25 of 33 resolves had sources
  // with no draws at all that frame". The contents were established earlier --
  // and were thrown away earlier, for exactly this reason.
  //
  // History is the right basis because these are properties of a surface's
  // ROLE, which is stable: a surface the guest resolves once is a resolve
  // target it will resolve again. Cost is bounded and visible -- more surfaces
  // qualify for an offscreen target, capped by the same budget, and the
  // "refused: budget" figure on the routing line is what says if it bites.
  for (uint32_t object : resolveSources) m_everResolveSource.insert(object);
  for (uint32_t object : sampledTargets) m_everSampledTarget.insert(object);
  for (const auto& d : m_gameDraws)
    if (d.targetObject) m_everDrawTarget.insert(d.targetObject);
  // Reset per frame. The final whole-backbuffer colour resolve is the exact
  // image handed to VdSwap, while the last target drawn into is only a fallback
  // for lists that contain no such resolve.
  m_presentResolveTexture = 0;
  m_presentSourceObject = 0;
  std::unordered_set<uint32_t> fullSizeTargets;
  uint32_t fullSizeDraws = 0;
  // Per surface, in the order first drawn into. "Two surfaces, 8 draws" cannot
  // say whether one is the scene and the other a one-draw overlay, or whether
  // the work is split evenly -- and presenting the LAST one written is only
  // right in the first case. The order matters as much as the counts: a
  // compositor writes its output last, a UI layer is written last over a scene
  // that was finished earlier, and those want opposite choices.
  std::vector<std::pair<uint32_t, uint32_t>> fullSizeOrder;
  for (const auto& d : m_gameDraws) {
    // A SURFACE BIND: the guest named this surface as an attachment. Create
    // host storage for it now, whether or not any draw we route ever targets
    // it, and clear it to its documented creation value so a surface that is
    // bound and resolved without a single draw reads as empty rather than as
    // whatever the recycled pool handed us.
    //
    // This is what makes a depth-only pass cost nothing. Storage used to be
    // created by the first DRAW naming a surface, so the menu's shadow atlas --
    // bound depth-only with no colour target, resolved 287 times in one run --
    // was never instantiated at all, its resolve found no source, and every
    // draw sampling the result was discarded whole (mx_1000: no-snapshot 447,
    // all depth). Xenia has no equivalent failure because it creates render
    // targets from register state with depth as an equal peer of colour, not as
    // a passenger on it (render_target_cache.cc:888, keys at
    // render_target_cache.h:268).
    //
    // What this did NOT fix is the missing arena backdrop it was written for. A
    // capture afterwards shows the surfaces created, the bands stitched and the
    // snapshot carrying real depth (0.148..1.0), the backdrop draw running --
    // and the arena still absent, with the draw count unchanged at 344 either
    // side of the change. Judge this on the counters it moves, not on that.
    //
    // Draws nothing.
    if (d.surfaceBind) {
      if (!d.surfaceBindIsDepth)
        LogGuestColorFormat(d.targetObject, d.targetWidth, d.targetHeight,
                            d.targetColorFormat);
      GameRenderTarget* bound =
          d.surfaceBindIsDepth
              ? EnsureGameDepthTarget(d.targetObject, d.targetWidth,
                                      d.targetHeight, d.targetBase)
              : EnsureGameRenderTarget(d.targetObject, d.targetWidth,
                                       d.targetHeight, d.targetBase,
                                       HostColorFormat(d.targetColorFormat));
      // Refused for budget or extent. m_rtRejectBudget already counts it; the
      // draw path will try again and fail the same way, which is what it did
      // before this record existed.
      if (!bound || !bound->needsInitialClear) continue;
      bound->needsInitialClear = false;
      if (d.surfaceBindIsDepth) {
        ++m_bindCreatedDepth;
        // Created in DEPTH_WRITE, which is the state a clear wants.
        if (bound->state != D3D12_RESOURCE_STATE_DEPTH_WRITE) {
          D3D12_RESOURCE_BARRIER toDepth = {};
          toDepth.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
          toDepth.Transition.pResource = bound->resource.Get();
          toDepth.Transition.StateBefore = bound->state;
          toDepth.Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
          toDepth.Transition.Subresource =
              D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
          m_commandList->ResourceBarrier(1, &toDepth);
          bound->state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        }
        auto dsv = m_gameDepthDsvHeap->GetCPUDescriptorHandleForHeapStart();
        dsv.ptr += SIZE_T(bound->rtvIndex) * m_gameDsvDescriptorSize;
        // 1.0 is the far plane: "nothing occludes". That is the correct reading
        // of a shadow map no caster was rendered into, and it is the value
        // EnsureGameDepthTarget already declares as the resource's clear value.
        m_commandList->ClearDepthStencilView(dsv, kGameDepthClearFlags, 1.0f,
                                             0, 0, nullptr);
      } else {
        ++m_bindCreatedColour;
        // Colour targets are created in PIXEL_SHADER_RESOURCE.
        if (bound->state != D3D12_RESOURCE_STATE_RENDER_TARGET) {
          D3D12_RESOURCE_BARRIER toRt = {};
          toRt.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
          toRt.Transition.pResource = bound->resource.Get();
          toRt.Transition.StateBefore = bound->state;
          toRt.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
          toRt.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
          m_commandList->ResourceBarrier(1, &toRt);
          bound->state = D3D12_RESOURCE_STATE_RENDER_TARGET;
        }
        auto rtv = m_gameRtvHeap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += SIZE_T(bound->rtvIndex) * m_gameRtvDescriptorSize;
        // TRANSPARENT BLACK, never white. A blanket white stand-in for missing
        // resolve results was tried before and put white over the Bink logo
        // composite; that is the regression this must not reintroduce.
        const float kEmpty[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        m_commandList->ClearRenderTargetView(rtv, kEmpty, 0, nullptr);
      }
      // everDrawn stays FALSE on purpose. A resolve out of a surface nothing
      // drew into is exactly what m_snapshotBlankSource counts, and it should
      // keep counting it -- the surface now exists, which is a different claim
      // from the surface having contents.
      continue;
    }
    // A resolve: snapshot the source target as it stands right now, so draws
    // recorded after this point sample these contents rather than whatever the
    // shared surface holds by the end of the frame. Draws nothing.
    if (d.resolveDest) {
      // What KIND of image this destination holds, recorded before any of the
      // branching below can refuse the resolve.
      //
      // A slot naming a destination we have no snapshot for used to fail the
      // whole draw, and could not do better because nothing said whether the
      // missing image was depth or colour -- so the only substitute available
      // was a blanket one, and a blanket substitute is what put white over the
      // Bink logo (see BindTranslatedTextures). The guest tells us on every
      // resolve; it just was not being kept. Recorded for EVERY resolve that
      // arrives, including ones that go on to be dropped for a missing source,
      // because those are precisely the destinations that end up with no
      // snapshot.
      m_resolveDestIsDepth[d.resolveDest] = d.resolveSourceIsDepth;
      // Where the source actually rendered. Resolve sources are routed
      // offscreen (isResolveSource, below), so this normally finds a surface of
      // the source's own — which is the point: offscreen targets are only
      // cleared when something draws into them, so they carry their contents
      // across frames. A resolve needs exactly that.
      ID3D12Resource* srcRes = nullptr;
      GameRenderTarget* srcEntry = nullptr;
      uint32_t srcWidth = 0, srcHeight = 0;
      D3D12_RESOURCE_STATES srcState = D3D12_RESOURCE_STATE_RENDER_TARGET;
      // The snapshot takes the source's format: a depth resolve has to land in
      // R32_FLOAT, not RGBA8, for CopyTextureRegion to accept it.
      DXGI_FORMAT snapFormat = kBackBufferFormat;
      // Gather the EDRAM bands of a depth resolve BEFORE looking for a target
      // of the source's own, because the two are now in competition. Since
      // surfaces are created when the guest binds them, the whole surface a
      // banded pass resolves out of is in the depth pool too -- so the direct
      // lookup below would succeed and quietly copy a surface nothing rendered
      // into, discarding the bands that hold the actual image. Bands win, but
      // only when something has been drawn into them.
      //
      // Excluding the resolve's OWN object from the band set is load-bearing
      // for the same reason: the 768x1024 atlas matches the band filter on
      // width and base as well as its own 768x640 band does, and including it
      // makes the heights sum to 2048 against a 1024 destination, which fails
      // the exact-cover test and loses the stitch entirely.
      std::vector<GameRenderTarget*> depthBands;
      bool depthBandsDrawn = false;
      if (d.resolveSourceIsDepth && d.resolveDestWidth && d.resolveDestHeight) {
        for (auto& [obj, t] : m_gameDepthTargets) {
          if (obj == d.resolveSource) continue;
          if (t.resource && t.width == d.resolveDestWidth &&
              t.edramBase >= d.resolveSourceBase)
            depthBands.push_back(&t);
        }
        std::sort(depthBands.begin(), depthBands.end(),
                  [](const GameRenderTarget* a, const GameRenderTarget* b) {
                    return a->edramBase < b->edramBase;
                  });
        uint32_t total = 0;
        for (const GameRenderTarget* b : depthBands) total += b->height;
        // Anything that is not an exact cover of the destination, starting at
        // the resolve's own base, is not a banding of this surface.
        //
        // WHICH of these three refuses, and with what numbers. Without this the
        // only evidence is a BLANK-SOURCE count, which says a resolve copied an
        // undrawn surface but not whether the stitch was unavailable, mis-shaped
        // or simply not yet drawn -- three different repairs. The exact-cover
        // test is the fragile one by construction: it sums EVERY same-width
        // target at or above the base, so one extra aliasing surface makes the
        // total overshoot and loses the stitch entirely, which is exactly the
        // failure the "exclude the resolve's own object" clause above was added
        // to dodge once already.
        const uint32_t candidates = uint32_t(depthBands.size());
        uint32_t refusal = 0;  // 0 = accepted
        if (depthBands.size() < 2)
          refusal = 1;  // fewer than two bands to stitch
        else if (total != d.resolveDestHeight)
          refusal = 2;  // heights do not cover the destination exactly
        else if (depthBands.front()->edramBase != d.resolveSourceBase)
          refusal = 3;  // cover does not start at the resolve's own base
        if (refusal) depthBands.clear();
        for (const GameRenderTarget* b : depthBands)
          if (b->everDrawn) depthBandsDrawn = true;
        if (!refusal && !depthBandsDrawn)
          refusal = 4;  // exact cover, but no band has been drawn into yet
        if (refusal) {
          auto& r = m_depthBandRefusals[refusal];
          if (!r.count) {
            r.destWidth = d.resolveDestWidth;
            r.destHeight = d.resolveDestHeight;
            r.observedTotal = total;
            r.candidates = candidates;
            r.sourceBase = d.resolveSourceBase;
            r.firstBase = candidates ? depthBands.empty()
                                           ? 0u
                                           : depthBands.front()->edramBase
                                     : 0u;
            r.source = d.resolveSource;
          }
          ++r.count;
        }
      }
      if (auto it = m_gameRenderTargets.find(d.resolveSource);
          !d.resolveSourceIsDepth && it != m_gameRenderTargets.end()) {
        srcEntry = &it->second;
        srcRes = srcEntry->resource.Get();
        srcWidth = srcEntry->width;
        srcHeight = srcEntry->height;
        srcState = srcEntry->state;
        // CopyTextureRegion requires the two formats to agree, so the snapshot
        // takes the source's -- which stopped being RGBA8 once the HDR targets
        // got their real formats.
        snapFormat = srcEntry->format;
      } else if (auto dit = m_gameDepthTargets.find(d.resolveSource);
                 dit != m_gameDepthTargets.end() && !depthBandsDrawn) {
        // A DEPTH resolve. The guest reads its depth buffer back as a texture
        // to reconstruct world position in the deferred lighting pass, and
        // Resolve names the depth surface by object exactly as it names a
        // colour one (source slot 4). Looking only in the colour map is what
        // made every one of these miss.
        srcEntry = &dit->second;
        srcRes = srcEntry->resource.Get();
        srcWidth = srcEntry->width;
        srcHeight = srcEntry->height;
        srcState = srcEntry->state;
        snapFormat = kGameDepthResourceFormat;  // planar: must match the source
        ++m_depthResolves;
      }
      // ALIASED COLOUR SOURCE. The guest gives one EDRAM allocation several
      // surface objects: 0x2653FDA0 is what its draws name as their target and
      // 0x2653FF20 is what the resolve names as its source, both 129x129 at
      // base 0x2D0 through surface descriptor 0x028000A0. Object identity
      // cannot connect them, so the resolve found nothing, the snapshot never
      // appeared, and every draw sampling it was discarded -- including the
      // draws into that same target, which is a permanent deadlock: the
      // snapshot only exists once the draw has run, and the draw only runs once
      // the snapshot exists.
      //
      // Match on the EDRAM base and the extent instead, which is what actually
      // identifies the storage. Both must agree, and the base must be non-zero,
      // so a target at an unknown base cannot capture an unrelated resolve.
      // Matched on the SOURCE's own extent, not the destination texture's: a
      // 640x360 source whose destination extent could not be decoded reads as
      // 0x0 and matched nothing, which left 0x22414860 losing 20 resolves a
      // window after the 129x129 pair was already fixed.
      //
      // Two passes, exact before containment. Exact is the 129x129 and 640x360
      // pairs -- one allocation named twice at one size. Containment is the
      // MULTISAMPLE case: 0x21DFCA60 is 640x360 with 4x MSAA in its surface
      // word (0x0A020280) and 0x2123C9BC is 640x720 at 1x (0x0A000280), both at
      // base 0x2D0 pitch 640, and only the 640x720 is ever drawn into -- which
      // the colour-pool dump confirmed. We render everything at 1x, so the
      // samples the guest would resolve down are not there to resolve; taking
      // the top 640x360 rows of the surface that IS drawn is the closest thing
      // we hold. PROVISIONAL: this is the one step here not established from
      // evidence, so it is counted separately and judged on the picture. If the
      // luminance it produces looks wrong, the row mapping is what to revisit.
      // HOLDING A SURFACE IS NOT HOLDING ITS CONTENTS. This used to run only
      // when the object lookup found nothing, and so it never ran at all --
      // `aliased-source matches 0 (+0 contained)` across whole runs, with the
      // 640x720 partner named in the comment above sitting right there in the
      // pool, drawn.
      //
      // The failing case is not a missing resource, it is a resource nothing
      // ever rendered into. 0x2653C8E0 is 640x360 at base 0x2D0 and is in the
      // colour pool with everDrawn false, while 0x2123C9BC is 640x720 at the
      // same base and pitch and IS drawn. We had a blank surface for the object
      // the resolve named, bound it, and copied its zeros -- which is what the
      // auto-exposure ladder then measured, giving luminance 0, ln(1e-4) two
      // rungs down, and an exposure that climbs until the frame saturates.
      GameRenderTarget* msaaPartner = nullptr;
      uint32_t srcScale = 1;
      // Why the substitution search below did or did not save a blank source.
      // Read at the BLANK-SOURCE counter further down; see BlankSourceInfo.
      // 0 = never attempted, 1 = no other surface at this EDRAM base,
      // 2 = candidates existed but none was ever drawn into, 3 = rescued.
      uint8_t blankRescue = 0;
      uint32_t blankCandidates = 0;
      if ((!srcRes || (srcEntry && !srcEntry->everDrawn)) &&
          !d.resolveSourceIsDepth && d.resolveSourceBase &&
          d.resolveSourceWidth && d.resolveSourceHeight) {
        GameRenderTarget* exact = nullptr;
        GameRenderTarget* contains = nullptr;
        GameRenderTarget* blank_exact = nullptr;
        // THE MULTISAMPLE PARTNER, and the one this case actually wants.
        //
        // 4x MSAA on Xenos is 2x2, so a 640x360 4x surface occupies the same
        // EDRAM samples as a 1280x720 surface at 1x. We render everything at
        // 1x, so the partner IS the image the guest would have resolved down.
        //
        // Matched on FORMAT as well as extent, which is what the first attempt
        // at this got wrong: searching on base and width alone found the
        // 640x720 RGBA8 at this base -- an LDR surface from an unrelated pass
        // -- and 1120 "contained" matches later the exposure was still
        // diverging. At base 0x2D0 the drawn RGBA16F is 1280x720, exactly twice
        // the source in both axes, and that is the HDR scene.
        for (auto& [obj, t] : m_gameRenderTargets) {
          if (!t.resource || t.edramBase != d.resolveSourceBase) continue;
          // Counted before any shape test: "nothing shares this EDRAM base" and
          // "something does but no shape matched" are different answers, and
          // only the first means the surface is genuinely absent from the pool.
          if (&t != srcEntry) ++blankCandidates;
          if (t.everDrawn && t.format == snapFormat &&
              t.width == d.resolveSourceWidth * 2 &&
              t.height == d.resolveSourceHeight * 2) {
            msaaPartner = &t;
            continue;
          }
          if (t.width != d.resolveSourceWidth) continue;
          if (t.height == d.resolveSourceHeight) {
            // A same-extent twin is only an improvement if it was DRAWN into.
            // Taking the first one regardless would have picked 0x2653C860 --
            // the other blank 640x360 at this base -- and copied zeros again.
            if (t.everDrawn) {
              exact = &t;
              break;
            }
            if (!blank_exact) blank_exact = &t;
            continue;
          }
          if (t.height > d.resolveSourceHeight && t.everDrawn &&
              (!contains || t.height < contains->height))
            contains = &t;
        }
        GameRenderTarget* hit = exact ? exact : (msaaPartner ? msaaPartner
                                                             : contains);
        // Only when we had nothing at all. With a blank surface already in
        // hand, swapping it for a different blank surface is not progress, and
        // it would spend the containment match on a no-op.
        if (!hit && !srcRes) hit = blank_exact;
        if (hit) {
          srcEntry = hit;
          srcRes = hit->resource.Get();
          // The partner is twice the size in each axis, so its extent is its
          // own -- and the guest's source rectangle, which is in the 1x
          // coordinates it thinks it resolved, has to be doubled to match. The
          // snapshot then comes out at full resolution, and the shader's
          // normalized UVs map [0,1] across it exactly as they did across the
          // smaller image.
          srcScale = hit == msaaPartner ? 2u : 1u;
          srcWidth = d.resolveSourceWidth * srcScale;
          srcHeight = d.resolveSourceHeight * srcScale;
          srcState = hit->state;
          snapFormat = hit->format;
          if (exact)
            ++m_aliasedSourceResolves;
          else if (hit == msaaPartner)
            ++m_msaaPartnerResolves;
          else if (contains)
            ++m_containedSourceResolves;
          blankRescue = 3;
        } else {
          blankRescue = blankCandidates ? 2 : 1;
        }
      }
      // A BANDED depth resolve, which no object-identity lookup can satisfy.
      // The shadow pass renders 768x1024 as two EDRAM bands -- 768x640 at base
      // 0x580 and 768x384 at base 0x710 -- and then resolves the whole image
      // through a THIRD surface object that aliases band 0's base and that no
      // draw ever binds. Measured: both bands are in the depth pool and drawn,
      // and the object the resolve names (0x214C5130) is in neither.
      //
      // Stitch them by EDRAM base, which is the only thing that says which band
      // is on top. The bands must start at the resolve's own base and their
      // heights must add up to the destination exactly; anything else is not a
      // banding of this surface and is left to fail as before rather than
      // assembled on a guess.
      // The gather and its exact-cover test now live above, beside the source
      // lookup they compete with; `depthBands` is empty unless it passed.
      if (!srcRes && !depthBands.empty()) {
        const std::vector<GameRenderTarget*>& bands = depthBands;
        GameRenderTarget* snap =
            EnsureGameSnapshot(d.resolveDest, d.resolveDestWidth,
                               d.resolveDestHeight, DXGI_FORMAT_R32_FLOAT);
        if (snap) {
            D3D12_RESOURCE_BARRIER toDest = {};
            toDest.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            toDest.Transition.pResource = snap->resource.Get();
            toDest.Transition.StateBefore = snap->state;
            toDest.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            toDest.Transition.Subresource =
                D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            m_commandList->ResourceBarrier(1, &toDest);
            uint32_t dstY = 0;
            for (GameRenderTarget* b : bands) {
              D3D12_RESOURCE_BARRIER toSrc = {};
              toSrc.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
              toSrc.Transition.pResource = b->resource.Get();
              toSrc.Transition.StateBefore = b->state;
              toSrc.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
              toSrc.Transition.Subresource =
                  D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
              m_commandList->ResourceBarrier(1, &toSrc);
              D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
              dstLoc.pResource = snap->resource.Get();
              dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
              dstLoc.SubresourceIndex = 0;
              D3D12_TEXTURE_COPY_LOCATION srcLoc = dstLoc;
              srcLoc.pResource = b->resource.Get();
              D3D12_BOX box = {};
              box.right = b->width;
              box.bottom = b->height;
              box.back = 1;
              m_commandList->CopyTextureRegion(&dstLoc, 0, dstY, 0, &srcLoc,
                                               &box);
              dstY += b->height;
              // Straight back to DEPTH_WRITE: the next frame's shadow pass
              // renders into these again, and that is the state it expects.
              D3D12_RESOURCE_BARRIER back = toSrc;
              back.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
              back.Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
              m_commandList->ResourceBarrier(1, &back);
              b->state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
            }
            D3D12_RESOURCE_BARRIER toSrv = toDest;
            toSrv.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            toSrv.Transition.StateAfter =
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            m_commandList->ResourceBarrier(1, &toSrv);
            snap->state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            snap->everDrawn = true;
            snap->stale = false;
            snap->lastCopyFrame = m_gameFrame;
            ++m_snapshotCopies;
            ++m_depthResolves;
            ++m_depthBandResolves;
            continue;
        }
      }
      // Nothing to copy out of, and the guest still asked for this image. Make
      // the source exist, from the resolve's OWN extent, and copy the empty
      // surface rather than refusing -- which is what Xenia does, creating
      // render targets from the resolve's EDRAM info without consulting whether
      // a draw was ever seen (render_target_cache.cc:1393).
      //
      // Only for a DEPTH source with an extent to build from. A colour source
      // is deliberately left to the refusal below: it has an aliased-source
      // matcher above that is measured and works, and an invented empty colour
      // target would compete with it.
      if (!srcRes && d.resolveSourceIsDepth && d.resolveSourceWidth &&
          d.resolveSourceHeight) {
        if (GameRenderTarget* made = EnsureGameDepthTarget(
                d.resolveSource, d.resolveSourceWidth, d.resolveSourceHeight,
                d.resolveSourceBase)) {
          if (made->needsInitialClear) {
            made->needsInitialClear = false;
            if (made->state != D3D12_RESOURCE_STATE_DEPTH_WRITE) {
              D3D12_RESOURCE_BARRIER toDepth = {};
              toDepth.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
              toDepth.Transition.pResource = made->resource.Get();
              toDepth.Transition.StateBefore = made->state;
              toDepth.Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
              toDepth.Transition.Subresource =
                  D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
              m_commandList->ResourceBarrier(1, &toDepth);
              made->state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
            }
            auto dsv = m_gameDepthDsvHeap->GetCPUDescriptorHandleForHeapStart();
            dsv.ptr += SIZE_T(made->rtvIndex) * m_gameDsvDescriptorSize;
            m_commandList->ClearDepthStencilView(dsv, kGameDepthClearFlags,
                                                 1.0f, 0, 0, nullptr);
          }
          srcEntry = made;
          srcRes = made->resource.Get();
          srcWidth = made->width;
          srcHeight = made->height;
          srcState = made->state;
          snapFormat = kGameDepthResourceFormat;  // planar: must match the source
          ++m_depthResolves;
          ++m_resolveCreatedSources;
        }
      }
      if (!srcRes) {
        // The source has no offscreen surface — it was refused one (budget or a
        // size change), so it rendered into m_gameRT with everything else.
        //
        // Do NOT copy from m_gameRT as a fallback. It is cleared every frame and
        // accumulates every logical surface, so it does not hold the source's
        // contents at resolve time: measured, 25 of 33 resolves in a frame have
        // sources with no draws at all that frame, their contents established
        // earlier. Copying from it was tried and captured the clear colour —
        // the logo screen turned {0.05, 0.08, 0.18}.
        //
        // Refusing leaves the draw on the old aliased path, which is wrong but
        // is what it had before. A steady non-zero count here means targets are
        // being refused offscreen surfaces upstream — read the routing line.
        ++m_snapshotMissingSource;
        // Which sources, and how often. Counted rather than logged on first
        // sighting: the first miss for any source happens in the opening frames
        // before anything has drawn into it, so a once-per-source line reports
        // startup state as if it were the steady state -- which is exactly the
        // mistake that produced a confident and wrong "never a draw target".
        // The tally is dumped with the periodic counters, with the status read
        // at dump time.
        ++m_missingSourceCounts[d.resolveSource];
        // The guest asked for this image to be refreshed and it was not. Any
        // snapshot already held for this destination is now a stale earlier
        // frame; mark it so the sampling path refuses it rather than blitting a
        // previous frame over this one. Measured as the intro overlap: ~600
        // dropped refreshes per sample window with ZERO offscreen refusals, so
        // these are sources we have no entry for at all, not budget drops.
        if (auto st = m_gameSnapshots.find(d.resolveDest);
            st != m_gameSnapshots.end()) {
          st->second.stale = true;
        }
        continue;
      }
      // Snapshotting a target nothing has ever drawn into copies a blank
      // surface. The copy still happens — refusing it would freeze the previous
      // snapshot, which is worse — but it is counted, because a large number
      // here means compositor quads are painting blanks over the frame and the
      // real defect is upstream, in whatever should have rendered that target.
      if (srcEntry && !srcEntry->everDrawn) {
        ++m_snapshotBlankSource;
        // The population behind that count. Keyed by SOURCE extent, so one line
        // describes a surface instead of an event, and carrying the frame range
        // because the whole question is whether these are boot-only (the legal /
        // loading / start screens) or ongoing.
        auto& b = m_blankSourceByExtent[(uint64_t(d.resolveSourceWidth) << 32) |
                                        d.resolveSourceHeight];
        if (!b.count) b.firstFrame = m_gameFrame;
        b.lastFrame = m_gameFrame;
        ++b.count;
        b.object = d.resolveSource;
        b.edramBase = d.resolveSourceBase;
        b.format = uint32_t(srcEntry->format);
        b.dest = d.resolveDest;
        switch (blankRescue) {
          case 1: ++b.rescueNoCandidate; break;
          case 2: ++b.rescueAllBlank; break;
          case 3: break;  // rescued, yet still blank: the stand-in was blank too
          default: ++b.rescueNotAttempted; break;
        }
      }
      // Which part of the source this band takes. The guest's rectangle is in
      // the coordinates of the full image, not of the band's own surface, so a
      // band at y 640..720 arrives as a rectangle our 1280x80 source resource
      // cannot contain. Clamping and then falling back to the whole source is
      // what makes both conventions land correctly: a genuine sub-rectangle
      // survives the clamp, an out-of-range band one does not and takes its
      // whole surface — which is exactly the band.
      uint32_t sx = 0, sy = 0, copyW = srcWidth, copyH = srcHeight;
      // Scaled by srcScale, which is 1 for every source but a multisample
      // partner. The guest states its rectangle in the resolution it believes
      // it rendered; taking it unscaled off a 2x partner would copy the
      // top-left quarter of the scene and call it the whole image.
      if (d.resolveSrcX2 > d.resolveSrcX1 && d.resolveSrcY2 > d.resolveSrcY1 &&
          uint32_t(d.resolveSrcX2) * srcScale <= srcWidth &&
          uint32_t(d.resolveSrcY2) * srcScale <= srcHeight &&
          d.resolveSrcX1 >= 0 && d.resolveSrcY1 >= 0) {
        sx = uint32_t(d.resolveSrcX1) * srcScale;
        sy = uint32_t(d.resolveSrcY1) * srcScale;
        copyW = (uint32_t(d.resolveSrcX2) - uint32_t(d.resolveSrcX1)) * srcScale;
        copyH = (uint32_t(d.resolveSrcY2) - uint32_t(d.resolveSrcY1)) * srcScale;
      }
      const uint32_t dx = d.resolveDestX > 0 ? uint32_t(d.resolveDestX) : 0;
      const uint32_t dy = d.resolveDestY > 0 ? uint32_t(d.resolveDestY) : 0;
      // The snapshot must be the size of the DESTINATION TEXTURE, not of the
      // region this resolve happens to cover.
      //
      // Covering was right for a banded resolve, which eventually fills the
      // whole image, and wrong for an atlas, which never does: the menu scene's
      // 2048x2048 atlas is built from repeated 256x256 sub-rect resolves, so
      // the first one created a 256x256 snapshot. The shader samples a texture
      // the guest declares as 2048x2048, so normalized UVs map [0,1] across our
      // 256x256 resource — every fetch lands at 1/8 scale and anything packed
      // outside the top-left corner cannot be reached at all.
      //
      // The covered region is still the floor, so a destination whose extent we
      // could not decode behaves exactly as before rather than shrinking.
      const uint32_t snapW = std::max(d.resolveDestWidth, dx + copyW);
      const uint32_t snapH = std::max(d.resolveDestHeight, dy + copyH);
      GameRenderTarget* snap =
          EnsureGameSnapshot(d.resolveDest, snapW, snapH, snapFormat);
      if (!snap) continue;
      D3D12_RESOURCE_BARRIER pre[2] = {};
      pre[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      pre[0].Transition.pResource = srcRes;
      pre[0].Transition.StateBefore = srcState;
      pre[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
      pre[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
      pre[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      pre[1].Transition.pResource = snap->resource.Get();
      pre[1].Transition.StateBefore = snap->state;
      pre[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
      pre[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
      m_commandList->ResourceBarrier(2, pre);
      // A placed region copy, not CopyResource. CopyResource demands identical
      // dimensions, so it could only ever express "this band IS the whole
      // texture" — and the snapshot had to be resized to the band to satisfy
      // it, which is the bug. Copying the band to its own offset lets the two
      // bands of a tiled resolve assemble into one image.
      D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
      dstLoc.pResource = snap->resource.Get();
      dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      dstLoc.SubresourceIndex = 0;
      D3D12_TEXTURE_COPY_LOCATION srcLoc = dstLoc;
      srcLoc.pResource = srcRes;
      D3D12_BOX srcBox = {};
      srcBox.left = sx;
      srcBox.top = sy;
      srcBox.right = sx + copyW;
      srcBox.bottom = sy + copyH;
      srcBox.back = 1;
      m_commandList->CopyTextureRegion(&dstLoc, dx, dy, 0, &srcLoc, &srcBox);
      // Back to shader-resource for both: the source may be rendered into again
      // later in the same frame, and the snapshot is about to be sampled.
      // Put the source back where it was. An offscreen entry can go to
      // shader-resource and be transitioned again on demand, but m_gameRT must
      // return to RENDER_TARGET: the rest of the frame keeps drawing into it,
      // and PresentGameFrame's own RT->COPY_SOURCE barrier declares that as the
      // before-state. The snapshot goes to shader-resource either way — being
      // sampled is all it exists for.
      D3D12_RESOURCE_BARRIER post[2] = {pre[0], pre[1]};
      post[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
      post[0].Transition.StateAfter =
          srcEntry ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
                   : D3D12_RESOURCE_STATE_RENDER_TARGET;
      post[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
      post[1].Transition.StateAfter =
          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
      m_commandList->ResourceBarrier(2, post);
      if (srcEntry)
        srcEntry->state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
      snap->state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
      // Refreshed: whatever earlier drop marked this stale is now irrelevant.
      snap->stale = false;
      snap->lastCopyFrame = m_gameFrame;
      // A D3D9 frame resolves its completed backbuffer immediately before
      // VdSwap. Because resolves keep their guest order in m_gameDraws, the
      // last successful whole 1280x720 colour resolve is the frame to present.
      // Presenting d.resolveSource instead is incorrect: that object is shared
      // scratch storage and later post-processing draws may overwrite it after
      // the resolve has preserved the intended image.
      if (!d.resolveSourceIsDepth && snapW == 1280 && snapH == 720 && dx == 0 &&
          dy == 0 && copyW == 1280 && copyH == 720) {
        m_presentResolveTexture = d.resolveDest;
      }
      // The target is bound again by whichever draw follows; forcing a rebind
      // keeps that from being skipped because boundTargetObject still matches.
      boundTargetObject = 0xFFFFFFFFu;
      ++m_snapshotCopies;
      // The guest LOADS the 1x1 exposure result out of guest memory instead of
      // sampling it, so this one destination has to travel back to the CPU.
      // See DrainLuminanceReadback and mx::hle::g_luminanceReadbackBits.
      if (snapW == 1 && snapH == 1 &&
          (oneByOneSeen++ % kMaxLuminanceSlots) ==
              (m_gameFrame % kMaxLuminanceSlots))
        QueueLuminanceReadback(snap, d.resolveDest);
      // And the same for a small destination that is bigger than 1x1: the
      // terrain feedback buffer is 64x64 and read the same way. Gated on the
      // RESOLVE's extent, not the snapshot's, because the snapshot grows.
      QueueSurfaceReadback(snap, d.resolveDest, d.resolveDestWidth,
                           d.resolveDestHeight);
      continue;
    }
    // A full-surface D3D9 DEPTH clear, ordered among the draws.
    //
    // Before this the ONLY depth clear was the once-per-frame first-use one
    // further down, whose comment ("one depth surface serves several colour
    // targets in a pass, so clearing it with each of them would wipe what the
    // previous target established") is right WITHIN a pass and wrong ACROSS
    // passes -- the guest separates its passes with its own clears and we were
    // discarding every one. freeroam.rdc: ResourceId::384 Cleared once at event
    // 15183, then DepthStencilTarget for six passes with nothing between them.
    //
    // usedThisFrame is set here too, so this and the first-use clear cannot
    // both fire on the same target in the same frame -- the guest's clear wins
    // and ours becomes the fallback for a frame where the guest issues none.
    if (d.depthClear) {
      GameRenderTarget* dt = EnsureGameDepthTarget(d.depthObject, d.depthWidth,
                                                   d.depthHeight, d.depthBase);
      if (dt) {
        if (dt->state != D3D12_RESOURCE_STATE_DEPTH_WRITE) {
          D3D12_RESOURCE_BARRIER toDepth = {};
          toDepth.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
          toDepth.Transition.pResource = dt->resource.Get();
          toDepth.Transition.StateBefore = dt->state;
          toDepth.Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
          toDepth.Transition.Subresource =
              D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
          m_commandList->ResourceBarrier(1, &toDepth);
          dt->state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        }
        auto dsv = m_gameDepthDsvHeap->GetCPUDescriptorHandleForHeapStart();
        dsv.ptr += SIZE_T(dt->rtvIndex) * m_gameDsvDescriptorSize;
        // The GUEST'S OWN flags, not kGameDepthClearFlags. That constant is
        // right for our first-use clears, which initialise a fresh surface and
        // should touch both planes, and wrong here: the guest issues depth-only
        // (0x1F), stencil-only (0x20) and both (0x30), and clearing stencil
        // alongside every depth clear would wipe a mask it deliberately kept.
        // Harmless until something tests stencil, and the first thing to break
        // when something does.
        const D3D12_CLEAR_FLAGS clearFlags = D3D12_CLEAR_FLAGS(
            (d.clearDepthPlane ? D3D12_CLEAR_FLAG_DEPTH : 0) |
            (d.clearStencilPlane ? D3D12_CLEAR_FLAG_STENCIL : 0));
        m_commandList->ClearDepthStencilView(dsv, clearFlags, d.clearDepth,
                                             d.clearStencil, 0, nullptr);
        dt->usedThisFrame = true;
        dt->everDrawn = true;
        ++m_guestDepthClears;
      } else {
        ++m_guestDepthClearsUnresolved;
      }
      // Clear does not bind through the normal draw path. Force the following
      // draw to restore its RTV/DSV and viewport.
      boundTargetObject = 0xFFFFFFFFu;
      boundDepthObject = 0xFFFFFFFFu;
      continue;
    }
    // A full-surface D3D9 colour clear. This is an ordered command, not setup:
    // the guest may resolve the cleared target immediately with no draw in
    // between (the front-end default-texture atlas does exactly that).
    if (d.colorClear) {
      const bool wantsOffscreen =
          d.targetObject && d.targetWidth && d.targetHeight &&
          (resolveSources.contains(d.targetObject) ||
           sampledTargets.contains(d.targetObject) ||
           d.targetWidth != 1280 || d.targetHeight != 720);
      GameRenderTarget* clearTarget = nullptr;
      D3D12_CPU_DESCRIPTOR_HANDLE rtv = {};
      if (wantsOffscreen) {
        LogGuestColorFormat(d.targetObject, d.targetWidth, d.targetHeight,
                            d.targetColorFormat);
        clearTarget = EnsureGameRenderTarget(
            d.targetObject, d.targetWidth, d.targetHeight, d.targetBase,
            HostColorFormat(d.targetColorFormat));
        if (!clearTarget) continue;
        if (clearTarget->state != D3D12_RESOURCE_STATE_RENDER_TARGET) {
          D3D12_RESOURCE_BARRIER barrier = {};
          barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
          barrier.Transition.pResource = clearTarget->resource.Get();
          barrier.Transition.StateBefore = clearTarget->state;
          barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
          barrier.Transition.Subresource =
              D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
          m_commandList->ResourceBarrier(1, &barrier);
          clearTarget->state = D3D12_RESOURCE_STATE_RENDER_TARGET;
        }
        rtv = m_gameRtvHeap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += SIZE_T(clearTarget->rtvIndex) * m_gameRtvDescriptorSize;
      } else {
        rtv = m_gameRtvHeap->GetCPUDescriptorHandleForHeapStart();
      }
      const float packedRgba[4] = {
          float((d.clearColor >> 16) & 0xFFu) / 255.0f,
          float((d.clearColor >> 8) & 0xFFu) / 255.0f,
          float(d.clearColor & 0xFFu) / 255.0f,
          float((d.clearColor >> 24) & 0xFFu) / 255.0f};
      const float* rgba =
          d.clearColorIsFloat ? d.clearColorFloat.data() : packedRgba;
      m_commandList->ClearRenderTargetView(rtv, rgba, 0, nullptr);
      if (clearTarget) {
        clearTarget->usedThisFrame = true;
        clearTarget->everDrawn = true;
      }
      // Clear does not bind through the normal draw path. Force the following
      // draw to restore its RTV/DSV and viewport.
      boundTargetObject = 0xFFFFFFFFu;
      boundDepthObject = 0xFFFFFFFFu;
      continue;
    }

    // Keep only the unsampled final 1280x720 surface on m_gameRT so
    // PresentGameFrame remains an exact-size copy. A full-size scene target
    // that a later compositor samples is still offscreen and needs its own SRV;
    // classifying solely by dimensions made that target alias m_gameRT and
    // left the final draw with nothing it could legally sample.
    GameRenderTarget* drawTarget = nullptr;
    // Declared out here because the depth-state decision below needs it.
    GameRenderTarget* depthTarget = nullptr;
    const bool feedsLaterDraw =
        d.targetObject && sampledTargets.contains(d.targetObject);
    // A resolve source needs storage that outlives the frame. Measured: of 33
    // resolves in one frame, 25 had sources with no draws at all that frame —
    // their contents were established earlier. m_gameRT is cleared every frame,
    // so it can never hold them at resolve time, which is why snapshotting from
    // it produced the clear colour. Offscreen targets are only cleared when
    // something actually draws into them (usedThisFrame below), so they carry
    // contents forward, which is exactly the semantics a resolve source needs.
    // This was disabled for a while to isolate a white-screen regression. That
    // regression has since been traced to something else entirely — exempting
    // sampled_render_target_object from the colourless filter, which admitted
    // 4000 fullscreen quads that still painted opaque white (51f3c80). That
    // filter no longer exists, and this routing was never the cause.
    // A resolve source needs storage that outlives the frame. Measured: of 33
    // resolves in one frame, 25 had sources with no draws at all that frame —
    // their contents were established earlier. m_gameRT is cleared every frame
    // so it can never hold them at resolve time, which is why snapshotting from
    // it produced the clear colour. Offscreen targets are cleared only when
    // something draws into them, so they carry contents forward.
    //
    // This was bisected off while chasing a 3s/frame stall. The stall was a
    // descriptor leak in the snapshot path, not this — measured with routing
    // off and the stall still present.
    const bool isResolveSource =
        d.targetObject && resolveSources.contains(d.targetObject);
    // A DEPTH-ONLY pass has no colour target at all -- the shadow map is
    // 768x1024 with "colour target now 0x00000000" -- so d.targetObject is 0
    // and every one of its draws fell through to the main render target. Two
    // consequences, both measured: the shadow geometry overpainted the
    // backbuffer, and the depth surface (0x213DCC30) was never created, so the
    // guest's depth resolve out of it found no source. That missing snapshot
    // then discarded every draw sampling the shadow map at s15 -- 396 of them,
    // which is the whole 320x180 luminance pass, which is why the exposure
    // divides by zero.
    //
    // Route it like any other offscreen pass, keyed by the DEPTH object, with a
    // scratch colour target at the same extent. The scratch target is not
    // wasted: every PSO declares NumRenderTargets = 1, so binding no RTV at all
    // would be invalid work against a pipeline that expects one.
    const bool depthOnlyPass =
        !d.targetObject && d.depthObject && d.depthWidth && d.depthHeight;
    // A depth-only draw gets a colour attachment it never asked for, because
    // every PSO declares NumRenderTargets = 1. Population is every draw
    // considered; fires are the ones handed a scratch target.
    mx::gpu::guard::Note(mx::gpu::guard::Guard::kScratchColourTarget, depthOnlyPass);
    const uint32_t targetObject = depthOnlyPass ? d.depthObject : d.targetObject;
    const uint32_t targetWidth = depthOnlyPass ? d.depthWidth : d.targetWidth;
    const uint32_t targetHeight = depthOnlyPass ? d.depthHeight : d.targetHeight;
    const bool wantsOffscreen =
        targetObject && targetWidth && targetHeight &&
        (depthOnlyPass || feedsLaterDraw || isResolveSource ||
         targetWidth != 1280 || targetHeight != 720);
    if (wantsOffscreen) {
      // A depth-only pass has no colour format of its own; its scratch target
      // is never sampled, so RGBA8 is as good as anything.
      if (!depthOnlyPass)
        LogGuestColorFormat(targetObject, targetWidth, targetHeight,
                            d.targetColorFormat);
      drawTarget = EnsureGameRenderTarget(
          targetObject, targetWidth, targetHeight, d.targetBase,
          depthOnlyPass ? kBackBufferFormat
                        : HostColorFormat(d.targetColorFormat));
      // The last guest-backbuffer-sized target drawn into this frame is the
      // finished scene, and what present should show. Tracked by last write
      // rather than by object identity because which surface ends up on screen
      // is a property of draw order, not of any particular target.
      if (drawTarget && !depthOnlyPass && d.targetWidth == 1280 &&
          d.targetHeight == 720) {
        m_presentSourceObject = d.targetObject;
        // Present shows the LAST guest-backbuffer-sized surface written this
        // frame. That is only correct if there is exactly one. Seven 1280x720
        // surfaces are live, and if the guest builds the scene across several
        // and composites them, presenting one of them shows a single layer --
        // which is what a white frame with content on one band looks like.
        // Count the distinct ones per frame rather than assume either way.
        if (fullSizeTargets.insert(d.targetObject).second)
          fullSizeOrder.emplace_back(d.targetObject, 0);
        for (auto& e : fullSizeOrder)
          if (e.first == d.targetObject) ++e.second;
        ++fullSizeDraws;
      }
    }
    // The three populations, kept apart on purpose. A draw that never wanted an
    // offscreen target and one that wanted it and was refused both end up on
    // the main render target, and a single "drew on main" count cannot tell
    // them apart — but only the second is a silent regression to overpainting.
    if (!wantsOffscreen) ++m_rtDrawsMain;
    else if (drawTarget) ++m_rtDrawsOffscreen;
    else ++m_rtDrawsOverpaint;
    if (drawTarget) {
      if (drawTarget->state != D3D12_RESOURCE_STATE_RENDER_TARGET) {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = drawTarget->resource.Get();
        barrier.Transition.StateBefore = drawTarget->state;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        m_commandList->ResourceBarrier(1, &barrier);
        drawTarget->state = D3D12_RESOURCE_STATE_RENDER_TARGET;
      }
      // Depth for this offscreen target, at the target's own extent. The guest
      // pairs one depth surface with a colour target of the same size, and
      // sizing to the colour target is what keeps the DSV and RTV agreeing when
      // the guest's own depth extent is unreadable.
      // Sized from the DEPTH surface's own declared extent, never from the
      // colour target's.
      //
      // Sizing it from the colour target collapsed the frame -- 2664 offscreen
      // draws fell to 133, resolve copies rose from 1750 to 30166 with hits
      // falling to 19 -- because the same depth object then demanded two
      // different sizes and was retired and recreated on every alternation. It
      // demands two sizes because DeviceState is thread_local: a draw carries
      // whatever depth surface ITS thread last saw, which is not always the one
      // paired with the colour target it is drawing into.
      //
      // The guest itself pairs one depth surface per colour target at matching
      // extents, including a separate depth object for each EDRAM band
      // (1280x640 and 1280x80 have their own, distinct from the 1280x720). So
      // each depth object has exactly one size and this never resizes.
      //
      // Binding only on an exact extent match keeps that guarantee honest: a
      // stale pairing skips depth for that draw rather than binding a DSV whose
      // size disagrees with the RTV.
      //
      // Sized from the depth surface's OWN extent, which removed the resize
      // churn entirely (resized 13 -> 0) and let 3423 depth resolves run from 4
      // surfaces, with 0x2123C208 leaving the missing-source offenders. The
      // frame still collapses to ~125 offscreen draws and ~20 translated,
      // identically to the first attempt -- so neither the churn nor depth
      // testing is the cause, and routing (which precedes all depth state) is
      // what falls.
      depthTarget = (d.depthObject && d.depthWidth == drawTarget->width &&
                     d.depthHeight == drawTarget->height)
                        ? EnsureGameDepthTarget(d.depthObject, d.depthWidth,
                                                d.depthHeight, d.depthBase)
                        : nullptr;
      if (depthTarget && depthTarget->state !=
                             D3D12_RESOURCE_STATE_DEPTH_WRITE) {
        D3D12_RESOURCE_BARRIER toDepth = {};
        toDepth.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toDepth.Transition.pResource = depthTarget->resource.Get();
        toDepth.Transition.StateBefore = depthTarget->state;
        toDepth.Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        toDepth.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        m_commandList->ResourceBarrier(1, &toDepth);
        depthTarget->state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
      }
      // The DEPTH binding is part of what makes this pair current, not just the
      // colour target. Gating the rebind on the colour object alone let two
      // consecutive draws onto the same colour target keep the first one's
      // depth binding -- including the case where the first bound no DSV at all
      // and the second runs a depth-enabled PSO against it, which is invalid
      // work and hangs the device (DXGI_ERROR_DEVICE_HUNG at ~frame 75).
      const uint32_t wantDepthObject = depthTarget ? d.depthObject : 0;
      if (boundTargetObject != targetObject ||
          boundDepthObject != wantDepthObject) {
        auto rtv = m_gameRtvHeap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += SIZE_T(drawTarget->rtvIndex) * m_gameRtvDescriptorSize;
        D3D12_CPU_DESCRIPTOR_HANDLE dsv = {};
        if (depthTarget) {
          dsv = m_gameDepthDsvHeap->GetCPUDescriptorHandleForHeapStart();
          dsv.ptr += SIZE_T(depthTarget->rtvIndex) * m_gameDsvDescriptorSize;
        }
        // MRT SLOT 1. The guest's terrain tile pass binds two 256x256
        // targets and resolves the SECOND one; rendering only slot 0 left slot
        // 1 never drawn, its resolve dropped for want of a source, and the tile
        // shader's texture bind failing. See DrawCall::render_target1_object.
        //
        // The second target is ensured the same way the first is, and a failure
        // to get one falls back to single-target binding rather than dropping
        // the draw -- half the output beats none, and the PSO key below is
        // built from what was actually bound so the two cannot disagree.
        GameRenderTarget* drawTarget1 = nullptr;
        if (d.target1Object) {
          // HostColorFormat, exactly as the first target does it -- the
          // field carries the GUEST colour nibble, not a DXGI value.
          drawTarget1 = EnsureGameRenderTarget(
              d.target1Object, d.target1Width, d.target1Height, d.target1Base,
              HostColorFormat(d.target1ColorFormat));
        }
        if (drawTarget1) {
          D3D12_CPU_DESCRIPTOR_HANDLE rtvs[2] = {rtv, rtv};
          rtvs[1] = m_gameRtvHeap->GetCPUDescriptorHandleForHeapStart();
          rtvs[1].ptr +=
              SIZE_T(drawTarget1->rtvIndex) * m_gameRtvDescriptorSize;
          if (drawTarget1->state != D3D12_RESOURCE_STATE_RENDER_TARGET) {
            D3D12_RESOURCE_BARRIER toRt = {};
            toRt.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            toRt.Transition.pResource = drawTarget1->resource.Get();
            toRt.Transition.StateBefore = drawTarget1->state;
            toRt.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
            toRt.Transition.Subresource =
                D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            m_commandList->ResourceBarrier(1, &toRt);
            drawTarget1->state = D3D12_RESOURCE_STATE_RENDER_TARGET;
          }
          m_commandList->OMSetRenderTargets(2, rtvs, FALSE,
                                            depthTarget ? &dsv : nullptr);
          // Drawn, so its resolve has a source. This is the flag whose absence
          // read as `0x70105890 x3(drawn:N)` in the missing-source census.
          drawTarget1->usedThisFrame = true;
          drawTarget1->everDrawn = true;
          boundTarget1Format = drawTarget1->format;
          ++m_mrtDrawsBound;
        } else {
          m_commandList->OMSetRenderTargets(1, &rtv, FALSE,
                                            depthTarget ? &dsv : nullptr);
          boundTarget1Format = DXGI_FORMAT_UNKNOWN;
          if (d.target1Object) ++m_mrtSecondTargetMissing;
        }
        boundDepthObject = wantDepthObject;
        D3D12_VIEWPORT viewport = {};
        // See GameDraw::halfPixel. TopLeftX/Y are floats and a sub-pixel origin
        // is exactly the screen-space translation the reference performs on the
        // vertex, without a shader constant.
        viewport.TopLeftX = d.halfPixel;
        viewport.TopLeftY = d.halfPixel;
        // Does the guest's own viewport agree with the extent we are about
        // to use? Census only -- the extent is still the target's.
        if (!d.guestVpWidth || !d.guestVpHeight) {
          ++m_vpUnknown;
        } else if (d.guestVpWidth == drawTarget->width &&
                   d.guestVpHeight == drawTarget->height) {
          ++m_vpMatch;
        } else {
          ++m_vpMismatch;
          static std::map<uint64_t, uint64_t> s_rows;
          const uint64_t key = (uint64_t(d.guestVpWidth) << 48) |
                               (uint64_t(d.guestVpHeight) << 32) |
                               (uint64_t(drawTarget->width) << 16) |
                               uint64_t(drawTarget->height);
          if (++s_rows[key] == 1 && s_rows.size() <= 24) {
            char m[176];
            std::snprintf(m, sizeof(m),
                          "VIEWPORT MISMATCH: guest %ux%u, host uses target "
                          "%ux%u",
                          d.guestVpWidth, d.guestVpHeight, drawTarget->width,
                          drawTarget->height);
            LogInfo(m);
          }
        }
        // The guest's own viewport when it disagrees and the cvar is on;
        // otherwise the target extent, which is what every draw used before.
        const bool takeGuestVp = d.useGuestVp && d.guestVpWidth &&
                                 d.guestVpHeight &&
                                 (d.guestVpWidth != drawTarget->width ||
                                  d.guestVpHeight != drawTarget->height);
        viewport.Width =
            float(takeGuestVp ? d.guestVpWidth : drawTarget->width);
        viewport.Height =
            float(takeGuestVp ? d.guestVpHeight : drawTarget->height);
        if (takeGuestVp) ++m_vpTakenFromGuest;
        viewport.MinDepth = 0.0f;
        viewport.MaxDepth = 1.0f;
        // No scissor here: the per-draw block below owns the rectangle for
        // every draw, and a second setter would leave boundScissor describing
        // a state the command list is not in.
        m_commandList->RSSetViewports(1, &viewport);
        boundTargetObject = targetObject;
      }
      if (!drawTarget->usedThisFrame) {
        // An EDRAM takeover inherits the previous owner's contents INSTEAD of
        // being cleared. The clear is the thing that would destroy them, so
        // this has to be the same decision, not an extra step before it.
        bool inherited = false;
        if (d.edramCopy) {
          const auto pend = m_edramPendingSource.find(targetObject);
          if (pend != m_edramPendingSource.end()) {
            const uint32_t srcObject = pend->second;
            m_edramPendingSource.erase(pend);
            auto src = m_gameRenderTargets.find(srcObject);
            if (src == m_gameRenderTargets.end() || !src->second.resource) {
              ++m_edramTransferNoSource;
            } else if (!src->second.everDrawn) {
              // Nothing was ever rendered into it, so there is nothing to
              // inherit and a copy would only propagate its creation clear.
              ++m_edramTransferNotDrawn;
            } else if (src->second.width == drawTarget->width &&
                       src->second.height == drawTarget->height &&
                       src->second.format == drawTarget->format) {
              D3D12_RESOURCE_BARRIER pre[2] = {};
              pre[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
              pre[0].Transition.pResource = src->second.resource.Get();
              pre[0].Transition.StateBefore = src->second.state;
              pre[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
              pre[0].Transition.Subresource =
                  D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
              pre[1] = pre[0];
              pre[1].Transition.pResource = drawTarget->resource.Get();
              pre[1].Transition.StateBefore = drawTarget->state;
              pre[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
              m_commandList->ResourceBarrier(2, pre);
              m_commandList->CopyResource(drawTarget->resource.Get(),
                                          src->second.resource.Get());
              D3D12_RESOURCE_BARRIER post[2] = {pre[0], pre[1]};
              post[0].Transition.StateBefore =
                  D3D12_RESOURCE_STATE_COPY_SOURCE;
              post[0].Transition.StateAfter = src->second.state;
              post[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
              post[1].Transition.StateAfter =
                  D3D12_RESOURCE_STATE_RENDER_TARGET;
              m_commandList->ResourceBarrier(2, post);
              drawTarget->state = D3D12_RESOURCE_STATE_RENDER_TARGET;
              ++m_edramTransfers;
              inherited = true;
            } else {
              ++m_edramTransferNoSource;
            }
          }
        }
        // THE FIRST-USE CLEAR IS GONE. Deleted 2026-08-26, not gated -- this is
        // the first guard removed under docs/strict_mode.md rather than merely
        // measured.
        //
        // What it used to do: on the first use of a target in a frame, clear it
        // to kOffscreenClear unless something had already claimed the contents.
        // Nothing in the guest asked for that. It is correct for a target the
        // guest refills every frame and WRONG for an accumulation buffer -- the
        // terrain deformation ping-pong writes a Laplacian DELTA (`mask *
        // (blur(self) - self)`, Xenia's shader_D1A0A3F6AE7AD8B5) which only
        // integrates if the previous contents survive to be blended onto.
        // Cleared every frame it stayed at zero forever: min = max = 0 on the
        // 512x512 every terrain draw samples, which is what sand.rdc measured.
        //
        // WHY IT IS SAFE TO DELETE RATHER THAN DEFAULT-OFF. Run 1453, with the
        // preserve path forced on: the guard was given 27,316 opportunities and
        // needed NONE of them -- 0/27316 in the guard census -- while the frame
        // still rendered 1720 frames at 797 guest draws accepted, 0 refused, no
        // crash, and the 512x512 went from blank to `52 stale`, content
        // surviving across frames as the ping-pong requires. Riding and looking
        // back showed the tracks. A guard reading 0/N with N that large has no
        // fallback role left; keeping it behind a cvar would only preserve the
        // option of reintroducing a known defect.
        //
        // The guest issues its own clears. Colour routes through AddGameClear
        // and, since the depth-clear commit, so does depth -- so there is no
        // longer a gap for this to cover.
        //
        // `m_targetCarriedContent` is KEPT and now counts what it always
        // measured: targets whose previous-frame content survives into this
        // frame. That is a fact about the workload, not about a guard, and it
        // is how a reintroduced clear would be spotted.
        if (drawTarget->everDrawn) {
          ++m_targetCarriedContent;
          static std::unordered_set<uint32_t> s_carried;
          if (s_carried.size() < 32 && s_carried.insert(targetObject).second) {
            REXLOG_INFO(
                "d3d12: target carries previous-frame content: "
                "target 0x{:08X} {}x{} -- PRESERVED",
                targetObject, drawTarget->width, drawTarget->height);
          }
        }
        drawTarget->usedThisFrame = true;
        drawTarget->everDrawn = true;
      }
      // Depth is cleared on its own schedule: one depth surface serves several
      // colour targets in a pass, so clearing it with each of them would wipe
      // what the previous target established.
      if (depthTarget && !depthTarget->usedThisFrame) {
        D3D12_CPU_DESCRIPTOR_HANDLE dsv =
            m_gameDepthDsvHeap->GetCPUDescriptorHandleForHeapStart();
        dsv.ptr += SIZE_T(depthTarget->rtvIndex) * m_gameDsvDescriptorSize;
        m_commandList->ClearDepthStencilView(dsv, kGameDepthClearFlags, 1.0f,
                                             0, 0, nullptr);
        depthTarget->usedThisFrame = true;
        depthTarget->everDrawn = true;
      }
    } else if (boundTargetObject != 0) {
      auto rtv = m_gameRtvHeap->GetCPUDescriptorHandleForHeapStart();
      auto dsv = m_gameDsvHeap->GetCPUDescriptorHandleForHeapStart();
      m_commandList->OMSetRenderTargets(1, &rtv, FALSE,
                                        m_gameDepth ? &dsv : nullptr);
      // One RTV again. Without this a draw that followed an MRT pair kept
      // rtvFormat1 set and asked for a two-target PSO against one bound RTV.
      boundTarget1Format = DXGI_FORMAT_UNKNOWN;
      // A local copy: m_viewport is also used by the present blit, which draws
      // our own quad and must not be shifted.
      D3D12_VIEWPORT mainViewport = m_viewport;
      mainViewport.TopLeftX += d.halfPixel;
      mainViewport.TopLeftY += d.halfPixel;
      m_commandList->RSSetViewports(1, &mainViewport);
      boundTargetObject = 0;
      // The main target binds m_gameDepth, which is not one of the per-object
      // depth surfaces, so the offscreen path must treat this as "not mine".
      boundDepthObject = 0;
    }

    // THE GUEST'S SCISSOR.
    //
    // Ignoring it drew the compass strip across the whole frame: the guest
    // clips it to a window in the middle of the screen, and every label on the
    // strip was visible instead of the three that fit. Anything else the guest
    // clips to a sub-rectangle -- bars, wipes, masked panels -- was equally
    // unclipped, so this is not one widget's bug.
    //
    // The guest rectangle is in its own render-target pixels. Off-screen
    // targets are created at the guest's dimensions, so it maps 1:1 there; the
    // main target is letterboxed into the window by m_viewport, so the same
    // linear map the viewport applies is applied here. Both are then
    // INTERSECTED with the full-target rectangle rather than replacing it,
    // which is what makes a stale scissor from a larger target harmless: it can
    // only ever shrink what is already allowed, never open the letterbox bars.
    {
      const D3D12_RECT full =
          drawTarget ? D3D12_RECT{0, 0, LONG(drawTarget->width),
                                  LONG(drawTarget->height)}
                     : m_scissorRect;
      D3D12_RECT want = full;
      if (d.scissorSeen) {
        LONG l, t, r, b;
        if (drawTarget) {
          l = LONG(d.scissorLeft);
          t = LONG(d.scissorTop);
          r = LONG(d.scissorRight);
          b = LONG(d.scissorBottom);
        } else {
          const float sx = m_viewport.Width / kGuestWidth;
          const float sy = m_viewport.Height / kGuestHeight;
          l = LONG(std::floor(m_viewport.TopLeftX + float(d.scissorLeft) * sx));
          t = LONG(std::floor(m_viewport.TopLeftY + float(d.scissorTop) * sy));
          r = LONG(std::ceil(m_viewport.TopLeftX + float(d.scissorRight) * sx));
          b = LONG(std::ceil(m_viewport.TopLeftY + float(d.scissorBottom) * sy));
        }
        want.left = std::max(full.left, l);
        want.top = std::max(full.top, t);
        want.right = std::min(full.right, r);
        want.bottom = std::min(full.bottom, b);
        // An empty or inverted rectangle is not a reason to draw everything.
        // The guest asking for nothing means nothing, and D3D12 rejects a
        // rectangle whose right is below its left, so it is collapsed rather
        // than widened.
        if (want.right < want.left) want.right = want.left;
        if (want.bottom < want.top) want.bottom = want.top;
        if (want.left != full.left || want.top != full.top ||
            want.right != full.right || want.bottom != full.bottom)
          ++m_scissorClipped;
      } else {
        ++m_scissorUnreadable;
      }
      if (want.left != boundScissor.left || want.top != boundScissor.top ||
          want.right != boundScissor.right ||
          want.bottom != boundScissor.bottom) {
        m_commandList->RSSetScissorRects(1, &want);
        boundScissor = want;
      }
    }

    uint32_t textureDescriptor = 0;
    bool textured = false;
    if (d.sampledTargetObject) {
      // Prefer the snapshot taken when the guest resolved into this specific
      // texture.
      //
      // The snapshot lookup deliberately runs even when the draw's own target
      // is the one that was resolved from. The old `sampled != target` guard
      // existed because a resource cannot be read and written in the same
      // draw — but a snapshot is a separate resource captured earlier, so that
      // hazard is gone, and the guard was rejecting the common case: one shared
      // scratch surface is both what the draw renders into and what it samples
      // a previous resolve of. With the guard in place this measured hits 0.
      //
      // The fallback keeps the old live-surface path, under the old guard,
      // because it is still a read-write hazard. It is wrong whenever more than
      // one texture resolves out of that target, but it is what draws got
      // before snapshots existed, so it beats binding nothing and turning them
      // black. A large steady m_snapshotFallbacks means resolves are being
      // dropped upstream.
      GameRenderTarget* sampledPtr = nullptr;
      //
      // A STALE snapshot is refused outright. It holds a complete earlier frame
      // at full screen size, so binding it does not degrade the draw — it
      // replaces the frame. Falling through to the untextured path instead lets
      // the fabricated-colour gate below drop the draw, which shows what is
      // underneath: incomplete, but not a previous frame painted over the
      // current one.
      // A DEPTH snapshot is not a colour source. The tex*col stand-in has one
      // texture and multiplies it by the vertex colour, so binding an
      // R32_FLOAT depth image gives (depth, 0, 0, 1) * white -- a flat red
      // sheet over the frame, which is exactly what appeared over the menu the
      // first time depth resolves started succeeding. Whatever the guest's real
      // shader does with depth, the stand-in cannot express it; falling through
      // leaves the draw to the fabricated-colour gate, which shows what is
      // underneath instead of painting depth over it.
      const bool depthSnapshot =
          [&] {
            auto s = m_gameSnapshots.find(d.sampledTextureObject);
            return s != m_gameSnapshots.end() && s->second.resource &&
                   s->second.resource->GetDesc().Format ==
                       DXGI_FORMAT_R32_FLOAT;
          }();
      if (depthSnapshot) ++m_standInDepthSnapshotRefused;
      if (auto snap = m_gameSnapshots.find(d.sampledTextureObject);
          d.sampledTextureObject && !depthSnapshot &&
          snap != m_gameSnapshots.end() && !snap->second.stale) {
        sampledPtr = &snap->second;
        // The stand-in path bypasses BindTranslatedTextures, so it has to stamp
        // its own use or EvictGameSnapshots would reclaim snapshots that only
        // this path ever samples.
        snap->second.lastUsedFrame = m_gameFrame;
        ++m_snapshotHits;
        if (snap->second.width * 2 >= m_width) {
          const uint64_t age = m_gameFrame - snap->second.lastCopyFrame;
          ++m_snapshotAge[age == 0   ? 0
                          : age == 1 ? 1
                          : age < 10 ? 2
                          : age < 100 ? 3
                                      : 4];
        }
      } else if (d.sampledTextureObject && !depthSnapshot &&
                 m_gameSnapshots.count(d.sampledTextureObject)) {
        // Not counted for a depth snapshot: that refusal has its own counter
        // and is deliberate, whereas STALE-REFUSED means a resolve was dropped
        // and the image is a known-wrong earlier frame. Letting the depth
        // refusals fall through here made both read 233 and made a healthy
        // number look like 233 lost refreshes.
        ++m_snapshotStaleRefused;
      } else if (d.sampledTargetObject != d.targetObject) {
        if (auto it = m_gameRenderTargets.find(d.sampledTargetObject);
            it != m_gameRenderTargets.end()) {
          sampledPtr = &it->second;
          ++m_snapshotFallbacks;
        }
      }
      if (sampledPtr) {
        auto& sampled = *sampledPtr;
        if (sampled.state != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) {
          D3D12_RESOURCE_BARRIER barrier = {};
          barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
          barrier.Transition.pResource = sampled.resource.Get();
          barrier.Transition.StateBefore = sampled.state;
          barrier.Transition.StateAfter =
              D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
          barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
          m_commandList->ResourceBarrier(1, &barrier);
          sampled.state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        }
        textureDescriptor = sampled.srvIndex;
        textured = true;
        static uint64_t s_resolved_hits = 0;
        if (++s_resolved_hits <= 16 || (s_resolved_hits % 1000) == 0) {
          char message[160];
          std::snprintf(message, sizeof(message),
                        "game resolved-target sample hit: object 0x%08X "
                        "descriptor %u (hit %llu)",
                        d.sampledTargetObject, textureDescriptor,
                        static_cast<unsigned long long>(s_resolved_hits));
          LogInfo(message);
        }
      }
    }
    // Bink's plane set takes precedence: it is several textures at once and
    // cannot go through the single-descriptor path below.
    bool yuv = false;
    uint32_t yuvDescriptorBase = kYuvPlaneDescriptorBase;
    if (d.yuvComposite && EnsureYuvPlanes(d, yuvDescriptorBase)) {
      yuv = true;
      textured = true;
      textureDescriptor = yuvDescriptorBase;
    }
    if (!textured)
      textured = EnsureGameTexture(d.texture, textureDescriptor);

    // Fabricated colour with no texture to modulate: do not draw it at all.
    //
    // This is what paints the screen white. The untextured PSO returns the
    // vertex colour unmodified, and a draw whose colour was meant to come from
    // a texture has no COLOR element in its declaration, so BuildHleDraw seeds
    // {1,1,1,1} (d3d9_draw.cpp). That seed is correct as a MODULATION IDENTITY
    // — kGameTexturePS computes tex * col, and zeroing it killed the logo
    // (0f66860) — but when the multiply never happens it is emitted literally,
    // as opaque white. These draws are geometrically exact fullscreen quads
    // (measured: 4.3 verts, 100% coverage, 2.01 ndc extent, 51f3c80), so each
    // is a white rectangle over the whole frame.
    //
    // The gate is the FABRICATION, not the reason the texture is missing. A
    // first attempt keyed on sampledTargetObject — "meant to sample a resolve
    // result, found none" — and it was too narrow by an order of magnitude:
    // measured in the menu, 123 draws land on the presented surface, 94 of them
    // untextured, but only about 4 per frame carry a sampled target. The other
    // ninety have no texture of any kind, mostly because the guest format was
    // rejected upstream, and they went on painting white.
    //
    // kPacked and kFallback colours are real vertex data and are left alone
    // even when untextured; only kNone is invented here.
    //
    // The comment above reasons that binding nothing turns such draws black. It
    // does not — colourless means white — which is why this read as an
    // overpaint problem for so long.
    //
    // Skipping shows what is underneath: incomplete, but honest. A steady count
    // is a real upstream defect (a dropped resolve, a rejected texture format),
    // and this only stops that defect from being painted over everything.
    // Decided here, applied below once it is known whether the guest's own
    // shader will run. `textured` describes only d.texture, the ONE texture the
    // stand-in samples; a translated draw carries its textures in
    // pixelTextures and binds them itself, so this says nothing about it.
    //
    // Applying the skip here cost the menu its whole post-processing chain.
    // Measured on mx_806: every skipped draw was translated -- 76 aimed at the
    // 320x180 luminance target, 74 at each of two bloom targets, 247 at the
    // scene. That left the luminance target cleared and never drawn into, so
    // the guest measured an average scene luminance of zero, computed its
    // auto-exposure as key/0 = +Infinity, and the composite turned the frame
    // to NaN. The white menu backdrop was this gate.
    //
    // A depth-only pass is exempt. Its draws have no colour source and no
    // texture BY DESIGN -- that is what a shadow-map pass is -- and their
    // colour output goes to a scratch target nothing samples. Skipping them
    // would leave the depth surface empty, which is the whole thing this pass
    // exists to fill.
    // A draw with its colour mask off is exempt, and this is the clause the two
    // earlier reverts were missing. Both of those exempted draws by WHY they
    // lost their translation, which is not a property that says anything about
    // what they paint; this exempts them by whether they can paint at all.
    //
    // The population is the guest's depth passes: 60,000 draws in mx_1098 with
    // NO pixel shader, because SetPixelShader(NULL) is legal for a pass that
    // writes only depth -- one 48-dword program, writes_position 1, export 0x0.
    // They bind a COLOUR target as well as depth (extents only the 1280x640 and
    // 1280x80 EDRAM scene bands), so they fail depthOnlyPass, which requires
    // !d.targetObject. But RB_COLOR_MASK is 0 for every one of them measured
    // ("WOULD PAINT 0, masked off 22894"), and colorWrite already carries that.
    //
    // Skipping them threw away their DEPTH write, and the scene draws that
    // depth-test against it then rendered wrong -- which is why letting them
    // through brought the menu rider and bike back into the 1280x640 band
    // (white-menu.rdc) even though these draws paint nothing themselves.
    //
    // Fabricating white requires writing colour. These cannot.
    // CULL census, at PSO-SELECTION time rather than where cullMode is stored.
    //
    // The guest-side probe already proves the register reads 0x00018006
    // (cull_back) for the draw that blacks out the menu background. What that
    // cannot show is whether the value survives the trip through
    // graphics_system -> AddGameDraw -> the PSO key, and honouring the cull mode
    // changed nothing on screen -- so the question is precisely where between
    // those two points it is lost, if it is.
    //
    // Counts the PACKED bits (PackCullBits), because those are what the key
    // carries and what ApplyCullBits consumes; counting the raw register would
    // re-measure what is already known. Reported unconditionally, all eight
    // buckets, so an all-zero histogram is distinguishable from no report.
    {
      static std::atomic<uint64_t> s_cullBuckets[8]{};
      static std::atomic<uint64_t> s_cullDraws{0};
      ++s_cullBuckets[PackCullBits(d.cullMode) & 7u];
      const uint64_t n = ++s_cullDraws;
      if ((n % 20000) == 0) {
        char line[224];
        std::snprintf(line, sizeof(line),
                      "CULL census %llu draws: none %llu/%llu front %llu/%llu "
                      "back %llu/%llu (second of each pair = front-CCW)",
                      (unsigned long long)n,
                      (unsigned long long)s_cullBuckets[0].load(),
                      (unsigned long long)s_cullBuckets[4].load(),
                      (unsigned long long)s_cullBuckets[1].load(),
                      (unsigned long long)s_cullBuckets[5].load(),
                      (unsigned long long)s_cullBuckets[2].load(),
                      (unsigned long long)s_cullBuckets[6].load());
        LogInfo(line);
      }
      // The one draw this is all about, named on its own line. The aggregate
      // above can be healthy while THIS draw still arrives with cullMode 0, and
      // an aggregate cannot show that -- 128 draws a frame would drown it.
      // One line per distinct (raw register, packed bits, translated) tuple.
      if (d.indexCount == 35) {
        static std::mutex s_mu;
        static std::set<uint64_t> s_seen;
        const uint64_t k = (uint64_t(d.cullMode) << 8) |
                           (uint64_t(PackCullBits(d.cullMode)) << 1) |
                           (d.translated ? 1u : 0u);
        bool fresh = false;
        {
          std::lock_guard<std::mutex> lk(s_mu);
          fresh = s_seen.size() < 16 && s_seen.insert(k).second;
        }
        if (fresh) {
          char l2[224];
          std::snprintf(l2, sizeof(l2),
                        "CULL 35-index draw: raw 0x%08X -> packed %u "
                        "(mode %u, frontCCW %u); translated %d ps 0x%08X",
                        d.cullMode, PackCullBits(d.cullMode),
                        PackCullBits(d.cullMode) & 3u,
                        (PackCullBits(d.cullMode) >> 2) & 1u,
                        d.translated ? 1 : 0, d.pixelShaderHandle);
          LogInfo(l2);
        }
      }
    }

    const bool fabricatedWhite =
        !depthOnlyPass && d.colorWrite &&
        d.colorSource == uint8_t(mx::hle::DrawCall::ColorSource::kNone) &&
        !textured;

    // Depth state is decided the same way for both paths, so it is computed
    // before the split rather than duplicated inside it.
    // Offscreen draws used to force depth off because they had no attachment.
    // They can have one now, so the guest's own depth state is honoured on both
    // paths; a draw whose depth surface could not be created still falls back
    // to no depth rather than binding a DSV that does not exist.
    const bool tDepthEnable =
        (drawTarget ? depthTarget != nullptr : true) && d.depthEnable;
    const bool tDepthWrite = tDepthEnable && d.depthWrite;

    // Run the guest's own pixel shader, when this draw has everything it needs:
    // a translated shader, its interpolators, and its constant bank. Anything
    // missing keeps the tex*col stand-in rather than rendering a guess.
    // The group every pipeline for this draw must declare. Computed once, above
    // the split, so the translated and stand-in paths cannot disagree about it —
    // they already did about nothing else, and a disagreement here is invisible
    // except as a draw that does not appear.
    const D3D12_PRIMITIVE_TOPOLOGY_TYPE topoType = TopologyTypeOf(d.topology);

    ID3D12PipelineState* translatedPso = nullptr;
    if (d.translated) {
      TranslatedKey key;
      key.handle = d.pixelShaderHandle;
      key.vsHandle = d.gpuVertex ? d.vertexShaderHandle : 0;
      key.src = d.srcBlend;
      key.dest = d.destBlend;
      key.op = d.blendOp;
      key.rtvFormat = drawTarget ? drawTarget->format : kBackBufferFormat;
      // From what was bound, never from d.target1Object: if the second target
      // could not be ensured we bound one RTV, and a PSO declaring two would
      // not match it.
      key.rtvFormat1 = boundTarget1Format;
      key.topoType = topoType;
      key.stencilIndex = d.stencilIndex;
      key.flags = uint8_t((tDepthEnable ? 1u : 0u) |
                          (tDepthWrite ? 2u : 0u) |
                          (d.colorWrite ? 0u : 4u) |
                          (d.blendEnable ? 8u : 0u) |
                          (d.gpuVertexFetch ? 16u : 0u) |
                          (PackCullBits(d.cullMode) << 5));
      // A depth pass has no guest pixel shader to compile, so it takes the
      // stand-in. key.handle is 0 for these — no real shader has that handle,
      // so m_translatedPsBlobs caches exactly one compilation of it for the
      // whole run, and the rest of the key still separates them by vertex
      // shader, blend, topology and target format the way it does for any other
      // pixel stage.
      static const std::string kDepthOnlyPs{
          mx::gfx::shaders::kTranslatedDepthOnlyPS};
      translatedPso = TranslatedPSO(
          key, d.depthOnlyStandIn ? kDepthOnlyPs : *d.pixelShaderHlsl, d);
    }
    // Every texture the shader reads must be bindable, or the draw falls back:
    // a shader sampling a descriptor that was never written reads whatever is
    // there, which is a confident wrong answer rather than a visible failure.
    D3D12_GPU_DESCRIPTOR_HANDLE translatedSrvTable = {};
    if (translatedPso && !BindTranslatedTextures(d, translatedSrvTable))
      translatedPso = nullptr;
    // Counted HERE rather than where the alpha state arrives, because the
    // question is not "does this draw have an alpha test" but "did the path it
    // ended up on run one". translatedPso is only final after the texture bind
    // above has had its chance to revoke it.
    if ((d.alphaControl >> 3) & 1u) {
      if (translatedPso && !d.depthOnlyStandIn)
        ++m_alphaTestHonoured;
      else
        ++m_alphaTestStandIn;
    }
    // Now that the translated path has had its chance, a draw still heading for
    // the untextured stand-in with an invented colour is the fabricated white
    // the guard was written for. See the note beside fabricatedWhite.
    //
    // TRIED AND REVERTED (mx_960): exempting draws that HAD a translation and
    // lost it in BindTranslatedTextures, on the argument that they are not
    // "an invented colour" but a draw we chose to discard. They are exactly an
    // invented colour. A draw whose texture binding failed has no colour source
    // and no texture, so the stand-in paints it white -- and letting the 354
    // such draws on the scene target through turned the whole menu backdrop
    // white and buried the rider and bike under it. WHITE-SKIPPED fell 451 -> 76
    // and the picture got worse; the counter was measuring the draws starting to
    // render, not starting to render correctly.
    // NARROW EXEMPTION, for the terrain tile pass. A blanket version of this
    // was tried and reverted (mx_960, see the note above): letting every draw
    // that HAD a translation and lost it through turned the whole menu backdrop
    // white and buried the rider and bike. That failure was on the SCENE
    // TARGET -- 1280x640 and 1280x80 -- where a fabricated white covers
    // everything behind it.
    //
    // The terrain tile draws are not on the scene target. They render into a
    // 256x256 offscreen surface (0x2653F020) that is then resolved into the
    // 2048x2048 terrain atlas at 0x1A2E3000, and skipping them is why that
    // atlas is empty on all 64 tiles and why the ground is black:
    //
    //   WHITE-SKIPPED target 256x256 obj 0x2653F020:
    //     3 draws, 3 translated, 3 wanted sampler slots
    //
    //   resolve dest ... (phys 0x1A2E3000) 2048x2048
    //     <- ... from surface 0x2653F020 (256x256)   x3 resolves
    //
    // Three skipped draws, three resolves. So: exempt ONLY small offscreen
    // targets, which cannot be a scene band, and count them separately so the
    // trade stays visible. A fabricated-white TILE is wrong too -- the real fix
    // is upstream, in whatever makes the texture bind fail for ps 0x216866E0 --
    // but a white ground and a black one are both wrong and only one of them
    // proves the chain.
    if (!translatedPso && fabricatedWhite && d.translated &&
        d.targetWidth <= 512 && d.targetHeight <= 512) {
      ++m_whiteAllowedOffscreen;
      static std::mutex s_amu;
      static std::set<uint32_t> s_aseen;
      bool afresh = false;
      {
        std::lock_guard<std::mutex> lk(s_amu);
        afresh = s_aseen.size() < 8 && s_aseen.insert(d.targetObject).second;
      }
      if (afresh) {
        char aline[192];
        std::snprintf(aline, sizeof(aline),
                      "WHITE-ALLOW (offscreen): target 0x%08X %ux%u ps 0x%08X "
                      "samplers %u -- letting it draw instead of skipping",
                      d.targetObject, d.targetWidth, d.targetHeight,
                      d.pixelShaderHandle, d.pixelSamplerCount);
        LogInfo(aline);
      }
    } else if (!translatedPso && fabricatedWhite) {
      ++m_sampleMissSkipped;
      // DIAG: what the skipped draws are aimed at.
      auto& e =
          m_skipByTarget[(uint64_t(d.targetWidth) << 32) | d.targetHeight];
      ++e.count;
      e.object = d.targetObject;
      if (d.translated) ++e.translated;
      if (d.pixelSamplerCount) ++e.wantedSlots;
      // PROBE: WHICH draws are still being skipped on the two scene bands.
      //
      // After the colorWrite clause the menu goes to zero on 1280x640 / 1280x80
      // but freeroam keeps 65-79 per interval, and the counters cannot say what
      // they are. The guest-side probe reports the whole null-PS colour
      // population as "WOULD PAINT 0, masked off 29436, mask unreadable 0", so
      // every one of them should already be exempt -- these are something else,
      // and a tally across four separately-sampled counters cannot identify it.
      // A colorMaskKnown flag was tried on the theory that the mask was simply
      // unobserved for them; it changed nothing, because "mask unreadable 0"
      // had already ruled that out and I read past it. Reverted.
      //
      // One line per distinct (target, shader), so a handful of lines names the
      // population instead of 65 copies of one draw.
      {
        static std::mutex s_mu;
        static std::set<uint64_t> s_seen;
        const uint64_t key =
            (uint64_t(d.targetObject) << 32) | d.pixelShaderHandle;
        bool fresh = false;
        {
          std::lock_guard<std::mutex> lk(s_mu);
          fresh = s_seen.size() < 24 && s_seen.insert(key).second;
        }
        if (fresh) {
          char line[256];
          std::snprintf(line, sizeof(line),
                        "WHITE-SKIP WHO: target 0x%08X %ux%u ps 0x%08X "
                        "colorWrite %d colorSource %u textured %d translated %d "
                        "samplers %u depth %d blend %d",
                        d.targetObject, d.targetWidth, d.targetHeight,
                        d.pixelShaderHandle, d.colorWrite ? 1 : 0,
                        unsigned(d.colorSource), d.texture ? 1 : 0,
                        d.translated ? 1 : 0, d.pixelSamplerCount,
                        d.depthEnable ? 1 : 0, d.blendEnable ? 1 : 0);
          LogInfo(line);
        }
      }
      continue;
    }
    // WHICH PATH does a stencil draw actually take, and is a DSV bound when it
    // gets there? Counted per path: a stencil draw on a path with no depth
    // attachment cannot be tested however correct its pipeline is.
    if (d.stencilIndex) {
      if (translatedPso) ++m_stencilViaTranslated;
      else ++m_stencilViaStandIn;
      if (!depthTarget) ++m_stencilNoDsv;
    }
    if (translatedPso) {
      m_commandList->SetGraphicsRootSignature(m_translatedRootSig.Get());
      // Per draw, not pipeline state -- same reason as the stand-in path, and
      // it has to be set on THIS path too or a translated stencil draw
      // inherits whatever reference the previous draw left behind.
      if (d.stencilIndex) m_commandList->OMSetStencilRef(d.stencil.ref);
      // The block heap is a different heap from the stand-in path's, so it has
      // to be bound alongside the sampler heap for this draw.
      ID3D12DescriptorHeap* theaps[] = {m_translatedSrvHeap.Get(),
                                        m_samplerHeap.Get()};
      m_commandList->SetDescriptorHeaps(2, theaps);
      m_commandList->SetPipelineState(translatedPso);
      // b0 vertex. Two different buffers for the two vertex stages, at the one
      // register each declares: the guest's own VERTEX constant bank when its
      // shader is running, and the per-draw transform when the passthrough
      // stage is — that one does not read b0 at all, but the root signature
      // requires a bound CBV either way.
      const D3D12_GPU_VIRTUAL_ADDRESS tcb =
          d.gpuVertex ? d.vscb.gpu
          : d.cb      ? d.cb.gpu
                      : m_gameCB->GetGPUVirtualAddress();
      m_commandList->SetGraphicsRootConstantBufferView(0, tcb);
      // b1 pixel: the guest's own pixel constant bank.
      m_commandList->SetGraphicsRootConstantBufferView(1, d.pscb.gpu);
      m_commandList->SetGraphicsRootDescriptorTable(2, translatedSrvTable);
      // One sampler per slot. This used to offset a four-descriptor heap by a
      // single per-draw variant index while the root signature's sampler range
      // declared sixteen — so slot 1 of a multi-sampler shader read the next
      // variant along and slot 4 onwards read off the end of the heap.
      D3D12_GPU_DESCRIPTOR_HANDLE samp = {};
      if (BindTranslatedSamplers(d, samp))
        m_commandList->SetGraphicsRootDescriptorTable(3, samp);
      // The vertex stage's own tables, at t17+/s16+. Bound only when its shader
      // samples: root parameters a shader does not reference need no binding,
      // and the overwhelming majority of draws have no sampling vertex stage.
      //
      // Both must succeed or neither is bound. A shader that declares textures
      // with only its samplers bound reads undefined descriptors, which is the
      // confident-wrong-answer failure the slot fill is all-or-nothing to avoid.
      if (d.vertexSamplerCount) {
        D3D12_GPU_DESCRIPTOR_HANDLE vsSrv = {};
        D3D12_GPU_DESCRIPTOR_HANDLE vsSamp = {};
        if (BindTranslatedTextures(d, vsSrv, /*vertex=*/true) &&
            BindTranslatedSamplers(d, vsSamp, /*vertex=*/true)) {
          m_commandList->SetGraphicsRootDescriptorTable(5, vsSrv);
          m_commandList->SetGraphicsRootDescriptorTable(6, vsSamp);
          ++m_vertexSampledDraws;
        } else {
          ++m_vertexSampleBindFailed;
        }
      }
      m_commandList->IASetPrimitiveTopology(d.topology);
      if (d.gpuVertexFetch) {
        // No vertex buffers at all. The stage's only input is SV_VertexID and
        // it reads the guest's raw bytes through t16, so binding a stream here
        // would contradict the empty input layout the PSO was built with.
        m_commandList->SetGraphicsRootShaderResourceView(4, d.rawvb.gpu);
        ++m_gpuVertexDraws;
        ++m_gpuVertexFetchDraws;
      } else if (d.gpuVertex) {
        // One stream: the guest's raw attributes. No stand-in vertex and no
        // interpolator stream, because neither exists for this draw — the
        // vertex shader produces the position and the rasterizer interpolates
        // what it exports.
        m_commandList->IASetVertexBuffers(0, 1, &d.vsvbv);
        ++m_gpuVertexDraws;
      } else {
        // Two streams: the stand-in vertex for position, the interpolator
        // stream for everything the pixel shader reads.
        const D3D12_VERTEX_BUFFER_VIEW views[2] = {d.vbv, d.ivbv};
        m_commandList->IASetVertexBuffers(0, 2, views);
      }
      m_commandList->IASetIndexBuffer(&d.ibv);
      m_commandList->DrawIndexedInstanced(d.indexCount, 1, 0, 0, 0);
      ++m_translatedDraws;
      // The root signature AND the heaps were swapped, so the next stand-in
      // draw has to have its own put back. Restored here rather than at the top
      // of the loop so the cost falls on the translated draw that caused it.
      ID3D12DescriptorHeap* heaps[] = {m_gameSrvHeap.Get(),
                                       m_samplerHeap.Get()};
      m_commandList->SetDescriptorHeaps(2, heaps);
      m_commandList->SetGraphicsRootSignature(m_gameRootSig.Get());
      // A translated draw: the guard did NOT fire, but this is an opportunity
      // and must be counted, or the stand-in rate has no denominator. This is
      // the exact counter that was read as "80% of draws lost" earlier today.
      mx::gpu::guard::Note(mx::gpu::guard::Guard::kStandInDraw, false);
      continue;
    }
    // STRICT: skip the draw entirely rather than paint an invented tex*col.
    // The geometry disappearing is a diagnostic; a plausible colour is not.
    if (mx::gpu::guard::Strict(mx::gpu::guard::Guard::kStandInDraw)) {
      mx::gpu::guard::Note(mx::gpu::guard::Guard::kStandInDraw, false);
      // COUNTED SEPARATELY, because the census cannot answer this. Both the
      // translated path and this one record "guard did not fire", so a census
      // reading of 0 is ambiguous between "no stand-ins existed" and "N were
      // suppressed" -- and knowing WHAT was removed is the entire point of
      // strict mode. Without this the experiment reports its own success and
      // nothing else.
      ++m_standInStrictSkipped;
      continue;
    }
    mx::gpu::guard::Note(mx::gpu::guard::Guard::kStandInDraw, true);
    ++m_standInDraws;

    // WHICH stand-in draws actually PAINT, named by target and shader.
    //
    // The complement of WHITE-SKIP WHO above: that one names the draws the gate
    // discards, this one names the draws it lets through, and only the second
    // population can put wrong colour on screen. The vast majority of stand-in
    // draws are the guest's null-PS depth passes, which carry RB_COLOR_MASK 0
    // and paint nothing (NULL-PS TARGETS: "WOULD PAINT 0"), so filtering on
    // colorWrite is what separates the noise from the suspects.
    //
    // This is what named the red gameplay screen. One line, carrying a guest
    // shader handle a capture could not give, identified ps 0x216012A0 as the
    // draw painting full-screen red -- and the same probe going quiet is how
    // the fix was confirmed.
    if (d.colorWrite && d.targetObject) {
      static std::mutex s_mu;
      static std::set<uint64_t> s_seen;
      const uint64_t key =
          (uint64_t(d.targetObject) << 32) | d.pixelShaderHandle;
      bool fresh = false;
      {
        std::lock_guard<std::mutex> lk(s_mu);
        fresh = s_seen.size() < 24 && s_seen.insert(key).second;
      }
      if (fresh) {
        char line[256];
        std::snprintf(line, sizeof(line),
                      "STAND-IN PAINTS: target 0x%08X %ux%u ps 0x%08X "
                      "colorSource %u textured %d translated %d samplers %u "
                      "indices %u depth %d blend %d src %u dest %u",
                      d.targetObject, d.targetWidth, d.targetHeight,
                      d.pixelShaderHandle, unsigned(d.colorSource),
                      d.texture ? 1 : 0, d.translated ? 1 : 0,
                      d.pixelSamplerCount, d.indexCount,
                      d.depthEnable ? 1 : 0, d.blendEnable ? 1 : 0,
                      d.srcBlend, d.destBlend);
        LogInfo(line);
      }
    }

    // Offscreen targets do not yet have per-surface depth resources. The
    // post-processing/resolve chain observed in ST_Southwest is colour-only;
    // keep depth disabled there rather than bind the 1280x720 DSV against a
    // smaller RTV, which is invalid D3D12 state.
    const bool depthEnable = tDepthEnable;
    const bool depthWrite = tDepthWrite;
    // Cull occupies bits 5-7, keeping the variant inside the low 8 bits that
    // m_gamePSOsByFormat's (format << 8) | variant key reserves for it.
    const uint32_t pso_index = (depthEnable ? 1u : 0u) |
                               (depthWrite ? 2u : 0u) |
                               (d.colorWrite ? 0u : 4u) |
                               (textured ? 8u : 0u) |
                               (yuv ? 16u : 0u) |
                               (PackCullBits(d.cullMode) << 5);
    // A blended draw takes a pipeline built for its exact blend state; anything
    // that cannot be translated falls back to the opaque one it used before.
    ID3D12PipelineState* pipeline = nullptr;
    // The pipeline must declare the format of the target it writes. Offscreen
    // targets are no longer all RGBA8.
    const DXGI_FORMAT rtvFormat =
        drawTarget ? drawTarget->format : kBackBufferFormat;
    if (d.blendEnable) {
      pipeline = BlendedPSO(BlendKey{pso_index, d.srcBlend, d.destBlend,
                                     d.blendOp, rtvFormat, topoType,
                                     d.stencilIndex});
    }
    if (!pipeline)
      pipeline = OpaquePSO(pso_index, rtvFormat, topoType, d.stencilIndex);
    m_commandList->SetPipelineState(pipeline);
    // The reference value is NOT pipeline state -- it is set per draw, which is
    // why configs differing only in ref share one pipeline. Set unconditionally
    // when this draw has stencil so it cannot inherit the previous draw's ref;
    // a stale ref is the kind of fault that only shows on the second draw of a
    // pair and reads as a geometry bug.
    if (d.stencilIndex) m_commandList->OMSetStencilRef(d.stencil.ref);
    // Each translated draw brings its own transform; a draw whose cb failed to
    // allocate falls back to the identity matrix rather than being dropped.
    const D3D12_GPU_VIRTUAL_ADDRESS cb =
        d.cb ? d.cb.gpu : m_gameCB->GetGPUVirtualAddress();
    m_commandList->SetGraphicsRootConstantBufferView(0, cb);
    if (textured) {
      // The table declares kMaxPlanes descriptors, so its base must leave that
      // many inside the heap. A single-texture draw reads only the first.
      const uint32_t maxBase =
          kMaxGameTextures - kMaxDrawPlanes;
      auto gpu = m_gameSrvHeap->GetGPUDescriptorHandleForHeapStart();
      gpu.ptr += UINT64(std::min(textureDescriptor, maxBase)) *
                 m_gameSrvDescriptorSize;
      m_commandList->SetGraphicsRootDescriptorTable(1, gpu);
      // The guest's own address mode for this texture, rather than WRAP for
      // everything. Only meaningful for a textured draw; the untextured PSO
      // never samples.
      //
      // The address bits arrive on the draw; the filter is read here off the
      // texture itself, so graphics_system stays a pass-through and the two
      // paths agree on what a variant index means.
      uint32_t variant = d.samplerIndex & (kSamplerClampU | kSamplerClampV);
      if (d.texture && !d.texture->linear_filter) variant |= kSamplerPoint;
      if (d.texture && d.texture->mip_filter == mx::hle::kMipFilterBaseMap)
        variant |= kSamplerBaseMap;
      if (d.texture && d.texture->level_count > 1 &&
          d.texture->mip_filter == mx::hle::kMipFilterPoint)
        variant |= kSamplerMipPoint;
      auto samp = m_samplerHeap->GetGPUDescriptorHandleForHeapStart();
      samp.ptr += UINT64(std::min(variant, kSamplerVariantCount - 1)) *
                  m_samplerDescriptorSize;
      m_commandList->SetGraphicsRootDescriptorTable(2, samp);
    }
    m_commandList->IASetPrimitiveTopology(d.topology);
    m_commandList->IASetVertexBuffers(0, 1, &d.vbv);
    m_commandList->IASetIndexBuffer(&d.ibv);
    m_commandList->DrawIndexedInstanced(d.indexCount, 1, 0, 0, 0);

  }

  // Cumulative, every 100th frame that drew anything. Distinct live targets is
  // reported alongside the cap because the two together say whether the budget
  // is comfortable or about to be exhausted — the count alone does not.
  // Every 20 frames, not 100. Frames cost ~0.5s here, so a 100-frame interval
  // is ~50 seconds and a driven session that reaches the menu and is watched
  // for a few seconds produces exactly ONE print -- from frame 1, before
  // anything has been drawn or resolved. Every measurement taken that way
  // describes startup.
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
                  // printed anywhere -- grep found exactly one occurrence in
                  // the tree. A counter nothing reads cannot answer a question,
                  // and this is the one that says whether a snapshot's
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
    // The figure that says whether the guest's own shaders are actually
    // carrying the picture. Translated against stand-in, because the translated
    // count alone cannot distinguish "the frame runs guest shaders" from "four
    // draws in a corner do".
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
    // The counts are what decide the next move: block-exhausted means the ring
    // is undersized or not being reset, no-snapshot means the draw wants a
    // resolve result we never captured, and the texture ones point upstream at
    // the hooks rather than at anything here.
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
    // The guest alpha test. STAND-IN is the figure to watch: those draws have
    // an enabled test and took a path with no shader to discard in, so they are
    // still painting the pixels the guest masks away.
    std::snprintf(message, sizeof(message),
                  "alpha test: honoured %llu, STAND-IN %llu; "
                  "fixed16 -32..32 targets %llu draws (scale identity); 7e3 clamped %llu; "
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
    // guard, fires beside the population they are a fraction of. A guard
    // reading 0/N with N large is reached constantly and never needed: that is
    // a guard that can be deleted, and it is the cheapest win here.
    std::snprintf(message, sizeof(message), "  GUARD CENSUS --%s",
                  mx::gpu::guard::Report().c_str());
    LogInfo(message);
    // Guest depth clears. Printed unconditionally, zero included: "the guest
    // never asked" and "it asked and we could not place it" are the two
    // outcomes this change exists to tell apart, and a missing line looks like
    // neither.
    std::snprintf(message, sizeof(message),
                  "  GUEST DEPTH CLEARS %llu honoured, %llu with no host "
                  "depth surface",
                  static_cast<unsigned long long>(m_guestDepthClears),
                  static_cast<unsigned long long>(m_guestDepthClearsUnresolved));
    LogInfo(message);
    // PHASE 2 STENCIL. Every number that decides whether this phase is sound,
    // on one line, zeros included.
    //
    //   states       distinct pipeline variants interned. The census says the
    //                guest uses 18 configurations and those differing only in
    //                ref collapse here, so a healthy run is well under 20. A
    //                number that climbs run over run means something varying is
    //                leaking into the key.
    //   refused      draws that wanted stencil past the intern cap and rendered
    //                WITHOUT it. Must be 0. Non-zero is a wrong picture rather
    //                than an error, which is why it is printed rather than
    //                trusted.
    //   blend PSOs   occupancy against kMaxBlendPSOs. This is the sharpest
    //                hazard in the plan: past the cap a blended draw silently
    //                falls back to its opaque pipeline and loses its blending,
    //                and stencil multiplies the variants that reach it.
    //   by-format    the on-demand opaque cache, same concern.
    //   translated   THE ONE THAT MATTERS NOW. Stencil is in the translated
    //                key, so every stencil state multiplies the variants of
    //                every shader that meets it. `capped` non-zero means the
    //                cache is full and pipelines are no longer being built.
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
    // DIAG: the population behind BLANK-SOURCE. A blank snapshot is a
    // compositor quad painting nothing, so if a full-screen extent shows up
    // here with a frame range that ends early, that is a boot-time screen whose
    // backdrop never arrived. `rescue` says why the substitution search did not
    // save it: no-cand = nothing else sits at that EDRAM base (the surface is
    // genuinely absent from the pool), all-blank = something does but nothing
    // was ever drawn into it either (the defect is upstream, in whatever should
    // have rendered it), n/a = depth source or no base to search from.
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
    // DIAG: the COLOUR pool with its EDRAM bases. The
    // 640x360 resolve source (0x21B0F320, base 0x2D0, pitch 640, 4x MSAA in
    // its surface word) has no host target of its own, while a 640x720 surface
    // (0x2123C9BC) sits at the same base and pitch at 1x. Whether that 640x720
    // is drawn into decides whether the 640x360 resolve should take a region of
    // it or whether the pass that fills it is being lost somewhere else.
    for (const auto& [object, t] : m_gameRenderTargets) {
      std::snprintf(message, sizeof(message),
                    "  COLOUR pool obj 0x%08X %ux%u base 0x%03X fmt %u "
                    "drawn:%s",
                    object, t.width, t.height, t.edramBase,
                    uint32_t(t.format), t.everDrawn ? "Y" : "N");
      LogInfo(message);
    }
    // DIAG: what the depth pool actually holds. The
    // shadow resolve names a 768x1024 depth surface while the pass appears to
    // render two EDRAM bands (768x640 at base 0x580, 768x384 at base 0x710),
    // so the object the resolve asks for may be one no draw ever bound.
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
    // "1906 shaders do one each" are distinguishable -- they mean completely
    // different things and the total cannot tell them apart.
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
      // line rather than a decoration.
      //
      // These counters are cumulative over a whole run, so on a 2800-frame run
      // this printed "2744" and read as a 2744-draw defect. It is ONE DRAW PER
      // FRAME. That number was carried as an open item for most of a session on
      // the strength of the total alone; "2643 draws" and "1 draw/frame across
      // 2643 frames" are the same measurement and completely different
      // findings. Print the denominator with the numerator, always.
      //
      // What the residual actually is on this title, measured in a capture
      // rather than assumed: the frame's FIRST draw, a screen-space quad over
      // roughly the top-left 40% x 35%, issued before the guest has bound any
      // shader (vs handle 0, 4 indices, triangle strip) and CLEARED five events
      // later by the frame's own clear. Traced at two pixels in flashing.rdc;
      // it never reaches the screen, so being a stand-in costs nothing but the
      // draw itself. Left uncounted-as-a-category on purpose -- that is an
      // observation about this game, not a classification we can test for here.
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

// Suballocate one per-draw range from the upload ring.
//
// Three ways this can be satisfied, cheapest first: bump the page already being
// filled, reset a page the GPU has finished with, or grow the ring. Only the
// third calls into the driver, and in steady state it never happens — the ring
// reaches the frame's working set within the first few frames and stays there.
bool D3D12Renderer::AllocUpload(UploadAlloc& out, uint32_t bytes) {
  out = {};
  if (!bytes || !m_device) return false;
  const uint32_t need = (bytes + kUploadAlign - 1) & ~(kUploadAlign - 1);
  m_uploadBytesThisFrame += need;

  auto take = [&](uint32_t index) {
    UploadPage& p = m_uploadPages[index];
    out.cpu = p.cpu + p.used;
    out.gpu = p.gpu + p.used;
    out.size = need;
    p.used += need;
    p.live = true;
    m_uploadPage = index;
    return true;
  };

  if (m_uploadPage < m_uploadPages.size()) {
    const UploadPage& p = m_uploadPages[m_uploadPage];
    if (p.cpu && p.used + need <= p.size) return take(m_uploadPage);
  }

  // A page is reusable only when BOTH are true: the GPU has passed the fence it
  // was last submitted under, and the current draw list no longer references it.
  // The second is not implied by the first — a replayed tick re-reads pages the
  // GPU finished with long ago.
  const uint64_t completed = m_fence ? m_fence->GetCompletedValue() : 0;
  for (uint32_t i = 0; i < uint32_t(m_uploadPages.size()); ++i) {
    UploadPage& p = m_uploadPages[i];
    if (!p.cpu || p.live || p.size < need || p.fence > completed) continue;
    p.used = 0;
    return take(i);
  }

  // Grow. An allocation larger than the page size gets a page of its own rather
  // than being refused; it recycles like any other.
  UploadPage page;
  page.size = std::max(kUploadPageBytes, need);
  D3D12_HEAP_PROPERTIES hp = {};
  hp.Type = D3D12_HEAP_TYPE_UPLOAD;
  D3D12_RESOURCE_DESC rd = {};
  rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  rd.Width = page.size;
  rd.Height = 1;
  rd.DepthOrArraySize = 1;
  rd.MipLevels = 1;
  rd.Format = DXGI_FORMAT_UNKNOWN;
  rd.SampleDesc.Count = 1;
  rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  if (FAILED(CreateTimedCommittedResource(
          &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ,
          nullptr, IID_PPV_ARGS(&page.resource))))
    return false;
  // Mapped once and never unmapped. An UPLOAD resource may stay mapped for its
  // whole life, and the Map/Unmap pair around every per-draw buffer was itself
  // part of what this removes.
  D3D12_RANGE noRead = {0, 0};
  void* mapped = nullptr;
  if (FAILED(page.resource->Map(0, &noRead, &mapped)) || !mapped) return false;
  page.cpu = static_cast<uint8_t*>(mapped);
  page.gpu = page.resource->GetGPUVirtualAddress();
  m_uploadPages.push_back(std::move(page));
  {
    char message[192];
    std::snprintf(message, sizeof(message),
                  "upload ring: grew to %zu pages, %llu MB total",
                  m_uploadPages.size(),
                  static_cast<unsigned long long>(
                      uint64_t(m_uploadPages.size()) * kUploadPageBytes /
                      (1024 * 1024)));
    LogInfo(message);
  }
  return take(uint32_t(m_uploadPages.size()) - 1);
}

void D3D12Renderer::ClearGameDraws() {
  // Release the ring pages this draw list was holding.
  //
  // This is the whole of what used to be here. Every per-draw buffer was handed
  // to the fenced retirement list to be destroyed a frame or two later, and the
  // destruction cost as much as the creation: 743ms of an 1815ms menu tick in
  // mx_1033, against 1031ms to create them. Neither exists now — a range of a
  // page is not a resource, and a page is reset rather than freed.
  //
  // The fence protection has not gone away, it has moved: a page carries the
  // submission it was last read under (UploadPage::fence) and cannot be reset
  // until that passes. `live` is the separate condition, and this is what
  // clears it — see the note on the field for why an empty tick makes the two
  // different questions.
  //
  // Done before the empty check on purpose. AddGameDraw can allocate and then
  // fail out before appending, so pages can be live with no draw referencing
  // them; leaving those marked would retire them from the ring permanently.
  for (auto& p : m_uploadPages) p.live = false;
  m_uploadBytesThisFrame = 0;

  if (m_gameDraws.empty()) return;
  m_gameDraws.clear();
  // The descriptor block window is NOT reset here. This runs on guest handoff,
  // not per host frame, and blocks are consumed per host frame — which is what
  // exhausted the ring. RenderGameFrame opens each frame's own slice instead.
}

void D3D12Renderer::DrainRetired() {
  if (m_retired.empty() || !m_fence) return;
  const uint64_t completed = m_fence->GetCompletedValue();
  while (!m_retired.empty() && m_retired.front().fence <= completed) {
    // The descriptors come back on the same fence as the resources. Returning
    // them any earlier would let a live command list sample a slot that has
    // been rewritten to point at a different texture.
    for (uint32_t index : m_retired.front().srv)
      m_freeGameSrvDescriptors.push_back(index);
    m_retired.pop_front();
  }
}

void D3D12Renderer::AddGameDraw(const uint8_t* vertices, uint32_t vtxBytes,
                                 uint32_t vtxStride, const uint8_t* indices,
                                 uint32_t idxBytes, bool idx16,
                                 uint32_t idxCount, const float* mvp,
                                 uint32_t topology, bool depthEnable,
                                 bool depthWrite, bool colorWrite,
                                 std::shared_ptr<const mx::hle::HleTexturePayload> texture,
                                 uint32_t targetObject, uint32_t targetWidth,
                                 uint32_t targetHeight,
                                 uint32_t sampledTargetObject,
                                 uint32_t sampledTextureObject,
                                 const std::shared_ptr<const mx::hle::HleTexturePayload>* planes,
                                 uint32_t planeCount, bool yuvHasAlpha,
                                 bool blendEnable, uint32_t srcBlend,
                                 uint32_t destBlend, uint32_t blendOp,
                                 uint8_t colorSource, uint32_t samplerIndex,
                                 uint32_t pixelShaderHandle,
                                 std::shared_ptr<const std::string> pixelShaderHlsl,
                                 std::shared_ptr<const std::vector<uint8_t>> pixelShaderDxbc,
                                 const uint8_t* interpolators,
                                 uint32_t interpBytes,
                                 const uint32_t* pixelConstants,
                                 uint32_t pixelConstDwords,
                                 uint32_t pixelSamplerCount,
                                 const std::shared_ptr<const mx::hle::HleTexturePayload>* pixelTextures,
                                 const uint32_t* pixelSampledObjects,
                                 const uint16_t* pixelSampledSwizzles,
                                 const GpuVertexStage* vertexStage,
                                 uint32_t pixelSamplerArrayMask,
                                 const uint8_t* pixelSamplerSigns,
                                 uint32_t pixelParamGen,
                                 uint32_t depthObject, uint32_t depthWidth,
                                 uint32_t depthHeight, uint32_t depthBase,
                                 uint32_t targetBase,
                                 uint32_t targetColorFormat,
                                 const int32_t* scissor,
                                 uint32_t alphaControl, float alphaRef,
                                 uint32_t cullMode, uint32_t vtxCntl,
                                 uint32_t guestVpWidth,
                                 uint32_t guestVpHeight, bool useGuestVp,
                                 bool edramCopy, const GameStencil* stencil) {
  // PERF(per-frame-allocs): DONE. This used to create an ID3D12Resource on the
  // UPLOAD heap for each of the buffers below — up to nine per call, once per
  // submitted draw — and the note here called for "a ring of upload buffers
  // recycled after MoveToNextFrame's fence sync". That ring is AllocUpload; the
  // buffers are now ranges of it and the only committed resources left in this
  // path are the ring's own pages, created a handful of times per session.
  //
  // What it cost, measured in mx_1033 before the change: a steady-state main
  // menu tick of 1815ms spent 1031ms creating those resources and 743ms
  // destroying them — 97.7% of the tick — against 17ms to record the frame and
  // 0ms waiting for the GPU. 1476 calls at ~683us each, 4.3 per draw. The guest
  // was blocked in SetDrawCalls behind all of it, which is what its 1.75s frames
  // and 0.55 fps actually were.
  //
  // This comment used to claim "D3D12's internal command-list tracking keeps
  // the underlying memory alive until the GPU finishes the last command using
  // it". That is false — D3D12 command lists do not reference-count the
  // resources they reference; that was a D3D11 guarantee. Lifetime is still the
  // application's job and is now the ring's: a page carries the submission it
  // was last read under and cannot be reset until that fence passes.
  // A fetch draw brings no host vertex buffer at all: its geometry arrives in
  // vertexStage->rawBytes and the shader reads it through the root SRV, so a
  // null `vertices` is correct here rather than a malformed draw. Tested before
  // the gate because the gate would otherwise drop every one of them.
  const bool fetchGeometry = vertexStage && vertexStage->rawBytes &&
                             vertexStage->rawByteCount &&
                             vertexStage->rawFetch && vertexStage->rawFetchCount;
  if (!indices || idxBytes == 0) return;
  if (!fetchGeometry && (!vertices || vtxBytes == 0)) return;
  if (m_gameDraws.size() >= kMaxGameDraws) {
    static bool s_logged = false;
    if (!s_logged) {
      s_logged = true;
      LogInfo("AddGameDraw: hit the per-frame draw cap, dropping the rest");
    }
    return;
  }

  // Was a CreateCommittedResource per buffer; now a bump allocation out of the
  // ring. Kept as a lambda of the same shape so the call sites below read the
  // same way, and because the ring hands back memory that is already mapped,
  // each of them also loses its Map/Unmap pair.
  auto createBuffer = [&](UploadAlloc& buf, uint32_t size) -> bool {
    return AllocUpload(buf, size);
  };

  // Built locally and only appended once complete, so a partial failure leaves
  // the frame's list untouched rather than half-populated.
  GameDraw d;

  // Skipped entirely for a fetch draw — a zero-byte buffer is not a valid D3D12
  // resource, and nothing binds `vbv` on that path. The guard at the end of this
  // function is what keeps such a draw from ever reaching the stand-in, which
  // WOULD read it.
  if (vertices && vtxBytes) {
    if (!createBuffer(d.vb, vtxBytes)) return;
    void* vtxMap = d.vb.cpu;
    memcpy(vtxMap, vertices, vtxBytes);
    // The fixed Bink YUV shader replaces the guest pixel shader, but it must
    // preserve the guest shader's final modulation:
    //
    //   export = decoded_yuva * c0
    //
    // kGameYuvPS performs that multiply through its COLOR input. Bink uses an
    // UP/FVF quad with no guest COLOR element, so the transcode supplies white
    // there; leaving it white would make the video ignore c0 entirely.
    //
    // The host layout is position float4 @0, color float4 @16, uv float2 @32.
    // Multiplying the seeded color by c0 is algebraically identical to the
    // guest shader and avoids adding a second constant-buffer binding solely
    // for this optimized path.
    if (planes && planeCount >= 3 && pixelConstants &&
        pixelConstDwords >= 4 && vtxStride >= 32) {
      float modulation[4];
      std::memcpy(modulation, pixelConstants, sizeof(modulation));
      auto* bytes = static_cast<uint8_t*>(vtxMap);
      const uint32_t vertexCount = vtxBytes / vtxStride;
      for (uint32_t i = 0; i < vertexCount; ++i) {
        float color[4];
        std::memcpy(color, bytes + i * vtxStride + 16, sizeof(color));
        for (uint32_t c = 0; c < 4; ++c) color[c] *= modulation[c];
        std::memcpy(bytes + i * vtxStride + 16, color, sizeof(color));
      }
      static uint32_t s_yuvModulationLogs = 0;
      if (s_yuvModulationLogs++ < 8) {
        const std::string message = fmt::format(
            "Bink YUV c0 modulation: ({:.4f}, {:.4f}, {:.4f}, {:.4f})",
            modulation[0], modulation[1], modulation[2], modulation[3]);
        LogInfo(message.c_str());
      }
    }
    d.vbv.BufferLocation = d.vb.gpu;
    d.vbv.StrideInBytes = vtxStride;
    d.vbv.SizeInBytes = vtxBytes;
  }

  if (!createBuffer(d.ib, idxBytes)) return;
  memcpy(d.ib.cpu, indices, idxBytes);
  d.ibv.BufferLocation = d.ib.gpu;
  d.ibv.Format = idx16 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
  d.ibv.SizeInBytes = idxBytes;
  d.indexCount = idxCount;
  d.topology = static_cast<D3D12_PRIMITIVE_TOPOLOGY>(topology);
  d.depthEnable = depthEnable;
  d.depthWrite = depthWrite;
  d.colorWrite = colorWrite;
  d.texture = std::move(texture);
  d.targetObject = targetObject;
  d.targetWidth = targetWidth;
  d.targetHeight = targetHeight;
  d.sampledTargetObject = sampledTargetObject;
  d.sampledTextureObject = sampledTextureObject;
  d.colorSource = colorSource;
  d.samplerIndex = samplerIndex;
  d.pixelShaderHandle = pixelShaderHandle;
  d.pixelShaderHlsl = std::move(pixelShaderHlsl);
  d.pixelShaderDxbc = std::move(pixelShaderDxbc);
  d.pixelSamplerCount = pixelSamplerCount;
  d.pixelSamplerArrayMask = pixelSamplerArrayMask;
  d.pixelParamGen = pixelParamGen;
  d.alphaControl = alphaControl;
  d.alphaRef = alphaRef;
  if (pixelSamplerSigns)
    std::memcpy(d.pixelSamplerSigns.data(), pixelSamplerSigns,
                d.pixelSamplerSigns.size());
  d.depthObject = depthObject;
  d.depthWidth = depthWidth;
  d.depthHeight = depthHeight;
  d.depthBase = depthBase;
  d.targetBase = targetBase;
  d.targetColorFormat = targetColorFormat;
  // DIAG: every draw aimed at a SQUARE POWER-OF-TWO offscreen target, grouped
  // by the frame it landed in.
  //
  // The question it exists to answer: the terrain's ground meshes take their
  // whole world Y from three texture samples -- their vertex buffer carries
  // only grid X and Z -- and the dominant term is the 512x512 at 0x132E2000,
  // which reads min = max = 0. The guest's own `ps_hft_deform_copy` copies the
  // heightfield into that buffer, but sub_82AD49A0 only runs it for tiles a
  // track segment has reached, so on an untouched map the buffer has to be
  // filled somewhere else -- at level load. Either those draws never reach us,
  // or they do and the per-frame first-use clear eats them.
  //
  // ONE LINE PER FRAME THAT HAS ANY, not per draw: a level load is thousands
  // of frames and the interesting ones are the handful that draw here at all.
  // The cap is on LINES, so a long run cannot flood, and the running total
  // keeps counting after the cap so the last line still states the truth.
  if (targetWidth == targetHeight && targetWidth >= 128 &&
      (targetWidth & (targetWidth - 1)) == 0) {
    static std::mutex s_mu;
    static uint64_t s_total = 0;
    static uint32_t s_lines = 0;
    static uint32_t s_lastFrame = 0xFFFFFFFFu;
    static uint32_t s_inFrame = 0;
    static uint32_t s_lastObject = 0;
    std::lock_guard<std::mutex> lk(s_mu);
    ++s_total;
    if (m_gameFrame != s_lastFrame) {
      if (s_inFrame && s_lines < 200) {
        ++s_lines;
        REXLOG_INFO(
            "d3d12: SQUARE TARGET frame {}: {} draws, last object 0x{:08X} "
            "{}x{} guest colour format {} (total {})",
            s_lastFrame, s_inFrame, s_lastObject, targetWidth, targetHeight,
            targetColorFormat, static_cast<unsigned long long>(s_total));
      }
      s_lastFrame = m_gameFrame;
      s_inFrame = 0;
    }
    ++s_inFrame;
    s_lastObject = targetObject;
  }
  if (pixelTextures && pixelSampledObjects) {
    for (uint32_t i = 0; i < kTranslatedSamplerSlots; ++i) {
      d.pixelTextures[i] = pixelTextures[i];
      d.pixelSampledObjects[i] = pixelSampledObjects[i];
      d.pixelSampledSwizzles[i] =
          pixelSampledSwizzles ? pixelSampledSwizzles[i] : uint16_t(0);
    }
  }
  // The vertex stage's own textures, bound through a separate descriptor range.
  // Copied here, before the translated gate below, so that the sign table the
  // fetch path writes into the cbuffer tail can read them.
  if (vertexStage && vertexStage->samplerCount && vertexStage->textures &&
      vertexStage->sampledObjects) {
    d.vertexSamplerCount = vertexStage->samplerCount;
    d.vertexSamplerArrayMask = vertexStage->samplerArrayMask;
    for (uint32_t i = 0; i < kTranslatedSamplerSlots; ++i) {
      d.vertexTextures[i] = vertexStage->textures[i];
      d.vertexSampledObjects[i] = vertexStage->sampledObjects[i];
      d.vertexSampledSwizzles[i] = vertexStage->sampledSwizzles
                                       ? vertexStage->sampledSwizzles[i]
                                       : uint16_t(0);
    }
    if (vertexStage->samplerSigns)
      std::memcpy(d.vertexSamplerSigns.data(), vertexStage->samplerSigns,
                  d.vertexSamplerSigns.size());
  }

  // Does this draw bring the guest's own vertex stage? Decided BEFORE the
  // translated gate below, because it changes what that gate requires: a draw
  // running the guest vertex shader has no interpolator stream and must not
  // have one. Demanding it anyway is what turned every qualifying draw into a
  // stand-in draw holding vertices the interpreter no longer transformed —
  // a flat red frame, and zero translated draws in a run where 36,064
  // qualified.
  //
  // Two valid shapes, and a fetch stage has NONE of the input-element fields by
  // design -- its only input is SV_VertexID. Requiring them unconditionally
  // dropped every fetch draw on the floor here: `hasVertexStage` was false, and
  // the guard further down that refuses a draw which brought a vertex stage and
  // could not get one then discarded it rather than falling back. Measured as a
  // frame going from 339 draws to 28.
  const bool hasVertexCommon = vertexStage && vertexStage->handle &&
                               vertexStage->hlsl && vertexStage->constants &&
                               vertexStage->constDwords;
  const bool hasFetchStage =
      hasVertexCommon && vertexStage->rawBytes && vertexStage->rawByteCount &&
      vertexStage->rawFetch && vertexStage->rawFetchCount &&
      vertexStage->rawFetchCount <= mx::hle::HlslShader::kMaxVertexFetches;
  const bool hasVertexStage =
      hasFetchStage ||
      (hasVertexCommon && vertexStage->inputs && vertexStage->inputBytes &&
       vertexStage->regs && vertexStage->regCount &&
       vertexStage->regCount <= 32);

  // The translated path needs all of its inputs or none of them. A shader run
  // without its interpolators reads undefined registers, and one run without
  // its constants computes from zeros — both produce a confident wrong picture
  // rather than a visible failure, which is worse than keeping the stand-in.
  //
  // "Its interpolators" means the CPU-built stream only when the CPU built the
  // vertices. With the guest vertex shader running, the rasterizer produces
  // them and there is nothing to require.
  //
  // The sampler limit is the honest current boundary: a descriptor block per
  // draw is not built yet, so only a shader reading a single texture can be
  // bound correctly, using the descriptor this draw already has. Multi-sampler
  // shaders keep the stand-in until that lands.
  // WHICH of the six conditions sent this draw to the stand-in. Without this the
  // only available numbers are measured at two different points -- the hook
  // counts ~475k D3D9 draw attempts, the renderer ~52k submitted draws -- so
  // "2000 draws with an untranslated shader" and "27015 stand-in draws" describe
  // different populations and cannot be subtracted from one another. Every
  // attempt to reason about the difference between them has been wrong.
  //
  // no-HANDLE is tested FIRST, and the order is the whole point. A draw with no
  // pixel shader bound has no handle AND no HLSL, so with the two the other way
  // round every one of them was counted as no-hlsl and `no-handle 0` could
  // never be anything but zero -- an unreachable branch reading exactly like a
  // measured absence. That cost a session: "no-handle 0, no-hlsl 28257" was
  // read as "every stand-in draw has a shader whose translation is missing",
  // which sent the search into the translator when the shaders were translating
  // fine. A counter that cannot fire is worse than no counter.
  // A guest DEPTH pass: a translated VERTEX stage and no pixel shader at all.
  //
  // It needs none of the four things the clause below demands of a normal
  // translated draw — no handle, no HLSL, no constant bank, no samplers —
  // because kTranslatedDepthOnlyPS reads nothing and its output is discarded by
  // a zero write mask. What it DOES need is the vertex stage, which is the whole
  // point: without this the draw loses it and runs on the software interpreter.
  //
  // `!colorWrite` is the load-bearing term and is checked here as well as on the
  // hooks side. The two decide it from the same guest register, in different
  // processes' worth of code, and this is the one that the write mask is
  // actually built from — so if they ever disagree, the draw falls back rather
  // than painting a stand-in colour the guest never asked for.
  const bool depthOnlyStandIn =
      !pixelShaderHlsl && hasVertexStage && !colorWrite;
  d.depthOnlyStandIn = depthOnlyStandIn;

  if (!d.pixelShaderHandle && !depthOnlyStandIn) {
    ++m_standInNoHandle;
    if (d.planeCount >= 3) ++m_standInNoHandlePlanes;
    if (d.colorClear) ++m_standInNoHandleClear;
    if (d.surfaceBind) ++m_standInNoHandleBind;
    m_standInNoHandleBy[(uint64_t(d.vertexShaderHandle) << 32) |
                        uint64_t(d.indexCount)]++;
  }
  else if (!d.pixelShaderHlsl && !depthOnlyStandIn) ++m_standInNoHlsl;
  else if (!hasVertexStage && !(interpolators && interpBytes))
    ++m_standInNoVertexInputs;
  else if ((!pixelConstants || !pixelConstDwords) && !depthOnlyStandIn)
    ++m_standInNoConstants;
  else if (pixelSamplerCount > kTranslatedSamplerSlots) ++m_standInTooManySamplers;

  if ((depthOnlyStandIn ||
       (d.pixelShaderHlsl && d.pixelShaderHandle && pixelConstants &&
        pixelConstDwords)) &&
      (hasVertexStage || (interpolators && interpBytes)) &&
      pixelSamplerCount <= kTranslatedSamplerSlots) {
    // The shader's cbuffer is xe_c[256], then xe_texinv[slots],
    // xe_texsign[slots], xe_param_gen and xe_alphatest, so the buffer must
    // cover all five.
    // Sizing it to the
    // constant bank alone would leave the shader reading past the end of the
    // resource for every unnormalized fetch. Rounded up to 256 bytes, the
    // constant-buffer granularity.
    // Zero for a depth pass, which brings no constant bank. The three payloads
    // after it still have to exist because the cbuffer is declared with them,
    // so the buffer is built and sized exactly as usual — only the bank part of
    // it is empty, and the stand-in reads none of it anyway.
    const uint32_t bankBytes = depthOnlyStandIn ? 0u : pixelConstDwords * 4;
    const uint32_t texInvBytes = kTranslatedSamplerSlots * 16;
    const uint32_t texSignBytes = kTranslatedSamplerSlots * 16;
    const uint32_t paramGenBytes = 16;
    const uint32_t alphaTestBytes = 16;
    const uint32_t colorScaleBytes = 16;
    const uint32_t constBytes =
        ((bankBytes + texInvBytes + texSignBytes + paramGenBytes +
          alphaTestBytes + colorScaleBytes) +
         255u) &
        ~255u;
    // Built only for the CPU-vertex shape. A zero-byte buffer is not a valid
    // D3D12 resource, so this cannot simply fall out of interpBytes == 0.
    bool haveInterp = hasVertexStage;
    if (!hasVertexStage && createBuffer(d.ivb, interpBytes)) {
      std::memcpy(d.ivb.cpu, interpolators, interpBytes);
      d.ivbv.BufferLocation = d.ivb.gpu;
      d.ivbv.SizeInBytes = interpBytes;
      d.ivbv.StrideInBytes =
          kTranslatedInterpolators * 4 * uint32_t(sizeof(float));
      haveInterp = true;
    }
    if (haveInterp && createBuffer(d.pscb, constBytes)) {
      {
        uint8_t* p = d.pscb.cpu;
        std::memset(p, 0, constBytes);
        // Guarded: a depth pass has bankBytes 0 and pixelConstants null, and
        // memcpy from a null pointer is undefined even for a zero count.
        if (bankBytes) std::memcpy(p, pixelConstants, bankBytes);
        // xe_texinv, immediately after the bank. An unnormalized fetch
        // addresses the texture in TEXELS, so the shader multiplies by this to
        // normalize — it is therefore 1/extent of the texture actually bound at
        // that slot, which with one sampler is this draw's texture.
        //
        // It held the extent itself until the emitter's divide was corrected,
        // which made every unnormalized fetch wrong by the size squared.
        //
        // Left zero when there is no texture, which makes such a fetch read
        // texel 0 rather than something plausible — and is why this is stored
        // reciprocated here instead of divided in the shader, where a zero
        // extent would produce infinity.
        // EVERY slot, not just the first. xe_texinv is declared
        // kTranslatedSamplerSlots wide and was filled at index 0 only, so an
        // unnormalized fetch on any slot above the first multiplied its
        // coordinate by ZERO -- sampling texel 0 and painting that single
        // texel's colour flat across the primitive. On a full-screen quad that
        // is a wash, which is what it looks like.
        //
        // The per-slot payloads were already carried here; only this fill was
        // still single-texture. Slot 0 falls back to d.texture because the
        // single-texture path populates that and not the array.
        //
        // A slot bound to a RESOLVE SNAPSHOT has no CPU payload at all, and
        // filling only from d.pixelTextures left those slots at zero -- the
        // same defect as the slot-0-only fill above, surviving in the one case
        // that never carries a payload. The extent then has to come from the
        // snapshot resource, which is the texture actually bound there.
        //
        // This is what the menu rider looks like: its material samples the
        // scene composite at s13, that slot is a snapshot, its texinv was zero,
        // and an unnormalized fetch times zero reads texel (0,0) and paints it
        // flat over 21753 indices of character mesh.
        //
        // .z is the LAYER COUNT, not a reciprocal, and it is the one component
        // here that scales up rather than down: a 3D/stacked fetch with
        // normalized coordinates delivers W as a fraction of the stack, and the
        // Texture2DArray it samples wants a slice index. See EmitTextureFetch.
        // Left at zero for a snapshot or an absent texture, which pins such a
        // fetch to slice 0 instead of sampling off the end.
        for (uint32_t s = 0; s < kTranslatedSamplerSlots; ++s) {
          uint32_t w = 0, h = 0, layers = 0;
          const auto& tex = s < d.pixelTextures.size() && d.pixelTextures[s]
                                ? d.pixelTextures[s]
                                : (s == 0 ? d.texture : nullptr);
          if (tex && tex->width && tex->height) {
            w = tex->width;
            h = tex->height;
            layers = tex->array_size;
          } else if (s < d.pixelSampledObjects.size() &&
                     d.pixelSampledObjects[s]) {
            const auto snap = m_gameSnapshots.find(d.pixelSampledObjects[s]);
            if (snap != m_gameSnapshots.end()) {
              w = snap->second.width;
              h = snap->second.height;
            }
          }
          if (!w || !h) continue;
          const float ts[4] = {1.0f / float(w), 1.0f / float(h),
                               float(layers), 0.0f};
          std::memcpy(static_cast<uint8_t*>(p) + bankBytes + s * 16, ts,
                      sizeof(ts));
        }
        // xe_texsign, immediately after xe_texinv: the per-component scale for
        // TEXTURE SIGNS, 2.0 where the guest fetch is kUnsignedBiased and the
        // shader must expand [0,1] to [-1,1]. The shader pairs it with an
        // offset of 1-scale, so 1.0 is the identity.
        //
        // Written for EVERY slot and every component, unconditionally. The
        // buffer was memset to zero above, and a zero scale here does not mean
        // "unsigned", it means the fetch becomes v*0 + 1 -- every texture
        // sampling as solid white. There is no slot this may be skipped for.
        for (uint32_t s = 0; s < kTranslatedSamplerSlots; ++s) {
          const uint8_t biased = d.pixelSamplerSigns[s];
          const float sc[4] = {(biased & 1) ? 2.0f : 1.0f,
                               (biased & 2) ? 2.0f : 1.0f,
                               (biased & 4) ? 2.0f : 1.0f,
                               (biased & 8) ? 2.0f : 1.0f};
          std::memcpy(
              static_cast<uint8_t*>(p) + bankBytes + texInvBytes + s * 16, sc,
              sizeof(sc));
        }
        // xe_param_gen follows xe_texsign. x is the biased destination
        // register; y identifies the primitive so the shader can reproduce the
        // Xenos point and line sign flags.
        uint32_t primitive = 0;
        if (d.topology == D3D_PRIMITIVE_TOPOLOGY_POINTLIST) {
          primitive = 1;
        } else if (d.topology == D3D_PRIMITIVE_TOPOLOGY_LINELIST ||
                   d.topology == D3D_PRIMITIVE_TOPOLOGY_LINESTRIP) {
          primitive = 2;
        }
        const uint32_t pg[4] = {d.pixelParamGen, primitive, 0, 0};
        std::memcpy(static_cast<uint8_t*>(p) + bankBytes + texInvBytes +
                        texSignBytes,
                    pg, sizeof(pg));
        // xe_alphatest follows xe_param_gen. RB_COLORCONTROL is decoded here
        // rather than in the shader so that the shader carries no knowledge of
        // the register layout, and so a wrong bit assignment is one edit away
        // from the comment that justifies it (hle_types.h, DrawCall::
        // colour_control -- bits 0-2 the comparison, bit 3 its enable).
        //
        // The reference is passed as raw bits through a uint4 rather than
        // converted: it is a float, and rounding it through an integer member
        // would quietly move every threshold.
        uint32_t refBits = 0;
        std::memcpy(&refBits, &d.alphaRef, 4);
        const uint32_t at[4] = {d.alphaControl & 7u,
                                (d.alphaControl >> 3) & 1u, refBits, 0};
        std::memcpy(static_cast<uint8_t*>(p) + bankBytes + texInvBytes +
                        texSignBytes + paramGenBytes,
                    at, sizeof(at));
        // xe_colorscale follows xe_alphatest. Guest colour formats 4 (k_16_16)
        // and 5 (k_16_16_16_16) are signed fixed point -32...32; every other
        // format is already in the range its host format expects.
        //
        // ONLY these two. Format 7 (k_16_16_16_16_FLOAT) is a genuine half
        // float and shares R16G16B16A16_FLOAT with format 5 -- scaling it too
        // would divide a correct HDR buffer by 32. The guest nibble is the only
        // thing that separates them, which is exactly why it is carried per
        // draw instead of being inferred from the host format.
        // Guest formats 4 (k_16_16) and 5 (k_16_16_16_16) are signed fixed
        // point -32...32. This USED to write 1/32 here, which was half of
        // Xenia's hack -- and the half that does not apply to us.
        //
        // Xenia biases the write down by 5 exponents
        // (d3d12_command_processor.cc:4329, "Remap from -32...32 to -1...1")
        // because ITS host render target is SNORM and physically cannot hold
        // -32...32. It pairs that with the exact inverse at resolve
        // (draw_util.cc:1345, `exp_bias + 5`, commented "the texture expects
        // 0x8001 = -32, 0x7FFF = 32 ... revert").
        //
        // We map both formats to a HALF FLOAT host target, which holds the
        // whole range. HostColorFormat already says so in as many words: "A
        // half-float host target holds the whole -32...32 range ... and needs
        // no shader-side scale." So the divide here had no counterpart and
        // nothing ever undid it -- every consumer of a fixed-point target read
        // values 32x too small. On console the round trip is identity: the
        // guest writes v in -32...32 and the texture reads back v in
        // -32...32.
        //
        // Measured in menu3.rdc. The deferred light accumulation buffer is
        // guest format 5 ("RB_COLOR_INFO object 0x2123CA94 1280x640 raw
        // 0x000502D0 format 5"), and the bike at (800,450) accumulated 0.0016
        // of light against an ambient of 0.00065. The whole deferred chain --
        // lights, material pass, composite -- ran a factor of 32 down.
        //
        // Kept as a named flag and still counted, because the population is
        // not marginal (262,970 draws in a menu run) and a regression here
        // needs to be attributable.
        const bool fixed16 =
            d.targetColorFormat == 4u || d.targetColorFormat == 5u;
        // .y and .z carry the range the GUEST format can represent, and are 0
        // for formats that need no clamp -- the shader branches on .y > 0.
        //
        // Guest format 3 (k_2_10_10_10_FLOAT) and 12
        // (k_2_10_10_10_FLOAT_AS_16_16_16_16) are 7e3: "[0, 32) RGB, unorm
        // alpha" (xenos.h:301). Both map to R16G16B16A16_FLOAT here, which is
        // SIGNED, so without this a shader's negative output is stored where
        // the console ROP would have clamped it to 0. 31.875 is the largest
        // representable 7e3 value and is the same bound Xenia clamps to.
        //
        // Every other format is already handled: 0/1 and 2/10 map to UNORM
        // host formats that clamp on write, and 7 (k_16_16_16_16_FLOAT) is a
        // genuine signed half float that must NOT be clamped. Formats 4 and 5
        // are the fixed-point -32..32 pair, signed, and their half-float host
        // target holds that range directly -- so no clamp and no scale.
        const bool float7e3 =
            d.targetColorFormat == 3u || d.targetColorFormat == 12u;
        const float cs[4] = {1.0f,
                             float7e3 ? 31.875f : 0.0f,
                             float7e3 ? 1.0f : 0.0f, 0.0f};
        // NOT A GUARD, and removed from the census 2026-08-27. It read 24.7%
        // freeroam / 24.3% menu -- the top entry -- and it does not belong
        // there at all.
        //
        // docs/strict_mode.md classifies by what a thing DOES: class A refuses
        // to act on bad input and models reality; class B manufactures a value
        // we do not have. This is neither. It applies the GUEST FORMAT'S OWN
        // RANGE: formats 3 and 12 are 7e3, unsigned [0, 32), and 31.875 is the
        // largest value the format can hold. A 7e3 target physically cannot
        // store a negative or a value at or above 32, so clamping to that range
        // is what the hardware storage does -- exactly as formats 0/1/2/10 get
        // it for free from their UNORM host formats. We do it in the shader
        // only because our half-float host target would otherwise keep values
        // the guest buffer never could.
        //
        // So 24.7% is not a guard rate. It is the share of draws that render to
        // a 7e3 target, which is a fact about the workload. Leaving it in the
        // census would have made correct format modelling look like the single
        // largest source of invented output in the renderer.
        //
        // m_float7e3Clamped still counts it, on the format line where it
        // belongs.
        if (float7e3) ++m_float7e3Clamped;
        std::memcpy(static_cast<uint8_t*>(p) + bankBytes + texInvBytes +
                        texSignBytes + paramGenBytes + alphaTestBytes,
                    cs, sizeof(cs));
        if (fixed16) ++m_fixed16Scaled;
        d.translated = true;
      }
    }
    if (!d.translated) {
      // Reached only with the gate already passed, so this is the upload ring
      // refusing a range -- not a property of the draw. Charged because it is
      // otherwise indistinguishable from a draw that never qualified, and the
      // two want completely different fixes.
      ++m_standInBufferFailed;
      d.ivb = {};
      d.pscb = {};
    }
  }

  // The guest's own vertex shader, on the GPU. Only offered for a draw whose
  // pixel shader also translated — the hooks side enforces that, and it is
  // re-checked here through `d.translated` because the two conditions are
  // decided in different processes' worth of code and a mismatch would show up
  // as geometry rather than as a message.
  //
  // Everything the CPU path derives on the side is REPLACED rather than lost:
  // the position buffer is what this stage now produces, the interpolator copy
  // is what the rasterizer does natively, and the param_gen UV becomes
  // SV_Position in a pixel shader that reads it. So there is nothing to carry
  // across — only something to stop doing.
  if (d.translated && hasVertexStage) {
    // The emitted cbuffer is xe_c[256] followed by xe_texinv[slots] in BOTH
    // stages, so the buffer has to cover both here too. Sizing it to the
    // constant bank alone leaves the shader reading past the end of the
    // resource — the same trap the pixel path documents above, and it does not
    // stop applying because this stage never samples.
    //
    // The fetch variant appends uint4 xe_vf[kMaxVertexFetches] after
    // xe_texinv, so its cbuffer is longer. Sized for it unconditionally: the
    // tail is zeroed either way and 512 spare bytes per draw is not worth a
    // second size.
    //
    // float4 xe_texsign[slots] follows xe_vf and is counted here too. It is
    // LAST on purpose: the renderer writes xe_vf at a fixed offset computed
    // from the two members before it, so appending is the only way to add to
    // this cbuffer without moving that. Leaving it out of the size was the
    // trap this comment already warns about -- the shader declares it either
    // way, so an unsized tail is a read past the end of the resource.
    const uint32_t vsConstBytes =
        ((vertexStage->constDwords * 4 + kTranslatedSamplerSlots * 16 +
          mx::hle::HlslShader::kMaxVertexFetches * 16 +
          kTranslatedSamplerSlots * 16) + 255u) & ~255u;
    if (vertexStage->rawBytes) {
      // The fetch path: one raw buffer, no vertex buffer view, and xe_vf[]
      // written into the cbuffer tail immediately after xe_texinv.
      if (createBuffer(d.rawvb, vertexStage->rawByteCount) &&
          createBuffer(d.vscb, vsConstBytes)) {
        // Both ranges are already mapped, so the pair of Map calls this used to
        // guard on is gone and with it the only way these writes could fail.
        std::memcpy(d.rawvb.cpu, vertexStage->rawBytes,
                    vertexStage->rawByteCount);
        uint8_t* p = d.vscb.cpu;
        std::memset(p, 0, vsConstBytes);
        std::memcpy(p, vertexStage->constants, vertexStage->constDwords * 4);
        // xe_vf sits directly after xe_c[256] and xe_texinv[16], which is
        // where the emitter declares it. This offset and that declaration
        // are one fact in two places -- if either moves the other must.
        const uint32_t vfOffset =
            vertexStage->constDwords * 4 + kTranslatedSamplerSlots * 16;
        std::memcpy(p + vfOffset, vertexStage->rawFetch,
                    vertexStage->rawFetchCount * 16);
        FillVertexTextureSigns(d, p, vsConstBytes, vertexStage->constDwords);
        d.vertexShaderHandle = vertexStage->handle;
        d.vertexShaderHlsl = vertexStage->hlsl;
        d.vertexShaderDxbc = vertexStage->dxbc;
        d.vertexInputCount = 0;
        d.gpuVertex = true;
        d.gpuVertexFetch = true;
      }
      if (!d.gpuVertexFetch) {
        d.rawvb = {};
        d.vscb = {};
      }
    } else if (createBuffer(d.vsvb, vertexStage->inputBytes) &&
               createBuffer(d.vscb, vsConstBytes)) {
      std::memcpy(d.vsvb.cpu, vertexStage->inputs, vertexStage->inputBytes);
      d.vsvbv.BufferLocation = d.vsvb.gpu;
      d.vsvbv.SizeInBytes = vertexStage->inputBytes;
      d.vsvbv.StrideInBytes = vertexStage->regCount * 16;
      // Zeroed first: the shader's cbuffer is xe_c[256] plus xe_texinv, and the
      // vertex bank is 256 constants, so the tail past the bank must read zero
      // rather than whatever the ring page held from an earlier frame.
      std::memset(d.vscb.cpu, 0, vsConstBytes);
      std::memcpy(d.vscb.cpu, vertexStage->constants,
                  vertexStage->constDwords * 4);
      FillVertexTextureSigns(d, d.vscb.cpu, vsConstBytes,
                             vertexStage->constDwords);
      d.vertexShaderHandle = vertexStage->handle;
      d.vertexShaderHlsl = vertexStage->hlsl;
      d.vertexShaderDxbc = vertexStage->dxbc;
      d.vertexInputCount = vertexStage->regCount;
      for (uint32_t i = 0; i < vertexStage->regCount; ++i)
        d.vertexInputRegs[i] = vertexStage->regs[i];
      d.gpuVertex = true;
    }
    if (!d.gpuVertex) {
      d.vsvb = {};
      d.rawvb = {};
      d.vscb = {};
    }
  }
  // A draw that brought a vertex stage and could not get one has no path left.
  // Its `vb` holds the raw declaration positions, NOT transformed ones — the
  // interpreter did not run for it — so the stand-in would paint untransformed
  // geometry over the scene. Dropping it loses one draw; falling back paints a
  // wrong one, and a whole frame of them is a flat red screen.
  if (hasVertexStage && !d.gpuVertex) {
    ++m_gpuVertexDropped;
    return;
  }
  d.blendEnable = blendEnable;
  d.srcBlend = srcBlend;
  d.destBlend = destBlend;
  d.blendOp = blendOp;
  d.cullMode = cullMode;
  // Bit 0 has PA_SU_VTX_CNTL::PIX_CENTER's meaning -- 0 is Direct3D 9 centres
  // at .0 and needs the offset, 1 is the host's own .5 and does not -- but the
  // value currently comes from the d3d9_half_pixel_offset cvar rather than from
  // the register, which is not locatable in the device shadow. Keeping the
  // decode identical means the register drops straight in when it is found.
  d.guestVpWidth = guestVpWidth;
  d.guestVpHeight = guestVpHeight;
  d.useGuestVp = useGuestVp;
  d.edramCopy = edramCopy;
  // Interned HERE rather than at draw time: StencilIndexFor mutates the intern
  // table, and the draw loop runs on the render thread while this runs on the
  // submitting one. Doing it at submission also means the index is fixed by the
  // time the draw is queued, so a state that arrives later cannot renumber a
  // draw already in the list.
  if (stencil && stencil->enable) {
    d.stencil = *stencil;
    d.stencilIndex = StencilIndexFor(*stencil);
    ++m_stencilDraws;
    // kAlways is 7 in the GUEST encoding, which is what GameStencil carries.
    if (stencil->frontFunc != 7u || stencil->backFunc != 7u)
      ++m_stencilTestingDraws;
  }
  d.halfPixel = (vtxCntl != 0xFFFFFFFFu && (vtxCntl & 1u) == 0) ? 0.5f : 0.0f;
  if (d.halfPixel != 0.0f) ++m_halfPixelDraws; else ++m_halfPixelSkipped;
  if (scissor) {
    d.scissorSeen = true;
    d.scissorLeft = scissor[0];
    d.scissorTop = scissor[1];
    d.scissorRight = scissor[2];
    d.scissorBottom = scissor[3];
  }
  if (planes && planeCount >= 3) {
    d.planeCount = std::min<uint32_t>(planeCount,
                                      kMaxDrawPlanes);
    for (uint32_t i = 0; i < d.planeCount; ++i) d.planes[i] = planes[i];
    d.yuvHasAlpha = yuvHasAlpha;
    d.yuvComposite = true;
  }

  // Carry the translator's transform. Without this the draw renders under the
  // fallback identity matrix in m_gameCB, which makes a correct transform and a
  // broken one look identical on screen. A null mvp, or a CB that fails to
  // allocate, falls back to that identity rather than dropping the draw.
  if (mvp && createBuffer(d.cb, 256)) {
    memcpy(d.cb.cpu, mvp, 16 * sizeof(float));
  }

  m_gameDraws.push_back(std::move(d));
}

bool D3D12Renderer::CreateGameRenderTargets() {
  D3D12_HEAP_PROPERTIES hp = {};
  hp.Type = D3D12_HEAP_TYPE_DEFAULT;

  D3D12_RESOURCE_DESC rd = {};
  rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  rd.Width = m_width;
  rd.Height = m_height;
  rd.DepthOrArraySize = 1;
  rd.MipLevels = 1;
  rd.Format = kBackBufferFormat;
  rd.SampleDesc.Count = 1;
  rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

  D3D12_CLEAR_VALUE cv = {};
  cv.Format = kBackBufferFormat;
  cv.Color[0] = 0.05f; cv.Color[1] = 0.08f;
  cv.Color[2] = 0.18f; cv.Color[3] = 1.0f;

  if (FAILED(m_device->CreateCommittedResource(
      &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
      &cv, IID_PPV_ARGS(&m_gameRT)))) {
    LogError("CreateGameRT: game RT failed");
    return false;
  }

  {
    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    // Descriptor zero is the final 1280x720 target; the remaining descriptors
    // are stable slots for D3D9 offscreen surface identities.
    hd.NumDescriptors = kMaxGameRenderTargets + 1;
    if (FAILED(m_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_gameRtvHeap)))) {
      return false;
    }
    m_gameRtvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    m_device->CreateRenderTargetView(m_gameRT.Get(), nullptr,
        m_gameRtvHeap->GetCPUDescriptorHandleForHeapStart());
  }

  rd.Format = kGameDepthFormat;
  rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
  cv.Format = kGameDepthFormat;
  cv.DepthStencil.Depth = 1.0f;
  cv.DepthStencil.Stencil = 0;

  if (FAILED(m_device->CreateCommittedResource(
      &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_DEPTH_WRITE,
      &cv, IID_PPV_ARGS(&m_gameDepth)))) {
    LogError("CreateGameRT: depth failed");
    return false;
  }

  {
    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    hd.NumDescriptors = 1;
    if (FAILED(m_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_gameDsvHeap)))) {
      return false;
    }
    m_device->CreateDepthStencilView(m_gameDepth.Get(), nullptr,
        m_gameDsvHeap->GetCPUDescriptorHandleForHeapStart());
  }

  // Depth for OFFSCREEN targets, one surface per guest depth object. The main
  // target keeps m_gameDepth above; only offscreen draws, which previously had
  // no depth attachment at all, are served from this heap.
  {
    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    hd.NumDescriptors = kMaxGameDepthTargets + 1;
    if (FAILED(m_device->CreateDescriptorHeap(
            &hd, IID_PPV_ARGS(&m_gameDepthDsvHeap))))
      return false;
    m_gameDsvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
  }

  LogInfo("CreateGameRT: done");
  return true;
}

void D3D12Renderer::PresentGameFrame() {
  if (!m_gameRT) return;

  // Blit path: the finished scene lives in a guest-sized offscreen target, so
  // it has to be scaled to the backbuffer rather than copied. m_viewport already
  // carries the pillarbox, so drawing through it puts the image in the same
  // place the copy path did.
  if (m_hasGamePipeline && m_presentVB) {
    GameRenderTarget* presentSource = nullptr;
    if (m_presentResolveTexture) {
      auto it = m_gameSnapshots.find(m_presentResolveTexture);
      if (it != m_gameSnapshots.end() && it->second.resource &&
          !it->second.stale) {
        // Present does not go through BindTranslatedTextures, so without this
        // the one snapshot the whole frame is built from would age out.
        it->second.lastUsedFrame = m_gameFrame;
        presentSource = &it->second;
      }
    }
    if (!presentSource && m_presentSourceObject) {
      auto it = m_gameRenderTargets.find(m_presentSourceObject);
      if (it != m_gameRenderTargets.end() && it->second.resource) {
        presentSource = &it->second;
      }
    }
    if (presentSource) {
      GameRenderTarget& src = *presentSource;
      if (src.state != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) {
        D3D12_RESOURCE_BARRIER toSrv = {};
        toSrv.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toSrv.Transition.pResource = src.resource.Get();
        toSrv.Transition.StateBefore = src.state;
        toSrv.Transition.StateAfter =
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        toSrv.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        m_commandList->ResourceBarrier(1, &toSrv);
        src.state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
      }
      // The backbuffer is already RENDER_TARGET here — EndFrame's RT→PRESENT
      // barrier depends on it still being so when this returns, which is why
      // this path adds no barrier on it at all.
      auto rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
      rtv.ptr += SIZE_T(m_frameIndex) * m_rtvDescriptorSize;
      m_commandList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
      m_commandList->RSSetViewports(1, &m_viewport);
      m_commandList->RSSetScissorRects(1, &m_scissorRect);
      m_commandList->SetGraphicsRootSignature(m_gameRootSig.Get());
      ID3D12DescriptorHeap* heaps[] = {m_gameSrvHeap.Get(),
                                       m_samplerHeap.Get()};
      m_commandList->SetDescriptorHeaps(2, heaps);
      // Textured, no depth, colour write on. See the pso_index bits in
      // CreateGamePipeline.
      m_commandList->SetPipelineState(m_gamePSOs[8].Get());
      m_commandList->IASetPrimitiveTopology(
          D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      m_commandList->SetGraphicsRootConstantBufferView(
          0, m_gameCB->GetGPUVirtualAddress());
      auto gpu = m_gameSrvHeap->GetGPUDescriptorHandleForHeapStart();
      gpu.ptr += UINT64(src.srvIndex) * m_gameSrvDescriptorSize;
      m_commandList->SetGraphicsRootDescriptorTable(1, gpu);
      // Clamped: the blit samples a finished frame edge to edge, and wrapping
      // the opposite side in is exactly the seam this change exists to remove.
      auto samp = m_samplerHeap->GetGPUDescriptorHandleForHeapStart();
      samp.ptr += UINT64(kSamplerClampU | kSamplerClampV) *
                  m_samplerDescriptorSize;
      m_commandList->SetGraphicsRootDescriptorTable(2, samp);
      m_commandList->IASetVertexBuffers(0, 1, &m_presentVbv);
      m_commandList->DrawInstanced(3, 1, 0, 0);
      // m_gameRT is untouched on this path, so it must still end in
      // PIXEL_SHADER_RESOURCE for the next BeginFrame's PSR→RT barrier to be
      // valid — the same postcondition the copy path below establishes.
      D3D12_RESOURCE_BARRIER rtToSrv = {};
      rtToSrv.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      rtToSrv.Transition.pResource = m_gameRT.Get();
      rtToSrv.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
      rtToSrv.Transition.StateAfter =
          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
      rtToSrv.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
      m_commandList->ResourceBarrier(1, &rtToSrv);
      return;
    }
  }

  // Forward barriers: m_gameRT RT→COPY_SOURCE (valid copy source), backbuf
  // RT→COPY_DEST (valid copy destination).
  D3D12_RESOURCE_BARRIER barriers[2] = {};
  barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barriers[0].Transition.pResource = m_gameRT.Get();
  barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
  barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
  barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

  barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barriers[1].Transition.pResource = m_renderTargets[m_frameIndex].Get();
  barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
  barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
  barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

  m_commandList->ResourceBarrier(2, barriers);

  D3D12_TEXTURE_COPY_LOCATION dst = {};
  dst.pResource = m_renderTargets[m_frameIndex].Get();
  dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  dst.SubresourceIndex = 0;

  D3D12_TEXTURE_COPY_LOCATION src = {};
  src.pResource = m_gameRT.Get();
  src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  src.SubresourceIndex = 0;

  m_commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

  // Reverse barriers: leave m_gameRT in PIXEL_SHADER_RESOURCE so the next
  // BeginFrame's PSR→RT barrier is valid; leave backbuf in RENDER_TARGET so
  // EndFrame's RT→PRESENT barrier is valid. Previous code left m_gameRT in RT
  // (next BeginFrame's PSR→RT would have been an invalid barrier) and backbuf
  // in PRESENT (EndFrame's RT→PRESENT would also have been invalid).
  barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
  barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

  barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
  barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;

  m_commandList->ResourceBarrier(2, barriers);
}
