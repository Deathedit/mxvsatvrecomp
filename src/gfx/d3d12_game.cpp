// D3D12Renderer -- game pipeline, offscreen game render target, and present.
//
// The game PSO takes a position+color+uv vertex layout with an MVP constant
// buffer, and draws the guest's own vertex/index data as fed to AddGameDraw.
// Geometry renders into a dedicated 1280x720 target + D32 depth buffer, which
// PresentGameFrame copies to the current swapchain backbuffer.
//
// Note the PSO leaves DepthStencilState zeroed, so depth test is off and guest
// geometry at z = 1.0 is not rejected. DSVFormat is set anyway, to match the DSV
// BeginFrame binds -- leaving it UNKNOWN while a D32_FLOAT view is bound is a
// debug-layer error, and the two must agree before depth can be turned on.

#include "gfx/d3d12_renderer.h"

#include "gfx/d3d12_game_internal.h"
#include "gfx/d3d12_internal.h"
#include "gfx/d3d12_shaders.h"
#include "gpu/d3d9_layout.h"
#include "gpu/hle_types.h"
#include "gpu/shader_hlsl.h"  // kHlslInterpolatorLinkage, for the static_assert

#include <chrono>
#include <cstring>
#include <string>
#include <algorithm>
#include <set>
#include <tuple>
#include <unordered_map>
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

// The one number the vertex and pixel stages of a translated draw must agree
// on. When they disagreed, every CreateGraphicsPipelineState call failed and
// D3D12 reported nothing — so this drift is caught at build time instead.
static_assert(D3D12Renderer::TranslatedInterpolatorLinkage() ==
                  mx::hle::kHlslInterpolatorLinkage,
              "translated VS/PS interpolator linkage has drifted");
static_assert(D3D12Renderer::TranslatedSamplerTableWidth() ==
                  mx::hle::HlslShader::kMaxSamplerSlots,
              "translated sampler table is narrower than the emitter declares");
static_assert(mx::hle::DrawCall::kMaxPixelTextures ==
                  mx::hle::HlslShader::kMaxSamplerSlots,
              "DrawCall carries fewer textures than a shader can declare");

using mx::gfx::CompileShader;
using mx::gfx::LogError;
using mx::gfx::LogInfo;

// One sampler variant, from the three bits that describe it: clamp on U, clamp
// on V, and point rather than linear filtering. Xenos has more address modes
// than these; every mode that is not plain repeat is treated as clamp-to-edge,
// which is the distinction that matters at a surface edge. Mirror and border are
// not modelled.
//
// MaxLOD used to be pinned to 0 because only the base mip was ever uploaded. Now
// there is a chain, and the pin survives only for kSamplerBaseMap -- the guest's
// own "never minify past level 0", which the reference expresses the same way.
D3D12_SAMPLER_DESC D3D12Renderer::SamplerVariantDesc(uint32_t variant) {
  variant &= kSamplerClampU | kSamplerClampV | kSamplerPoint | kSamplerBaseMap |
             kSamplerMipPoint;
  D3D12_SAMPLER_DESC sd = {};
  // Two independent choices: how to filter WITHIN a level (the guest's min/mag
  // filter) and how to filter BETWEEN levels (its mip filter). D3D12 spells the
  // four combinations as four enumerants.
  if (variant & kSamplerPoint)
    sd.Filter = (variant & kSamplerMipPoint)
                    ? D3D12_FILTER_MIN_MAG_MIP_POINT
                    : D3D12_FILTER_MIN_MAG_POINT_MIP_LINEAR;
  else
    sd.Filter = (variant & kSamplerMipPoint)
                    ? D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT
                    : D3D12_FILTER_MIN_MAG_MIP_LINEAR;
  sd.AddressU = (variant & kSamplerClampU) ? D3D12_TEXTURE_ADDRESS_MODE_CLAMP
                                           : D3D12_TEXTURE_ADDRESS_MODE_WRAP;
  sd.AddressV = (variant & kSamplerClampV) ? D3D12_TEXTURE_ADDRESS_MODE_CLAMP
                                           : D3D12_TEXTURE_ADDRESS_MODE_WRAP;
  sd.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sd.MaxAnisotropy = 1;
  sd.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
  sd.MinLOD = 0.0f;
  sd.MaxLOD = (variant & kSamplerBaseMap) ? sd.MinLOD : D3D12_FLOAT32_MAX;
  return sd;
}

