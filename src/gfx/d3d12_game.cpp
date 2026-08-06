// D3D12Renderer — game pipeline, offscreen game render target, and present.
//
// The game PSO takes a position+color vertex layout with an MVP constant
// buffer. It draws either the placeholder triangle baked in by
// CreateGamePipeline, or, once AddGameDraw has been fed translated PM4 draws,
// the guest's own vertex/index data — every draw the frame produced, each with
// its own transform and topology. Geometry renders into a dedicated 1280x720
// render target + D32 depth buffer, which PresentGameFrame copies to the
// current swapchain backbuffer.
//
// Note the PSO leaves DepthStencilState zeroed, so depth test is off and guest
// geometry at z = 1.0 is not rejected. DSVFormat is set anyway, to match the
// DSV BeginFrame binds — leaving it UNKNOWN while a D32_FLOAT view is bound is
// a debug-layer error, and the two must agree before depth can be turned on.

#include "gfx/d3d12_renderer.h"

#include "gfx/d3d12_internal.h"
#include "gfx/d3d12_shaders.h"
#include "gpu/hle_types.h"

#include <cstring>
#include <string>
#include <algorithm>
#include <unordered_set>
#include <utility>

// mx::hle::HostTopology carries D3D_PRIMITIVE_TOPOLOGY values so the translator
// need not include <d3dcommon.h>. That only holds while these agree.
static_assert(static_cast<int>(mx::hle::HostTopology::kPointList) ==
                  D3D_PRIMITIVE_TOPOLOGY_POINTLIST &&
              static_cast<int>(mx::hle::HostTopology::kLineList) ==
                  D3D_PRIMITIVE_TOPOLOGY_LINELIST &&
              static_cast<int>(mx::hle::HostTopology::kLineStrip) ==
                  D3D_PRIMITIVE_TOPOLOGY_LINESTRIP &&
              static_cast<int>(mx::hle::HostTopology::kTriangleList) ==
                  D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST &&
              static_cast<int>(mx::hle::HostTopology::kTriangleStrip) ==
                  D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP,
              "HostTopology has drifted from D3D_PRIMITIVE_TOPOLOGY");

static_assert(kMaxDrawPlanes == mx::hle::DrawCall::kMaxPlanes,
              "renderer plane budget has drifted from DrawCall::kMaxPlanes");

using mx::gfx::CompileShader;
using mx::gfx::LogError;
using mx::gfx::LogInfo;

