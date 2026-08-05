// D3D12Renderer — device, swapchain and per-frame lifecycle.
//
// Owns everything that exists once for the lifetime of the window: the DXGI
// factory/adapter, the device, the direct command queue, the flip-discard
// swapchain and its RTVs, the command allocators/list, and the frame fence.
// The video pipeline lives in d3d12_video.cpp, the game pipeline and the game
// render target in d3d12_game.cpp.

#include "gfx/d3d12_renderer.h"

#include "gfx/d3d12_internal.h"

#include <cassert>
#include <cstdio>
#include <memory>

#include <dxgidebug.h>
#pragma comment(lib, "dxguid.lib")

#include <rex/cvar.h>

// The game render target is cleared to this every frame. Magenta is available
// because the two failure modes a screenshot has to tell apart look identical
// against the dark blue default: "nothing was drawn at all" and "something was
// drawn over the whole target in black". Against magenta they do not.
REXCVAR_DEFINE_BOOL(clear_magenta, false, "Debug",
                    "Clear the game render target to magenta instead of the "
                    "default dark blue, to make undrawn areas obvious");

using mx::gfx::kAdapterNamePrefix;
using mx::gfx::LogError;
using mx::gfx::LogInfo;

namespace {
const float* GameClearColor() {
  static const float kDarkBlue[4] = {0.05f, 0.08f, 0.18f, 1.0f};
  static const float kMagenta[4] = {1.0f, 0.0f, 1.0f, 1.0f};
  static const float* const kChosen =
      REXCVAR_GET(clear_magenta) ? kMagenta : kDarkBlue;
  return kChosen;
}
}  // namespace

D3D12Renderer::~D3D12Renderer() {
  Shutdown();
}

bool D3D12Renderer::Initialize(HWND hwnd) {
  if (m_initialized) {
    return true;
  }

  if (hwnd == nullptr) {
    LogError("Initialize: invalid HWND");
    return false;
  }
  m_hwnd = hwnd;

  RECT clientRect{};
  if (GetClientRect(m_hwnd, &clientRect)) {
    m_width = static_cast<uint32_t>(clientRect.right - clientRect.left);
    m_height = static_cast<uint32_t>(clientRect.bottom - clientRect.top);
  }

#if defined(_DEBUG) && 1
  {
    Microsoft::WRL::ComPtr<ID3D12Debug> debugController;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
      debugController->EnableDebugLayer();
      LogInfo("D3D12 debug layer enabled.");

      Microsoft::WRL::ComPtr<ID3D12Debug1> debugController1;
      if (SUCCEEDED(debugController.As(&debugController1))) {
        debugController1->SetEnableGPUBasedValidation(TRUE);
        LogInfo("GPU-based validation enabled.");
      }
    }

    Microsoft::WRL::ComPtr<IDXGIInfoQueue> dxgiInfoQueue;
    DXGIGetDebugInterface1(0, IID_PPV_ARGS(&dxgiInfoQueue));
    if (dxgiInfoQueue) {
      LogInfo("DXGI debug interface acquired.");
    }
  }
#endif

  if (!CreateFactory()) return false;
  if (!CreateDevice()) return false;
  if (!CreateCommandQueue()) return false;
  if (!CreateSwapChain()) return false;
  if (!CreateRtvDescriptorHeap()) return false;
  if (!CreateRenderTargetViews()) return false;
  if (!CreateCommandAllocator()) return false;
  if (!CreateCommandList()) return false;
  if (!CreateFence()) return false;

  CreateViewportAndScissor();

  if (!CreateVideoPipeline()) {
    LogError("Initialize: CreateVideoPipeline failed");
    return false;
  }

  if (!CreateGamePipeline()) {
    LogError("Initialize: CreateGamePipeline failed");
    return false;
  }

  if (!CreateGameRenderTargets()) {
    LogError("Initialize: CreateGameRenderTargets failed");
    return false;
  }

  m_initialized = true;
  LogInfo("D3D12 renderer initialized successfully.");
  return true;
}

