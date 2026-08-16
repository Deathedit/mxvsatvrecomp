#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef UNICODE
#define UNICODE
#endif

#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <vector>

namespace mx::hle { struct HleTexturePayload; }

// Bink's Y/Cr/Cb + optional alpha plane set: the largest number of textures a
// single draw binds. Declared here rather than including hle_types.h, which
// this header deliberately does not; d3d12_game.cpp static_asserts that the
// two agree.
inline constexpr uint32_t kMaxDrawPlanes = 4;

class D3D12Renderer {
 public:
  D3D12Renderer() = default;
  ~D3D12Renderer();

  D3D12Renderer(const D3D12Renderer&) = delete;
  D3D12Renderer& operator=(const D3D12Renderer&) = delete;
  D3D12Renderer(D3D12Renderer&&) = delete;
  D3D12Renderer& operator=(D3D12Renderer&&) = delete;

  bool Initialize(HWND hwnd);
  void Shutdown();

  void BeginFrame();
  void EndFrame();


// Append one translated draw to this frame's list. `mvp` is the 16-float
// row-major transform the PM4 translator recovered (DrawCall::mvp); pass
// nullptr to fall back to the identity matrix in m_gameCB. `topology` is a
// D3D_PRIMITIVE_TOPOLOGY value, matching mx::hle::HostTopology.
//
// This replaced SetGameDrawData, which held exactly one draw — so however many
// draws a frame translated, at most one could ever be submitted.

// The guest vertex stage, when this draw runs it on the GPU. Grouped into a
// struct rather than added as six more positional arguments to a call that
// already takes thirty: `AddGameDraw(..., nullptr, 0, nullptr, 0, ...)` is not
// something a reader can check.
//
// All-or-nothing. A null pointer, or any member missing, keeps the draw on the
// CPU interpreter — which is the path for every draw whose vertex or pixel
// shader did not translate, and will be for as long as that is true of any of
// them.
struct GpuVertexStage {
  uint32_t handle = 0;
  std::shared_ptr<const std::string> hlsl;
  // The compiled DXBC matching `hlsl` (the fetch variant's bytecode when the
  // stage is the fetch form). Null falls back to compiling `hlsl` here, which
  // is the pre-cache behaviour and costs an FXC compile per new handle.
  std::shared_ptr<const std::vector<uint8_t>> dxbc;
  // One float4 per declared register per vertex, in `regs` order.
  const uint8_t* inputs = nullptr;
  uint32_t inputBytes = 0;
  // The registers the shader reads, ascending. Element i of the input layout
  // carries register regs[i] at TEXCOORD<regs[i]> — the semantic index is the
  // register number, which is what EmitShaderHlsl declares.
  const uint8_t* regs = nullptr;
  uint32_t regCount = 0;
  // The VERTEX ALU constant bank, constants 0-255 at device+0x780. A different
  // bank from the pixel one; each stage indexes its own from 0.
  const uint32_t* constants = nullptr;
  uint32_t constDwords = 0;

  // --- the fetch variant ----------------------------------------------------
  //
  // When `rawBytes` is set the shader fetches for itself: `hlsl` is the fetch
  // form, `inputs`/`regs` are unused, and the pipeline takes an EMPTY input
  // layout plus a root SRV over the raw buffer. The two forms are not
  // interchangeable, so this pointer is what selects between them.
  const uint8_t* rawBytes = nullptr;
  uint32_t rawByteCount = 0;
  // {base, stride, endian, pad} per emitted fetch, uploaded as the shader's
  // xe_vf[]. 4 dwords each, matching DrawCall::RawFetch.
  const uint32_t* rawFetch = nullptr;
  uint32_t rawFetchCount = 0;

  // --- textures this stage samples -----------------------------------------
  //
  // Zero for almost every draw. A vertex shader that samples is terrain
  // displacement and similar; those used to be refused the GPU path outright
  // and fell to an interpreter with no texture fetch at all, so their samples
  // came back as zeros and their positions were silently wrong.
  //
  // Carried here rather than as five more AddGameDraw parameters, which is
  // already the widest signature in the renderer.
  uint32_t samplerCount = 0;
  uint32_t samplerArrayMask = 0;
  const std::shared_ptr<const mx::hle::HleTexturePayload>* textures = nullptr;
  const uint32_t* sampledObjects = nullptr;
  const uint8_t* samplerSigns = nullptr;
  // Per slot, for snapshot-backed slots only: the guest fetch swizzle. See
  // DrawCall::vertex_sampled_swizzles.
  const uint16_t* sampledSwizzles = nullptr;
};

void AddGameDraw(const uint8_t* vertices, uint32_t vtxBytes, uint32_t vtxStride,
                 const uint8_t* indices, uint32_t idxBytes, bool idx16,
                 uint32_t idxCount, const float* mvp, uint32_t topology,
                 bool depthEnable, bool depthWrite, bool colorWrite,
                 std::shared_ptr<const mx::hle::HleTexturePayload> texture = {},
                 uint32_t targetObject = 0, uint32_t targetWidth = 0,
                 uint32_t targetHeight = 0,
                 uint32_t sampledTargetObject = 0,
                 uint32_t sampledTextureObject = 0,
                 const std::shared_ptr<const mx::hle::HleTexturePayload>* planes
                     = nullptr,
                 uint32_t planeCount = 0, bool yuvHasAlpha = false,
                 bool blendEnable = false, uint32_t srcBlend = 0,
                 uint32_t destBlend = 0, uint32_t blendOp = 0,
                 uint8_t colorSource = 0, uint32_t samplerIndex = 0,
                 uint32_t pixelShaderHandle = 0,
                 std::shared_ptr<const std::string> pixelShaderHlsl = {},
                 std::shared_ptr<const std::vector<uint8_t>> pixelShaderDxbc =
                     {},
                 const uint8_t* interpolators = nullptr,
                 uint32_t interpBytes = 0,
                 const uint32_t* pixelConstants = nullptr,
                 uint32_t pixelConstDwords = 0,
                 uint32_t pixelSamplerCount = 0,
                 const std::shared_ptr<const mx::hle::HleTexturePayload>*
                     pixelTextures = nullptr,
                 const uint32_t* pixelSampledObjects = nullptr,
                 const uint16_t* pixelSampledSwizzles = nullptr,
                 const GpuVertexStage* vertexStage = nullptr,
                 uint32_t pixelSamplerArrayMask = 0,
                 const uint8_t* pixelSamplerSigns = nullptr,
                 uint32_t pixelParamGen = 0,
                 uint32_t depthObject = 0, uint32_t depthWidth = 0,
                 uint32_t depthHeight = 0, uint32_t depthBase = 0,
                 uint32_t targetBase = 0, uint32_t targetColorFormat = 0,
                 // {left, top, right, bottom} in guest render-target pixels, or
                 // null when the guest's scissor register could not be read.
                 // One pointer rather than four more scalars, for the same
                 // reason GpuVertexStage is a struct: a call site reading
                 // `..., 0, 0, 0, 0)` cannot be checked by eye.
                 const int32_t* scissor = nullptr,
                 // RB_COLORCONTROL as read from the guest's register shadow:
                 // bits 0-2 the comparison, bit 3 its enable. Zero means the
                 // registers were unreadable, which decodes as "disabled" and
                 // so keeps the draw on the path it has today.
                 uint32_t alphaControl = 0, float alphaRef = 0.0f);

// Append a resolve to this frame's list, in order with the draws around it.
//
// D3DDevice_Resolve copies a render target into a texture. It used to be
// recorded as a relationship only, which left every draw sampling any texture
// resolved out of a given target bound to that target's single live surface —
// and one guest surface is a shared scratch buffer that the scene and every
// video render into in turn (six distinct textures measured resolving from one
// target in a single run). They all aliased one resource and each showed
// whatever had been drawn most recently.
//
// Ordering is the whole point: the snapshot is the target's contents at this
// position in the frame, so this shares the draw list rather than being
// collected separately.
void AddGameResolve(uint32_t destTexture, uint32_t sourceObject,
                    int32_t destX, int32_t destY, int32_t srcX1, int32_t srcY1,
                    int32_t srcX2, int32_t srcY2,
                    uint32_t destWidth = 0, uint32_t destHeight = 0,
                    bool sourceIsDepth = false, uint32_t sourceBase = 0,
                    uint32_t sourceWidth = 0, uint32_t sourceHeight = 0);

// Append a full-surface colour clear in order with draws and resolves.
void AddGameClear(uint32_t targetObject, uint32_t targetWidth,
                  uint32_t targetHeight, uint32_t targetBase,
                  uint32_t targetColorFormat, uint32_t color,
                  const float* floatColor = nullptr);

// Append a SURFACE BIND in order with draws, clears and resolves: the guest
// named this surface as an attachment, so host storage for it must exist even
// if no draw we route ever targets it. See DrawCall::surface_bind.
void AddGameSurface(uint32_t object, uint32_t width, uint32_t height,
                    uint32_t edramBase, uint32_t colorFormat, bool isDepth);

// Drop the previous frame's draws. Called when a real guest-frame handoff
// arrives, including one whose draws are all filtered; an empty render-thread
// tick still re-presents the previous frame.
void ClearGameDraws();

// Hand this tick's AddGameDraw cost to the phase breakdown reported at the end
// of EndFrame.
//
// Every figure we have had about the render tick so far was inferred from how
// far apart two periodic log lines landed, and two hypotheses built that way --
// a retirement list that failed to drain, and a per-vertex cost -- were both
// wrong. This is the one phase the renderer cannot time for itself: the loop
// runs in the render thread's own function, before BeginFrame is called.
//
// `draws` is the number of AddGameDraw calls, not the number that survived
// them, so a per-call figure can be taken from it.
void ReportAddGameDrawsCost(uint64_t microseconds, uint32_t draws);

  [[nodiscard]] ID3D12Device* GetDevice() const noexcept { return m_device.Get(); }
  [[nodiscard]] ID3D12GraphicsCommandList* GetCommandList() const noexcept {
    return m_commandList.Get();
  }
  [[nodiscard]] uint32_t GetCurrentBackBufferIndex() const noexcept {
    return m_frameIndex;
  }
  [[nodiscard]] uint32_t GetWidth() const noexcept { return m_width; }
  [[nodiscard]] uint32_t GetHeight() const noexcept { return m_height; }

