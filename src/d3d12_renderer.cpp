#include "d3d12_renderer.h"

#include <cassert>
#include <cstdio>
#include <memory>
#include <d3dcompiler.h>
#include <rex/logging.h>

#include <dxgidebug.h>
#pragma comment(lib, "dxguid.lib")

namespace {

constexpr const wchar_t* kAdapterNamePrefix = L"NVIDIA";

void LogError(const char* msg) {
  char buf[512] = {};
  snprintf(buf, sizeof(buf), "[D3D12Renderer] ERROR: %s", msg);
  OutputDebugStringA(buf);
  REXLOG_ERROR("{}", buf);
}

void LogInfo(const char* msg) {
  char buf[512] = {};
  snprintf(buf, sizeof(buf), "[D3D12Renderer] %s", msg);
  OutputDebugStringA(buf);
  REXLOG_INFO("{}", buf);
}

const char* kVertexShader = R"(
struct VSOutput {
  float4 pos : SV_Position;
  float2 uv  : TEXCOORD0;
};
VSOutput main(uint vid : SV_VertexID) {
  VSOutput o;
  o.uv = float2((vid << 1) & 2, vid & 2);
  o.pos = float4(o.uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
  return o;
}
)";

const char* kPixelShader = R"(
Texture2D    g_tex : register(t0);
SamplerState g_smp : register(s0);
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
  return g_tex.Sample(g_smp, uv);
}
)";

const char* kGameVertexShader = R"(
struct VSInput {
  float3 pos : POSITION;
  float4 col : COLOR;
};
struct VSOutput {
  float4 pos : SV_POSITION;
  float4 col : COLOR;
};
cbuffer GameCB : register(b0) {
  float4x4 mvp;
};
VSOutput main(VSInput input) {
  VSOutput o;
  o.pos = mul(mvp, float4(input.pos, 1.0));
  o.col = input.col;
  return o;
}
)";

const char* kGamePixelShader = R"(
float4 main(float4 pos : SV_POSITION, float4 col : COLOR) : SV_TARGET {
  return col;
}
)";

Microsoft::WRL::ComPtr<ID3DBlob> CompileShader(const char* source,
                                                const char* target,
                                                const char* entry) {
  Microsoft::WRL::ComPtr<ID3DBlob> blob;
  Microsoft::WRL::ComPtr<ID3DBlob> errors;
  HRESULT hr = D3DCompile(source, strlen(source), nullptr, nullptr, nullptr,
                          entry, target, 0, 0, &blob, &errors);
  if (FAILED(hr)) {
    if (errors) {
      LogError(static_cast<const char*>(errors->GetBufferPointer()));
    }
    return nullptr;
  }
  return blob;
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

  m_gamePSO.Reset();
  m_gameRootSig.Reset();
  m_gameVB.Reset();
  m_gameIB.Reset();
  m_gameCB.Reset();
  m_gameCbvHeap.Reset();
  m_hasGamePipeline = false;

  m_gameDrawVB.Reset();
  m_gameDrawIB.Reset();
  m_hasGameDrawData = false;

  m_gameRT.Reset();
  m_gameDepth.Reset();
  m_gameRtvHeap.Reset();
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

  if (m_hasVideoFrame) {
    RenderVideoFrame();
    m_hasVideoFrame = false;
  } else if (m_hasGameDrawData || m_hasGamePipeline) {
    RenderGameFrame();
  } else {
    const float clearColor[4] = {0.05f, 0.08f, 0.18f, 1.0f};
    m_commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
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

void D3D12Renderer::UploadVideoFrame(const uint8_t* rgba, uint32_t width,
                                        uint32_t height) {
  D3D12_RESOURCE_DESC texDesc = {};
  texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  texDesc.Width = width;
  texDesc.Height = height;
  texDesc.DepthOrArraySize = 1;
  texDesc.MipLevels = 1;
  texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  texDesc.SampleDesc.Count = 1;
  texDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

  if (!m_videoTexture || width != m_videoWidth || height != m_videoHeight) {
    m_videoTexture.Reset();
    m_videoWidth = width;
    m_videoHeight = height;
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    if (FAILED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &texDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_videoTexture)))) {
      m_videoTexture.Reset(); return;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    m_device->CreateShaderResourceView(m_videoTexture.Get(), &srv,
        m_videoSrvHeap->GetCPUDescriptorHandleForHeapStart());
  }
  D3D12_RESOURCE_DESC uDesc = {};
  uDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  uDesc.Width = ((width * 4 + 255) & ~255) * height;
  uDesc.Height = 1; uDesc.DepthOrArraySize = 1; uDesc.MipLevels = 1;
  uDesc.Format = DXGI_FORMAT_UNKNOWN;
  uDesc.SampleDesc.Count = 1;
  uDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  D3D12_HEAP_PROPERTIES uh = {};
  uh.Type = D3D12_HEAP_TYPE_UPLOAD;
  m_videoUploadBuffer.Reset();
  if (FAILED(m_device->CreateCommittedResource(&uh, D3D12_HEAP_FLAG_NONE, &uDesc,
      D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_videoUploadBuffer)))) return;
  void* m = nullptr;
  if (FAILED(m_videoUploadBuffer->Map(0, nullptr, &m))) return;
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
  UINT nr = 0; UINT64 rs = 0;
  m_device->GetCopyableFootprints(&texDesc, 0, 1, 0, &fp, &nr, &rs, nullptr);
  for (UINT row = 0; row < height; ++row)
    memcpy((uint8_t*)m + row * fp.Footprint.RowPitch, rgba + row * width * 4, width * 4);
  m_videoUploadBuffer->Unmap(0, nullptr);
  m_hasVideoFrame = true;
}

