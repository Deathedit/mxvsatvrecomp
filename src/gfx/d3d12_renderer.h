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

void SetGameDrawData(const uint8_t* vertices, uint32_t vtxBytes, uint32_t vtxStride,
                     const uint8_t* indices, uint32_t idxBytes, bool idx16,
                     uint32_t idxCount);

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

  Microsoft::WRL::ComPtr<ID3D12Resource> m_gameDrawVB;
  Microsoft::WRL::ComPtr<ID3D12Resource> m_gameDrawIB;
  D3D12_VERTEX_BUFFER_VIEW m_gameDrawVbv = {};
  D3D12_INDEX_BUFFER_VIEW m_gameDrawIbv = {};
  uint32_t m_gameDrawIndexCount = 0;
  bool m_hasGameDrawData = false;

  Microsoft::WRL::ComPtr<ID3D12Resource> m_gameRT;
  Microsoft::WRL::ComPtr<ID3D12Resource> m_gameDepth;
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_gameRtvHeap;
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_gameDsvHeap;
};