bool D3D12Renderer::CreateGamePipeline() {
  LogInfo("CreateGamePipeline: starting");

  auto vsBlob = CompileShader(mx::gfx::shaders::kGameVS, "vs_5_0", "main");
  auto psBlob = CompileShader(mx::gfx::shaders::kGamePS, "ps_5_0", "main");
  auto texturePsBlob = CompileShader(mx::gfx::shaders::kGameTexturePS,
                                     "ps_5_0", "main");
  auto yuvPsBlob = CompileShader(mx::gfx::shaders::kGameYuvPS, "ps_5_0",
                                 "main");
  if (!vsBlob || !psBlob || !texturePsBlob || !yuvPsBlob) {
    LogError("CreateGamePipeline: shader compilation failed");
    return false;
  }
  LogInfo("CreateGamePipeline: shaders compiled");

  D3D12_DESCRIPTOR_RANGE srvRange = {};
  srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  // Four, for Bink's Y/Cr/Cb + alpha plane set. Single-texture draws still
  // point the table at their own descriptor and read only t0; the three slots
  // that follow belong to other textures and are never sampled, so they only
  // have to exist inside the heap.
  srvRange.NumDescriptors = kMaxDrawPlanes;
  srvRange.BaseShaderRegister = 0;
  srvRange.RegisterSpace = 0;
  srvRange.OffsetInDescriptorsFromTableStart = 0;

  D3D12_ROOT_PARAMETER rootParams[2] = {};
  rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
  rootParams[0].Descriptor.ShaderRegister = 0;
  rootParams[0].Descriptor.RegisterSpace = 0;
  rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

  rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  rootParams[1].DescriptorTable.NumDescriptorRanges = 1;
  rootParams[1].DescriptorTable.pDescriptorRanges = &srvRange;
  rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

  D3D12_STATIC_SAMPLER_DESC sampler = {};
  sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
  sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
  sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
  sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
  sampler.MipLODBias = 0.0f;
  sampler.MaxAnisotropy = 1;
  sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
  sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
  sampler.MinLOD = 0.0f;
  sampler.MaxLOD = 0.0f;
  sampler.ShaderRegister = 0;
  sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

  D3D12_ROOT_SIGNATURE_DESC rootDesc = {};
  rootDesc.NumParameters = 2;
  rootDesc.pParameters = rootParams;
  rootDesc.NumStaticSamplers = 1;
  rootDesc.pStaticSamplers = &sampler;
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
    {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 28,
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
  pso.SampleMask = UINT_MAX;
  pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pso.NumRenderTargets = 1;
  pso.RTVFormats[0] = kBackBufferFormat;
  // DepthStencilState stays zeroed — DepthEnable FALSE — so nothing is tested
  // or written. DSVFormat must still name the format of the DSV BeginFrame
  // binds: a PSO declaring DXGI_FORMAT_UNKNOWN while a D32_FLOAT DSV is bound
  // is a debug-layer error, and the two disagreeing is the sort of thing that
  // becomes a real state mismatch the moment depth is turned on.
  pso.DSVFormat = kGameDepthFormat;
  pso.SampleDesc.Count = 1;
  pso.InputLayout.NumElements = 3;
  pso.InputLayout.pInputElementDescs = inputLayout;

  for (uint32_t i = 0; i < m_gamePSOs.size(); ++i) {
    const bool depth_enable = (i & 1u) != 0;
    const bool depth_write = depth_enable && (i & 2u) != 0;
    const bool color_write = (i & 4u) == 0;
    const bool textured = (i & 8u) != 0;
    const bool yuv = (i & 16u) != 0;
    // A YUV variant that is not also textured would have no descriptor table
    // bound, so it is never selected and is skipped rather than created.
    if (yuv && !textured) continue;
    ID3DBlob* ps = yuv ? yuvPsBlob.Get()
                       : (textured ? texturePsBlob.Get() : psBlob.Get());
    pso.PS.pShaderBytecode = ps->GetBufferPointer();
    pso.PS.BytecodeLength = ps->GetBufferSize();
    pso.DepthStencilState = {};
    pso.DepthStencilState.DepthEnable = depth_enable;
    pso.DepthStencilState.DepthWriteMask = depth_write
        ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = color_write
        ? D3D12_COLOR_WRITE_ENABLE_ALL : 0;
    hr = m_device->CreateGraphicsPipelineState(
        &pso, IID_PPV_ARGS(&m_gamePSOs[i]));
    if (FAILED(hr)) {
      LogError("CreateGamePipeline: PSO variant creation failed");
      return false;
    }
  }
  LogInfo("CreateGamePipeline: PSO variants created");

  struct Vertex { float x, y, z, r, g, b, a, u, v; };
  Vertex verts[] = {
    { 0.0f,  0.6f, 0.0f, 1.0f, 0.2f, 0.2f, 1.0f, 0.5f, 0.0f},
    { 0.6f, -0.6f, 0.0f, 0.2f, 1.0f, 0.2f, 1.0f, 1.0f, 1.0f},
    {-0.6f, -0.6f, 0.0f, 0.2f, 0.2f, 1.0f, 1.0f, 0.0f, 1.0f},
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

  {
    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = kMaxGameTextures;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(m_device->CreateDescriptorHeap(&hd,
                                               IID_PPV_ARGS(&m_gameSrvHeap))))
      return false;
    m_gameSrvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    // Descriptors 0..kMaxPlanes-1 belong to the video plane set, which is
    // rewritten every frame rather than cached; the general allocator starts
    // above them.
    m_nextGameSrvDescriptor =
        kYuvPlaneDescriptorBase + kMaxDrawPlanes;
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

// Uploads Bink's plane set into reusable host textures and writes their SRVs
// into the reserved descriptors at the head of the heap. A plane's resource is
// recreated only when its dimensions change, so steady-state playback creates
// nothing per frame; only the staging copy happens each time.
bool D3D12Renderer::EnsureYuvPlanes(const GameDraw& draw) {
  if (!m_gameSrvHeap || draw.planeCount < 3) return false;
  const uint32_t kPlanes = kMaxDrawPlanes;

  for (uint32_t i = 0; i < kPlanes; ++i) {
    // Slot 3 with no alpha plane gets a 1x1 white stand-in so the shader can
    // sample t3 unconditionally. It is built as a one-pixel payload rather
    // than special-cased through the whole upload path below.
    std::shared_ptr<const mx::hle::HleTexturePayload> src;
    if (i < draw.planeCount && draw.planes[i]) {
      src = draw.planes[i];
    } else {
      static std::shared_ptr<const mx::hle::HleTexturePayload> s_white = [] {
        auto p = std::make_shared<mx::hle::HleTexturePayload>();
        p->width = p->height = 1;
        p->row_pitch = 1;
        p->format = mx::hle::HostTextureFormat::kR8;
        p->data.assign(1, uint8_t(0xFF));
        return p;
      }();
      src = s_white;
    }
    if (src->data.empty()) return false;

    auto& plane = m_yuvPlanes[i];
    const DXGI_FORMAT format = DXGI_FORMAT_R8_UNORM;
    if (!plane.resource || plane.width != src->width ||
        plane.height != src->height || plane.format != format) {
      // Retire the old resources through the deferred-release list rather than
      // dropping them here: the GPU may still be reading last frame's plane.
      // See RetiredFrame in the header — a command list does not keep the
      // resources it references alive.
      RetiredFrame& r =
          (!m_retired.empty() && m_retired.back().fence == m_fenceValue)
              ? m_retired.back()
              : m_retired.emplace_back(RetiredFrame{m_fenceValue, {}});
      if (plane.resource) r.res.push_back(std::move(plane.resource));
      for (auto& up : plane.upload) {
        if (up) r.res.push_back(std::move(up));
        up.Reset();
      }
      plane.resource.Reset();

      D3D12_RESOURCE_DESC td = {};
      td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
      td.Width = src->width;
      td.Height = src->height;
      td.DepthOrArraySize = 1;
      td.MipLevels = 1;
      td.Format = format;
      td.SampleDesc.Count = 1;
      td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
      D3D12_HEAP_PROPERTIES defaultHeap = {};
      defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
      if (FAILED(m_device->CreateCommittedResource(
              &defaultHeap, D3D12_HEAP_FLAG_NONE, &td,
              D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
              IID_PPV_ARGS(&plane.resource))))
        return false;
      plane.width = src->width;
      plane.height = src->height;
      plane.format = format;

      D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
      srv.Format = format;
      srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
      srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
      srv.Texture2D.MipLevels = 1;
      auto cpu = m_gameSrvHeap->GetCPUDescriptorHandleForHeapStart();
      cpu.ptr += SIZE_T(kYuvPlaneDescriptorBase + i) * m_gameSrvDescriptorSize;
      m_device->CreateShaderResourceView(plane.resource.Get(), &srv, cpu);
    }

    D3D12_RESOURCE_DESC td = plane.resource->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT rows = 0;
    UINT64 rowBytes = 0, uploadBytes = 0;
    m_device->GetCopyableFootprints(&td, 0, 1, 0, &footprint, &rows, &rowBytes,
                                    &uploadBytes);
    auto& upload = plane.upload[m_frameIndex];
    if (!upload) {
      D3D12_RESOURCE_DESC bd = {};
      bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
      bd.Width = uploadBytes;
      bd.Height = 1;
      bd.DepthOrArraySize = 1;
      bd.MipLevels = 1;
      bd.Format = DXGI_FORMAT_UNKNOWN;
      bd.SampleDesc.Count = 1;
      bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
      D3D12_HEAP_PROPERTIES uploadHeap = {};
      uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
      if (FAILED(m_device->CreateCommittedResource(
              &uploadHeap, D3D12_HEAP_FLAG_NONE, &bd,
              D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
              IID_PPV_ARGS(&upload))))
        return false;
    }
    uint8_t* mapped = nullptr;
    if (FAILED(upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped))))
      return false;
    const uint32_t copyRows =
        std::min<uint32_t>(rows, src->row_pitch
                                     ? uint32_t(src->data.size() / src->row_pitch)
                                     : 0);
    const size_t copyBytes = std::min<size_t>(src->row_pitch, size_t(rowBytes));
    for (uint32_t y = 0; y < copyRows; ++y) {
      std::memcpy(
          mapped + footprint.Offset + size_t(y) * footprint.Footprint.RowPitch,
          src->data.data() + size_t(y) * src->row_pitch, copyBytes);
    }
    upload->Unmap(0, nullptr);

    D3D12_RESOURCE_BARRIER toCopy = {};
    toCopy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toCopy.Transition.pResource = plane.resource.Get();
    toCopy.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    toCopy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    toCopy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_commandList->ResourceBarrier(1, &toCopy);

    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = plane.resource.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
    srcLoc.pResource = upload.Get();
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcLoc.PlacedFootprint = footprint;
    m_commandList->CopyTextureRegion(&dst, 0, 0, 0, &srcLoc, nullptr);

    D3D12_RESOURCE_BARRIER toRead = toCopy;
    toRead.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    toRead.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    m_commandList->ResourceBarrier(1, &toRead);
  }

  static uint64_t s_frames = 0;
  if (++s_frames <= 4 || (s_frames % 300) == 0) {
    char message[192];
    std::snprintf(message, sizeof(message),
                  "yuv planes uploaded %llu: %ux%u luma, %u planes, alpha %d",
                  static_cast<unsigned long long>(s_frames), m_yuvPlanes[0].width,
                  m_yuvPlanes[0].height, draw.planeCount,
                  draw.yuvHasAlpha ? 1 : 0);
    LogInfo(message);
  }
  return true;
}

bool D3D12Renderer::EnsureGameTexture(
    const std::shared_ptr<const mx::hle::HleTexturePayload>& texture,
    uint32_t& descriptorIndex) {
  if (!texture || texture->data.empty() || !m_gameSrvHeap) return false;
  if (auto it = m_gameTextures.find(texture->key); it != m_gameTextures.end()) {
    descriptorIndex = it->second.descriptorIndex;
    return true;
  }
  if (m_nextGameSrvDescriptor >= kMaxGameTextures) {
    static bool s_logged = false;
    if (!s_logged) {
      s_logged = true;
      LogError("game texture cache full; falling back to vertex colour");
    }
    return false;
  }

  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  switch (texture->format) {
    case mx::hle::HostTextureFormat::kRgba8:
      format = DXGI_FORMAT_R8G8B8A8_UNORM;
      break;
    case mx::hle::HostTextureFormat::kBc1:
      format = DXGI_FORMAT_BC1_UNORM;
      break;
    case mx::hle::HostTextureFormat::kBc2:
      format = DXGI_FORMAT_BC2_UNORM;
      break;
    case mx::hle::HostTextureFormat::kBc3:
      format = DXGI_FORMAT_BC3_UNORM;
      break;
    case mx::hle::HostTextureFormat::kBc5:
      format = DXGI_FORMAT_BC5_UNORM;
      break;
    case mx::hle::HostTextureFormat::kR16Float:
      format = DXGI_FORMAT_R16_FLOAT;
      break;
    case mx::hle::HostTextureFormat::kRgba16Float:
      format = DXGI_FORMAT_R16G16B16A16_FLOAT;
      break;
    case mx::hle::HostTextureFormat::kBgra4:
      format = DXGI_FORMAT_B4G4R4A4_UNORM;
      break;
    case mx::hle::HostTextureFormat::kR8:
      format = DXGI_FORMAT_R8_UNORM;
      break;
    case mx::hle::HostTextureFormat::kR16:
      format = DXGI_FORMAT_R16_UNORM;
      break;
    case mx::hle::HostTextureFormat::kR32Float:
      format = DXGI_FORMAT_R32_FLOAT;
      break;
  }
  if (format == DXGI_FORMAT_UNKNOWN || !texture->width || !texture->height)
    return false;

  // B4G4R4A4_UNORM is an optional D3D12 format, so support is a question the
  // driver answers rather than one to assume. Asked once per format and
  // logged, so an unsupported host produces one clear line instead of an
  // opaque CreateCommittedResource failure.
  {
    static std::map<DXGI_FORMAT, bool> s_supported;
    auto it = s_supported.find(format);
    if (it == s_supported.end()) {
      D3D12_FEATURE_DATA_FORMAT_SUPPORT fs = {};
      fs.Format = format;
      const bool ok =
          SUCCEEDED(m_device->CheckFeatureSupport(
              D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof(fs))) &&
          (fs.Support1 & D3D12_FORMAT_SUPPORT1_TEXTURE2D) &&
          (fs.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE);
      it = s_supported.emplace(format, ok).first;
      REXLOG_INFO("[D3D12Renderer] texture format {} sample support: {}",
                  uint32_t(format), ok ? "yes" : "NO — textures dropped");
    }
    if (!it->second) return false;
  }

  GameTexture entry;
  D3D12_RESOURCE_DESC td = {};
  td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  td.Width = texture->width;
  td.Height = texture->height;
  td.DepthOrArraySize = 1;
  td.MipLevels = 1;
  td.Format = format;
  td.SampleDesc.Count = 1;
  td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  D3D12_HEAP_PROPERTIES defaultHeap = {};
  defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
  if (FAILED(m_device->CreateCommittedResource(
          &defaultHeap, D3D12_HEAP_FLAG_NONE, &td,
          D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
          IID_PPV_ARGS(&entry.resource))))
    return false;

  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
  UINT rows = 0;
  UINT64 rowBytes = 0;
  UINT64 uploadBytes = 0;
  m_device->GetCopyableFootprints(&td, 0, 1, 0, &footprint, &rows,
                                  &rowBytes, &uploadBytes);
  D3D12_RESOURCE_DESC bd = {};
  bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  bd.Width = uploadBytes;
  bd.Height = 1;
  bd.DepthOrArraySize = 1;
  bd.MipLevels = 1;
  bd.Format = DXGI_FORMAT_UNKNOWN;
  bd.SampleDesc.Count = 1;
  bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  D3D12_HEAP_PROPERTIES uploadHeap = {};
  uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
  if (FAILED(m_device->CreateCommittedResource(
          &uploadHeap, D3D12_HEAP_FLAG_NONE, &bd,
          D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
          IID_PPV_ARGS(&entry.upload))))
    return false;

  uint8_t* mapped = nullptr;
  if (FAILED(entry.upload->Map(0, nullptr,
                               reinterpret_cast<void**>(&mapped))))
    return false;
  const uint32_t copyRows = std::min<uint32_t>(rows,
      uint32_t(texture->data.size() / texture->row_pitch));
  const size_t copyBytes = std::min<size_t>(texture->row_pitch, size_t(rowBytes));
  for (uint32_t y = 0; y < copyRows; ++y) {
    std::memcpy(mapped + footprint.Offset + size_t(y) * footprint.Footprint.RowPitch,
                texture->data.data() + size_t(y) * texture->row_pitch,
                copyBytes);
  }
  entry.upload->Unmap(0, nullptr);

  D3D12_TEXTURE_COPY_LOCATION dst = {};
  dst.pResource = entry.resource.Get();
  dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  dst.SubresourceIndex = 0;
  D3D12_TEXTURE_COPY_LOCATION src = {};
  src.pResource = entry.upload.Get();
  src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  src.PlacedFootprint = footprint;
  m_commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  D3D12_RESOURCE_BARRIER barrier = {};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = entry.resource.Get();
  barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
  barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  m_commandList->ResourceBarrier(1, &barrier);

  if (m_nextGameSrvDescriptor >= kMaxGameTextures) return false;
  entry.descriptorIndex = m_nextGameSrvDescriptor++;
  D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
  srv.Format = format;
  srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srv.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
      (texture->swizzle >> 0) & 7u, (texture->swizzle >> 3) & 7u,
      (texture->swizzle >> 6) & 7u, (texture->swizzle >> 9) & 7u);
  srv.Texture2D.MipLevels = 1;
  auto cpu = m_gameSrvHeap->GetCPUDescriptorHandleForHeapStart();
  cpu.ptr += SIZE_T(entry.descriptorIndex) * m_gameSrvDescriptorSize;
  m_device->CreateShaderResourceView(entry.resource.Get(), &srv, cpu);
  descriptorIndex = entry.descriptorIndex;
  m_gameTextures.emplace(texture->key, std::move(entry));
  static uint64_t s_uploads = 0;
  if (++s_uploads <= 16 || (s_uploads % 100) == 0) {
    char message[192];
    std::snprintf(message, sizeof(message),
                  "game texture upload: %ux%u, format %u, cache %zu",
                  texture->width, texture->height,
                  uint32_t(texture->format), m_gameTextures.size());
    LogInfo(message);
  }
  return true;
}

