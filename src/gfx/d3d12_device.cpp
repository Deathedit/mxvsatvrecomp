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

// clear_magenta and native_res_viewport were bring-up cvars and are gone
// (2026-08-07). The magenta clear existed to tell "nothing was drawn at all"
// apart from "something was drawn over the whole target in black", which look
// identical against the dark blue; nobody ran it, and the two clears below make
// the same distinction by keeping the bars black. native_res_viewport was on by
// default throughout, so the 1:1 path it selected is simply what this does now.

using mx::gfx::kAdapterNamePrefix;
using mx::gfx::LogError;
using mx::gfx::LogInfo;

namespace {
// Ours, not the guest's — see the two clears in BeginFrame.
const float* GameClearColor() {
  static const float kDarkBlue[4] = {0.05f, 0.08f, 0.18f, 1.0f};
  return kDarkBlue;
}

// Scoped microsecond timer for the render-thread phase breakdown. Accumulates
// rather than assigns, because a phase can be entered more than once in a tick.
struct PhaseTimer {
  uint64_t& sink;
  std::chrono::steady_clock::time_point t0{std::chrono::steady_clock::now()};
  explicit PhaseTimer(uint64_t& s) : sink(s) {}
  ~PhaseTimer() {
    sink += uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                         std::chrono::steady_clock::now() - t0)
                         .count());
  }
};

// The subset of D3D12_AUTO_BREADCRUMB_OP this renderer can actually emit.
// Anything else prints as its number rather than a wrong name.
const char* BreadcrumbOpName(D3D12_AUTO_BREADCRUMB_OP op) {
  switch (op) {
    case D3D12_AUTO_BREADCRUMB_OP_SETMARKER: return "SetMarker";
    case D3D12_AUTO_BREADCRUMB_OP_BEGINEVENT: return "BeginEvent";
    case D3D12_AUTO_BREADCRUMB_OP_ENDEVENT: return "EndEvent";
    case D3D12_AUTO_BREADCRUMB_OP_DRAWINSTANCED: return "DrawInstanced";
    case D3D12_AUTO_BREADCRUMB_OP_DRAWINDEXEDINSTANCED:
      return "DrawIndexedInstanced";
    case D3D12_AUTO_BREADCRUMB_OP_EXECUTEINDIRECT: return "ExecuteIndirect";
    case D3D12_AUTO_BREADCRUMB_OP_DISPATCH: return "Dispatch";
    case D3D12_AUTO_BREADCRUMB_OP_COPYBUFFERREGION: return "CopyBufferRegion";
    case D3D12_AUTO_BREADCRUMB_OP_COPYTEXTUREREGION: return "CopyTextureRegion";
    case D3D12_AUTO_BREADCRUMB_OP_COPYRESOURCE: return "CopyResource";
    case D3D12_AUTO_BREADCRUMB_OP_RESOLVESUBRESOURCE: return "ResolveSubresource";
    case D3D12_AUTO_BREADCRUMB_OP_CLEARRENDERTARGETVIEW:
      return "ClearRenderTargetView";
    case D3D12_AUTO_BREADCRUMB_OP_CLEARDEPTHSTENCILVIEW:
      return "ClearDepthStencilView";
    case D3D12_AUTO_BREADCRUMB_OP_RESOURCEBARRIER: return "ResourceBarrier";
    case D3D12_AUTO_BREADCRUMB_OP_PRESENT: return "Present";
    default: return nullptr;
  }
}

