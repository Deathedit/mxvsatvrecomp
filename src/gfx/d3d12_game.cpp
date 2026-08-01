// D3D12Renderer — game pipeline, offscreen game render target, and present.
//
// The game PSO takes a position+color vertex layout with an MVP constant
// buffer. It draws either the placeholder triangle baked in by
// CreateGamePipeline, or, once SetGameDrawData has been fed a translated PM4
// draw, the guest's own vertex/index data. Geometry renders into a dedicated
// 1280x720 render target + D32 depth buffer, which PresentGameFrame copies to
// the current swapchain backbuffer.

#include "gfx/d3d12_renderer.h"

#include "gfx/d3d12_internal.h"
#include "gfx/d3d12_shaders.h"

#include <cstring>

using mx::gfx::CompileShader;
using mx::gfx::LogError;
using mx::gfx::LogInfo;

bool D3D12Renderer::CreateGamePipeline() {
  LogInfo("CreateGamePipeline: starting");

  auto vsBlob = CompileShader(mx::gfx::shaders::kGameVS, "vs_5_0", "main");
  auto psBlob = CompileShader(mx::gfx::shaders::kGamePS, "ps_5_0", "main");
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