void D3D12Renderer::RenderVideoFrame() {
  D3D12_RESOURCE_BARRIER barrier = {};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

  D3D12_TEXTURE_COPY_LOCATION dst = {};
  dst.pResource = m_videoTexture.Get();
  dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  dst.SubresourceIndex = 0;

  D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
  UINT nr = 0;
  UINT64 rs = 0;
  UINT64 ts = 0;
  D3D12_RESOURCE_DESC td = m_videoTexture->GetDesc();
  m_device->GetCopyableFootprints(&td, 0, 1, 0, &fp, &nr, &rs, &ts);

  D3D12_TEXTURE_COPY_LOCATION src = {};
  src.pResource = m_videoUploadBuffer.Get();
  src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  src.PlacedFootprint = fp;

  m_commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

  barrier.Transition.pResource = m_videoTexture.Get();
  barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
  barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  m_commandList->ResourceBarrier(1, &barrier);

  m_commandList->SetGraphicsRootSignature(m_videoRootSignature.Get());
  m_commandList->SetPipelineState(m_videoPipelineState.Get());

  ID3D12DescriptorHeap* heaps[] = {m_videoSrvHeap.Get()};
  m_commandList->SetDescriptorHeaps(1, heaps);
  m_commandList->SetGraphicsRootDescriptorTable(
      0, m_videoSrvHeap->GetGPUDescriptorHandleForHeapStart());

  m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  m_commandList->DrawInstanced(3, 1, 0, 0);

  barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
  m_commandList->ResourceBarrier(1, &barrier);
}

