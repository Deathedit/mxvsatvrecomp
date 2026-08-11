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

#include "gfx/d3d12_internal.h"
#include "gfx/d3d12_shaders.h"
#include "gpu/d3d9_layout.h"
#include "gpu/hle_types.h"
#include "gpu/shader_hlsl.h"  // kHlslInterpolatorLinkage, for the static_assert

#include <chrono>
#include <cstring>
#include <string>
#include <algorithm>
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
// MaxLOD is pinned to 0 because only the base mip is ever uploaded, so there is
// no mip chain for a filter to select from.
D3D12_SAMPLER_DESC D3D12Renderer::SamplerVariantDesc(uint32_t variant) {
  variant &= kSamplerClampU | kSamplerClampV | kSamplerPoint;
  D3D12_SAMPLER_DESC sd = {};
  sd.Filter = (variant & kSamplerPoint) ? D3D12_FILTER_MIN_MAG_MIP_POINT
                                        : D3D12_FILTER_MIN_MAG_MIP_LINEAR;
  sd.AddressU = (variant & kSamplerClampU) ? D3D12_TEXTURE_ADDRESS_MODE_CLAMP
                                           : D3D12_TEXTURE_ADDRESS_MODE_WRAP;
  sd.AddressV = (variant & kSamplerClampV) ? D3D12_TEXTURE_ADDRESS_MODE_CLAMP
                                           : D3D12_TEXTURE_ADDRESS_MODE_WRAP;
  sd.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sd.MaxAnisotropy = 1;
  sd.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
  sd.MinLOD = 0.0f;
  sd.MaxLOD = 0.0f;
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
    for (uint32_t i = 0; i < kSamplerBlockSlots; ++i) {
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

namespace {

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

}  // namespace

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

  D3D12_ROOT_SIGNATURE_DESC rootDesc = {};
  rootDesc.NumParameters = 5;
  rootDesc.pParameters = params;
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
    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = kMaxTranslatedBlocks * kTranslatedSamplerSlots;
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

bool D3D12Renderer::BindTranslatedTextures(const GameDraw& d,
                                           D3D12_GPU_DESCRIPTOR_HANDLE& out) {
  // A shader that fetches NO texture is allowed through. It used to be refused
  // here and at the translated gate, purely because the count was zero -- which
  // is exactly backwards: a shader sampling nothing is the one case that needs
  // no texture, and the tex*col stand-in it fell back to is the one thing that
  // does. The descriptor range still has to be filled, with null descriptors;
  // the shader declares no Texture2D at all, so nothing reads them.
  if (!m_translatedSrvHeap) return false;
  if (m_translatedBlockNext >= m_translatedBlockLimit) {
    ++m_translatedBlockExhausted;
    return false;
  }
  const uint32_t block = m_translatedBlockNext;

  // Resolve every slot BEFORE claiming the block, so a draw that cannot be
  // fully bound does not consume one and does not leave a half-written range
  // that a later draw might read.
  struct Slot {
    ID3D12Resource* resource = nullptr;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    uint32_t swizzle = 0;
    bool useSwizzle = false;
  };
  Slot slots[kTranslatedSamplerSlots];
  for (uint32_t i = 0; i < d.pixelSamplerCount; ++i) {
    if (const uint32_t object = d.pixelSampledObjects[i]) {
      // A resolve result: sample the snapshot the guest resolved into, which is
      // the same resource the stand-in path samples for this object.
      auto it = m_gameSnapshots.find(object);
      if (it == m_gameSnapshots.end() || !it->second.resource) {
        // TRIED AND REMOVED: binding a 1x1 far-plane (white) stand-in here
        // instead of failing the draw. The intent was that a depth resolve can
        // never produce a snapshot -- a depth surface is never a colour draw
        // target -- so one permanently unsatisfiable slot was discarding whole
        // draws under the all-or-nothing slot fill.
        //
        // It was measured and it does not work. 1143 of 1160 missing snapshots
        // were depth-sourced (mx_766), so gating on depth barely narrowed it:
        // this was a blanket substitution, not a targeted one, and blanket
        // substitution is what put white over the Bink logo composite. The menu
        // scene did not come back either. Removed rather than left in as a
        // plausible-looking no-op.
        // CLASSIFY, do not substitute. What the missing image would have held
        // is recorded by the resolve (m_resolveDestIsDepth) and is worth
        // counting, because it corrected a wrong reading: "no-snapshot 378"
        // sitting near "stand-in depth refused 384" in mx_958 looked like the
        // missing snapshots being overwhelmingly depth-sourced, and they are
        // not. mx_960 split 613 into depth 165 and never-resolved 448, so the
        // question is why those resolves never arrive, not what to bind in
        // their place.
        //
        // Binding a substitute was tried here and reverted with the
        // fabricatedWhite change it depended on -- see that gate. Leave the
        // draw failing, which is what keeps it out of the stand-in and off the
        // screen as white.
        const auto kind = m_resolveDestIsDepth.find(object);
        const char* kindName = "never-resolved";
        if (kind == m_resolveDestIsDepth.end())
          ++m_noSnapshotUnknown;
        else if (kind->second) {
          kindName = "depth";
          ++m_noSnapshotDepth;
        } else {
          kindName = "colour";
          ++m_noSnapshotColour;
        }
        // One line per distinct failing link. The cumulative counters say that
        // two full-screen draws are lost every menu frame, but without the
        // shader, slot and destination object they cannot say which resolve is
        // absent. Keep this bounded by the naturally small tuple population;
        // repeated frames do not produce repeated log lines.
        static std::unordered_set<std::string> s_missingLinks;
        const std::string link =
            fmt::format("{:08X}/{}/{:08X}/{}", d.pixelShaderHandle, i,
                        object, kindName);
        if (s_missingLinks.size() < 64 && s_missingLinks.insert(link).second) {
          REXLOG_INFO(
              "d3d12: translated snapshot MISSING: PS 0x{:08X}, target "
              "0x{:08X} {}x{}, compact slot {} of {}, destination texture "
              "0x{:08X}, class {}",
              d.pixelShaderHandle, d.targetObject, d.targetWidth,
              d.targetHeight, i, d.pixelSamplerCount, object, kindName);
        }
        ++m_translatedNoSnapshot;
        return false;
      }
      slots[i].resource = it->second.resource.Get();
      // From the resource, not assumed. Every snapshot was RGBA8 until depth
      // resolves started producing R32_FLOAT ones, and an RGBA8 view over an
      // R32_FLOAT resource is not merely wrong -- CreateShaderResourceView
      // rejects it as a cross-family format and D3D12 removes the device
      // (DXGI_ERROR_INVALID_CALL). That was the hang.
      slots[i].format = it->second.resource->GetDesc().Format;
      continue;
    }
    const auto& tex = d.pixelTextures[i];
    if (!tex) {
      ++m_translatedNoTexture;
      return false;
    }
    uint32_t unusedDescriptor = 0;
    // Uploads the texture and gives it its cached descriptor. That descriptor
    // is not the one bound here — it lives in a different heap — but the upload
    // and the resource it creates are exactly what this needs.
    if (!EnsureGameTexture(tex, unusedDescriptor)) {
      ++m_translatedUploadFailed;
      return false;
    }
    auto it = m_gameTextures.find(tex->key);
    if (it == m_gameTextures.end() || !it->second.resource) {
      ++m_translatedUploadFailed;
      return false;
    }
    slots[i].resource = it->second.resource.Get();
    slots[i].format = it->second.resource->GetDesc().Format;
    slots[i].swizzle = tex->swizzle;
    slots[i].useSwizzle = true;
  }

  // SLOT CENSUS. The menu's deferred lighting shader declares three textures
  // and its third sample returns (0,0,0,1) -- the exact pattern of a
  // single-channel read of zero, or of a null descriptor. Which one it is
  // cannot be told from a capture: RenderDoc reports the slot by NAME, so
  // mapping slot to resource there means brute-forcing every resource in the
  // frame. It is one line from this side, where both halves are in hand.
  //
  // Once per pixel shader handle, bounded, so it costs nothing after the first
  // sighting of each. What it answers: whether pixelSamplerCount agrees with
  // what the shader declares, and for every slot, whether it came from a
  // resolve snapshot or a CPU texture, and at what format and extent.
  if (d.pixelShaderHandle) {
    static std::unordered_set<uint32_t> s_censused;
    if (s_censused.size() < 64 && s_censused.insert(d.pixelShaderHandle).second) {
      std::string slotDesc;
      for (uint32_t i = 0; i < d.pixelSamplerCount && i < kTranslatedSamplerSlots;
           ++i) {
        if (!slots[i].resource) {
          slotDesc += fmt::format(" [{}]=NONE", i);
          continue;
        }
        const D3D12_RESOURCE_DESC rd = slots[i].resource->GetDesc();
        slotDesc += fmt::format(" [{}]={} {}x{} fmt{}{}", i,
                                slots[i].useSwizzle ? "tex" : "snap",
                                uint32_t(rd.Width), rd.Height,
                                uint32_t(slots[i].format),
                                slots[i].useSwizzle ? "" : " (no swizzle)");
      }
      REXLOG_INFO("d3d12: PS 0x{:08X} slot census: count {}, array mask 0x{:X};{}",
                  d.pixelShaderHandle, d.pixelSamplerCount,
                  d.pixelSamplerArrayMask, slotDesc);
    }
  }

  auto cpu = m_translatedSrvHeap->GetCPUDescriptorHandleForHeapStart();
  cpu.ptr += SIZE_T(block) * kTranslatedSamplerSlots * m_gameSrvDescriptorSize;
  for (uint32_t i = 0; i < kTranslatedSamplerSlots; ++i) {
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    // Slots past what the shader declares still need a valid descriptor: the
    // table's range covers all of them whether or not they are sampled. They
    // repeat slot 0 rather than being left undefined.
    const uint32_t from = i < d.pixelSamplerCount ? i : 0;
    const Slot& s = slots[from];
    // No resource at all (a shader that samples nothing): a null descriptor is
    // legal and reads as zero. It still needs a concrete format -- UNKNOWN is
    // rejected -- so give it one nothing will look at.
    if (!s.resource) {
      srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
      srv.Texture2D.MipLevels = 1;
      srv.Shader4ComponentMapping = UINT(D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING);
      m_device->CreateShaderResourceView(nullptr, &srv, cpu);
      cpu.ptr += SIZE_T(m_gameSrvDescriptorSize);
      continue;
    }
    // The dimension must be the one the SHADER declared, not the one the
    // resource happens to have: a Texture2DArray declaration read through a
    // TEXTURE2D descriptor is undefined, not merely wrong-looking.
    //
    // The two can disagree -- the shader is translated from the microcode while
    // the texture is decoded from the fetch constant, and a cube-sampling shader
    // can be handed a plain 2D texture. That case is safe without a stand-in
    // resource: a one-slice array view over a DepthOrArraySize=1 resource is
    // legal, and D3D clamps the slice index, so every face reads slice 0. Wrong
    // colour, never a garbage descriptor. Counted so it is visible.
    if ((d.pixelSamplerArrayMask >> from) & 1u) {
      const UINT16 arraySize =
          s.resource ? s.resource->GetDesc().DepthOrArraySize : 1;
      if (arraySize < 2) ++m_translatedArraySlotNot2DArray;
      srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
      srv.Texture2DArray.MipLevels = 1;
      srv.Texture2DArray.ArraySize = std::max<UINT>(arraySize, 1);
    } else {
      srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
      srv.Texture2D.MipLevels = 1;
    }
    srv.Format = s.format;
    srv.Shader4ComponentMapping =
        s.useSwizzle
            ? D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
                  (s.swizzle >> 0) & 7u, (s.swizzle >> 3) & 7u,
                  (s.swizzle >> 6) & 7u, (s.swizzle >> 9) & 7u)
            : UINT(D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING);
    m_device->CreateShaderResourceView(s.resource, &srv, cpu);
    cpu.ptr += SIZE_T(m_gameSrvDescriptorSize);
  }

  ++m_translatedBlockNext;
  out = m_translatedSrvHeap->GetGPUDescriptorHandleForHeapStart();
  out.ptr += UINT64(block) * kTranslatedSamplerSlots * m_gameSrvDescriptorSize;
  return true;
}

bool D3D12Renderer::BindTranslatedSamplers(const GameDraw& d,
                                           D3D12_GPU_DESCRIPTOR_HANDLE& out) {
  if (!m_samplerHeap) return false;

  // The configuration first, as a key. Slots past what the shader declares
  // repeat slot 0, matching how BindTranslatedTextures fills the SRV table --
  // the range covers all sixteen whether or not the shader samples them, so
  // every one needs a defined descriptor.
  uint32_t variants[kSamplerBlockSlots] = {};
  uint64_t key = 0;
  for (uint32_t i = 0; i < kSamplerBlockSlots; ++i) {
    const uint32_t slot = i < d.pixelSamplerCount ? i : 0;
    // A resolve snapshot is a host render target, not a guest texture: there is
    // no fetch constant to read a mode off, and it is sampled 1:1, so it takes
    // the clamped point variant rather than inheriting slot 0's.
    if (slot < d.pixelSampledObjects.size() && d.pixelSampledObjects[slot]) {
      variants[i] = kSamplerClampU | kSamplerClampV | kSamplerPoint;
    } else if (slot < d.pixelTextures.size() && d.pixelTextures[slot]) {
      variants[i] = SamplerVariantFor(*d.pixelTextures[slot]);
    }
    key |= uint64_t(variants[i] & 7u) << (i * 3);
  }

  if (auto it = m_samplerBlocks.find(key); it != m_samplerBlocks.end()) {
    out = m_samplerHeap->GetGPUDescriptorHandleForHeapStart();
    out.ptr += UINT64(it->second) * kSamplerBlockSlots * m_samplerDescriptorSize;
    return true;
  }
  if (m_samplerBlockNext >= kSamplerBlockCount) {
    // Out of distinct configurations. Fall back to the reserved first block
    // rather than failing the draw: its slot 0 is the plain linear-wrap variant,
    // which is what every slot got before this function existed.
    ++m_samplerBlockExhausted;
    out = m_samplerHeap->GetGPUDescriptorHandleForHeapStart();
    return true;
  }

  // Block 0 is the reserved variant block, so the caches start at 1.
  const uint32_t block = 1 + m_samplerBlockNext++;
  auto cpu = m_samplerHeap->GetCPUDescriptorHandleForHeapStart();
  cpu.ptr += SIZE_T(block) * kSamplerBlockSlots * m_samplerDescriptorSize;
  for (uint32_t i = 0; i < kSamplerBlockSlots; ++i) {
    const D3D12_SAMPLER_DESC sd = SamplerVariantDesc(variants[i]);
    m_device->CreateSampler(&sd, cpu);
    cpu.ptr += SIZE_T(m_samplerDescriptorSize);
  }
  m_samplerBlocks.emplace(key, block);
  out = m_samplerHeap->GetGPUDescriptorHandleForHeapStart();
  out.ptr += UINT64(block) * kSamplerBlockSlots * m_samplerDescriptorSize;
  return true;
}

ID3D12PipelineState* D3D12Renderer::TranslatedPSO(const TranslatedKey& key,
                                                  const std::string& hlsl,
                                                  const GameDraw& draw) {
  if (auto it = m_translatedPSOs.find(key); it != m_translatedPSOs.end())
    return it->second.failed ? nullptr : it->second.pso.Get();
  if (!m_translatedRootSig || !m_translatedVsBlob) return nullptr;
  // Past the cap a draw falls back to the stand-in rather than being dropped,
  // and nothing is cached, so the cap bounds memory without hiding shaders.
  if (m_translatedPSOs.size() >= kMaxTranslatedPSOs) return nullptr;

  TranslatedPipeline entry;
  entry.failed = true;

  // Compiled once per shader, not once per blend state: one shader commonly
  // appears under several states, and FXC is the expensive part.
  auto& cached = m_translatedPsBlobs[key.handle];
  if (!cached) cached = CompileShader(hlsl.c_str(), "ps_5_0", "main");
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
    if (!cachedVs && draw.vertexShaderHlsl)
      cachedVs = CompileShader(draw.vertexShaderHlsl->c_str(), "vs_5_0", "main");
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
  pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
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
    LogError("TranslatedPSO: CreateGraphicsPipelineState failed");
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
    case 15:  // k_32_FLOAT
      return DXGI_FORMAT_R32_FLOAT;
    case 16:  // k_32_32_FLOAT
      return DXGI_FORMAT_R32G32_FLOAT;
    default:
      return DXGI_FORMAT_R8G8B8A8_UNORM;
  }
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
  if (topoType == D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE &&
      (rtvFormat == kBackBufferFormat || rtvFormat == DXGI_FORMAT_UNKNOWN))
    return m_gamePSOs[variant].Get();
  const uint32_t key = (uint32_t(rtvFormat) << 8) | (variant & 0xFFu) |
                       (uint32_t(topoType) << 28);
  if (auto it = m_gamePSOsByFormat.find(key); it != m_gamePSOsByFormat.end())
    return it->second.Get();
  if (!m_gameVsBlob || m_gamePsBlobs[0] == nullptr)
    return m_gamePSOs[variant].Get();

  D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = m_gamePsoTemplate;
  pso.RTVFormats[0] = rtvFormat;
  pso.PrimitiveTopologyType = topoType;
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
  if (FAILED(m_device->CreateGraphicsPipelineState(&pso,
                                                   IID_PPV_ARGS(&created)))) {
    // Fall back to the RGBA8 variant rather than dropping the draw. It will be
    // refused by the debug layer against a mismatched RTV, which is visible,
    // whereas a silently missing draw is not.
    LogError("OpaquePSO: variant creation failed for a non-RGBA8 target or a "
             "non-triangle topology");
    return m_gamePSOs[variant].Get();
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
  if (FAILED(m_device->CreateGraphicsPipelineState(&pso,
                                                   IID_PPV_ARGS(&created)))) {
    LogError("BlendedPSO: pipeline creation failed — drawing opaque");
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

// Uploads Bink's plane set into reusable host textures and writes their SRVs
// into the reserved descriptors at the head of the heap. A plane's resource is
// recreated only when its dimensions change, so steady-state playback creates
// nothing per frame; only the staging copy happens each time.
bool D3D12Renderer::EnsureYuvPlanes(const GameDraw& draw,
                                    uint32_t& descriptorBase) {
  if (!m_gameSrvHeap || draw.planeCount < 3) return false;
  const uint32_t kPlanes = kMaxDrawPlanes;
  // One block per composite draw per frame in flight. Beyond the budget the
  // draw is refused rather than aliasing an earlier draw's descriptors, which
  // is the failure this striping exists to prevent.
  if (m_yuvDrawsThisFrame >= kMaxYuvDrawsPerFrame) return false;
  descriptorBase =
      kYuvPlaneDescriptorBase +
      (m_frameIndex * kMaxYuvDrawsPerFrame + m_yuvDrawsThisFrame) * kPlanes;
  ++m_yuvDrawsThisFrame;

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
    }

    // Written every frame, not only when the resource is recreated: this
    // draw's block is its own, so there is nothing to preserve in it, and the
    // resource it must name may have been recreated since the block was last
    // used three frames ago.
    {
      D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
      srv.Format = format;
      srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
      srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
      srv.Texture2D.MipLevels = 1;
      auto cpu = m_gameSrvHeap->GetCPUDescriptorHandleForHeapStart();
      cpu.ptr += SIZE_T(descriptorBase + i) * m_gameSrvDescriptorSize;
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

// Fills an existing game-texture resource from a decoded payload, and records
// which content_version it now holds.
//
// Split out of EnsureGameTexture so the first fill and every later refill are
// the same code. Refills exist because Scaleform repacks its glyph atlas under
// a stable cache key -- see HleTexturePayload::content_version.
//
// The upload buffer is per frame in flight: a refill recorded this frame must
// not write over the staging bytes an earlier frame's CopyTextureRegion may
// still be reading. The destination resource needs no such care because the
// copies are ordered against each other on the same queue.
bool D3D12Renderer::UploadGameTexture(GameTexture& entry,
                                      const mx::hle::HleTexturePayload& src) {
  if (!entry.resource || src.data.empty() || !src.row_pitch) return false;
  const D3D12_RESOURCE_DESC td = entry.resource->GetDesc();

  // One subresource per array slice. A cube arrives as six tightly packed 2D
  // images in `src.data`; the host wants each at its own aligned footprint
  // offset, so the footprints are laid out here in one pass and copied
  // slice-by-slice below.
  const uint32_t slices = std::max<uint32_t>(td.DepthOrArraySize, 1);
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprints[6] = {};
  UINT rowCounts[6] = {};
  UINT64 rowByteCounts[6] = {};
  UINT64 uploadBytes = 0;
  if (slices > std::size(footprints)) return false;
  m_device->GetCopyableFootprints(&td, 0, slices, 0, footprints, rowCounts,
                                  rowByteCounts, &uploadBytes);

  auto& upload = entry.upload[m_frameIndex % kFrameCount];
  if (!upload || upload->GetDesc().Width < uploadBytes) {
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
    if (upload) {
      // Retire rather than release: a command list does not keep the resources
      // it references alive. Same list the YUV planes use.
      RetiredFrame& r =
          (!m_retired.empty() && m_retired.back().fence == m_fenceValue)
              ? m_retired.back()
              : m_retired.emplace_back(RetiredFrame{m_fenceValue, {}});
      r.res.push_back(std::move(upload));
      upload.Reset();
    }
    if (FAILED(m_device->CreateCommittedResource(
            &uploadHeap, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&upload))))
      return false;
  }

  uint8_t* mapped = nullptr;
  if (FAILED(upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped))))
    return false;
  // Rows available per slice in the payload. The decoder packs slices back to
  // back with no padding, so slice n starts at n * srcRows * row_pitch.
  const uint32_t srcRowsTotal = uint32_t(src.data.size() / src.row_pitch);
  const uint32_t srcRowsPerSlice = srcRowsTotal / slices;
  for (uint32_t s = 0; s < slices; ++s) {
    const uint32_t copyRows =
        std::min<uint32_t>(rowCounts[s], srcRowsPerSlice);
    const size_t copyBytes =
        std::min<size_t>(src.row_pitch, size_t(rowByteCounts[s]));
    const uint8_t* srcSlice =
        src.data.data() + size_t(s) * srcRowsPerSlice * src.row_pitch;
    for (uint32_t y = 0; y < copyRows; ++y) {
      std::memcpy(mapped + footprints[s].Offset +
                      size_t(y) * footprints[s].Footprint.RowPitch,
                  srcSlice + size_t(y) * src.row_pitch, copyBytes);
    }
  }
  upload->Unmap(0, nullptr);

  D3D12_RESOURCE_BARRIER barrier = {};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = entry.resource.Get();
  barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  m_commandList->ResourceBarrier(1, &barrier);

  for (uint32_t s = 0; s < slices; ++s) {
    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = entry.resource.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = s;
    D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
    srcLoc.pResource = upload.Get();
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcLoc.PlacedFootprint = footprints[s];
    m_commandList->CopyTextureRegion(&dst, 0, 0, 0, &srcLoc, nullptr);
  }

  std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
  m_commandList->ResourceBarrier(1, &barrier);

  entry.uploadedVersion = src.content_version;
  return true;
}

bool D3D12Renderer::EnsureGameTexture(
    const std::shared_ptr<const mx::hle::HleTexturePayload>& texture,
    uint32_t& descriptorIndex) {
  if (!texture || texture->data.empty() || !m_gameSrvHeap) return false;
  if (auto it = m_gameTextures.find(texture->key); it != m_gameTextures.end()) {
    descriptorIndex = it->second.descriptorIndex;
    // The guest repacked this texture under a stable key -- a Scaleform glyph
    // atlas. Refill the existing resource rather than making a new one: the
    // key does not change, so a new resource would leak one per repack, and
    // the descriptor already published in the heap points here.
    if (it->second.uploadedVersion != texture->content_version)
      UploadGameTexture(it->second, *texture);
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
  // A cube is six array slices of a plain 2D resource -- not a D3D12 cube
  // resource, because the shader samples it by face index rather than by
  // direction. See the cube note in EmitTextureFetch.
  td.DepthOrArraySize = UINT16(std::max<uint32_t>(texture->array_size, 1));
  td.MipLevels = 1;
  td.Format = format;
  td.SampleDesc.Count = 1;
  td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  D3D12_HEAP_PROPERTIES defaultHeap = {};
  defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
  // Created in the state UploadGameTexture expects to find it in, so the one
  // upload path serves both the first fill and every later refill.
  if (FAILED(m_device->CreateCommittedResource(
          &defaultHeap, D3D12_HEAP_FLAG_NONE, &td,
          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
          IID_PPV_ARGS(&entry.resource))))
    return false;
  if (!UploadGameTexture(entry, *texture)) return false;

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

void D3D12Renderer::RetireResource(
    Microsoft::WRL::ComPtr<ID3D12Resource>&& res) {
  if (!res) return;
  RetiredFrame& r = (!m_retired.empty() && m_retired.back().fence == m_fenceValue)
                        ? m_retired.back()
                        : m_retired.emplace_back(RetiredFrame{m_fenceValue, {}});
  r.res.push_back(std::move(res));
}

bool D3D12Renderer::CreatePooledSurface(GameRenderTarget& entry, uint32_t width,
                                        uint32_t height,
                                        const PooledSurfaceSpec& spec,
                                        uint32_t reuseSrvIndex) {
  D3D12_HEAP_PROPERTIES hp = {};
  hp.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC rd = {};
  rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  rd.Width = width;
  rd.Height = height;
  rd.DepthOrArraySize = 1;
  rd.MipLevels = 1;
  rd.Format = spec.resourceFormat;
  rd.SampleDesc.Count = 1;
  rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  rd.Flags = spec.flags;
  if (FAILED(m_device->CreateCommittedResource(
          &hp, D3D12_HEAP_FLAG_NONE, &rd, spec.initialState, spec.clear,
          IID_PPV_ARGS(&entry.resource))))
    return false;
  // Recorded HERE, from what the resource was actually created with, rather
  // than by each caller. EnsureGameRenderTarget set it and EnsureGameSnapshot
  // did not, so every snapshot claimed to be R8G8B8A8_UNORM — the struct
  // default — whatever it really was. Anything reasoning about a snapshot's
  // format was reading a lie, which is half of why an HDR resolve reached
  // CopyTextureRegion with a mismatched destination.
  entry.format = spec.resourceFormat;
  // Claimed only once the resource exists. Claiming before the call leaks a
  // descriptor on every failure, and the caller retries the same object every
  // frame — that drained the heap to 1024/1024 in about twenty seconds.
  entry.srvIndex =
      reuseSrvIndex != UINT32_MAX ? reuseSrvIndex : m_nextGameSrvDescriptor++;

  D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
  srv.Format = spec.srvFormat;
  srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv.Texture2D.MipLevels = 1;
  auto cpu = m_gameSrvHeap->GetCPUDescriptorHandleForHeapStart();
  cpu.ptr += SIZE_T(entry.srvIndex) * m_gameSrvDescriptorSize;
  m_device->CreateShaderResourceView(entry.resource.Get(), &srv, cpu);
  return true;
}

D3D12Renderer::GameRenderTarget* D3D12Renderer::EnsureGameRenderTarget(
    uint32_t object, uint32_t width, uint32_t height, uint32_t edramBase,
    DXGI_FORMAT format) {
  if (!object || !width || !height || width > 8192 || height > 8192 ||
      !m_gameRtvHeap || !m_gameSrvHeap)
    return nullptr;
  uint32_t reuseRtvIndex = UINT32_MAX;
  uint32_t reuseSrvIndex = UINT32_MAX;
  if (auto it = m_gameRenderTargets.find(object);
      it != m_gameRenderTargets.end()) {
    // A format change is handled exactly like a size change -- the resource has
    // to be recreated either way, and refusing would make the object
    // unroutable for the rest of the run.
    if (it->second.width != width || it->second.height != height ||
        it->second.format != format) {
      // A guest heap address reused at a different size. This used to refuse,
      // which made the object unroutable for the REST OF THE RUN — every later
      // draw onto it fell back to the main target and overpainted the scene,
      // which is the exact bug offscreen routing exists to prevent. Measured
      // 271 such refusals in one run.
      //
      // Replace instead. Both descriptor slots are reusable in place; only the
      // resource changes. The old one goes through the retirement list because
      // the GPU may still be reading it — releasing inline made every later
      // create of that size fail (see the snapshot path).
      ++m_rtRejectResized;
      reuseRtvIndex = it->second.rtvIndex;
      reuseSrvIndex = it->second.srvIndex;
      RetireResource(std::move(it->second.resource));
      m_gameRenderTargets.erase(it);
    } else {
      return &it->second;
    }
  }
  // Budget exhausted. Counted separately from every other refusal because the
  // consequence is invisible: the caller falls back to the main target and the
  // draw overpaints the scene, which is exactly the bug offscreen routing was
  // built to fix. m_gameRenderTargets is never evicted, so once this trips it
  // stays tripped.
  if (reuseSrvIndex == UINT32_MAX &&
      (m_gameRenderTargets.size() >= kMaxGameRenderTargets ||
       m_nextGameSrvDescriptor >= kMaxGameTextures)) {
    ++m_rtRejectBudget;
    return nullptr;
  }

  GameRenderTarget entry;
  entry.width = width;
  entry.height = height;
  entry.edramBase = edramBase;
  entry.format = format;
  entry.rtvIndex = reuseRtvIndex != UINT32_MAX
                       ? reuseRtvIndex
                       : uint32_t(m_gameRenderTargets.size()) + 1;

  D3D12_CLEAR_VALUE cv = {};
  cv.Format = format;
  cv.Color[0] = 0.0f;
  cv.Color[1] = 0.0f;
  cv.Color[2] = 0.0f;
  cv.Color[3] = 0.0f;
  PooledSurfaceSpec spec;
  spec.resourceFormat = format;
  spec.srvFormat = format;
  spec.flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  spec.clear = &cv;
  if (!CreatePooledSurface(entry, width, height, spec, reuseSrvIndex))
    return nullptr;

  auto rtv = m_gameRtvHeap->GetCPUDescriptorHandleForHeapStart();
  rtv.ptr += SIZE_T(entry.rtvIndex) * m_gameRtvDescriptorSize;
  m_device->CreateRenderTargetView(entry.resource.Get(), nullptr, rtv);

  // Clear once, HERE, at creation — not only when a draw first lands on it.
  //
  // The per-frame clear below is gated on usedThisFrame, deliberately, so a
  // target carries its contents across frames; that is what a resolve source
  // needs. But it means a target that is created and never drawn into is never
  // cleared at all, and CreateCommittedResource does not guarantee zeroed
  // memory. The resolve branch copies such a target into a snapshot regardless,
  // and a compositor quad then paints that snapshot over the frame — so
  // undefined GPU memory reaches the screen, which is both a real defect and
  // exactly the kind of thing that shows up as a flat uniform colour.
  {
    D3D12_RESOURCE_BARRIER toRt = {};
    toRt.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toRt.Transition.pResource = entry.resource.Get();
    toRt.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    toRt.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    toRt.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_commandList->ResourceBarrier(1, &toRt);
    static const float kZero[4] = {0, 0, 0, 0};
    m_commandList->ClearRenderTargetView(rtv, kZero, 0, nullptr);
    D3D12_RESOURCE_BARRIER back = toRt;
    back.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    back.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    m_commandList->ResourceBarrier(1, &back);
  }

  auto [it, inserted] = m_gameRenderTargets.emplace(object, std::move(entry));
  if (!inserted) return nullptr;
  char message[192];
  std::snprintf(message, sizeof(message),
                "game render target: object 0x%08X %ux%u cache %zu",
                object, width, height, m_gameRenderTargets.size());
  LogInfo(message);
  return &it->second;
}

// A depth surface for one guest depth-stencil object.
//
// Offscreen colour targets were rendered with OMSetRenderTargets(..., nullptr)
// and tDepthEnable forced false, so the whole deferred scene ran with no depth
// buffer. That is not only wrong for depth testing: the guest RESOLVES its
// depth surface to a texture to reconstruct world position in the lighting
// pass, and with nothing to copy every one of those resolves missed. Measured
// on the menu: source 0x2123C208 missed 224 times in one sample window, and it
// is the depth surface (Resolve source slot 4).
//
// R32_TYPELESS so one resource serves both views. D3D12 will not give a
// D32_FLOAT resource a colour SRV, and a resolve has to be sampled.
D3D12Renderer::GameRenderTarget* D3D12Renderer::EnsureGameDepthTarget(
    uint32_t object, uint32_t width, uint32_t height, uint32_t edramBase) {
  if (!object || !width || !height || width > 8192 || height > 8192 ||
      !m_gameDepthDsvHeap || !m_gameSrvHeap)
    return nullptr;
  uint32_t reuseDsvIndex = UINT32_MAX;
  uint32_t reuseSrvIndex = UINT32_MAX;
  if (auto it = m_gameDepthTargets.find(object);
      it != m_gameDepthTargets.end()) {
    if (it->second.width == width && it->second.height == height) {
      it->second.edramBase = edramBase;
      return &it->second;
    }
    // Same policy as the colour targets: replace in place rather than refuse,
    // so a guest address reused at another size does not become unroutable.
    ++m_rtRejectResized;
    reuseDsvIndex = it->second.rtvIndex;
    reuseSrvIndex = it->second.srvIndex;
    RetireResource(std::move(it->second.resource));
    m_gameDepthTargets.erase(it);
  }
  if (reuseSrvIndex == UINT32_MAX &&
      (m_gameDepthTargets.size() >= kMaxGameDepthTargets ||
       m_nextGameSrvDescriptor >= kMaxGameTextures)) {
    ++m_rtRejectBudget;
    return nullptr;
  }

  GameRenderTarget entry;
  entry.width = width;
  entry.height = height;
  entry.edramBase = edramBase;
  // rtvIndex doubles as the DSV index here — same bookkeeping, different heap.
  entry.rtvIndex = reuseDsvIndex != UINT32_MAX
                       ? reuseDsvIndex
                       : uint32_t(m_gameDepthTargets.size()) + 1;

  D3D12_CLEAR_VALUE cv = {};
  cv.Format = kGameDepthFormat;
  cv.DepthStencil.Depth = 1.0f;
  cv.DepthStencil.Stencil = 0;
  PooledSurfaceSpec spec;
  spec.resourceFormat = DXGI_FORMAT_R32_TYPELESS;
  spec.srvFormat = DXGI_FORMAT_R32_FLOAT;
  spec.flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
  spec.initialState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
  spec.clear = &cv;
  if (!CreatePooledSurface(entry, width, height, spec, reuseSrvIndex))
    return nullptr;
  entry.state = D3D12_RESOURCE_STATE_DEPTH_WRITE;

  D3D12_DEPTH_STENCIL_VIEW_DESC dsv = {};
  dsv.Format = kGameDepthFormat;
  dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
  auto handle = m_gameDepthDsvHeap->GetCPUDescriptorHandleForHeapStart();
  handle.ptr += SIZE_T(entry.rtvIndex) * m_gameDsvDescriptorSize;
  m_device->CreateDepthStencilView(entry.resource.Get(), &dsv, handle);

  auto [it, inserted] = m_gameDepthTargets.emplace(object, std::move(entry));
  if (!inserted) return nullptr;
  char message[192];
  std::snprintf(message, sizeof(message),
                "game depth target: object 0x%08X %ux%u cache %zu", object,
                width, height, m_gameDepthTargets.size());
  LogInfo(message);
  return &it->second;
}

// A snapshot is an offscreen surface like any other — same struct, same
// creation, same budget — so this defers to EnsureGameRenderTarget's storage
// rather than duplicating it. The only difference is the key: destination
// texture object, not source target object. That is what stops six resolves out
// of one shared scratch surface from aliasing each other.
// -- see EnsureGameSnapshot below, past the two exposure-readback helpers.

// Hand the frame-old 1x1 exposure result to the guest.
//
// Called at the top of a frame, so the slot about to be reused has already
// been waited out by MoveToNextFrame and the map is guaranteed non-blocking.
// Only the R channel is published: the reduction targets are R16G16_FLOAT and
// the guest's destination is a single FMT_16_FLOAT texel, so the second
// channel has nowhere to go.
void D3D12Renderer::DrainLuminanceReadback() {
  const uint32_t count = m_luminancePending[m_frameIndex];
  if (!count) return;
  m_luminancePending[m_frameIndex] = 0;
  ID3D12Resource* rb = m_luminanceReadback[m_frameIndex].Get();
  if (!rb) return;
  void* mapped = nullptr;
  D3D12_RANGE readRange = {0, 4};
  if (FAILED(rb->Map(0, &readRange, &mapped)) || !mapped) return;
  uint32_t texel = 0;
  {
    std::lock_guard<std::mutex> lk(mx::hle::g_luminanceReadbackMutex);
    mx::hle::g_luminanceReadbackCount = count;
    for (uint32_t i = 0; i < count; ++i) {
      uint32_t bits = 0;
      std::memcpy(&bits,
                  static_cast<const uint8_t*>(mapped) +
                      size_t(i) * kLuminanceSlotStride,
                  sizeof(bits));
      mx::hle::g_luminanceReadbacks[i].destObject =
          m_luminanceDestObject[m_frameIndex][i];
      mx::hle::g_luminanceReadbacks[i].bits = bits & 0xFFFFu;
      if (i == 0) texel = bits;
    }
  }
  D3D12_RANGE noWrite = {0, 0};
  rb->Unmap(0, &noWrite);
  const uint32_t half = texel & 0xFFFFu;
  // DIAG (remove before commit): whether the GPU's own answer is non-zero.
  // If this only ever reports 0x0000 the write-back is faithful and the
  // reduction chain is the thing producing nothing -- a different defect from
  // the value not reaching the guest.
  if (half != m_luminanceLastBits && m_luminanceReadbacks < 40) {
    char msg[160];
    std::snprintf(msg, sizeof(msg),
                  "EXPOSURE readback #%llu texel 0x%08X -> half 0x%04X",
                  static_cast<unsigned long long>(m_luminanceReadbacks), texel,
                  half);
    LogInfo(msg);
  }
  m_luminanceLastBits = half;
  ++m_luminanceReadbacks;
  // Bumped last so a consumer that reads the sequence first can never pair a
  // new sequence with a half-written set.
  mx::hle::g_luminanceReadbackSeq.fetch_add(1, std::memory_order_release);
}

// Copy a freshly written 1x1 snapshot into this frame's readback buffer.
//
// The snapshot is in PIXEL_SHADER_RESOURCE when this runs -- the resolve path
// just put it there -- and is returned to it, because the composite samples it
// later in the same frame.
void D3D12Renderer::QueueLuminanceReadback(GameRenderTarget* snap,
                                           uint32_t destObject) {
  if (!snap || !snap->resource || !destObject) return;
  // ONE copy per frame, which is the shape that was measured working. A
  // four-slot version -- four placed footprints into one buffer at 512-byte
  // offsets -- failed Close every time, and rather than keep guessing at why,
  // this rotates across destinations instead: each frame samples the next 1x1
  // resolve in turn, so over a handful of frames every buffer of the guest's
  // ping-pong gets its own measurement. The adaptation filters over time
  // anyway, so a value that refreshes every few frames is in keeping with it.
  if (m_luminancePending[m_frameIndex]) return;
  const D3D12_RESOURCE_DESC sd = snap->resource->GetDesc();
  // A typeless resource cannot be the source of a buffer copy -- the footprint
  // has no way to say what the bytes mean. Depth snapshots are R32_TYPELESS,
  // and a 1x1 depth resolve would otherwise land here and record a copy the
  // runtime rejects at Close, which kills the command list for the rest of the
  // run. The reduction chain is R16G16_FLOAT (confirmed in intro-all-white.rdc)
  // so nothing legitimate is turned away by demanding a typed format.
  if (sd.Format == DXGI_FORMAT_R32_TYPELESS ||
      sd.Format == DXGI_FORMAT_R24G8_TYPELESS ||
      sd.Format == DXGI_FORMAT_UNKNOWN)
    return;
  // The caller checked the RESOLVE was 1x1; this checks the RESOURCE is, since
  // EnsureGameSnapshot grows and never shrinks. Copying a whole subresource
  // means the two have to agree.
  if (sd.Width != 1 || sd.Height != 1 || sd.DepthOrArraySize != 1 ||
      sd.MipLevels != 1 || sd.SampleDesc.Count != 1)
    return;
  // Ask the device for the footprint rather than deriving one. The first cut
  // hand-computed a 256-byte row and sized the buffer to match; the row pitch
  // was right but D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT is 512, so the
  // destination was too small for a placed footprint. The debug layer never
  // flagged it -- the runtime deferred the complaint to Close(), which then
  // failed every frame and took the command list with it.
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout = {};
  UINT numRows = 0;
  UINT64 rowSize = 0, totalBytes = 0;
  m_device->GetCopyableFootprints(&sd, 0, 1, 0, &layout, &numRows, &rowSize,
                                  &totalBytes);
  if (!totalBytes || totalBytes > kLuminanceSlotStride) return;
  auto& rb = m_luminanceReadback[m_frameIndex];
  if (!rb) return;  // Created at init; absent only if that failed.
  D3D12_RESOURCE_BARRIER pre = {};
  pre.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  pre.Transition.pResource = snap->resource.Get();
  pre.Transition.StateBefore = snap->state;
  pre.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
  pre.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  m_commandList->ResourceBarrier(1, &pre);
  D3D12_TEXTURE_COPY_LOCATION src = {};
  src.pResource = snap->resource.Get();
  src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  src.SubresourceIndex = 0;
  D3D12_TEXTURE_COPY_LOCATION dst = {};
  dst.pResource = rb.Get();
  dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  dst.PlacedFootprint = layout;
  // No source box: the footprint above already describes the whole subresource,
  // and the guard below only lets a genuinely 1x1 resource through, so a box
  // could only ever restate what the footprint says. Capture layers replay
  // boxed copies less reliably than whole-subresource ones, and there is
  // nothing to express here that needs one.
  m_commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  D3D12_RESOURCE_BARRIER post = pre;
  post.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
  post.Transition.StateAfter = snap->state;
  m_commandList->ResourceBarrier(1, &post);
  m_luminanceDestObject[m_frameIndex][0] = destObject;
  m_luminancePending[m_frameIndex] = 1;
}

D3D12Renderer::GameRenderTarget* D3D12Renderer::EnsureGameSnapshot(
    uint32_t destTexture, uint32_t width, uint32_t height,
    DXGI_FORMAT format) {
  if (!destTexture || !width || !height) return nullptr;
  // GROW to cover, never resize to match. A snapshot is assembled from one or
  // more resolve bands: the scene arrives as 1280x640 then 1280x80 at y=640,
  // two EDRAM bands of one 1280x720 surface (see hle_types.h). Sizing to the
  // band that happened to arrive last is what produced the white screen — the
  // 640-line band was destroyed microseconds after being copied and the whole
  // scene became an 80-line strip, stretched over every compositor quad.
  //
  // An entry that already covers the request is returned untouched, so the
  // steady state after the first frame is no allocation at all. That also ends
  // the 2x-per-frame create-and-destroy of a 3.5MB committed resource — 2711
  // of them in one run — which was the RenderPipeline stall.
  uint32_t reuseSrvIndex = UINT32_MAX;
  Microsoft::WRL::ComPtr<ID3D12Resource> growFrom;
  D3D12_RESOURCE_STATES growFromState = D3D12_RESOURCE_STATE_COMMON;
  uint32_t growWidth = 0, growHeight = 0;
  if (auto it = m_gameSnapshots.find(destTexture); it != m_gameSnapshots.end()) {
    // FORMAT, not only extent. One guest destination texture can be resolved
    // into from sources of different formats over a run, and this cache
    // returned the first snapshot ever made for it — so a later resolve out of
    // an R16G16B16A16 HDR target copied into an R8G8B8A8 snapshot, which
    // CopyTextureRegion rejects outright:
    //
    //   D3D12 ERROR [id 874]: CopyTextureRegion: The source and destination
    //   resource formats are incompatible. The source format is
    //   R16G16B16A16_TYPELESS and the destination format is R8G8B8A8_TYPELESS.
    //
    // An invalid call makes the whole command list fail to Close, and until
    // EndFrame learned to rebuild the list that killed the renderer for the
    // rest of the run — 4-5 SECONDS per frame, which is the 0.40 fps menu.
    // Recovery alone does not fix it: the same copy is re-issued every frame.
    const bool format_ok = it->second.format == format;
    if (format_ok && it->second.width >= width && it->second.height >= height)
      return &it->second;
    ++m_rtRejectResized;
    reuseSrvIndex = it->second.srvIndex;
    // The union, so a later band cannot shrink away an earlier one.
    growWidth = it->second.width;
    growHeight = it->second.height;
    width = std::max(width, growWidth);
    height = std::max(height, growHeight);
    if (format_ok) {
      growFrom = it->second.resource;
      growFromState = it->second.state;
    } else {
      // Nothing to carry forward across a format change — the copy that would
      // do it is the very one that is illegal. The bands already resolved are
      // lost for one frame and re-resolved into the new format after it.
      ++m_snapshotFormatChanged;
    }
    RetireResource(std::move(it->second.resource));
    m_gameSnapshots.erase(it);
  }
  if (reuseSrvIndex == UINT32_MAX &&
      (m_gameSnapshots.size() + m_gameRenderTargets.size() >=
           kMaxGameRenderTargets ||
       m_nextGameSrvDescriptor >= kMaxGameTextures)) {
    ++m_rtRejectBudget;
    return nullptr;
  }

  GameRenderTarget entry;
  entry.width = width;
  entry.height = height;
  // No RTV: a snapshot is only ever a copy destination and a shader resource.
  // Sharing the RTV heap's index space would eat slots the offscreen targets
  // need, and nothing renders into it.
  entry.rtvIndex = 0;

  // A depth resolve lands in an R32_FLOAT snapshot: CopyTextureRegion accepts
  // it from the R32_TYPELESS depth resource because they share a typeless
  // family, and the shader reads the depth in .x.
  PooledSurfaceSpec snapSpec;
  snapSpec.resourceFormat = format;
  snapSpec.srvFormat = format;
  if (!CreatePooledSurface(entry, width, height, snapSpec, reuseSrvIndex)) {
    // Loudly, and without having spent a descriptor — CreatePooledSurface
    // claims the index only after the resource exists. Claiming it before
    // leaked one on every failure, silently, because this path used to return
    // with no log; the caller retries the same texture next frame, so it
    // drained the heap to 1024/1024 in about twenty seconds and every snapshot
    // after that was refused for budget.
    ++m_snapshotCreateFailed;
    static bool s_logged = false;
    if (!s_logged) {
      s_logged = true;
      char failure[160];
      std::snprintf(failure, sizeof(failure),
                    "resolve snapshot: creation FAILED for %ux%u — snapshots "
                    "for this size are unavailable", width, height);
      LogError(failure);
    }
    return nullptr;
  }
  // Carry the old contents forward. Growing must not discard the bands already
  // resolved into this texture — the band that triggered the growth covers only
  // its own slice, so without this the rest of the image would be undefined
  // memory every time the extent changes. The old resource is already retired,
  // so it stays alive until the fence passes; leaving it in COPY_SOURCE is fine
  // because nothing will read it again.
  if (growFrom) {
    D3D12_RESOURCE_BARRIER pre[2] = {};
    pre[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    pre[0].Transition.pResource = growFrom.Get();
    pre[0].Transition.StateBefore = growFromState;
    pre[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    pre[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    pre[1] = pre[0];
    pre[1].Transition.pResource = entry.resource.Get();
    pre[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    pre[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    m_commandList->ResourceBarrier(2, pre);

    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = entry.resource.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION src = dst;
    src.pResource = growFrom.Get();
    D3D12_BOX box = {};
    box.right = growWidth;
    box.bottom = growHeight;
    box.back = 1;
    m_commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);

    D3D12_RESOURCE_BARRIER post = pre[1];
    post.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    post.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    m_commandList->ResourceBarrier(1, &post);
  }

  auto [it, inserted] = m_gameSnapshots.emplace(destTexture, std::move(entry));
  if (!inserted) return nullptr;
  char message[192];
  std::snprintf(message, sizeof(message),
                "resolve snapshot: texture 0x%08X %ux%u cache %zu",
                destTexture, width, height, m_gameSnapshots.size());
  LogInfo(message);
  return &it->second;
}

void D3D12Renderer::AddGameResolve(uint32_t destTexture,
                                   uint32_t sourceObject,
                                   int32_t destX, int32_t destY, int32_t srcX1,
                                   int32_t srcY1, int32_t srcX2,
                                   int32_t srcY2, uint32_t destWidth,
                                   uint32_t destHeight, bool sourceIsDepth,
                                   uint32_t sourceBase, uint32_t sourceWidth,
                                   uint32_t sourceHeight) {
  if (!destTexture || !sourceObject) return;
  // Counted, not silent. Resolves are interleaved through the stream, so a
  // frame that overruns kMaxGameDraws loses the tail — and the tail is mostly
  // resolves. A silent drop here looks exactly like a working fix whose
  // snapshots have simply stopped updating.
  if (m_gameDraws.size() >= kMaxGameDraws) {
    ++m_resolvesDroppedFull;
    return;
  }
  GameDraw d;
  d.resolveDest = destTexture;
  d.resolveSource = sourceObject;
  d.resolveSourceIsDepth = sourceIsDepth;
  d.resolveSourceBase = sourceBase;
  d.resolveSourceWidth = sourceWidth;
  d.resolveSourceHeight = sourceHeight;
  d.resolveDestX = destX;
  d.resolveDestY = destY;
  d.resolveSrcX1 = srcX1;
  d.resolveSrcY1 = srcY1;
  d.resolveSrcX2 = srcX2;
  d.resolveSrcY2 = srcY2;
  d.resolveDestWidth = destWidth;
  d.resolveDestHeight = destHeight;
  m_gameDraws.push_back(std::move(d));
}

void D3D12Renderer::AddGameClear(uint32_t targetObject, uint32_t targetWidth,
                                 uint32_t targetHeight, uint32_t targetBase,
                                 uint32_t targetColorFormat, uint32_t color,
                                 const float* floatColor) {
  if (!targetObject || !targetWidth || !targetHeight) return;
  if (m_gameDraws.size() >= kMaxGameDraws) return;
  GameDraw d;
  d.colorClear = true;
  d.clearColor = color;
  d.targetObject = targetObject;
  d.targetWidth = targetWidth;
  d.targetHeight = targetHeight;
  d.targetBase = targetBase;
  d.targetColorFormat = targetColorFormat;
  if (floatColor) {
    d.clearColorIsFloat = true;
    std::memcpy(d.clearColorFloat.data(), floatColor,
                sizeof(d.clearColorFloat));
  }
  m_gameDraws.push_back(std::move(d));
}

void D3D12Renderer::RenderGameFrame() {
  if (!m_hasGamePipeline) return;
  m_commandList->SetGraphicsRootSignature(m_gameRootSig.Get());
  m_commandList->SetPipelineState(m_gamePSOs[0].Get());

  ID3D12DescriptorHeap* heaps[] = {m_gameSrvHeap.Get(), m_samplerHeap.Get()};
  m_commandList->SetDescriptorHeaps(2, heaps);
  // Composite draws take their plane descriptor blocks in order within a frame.
  m_yuvDrawsThisFrame = 0;
  ++m_gameFrame;
  // Before anything is recorded into this slot: the copy issued the last time
  // round is complete, and the guest is waiting on its exposure.
  DrainLuminanceReadback();
  // This frame in flight takes its own slice of the descriptor blocks, so the
  // window resets every host frame rather than only when the guest hands off a
  // new draw list. See kTranslatedBlocksPerFrame.
  m_translatedBlockNext = m_frameIndex * kTranslatedBlocksPerFrame;
  m_translatedBlockLimit = m_translatedBlockNext + kTranslatedBlocksPerFrame;
  for (auto& [object, target] : m_gameRenderTargets)
    target.usedThisFrame = false;
  for (auto& [object, target] : m_gameDepthTargets)
    target.usedThisFrame = false;

  // Which 1x1 resolve of this frame we are looking at, for the rotation in
  // QueueLuminanceReadback.
  uint32_t oneByOneSeen = 0;
  uint32_t boundTargetObject = 0;  // zero is the final m_gameRT.
  // Zero means "no DSV bound", which is a distinct state from any depth object.
  uint32_t boundDepthObject = 0;
  static const float kOffscreenClear[4] = {0, 0, 0, 0};
  std::unordered_set<uint32_t> sampledTargets;
  sampledTargets.reserve(m_gameDraws.size());
  std::unordered_set<uint32_t> resolveSources;
  for (const auto& d : m_gameDraws) {
    if (d.sampledTargetObject &&
        d.sampledTargetObject != d.targetObject)
      sampledTargets.insert(d.sampledTargetObject);
    if (d.resolveDest && d.resolveSource) resolveSources.insert(d.resolveSource);
  }
  // Carry both sets forward across frames, and decide routing on the union.
  //
  // The routing decision below is made when a surface is DRAWN INTO, but the
  // facts it needs -- will this be resolved, will a later draw sample it -- are
  // only known when the resolve or the sample arrives, which is often a
  // different frame. Built per frame, the sets answer for the wrong frame: a
  // surface drawn in frame N and resolved in N+1 is not a resolve source in N,
  // so N routes it to m_gameRT, which is cleared every frame, and N+1's resolve
  // finds no offscreen entry and copies nothing. Its snapshot stays at the
  // clear colour, and every draw sampling it paints black.
  //
  // That is the other side of the note below: "25 of 33 resolves had sources
  // with no draws at all that frame". The contents were established earlier --
  // and were thrown away earlier, for exactly this reason.
  //
  // History is the right basis because these are properties of a surface's
  // ROLE, which is stable: a surface the guest resolves once is a resolve
  // target it will resolve again. Cost is bounded and visible -- more surfaces
  // qualify for an offscreen target, capped by the same budget, and the
  // "refused: budget" figure on the routing line is what says if it bites.
  for (uint32_t object : resolveSources) m_everResolveSource.insert(object);
  for (uint32_t object : sampledTargets) m_everSampledTarget.insert(object);
  for (const auto& d : m_gameDraws)
    if (d.targetObject) m_everDrawTarget.insert(d.targetObject);
  // Reset per frame. The final whole-backbuffer colour resolve is the exact
  // image handed to VdSwap, while the last target drawn into is only a fallback
  // for lists that contain no such resolve.
  m_presentResolveTexture = 0;
  m_presentSourceObject = 0;
  std::unordered_set<uint32_t> fullSizeTargets;
  uint32_t fullSizeDraws = 0;
  // Per surface, in the order first drawn into. "Two surfaces, 8 draws" cannot
  // say whether one is the scene and the other a one-draw overlay, or whether
  // the work is split evenly -- and presenting the LAST one written is only
  // right in the first case. The order matters as much as the counts: a
  // compositor writes its output last, a UI layer is written last over a scene
  // that was finished earlier, and those want opposite choices.
  std::vector<std::pair<uint32_t, uint32_t>> fullSizeOrder;
  for (const auto& d : m_gameDraws) {
    // A resolve: snapshot the source target as it stands right now, so draws
    // recorded after this point sample these contents rather than whatever the
    // shared surface holds by the end of the frame. Draws nothing.
    if (d.resolveDest) {
      // What KIND of image this destination holds, recorded before any of the
      // branching below can refuse the resolve.
      //
      // A slot naming a destination we have no snapshot for used to fail the
      // whole draw, and could not do better because nothing said whether the
      // missing image was depth or colour -- so the only substitute available
      // was a blanket one, and a blanket substitute is what put white over the
      // Bink logo (see BindTranslatedTextures). The guest tells us on every
      // resolve; it just was not being kept. Recorded for EVERY resolve that
      // arrives, including ones that go on to be dropped for a missing source,
      // because those are precisely the destinations that end up with no
      // snapshot.
      m_resolveDestIsDepth[d.resolveDest] = d.resolveSourceIsDepth;
      // Where the source actually rendered. Resolve sources are routed
      // offscreen (isResolveSource, below), so this normally finds a surface of
      // the source's own — which is the point: offscreen targets are only
      // cleared when something draws into them, so they carry their contents
      // across frames. A resolve needs exactly that.
      ID3D12Resource* srcRes = nullptr;
      GameRenderTarget* srcEntry = nullptr;
      uint32_t srcWidth = 0, srcHeight = 0;
      D3D12_RESOURCE_STATES srcState = D3D12_RESOURCE_STATE_RENDER_TARGET;
      // The snapshot takes the source's format: a depth resolve has to land in
      // R32_FLOAT, not RGBA8, for CopyTextureRegion to accept it.
      DXGI_FORMAT snapFormat = kBackBufferFormat;
      if (auto it = m_gameRenderTargets.find(d.resolveSource);
          !d.resolveSourceIsDepth && it != m_gameRenderTargets.end()) {
        srcEntry = &it->second;
        srcRes = srcEntry->resource.Get();
        srcWidth = srcEntry->width;
        srcHeight = srcEntry->height;
        srcState = srcEntry->state;
        // CopyTextureRegion requires the two formats to agree, so the snapshot
        // takes the source's -- which stopped being RGBA8 once the HDR targets
        // got their real formats.
        snapFormat = srcEntry->format;
      } else if (auto dit = m_gameDepthTargets.find(d.resolveSource);
                 dit != m_gameDepthTargets.end()) {
        // A DEPTH resolve. The guest reads its depth buffer back as a texture
        // to reconstruct world position in the deferred lighting pass, and
        // Resolve names the depth surface by object exactly as it names a
        // colour one (source slot 4). Looking only in the colour map is what
        // made every one of these miss.
        srcEntry = &dit->second;
        srcRes = srcEntry->resource.Get();
        srcWidth = srcEntry->width;
        srcHeight = srcEntry->height;
        srcState = srcEntry->state;
        snapFormat = DXGI_FORMAT_R32_FLOAT;
        ++m_depthResolves;
      }
      // ALIASED COLOUR SOURCE. The guest gives one EDRAM allocation several
      // surface objects: 0x2653FDA0 is what its draws name as their target and
      // 0x2653FF20 is what the resolve names as its source, both 129x129 at
      // base 0x2D0 through surface descriptor 0x028000A0. Object identity
      // cannot connect them, so the resolve found nothing, the snapshot never
      // appeared, and every draw sampling it was discarded -- including the
      // draws into that same target, which is a permanent deadlock: the
      // snapshot only exists once the draw has run, and the draw only runs once
      // the snapshot exists.
      //
      // Match on the EDRAM base and the extent instead, which is what actually
      // identifies the storage. Both must agree, and the base must be non-zero,
      // so a target at an unknown base cannot capture an unrelated resolve.
      // Matched on the SOURCE's own extent, not the destination texture's: a
      // 640x360 source whose destination extent could not be decoded reads as
      // 0x0 and matched nothing, which left 0x22414860 losing 20 resolves a
      // window after the 129x129 pair was already fixed.
      //
      // Two passes, exact before containment. Exact is the 129x129 and 640x360
      // pairs -- one allocation named twice at one size. Containment is the
      // MULTISAMPLE case: 0x21DFCA60 is 640x360 with 4x MSAA in its surface
      // word (0x0A020280) and 0x2123C9BC is 640x720 at 1x (0x0A000280), both at
      // base 0x2D0 pitch 640, and only the 640x720 is ever drawn into -- which
      // the colour-pool dump confirmed. We render everything at 1x, so the
      // samples the guest would resolve down are not there to resolve; taking
      // the top 640x360 rows of the surface that IS drawn is the closest thing
      // we hold. PROVISIONAL: this is the one step here not established from
      // evidence, so it is counted separately and judged on the picture. If the
      // luminance it produces looks wrong, the row mapping is what to revisit.
      if (!srcRes && !d.resolveSourceIsDepth && d.resolveSourceBase &&
          d.resolveSourceWidth && d.resolveSourceHeight) {
        GameRenderTarget* exact = nullptr;
        GameRenderTarget* contains = nullptr;
        for (auto& [obj, t] : m_gameRenderTargets) {
          if (!t.resource || t.edramBase != d.resolveSourceBase ||
              t.width != d.resolveSourceWidth)
            continue;
          if (t.height == d.resolveSourceHeight) {
            exact = &t;
            break;
          }
          if (t.height > d.resolveSourceHeight && t.everDrawn &&
              (!contains || t.height < contains->height))
            contains = &t;
        }
        if (GameRenderTarget* hit = exact ? exact : contains) {
          srcEntry = hit;
          srcRes = hit->resource.Get();
          srcWidth = d.resolveSourceWidth;
          srcHeight = d.resolveSourceHeight;
          srcState = hit->state;
          snapFormat = hit->format;
          if (exact)
            ++m_aliasedSourceResolves;
          else
            ++m_containedSourceResolves;
        }
      }
      // A BANDED depth resolve, which no object-identity lookup can satisfy.
      // The shadow pass renders 768x1024 as two EDRAM bands -- 768x640 at base
      // 0x580 and 768x384 at base 0x710 -- and then resolves the whole image
      // through a THIRD surface object that aliases band 0's base and that no
      // draw ever binds. Measured: both bands are in the depth pool and drawn,
      // and the object the resolve names (0x214C5130) is in neither.
      //
      // Stitch them by EDRAM base, which is the only thing that says which band
      // is on top. The bands must start at the resolve's own base and their
      // heights must add up to the destination exactly; anything else is not a
      // banding of this surface and is left to fail as before rather than
      // assembled on a guess.
      if (!srcRes && d.resolveSourceIsDepth && d.resolveDestWidth &&
          d.resolveDestHeight) {
        std::vector<GameRenderTarget*> bands;
        for (auto& [obj, t] : m_gameDepthTargets) {
          if (t.resource && t.width == d.resolveDestWidth &&
              t.edramBase >= d.resolveSourceBase)
            bands.push_back(&t);
        }
        std::sort(bands.begin(), bands.end(),
                  [](const GameRenderTarget* a, const GameRenderTarget* b) {
                    return a->edramBase < b->edramBase;
                  });
        uint32_t total = 0;
        for (const GameRenderTarget* b : bands) total += b->height;
        if (bands.size() >= 2 && total == d.resolveDestHeight &&
            bands.front()->edramBase == d.resolveSourceBase) {
          GameRenderTarget* snap =
              EnsureGameSnapshot(d.resolveDest, d.resolveDestWidth,
                                 d.resolveDestHeight, DXGI_FORMAT_R32_FLOAT);
          if (snap) {
            D3D12_RESOURCE_BARRIER toDest = {};
            toDest.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            toDest.Transition.pResource = snap->resource.Get();
            toDest.Transition.StateBefore = snap->state;
            toDest.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            toDest.Transition.Subresource =
                D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            m_commandList->ResourceBarrier(1, &toDest);
            uint32_t dstY = 0;
            for (GameRenderTarget* b : bands) {
              D3D12_RESOURCE_BARRIER toSrc = {};
              toSrc.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
              toSrc.Transition.pResource = b->resource.Get();
              toSrc.Transition.StateBefore = b->state;
              toSrc.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
              toSrc.Transition.Subresource =
                  D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
              m_commandList->ResourceBarrier(1, &toSrc);
              D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
              dstLoc.pResource = snap->resource.Get();
              dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
              dstLoc.SubresourceIndex = 0;
              D3D12_TEXTURE_COPY_LOCATION srcLoc = dstLoc;
              srcLoc.pResource = b->resource.Get();
              D3D12_BOX box = {};
              box.right = b->width;
              box.bottom = b->height;
              box.back = 1;
              m_commandList->CopyTextureRegion(&dstLoc, 0, dstY, 0, &srcLoc,
                                               &box);
              dstY += b->height;
              // Straight back to DEPTH_WRITE: the next frame's shadow pass
              // renders into these again, and that is the state it expects.
              D3D12_RESOURCE_BARRIER back = toSrc;
              back.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
              back.Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
              m_commandList->ResourceBarrier(1, &back);
              b->state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
            }
            D3D12_RESOURCE_BARRIER toSrv = toDest;
            toSrv.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            toSrv.Transition.StateAfter =
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            m_commandList->ResourceBarrier(1, &toSrv);
            snap->state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            snap->everDrawn = true;
            snap->stale = false;
            snap->lastCopyFrame = m_gameFrame;
            ++m_snapshotCopies;
            ++m_depthResolves;
            ++m_depthBandResolves;
            continue;
          }
        }
      }
      if (!srcRes) {
        // The source has no offscreen surface — it was refused one (budget or a
        // size change), so it rendered into m_gameRT with everything else.
        //
        // Do NOT copy from m_gameRT as a fallback. It is cleared every frame and
        // accumulates every logical surface, so it does not hold the source's
        // contents at resolve time: measured, 25 of 33 resolves in a frame have
        // sources with no draws at all that frame, their contents established
        // earlier. Copying from it was tried and captured the clear colour —
        // the logo screen turned {0.05, 0.08, 0.18}.
        //
        // Refusing leaves the draw on the old aliased path, which is wrong but
        // is what it had before. A steady non-zero count here means targets are
        // being refused offscreen surfaces upstream — read the routing line.
        ++m_snapshotMissingSource;
        // Which sources, and how often. Counted rather than logged on first
        // sighting: the first miss for any source happens in the opening frames
        // before anything has drawn into it, so a once-per-source line reports
        // startup state as if it were the steady state -- which is exactly the
        // mistake that produced a confident and wrong "never a draw target".
        // The tally is dumped with the periodic counters, with the status read
        // at dump time.
        ++m_missingSourceCounts[d.resolveSource];
        // The guest asked for this image to be refreshed and it was not. Any
        // snapshot already held for this destination is now a stale earlier
        // frame; mark it so the sampling path refuses it rather than blitting a
        // previous frame over this one. Measured as the intro overlap: ~600
        // dropped refreshes per sample window with ZERO offscreen refusals, so
        // these are sources we have no entry for at all, not budget drops.
        if (auto st = m_gameSnapshots.find(d.resolveDest);
            st != m_gameSnapshots.end()) {
          st->second.stale = true;
        }
        continue;
      }
      // Snapshotting a target nothing has ever drawn into copies a blank
      // surface. The copy still happens — refusing it would freeze the previous
      // snapshot, which is worse — but it is counted, because a large number
      // here means compositor quads are painting blanks over the frame and the
      // real defect is upstream, in whatever should have rendered that target.
      if (srcEntry && !srcEntry->everDrawn) ++m_snapshotBlankSource;
      // Which part of the source this band takes. The guest's rectangle is in
      // the coordinates of the full image, not of the band's own surface, so a
      // band at y 640..720 arrives as a rectangle our 1280x80 source resource
      // cannot contain. Clamping and then falling back to the whole source is
      // what makes both conventions land correctly: a genuine sub-rectangle
      // survives the clamp, an out-of-range band one does not and takes its
      // whole surface — which is exactly the band.
      uint32_t sx = 0, sy = 0, copyW = srcWidth, copyH = srcHeight;
      if (d.resolveSrcX2 > d.resolveSrcX1 && d.resolveSrcY2 > d.resolveSrcY1 &&
          uint32_t(d.resolveSrcX2) <= srcWidth &&
          uint32_t(d.resolveSrcY2) <= srcHeight && d.resolveSrcX1 >= 0 &&
          d.resolveSrcY1 >= 0) {
        sx = uint32_t(d.resolveSrcX1);
        sy = uint32_t(d.resolveSrcY1);
        copyW = uint32_t(d.resolveSrcX2) - sx;
        copyH = uint32_t(d.resolveSrcY2) - sy;
      }
      const uint32_t dx = d.resolveDestX > 0 ? uint32_t(d.resolveDestX) : 0;
      const uint32_t dy = d.resolveDestY > 0 ? uint32_t(d.resolveDestY) : 0;
      // The snapshot must be the size of the DESTINATION TEXTURE, not of the
      // region this resolve happens to cover.
      //
      // Covering was right for a banded resolve, which eventually fills the
      // whole image, and wrong for an atlas, which never does: the menu scene's
      // 2048x2048 atlas is built from repeated 256x256 sub-rect resolves, so
      // the first one created a 256x256 snapshot. The shader samples a texture
      // the guest declares as 2048x2048, so normalized UVs map [0,1] across our
      // 256x256 resource — every fetch lands at 1/8 scale and anything packed
      // outside the top-left corner cannot be reached at all.
      //
      // The covered region is still the floor, so a destination whose extent we
      // could not decode behaves exactly as before rather than shrinking.
      const uint32_t snapW = std::max(d.resolveDestWidth, dx + copyW);
      const uint32_t snapH = std::max(d.resolveDestHeight, dy + copyH);
      GameRenderTarget* snap =
          EnsureGameSnapshot(d.resolveDest, snapW, snapH, snapFormat);
      if (!snap) continue;
      D3D12_RESOURCE_BARRIER pre[2] = {};
      pre[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      pre[0].Transition.pResource = srcRes;
      pre[0].Transition.StateBefore = srcState;
      pre[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
      pre[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
      pre[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      pre[1].Transition.pResource = snap->resource.Get();
      pre[1].Transition.StateBefore = snap->state;
      pre[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
      pre[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
      m_commandList->ResourceBarrier(2, pre);
      // A placed region copy, not CopyResource. CopyResource demands identical
      // dimensions, so it could only ever express "this band IS the whole
      // texture" — and the snapshot had to be resized to the band to satisfy
      // it, which is the bug. Copying the band to its own offset lets the two
      // bands of a tiled resolve assemble into one image.
      D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
      dstLoc.pResource = snap->resource.Get();
      dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      dstLoc.SubresourceIndex = 0;
      D3D12_TEXTURE_COPY_LOCATION srcLoc = dstLoc;
      srcLoc.pResource = srcRes;
      D3D12_BOX srcBox = {};
      srcBox.left = sx;
      srcBox.top = sy;
      srcBox.right = sx + copyW;
      srcBox.bottom = sy + copyH;
      srcBox.back = 1;
      m_commandList->CopyTextureRegion(&dstLoc, dx, dy, 0, &srcLoc, &srcBox);
      // Back to shader-resource for both: the source may be rendered into again
      // later in the same frame, and the snapshot is about to be sampled.
      // Put the source back where it was. An offscreen entry can go to
      // shader-resource and be transitioned again on demand, but m_gameRT must
      // return to RENDER_TARGET: the rest of the frame keeps drawing into it,
      // and PresentGameFrame's own RT->COPY_SOURCE barrier declares that as the
      // before-state. The snapshot goes to shader-resource either way — being
      // sampled is all it exists for.
      D3D12_RESOURCE_BARRIER post[2] = {pre[0], pre[1]};
      post[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
      post[0].Transition.StateAfter =
          srcEntry ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
                   : D3D12_RESOURCE_STATE_RENDER_TARGET;
      post[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
      post[1].Transition.StateAfter =
          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
      m_commandList->ResourceBarrier(2, post);
      if (srcEntry)
        srcEntry->state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
      snap->state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
      // Refreshed: whatever earlier drop marked this stale is now irrelevant.
      snap->stale = false;
      snap->lastCopyFrame = m_gameFrame;
      // A D3D9 frame resolves its completed backbuffer immediately before
      // VdSwap. Because resolves keep their guest order in m_gameDraws, the
      // last successful whole 1280x720 colour resolve is the frame to present.
      // Presenting d.resolveSource instead is incorrect: that object is shared
      // scratch storage and later post-processing draws may overwrite it after
      // the resolve has preserved the intended image.
      if (!d.resolveSourceIsDepth && snapW == 1280 && snapH == 720 && dx == 0 &&
          dy == 0 && copyW == 1280 && copyH == 720) {
        m_presentResolveTexture = d.resolveDest;
      }
      // The target is bound again by whichever draw follows; forcing a rebind
      // keeps that from being skipped because boundTargetObject still matches.
      boundTargetObject = 0xFFFFFFFFu;
      ++m_snapshotCopies;
      // The guest LOADS the 1x1 exposure result out of guest memory instead of
      // sampling it, so this one destination has to travel back to the CPU.
      // See DrainLuminanceReadback and mx::hle::g_luminanceReadbackBits.
      if (snapW == 1 && snapH == 1 &&
          (oneByOneSeen++ % kMaxLuminanceSlots) ==
              (m_gameFrame % kMaxLuminanceSlots))
        QueueLuminanceReadback(snap, d.resolveDest);
      continue;
    }
    // A full-surface D3D9 colour clear. This is an ordered command, not setup:
    // the guest may resolve the cleared target immediately with no draw in
    // between (the front-end default-texture atlas does exactly that).
    if (d.colorClear) {
      const bool wantsOffscreen =
          d.targetObject && d.targetWidth && d.targetHeight &&
          (resolveSources.contains(d.targetObject) ||
           sampledTargets.contains(d.targetObject) ||
           d.targetWidth != 1280 || d.targetHeight != 720);
      GameRenderTarget* clearTarget = nullptr;
      D3D12_CPU_DESCRIPTOR_HANDLE rtv = {};
      if (wantsOffscreen) {
        clearTarget = EnsureGameRenderTarget(
            d.targetObject, d.targetWidth, d.targetHeight, d.targetBase,
            HostColorFormat(d.targetColorFormat));
        if (!clearTarget) continue;
        if (clearTarget->state != D3D12_RESOURCE_STATE_RENDER_TARGET) {
          D3D12_RESOURCE_BARRIER barrier = {};
          barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
          barrier.Transition.pResource = clearTarget->resource.Get();
          barrier.Transition.StateBefore = clearTarget->state;
          barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
          barrier.Transition.Subresource =
              D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
          m_commandList->ResourceBarrier(1, &barrier);
          clearTarget->state = D3D12_RESOURCE_STATE_RENDER_TARGET;
        }
        rtv = m_gameRtvHeap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += SIZE_T(clearTarget->rtvIndex) * m_gameRtvDescriptorSize;
      } else {
        rtv = m_gameRtvHeap->GetCPUDescriptorHandleForHeapStart();
      }
      const float packedRgba[4] = {
          float((d.clearColor >> 16) & 0xFFu) / 255.0f,
          float((d.clearColor >> 8) & 0xFFu) / 255.0f,
          float(d.clearColor & 0xFFu) / 255.0f,
          float((d.clearColor >> 24) & 0xFFu) / 255.0f};
      const float* rgba =
          d.clearColorIsFloat ? d.clearColorFloat.data() : packedRgba;
      m_commandList->ClearRenderTargetView(rtv, rgba, 0, nullptr);
      if (clearTarget) {
        clearTarget->usedThisFrame = true;
        clearTarget->everDrawn = true;
      }
      // Clear does not bind through the normal draw path. Force the following
      // draw to restore its RTV/DSV and viewport.
      boundTargetObject = 0xFFFFFFFFu;
      boundDepthObject = 0xFFFFFFFFu;
      continue;
    }

    // Keep only the unsampled final 1280x720 surface on m_gameRT so
    // PresentGameFrame remains an exact-size copy. A full-size scene target
    // that a later compositor samples is still offscreen and needs its own SRV;
    // classifying solely by dimensions made that target alias m_gameRT and
    // left the final draw with nothing it could legally sample.
    GameRenderTarget* drawTarget = nullptr;
    // Declared out here because the depth-state decision below needs it.
    GameRenderTarget* depthTarget = nullptr;
    const bool feedsLaterDraw =
        d.targetObject && sampledTargets.contains(d.targetObject);
    // A resolve source needs storage that outlives the frame. Measured: of 33
    // resolves in one frame, 25 had sources with no draws at all that frame —
    // their contents were established earlier. m_gameRT is cleared every frame,
    // so it can never hold them at resolve time, which is why snapshotting from
    // it produced the clear colour. Offscreen targets are only cleared when
    // something actually draws into them (usedThisFrame below), so they carry
    // contents forward, which is exactly the semantics a resolve source needs.
    // This was disabled for a while to isolate a white-screen regression. That
    // regression has since been traced to something else entirely — exempting
    // sampled_render_target_object from the colourless filter, which admitted
    // 4000 fullscreen quads that still painted opaque white (51f3c80). That
    // filter no longer exists, and this routing was never the cause.
    // A resolve source needs storage that outlives the frame. Measured: of 33
    // resolves in one frame, 25 had sources with no draws at all that frame —
    // their contents were established earlier. m_gameRT is cleared every frame
    // so it can never hold them at resolve time, which is why snapshotting from
    // it produced the clear colour. Offscreen targets are cleared only when
    // something draws into them, so they carry contents forward.
    //
    // This was bisected off while chasing a 3s/frame stall. The stall was a
    // descriptor leak in the snapshot path, not this — measured with routing
    // off and the stall still present.
    const bool isResolveSource =
        d.targetObject && resolveSources.contains(d.targetObject);
    // A DEPTH-ONLY pass has no colour target at all -- the shadow map is
    // 768x1024 with "colour target now 0x00000000" -- so d.targetObject is 0
    // and every one of its draws fell through to the main render target. Two
    // consequences, both measured: the shadow geometry overpainted the
    // backbuffer, and the depth surface (0x213DCC30) was never created, so the
    // guest's depth resolve out of it found no source. That missing snapshot
    // then discarded every draw sampling the shadow map at s15 -- 396 of them,
    // which is the whole 320x180 luminance pass, which is why the exposure
    // divides by zero.
    //
    // Route it like any other offscreen pass, keyed by the DEPTH object, with a
    // scratch colour target at the same extent. The scratch target is not
    // wasted: every PSO declares NumRenderTargets = 1, so binding no RTV at all
    // would be invalid work against a pipeline that expects one.
    const bool depthOnlyPass =
        !d.targetObject && d.depthObject && d.depthWidth && d.depthHeight;
    const uint32_t targetObject = depthOnlyPass ? d.depthObject : d.targetObject;
    const uint32_t targetWidth = depthOnlyPass ? d.depthWidth : d.targetWidth;
    const uint32_t targetHeight = depthOnlyPass ? d.depthHeight : d.targetHeight;
    const bool wantsOffscreen =
        targetObject && targetWidth && targetHeight &&
        (depthOnlyPass || feedsLaterDraw || isResolveSource ||
         targetWidth != 1280 || targetHeight != 720);
    if (wantsOffscreen) {
      // A depth-only pass has no colour format of its own; its scratch target
      // is never sampled, so RGBA8 is as good as anything.
      drawTarget = EnsureGameRenderTarget(
          targetObject, targetWidth, targetHeight, d.targetBase,
          depthOnlyPass ? kBackBufferFormat
                        : HostColorFormat(d.targetColorFormat));
      // The last guest-backbuffer-sized target drawn into this frame is the
      // finished scene, and what present should show. Tracked by last write
      // rather than by object identity because which surface ends up on screen
      // is a property of draw order, not of any particular target.
      if (drawTarget && !depthOnlyPass && d.targetWidth == 1280 &&
          d.targetHeight == 720) {
        m_presentSourceObject = d.targetObject;
        // Present shows the LAST guest-backbuffer-sized surface written this
        // frame. That is only correct if there is exactly one. Seven 1280x720
        // surfaces are live, and if the guest builds the scene across several
        // and composites them, presenting one of them shows a single layer --
        // which is what a white frame with content on one band looks like.
        // Count the distinct ones per frame rather than assume either way.
        if (fullSizeTargets.insert(d.targetObject).second)
          fullSizeOrder.emplace_back(d.targetObject, 0);
        for (auto& e : fullSizeOrder)
          if (e.first == d.targetObject) ++e.second;
        ++fullSizeDraws;
      }
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
      // Depth for this offscreen target, at the target's own extent. The guest
      // pairs one depth surface with a colour target of the same size, and
      // sizing to the colour target is what keeps the DSV and RTV agreeing when
      // the guest's own depth extent is unreadable.
      // Sized from the DEPTH surface's own declared extent, never from the
      // colour target's.
      //
      // Sizing it from the colour target collapsed the frame -- 2664 offscreen
      // draws fell to 133, resolve copies rose from 1750 to 30166 with hits
      // falling to 19 -- because the same depth object then demanded two
      // different sizes and was retired and recreated on every alternation. It
      // demands two sizes because DeviceState is thread_local: a draw carries
      // whatever depth surface ITS thread last saw, which is not always the one
      // paired with the colour target it is drawing into.
      //
      // The guest itself pairs one depth surface per colour target at matching
      // extents, including a separate depth object for each EDRAM band
      // (1280x640 and 1280x80 have their own, distinct from the 1280x720). So
      // each depth object has exactly one size and this never resizes.
      //
      // Binding only on an exact extent match keeps that guarantee honest: a
      // stale pairing skips depth for that draw rather than binding a DSV whose
      // size disagrees with the RTV.
      // TEMPORARILY ENABLED for a RenderDoc capture of the collapsed frame.
      //
      // Sized from the depth surface's OWN extent, which removed the resize
      // churn entirely (resized 13 -> 0) and let 3423 depth resolves run from 4
      // surfaces, with 0x2123C208 leaving the missing-source offenders. The
      // frame still collapses to ~125 offscreen draws and ~20 translated,
      // identically to the first attempt -- so neither the churn nor depth
      // testing is the cause, and routing (which precedes all depth state) is
      // what falls. The capture is to find what stops being submitted.
      depthTarget = (d.depthObject && d.depthWidth == drawTarget->width &&
                     d.depthHeight == drawTarget->height)
                        ? EnsureGameDepthTarget(d.depthObject, d.depthWidth,
                                                d.depthHeight, d.depthBase)
                        : nullptr;
      if (depthTarget && depthTarget->state !=
                             D3D12_RESOURCE_STATE_DEPTH_WRITE) {
        D3D12_RESOURCE_BARRIER toDepth = {};
        toDepth.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toDepth.Transition.pResource = depthTarget->resource.Get();
        toDepth.Transition.StateBefore = depthTarget->state;
        toDepth.Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        toDepth.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        m_commandList->ResourceBarrier(1, &toDepth);
        depthTarget->state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
      }
      // The DEPTH binding is part of what makes this pair current, not just the
      // colour target. Gating the rebind on the colour object alone let two
      // consecutive draws onto the same colour target keep the first one's
      // depth binding -- including the case where the first bound no DSV at all
      // and the second runs a depth-enabled PSO against it, which is invalid
      // work and hangs the device (DXGI_ERROR_DEVICE_HUNG at ~frame 75).
      const uint32_t wantDepthObject = depthTarget ? d.depthObject : 0;
      if (boundTargetObject != targetObject ||
          boundDepthObject != wantDepthObject) {
        auto rtv = m_gameRtvHeap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += SIZE_T(drawTarget->rtvIndex) * m_gameRtvDescriptorSize;
        D3D12_CPU_DESCRIPTOR_HANDLE dsv = {};
        if (depthTarget) {
          dsv = m_gameDepthDsvHeap->GetCPUDescriptorHandleForHeapStart();
          dsv.ptr += SIZE_T(depthTarget->rtvIndex) * m_gameDsvDescriptorSize;
        }
        m_commandList->OMSetRenderTargets(1, &rtv, FALSE,
                                          depthTarget ? &dsv : nullptr);
        boundDepthObject = wantDepthObject;
        D3D12_VIEWPORT viewport = {};
        viewport.Width = float(drawTarget->width);
        viewport.Height = float(drawTarget->height);
        viewport.MinDepth = 0.0f;
        viewport.MaxDepth = 1.0f;
        D3D12_RECT scissor = {0, 0, LONG(drawTarget->width),
                              LONG(drawTarget->height)};
        m_commandList->RSSetViewports(1, &viewport);
        m_commandList->RSSetScissorRects(1, &scissor);
        boundTargetObject = targetObject;
      }
      if (!drawTarget->usedThisFrame) {
        auto rtv = m_gameRtvHeap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += SIZE_T(drawTarget->rtvIndex) * m_gameRtvDescriptorSize;
        m_commandList->ClearRenderTargetView(rtv, kOffscreenClear, 0, nullptr);
        drawTarget->usedThisFrame = true;
        drawTarget->everDrawn = true;
      }
      // Depth is cleared on its own schedule: one depth surface serves several
      // colour targets in a pass, so clearing it with each of them would wipe
      // what the previous target established.
      if (depthTarget && !depthTarget->usedThisFrame) {
        D3D12_CPU_DESCRIPTOR_HANDLE dsv =
            m_gameDepthDsvHeap->GetCPUDescriptorHandleForHeapStart();
        dsv.ptr += SIZE_T(depthTarget->rtvIndex) * m_gameDsvDescriptorSize;
        m_commandList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f,
                                             0, 0, nullptr);
        depthTarget->usedThisFrame = true;
        depthTarget->everDrawn = true;
      }
    } else if (boundTargetObject != 0) {
      auto rtv = m_gameRtvHeap->GetCPUDescriptorHandleForHeapStart();
      auto dsv = m_gameDsvHeap->GetCPUDescriptorHandleForHeapStart();
      m_commandList->OMSetRenderTargets(1, &rtv, FALSE,
                                        m_gameDepth ? &dsv : nullptr);
      m_commandList->RSSetViewports(1, &m_viewport);
      m_commandList->RSSetScissorRects(1, &m_scissorRect);
      boundTargetObject = 0;
      // The main target binds m_gameDepth, which is not one of the per-object
      // depth surfaces, so the offscreen path must treat this as "not mine".
      boundDepthObject = 0;
    }

    uint32_t textureDescriptor = 0;
    bool textured = false;
    if (d.sampledTargetObject) {
      // Prefer the snapshot taken when the guest resolved into this specific
      // texture.
      //
      // The snapshot lookup deliberately runs even when the draw's own target
      // is the one that was resolved from. The old `sampled != target` guard
      // existed because a resource cannot be read and written in the same
      // draw — but a snapshot is a separate resource captured earlier, so that
      // hazard is gone, and the guard was rejecting the common case: one shared
      // scratch surface is both what the draw renders into and what it samples
      // a previous resolve of. With the guard in place this measured hits 0.
      //
      // The fallback keeps the old live-surface path, under the old guard,
      // because it is still a read-write hazard. It is wrong whenever more than
      // one texture resolves out of that target, but it is what draws got
      // before snapshots existed, so it beats binding nothing and turning them
      // black. A large steady m_snapshotFallbacks means resolves are being
      // dropped upstream.
      GameRenderTarget* sampledPtr = nullptr;
      //
      // A STALE snapshot is refused outright. It holds a complete earlier frame
      // at full screen size, so binding it does not degrade the draw — it
      // replaces the frame. Falling through to the untextured path instead lets
      // the fabricated-colour gate below drop the draw, which shows what is
      // underneath: incomplete, but not a previous frame painted over the
      // current one.
      // A DEPTH snapshot is not a colour source. The tex*col stand-in has one
      // texture and multiplies it by the vertex colour, so binding an
      // R32_FLOAT depth image gives (depth, 0, 0, 1) * white -- a flat red
      // sheet over the frame, which is exactly what appeared over the menu the
      // first time depth resolves started succeeding. Whatever the guest's real
      // shader does with depth, the stand-in cannot express it; falling through
      // leaves the draw to the fabricated-colour gate, which shows what is
      // underneath instead of painting depth over it.
      const bool depthSnapshot =
          [&] {
            auto s = m_gameSnapshots.find(d.sampledTextureObject);
            return s != m_gameSnapshots.end() && s->second.resource &&
                   s->second.resource->GetDesc().Format ==
                       DXGI_FORMAT_R32_FLOAT;
          }();
      if (depthSnapshot) ++m_standInDepthSnapshotRefused;
      if (auto snap = m_gameSnapshots.find(d.sampledTextureObject);
          d.sampledTextureObject && !depthSnapshot &&
          snap != m_gameSnapshots.end() && !snap->second.stale) {
        sampledPtr = &snap->second;
        ++m_snapshotHits;
        if (snap->second.width * 2 >= m_width) {
          const uint64_t age = m_gameFrame - snap->second.lastCopyFrame;
          ++m_snapshotAge[age == 0   ? 0
                          : age == 1 ? 1
                          : age < 10 ? 2
                          : age < 100 ? 3
                                      : 4];
        }
      } else if (d.sampledTextureObject && !depthSnapshot &&
                 m_gameSnapshots.count(d.sampledTextureObject)) {
        // Not counted for a depth snapshot: that refusal has its own counter
        // and is deliberate, whereas STALE-REFUSED means a resolve was dropped
        // and the image is a known-wrong earlier frame. Letting the depth
        // refusals fall through here made both read 233 and made a healthy
        // number look like 233 lost refreshes.
        ++m_snapshotStaleRefused;
      } else if (d.sampledTargetObject != d.targetObject) {
        if (auto it = m_gameRenderTargets.find(d.sampledTargetObject);
            it != m_gameRenderTargets.end()) {
          sampledPtr = &it->second;
          ++m_snapshotFallbacks;
        }
      }
      if (sampledPtr) {
        auto& sampled = *sampledPtr;
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
    uint32_t yuvDescriptorBase = kYuvPlaneDescriptorBase;
    if (d.yuvComposite && EnsureYuvPlanes(d, yuvDescriptorBase)) {
      yuv = true;
      textured = true;
      textureDescriptor = yuvDescriptorBase;
    }
    if (!textured)
      textured = EnsureGameTexture(d.texture, textureDescriptor);

    // Fabricated colour with no texture to modulate: do not draw it at all.
    //
    // This is what paints the screen white. The untextured PSO returns the
    // vertex colour unmodified, and a draw whose colour was meant to come from
    // a texture has no COLOR element in its declaration, so BuildHleDraw seeds
    // {1,1,1,1} (d3d9_draw.cpp). That seed is correct as a MODULATION IDENTITY
    // — kGameTexturePS computes tex * col, and zeroing it killed the logo
    // (0f66860) — but when the multiply never happens it is emitted literally,
    // as opaque white. These draws are geometrically exact fullscreen quads
    // (measured: 4.3 verts, 100% coverage, 2.01 ndc extent, 51f3c80), so each
    // is a white rectangle over the whole frame.
    //
    // The gate is the FABRICATION, not the reason the texture is missing. A
    // first attempt keyed on sampledTargetObject — "meant to sample a resolve
    // result, found none" — and it was too narrow by an order of magnitude:
    // measured in the menu, 123 draws land on the presented surface, 94 of them
    // untextured, but only about 4 per frame carry a sampled target. The other
    // ninety have no texture of any kind, mostly because the guest format was
    // rejected upstream, and they went on painting white.
    //
    // kPacked and kFallback colours are real vertex data and are left alone
    // even when untextured; only kNone is invented here.
    //
    // The comment above reasons that binding nothing turns such draws black. It
    // does not — colourless means white — which is why this read as an
    // overpaint problem for so long.
    //
    // Skipping shows what is underneath: incomplete, but honest. A steady count
    // is a real upstream defect (a dropped resolve, a rejected texture format),
    // and this only stops that defect from being painted over everything.
    // Decided here, applied below once it is known whether the guest's own
    // shader will run. `textured` describes only d.texture, the ONE texture the
    // stand-in samples; a translated draw carries its textures in
    // pixelTextures and binds them itself, so this says nothing about it.
    //
    // Applying the skip here cost the menu its whole post-processing chain.
    // Measured on mx_806: every skipped draw was translated -- 76 aimed at the
    // 320x180 luminance target, 74 at each of two bloom targets, 247 at the
    // scene. That left the luminance target cleared and never drawn into, so
    // the guest measured an average scene luminance of zero, computed its
    // auto-exposure as key/0 = +Infinity, and the composite turned the frame
    // to NaN. The white menu backdrop was this gate.
    //
    // A depth-only pass is exempt. Its draws have no colour source and no
    // texture BY DESIGN -- that is what a shadow-map pass is -- and their
    // colour output goes to a scratch target nothing samples. Skipping them
    // would leave the depth surface empty, which is the whole thing this pass
    // exists to fill.
    const bool fabricatedWhite =
        !depthOnlyPass &&
        d.colorSource == uint8_t(mx::hle::DrawCall::ColorSource::kNone) &&
        !textured;

    // Depth state is decided the same way for both paths, so it is computed
    // before the split rather than duplicated inside it.
    // Offscreen draws used to force depth off because they had no attachment.
    // They can have one now, so the guest's own depth state is honoured on both
    // paths; a draw whose depth surface could not be created still falls back
    // to no depth rather than binding a DSV that does not exist.
    const bool tDepthEnable =
        (drawTarget ? depthTarget != nullptr : true) && d.depthEnable;
    const bool tDepthWrite = tDepthEnable && d.depthWrite;

    // Run the guest's own pixel shader, when this draw has everything it needs:
    // a translated shader, its interpolators, and its constant bank. Anything
    // missing keeps the tex*col stand-in rather than rendering a guess.
    // The group every pipeline for this draw must declare. Computed once, above
    // the split, so the translated and stand-in paths cannot disagree about it —
    // they already did about nothing else, and a disagreement here is invisible
    // except as a draw that does not appear.
    const D3D12_PRIMITIVE_TOPOLOGY_TYPE topoType = TopologyTypeOf(d.topology);

    ID3D12PipelineState* translatedPso = nullptr;
    if (d.translated) {
      TranslatedKey key;
      key.handle = d.pixelShaderHandle;
      key.vsHandle = d.gpuVertex ? d.vertexShaderHandle : 0;
      key.src = d.srcBlend;
      key.dest = d.destBlend;
      key.op = d.blendOp;
      key.rtvFormat = drawTarget ? drawTarget->format : kBackBufferFormat;
      key.topoType = topoType;
      key.flags = uint8_t((tDepthEnable ? 1u : 0u) |
                          (tDepthWrite ? 2u : 0u) |
                          (d.colorWrite ? 0u : 4u) |
                          (d.blendEnable ? 8u : 0u) |
                          (d.gpuVertexFetch ? 16u : 0u));
      translatedPso = TranslatedPSO(key, *d.pixelShaderHlsl, d);
    }
    // Every texture the shader reads must be bindable, or the draw falls back:
    // a shader sampling a descriptor that was never written reads whatever is
    // there, which is a confident wrong answer rather than a visible failure.
    D3D12_GPU_DESCRIPTOR_HANDLE translatedSrvTable = {};
    if (translatedPso && !BindTranslatedTextures(d, translatedSrvTable))
      translatedPso = nullptr;
    // Now that the translated path has had its chance, a draw still heading for
    // the untextured stand-in with an invented colour is the fabricated white
    // the guard was written for. See the note beside fabricatedWhite.
    //
    // TRIED AND REVERTED (mx_960): exempting draws that HAD a translation and
    // lost it in BindTranslatedTextures, on the argument that they are not
    // "an invented colour" but a draw we chose to discard. They are exactly an
    // invented colour. A draw whose texture binding failed has no colour source
    // and no texture, so the stand-in paints it white -- and letting the 354
    // such draws on the scene target through turned the whole menu backdrop
    // white and buried the rider and bike under it. WHITE-SKIPPED fell 451 -> 76
    // and the picture got worse; the counter was measuring the draws starting to
    // render, not starting to render correctly.
    if (!translatedPso && fabricatedWhite) {
      ++m_sampleMissSkipped;
      // DIAG (remove before commit): what the skipped draws are aimed at.
      auto& e =
          m_skipByTarget[(uint64_t(d.targetWidth) << 32) | d.targetHeight];
      ++e.count;
      e.object = d.targetObject;
      if (d.translated) ++e.translated;
      if (d.pixelSamplerCount) ++e.wantedSlots;
      continue;
    }
    if (translatedPso) {
      m_commandList->SetGraphicsRootSignature(m_translatedRootSig.Get());
      // The block heap is a different heap from the stand-in path's, so it has
      // to be bound alongside the sampler heap for this draw.
      ID3D12DescriptorHeap* theaps[] = {m_translatedSrvHeap.Get(),
                                        m_samplerHeap.Get()};
      m_commandList->SetDescriptorHeaps(2, theaps);
      m_commandList->SetPipelineState(translatedPso);
      // b0 vertex. Two different buffers for the two vertex stages, at the one
      // register each declares: the guest's own VERTEX constant bank when its
      // shader is running, and the per-draw transform when the passthrough
      // stage is — that one does not read b0 at all, but the root signature
      // requires a bound CBV either way.
      ID3D12Resource* tcb = d.gpuVertex   ? d.vscb.Get()
                            : d.cb        ? d.cb.Get()
                                          : m_gameCB.Get();
      m_commandList->SetGraphicsRootConstantBufferView(
          0, tcb->GetGPUVirtualAddress());
      // b1 pixel: the guest's own pixel constant bank.
      m_commandList->SetGraphicsRootConstantBufferView(
          1, d.pscb->GetGPUVirtualAddress());
      m_commandList->SetGraphicsRootDescriptorTable(2, translatedSrvTable);
      // One sampler per slot. This used to offset a four-descriptor heap by a
      // single per-draw variant index while the root signature's sampler range
      // declared sixteen — so slot 1 of a multi-sampler shader read the next
      // variant along and slot 4 onwards read off the end of the heap.
      D3D12_GPU_DESCRIPTOR_HANDLE samp = {};
      if (BindTranslatedSamplers(d, samp))
        m_commandList->SetGraphicsRootDescriptorTable(3, samp);
      m_commandList->IASetPrimitiveTopology(d.topology);
      if (d.gpuVertexFetch) {
        // No vertex buffers at all. The stage's only input is SV_VertexID and
        // it reads the guest's raw bytes through t16, so binding a stream here
        // would contradict the empty input layout the PSO was built with.
        m_commandList->SetGraphicsRootShaderResourceView(
            4, d.rawvb->GetGPUVirtualAddress());
        ++m_gpuVertexDraws;
        ++m_gpuVertexFetchDraws;
      } else if (d.gpuVertex) {
        // One stream: the guest's raw attributes. No stand-in vertex and no
        // interpolator stream, because neither exists for this draw — the
        // vertex shader produces the position and the rasterizer interpolates
        // what it exports.
        m_commandList->IASetVertexBuffers(0, 1, &d.vsvbv);
        ++m_gpuVertexDraws;
      } else {
        // Two streams: the stand-in vertex for position, the interpolator
        // stream for everything the pixel shader reads.
        const D3D12_VERTEX_BUFFER_VIEW views[2] = {d.vbv, d.ivbv};
        m_commandList->IASetVertexBuffers(0, 2, views);
      }
      m_commandList->IASetIndexBuffer(&d.ibv);
      m_commandList->DrawIndexedInstanced(d.indexCount, 1, 0, 0, 0);
      ++m_translatedDraws;
      // The root signature AND the heaps were swapped, so the next stand-in
      // draw has to have its own put back. Restored here rather than at the top
      // of the loop so the cost falls on the translated draw that caused it.
      ID3D12DescriptorHeap* heaps[] = {m_gameSrvHeap.Get(),
                                       m_samplerHeap.Get()};
      m_commandList->SetDescriptorHeaps(2, heaps);
      m_commandList->SetGraphicsRootSignature(m_gameRootSig.Get());
      continue;
    }
    ++m_standInDraws;

    // Offscreen targets do not yet have per-surface depth resources. The
    // post-processing/resolve chain observed in ST_Southwest is colour-only;
    // keep depth disabled there rather than bind the 1280x720 DSV against a
    // smaller RTV, which is invalid D3D12 state.
    const bool depthEnable = tDepthEnable;
    const bool depthWrite = tDepthWrite;
    const uint32_t pso_index = (depthEnable ? 1u : 0u) |
                               (depthWrite ? 2u : 0u) |
                               (d.colorWrite ? 0u : 4u) |
                               (textured ? 8u : 0u) |
                               (yuv ? 16u : 0u);
    // A blended draw takes a pipeline built for its exact blend state; anything
    // that cannot be translated falls back to the opaque one it used before.
    ID3D12PipelineState* pipeline = nullptr;
    // The pipeline must declare the format of the target it writes. Offscreen
    // targets are no longer all RGBA8.
    const DXGI_FORMAT rtvFormat =
        drawTarget ? drawTarget->format : kBackBufferFormat;
    if (d.blendEnable) {
      pipeline = BlendedPSO(BlendKey{pso_index, d.srcBlend, d.destBlend,
                                     d.blendOp, rtvFormat, topoType});
    }
    if (!pipeline) pipeline = OpaquePSO(pso_index, rtvFormat, topoType);
    m_commandList->SetPipelineState(pipeline);
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
      // The guest's own address mode for this texture, rather than WRAP for
      // everything. Only meaningful for a textured draw; the untextured PSO
      // never samples.
      //
      // The address bits arrive on the draw; the filter is read here off the
      // texture itself, so graphics_system stays a pass-through and the two
      // paths agree on what a variant index means.
      uint32_t variant = d.samplerIndex & (kSamplerClampU | kSamplerClampV);
      if (d.texture && !d.texture->linear_filter) variant |= kSamplerPoint;
      auto samp = m_samplerHeap->GetGPUDescriptorHandleForHeapStart();
      samp.ptr += UINT64(std::min(variant, kSamplerVariantCount - 1)) *
                  m_samplerDescriptorSize;
      m_commandList->SetGraphicsRootDescriptorTable(2, samp);
    }
    m_commandList->IASetPrimitiveTopology(d.topology);
    m_commandList->IASetVertexBuffers(0, 1, &d.vbv);
    m_commandList->IASetIndexBuffer(&d.ibv);
    m_commandList->DrawIndexedInstanced(d.indexCount, 1, 0, 0, 0);

  }

  // Cumulative, every 100th frame that drew anything. Distinct live targets is
  // reported alongside the cap because the two together say whether the budget
  // is comfortable or about to be exhausted — the count alone does not.
  // Every 20 frames, not 100. Frames cost ~0.5s here, so a 100-frame interval
  // is ~50 seconds and a driven session that reaches the menu and is watched
  // for a few seconds produces exactly ONE print -- from frame 1, before
  // anything has been drawn or resolved. Every measurement taken that way
  // describes startup.
  static uint32_t s_rtFrame = 0;
  if (!m_gameDraws.empty() && (++s_rtFrame % 20) == 1) {
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
    // The figure that says whether the guest's own shaders are actually
    // carrying the picture. Translated against stand-in, because the translated
    // count alone cannot distinguish "the frame runs guest shaders" from "four
    // draws in a corner do".
    std::snprintf(message, sizeof(message),
                  "guest shaders: %llu draws TRANSLATED (%llu of them running "
                  "the guest VERTEX shader too, %llu of those fetching their "
                  "own vertices, %llu dropped for want of one), "
                  "%llu stand-in; %llu pipelines built, %llu failed",
                  static_cast<unsigned long long>(m_translatedDraws),
                  static_cast<unsigned long long>(m_gpuVertexDraws),
                  static_cast<unsigned long long>(m_gpuVertexFetchDraws),
                  static_cast<unsigned long long>(m_gpuVertexDropped),
                  static_cast<unsigned long long>(m_standInDraws),
                  static_cast<unsigned long long>(m_translatedOk),
                  static_cast<unsigned long long>(m_translatedFailed));
    LogInfo(message);
    // Which of the four bind failures sent a translatable draw to the stand-in.
    // The counts are what decide the next move: block-exhausted means the ring
    // is undersized or not being reset, no-snapshot means the draw wants a
    // resolve result we never captured, and the texture ones point upstream at
    // the hooks rather than at anything here.
    std::snprintf(message, sizeof(message),
                  "translated bind failures: block-exhausted %llu, "
                  "no-snapshot %llu (depth %llu, colour %llu, "
                  "never-resolved %llu), no-texture %llu, "
                  "upload-failed %llu, array-slot-flat %llu; "
                  "sampler blocks %zu of %u, exhausted %llu",
                  static_cast<unsigned long long>(m_translatedBlockExhausted),
                  static_cast<unsigned long long>(m_translatedNoSnapshot),
                  static_cast<unsigned long long>(m_noSnapshotDepth),
                  static_cast<unsigned long long>(m_noSnapshotColour),
                  static_cast<unsigned long long>(m_noSnapshotUnknown),
                  static_cast<unsigned long long>(m_translatedNoTexture),
                  static_cast<unsigned long long>(m_translatedUploadFailed),
                  static_cast<unsigned long long>(
                      m_translatedArraySlotNot2DArray),
                  m_samplerBlocks.size(), kSamplerBlockCount,
                  static_cast<unsigned long long>(m_samplerBlockExhausted));
    LogInfo(message);
    // Separate line rather than a longer format: the snapshot numbers answer a
    // different question (which resolve result a draw sampled) from the routing
    // ones (where a draw landed), and fallbacks are the figure to watch.
    std::snprintf(message, sizeof(message),
                  "resolve snapshots: copies %llu, hits %llu, FALLBACKS %llu, "
                  "source-not-offscreen %llu, WHITE-SKIPPED %llu, "
                  "BLANK-SOURCE %llu, STALE-REFUSED %llu; live snapshots %u, "
                  "DEPTH resolves %llu (%llu band-stitched) from %zu depth "
                  "surfaces, stand-in depth refused %llu, "
                  "aliased-source matches %llu (+%llu contained)",
                  static_cast<unsigned long long>(m_snapshotCopies),
                  static_cast<unsigned long long>(m_snapshotHits),
                  static_cast<unsigned long long>(m_snapshotFallbacks),
                  static_cast<unsigned long long>(m_snapshotMissingSource),
                  static_cast<unsigned long long>(m_sampleMissSkipped),
                  static_cast<unsigned long long>(m_snapshotBlankSource),
                  static_cast<unsigned long long>(m_snapshotStaleRefused),
                  uint32_t(m_gameSnapshots.size()),
                  static_cast<unsigned long long>(m_depthResolves),
                  static_cast<unsigned long long>(m_depthBandResolves),
                  m_gameDepthTargets.size(),
                  static_cast<unsigned long long>(
                      m_standInDepthSnapshotRefused),
                  static_cast<unsigned long long>(m_aliasedSourceResolves),
                  static_cast<unsigned long long>(m_containedSourceResolves));
    LogInfo(message);
    // DIAG (remove before commit): what the WHITE-SKIPPED draws were aimed at.
    for (const auto& [extent, e] : m_skipByTarget) {
      std::snprintf(message, sizeof(message),
                    "  WHITE-SKIPPED target %ux%u obj 0x%08X: %llu draws, "
                    "%llu translated, %llu wanted sampler slots",
                    uint32_t(extent >> 32), uint32_t(extent), e.object,
                    static_cast<unsigned long long>(e.count),
                    static_cast<unsigned long long>(e.translated),
                    static_cast<unsigned long long>(e.wantedSlots));
      LogInfo(message);
    }
    // DIAG (remove before commit): the COLOUR pool with its EDRAM bases. The
    // 640x360 resolve source (0x21B0F320, base 0x2D0, pitch 640, 4x MSAA in
    // its surface word) has no host target of its own, while a 640x720 surface
    // (0x2123C9BC) sits at the same base and pitch at 1x. Whether that 640x720
    // is drawn into decides whether the 640x360 resolve should take a region of
    // it or whether the pass that fills it is being lost somewhere else.
    for (const auto& [object, t] : m_gameRenderTargets) {
      std::snprintf(message, sizeof(message),
                    "  COLOUR pool obj 0x%08X %ux%u base 0x%03X fmt %u "
                    "drawn:%s",
                    object, t.width, t.height, t.edramBase,
                    uint32_t(t.format), t.everDrawn ? "Y" : "N");
      LogInfo(message);
    }
    // DIAG (remove before commit): what the depth pool actually holds. The
    // shadow resolve names a 768x1024 depth surface while the pass appears to
    // render two EDRAM bands (768x640 at base 0x580, 768x384 at base 0x710),
    // so the object the resolve asks for may be one no draw ever bound.
    for (const auto& [object, t] : m_gameDepthTargets) {
      std::snprintf(message, sizeof(message),
                    "  DEPTH pool obj 0x%08X %ux%u drawn:%s", object, t.width,
                    t.height, t.everDrawn ? "Y" : "N");
      LogInfo(message);
    }
    // The worst offenders behind source-not-offscreen, with their status read
    // NOW rather than at first sighting. Sorted by how many resolves each one
    // cost, because one source losing a thousand resolves and a thousand losing
    // one are different defects.
    if (!m_missingSourceCounts.empty()) {
      std::vector<std::pair<uint32_t, uint64_t>> worst(
          m_missingSourceCounts.begin(), m_missingSourceCounts.end());
      std::sort(worst.begin(), worst.end(),
                [](const auto& a, const auto& b) { return a.second > b.second; });
      std::string line;
      for (size_t i = 0; i < worst.size() && i < 6; ++i) {
        char one[96];
        std::snprintf(one, sizeof(one), " 0x%08X x%llu(drawn:%s)",
                      worst[i].first,
                      static_cast<unsigned long long>(worst[i].second),
                      m_everDrawTarget.count(worst[i].first) ? "Y" : "N");
        line += one;
      }
      std::snprintf(message, sizeof(message),
                    "missing-source offenders (%zu distinct):%s",
                    m_missingSourceCounts.size(), line.c_str());
      LogInfo(message);
    }
    std::snprintf(message, sizeof(message),
                  "stand-in reasons: no-hlsl %llu, no-handle %llu, "
                  "no-vertex-inputs %llu, no-constants %llu, "
                  "too-many-samplers %llu",
                  static_cast<unsigned long long>(m_standInNoHlsl),
                  static_cast<unsigned long long>(m_standInNoHandle),
                  static_cast<unsigned long long>(m_standInNoVertexInputs),
                  static_cast<unsigned long long>(m_standInNoConstants),
                  static_cast<unsigned long long>(m_standInTooManySamplers));
    LogInfo(message);
    // THIS frame, not cumulative: the question is whether the frame on screen
    // was assembled on one surface or several. "presented 1 of 1" means present
    // is showing the finished scene; "1 of 4" means it is showing one layer.
    std::snprintf(message, sizeof(message),
                  "present source: resolve 0x%08X, fallback object 0x%08X, %u "
                  "full-size surfaces drawn this frame, %u draws across them",
                  m_presentResolveTexture, m_presentSourceObject,
                  uint32_t(fullSizeTargets.size()), fullSizeDraws);
    LogInfo(message);
    {
      std::string order;
      char one[64];
      for (const auto& e : fullSizeOrder) {
        std::snprintf(one, sizeof(one), " 0x%08X=%u", e.first, e.second);
        order += one;
      }
      std::snprintf(message, sizeof(message),
                    "full-size surfaces this frame, in draw order:%s "
                    "(presenting the last)",
                    order.empty() ? " none" : order.c_str());
      LogInfo(message);
    }
    // Age of a full-screen snapshot when a draw samples it. The 100+ bucket is
    // the one that matters: those are whole leftover frames being composited.
    std::snprintf(message, sizeof(message),
                  "fullscreen snapshot age at sample: this-frame %llu, "
                  "last %llu, 2-9 %llu, 10-99 %llu, 100+ %llu",
                  static_cast<unsigned long long>(m_snapshotAge[0]),
                  static_cast<unsigned long long>(m_snapshotAge[1]),
                  static_cast<unsigned long long>(m_snapshotAge[2]),
                  static_cast<unsigned long long>(m_snapshotAge[3]),
                  static_cast<unsigned long long>(m_snapshotAge[4]));
    LogInfo(message);
    // Both of these have been non-zero for real reasons — a draw-list cap that
    // dropped resolves, and a descriptor leak that made every creation fail —
    // and both were invisible until they were counted. Kept reported.
    std::snprintf(message, sizeof(message),
                  "resolve budget: dropped-list-full %llu, create-failed %llu",
                  static_cast<unsigned long long>(m_resolvesDroppedFull),
                  static_cast<unsigned long long>(m_snapshotCreateFailed));
    LogInfo(message);
  }
}

void D3D12Renderer::ClearGameDraws() {
  if (m_gameDraws.empty()) return;

  // Hand the buffers to the retirement list rather than releasing them here.
  // These draws were recorded into the command list submitted at the end of the
  // previous frame, which signalled m_fenceValue; the GPU may still be reading
  // them. See RetiredFrame in the header for why the command list itself is no
  // protection.
  RetiredFrame& r = (!m_retired.empty() && m_retired.back().fence == m_fenceValue)
                        ? m_retired.back()
                        : m_retired.emplace_back(RetiredFrame{m_fenceValue, {}});
  //
  // Every per-draw buffer, not just the first three. vsvb, vscb, ivb and pscb
  // were being destroyed here while the GPU could still be reading them --
  // pre-existing, and not a defect I hit, but rawvb joins exactly that set and
  // retiring one while dropping its neighbours would be incoherent. Retiring
  // more only ever delays a release.
  r.res.reserve(r.res.size() + m_gameDraws.size() * 7);
  for (auto& d : m_gameDraws) {
    if (d.vb) r.res.push_back(std::move(d.vb));
    if (d.ib) r.res.push_back(std::move(d.ib));
    if (d.cb) r.res.push_back(std::move(d.cb));
    if (d.ivb) r.res.push_back(std::move(d.ivb));
    if (d.pscb) r.res.push_back(std::move(d.pscb));
    if (d.vsvb) r.res.push_back(std::move(d.vsvb));
    if (d.vscb) r.res.push_back(std::move(d.vscb));
    if (d.rawvb) r.res.push_back(std::move(d.rawvb));
  }
  m_gameDraws.clear();
  // The descriptor block window is NOT reset here. This runs on guest handoff,
  // not per host frame, and blocks are consumed per host frame — which is what
  // exhausted the ring. RenderGameFrame opens each frame's own slice instead.
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
                                 uint32_t sampledTextureObject,
                                 const std::shared_ptr<const mx::hle::HleTexturePayload>* planes,
                                 uint32_t planeCount, bool yuvHasAlpha,
                                 bool blendEnable, uint32_t srcBlend,
                                 uint32_t destBlend, uint32_t blendOp,
                                 uint8_t colorSource, uint32_t samplerIndex,
                                 uint32_t pixelShaderHandle,
                                 std::shared_ptr<const std::string> pixelShaderHlsl,
                                 const uint8_t* interpolators,
                                 uint32_t interpBytes,
                                 const uint32_t* pixelConstants,
                                 uint32_t pixelConstDwords,
                                 uint32_t pixelSamplerCount,
                                 const std::shared_ptr<const mx::hle::HleTexturePayload>* pixelTextures,
                                 const uint32_t* pixelSampledObjects,
                                 const GpuVertexStage* vertexStage,
                                 uint32_t pixelSamplerArrayMask,
                                 const uint8_t* pixelSamplerSigns,
                                 uint32_t pixelParamGen,
                                 uint32_t depthObject, uint32_t depthWidth,
                                 uint32_t depthHeight, uint32_t depthBase,
                                 uint32_t targetBase,
                                 uint32_t targetColorFormat) {
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
  // A fetch draw brings no host vertex buffer at all: its geometry arrives in
  // vertexStage->rawBytes and the shader reads it through the root SRV, so a
  // null `vertices` is correct here rather than a malformed draw. Tested before
  // the gate because the gate would otherwise drop every one of them.
  const bool fetchGeometry = vertexStage && vertexStage->rawBytes &&
                             vertexStage->rawByteCount &&
                             vertexStage->rawFetch && vertexStage->rawFetchCount;
  if (!indices || idxBytes == 0) return;
  if (!fetchGeometry && (!vertices || vtxBytes == 0)) return;
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

  // Skipped entirely for a fetch draw — a zero-byte buffer is not a valid D3D12
  // resource, and nothing binds `vbv` on that path. The guard at the end of this
  // function is what keeps such a draw from ever reaching the stand-in, which
  // WOULD read it.
  if (vertices && vtxBytes) {
    if (!createBuffer(d.vb, vtxBytes)) return;
    void* vtxMap = nullptr;
    if (FAILED(d.vb->Map(0, nullptr, &vtxMap))) return;
    memcpy(vtxMap, vertices, vtxBytes);
    // The fixed Bink YUV shader replaces the guest pixel shader, but it must
    // preserve the guest shader's final modulation:
    //
    //   export = decoded_yuva * c0
    //
    // kGameYuvPS performs that multiply through its COLOR input. Bink uses an
    // UP/FVF quad with no guest COLOR element, so the transcode supplies white
    // there; leaving it white would make the video ignore c0 entirely.
    //
    // The host layout is position float4 @0, color float4 @16, uv float2 @32.
    // Multiplying the seeded color by c0 is algebraically identical to the
    // guest shader and avoids adding a second constant-buffer binding solely
    // for this optimized path.
    if (planes && planeCount >= 3 && pixelConstants &&
        pixelConstDwords >= 4 && vtxStride >= 32) {
      float modulation[4];
      std::memcpy(modulation, pixelConstants, sizeof(modulation));
      auto* bytes = static_cast<uint8_t*>(vtxMap);
      const uint32_t vertexCount = vtxBytes / vtxStride;
      for (uint32_t i = 0; i < vertexCount; ++i) {
        float color[4];
        std::memcpy(color, bytes + i * vtxStride + 16, sizeof(color));
        for (uint32_t c = 0; c < 4; ++c) color[c] *= modulation[c];
        std::memcpy(bytes + i * vtxStride + 16, color, sizeof(color));
      }
      static uint32_t s_yuvModulationLogs = 0;
      if (s_yuvModulationLogs++ < 8) {
        const std::string message = fmt::format(
            "Bink YUV c0 modulation: ({:.4f}, {:.4f}, {:.4f}, {:.4f})",
            modulation[0], modulation[1], modulation[2], modulation[3]);
        LogInfo(message.c_str());
      }
    }
    d.vb->Unmap(0, nullptr);
    d.vbv.BufferLocation = d.vb->GetGPUVirtualAddress();
    d.vbv.StrideInBytes = vtxStride;
    d.vbv.SizeInBytes = vtxBytes;
  }

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
  d.sampledTextureObject = sampledTextureObject;
  d.colorSource = colorSource;
  d.samplerIndex = samplerIndex;
  d.pixelShaderHandle = pixelShaderHandle;
  d.pixelShaderHlsl = std::move(pixelShaderHlsl);
  d.pixelSamplerCount = pixelSamplerCount;
  d.pixelSamplerArrayMask = pixelSamplerArrayMask;
  d.pixelParamGen = pixelParamGen;
  if (pixelSamplerSigns)
    std::memcpy(d.pixelSamplerSigns.data(), pixelSamplerSigns,
                d.pixelSamplerSigns.size());
  d.depthObject = depthObject;
  d.depthWidth = depthWidth;
  d.depthHeight = depthHeight;
  d.depthBase = depthBase;
  d.targetBase = targetBase;
  d.targetColorFormat = targetColorFormat;
  if (pixelTextures && pixelSampledObjects) {
    for (uint32_t i = 0; i < kTranslatedSamplerSlots; ++i) {
      d.pixelTextures[i] = pixelTextures[i];
      d.pixelSampledObjects[i] = pixelSampledObjects[i];
    }
  }

  // Does this draw bring the guest's own vertex stage? Decided BEFORE the
  // translated gate below, because it changes what that gate requires: a draw
  // running the guest vertex shader has no interpolator stream and must not
  // have one. Demanding it anyway is what turned every qualifying draw into a
  // stand-in draw holding vertices the interpreter no longer transformed —
  // a flat red frame, and zero translated draws in a run where 36,064
  // qualified.
  //
  // Two valid shapes, and a fetch stage has NONE of the input-element fields by
  // design -- its only input is SV_VertexID. Requiring them unconditionally
  // dropped every fetch draw on the floor here: `hasVertexStage` was false, and
  // the guard further down that refuses a draw which brought a vertex stage and
  // could not get one then discarded it rather than falling back. Measured as a
  // frame going from 339 draws to 28.
  const bool hasVertexCommon = vertexStage && vertexStage->handle &&
                               vertexStage->hlsl && vertexStage->constants &&
                               vertexStage->constDwords;
  const bool hasFetchStage =
      hasVertexCommon && vertexStage->rawBytes && vertexStage->rawByteCount &&
      vertexStage->rawFetch && vertexStage->rawFetchCount &&
      vertexStage->rawFetchCount <= mx::hle::HlslShader::kMaxVertexFetches;
  const bool hasVertexStage =
      hasFetchStage ||
      (hasVertexCommon && vertexStage->inputs && vertexStage->inputBytes &&
       vertexStage->regs && vertexStage->regCount &&
       vertexStage->regCount <= 32);

  // The translated path needs all of its inputs or none of them. A shader run
  // without its interpolators reads undefined registers, and one run without
  // its constants computes from zeros — both produce a confident wrong picture
  // rather than a visible failure, which is worse than keeping the stand-in.
  //
  // "Its interpolators" means the CPU-built stream only when the CPU built the
  // vertices. With the guest vertex shader running, the rasterizer produces
  // them and there is nothing to require.
  //
  // The sampler limit is the honest current boundary: a descriptor block per
  // draw is not built yet, so only a shader reading a single texture can be
  // bound correctly, using the descriptor this draw already has. Multi-sampler
  // shaders keep the stand-in until that lands.
  // WHICH of the six conditions sent this draw to the stand-in. Without this the
  // only available numbers are measured at two different points -- the hook
  // counts ~475k D3D9 draw attempts, the renderer ~52k submitted draws -- so
  // "2000 draws with an untranslated shader" and "27015 stand-in draws" describe
  // different populations and cannot be subtracted from one another. Every
  // attempt to reason about the difference between them has been wrong.
  if (!d.pixelShaderHlsl) {
  }
  else if (!d.pixelShaderHandle) ++m_standInNoHandle;
  else if (!hasVertexStage && !(interpolators && interpBytes))
    ++m_standInNoVertexInputs;
  else if (!pixelConstants || !pixelConstDwords) ++m_standInNoConstants;
  else if (pixelSamplerCount > kTranslatedSamplerSlots) ++m_standInTooManySamplers;

  if (d.pixelShaderHlsl && d.pixelShaderHandle &&
      (hasVertexStage || (interpolators && interpBytes)) && pixelConstants &&
      pixelConstDwords && pixelSamplerCount <= kTranslatedSamplerSlots) {
    // The shader's cbuffer is xe_c[256], then xe_texinv[slots],
    // xe_texsign[slots], and xe_param_gen, so the buffer must cover all four.
    // Sizing it to the
    // constant bank alone would leave the shader reading past the end of the
    // resource for every unnormalized fetch. Rounded up to 256 bytes, the
    // constant-buffer granularity.
    const uint32_t bankBytes = pixelConstDwords * 4;
    const uint32_t texInvBytes = kTranslatedSamplerSlots * 16;
    const uint32_t texSignBytes = kTranslatedSamplerSlots * 16;
    const uint32_t paramGenBytes = 16;
    const uint32_t constBytes =
        ((bankBytes + texInvBytes + texSignBytes + paramGenBytes) + 255u) &
        ~255u;
    // Built only for the CPU-vertex shape. A zero-byte buffer is not a valid
    // D3D12 resource, so this cannot simply fall out of interpBytes == 0.
    bool haveInterp = hasVertexStage;
    if (!hasVertexStage && createBuffer(d.ivb, interpBytes)) {
      void* ip = nullptr;
      D3D12_RANGE inone = {0, 0};
      if (SUCCEEDED(d.ivb->Map(0, &inone, &ip)) && ip) {
        std::memcpy(ip, interpolators, interpBytes);
        d.ivb->Unmap(0, nullptr);
        d.ivbv.BufferLocation = d.ivb->GetGPUVirtualAddress();
        d.ivbv.SizeInBytes = interpBytes;
        d.ivbv.StrideInBytes =
            kTranslatedInterpolators * 4 * uint32_t(sizeof(float));
        haveInterp = true;
      }
    }
    if (haveInterp && createBuffer(d.pscb, constBytes)) {
      void* p = nullptr;
      D3D12_RANGE none = {0, 0};
      if (SUCCEEDED(d.pscb->Map(0, &none, &p)) && p) {
        std::memset(p, 0, constBytes);
        std::memcpy(p, pixelConstants, bankBytes);
        // xe_texinv, immediately after the bank. An unnormalized fetch
        // addresses the texture in TEXELS, so the shader multiplies by this to
        // normalize — it is therefore 1/extent of the texture actually bound at
        // that slot, which with one sampler is this draw's texture.
        //
        // It held the extent itself until the emitter's divide was corrected,
        // which made every unnormalized fetch wrong by the size squared.
        //
        // Left zero when there is no texture, which makes such a fetch read
        // texel 0 rather than something plausible — and is why this is stored
        // reciprocated here instead of divided in the shader, where a zero
        // extent would produce infinity.
        // EVERY slot, not just the first. xe_texinv is declared
        // kTranslatedSamplerSlots wide and was filled at index 0 only, so an
        // unnormalized fetch on any slot above the first multiplied its
        // coordinate by ZERO -- sampling texel 0 and painting that single
        // texel's colour flat across the primitive. On a full-screen quad that
        // is a wash, which is what it looks like.
        //
        // The per-slot payloads were already carried here; only this fill was
        // still single-texture. Slot 0 falls back to d.texture because the
        // single-texture path populates that and not the array.
        //
        // A slot bound to a RESOLVE SNAPSHOT has no CPU payload at all, and
        // filling only from d.pixelTextures left those slots at zero -- the
        // same defect as the slot-0-only fill above, surviving in the one case
        // that never carries a payload. The extent then has to come from the
        // snapshot resource, which is the texture actually bound there.
        //
        // This is what the menu rider looks like: its material samples the
        // scene composite at s13, that slot is a snapshot, its texinv was zero,
        // and an unnormalized fetch times zero reads texel (0,0) and paints it
        // flat over 21753 indices of character mesh.
        for (uint32_t s = 0; s < kTranslatedSamplerSlots; ++s) {
          uint32_t w = 0, h = 0;
          const auto& tex = s < d.pixelTextures.size() && d.pixelTextures[s]
                                ? d.pixelTextures[s]
                                : (s == 0 ? d.texture : nullptr);
          if (tex && tex->width && tex->height) {
            w = tex->width;
            h = tex->height;
          } else if (s < d.pixelSampledObjects.size() &&
                     d.pixelSampledObjects[s]) {
            const auto snap = m_gameSnapshots.find(d.pixelSampledObjects[s]);
            if (snap != m_gameSnapshots.end()) {
              w = snap->second.width;
              h = snap->second.height;
            }
          }
          if (!w || !h) continue;
          const float ts[4] = {1.0f / float(w), 1.0f / float(h), 0.0f, 0.0f};
          std::memcpy(static_cast<uint8_t*>(p) + bankBytes + s * 16, ts,
                      sizeof(ts));
        }
        // xe_texsign, immediately after xe_texinv: the per-component scale for
        // TEXTURE SIGNS, 2.0 where the guest fetch is kUnsignedBiased and the
        // shader must expand [0,1] to [-1,1]. The shader pairs it with an
        // offset of 1-scale, so 1.0 is the identity.
        //
        // Written for EVERY slot and every component, unconditionally. The
        // buffer was memset to zero above, and a zero scale here does not mean
        // "unsigned", it means the fetch becomes v*0 + 1 -- every texture
        // sampling as solid white. There is no slot this may be skipped for.
        for (uint32_t s = 0; s < kTranslatedSamplerSlots; ++s) {
          const uint8_t biased = d.pixelSamplerSigns[s];
          const float sc[4] = {(biased & 1) ? 2.0f : 1.0f,
                               (biased & 2) ? 2.0f : 1.0f,
                               (biased & 4) ? 2.0f : 1.0f,
                               (biased & 8) ? 2.0f : 1.0f};
          std::memcpy(
              static_cast<uint8_t*>(p) + bankBytes + texInvBytes + s * 16, sc,
              sizeof(sc));
        }
        // xe_param_gen follows xe_texsign. x is the biased destination
        // register; y identifies the primitive so the shader can reproduce the
        // Xenos point and line sign flags.
        uint32_t primitive = 0;
        if (d.topology == D3D_PRIMITIVE_TOPOLOGY_POINTLIST) {
          primitive = 1;
        } else if (d.topology == D3D_PRIMITIVE_TOPOLOGY_LINELIST ||
                   d.topology == D3D_PRIMITIVE_TOPOLOGY_LINESTRIP) {
          primitive = 2;
        }
        const uint32_t pg[4] = {d.pixelParamGen, primitive, 0, 0};
        std::memcpy(static_cast<uint8_t*>(p) + bankBytes + texInvBytes +
                        texSignBytes,
                    pg, sizeof(pg));
        d.pscb->Unmap(0, nullptr);
        d.translated = true;
      }
    }
    if (!d.translated) {
      d.ivb.Reset();
      d.pscb.Reset();
    }
  }

  // The guest's own vertex shader, on the GPU. Only offered for a draw whose
  // pixel shader also translated — the hooks side enforces that, and it is
  // re-checked here through `d.translated` because the two conditions are
  // decided in different processes' worth of code and a mismatch would show up
  // as geometry rather than as a message.
  //
  // Everything the CPU path derives on the side is REPLACED rather than lost:
  // the position buffer is what this stage now produces, the interpolator copy
  // is what the rasterizer does natively, and the param_gen UV becomes
  // SV_Position in a pixel shader that reads it. So there is nothing to carry
  // across — only something to stop doing.
  if (d.translated && hasVertexStage) {
    // The emitted cbuffer is xe_c[256] followed by xe_texinv[slots] in BOTH
    // stages, so the buffer has to cover both here too. Sizing it to the
    // constant bank alone leaves the shader reading past the end of the
    // resource — the same trap the pixel path documents above, and it does not
    // stop applying because this stage never samples.
    //
    // The fetch variant appends uint4 xe_vf[kMaxVertexFetches] after
    // xe_texinv, so its cbuffer is longer. Sized for it unconditionally: the
    // tail is zeroed either way and 512 spare bytes per draw is not worth a
    // second size.
    const uint32_t vsConstBytes =
        ((vertexStage->constDwords * 4 + kTranslatedSamplerSlots * 16 +
          mx::hle::HlslShader::kMaxVertexFetches * 16) + 255u) & ~255u;
    if (vertexStage->rawBytes) {
      // The fetch path: one raw buffer, no vertex buffer view, and xe_vf[]
      // written into the cbuffer tail immediately after xe_texinv.
      if (createBuffer(d.rawvb, vertexStage->rawByteCount) &&
          createBuffer(d.vscb, vsConstBytes)) {
        void* p = nullptr;
        D3D12_RANGE none = {0, 0};
        if (SUCCEEDED(d.rawvb->Map(0, &none, &p)) && p) {
          std::memcpy(p, vertexStage->rawBytes, vertexStage->rawByteCount);
          d.rawvb->Unmap(0, nullptr);
          p = nullptr;
          if (SUCCEEDED(d.vscb->Map(0, &none, &p)) && p) {
            std::memset(p, 0, vsConstBytes);
            std::memcpy(p, vertexStage->constants,
                        vertexStage->constDwords * 4);
            // xe_vf sits directly after xe_c[256] and xe_texinv[16], which is
            // where the emitter declares it. This offset and that declaration
            // are one fact in two places -- if either moves the other must.
            const uint32_t vfOffset =
                vertexStage->constDwords * 4 + kTranslatedSamplerSlots * 16;
            std::memcpy(static_cast<uint8_t*>(p) + vfOffset,
                        vertexStage->rawFetch,
                        vertexStage->rawFetchCount * 16);
            d.vscb->Unmap(0, nullptr);
            d.vertexShaderHandle = vertexStage->handle;
            d.vertexShaderHlsl = vertexStage->hlsl;
            d.vertexInputCount = 0;
            d.gpuVertex = true;
            d.gpuVertexFetch = true;
          }
        }
      }
      if (!d.gpuVertexFetch) {
        d.rawvb.Reset();
        d.vscb.Reset();
      }
    } else if (createBuffer(d.vsvb, vertexStage->inputBytes) &&
               createBuffer(d.vscb, vsConstBytes)) {
      void* p = nullptr;
      D3D12_RANGE none = {0, 0};
      if (SUCCEEDED(d.vsvb->Map(0, &none, &p)) && p) {
        std::memcpy(p, vertexStage->inputs, vertexStage->inputBytes);
        d.vsvb->Unmap(0, nullptr);
        d.vsvbv.BufferLocation = d.vsvb->GetGPUVirtualAddress();
        d.vsvbv.SizeInBytes = vertexStage->inputBytes;
        d.vsvbv.StrideInBytes = vertexStage->regCount * 16;
        p = nullptr;
        if (SUCCEEDED(d.vscb->Map(0, &none, &p)) && p) {
          // Zeroed first: the shader's cbuffer is xe_c[256] plus xe_texinv,
          // and the vertex bank is 256 constants, so the tail past the bank
          // must read zero rather than whatever the upload heap held.
          std::memset(p, 0, vsConstBytes);
          std::memcpy(p, vertexStage->constants, vertexStage->constDwords * 4);
          d.vscb->Unmap(0, nullptr);
          d.vertexShaderHandle = vertexStage->handle;
          d.vertexShaderHlsl = vertexStage->hlsl;
          d.vertexInputCount = vertexStage->regCount;
          for (uint32_t i = 0; i < vertexStage->regCount; ++i)
            d.vertexInputRegs[i] = vertexStage->regs[i];
          d.gpuVertex = true;
        }
      }
    }
    if (!d.gpuVertex) {
      d.vsvb.Reset();
      d.rawvb.Reset();
      d.vscb.Reset();
    }
  }
  // A draw that brought a vertex stage and could not get one has no path left.
  // Its `vb` holds the raw declaration positions, NOT transformed ones — the
  // interpreter did not run for it — so the stand-in would paint untransformed
  // geometry over the scene. Dropping it loses one draw; falling back paints a
  // wrong one, and a whole frame of them is a flat red screen.
  if (hasVertexStage && !d.gpuVertex) {
    ++m_gpuVertexDropped;
    return;
  }
  d.blendEnable = blendEnable;
  d.srcBlend = srcBlend;
  d.destBlend = destBlend;
  d.blendOp = blendOp;
  if (planes && planeCount >= 3) {
    d.planeCount = std::min<uint32_t>(planeCount,
                                      kMaxDrawPlanes);
    for (uint32_t i = 0; i < d.planeCount; ++i) d.planes[i] = planes[i];
    d.yuvHasAlpha = yuvHasAlpha;
    d.yuvComposite = true;
  }

  // Carry the translator's transform. Without this the draw renders under the
  // fallback identity matrix in m_gameCB, which makes a correct transform and a
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

  // Depth for OFFSCREEN targets, one surface per guest depth object. The main
  // target keeps m_gameDepth above; only offscreen draws, which previously had
  // no depth attachment at all, are served from this heap.
  {
    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    hd.NumDescriptors = kMaxGameDepthTargets + 1;
    if (FAILED(m_device->CreateDescriptorHeap(
            &hd, IID_PPV_ARGS(&m_gameDepthDsvHeap))))
      return false;
    m_gameDsvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
  }

  LogInfo("CreateGameRT: done");
  return true;
}

void D3D12Renderer::PresentGameFrame() {
  if (!m_gameRT) return;

  // Blit path: the finished scene lives in a guest-sized offscreen target, so
  // it has to be scaled to the backbuffer rather than copied. m_viewport already
  // carries the pillarbox, so drawing through it puts the image in the same
  // place the copy path did.
  if (m_hasGamePipeline && m_presentVB) {
    GameRenderTarget* presentSource = nullptr;
    if (m_presentResolveTexture) {
      auto it = m_gameSnapshots.find(m_presentResolveTexture);
      if (it != m_gameSnapshots.end() && it->second.resource &&
          !it->second.stale) {
        presentSource = &it->second;
      }
    }
    if (!presentSource && m_presentSourceObject) {
      auto it = m_gameRenderTargets.find(m_presentSourceObject);
      if (it != m_gameRenderTargets.end() && it->second.resource) {
        presentSource = &it->second;
      }
    }
    if (presentSource) {
      GameRenderTarget& src = *presentSource;
      if (src.state != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) {
        D3D12_RESOURCE_BARRIER toSrv = {};
        toSrv.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toSrv.Transition.pResource = src.resource.Get();
        toSrv.Transition.StateBefore = src.state;
        toSrv.Transition.StateAfter =
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        toSrv.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        m_commandList->ResourceBarrier(1, &toSrv);
        src.state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
      }
      // The backbuffer is already RENDER_TARGET here — EndFrame's RT→PRESENT
      // barrier depends on it still being so when this returns, which is why
      // this path adds no barrier on it at all.
      auto rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
      rtv.ptr += SIZE_T(m_frameIndex) * m_rtvDescriptorSize;
      m_commandList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
      m_commandList->RSSetViewports(1, &m_viewport);
      m_commandList->RSSetScissorRects(1, &m_scissorRect);
      m_commandList->SetGraphicsRootSignature(m_gameRootSig.Get());
      ID3D12DescriptorHeap* heaps[] = {m_gameSrvHeap.Get(),
                                       m_samplerHeap.Get()};
      m_commandList->SetDescriptorHeaps(2, heaps);
      // Textured, no depth, colour write on. See the pso_index bits in
      // CreateGamePipeline.
      m_commandList->SetPipelineState(m_gamePSOs[8].Get());
      m_commandList->IASetPrimitiveTopology(
          D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      m_commandList->SetGraphicsRootConstantBufferView(
          0, m_gameCB->GetGPUVirtualAddress());
      auto gpu = m_gameSrvHeap->GetGPUDescriptorHandleForHeapStart();
      gpu.ptr += UINT64(src.srvIndex) * m_gameSrvDescriptorSize;
      m_commandList->SetGraphicsRootDescriptorTable(1, gpu);
      // Clamped: the blit samples a finished frame edge to edge, and wrapping
      // the opposite side in is exactly the seam this change exists to remove.
      auto samp = m_samplerHeap->GetGPUDescriptorHandleForHeapStart();
      samp.ptr += UINT64(kSamplerClampU | kSamplerClampV) *
                  m_samplerDescriptorSize;
      m_commandList->SetGraphicsRootDescriptorTable(2, samp);
      m_commandList->IASetVertexBuffers(0, 1, &m_presentVbv);
      m_commandList->DrawInstanced(3, 1, 0, 0);
      // m_gameRT is untouched on this path, so it must still end in
      // PIXEL_SHADER_RESOURCE for the next BeginFrame's PSR→RT barrier to be
      // valid — the same postcondition the copy path below establishes.
      D3D12_RESOURCE_BARRIER rtToSrv = {};
      rtToSrv.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      rtToSrv.Transition.pResource = m_gameRT.Get();
      rtToSrv.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
      rtToSrv.Transition.StateAfter =
          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
      rtToSrv.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
      m_commandList->ResourceBarrier(1, &rtToSrv);
      return;
    }
  }

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