  // Exposed only so d3d12_game.cpp can static_assert it against the emitter's
  // mx::hle::kHlslInterpolatorLinkage. See kTranslatedInterpolators.
  [[nodiscard]] static constexpr uint32_t TranslatedInterpolatorLinkage() {
    return kTranslatedInterpolators;
  }
  // Likewise against mx::hle::HlslShader::kMaxSamplerSlots.
  [[nodiscard]] static constexpr uint32_t TranslatedSamplerTableWidth() {
    return kTranslatedSamplerSlots;
  }

 private:
  static constexpr uint32_t kFrameCount = 3;
  static constexpr DXGI_FORMAT kBackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
  // The offscreen game depth buffer's format. Named because it has to appear in
  // three places that must agree — the resource, its clear value, and the PSO's
  // DSVFormat — and the PSO's copy was the one that got left out.
  // D32_FLOAT_S8X24 rather than D32_FLOAT, to give the guest's stencil a plane:
  // half of every level frame asks for stencil (141,960 of 284,794 draws, 15
  // configurations, two-sided shadow volumes among them). The depth half is
  // deliberately UNCHANGED — still 32-bit float — so this can be judged on
  // "nothing moved". Xenia maps the guest's kD24FS8 the same way
  // (d3d12_render_target_cache.cc:1858).
  static constexpr DXGI_FORMAT kGameDepthFormat =
      DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
  // A depth resource is planar and its views are not interchangeable: typeless
  // so one allocation carries both a DSV and an SRV, plus an SRV of the depth
  // plane alone. The RESOURCE format is also the depth snapshot's, because
  // depth resolves are a CopyTextureRegion and D3D12 wants one typeless family
  // on both ends.
  static constexpr DXGI_FORMAT kGameDepthResourceFormat =
      DXGI_FORMAT_R32G8X24_TYPELESS;
  static constexpr DXGI_FORMAT kGameDepthSrvFormat =
      DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
  // All four depth clears go through this so the stencil plane cannot be left
  // out of one of them. Undefined stencil is invisible until the test is turned
  // on, and then geometry vanishes rather than degrades.
  static constexpr D3D12_CLEAR_FLAGS kGameDepthClearFlags =
      D3D12_CLEAR_FLAGS(D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL);
  // Sampler variants, indexed by these bits. The guest's per-texture address
  // mode reaches the renderer on HleTexturePayload (clamp_x/clamp_y) and used
  // to be discarded in favour of one static WRAP sampler.
  // kSamplerPoint carries the guest's own min/mag filter, decoded onto
  // HleTexturePayload::linear_filter and, until this bit existed, thrown away:
  // every texture was sampled MIN_MAG_MIP_LINEAR whatever the guest asked for.
  // A UI font atlas blitted one texel to one pixel is exactly the case that
  // asks for point, and filtering it linearly blends each glyph edge with its
  // neighbour in both axes -- the smeared menu text.
  // kSamplerBaseMap is the guest's mip_filter == kBaseMap: sample level 0 and
  // never minify past it. It pins MaxLOD back to MinLOD, which is what MaxLOD
  // was unconditionally before the mip chain was uploaded.
  // kSamplerMipPoint is mip_filter == kPoint: pick the nearest level rather
  // than blending the two. Measured at 3,455 of 8,990 chains in one freeroam
  // run -- 38%, which is why it is a variant bit and not a rounding error.
  // Only meaningful where a chain exists, so it is set only then; with one
  // level the two filters are identical and the extra variants would be churn.
  static constexpr uint32_t kSamplerClampU = 1;
  static constexpr uint32_t kSamplerClampV = 2;
  static constexpr uint32_t kSamplerPoint = 4;
  static constexpr uint32_t kSamplerBaseMap = 8;
  static constexpr uint32_t kSamplerMipPoint = 16;
  static constexpr uint32_t kSamplerVariantCount = 32;

  // A shader-visible sampler heap is capped at 2048 descriptors, which is what
  // sizes everything below. The reserved region at the front holds the
  // single-descriptor variants the stand-in path indexes directly; the blocks
  // after it are kTranslatedSamplerSlots wide, because the translated root
  // signature's sampler range is that wide and a table must be contiguous.
  //
  // The two used to share one width, which worked only while the variants
  // happened to fit in a block. The fifth variant bit broke that tie, so they
  // are sized independently now: 32 + 16 * 126 is still exactly 2048, and no
  // run has ever used more than 24 blocks.
  //
  // Blocks are CACHED by their slot configuration rather than allocated per
  // draw: this game uses a handful of distinct ones, so a ring sized for draws
  // was never needed.
  static constexpr uint32_t kSamplerBlockSlots = 16;
  static constexpr uint32_t kSamplerReservedSlots = kSamplerVariantCount;
  static constexpr uint32_t kSamplerBlockCount = 126;
  static constexpr uint32_t kSamplerHeapSize =
      kSamplerReservedSlots + kSamplerBlockSlots * kSamplerBlockCount;
  static_assert(kSamplerHeapSize <= 2048,
                "a shader-visible sampler heap is capped at 2048 descriptors");
  static_assert(kSamplerBlockSlots <= kSamplerReservedSlots,
                "the exhausted-block fallback reads a translated table's worth "
                "of descriptors from the reserved region");
  // Descriptor index of cached block n. The reserved variants come first, so
  // this is not a plain multiply -- every site that offsets into the heap must
  // go through it.
  static constexpr uint32_t SamplerBlockBase(uint32_t block) {
    return kSamplerReservedSlots + block * kSamplerBlockSlots;
  }

  // Frame internals. BeginFrame picks between the two Render* and EndFrame
  // calls PresentGameFrame, so nothing outside this class should ever invoke
  // them: PresentGameFrame's barriers are directional and a second call
  // declares a StateBefore the first one already moved away from. These were
  // public, and the render thread was calling RenderGameFrame and
  // PresentGameFrame a second time in between BeginFrame and EndFrame.
  void RenderGameFrame();
  void PresentGameFrame();

  bool CreateFactory();
  bool CreateDevice();
  bool CreateCommandQueue();
  bool CreateSwapChain();
  bool CreateRtvDescriptorHeap();
  bool CreateRenderTargetViews();
  bool CreateCommandAllocator();
  bool CreateCommandList();
  // Abandon and rebuild the command list after a Close that failed. Without
  // this one failure killed every later frame in the run.
  void RecoverCommandList();
  bool CreateFence();
  void CreateViewportAndScissor();


  bool CreateGamePipeline();
  bool CreateGameRenderTargets();
  // Empties the debug layer's message queue into the log. No-op unless
  // MX_D3D12_DEBUG enabled the layer.
  void DrainD3D12Messages();
  Microsoft::WRL::ComPtr<ID3D12InfoQueue> m_infoQueue;
  // Uploads the Bink plane set into reusable host textures and writes their
  // SRVs into the four descriptors reserved at the head of the heap. Separate
  // from EnsureGameTexture because these planes are new content every video
  // frame: the keyed cache would grow without bound, and the descriptor heap
  // with it.
  //
  // Returns the descriptor index the draw's table must point at. That block is
  // per frame-in-flight and per composite draw, and never reused inside the
  // window the GPU may still be reading — see kYuvPlaneDescriptorCount.
  struct GameDraw;
  bool EnsureYuvPlanes(const GameDraw& draw, uint32_t& descriptorBase);
  bool EnsureGameTexture(const std::shared_ptr<const mx::hle::HleTexturePayload>& texture,
                         uint32_t& descriptorIndex);
  struct GameRenderTarget;
  GameRenderTarget* EnsureGameRenderTarget(
      uint32_t object, uint32_t width, uint32_t height,
      uint32_t edramBase = 0,
      DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM);

  // What distinguishes one pooled offscreen surface from another. Everything
  // else about creating one — the heap properties, the resource desc, claiming
  // an SRV descriptor only AFTER the resource exists, and building the view —
  // is identical for colour targets, resolve snapshots and depth surfaces, and
  // had grown two verbatim copies before this was extracted.
  struct PooledSurfaceSpec {
    DXGI_FORMAT resourceFormat = kBackBufferFormat;
    // Separate from the resource format so a depth surface can be created
    // typeless and read as R32_FLOAT: a DSV-capable resource cannot also carry
    // a depth-typed SRV, which is why depth was unsamplable and every depth
    // resolve missed its source.
    DXGI_FORMAT srvFormat = kBackBufferFormat;
    D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE;
    D3D12_RESOURCE_STATES initialState =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    const D3D12_CLEAR_VALUE* clear = nullptr;
  };
  // Fills entry.resource and entry.srvIndex, or returns false having claimed
  // nothing. Pass reuseSrvIndex to keep an existing descriptor slot when a
  // surface is replaced or grown in place.
  bool CreatePooledSurface(GameRenderTarget& entry, uint32_t width,
                           uint32_t height, const PooledSurfaceSpec& spec,
                           uint32_t reuseSrvIndex);
  // Hand a resource to the retirement list rather than releasing it inline. A
  // command list does not keep its resources alive, so the GPU may still be
  // reading one this frame; releasing inline made every later create of that
  // size fail — 1834 failures in one run.
  void RetireResource(Microsoft::WRL::ComPtr<ID3D12Resource>&& res);

  void WaitForGpu();
  void MoveToNextFrame();

  HWND m_hwnd = nullptr;
  uint32_t m_width = 1280;
  uint32_t m_height = 720;
  bool m_allowTearing = false;
  bool m_initialized = false;

