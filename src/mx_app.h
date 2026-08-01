#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef UNICODE
#define UNICODE
#endif

#include <Windows.h>
#include <timeapi.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_gamepad.h>
#include <rex/rex_app.h>
#include <rex/cvar.h>
#include <rex/graphics/flags.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/system/xtypes.h>

#include "d3d12_renderer.h"
#include "bink_player.h"
#include "native_graphics.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace rex {
namespace system {

class D3D12GraphicsSystem final : public IGraphicsSystem {
 public:
  D3D12GraphicsSystem() = default;
  ~D3D12GraphicsSystem() override { Shutdown(); }
  D3D12GraphicsSystem(const D3D12GraphicsSystem&) = delete;
  D3D12GraphicsSystem& operator=(const D3D12GraphicsSystem&) = delete;

X_STATUS SetupPresentation(ui::WindowedAppContext* app_context) override {
    m_initialized = true;
    return X_STATUS_SUCCESS;
  }
  X_STATUS SetupGuestGpu(runtime::FunctionDispatcher*, KernelState*) override {
    return X_STATUS_SUCCESS;
  }
  bool has_presentation() const override { return m_initialized; }
  void Shutdown() override {
    m_running = false;
    if (m_renderThread.joinable()) m_renderThread.join();
    m_renderer.reset();
    m_initialized = false;
  }
bool InitializeRenderer(HWND hwnd);
  void RenderThreadFunc();

 private:
  static constexpr const char* kIntroVideos[] = {
      "assets\\Videos\\THQ_Logo_wSound.bik",
      "assets\\Videos\\Attract.ENG.bik",
  };
  HWND m_hwnd = nullptr;
  std::unique_ptr<D3D12Renderer> m_renderer;
  std::thread m_renderThread;
  std::atomic<bool> m_running{false};
  bool m_initialized = false;
};

}  // namespace system
}  // namespace rex

inline bool rex::system::D3D12GraphicsSystem::InitializeRenderer(HWND hwnd) {
  m_hwnd = hwnd;
  m_renderer = std::make_unique<D3D12Renderer>();
  if (!m_renderer->Initialize(hwnd)) return false;
  mx::native::SetRenderer(m_renderer.get());
  m_running = true;
  m_renderThread = std::thread([this]() { RenderThreadFunc(); });
  return true;
}

inline void rex::system::D3D12GraphicsSystem::RenderThreadFunc() {
  using namespace mx;
  BinkPlayer bink;
  const char* videos[] = {"assets\\Videos\\THQ_Logo_wSound.bik",
                           "assets\\Videos\\Attract.ENG.bik"};
  for (int i = 0; i < 2 && m_running; ++i) {
    if (!bink.Open(videos[i])) continue;
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
      m_renderer->BeginFrame();
      m_renderer->RenderGameFrame();
      m_renderer->PresentGameFrame();
      m_renderer->EndFrame();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
  }
}

class MxApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<MxApp>(new MxApp(ctx, "mx",
        PPCImageConfig));
  }

  void OnPreSetup(rex::RuntimeConfig& config) override;
  void OnPostSetup() override;

 private:
  rex::system::D3D12GraphicsSystem* m_graphicsSystem = nullptr;
};

inline void MxApp::OnPreSetup(rex::RuntimeConfig& config) {
  REXLOG_INFO("MxApp::OnPreSetup");

  if (!config.gpu_plugin.empty()) {
    REXLOG_INFO("MxApp::OnPreSetup - gpu_plugin='{}' requested, deferring to runtime",
                config.gpu_plugin);
    mx::native::g_plugin_mode = true;
    return;
  }

  auto gs = std::make_unique<rex::system::D3D12GraphicsSystem>();
  m_graphicsSystem = gs.get();
  config.graphics = std::move(gs);
}

inline void MxApp::OnPostSetup() {
  REXLOG_INFO("MxApp::OnPostSetup");

  // Dump all registered cvars (post plugin load — includes GPU plugin cvars)
  REXLOG_INFO("MxApp::OnPostSetup — dumping all cvars:");
  for (const auto& name : rex::cvar::ListFlags()) {
    const auto* info = rex::cvar::GetFlagInfo(name);
    std::string cur = rex::cvar::GetFlagByName(name);
    bool nondef = rex::cvar::HasNonDefaultValue(name);
    REXLOG_INFO("  cvar: {} = '{}' (default='{}', nondef={})",
                name, cur,
                info ? info->default_value : std::string{"?"},
                nondef);
  }

  rex::ReXApp::OnPostSetup();
  if (m_graphicsSystem == nullptr) {
    REXLOG_INFO("MxApp::OnPostSetup - no graphics system");
    return;
  }
  auto* w = window();
  if (w == nullptr) {
    REXLOG_INFO("MxApp::OnPostSetup - no window");
    return;
  }
  HWND hwnd = static_cast<HWND>(w->GetNativeWindowHandle());
  REXLOG_INFO("MxApp::OnPostSetup - hwnd=0x{:08X}", (uintptr_t)hwnd);
  mx::native::SetWindowHandle(hwnd);
  m_graphicsSystem->InitializeRenderer(hwnd);
  REXLOG_INFO("MxApp::OnPostSetup done");
}