D3D12Renderer::GameRenderTarget* D3D12Renderer::EnsureGameRenderTarget(
    uint32_t object, uint32_t width, uint32_t height) {
  if (!object || !width || !height || width > 8192 || height > 8192 ||
      !m_gameRtvHeap || !m_gameSrvHeap)
    return nullptr;
  if (auto it = m_gameRenderTargets.find(object);
      it != m_gameRenderTargets.end()) {
    if (it->second.width != width || it->second.height != height) {
      // A guest heap address reused at a different size. The entry cannot be
      // resized in place (its RTV/SRV descriptors are baked), and there is no
      // eviction, so this object is unroutable for the rest of the run.
      ++m_rtRejectResized;
      static bool s_logged = false;
      if (!s_logged) {
        s_logged = true;
        LogError("game render-target object changed dimensions");
      }
      return nullptr;
    }
    return &it->second;
  }
  // Budget exhausted. Counted separately from every other refusal because the
  // consequence is invisible: the caller falls back to the main target and the
  // draw overpaints the scene, which is exactly the bug offscreen routing was
  // built to fix. m_gameRenderTargets is never evicted, so once this trips it
  // stays tripped.
  if (m_gameRenderTargets.size() >= kMaxGameRenderTargets ||
      m_nextGameSrvDescriptor >= kMaxGameTextures) {
    ++m_rtRejectBudget;
    return nullptr;
  }

  GameRenderTarget entry;
  entry.width = width;
  entry.height = height;
  entry.rtvIndex = uint32_t(m_gameRenderTargets.size()) + 1;
  entry.srvIndex = m_nextGameSrvDescriptor++;

  D3D12_HEAP_PROPERTIES hp = {};
  hp.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC rd = {};
  rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  rd.Width = width;
  rd.Height = height;
  rd.DepthOrArraySize = 1;
  rd.MipLevels = 1;
  rd.Format = kBackBufferFormat;
  rd.SampleDesc.Count = 1;
  rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  D3D12_CLEAR_VALUE cv = {};
  cv.Format = kBackBufferFormat;
  cv.Color[0] = 0.0f;
  cv.Color[1] = 0.0f;
  cv.Color[2] = 0.0f;
  cv.Color[3] = 0.0f;
  if (FAILED(m_device->CreateCommittedResource(
          &hp, D3D12_HEAP_FLAG_NONE, &rd,
          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &cv,
          IID_PPV_ARGS(&entry.resource))))
    return nullptr;

  auto rtv = m_gameRtvHeap->GetCPUDescriptorHandleForHeapStart();
  rtv.ptr += SIZE_T(entry.rtvIndex) * m_gameRtvDescriptorSize;
  m_device->CreateRenderTargetView(entry.resource.Get(), nullptr, rtv);

  D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
  srv.Format = kBackBufferFormat;
  srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv.Texture2D.MipLevels = 1;
  auto cpu = m_gameSrvHeap->GetCPUDescriptorHandleForHeapStart();
  cpu.ptr += SIZE_T(entry.srvIndex) * m_gameSrvDescriptorSize;
  m_device->CreateShaderResourceView(entry.resource.Get(), &srv, cpu);

  auto [it, inserted] = m_gameRenderTargets.emplace(object, std::move(entry));
  if (!inserted) return nullptr;
  char message[192];
  std::snprintf(message, sizeof(message),
                "game render target: object 0x%08X %ux%u cache %zu",
                object, width, height, m_gameRenderTargets.size());
  LogInfo(message);
  return &it->second;
}

