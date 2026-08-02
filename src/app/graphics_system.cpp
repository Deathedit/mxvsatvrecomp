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
      // pipeline. GetDrawCalls moves-and-clears, so a frame with no VdSwap gets
      // an empty list and RenderGameFrame falls back to the placeholder
      // triangle rather than replaying stale geometry.
      //
      // DrawCall::mvp is the window-space -> NDC transform Pm4Translator built
      // from the guest's PA_CL_VPORT_* registers. It used to be the ALU
      // constant block, which this game never writes — so it was identically
      // zero and collapsed every vertex to the origin.
      auto draws = native::NativeGraphics::Get().GetDrawCalls();
      m_renderer->ClearGameDraws();
      uint32_t submitted = 0, skipped = 0;
      // Only stride 28 matches the input layout the game PSO declares
      // (POSITION float3 @0, COLOR float4 @12), and it is the only stride the
      // vertex dump has actually validated. Anything else would be reinterpreted
      // as position+colour and drawn as noise, so it is counted, not drawn.
      // The histogram is the input to the vertex-format work.
      static std::map<uint32_t, uint32_t> s_skippedStrides;
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
        m_renderer->AddGameDraw(d.vertices.data(),
                                static_cast<uint32_t>(d.vertices.size()),
                                d.vertex_stride, d.indices.data(),
                                static_cast<uint32_t>(d.indices.size()),
                                d.index_16bit, d.index_count, d.mvp,
                                static_cast<uint32_t>(d.topology));
        ++submitted;
        static bool s_loggedFirst = false;
        if (!s_loggedFirst) {
          s_loggedFirst = true;
          REXLOG_INFO("RenderThread: first translated draw — {} verts ({} B, stride {}), {} indices, topology {}",
                      d.vertex_count, d.vertices.size(), d.vertex_stride,
                      d.index_count, static_cast<uint32_t>(d.topology));
        }
      }
      static uint32_t s_frame = 0;
      if ((submitted || skipped) && (++s_frame % 100) == 1) {
        std::string hist;
        for (const auto& [stride, count] : s_skippedStrides)
          hist += fmt::format("{}:{} ", stride, count);
        REXLOG_INFO("RenderThread: frame #{} submitted {} draws, skipped {} "
                    "— skipped strides (cumulative) {}",
                    s_frame, submitted, skipped, hist.empty() ? "none" : hist);
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
