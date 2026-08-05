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

#include "gfx/bink_player.h"
#include "gpu/pm4_translator.h"
#include "gpu/d3d9_layout.h"
#include "hooks/native_bridge.h"

// The one vertex stride the game PSO's input layout actually describes —
// POSITION float3 at offset 0 plus COLOR float4 at offset 12. See the gate in
// RenderThreadFunc.
static constexpr uint32_t kSupportedStride = mx::pm4::kHostVertexStride;

// RB_SURFACE_INFO pitch of the guest's main scene target, matching the 1280x720
// output. A frame also renders into ~15 other colour surfaces at pitches 0, 80,
// 160, 320, 400, 640, 800 and 2609; with one host render target those all
// overpaint each other. See the gate in RenderThreadFunc.
static constexpr uint32_t kMainSurfacePitch = 1280;

// Default OFF — this was tried at true and made things worse, measurably.
// Gating on pitch 1280 dropped submitted draws from 85-89/frame to 5, because
// the stride-28 draws we can actually render mostly do NOT live on the
// pitch-1280 surface; the 62884 draws counted there are overwhelmingly strides
// we reject anyway. It also destabilised the run: 2 of 4 gated runs took an
// access violation on the translator thread during load, against 0 of 4
// ungated, most likely because a render thread doing 5 draws instead of 85
// spins far faster and shifts the timing of a known race.
//
// Kept, off, because the mechanism is right and the selector is wrong: the fix
// is to honour the surface binding (render passes to separate targets, present
// the one the guest swaps), not to guess one surface by pitch.
// Two halves of one diagnostic: show the frame without the overpaint, and show
// the overpaint by itself.
//
// Measured: draws with no colour attribute average 16 vertices yet their
// transformed bounding box covers 52% of the viewport, against 0.45% for draws
// carrying a real packed colour — a 115x difference on a hundredth of the
// vertices. 6581 of them cover more than half the screen each. That is not a
// fullscreen pass (which would be a handful of draws); it is small geometry
// smeared across the viewport by a bad transform.
//
// hide_colorless_draws asks whether a real scene is underneath.
// hide_colored_draws shows the shape of the smear on its own.
//
// DIAGNOSTICS, not rendering modes — the hidden draws are still produced and
// still wrong. Never set both: that submits almost nothing and is not a
// configuration worth reporting.
REXCVAR_DEFINE_BOOL(hide_colorless_draws, false, "Debug",
                    "Do not submit draws whose shader has no colour attribute "
                    "(written opaque white). A diagnostic: it says whether a "
                    "real scene is hidden under the overpaint, not a fix");
REXCVAR_DEFINE_BOOL(hide_colored_draws, false, "Debug",
                    "Do not submit draws that resolved a vertex colour, so only "
                    "the colourless ones remain. A diagnostic: it shows the "
                    "shape of the overpaint on its own, not a fix");

// The D3D9 -> D3D12 high-level path, step one: describe every draw from the
// API calls that produced it instead of reconstructing it from PM4.
//
// Capture only. It changes nothing that is submitted to D3D12 — no PSOs, no
// uploads, no shader translation — so with it off a run is byte-identical to
// one built before it existed. The state shadow behind it fills unconditionally
// (state set before the first draw would otherwise read as unknown); this cvar
// gates the per-draw scoring, the coverage report and the dump.
REXCVAR_DEFINE_BOOL(hle_render, false, "Debug",
                    "Build draws from the D3D9 description — declaration, "
                    "stream bindings and index buffer — and submit those "
                    "instead of the PM4 translator's. Off by default: PM4 "
                    "still owns rendering until this is proven");

REXCVAR_DEFINE_BOOL(hle_capture, false, "Debug",
                    "Score every D3D9 draw against the state shadow and report "
                    "what fraction is fully described, plus the first few "
                    "resolved draws to d3d9_dump_decls.txt. Capture only — it "
                    "submits nothing and renders nothing");

