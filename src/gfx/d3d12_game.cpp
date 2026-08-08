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
    hd.NumDescriptors = kSamplerVariantCount;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(m_device->CreateDescriptorHeap(&hd,
                                              IID_PPV_ARGS(&m_samplerHeap)))) {
      LogError("CreateGamePipeline: sampler heap failed");
      return false;
    }
    m_samplerDescriptorSize = m_device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    auto cpu = m_samplerHeap->GetCPUDescriptorHandleForHeapStart();
    for (uint32_t i = 0; i < kSamplerVariantCount; ++i) {
      D3D12_SAMPLER_DESC sd = {};
      sd.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
      sd.AddressU = (i & kSamplerClampU)
                        ? D3D12_TEXTURE_ADDRESS_MODE_CLAMP
                        : D3D12_TEXTURE_ADDRESS_MODE_WRAP;
      sd.AddressV = (i & kSamplerClampV)
                        ? D3D12_TEXTURE_ADDRESS_MODE_CLAMP
                        : D3D12_TEXTURE_ADDRESS_MODE_WRAP;
      sd.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
      sd.MaxAnisotropy = 1;
      sd.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
      sd.MinLOD = 0.0f;
      sd.MaxLOD = 0.0f;
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
  D3D12_ROOT_PARAMETER params[4] = {rootParams[0], rootParams[1], rootParams[2],
                                    {}};
  params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[3].DescriptorTable.NumDescriptorRanges = 1;
  params[3].DescriptorTable.pDescriptorRanges = &ranges[1];
  params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

  D3D12_ROOT_SIGNATURE_DESC rootDesc = {};
  rootDesc.NumParameters = 4;
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
  if (!m_translatedSrvHeap || !d.pixelSamplerCount) return false;
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
        ++m_translatedNoSnapshot;
        return false;
      }
      slots[i].resource = it->second.resource.Get();
      slots[i].format = kBackBufferFormat;
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

  auto cpu = m_translatedSrvHeap->GetCPUDescriptorHandleForHeapStart();
  cpu.ptr += SIZE_T(block) * kTranslatedSamplerSlots * m_gameSrvDescriptorSize;
  for (uint32_t i = 0; i < kTranslatedSamplerSlots; ++i) {
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;
    // Slots past what the shader declares still need a valid descriptor: the
    // table's range covers all of them whether or not they are sampled. They
    // repeat slot 0 rather than being left undefined.
    const Slot& s = slots[i < d.pixelSamplerCount ? i : 0];
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

ID3D12PipelineState* D3D12Renderer::TranslatedPSO(const TranslatedKey& key,
                                                  const std::string& hlsl) {
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

  // Two streams, matching XeVsIn above and the two buffers the draw binds.
  //
  // Slot 0 is the stand-in vertex, read only for its position — the transcode
  // still builds it and the CPU interpreter still fills it, so the translated
  // path takes the position from where it already is rather than duplicating
  // it. Slot 1 is the interpolator stream, one float4 per linkage slot.
  //
  // Keeping them separate is what lets the stand-in layout stay untouched: a
  // single interleaved stream would mean rebuilding the vertex the working path
  // depends on.
  std::vector<D3D12_INPUT_ELEMENT_DESC> layout;
  layout.push_back({"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0,
                    D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0});
  for (uint32_t i = 0; i < kTranslatedInterpolators; ++i) {
    layout.push_back({"TEXCOORD", i, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, i * 16,
                      D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0});
  }

  D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
  pso.pRootSignature = m_translatedRootSig.Get();
  pso.VS.pShaderBytecode = m_translatedVsBlob->GetBufferPointer();
  pso.VS.BytecodeLength = m_translatedVsBlob->GetBufferSize();
  pso.PS.pShaderBytecode = ps->GetBufferPointer();
  pso.PS.BytecodeLength = ps->GetBufferSize();
  pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  pso.SampleMask = UINT_MAX;
  pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pso.NumRenderTargets = 1;
  pso.RTVFormats[0] = kBackBufferFormat;
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
  uint32_t reuseRtvIndex = UINT32_MAX;
  uint32_t reuseSrvIndex = UINT32_MAX;
  if (auto it = m_gameRenderTargets.find(object);
      it != m_gameRenderTargets.end()) {
    if (it->second.width != width || it->second.height != height) {
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
      if (it->second.resource) {
        RetiredFrame& r =
            (!m_retired.empty() && m_retired.back().fence == m_fenceValue)
                ? m_retired.back()
                : m_retired.emplace_back(RetiredFrame{m_fenceValue, {}});
        r.res.push_back(std::move(it->second.resource));
      }
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
  entry.rtvIndex = reuseRtvIndex != UINT32_MAX
                       ? reuseRtvIndex
                       : uint32_t(m_gameRenderTargets.size()) + 1;

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
  // Claimed only once the resource exists. Claiming before the call leaks a
  // descriptor on every failure, and the caller retries the same object every
  // frame — see the snapshot path for the run where that drained the heap.
  entry.srvIndex = reuseSrvIndex != UINT32_MAX ? reuseSrvIndex
                                               : m_nextGameSrvDescriptor++;

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

// A snapshot is an offscreen surface like any other — same struct, same
// creation, same budget — so this defers to EnsureGameRenderTarget's storage
// rather than duplicating it. The only difference is the key: destination
// texture object, not source target object. That is what stops six resolves out
// of one shared scratch surface from aliasing each other.
D3D12Renderer::GameRenderTarget* D3D12Renderer::EnsureGameSnapshot(
    uint32_t destTexture, uint32_t width, uint32_t height) {
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
    if (it->second.width >= width && it->second.height >= height)
      return &it->second;
    ++m_rtRejectResized;
    reuseSrvIndex = it->second.srvIndex;
    // The union, so a later band cannot shrink away an earlier one.
    growWidth = it->second.width;
    growHeight = it->second.height;
    width = std::max(width, growWidth);
    height = std::max(height, growHeight);
    growFrom = it->second.resource;
    growFromState = it->second.state;
    // Retire the old resource rather than releasing it here. It was referenced
    // by the command list submitted for the previous frame and the GPU may
    // still be reading it; a command list does not keep its resources alive.
    // Dropping it inline made every subsequent CreateCommittedResource for that
    // size fail — 1834 failures in one run.
    if (it->second.resource) {
      RetiredFrame& r =
          (!m_retired.empty() && m_retired.back().fence == m_fenceValue)
              ? m_retired.back()
              : m_retired.emplace_back(RetiredFrame{m_fenceValue, {}});
      r.res.push_back(std::move(it->second.resource));
    }
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
  rd.Flags = D3D12_RESOURCE_FLAG_NONE;
  if (FAILED(m_device->CreateCommittedResource(
          &hp, D3D12_HEAP_FLAG_NONE, &rd,
          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
          IID_PPV_ARGS(&entry.resource)))) {
    // Loudly, and without having spent a descriptor. Claiming the index before
    // this call leaked one on every failure — silently, because this path used
    // to return with no log — and the caller retries the same texture next
    // frame, so it drained the heap to 1024/1024 in about twenty seconds and
    // every snapshot after that was refused for budget.
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
  entry.srvIndex = reuseSrvIndex != UINT32_MAX ? reuseSrvIndex
                                               : m_nextGameSrvDescriptor++;

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

  D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
  srv.Format = kBackBufferFormat;
  srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv.Texture2D.MipLevels = 1;
  auto cpu = m_gameSrvHeap->GetCPUDescriptorHandleForHeapStart();
  cpu.ptr += SIZE_T(entry.srvIndex) * m_gameSrvDescriptorSize;
  m_device->CreateShaderResourceView(entry.resource.Get(), &srv, cpu);

  auto [it, inserted] = m_gameSnapshots.emplace(destTexture, std::move(entry));
  if (!inserted) return nullptr;
  char message[192];
  std::snprintf(message, sizeof(message),
                "resolve snapshot: texture 0x%08X %ux%u cache %zu",
                destTexture, width, height, m_gameSnapshots.size());
  LogInfo(message);
  return &it->second;
}

void D3D12Renderer::AddGameResolve(uint32_t destTexture, uint32_t sourceObject,
                                   int32_t destX, int32_t destY, int32_t srcX1,
                                   int32_t srcY1, int32_t srcX2,
                                   int32_t srcY2) {
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
  d.resolveDestX = destX;
  d.resolveDestY = destY;
  d.resolveSrcX1 = srcX1;
  d.resolveSrcY1 = srcY1;
  d.resolveSrcX2 = srcX2;
  d.resolveSrcY2 = srcY2;
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
  // This frame in flight takes its own slice of the descriptor blocks, so the
  // window resets every host frame rather than only when the guest hands off a
  // new draw list. See kTranslatedBlocksPerFrame.
  m_translatedBlockNext = m_frameIndex * kTranslatedBlocksPerFrame;
  m_translatedBlockLimit = m_translatedBlockNext + kTranslatedBlocksPerFrame;
  for (auto& [object, target] : m_gameRenderTargets)
    target.usedThisFrame = false;

  uint32_t boundTargetObject = 0;  // zero is the final m_gameRT.
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
  // Reset per frame: if nothing is drawn offscreen at guest-backbuffer size
  // this frame, present must fall back to m_gameRT rather than blit a target
  // that belongs to an earlier frame.
  m_presentSourceObject = 0;
  std::unordered_set<uint32_t> fullSizeTargets;
  uint32_t fullSizeDraws = 0;
  for (const auto& d : m_gameDraws) {
    // A resolve: snapshot the source target as it stands right now, so draws
    // recorded after this point sample these contents rather than whatever the
    // shared surface holds by the end of the frame. Draws nothing.
    if (d.resolveDest) {
      // Where the source actually rendered. Resolve sources are routed
      // offscreen (isResolveSource, below), so this normally finds a surface of
      // the source's own — which is the point: offscreen targets are only
      // cleared when something draws into them, so they carry their contents
      // across frames. A resolve needs exactly that.
      ID3D12Resource* srcRes = nullptr;
      GameRenderTarget* srcEntry = nullptr;
      uint32_t srcWidth = 0, srcHeight = 0;
      D3D12_RESOURCE_STATES srcState = D3D12_RESOURCE_STATE_RENDER_TARGET;
      if (auto it = m_gameRenderTargets.find(d.resolveSource);
          it != m_gameRenderTargets.end()) {
        srcEntry = &it->second;
        srcRes = srcEntry->resource.Get();
        srcWidth = srcEntry->width;
        srcHeight = srcEntry->height;
        srcState = srcEntry->state;
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
      // The snapshot must cover this band's placement, not merely match the
      // band's size — that conflation is what made the scene an 80-line strip.
      GameRenderTarget* snap =
          EnsureGameSnapshot(d.resolveDest, dx + copyW, dy + copyH);
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
      // The target is bound again by whichever draw follows; forcing a rebind
      // keeps that from being skipped because boundTargetObject still matches.
      boundTargetObject = 0xFFFFFFFFu;
      ++m_snapshotCopies;
      continue;
    }
    // Keep only the unsampled final 1280x720 surface on m_gameRT so
    // PresentGameFrame remains an exact-size copy. A full-size scene target
    // that a later compositor samples is still offscreen and needs its own SRV;
    // classifying solely by dimensions made that target alias m_gameRT and
    // left the final draw with nothing it could legally sample.
    GameRenderTarget* drawTarget = nullptr;
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
    const bool wantsOffscreen =
        d.targetObject && d.targetWidth && d.targetHeight &&
        (feedsLaterDraw || isResolveSource ||
         d.targetWidth != 1280 || d.targetHeight != 720);
    if (wantsOffscreen) {
      drawTarget = EnsureGameRenderTarget(d.targetObject, d.targetWidth,
                                          d.targetHeight);
      // The last guest-backbuffer-sized target drawn into this frame is the
      // finished scene, and what present should show. Tracked by last write
      // rather than by object identity because which surface ends up on screen
      // is a property of draw order, not of any particular target.
      if (drawTarget && d.targetWidth == 1280 && d.targetHeight == 720) {
        m_presentSourceObject = d.targetObject;
        // Present shows the LAST guest-backbuffer-sized surface written this
        // frame. That is only correct if there is exactly one. Seven 1280x720
        // surfaces are live, and if the guest builds the scene across several
        // and composites them, presenting one of them shows a single layer --
        // which is what a white frame with content on one band looks like.
        // Count the distinct ones per frame rather than assume either way.
        fullSizeTargets.insert(d.targetObject);
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
        drawTarget->everDrawn = true;
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
      if (auto snap = m_gameSnapshots.find(d.sampledTextureObject);
          d.sampledTextureObject && snap != m_gameSnapshots.end() &&
          !snap->second.stale) {
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
      } else if (d.sampledTextureObject &&
                 m_gameSnapshots.count(d.sampledTextureObject)) {
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
    if (d.colorSource ==
            uint8_t(mx::hle::DrawCall::ColorSource::kNone) && !textured) {
      ++m_sampleMissSkipped;
      continue;
    }

    // Depth state is decided the same way for both paths, so it is computed
    // before the split rather than duplicated inside it.
    const bool tDepthEnable = !drawTarget && d.depthEnable;
    const bool tDepthWrite = tDepthEnable && d.depthWrite;

    // Run the guest's own pixel shader, when this draw has everything it needs:
    // a translated shader, its interpolators, and its constant bank. Anything
    // missing keeps the tex*col stand-in rather than rendering a guess.
    ID3D12PipelineState* translatedPso = nullptr;
    if (d.translated) {
      TranslatedKey key;
      key.handle = d.pixelShaderHandle;
      key.src = d.srcBlend;
      key.dest = d.destBlend;
      key.op = d.blendOp;
      key.flags = uint8_t((tDepthEnable ? 1u : 0u) |
                          (tDepthWrite ? 2u : 0u) |
                          (d.colorWrite ? 0u : 4u) |
                          (d.blendEnable ? 8u : 0u));
      translatedPso = TranslatedPSO(key, *d.pixelShaderHlsl);
    }
    // Every texture the shader reads must be bindable, or the draw falls back:
    // a shader sampling a descriptor that was never written reads whatever is
    // there, which is a confident wrong answer rather than a visible failure.
    D3D12_GPU_DESCRIPTOR_HANDLE translatedSrvTable = {};
    if (translatedPso && !BindTranslatedTextures(d, translatedSrvTable))
      translatedPso = nullptr;
    if (translatedPso) {
      m_commandList->SetGraphicsRootSignature(m_translatedRootSig.Get());
      // The block heap is a different heap from the stand-in path's, so it has
      // to be bound alongside the sampler heap for this draw.
      ID3D12DescriptorHeap* theaps[] = {m_translatedSrvHeap.Get(),
                                        m_samplerHeap.Get()};
      m_commandList->SetDescriptorHeaps(2, theaps);
      m_commandList->SetPipelineState(translatedPso);
      // b0 vertex: the same transform the stand-in pipeline uses, since the
      // passthrough VS is still fed CPU-transformed positions.
      ID3D12Resource* tcb = d.cb ? d.cb.Get() : m_gameCB.Get();
      m_commandList->SetGraphicsRootConstantBufferView(
          0, tcb->GetGPUVirtualAddress());
      // b1 pixel: the guest's own pixel constant bank.
      m_commandList->SetGraphicsRootConstantBufferView(
          1, d.pscb->GetGPUVirtualAddress());
      m_commandList->SetGraphicsRootDescriptorTable(2, translatedSrvTable);
      auto samp = m_samplerHeap->GetGPUDescriptorHandleForHeapStart();
      samp.ptr += UINT64(std::min(d.samplerIndex, kSamplerVariantCount - 1)) *
                  m_samplerDescriptorSize;
      m_commandList->SetGraphicsRootDescriptorTable(3, samp);
      // Two streams: the stand-in vertex for position, the interpolator stream
      // for everything the pixel shader reads.
      const D3D12_VERTEX_BUFFER_VIEW views[2] = {d.vbv, d.ivbv};
      m_commandList->IASetPrimitiveTopology(d.topology);
      m_commandList->IASetVertexBuffers(0, 2, views);
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
    if (d.blendEnable) {
      pipeline = BlendedPSO(BlendKey{pso_index, d.srcBlend, d.destBlend,
                                     d.blendOp});
    }
    if (!pipeline) pipeline = m_gamePSOs[pso_index].Get();
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
      auto samp = m_samplerHeap->GetGPUDescriptorHandleForHeapStart();
      samp.ptr += UINT64(std::min(d.samplerIndex, kSamplerVariantCount - 1)) *
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
    // The figure that says whether the guest's own shaders are actually
    // carrying the picture. Translated against stand-in, because the translated
    // count alone cannot distinguish "the frame runs guest shaders" from "four
    // draws in a corner do".
    std::snprintf(message, sizeof(message),
                  "guest pixel shaders: %llu draws TRANSLATED, %llu stand-in; "
                  "%llu pipelines built, %llu failed",
                  static_cast<unsigned long long>(m_translatedDraws),
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
                  "no-snapshot %llu, no-texture %llu, upload-failed %llu",
                  static_cast<unsigned long long>(m_translatedBlockExhausted),
                  static_cast<unsigned long long>(m_translatedNoSnapshot),
                  static_cast<unsigned long long>(m_translatedNoTexture),
                  static_cast<unsigned long long>(m_translatedUploadFailed));
    LogInfo(message);
    // Separate line rather than a longer format: the snapshot numbers answer a
    // different question (which resolve result a draw sampled) from the routing
    // ones (where a draw landed), and fallbacks are the figure to watch.
    std::snprintf(message, sizeof(message),
                  "resolve snapshots: copies %llu, hits %llu, FALLBACKS %llu, "
                  "source-not-offscreen %llu, WHITE-SKIPPED %llu, "
                  "BLANK-SOURCE %llu, STALE-REFUSED %llu; live snapshots %u",
                  static_cast<unsigned long long>(m_snapshotCopies),
                  static_cast<unsigned long long>(m_snapshotHits),
                  static_cast<unsigned long long>(m_snapshotFallbacks),
                  static_cast<unsigned long long>(m_snapshotMissingSource),
                  static_cast<unsigned long long>(m_sampleMissSkipped),
                  static_cast<unsigned long long>(m_snapshotBlankSource),
                  static_cast<unsigned long long>(m_snapshotStaleRefused),
                  uint32_t(m_gameSnapshots.size()));
    LogInfo(message);
    // THIS frame, not cumulative: the question is whether the frame on screen
    // was assembled on one surface or several. "presented 1 of 1" means present
    // is showing the finished scene; "1 of 4" means it is showing one layer.
    std::snprintf(message, sizeof(message),
                  "present source: object 0x%08X, %u full-size surfaces drawn "
                  "this frame, %u draws across them",
                  m_presentSourceObject, uint32_t(fullSizeTargets.size()),
                  fullSizeDraws);
    LogInfo(message);
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
  r.res.reserve(r.res.size() + m_gameDraws.size() * 3);
  for (auto& d : m_gameDraws) {
    if (d.vb) r.res.push_back(std::move(d.vb));
    if (d.ib) r.res.push_back(std::move(d.ib));
    if (d.cb) r.res.push_back(std::move(d.cb));
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
                                 const uint32_t* pixelSampledObjects) {
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
  d.sampledTextureObject = sampledTextureObject;
  d.colorSource = colorSource;
  d.samplerIndex = samplerIndex;
  d.pixelShaderHandle = pixelShaderHandle;
  d.pixelShaderHlsl = std::move(pixelShaderHlsl);
  d.pixelSamplerCount = pixelSamplerCount;
  if (pixelTextures && pixelSampledObjects) {
    for (uint32_t i = 0; i < kTranslatedSamplerSlots; ++i) {
      d.pixelTextures[i] = pixelTextures[i];
      d.pixelSampledObjects[i] = pixelSampledObjects[i];
    }
  }

  // The translated path needs all of its inputs or none of them. A shader run
  // without its interpolators reads undefined registers, and one run without
  // its constants computes from zeros — both produce a confident wrong picture
  // rather than a visible failure, which is worse than keeping the stand-in.
  //
  // The sampler limit is the honest current boundary: a descriptor block per
  // draw is not built yet, so only a shader reading a single texture can be
  // bound correctly, using the descriptor this draw already has. Multi-sampler
  // shaders keep the stand-in until that lands.
  if (d.pixelShaderHlsl && d.pixelShaderHandle && interpolators &&
      interpBytes && pixelConstants && pixelConstDwords && pixelSamplerCount &&
      pixelSamplerCount <= kTranslatedSamplerSlots) {
    // The shader's cbuffer is xe_c[256] followed by xe_texsize[slots], so the
    // buffer must cover BOTH. Sizing it to the constant bank alone would leave
    // the shader reading past the end of the resource for every unnormalized
    // fetch. Rounded up to 256 bytes, the constant-buffer granularity.
    const uint32_t bankBytes = pixelConstDwords * 4;
    const uint32_t texSizeBytes = kTranslatedSamplerSlots * 16;
    const uint32_t constBytes = ((bankBytes + texSizeBytes) + 255u) & ~255u;
    if (createBuffer(d.ivb, interpBytes) && createBuffer(d.pscb, constBytes)) {
      void* p = nullptr;
      D3D12_RANGE none = {0, 0};
      if (SUCCEEDED(d.ivb->Map(0, &none, &p)) && p) {
        std::memcpy(p, interpolators, interpBytes);
        d.ivb->Unmap(0, nullptr);
        d.ivbv.BufferLocation = d.ivb->GetGPUVirtualAddress();
        d.ivbv.SizeInBytes = interpBytes;
        d.ivbv.StrideInBytes =
            kTranslatedInterpolators * 4 * uint32_t(sizeof(float));
        p = nullptr;
        if (SUCCEEDED(d.pscb->Map(0, &none, &p)) && p) {
          std::memset(p, 0, constBytes);
          std::memcpy(p, pixelConstants, bankBytes);
          // xe_texsize, immediately after the bank. An unnormalized fetch
          // multiplies its coordinate by this, so it must be the extent of the
          // texture actually bound at that slot — with one sampler that is this
          // draw's texture. Left zero when there is none, which makes such a
          // fetch read texel 0 rather than something plausible.
          if (d.texture) {
            float ts[4] = {float(d.texture->width), float(d.texture->height),
                           0.0f, 0.0f};
            std::memcpy(static_cast<uint8_t*>(p) + bankBytes, ts, sizeof(ts));
          }
          d.pscb->Unmap(0, nullptr);
          d.translated = true;
        }
      }
    }
    if (!d.translated) {
      d.ivb.Reset();
      d.pscb.Reset();
    }
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

  LogInfo("CreateGameRT: done");
  return true;
}

void D3D12Renderer::PresentGameFrame() {
  if (!m_gameRT) return;

  // Blit path: the finished scene lives in a guest-sized offscreen target, so
  // it has to be scaled to the backbuffer rather than copied. m_viewport already
  // carries the pillarbox, so drawing through it puts the image in the same
  // place the copy path did.
  if (m_presentSourceObject && m_hasGamePipeline && m_presentVB) {
    auto it = m_gameRenderTargets.find(m_presentSourceObject);
    if (it != m_gameRenderTargets.end() && it->second.resource) {
      GameRenderTarget& src = it->second;
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