// The variant a decoded guest texture asks for. Xenos address modes 0 and 1 are
// repeat and mirrored repeat; anything higher clamps. The filter comes from the
// guest's own min/mag filter, which reached the renderer on the payload and was
// read by nothing until now.
uint32_t D3D12Renderer::SamplerVariantFor(
    const mx::hle::HleTexturePayload& tex) {
  uint32_t variant = 0;
  if (tex.clamp_x >= 2) variant |= kSamplerClampU;
  if (tex.clamp_y >= 2) variant |= kSamplerClampV;
  if (!tex.linear_filter) variant |= kSamplerPoint;
  // TextureFilter::kBaseMap. A payload that asked for it never carries a chain
  // -- the decode declines to build one -- so this only matters for a texture
  // whose chain was suppressed while some OTHER texture in the same block has
  // one. Set anyway, because a sampler is chosen per slot, not per resource.
  if (tex.mip_filter == mx::hle::kMipFilterBaseMap) variant |= kSamplerBaseMap;
  // Gated on there BEING a chain. mip_filter's zero value is kPoint, which is
  // also what a payload built by anything other than the guest texture path
  // carries, so an ungated test would put every Bink plane and resolve snapshot
  // into its own variant to express a distinction that cannot arise with one
  // level.
  if (tex.level_count > 1 && tex.mip_filter == mx::hle::kMipFilterPoint)
    variant |= kSamplerMipPoint;
  return variant;
}

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

  // The sampler is chosen PER DRAW, from a heap, rather than baked in as a
  // static one. The guest's address mode is per texture and the renderer used to
  // discard it and sample everything WRAP: a fullscreen post-process pass
  // reaching a hair past the edge then wrapped to the opposite side and blended
  // across the seam. A descriptor table costs one root parameter and keeps the
  // pixel shader and the PSO table untouched.
  D3D12_DESCRIPTOR_RANGE samplerRange = {};
  samplerRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
  samplerRange.NumDescriptors = 1;
  samplerRange.BaseShaderRegister = 0;
  samplerRange.RegisterSpace = 0;
  samplerRange.OffsetInDescriptorsFromTableStart = 0;

  D3D12_ROOT_PARAMETER rootParams[3] = {};
  rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
  rootParams[0].Descriptor.ShaderRegister = 0;
  rootParams[0].Descriptor.RegisterSpace = 0;
  rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

  rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  rootParams[1].DescriptorTable.NumDescriptorRanges = 1;
  rootParams[1].DescriptorTable.pDescriptorRanges = &srvRange;
  rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

  rootParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  rootParams[2].DescriptorTable.NumDescriptorRanges = 1;
  rootParams[2].DescriptorTable.pDescriptorRanges = &samplerRange;
  rootParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

  D3D12_ROOT_SIGNATURE_DESC rootDesc = {};
  rootDesc.NumParameters = 3;
  rootDesc.pParameters = rootParams;
  rootDesc.NumStaticSamplers = 0;
  rootDesc.pStaticSamplers = nullptr;
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

  // One sampler per (U,V) address-mode combination, indexed by
  // kSamplerClampU/kSamplerClampV. Xenos has more modes than these two; every
  // mode that is not plain repeat is treated as clamp-to-edge, which is the
  // distinction that matters at a surface edge. Mirror and border are not
  // modelled and would need their own entries.
  {
    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    hd.NumDescriptors = kSamplerHeapSize;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(m_device->CreateDescriptorHeap(&hd,
                                              IID_PPV_ARGS(&m_samplerHeap)))) {
      LogError("CreateGamePipeline: sampler heap failed");
      return false;
    }
    m_samplerDescriptorSize = m_device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    // The reserved first block. The stand-in path indexes these directly by
    // variant; the translated path copies them into its own per-slot blocks.
    auto cpu = m_samplerHeap->GetCPUDescriptorHandleForHeapStart();
    for (uint32_t i = 0; i < kSamplerVariantCount; ++i) {
      const D3D12_SAMPLER_DESC sd = SamplerVariantDesc(i);
      m_device->CreateSampler(&sd, cpu);
      cpu.ptr += SIZE_T(m_samplerDescriptorSize);
    }
  }

  D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
    // Four-component position: the guest exports clip space, and the hardware
    // does the near-plane clip and the perspective divide. See kHostVertexStride.
    {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0,
     D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16,
     D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 32,
     D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
  };
  D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
  pso.pRootSignature = m_gameRootSig.Get();
  pso.VS.pShaderBytecode = vsBlob->GetBufferPointer();
  pso.VS.BytecodeLength = vsBlob->GetBufferSize();
  pso.PS.pShaderBytecode = psBlob->GetBufferPointer();
  pso.PS.BytecodeLength = psBlob->GetBufferSize();
  pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  // Deliberately CULL_NONE: these are the 32 eagerly-built variants, which are
  // by definition the packed-cull-bits-0 state ("cull nothing, front is CW").
  // OpaquePSO and BlendedPSO override the rasterizer for every other mode via
  // ApplyCullBits. This line is NOT the hardcode that blacked out the menu.
  pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  // Depth clipping. A zero-initialised D3D12_RASTERIZER_DESC leaves this FALSE,
  // which is NOT the API's own default (D3D12_DEFAULT is TRUE) and not what
  // Xenos does: the reference sets it from PA_CL_CLIP_CNTL::clip_disable
  // (0x2204 bit 16), which this title leaves clear, so clipping is on.
  //
  // With it off, a primitive crossing the near plane is not clipped -- its
  // depth is clamped into [0,1] and it rasterises anyway. In the helmet camera
  // the rider straddles the near plane, and the parts in front of it projected
  // through w near zero into two dark blades across the sky. The tell is a
  // shaded fragment with SV_Position.z NEGATIVE, which near-plane clipping
  // makes impossible: event 30601 of the 2026-09-05 capture debugs to
  // z = -0.386 writing depth 0.
  pso.RasterizerState.DepthClipEnable = TRUE;
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

  // Keep everything BlendedPSO needs to rebuild a variant. The blobs have to be
  // retained too: the description holds bare pointers into them, and they would
  // otherwise be released when this function returns.
  m_gameVsBlob = vsBlob;
  m_gamePsBlobs = {psBlob, texturePsBlob, yuvPsBlob};
  m_gameInputLayout.assign(std::begin(inputLayout), std::end(inputLayout));
  m_gamePsoTemplate = pso;
  m_gamePsoTemplate.InputLayout.pInputElementDescs = m_gameInputLayout.data();
  m_gamePsoTemplate.InputLayout.NumElements =
      UINT(m_gameInputLayout.size());
  LogInfo("CreateGamePipeline: PSO variants created");

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
    // The video plane blocks live at the head of the heap and are rewritten as
    // the video plays; the general allocator starts above all of them.
    m_nextGameSrvDescriptor =
        kYuvPlaneDescriptorBase + kYuvPlaneDescriptorCount;
  }

  // The fallback transform for a draw whose own constant buffer failed to
  // allocate. Bound as a root CBV (rootParams[0]), so it needs no descriptor
  // heap — there used to be an m_gameCbvHeap here holding two views of this
  // buffer, and nothing ever bound it.
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

  if (!CreatePresentQuad()) return false;

  // Up front, not on first use. Creating a committed resource part-way through
  // recording a command list is legal, but doing it under a capture layer is
  // the kind of thing that only works most of the time -- and it did not work
  // under RenderDoc, which failed the Close of the list that created one.
  // There is nothing to gain by deferring three 4 KB buffers.
  for (auto& rb : m_luminanceReadback) {
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = kLuminanceReadbackBytes;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    // Non-fatal: without it the guest keeps reading a stale exposure, which is
    // a wrong picture rather than no picture.
    m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bd,
                                      D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                      IID_PPV_ARGS(&rb));
  }
  // The same, for small destinations the guest reads out of memory -- the
  // terrain feedback buffer. Up front for the same reason.
  for (auto& frameSlots : m_surfaceSlots)
    for (auto& slot : frameSlots) {
    auto& rb = slot.buffer;
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = kSurfaceReadbackBytes;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    // Non-fatal: without it the guest keeps reading a stale feedback buffer,
    // which is the behaviour it has today.
    m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bd,
                                      D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                      IID_PPV_ARGS(&rb));
  }

  m_hasGamePipeline = true;
  // Non-fatal: a failure here costs the translated path, not the stand-in one,
  // so the game still renders exactly as it did.
  if (!CreateTranslatedRootSignature())
    LogError("CreateGamePipeline: translated pipeline unavailable");

  LogInfo("CreateGamePipeline: done");
  return true;
}

// The fullscreen triangle PresentGameFrame blits with. Clip space, so the
// identity MVP in m_gameCB passes it through untouched; white vertex colour
// because kGameTexturePS multiplies the sample by it. The overhang (y=3, x=3) is
// deliberate: one triangle covering the viewport has no diagonal seam, and uv
// runs to 2 to match, so the visible region still maps 0..1.
bool D3D12Renderer::CreatePresentQuad() {
  struct V { float pos[4]; float col[4]; float uv[2]; };
  static_assert(sizeof(V) == mx::hle::kHostVertexStride,
                "present quad must match the game input layout stride");
  const V verts[3] = {
    {{-1.0f, -1.0f, 0.0f, 1.0f}, {1, 1, 1, 1}, {0.0f, 1.0f}},
    {{-1.0f,  3.0f, 0.0f, 1.0f}, {1, 1, 1, 1}, {0.0f, -1.0f}},
    {{ 3.0f, -1.0f, 0.0f, 1.0f}, {1, 1, 1, 1}, {2.0f, 1.0f}},
  };
  D3D12_HEAP_PROPERTIES hp = {};
  hp.Type = D3D12_HEAP_TYPE_UPLOAD;
  D3D12_RESOURCE_DESC rd = {};
  rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  rd.Width = sizeof(verts); rd.Height = 1; rd.DepthOrArraySize = 1;
  rd.MipLevels = 1; rd.Format = DXGI_FORMAT_UNKNOWN; rd.SampleDesc.Count = 1;
  rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  if (FAILED(m_device->CreateCommittedResource(
          &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ,
          nullptr, IID_PPV_ARGS(&m_presentVB)))) {
    LogError("CreatePresentQuad: vertex buffer creation failed");
    return false;
  }
  void* mapped = nullptr;
  if (FAILED(m_presentVB->Map(0, nullptr, &mapped))) return false;
  memcpy(mapped, verts, sizeof(verts));
  m_presentVB->Unmap(0, nullptr);
  m_presentVbv.BufferLocation = m_presentVB->GetGPUVirtualAddress();
  m_presentVbv.SizeInBytes = sizeof(verts);
  m_presentVbv.StrideInBytes = sizeof(V);
  return true;
}


// An HRESULT with its name where there is one.
//
// Every D3D12 creation failure in this file used to log the fact and discard the
// code, which makes a bare "creation failed" indistinguishable between
// out-of-memory, an invalid format combination and a device that has already
// gone. That cost a session: a depth-format change removed the device, and the
// only evidence was `resolve snapshot: creation FAILED for 64x64` with no code.
//
// An earlier version of this comment claimed the debug layer and DRED were
// unavailable here, and that was WRONG -- Graphics Tools IS installed and a
// later run logged "DRED enabled". Read the CURRENT run's DRED line before
// concluding anything about what is available. The HRESULT still earns its
// place: the debug layer is off by default in a release build (MX_D3D12_DEBUG=1,
// or 2 for GPU-based validation).
const char* HrName(HRESULT hr) {
  switch (hr) {
    case E_OUTOFMEMORY:            return "E_OUTOFMEMORY";
    case E_INVALIDARG:             return "E_INVALIDARG";
    case E_NOINTERFACE:            return "E_NOINTERFACE";
    case E_NOTIMPL:                return "E_NOTIMPL";
    case E_FAIL:                   return "E_FAIL";
    case DXGI_ERROR_DEVICE_REMOVED: return "DXGI_ERROR_DEVICE_REMOVED";
    case DXGI_ERROR_DEVICE_HUNG:   return "DXGI_ERROR_DEVICE_HUNG";
    case DXGI_ERROR_DEVICE_RESET:  return "DXGI_ERROR_DEVICE_RESET";
    case DXGI_ERROR_INVALID_CALL:  return "DXGI_ERROR_INVALID_CALL";
    case DXGI_ERROR_UNSUPPORTED:   return "DXGI_ERROR_UNSUPPORTED";
    default:                       return "";
  }
}