void D3D12Renderer::Shutdown() {
  if (!m_initialized) {
    return;
  }

  WaitForGpu();

  if (m_fenceEvent != nullptr) {
    CloseHandle(m_fenceEvent);
    m_fenceEvent = nullptr;
  }

  m_videoTexture.Reset();
  m_videoUploadBuffer.Reset();
  m_videoSrvHeap.Reset();
  m_videoPipelineState.Reset();
  m_videoRootSignature.Reset();

  for (auto& pso : m_gamePSOs) pso.Reset();
  m_gameRootSig.Reset();
  m_gameVB.Reset();
  m_gameIB.Reset();
  m_gameCB.Reset();
  m_gameCbvHeap.Reset();
  m_gameTextures.clear();
  m_gameRenderTargets.clear();
  m_gameSrvHeap.Reset();
  m_gameSrvDescriptorSize = 0;
  m_nextGameSrvDescriptor = 0;
  m_hasGamePipeline = false;

  m_gameDraws.clear();
  // WaitForGpu above has already drained the queue, so nothing here is still in
  // flight and the retirement list can be dropped outright.
  m_retired.clear();
  m_hasEverDrawnGame = false;

  m_gameRT.Reset();
  m_gameDepth.Reset();
  m_gameRtvHeap.Reset();
  m_gameRtvDescriptorSize = 0;
  m_gameDsvHeap.Reset();

  for (auto& rt : m_renderTargets) {
    rt.Reset();
  }

  m_commandList.Reset();
  for (auto& a : m_commandAllocators) a.Reset();
  m_rtvHeap.Reset();
  m_swapChain.Reset();
  m_commandQueue.Reset();
  m_device.Reset();
  m_factory.Reset();

  m_initialized = false;
  LogInfo("D3D12 renderer shut down.");
}

void D3D12Renderer::BeginFrame() {
  assert(m_initialized);
  assert(m_commandAllocators[m_frameIndex] != nullptr);
  assert(m_commandList != nullptr);

  if (FAILED(m_commandAllocators[m_frameIndex]->Reset())) {
    LogError("BeginFrame: failed to reset command allocator");
    return;
  }
  if (FAILED(m_commandList->Reset(m_commandAllocators[m_frameIndex].Get(), nullptr))) {
    LogError("BeginFrame: failed to reset command list");
    return;
  }

  m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

  D3D12_RESOURCE_BARRIER barrier = {};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
  barrier.Transition.pResource = m_renderTargets[m_frameIndex].Get();
  barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
  barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  m_commandList->ResourceBarrier(1, &barrier);

  D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle;
  if (m_gameRT) {
    rtvHandle = m_gameRtvHeap->GetCPUDescriptorHandleForHeapStart();
    barrier.Transition.pResource = m_gameRT.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    m_commandList->ResourceBarrier(1, &barrier);
  } else {
    rtvHandle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtvHandle.ptr += static_cast<SIZE_T>(m_frameIndex) * m_rtvDescriptorSize;
  }

  D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = {};
  if (m_gameDepth) {
    dsvHandle = m_gameDsvHeap->GetCPUDescriptorHandleForHeapStart();
    // Clear depth each frame: DSV was bound but never cleared, so without
    // this, depth accumulates across frames and real geometry would
    // depth-fail against random initial values.
    m_commandList->ClearDepthStencilView(
        dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    m_commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);
  } else {
    m_commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);
  }
  m_commandList->RSSetViewports(1, &m_viewport);
  m_commandList->RSSetScissorRects(1, &m_scissorRect);

  // Clear the colour target every frame, unconditionally.
  //
  // This call used to sit in the final else of a three-way chain whose middle
  // arm tested `!m_gameDraws.empty() || m_hasGamePipeline`. CreateGamePipeline
  // sets m_hasGamePipeline true and Initialize hard-fails if it does not, so in
  // any renderer that started successfully the middle arm always won and the
  // clear was dead code. m_gameRT had therefore been accumulating every frame
  // ever drawn — which is what the control screenshot was showing when it
  // displayed the placeholder triangle and a guest quad from different frames
  // at the same time. No screenshot before 2026-08-02 showed a single frame.
  //
  // This cannot fight the guest's own clear. That arrives as a RectangleList
  // *draw*, lands in m_gameDraws, and is replayed inside RenderGameFrame — so
  // it runs after this clear, not instead of it.
  // rtvHandle is m_gameRT when it exists and the backbuffer otherwise; both
  // want clearing, so this is not conditioned on which one it is.
  //
  // Two clears, not one. GameClearColor is *our* debug colour, not the guest's,
  // and a single full-window clear painted it into the pillarbox bars as well —
  // so the bars came out dark blue and read as part of the image rather than as
  // dead space beside it. Black bars are the letterbox convention precisely
  // because they cannot be mistaken for content.
  //
  // The scoped clear uses m_scissorRect, which CreateViewportAndScissor already
  // fitted to the guest's 16:9, so the two cannot drift apart.
  static const float kBars[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  m_commandList->ClearRenderTargetView(rtvHandle, kBars, 0, nullptr);
  m_commandList->ClearRenderTargetView(rtvHandle, GameClearColor(), 1,
                                       &m_scissorRect);

  if (m_hasVideoFrame) {
    RenderVideoFrame();
    m_hasVideoFrame = false;
  } else {
    RenderGameFrame();
  }
}