bool D3D12Renderer::CreateVideoPipeline() {
  auto vsBlob = CompileShader(kVertexShader, "vs_5_0", "main");
  auto psBlob = CompileShader(kPixelShader, "ps_5_0", "main");
  if (!vsBlob || !psBlob) {
    LogError("CreateVideoPipeline: shader compilation failed");
    return false;
  }
  LogInfo("CreateVideoPipeline: shaders compiled");

  D3D12_DESCRIPTOR_RANGE range = {};
  range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  range.NumDescriptors = 1;
  range.BaseShaderRegister = 0;

  D3D12_ROOT_PARAMETER rootParam = {};
  rootParam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  rootParam.DescriptorTable.NumDescriptorRanges = 1;
  rootParam.DescriptorTable.pDescriptorRanges = &range;
  rootParam.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

  D3D12_STATIC_SAMPLER_DESC sampler = {};
  sampler.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
  sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

  D3D12_ROOT_SIGNATURE_DESC rootDesc = {};
  rootDesc.NumParameters = 1;
  rootDesc.pParameters = &rootParam;
  rootDesc.NumStaticSamplers = 1;
  rootDesc.pStaticSamplers = &sampler;
  rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

  Microsoft::WRL::ComPtr<ID3DBlob> sigBlob;
  Microsoft::WRL::ComPtr<ID3DBlob> errBlob;
  HRESULT hr = D3D12SerializeRootSignature(
      &rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &errBlob);
  if (FAILED(hr)) {
    if (errBlob) {
      LogError(static_cast<const char*>(errBlob->GetBufferPointer()));
    }
    LogError("CreateVideoPipeline: root signature serialization failed");
    return false;
  }

  hr = m_device->CreateRootSignature(0, sigBlob->GetBufferPointer(),
                                      sigBlob->GetBufferSize(),
                                      IID_PPV_ARGS(&m_videoRootSignature));
  if (FAILED(hr)) {
    char buf[128] = {};
    snprintf(buf, sizeof(buf), "CreateVideoPipeline: CreateRootSignature failed HR=0x%08lX", hr);
    LogError(buf);
    return false;
  }
  LogInfo("CreateVideoPipeline: root signature created");

  D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
  pso.pRootSignature = m_videoRootSignature.Get();
  pso.VS.pShaderBytecode = vsBlob->GetBufferPointer();
  pso.VS.BytecodeLength = vsBlob->GetBufferSize();
  pso.PS.pShaderBytecode = psBlob->GetBufferPointer();
  pso.PS.BytecodeLength = psBlob->GetBufferSize();
  pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  pso.BlendState.RenderTarget[0].RenderTargetWriteMask =
      D3D12_COLOR_WRITE_ENABLE_ALL;
  pso.SampleMask = UINT_MAX;
  pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pso.NumRenderTargets = 1;
  pso.RTVFormats[0] = kBackBufferFormat;
  pso.SampleDesc.Count = 1;

  hr = m_device->CreateGraphicsPipelineState(
      &pso, IID_PPV_ARGS(&m_videoPipelineState));
  if (FAILED(hr)) {
    char buf[128] = {};
    snprintf(buf, sizeof(buf), "CreateVideoPipeline: PSO failed HR=0x%08lX", hr);
    LogError(buf);
    return false;
  }
  LogInfo("CreateVideoPipeline: PSO created");

  D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
  srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  srvHeapDesc.NumDescriptors = 1;
  srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

  hr = m_device->CreateDescriptorHeap(&srvHeapDesc,
                                       IID_PPV_ARGS(&m_videoSrvHeap));
  if (FAILED(hr)) {
    char buf[128] = {};
    snprintf(buf, sizeof(buf), "CreateVideoPipeline: SRV heap failed HR=0x%08lX", hr);
    LogError(buf);
    return false;
  }
  LogInfo("CreateVideoPipeline: SRV heap created");

  {
    D3D12_RESOURCE_DESC placeTex = {};
    placeTex.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    placeTex.Width = 1; placeTex.Height = 1;
    placeTex.DepthOrArraySize = 1; placeTex.MipLevels = 1;
    placeTex.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    placeTex.SampleDesc.Count = 1;
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    Microsoft::WRL::ComPtr<ID3D12Resource> pt;
    m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &placeTex,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&pt));
    D3D12_SHADER_RESOURCE_VIEW_DESC psv = {};
    psv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    psv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    psv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    psv.Texture2D.MipLevels = 1;
    m_device->CreateShaderResourceView(pt.Get(), &psv,
        m_videoSrvHeap->GetCPUDescriptorHandleForHeapStart());
  }

  return true;
}

