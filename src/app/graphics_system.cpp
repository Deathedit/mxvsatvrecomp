#include "app/graphics_system.h"

#include <SDL3/SDL_init.h>
#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_gamepad.h>
#include <rex/cvar.h>
#include <rex/logging.h>

#include <chrono>
#include <iterator>
#include <vector>

#include "gfx/bink_player.h"
#include "hooks/native_bridge.h"

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
      // DrawCall::mvp now reaches the game pipeline's constant buffer. Note it
      // is whatever constant-register block Pm4Translator guessed is the
      // transform — if geometry lands off-screen, that guess is the first
      // suspect, ahead of the vertex data.
      auto draws = native::NativeGraphics::Get().GetDrawCalls();
      for (const auto& d : draws) {
        // vertices are only populated when the translator resolved a vertex
        // fetch constant; index-only draws have nothing to bind.
        if (!d.valid || d.vertices.empty() || d.index_count == 0) continue;
        m_renderer->SetGameDrawData(d.vertices.data(),
                                    static_cast<uint32_t>(d.vertices.size()),
                                    d.vertex_stride, d.indices.data(),
                                    static_cast<uint32_t>(d.indices.size()),
                                    d.index_16bit, d.index_count, d.mvp);
        static bool s_loggedFirst = false;
        if (!s_loggedFirst) {
          s_loggedFirst = true;
          REXLOG_INFO("RenderThread: first translated draw — {} verts ({} B, stride {}), {} indices",
                      d.vertex_count, d.vertices.size(), d.vertex_stride, d.index_count);
        }
        break;  // one draw per frame until the pipeline handles batches
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