// Everything DRED knows, written out at the moment of removal.
//
// The breadcrumb count is what the GPU *finished*; the op at that index is the
// one it was executing when it died, which is the single most useful fact
// available. Only nodes that did not finish are printed -- a completed list
// says nothing about the fault, and a level's worth of them would bury the one
// that matters.
void ReportDred(ID3D12Device* device) {
  if (!device) return;
  Microsoft::WRL::ComPtr<ID3D12DeviceRemovedExtendedData> dred;
  if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dred)))) {
    LogError("DRED: not available (was it enabled before device creation?)");
    return;
  }

  char buf[256] = {};
  D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT crumbs = {};
  if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&crumbs))) {
    uint32_t nodes = 0;
    for (const D3D12_AUTO_BREADCRUMB_NODE* n = crumbs.pHeadAutoBreadcrumbNode;
         n; n = n->pNext) {
      const uint32_t done = n->pLastBreadcrumbValue ? *n->pLastBreadcrumbValue : 0;
      if (done == n->BreadcrumbCount) continue;  // this list completed
      ++nodes;
      std::snprintf(buf, sizeof(buf),
                    "DRED: list '%ls' queue '%ls' completed %u of %u ops",
                    n->pCommandListDebugNameW ? n->pCommandListDebugNameW : L"?",
                    n->pCommandQueueDebugNameW ? n->pCommandQueueDebugNameW : L"?",
                    done, n->BreadcrumbCount);
      LogError(buf);
      // The failing op, plus a little history either side of it.
      const uint32_t first = done > 4 ? done - 4 : 0;
      const uint32_t last =
          done + 4 < n->BreadcrumbCount ? done + 4 : n->BreadcrumbCount;
      for (uint32_t i = first; i < last; ++i) {
        const D3D12_AUTO_BREADCRUMB_OP op = n->pCommandHistory[i];
        const char* name = BreadcrumbOpName(op);
        if (name)
          std::snprintf(buf, sizeof(buf), "DRED:   [%u]%s %s", i,
                        i == done ? " <-- FAULTED HERE" : "", name);
        else
          std::snprintf(buf, sizeof(buf), "DRED:   [%u]%s op %u", i,
                        i == done ? " <-- FAULTED HERE" : "", uint32_t(op));
        LogError(buf);
      }
    }
    if (!nodes) LogError("DRED: every command list completed; no faulting op");
  }

  D3D12_DRED_PAGE_FAULT_OUTPUT fault = {};
  if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&fault)) &&
      fault.PageFaultVA) {
    std::snprintf(buf, sizeof(buf), "DRED: page fault at VA 0x%llX",
                  static_cast<unsigned long long>(fault.PageFaultVA));
    LogError(buf);
    // Existing allocations are live objects the fault landed in; recent freed
    // ones are use-after-free candidates, which is the likelier shape here
    // given the ring allocator and per-draw upload buffers.
    for (const D3D12_DRED_ALLOCATION_NODE* a = fault.pHeadExistingAllocationNode;
         a; a = a->pNext) {
      std::snprintf(buf, sizeof(buf), "DRED:   live allocation '%ls' type %u",
                    a->ObjectNameW ? a->ObjectNameW : L"?",
                    uint32_t(a->AllocationType));
      LogError(buf);
    }
    for (const D3D12_DRED_ALLOCATION_NODE* a =
             fault.pHeadRecentFreedAllocationNode;
         a; a = a->pNext) {
      std::snprintf(buf, sizeof(buf), "DRED:   RECENTLY FREED '%ls' type %u",
                    a->ObjectNameW ? a->ObjectNameW : L"?",
                    uint32_t(a->AllocationType));
      LogError(buf);
    }
  }
}
}  // namespace

HRESULT D3D12Renderer::CreateTimedCommittedResource(
    const D3D12_HEAP_PROPERTIES* heap, D3D12_HEAP_FLAGS flags,
    const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES state,
    const D3D12_CLEAR_VALUE* clear, REFIID riid, void** out) {
  const auto t0 = std::chrono::steady_clock::now();
  const HRESULT hr = m_device->CreateCommittedResource(heap, flags, desc, state,
                                                       clear, riid, out);
  m_committedUs += uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now() - t0)
                                .count());
  ++m_committedCalls;
  return hr;
}

void D3D12Renderer::ReportAddGameDrawsCost(uint64_t microseconds,
                                           uint32_t draws) {
  m_phaseAddDrawsUs += microseconds;
  m_phaseAddDraws += draws;
}

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

  // The debug layer used to be reachable only in a _DEBUG build, and a debug
  // build of this emulator is too slow to reach the menu — so the one tool that
  // names an invalid-work bug exactly was unavailable for the only scenes where
  // such bugs appear. MX_D3D12_DEBUG=1 turns it on in a release build;
  // MX_D3D12_DEBUG=2 adds GPU-based validation, which is far slower again but
  // catches descriptor and resource-state errors the basic layer misses.
  uint32_t debugLevel = 0;
