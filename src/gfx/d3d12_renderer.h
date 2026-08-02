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
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <vector>

class D3D12Renderer {
 public:
  D3D12Renderer() = default;
  ~D3D12Renderer();

  D3D12Renderer(const D3D12Renderer&) = delete;
  D3D12Renderer& operator=(const D3D12Renderer&) = delete;
  D3D12Renderer(D3D12Renderer&&) = delete;
  D3D12Renderer& operator=(D3D12Renderer&&) = delete;

  bool Initialize(HWND hwnd);
  void Shutdown();

  void BeginFrame();
  void EndFrame();

void UploadVideoFrame(const uint8_t* rgba, uint32_t width, uint32_t height);

// Append one translated draw to this frame's list. `mvp` is the 16-float
// row-major transform the PM4 translator recovered (DrawCall::mvp); pass
// nullptr to fall back to the identity placeholder. `topology` is a
// D3D_PRIMITIVE_TOPOLOGY value, matching mx::pm4::HostTopology.
//
// This replaced SetGameDrawData, which held exactly one draw — so however many
// draws a frame translated, at most one could ever be submitted.
void AddGameDraw(const uint8_t* vertices, uint32_t vtxBytes, uint32_t vtxStride,
                 const uint8_t* indices, uint32_t idxBytes, bool idx16,
                 uint32_t idxCount, const float* mvp, uint32_t topology);

// Drop the previous frame's draws. Called once per render-thread iteration
// before any AddGameDraw, so a frame that translated nothing falls back to the
// placeholder triangle rather than replaying stale geometry.
void ClearGameDraws();

  [[nodiscard]] ID3D12Device* GetDevice() const noexcept { return m_device.Get(); }
  [[nodiscard]] ID3D12GraphicsCommandList* GetCommandList() const noexcept {
    return m_commandList.Get();
  }
  [[nodiscard]] uint32_t GetCurrentBackBufferIndex() const noexcept {
    return m_frameIndex;
  }
  [[nodiscard]] uint32_t GetWidth() const noexcept { return m_width; }
  [[nodiscard]] uint32_t GetHeight() const noexcept { return m_height; }
  void RenderGameFrame();
  void PresentGameFrame();
  void RenderVideoFrame();


 private:
  static constexpr uint32_t kFrameCount = 3;
  static constexpr DXGI_FORMAT kBackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

  bool CreateFactory();
  bool CreateDevice();
  bool CreateCommandQueue();
  bool CreateSwapChain();
  bool CreateRtvDescriptorHeap();
  bool CreateRenderTargetViews();
  bool CreateCommandAllocator();
  bool CreateCommandList();
  bool CreateFence();
  void CreateViewportAndScissor();

  bool CreateVideoPipeline();

bool CreateGamePipeline();
  bool CreateGameRenderTargets();

  void WaitForGpu();
  void MoveToNextFrame();

  HWND m_hwnd = nullptr;
  uint32_t m_width = 1280;
  uint32_t m_height = 720;
  bool m_allowTearing = false;
  bool m_initialized = false;

  Microsoft::WRL::ComPtr<IDXGIFactory6> m_factory;
  Microsoft::WRL::ComPtr<ID3D12Device> m_device;
  Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_commandQueue;
  Microsoft::WRL::ComPtr<IDXGISwapChain3> m_swapChain;
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
  std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, kFrameCount> m_renderTargets;
  std::array<Microsoft::WRL::ComPtr<ID3D12CommandAllocator>, kFrameCount> m_commandAllocators;
  Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_commandList;
  Microsoft::WRL::ComPtr<ID3D12Fence> m_fence;
  uint64_t m_fenceValues[kFrameCount] = {};
  uint64_t m_fenceValue = 0;
  HANDLE m_fenceEvent = nullptr;
  uint32_t m_rtvDescriptorSize = 0;
  uint32_t m_frameIndex = 0;
  D3D12_VIEWPORT m_viewport = {};
  D3D12_RECT m_scissorRect = {};

  Microsoft::WRL::ComPtr<ID3D12RootSignature> m_videoRootSignature;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> m_videoPipelineState;
  Microsoft::WRL::ComPtr<ID3D12Resource> m_videoTexture;
  Microsoft::WRL::ComPtr<ID3D12Resource> m_videoUploadBuffer;
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_videoSrvHeap;
  bool m_hasVideoFrame = false;
  uint32_t m_videoWidth = 0;
  uint32_t m_videoHeight = 0;

  Microsoft::WRL::ComPtr<ID3D12RootSignature> m_gameRootSig;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> m_gamePSO;
  Microsoft::WRL::ComPtr<ID3D12Resource> m_gameVB;
  Microsoft::WRL::ComPtr<ID3D12Resource> m_gameIB;
  Microsoft::WRL::ComPtr<ID3D12Resource> m_gameCB;
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_gameCbvHeap;
  D3D12_VERTEX_BUFFER_VIEW m_gameVbv = {};
  D3D12_INDEX_BUFFER_VIEW m_gameIbv = {};
  uint32_t m_gameIndexCount = 0;
  bool m_hasGamePipeline = false;

  // One translated draw. The CB is separate from m_gameCB so a translated
  // transform never overwrites the placeholder triangle's identity matrix, and
  // so a per-draw buffer is not rewritten while the GPU may still be reading
  // the previous frame's value.
  struct GameDraw {
    Microsoft::WRL::ComPtr<ID3D12Resource> vb;
    Microsoft::WRL::ComPtr<ID3D12Resource> ib;
    Microsoft::WRL::ComPtr<ID3D12Resource> cb;
    D3D12_VERTEX_BUFFER_VIEW vbv = {};
    D3D12_INDEX_BUFFER_VIEW ibv = {};
    uint32_t indexCount = 0;
    D3D12_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
  };
  // Bounded because each entry costs three CreateCommittedResource calls — see
  // the PERF(per-frame-allocs) note in d3d12_game.cpp.
  static constexpr size_t kMaxGameDraws = 256;
  std::vector<GameDraw> m_gameDraws;

  Microsoft::WRL::ComPtr<ID3D12Resource> m_gameRT;
  Microsoft::WRL::ComPtr<ID3D12Resource> m_gameDepth;
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_gameRtvHeap;
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_gameDsvHeap;
};