// "0x887A0005 DXGI_ERROR_DEVICE_REMOVED", or just the number if unnamed.
std::string HrText(HRESULT hr) {
  char buf[64];
  const char* name = HrName(hr);
  std::snprintf(buf, sizeof(buf), "0x%08lX%s%s", static_cast<unsigned long>(hr),
                *name ? " " : "", name);
  return buf;
}

// A resource's format expressed as one a shader-resource view can use.
//
// Most formats view as themselves. TYPELESS ones cannot: CreateShaderResourceView
// rejects them and D3D12 removes the device with DXGI_ERROR_INVALID_CALL -- and
// that failure does not look like what it is. The removal is triggered by an
// invalid CPU-side call, so there is no faulting GPU work and DRED reports
// "every command list completed; no faulting op", while every later Create* call
// returns DEVICE_REMOVED, so the first error in the log is some unrelated
// innocent allocation. Only the debug layer names it, and it is off by default.
//
// The depth entries are the ones that matter here: the game depth surface is
// R32G8X24_TYPELESS so one allocation can carry both a DSV and an SRV, and its
// depth plane views as R32_FLOAT_X8X24_TYPELESS.
DXGI_FORMAT SrvFormatForResource(DXGI_FORMAT resourceFormat) {
  switch (resourceFormat) {
    case DXGI_FORMAT_R32G8X24_TYPELESS:
      return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_R24G8_TYPELESS:
      return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R32_TYPELESS:
      return DXGI_FORMAT_R32_FLOAT;
    default:
      return resourceFormat;
  }
}

// Guest blend factor -> D3D12_BLEND.
//
// These are the Xenos hardware values, NOT the PC D3D9 D3DBLEND enum. Measured:
// the front end sets src 6, dest 7, op 0. Under the PC enum that reads
// INVSRCALPHA / DESTALPHA / an op that does not exist -- D3DBLENDOP starts at 1,
// so a zero op alone rules that enum out. Under the Xenos values it is
// SRC_ALPHA / ONE_MINUS_SRC_ALPHA / ADD, ordinary UI alpha blending.
//
// Returns false for anything not listed rather than substituting a default: a
// wrong blend factor still draws, so a silent fallback would be a visible bug
// with nothing in the log pointing at it.
bool ToD3D12Blend(uint32_t guest, bool alpha_channel, D3D12_BLEND& out) {
  switch (guest) {
    case 0:  out = D3D12_BLEND_ZERO; return true;
    case 1:  out = D3D12_BLEND_ONE; return true;
    // The *_COLOR factors are illegal in the alpha equation, so the alpha
    // channel takes the matching alpha factor.
    case 4:  out = alpha_channel ? D3D12_BLEND_SRC_ALPHA
                                 : D3D12_BLEND_SRC_COLOR; return true;
    case 5:  out = alpha_channel ? D3D12_BLEND_INV_SRC_ALPHA
                                 : D3D12_BLEND_INV_SRC_COLOR; return true;
    case 6:  out = D3D12_BLEND_SRC_ALPHA; return true;
    case 7:  out = D3D12_BLEND_INV_SRC_ALPHA; return true;
    case 8:  out = alpha_channel ? D3D12_BLEND_DEST_ALPHA
                                 : D3D12_BLEND_DEST_COLOR; return true;
    case 9:  out = alpha_channel ? D3D12_BLEND_INV_DEST_ALPHA
                                 : D3D12_BLEND_INV_DEST_COLOR; return true;
    case 10: out = D3D12_BLEND_DEST_ALPHA; return true;
    case 11: out = D3D12_BLEND_INV_DEST_ALPHA; return true;
    case 12: out = D3D12_BLEND_BLEND_FACTOR; return true;
    case 13: out = D3D12_BLEND_INV_BLEND_FACTOR; return true;
    case 16: out = D3D12_BLEND_SRC_ALPHA_SAT; return true;
    default: return false;
  }
}

// Guest blend op -> D3D12_BLEND_OP, same enum family and same contract.
bool ToD3D12BlendOp(uint32_t guest, D3D12_BLEND_OP& out) {
  switch (guest) {
    case 0: out = D3D12_BLEND_OP_ADD; return true;
    case 1: out = D3D12_BLEND_OP_SUBTRACT; return true;
    case 2: out = D3D12_BLEND_OP_MIN; return true;
    case 3: out = D3D12_BLEND_OP_MAX; return true;
    case 4: out = D3D12_BLEND_OP_REV_SUBTRACT; return true;
    default: return false;
  }
}

uint64_t g_blendUnmapped = 0;   // draws whose state did not translate
uint64_t g_blendBudget = 0;     // draws refused because the cache was full


