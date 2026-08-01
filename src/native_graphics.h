#pragma once

#include <Windows.h>
#include <atomic>
#include <mutex>
#include <memory>

#include <rex/hook.h>
#include <rex/logging.h>

#include "d3d12_renderer.h"
#include "pm4_translator.h"

namespace mx::native {

// Set true when --gpu_plugin=<name> was passed on the command line. All native
// C++ hooks short-circuit to "call orig" when this is true, letting the plugin
// own rendering. Set once from MxApp::OnPreSetup (read in ~23 REX_FUNC hooks).
extern bool g_plugin_mode;

void SetWindowHandle(HWND hwnd);
void SetRenderer(D3D12Renderer* renderer);
void SetBinkPlaying(bool playing);

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

  void SetDrawCalls(const std::vector<mx::pm4::DrawCall>& calls);
  std::vector<mx::pm4::DrawCall> GetDrawCalls();
  void ClearDrawCalls();

 private:
  NativeGraphics() = default;
  D3D12Renderer* m_renderer = nullptr;
  uint8_t* m_guest_base = nullptr;

  std::mutex m_drawMutex;
  std::vector<mx::pm4::DrawCall> m_drawCalls;
};

//=============================================================================
// Hook declarations — replace guest render functions with native implementations
//=============================================================================

// Phase 1: Frame lifecycle
void Hook_RenderInit(PPCContext& ctx, uint8_t* base, u32 arg);
void Hook_BeginFrame(PPCContext& ctx, uint8_t* base, u32 arg);
void Hook_VdSwap(PPCContext& ctx, uint8_t* base);

}  // namespace mx::native
