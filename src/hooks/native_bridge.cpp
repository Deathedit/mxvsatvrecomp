#include "hooks/native_bridge.h"

namespace mx::native {

// True when --gpu_plugin=<name> is requested. Hooks short-circuit to call
// their original function instead of diverting through our native path.
bool g_plugin_mode = false;

namespace {

HWND g_native_hwnd = nullptr;

}  // namespace

NativeGraphics& NativeGraphics::Get() {
  static NativeGraphics instance;
  return instance;
}

void SetRenderer(D3D12Renderer* renderer) {
  NativeGraphics::Get().Attach(renderer);
  REXLOG_INFO("NativeGraphics: renderer set");
}

void SetWindowHandle(HWND hwnd) {
  g_native_hwnd = hwnd;
}

void NativeGraphics::Attach(D3D12Renderer* r) {
  {
    std::lock_guard<std::mutex> lock(m_drawMutex);
    m_renderer = r;
    m_acceptDrawCalls = r != nullptr;
  }
  m_drawConsumed.notify_all();
}

void NativeGraphics::Shutdown() {
  {
    std::lock_guard<std::mutex> lock(m_drawMutex);
    m_acceptDrawCalls = false;
    m_drawCalls.clear();
    m_renderer = nullptr;
  }
  // A guest VdSwap may be waiting for the render thread to consume the
  // previous frame. Release it when that consumer is being torn down.
  m_drawConsumed.notify_all();
}

void NativeGraphics::BeginFrame() {
  if (!m_renderer) return;
  m_renderer->BeginFrame();
}

void NativeGraphics::EndFrame() {
  if (!m_renderer) return;
  m_renderer->EndFrame();
}

void NativeGraphics::SetDrawCalls(const std::vector<mx::hle::DrawCall>& calls) {
  std::unique_lock<std::mutex> lock(m_drawMutex);
  const bool waited = !m_drawCalls.empty();
  // This is a one-frame mailbox, but it must not be a lossy one. Clear and
  // Resolve are ordered commands carried in the frame list; replacing an
  // unconsumed list can permanently discard a render-to-texture update even
  // though a newer geometry frame looks superficially complete.
  m_drawConsumed.wait(
      lock, [this] { return m_drawCalls.empty() || !m_acceptDrawCalls; });
  if (!m_acceptDrawCalls) return;
  m_drawCalls = calls;
  if (waited) {
    static std::atomic<bool> s_logged = false;
    if (!s_logged.exchange(true)) {
      REXLOG_INFO(
          "NativeGraphics: lossless draw handoff applied backpressure");
    }
  }
}

std::vector<mx::hle::DrawCall> NativeGraphics::GetDrawCalls() {
  std::unique_lock<std::mutex> lock(m_drawMutex);
  auto calls = std::move(m_drawCalls);
  m_drawCalls.clear();
  lock.unlock();
  m_drawConsumed.notify_one();
  return calls;
}

void NativeGraphics::ClearDrawCalls() {
  {
    std::lock_guard<std::mutex> lock(m_drawMutex);
    m_drawCalls.clear();
  }
  m_drawConsumed.notify_one();
}

}  // namespace mx::native