void D3D12Renderer::RenderGameFrame() {
  if (!m_hasGamePipeline) return;
  m_commandList->SetGraphicsRootSignature(m_gameRootSig.Get());
  m_commandList->SetPipelineState(m_gamePSOs[0].Get());

  ID3D12DescriptorHeap* heaps[] = {m_gameSrvHeap.Get()};
  m_commandList->SetDescriptorHeaps(1, heaps);
  for (auto& [object, target] : m_gameRenderTargets)
    target.usedThisFrame = false;

  if (m_gameDraws.empty() && !m_hasEverDrawnGame) {
    // Placeholder triangle, under the identity matrix in m_gameCB. Only until
    // the guest has drawn something once — see m_hasEverDrawnGame. This branch
    // used to be taken on every host tick that fell between two guest swaps,
    // which is most of them, so the triangle was being drawn at roughly half
    // the frames even in the post-load state. That was invisible while the
    // render target accumulated; with the clear fixed it reads as a flash.
    m_commandList->SetGraphicsRootConstantBufferView(
        0, m_gameCB->GetGPUVirtualAddress());
    m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_commandList->IASetVertexBuffers(0, 1, &m_gameVbv);
    m_commandList->IASetIndexBuffer(&m_gameIbv);
    m_commandList->DrawIndexedInstanced(m_gameIndexCount, 1, 0, 0, 0);
    return;
  }

  uint32_t boundTargetObject = 0;  // zero is the final m_gameRT.
  static const float kOffscreenClear[4] = {0, 0, 0, 0};
  std::unordered_set<uint32_t> sampledTargets;
  sampledTargets.reserve(m_gameDraws.size());
  for (const auto& d : m_gameDraws) {
    if (d.sampledTargetObject &&
        d.sampledTargetObject != d.targetObject)
      sampledTargets.insert(d.sampledTargetObject);
  }
  for (const auto& d : m_gameDraws) {
    // Keep only the unsampled final 1280x720 surface on m_gameRT so
    // PresentGameFrame remains an exact-size copy. A full-size scene target
    // that a later compositor samples is still offscreen and needs its own SRV;
    // classifying solely by dimensions made that target alias m_gameRT and
    // left the final draw with nothing it could legally sample.
    GameRenderTarget* drawTarget = nullptr;
    const bool feedsLaterDraw =
        d.targetObject && sampledTargets.contains(d.targetObject);
    const bool wantsOffscreen =
        d.targetObject && d.targetWidth && d.targetHeight &&
        (feedsLaterDraw || d.targetWidth != 1280 || d.targetHeight != 720);
    if (wantsOffscreen) {
      drawTarget = EnsureGameRenderTarget(d.targetObject, d.targetWidth,
                                          d.targetHeight);
    }
    // The three populations, kept apart on purpose. A draw that never wanted an
    // offscreen target and one that wanted it and was refused both end up on
    // the main render target, and a single "drew on main" count cannot tell
    // them apart — but only the second is a silent regression to overpainting.
    if (!wantsOffscreen) ++m_rtDrawsMain;
    else if (drawTarget) ++m_rtDrawsOffscreen;
    else ++m_rtDrawsOverpaint;
    if (drawTarget) {
      if (drawTarget->state != D3D12_RESOURCE_STATE_RENDER_TARGET) {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = drawTarget->resource.Get();
        barrier.Transition.StateBefore = drawTarget->state;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        m_commandList->ResourceBarrier(1, &barrier);
        drawTarget->state = D3D12_RESOURCE_STATE_RENDER_TARGET;
      }
      if (boundTargetObject != d.targetObject) {
        auto rtv = m_gameRtvHeap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += SIZE_T(drawTarget->rtvIndex) * m_gameRtvDescriptorSize;
        m_commandList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        D3D12_VIEWPORT viewport = {};
        viewport.Width = float(drawTarget->width);
        viewport.Height = float(drawTarget->height);
        viewport.MinDepth = 0.0f;
        viewport.MaxDepth = 1.0f;
        D3D12_RECT scissor = {0, 0, LONG(drawTarget->width),
                              LONG(drawTarget->height)};
        m_commandList->RSSetViewports(1, &viewport);
        m_commandList->RSSetScissorRects(1, &scissor);
        boundTargetObject = d.targetObject;
      }
      if (!drawTarget->usedThisFrame) {
        auto rtv = m_gameRtvHeap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += SIZE_T(drawTarget->rtvIndex) * m_gameRtvDescriptorSize;
        m_commandList->ClearRenderTargetView(rtv, kOffscreenClear, 0, nullptr);
        drawTarget->usedThisFrame = true;
      }
    } else if (boundTargetObject != 0) {
      auto rtv = m_gameRtvHeap->GetCPUDescriptorHandleForHeapStart();
      auto dsv = m_gameDsvHeap->GetCPUDescriptorHandleForHeapStart();
      m_commandList->OMSetRenderTargets(1, &rtv, FALSE,
                                        m_gameDepth ? &dsv : nullptr);
      m_commandList->RSSetViewports(1, &m_viewport);
      m_commandList->RSSetScissorRects(1, &m_scissorRect);
      boundTargetObject = 0;
    }

    uint32_t textureDescriptor = 0;
    bool textured = false;
    if (d.sampledTargetObject &&
        d.sampledTargetObject != d.targetObject) {
      if (auto it = m_gameRenderTargets.find(d.sampledTargetObject);
          it != m_gameRenderTargets.end()) {
        auto& sampled = it->second;
        if (sampled.state != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) {
          D3D12_RESOURCE_BARRIER barrier = {};
          barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
          barrier.Transition.pResource = sampled.resource.Get();
          barrier.Transition.StateBefore = sampled.state;
          barrier.Transition.StateAfter =
              D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
          barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
          m_commandList->ResourceBarrier(1, &barrier);
          sampled.state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        }
        textureDescriptor = sampled.srvIndex;
        textured = true;
        static uint64_t s_resolved_hits = 0;
        if (++s_resolved_hits <= 16 || (s_resolved_hits % 1000) == 0) {
          char message[160];
          std::snprintf(message, sizeof(message),
                        "game resolved-target sample hit: object 0x%08X "
                        "descriptor %u (hit %llu)",
                        d.sampledTargetObject, textureDescriptor,
                        static_cast<unsigned long long>(s_resolved_hits));
          LogInfo(message);
        }
      }
    }
    // Bink's plane set takes precedence: it is several textures at once and
    // cannot go through the single-descriptor path below.
    bool yuv = false;
    if (d.yuvComposite && EnsureYuvPlanes(d)) {
      yuv = true;
      textured = true;
      textureDescriptor = kYuvPlaneDescriptorBase;
    }
    if (!textured)
      textured = EnsureGameTexture(d.texture, textureDescriptor);

    // Offscreen targets do not yet have per-surface depth resources. The
    // post-processing/resolve chain observed in ST_Southwest is colour-only;
    // keep depth disabled there rather than bind the 1280x720 DSV against a
    // smaller RTV, which is invalid D3D12 state.
    const bool depthEnable = !drawTarget && d.depthEnable;
    const bool depthWrite = depthEnable && d.depthWrite;
    const uint32_t pso_index = (depthEnable ? 1u : 0u) |
                               (depthWrite ? 2u : 0u) |
                               (d.colorWrite ? 0u : 4u) |
                               (textured ? 8u : 0u) |
                               (yuv ? 16u : 0u);
    m_commandList->SetPipelineState(m_gamePSOs[pso_index].Get());
    // Each translated draw brings its own transform; a draw whose cb failed to
    // allocate falls back to the identity matrix rather than being dropped.
    ID3D12Resource* cb = d.cb ? d.cb.Get() : m_gameCB.Get();
    m_commandList->SetGraphicsRootConstantBufferView(0,
                                                     cb->GetGPUVirtualAddress());
    if (textured) {
      // The table declares kMaxPlanes descriptors, so its base must leave that
      // many inside the heap. A single-texture draw reads only the first.
      const uint32_t maxBase =
          kMaxGameTextures - kMaxDrawPlanes;
      auto gpu = m_gameSrvHeap->GetGPUDescriptorHandleForHeapStart();
      gpu.ptr += UINT64(std::min(textureDescriptor, maxBase)) *
                 m_gameSrvDescriptorSize;
      m_commandList->SetGraphicsRootDescriptorTable(1, gpu);
    }
    m_commandList->IASetPrimitiveTopology(d.topology);
    m_commandList->IASetVertexBuffers(0, 1, &d.vbv);
    m_commandList->IASetIndexBuffer(&d.ibv);
    m_commandList->DrawIndexedInstanced(d.indexCount, 1, 0, 0, 0);
  }

  // Cumulative, every 100th frame that drew anything. Distinct live targets is
  // reported alongside the cap because the two together say whether the budget
  // is comfortable or about to be exhausted — the count alone does not.
  static uint32_t s_rtFrame = 0;
  if (!m_gameDraws.empty() && (++s_rtFrame % 100) == 1) {
    char message[300];
    std::snprintf(message, sizeof(message),
                  "game RT routing: offscreen %llu, main %llu, OVERPAINT %llu "
                  "(refused: budget %llu, resized %llu); live targets %u/%u, "
                  "srv %u/%u",
                  static_cast<unsigned long long>(m_rtDrawsOffscreen),
                  static_cast<unsigned long long>(m_rtDrawsMain),
                  static_cast<unsigned long long>(m_rtDrawsOverpaint),
                  static_cast<unsigned long long>(m_rtRejectBudget),
                  static_cast<unsigned long long>(m_rtRejectResized),
                  uint32_t(m_gameRenderTargets.size()), kMaxGameRenderTargets,
                  m_nextGameSrvDescriptor, kMaxGameTextures);
    LogInfo(message);
  }
}

