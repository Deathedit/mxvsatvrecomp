#include "app/graphics_system.h"

#include <SDL3/SDL_init.h>
#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_gamepad.h>
#include <rex/cvar.h>
#include <rex/logging.h>

#include <chrono>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include <windows.h>

#include "gpu/hle_types.h"
#include "gpu/d3d9_layout.h"
#include "hooks/native_bridge.h"

// The one vertex stride the game PSO's input layout actually describes —
// POSITION float3 at offset 0, COLOR float4 at offset 12, TEXCOORD float2 at
// offset 28, so 36 bytes. See the gate in RenderThreadFunc.
static constexpr uint32_t kSupportedStride = mx::hle::kHostVertexStride;

// hide_colorless_draws and hide_colored_draws were two halves of one
// diagnostic, splitting the frame by colour source so a screenshot could show
// the scene without the colourless overpaint, or the overpaint on its own. Both
// are gone (2026-08-07): every draw is submitted regardless of colour source.
//
// What retired them: they hid a defect rather than fixing it. Draws with no
// colour attribute are written opaque white and paint *over* textured draws
// instead of substituting for missing ones, so `hide_colorless_draws=true` was
// passed on every run and the shipped default was the untested configuration.
// Keeping the filter would have made that permanent.
//
// The overpaint is NOT a transform bug, though this comment claimed it was
// until the claim was measured (46404e1). Colourless draws are 4-vertex
// fullscreen quads with a raw NDC extent of 2.01 and none outside the cube —
// geometrically exact, not "small geometry smeared across the viewport". 91.5%
// of them sample a render target: they are compositor passes whose colour was
// always going to come from that target, and they paint white because the
// target is empty. The fix is the resolve snapshot, not anything here.

// The D3D9 -> D3D12 path describes every draw from the API calls that produced
// it, rather than reconstructing it from the PM4 ring.
//
// hle_render and pm4_translate were the cvars that selected between the two
// sources. Both are gone with the translator (2026-08-06): the D3D9 description
// is the only source of draws, so there is nothing to select and no fallback.
//
// What retired them: with pm4_translate=false the HLE path held 87.66-87.67%
// applied and 309.3 verts/draw against PM4-on's 87.71%/308.9, and the one
// dependency that gate missed — the pixel shader CF offset — was replaced by
// the field the guest's own emitter reads (sub_82565928), which decodes more
// shaders than the ring ever did (40/49 against 37/61, 0 ambiguous).

// Capture only: no PSOs, no uploads, no shader translation, nothing submitted.
// It gates the per-draw scoring, the coverage report and the dump — including
// every `hle-render` and `stageF` line, which is why those are absent from a
// run that passes hle_shader_exec without this.
REXCVAR_DEFINE_BOOL(hle_capture, false, "Debug",
                    "Score every D3D9 draw against the state shadow and report "
                    "what fraction is fully described, plus the first few "
                    "resolved draws to logs/decldump/decls.txt. Capture only — it "
                    "submits nothing and renders nothing");

// The per-draw and per-vertex diagnostics this investigation accumulated.
//
// Default OFF. They are not free — the Stage-3 transform probe alone reads 256
// guest dwords and scores every vertex of every draw to log a ranking nothing
// acts on — and a run is now expected to be a measurement of the emulator, not
// of its instrumentation. Per-FRAME reporting is unaffected and stays on.
REXCVAR_DEFINE_BOOL(hle_diag, false, "Debug",
                    "Per-draw and per-vertex HLE diagnostics: the transform "
                    "probe, the prim-type and vfetch censuses, and the vertex "
                    "fetch addressing self-check. Off by default; they cost "
                    "real frame time");