// Samples the guest's own vertex shader microcode, from the D3D9 side, on the
// vertices of the draws hle_capture describes. hle_render independently runs
// every referenced vertex and renders it. hle_shader_exec only controls the
// sampled measurement against the clip volume and its report.
//
// A divisor rather than a bool because the interpreter's cost is the open
// question and a fixed sampling rate cannot answer it: at 64 the measurement is
// a sliver of one draw, at 1 it is what using the thing actually costs. 0 is
// off, and off is the default — this changes nothing that is submitted.
REXCVAR_DEFINE_UINT32(hle_shader_exec, 0, "Debug",
                      "Execute the bound guest vertex shader for one D3D9 draw "
                      "in N (0 = off, 1 = every draw) and report where the "
                      "exported positions land. Requires hle_capture. Capture "
                      "only — it renders nothing");

// A cvar rather than the constant it replaces for the same reason as the one
// above: the cost scales with it, so the run matrix has to vary it without a
// rebuild, or the four configurations are four different binaries and the
// timings are not comparable.
REXCVAR_DEFINE_UINT32(hle_shader_verts, 8, "Debug",
                      "How many vertices of each executed draw to run the "
                      "guest vertex shader on. Only has effect when "
                      "hle_shader_exec is non-zero");

REXCVAR_DEFINE_BOOL(main_surface_only, false, "Debug",
                    "Submit only draws targeting the guest's main colour "
                    "surface by PM4 pitch. Known to make PM4 rendering worse; "
                    "see the note above");

// HLE does not yet create a host target for every guest render target. Until
// it does, mixing the 129x129 shadow pass and other off-screen viewports into
// the 1280x720 scene produces the long white wedges seen in ST_Southwest.
// Unlike main_surface_only's PM4 pitch guess, this selector comes from D3D9's
// resolved, render-target-clamped viewport and is enabled only for HLE.
REXCVAR_DEFINE_BOOL(hle_main_viewport_only, true, "Debug",
                    "In HLE rendering, submit only draws using the resolved "
                    "1280x720 D3D9 viewport until separate render targets are "
                    "modelled");

