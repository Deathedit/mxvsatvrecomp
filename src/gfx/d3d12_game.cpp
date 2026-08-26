// D3D12Renderer — game pipeline, offscreen game render target, and present.
//
// The game PSO takes a position+color+uv vertex layout with an MVP constant
// buffer. It draws the guest's own vertex/index data as fed to AddGameDraw —
// every draw the frame produced, each with its own transform and topology.
// Geometry renders into a dedicated 1280x720 render target + D32 depth buffer,
// which PresentGameFrame copies to the current swapchain backbuffer.
//
// CreateGamePipeline used to bake in a placeholder triangle, drawn until the
// guest produced geometry of its own. It was a startup output check and is gone
// as of 2026-08-07; a frame with nothing to draw now shows the clear colour.
//
// Note the PSO leaves DepthStencilState zeroed, so depth test is off and guest
// geometry at z = 1.0 is not rejected. DSVFormat is set anyway, to match the
// DSV BeginFrame binds — leaving it UNKNOWN while a D32_FLOAT view is bound is
// a debug-layer error, and the two must agree before depth can be turned on.

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
// which is the distinction that matters at a surface edge. Mirror and border
// are not modelled.
//
// MaxLOD used to be pinned to 0 because only the base mip was ever uploaded, so
// there was no chain for a filter to select from. Now there is, and the pin
// survives only for kSamplerBaseMap -- the guest's own "never minify past level
// 0", which the reference expresses the same way (MaxLOD = MinLOD, see
// xenia/gpu/d3d12/d3d12_texture_cache.cc:1086).
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
  // static one. The guest's address mode is per texture: d3d9_texture.cpp
  // decodes clamp_x/clamp_y off the fetch constant and carries them all the way
  // to HleTexturePayload, and the renderer used to discard them and sample
  // everything WRAP. A fullscreen post-process pass reaching a hair past the
  // edge then wrapped to the opposite side and linearly blended across the
  // seam - measured as a symmetric ramp to black over the last 13 of 720 rows.
  //
  // A descriptor table costs one root parameter and keeps the pixel shader and
  // the PSO table untouched; encoding the mode as PSO bits would have
  // multiplied an already 32-entry table that is built up front.
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
// because kGameTexturePS multiplies the sample by it.
//
// The overhang (y=3, x=3) is deliberate: one triangle covering the viewport has
// no diagonal seam, and the parts outside clip are discarded before they cost
// anything. uv runs to 2 to match, so the visible region still maps 0..1.
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
// Every D3D12 creation failure in this file used to log the fact and discard
// the code, which makes a bare "creation failed" indistinguishable between
// out-of-memory, an invalid format combination and a device that has already
// gone. That cost a session: a depth-format change removed the device, and the
// only evidence was `resolve snapshot: creation FAILED for 64x64` with no code
// and 40,000 lines of downstream noise after it.
//
// An earlier version of this comment claimed the debug layer and DRED were
// unavailable here because one run logged `D3D12GetDebugInterface HR=0x80004002`.
// That was WRONG and is corrected rather than deleted, because the wrong version
// was used to argue that guessing was the only option. Graphics Tools IS
// installed (d3d12SDKLayers.dll is in System32), a standalone probe gets
// ID3D12Debug, ID3D12Debug1 and both DRED settings interfaces with S_OK, and a
// later run logged "DRED enabled". Read the CURRENT run's DRED line before
// concluding anything about what is available.
//
// The HRESULT still earns its place: the debug layer is off by default in a
// release build (MX_D3D12_DEBUG=1, or 2 for GPU-based validation), so an
// ordinary run has no validation message and this is all the runtime says.
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
// Most formats view as themselves and pass straight through. TYPELESS ones
// cannot: CreateShaderResourceView rejects them and D3D12 removes the device
// with DXGI_ERROR_INVALID_CALL.
//
// That failure is worth describing because it does not look like what it is.
// The removal is triggered by an invalid CPU-side call, so there is no faulting
// GPU work and DRED reports "every command list completed; no faulting op" with
// no page fault — which reads as though the device died for no reason. Every
// later Create* call then returns DEVICE_REMOVED, so the first error in the log
// is some unrelated innocent allocation. Only the debug layer names it
// (MX_D3D12_DEBUG=1), and it is off by default.
//
// The depth entries are the ones that matter here: the game depth surface is
// R32G8X24_TYPELESS so that one allocation can carry both a DSV and an SRV, and
// its depth plane views as R32_FLOAT_X8X24_TYPELESS. R32_TYPELESS/R24G8 are
// included because the same rule governs them and a future format change
// should not have to rediscover this.
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
// INVSRCALPHA / DESTALPHA / an op that does not exist — D3DBLENDOP starts at 1,
// so a zero op alone rules that enum out. Under the Xenos values it is
// SRC_ALPHA / ONE_MINUS_SRC_ALPHA / ADD, which is ordinary UI alpha blending
// and what the overlays plainly want.
//
// Returns false for anything not listed rather than substituting a default: a
// wrong blend factor still draws, so a silent fallback would be a visible bug
// with nothing in the log pointing at it. An unmapped factor leaves the draw
// opaque — what it was before blending existed — and is counted.
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
// is the point — the stand-in path renders the game today, and the translated
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
  // Not a collision: ShaderVisibility scopes a register to a stage, so a
  // VERTEX-visible table at s0 and a PIXEL-visible one at s0 are different bind
  // points and each stage reads its own descriptors. Separate root parameters
  // rather than making the pixel tables ALL-visible, because the two shaders
  // are cached independently and their compact slot 0 IS a different guest
  // sampler -- one shared table would hand the vertex stage the pixel stage's
  // textures.
  //
  // These cannot be moved to higher registers to "keep them apart": vs_5_0 has
  // exactly 16 sampler slots, so anything at s16+ is not a register and FXC
  // rejects the shader with X4509.
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

  // t16, vertex: the guest's raw vertex buffer, for a vertex shader that
  // fetches and decodes its own attributes instead of reading input elements
  // the CPU unpacked.
  //
  // A ROOT SRV, not a table: it needs no descriptor heap slot, takes an upload
  // heap's GPU virtual address directly, and so leaves BindTranslatedTextures
  // and its block ring completely untouched. t16 because the pixel table
  // occupies t0..t15 — a register space would have been cleaner but needs
  // shader model 5.1 and this compiles vs_5_0.
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

  // The passthrough vertex shader. The guest vertex shader is not on the GPU
  // yet — the CPU interpreter still transforms every vertex — so this stage
  // only forwards the position and the interpolators the interpreter already
  // computed. Its output signature is exactly the XeInterpolants struct
  // EmitShaderHlsl declares for the pixel stage, which is what makes the two
  // link: same order, same count, same semantics.
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

  // The vertex stage, and the input layout that feeds it.
  //
  // Two shapes, because the migration is per draw. With the guest's own vertex
  // shader, one stream of float4s — one per register the shader reads, at
  // TEXCOORD<register>, which is the semantic EmitShaderHlsl declares. Without
  // it, the passthrough stage over two streams: slot 0 the stand-in vertex read
  // only for its CPU-transformed position, slot 1 the interpolator stream the
  // interpreter filled.
  //
  // The second shape keeps the stand-in vertex layout untouched, which is why
  // it was two streams to begin with: interleaving would have meant rebuilding
  // the vertex the working path depends on.
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
  pso.SampleMask = UINT_MAX;
  pso.PrimitiveTopologyType = key.topoType;
  pso.NumRenderTargets = 1;
  pso.RTVFormats[0] = key.rtvFormat;
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
  pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
  auto& rt = pso.BlendState.RenderTarget[0];
  rt.RenderTargetWriteMask =
      colorWrite ? D3D12_COLOR_WRITE_ENABLE_ALL : 0;
  if (key.flags & 8u) {
    D3D12_BLEND src{}, dest{}, srcA{}, destA{};
    D3D12_BLEND_OP op{};
    // A state that does not translate falls back to opaque rather than being
    // approximated — the same rule BlendedPSO follows.
    if (ToD3D12Blend(key.src, false, src) &&
        ToD3D12Blend(key.dest, false, dest) &&
        ToD3D12Blend(key.src, true, srcA) &&
        ToD3D12Blend(key.dest, true, destA) && ToD3D12BlendOp(key.op, op)) {
      rt.BlendEnable = TRUE;
      rt.SrcBlend = src;
      rt.DestBlend = dest;
      rt.BlendOp = op;
      rt.SrcBlendAlpha = srcA;
      rt.DestBlendAlpha = destA;
      rt.BlendOpAlpha = op;
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
// Read from RB_COLOR_INFO bits [16:19] at draw time. Every offscreen target
// used to be created RGBA8 regardless, which is correct for the scene (format
// 0) and destroys anything HDR: the menu's luminance chain is format 3 at
// 320x180/160x90 and format 6 from 64x64 down to 1x1, so the log-average
// luminance was being clamped to [0,1] and quantised to 8 bits.
//
// 2_10_10_10_FLOAT has no host equivalent and takes RGBA16F, which is what
// Xenia does: wider than the guest, so nothing is lost.
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
    // SIGNED FIXED POINT, -32...32 -- not UNORM. From the SDK, xenos.h:305 and
    // :308 label both of these "Fixed point -32...32", and xenos.h:566 says a
    // resolve out of them is NOT bitwise equivalent to the texture format:
    // "k_16_16 and k_16_16_16_16 render target formats, which are signed and
    // also have a different range, are not equivalent to the respective texture
    // formats". IsColorResolveFormatBitwiseEquivalent returns false for both.
    //
    // We resolve with a plain CopyTextureRegion, which IS bitwise -- so calling
    // these UNORM meant the guest wrote values across -32...32, we stored the
    // bits as if they spanned 0...1, and the copy carried the pattern into a
    // snapshot that a shader then sampled. Measured: the menu's tonemap read
    // 32736.0 out of the scene colour and produced 40.09, which the 8-bit
    // target clamped to saturated cyan on the bike.
    //
    // A half-float host target holds the whole -32...32 range, keeps the copy
    // inside one typeless family, and needs no shader-side scale. It trades
    // some mantissa -- 11 bits against the guest's 16 -- which is the right way
    // round: a little banding beats a 32x error and a sign flip.
    case 4:   // k_16_16
      return DXGI_FORMAT_R16G16_FLOAT;
    case 5:   // k_16_16_16_16
      return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case 6:   // k_16_16_FLOAT
      return DXGI_FORMAT_R16G16_FLOAT;
    // 14 and 15, NOT 15 and 16. These were off by one against
    // ColorRenderTargetFormat (xenia-edge xenos.h:315,
    // `k_32_FLOAT = 14, k_32_32_FLOAT = 15`), so guest 14 fell through to the
    // RGBA8 default and 15 took the single-channel format. There is no guest
    // format 16; that arm was dead.
    //
    // What the old values cost: the terrain clipmap renders its heightmap
    // tiles into 129x129 targets declared k_32_FLOAT, and world height is
    // metres, not a fraction. Caught with pixel_history at event 17410 -- the
    // tile shader writes 611.71 and the RGBA8 target stores 1.0. That is the
    // constant world Y in terrain-is-depth-rejected ("every one of 4225
    // vertices solves back to world Y = 1.000 against a camera at Y = 616"),
    // measured across four sessions and never explained. It was a UNORM8
    // clamp. 15 of a run's targets carry format 14, so this is the terrain,
    // not a decorative surface.
    //
    // KNOWN OPEN CONSEQUENCE, and the reason this looks like a regression on
    // screen: with real heights the clipmap writes correct near depth (0.876),
    // and the irregular meshes that actually PAINT the ground are then behind
    // it -- every one depthTestFailed, so the ground renders as the ambient
    // term only. Those meshes take their whole world Y from three texture
    // samples (their vertex buffer carries only grid X and Z) and their
    // dominant term is a 512x512 that reads min = max = 0.
    //
    // Kept anyway: this table states what Xenos does, and the old values were
    // only ever cancelling that second bug by putting the terrain 600 units
    // underground. Ruled out for the second half, in order: a missing seed,
    // resolve writeback, a disabled feature (dword_82D55078 = 1), an unhooked
    // draw entry (sub_82555B88 IS hooked), the wrong depth target, and the
    // tiled resolve (destpoint is honoured). Unchased lead: vs 0x26E72FE0
    // slot 0 samples a 2048x2048 at 0x11647000 that none of those three are.
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
// (k_16_16_16_16, signed fixed point -32...32) and guest 7
// (k_16_16_16_16_FLOAT, a genuine half float) both become
// R16G16B16A16_FLOAT, so nothing downstream -- not the resource desc, not a
// RenderDoc capture -- can say which one the guest asked for. That question is
// load-bearing: if the scene target is 5, values in it carry the -32...32
// range and a shader reading 0.296 is reading a guest 9.48.
//
// It came up chasing the rider's gear rendering green: its shader computes
// rcp(luminance(scene snapshot)) and saturates, and the saturate pins at 1 --
// killing the red channel -- unless that luminance exceeds 3.42. Measured 0.296.
// Whether that is an 11x error or the correct value depends entirely on this
// nibble, and there was no way to ask.
//
// Deduplicated because it is called per draw. The set is small: one entry per
// distinct target, which is tens across a run, not thousands.
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
// refused outright by the runtime —
//
//   D3D12 ERROR [id 611]: DrawIndexedInstanced: The primitive topology does not
//   belong to the appropriate group specified by the current pipeline state.
//
// — and the draw renders nothing. Every PSO here was hardcoded to TRIANGLE, so
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
    uint32_t variant, DXGI_FORMAT rtvFormat,
    D3D12_PRIMITIVE_TOPOLOGY_TYPE topoType) {
  // m_gamePSOs is the 32 eagerly-built variants, indexed by the low five bits.
  // Cull lives in bits 5-7 and is NOT part of that array: growing it to 256
  // would build eight times the pipelines at startup to serve a state most
  // draws do not use. Bits 5-7 == 0 is exactly "cull nothing, front is CW",
  // which is the fixed state those 32 were built with, so an unculled draw
  // still takes the array untouched and everything else goes on demand below.
  const uint32_t cullBits = (variant >> 5) & 7u;
  const uint32_t baseVariant = variant & 0x1Fu;
  if (cullBits == 0 && topoType == D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE &&
      (rtvFormat == kBackBufferFormat || rtvFormat == DXGI_FORMAT_UNKNOWN))
    return m_gamePSOs[baseVariant].Get();
  const uint32_t key = (uint32_t(rtvFormat) << 8) | (variant & 0xFFu) |
                       (uint32_t(topoType) << 28);
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
  pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
  pso.BlendState.RenderTarget[0] = {};
  pso.BlendState.RenderTarget[0].RenderTargetWriteMask =
      color_write ? D3D12_COLOR_WRITE_ENABLE_ALL : 0;

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
  pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;

  auto& rt = pso.BlendState.RenderTarget[0];
  rt = {};
  rt.BlendEnable = TRUE;
  rt.SrcBlend = src;
  rt.DestBlend = dest;
  rt.BlendOp = op;
  rt.SrcBlendAlpha = src_a;
  rt.DestBlendAlpha = dest_a;
  // D3DRS_BLENDOPALPHA is only consulted under SEPARATEALPHABLENDENABLE, which
  // is hooked but not carried on the draw yet; until it is, alpha follows the
  // colour equation, which is what D3D9 does when separate alpha is off.
  rt.BlendOpAlpha = op;
  rt.RenderTargetWriteMask =
      color_write ? D3D12_COLOR_WRITE_ENABLE_ALL : 0;

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