#if defined(_DEBUG)
  debugLevel = 2;
#endif
  {
    char value[8] = {};
    size_t len = 0;
    if (getenv_s(&len, value, sizeof(value), "MX_D3D12_DEBUG") == 0 && len)
      debugLevel = uint32_t(std::atoi(value));
  }
  if (debugLevel) {
    Microsoft::WRL::ComPtr<ID3D12Debug> debugController;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
      debugController->EnableDebugLayer();
      LogInfo("D3D12 debug layer enabled.");

      Microsoft::WRL::ComPtr<ID3D12Debug1> debugController1;
      if (debugLevel >= 2 && SUCCEEDED(debugController.As(&debugController1))) {
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

  // DRED — Device Removed Extended Data.
  //
  // GetDeviceRemovedReason only ever answers DEVICE_HUNG, which names a symptom
  // and nothing else. Three theories were argued from that one word and two of
  // them were wrong: a long frame crossing the TDR threshold (disproved -- runs
  // survived 3.7s frames and hung on a 0.77s one) and a driver left unstable by
  // an earlier reset (disproved -- Windows logged no display reset at all).
  //
  // DRED records the breadcrumb trail of commands the GPU actually completed,
  // so the first UNFINISHED op is the one that faulted, plus the faulting
  // address and which allocation owned it. That is the difference between
  // reading the fault and guessing at it.
  //
  // MUST be set before device creation: afterwards the settings object still
  // hands back S_OK and changes nothing.
  //
  // On by default. Auto-breadcrumbs cost a small write per command-list op,
  // and the bug being chased appears only after minutes of play -- an
  // instrument that has to be armed in advance is one that will not be armed
  // when it matters. MX_D3D12_DRED=0 turns it off for timing runs.
  {
    bool dred = true;
    char value[8] = {};
    size_t len = 0;
    if (getenv_s(&len, value, sizeof(value), "MX_D3D12_DRED") == 0 && len)
      dred = std::atoi(value) != 0;
    if (dred) {
      Microsoft::WRL::ComPtr<ID3D12DeviceRemovedExtendedDataSettings> dredSettings;
      const HRESULT dhr = D3D12GetDebugInterface(IID_PPV_ARGS(&dredSettings));
      if (SUCCEEDED(dhr)) {
        dredSettings->SetAutoBreadcrumbsEnablement(
            D3D12_DRED_ENABLEMENT_FORCED_ON);
        dredSettings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        LogInfo("DRED enabled (auto-breadcrumbs + page fault).");
      } else {
        // Logged loudly rather than skipped. The first build of this reported
        // only on success, so a failure looked exactly like a run where the
        // fault never happened -- an instrument that cannot say "I am not
        // armed" is worse than none. 0x887E0003 is DXGI_ERROR_SDK_COMPONENT_
        // MISSING: D3D12GetDebugInterface needs the "Graphics Tools" optional
        // Windows feature, which is not installed by default.
        char buf[160] = {};
        std::snprintf(buf, sizeof(buf),
                      "DRED NOT ENABLED: D3D12GetDebugInterface HR=0x%08lX%s",
                      dhr,
                      dhr == 0x887E0003
                          ? " (install the Graphics Tools optional feature)"
                          : "");
        LogError(buf);
      }
    }
  }

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


  for (auto& pso : m_gamePSOs) pso.Reset();
  m_gameRootSig.Reset();
  m_gameCB.Reset();
  m_gameTextures.clear();
  m_gameRenderTargets.clear();
  m_gameSrvHeap.Reset();
  m_gameSrvDescriptorSize = 0;
  m_nextGameSrvDescriptor = 0;
  // With the bump allocator. Slots recycled from evicted textures index into
  // the heap being released here, so keeping them would hand out descriptors
  // into freed storage on the next device.
  m_freeGameSrvDescriptors.clear();
  m_hasGamePipeline = false;

  m_gameDraws.clear();
  // WaitForGpu above has already drained the queue, so nothing here is still in
  // flight and the retirement list can be dropped outright. The upload ring goes
  // with it — the pages are still mapped, which is fine to release from.
  m_retired.clear();
  m_uploadPages.clear();
  m_uploadPage = UINT32_MAX;

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

  HRESULT reset_hr = m_commandAllocators[m_frameIndex]->Reset();
  if (FAILED(reset_hr)) {
    char buf[160] = {};
    std::snprintf(buf, sizeof(buf),
                  "BeginFrame: failed to reset command allocator HR=0x%08lX",
                  reset_hr);
    LogError(buf);
    return;
  }
  reset_hr = m_commandList->Reset(m_commandAllocators[m_frameIndex].Get(),
                                  nullptr);
  if (FAILED(reset_hr)) {
    // Logged with its HRESULT because the bare message was unreadable: this
    // fires every frame once EndFrame's Close has failed, and without the code
    // there is no way to tell "the list is still open" from a device removal.
    char buf[160] = {};
    std::snprintf(buf, sizeof(buf),
                  "BeginFrame: failed to reset command list HR=0x%08lX "
                  "removed=0x%08lX",
                  reset_hr, m_device->GetDeviceRemovedReason());
    LogError(buf);
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
  // displayed the startup placeholder triangle (since removed) and a guest quad
  // from different frames at the same time. No screenshot before 2026-08-02
  // showed a single frame.
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

  {
    PhaseTimer _record{m_phaseRecordUs};
    RenderGameFrame();
  }
}

void D3D12Renderer::EndFrame() {
  assert(m_initialized);

  // Opened here and closed before the report below, so the report sees the
  // finished figure. Everything from the present blit to Present() itself is one
  // phase: it is all "hand this frame to the GPU", and splitting it further is
  // only worth doing if it turns out to be the phase that grows.
  auto submit = std::make_unique<PhaseTimer>(m_phaseSubmitUs);

  PresentGameFrame();

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
    // This used to return here and nothing else, which killed the renderer for
    // the rest of the run. A failed Close leaves the list OPEN, so every later
    // BeginFrame failed to reset it; and because MoveToNextFrame never ran, the
    // fence never advanced, so DrainRetired released nothing and the per-draw
    // upload buffers accumulated without bound.
    //
    // Measured in mx_881: one Close failure at frame 43, then 521 consecutive
    // dead frames at 4-5 SECONDS of wall clock each. That is what 0.40 fps in
    // the menu actually is -- not the vertex path, which by then was not
    // rendering at all.
    char buf[192] = {};
    std::snprintf(buf, sizeof(buf),
                  "EndFrame: Close failed HR=0x%08lX removed=0x%08lX", hr,
                  m_device->GetDeviceRemovedReason());
    LogError(buf);
    // Drained HERE rather than only after ExecuteCommandLists below: the
    // validation message that REJECTED this Close is queued before it, so on
    // the old ordering the one line naming the cause was never printed.
    DrainD3D12Messages();
    RecoverCommandList();
    submit.reset();
    ReportTickPhases();
    return;
  }

  ID3D12CommandList* lists[] = {m_commandList.Get()};
  m_commandQueue->ExecuteCommandLists(1, lists);

  // Before Present, so the messages for the work just recorded appear BEFORE
  // the device-removal line rather than after it. Ordering is the whole value:
  // the last validation error before the first hang is the one that caused it.
  DrainD3D12Messages();

  hr = m_swapChain->Present(1, 0);
  if (FAILED(hr)) {
    char buf[128] = {};
    snprintf(buf, sizeof(buf), "EndFrame: Present failed HR=0x%08lX", hr);
    LogError(buf);
    HRESULT reason = m_device->GetDeviceRemovedReason();
    char rbuf[128] = {};
    snprintf(rbuf, sizeof(rbuf), "EndFrame: DeviceRemovedReason HR=0x%08lX", reason);
    LogError(rbuf);
    // Once. A removed device fails Present every frame afterwards, and the
    // breadcrumb trail never changes -- dumping it per frame would bury the
    // one copy that matters under thousands of identical ones, which is
    // exactly how the dead-renderer logs already read.
    static bool s_dredReported = false;
    if (!s_dredReported) {
      s_dredReported = true;
      ReportDred(m_device.Get());
    }
  }

  submit.reset();
  MoveToNextFrame();
  ReportTickPhases();
}

// One line per 20 ticks, matching the cadence of the routing counters so the two
// can be read side by side.
//
// Sampled rather than averaged: these are the phases of THIS tick, not a mean
// over twenty. The question is how the split changes as the session goes on, and
// a mean over a window that spans a scene change answers it for neither scene.
void D3D12Renderer::ReportTickPhases() {
  const auto now = std::chrono::steady_clock::now();
  uint64_t tickUs = 0;
  if (m_tickEnd.time_since_epoch().count() != 0)
    tickUs = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                          now - m_tickEnd)
                          .count());
  m_tickEnd = now;

  static uint32_t s_tick = 0;
  if ((++s_tick % 20) == 1) {
    // The four phases plus the render thread's 16ms sleep account for the tick
    // period; anything left over is unmeasured and the residual says how much.
    const uint64_t measured = m_phaseAddDrawsUs + m_phaseRecordUs +
                              m_phaseSubmitUs + m_phaseFenceWaitUs +
                              m_phaseRetireUs;
    char message[512];
    std::snprintf(
        message, sizeof(message),
        "render tick #%u %llums = add-draws %llums (%u calls) + record %llums + "
        "submit %llums + FENCE-WAIT %llums + retire %llums, unmeasured %lldms; "
        "upload ring %zu pages, %llu KB this frame; "
        "CreateCommittedResource %llu calls %llums (mean %.1fus/call)",
        s_tick, static_cast<unsigned long long>(tickUs / 1000),
        static_cast<unsigned long long>(m_phaseAddDrawsUs / 1000),
        m_phaseAddDraws,
        static_cast<unsigned long long>(m_phaseRecordUs / 1000),
        static_cast<unsigned long long>(m_phaseSubmitUs / 1000),
        static_cast<unsigned long long>(m_phaseFenceWaitUs / 1000),
        static_cast<unsigned long long>(m_phaseRetireUs / 1000),
        static_cast<long long>(int64_t(tickUs) - int64_t(measured)) / 1000,
        m_uploadPages.size(),
        static_cast<unsigned long long>(m_uploadBytesThisFrame / 1024),
        static_cast<unsigned long long>(m_committedCalls),
        static_cast<unsigned long long>(m_committedUs / 1000),
        m_committedCalls ? double(m_committedUs) / double(m_committedCalls)
                         : 0.0);
    LogInfo(message);
  }

  m_phaseAddDrawsUs = 0;
  m_phaseAddDraws = 0;
  m_phaseRecordUs = 0;
  m_phaseSubmitUs = 0;
  m_phaseFenceWaitUs = 0;
  m_phaseRetireUs = 0;
  m_committedCalls = 0;
  m_committedUs = 0;
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

  // Route the debug layer's messages into OUR log. Without this the layer is
  // enabled and silent from the caller's point of view: its output goes to the
  // debugger via OutputDebugString, and this process is normally run from a
  // shell with no debugger attached. Breaking on corruption and error also
  // stops AT the offending call rather than at the device hang it causes some
  // frames later, which is the difference between a name and a guess.
  Microsoft::WRL::ComPtr<ID3D12InfoQueue> infoQueue;
  if (SUCCEEDED(m_device.As(&infoQueue))) {
    infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, FALSE);
    infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, FALSE);
    m_infoQueue = infoQueue;
    LogInfo("D3D12 info queue attached — validation messages will be logged.");
  }

  return true;
}