//===========================================================================
// The translated-shader pipeline.
//
// Separate from the stand-in pipeline in every respect that matters: its own
// root signature, its own vertex shader, its own input layout. That separation
// is the point -- the stand-in path renders the game today, and the translated
// path is not allowed to change the layout it depends on in order to exist.
//===========================================================================
bool D3D12Renderer::CreateTranslatedRootSignature() {
  // One texture and one sampler per guest sampler slot, at the registers the
  // emitter names: EmitShaderHlsl writes `xe_texN : register(tN)` with N the
  // guest fetch-constant index, so the ranges must start at t0/s0 and be
  // contiguous. Sixteen covers every sampler seen in this game's profiles
  // (the widest was s8-s12).
  D3D12_DESCRIPTOR_RANGE srvRange = {};
  srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  srvRange.NumDescriptors = kTranslatedSamplerSlots;
  srvRange.BaseShaderRegister = 0;
  srvRange.OffsetInDescriptorsFromTableStart = 0;

  D3D12_DESCRIPTOR_RANGE samplerRange = {};
  samplerRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
  samplerRange.NumDescriptors = kTranslatedSamplerSlots;
  samplerRange.BaseShaderRegister = 0;
  samplerRange.OffsetInDescriptorsFromTableStart = 0;

  // The VERTEX stage's own ranges, at the SAME t0/s0 as the pixel stage.
  //
  // Not a collision: ShaderVisibility scopes a register to a stage. Separate
  // root parameters rather than making the pixel tables ALL-visible, because the
  // two shaders are cached independently and their compact slot 0 IS a different
  // guest sampler. These cannot be moved to higher registers to "keep them
  // apart": vs_5_0 has exactly 16 sampler slots, so anything at s16+ is not a
  // register and FXC rejects the shader with X4509.
  D3D12_DESCRIPTOR_RANGE vsSrvRange = srvRange;
  vsSrvRange.BaseShaderRegister = mx::hle::HlslShader::kVertexTextureBaseRegister;
  D3D12_DESCRIPTOR_RANGE vsSamplerRange = samplerRange;
  vsSamplerRange.BaseShaderRegister =
      mx::hle::HlslShader::kVertexSamplerBaseRegister;

  D3D12_ROOT_PARAMETER rootParams[3] = {};
  // b0, vertex: the same transform CBV the stand-in vertex shader takes, so the
  // passthrough VS below can share the per-draw constant buffer already built.
  rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
  rootParams[0].Descriptor.ShaderRegister = 0;
  rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
  // b1, pixel: the guest's own ALU constant bank plus the per-sampler texture
  // extents an unnormalized fetch needs. The bank is the PIXEL half, ALU
  // constants 256-511 at device+0x1780 — the emitter indexes it from 0, so the
  // rebase happens at upload, not in the shader.
  rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
  rootParams[1].Descriptor.ShaderRegister = 1;
  rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

  D3D12_DESCRIPTOR_RANGE ranges[2] = {srvRange, samplerRange};
  rootParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  rootParams[2].DescriptorTable.NumDescriptorRanges = 1;
  rootParams[2].DescriptorTable.pDescriptorRanges = &ranges[0];
  rootParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

  // Samplers live in their own heap type, so they cannot share a table with the
  // SRVs and need a fourth parameter.
  D3D12_ROOT_PARAMETER params[5] = {rootParams[0], rootParams[1], rootParams[2],
                                    {}, {}};
  params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[3].DescriptorTable.NumDescriptorRanges = 1;
  params[3].DescriptorTable.pDescriptorRanges = &ranges[1];
  params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

  // t16, vertex: the guest's raw vertex buffer, for a vertex shader that fetches
  // and decodes its own attributes instead of reading input elements the CPU
  // unpacked. A ROOT SRV, not a table: it needs no descriptor heap slot, takes
  // an upload heap's GPU virtual address directly, and so leaves
  // BindTranslatedTextures and its block ring untouched. t16 because the pixel
  // table occupies t0..t15 -- a register space would need shader model 5.1.
  params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
  params[4].Descriptor.ShaderRegister = 16;
  params[4].Descriptor.RegisterSpace = 0;
  params[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

  // t0.., s0.., VERTEX-visible: the vertex stage's own textures. Always present
  // even though most draws have no sampling vertex shader — a root signature is
  // per-pipeline and these cost two unused root parameters on the draws that do
  // not use them, against recompiling a second signature for the ones that do.
  D3D12_ROOT_PARAMETER vsTex[2] = {};
  vsTex[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  vsTex[0].DescriptorTable.NumDescriptorRanges = 1;
  vsTex[0].DescriptorTable.pDescriptorRanges = &vsSrvRange;
  vsTex[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
  vsTex[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  vsTex[1].DescriptorTable.NumDescriptorRanges = 1;
  vsTex[1].DescriptorTable.pDescriptorRanges = &vsSamplerRange;
  vsTex[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

  D3D12_ROOT_PARAMETER allParams[7] = {params[0], params[1], params[2],
                                       params[3], params[4], vsTex[0],
                                       vsTex[1]};

  D3D12_ROOT_SIGNATURE_DESC rootDesc = {};
  rootDesc.NumParameters = 7;
  rootDesc.pParameters = allParams;
  rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

  Microsoft::WRL::ComPtr<ID3DBlob> sigBlob, errBlob;
  HRESULT hr = D3D12SerializeRootSignature(
      &rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &errBlob);
  if (FAILED(hr)) {
    if (errBlob)
      LogError(static_cast<const char*>(errBlob->GetBufferPointer()));
    return false;
  }
  hr = m_device->CreateRootSignature(0, sigBlob->GetBufferPointer(),
                                     sigBlob->GetBufferSize(),
                                     IID_PPV_ARGS(&m_translatedRootSig));
  if (FAILED(hr)) {
    LogError("CreateTranslatedRootSignature: CreateRootSignature failed");
    return false;
  }

  // The passthrough vertex shader. The guest vertex shader is not on the GPU yet
  // -- the CPU interpreter still transforms every vertex -- so this stage only
  // forwards the position and the interpolators the interpreter computed. Its
  // output signature is exactly the XeInterpolants struct EmitShaderHlsl
  // declares for the pixel stage, which is what makes the two link.
  std::string vs =
      "struct XeInterpolants {\n"
      "  float4 pos : SV_Position;\n";
  for (uint32_t i = 0; i < kTranslatedInterpolators; ++i) {
    const std::string n = std::to_string(i);
    vs += "  float4 i" + n + " : TEXCOORD" + n + ";\n";
  }
  vs += "};\nstruct XeVsIn {\n  float4 pos : POSITION;\n";
  for (uint32_t i = 0; i < kTranslatedInterpolators; ++i) {
    const std::string n = std::to_string(i);
    vs += "  float4 i" + n + " : TEXCOORD" + n + ";\n";
  }
  vs +=
      "};\n"
      "XeInterpolants main(XeVsIn v) {\n"
      "  XeInterpolants o;\n"
      "  o.pos = v.pos;\n";
  for (uint32_t i = 0; i < kTranslatedInterpolators; ++i) {
    const std::string n = std::to_string(i);
    vs += "  o.i" + n + " = v.i" + n + ";\n";
  }
  vs += "  return o;\n}\n";

  m_translatedVsBlob = CompileShader(vs.c_str(), "vs_5_0", "main");
  if (!m_translatedVsBlob) {
    LogError("CreateTranslatedRootSignature: passthrough VS failed to compile");
    return false;
  }
  {
    // Ask the device what it can actually hold. The previous size assumed
    // Resource Binding Tier 1's 65536-descriptor cap and sized to stay under
    // it; that assumption, not the hardware, is what limited the ring to 1024
    // blocks per frame in flight and sent freeroam's UI to the stand-in. See
    // the note by kMaxTranslatedBlocksTier1.
    D3D12_FEATURE_DATA_D3D12_OPTIONS options = {};
    HRESULT hrOptions = m_device->CheckFeatureSupport(
        D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options));
    // A failed query is treated as Tier 1. It is the conservative direction:
    // too small only costs stand-in draws, while too large fails heap creation
    // outright and takes the whole translated path with it.
    const bool tier1 =
        FAILED(hrOptions) ||
        options.ResourceBindingTier == D3D12_RESOURCE_BINDING_TIER_1;
    m_maxTranslatedBlocks =
        tier1 ? kMaxTranslatedBlocksTier1 : kMaxTranslatedBlocksTier2;
    m_translatedBlocksPerFrame = m_maxTranslatedBlocks / kFrameCount;
    m_translatedBlockLimit = m_translatedBlocksPerFrame;
    {
      char msg[256];
      std::snprintf(msg, sizeof(msg),
                    "CreateTranslatedRootSignature: resource binding tier %u%s "
                    "-> %u descriptor blocks (%u per frame in flight, %u "
                    "descriptors)",
                    FAILED(hrOptions) ? 0u
                                      : uint32_t(options.ResourceBindingTier),
                    FAILED(hrOptions) ? " (query FAILED, assuming tier 1)" : "",
                    m_maxTranslatedBlocks, m_translatedBlocksPerFrame,
                    m_maxTranslatedBlocks * kTranslatedSamplerSlots);
      LogInfo(msg);
    }

    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = m_maxTranslatedBlocks * kTranslatedSamplerSlots;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(m_device->CreateDescriptorHeap(&hd,
                                              IID_PPV_ARGS(&m_translatedSrvHeap)))) {
      LogError("CreateTranslatedRootSignature: descriptor block heap failed");
      return false;
    }
  }
  LogInfo("CreateTranslatedRootSignature: ready");
  return true;
}

// Guest stencil op / compare -> D3D12. Both enums are the same ordering with
// D3D12 starting at 1, verified against rex/graphics/xenos.h:677 and :688, so
// this is an offset rather than a table. Clamped rather than trusted: the field
// is three bits and every value is legal, but a caller passing an already-
// converted value would otherwise index off the end of the D3D12 enum.
D3D12_STENCIL_OP GuestStencilOp(uint8_t v) {
  return D3D12_STENCIL_OP((v & 7u) + 1u);
}
D3D12_COMPARISON_FUNC GuestStencilFunc(uint8_t v) {
  return D3D12_COMPARISON_FUNC((v & 7u) + 1u);
}

// Write the stencil half of a pipeline description. Called from BOTH PSO
// builders so they cannot drift: a stencil variant built one way in the opaque
// path and another in the blended path would differ only for blended draws,
// which is exactly the kind of divergence that is invisible until it is not.
void ApplyStencil(const D3D12Renderer::GameStencil& st,
                  D3D12_DEPTH_STENCIL_DESC& ds) {
  ds.StencilEnable = st.enable ? TRUE : FALSE;
  if (!st.enable) return;
  ds.StencilReadMask = st.readMask;
  ds.StencilWriteMask = st.writeMask;
  ds.FrontFace.StencilFailOp = GuestStencilOp(st.frontFail);
  ds.FrontFace.StencilDepthFailOp = GuestStencilOp(st.frontZFail);
  ds.FrontFace.StencilPassOp = GuestStencilOp(st.frontPass);
  ds.FrontFace.StencilFunc = GuestStencilFunc(st.frontFunc);
  ds.BackFace.StencilFailOp = GuestStencilOp(st.backFail);
  ds.BackFace.StencilDepthFailOp = GuestStencilOp(st.backZFail);
  ds.BackFace.StencilPassOp = GuestStencilOp(st.backPass);
  ds.BackFace.StencilFunc = GuestStencilFunc(st.backFunc);
}

uint32_t D3D12Renderer::StencilIndexFor(const GameStencil& st) {
  const uint64_t key = st.PipelineKey();
  if (!key) return 0;  // disabled: index 0, no lookup, no entry.
  if (auto it = m_stencilStateIndex.find(key); it != m_stencilStateIndex.end())
    return it->second;
  // BOUNDED. The census says the guest uses 18 configurations, and those
  // differing only in ref collapse here because ref is not in the key. A cap
  // rather than unbounded growth because every new state is a new PIPELINE per
  // (format, topology, blend) combination it meets, and the blend cache is
  // already capped at 128: an unbounded stencil table would reach that cap and
  // start silently dropping draws to their opaque pipeline.
  //
  // Past the cap a draw renders WITHOUT stencil rather than being dropped, and
  // it is counted.
  constexpr size_t kMaxStencilStates = 64;
  if (m_stencilStates.size() >= kMaxStencilStates) {
    ++m_stencilStatesRefused;
    return 0;
  }
  const uint32_t index = uint32_t(m_stencilStates.size());
  m_stencilStates.push_back(st);
  m_stencilStateIndex.emplace(key, index);
  return index;
}

uint32_t D3D12Renderer::OmIndexFor(const GameOmState& st) {
  const uint64_t key = st.PipelineKey();
  // Index 0 is the state the renderer used to hardcode. A draw that resolves to
  // it takes exactly the path it took before this was plumbed.
  if (key == GameOmState{}.PipelineKey()) return 0;
  if (auto it = m_omStateIndex.find(key); it != m_omStateIndex.end())
    return it->second;
  // Bounded for the reason StencilIndexFor is: each new state is a new PIPELINE
  // per (shader, format, topology, blend, stencil) it meets, and the translated
  // cache is capped. Past the cap a draw renders with the OLD hardcoded state
  // rather than being dropped, and it is counted.
  constexpr size_t kMaxOmStates = 64;
  if (m_omStates.size() >= kMaxOmStates) {
    ++m_omStatesRefused;
    return 0;
  }
  const uint32_t index = uint32_t(m_omStates.size());
  m_omStates.push_back(st);
  m_omStateIndex.emplace(key, index);
  return index;
}

// Write the output-merger half of a pipeline description. Called from ALL THREE
// builders for the reason ApplyStencil is called from both: a state applied one
// way in the translated path and another in the opaque path would differ only
// for some draws, which is the kind of divergence that stays invisible until it
// is not.
//
// `colorWrite` stays a separate argument because it is the draw's own
// all-or-nothing decision (flags bit 2, and depth-only draws depend on it);
// the mask refines it rather than replacing it.
void ApplyOmState(const D3D12Renderer::GameOmState& om, bool colorWrite,
                  D3D12_GRAPHICS_PIPELINE_STATE_DESC& pso) {
  // Xenos CompareFunction and D3D12_COMPARISON_FUNC share an ordering with
  // D3D12 starting at 1 -- the same offset GuestStencilFunc relies on, and
  // guest 3 maps to LESS_EQUAL, which is what every builder hardcoded.
  pso.DepthStencilState.DepthFunc =
      D3D12_COMPARISON_FUNC((om.zfunc & 7u) + 1u);
  pso.RasterizerState.DepthClipEnable = om.depthClip ? TRUE : FALSE;
  // RB_COLOR_MASK bits 0-3 are R,G,B,A and D3D12_COLOR_WRITE_ENABLE_* are
  // 1,2,4,8 in the same order, so this is a pass-through, not a table.
  pso.BlendState.RenderTarget[0].RenderTargetWriteMask =
      colorWrite ? UINT8(om.colourMask & 0xFu) : 0;
}

ID3D12PipelineState* D3D12Renderer::TranslatedPSO(const TranslatedKey& key,
                                                  const std::string& hlsl,
                                                  const GameDraw& draw) {
  if (auto it = m_translatedPSOs.find(key); it != m_translatedPSOs.end())
    return it->second.failed ? nullptr : it->second.pso.Get();
  if (!m_translatedRootSig || !m_translatedVsBlob) {
    ++m_translatedNoRootSig;
    return nullptr;
  }
  // Past the cap a draw falls back to the stand-in rather than being dropped,
  // and nothing is cached, so the cap bounds memory without hiding shaders.
  if (m_translatedPSOs.size() >= kMaxTranslatedPSOs) {
    ++m_translatedPsoCapped;
    // Once, and loudly. Reaching the cap is not a throttle, it is the point
    // after which a share of every frame is drawn with the wrong shader, and
    // the only previous evidence of it was a counter buried in a summary line
    // that had to be read against the total to mean anything.
    static bool s_reported = false;
    if (!s_reported) {
      s_reported = true;
      LogError("TranslatedPSO: PSO cache FULL at kMaxTranslatedPSOs — every "
               "new shader/state combination from here on renders as a "
               "stand-in");
    }
    return nullptr;
  }

  TranslatedPipeline entry;
  entry.failed = true;

  // Compiled once per shader, not once per blend state: one shader commonly
  // appears under several states, and FXC is the expensive part. When the
  // hooks side carried precompiled bytecode (the content-keyed cache), use it
  // directly — a cache hit costs a D3DCreateBlob instead of a compile.
  auto& cached = m_translatedPsBlobs[key.handle];
  if (!cached) {
    if (draw.pixelShaderDxbc) {
      D3DCreateBlob(draw.pixelShaderDxbc->size(), &cached);
      if (cached)
        std::memcpy(cached->GetBufferPointer(), draw.pixelShaderDxbc->data(),
                    draw.pixelShaderDxbc->size());
    } else {
      cached = CompileShader(hlsl.c_str(), "ps_5_0", "main");
    }
  }
  auto ps = cached;
  if (!ps) {
    // The hooks-side probe already compiles what it emits, so reaching here
    // means the two compilers disagreed — worth saying plainly rather than
    // counting silently.
    LogError("TranslatedPSO: pixel shader failed to compile in the renderer");
    ++m_translatedFailed;
    m_translatedPSOs[key] = entry;
    return nullptr;
  }

  // The vertex stage, and the input layout that feeds it. Two shapes, because
  // the migration is per draw. With the guest's own vertex shader, one stream of
  // float4s -- one per register the shader reads, at TEXCOORD<register>, which
  // is the semantic EmitShaderHlsl declares. Without it, the passthrough stage
  // over two streams: slot 0 the stand-in vertex read only for its
  // CPU-transformed position, slot 1 the interpolator stream.
  //
  // The second shape keeps the stand-in vertex layout untouched, which is why it
  // was two streams to begin with.
  ID3DBlob* vsBlob = m_translatedVsBlob.Get();
  std::vector<D3D12_INPUT_ELEMENT_DESC> layout;
  if (key.vsHandle) {
    // The two variants of one guest vertex shader are separate compilations
    // with incompatible input signatures, so they get separate caches. Sharing
    // one keyed by handle would hand a draw the other's bytecode.
    const bool fetch = (key.flags & 16) != 0;
    auto& cachedVs = fetch ? m_translatedVsFetchBlobs[key.vsHandle]
                           : m_translatedVsBlobs[key.vsHandle];
    if (!cachedVs && draw.vertexShaderDxbc) {
      D3DCreateBlob(draw.vertexShaderDxbc->size(), &cachedVs);
      if (cachedVs)
        std::memcpy(cachedVs->GetBufferPointer(),
                    draw.vertexShaderDxbc->data(), draw.vertexShaderDxbc->size());
    } else if (!cachedVs && draw.vertexShaderHlsl) {
      cachedVs = CompileShader(draw.vertexShaderHlsl->c_str(), "vs_5_0", "main");
    }
    if (!cachedVs) {
      LogError("TranslatedPSO: vertex shader failed to compile in the renderer");
      ++m_translatedFailed;
      m_translatedPSOs[key] = entry;
      return nullptr;
    }
    vsBlob = cachedVs.Get();
    // A fetch stage declares no input elements at all: its only input is
    // SV_VertexID, and it reads everything else out of the raw buffer. The loop
    // below is a no-op for it anyway (vertexInputCount is 0), but leaving that
    // implicit would make an empty layout look like a bug rather than the
    // design.
    if (!fetch) {
      for (uint32_t i = 0; i < draw.vertexInputCount; ++i) {
        layout.push_back({"TEXCOORD", draw.vertexInputRegs[i],
                          DXGI_FORMAT_R32G32B32A32_FLOAT, 0, i * 16,
                          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0});
      }
    }
  } else {
    layout.push_back({"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0,
                      D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0});
    for (uint32_t i = 0; i < kTranslatedInterpolators; ++i) {
      layout.push_back({"TEXCOORD", i, DXGI_FORMAT_R32G32B32A32_FLOAT, 1,
                        i * 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0});
    }
  }

  D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
  pso.pRootSignature = m_translatedRootSig.Get();
  pso.VS.pShaderBytecode = vsBlob->GetBufferPointer();
  pso.VS.BytecodeLength = vsBlob->GetBufferSize();
  pso.PS.pShaderBytecode = ps->GetBufferPointer();
  pso.PS.BytecodeLength = ps->GetBufferSize();
  pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  // The guest's cull mode, carried in flags bits 5-7. Hardcoding CULL_NONE
  // here is what let a closed volume containing the camera paint the menu
  // background black -- see DrawCall::pa_su_sc_mode_cntl.
  ApplyCullBits((key.flags >> 5) & 7u, pso.RasterizerState);
  // See the note in the game PSO template above. ApplyCullBits touches only
  // CullMode and FrontCounterClockwise, so OpaquePSO and BlendedPSO inherit
  // this from m_gamePsoTemplate; the translated desc is zeroed separately and
  // needs its own.
  pso.RasterizerState.DepthClipEnable = TRUE;
  pso.SampleMask = UINT_MAX;
  pso.PrimitiveTopologyType = key.topoType;
  // MRT slot 1 when the caller bound two RTVs. NumRenderTargets and RTVFormats
  // must match the bound set exactly; the key carries what was actually bound
  // (see boundTarget1Format) rather than what the draw asked for.
  pso.NumRenderTargets = key.rtvFormat1 == DXGI_FORMAT_UNKNOWN ? 1u : 2u;
  pso.RTVFormats[0] = key.rtvFormat;
  if (key.rtvFormat1 != DXGI_FORMAT_UNKNOWN) pso.RTVFormats[1] = key.rtvFormat1;
  pso.DSVFormat = kGameDepthFormat;
  pso.SampleDesc.Count = 1;
  pso.InputLayout.pInputElementDescs = layout.data();
  pso.InputLayout.NumElements = UINT(layout.size());

  // The guest's output-merger state, exactly as the stand-in path applies it.
  // Ignoring it is what made every translated overlay an opaque rectangle.
  const bool depthEnable = (key.flags & 1u) != 0;
  const bool depthWrite = (key.flags & 2u) != 0;
  const bool colorWrite = (key.flags & 4u) == 0;
  pso.DepthStencilState.DepthEnable = depthEnable;
  pso.DepthStencilState.DepthWriteMask =
      depthWrite ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
  // zfunc, colour mask and depth clip, from the state the guest programmed
  // rather than the three constants this used to hardcode. Index 0 reproduces
  // those constants exactly, so an unchanged draw takes an unchanged path.
  ApplyOmState(OmStateAt(key.omIndex), colorWrite, pso);
  // THE PATH THAT ACTUALLY MATTERS. Phases 2 and 3 wired stencil into the opaque
  // and blended builders and left this one alone, and in a level nearly every
  // draw is translated -- so the whole thing was inert. The mutation test
  // (--d3d9_stencil_force_never) is what exposed it: 99,571 draws forced to
  // NEVER, which cannot pass a fragment, and the screen did not change.
  //
  // Read from the DRAW, not from the key: the key carries stencilIndex only to
  // keep variants apart, and the state itself already travels on the draw.
  if (draw.stencilIndex) ApplyStencil(draw.stencil, pso.DepthStencilState);
  // WHAT WE ACTUALLY HAND D3D12, once per built pipeline. Everything upstream
  // says the state is correct and the mutation test says it has no effect, so
  // the next thing worth knowing is whether the description leaving here
  // carries stencil at all. Reasoning further without this is guessing.
  {
    static uint32_t s_logged = 0;
    if (s_logged < 12) {
      ++s_logged;
      char m[256];
      std::snprintf(m, sizeof(m),
                    "TranslatedPSO stencil: idx %u enable %u func %u/%u ops "
                    "%u/%u/%u masks %02X/%02X dsvfmt %u",
                    draw.stencilIndex,
                    uint32_t(pso.DepthStencilState.StencilEnable),
                    uint32_t(pso.DepthStencilState.FrontFace.StencilFunc),
                    uint32_t(pso.DepthStencilState.BackFace.StencilFunc),
                    uint32_t(pso.DepthStencilState.FrontFace.StencilFailOp),
                    uint32_t(pso.DepthStencilState.FrontFace.StencilDepthFailOp),
                    uint32_t(pso.DepthStencilState.FrontFace.StencilPassOp),
                    uint32_t(pso.DepthStencilState.StencilReadMask),
                    uint32_t(pso.DepthStencilState.StencilWriteMask),
                    uint32_t(pso.DSVFormat));
      LogInfo(m);
    }
  }
  // The write mask is ApplyOmState's; the blend equation below is not, because
  // it also depends on key.flags bit 3 and on translatability.
  auto& rt = pso.BlendState.RenderTarget[0];
  if (key.flags & 8u) {
    const GameOmState& om = OmStateAt(key.omIndex);
    D3D12_BLEND src{}, dest{}, srcA{}, destA{};
    D3D12_BLEND_OP op{}, opA{};
    // A state that does not translate falls back to opaque rather than being
    // approximated — the same rule BlendedPSO follows.
    if (ToD3D12Blend(key.src, false, src) &&
        ToD3D12Blend(key.dest, false, dest) && ToD3D12BlendOp(key.op, op)) {
      // ALPHA FROM THE GUEST when it programmed a different equation, else
      // from the colour factors, which is what this always did. Falling back
      // rather than refusing keeps a draw whose alpha equation does not
      // translate rendering as it did instead of turning blending off.
      bool alpha_ok = false;
      if (om.separateAlpha)
        alpha_ok = ToD3D12Blend(om.srcBlendAlpha, true, srcA) &&
                   ToD3D12Blend(om.destBlendAlpha, true, destA) &&
                   ToD3D12BlendOp(om.blendOpAlpha, opA);
      if (!alpha_ok) {
        alpha_ok = ToD3D12Blend(key.src, true, srcA) &&
                   ToD3D12Blend(key.dest, true, destA);
        opA = op;
      }
      if (alpha_ok) {
        rt.BlendEnable = TRUE;
        rt.SrcBlend = src;
        rt.DestBlend = dest;
        rt.BlendOp = op;
        rt.SrcBlendAlpha = srcA;
        rt.DestBlendAlpha = destA;
        rt.BlendOpAlpha = opA;
      }
    }
  }

  const HRESULT hr =
      m_device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&entry.pso));
  if (FAILED(hr)) {
    // The RTV/DSV formats are named because they are what a PSO is most often
    // refused for, and because the depth format is a thing this project
    // changes. DEVICE_REMOVED here means the failure is somewhere else
    // entirely and every later PSO will fail too -- read the FIRST one.
    char message[224];
    std::snprintf(message, sizeof(message),
                  "TranslatedPSO: CreateGraphicsPipelineState failed hr=%s "
                  "(rtv fmt %u, dsv fmt %u, depth %u/%u)",
                  HrText(hr).c_str(), uint32_t(pso.RTVFormats[0]),
                  uint32_t(pso.DSVFormat),
                  uint32_t(pso.DepthStencilState.DepthEnable),
                  uint32_t(pso.DepthStencilState.StencilEnable));
    LogError(message);
    ++m_translatedFailed;
    m_translatedPSOs[key] = entry;
    return nullptr;
  }
  entry.failed = false;
  ++m_translatedOk;
  ID3D12PipelineState* result = entry.pso.Get();
  m_translatedPSOs[key] = std::move(entry);
  {
    const std::string msg =
        "TranslatedPSO: built pipeline for guest pixel shader (" +
        std::to_string(m_translatedOk) + " ok, " +
        std::to_string(m_translatedFailed) + " failed)";
    LogInfo(msg.c_str());
  }
  return result;
}

// Xenos ColorRenderTargetFormat -> the host format that can hold it.
//
// Read from RB_COLOR_INFO bits [16:19] at draw time. Every offscreen target used
// to be created RGBA8 regardless, which is correct for the scene (format 0) and
// destroys anything HDR: the menu's luminance chain is format 3 at 320x180 and
// format 6 from 64x64 down to 1x1, so the log-average luminance was being
// clamped to [0,1] and quantised to 8 bits.
//
// 2_10_10_10_FLOAT has no host equivalent and takes RGBA16F, which is what Xenia
// does: wider than the guest, so nothing is lost.
DXGI_FORMAT D3D12Renderer::HostColorFormat(uint32_t guestColorFormat) {
  switch (guestColorFormat) {
    case 0:   // k_8_8_8_8
    case 1:   // k_8_8_8_8_GAMMA
      return DXGI_FORMAT_R8G8B8A8_UNORM;
    case 2:   // k_2_10_10_10
    case 10:  // k_2_10_10_10_AS_10_10_10_10
      return DXGI_FORMAT_R10G10B10A2_UNORM;
    case 3:   // k_2_10_10_10_FLOAT
    case 12:  // k_2_10_10_10_FLOAT_AS_16_16_16_16
    case 7:   // k_16_16_16_16_FLOAT
      return DXGI_FORMAT_R16G16B16A16_FLOAT;
    // SIGNED FIXED POINT, -32...32 -- not UNORM. The SDK labels both of these
    // "Fixed point -32...32" (xenos.h:305, :308) and says a resolve out of them
    // is NOT bitwise equivalent to the texture format (:566).
    //
    // We resolve with a plain CopyTextureRegion, which IS bitwise -- so calling
    // these UNORM meant the guest wrote values across -32...32, we stored the
    // bits as if they spanned 0...1, and the copy carried the pattern into a
    // snapshot a shader then sampled: the menu's tonemap read 32736.0 out of the
    // scene colour and produced 40.09, saturated cyan on the bike.
    //
    // A half-float host target holds the whole range, keeps the copy inside one
    // typeless family, and needs no shader-side scale. It trades some mantissa --
    // 11 bits against the guest's 16 -- which is the right way round.
    case 4:   // k_16_16
      return DXGI_FORMAT_R16G16_FLOAT;
    case 5:   // k_16_16_16_16
      return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case 6:   // k_16_16_FLOAT
      return DXGI_FORMAT_R16G16_FLOAT;
    // 14 and 15, NOT 15 and 16. These were off by one against
    // ColorRenderTargetFormat (xenos.h:315, `k_32_FLOAT = 14, k_32_32_FLOAT =
    // 15`), so guest 14 fell through to the RGBA8 default and 15 took the
    // single-channel format. There is no guest format 16.
    //
    // What the old values cost: the terrain clipmap renders its heightmap tiles
    // into 129x129 targets declared k_32_FLOAT, and world height is metres.
    // Caught with pixel_history -- the tile shader writes 611.71 and the RGBA8
    // target stores 1.0, which is the constant world Y measured across four
    // sessions and never explained.
    //
    // KNOWN OPEN CONSEQUENCE, and why this looks like a regression on screen:
    // with real heights the clipmap writes correct near depth (0.876), and the
    // irregular meshes that actually PAINT the ground are then behind it --
    // every one depthTestFailed. Those meshes take their whole world Y from
    // three texture samples and their dominant term is a 512x512 that reads
    // min = max = 0.
    //
    // Kept anyway: this table states what Xenos does, and the old values were
    // only cancelling that second bug by putting the terrain 600 units
    // underground. Ruled out for the second half, in order: a missing seed,
    // resolve writeback, a disabled feature, an unhooked draw entry, the wrong
    // depth target, and the tiled resolve. Unchased lead: vs 0x26E72FE0 slot 0
    // samples a 2048x2048 that none of those three are.
    case 14:  // k_32_FLOAT
      return DXGI_FORMAT_R32_FLOAT;
    case 15:  // k_32_32_FLOAT
      return DXGI_FORMAT_R32G32_FLOAT;
    default:
      return DXGI_FORMAT_R8G8B8A8_UNORM;
  }
}

// The guest colour-format nibble, reported once per (object, extent, format).
//
// HostColorFormat is many-to-one and the collision matters: guest 5
// (k_16_16_16_16, signed fixed point -32...32) and guest 7 (k_16_16_16_16_FLOAT,
// a genuine half float) both become R16G16B16A16_FLOAT, so nothing downstream --
// not the resource desc, not a RenderDoc capture -- can say which the guest
// asked for. If the scene target is 5, a shader reading 0.296 is reading a guest
// 9.48, which is exactly the question the rider's green gear turns on.
//
// Deduplicated because it is called per draw; the set is tens across a run.
void LogGuestColorFormat(uint32_t object, uint32_t width, uint32_t height,
                         uint32_t guestColorFormat) {
  if (!object) return;
  static std::set<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t>> s_seen;
  if (!s_seen.emplace(object, width, height, guestColorFormat).second) return;
  REXLOG_INFO(
      "d3d12: target object 0x{:08X} {}x{} guest colour format {}{}", object,
      width, height, guestColorFormat,
      guestColorFormat == 5    ? " (k_16_16_16_16, SIGNED FIXED -32..32)"
      : guestColorFormat == 7  ? " (k_16_16_16_16_FLOAT)"
      : guestColorFormat == 3  ? " (k_2_10_10_10_FLOAT)"
      : guestColorFormat == 4  ? " (k_16_16, SIGNED FIXED -32..32)"
      : guestColorFormat == 6  ? " (k_16_16_FLOAT)"
      : guestColorFormat == 0  ? " (k_8_8_8_8)"
                               : "");
}

// Which topology GROUP a PSO must declare for this topology to be legal against
// it. Not a nicety: a LINESTRIP draw submitted to a PSO built for TRIANGLE is
// refused outright by the runtime --
//
//   D3D12 ERROR [id 611]: DrawIndexedInstanced: The primitive topology does not
//   belong to the appropriate group specified by the current pipeline state.
//
// -- and the draw renders nothing. Every PSO here was hardcoded to TRIANGLE, so
// all 944 D3DPT_LINESTRIP draws per 170,000 were silently discarded.
D3D12_PRIMITIVE_TOPOLOGY_TYPE TopologyTypeOf(D3D12_PRIMITIVE_TOPOLOGY topo) {
  switch (topo) {
    case D3D_PRIMITIVE_TOPOLOGY_POINTLIST:
      return D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
    case D3D_PRIMITIVE_TOPOLOGY_LINELIST:
    case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP:
    case D3D_PRIMITIVE_TOPOLOGY_LINELIST_ADJ:
    case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ:
      return D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
    default:
      return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  }
}

// The opaque stand-in variant for a target that is not RGBA8, or for a topology
// group other than triangles. The 32 built at startup cover the back-buffer
// format and triangles only; these are the same descriptions with RTVFormats[0]
// and PrimitiveTopologyType changed, built once each on demand.
ID3D12PipelineState* D3D12Renderer::OpaquePSO(
    uint32_t variant, DXGI_FORMAT rtvFormat, uint32_t omIndex,
    D3D12_PRIMITIVE_TOPOLOGY_TYPE topoType, uint32_t stencilIndex) {
  // m_gamePSOs is the 32 eagerly-built variants, indexed by the low five bits.
  // Cull lives in bits 5-7 and is NOT part of that array: growing it to 256
  // would build eight times the pipelines at startup to serve a state most draws
  // do not use. Bits 5-7 == 0 is exactly "cull nothing, front is CW", which is
  // the fixed state those 32 were built with.
  const uint32_t cullBits = (variant >> 5) & 7u;
  const uint32_t baseVariant = variant & 0x1Fu;
  // The 32 eagerly-built pipelines carry no stencil, so a draw that wants
  // stencil can never take that shortcut however plain the rest of its state
  // is. Missing this is how a stencil variant silently renders without stencil.
  //
  // They were also built with the output-merger state this function used to
  // hardcode -- LESS_EQUAL, RGBA, depth clip on -- so `omIndex == 0` joins the
  // same condition for exactly the same reason. Index 0 IS that state, which is
  // why the overwhelmingly common draw still takes the shortcut.
  if (stencilIndex == 0 && omIndex == 0 && cullBits == 0 &&
      topoType == D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE &&
      (rtvFormat == kBackBufferFormat || rtvFormat == DXGI_FORMAT_UNKNOWN))
    return m_gamePSOs[baseVariant].Get();
  // stencilIndex and omIndex are both capped at 64, so six bits each.
  const uint64_t key = uint64_t((uint32_t(rtvFormat) << 8) |
                                (variant & 0xFFu) |
                                (uint32_t(topoType) << 28)) |
                       (uint64_t(stencilIndex) << 32) |
                       (uint64_t(omIndex) << 40);
  if (auto it = m_gamePSOsByFormat.find(key); it != m_gamePSOsByFormat.end())
    return it->second.Get();
  if (!m_gameVsBlob || m_gamePsBlobs[0] == nullptr)
    return m_gamePSOs[baseVariant].Get();

  D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = m_gamePsoTemplate;
  pso.RTVFormats[0] = rtvFormat;
  pso.PrimitiveTopologyType = topoType;
  ApplyCullBits(cullBits, pso.RasterizerState);
  const bool depth_enable = (variant & 1u) != 0;
  const bool depth_write = depth_enable && (variant & 2u) != 0;
  const bool color_write = (variant & 4u) == 0;
  const bool textured = (variant & 8u) != 0;
  const bool yuv = (variant & 16u) != 0;
  ID3DBlob* ps = yuv ? m_gamePsBlobs[2].Get()
                     : (textured ? m_gamePsBlobs[1].Get()
                                 : m_gamePsBlobs[0].Get());
  pso.PS.pShaderBytecode = ps->GetBufferPointer();
  pso.PS.BytecodeLength = ps->GetBufferSize();
  pso.DepthStencilState = {};
  pso.DepthStencilState.DepthEnable = depth_enable;
  pso.DepthStencilState.DepthWriteMask =
      depth_write ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
  if (stencilIndex && stencilIndex < m_stencilStates.size())
    ApplyStencil(m_stencilStates[stencilIndex], pso.DepthStencilState);
  pso.BlendState.RenderTarget[0] = {};
  // After the RenderTarget[0] reset, or the write mask it sets is cleared.
  ApplyOmState(OmStateAt(omIndex), color_write, pso);

  Microsoft::WRL::ComPtr<ID3D12PipelineState> created;
  const HRESULT hr =
      m_device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&created));
  if (FAILED(hr)) {
    // Fall back to the RGBA8 variant rather than dropping the draw. It will be
    // refused by the debug layer against a mismatched RTV, which is visible,
    // whereas a silently missing draw is not.
    static uint32_t s_logged = 0;
    if (++s_logged <= 16) {
      char message[224];
      std::snprintf(message, sizeof(message),
                    "OpaquePSO: variant creation failed hr=%s (rtv fmt %u, dsv "
                    "fmt %u, topo %u, variant %u)",
                    HrText(hr).c_str(), uint32_t(rtvFormat),
                    uint32_t(pso.DSVFormat), uint32_t(topoType), variant);
      LogError(message);
    }
    return m_gamePSOs[baseVariant].Get();
  }
  auto [it, ok] = m_gamePSOsByFormat.emplace(key, std::move(created));
  return it->second.Get();
}