void D3D12Renderer::EndFrame() {
  assert(m_initialized);

  // Only present game frame when Bink video is NOT playing — during Bink we
  // only render the video quad and the game RT may not be in RENDER_TARGET
  // state, so transitioning it would cause D3D12 device removal.
  if (!m_hasVideoFrame) {
    PresentGameFrame();
  }

  D3D12_RESOURCE_BARRIER barrier = {};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
  barrier.Transition.pResource = m_renderTargets[m_frameIndex].Get();
  barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
  barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  m_commandList->ResourceBarrier(1, &barrier);

  HRESULT hr = m_commandList->Close();
  if (FAILED(hr)) {
    LogError("EndFrame: Close failed");
    return;
  }

  ID3D12CommandList* lists[] = {m_commandList.Get()};
  m_commandQueue->ExecuteCommandLists(1, lists);

  hr = m_swapChain->Present(1, 0);
  if (FAILED(hr)) {
    char buf[128] = {};
    snprintf(buf, sizeof(buf), "EndFrame: Present failed HR=0x%08lX", hr);
    LogError(buf);
    HRESULT reason = m_device->GetDeviceRemovedReason();
    char rbuf[128] = {};
    snprintf(rbuf, sizeof(rbuf), "EndFrame: DeviceRemovedReason HR=0x%08lX", reason);
    LogError(rbuf);
  }

  MoveToNextFrame();
}

bool D3D12Renderer::CreateFactory() {
  UINT factoryFlags = 0;

  HRESULT hr = CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&m_factory));
  if (FAILED(hr)) {
    LogError("CreateFactory: CreateDXGIFactory2 failed");
    return false;
  }
  return true;
}

bool D3D12Renderer::CreateDevice() {
  Microsoft::WRL::ComPtr<IDXGIAdapter1> selectedAdapter;
  Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
  SIZE_T maxDedicatedVideoMemory = 0;

  for (UINT i = 0;
       m_factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
    DXGI_ADAPTER_DESC1 desc{};
    if (FAILED(adapter->GetDesc1(&desc))) {
      continue;
    }
    if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
      continue;
    }

    wchar_t nameLower[128] = {};
    for (size_t c = 0; c < 127 && desc.Description[c] != L'\0'; ++c) {
      nameLower[c] = towlower(desc.Description[c]);
    }

    if (wcsstr(nameLower, kAdapterNamePrefix) != nullptr) {
      if (desc.DedicatedVideoMemory > maxDedicatedVideoMemory) {
        maxDedicatedVideoMemory = desc.DedicatedVideoMemory;
        selectedAdapter = adapter;
      }
    }

    if (selectedAdapter == nullptr) {
      if (desc.DedicatedVideoMemory > maxDedicatedVideoMemory) {
        maxDedicatedVideoMemory = desc.DedicatedVideoMemory;
        selectedAdapter = adapter;
      }
    }
  }

  if (selectedAdapter == nullptr) {
    LogError("CreateDevice: no suitable adapter found");
    return false;
  }

  IDXGIAdapter* rawAdapter = selectedAdapter.Get();

  HRESULT hr = D3D12CreateDevice(rawAdapter, D3D_FEATURE_LEVEL_12_0,
                                  IID_PPV_ARGS(&m_device));
  if (FAILED(hr)) {
    hr = D3D12CreateDevice(rawAdapter, D3D_FEATURE_LEVEL_11_0,
                           IID_PPV_ARGS(&m_device));
    if (FAILED(hr)) {
      LogError("CreateDevice: D3D12CreateDevice failed");
      return false;
    }
    LogInfo("Created D3D12 device at feature level 11.0");
  } else {
    LogInfo("Created D3D12 device at feature level 12.0");
  }

  return true;
}

