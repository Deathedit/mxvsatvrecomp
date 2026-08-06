#include "hooks/native_bridge.h"

namespace mx::native {

// True when --gpu_plugin=<name> is requested. Hooks short-circuit to call
// their original function instead of diverting through our native path.
bool g_plugin_mode = false;

namespace {

HWND g_native_hwnd = nullptr;
bool g_bink_playing = false;

}  // namespace

bool IsBinkPlaying() {
  return g_bink_playing;
}

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

void SetBinkPlaying(bool playing) {
  g_bink_playing = playing;
}

void NativeGraphics::Shutdown() {
  m_renderer = nullptr;
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
  std::lock_guard<std::mutex> lock(m_drawMutex);
  m_drawCalls = calls;
}

std::vector<mx::hle::DrawCall> NativeGraphics::GetDrawCalls() {
  std::lock_guard<std::mutex> lock(m_drawMutex);
  return std::move(m_drawCalls);
}

void NativeGraphics::ClearDrawCalls() {
  std::lock_guard<std::mutex> lock(m_drawMutex);
  m_drawCalls.clear();
}

}  // namespace mx::native
