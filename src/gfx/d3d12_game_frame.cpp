// D3D12Renderer -- per-frame submission: the draw/clear/resolve queue,
// RenderGameFrame, and present.
//
// Split verbatim out of d3d12_game.cpp. RenderGameFrame is ~1900 lines and
// AddGameDraw ~600; both moved WHOLE, because splitting a function body is a
// behaviour risk rather than a move.
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
  // new draw list. High-water is taken BEFORE the reset, so it measures the
  // frame that just ended.
  //
  // The base comes from m_translatedBlockLimit, NOT m_frameIndex: by here
  // m_frameIndex has advanced to the frame about to be recorded while
  // m_translatedBlockNext still points into the one that ended, so deriving the
  // base from the new index reads across the wrap and prints a high-water mark
  // exceeding its own limit.
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
  for (auto& [object, target] : m_gameDepthTargets) {
    target.usedThisFrame = false;
    // The owner's depth is rewritten every frame, so a band's copy is only
    // good for the frame it was taken in.
    target.bandDepthSynced = false;
  }

  // Which 1x1 resolve of this frame we are looking at, for the rotation in
  // QueueLuminanceReadback.
  uint32_t oneByOneSeen = 0;
  uint32_t boundTargetObject = 0;  // zero is the final m_gameRT.
  // The scissor last handed to the command list. Tracked so the per-draw scissor
  // costs one comparison rather than a state change per draw, and so the
  // target-binding blocks below no longer own the scissor at all -- they set the
  // viewport, this owns the rectangle.
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
  // The routing decision is made when a surface is DRAWN INTO, but the facts it
  // needs -- will this be resolved, will a later draw sample it -- are only
  // known when the resolve or the sample arrives, often in a different frame.
  // Built per frame, a surface drawn in N and resolved in N+1 is not a resolve
  // source in N, so N routes it to m_gameRT (cleared every frame) and N+1's
  // resolve finds no offscreen entry and copies nothing.
  //
  // History is the right basis because these are properties of a surface's ROLE,
  // which is stable. Cost is bounded by the same budget, and the "refused:
  // budget" figure on the routing line says if it bites.
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
  // say whether one is the scene and the other a one-draw overlay -- and
  // presenting the LAST one written is only right in the first case. A
  // compositor writes its output last, a UI layer is written last over a scene
  // finished earlier, and those want opposite choices.
  std::vector<std::pair<uint32_t, uint32_t>> fullSizeOrder;
  for (const auto& d : m_gameDraws) {
    // A SURFACE BIND: the guest named this surface as an attachment. Create host
    // storage now, whether or not any draw we route targets it, and clear it to
    // its documented creation value so a surface bound and resolved without a
    // single draw reads as empty rather than as whatever the pool handed us.
    //
    // This is what makes a depth-only pass cost nothing. Storage used to be
    // created by the first DRAW naming a surface, so the menu's shadow atlas --
    // bound depth-only, resolved 287 times in one run -- was never instantiated,
    // its resolve found no source, and every draw sampling the result was
    // discarded. Xenia creates render targets from register state with depth as
    // an equal peer of colour (render_target_cache.cc:888).
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
      // drew into is exactly what m_snapshotBlankSource counts, and the surface
      // now existing is a different claim from the surface having contents.
      continue;
    }
    // A resolve: snapshot the source target as it stands right now, so draws
    // recorded after this point sample these contents rather than whatever the
    // shared surface holds by the end of the frame. Draws nothing.
    if (d.resolveDest) {
      // What KIND of image this destination holds, recorded before any of the
      // branching below can refuse the resolve. A slot naming a destination we
      // have no snapshot for used to fail the whole draw, with nothing to say
      // whether the missing image was depth or colour -- so the only substitute
      // available was a blanket one, which put white over the Bink logo.
      // Recorded for EVERY resolve, including ones dropped for a missing source,
      // because those are precisely the destinations that end up with no
      // snapshot.
      m_resolveDestIsDepth[d.resolveDest] = d.resolveSourceIsDepth;
      // Where the source actually rendered. Resolve sources are routed offscreen
      // (isResolveSource, below), so this normally finds a surface of the
      // source's own -- which is the point: offscreen targets are only cleared
      // when something draws into them, so they carry their contents across
      // frames, and a resolve needs exactly that.
      ID3D12Resource* srcRes = nullptr;
      GameRenderTarget* srcEntry = nullptr;
      uint32_t srcWidth = 0, srcHeight = 0;
      D3D12_RESOURCE_STATES srcState = D3D12_RESOURCE_STATE_RENDER_TARGET;
      // The snapshot takes the source's format: a depth resolve has to land in
      // R32_FLOAT, not RGBA8, for CopyTextureRegion to accept it.
      DXGI_FORMAT snapFormat = kBackBufferFormat;
      // Gather the EDRAM bands of a depth resolve BEFORE looking for a target of
      // the source's own, because the two are in competition: surfaces are
      // created when the guest binds them, so the whole surface a banded pass
      // resolves out of is in the depth pool too and the direct lookup would
      // quietly copy a surface nothing rendered into. Bands win, but only when
      // something has been drawn into them.
      //
      // Excluding the resolve's OWN object from the band set is load-bearing:
      // the 768x1024 atlas matches the band filter on width and base as well as
      // its own 768x640 band does, and including it makes the heights sum to
      // 2048 against a 1024 destination, failing the exact-cover test.
      std::vector<GameRenderTarget*> depthBands;
      bool depthBandsDrawn = false;
      if (d.resolveSourceIsDepth && d.resolveDestWidth && d.resolveDestHeight) {
        // EVERY candidate considered, and why each was rejected. The refusal
        // counters say the stitch failed and with what totals, not which
        // surfaces were looked at. Four things can drop a candidate -- it is the
        // resolve source object, it has no resource yet, its width differs, or
        // its base is below the resolve -- and they need different repairs.
        for (auto& [obj, t] : m_gameDepthTargets) {
          const char* why = nullptr;
          if (obj == d.resolveSource) why = "is-resolve-source";
          else if (!t.resource) why = "no-resource";
          else if (t.width != d.resolveDestWidth) why = "width";
          else if (t.edramBase < d.resolveSourceBase) why = "base-below";
          if (d.resolveSourceIsDepth && d.resolveDestHeight > 720) {
            static std::map<uint64_t, uint64_t> s_seen;
            const uint64_t key = (uint64_t(obj) << 20) ^
                                 (uint64_t(t.width) << 8) ^ t.height;
            if (s_seen.size() < 24 && s_seen[key]++ == 0) {
              char m[224];
              std::snprintf(m, sizeof(m),
                            "band candidate 0x%08X %ux%u base 0x%X drawn %d"
                            " dsc %d -> %s (resolve src 0x%08X base 0x%X"
                            " dest %ux%u, writebacks %llu)",
                            obj, t.width, t.height, t.edramBase,
                            t.everDrawn ? 1 : 0,
                            t.drawnSinceClear ? 1 : 0,
                            why ? why : "ACCEPTED", d.resolveSource,
                            d.resolveSourceBase, d.resolveDestWidth,
                            d.resolveDestHeight,
                            (unsigned long long)m_depthBandWriteBacks);
              LogInfo(m);
            }
          }
          if (!why) depthBands.push_back(&t);
        }
        std::sort(depthBands.begin(), depthBands.end(),
                  [](const GameRenderTarget* a, const GameRenderTarget* b) {
                    return a->edramBase < b->edramBase;
                  });
        uint32_t total = 0;
        for (const GameRenderTarget* b : depthBands) total += b->height;
        // Anything that is not an exact cover of the destination, starting at
        // the resolve's own base, is not a banding of this surface. WHICH of the
        // three refuses, and with what numbers: a BLANK-SOURCE count alone
        // cannot say whether the stitch was unavailable, mis-shaped or simply
        // not yet drawn. The exact-cover test is the fragile one by construction
        // -- it sums EVERY same-width target at or above the base, so one extra
        // aliasing surface makes the total overshoot.
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
      // BAND WRITE-BACK, before the source is read.
      //
      // The band copy at the draw site runs OWNER -> BAND, so a band can depth
      // test against content already in the surface. That is the whole story for
      // the deferred light bands, which only READ. The shadow map band is
      // WRITTEN: 553 (768x1024) is cleared, copied into 554 (768x384), the
      // casters put 38 draws into 554, and then the resolve reads 553 -- which
      // still holds nothing but the clear, so the shadow map resolved
      // min = max = 1.0 and every lookup said "unshadowed".
      //
      // This carries the band back, for bands of THIS resolve source that have
      // actually been drawn into. It has fired ZERO times in every run on record
      // -- including runs taken right after it shipped, when shadows were
      // confirmed back -- so it is NOT what fixed them.
      static std::atomic<uint64_t> s_wbNoOwner{0}, s_wbOwnerDrawn{0},
          s_wbNotOurBand{0}, s_wbBandNotDrawn{0}, s_wbWidth{0}, s_wbRow{0},
          s_wbAccepted{0}, s_wbResolves{0};
      if (d.resolveSourceIsDepth) {
        auto oit = m_gameDepthTargets.find(d.resolveSource);
        const bool have_owner =
            oit != m_gameDepthTargets.end() && oit->second.resource;
        if (!have_owner) ++s_wbNoOwner;
        else if (oit->second.drawnSinceClear) ++s_wbOwnerDrawn;
        if ((++s_wbResolves % 4000) == 0) {
          char wb[400];
          std::snprintf(
              wb, sizeof(wb),
              "  BAND WRITE-BACK census over %llu depth resolves: accepted "
              "%llu | rejected: no-owner %llu, owner already drawn %llu, band "
              "belongs to another owner %llu, band not drawn %llu, width "
              "mismatch %llu, row+height past owner %llu",
              (unsigned long long)s_wbResolves.load(),
              (unsigned long long)s_wbAccepted.load(),
              (unsigned long long)s_wbNoOwner.load(),
              (unsigned long long)s_wbOwnerDrawn.load(),
              (unsigned long long)s_wbNotOurBand.load(),
              (unsigned long long)s_wbBandNotDrawn.load(),
              (unsigned long long)s_wbWidth.load(),
              (unsigned long long)s_wbRow.load());
          LogInfo(wb);
        }
        if (have_owner && !oit->second.drawnSinceClear) {
          // ONLY into an owner nothing has drawn into. Without this the
          // write-back also fires for the deferred LIGHT bands, which are bands
          // of the 1280x720 scene depth, and the 1280x80 band at base 0x280 is
          // its bottom 80 rows: copying it back overwrote the bottom of the
          // scene depth buffer. The case this exists for is the opposite -- an
          // owner holding NOTHING but its clear while the real content sits in a
          // band, which is the shadow map.
          GameRenderTarget& owner = oit->second;
          for (auto& [bobj, band] : m_gameDepthTargets) {
            if (bobj == d.resolveSource) continue;
            if (band.bandDepthOwner != d.resolveSource) {
              ++s_wbNotOurBand;
              continue;
            }
            if (!band.resource || !band.drawnSinceClear) {
              ++s_wbBandNotDrawn;
              continue;
            }
            if (band.width != owner.width) {
              ++s_wbWidth;
              continue;
            }
            if (band.bandDepthRow + band.height > owner.height) {
              ++s_wbRow;
              continue;
            }
            ++s_wbAccepted;
            D3D12_RESOURCE_BARRIER toCopy[2] = {};
            for (auto& b : toCopy)
              b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            toCopy[0].Transition.pResource = band.resource.Get();
            toCopy[0].Transition.StateBefore = band.state;
            toCopy[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            toCopy[0].Transition.Subresource =
                D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            toCopy[1].Transition.pResource = owner.resource.Get();
            toCopy[1].Transition.StateBefore = owner.state;
            toCopy[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            toCopy[1].Transition.Subresource =
                D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            m_commandList->ResourceBarrier(2, toCopy);

            D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
            dstLoc.pResource = owner.resource.Get();
            dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dstLoc.SubresourceIndex = 0;  // depth plane only, as on the way in
            D3D12_TEXTURE_COPY_LOCATION srcLoc = dstLoc;
            srcLoc.pResource = band.resource.Get();
            D3D12_BOX box = {};
            box.right = band.width;
            box.bottom = band.height;
            box.back = 1;
            m_commandList->CopyTextureRegion(&dstLoc, 0, band.bandDepthRow, 0,
                                             &srcLoc, &box);

            D3D12_RESOURCE_BARRIER back[2] = {toCopy[0], toCopy[1]};
            back[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
            back[0].Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
            back[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            back[1].Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
            m_commandList->ResourceBarrier(2, back);
            band.state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
            owner.state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
            // NOT owner.everDrawn = true. That was self-referential: it made the
            // owner look drawn to the stitcher and to the guard above, so the
            // second write-back of a frame saw a "drawn" owner it had marked
            // itself.
            ++m_depthBandWriteBacks;
          }
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
        // A DEPTH resolve. The guest reads its depth buffer back as a texture to
        // reconstruct world position in the deferred lighting pass, and Resolve
        // names the depth surface by object exactly as it names a colour one
        // (source slot 4). Looking only in the colour map is what made every one
        // of these miss.
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
      // 0x2653FF20 is what the resolve names as its source, both 129x129 at base
      // 0x2D0. Object identity cannot connect them, so the resolve found
      // nothing, the snapshot never appeared, and every draw sampling it was
      // discarded -- including the draws into that same target, a permanent
      // deadlock.
      //
      // Match on the EDRAM base and the extent instead. Both must agree and the
      // base must be non-zero, so a target at an unknown base cannot capture an
      // unrelated resolve. Matched on the SOURCE's own extent, not the
      // destination texture's, which reads 0x0 when it could not be decoded.
      //
      // Two passes, exact before containment. Exact is one allocation named
      // twice at one size. Containment is the MULTISAMPLE case: a 640x360 source
      // with 4x MSAA against a 640x720 at 1x, both at base 0x2D0, where only the
      // 640x720 is ever drawn into -- we render everything at 1x, so the samples
      // the guest would resolve down are not there. PROVISIONAL: the one step
      // here not established from evidence.
      //
      // HOLDING A SURFACE IS NOT HOLDING ITS CONTENTS. This used to run only
      // when the object lookup found nothing, and so never ran at all: the
      // failing case is not a missing resource but a resource nothing rendered
      // into, whose zeros the auto-exposure ladder then measured.
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
        // THE MULTISAMPLE PARTNER. 4x MSAA on Xenos is 2x2, so a 640x360 4x
        // surface occupies the same EDRAM samples as a 1280x720 at 1x -- and we
        // render everything at 1x, so the partner IS the image the guest would
        // have resolved down.
        //
        // Matched on FORMAT as well as extent: searching on base and width alone
        // found a 640x720 RGBA8 from an unrelated pass, and 1120 "contained"
        // matches later the exposure was still diverging. At base 0x2D0 the
        // drawn RGBA16F is 1280x720, exactly twice the source in both axes.
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
          // The partner is twice the size in each axis, so the guest's source
          // rectangle -- in the 1x coordinates it thinks it resolved -- has to be
          // doubled. The snapshot then comes out at full resolution and the
          // shader's normalized UVs map [0,1] across it exactly as before.
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
      // A BANDED depth resolve, which no object-identity lookup can satisfy. The
      // shadow pass renders 768x1024 as two EDRAM bands -- 768x640 at base 0x580
      // and 768x384 at 0x710 -- and resolves the whole image through a THIRD
      // surface object that aliases band 0's base and that no draw ever binds.
      // Stitch them by EDRAM base, which is the only thing that says which band
      // is on top; the gather and its exact-cover test live above.
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
      // Only for a DEPTH source with an extent to build from: a colour source
      // has the aliased-source matcher above, which is measured and works, and
      // an invented empty colour target would compete with it.
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
        // The source has no offscreen surface -- it was refused one (budget or a
        // size change), so it rendered into m_gameRT with everything else.
        //
        // Do NOT copy from m_gameRT as a fallback. It is cleared every frame and
        // accumulates every logical surface, so it does not hold the source's
        // contents at resolve time: 25 of 33 resolves in a frame have sources
        // with no draws at all that frame, and copying from it captures the
        // clear colour. Refusing leaves the draw on the old aliased path, which
        // is wrong but is what it had before.
        ++m_snapshotMissingSource;
        // Which sources, and how often. Counted rather than logged on first
        // sighting: the first miss for any source happens in the opening frames
        // before anything has drawn into it, so a once-per-source line reports
        // startup state as the steady state.
        ++m_missingSourceCounts[d.resolveSource];
        // The guest asked for this image to be refreshed and it was not. Any
        // snapshot already held is now a stale earlier frame; mark it so the
        // sampling path refuses it rather than blitting a previous frame over
        // this one. ~600 dropped refreshes per sample window with ZERO offscreen
        // refusals, so these are sources we have no entry for at all.
        if (auto st = m_gameSnapshots.find(d.resolveDest);
            st != m_gameSnapshots.end()) {
          st->second.stale = true;
        }
        continue;
      }
      // Snapshotting a target nothing has ever drawn into copies a blank
      // surface. The copy still happens -- refusing it would freeze the previous
      // snapshot, which is worse -- but it is counted, because a large number
      // here means compositor quads are painting blanks over the frame and the
      // real defect is upstream.
      if (srcEntry && !srcEntry->everDrawn) {
        ++m_snapshotBlankSource;
        // The population behind that count. Keyed by SOURCE extent, so one line
        // describes a surface instead of an event, and carrying the frame range
        // because the whole question is whether these are boot-only.
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
      // band at y 640..720 arrives as a rectangle our 1280x80 source cannot
      // contain. Clamping and then falling back to the whole source makes both
      // conventions land correctly.
      uint32_t sx = 0, sy = 0, copyW = srcWidth, copyH = srcHeight;
      // Scaled by srcScale, which is 1 for every source but a multisample
      // partner. The guest states its rectangle in the resolution it believes it
      // rendered; taking it unscaled off a 2x partner would copy the top-left
      // quarter of the scene and call it the whole image.
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
      // region this resolve happens to cover. Covering was right for a banded
      // resolve, which eventually fills the whole image, and wrong for an atlas,
      // which never does: the menu scene's 2048x2048 atlas is built from
      // repeated 256x256 sub-rect resolves, so the first one created a 256x256
      // snapshot and normalized UVs then mapped [0,1] across it.
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
      // texture" -- and the snapshot had to be resized to the band to satisfy
      // it. Copying the band to its own offset lets the two bands of a tiled
      // resolve assemble into one image.
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
      // Put both back where they were. An offscreen entry can go to
      // shader-resource and be transitioned again on demand, but m_gameRT must
      // return to RENDER_TARGET: the rest of the frame keeps drawing into it,
      // and PresentGameFrame's own RT->COPY_SOURCE barrier declares that as the
      // before-state.
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
      // VdSwap, and resolves keep their guest order in m_gameDraws, so the last
      // successful whole 1280x720 colour resolve is the frame to present.
      // Presenting d.resolveSource instead is incorrect: that object is shared
      // scratch storage and later post-processing draws may overwrite it.
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
      // And the same for any destination whose COPIED REGION is small enough to
      // travel, whatever the destination's own extent: the VT feedback buffer is
      // 64x64 resolved to (0,0), and the terrain deformation is a 128x32 tile
      // resolved into a 2048x2048 accumulation at a destpoint. The destpoint
      // goes with it, AND the region's extent -- both were omitted while the
      // feedback buffer was the only caller, where they were indistinguishable
      // from the defaults.
      QueueSurfaceReadback(snap, d.resolveDest, d.resolveDestWidth,
                           d.resolveDestHeight, dx, dy, copyW, copyH);
      continue;
    }
    // A full-surface D3D9 DEPTH clear, ordered among the draws.
    //
    // Before this the ONLY depth clear was the once-per-frame first-use one
    // below, whose reasoning ("one depth surface serves several colour targets
    // in a pass") is right WITHIN a pass and wrong ACROSS passes -- the guest
    // separates its passes with its own clears and we were discarding every one.
    //
    // usedThisFrame is set here too, so this and the first-use clear cannot both
    // fire on the same target in the same frame: the guest's clear wins and ours
    // becomes the fallback for a frame where the guest issues none.
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
        // right for our first-use clears, which initialise a fresh surface, and
        // wrong here: the guest issues depth-only (0x1F), stencil-only (0x20)
        // and both (0x30), and clearing stencil alongside every depth clear
        // would wipe a mask it deliberately kept.
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
        // The clear is where this becomes false: whatever the surface held
        // before, it now holds only the clear value. everDrawn does NOT reset
        // here, which is why it cannot answer "does this surface have real
        // content".
        clearTarget->drawnSinceClear = false;
      }
      // Clear does not bind through the normal draw path. Force the following
      // draw to restore its RTV/DSV and viewport.
      boundTargetObject = 0xFFFFFFFFu;
      boundDepthObject = 0xFFFFFFFFu;
      continue;
    }

    // Keep only the unsampled final 1280x720 surface on m_gameRT so
    // PresentGameFrame remains an exact-size copy. A full-size scene target that
    // a later compositor samples is still offscreen and needs its own SRV;
    // classifying solely by dimensions made that target alias m_gameRT and left
    // the final draw with nothing it could legally sample.
    GameRenderTarget* drawTarget = nullptr;
    // Declared out here because the depth-state decision below needs it.
    GameRenderTarget* depthTarget = nullptr;
    const bool feedsLaterDraw =
        d.targetObject && sampledTargets.contains(d.targetObject);
    // A resolve source needs storage that outlives the frame: of 33 resolves in
    // one frame, 25 had sources with no draws at all that frame, their contents
    // established earlier. m_gameRT is cleared every frame so it can never hold
    // them at resolve time, while offscreen targets are cleared only when
    // something draws into them.
    //
    // Bisected off twice, and neither time was it the cause: a white-screen
    // regression traced to exempting sampled_render_target_object from the
    // colourless filter, and a 3s/frame stall to a descriptor leak in the
    // snapshot path.
    const bool isResolveSource =
        d.targetObject && resolveSources.contains(d.targetObject);
    // A DEPTH-ONLY pass has no colour target at all -- the shadow map is
    // 768x1024 with "colour target now 0x00000000" -- so d.targetObject is 0 and
    // every one of its draws fell through to the main render target. Two
    // measured consequences: the shadow geometry overpainted the backbuffer, and
    // the depth surface was never created, so the guest's depth resolve out of
    // it found no source and every draw sampling the shadow map at s15 was
    // discarded -- including the whole 320x180 luminance pass, which is why the
    // exposure divides by zero.
    //
    // Route it like any other offscreen pass, keyed by the DEPTH object, with a
    // scratch colour target at the same extent. That scratch target is not
    // wasted: every PSO declares NumRenderTargets = 1, so binding no RTV would
    // be invalid work against a pipeline that expects one.
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
        // frame, which is only correct if there is exactly one. Seven 1280x720
        // surfaces are live, and if the guest composites the scene across
        // several, presenting one shows a single layer.
        if (fullSizeTargets.insert(d.targetObject).second)
          fullSizeOrder.emplace_back(d.targetObject, 0);
        for (auto& e : fullSizeOrder)
          if (e.first == d.targetObject) ++e.second;
        ++fullSizeDraws;
      }
    }
    // The three populations, kept apart on purpose. A draw that never wanted an
    // offscreen target and one that wanted it and was refused both end up on the
    // main render target, and a single "drew on main" count cannot tell them
    // apart -- but only the second is a silent regression to overpainting.
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
      // Depth for this offscreen target, sized from the DEPTH surface's own
      // declared extent, never from the colour target's.
      //
      // Sizing it from the colour target collapsed the frame -- 2664 offscreen
      // draws fell to 133, resolve copies rose from 1750 to 30166 -- because the
      // same depth object then demanded two different sizes and was retired and
      // recreated on every alternation. It demands two sizes because DeviceState
      // is thread_local: a draw carries whatever depth surface ITS thread last
      // saw.
      //
      // The guest itself pairs one depth surface per colour target at matching
      // extents, including a separate depth object for each EDRAM band, so each
      // depth object has exactly one size and this never resizes. Binding only
      // on an exact extent match keeps that honest: a stale pairing skips depth
      // for that draw rather than binding a DSV whose size disagrees with the
      // RTV.
      depthTarget = (d.depthObject && d.depthWidth == drawTarget->width &&
                     d.depthHeight == drawTarget->height)
                        ? EnsureGameDepthTarget(d.depthObject, d.depthWidth,
                                                d.depthHeight, d.depthBase)
                        : nullptr;
      // EDRAM BAND DEPTH. A band sitting at a row offset inside another surface
      // takes a copy of the owner's rows before its first draw of the frame --
      // see GameRenderTarget::bandDepthOwner for why it copies rather than
      // shares. Without this the band tests against its own creation clear,
      // which for the menu's second light band meant a far plane and every light
      // in those rows discarded. Depth plane only: the stencil plane belongs to
      // the guest's own per-light clears.
      if (depthTarget && depthTarget->bandDepthOwner &&
          !depthTarget->bandDepthSynced) {
        depthTarget->bandDepthSynced = true;
        auto oit = m_gameDepthTargets.find(depthTarget->bandDepthOwner);
        if (oit != m_gameDepthTargets.end() && oit->second.resource &&
            oit->second.everDrawn &&
            depthTarget->bandDepthRow + depthTarget->height <=
                oit->second.height) {
          GameRenderTarget& owner = oit->second;
          D3D12_RESOURCE_BARRIER toCopy[2] = {};
          for (auto& b : toCopy)
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
          toCopy[0].Transition.pResource = owner.resource.Get();
          toCopy[0].Transition.StateBefore = owner.state;
          toCopy[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
          toCopy[0].Transition.Subresource =
              D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
          toCopy[1].Transition.pResource = depthTarget->resource.Get();
          toCopy[1].Transition.StateBefore = depthTarget->state;
          toCopy[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
          toCopy[1].Transition.Subresource =
              D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
          m_commandList->ResourceBarrier(2, toCopy);

          D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
          dstLoc.pResource = depthTarget->resource.Get();
          dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
          dstLoc.SubresourceIndex = 0;
          D3D12_TEXTURE_COPY_LOCATION srcLoc = dstLoc;
          srcLoc.pResource = owner.resource.Get();
          D3D12_BOX box = {};
          box.top = depthTarget->bandDepthRow;
          box.bottom = depthTarget->bandDepthRow + depthTarget->height;
          box.right = depthTarget->width;
          box.back = 1;
          m_commandList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, &box);

          // Both back to DEPTH_WRITE: the band is about to be rendered into and
          // the owner is still the scene's depth buffer.
          D3D12_RESOURCE_BARRIER back[2] = {toCopy[0], toCopy[1]};
          back[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
          back[0].Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
          back[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
          back[1].Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
          m_commandList->ResourceBarrier(2, back);
          owner.state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
          depthTarget->state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
          // The band now holds real geometry, so NEITHER clear may wipe it back
          // to the far plane. usedThisFrame is the one that matters: the
          // per-frame clear below is gated on it alone, so setting only
          // needsInitialClear left it firing straight after this copy.
          depthTarget->needsInitialClear = false;
          depthTarget->usedThisFrame = true;
          depthTarget->everDrawn = true;
          depthTarget->drawnSinceClear = true;
          ++m_depthBandDepthCopies;
        } else {
          ++m_depthBandCopySkipped;
        }
      }
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
      // consecutive draws onto the same colour target keep the first one's depth
      // binding -- including the case where the first bound no DSV at all and
      // the second runs a depth-enabled PSO against it, which hangs the device.
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
        // MRT SLOT 1. The guest's terrain tile pass binds two 256x256 targets
        // and resolves the SECOND; rendering only slot 0 left slot 1 never
        // drawn, its resolve dropped for want of a source, and the tile shader's
        // texture bind failing. A failure to ensure it falls back to
        // single-target binding rather than dropping the draw, and the PSO key
        // is built from what was actually bound so the two cannot disagree.
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
        viewport.TopLeftX = 0.0f;
        viewport.TopLeftY = 0.0f;
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
        // THE FIRST-USE CLEAR IS GONE -- the first guard removed under
        // docs/strict_mode.md rather than merely measured.
        //
        // It cleared a target to kOffscreenClear on its first use in a frame,
        // which nothing in the guest asked for: correct for a target the guest
        // refills every frame and WRONG for an accumulation buffer, since the
        // terrain deformation ping-pong writes a Laplacian DELTA that only
        // integrates if the previous contents survive.
        //
        // WHY DELETE RATHER THAN DEFAULT-OFF: with the preserve path forced on,
        // the guard was given 27,316 opportunities and needed NONE of them,
        // while the run still rendered 1720 frames at 797 guest draws accepted
        // and 0 refused. The guest issues its own colour and depth clears, so
        // there is no gap left to cover. `m_targetCarriedContent` is KEPT and
        // counts what it always measured -- targets whose previous-frame content
        // survives -- which is how a reintroduced clear would be spotted.
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
        // DEPTH ONLY. This is our per-frame schedule, not the guest's, and the
        // stencil plane belongs to the guest's own clears; the creation-time
        // clear at the needsInitialClear sites gives stencil its first value.
        //
        // MEASURED, so nobody has to try it twice: adding
        // D3D12_CLEAR_FLAG_STENCIL here was A/B'd and it is WORSE -- the menu
        // text stops appearing entirely and the white tires come back. The guest
        // DELIBERATELY PERSISTS STENCIL MASKS ACROSS FRAMES.
        m_commandList->ClearDepthStencilView(dsv, kGameDepthFrameClearFlags,
                                             1.0f, 0, 0, nullptr);
        depthTarget->usedThisFrame = true;
        depthTarget->everDrawn = true;
        depthTarget->drawnSinceClear = true;
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
      m_commandList->RSSetViewports(1, &mainViewport);
      boundTargetObject = 0;
      // The main target binds m_gameDepth, which is not one of the per-object
      // depth surfaces, so the offscreen path must treat this as "not mine".
      boundDepthObject = 0;
    }

    // THE GUEST'S SCISSOR. Ignoring it drew the compass strip across the whole
    // frame, and anything else the guest clips to a sub-rectangle was equally
    // unclipped.
    //
    // The guest rectangle is in its own render-target pixels. Off-screen targets
    // are created at the guest's dimensions so it maps 1:1 there; the main
    // target is letterboxed by m_viewport, so the same linear map is applied
    // here. Both are then INTERSECTED with the full-target rectangle rather than
    // replacing it, which makes a stale scissor from a larger target harmless.
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
        // An empty or inverted rectangle is not a reason to draw everything. The
        // guest asking for nothing means nothing, and D3D12 rejects a rectangle
        // whose right is below its left, so it is collapsed rather than widened.
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
      // The lookup deliberately runs even when the draw's own target is the one
      // that was resolved from. The old `sampled != target` guard existed
      // because a resource cannot be read and written in the same draw -- but a
      // snapshot is a separate resource captured earlier, so that hazard is gone
      // and the guard was rejecting the common case: one shared scratch surface
      // that is both what the draw renders into and what it samples a previous
      // resolve of. With the guard in place this measured hits 0.
      //
      // The fallback keeps the old live-surface path under the old guard,
      // because that IS still a read-write hazard. It is wrong whenever more
      // than one texture resolves out of that target, but it beats binding
      // nothing.
      GameRenderTarget* sampledPtr = nullptr;
      // A STALE snapshot is refused outright: it holds a complete earlier frame
      // at full screen size, so binding it does not degrade the draw, it
      // replaces the frame.
      //
      // A DEPTH snapshot is not a colour source. The tex*col stand-in has one
      // texture and multiplies it by the vertex colour, so an R32_FLOAT depth
      // image gives (depth, 0, 0, 1) * white -- the flat red sheet that appeared
      // over the menu the first time depth resolves started succeeding.
      //
      // Both fall through to the fabricated-colour gate, which shows what is
      // underneath.
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
        // Not counted for a depth snapshot: that refusal has its own counter and
        // is deliberate, whereas STALE-REFUSED means a resolve was dropped and
        // the image is a known-wrong earlier frame. Letting the depth refusals
        // fall through here made a healthy number look like 233 lost refreshes.
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
    // vertex colour unmodified, and a draw whose colour was meant to come from a
    // texture has no COLOR element in its declaration, so BuildHleDraw seeds
    // {1,1,1,1}. That seed is correct as a MODULATION IDENTITY -- kGameTexturePS
    // computes tex * col, and zeroing it killed the logo -- but when the
    // multiply never happens it is emitted literally, as opaque white over a
    // geometrically exact fullscreen quad. ("Binding nothing turns such draws
    // black" is false; colourless means white, which is why this read as an
    // overpaint problem for so long.)
    //
    // The gate is the FABRICATION, not the reason the texture is missing: keying
    // on sampledTargetObject was too narrow by an order of magnitude. kPacked
    // and kFallback colours are real vertex data and are left alone; only kNone
    // is invented here.
    //
    // Decided here, applied below once it is known whether the guest's own
    // shader will run. Applying the skip HERE cost the menu its whole
    // post-processing chain: 76 skipped draws were aimed at the 320x180
    // luminance target, which left it cleared and never drawn into, so the guest
    // computed exposure as key/0 and the composite turned the frame to NaN.
    //
    // TWO EXEMPTIONS. A depth-only pass has no colour source and no texture BY
    // DESIGN. And a draw with its COLOUR MASK OFF -- the clause two earlier
    // reverts were missing, because both exempted draws by WHY they lost their
    // translation, which says nothing about what they paint. That population is
    // the guest's depth passes: null pixel shader, binding a colour target so
    // they fail depthOnlyPass, but RB_COLOR_MASK 0 for every one measured.
    // Fabricating white requires writing colour; these cannot.
    //
    // Below: CULL census at PSO-SELECTION time rather than where cullMode is
    // stored. The guest-side probe already proves the register reads cull_back
    // for the draw that blacks out the menu background; what it cannot show is
    // whether the value survives graphics_system -> AddGameDraw -> the PSO key.
    // Counts the PACKED bits, all eight buckets, reported unconditionally.
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
      // 128 draws a frame would drown it. One line per distinct (raw register,
      // packed bits, translated) tuple.
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
    // before the split rather than duplicated inside it. Offscreen draws used to
    // force depth off because they had no attachment; they can have one now, and
    // a draw whose depth surface could not be created still falls back to no
    // depth rather than binding a DSV that does not exist.
    const bool tDepthEnable =
        (drawTarget ? depthTarget != nullptr : true) && d.depthEnable;
    const bool tDepthWrite = tDepthEnable && d.depthWrite;

    // Run the guest's own pixel shader when this draw has everything it needs: a
    // translated shader, its interpolators, and its constant bank. Anything
    // missing keeps the tex*col stand-in rather than rendering a guess.
    //
    // The topology group is computed once, above the split, so the translated
    // and stand-in paths cannot disagree about it -- a disagreement there is
    // invisible except as a draw that does not appear.
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
      // stand-in. key.handle is 0 for these -- no real shader has that handle,
      // so m_translatedPsBlobs caches exactly one compilation of it for the
      // whole run, and the rest of the key still separates them by vertex
      // shader, blend, topology and target format.
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
    // the guard was written for.
    //
    // TRIED AND REVERTED: exempting every draw that HAD a translation and lost
    // it in BindTranslatedTextures, on the argument that they are not "an
    // invented colour" but a draw we chose to discard. They are exactly an
    // invented colour -- no colour source and no texture, so the stand-in paints
    // white -- and letting the 354 such draws on the scene target through turned
    // the whole menu backdrop white. WHITE-SKIPPED fell 451 -> 76 while the
    // picture got worse: the counter was measuring draws starting to render, not
    // starting to render correctly.
    //
    // NARROW EXEMPTION, for the terrain tile pass. Those draws render into a
    // 256x256 offscreen surface resolved into the 2048x2048 terrain atlas, and
    // skipping them is why that atlas is empty on all 64 tiles. So exempt ONLY
    // small offscreen targets, which cannot be a scene band, and count them
    // separately. A fabricated-white TILE is wrong too -- the real fix is
    // upstream, in whatever makes the texture bind fail.
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
      // PROBE: WHICH draws are still being skipped on the two scene bands. After
      // the colorWrite clause the menu goes to zero on 1280x640 / 1280x80 but
      // freeroam keeps 65-79 per interval, and the counters cannot say what they
      // are: the guest-side probe reports the whole null-PS colour population as
      // already exempt, and a colorMaskKnown flag changed nothing because "mask
      // unreadable 0" had already ruled that out. One line per distinct (target,
      // shader).
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
      // THE FIRST STENCIL DRAWS OF A FRAME, IN ORDER, with the state they carry.
      // menu.rdc shows a fullscreen quad stamping the mask and the very next
      // fullscreen quad being rejected by it -- and a fullscreen fill that is
      // entirely rejected is doing nothing, which the guest would not have
      // issued. So the question is whether the WRITER and the READER agree, and
      // nothing else in this tree can show that: RenderDoc's pipeline JSON has
      // no depthStencilState field, and the census reports configurations
      // without their order.
      //
      // Draw ORDINAL is logged so the lines can be matched against a capture's
      // draw order, and the SURFACE, because the stencil plane is per depth
      // OBJECT and menu.rdc shows the incrementing geometry and the testing fill
      // running against different EDRAM bases. First frame only.
      static uint32_t s_seq = 0;
      if (s_seq < 120) {
        ++s_seq;
        char m[256];
        std::snprintf(m, sizeof(m),
                      "  STENCIL SEQ %u: depth 0x%08X %ux%u base 0x%03X | "
                      "idx%u indices %u func %u/%u ref %u ops %u/%u/%u "
                      "masks %02X/%02X",
                      s_seq, d.depthObject, d.depthWidth, d.depthHeight,
                      d.depthBase, d.stencilIndex, d.indexCount,
                      uint32_t(d.stencil.frontFunc), uint32_t(d.stencil.backFunc),
                      uint32_t(d.stencil.ref), uint32_t(d.stencil.frontFail),
                      uint32_t(d.stencil.frontZFail),
                      uint32_t(d.stencil.frontPass),
                      uint32_t(d.stencil.readMask),
                      uint32_t(d.stencil.writeMask));
        LogInfo(m);
      }
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
      // shader is running, and the per-draw transform when the passthrough stage
      // is — that one does not read b0 at all, but the root signature requires a
      // bound CBV either way.
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
      // samples: root parameters a shader does not reference need no binding.
      // Both must succeed or neither is bound -- a shader that declares textures
      // with only its samplers bound reads undefined descriptors.
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
        // interpolator stream, because neither exists for this draw — the vertex
        // shader produces the position and the rasterizer interpolates what it
        // exports.
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
      // suppressed".
      ++m_standInStrictSkipped;
      continue;
    }
    mx::gpu::guard::Note(mx::gpu::guard::Guard::kStandInDraw, true);
    ++m_standInDraws;

    // WHICH stand-in draws actually PAINT, named by target and shader -- the
    // complement of WHITE-SKIP WHO above, and only this half can put wrong
    // colour on screen. Most stand-in draws are the guest's null-PS depth
    // passes, which carry RB_COLOR_MASK 0 and paint nothing, so filtering on
    // colorWrite separates the noise from the suspects. This is what named the
    // red gameplay screen, and the same probe going quiet confirmed the fix.
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
    // when this draw has stencil so it cannot inherit the previous draw's ref; a
    // stale ref only shows on the second draw of a pair and reads as a geometry
    // bug.
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
      // everything. Only meaningful for a textured draw. The address bits arrive
      // on the draw; the filter is read here off the texture itself, so
      // graphics_system stays a pass-through and the two paths agree on what a
      // variant index means.
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

// Suballocate one per-draw range from the upload ring. Three ways this can be
// satisfied, cheapest first: bump the page already being filled, reset a page
// the GPU has finished with, or grow the ring. Only the third calls into the
// driver, and in steady state it never happens.
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
  // destruction cost as much as the creation: 743ms of an 1815ms menu tick,
  // against 1031ms to create them. Neither exists now -- a range of a page is
  // not a resource, and a page is reset rather than freed. The fence protection
  // moved rather than went away: a page carries the submission it was last read
  // under, and `live` is the separate condition this clears.
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
  // UPLOAD heap for each of the buffers below -- up to nine per call, once per
  // submitted draw. They are now ranges of the AllocUpload ring, and the only
  // committed resources left in this path are the ring's own pages.
  //
  // What it cost, measured before the change: a steady-state main menu tick of
  // 1815ms spent 1031ms creating those resources and 743ms destroying them --
  // 97.7% of the tick -- against 17ms to record the frame and 0ms waiting for
  // the GPU. This comment used to claim D3D12's command-list tracking keeps the
  // underlying memory alive until the GPU is done with it; that is FALSE, it was
  // a D3D11 guarantee, and lifetime is now the ring's job.
  //
  // A fetch draw brings no host vertex buffer at all: its geometry arrives in
  // vertexStage->rawBytes and the shader reads it through the root SRV, so a
  // null `vertices` is correct rather than a malformed draw. Tested before the
  // gate, which would otherwise drop every one of them.
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
    // preserve that shader's final modulation:
    //
    //   export = decoded_yuva * c0
    //
    // kGameYuvPS performs that multiply through its COLOR input. Bink uses an
    // UP/FVF quad with no guest COLOR element, so the transcode supplies white
    // there; leaving it white would make the video ignore c0 entirely.
    //
    // Host layout: position float4 @0, color float4 @16, uv float2 @32.
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
  // BEFORE the constant buffers are filled, not after. FillVertexNdcOffset reads
  // d.halfPixel and the viewport extents, and these used to be assigned ~680
  // lines further down -- so the fill saw halfPixel 0.0 and wrote a zero offset
  // on every draw while the shader dutifully applied it. The census read
  // "applied 174478, skipped 0" because it was incremented beside the
  // assignment, measuring a later moment than the one that used the value.
  d.guestVpWidth = guestVpWidth;
  d.guestVpHeight = guestVpHeight;
  d.useGuestVp = useGuestVp;
  d.halfPixel = (vtxCntl != 0xFFFFFFFFu && (vtxCntl & 1u) == 0) ? 0.5f : 0.0f;
  if (d.halfPixel != 0.0f) ++m_halfPixelDraws; else ++m_halfPixelSkipped;
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
  // DIAG: every draw aimed at a SQUARE POWER-OF-TWO offscreen target, grouped by
  // the frame it landed in.
  //
  // The terrain's ground meshes take their whole world Y from three texture
  // samples -- their vertex buffer carries only grid X and Z -- and the dominant
  // term is a 512x512 that reads min = max = 0. The guest's own
  // `ps_hft_deform_copy` copies the heightfield into it, but only for tiles a
  // track segment has reached, so on an untouched map the buffer has to be
  // filled at level load.
  //
  // ONE LINE PER FRAME THAT HAS ANY, not per draw: a level load is thousands of
  // frames and the interesting ones are the handful that draw here at all.
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
  // running the guest vertex shader has no interpolator stream and must not have
  // one. Demanding it anyway turned every qualifying draw into a stand-in draw
  // holding vertices the interpreter no longer transformed -- a flat red frame,
  // and zero translated draws in a run where 36,064 qualified.
  //
  // Two valid shapes, and a fetch stage has NONE of the input-element fields by
  // design -- its only input is SV_VertexID -- so requiring them unconditionally
  // took a frame from 339 draws to 28.
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
  // without its interpolators reads undefined registers, and one run without its
  // constants computes from zeros -- both produce a confident wrong picture
  // rather than a visible failure. "Its interpolators" means the CPU-built
  // stream only when the CPU built the vertices; with the guest vertex shader
  // running, the rasterizer produces them.
  //
  // WHICH of the six conditions sent this draw to the stand-in. Without it the
  // only numbers available are measured at two different points -- ~475k D3D9
  // draw attempts against ~52k submitted draws -- so they cannot be subtracted.
  //
  // no-HANDLE is tested FIRST, and the order is the whole point: a draw with no
  // pixel shader has no handle AND no HLSL, so the other way round every one was
  // counted as no-hlsl and `no-handle 0` could never be anything but zero. That
  // cost a session, sending the search into a translator that was working fine.
  //
  // A guest DEPTH pass -- a translated VERTEX stage and no pixel shader at all
  // -- needs none of the four things demanded of a normal translated draw,
  // because kTranslatedDepthOnlyPS reads nothing and its output is discarded by
  // a zero write mask. What it DOES need is the vertex stage, without which it
  // runs on the software interpreter. `!colorWrite` is the load-bearing term and
  // is checked here as well as on the hooks side.
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
    // xe_texsign[slots], xe_param_gen and xe_alphatest, so the buffer must cover
    // all five -- sizing it to the constant bank alone leaves the shader reading
    // past the end of the resource for every unnormalized fetch. Rounded up to
    // 256 bytes, the constant-buffer granularity. Zero for a depth pass, which
    // brings no constant bank; the three payloads after it still have to exist
    // because the cbuffer is declared with them.
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
        // xe_texinv, immediately after the bank. An unnormalized fetch addresses
        // the texture in TEXELS, so the shader multiplies by this to normalize:
        // 1/extent of the texture actually bound at that slot. Stored
        // reciprocated here rather than divided in the shader, where a zero
        // extent would produce infinity -- left zero, an unnormalized fetch
        // reads texel 0 rather than something plausible.
        //
        // EVERY slot, not just the first. Filled at index 0 only, an
        // unnormalized fetch on any higher slot multiplied its coordinate by
        // ZERO, sampling texel 0 and painting it flat across the primitive.
        // Slot 0 falls back to d.texture because the single-texture path
        // populates that and not the array.
        //
        // A slot bound to a RESOLVE SNAPSHOT has no CPU payload at all, so
        // filling only from d.pixelTextures left those slots at zero -- the same
        // defect surviving in the one case that never carries a payload, and
        // what the menu rider looks like: its material samples the scene
        // composite at s13, that slot is a snapshot, and the fetch reads texel
        // (0,0) flat over 21753 indices.
        //
        // .z is the LAYER COUNT, not a reciprocal, and the one component that
        // scales up: a 3D/stacked fetch with normalized coordinates delivers W
        // as a fraction of the stack while the Texture2DArray wants a slice
        // index. Left zero for a snapshot, pinning such a fetch to slice 0.
        //
        // SNAPSHOT FIRST, because that is the order the DESCRIPTOR uses --
        // BindTranslatedTextures tests stageSampledObjects[i] before anything
        // else -- and testing the payload first let a snapshot slot be
        // NORMALIZED by a different texture's extent.
        //
        // The shadowing texture is usually `d.texture`, NOT d.pixelTextures[s],
        // and that distinction cost a revert: inside ResolvePixelSlotTexture the
        // object and payload outputs are mutually exclusive, so a census keyed
        // on d.pixelTextures[s] reads a structural ZERO.
        for (uint32_t s = 0; s < kTranslatedSamplerSlots; ++s) {
          uint32_t w = 0, h = 0, layers = 0;
          // .w of xe_texinv is the guest's per-texture LOD BIAS, in LOD units.
          // See HleTexturePayload::lod_bias for why it matters.
          //
          // SET ONLY ON THE NON-SNAPSHOT PATH, and that is not a detail:
          // resolving the bias from d.pixelTextures[s] for EVERY slot includes
          // snapshot slots, where a shadowing payload routinely exists and
          // describes a DIFFERENT texture. That is texinv-shadowed-by-payload,
          // the defect fixed at the top of this same file, reintroduced one
          // field over. A snapshot has no fetch constant behind it, so 0.0 is
          // correct.
          float lodBias = 0.0f;
          const uint32_t object =
              s < d.pixelSampledObjects.size() ? d.pixelSampledObjects[s] : 0;
          if (object) {
            const auto snap = m_gameSnapshots.find(object);
            if (snap != m_gameSnapshots.end()) {
              w = snap->second.width;
              h = snap->second.height;
              // What the OLD ordering would have used instead. Counted only when
              // the extents actually DIFFER -- a shadowing texture that happens
              // to match normalized correctly by accident. Includes the
              // d.texture fallback, which is the whole point: without it this
              // reads zero while the defect is live.
              const auto& shadowed =
                  (s < d.pixelTextures.size() && d.pixelTextures[s])
                      ? d.pixelTextures[s]
                      : (s == 0 ? d.texture : nullptr);
              if (shadowed && shadowed->width && shadowed->height &&
                  (shadowed->width != w || shadowed->height != h))
                ++m_texinvSlotMismatch;
            }
          } else {
            const auto& tex = s < d.pixelTextures.size() && d.pixelTextures[s]
                                  ? d.pixelTextures[s]
                                  : (s == 0 ? d.texture : nullptr);
            if (tex && tex->width && tex->height) {
              w = tex->width;
              h = tex->height;
              layers = tex->array_size;
              lodBias = tex->lod_bias;
            }
          }
          // WHICH BRANCH EACH SLOT TOOK, per slot, for the life of the run.
          //
          // The floating-bike defect is here. A capture A/B proved the terrain
          // height-tile shader computes sample(tex3) * 4 - 2, so a slot that
          // samples BLACK subtracts exactly 2.008 from every height in the tile,
          // and the two captures differ in exactly one thing: xe_texinv[3].z, 1
          // in the good run and 0 in the bad one. Only the payload branch sets
          // .z, so .z == 0 means the slot took the SNAPSHOT branch -- same
          // 2048x2048 texture, resolved two mutually exclusive ways.
          //
          // So this counts, per slot: snapshot-with-a-resource, snapshot whose
          // object has NO entry in the map (the case that samples black), and
          // payload. A slot that flips between columns run to run is the defect;
          // a total cannot show a flip in slot 3 against 15 stable slots.
          {
            const uint32_t kind = !object ? 2u : (w && h ? 0u : 1u);
            const uint32_t si = s < kTranslatedSamplerSlots ? s : 0;
            ++m_texSlotPath[si][kind];
            // SPLIT OUT THE HEIGHT TILE. Counting per slot across ALL draws
            // produced "slot 3: snap 3926, NO-MAP 37, payload 194649", which
            // cannot say whether those 37 were the draws that matter. The
            // terrain height tile is a 129x129 target and nothing else in the
            // frame is: 37 NO-MAP out of 198k draws is noise, 37 out of ~40
            // height tiles is the entire defect.
            if (d.targetWidth == 129 && d.targetHeight == 129) {
              ++m_texSlotPathTile[si][kind];
              // Name the object on the slot that matters. Recorded for BOTH
              // object outcomes -- with a snapshot entry and without -- because
              // the dominant case turned out to be "has a resource and it is
              // black", not "no entry".
              if (si == 3 && object) {
                uint32_t k = 0;
                for (; k < m_tileSlot3Count; ++k)
                  if (m_tileSlot3[k].object == object) break;
                if (k == m_tileSlot3Count && m_tileSlot3Count < 8) {
                  m_tileSlot3[m_tileSlot3Count].object = object;
                  m_tileSlot3[m_tileSlot3Count].width = w;
                  m_tileSlot3[m_tileSlot3Count].height = h;
                  ++m_tileSlot3Count;
                }
                if (k < m_tileSlot3Count) {
                  if (w && h) {
                    ++m_tileSlot3[k].withEntry;
                    m_tileSlot3[k].width = w;
                    m_tileSlot3[k].height = h;
                  } else {
                    ++m_tileSlot3[k].withoutEntry;
                  }
                } else {
                  ++m_tileSlot3Overflow;
                }
              }
            }
          }
          if (!w || !h) continue;
          const float ts[4] = {1.0f / float(w), 1.0f / float(h),
                               float(layers), lodBias};
          std::memcpy(static_cast<uint8_t*>(p) + bankBytes + s * 16, ts,
                      sizeof(ts));
        }
        // xe_texsign, immediately after xe_texinv: the per-component scale for
        // TEXTURE SIGNS, 2.0 where the guest fetch is kUnsignedBiased and the
        // shader must expand [0,1] to [-1,1]. The shader pairs it with an offset
        // of 1-scale, so 1.0 is the identity.
        //
        // Written for EVERY slot and component, unconditionally: the buffer was
        // memset to zero above, and a zero scale here does not mean "unsigned",
        // it means the fetch becomes v*0 + 1 -- solid white.
        for (uint32_t s = 0; s < kTranslatedSamplerSlots; ++s) {
          const uint8_t signs = d.pixelSamplerSigns[s];
          const float sc[4] = {
              TextureSignScale(signs, 0), TextureSignScale(signs, 1),
              TextureSignScale(signs, 2), TextureSignScale(signs, 3)};
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
        // rather than in the shader so the shader carries no knowledge of the
        // register layout (hle_types.h, DrawCall::colour_control). The reference
        // is passed as raw bits through a uint4 rather than converted: it is a
        // float, and rounding it through an integer member would quietly move
        // every threshold.
        uint32_t refBits = 0;
        std::memcpy(&refBits, &d.alphaRef, 4);
        const uint32_t at[4] = {d.alphaControl & 7u,
                                (d.alphaControl >> 3) & 1u, refBits, 0};
        std::memcpy(static_cast<uint8_t*>(p) + bankBytes + texInvBytes +
                        texSignBytes + paramGenBytes,
                    at, sizeof(at));
        // xe_colorscale follows xe_alphatest. Guest colour formats 4 (k_16_16)
        // and 5 (k_16_16_16_16) are signed fixed point -32...32.
        //
        // ONLY these two. Format 7 (k_16_16_16_16_FLOAT) is a genuine half float
        // and shares R16G16B16A16_FLOAT with format 5, so scaling it too would
        // divide a correct HDR buffer by 32. The guest nibble is the only thing
        // that separates them, which is why it is carried per draw.
        //
        // This USED to write 1/32, which was half of Xenia's hack -- and the
        // half that does not apply to us. Xenia biases the write down by 5
        // exponents because ITS host render target is SNORM and cannot hold
        // -32...32, and pairs that with the exact inverse at resolve. We map
        // both formats to a HALF FLOAT target, which holds the whole range, so
        // the divide had no counterpart and every consumer of a fixed-point
        // target read values 32x too small: the deferred light accumulation
        // buffer is guest format 5, and the whole deferred chain ran 32x down.
        const bool fixed16 =
            d.targetColorFormat == 4u || d.targetColorFormat == 5u;
        // .y and .z carry the range the GUEST format can represent, and are 0
        // for formats that need no clamp -- the shader branches on .y > 0.
        //
        // Guest formats 3 and 12 are 7e3: "[0, 32) RGB, unorm alpha"
        // (xenos.h:301). Both map to R16G16B16A16_FLOAT here, which is SIGNED,
        // so without this a shader's negative output is stored where the console
        // ROP would have clamped it to 0. 31.875 is the largest representable
        // 7e3 value and the same bound Xenia clamps to.
        //
        // Every other format is already handled: 0/1 and 2/10 map to UNORM host
        // formats that clamp on write, 7 is a genuine signed half float that
        // must NOT be clamped, and 4/5 are the fixed-point pair whose half-float
        // target holds their range directly.
        const bool float7e3 =
            d.targetColorFormat == 3u || d.targetColorFormat == 12u;
        const float cs[4] = {1.0f,
                             float7e3 ? 31.875f : 0.0f,
                             float7e3 ? 1.0f : 0.0f, 0.0f};
        // NOT A GUARD, and removed from the census, where it read 24.7% and was
        // the top entry. docs/strict_mode.md classifies by what a thing DOES:
        // class A refuses to act on bad input, class B manufactures a value we
        // do not have. This is neither -- it applies the GUEST FORMAT'S OWN
        // RANGE, since a 7e3 target physically cannot store a negative or a
        // value at or above 32, exactly as formats 0/1/2/10 get for free from
        // their UNORM host formats. So 24.7% is the share of draws that render
        // to a 7e3 target, not a guard rate. m_float7e3Clamped still counts it,
        // on the format line.
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
  // pixel shader also translated -- the hooks side enforces that, and it is
  // re-checked here through `d.translated` because the two conditions are
  // decided in different processes' worth of code.
  //
  // Everything the CPU path derives on the side is REPLACED rather than lost:
  // the position buffer is what this stage produces, the interpolator copy is
  // what the rasterizer does natively, and the param_gen UV becomes SV_Position
  // in a pixel shader that reads it.
  if (d.translated && hasVertexStage) {
    // The emitted cbuffer is xe_c[256], xe_texinv[slots], then (fetch variant
    // only) uint4 xe_vf[kMaxVertexFetches], float4 xe_texsign[slots] and float4
    // xe_ndc_offset. The buffer has to cover ALL of it: the shader declares
    // every member unconditionally, so anything left out of the size is a read
    // past the end of the resource. Sized for the fetch variant unconditionally
    // -- the tail is zeroed either way.
    //
    // xe_texsign and xe_ndc_offset are LAST on purpose: the renderer writes
    // xe_vf at a fixed offset computed from the two members before it.
    const uint32_t vsConstBytes =
        ((vertexStage->constDwords * 4 + kTranslatedSamplerSlots * 16 +
          mx::hle::HlslShader::kMaxVertexFetches * 16 +
          kTranslatedSamplerSlots * 16 + 16) + 255u) & ~255u;
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
        FillVertexTexinv(d, p, vsConstBytes, vertexStage->constDwords);
        FillVertexTextureSigns(d, p, vsConstBytes, vertexStage->constDwords);
        FillVertexNdcOffset(d, p, vsConstBytes, vertexStage->constDwords);
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
      FillVertexTexinv(d, d.vscb.cpu, vsConstBytes, vertexStage->constDwords);
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
  // Bit 0 has PA_SU_VTX_CNTL::PIX_CENTER's meaning -- 0 is Direct3D 9 centres at
  // .0 and needs the offset, 1 is the host's own .5 and does not -- but the
  // value is assumed rather than read: the register is not locatable and this is
  // a D3D9 title, so 0 is what it would hold. Keeping the decode identical means
  // the register drops straight in when it is found.
  d.edramCopy = edramCopy;
  // Interned HERE rather than at draw time: StencilIndexFor mutates the intern
  // table, and the draw loop runs on the render thread while this runs on the
  // submitting one. Doing it at submission also fixes the index by the time the
  // draw is queued, so a state that arrives later cannot renumber a queued draw.
  if (stencil && stencil->enable) {
    d.stencil = *stencil;
    d.stencilIndex = StencilIndexFor(*stencil);
    ++m_stencilDraws;
    // kAlways is 7 in the GUEST encoding, which is what GameStencil carries.
    if (stencil->frontFunc != 7u || stencil->backFunc != 7u)
      ++m_stencilTestingDraws;
  }
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

  // Blit path: the finished scene lives in a guest-sized offscreen target, so it
  // has to be scaled to the backbuffer rather than copied. m_viewport already
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
  // and backbuf in PRESENT, making both of those barriers invalid.
  barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
  barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

  barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
  barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;

  m_commandList->ResourceBarrier(2, barriers);
}
