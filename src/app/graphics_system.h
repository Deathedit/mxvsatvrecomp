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

#include <rex/system/interfaces/graphics.h>
#include <rex/system/xtypes.h>

#include <atomic>
#include <memory>
#include <thread>

#include "gfx/d3d12_renderer.h"

namespace rex {
namespace system {

// The graphics system ReXGlue hands control of presentation to. Owns the
// D3D12 renderer and the host render thread, which plays the Bink intros and
// then falls through to presenting game frames.
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
