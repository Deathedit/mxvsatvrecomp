#pragma once

// Host-side bridge between the guest hooks (hooks_*.cpp) and the D3D12
// renderer. Owns the guest memory base, the renderer pointer, and the queue of
// PM4-translated draw calls handed from the VdSwap hook to the render thread.

#include <Windows.h>
#include <atomic>
#include <condition_variable>
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
  void Attach(D3D12Renderer* r);

  void BeginFrame();
  void EndFrame();

  // IsInitialized / GetRenderer / ClearDrawCalls removed -- none had a caller
  // anywhere in src/, tools/ or generated/.
  //
  // The guest base pair STAYS, and the reason is worth reading before touching
  // it again. GetGuestMemory() is called by the crash reporter to decide whether
  // a faulting address is inside guest memory. Its writer used to be the
  // EngineInit hook; that hook was deleted and left a note claiming both were
  // dead -- which was wrong, and silently disabled guest-fault classification,
  // because gbase == 0 makes `in_guest` false for every address. Bootstrap now
  // sets it. If the writer is ever removed again, the reporter degrades quietly
  // rather than failing, so check mx_app.cpp first.
  void SetGuestMemory(uint8_t* base) { m_guest_base = base; }
  uint8_t* GetGuestMemory() const { return m_guest_base; }

  void SetDrawCalls(const std::vector<mx::hle::DrawCall>& calls);
  std::vector<mx::hle::DrawCall> GetDrawCalls();
  // Blocks until the guest posts a draw list or Shutdown() runs. The render
  // thread's idle state: the mailbox is the wakeup source, not a clock.
  void WaitForDrawsOrShutdown();

 private:
  NativeGraphics() = default;
  D3D12Renderer* m_renderer = nullptr;
  uint8_t* m_guest_base = nullptr;

  std::mutex m_drawMutex;
  std::condition_variable m_drawConsumed;   // guest waits: list must be empty
  std::condition_variable m_drawAvailable;  // render thread waits: list non-empty
  std::vector<mx::hle::DrawCall> m_drawCalls;
  bool m_acceptDrawCalls = true;
};

}  // namespace mx::native
