#include "hooks/native_bridge.h"

// The header only forward-declares D3D12Renderer. This is the one place
// that calls through the pointer, so this is the one place that needs the
// definition.
#include "gfx/d3d12_renderer.h"

namespace mx::native {

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
  m_drawAvailable.notify_all();
}

void NativeGraphics::WaitForDrawsOrShutdown() {
  std::unique_lock<std::mutex> lock(m_drawMutex);
  m_drawAvailable.wait(
      lock, [this] { return !m_drawCalls.empty() || !m_acceptDrawCalls; });
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
  m_drawAvailable.notify_one();
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

// ClearDrawCalls() removed 2026-08-17 -- no caller. GetDrawCalls() already
// hands the list out by move and notifies m_drawConsumed, so the mailbox is
// drained by the consumer rather than by anyone clearing it out of band.

}  // namespace mx::native