ID3D12PipelineState* D3D12Renderer::BlendedPSO(const BlendKey& key) {
  if (auto it = m_blendPSOs.find(key); it != m_blendPSOs.end())
    return it->second.Get();

  D3D12_BLEND src{}, dest{};
  D3D12_BLEND src_a{}, dest_a{};
  D3D12_BLEND_OP op{};
  if (!ToD3D12Blend(key.src, false, src) ||
      !ToD3D12Blend(key.dest, false, dest) ||
      !ToD3D12Blend(key.src, true, src_a) ||
      !ToD3D12Blend(key.dest, true, dest_a) ||
      !ToD3D12BlendOp(key.op, op)) {
    if (++g_blendUnmapped <= 8) {
      char message[160];
      std::snprintf(message, sizeof(message),
                    "blend state not translated: src %u dest %u op %u — drawn "
                    "opaque",
                    key.src, key.dest, key.op);
      LogInfo(message);
    }
    return nullptr;
  }
  if (m_blendPSOs.size() >= kMaxBlendPSOs) {
    if (++g_blendBudget <= 4)
      LogInfo("blend PSO cache full — remaining blended draws are opaque");
    return nullptr;
  }

D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = m_gamePsoTemplate;
  // Cull travels in pso_index bits 5-7, the same packing OpaquePSO uses.
  ApplyCullBits((key.pso_index >> 5) & 7u, pso.RasterizerState);
  pso.RTVFormats[0] = key.rtvFormat;
  pso.PrimitiveTopologyType = key.topoType;
  const bool depth_enable = (key.pso_index & 1u) != 0;
  const bool depth_write = depth_enable && (key.pso_index & 2u) != 0;
  const bool color_write = (key.pso_index & 4u) == 0;
  const bool textured = (key.pso_index & 8u) != 0;
  const bool yuv = (key.pso_index & 16u) != 0;
  ID3DBlob* ps = yuv ? m_gamePsBlobs[2].Get()
                     : (textured ? m_gamePsBlobs[1].Get()
                                 : m_gamePsBlobs[0].Get());
  pso.PS.pShaderBytecode = ps->GetBufferPointer();
  pso.PS.BytecodeLength = ps->GetBufferSize();
  pso.DepthStencilState = {};
  pso.DepthStencilState.DepthEnable = depth_enable;
  pso.DepthStencilState.DepthWriteMask =
      depth_write ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
  // Same helper as the opaque path, deliberately: a stencil variant built one
  // way here and another there would diverge only for blended draws, which is
  // invisible right up until it is not.
  if (key.stencilIndex && key.stencilIndex < m_stencilStates.size())
    ApplyStencil(m_stencilStates[key.stencilIndex], pso.DepthStencilState);

  const GameOmState& om = OmStateAt(key.omIndex);
  auto& rt = pso.BlendState.RenderTarget[0];
  rt = {};
  rt.BlendEnable = TRUE;
  rt.SrcBlend = src;
  rt.DestBlend = dest;
  rt.BlendOp = op;
  // ALPHA FROM THE GUEST when it programmed a different equation. The comment
  // this replaces said D3DRS_BLENDOPALPHA "is hooked but not carried on the
  // draw yet" -- it is carried now, in RB_BLENDCONTROL0 bits 16-26, which the
  // register read has been keeping whole in DrawCall::blend_control all along.
  D3D12_BLEND src_a2{}, dest_a2{};
  D3D12_BLEND_OP op_a{};
  if (om.separateAlpha && ToD3D12Blend(om.srcBlendAlpha, true, src_a2) &&
      ToD3D12Blend(om.destBlendAlpha, true, dest_a2) &&
      ToD3D12BlendOp(om.blendOpAlpha, op_a)) {
    rt.SrcBlendAlpha = src_a2;
    rt.DestBlendAlpha = dest_a2;
    rt.BlendOpAlpha = op_a;
  } else {
    rt.SrcBlendAlpha = src_a;
    rt.DestBlendAlpha = dest_a;
    rt.BlendOpAlpha = op;
  }
  ApplyOmState(om, color_write, pso);

  Microsoft::WRL::ComPtr<ID3D12PipelineState> created;
  const HRESULT hr =
      m_device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&created));
  if (FAILED(hr)) {
    // Rate-limited: once the device is removed every call here fails, and this
    // site alone produced 40,000 identical lines that buried the one error
    // that mattered.
    static uint32_t s_logged = 0;
    if (++s_logged <= 16) {
      char message[224];
      std::snprintf(message, sizeof(message),
                    "BlendedPSO: pipeline creation failed hr=%s — drawing "
                    "opaque (rtv fmt %u, dsv fmt %u, src %u dest %u op %u)",
                    HrText(hr).c_str(), uint32_t(pso.RTVFormats[0]),
                    uint32_t(pso.DSVFormat), key.src, key.dest, key.op);
      LogError(message);
    }
    return nullptr;
  }
  // Each distinct blend mode the guest uses, once. A frame that renders wrong
  // and logs nothing here is a frame where no draw asked to blend at all, which
  // is a different problem from a blend that came out wrong.
  {
    char message[176];
    std::snprintf(message, sizeof(message),
                  "blend PSO: src %u dest %u op %u (pso %u) — %zu cached",
                  key.src, key.dest, key.op, key.pso_index,
                  m_blendPSOs.size() + 1);
    LogInfo(message);
  }
  auto [it, _] = m_blendPSOs.emplace(key, std::move(created));
  return it->second.Get();
}
