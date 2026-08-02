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
#include "hooks/native_bridge.h"

// The one vertex stride the game PSO's input layout actually describes —
// POSITION float3 at offset 0 plus COLOR float4 at offset 12. See the gate in
// RenderThreadFunc.
static constexpr uint32_t kSupportedStride = 28;

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
REXCVAR_DEFINE_BOOL(main_surface_only, false, "Debug",
                    "Submit only draws targeting the guest's main colour "
                    "surface, instead of flattening every render pass into one "
                    "target. Known to make things worse — see the note above");

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
        m_renderer->BeginFrame();
        m_renderer->RenderVideoFrame();
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
      // Only stride 28 matches the input layout the game PSO declares
      // (POSITION float3 @0, COLOR float4 @12), and it is the only stride the
      // vertex dump has actually validated. Anything else would be reinterpreted
      // as position+colour and drawn as noise, so it is counted, not drawn.
      // The histogram is the input to the vertex-format work.
      static std::map<uint32_t, uint32_t> s_skippedStrides;
      static std::map<uint64_t, uint32_t> s_skippedSurfaces;
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
        if (REXCVAR_GET(main_surface_only) &&
            d.surface_pitch != kMainSurfacePitch) {
          ++skipped;
          ++s_skippedSurfaces[(uint64_t(d.surface_base) << 32) |
                              d.surface_pitch];
          continue;
        }
        submittable.push_back(&d);
        ++submitted;
      }

      if (!submittable.empty()) {
        m_renderer->ClearGameDraws();
        for (const auto* d : submittable) {
          m_renderer->AddGameDraw(d->vertices.data(),
                                  static_cast<uint32_t>(d->vertices.size()),
                                  d->vertex_stride, d->indices.data(),
                                  static_cast<uint32_t>(d->indices.size()),
                                  d->index_16bit, d->index_count, d->mvp,
                                  static_cast<uint32_t>(d->topology));
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
        REXLOG_INFO("RenderThread: frame #{} submitted {} draws, skipped {} "
                    "— skipped strides (cumulative) {} — host ticks with/without "
                    "new draws {}/{} — skipped surfaces {}",
                    s_frame, submitted, skipped, hist.empty() ? "none" : hist,
                    s_ticksWithDraws, s_ticksEmpty,
                    surf.empty() ? "none" : surf);
      }
      m_renderer->BeginFrame();
      m_renderer->RenderGameFrame();
      m_renderer->PresentGameFrame();
      m_renderer->EndFrame();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
  }
}

}  // namespace system
}  // namespace rex