bool D3D12Renderer::CreateGamePipeline() {
  LogInfo("CreateGamePipeline: starting");

  auto vsBlob = CompileShader(kGameVertexShader, "vs_5_0", "main");
  auto psBlob = CompileShader(kGamePixelShader, "ps_5_0", "main");
  if (!vsBlob || !psBlob) {
    LogError("CreateGamePipeline: shader compilation failed");
    return false;
  }
  LogInfo("CreateGamePipeline: shaders compiled");

  D3D12_ROOT_PARAMETER rootParams[2] = {};
  rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
  rootParams[0].Descriptor.ShaderRegister = 0;
  rootParams[0].Descriptor.RegisterSpace = 0;
  rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

  rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
  rootParams[1].Descriptor.ShaderRegister = 1;
  rootParams[1].Descriptor.RegisterSpace = 0;
  rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

  D3D12_ROOT_SIGNATURE_DESC rootDesc = {};
  rootDesc.NumParameters = 2;
  rootDesc.pParameters = rootParams;
  rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

  Microsoft::WRL::ComPtr<ID3DBlob> sigBlob;
  Microsoft::WRL::ComPtr<ID3DBlob> errBlob;
  HRESULT hr = D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                            &sigBlob, &errBlob);
  if (FAILED(hr)) {
    if (errBlob) LogError(static_cast<const char*>(errBlob->GetBufferPointer()));
    return false;
  }
  hr = m_device->CreateRootSignature(0, sigBlob->GetBufferPointer(),
                                      sigBlob->GetBufferSize(),
                                      IID_PPV_ARGS(&m_gameRootSig));
  if (FAILED(hr)) return false;
  LogInfo("CreateGamePipeline: root signature created");

  D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
    {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
     D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12,
     D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
  };

  D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
  pso.pRootSignature = m_gameRootSig.Get();
  pso.VS.pShaderBytecode = vsBlob->GetBufferPointer();
  pso.VS.BytecodeLength = vsBlob->GetBufferSize();
  pso.PS.pShaderBytecode = psBlob->GetBufferPointer();
  pso.PS.BytecodeLength = psBlob->GetBufferSize();
  pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  pso.SampleMask = UINT_MAX;
  pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pso.NumRenderTargets = 1;
  pso.RTVFormats[0] = kBackBufferFormat;
  pso.SampleDesc.Count = 1;
  pso.InputLayout.NumElements = 2;
  pso.InputLayout.pInputElementDescs = inputLayout;

  hr = m_device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_gamePSO));
  if (FAILED(hr)) {
    LogError("CreateGamePipeline: PSO creation failed");
    return false;
  }
  LogInfo("CreateGamePipeline: PSO created");

  struct Vertex { float x, y, z, r, g, b, a; };
  Vertex verts[] = {
    { 0.0f,  0.6f, 0.0f, 1.0f, 0.2f, 0.2f, 1.0f},
    { 0.6f, -0.6f, 0.0f, 0.2f, 1.0f, 0.2f, 1.0f},
    {-0.6f, -0.6f, 0.0f, 0.2f, 0.2f, 1.0f, 1.0f},
  };
  uint16_t idx[] = {0, 1, 2};
  m_gameIndexCount = 3;

  UINT vbSize = sizeof(verts);
  {
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = vbSize; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_UNKNOWN; rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_gameVB))))
      return false;
    void* m = nullptr;
    m_gameVB->Map(0, nullptr, &m);
    memcpy(m, verts, vbSize);
    m_gameVB->Unmap(0, nullptr);
    m_gameVbv.BufferLocation = m_gameVB->GetGPUVirtualAddress();
    m_gameVbv.StrideInBytes = sizeof(Vertex);
    m_gameVbv.SizeInBytes = vbSize;
  }

  UINT ibSize = sizeof(idx);
  {
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = ibSize; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_UNKNOWN; rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_gameIB))))
      return false;
    void* m = nullptr;
    m_gameIB->Map(0, nullptr, &m);
    memcpy(m, idx, ibSize);
    m_gameIB->Unmap(0, nullptr);
    m_gameIbv.BufferLocation = m_gameIB->GetGPUVirtualAddress();
    m_gameIbv.Format = DXGI_FORMAT_R16_UINT;
    m_gameIbv.SizeInBytes = ibSize;
  }

  {
    float mvp[16] = {
      1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1
    };
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = 256; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_UNKNOWN; rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_gameCB))))
      return false;
    void* m = nullptr;
    m_gameCB->Map(0, nullptr, &m);
    memcpy(m, mvp, sizeof(mvp));
    m_gameCB->Unmap(0, nullptr);
  }

  {
    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 2;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(m_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_gameCbvHeap))))
      return false;

    D3D12_CONSTANT_BUFFER_VIEW_DESC cbv = {};
    cbv.BufferLocation = m_gameCB->GetGPUVirtualAddress();
    cbv.SizeInBytes = 256;
    m_device->CreateConstantBufferView(&cbv,
        m_gameCbvHeap->GetCPUDescriptorHandleForHeapStart());

    D3D12_CONSTANT_BUFFER_VIEW_DESC cbv2 = {};
    cbv2.BufferLocation = m_gameCB->GetGPUVirtualAddress();
    cbv2.SizeInBytes = 256;
    auto h = m_gameCbvHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    m_device->CreateConstantBufferView(&cbv2, h);
  }

  m_hasGamePipeline = true;
  LogInfo("CreateGamePipeline: done");
  return true;
}

