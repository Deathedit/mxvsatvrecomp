#pragma once

// Host-side bridge between the guest hooks (hooks_*.cpp) and the D3D12
// renderer. Owns the guest memory base, the renderer pointer, and the queue of
// PM4-translated draw calls handed from the VdSwap hook to the render thread.

#include <Windows.h>
#include <atomic>
#include <mutex>
#include <memory>

#include <rex/hook.h>
#include <rex/logging.h>

#include "gfx/d3d12_renderer.h"
#include "gpu/hle_types.h"

namespace mx::native {

// Set true when --gpu_plugin=<name> was passed on the command line. All native
// C++ hooks short-circuit to "call orig" when this is true, letting the plugin
// own rendering. Set once from MxApp::OnPreSetup (read in ~23 REX_FUNC hooks).
extern bool g_plugin_mode;

void SetWindowHandle(HWND hwnd);
void SetRenderer(D3D12Renderer* renderer);

class NativeGraphics {
 public:
  static NativeGraphics& Get();

  void Shutdown();
  void Attach(D3D12Renderer* r) { m_renderer = r; }

  void BeginFrame();
  void EndFrame();

  bool IsInitialized() const { return m_renderer != nullptr; }

  void SetGuestMemory(uint8_t* base) { m_guest_base = base; }
  uint8_t* GetGuestMemory() const { return m_guest_base; }
  D3D12Renderer* GetRenderer() const { return m_renderer; }

  void SetDrawCalls(const std::vector<mx::hle::DrawCall>& calls);
  std::vector<mx::hle::DrawCall> GetDrawCalls();
  void ClearDrawCalls();

 private:
  NativeGraphics() = default;
  D3D12Renderer* m_renderer = nullptr;
  uint8_t* m_guest_base = nullptr;

  std::mutex m_drawMutex;
  std::vector<mx::hle::DrawCall> m_drawCalls;
};

}  // namespace mx::native