bool D3D12Renderer::CreateCommandQueue() {
  D3D12_COMMAND_QUEUE_DESC desc = {};
  desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
  desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
  desc.NodeMask = 0;

  HRESULT hr = m_device->CreateCommandQueue(&desc, IID_PPV_ARGS(&m_commandQueue));
  if (FAILED(hr)) {
    LogError("CreateCommandQueue: failed");
    return false;
  }
  return true;
}

bool D3D12Renderer::CreateSwapChain() {
  Microsoft::WRL::ComPtr<IDXGISwapChain1> swapChain1;

  {
    BOOL allowTearing = FALSE;
    if (SUCCEEDED(m_factory->CheckFeatureSupport(
            DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowTearing,
            sizeof(allowTearing)))) {
      m_allowTearing = (allowTearing == TRUE);
    }
  }

  DXGI_SWAP_CHAIN_DESC1 desc = {};
  desc.Width = m_width;
  desc.Height = m_height;
  desc.Format = kBackBufferFormat;
  desc.Stereo = FALSE;
  desc.SampleDesc.Count = 1;
  desc.SampleDesc.Quality = 0;
  desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  desc.BufferCount = kFrameCount;
  desc.Scaling = DXGI_SCALING_STRETCH;
  desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  desc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
  desc.Flags = m_allowTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u;

  HRESULT hr = m_factory->CreateSwapChainForHwnd(
      m_commandQueue.Get(), m_hwnd, &desc, nullptr, nullptr, &swapChain1);
  if (FAILED(hr)) {
    LogError("CreateSwapChain: CreateSwapChainForHwnd failed");
    return false;
  }

  hr = swapChain1.As(&m_swapChain);
  if (FAILED(hr)) {
    LogError("CreateSwapChain: QueryInterface(IDXGISwapChain3) failed");
    return false;
  }

  m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

  LogInfo("Swap chain created.");
  return true;
}

bool D3D12Renderer::CreateRtvDescriptorHeap() {
  D3D12_DESCRIPTOR_HEAP_DESC desc = {};
  desc.NumDescriptors = kFrameCount;
  desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
  desc.NodeMask = 0;

  HRESULT hr = m_device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_rtvHeap));
  if (FAILED(hr)) {
    LogError("CreateRtvDescriptorHeap: failed");
    return false;
  }

  m_rtvDescriptorSize =
      m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

  return true;
}

bool D3D12Renderer::CreateRenderTargetViews() {
  D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle =
      m_rtvHeap->GetCPUDescriptorHandleForHeapStart();

  for (uint32_t i = 0; i < kFrameCount; ++i) {
    HRESULT hr =
        m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_renderTargets[i]));
    if (FAILED(hr)) {
      LogError("CreateRenderTargetViews: GetBuffer failed");
      return false;
    }
    m_device->CreateRenderTargetView(m_renderTargets[i].Get(), nullptr,
                                     rtvHandle);
    rtvHandle.ptr += static_cast<SIZE_T>(m_rtvDescriptorSize);
  }

  return true;
}

bool D3D12Renderer::CreateCommandAllocator() {
  for (uint32_t i = 0; i < kFrameCount; ++i) {
    HRESULT hr = m_device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_commandAllocators[i]));
    if (FAILED(hr)) {
      LogError("CreateCommandAllocator: failed");
      return false;
    }
  }
  m_frameIndex = 0;
  return true;
}

bool D3D12Renderer::CreateCommandList() {
  HRESULT hr = m_device->CreateCommandList(
      0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_commandAllocators[0].Get(), nullptr,
      IID_PPV_ARGS(&m_commandList));
  if (FAILED(hr)) {
    LogError("CreateCommandList: failed");
    return false;
  }

  m_commandList->Close();
  return true;
}