void D3D12Renderer::ClearGameDraws() {
  // ClearGameDraws is called when the render thread receives a real guest
  // frame, even if filtering leaves it with zero submittable draws. Receipt of
  // that frame permanently retires the startup placeholder triangle.
  m_hasEverDrawnGame = true;
  if (m_gameDraws.empty()) return;

  // Hand the buffers to the retirement list rather than releasing them here.
  // These draws were recorded into the command list submitted at the end of the
  // previous frame, which signalled m_fenceValue; the GPU may still be reading
  // them. See RetiredFrame in the header for why the command list itself is no
  // protection.
  RetiredFrame& r = (!m_retired.empty() && m_retired.back().fence == m_fenceValue)
                        ? m_retired.back()
                        : m_retired.emplace_back(RetiredFrame{m_fenceValue, {}});
  r.res.reserve(r.res.size() + m_gameDraws.size() * 3);
  for (auto& d : m_gameDraws) {
    if (d.vb) r.res.push_back(std::move(d.vb));
    if (d.ib) r.res.push_back(std::move(d.ib));
    if (d.cb) r.res.push_back(std::move(d.cb));
  }
  m_gameDraws.clear();
}

void D3D12Renderer::DrainRetired() {
  if (m_retired.empty() || !m_fence) return;
  const uint64_t completed = m_fence->GetCompletedValue();
  while (!m_retired.empty() && m_retired.front().fence <= completed) {
    m_retired.pop_front();
  }
}