void D3D12Renderer::RenderGameFrame() {
  if (!m_hasGamePipeline) return;
  m_commandList->SetGraphicsRootSignature(m_gameRootSig.Get());
  m_commandList->SetPipelineState(m_gamePSO.Get());

  ID3D12DescriptorHeap* heaps[] = {m_gameCbvHeap.Get()};
  m_commandList->SetDescriptorHeaps(1, heaps);
  m_commandList->SetGraphicsRootConstantBufferView(
      0, m_gameCB->GetGPUVirtualAddress());

  m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

  if (m_hasGameDrawData) {
    m_commandList->IASetVertexBuffers(0, 1, &m_gameDrawVbv);
    m_commandList->IASetIndexBuffer(&m_gameDrawIbv);
    m_commandList->DrawIndexedInstanced(m_gameDrawIndexCount, 1, 0, 0, 0);
  } else {
    m_commandList->IASetVertexBuffers(0, 1, &m_gameVbv);
    m_commandList->IASetIndexBuffer(&m_gameIbv);
    m_commandList->DrawIndexedInstanced(m_gameIndexCount, 1, 0, 0, 0);
  }
}

void D3D12Renderer::SetGameDrawData(const uint8_t* vertices, uint32_t vtxBytes,
                                     uint32_t vtxStride, const uint8_t* indices,
                                     uint32_t idxBytes, bool idx16,
                                     uint32_t idxCount) {
  // PERF(per-frame-allocs): this currently creates two ID3D12Resource's per
  // call (VB + IB) on the UPLOAD heap. Correctness is fine — old COM refs are
  // dropped via Reset() and D3D12's internal command-list tracking keeps the
  // underlying memory alive until the GPU finishes the last command using it
  // — but at 60fps this is ~120 CreateCommittedResource calls/sec. The proper
  // fix is a ring of N upload buffers (one per kFrameCount slot in flight),
  // recycled after MoveToNextFrame's fence sync. Deferred until the perf
  // budget matters; same TODO applies to UploadVideoFrame's m_videoUploadBuffer.
  if (!vertices || !indices || vtxBytes == 0 || idxBytes == 0) {
    m_hasGameDrawData = false;
    return;
  }

  auto createBuffer = [&](Microsoft::WRL::ComPtr<ID3D12Resource>& buf,
                          uint32_t size) -> bool {
    buf.Reset();
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = size; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_UNKNOWN; rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return SUCCEEDED(m_device->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_PPV_ARGS(&buf)));
  };

  if (!createBuffer(m_gameDrawVB, vtxBytes)) {
    m_hasGameDrawData = false;
    return;
  }
  void* vtxMap = nullptr;
  if (FAILED(m_gameDrawVB->Map(0, nullptr, &vtxMap))) {
    m_hasGameDrawData = false;
    return;
  }
  memcpy(vtxMap, vertices, vtxBytes);
  m_gameDrawVB->Unmap(0, nullptr);
  m_gameDrawVbv.BufferLocation = m_gameDrawVB->GetGPUVirtualAddress();
  m_gameDrawVbv.StrideInBytes = vtxStride;
  m_gameDrawVbv.SizeInBytes = vtxBytes;

  if (!createBuffer(m_gameDrawIB, idxBytes)) {
    m_hasGameDrawData = false;
    return;
  }
  void* idxMap = nullptr;
  if (FAILED(m_gameDrawIB->Map(0, nullptr, &idxMap))) {
    m_hasGameDrawData = false;
    return;
  }
  memcpy(idxMap, indices, idxBytes);
  m_gameDrawIB->Unmap(0, nullptr);
  m_gameDrawIbv.BufferLocation = m_gameDrawIB->GetGPUVirtualAddress();
  m_gameDrawIbv.Format = idx16 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
  m_gameDrawIbv.SizeInBytes = idxBytes;
  m_gameDrawIndexCount = idxCount;
  m_hasGameDrawData = true;
}