bool D3D12Renderer::CreateFence() {
  HRESULT hr = m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                      IID_PPV_ARGS(&m_fence));
  if (FAILED(hr)) {
    LogError("CreateFence: failed");
    return false;
  }

  for (uint32_t i = 0; i < kFrameCount; ++i) {
    m_fenceValues[i] = 0;
  }

  m_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (m_fenceEvent == nullptr) {
    LogError("CreateFence: CreateEventW failed");
    return false;
  }

  return true;
}

void D3D12Renderer::CreateViewportAndScissor() {
  // Pillarbox to the guest's aspect rather than stretching to the window's.
  //
  // The guest renders 1280x720 — its own viewport registers read xs=640,
  // ys=-360, and it never learns the host window size because none of
  // VdQueryVideoMode, XGetVideoMode or VdGetCurrentDisplayInformation is
  // hooked. The transcode then normalises to clip [-1,1], which carries no
  // aspect information at all. So stretching clip space across the full client
  // area scales x and y by different factors: on a 3440x1440 window that is
  // 2.389 against the guest's 1.778, and everything drawn came out 1.34x too
  // wide. Every screenshot taken before this was distorted.
  //
  // The render target stays window-sized on purpose. PresentGameFrame copies it
  // to the backbuffer with CopyTextureRegion, which requires matching
  // dimensions — fitting the image is the viewport's job, not the resource's.
  const float win_w = static_cast<float>(m_width);
  const float win_h = static_cast<float>(m_height);
  float draw_w = win_w;
  float draw_h = win_h;
  if (win_w > 0.0f && win_h > 0.0f) {
    if (win_w / win_h > kGuestAspect) {
      draw_w = win_h * kGuestAspect;   // window wider than 16:9 — bars at the sides
    } else {
      draw_h = win_w / kGuestAspect;   // taller — bars top and bottom
    }
  }
  const float off_x = (win_w - draw_w) * 0.5f;
  const float off_y = (win_h - draw_h) * 0.5f;

  m_viewport.TopLeftX = off_x;
  m_viewport.TopLeftY = off_y;
  m_viewport.Width = draw_w;
  m_viewport.Height = draw_h;
  m_viewport.MinDepth = 0.0f;
  m_viewport.MaxDepth = 1.0f;

  // The scissor has to match, or the bars keep whatever the clear left and any
  // geometry that escapes clip space paints into them.
  m_scissorRect.left = static_cast<LONG>(off_x);
  m_scissorRect.top = static_cast<LONG>(off_y);
  m_scissorRect.right = static_cast<LONG>(off_x + draw_w);
  m_scissorRect.bottom = static_cast<LONG>(off_y + draw_h);

  REXLOG_INFO("renderer: client {}x{} (aspect {:.3f}) -> 16:9 drawn region "
              "{}x{} at ({},{})",
              m_width, m_height, win_h > 0.0f ? win_w / win_h : 0.0f,
              LONG(draw_w), LONG(draw_h), LONG(off_x), LONG(off_y));
}

void D3D12Renderer::WaitForGpu() {
  if (m_commandQueue == nullptr || m_fence == nullptr) {
    return;
  }

  if (FAILED(m_commandQueue->Signal(m_fence.Get(), ++m_fenceValue))) {
    return;
  }

  if (m_fence->GetCompletedValue() < m_fenceValue) {
    if (FAILED(m_fence->SetEventOnCompletion(m_fenceValue, m_fenceEvent))) {
      return;
    }
    WaitForSingleObject(m_fenceEvent, INFINITE);
  }
}

void D3D12Renderer::MoveToNextFrame() {
  const uint64_t currentFenceValue = ++m_fenceValue;
  m_fenceValues[m_frameIndex] = currentFenceValue;

  if (FAILED(m_commandQueue->Signal(m_fence.Get(), currentFenceValue))) {
    LogError("MoveToNextFrame: Signal failed");
    return;
  }

  m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

  if (m_fence->GetCompletedValue() < m_fenceValues[m_frameIndex]) {
    if (FAILED(m_fence->SetEventOnCompletion(m_fenceValues[m_frameIndex],
                                              m_fenceEvent))) {
      LogError("MoveToNextFrame: SetEventOnCompletion failed");
      return;
    }
    WaitForSingleObject(m_fenceEvent, INFINITE);
  }

  // Release the per-draw upload buffers the GPU has now finished with. Done
  // here rather than in ClearGameDraws because this is the only place that
  // knows the fence has moved.
  DrainRetired();
}