  Microsoft::WRL::ComPtr<IDXGIFactory6> m_factory;
  Microsoft::WRL::ComPtr<ID3D12Device> m_device;
  Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_commandQueue;
  Microsoft::WRL::ComPtr<IDXGISwapChain3> m_swapChain;
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
  std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, kFrameCount> m_renderTargets;
  std::array<Microsoft::WRL::ComPtr<ID3D12CommandAllocator>, kFrameCount> m_commandAllocators;
  Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_commandList;
  Microsoft::WRL::ComPtr<ID3D12Fence> m_fence;
  uint64_t m_fenceValues[kFrameCount] = {};
  uint64_t m_fenceValue = 0;
  HANDLE m_fenceEvent = nullptr;
  uint32_t m_rtvDescriptorSize = 0;
  uint32_t m_frameIndex = 0;
  // The guest's aspect. It renders 1280x720 and nothing tells it otherwise —
  // no video-mode export is hooked — so this is the shape the image must keep
  // however wide the host window is.
  static constexpr float kGuestAspect = 16.0f / 9.0f;
  // The resolution the guest actually renders, for drawing it unscaled.
  static constexpr float kGuestWidth = 1280.0f;
  static constexpr float kGuestHeight = 720.0f;

  D3D12_VIEWPORT m_viewport = {};
  D3D12_RECT m_scissorRect = {};