// Drain whatever the debug layer has queued since the last call. Cheap when the
// layer is off, because the queue is then never populated.
void D3D12Renderer::DrainD3D12Messages() {
  if (!m_infoQueue) return;
  const UINT64 count = m_infoQueue->GetNumStoredMessages();
  for (UINT64 i = 0; i < count; ++i) {
    SIZE_T bytes = 0;
    if (FAILED(m_infoQueue->GetMessage(i, nullptr, &bytes)) || !bytes) continue;
    std::vector<uint8_t> storage(bytes);
    auto* msg = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
    if (FAILED(m_infoQueue->GetMessage(i, msg, &bytes))) continue;
    // Only the two severities that indicate we did something wrong. Warnings
    // and info are voluminous and would bury them.
    if (msg->Severity != D3D12_MESSAGE_SEVERITY_CORRUPTION &&
        msg->Severity != D3D12_MESSAGE_SEVERITY_ERROR)
      continue;
    char line[1024];
    std::snprintf(line, sizeof(line), "D3D12 %s [id %d]: %.*s",
                  msg->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION
                      ? "CORRUPTION"
                      : "ERROR",
                  int(msg->ID), int(msg->DescriptionByteLength),
                  msg->pDescription);
    LogError(line);
  }
  m_infoQueue->ClearStoredMessages();
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

// Discard the command list and build a new one, after a Close that failed.
//
// D3D12 offers no way to return an indeterminate list to a usable state — the
// runtime's own guidance is to abandon it — so the only recovery is a fresh
// one. The GPU is drained first because we do not know how much of the
// abandoned frame, if any, is still referencing resources; after that wait
// nothing is, which is also the one moment the retirement list can be emptied
// outright.
void D3D12Renderer::RecoverCommandList() {
  WaitForGpu();
  m_commandList.Reset();
  for (auto& a : m_commandAllocators) {
    if (a) a->Reset();
  }
  DrainRetired();
  HRESULT hr = m_device->CreateCommandList(
      0, D3D12_COMMAND_LIST_TYPE_DIRECT,
      m_commandAllocators[m_frameIndex].Get(), nullptr,
      IID_PPV_ARGS(&m_commandList));
  if (FAILED(hr)) {
    char buf[160] = {};
    std::snprintf(buf, sizeof(buf),
                  "EndFrame: could not rebuild the command list HR=0x%08lX — "
                  "the renderer is dead from here",
                  hr);
    LogError(buf);
    return;
  }
  // Left closed, which is the state BeginFrame expects to reset from.
  m_commandList->Close();
  LogError("EndFrame: command list rebuilt after a failed Close");
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
  // The guest's image fills the pillarboxed region at whatever size the window
  // is, rather than being pinned to 1280x720 in the middle of a larger one.
  //
  // It used to be pinned, so that nothing was stretched or resampled and a
  // screenshot pixel mapped to a guest pixel — which was the right trade while
  // every question was "what exact colour did this draw write", and it is worth
  // knowing that trade is now gone: at a larger window a screenshot pixel no
  // longer corresponds to a guest pixel, so pixel-exact work belongs in a
  // capture rather than in a screenshot.
  //
  // This is only the VIEWPORT. Geometry rasterises at the larger size because
  // the main target is window-sized, but every guest-allocated render target,
  // resolve and snapshot is still 1280x720 — so the post chain and anything
  // sampled through it is unchanged. This is a bigger window, not a resolution
  // scale.

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

  // The submission just signalled reads every ring page the draw list touches,
  // so none of them may be reset until this value passes. Restamped every tick
  // rather than once at allocation: an empty tick re-records the same draws, and
  // a page stamped only when it was written would come free while a later
  // submission was still reading it.
  for (auto& p : m_uploadPages)
    if (p.live) p.fence = currentFenceValue;

  m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

  // The one phase that is GPU time rather than CPU time. If the tick's growth
  // lives here the work being submitted is genuinely getting slower to execute;
  // if it does not, the render thread is spending the time itself and the GPU is
  // idle waiting for us.
  {
    PhaseTimer _wait{m_phaseFenceWaitUs};
    if (m_fence->GetCompletedValue() < m_fenceValues[m_frameIndex]) {
      if (FAILED(m_fence->SetEventOnCompletion(m_fenceValues[m_frameIndex],
                                                m_fenceEvent))) {
        LogError("MoveToNextFrame: SetEventOnCompletion failed");
        return;
      }
      WaitForSingleObject(m_fenceEvent, INFINITE);
    }
  }

  // Release the per-draw upload buffers the GPU has now finished with. Done
  // here rather than in ClearGameDraws because this is the only place that
  // knows the fence has moved.
  //
  // Timed because releasing ~4,800 committed resources is the other half of
  // creating them, and a driver allocator that has become slow to allocate is
  // usually slow to free as well.
  PhaseTimer _retire{m_phaseRetireUs};
  DrainRetired();
}