bool D3D12Renderer::CreateGameRenderTargets() {
  D3D12_HEAP_PROPERTIES hp = {};
  hp.Type = D3D12_HEAP_TYPE_DEFAULT;

  D3D12_RESOURCE_DESC rd = {};
  rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  rd.Width = m_width;
  rd.Height = m_height;
  rd.DepthOrArraySize = 1;
  rd.MipLevels = 1;
  rd.Format = kBackBufferFormat;
  rd.SampleDesc.Count = 1;
  rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

  D3D12_CLEAR_VALUE cv = {};
  cv.Format = kBackBufferFormat;
  cv.Color[0] = 0.05f; cv.Color[1] = 0.08f;
  cv.Color[2] = 0.18f; cv.Color[3] = 1.0f;

  if (FAILED(m_device->CreateCommittedResource(
      &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
      &cv, IID_PPV_ARGS(&m_gameRT)))) {
    LogError("CreateGameRT: game RT failed");
    return false;
  }

  {
    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = 1;
    if (FAILED(m_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_gameRtvHeap)))) {
      return false;
    }
    m_device->CreateRenderTargetView(m_gameRT.Get(), nullptr,
        m_gameRtvHeap->GetCPUDescriptorHandleForHeapStart());
  }

  rd.Format = DXGI_FORMAT_D32_FLOAT;
  rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
  cv.Format = DXGI_FORMAT_D32_FLOAT;
  cv.DepthStencil.Depth = 1.0f;
  cv.DepthStencil.Stencil = 0;

  if (FAILED(m_device->CreateCommittedResource(
      &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_DEPTH_WRITE,
      &cv, IID_PPV_ARGS(&m_gameDepth)))) {
    LogError("CreateGameRT: depth failed");
    return false;
  }

  {
    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    hd.NumDescriptors = 1;
    if (FAILED(m_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_gameDsvHeap)))) {
      return false;
    }
    m_device->CreateDepthStencilView(m_gameDepth.Get(), nullptr,
        m_gameDsvHeap->GetCPUDescriptorHandleForHeapStart());
  }

  LogInfo("CreateGameRT: done");
  return true;
}

void D3D12Renderer::PresentGameFrame() {
  if (!m_gameRT) return;

  // Forward barriers: m_gameRT RT→COPY_SOURCE (valid copy source), backbuf
  // RT→COPY_DEST (valid copy destination).
  D3D12_RESOURCE_BARRIER barriers[2] = {};
  barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barriers[0].Transition.pResource = m_gameRT.Get();
  barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
  barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
  barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

  barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barriers[1].Transition.pResource = m_renderTargets[m_frameIndex].Get();
  barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
  barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
  barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

  m_commandList->ResourceBarrier(2, barriers);

  D3D12_TEXTURE_COPY_LOCATION dst = {};
  dst.pResource = m_renderTargets[m_frameIndex].Get();
  dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  dst.SubresourceIndex = 0;

  D3D12_TEXTURE_COPY_LOCATION src = {};
  src.pResource = m_gameRT.Get();
  src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  src.SubresourceIndex = 0;

  m_commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

  // Reverse barriers: leave m_gameRT in PIXEL_SHADER_RESOURCE so the next
  // BeginFrame's PSR→RT barrier is valid; leave backbuf in RENDER_TARGET so
  // EndFrame's RT→PRESENT barrier is valid. Previous code left m_gameRT in RT
  // (next BeginFrame's PSR→RT would have been an invalid barrier) and backbuf
  // in PRESENT (EndFrame's RT→PRESENT would also have been invalid).
  barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
  barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

  barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
  barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;

  m_commandList->ResourceBarrier(2, barriers);
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
  m_viewport.TopLeftX = 0.0f;
  m_viewport.TopLeftY = 0.0f;
  m_viewport.Width = static_cast<float>(m_width);
  m_viewport.Height = static_cast<float>(m_height);
  m_viewport.MinDepth = 0.0f;
  m_viewport.MaxDepth = 1.0f;

  m_scissorRect.left = 0;
  m_scissorRect.top = 0;
  m_scissorRect.right = static_cast<LONG>(m_width);
  m_scissorRect.bottom = static_cast<LONG>(m_height);
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
}