// The intro playlist runs 47.4s (THQ 9.5s + Attract 37.9s) and the RenderPipeline
// hook stands down for its whole duration, so the guest render path cannot run
// until it finishes. Set `skip_intro = true` in mx.toml (or pass --skip_intro)
// to jump straight to the game-frame loop when iterating on rendering.
REXCVAR_DEFINE_BOOL(skip_intro, false, "Debug",
                    "Skip the Bink intro videos and go straight to game frames");

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
  const bool skipIntro = REXCVAR_GET(skip_intro);
  REXLOG_INFO("RenderThread: skip_intro={}", skipIntro);
  BinkPlayer bink;
  for (size_t i = 0; i < std::size(kIntroVideos) && m_running && !skipIntro; ++i) {
    if (!bink.Open(kIntroVideos[i])) continue;
    native::SetBinkPlaying(true);
    std::vector<uint8_t> rgba;
    int frameMs = bink.GetFrameDurationMs();
    if (frameMs <= 0) frameMs = 33;  // ~30fps fallback
    while (m_running) {
      auto t0 = std::chrono::steady_clock::now();
      if (!bink.DecodeNextFrame(rgba)) break;
      if (!rgba.empty() && m_renderer)
        m_renderer->UploadVideoFrame(rgba.data(), bink.GetVideoWidth(), bink.GetVideoHeight());
      if (m_renderer) {
        // BeginFrame dispatches to RenderVideoFrame itself when a video frame
        // has been uploaded, and EndFrame does the present. Calling either
        // again here is not idempotent — see the note in the game loop below.
        m_renderer->BeginFrame();
        m_renderer->EndFrame();
      }
      // Pace to video frame rate (Bink has no internal clock sync).
      auto t1 = std::chrono::steady_clock::now();
      int elapsed = static_cast<int>(
          std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());
      int sleep = frameMs - elapsed;
      if (sleep > 0) std::this_thread::sleep_for(std::chrono::milliseconds(sleep));
    }
    native::SetBinkPlaying(false);
  }
  while (m_running) {
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

      // This loop ticks on a fixed 16ms sleep, and the guest swaps at its own
      // rate, so most iterations find nothing new. Such a tick must re-present
      // the last frame we did get, not a cleared screen: GetDrawCalls
      // moves-and-clears, so an unconditional ClearGameDraws here threw away
      // the only geometry we had every time the two rates disagreed. Combined
      // with the placeholder-triangle fallback in RenderGameFrame, that made
      // the post-load screen alternate between guest geometry and the
      // placeholder — invisible while the render target accumulated, a visible
      // flash once the clear was fixed.
      static uint64_t s_ticksWithDraws = 0, s_ticksEmpty = 0;
      if (draws.empty()) ++s_ticksEmpty; else ++s_ticksWithDraws;

      uint32_t submitted = 0, skipped = 0;
      // Stride 28 is the input layout the game PSO declares — POSITION float3
      // @0, COLOR float4 @12 — and anything else would be reinterpreted as
      // position+colour and drawn as noise, so it is still counted rather than
      // drawn.
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
      static std::map<uint64_t, uint32_t> s_skippedSurfaces;
      static std::map<uint64_t, uint32_t> s_skippedViewports;
      static uint64_t s_skippedUntransformable = 0;
      static uint64_t s_skippedByColor = 0;
      // Filter first, bind second. The renderer's list is only replaced once we
      // know the new frame has something in it — a frame whose draws were all
      // skipped for stride would otherwise blank the screen just as surely as a
      // frame that never arrived.
      std::vector<const mx::pm4::DrawCall*> submittable;
      for (const auto& d : draws) {
        // vertices are only populated when the translator resolved a vertex
        // fetch constant; index-only draws have nothing to bind.
        if (!d.valid || d.vertices.empty() || d.index_count == 0) continue;
        if (d.topology == mx::pm4::HostTopology::kUndefined) {
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
        // Colour-source diagnostics. Counted separately from every other skip
        // reason so a screenshot taken with either one on can be read honestly
        // against the draw counts.
        {
          using CS = mx::pm4::DrawCall::ColorSource;
          // Spelled out rather than using !colorless, which would also catch
          // kNotTranscoded — those resolved no colour at all and are neither
          // population. The stride gate below drops them regardless.
          const bool colorless = d.color_source == CS::kNone && !d.texture;
          const bool colored = d.color_source == CS::kPacked ||
                               d.color_source == CS::kFallback;
          if ((REXCVAR_GET(hide_colorless_draws) && colorless) ||
              (REXCVAR_GET(hide_colored_draws) && colored)) {
            ++skipped;
            ++s_skippedByColor;
            continue;
          }
        }
        if (d.vertex_stride != kSupportedStride) {
          ++skipped;
          ++s_skippedStrides[d.vertex_stride];
          continue;
        }
        // A frame touches ~16 distinct guest colour surfaces (measured) and we
        // have one host render target, so without this every off-screen pass —
        // the pitch-80/160 buffers, the pitch-800 pass — overpaints the main
        // scene, and whichever one happens to be last decides what is on screen.
        // That is what made the window cycle through colours. Draw only the
        // surface whose pitch matches the 1280x720 output, which is also the one
        // carrying the plurality of draws by a wide margin (~63000 of ~140000).
        //
        // Skipped entirely under hle_render: the surface a draw targets comes
        // from RB_SURFACE_INFO, which is a PM4 register the D3D9 path never
        // sees. Leaving the filter on would drop every HLE draw for having
        // pitch 0 — silently, and looking exactly like "HLE produced nothing".
        // Modelling render targets is a later step; pretending to know the
        // pitch here would be worse than admitting the gap.
        if (REXCVAR_GET(hle_render)) {
          if (REXCVAR_GET(hle_main_viewport_only) &&
              (d.viewport_width != 1280 || d.viewport_height != 720)) {
            ++skipped;
            ++s_skippedViewports[(uint64_t(d.viewport_width) << 32) |
                                 d.viewport_height];
            continue;
          }
        } else if (REXCVAR_GET(main_surface_only) &&
                   d.surface_pitch != kMainSurfacePitch) {
            ++skipped;
            ++s_skippedSurfaces[(uint64_t(d.surface_base) << 32) |
                                d.surface_pitch];
            continue;
        }
        submittable.push_back(&d);
        ++submitted;
      }

      // A non-empty handoff is a real guest frame even when every draw is
      // filtered. Retire the previous frame and, crucially, retire the baked
      // placeholder triangle. Previously ClearGameDraws only ran when a draw
      // survived filtering, so hide_colorless_draws filtered all 14 startup
      // draws and left m_hasEverDrawnGame false forever: the guest kept
      // swapping while the host visibly replayed its placeholder.
      if (!draws.empty()) {
        m_renderer->ClearGameDraws();
      }
      if (!submittable.empty()) {
        for (const auto* d : submittable) {
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
                                  d->texture);
          static bool s_loggedFirst = false;
          if (!s_loggedFirst) {
            s_loggedFirst = true;
            REXLOG_INFO("RenderThread: first translated draw — {} verts ({} B, stride {}), {} indices, topology {}",
                        d->vertex_count, d->vertices.size(), d->vertex_stride,
                        d->index_count, static_cast<uint32_t>(d->topology));
          }
        }
      }
      static uint32_t s_frame = 0;
      if ((submitted || skipped) && (++s_frame % 100) == 1) {
        std::string hist;
        for (const auto& [stride, count] : s_skippedStrides)
          hist += fmt::format("{}:{} ", stride, count);
        std::string surf;
        for (const auto& [key, count] : s_skippedSurfaces)
          surf += fmt::format("{:03X}/{}:{} ", uint32_t(key >> 32),
                              uint32_t(key & 0xFFFFFFFF), count);
        std::string viewports;
        for (const auto& [key, count] : s_skippedViewports)
          viewports += fmt::format("{}x{}:{} ", uint32_t(key >> 32),
                                   uint32_t(key & 0xFFFFFFFF), count);
        REXLOG_INFO("RenderThread: frame #{} submitted {} draws, skipped {} "
                    "— skipped strides (cumulative) {} — host ticks with/without "
                    "new draws {}/{} — skipped surfaces {} — skipped viewports {} — skipped "
                    "untransformable (cumulative) {} — skipped by colour "
                    "source (cumulative) {}",
                    s_frame, submitted, skipped, hist.empty() ? "none" : hist,
                    s_ticksWithDraws, s_ticksEmpty,
                    surf.empty() ? "none" : surf,
                    viewports.empty() ? "none" : viewports,
                    s_skippedUntransformable,
                    s_skippedByColor);
      }
      // BeginFrame and EndFrame own the whole frame: BeginFrame opens the
      // command list, transitions and clears the targets and then calls
      // RenderGameFrame (or RenderVideoFrame) itself; EndFrame calls
      // PresentGameFrame and then swaps. This loop used to call
      // RenderGameFrame and PresentGameFrame again in between, which was not a
      // harmless repeat — PresentGameFrame's barriers are directional. Its
      // first call leaves m_gameRT in PIXEL_SHADER_RESOURCE, so EndFrame's
      // call then declared StateBefore = RENDER_TARGET for a resource that was
      // not in it, an invalid transition, on top of drawing and copying the
      // whole frame twice.
      m_renderer->BeginFrame();
      m_renderer->EndFrame();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
  }
}

}  // namespace system
}  // namespace rex