namespace rex {
namespace system {

bool D3D12GraphicsSystem::InitializeRenderer(HWND hwnd) {
  m_hwnd = hwnd;
  m_renderer = std::make_unique<D3D12Renderer>();
  if (!m_renderer->Initialize(hwnd)) {
    m_renderer.reset();
    return false;
  }
  mx::native::SetRenderer(m_renderer.get());
  m_running = true;
  m_renderThread = std::thread([this]() { RenderThreadFunc(); });
  return true;
}

void D3D12GraphicsSystem::Shutdown() {
  m_running = false;
  if (m_renderThread.joinable()) m_renderThread.join();
  // Drop the bridge's non-owning alias before the renderer dies.
  mx::native::NativeGraphics::Get().Shutdown();
  m_renderer.reset();
  m_initialized = false;
}

void D3D12GraphicsSystem::RenderThreadFunc() {
  using namespace mx;
  // The host FFmpeg intro player was REMOVED 2026-08-06. The guest opens and
  // decodes its own Bink videos natively — proven by hooking BinkOpen, and
  // visible as the intro playing twice while both existed. Nothing here needs
  // to own the swapchain before the guest starts.
  while (m_running) {
    bool idle = false;
    if (m_renderer) {
      // Hand any PM4 geometry the VdSwap hook translated this frame to the game
      // pipeline. GetDrawCalls moves-and-clears, so a tick with no VdSwap since
      // the last one gets an empty list — see the note below on why that must
      // re-present the previous frame rather than clear.
      //
      // DrawCall::mvp is the window-space -> NDC transform Pm4Translator built
      // from the guest's PA_CL_VPORT_* registers. It used to be the ALU
      // constant block, which this game never writes — so it was identically
      // zero and collapsed every vertex to the origin.
      auto draws = native::NativeGraphics::Get().GetDrawCalls();

      // This loop no longer ticks on a fixed sleep: an idle tick parks on the
      // mailbox cv and wakes exactly when the guest posts a frame (bottom of
      // the loop).
      //
      // Expect the two counters below to run at ROUGHLY 1:1, not with the empty
      // side at zero — measured 7517 with draws / 6606 empty over mx_1270. That
      // ratio is structural rather than waste: a tick that rendered loops
      // straight back without waiting, so it always finds an empty list on the
      // next pass, re-presents, and only THEN parks. One empty tick per
      // productive one is the design. A count that climbs far past the
      // productive side means the guest is posting lists nobody consumed; the
      // millions-of-empties reading the fixed-sleep version produced is not
      // reachable here, because there is no longer a clock to spin against.
      //
      // A tick that finds nothing
      // must still re-present the last frame we did get, not a cleared screen:
      // GetDrawCalls moves-and-clears, so an unconditional ClearGameDraws here
      // threw away the only geometry we had every time the two rates disagreed.
      // Combined with the placeholder-triangle fallback that used to sit in
      // RenderGameFrame, that made the post-load screen alternate between guest
      // geometry and the placeholder — invisible while the render target
      // accumulated, a visible flash once the clear was fixed. The placeholder
      // is gone; the re-present rule below is not, and still carries the frame.
      static uint64_t s_ticksWithDraws = 0, s_ticksEmpty = 0;
      if (draws.empty()) ++s_ticksEmpty; else ++s_ticksWithDraws;

      uint32_t submitted = 0, skipped = 0;
      // kSupportedStride (36) is the input layout the game PSO declares —
      // POSITION float3 @0, COLOR float4 @12, TEXCOORD float2 @28 — and
      // anything else would be reinterpreted as that layout and drawn as noise,
      // so it is still counted rather than drawn.
      //
      // What changed is what reaches here: the translator now transcodes guest
      // vertices into this layout using the shader's own declared formats, so a
      // draw arriving at stride 28 is one that was *converted* to it, not one
      // that happened to be it. A draw still arriving at some other stride is
      // one the transcode could not handle — no shader bound, no identifiable
      // position attribute, or a vertex format not implemented — and the
      // histogram below is now a list of those gaps rather than of the guest's
      // strides.
      static std::map<uint32_t, uint32_t> s_skippedStrides;
      static uint64_t s_skippedUntransformable = 0;
      // Filter first, bind second. The renderer's list is only replaced once we
      // know the new frame has something in it — a frame whose draws were all
      // skipped for stride would otherwise blank the screen just as surely as a
      // frame that never arrived.
      std::vector<const mx::hle::DrawCall*> submittable;
      for (const auto& d : draws) {
        // A resolve, a clear and a surface bind are not draws. None has
        // geometry, so every gate below would reject them — the first one
        // silently, without even counting it — and a dropped resolve leaves a
        // stale snapshot, which looks like a partial fix rather than a failure.
        // They keep their slots in `submittable` because each is only
        // meaningful in sequence: the snapshot must be taken at this point in
        // the draw order rather than at the end of the frame, and a bind that
        // arrives after a resolve must not create the surface the resolve
        // wanted.
        if (d.resolve_dest_texture || d.clear_color_target || d.surface_bind) {
          submittable.push_back(&d);
          continue;
        }
        // A draw whose vertex shader fetches for itself carries NO host
        // vertices and no stride — the 36-byte transcode that used to produce
        // them is the CPU pass that path exists to remove. Its geometry is in
        // raw_vertex_bytes, so the two gates below have to let it through or
        // the saving turns into a blank frame.
        const bool fetch_draw =
            d.vertex_shader_hlsl && !d.raw_vertex_bytes.empty() &&
            d.raw_fetch_count && !d.vertex_constants.empty();
        // vertices are only populated when the translator resolved a vertex
        // fetch constant; index-only draws have nothing to bind.
        if (!d.valid || (!fetch_draw && d.vertices.empty()) ||
            d.index_count == 0)
          continue;
        if (d.topology == mx::hle::HostTopology::kUndefined) {
          ++skipped;
          continue;
        }
        // Set only when skip_untransformable_draws is on. A MITIGATION: the
        // draw is still transformed wrongly, this only stops it being painted
        // over the draws that come out right. Counted separately from the
        // other skip reasons so a screenshot can be read honestly against it.
        if (d.untransformable) {
          ++skipped;
          ++s_skippedUntransformable;
          continue;
        }
        if (!fetch_draw && d.vertex_stride != kSupportedStride) {
          ++skipped;
          ++s_skippedStrides[d.vertex_stride];
          continue;
        }
        // RETIRED 2026-08-12: hle_main_viewport_only, which dropped any draw
        // whose viewport was not 1280x720. It was a stand-in for modelling
        // render targets from D3D9 state, from when a frame's ~16 guest colour
        // surfaces all landed on one host target and whichever pass ran last
        // decided what was on screen. Render targets ARE modelled now
        // (EnsureGameRenderTarget routes each surface to its own), so the
        // filter's own help text already called it superseded — and it was
        // off by default, so no run has ever used it.
        submittable.push_back(&d);
        ++submitted;
      }

      // A non-empty handoff is a real guest frame even when every draw is
      // filtered, so the previous frame is retired here rather than inside the
      // submittable check below. Otherwise a frame whose draws were all skipped
      // — for stride, or for the viewport gate — leaves the host replaying the
      // previous frame's draw list while the guest swaps, which reads as a
      // frozen picture rather than as an empty one.
      if (!draws.empty()) {
        m_renderer->ClearGameDraws();
      }
      // The AddGameDraw loop, timed. It allocates every per-draw upload buffer
      // for the frame — up to nine committed resources per draw — and it is the
      // one phase of the render tick that runs out here rather than inside the
      // renderer, so the renderer cannot time it for itself.
      const auto addDrawsStart = std::chrono::steady_clock::now();
      uint32_t addDrawCalls = 0;
      if (!submittable.empty()) {
        for (const auto* d : submittable) {
          if (d->resolve_dest_texture) {
            m_renderer->AddGameResolve(
                d->resolve_dest_texture, d->resolve_source_object,
                d->resolve_dest_x, d->resolve_dest_y, d->resolve_src_x1,
                d->resolve_src_y1, d->resolve_src_x2, d->resolve_src_y2,
                d->resolve_dest_width, d->resolve_dest_height,
                d->resolve_source_is_depth, d->resolve_source_base,
                d->resolve_source_width, d->resolve_source_height);
            continue;
          }
          if (d->surface_bind) {
            m_renderer->AddGameSurface(
                d->surface_bind_object, d->surface_bind_width,
                d->surface_bind_height, d->surface_bind_base,
                d->surface_bind_color_format, d->surface_bind_is_depth);
            continue;
          }
          if (d->clear_color_target) {
            m_renderer->AddGameClear(
                d->render_target_object, d->render_target_width,
                d->render_target_height, d->surface_base,
                (d->render_target_color_info >> 16) & 0xFu,
                d->clear_color,
                d->clear_color_is_float ? d->clear_color_float.data()
                                        : nullptr);
            continue;
          }
          // The guest vertex stage, when the hooks built one for this draw.
          // Pass-through only: every decision about whether a draw qualifies
          // was made where the microcode and the attributes are.
          D3D12Renderer::GpuVertexStage vertexStage;
          if (d->vertex_shader_hlsl && !d->raw_vertex_bytes.empty() &&
              d->raw_fetch_count && !d->vertex_constants.empty()) {
            // The fetch form. No inputs and no regs by construction — the
            // shader reads the raw buffer itself.
            vertexStage.handle = d->vertex_shader_handle;
            vertexStage.hlsl = d->vertex_shader_hlsl;
            vertexStage.dxbc = d->vertex_shader_dxbc;
            vertexStage.constants = d->vertex_constants.data();
            vertexStage.constDwords =
                static_cast<uint32_t>(d->vertex_constants.size());
            vertexStage.rawBytes = d->raw_vertex_bytes.data();
            vertexStage.rawByteCount =
                static_cast<uint32_t>(d->raw_vertex_bytes.size());
            vertexStage.rawFetch =
                reinterpret_cast<const uint32_t*>(d->raw_fetch.data());
            vertexStage.rawFetchCount = d->raw_fetch_count;
          } else if (d->vertex_shader_hlsl && !d->vertex_inputs.empty() &&
                     d->vertex_input_count && !d->vertex_constants.empty()) {
            vertexStage.handle = d->vertex_shader_handle;
            vertexStage.hlsl = d->vertex_shader_hlsl;
            vertexStage.dxbc = d->vertex_shader_dxbc;
            vertexStage.inputs = d->vertex_inputs.data();
            vertexStage.inputBytes =
                static_cast<uint32_t>(d->vertex_inputs.size());
            vertexStage.regs = d->vertex_input_regs.data();
            vertexStage.regCount = d->vertex_input_count;
            vertexStage.constants = d->vertex_constants.data();
            vertexStage.constDwords =
                static_cast<uint32_t>(d->vertex_constants.size());
          }
          // The vertex stage's textures, if its shader samples any. Set for
          // BOTH forms above -- a displacement fetch is orthogonal to whether
          // the stage fetches its own attributes.
          if (vertexStage.handle && d->vertex_sampler_count) {
            vertexStage.samplerCount = d->vertex_sampler_count;
            vertexStage.samplerArrayMask = d->vertex_sampler_array_mask;
            vertexStage.textures = d->vertex_textures.data();
            vertexStage.sampledObjects = d->vertex_sampled_objects.data();
            vertexStage.samplerSigns = d->vertex_sampler_signs.data();
            vertexStage.sampledSwizzles =
                d->vertex_sampled_swizzles.data();
          }
          ++addDrawCalls;
          const int32_t scissor[4] = {d->scissor_left, d->scissor_top,
                                      d->scissor_right, d->scissor_bottom};
          m_renderer->AddGameDraw(d->vertices.data(),
                                  static_cast<uint32_t>(d->vertices.size()),
                                  d->vertex_stride, d->indices.data(),
                                  static_cast<uint32_t>(d->indices.size()),
                                  d->index_16bit, d->index_count, d->mvp,
                                  static_cast<uint32_t>(d->topology),
                                  (d->om_seen & (1u << 1)) != 0 &&
                                      (d->depth_control & (1u << 1)) != 0,
                                  (d->om_seen & (1u << 1)) != 0 &&
                                      (d->depth_control & (1u << 2)) != 0,
                                  (d->om_seen & (1u << 0)) == 0 ||
                                      (d->colour_mask & 0xFu) != 0,
                                  d->texture, d->render_target_object,
                                  d->render_target_width,
                                  d->render_target_height,
                                  d->sampled_render_target_object,
                                  d->sampled_texture_object,
                                  d->planes.data(), d->plane_count,
                                  d->yuv_has_alpha,
                                  // Only blend when the guest said so — a draw
                                  // that never set ALPHABLENDENABLE keeps the
                                  // opaque path it has always had.
                                  (d->om_seen & (1u << 2)) != 0 &&
                                      d->blend_enable != 0,
                                  d->src_blend, d->dest_blend, d->blend_op,
                                  // No COLOR element in the declaration, so the
                                  // {1,1,1,1} in the vertex buffer is a seed,
                                  // not data. Harmless when a texture
                                  // modulates it; opaque white when nothing
                                  // does.
                                  static_cast<uint8_t>(d->color_source),
                                  // Xenos address modes: 0 repeat and 1
                                  // mirrored repeat wrap, everything above
                                  // clamps. Mirroring is not modelled, so a
                                  // mirrored mode takes the nearer of the two.
                                  (d->clamp_x >= 2 ? 1u : 0u) |
                                      (d->clamp_y >= 2 ? 2u : 0u),
                                  d->pixel_shader_handle,
                                  d->pixel_shader_hlsl,
                                  d->pixel_shader_dxbc,
                                  d->interpolators.data(),
                                  static_cast<uint32_t>(
                                      d->interpolators.size()),
                                  d->pixel_constants.data(),
                                  static_cast<uint32_t>(
                                      d->pixel_constants.size()),
                                  d->pixel_sampler_count,
                                  d->pixel_textures.data(),
                                  d->pixel_sampled_objects.data(),
                                  d->pixel_sampled_swizzles.data(),
                                  vertexStage.handle ? &vertexStage : nullptr,
                                  d->pixel_sampler_array_mask,
                                  d->pixel_sampler_signs.data(),
                                  d->pixel_param_gen,
                                  d->depth_target_object,
                                  d->depth_target_width,
                                  d->depth_target_height,
                                  d->depth_target_base, d->surface_base,
                                  (d->render_target_color_info >> 16) & 0xFu,
                                  d->scissor_seen ? scissor : nullptr,
                                  // Only when the register shadow was actually
                                  // readable. `alpha_state_seen` false means we
                                  // could not read it, not that the test is
                                  // off, and passing a zeroed control would
                                  // spell that unreadable state as a decoded
                                  // "disabled" — which is the same thing here,
                                  // but only by accident, and would stop being
                                  // so the moment the enable bit's polarity or
                                  // position is ever revisited.
                                  d->alpha_state_seen ? d->colour_control : 0u,
                                  d->alpha_state_seen ? d->alpha_ref : 0.0f);
          static bool s_loggedFirst = false;
          if (!s_loggedFirst) {
            s_loggedFirst = true;
            REXLOG_INFO("RenderThread: first translated draw — {} verts ({} B, stride {}), {} indices, topology {}",
                        d->vertex_count, d->vertices.size(), d->vertex_stride,
                        d->index_count, static_cast<uint32_t>(d->topology));
            REXLOG_INFO("RenderThread: first translated draw target 0x{:08X} "
                        "{}x{} samples resolved target 0x{:08X}",
                        d->render_target_object, d->render_target_width,
                        d->render_target_height,
                        d->sampled_render_target_object);
          }
        }
      }
      m_renderer->ReportAddGameDrawsCost(
          uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                       std::chrono::steady_clock::now() - addDrawsStart)
                       .count()),
          addDrawCalls);

      static uint32_t s_frame = 0;
      if ((submitted || skipped) && (++s_frame % 100) == 1) {
        std::string hist;
        for (const auto& [stride, count] : s_skippedStrides)
          hist += fmt::format("{}:{} ", stride, count);
        REXLOG_INFO("RenderThread: frame #{} submitted {} draws, skipped {} "
                    "— skipped strides (cumulative) {} — ticks with new draws {} "
                    "/ empty wakeups {} — skipped untransformable (cumulative) {}",
                    s_frame, submitted, skipped, hist.empty() ? "none" : hist,
                    s_ticksWithDraws, s_ticksEmpty, s_skippedUntransformable);
      }
      // BeginFrame and EndFrame own the whole frame: BeginFrame opens the
      // command list, transitions and clears the targets and then calls
      // RenderGameFrame itself; EndFrame calls
      // PresentGameFrame and then swaps. This loop used to call
      // RenderGameFrame and PresentGameFrame again in between, which was not a
      // harmless repeat — PresentGameFrame's barriers are directional. Its
      // first call leaves m_gameRT in PIXEL_SHADER_RESOURCE, so EndFrame's
      // call then declared StateBefore = RENDER_TARGET for a resource that was
      // not in it, an invalid transition, on top of drawing and copying the
      // whole frame twice.
      //
      // Nothing new from the guest means the frame this would draw is the one
      // already on screen, byte for byte: m_gameDraws is unchanged, so is every
      // texture it samples, and RenderGameFrame is deterministic in them. Skip
      // it whole.
      //
      // MEASURED, mx_1034: 713 ticks with new draws against 813 without, and an
      // empty tick still spent 25-37ms re-recording ~340 draws, ~50 snapshot
      // copies and ~50 clears to arrive at the same image. Once the upload ring
      // took the allocator out of the tick, that re-recording was the largest
      // phase left in it -- and over half of it was thrown away.
      //
      // NOT PRESENTING is what re-presents. The swapchain is flip-discard, so
      // the last presented back buffer stays on screen until another Present
      // replaces it; the note above about a tick with no new draws having to
      // "re-present the last frame we did get, not a cleared screen" still holds,
      // it is just satisfied by doing nothing rather than by drawing it again.
      //
      // Skipping the pair rather than only RenderGameFrame is deliberate.
      // BeginFrame clears the targets, and every resource state either side of
      // these two is directional -- m_gameRT ends in PIXEL_SHADER_RESOURCE for
      // the next BeginFrame's barrier, the back buffer ends in PRESENT for the
      // next transition. Running half the pair would leave those disagreeing;
      // running neither leaves them exactly where the last full tick put them,
      // which is the state BeginFrame already expects to find.
      //
      // GetDrawCalls above is NOT skipped, so a guest blocked in SetDrawCalls is
      // still released every tick. This shortens that wait rather than extending
      // it: the release no longer sits behind a frame nobody needed.
      //
      // The exception is a tick with nothing to replay. That is startup, before
      // the first guest frame arrives -- it has to run, or the window is never
      // cleared and shows whatever was behind it.
      static bool s_rendered = false;
      static uint64_t s_ticksSkippedRender = 0;
      if (draws.empty() && s_rendered) {
        idle = true;
        // Counted and reported here rather than after the branch: the counter
        // only changes on this path, so testing it on every tick re-logs the
        // same line for as long as it rests on a multiple of 500.
        if ((++s_ticksSkippedRender % 500) == 1)
          REXLOG_INFO("RenderThread: skipped {} identical re-renders",
                      s_ticksSkippedRender);
      } else {
        m_renderer->BeginFrame();
        m_renderer->EndFrame();
        s_rendered = true;
      }
    }
    // Replaces the fixed-tick sleep. The guest's SetDrawCalls already applies
    // backpressure (it blocks until the list is consumed), so an idle tick
    // parks on the mailbox cv and wakes exactly when a frame is posted — no
    // spin, no log flood, and the core goes back to the compute-bound guest.
    // A tick that just rendered loops straight back to consume the next
    // frame; Shutdown() releases the wait and m_running ends the loop.
    if (idle || !m_renderer) {
      native::NativeGraphics::Get().WaitForDrawsOrShutdown();
    }
  }
}

}  // namespace system
}  // namespace rex