  Microsoft::WRL::ComPtr<ID3D12RootSignature> m_gameRootSig;
  // Indexed by depth-enable bit 0, depth-write bit 1, no-colour bit 2,
  // textured bit 3, YUV composite bit 4. Opaque draws only.
  std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 32> m_gamePSOs;
  // The same 32 opaque variants for a NON-RGBA8 render target, built on demand.
  // Keyed (format << 8) | variant. The guest's HDR chain -- the 320x180 and
  // 160x90 luminance targets are k_2_10_10_10_FLOAT and the 64x64 down to 1x1
  // reduction is k_16_16_FLOAT -- cannot be drawn by a pipeline that declares
  // RGBA8, and drawing it into an RGBA8 surface clamped the log-average to
  // [0,1] before anything could read it.
  std::unordered_map<uint32_t, Microsoft::WRL::ComPtr<ID3D12PipelineState>>
      m_gamePSOsByFormat;
  ID3D12PipelineState* OpaquePSO(
      uint32_t variant, DXGI_FORMAT rtvFormat,
      D3D12_PRIMITIVE_TOPOLOGY_TYPE topoType =
          D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
  // The host format an offscreen target takes for a given guest
  // ColorRenderTargetFormat (RB_COLOR_INFO bits [16:19]).
  static DXGI_FORMAT HostColorFormat(uint32_t guestColorFormat);

  // Blended draws, built on demand and keyed by the state they need.
  //
  // Not more bits in the array above: src factor, dest factor and op are three
  // guest enums, and enumerating their product up front would be thousands of
  // pipelines to cover the handful a frame actually uses. Blend modes repeat
  // heavily within a frame, so a cache converges after the first few and costs
  // a hash lookup per draw thereafter.
  struct BlendKey {
    uint32_t pso_index = 0;   // the five bits above
    uint32_t src = 0;         // D3DBLEND
    uint32_t dest = 0;        // D3DBLEND
    uint32_t op = 0;          // D3DBLENDOP
    // The render target's format. A PSO declares the format it writes, and
    // binding it to an RTV of a different one is invalid: once offscreen
    // targets stopped all being RGBA8, one cached pipeline per blend mode was
    // no longer enough.
    DXGI_FORMAT rtvFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    // The topology GROUP the pipeline declares. A draw whose topology is in a
    // different group is refused by the runtime and renders nothing, so this
    // belongs in the key for the same reason rtvFormat does.
    D3D12_PRIMITIVE_TOPOLOGY_TYPE topoType =
        D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    bool operator==(const BlendKey& o) const noexcept {
      return pso_index == o.pso_index && src == o.src && dest == o.dest &&
             op == o.op && rtvFormat == o.rtvFormat && topoType == o.topoType;
    }
  };
  struct BlendKeyHash {
    size_t operator()(const BlendKey& k) const noexcept {
      return (size_t(k.pso_index) << 24) ^ (size_t(k.src) << 16) ^
             (size_t(k.dest) << 8) ^ size_t(k.op) ^
             (size_t(k.rtvFormat) << 32) ^ (size_t(k.topoType) << 44);
    }
  };
  // Bounded so an unrecognised state cannot grow this without limit; past the
  // cap a draw falls back to its opaque PSO rather than being dropped.
  static constexpr size_t kMaxBlendPSOs = 128;
  std::unordered_map<BlendKey, Microsoft::WRL::ComPtr<ID3D12PipelineState>,
                     BlendKeyHash>
      m_blendPSOs;
  // Returns the blended pipeline for this draw, or nullptr to draw it opaque.
  ID3D12PipelineState* BlendedPSO(const BlendKey& key);
  // Retained so BlendedPSO can rebuild a variant; the description is identical
  // to the opaque one apart from the blend state.
  D3D12_GRAPHICS_PIPELINE_STATE_DESC m_gamePsoTemplate = {};
  std::vector<D3D12_INPUT_ELEMENT_DESC> m_gameInputLayout;
  Microsoft::WRL::ComPtr<ID3DBlob> m_gameVsBlob;
  std::array<Microsoft::WRL::ComPtr<ID3DBlob>, 3> m_gamePsBlobs;  // plain/tex/yuv

  //=========================================================================
  // Translated guest pixel shaders.
  //
  // A guest pixel shader that EmitShaderHlsl accepted arrives on the draw as
  // HLSL source. It is compiled and turned into a pipeline once per shader
  // handle and cached here — a frame issues ~158 draws across a few dozen
  // shaders, so compiling per draw would cost more than the translation saves.
  //
  // Deliberately a SEPARATE root signature from m_gameRootSig. The emitted
  // shaders declare their own resource layout — a constant bank at b1 and one
  // texture/sampler pair per guest sampler slot — which does not fit the
  // stand-in pipeline's four-plane table. Sharing one signature would mean
  // changing the layout the working path depends on, to suit a path that does
  // not render yet.
  //=========================================================================
  // Width of the translated pipeline's texture and sampler tables. Must equal
  // mx::hle::HlslShader::kMaxSamplerSlots: the emitter declares its textures
  // contiguously from t0 up to that cap, and a table narrower than the
  // registers a shader declares is invalid. static_asserted in d3d12_game.cpp.
  //
  // Small on purpose. Guest sampler indices are remapped to compact slots
  // precisely so this stays narrow — binding at the guest index would make the
  // 14-fetch shader's s8-s12 need a thirteen-wide table to deliver five
  // textures, and one block that size per draw exhausts the heap within a
  // frame.
  // The descriptor table width. Every block costs this many descriptors, so it
  // multiplies against kMaxTranslatedBlocks into the heap size — see there.
  static constexpr uint32_t kTranslatedSamplerSlots = 16;
  static_assert(kTranslatedSamplerSlots == kSamplerBlockSlots,
                "one sampler per texture slot, in one contiguous table");
  // The VS-to-PS linkage width. MUST equal mx::hle::kHlslInterpolatorLinkage:
  // the emitted pixel shader declares its input struct with that many
  // interpolators, and a vertex stage offering a different count produces two
  // signatures that cannot link — CreateGraphicsPipelineState then fails with
  // no message at all, which is exactly how this first went wrong.
  //
  // Restated rather than included, following kMaxDrawPlanes above: this header
  // deliberately does not include the hle headers. d3d12_game.cpp
  // static_asserts that the two agree.
  static constexpr uint32_t kTranslatedInterpolators = 8;
  // Bounded for the same reason as m_blendPSOs: a shader set that grew without
  // limit would otherwise grow this without limit.
  //
  // The bound was 256 and was never sized against a loaded level. Measured in
  // mx_1264: the menu and intro fit inside it, freeroam does not. The 256th
  // pipeline is built 84s in, at level load, and from that instant every draw
  // whose key is new falls back to the stand-in — 331,785 of 1,312,382 draws,
  // 25% of the level, drawn flat for the rest of the run. That is not a memory
  // bound doing its job, it is a silent 25% correctness hole, and it looks
  // exactly like "assets are not loading".
  //
  // The key is (ps handle, vs handle, blend src/dest/op, flags, rtv format,
  // topology), so the working set is a shader set times its output-merger
  // states; a few thousand is the expected order for a level. The bytecode is
  // cached per handle either side of this, so a new key past first sight costs
  // one CreateGraphicsPipelineState, not a compile.
  static constexpr size_t kMaxTranslatedPSOs = 4096;
  Microsoft::WRL::ComPtr<ID3D12RootSignature> m_translatedRootSig;
  Microsoft::WRL::ComPtr<ID3DBlob> m_translatedVsBlob;
  struct TranslatedPipeline {
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pso;
    bool failed = false;  // compiled or created badly; do not retry every draw
  };
  // Keyed on the shader AND the output-merger state, not the shader alone.
  //
  // Keying on the shader alone is what turned every translated overlay into an
  // opaque rectangle: the pipeline was built with the default blend state, so a
  // blended draw — text, UI, anything alpha — painted its whole quad. The
  // stand-in path has always honoured this state via BlendedPSO, and a
  // translated draw that ignored it was a regression, not a translation error.
  struct TranslatedKey {
    uint32_t handle = 0;
    // The guest vertex shader paired with it, or 0 for the passthrough stage
    // that forwards CPU-interpreted results. Part of the key because the two
    // stages are linked into one pipeline: the same pixel shader under a
    // different vertex shader is a different PSO with a different input layout.
    uint32_t vsHandle = 0;
    uint32_t src = 0, dest = 0, op = 0;
    // 1 depth, 2 depth write, 4 no colour, 8 blend,
    // 16 the vertex stage is the FETCH variant of vsHandle -- a different
    // compilation of the same guest shader, with an empty input layout and a
    // raw-buffer SRV instead of input elements. Without this bit the two
    // variants would share a PSO and whichever compiled first would be used for
    // both, which is a mismatched input layout rather than a visible error.
    uint8_t flags = 0;
    // See BlendKey::rtvFormat.
    DXGI_FORMAT rtvFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    // See BlendKey::topoType.
    D3D12_PRIMITIVE_TOPOLOGY_TYPE topoType =
        D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    bool operator==(const TranslatedKey& o) const noexcept {
      return handle == o.handle && vsHandle == o.vsHandle && src == o.src &&
             dest == o.dest && op == o.op && flags == o.flags &&
             rtvFormat == o.rtvFormat && topoType == o.topoType;
    }
  };
  struct TranslatedKeyHash {
    size_t operator()(const TranslatedKey& k) const noexcept {
      return (size_t(k.handle) << 20) ^ (size_t(k.vsHandle) << 28) ^
             (size_t(k.src) << 12) ^ (size_t(k.dest) << 6) ^
             (size_t(k.op) << 3) ^ size_t(k.flags) ^
             (size_t(k.rtvFormat) << 40) ^ (size_t(k.topoType) << 50);
    }
  };
  std::unordered_map<TranslatedKey, TranslatedPipeline, TranslatedKeyHash>
      m_translatedPSOs;
  // Compile `hlsl` and build a pipeline for it, or return null. Caches both
  // outcomes against the key, so a shader that fails is not recompiled once per
  // draw for the rest of the run.
  //
  // `draw` supplies the vertex stage: its translated VS source and the register
  // list the input layout is built from, when key.vsHandle is non-zero.
  ID3D12PipelineState* TranslatedPSO(const TranslatedKey& key,
                                     const std::string& hlsl,
                                     const GameDraw& draw);
  // Compiled bytecode per VERTEX shader handle, alongside the pixel one below.
  std::unordered_map<uint32_t, Microsoft::WRL::ComPtr<ID3DBlob>>
      m_translatedVsBlobs;
  // The fetch variant of the same handles, in its own map. One map keyed by
  // handle alone would hand a draw the other variant's bytecode, whose input
  // signature does not match the layout the PSO declares.
  std::unordered_map<uint32_t, Microsoft::WRL::ComPtr<ID3DBlob>>
      m_translatedVsFetchBlobs;
  // Compiled bytecode per shader handle, so one shader used with several blend
  // states is compiled once rather than once per state.
  std::unordered_map<uint32_t, Microsoft::WRL::ComPtr<ID3DBlob>>
      m_translatedPsBlobs;
  bool CreateTranslatedRootSignature();
  uint64_t m_translatedOk = 0;
  uint64_t m_translatedFailed = 0;
  // Per-frame split of draws that ran the guest's own pixel shader against
  // those that kept the stand-in. Reported together, because the translated
  // count alone cannot say whether the path is carrying the frame or a corner
  // of it.
  uint64_t m_translatedDraws = 0;
  uint64_t m_standInDraws = 0;
  // Of the translated draws, how many also ran the guest's VERTEX shader. This
  // is the one that says whether the CPU interpreter is still the frame.
  uint64_t m_gpuVertexDraws = 0;
  // Of those, the ones whose vertex stage also fetched its own attributes.
  uint64_t m_gpuVertexFetchDraws = 0;
  // Draws that brought a vertex stage and could not be given one, so had to be
  // dropped: their vertices were never transformed by anything. Must stay at
  // zero — anything else is geometry missing from the picture.
  uint64_t m_gpuVertexDropped = 0;

  // Draws whose guest scissor actually clips something, and draws whose scissor
  // register could not be read. `clipped` at zero means honouring the scissor
  // changed nothing on screen -- which is a finding, not a silence, so it is
  // reported either way. `unreadable` must stay at zero: a draw with no
  // readable scissor is drawn unclipped, which is the old behaviour and the
  // bug this pair exists to detect the return of.
  uint64_t m_scissorClipped = 0;
  uint64_t m_scissorUnreadable = 0;

  // A shader's textures have to sit in ONE contiguous descriptor range, and the
  // cached per-texture descriptors in m_gameSrvHeap are scattered — a texture
  // gets its slot when it is first uploaded, not when a shader binds it. So a
  // translated draw gets a fresh block here and its views are created into it.
  //
  // Not a copy from m_gameSrvHeap: D3D12 forbids CopyDescriptors from a
  // shader-visible heap, so the views are created directly.
  //
  // Ring-allocated and never rewritten within a frame, because overwriting a
  // descriptor the GPU may still be reading is undefined — the video-plane
  // block above was moved off a shared block for exactly this reason, and it
  // read as zero on this hardware rather than failing.
  // Per-draw descriptor blocks, PARTITIONED BY FRAME IN FLIGHT: frame f owns
  // [f*kTranslatedBlocksPerFrame, (f+1)*kTranslatedBlocksPerFrame). The ring
  // used to be one shared range reset in ClearGameDraws, which runs only when
  // the guest hands off a new draw list — while blocks are consumed by
  // RenderGameFrame, which runs every HOST frame and replays the previous list
  // when the guest has not handed off. Blocks therefore accumulated across
  // every replay: measured, ~125 translated draws a frame exhausted 2048 after
  // roughly sixteen frames, and block-exhausted became the dominant reason a
  // translatable draw fell back (7789 against 381 for every other cause).
  //
  // Slicing per frame lets the window reset every host frame while still only
  // rewriting blocks the GPU finished with kFrameCount frames ago, which is
  // what putting the reset in ClearGameDraws was protecting.
  // 3072 rather than 4096 because the slot count doubled to 16: the heap is
  // kMaxTranslatedBlocks * kTranslatedSamplerSlots descriptors, and 4096 * 16
  // would be 65536 — exactly the Resource Binding Tier 1 cap for a
  // shader-visible CBV/SRV/UAV heap, with no margin. 3072 * 16 = 49152 keeps
  // headroom, and 1024 blocks per frame in flight is still eight times the
  // measured per-frame demand of ~125.
  static constexpr uint32_t kMaxTranslatedBlocks = 3072;
  static constexpr uint32_t kTranslatedBlocksPerFrame =
      kMaxTranslatedBlocks / kFrameCount;
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_translatedSrvHeap;
  uint32_t m_translatedBlockNext = 0;
  uint32_t m_translatedBlockLimit = kTranslatedBlocksPerFrame;
  uint64_t m_translatedBlockExhausted = 0;
  // Why a draw with a translated shader fell back to the stand-in anyway. All
  // four used to be a silent `return false`, which made "translated 17243,
  // stand-in 670699" impossible to act on: it could not distinguish a shader
  // we cannot bind from a resource we cannot find.
  uint64_t m_translatedNoSnapshot = 0;
  uint64_t m_translatedNoTexture = 0;
  uint64_t m_translatedUploadFailed = 0;
  // A slot the shader declared Texture2DArray whose bound resource has only one
  // slice, so every cube face reads slice 0. Not a failure -- the draw still
  // runs -- but it means the shader and the fetch constant disagree about the
  // texture's dimension, which would otherwise be invisible.
  uint64_t m_translatedArraySlotNot2DArray = 0;
  // Points `out` at a block holding this draw's textures. False when a block
  // could not be allocated or a slot had no resource, in which case the draw
  // must fall back rather than sample an undefined descriptor.
  // Writes xe_texsign for the VERTEX stage into the tail of its constant
  // buffer. Defaults to 1.0 rather than the surrounding memset's 0, which the
  // shader would apply as "sample becomes constant white".
  static void FillVertexTextureSigns(const GameDraw& d, uint8_t* cb,
                                     uint32_t cbBytes, uint32_t constDwords);
  // `vertex` fills from the VERTEX stage's slot arrays instead of the pixel
  // ones; the caller binds the result to a different root parameter.
  bool BindTranslatedTextures(const GameDraw& d,
                              D3D12_GPU_DESCRIPTOR_HANDLE& out,
                              bool vertex = false);
  // Points `out` at a block holding one sampler per texture slot, matching the
  // per-slot filter and address mode the guest asked for.
  //
  // The translated root signature's sampler range has always been
  // kTranslatedSamplerSlots wide, but the bind pointed at a four-descriptor
  // heap offset by a single per-draw variant index -- so slot 1 of any
  // multi-sampler shader read the next variant along, and slot 4 onwards read
  // off the end of the heap entirely. This makes the table as wide as the range
  // that describes it.
  bool BindTranslatedSamplers(const GameDraw& d,
                              D3D12_GPU_DESCRIPTOR_HANDLE& out,
                              bool vertex = false);
  // Draws whose vertex stage sampled a texture, and those refused because its
  // descriptor tables could not both be bound.
  uint64_t m_vertexSampledDraws = 0;
  uint64_t m_vertexSampleBindFailed = 0;
  // Sampler blocks, keyed by their slot configuration. Distinct
  // configurations, not draws: the cache is what keeps 16-wide blocks inside a
  // 2048-descriptor heap.
  //
  // The key is the variants themselves rather than a packing of them. It was
  // three bits per slot in a uint64_t, which stopped fitting at five bits
  // across sixteen slots (80). Hashing them down would have reintroduced the
  // possibility of two configurations colliding onto one block -- silently
  // giving a draw someone else's filter -- so the whole tuple is the key.
  std::map<std::array<uint8_t, kSamplerBlockSlots>, uint32_t> m_samplerBlocks;
  uint32_t m_samplerBlockNext = 0;
  uint64_t m_samplerBlockExhausted = 0;
  static D3D12_SAMPLER_DESC SamplerVariantDesc(uint32_t variant);
  static uint32_t SamplerVariantFor(const mx::hle::HleTexturePayload& tex);
  // The fallback transform: an identity matrix, used by any translated draw
  // whose own constant buffer failed to allocate. Bound as a root CBV, so it
  // needs no descriptor heap.
  Microsoft::WRL::ComPtr<ID3D12Resource> m_gameCB;
  // Present blit. Once resolve sources render into their own guest-sized
  // offscreen targets, the final scene is 1280x720 and the backbuffer is
  // window-sized, so PresentGameFrame can no longer be a CopyTextureRegion —
  // that requires matching dimensions. It becomes a fullscreen triangle drawn
  // through the ordinary game pipeline, which is already a textured-quad
  // pipeline with an MVP root CBV and an SRV at t0. Reusing it rather than
  // adding a present pipeline keeps the scaling on a proven path, and m_gameCB
  // already holds the identity matrix the blit wants.
  //
  // Three vertices, not four: a triangle that overhangs the viewport covers it
  // with no seam down the diagonal.
  Microsoft::WRL::ComPtr<ID3D12Resource> m_presentVB;
  D3D12_VERTEX_BUFFER_VIEW m_presentVbv = {};
  // The final full-backbuffer colour resolve before VdSwap is the guest's
  // completed frame. Prefer that immutable snapshot for presentation; the
  // draw-target object remains a fallback for frames that contain no usable
  // full-size resolve.
  uint32_t m_presentResolveTexture = 0;
  // Which offscreen target was written last, or 0 for m_gameRT. This is only
  // the fallback now -- a render target may be shared scratch storage whose
  // contents change again after the guest resolved the frame from it.
  uint32_t m_presentSourceObject = 0;
  bool CreatePresentQuad();
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_gameSrvHeap;
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_samplerHeap;
  uint32_t m_samplerDescriptorSize = 0;
  uint32_t m_gameSrvDescriptorSize = 0;
  uint32_t m_nextGameSrvDescriptor = 0;
  static constexpr uint32_t kMaxGameTextures = 1024;
  struct GameTexture {
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    // One upload buffer per frame in flight. A texture whose contents the
    // guest rewrites is re-uploaded while earlier frames may still be copying
    // out of their own buffer, so they cannot share one.
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, kFrameCount> upload;
    uint32_t descriptorIndex = 0;
    // The payload content_version this resource was last filled from. A
    // mismatch means the guest repacked the texture under a stable key and the
    // resource is showing stale bytes -- see HleTexturePayload::content_version.
    uint32_t uploadedVersion = 0;
    // m_fenceValue when this texture was last bound. The LRU stamp for
    // eviction, and the guard that stops a texture in use by the frame being
    // recorded from being evicted out from under it.
    uint64_t lastUsedFence = 0;
  };
  // Every surface object ever seen as a resolve source, and ever sampled by a
  // later draw. Historical rather than per frame because the offscreen routing
  // decision is taken when a surface is drawn into, which is usually an earlier
  // frame than the resolve or sample that proves it needed the storage. See the
  // note at the top of the draw loop.
  std::unordered_set<uint32_t> m_everResolveSource;
  std::unordered_set<uint32_t> m_everSampledTarget;
  // Every object ever named as a draw's render target. A resolve source that
  // never appears here was never drawn into through this path at all, so no
  // routing decision could ever have given it an offscreen surface -- a
  // different defect from one that was drawn and routed to m_gameRT.
  std::unordered_set<uint32_t> m_everDrawTarget;
  // resolve source object -> how many resolves it lost for want of an offscreen
  // surface, over the whole run.
  std::map<uint32_t, uint64_t> m_missingSourceCounts;
  // Which condition of the translated gate a stand-in draw failed. Exactly one
  // is charged per draw, in the order the gate tests them.
  uint64_t m_standInNoHlsl = 0;
  uint64_t m_standInNoHandle = 0;
  uint64_t m_standInNoVertexInputs = 0;
  uint64_t m_standInNoConstants = 0;
  uint64_t m_standInTooManySamplers = 0;
  // And the three ways a draw that PASSED that gate still reaches the stand-in.
  // Without these the gate counters describe 16% of the population and the rest
  // is silent -- measured 2026-08-12: 54985 stand-in, 8833 charged to the gate,
  // 346 to a bind failure, 45806 to nothing at all.
  uint64_t m_standInBufferFailed = 0;   // gate passed, ivb/pscb never mapped
  uint64_t m_translatedPsoCapped = 0;   // PSO cache at kMaxTranslatedPSOs
  uint64_t m_translatedNoRootSig = 0;   // root signature or VS blob absent
  std::unordered_map<uint64_t, GameTexture> m_gameTextures;
  // SRV slots returned by evicted textures, safe to hand out again. Without
  // this m_nextGameSrvDescriptor is a pure bump allocator and the heap is a
  // one-way ratchet: the cache key is an FNV-1a hash over all six fetch-constant
  // dwords INCLUDING base_address, so the same artwork streamed to a different
  // guest allocation is a new key and burns another slot forever. Measured
  // 53 -> 740 of 1024 in five minutes of swapping riders, with live render
  // targets flat at 27/256 throughout. On exhaustion EnsureGameTexture returns
  // false and every texture after it renders as vertex colour, permanently.
  std::vector<uint32_t> m_freeGameSrvDescriptors;
  // Evict down to this before the heap is exhausted rather than at the wall.
  // Hitting the cap is not a soft failure -- it is untextured for the rest of
  // the process -- so the cache is kept clear of it by a margin.
  // Verified by temporarily forcing this to 96, which made eviction run
  // continuously instead of never: 656 evictions, srv flat at 230/1024, the
  // free list cycling 2..16, evict-blocked 0, no device-removed and no page
  // fault. At the real threshold a menu session peaks around 293 cached and
  // this never fires -- which is why it had to be tested at a value that does.
  static constexpr uint32_t kGameTextureHighWater = kMaxGameTextures * 7 / 8;
  uint64_t m_gameTextureEvictions = 0;
  uint64_t m_gameTextureEvictBlocked = 0;
  void EvictGameTexturesToHighWater();
  bool UploadGameTexture(GameTexture& entry,
                         const mx::hle::HleTexturePayload& src);
  // The video plane descriptors sit at the head of the heap; the general
  // allocator starts above them.
  //
  // One block of kMaxDrawPlanes per composite draw per frame in flight, rather
  // than a single shared block. A shared block has to be rewritten every frame,
  // because the planes are new resources whenever the video's dimensions change
  // — and rewriting a descriptor the GPU may still be reading is undefined in
  // D3D12. It read as zero on this hardware, so the composite sampled black
  // from four correctly-populated textures. Cached textures never hit this:
  // their descriptor is written once and never touched again.
  //
  // Frame-index striping is the same guarantee the per-frame upload buffers
  // already rely on — MoveToNextFrame waits out the frame kFrameCount ago
  // before its slots come round again.
  static constexpr uint32_t kYuvPlaneDescriptorBase = 0;
  static constexpr uint32_t kMaxYuvDrawsPerFrame = 4;
  static constexpr uint32_t kYuvPlaneDescriptorCount =
      kFrameCount * kMaxYuvDrawsPerFrame * kMaxDrawPlanes;
  // Composite draws recorded so far this frame; picks the block above.
  uint32_t m_yuvDrawsThisFrame = 0;
  struct YuvPlane {
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    // One staging buffer per frame in flight: the plane is rewritten every
    // frame, and MoveToNextFrame only guarantees the frame kFrameCount ago has
    // retired.
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, kFrameCount> upload;
    uint32_t width = 0;
    uint32_t height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  };
  std::array<YuvPlane, kMaxDrawPlanes> m_yuvPlanes;
  bool m_hasGamePipeline = false;

  // A range inside one page of the per-draw upload ring. Replaces the
  // ComPtr<ID3D12Resource> each of these used to be.
  //
  // MEASURED, mx_1033, steady-state main menu: a 1815ms render tick spent
  // 1031ms creating per-draw buffers and 743ms destroying them -- 97.7% of the
  // tick -- for 1476 CreateCommittedResource calls at ~683us each, 4.3 per draw.
  // The GPU waited 0ms. The frame cost was never the rendering; it was the
  // allocator, and the guest sat in SetDrawCalls behind all of it.
  //
  // A committed resource is its own kernel-mode video-memory allocation, so a
  // few hundred microseconds each is simply what it costs. The fix is not to
  // make the call cheaper but to stop making it: suballocate from a handful of
  // large pages that live for the whole session.
  struct UploadAlloc {
    D3D12_GPU_VIRTUAL_ADDRESS gpu = 0;
    uint8_t* cpu = nullptr;  // inside a persistently mapped page
    uint32_t size = 0;
    explicit operator bool() const noexcept { return cpu != nullptr; }
  };

  // One page of that ring. Created once and recycled forever after.
  struct UploadPage {
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    uint8_t* cpu = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS gpu = 0;
    uint32_t size = 0;
    uint32_t used = 0;
    // The submission that last read this page. It cannot be reset until the
    // fence passes this.
    uint64_t fence = 0;
    // Referenced by the CURRENT m_gameDraws. Not the same question as the fence:
    // an empty render tick re-records the same draw list, so its pages stay live
    // across however many fences pass, until the guest hands over a new frame
    // and ClearGameDraws releases them.
    bool live = false;
  };

  // 8MB rather than one page per frame: the menu frame measured ~10MB of
  // per-draw data, so a page is a fraction of a frame and the ring settles near
  // the true working set instead of rounding up to it. Growth is logged.
  static constexpr uint32_t kUploadPageBytes = 8u * 1024u * 1024u;
  // The strictest of the three requirements -- root CBVs need 256, root SRVs
  // need their element size, vertex and index buffers less again -- applied to
  // all of them so no call site has to know which it is.
  static constexpr uint32_t kUploadAlign = 256;
  std::vector<UploadPage> m_uploadPages;
  uint32_t m_uploadPage = UINT32_MAX;  // the page being filled
  uint64_t m_uploadBytesThisFrame = 0;

  // Suballocate `bytes` from the ring. The returned range is already mapped:
  // there is no Map/Unmap pair, which is the other per-draw cost this removes.
  bool AllocUpload(UploadAlloc& out, uint32_t bytes);

  // One translated draw. The CB is per-draw rather than one shared buffer so it
  // is not rewritten while the GPU may still be reading the previous frame's
  // value — now a distinct range of the ring rather than a distinct resource.
  struct GameDraw {
    UploadAlloc vb;
    UploadAlloc ib;
    UploadAlloc cb;
    D3D12_VERTEX_BUFFER_VIEW vbv = {};
    D3D12_INDEX_BUFFER_VIEW ibv = {};
    uint32_t indexCount = 0;
    D3D12_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    bool depthEnable = false;
    bool depthWrite = false;
    bool colorWrite = true;
    // The guest's D3DRS_* blend state, translated in BlendedPSO.
    bool blendEnable = false;
    uint32_t srcBlend = 0;
    uint32_t destBlend = 0;
    uint32_t blendOp = 0;
    // The guest's scissor, in its own render-target pixels. See the note on
    // DrawCall::scissor_left for why this comes from the register and not from
    // D3DDevice_SetScissorRect.
    bool scissorSeen = false;
    int32_t scissorLeft = 0, scissorTop = 0, scissorRight = 0, scissorBottom = 0;
    std::shared_ptr<const mx::hle::HleTexturePayload> texture;
    uint32_t targetObject = 0;
    uint32_t targetWidth = 0;
    uint32_t targetHeight = 0;
    // EDRAM tile base of the colour target. The guest gives one EDRAM
    // allocation several surface OBJECTS -- 0x2653FDA0 is drawn into and
    // 0x2653FF20 is resolved out of, both 129x129 at base 0x2D0 -- so object
    // identity alone cannot connect a resolve to the surface that holds its
    // contents.
    uint32_t targetBase = 0;
    // Guest ColorRenderTargetFormat for that target, RB_COLOR_INFO[16:19].
    uint32_t targetColorFormat = 0;
    // Ordered full-surface D3DDevice_Clear. Carries no geometry.
    bool colorClear = false;
    uint32_t clearColor = 0;  // D3DCOLOR A8R8G8B8.
    bool clearColorIsFloat = false;
    std::array<float, 4> clearColorFloat = {};
    // Ordered SURFACE BIND. Carries no geometry and draws nothing: it exists so
    // host storage is created when the guest NAMES a surface rather than when a
    // draw first targets one. Reuses targetObject/Width/Height/Base and
    // targetColorFormat above, since a bind describes the same thing a draw's
    // target does. See DrawCall::surface_bind for why this record exists.
    bool surfaceBind = false;
    bool surfaceBindIsDepth = false;
    // The guest's depth surface for this draw, by object identity. Offscreen
    // colour targets used to get no depth attachment at all.
    uint32_t depthObject = 0;
    uint32_t depthWidth = 0;
    uint32_t depthHeight = 0;
    // EDRAM tile base of that depth surface, so a banded pass can be stitched
    // back together when the guest resolves the whole of it. See resolveSourceBase.
    uint32_t depthBase = 0;
    // The vertex declaration carried no COLOR element, so the {1,1,1,1} in the
    // vertex buffer is a seed rather than data — a modulation identity for the
    // textured shader. If such a draw is not textured, that value is emitted
    // literally as opaque white and is pure fabrication. See RenderGameFrame.
    uint8_t colorSource = 0;  // mx::hle::DrawCall::ColorSource
    // kSamplerClampU/V, from the guest's fetch constant.
    uint32_t samplerIndex = 0;
    uint32_t sampledTargetObject = 0;
    // Which resolve result to sample, where sampledTargetObject only says which
    // surface produced it. Several textures resolve out of one shared target.
    uint32_t sampledTextureObject = 0;
    // Non-zero marks this entry a resolve rather than a draw: snapshot
    // `resolveSource`'s target into the texture `resolveDest` names, here, in
    // draw order. It carries no geometry.
    uint32_t resolveDest = 0;
    uint32_t resolveSource = 0;
    // Source slot 4: the depth surface, which lives in its own pool. One
    // guest object can be bound as depth in one place and colour in another,
    // so the object alone cannot say which pool to search.
    bool resolveSourceIsDepth = false;
    // EDRAM tile base of the resolve source. The shadow pass renders two depth
    // bands (768x640 at base 0x580, 768x384 at base 0x710) and then resolves
    // the whole 768x1024 through a THIRD object that aliases band 0's base and
    // that no draw ever binds -- so an object-identity lookup misses it and the
    // shadow map never reaches the shader that samples it. Base ordering is
    // what puts the bands back in the right vertical order.
    uint32_t resolveSourceBase = 0;
    uint32_t resolveSourceWidth = 0;
    uint32_t resolveSourceHeight = 0;
    // Placement of this band within the destination, and the sub-rectangle of
    // the source it takes. `resolveSrcX2 == 0` means the whole source.
    int32_t resolveDestX = 0;
    int32_t resolveDestY = 0;
    int32_t resolveSrcX1 = 0;
    int32_t resolveSrcY1 = 0;
    int32_t resolveSrcX2 = 0;
    int32_t resolveSrcY2 = 0;
    // The destination texture's declared extent. The snapshot is sized to this
    // rather than to the region this resolve covers -- see
    // DrawCall::resolve_dest_width.
    uint32_t resolveDestWidth = 0;
    uint32_t resolveDestHeight = 0;
    // Bink's Y/Cr/Cb (+ optional alpha) plane set, bound together.
    std::array<std::shared_ptr<const mx::hle::HleTexturePayload>,
               kMaxDrawPlanes> planes;
    uint32_t planeCount = 0;
    bool yuvHasAlpha = false;
    bool yuvComposite = false;
    // The guest pixel shader translated to HLSL, when it translated. Null keeps
    // this draw on the tex*col stand-in. The handle is the PSO cache key, so
    // the source is only read the first time a shader is seen.
    uint32_t pixelShaderHandle = 0;
    std::shared_ptr<const std::string> pixelShaderHlsl;
    // Precompiled bytecode for pixelShaderHlsl when the hooks-side content
    // cache held it; see GpuVertexStage::dxbc for the fallback contract.
    std::shared_ptr<const std::vector<uint8_t>> pixelShaderDxbc;
    // The guest interpolator stream, as a second vertex buffer, and the guest's
    // pixel constant bank. Both are required for the translated path: without
    // them the shader would read undefined inputs and compute from zeros, which
    // is a confident wrong answer rather than a visible failure. `translated`
    // is only set once every piece is present.
    UploadAlloc ivb;
    D3D12_VERTEX_BUFFER_VIEW ivbv = {};
    UploadAlloc pscb;
    uint32_t pixelSamplerCount = 0;
    // Bit i set = the shader declares slot i as Texture2DArray (a cube fetch),
    // so its SRV must be TEXTURE2DARRAY. A descriptor whose dimension
    // contradicts the shader's declaration is undefined behaviour, not just a
    // wrong colour.
    uint32_t pixelSamplerArrayMask = 0;
    // One texture per compact sampler slot, in the order the shader declares
    // them. Parallel arrays: a slot names either a CPU payload or a resolved
    // render target, never both.
    std::array<std::shared_ptr<const mx::hle::HleTexturePayload>,
               kTranslatedSamplerSlots> pixelTextures;
    std::array<uint32_t, kTranslatedSamplerSlots> pixelSampledObjects = {};
    std::array<uint16_t, kTranslatedSamplerSlots> pixelSampledSwizzles = {};
    // Per slot, bit c set = host component c of this fetch is
    // kUnsignedBiased and the shader must expand it as 2*c-1. Already
    // permuted into host component order by the hooks side.
    std::array<uint8_t, kTranslatedSamplerSlots> pixelSamplerSigns = {};

    // The VERTEX stage's samplers, same shape as the pixel ones above. Bound
    // through a SEPARATE descriptor range (t17+/s16+) because the two stages
    // are translated and cached independently, so their compact slot 0 names
    // different guest samplers. Zero for the overwhelming majority of draws --
    // a vertex shader that samples is terrain displacement and similar.
    uint32_t vertexSamplerCount = 0;
    uint32_t vertexSamplerArrayMask = 0;
    std::array<std::shared_ptr<const mx::hle::HleTexturePayload>,
               kTranslatedSamplerSlots> vertexTextures;
    std::array<uint32_t, kTranslatedSamplerSlots> vertexSampledObjects = {};
    std::array<uint16_t, kTranslatedSamplerSlots> vertexSampledSwizzles = {};
    std::array<uint8_t, kTranslatedSamplerSlots> vertexSamplerSigns = {};

    // Zero when disabled, otherwise one plus SQ_CONTEXT_MISC.param_gen_pos.
    uint32_t pixelParamGen = 0;
    // The guest alpha test, uploaded into the pixel cbuffer's xe_alphatest.
    // Raw RB_COLORCONTROL rather than the decoded pair, so a wrong bit
    // assignment shows up as a wrong number here instead of a plausible test.
    uint32_t alphaControl = 0;
    float alphaRef = 0.0f;
    bool translated = false;

    // This draw is a guest DEPTH pass: a translated vertex stage with NO pixel
    // shader, paired with shaders::kTranslatedDepthOnlyPS. Set only when the
    // draw cannot write colour, so the stand-in's output is discarded by a write
    // mask that is already zero — see the note on that shader.
    //
    // Distinct from `depthOnlyPass` in RenderGameFrame, which asks whether a
    // COLOUR TARGET is bound. These draws mostly do bind one; what they do not
    // do is write to it.
    bool depthOnlyStandIn = false;

    // The guest VERTEX shader on the GPU. `gpuVertex` is only set once every
    // piece is present, on the same all-or-nothing rule as `translated`: a
    // vertex stage missing its attribute stream or its constant bank computes
    // from undefined inputs, and geometry built from that is a confident wrong
    // answer rather than a visible failure.
    //
    // When set, `vsvb` REPLACES the CPU-transformed `vb` at slot 0 and the
    // interpolator stream at slot 1 is not bound at all — the rasterizer
    // interpolates what the vertex stage exports, which is the entire point.
    uint32_t vertexShaderHandle = 0;
    std::shared_ptr<const std::string> vertexShaderHlsl;
    // Precompiled bytecode for vertexShaderHlsl (fetch or inputs form,
    // whichever `hlsl` carries) — same contract as pixelShaderDxbc.
    std::shared_ptr<const std::vector<uint8_t>> vertexShaderDxbc;
    UploadAlloc vsvb;
    D3D12_VERTEX_BUFFER_VIEW vsvbv = {};
    UploadAlloc vscb;
    // The registers the translated vertex shader reads, ascending — the input
    // layout the PSO must be built with. Carried per draw and not derived from
    // the handle because the PSO cache is what turns it back into a layout.
    std::array<uint8_t, 32> vertexInputRegs = {};
    uint32_t vertexInputCount = 0;
    bool gpuVertex = false;

    // The FETCH variant: the vertex stage reads the guest's raw vertex buffer
    // through a root SRV and decodes attributes itself. `vsvb` and the input
    // layout are then both unused -- there are no input elements at all, only
    // SV_VertexID -- and `rawvb` is bound as root parameter 4.
    UploadAlloc rawvb;
    bool gpuVertexFetch = false;
  };
  // Bounded because each entry costs three CreateCommittedResource calls — see
  // the PERF(per-frame-allocs) note in d3d12_game.cpp.
  //
  // Raised from 256 once resolves began sharing this list. They are interleaved
  // through the stream and mostly land in its tail, so an overrunning frame lost
  // them first: measured 340 resolves dropped in one run, which froze every
  // snapshot at its last contents while draws went on sampling them. The cap now
  // has to cover draws AND resolves for the whole frame, not draws alone.
  static constexpr size_t kMaxGameDraws = 4096;
  std::vector<GameDraw> m_gameDraws;

  // Resources whose last GPU use was in the frame that signalled `fence`.
  //
  // A D3D12 command list does not reference-count the resources it references —
  // recording a draw against a buffer keeps nothing alive. ClearGameDraws used
  // to release every draw's vb/ib/cb directly, once per frame, while the
  // previous frame's command list was still in flight on the queue, so the GPU
  // could be reading UPLOAD-heap memory the CPU had already freed. Resources
  // now move here instead and are released only once m_fence has passed the
  // value signalled for the submission that last used them.
  struct RetiredFrame {
    uint64_t fence = 0;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> res;
    // Game-texture SRV slots freed with this batch. A descriptor is not safe to
    // overwrite the moment its texture is evicted: it lives in a SHADER-VISIBLE
    // heap, and any command list already submitted may still dereference it.
    // Carried on the same fence as the resources for exactly that reason --
    // same lifetime question, same answer.
    std::vector<uint32_t> srv;
  };
  std::deque<RetiredFrame> m_retired;

  // Release everything the GPU has finished with. Cheap and called per frame.
  void DrainRetired();

  // RENDER-THREAD PHASE TIMING.
  //
  // The render tick is the whole of the frame cost: the guest's own VdSwap never
  // once exceeded 50ms (`VdSwap orig` has zero log lines in mx_1032), its
  // translation work is ~470ms of `FRAME COST`, and the remaining ~3.7s is the
  // guest parked in SetDrawCalls waiting for this thread. Yet nothing here was
  // ever timed, so which of the four phases spends it is unknown -- and the
  // spacing-of-log-lines estimates that stood in for a measurement produced two
  // hypotheses that were both wrong.
  //
  // Microseconds, accumulated for one tick and reset when it is reported. The
  // four are disjoint and, with the sleep, sum to the tick period, so a phase
  // that fails to account for the growth is ruled out rather than argued about.
  uint64_t m_phaseAddDrawsUs = 0;    // the caller's AddGameDraw loop
  uint32_t m_phaseAddDraws = 0;      // how many calls that was
  uint64_t m_phaseRecordUs = 0;      // RenderGameFrame
  uint64_t m_phaseSubmitUs = 0;      // PresentGameFrame + Close + Execute + Present
  uint64_t m_phaseFenceWaitUs = 0;   // MoveToNextFrame waiting on the GPU
  uint64_t m_phaseRetireUs = 0;      // DrainRetired
  // Wall clock between consecutive EndFrame exits: the tick period the guest is
  // actually blocked behind, including the render thread's 16ms sleep.
  std::chrono::steady_clock::time_point m_tickEnd{};

  // Print the split and reset it. Called at both of EndFrame's exits, so a tick
  // that died on a failed Close still reports where it had got to.
  void ReportTickPhases();

  // CreateCommittedResource, counted and timed.
  //
  // The leading suspect for the growth is the upload-heap churn in AddGameDraw
  // (see the PERF note there): ~4,800 committed resources created and retired
  // per guest frame, ~1.4 million over a session. The call COUNT is fixed by the
  // draw count and is not the question; whether each call gets slower as the
  // driver's allocator fragments is. Reported as a mean per call so that is
  // legible directly.
  uint64_t m_committedCalls = 0;
  uint64_t m_committedUs = 0;
  HRESULT CreateTimedCommittedResource(const D3D12_HEAP_PROPERTIES* heap,
                                       D3D12_HEAP_FLAGS flags,
                                       const D3D12_RESOURCE_DESC* desc,
                                       D3D12_RESOURCE_STATES state,
                                       const D3D12_CLEAR_VALUE* clear,
                                       REFIID riid, void** out);

  Microsoft::WRL::ComPtr<ID3D12Resource> m_gameRT;
  Microsoft::WRL::ComPtr<ID3D12Resource> m_gameDepth;
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_gameRtvHeap;
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_gameDsvHeap;
  uint32_t m_gameRtvDescriptorSize = 0;
  // 64 was a bring-up number sized against the front end, which uses 22. A
  // LOADED LEVEL does not fit: measured 2026-08-12 with --force_load=NAT_Farm,
  // this pinned at 64/64 with 5530 draws refused and OVERPAINT climbing to
  // 2765, against the "OVERPAINT 0, refusals 0, 22/64" that a menu-only run
  // reports and that AGENTS.md had recorded as healthy.
  //
  // The consequence is the reason force_load stopped being usable for looking
  // at level geometry. A refused draw falls back to the MAIN target and paints
  // over the scene, and m_gameRenderTargets is never evicted -- so once a level
  // pushes past the cap it stays past it for the rest of the run, and the
  // fallback is silent apart from that one counter.
  //
  // Same defect and same fix as kMaxGameSnapshots below, found the same day:
  // a cap sized against the menu, met by scene content. RTV descriptors are
  // non-shader-visible and cost nothing to reserve, and the real bound is the
  // SRV budget (kMaxGameTextures), checked on the same line and sitting at
  // 301 of 1024 in that run. Raised to 4x the point of failure rather than
  // uncapped, so a runaway allocator is still caught.
  static constexpr uint32_t kMaxGameRenderTargets = 256;
  // Snapshots get their OWN cap, and it is not this one. 64 bounds the RTV heap
  // (`kMaxGameRenderTargets + 1` descriptors, see the heap desc), and a snapshot
  // has no RTV -- EnsureGameSnapshot sets rtvIndex = 0 and says so. Charging
  // snapshots against the RTV budget anyway is what broke the 129x129
  // post-process chain: measured 2026-08-12, 50 targets + 33 snapshots = 83
  // against 64, so every NEW resolve destination past that point was refused,
  // its resolve dropped on the `if (!snap) continue`, and the six draws that
  // sampled those destinations fell to the tex*col stand-in.
  //
  // 128 rather than uncapped: the observed steady state is 33, so this is ~4x
  // headroom, and a runaway allocator is worth catching. The real bound is the
  // SRV budget (kMaxGameTextures), which is checked on the same line and sat at
  // 359 of 1024 in that run.
  static constexpr uint32_t kMaxGameSnapshots = 128;
  // Offscreen render-target routing counters. Reported by RenderGameDraws.
  // m_rtDrawsOverpaint is the one that matters: a draw that asked for its own
  // target, was refused, and therefore painted onto the main scene instead.
  uint64_t m_rtDrawsMain = 0;
  uint64_t m_rtDrawsOffscreen = 0;
  uint64_t m_rtDrawsOverpaint = 0;
  uint64_t m_rtRejectBudget = 0;
  uint64_t m_rtRejectResized = 0;
  // Snapshot creations refused for budget. Its own counter because a snapshot
  // refusal and an offscreen-target refusal have different causes and different
  // fixes, and reporting both as "routing refused: budget" hid the former.
  uint64_t m_snapshotRejectBudget = 0;
  // Resolve snapshots. m_snapshotFallbacks is the one that matters: a draw that
  // wanted a snapshot, found none, and fell back to sampling the source
  // target's live surface — which is the old aliasing behaviour, so a large
  // steady count means resolves are being dropped upstream rather than that the
  // fix is working.
  uint64_t m_snapshotCopies = 0;
  uint64_t m_snapshotHits = 0;
  uint64_t m_snapshotFallbacks = 0;
  // Draws that wanted a resolved-target sample and had nothing to bind. These
  // used to fall through to the untextured PSO and paint fabricated opaque
  // white over the frame; they are now skipped. See RenderGameFrame.
  uint64_t m_sampleMissSkipped = 0;
  // Draws whose guest alpha test is ENABLED, split by whether the path they
  // actually took can honour it. The discard lives in the translated pixel
  // shader, so a draw that fell to the stand-in gets no test at all and still
  // paints its masked-away pixels.
  //
  // Two counters rather than one because the second is the only thing that
  // says whether this change is finished: `honoured` rising on its own proves
  // the plumbing works, and only `standIn` staying at zero proves there is no
  // second population still rendering as filled quads. A single total could
  // not tell those apart.
  uint64_t m_alphaTestHonoured = 0;
  uint64_t m_alphaTestStandIn = 0;
  // Draws whose colour output was remapped from the guest's -32...32 fixed
  // point into -1...1. Zero here would mean the scale never fired and any
  // improvement came from something else.
  uint64_t m_fixed16Scaled = 0;
  // DIAG: WHITE-SKIPPED draws grouped by target extent.
  struct SkipTargetInfo {
    uint64_t count = 0;
    uint64_t translated = 0;
    uint64_t wantedSlots = 0;
    uint32_t object = 0;
  };
  std::map<uint64_t, SkipTargetInfo> m_skipByTarget;
  // Resolves whose source target has never been drawn into: the snapshot they
  // produce is blank by construction.
  uint64_t m_snapshotBlankSource = 0;
  // ...and WHICH ones, because the bare count above has been quoted for three
  // days without anyone able to say what it covers. Measured 2026-08-16: it
  // reads 353 in a two-minute menu-only run AND 353 in a nine-minute run that
  // also loaded a level -- identical, so the whole population lands during
  // boot/legal/loading/start and never grows. That is the same set of screens
  // reported as having no background, which is why this breakdown exists.
  //
  // Keyed by resolve-source extent so one line covers a surface rather than an
  // event. `rescue*` record why the substitution search at the top of the
  // resolve path failed to find a drawn stand-in, which is the actual question:
  // a blank source with no candidate at its EDRAM base is a different defect
  // from one whose candidates were all blank too.
  struct BlankSourceInfo {
    uint64_t count = 0;
    uint32_t object = 0;
    uint32_t edramBase = 0;
    uint32_t format = 0;
    uint32_t dest = 0;
    uint64_t rescueNoCandidate = 0;   // nothing else at this EDRAM base
    uint64_t rescueAllBlank = 0;      // candidates existed, none everDrawn
    uint64_t rescueNotAttempted = 0;  // depth source, or no base/extent to search
    uint64_t firstFrame = 0;
    uint64_t lastFrame = 0;
  };
  std::map<uint64_t, BlankSourceInfo> m_blankSourceByExtent;
  // Resolves served from a surface twice their size in each axis at the same
  // EDRAM base and format -- the 1x partner of a 4x-MSAA source, which is the
  // image we actually hold because we render everything at 1x.
  uint64_t m_msaaPartnerResolves = 0;
  // Draws that wanted a snapshot which exists but was left unrefreshed by a
  // dropped resolve. Each one is a full previous frame NOT painted over this
  // one. A large count means the drop upstream is the defect, not this refusal.
  uint64_t m_snapshotStaleRefused = 0;
  // Monotonic game-frame counter, for snapshot age at sample time.
  uint64_t m_gameFrame = 0;
  // Age histogram of FULL-SCREEN snapshots (>= half the presented width) at the
  // moment a draw samples one: refreshed this frame, last frame, 2-9, 10-99,
  // 100+ frames ago. A large 100+ bucket means whole leftover frames are being
  // composited over the current one. Small snapshots are excluded because
  // static compositor content is legitimately old and would swamp the signal.
  uint64_t m_snapshotAge[5] = {};
  uint64_t m_snapshotMissingSource = 0;
  // Resolves lost because the frame's draw list was already full, and snapshot
  // resources that failed to create. Both exist to tell a fix that stopped
  // working apart from one that never ran, and both have been non-zero for real
  // reasons.
  // Depth resolves satisfied by stitching EDRAM bands rather than by an
  // object-identity hit. Non-zero means the shadow pass is reaching its shader.
  uint64_t m_depthBandResolves = 0;
  // Colour resolves matched to their source by EDRAM base rather than by
  // object identity, because the guest named the storage twice.
  uint64_t m_aliasedSourceResolves = 0;
  // PROVISIONAL: resolves satisfied from a LARGER host surface at the same
  // EDRAM base, by taking the top rows. The multisample-alias case.
  uint64_t m_containedSourceResolves = 0;
  uint64_t m_resolvesDroppedFull = 0;
  uint64_t m_snapshotCreateFailed = 0;
  // A snapshot recreated because the resolve source's format changed under it.
  // Each one loses the bands already resolved for one frame; a large number
  // means two sources of different formats are alternating into one texture,
  // which would thrash and wants a per-format snapshot instead.
  uint64_t m_snapshotFormatChanged = 0;
  // The 1x1 auto-exposure result on its way back to the guest. The guest reads
  // the resolve destination's bytes out of its own memory rather than sampling
  // them (mx::hle::g_luminanceReadbackBits explains why that matters), so the
  // value has to make the round trip through host memory.
  //
  // One buffer per frame in flight, drained at the top of the frame that
  // reuses the slot: MoveToNextFrame has already waited out the frame
  // kFrameCount ago, so that slot's copy is complete and mapping it cannot
  // stall. The cost is a couple of frames of latency on a value the guest
  // filters over time anyway.
  // Generous enough that GetCopyableFootprints for any single small texel can
  // never need more, and a round multiple of the 512-byte placement alignment.
  static constexpr uint32_t kLuminanceReadbackBytes = 4096;
  // One buffer per frame in flight, carved into slots 512 bytes apart so each
  // 1x1 resolve in the frame gets its own. 512 is
  // D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, the minimum spacing a placed
  // footprint may use.
  static constexpr uint32_t kLuminanceSlotStride = 512;
  std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, kFrameCount>
      m_luminanceReadback;
  std::array<uint32_t, kFrameCount> m_luminancePending = {};
  // Mirrors mx::hle::kMaxLuminanceReadbacks; this header deliberately does not
  // include hle_types.h, and d3d12_game.cpp static_asserts the two agree.
  static constexpr uint32_t kMaxLuminanceSlots = 4;
  std::array<std::array<uint32_t, kMaxLuminanceSlots>, kFrameCount>
      m_luminanceDestObject = {};
  uint64_t m_luminanceReadbacks = 0;
  uint32_t m_luminanceLastBits = 0;
  void DrainLuminanceReadback();
  void QueueLuminanceReadback(GameRenderTarget* snap, uint32_t destObject);
  struct GameRenderTarget {
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t rtvIndex = 0;
    uint32_t srvIndex = 0;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    bool usedThisFrame = false;
    // Whether anything has EVER been drawn into this target. A resolve whose
    // source has never been drawn into is copying a surface that holds only its
    // creation clear — the snapshot is then blank, and a compositor quad paints
    // that blank over the frame.
    bool everDrawn = false;
    // Set on create and on resize, cleared once the surface has actually been
    // cleared on the GPU. Pooled surfaces are RECYCLED, so a fresh entry's
    // contents are whatever the previous tenant left behind -- which is why the
    // everDrawn comment above ("holds only its creation clear") was aspirational
    // rather than true: nothing performed that clear. A surface the guest binds
    // and resolves without ever drawing into must read as its documented
    // creation value -- the far plane for depth, transparent black for colour --
    // and not as a stale image from an unrelated pass.
    bool needsInitialClear = true;
    // Snapshots only. Set when the guest asked to resolve into this texture and
    // we could not perform the copy, so the contents are a KNOWN-WRONG earlier
    // frame rather than merely an old one. Snapshots legitimately persist across
    // frames — static compositor content is resolved once and sampled for many
    // frames — so age is not evidence of staleness, but a dropped refresh is.
    // Binding one paints a whole previous frame over the current one.
    bool stale = false;
    // Snapshots only: the frame counter value at the last successful copy into
    // this snapshot. Age at sample time is the measurement that separates a
    // static compositor image -- resolved once, sampled for many frames, and
    // legitimately old -- from a leftover full-screen frame that nothing has
    // refreshed and that a draw is about to paint over the current one.
    uint64_t lastCopyFrame = 0;
    // Depth targets only: the EDRAM tile base this surface was rendered at.
    uint32_t edramBase = 0;
    // The format this surface was created with, so a PSO can be built to match
    // it and a snapshot can be created in the same format.
    DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
  };
  std::unordered_map<uint32_t, GameRenderTarget> m_gameRenderTargets;
  // Resolve snapshots, keyed by DESTINATION TEXTURE object rather than by the
  // source target — that distinction is the entire fix. Same struct and same
  // budget as the offscreen targets above, so exhaustion is reported through
  // the existing refusal counters.
  std::unordered_map<uint32_t, GameRenderTarget> m_gameSnapshots;
  // `width`/`height` are the REQUIRED MINIMUM extent, not the exact size: a
  // snapshot is assembled from one or more resolve bands and must cover the
  // union of them. An existing snapshot that is already at least this large is
  // returned untouched; a smaller one grows, preserving what it holds.
  GameRenderTarget* EnsureGameSnapshot(uint32_t destTexture, uint32_t width,
                                       uint32_t height,
                                       DXGI_FORMAT format = kBackBufferFormat);
  // Per-object depth surfaces, keyed like the colour targets. Created
  // R32_TYPELESS so the same resource can carry a D32_FLOAT DSV for rendering
  // and an R32_FLOAT SRV for the depth resolve; a D32_FLOAT resource can do
  // only the first, which is why depth was unresolvable.
  std::unordered_map<uint32_t, GameRenderTarget> m_gameDepthTargets;
  GameRenderTarget* EnsureGameDepthTarget(uint32_t object, uint32_t width,
                                          uint32_t height, uint32_t edramBase);
  // Descriptor 0 stays the main-target depth created by CreateGameRenderTargets;
  // the rest are stable slots for guest depth-surface identities.
  // 16 was sized against the four depth surfaces a DRAW ever named. Surfaces
  // are now created when the guest BINDS them, and the menu run binds twelve
  // distinct ones -- close enough to the old cap that a busier scene would
  // start losing them to m_rtRejectBudget, which fails silently and looks
  // exactly like the missing-surface bug this change exists to fix.
  static constexpr uint32_t kMaxGameDepthTargets = 32;
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_gameDepthDsvHeap;
  uint32_t m_gameDsvDescriptorSize = 0;
  // Depth resolves served from a per-object depth surface. Separate from the
  // colour counters so the two cannot be confused when reading whether this
  // path is working at all.
  uint64_t m_depthResolves = 0;
  // Stand-in draws refused a depth snapshot as their one colour texture.
  uint64_t m_standInDepthSnapshotRefused = 0;

  // Whether each resolve DESTINATION carries depth or colour, recorded from
  // d.resolveSourceIsDepth as the resolve arrives. Read only when a translated
  // draw names a destination we hold no snapshot for, to say WHICH KIND of
  // image went missing -- see BindTranslatedTextures.
  std::unordered_map<uint32_t, bool> m_resolveDestIsDepth;
  // What a missing snapshot WOULD have held, split three ways. Diagnostic only:
  // every one of these still fails the draw. The split is what showed the
  // missing snapshots are mostly destinations no resolve ever named (448 of 613
  // in mx_960), not depth-sourced ones as the mx_958 counters had suggested.
  uint64_t m_noSnapshotDepth = 0;
  uint64_t m_noSnapshotColour = 0;
  uint64_t m_noSnapshotUnknown = 0;
  // Surfaces instantiated because the guest BOUND them, not because a draw
  // targeted them. The loss the no-snapshot counters above measure has to move
  // into these rather than merely disappear from the accounting -- a counter
  // going quiet has already been mistaken for a fix once this cycle.
  uint64_t m_bindCreatedDepth = 0;
  uint64_t m_bindCreatedColour = 0;
  // Surfaces instantiated at RESOLVE time because the guest resolved out of one
  // it never bound as an attachment we saw.
  uint64_t m_resolveCreatedSources = 0;
};
