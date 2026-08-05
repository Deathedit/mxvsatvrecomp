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
#include <deque>
#include <memory>
#include <unordered_map>
#include <vector>

namespace mx::pm4 { struct HleTexturePayload; }

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
                 uint32_t idxCount, const float* mvp, uint32_t topology,
                 bool depthEnable, bool depthWrite, bool colorWrite,
                 std::shared_ptr<const mx::pm4::HleTexturePayload> texture = {});

// Drop the previous frame's draws and retire the startup placeholder. Called
// when a real guest-frame handoff arrives, including one whose draws are all
// filtered; an empty render-thread tick still re-presents the previous frame.
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

 private:
  static constexpr uint32_t kFrameCount = 3;
  static constexpr DXGI_FORMAT kBackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
  // The offscreen game depth buffer's format. Named because it has to appear in
  // three places that must agree — the resource, its clear value, and the PSO's
  // DSVFormat — and the PSO's copy was the one that got left out.
  static constexpr DXGI_FORMAT kGameDepthFormat = DXGI_FORMAT_D32_FLOAT;

  // Frame internals. BeginFrame picks between the two Render* and EndFrame
  // calls PresentGameFrame, so nothing outside this class should ever invoke
  // them: PresentGameFrame's barriers are directional and a second call
  // declares a StateBefore the first one already moved away from. These were
  // public, and the render thread was calling RenderGameFrame and
  // PresentGameFrame a second time in between BeginFrame and EndFrame.
  void RenderGameFrame();
  void PresentGameFrame();
  void RenderVideoFrame();

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
  bool EnsureGameTexture(const std::shared_ptr<const mx::pm4::HleTexturePayload>& texture,
                         uint32_t& descriptorIndex);

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
  // The guest's aspect. It renders 1280x720 and nothing tells it otherwise —
  // no video-mode export is hooked — so this is the shape the image must keep
  // however wide the host window is.
  static constexpr float kGuestAspect = 16.0f / 9.0f;

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
  // Indexed by depth-enable bit 0, depth-write bit 1, no-colour bit 2,
  // textured bit 3.
  std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 16> m_gamePSOs;
  Microsoft::WRL::ComPtr<ID3D12Resource> m_gameVB;
  Microsoft::WRL::ComPtr<ID3D12Resource> m_gameIB;
  Microsoft::WRL::ComPtr<ID3D12Resource> m_gameCB;
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_gameCbvHeap;
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_gameSrvHeap;
  uint32_t m_gameSrvDescriptorSize = 0;
  static constexpr uint32_t kMaxGameTextures = 1024;
  struct GameTexture {
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    Microsoft::WRL::ComPtr<ID3D12Resource> upload;
    uint32_t descriptorIndex = 0;
  };
  std::unordered_map<uint64_t, GameTexture> m_gameTextures;
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
    bool depthEnable = false;
    bool depthWrite = false;
    bool colorWrite = true;
    std::shared_ptr<const mx::pm4::HleTexturePayload> texture;
  };
  // Bounded because each entry costs three CreateCommittedResource calls — see
  // the PERF(per-frame-allocs) note in d3d12_game.cpp.
  static constexpr size_t kMaxGameDraws = 256;
  std::vector<GameDraw> m_gameDraws;

  // Resources whose last GPU use was in the frame that signalled `fence`.
  //
  // A D3D12 command list does not reference-count the resources it references —
  // recording a draw against a buffer keeps nothing alive. ClearGameDraws used
  // to release every draw's vb/ib/cb directly, once per frame, while the
  // previous frame's command list was still in flight on the queue, so the GPU
  // could be reading UPLOAD-heap memory the CPU had already freed. Resources
  // now move here instead and are released only once m_fence has passed the
  // value signalled for the submission that last used them.
  struct RetiredFrame {
    uint64_t fence = 0;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> res;
  };
  std::deque<RetiredFrame> m_retired;

  // Release everything the GPU has finished with. Cheap and called per frame.
  void DrainRetired();

  // Latched true by the first AddGameDraw and never cleared until Shutdown.
  // Once the guest has produced real geometry the placeholder triangle is
  // retired for good: a later frame that translates nothing (or whose draws are
  // all skipped for stride) must show an empty scene, not the placeholder.
  bool m_hasEverDrawnGame = false;

  Microsoft::WRL::ComPtr<ID3D12Resource> m_gameRT;
  Microsoft::WRL::ComPtr<ID3D12Resource> m_gameDepth;
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_gameRtvHeap;
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_gameDsvHeap;
};