void D3D12Renderer::AddGameDraw(const uint8_t* vertices, uint32_t vtxBytes,
                                 uint32_t vtxStride, const uint8_t* indices,
                                 uint32_t idxBytes, bool idx16,
                                 uint32_t idxCount, const float* mvp,
                                 uint32_t topology, bool depthEnable,
                                 bool depthWrite, bool colorWrite,
                                 std::shared_ptr<const mx::hle::HleTexturePayload> texture,
                                 uint32_t targetObject, uint32_t targetWidth,
                                 uint32_t targetHeight,
                                 uint32_t sampledTargetObject,
                                 const std::shared_ptr<const mx::hle::HleTexturePayload>* planes,
                                 uint32_t planeCount, bool yuvHasAlpha) {
  // PERF(per-frame-allocs): this creates three ID3D12Resource's per call (VB +
  // IB + CB) on the UPLOAD heap, and is called once per submitted draw rather
  // than once per frame, so the allocation rate scales with the draw count —
  // which is why kMaxGameDraws caps it. The proper fix is a ring of upload
  // buffers recycled after MoveToNextFrame's fence sync. Same TODO applies to
  // the former UploadVideoFrame staging buffer.
  //
  // This comment used to claim "D3D12's internal command-list tracking keeps
  // the underlying memory alive until the GPU finishes the last command using
  // it". That is false — D3D12 command lists do not reference-count the
  // resources they reference; that was a D3D11 guarantee. Lifetime is the
  // application's job, and it is now done by ClearGameDraws handing these to
  // the fenced retirement list rather than releasing them outright.
  if (!vertices || !indices || vtxBytes == 0 || idxBytes == 0) return;
  if (m_gameDraws.size() >= kMaxGameDraws) {
    static bool s_logged = false;
    if (!s_logged) {
      s_logged = true;
      LogInfo("AddGameDraw: hit the per-frame draw cap, dropping the rest");
    }
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

  // Built locally and only appended once complete, so a partial failure leaves
  // the frame's list untouched rather than half-populated.
  GameDraw d;

  if (!createBuffer(d.vb, vtxBytes)) return;
  void* vtxMap = nullptr;
  if (FAILED(d.vb->Map(0, nullptr, &vtxMap))) return;
  memcpy(vtxMap, vertices, vtxBytes);
  d.vb->Unmap(0, nullptr);
  d.vbv.BufferLocation = d.vb->GetGPUVirtualAddress();
  d.vbv.StrideInBytes = vtxStride;
  d.vbv.SizeInBytes = vtxBytes;

  if (!createBuffer(d.ib, idxBytes)) return;
  void* idxMap = nullptr;
  if (FAILED(d.ib->Map(0, nullptr, &idxMap))) return;
  memcpy(idxMap, indices, idxBytes);
  d.ib->Unmap(0, nullptr);
  d.ibv.BufferLocation = d.ib->GetGPUVirtualAddress();
  d.ibv.Format = idx16 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
  d.ibv.SizeInBytes = idxBytes;
  d.indexCount = idxCount;
  d.topology = static_cast<D3D12_PRIMITIVE_TOPOLOGY>(topology);
  d.depthEnable = depthEnable;
  d.depthWrite = depthWrite;
  d.colorWrite = colorWrite;
  d.texture = std::move(texture);
  d.targetObject = targetObject;
  d.targetWidth = targetWidth;
  d.targetHeight = targetHeight;
  d.sampledTargetObject = sampledTargetObject;
  if (planes && planeCount >= 3) {
    d.planeCount = std::min<uint32_t>(planeCount,
                                      kMaxDrawPlanes);
    for (uint32_t i = 0; i < d.planeCount; ++i) d.planes[i] = planes[i];
    d.yuvHasAlpha = yuvHasAlpha;
    d.yuvComposite = true;
  }

  // Carry the translator's transform. Without this the draw renders under the
  // identity matrix baked into m_gameCB, which makes a correct transform and a
  // broken one look identical on screen. A null mvp, or a CB that fails to
  // allocate, falls back to that identity rather than dropping the draw.
  if (mvp && createBuffer(d.cb, 256)) {
    void* cbMap = nullptr;
    if (SUCCEEDED(d.cb->Map(0, nullptr, &cbMap))) {
      memcpy(cbMap, mvp, 16 * sizeof(float));
      d.cb->Unmap(0, nullptr);
    } else {
      d.cb.Reset();
    }
  }

  m_gameDraws.push_back(std::move(d));
  m_hasEverDrawnGame = true;
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
    // Descriptor zero is the final 1280x720 target; the remaining descriptors
    // are stable slots for D3D9 offscreen surface identities.
    hd.NumDescriptors = kMaxGameRenderTargets + 1;
    if (FAILED(m_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_gameRtvHeap)))) {
      return false;
    }
    m_gameRtvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    m_device->CreateRenderTargetView(m_gameRT.Get(), nullptr,
        m_gameRtvHeap->GetCPUDescriptorHandleForHeapStart());
  }

  rd.Format = kGameDepthFormat;
  rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
  cv.Format = kGameDepthFormat;
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
