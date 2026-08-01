// D3D12Renderer — Bink video pipeline.
//
// A fullscreen triangle sampling a single R8G8B8A8 texture. Frames arrive from
// the Bink decode thread as tightly packed RGBA (UploadVideoFrame), get staged
// through an UPLOAD-heap buffer, and are copied into the texture on the direct
// queue at draw time (RenderVideoFrame).

#include "gfx/d3d12_renderer.h"

#include "gfx/d3d12_internal.h"
#include "gfx/d3d12_shaders.h"

#include <cstdio>
#include <cstring>

using mx::gfx::CompileShader;
using mx::gfx::LogError;
using mx::gfx::LogInfo;

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
  auto vsBlob = CompileShader(mx::gfx::shaders::kVideoVS, "vs_5_0", "main");
  auto psBlob = CompileShader(mx::gfx::shaders::kVideoPS, "ps_5_0", "main");
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
