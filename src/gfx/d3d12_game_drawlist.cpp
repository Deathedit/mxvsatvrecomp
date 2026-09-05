// D3D12Renderer -- the draw list: everything that appends to it, and its
// lifecycle.
//
// Gathered from two files. AddGameDraw, ClearGameDraws and DrainRetired came
// from d3d12_game_frame.cpp; the five specialised appends -- second target,
// resolve, colour clear, depth clear and surface bind -- came from
// d3d12_game_resource.cpp. They are one public API and were split 3/5 across
// two translation units for no reason anyone recorded, so a caller reading
// "what can I put on the draw list" had to find both.
//
// The distinction that DOES matter is kept: this file BUILDS the list,
// d3d12_game_frame.cpp CONSUMES it. Nothing here touches the command list.

#include "gfx/d3d12_renderer.h"

#include "gfx/d3d12_game_internal.h"
#include "gfx/d3d12_internal.h"
#include "gfx/d3d12_shaders.h"
#include "gpu/d3d9_layout.h"
#include "gpu/guard_census.h"
#include "gpu/hle_types.h"
#include "gpu/shader_hlsl.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

using mx::gfx::LogError;
using mx::gfx::LogInfo;

void D3D12Renderer::ClearGameDraws() {
  // Release the ring pages this draw list was holding.
  //
  // This is the whole of what used to be here. Every per-draw buffer was handed
  // to the fenced retirement list to be destroyed a frame or two later, and the
  // destruction cost as much as the creation: 743ms of an 1815ms menu tick,
  // against 1031ms to create them. Neither exists now -- a range of a page is
  // not a resource, and a page is reset rather than freed. The fence protection
  // moved rather than went away: a page carries the submission it was last read
  // under, and `live` is the separate condition this clears.
  //
  // Done before the empty check on purpose. AddGameDraw can allocate and then
  // fail out before appending, so pages can be live with no draw referencing
  // them; leaving those marked would retire them from the ring permanently.
  for (auto& p : m_uploadPages) p.live = false;
  m_uploadBytesThisFrame = 0;

  if (m_gameDraws.empty()) return;
  m_gameDraws.clear();
  // The descriptor block window is NOT reset here. This runs on guest handoff,
  // not per host frame, and blocks are consumed per host frame — which is what
  // exhausted the ring. RenderGameFrame opens each frame's own slice instead.
}

void D3D12Renderer::DrainRetired() {
  if (m_retired.empty() || !m_fence) return;
  const uint64_t completed = m_fence->GetCompletedValue();
  while (!m_retired.empty() && m_retired.front().fence <= completed) {
    // The descriptors come back on the same fence as the resources. Returning
    // them any earlier would let a live command list sample a slot that has
    // been rewritten to point at a different texture.
    for (uint32_t index : m_retired.front().srv)
      m_freeGameSrvDescriptors.push_back(index);
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
                                 std::shared_ptr<const std::vector<uint8_t>> pixelShaderDxbc,
                                 const uint8_t* interpolators,
                                 uint32_t interpBytes,
                                 const uint32_t* pixelConstants,
                                 uint32_t pixelConstDwords,
                                 uint32_t pixelSamplerCount,
                                 const std::shared_ptr<const mx::hle::HleTexturePayload>* pixelTextures,
                                 const uint32_t* pixelSampledObjects,
                                 const uint16_t* pixelSampledSwizzles,
                                 const GpuVertexStage* vertexStage,
                                 uint32_t pixelSamplerArrayMask,
                                 const uint8_t* pixelSamplerSigns,
                                 uint32_t pixelParamGen,
                                 uint32_t depthObject, uint32_t depthWidth,
                                 uint32_t depthHeight, uint32_t depthBase,
                                 uint32_t targetBase,
                                 uint32_t targetColorFormat,
                                 const int32_t* scissor,
                                 uint32_t alphaControl, float alphaRef,
                                 uint32_t cullMode, uint32_t vtxCntl,
                                 uint32_t guestVpWidth,
                                 uint32_t guestVpHeight, bool useGuestVp,
                                 bool edramCopy, const GameStencil* stencil,
                                 const GameOmState* om) {
  // PERF(per-frame-allocs): DONE. This used to create an ID3D12Resource on the
  // UPLOAD heap for each of the buffers below -- up to nine per call, once per
  // submitted draw. They are now ranges of the AllocUpload ring, and the only
  // committed resources left in this path are the ring's own pages.
  //
  // What it cost, measured before the change: a steady-state main menu tick of
  // 1815ms spent 1031ms creating those resources and 743ms destroying them --
  // 97.7% of the tick -- against 17ms to record the frame and 0ms waiting for
  // the GPU. This comment used to claim D3D12's command-list tracking keeps the
  // underlying memory alive until the GPU is done with it; that is FALSE, it was
  // a D3D11 guarantee, and lifetime is now the ring's job.
  //
  // A fetch draw brings no host vertex buffer at all: its geometry arrives in
  // vertexStage->rawBytes and the shader reads it through the root SRV, so a
  // null `vertices` is correct rather than a malformed draw. Tested before the
  // gate, which would otherwise drop every one of them.
  // OM ARRIVALS, counted before the early returns below so this population is
  // the caller's. `arrived` against the caller's own count says whether a
  // clip-disabled draw ever gets this far; `clipOff` staying 0 while the
  // caller sees ~1000 means those draws are rejected below, not that the
  // field was lost.
  if (om) {
    ++m_omDraws;
    if (!om->depthClip) ++m_omDrawsClipOff;
    // PARTIAL is neither 0xF nor 0: counting `!= 0xF` counts every depth-only
    // draw and makes a 1-in-200,000 state look like a quarter of the frame.
    if ((om->colourMask & 0xFu) != 0xFu && (om->colourMask & 0xFu) != 0u)
      ++m_omDrawsMasked;
    // Normalised to the default unless the draw depth-tests, so this counts
    // draws where the zfunc can change the outcome.
    if (om->zfunc != GameOmState{}.zfunc) ++m_omDrawsZfunc;
    if (om->separateAlpha) ++m_omDrawsSepAlpha;
  }
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

  // Was a CreateCommittedResource per buffer; now a bump allocation out of the
  // ring. Kept as a lambda of the same shape so the call sites below read the
  // same way, and because the ring hands back memory that is already mapped,
  // each of them also loses its Map/Unmap pair.
  auto createBuffer = [&](UploadAlloc& buf, uint32_t size) -> bool {
    return AllocUpload(buf, size);
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
    void* vtxMap = d.vb.cpu;
    memcpy(vtxMap, vertices, vtxBytes);
    // The fixed Bink YUV shader replaces the guest pixel shader, but it must
    // preserve that shader's final modulation:
    //
    //   export = decoded_yuva * c0
    //
    // kGameYuvPS performs that multiply through its COLOR input. Bink uses an
    // UP/FVF quad with no guest COLOR element, so the transcode supplies white
    // there; leaving it white would make the video ignore c0 entirely.
    //
    // Host layout: position float4 @0, color float4 @16, uv float2 @32.
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
    d.vbv.BufferLocation = d.vb.gpu;
    d.vbv.StrideInBytes = vtxStride;
    d.vbv.SizeInBytes = vtxBytes;
  }

  if (!createBuffer(d.ib, idxBytes)) return;
  memcpy(d.ib.cpu, indices, idxBytes);
  d.ibv.BufferLocation = d.ib.gpu;
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
  // BEFORE the constant buffers are filled, not after. FillVertexNdcOffset reads
  // d.halfPixel and the viewport extents, and these used to be assigned ~680
  // lines further down -- so the fill saw halfPixel 0.0 and wrote a zero offset
  // on every draw while the shader dutifully applied it. The census read
  // "applied 174478, skipped 0" because it was incremented beside the
  // assignment, measuring a later moment than the one that used the value.
  d.guestVpWidth = guestVpWidth;
  d.guestVpHeight = guestVpHeight;
  d.useGuestVp = useGuestVp;
  d.halfPixel = (vtxCntl != 0xFFFFFFFFu && (vtxCntl & 1u) == 0) ? 0.5f : 0.0f;
  if (d.halfPixel != 0.0f) ++m_halfPixelDraws; else ++m_halfPixelSkipped;
  d.targetHeight = targetHeight;
  d.sampledTargetObject = sampledTargetObject;
  d.sampledTextureObject = sampledTextureObject;
  d.colorSource = colorSource;
  d.samplerIndex = samplerIndex;
  d.pixelShaderHandle = pixelShaderHandle;
  d.pixelShaderHlsl = std::move(pixelShaderHlsl);
  d.pixelShaderDxbc = std::move(pixelShaderDxbc);
  d.pixelSamplerCount = pixelSamplerCount;
  d.pixelSamplerArrayMask = pixelSamplerArrayMask;
  d.pixelParamGen = pixelParamGen;
  d.alphaControl = alphaControl;
  d.alphaRef = alphaRef;
  if (pixelSamplerSigns)
    std::memcpy(d.pixelSamplerSigns.data(), pixelSamplerSigns,
                d.pixelSamplerSigns.size());
  d.depthObject = depthObject;
  d.depthWidth = depthWidth;
  d.depthHeight = depthHeight;
  d.depthBase = depthBase;
  d.targetBase = targetBase;
  d.targetColorFormat = targetColorFormat;
  // DIAG: every draw aimed at a SQUARE POWER-OF-TWO offscreen target, grouped by
  // the frame it landed in.
  //
  // The terrain's ground meshes take their whole world Y from three texture
  // samples -- their vertex buffer carries only grid X and Z -- and the dominant
  // term is a 512x512 that reads min = max = 0. The guest's own
  // `ps_hft_deform_copy` copies the heightfield into it, but only for tiles a
  // track segment has reached, so on an untouched map the buffer has to be
  // filled at level load.
  //
  // ONE LINE PER FRAME THAT HAS ANY, not per draw: a level load is thousands of
  // frames and the interesting ones are the handful that draw here at all.
  if (targetWidth == targetHeight && targetWidth >= 128 &&
      (targetWidth & (targetWidth - 1)) == 0) {
    static std::mutex s_mu;
    static uint64_t s_total = 0;
    static uint32_t s_lines = 0;
    static uint32_t s_lastFrame = 0xFFFFFFFFu;
    static uint32_t s_inFrame = 0;
    static uint32_t s_lastObject = 0;
    std::lock_guard<std::mutex> lk(s_mu);
    ++s_total;
    if (m_gameFrame != s_lastFrame) {
      if (s_inFrame && s_lines < 200) {
        ++s_lines;
        REXLOG_INFO(
            "d3d12: SQUARE TARGET frame {}: {} draws, last object 0x{:08X} "
            "{}x{} guest colour format {} (total {})",
            s_lastFrame, s_inFrame, s_lastObject, targetWidth, targetHeight,
            targetColorFormat, static_cast<unsigned long long>(s_total));
      }
      s_lastFrame = m_gameFrame;
      s_inFrame = 0;
    }
    ++s_inFrame;
    s_lastObject = targetObject;
  }
  if (pixelTextures && pixelSampledObjects) {
    for (uint32_t i = 0; i < kTranslatedSamplerSlots; ++i) {
      d.pixelTextures[i] = pixelTextures[i];
      d.pixelSampledObjects[i] = pixelSampledObjects[i];
      d.pixelSampledSwizzles[i] =
          pixelSampledSwizzles ? pixelSampledSwizzles[i] : uint16_t(0);
    }
  }
  // The vertex stage's own textures, bound through a separate descriptor range.
  // Copied here, before the translated gate below, so that the sign table the
  // fetch path writes into the cbuffer tail can read them.
  if (vertexStage && vertexStage->samplerCount && vertexStage->textures &&
      vertexStage->sampledObjects) {
    d.vertexSamplerCount = vertexStage->samplerCount;
    d.vertexSamplerArrayMask = vertexStage->samplerArrayMask;
    for (uint32_t i = 0; i < kTranslatedSamplerSlots; ++i) {
      d.vertexTextures[i] = vertexStage->textures[i];
      d.vertexSampledObjects[i] = vertexStage->sampledObjects[i];
      d.vertexSampledSwizzles[i] = vertexStage->sampledSwizzles
                                       ? vertexStage->sampledSwizzles[i]
                                       : uint16_t(0);
    }
    if (vertexStage->samplerSigns)
      std::memcpy(d.vertexSamplerSigns.data(), vertexStage->samplerSigns,
                  d.vertexSamplerSigns.size());
  }

  // Does this draw bring the guest's own vertex stage? Decided BEFORE the
  // translated gate below, because it changes what that gate requires: a draw
  // running the guest vertex shader has no interpolator stream and must not have
  // one. Demanding it anyway turned every qualifying draw into a stand-in draw
  // holding vertices the interpreter no longer transformed -- a flat red frame,
  // and zero translated draws in a run where 36,064 qualified.
  //
  // Two valid shapes, and a fetch stage has NONE of the input-element fields by
  // design -- its only input is SV_VertexID -- so requiring them unconditionally
  // took a frame from 339 draws to 28.
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
  // without its interpolators reads undefined registers, and one run without its
  // constants computes from zeros -- both produce a confident wrong picture
  // rather than a visible failure. "Its interpolators" means the CPU-built
  // stream only when the CPU built the vertices; with the guest vertex shader
  // running, the rasterizer produces them.
  //
  // WHICH of the six conditions sent this draw to the stand-in. Without it the
  // only numbers available are measured at two different points -- ~475k D3D9
  // draw attempts against ~52k submitted draws -- so they cannot be subtracted.
  //
  // no-HANDLE is tested FIRST, and the order is the whole point: a draw with no
  // pixel shader has no handle AND no HLSL, so the other way round every one was
  // counted as no-hlsl and `no-handle 0` could never be anything but zero. That
  // cost a session, sending the search into a translator that was working fine.
  //
  // A guest DEPTH pass -- a translated VERTEX stage and no pixel shader at all
  // -- needs none of the four things demanded of a normal translated draw,
  // because kTranslatedDepthOnlyPS reads nothing and its output is discarded by
  // a zero write mask. What it DOES need is the vertex stage, without which it
  // runs on the software interpreter. `!colorWrite` is the load-bearing term and
  // is checked here as well as on the hooks side.
  const bool depthOnlyStandIn =
      !pixelShaderHlsl && hasVertexStage && !colorWrite;
  d.depthOnlyStandIn = depthOnlyStandIn;

  if (!d.pixelShaderHandle && !depthOnlyStandIn) {
    ++m_standInNoHandle;
    if (d.planeCount >= 3) ++m_standInNoHandlePlanes;
    if (d.colorClear) ++m_standInNoHandleClear;
    if (d.surfaceBind) ++m_standInNoHandleBind;
    m_standInNoHandleBy[(uint64_t(d.vertexShaderHandle) << 32) |
                        uint64_t(d.indexCount)]++;
  }
  else if (!d.pixelShaderHlsl && !depthOnlyStandIn) ++m_standInNoHlsl;
  else if (!hasVertexStage && !(interpolators && interpBytes))
    ++m_standInNoVertexInputs;
  else if ((!pixelConstants || !pixelConstDwords) && !depthOnlyStandIn)
    ++m_standInNoConstants;
  else if (pixelSamplerCount > kTranslatedSamplerSlots) ++m_standInTooManySamplers;

  if ((depthOnlyStandIn ||
       (d.pixelShaderHlsl && d.pixelShaderHandle && pixelConstants &&
        pixelConstDwords)) &&
      (hasVertexStage || (interpolators && interpBytes)) &&
      pixelSamplerCount <= kTranslatedSamplerSlots) {
    // The shader's cbuffer is xe_c[256], then xe_texinv[slots],
    // xe_texsign[slots], xe_param_gen and xe_alphatest, so the buffer must cover
    // all five -- sizing it to the constant bank alone leaves the shader reading
    // past the end of the resource for every unnormalized fetch. Rounded up to
    // 256 bytes, the constant-buffer granularity. Zero for a depth pass, which
    // brings no constant bank; the three payloads after it still have to exist
    // because the cbuffer is declared with them.
    const uint32_t bankBytes = depthOnlyStandIn ? 0u : pixelConstDwords * 4;
    const uint32_t texInvBytes = kTranslatedSamplerSlots * 16;
    const uint32_t texSignBytes = kTranslatedSamplerSlots * 16;
    const uint32_t paramGenBytes = 16;
    const uint32_t alphaTestBytes = 16;
    const uint32_t colorScaleBytes = 16;
    const uint32_t constBytes =
        ((bankBytes + texInvBytes + texSignBytes + paramGenBytes +
          alphaTestBytes + colorScaleBytes) +
         255u) &
        ~255u;
    // Built only for the CPU-vertex shape. A zero-byte buffer is not a valid
    // D3D12 resource, so this cannot simply fall out of interpBytes == 0.
    bool haveInterp = hasVertexStage;
    if (!hasVertexStage && createBuffer(d.ivb, interpBytes)) {
      std::memcpy(d.ivb.cpu, interpolators, interpBytes);
      d.ivbv.BufferLocation = d.ivb.gpu;
      d.ivbv.SizeInBytes = interpBytes;
      d.ivbv.StrideInBytes =
          kTranslatedInterpolators * 4 * uint32_t(sizeof(float));
      haveInterp = true;
    }
    if (haveInterp && createBuffer(d.pscb, constBytes)) {
      {
        uint8_t* p = d.pscb.cpu;
        std::memset(p, 0, constBytes);
        // Guarded: a depth pass has bankBytes 0 and pixelConstants null, and
        // memcpy from a null pointer is undefined even for a zero count.
        if (bankBytes) std::memcpy(p, pixelConstants, bankBytes);
        // xe_texinv, immediately after the bank. An unnormalized fetch addresses
        // the texture in TEXELS, so the shader multiplies by this to normalize:
        // 1/extent of the texture actually bound at that slot. Stored
        // reciprocated here rather than divided in the shader, where a zero
        // extent would produce infinity -- left zero, an unnormalized fetch
        // reads texel 0 rather than something plausible.
        //
        // EVERY slot, not just the first. Filled at index 0 only, an
        // unnormalized fetch on any higher slot multiplied its coordinate by
        // ZERO, sampling texel 0 and painting it flat across the primitive.
        // Slot 0 falls back to d.texture because the single-texture path
        // populates that and not the array.
        //
        // A slot bound to a RESOLVE SNAPSHOT has no CPU payload at all, so
        // filling only from d.pixelTextures left those slots at zero -- the same
        // defect surviving in the one case that never carries a payload, and
        // what the menu rider looks like: its material samples the scene
        // composite at s13, that slot is a snapshot, and the fetch reads texel
        // (0,0) flat over 21753 indices.
        //
        // .z is the LAYER COUNT, not a reciprocal, and the one component that
        // scales up: a 3D/stacked fetch with normalized coordinates delivers W
        // as a fraction of the stack while the Texture2DArray wants a slice
        // index. Left zero for a snapshot, pinning such a fetch to slice 0.
        //
        // SNAPSHOT FIRST, because that is the order the DESCRIPTOR uses --
        // BindTranslatedTextures tests stageSampledObjects[i] before anything
        // else -- and testing the payload first let a snapshot slot be
        // NORMALIZED by a different texture's extent.
        //
        // The shadowing texture is usually `d.texture`, NOT d.pixelTextures[s],
        // and that distinction cost a revert: inside ResolvePixelSlotTexture the
        // object and payload outputs are mutually exclusive, so a census keyed
        // on d.pixelTextures[s] reads a structural ZERO.
        for (uint32_t s = 0; s < kTranslatedSamplerSlots; ++s) {
          uint32_t w = 0, h = 0, layers = 0;
          // .w of xe_texinv is the guest's per-texture LOD BIAS, in LOD units.
          // See HleTexturePayload::lod_bias for why it matters.
          //
          // SET ONLY ON THE NON-SNAPSHOT PATH, and that is not a detail:
          // resolving the bias from d.pixelTextures[s] for EVERY slot includes
          // snapshot slots, where a shadowing payload routinely exists and
          // describes a DIFFERENT texture. That is texinv-shadowed-by-payload,
          // the defect fixed at the top of this same file, reintroduced one
          // field over. A snapshot has no fetch constant behind it, so 0.0 is
          // correct.
          float lodBias = 0.0f;
          const uint32_t object =
              s < d.pixelSampledObjects.size() ? d.pixelSampledObjects[s] : 0;
          if (object) {
            const auto snap = m_gameSnapshots.find(object);
            if (snap != m_gameSnapshots.end()) {
              w = snap->second.width;
              h = snap->second.height;
              // What the OLD ordering would have used instead. Counted only when
              // the extents actually DIFFER -- a shadowing texture that happens
              // to match normalized correctly by accident. Includes the
              // d.texture fallback, which is the whole point: without it this
              // reads zero while the defect is live.
              const auto& shadowed =
                  (s < d.pixelTextures.size() && d.pixelTextures[s])
                      ? d.pixelTextures[s]
                      : (s == 0 ? d.texture : nullptr);
              if (shadowed && shadowed->width && shadowed->height &&
                  (shadowed->width != w || shadowed->height != h))
                ++m_texinvSlotMismatch;
            }
          } else {
            const auto& tex = s < d.pixelTextures.size() && d.pixelTextures[s]
                                  ? d.pixelTextures[s]
                                  : (s == 0 ? d.texture : nullptr);
            if (tex && tex->width && tex->height) {
              w = tex->width;
              h = tex->height;
              layers = tex->array_size;
              lodBias = tex->lod_bias;
            }
          }
          // WHICH BRANCH EACH SLOT TOOK, per slot, for the life of the run.
          //
          // The floating-bike defect is here. A capture A/B proved the terrain
          // height-tile shader computes sample(tex3) * 4 - 2, so a slot that
          // samples BLACK subtracts exactly 2.008 from every height in the tile,
          // and the two captures differ in exactly one thing: xe_texinv[3].z, 1
          // in the good run and 0 in the bad one. Only the payload branch sets
          // .z, so .z == 0 means the slot took the SNAPSHOT branch -- same
          // 2048x2048 texture, resolved two mutually exclusive ways.
          //
          // So this counts, per slot: snapshot-with-a-resource, snapshot whose
          // object has NO entry in the map (the case that samples black), and
          // payload. A slot that flips between columns run to run is the defect;
          // a total cannot show a flip in slot 3 against 15 stable slots.
          {
            const uint32_t kind = !object ? 2u : (w && h ? 0u : 1u);
            const uint32_t si = s < kTranslatedSamplerSlots ? s : 0;
            ++m_texSlotPath[si][kind];
            // SPLIT OUT THE HEIGHT TILE. Counting per slot across ALL draws
            // produced "slot 3: snap 3926, NO-MAP 37, payload 194649", which
            // cannot say whether those 37 were the draws that matter. The
            // terrain height tile is a 129x129 target and nothing else in the
            // frame is: 37 NO-MAP out of 198k draws is noise, 37 out of ~40
            // height tiles is the entire defect.
            if (d.targetWidth == 129 && d.targetHeight == 129) {
              ++m_texSlotPathTile[si][kind];
              // Name the object on the slot that matters. Recorded for BOTH
              // object outcomes -- with a snapshot entry and without -- because
              // the dominant case turned out to be "has a resource and it is
              // black", not "no entry".
              if (si == 3 && object) {
                uint32_t k = 0;
                for (; k < m_tileSlot3Count; ++k)
                  if (m_tileSlot3[k].object == object) break;
                if (k == m_tileSlot3Count && m_tileSlot3Count < 8) {
                  m_tileSlot3[m_tileSlot3Count].object = object;
                  m_tileSlot3[m_tileSlot3Count].width = w;
                  m_tileSlot3[m_tileSlot3Count].height = h;
                  ++m_tileSlot3Count;
                }
                if (k < m_tileSlot3Count) {
                  if (w && h) {
                    ++m_tileSlot3[k].withEntry;
                    m_tileSlot3[k].width = w;
                    m_tileSlot3[k].height = h;
                  } else {
                    ++m_tileSlot3[k].withoutEntry;
                  }
                } else {
                  ++m_tileSlot3Overflow;
                }
              }
            }
          }
          if (!w || !h) continue;
          const float ts[4] = {1.0f / float(w), 1.0f / float(h),
                               float(layers), lodBias};
          std::memcpy(static_cast<uint8_t*>(p) + bankBytes + s * 16, ts,
                      sizeof(ts));
        }
        // xe_texsign, immediately after xe_texinv: the per-component scale for
        // TEXTURE SIGNS, 2.0 where the guest fetch is kUnsignedBiased and the
        // shader must expand [0,1] to [-1,1]. The shader pairs it with an offset
        // of 1-scale, so 1.0 is the identity.
        //
        // Written for EVERY slot and component, unconditionally: the buffer was
        // memset to zero above, and a zero scale here does not mean "unsigned",
        // it means the fetch becomes v*0 + 1 -- solid white.
        for (uint32_t s = 0; s < kTranslatedSamplerSlots; ++s) {
          const uint8_t signs = d.pixelSamplerSigns[s];
          const float sc[4] = {
              TextureSignScale(signs, 0), TextureSignScale(signs, 1),
              TextureSignScale(signs, 2), TextureSignScale(signs, 3)};
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
        // xe_alphatest follows xe_param_gen. RB_COLORCONTROL is decoded here
        // rather than in the shader so the shader carries no knowledge of the
        // register layout (hle_types.h, DrawCall::colour_control). The reference
        // is passed as raw bits through a uint4 rather than converted: it is a
        // float, and rounding it through an integer member would quietly move
        // every threshold.
        uint32_t refBits = 0;
        std::memcpy(&refBits, &d.alphaRef, 4);
        const uint32_t at[4] = {d.alphaControl & 7u,
                                (d.alphaControl >> 3) & 1u, refBits, 0};
        std::memcpy(static_cast<uint8_t*>(p) + bankBytes + texInvBytes +
                        texSignBytes + paramGenBytes,
                    at, sizeof(at));
        // xe_colorscale follows xe_alphatest. Guest colour formats 4 (k_16_16)
        // and 5 (k_16_16_16_16) are signed fixed point -32...32.
        //
        // ONLY these two. Format 7 (k_16_16_16_16_FLOAT) is a genuine half float
        // and shares R16G16B16A16_FLOAT with format 5, so scaling it too would
        // divide a correct HDR buffer by 32. The guest nibble is the only thing
        // that separates them, which is why it is carried per draw.
        //
        // This USED to write 1/32, which was half of Xenia's hack -- and the
        // half that does not apply to us. Xenia biases the write down by 5
        // exponents because ITS host render target is SNORM and cannot hold
        // -32...32, and pairs that with the exact inverse at resolve. We map
        // both formats to a HALF FLOAT target, which holds the whole range, so
        // the divide had no counterpart and every consumer of a fixed-point
        // target read values 32x too small: the deferred light accumulation
        // buffer is guest format 5, and the whole deferred chain ran 32x down.
        const bool fixed16 =
            d.targetColorFormat == 4u || d.targetColorFormat == 5u;
        // .y and .z carry the range the GUEST format can represent, and are 0
        // for formats that need no clamp -- the shader branches on .y > 0.
        //
        // Guest formats 3 and 12 are 7e3: "[0, 32) RGB, unorm alpha"
        // (xenos.h:301). Both map to R16G16B16A16_FLOAT here, which is SIGNED,
        // so without this a shader's negative output is stored where the console
        // ROP would have clamped it to 0. 31.875 is the largest representable
        // 7e3 value and the same bound Xenia clamps to.
        //
        // Every other format is already handled: 0/1 and 2/10 map to UNORM host
        // formats that clamp on write, 7 is a genuine signed half float that
        // must NOT be clamped, and 4/5 are the fixed-point pair whose half-float
        // target holds their range directly.
        const bool float7e3 =
            d.targetColorFormat == 3u || d.targetColorFormat == 12u;
        const float cs[4] = {1.0f,
                             float7e3 ? 31.875f : 0.0f,
                             float7e3 ? 1.0f : 0.0f, 0.0f};
        // NOT A GUARD, and removed from the census, where it read 24.7% and was
        // the top entry. docs/strict_mode.md classifies by what a thing DOES:
        // class A refuses to act on bad input, class B manufactures a value we
        // do not have. This is neither -- it applies the GUEST FORMAT'S OWN
        // RANGE, since a 7e3 target physically cannot store a negative or a
        // value at or above 32, exactly as formats 0/1/2/10 get for free from
        // their UNORM host formats. So 24.7% is the share of draws that render
        // to a 7e3 target, not a guard rate. m_float7e3Clamped still counts it,
        // on the format line.
        if (float7e3) ++m_float7e3Clamped;
        std::memcpy(static_cast<uint8_t*>(p) + bankBytes + texInvBytes +
                        texSignBytes + paramGenBytes + alphaTestBytes,
                    cs, sizeof(cs));
        if (fixed16) ++m_fixed16Scaled;
        d.translated = true;
      }
    }
    if (!d.translated) {
      // Reached only with the gate already passed, so this is the upload ring
      // refusing a range -- not a property of the draw. Charged because it is
      // otherwise indistinguishable from a draw that never qualified, and the
      // two want completely different fixes.
      ++m_standInBufferFailed;
      d.ivb = {};
      d.pscb = {};
    }
  }

  // The guest's own vertex shader, on the GPU. Only offered for a draw whose
  // pixel shader also translated -- the hooks side enforces that, and it is
  // re-checked here through `d.translated` because the two conditions are
  // decided in different processes' worth of code.
  //
  // Everything the CPU path derives on the side is REPLACED rather than lost:
  // the position buffer is what this stage produces, the interpolator copy is
  // what the rasterizer does natively, and the param_gen UV becomes SV_Position
  // in a pixel shader that reads it.
  if (d.translated && hasVertexStage) {
    // The emitted cbuffer is xe_c[256], xe_texinv[slots], then (fetch variant
    // only) uint4 xe_vf[kMaxVertexFetches], float4 xe_texsign[slots] and float4
    // xe_ndc_offset. The buffer has to cover ALL of it: the shader declares
    // every member unconditionally, so anything left out of the size is a read
    // past the end of the resource. Sized for the fetch variant unconditionally
    // -- the tail is zeroed either way.
    //
    // xe_texsign and xe_ndc_offset are LAST on purpose: the renderer writes
    // xe_vf at a fixed offset computed from the two members before it.
    const uint32_t vsConstBytes =
        ((vertexStage->constDwords * 4 + kTranslatedSamplerSlots * 16 +
          mx::hle::HlslShader::kMaxVertexFetches * 16 +
          kTranslatedSamplerSlots * 16 + 16) + 255u) & ~255u;
    if (vertexStage->rawBytes) {
      // The fetch path: one raw buffer, no vertex buffer view, and xe_vf[]
      // written into the cbuffer tail immediately after xe_texinv.
      if (createBuffer(d.rawvb, vertexStage->rawByteCount) &&
          createBuffer(d.vscb, vsConstBytes)) {
        // Both ranges are already mapped, so the pair of Map calls this used to
        // guard on is gone and with it the only way these writes could fail.
        std::memcpy(d.rawvb.cpu, vertexStage->rawBytes,
                    vertexStage->rawByteCount);
        uint8_t* p = d.vscb.cpu;
        std::memset(p, 0, vsConstBytes);
        std::memcpy(p, vertexStage->constants, vertexStage->constDwords * 4);
        // xe_vf sits directly after xe_c[256] and xe_texinv[16], which is
        // where the emitter declares it. This offset and that declaration
        // are one fact in two places -- if either moves the other must.
        const uint32_t vfOffset =
            vertexStage->constDwords * 4 + kTranslatedSamplerSlots * 16;
        std::memcpy(p + vfOffset, vertexStage->rawFetch,
                    vertexStage->rawFetchCount * 16);
        FillVertexTexinv(d, p, vsConstBytes, vertexStage->constDwords);
        FillVertexTextureSigns(d, p, vsConstBytes, vertexStage->constDwords);
        FillVertexNdcOffset(d, p, vsConstBytes, vertexStage->constDwords);
        d.vertexShaderHandle = vertexStage->handle;
        d.vertexShaderHlsl = vertexStage->hlsl;
        d.vertexShaderDxbc = vertexStage->dxbc;
        d.vertexInputCount = 0;
        d.gpuVertex = true;
        d.gpuVertexFetch = true;
      }
      if (!d.gpuVertexFetch) {
        d.rawvb = {};
        d.vscb = {};
      }
    } else if (createBuffer(d.vsvb, vertexStage->inputBytes) &&
               createBuffer(d.vscb, vsConstBytes)) {
      std::memcpy(d.vsvb.cpu, vertexStage->inputs, vertexStage->inputBytes);
      d.vsvbv.BufferLocation = d.vsvb.gpu;
      d.vsvbv.SizeInBytes = vertexStage->inputBytes;
      d.vsvbv.StrideInBytes = vertexStage->regCount * 16;
      // Zeroed first: the shader's cbuffer is xe_c[256] plus xe_texinv, and the
      // vertex bank is 256 constants, so the tail past the bank must read zero
      // rather than whatever the ring page held from an earlier frame.
      std::memset(d.vscb.cpu, 0, vsConstBytes);
      std::memcpy(d.vscb.cpu, vertexStage->constants,
                  vertexStage->constDwords * 4);
      FillVertexTexinv(d, d.vscb.cpu, vsConstBytes, vertexStage->constDwords);
      FillVertexTextureSigns(d, d.vscb.cpu, vsConstBytes,
                             vertexStage->constDwords);
      d.vertexShaderHandle = vertexStage->handle;
      d.vertexShaderHlsl = vertexStage->hlsl;
      d.vertexShaderDxbc = vertexStage->dxbc;
      d.vertexInputCount = vertexStage->regCount;
      for (uint32_t i = 0; i < vertexStage->regCount; ++i)
        d.vertexInputRegs[i] = vertexStage->regs[i];
      d.gpuVertex = true;
    }
    if (!d.gpuVertex) {
      d.vsvb = {};
      d.rawvb = {};
      d.vscb = {};
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
  d.cullMode = cullMode;
  // Bit 0 has PA_SU_VTX_CNTL::PIX_CENTER's meaning -- 0 is Direct3D 9 centres at
  // .0 and needs the offset, 1 is the host's own .5 and does not -- but the
  // value is assumed rather than read: the register is not locatable and this is
  // a D3D9 title, so 0 is what it would hold. Keeping the decode identical means
  // the register drops straight in when it is found.
  d.edramCopy = edramCopy;
  // Interned HERE rather than at draw time: StencilIndexFor mutates the intern
  // table, and the draw loop runs on the render thread while this runs on the
  // submitting one. Doing it at submission also fixes the index by the time the
  // draw is queued, so a state that arrives later cannot renumber a queued draw.
  if (stencil && stencil->enable) {
    d.stencil = *stencil;
    d.stencilIndex = StencilIndexFor(*stencil);
  // Interned here rather than in the caller for the same reason stencil is:
  // the table lives on the renderer, and a caller holding indices into it
  // would be holding a reference to renderer state it cannot see change.
  // The interning has to be here, because this is where `d` exists. The
  // COUNTERS are at the top of this function instead: half the draws that
  // reach the caller return early above, and counting here made the renderer
  // report 0 clip-disabled draws while the caller reported 1051 -- two numbers
  // over two different populations, which is not a contradiction, just a
  // useless pair.
  if (om) d.omIndex = OmIndexFor(*om);
    ++m_stencilDraws;
    // kAlways is 7 in the GUEST encoding, which is what GameStencil carries.
    if (stencil->frontFunc != 7u || stencil->backFunc != 7u)
      ++m_stencilTestingDraws;
  }
  if (scissor) {
    d.scissorSeen = true;
    d.scissorLeft = scissor[0];
    d.scissorTop = scissor[1];
    d.scissorRight = scissor[2];
    d.scissorBottom = scissor[3];
  }
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
    memcpy(d.cb.cpu, mvp, 16 * sizeof(float));
  }

  m_gameDraws.push_back(std::move(d));
}

void D3D12Renderer::SetGameDrawSecondTarget(uint32_t object, uint32_t width,
                                           uint32_t height,
                                           uint32_t edramBase,
                                           uint32_t colorFormat) {
  // Patches the draw AddGameDraw just pushed. Silent when there is none: the
  // draw may have been refused for budget, and a second target with no draw to
  // attach it to is simply nothing to do.
  if (m_gameDraws.empty() || !object || !width || !height) return;
  GameDraw& d = m_gameDraws.back();
  // Only a real draw takes one. A resolve or clear entry shares the vector and
  // must not have a colour target grafted onto it.
  if (d.resolveDest || d.colorClear || d.depthClear) return;
  d.target1Object = object;
  d.target1Width = width;
  d.target1Height = height;
  d.target1Base = edramBase;
  d.target1ColorFormat = colorFormat;
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

void D3D12Renderer::AddGameDepthClear(uint32_t depthObject, uint32_t width,
                                      uint32_t height, uint32_t edramBase,
                                      float depth, bool clearDepthPlane,
                                      bool clearStencilPlane,
                                      uint8_t clearStencil) {
  if (!depthObject || !width || !height) return;
  if (m_gameDraws.size() >= kMaxGameDraws) return;
  // Neither plane asked for is not a clear. Guarded rather than assumed: the
  // caller gates on (depth || stencil) and a future caller might not.
  if (!clearDepthPlane && !clearStencilPlane) return;
  GameDraw d;
  d.depthClear = true;
  d.clearDepth = depth;
  d.clearDepthPlane = clearDepthPlane;
  d.clearStencilPlane = clearStencilPlane;
  d.clearStencil = clearStencil;
  d.depthObject = depthObject;
  d.depthWidth = width;
  d.depthHeight = height;
  d.depthBase = edramBase;
  m_gameDraws.push_back(std::move(d));
}

void D3D12Renderer::AddGameSurface(uint32_t object, uint32_t width,
                                   uint32_t height, uint32_t edramBase,
                                   uint32_t colorFormat, bool isDepth) {
  if (!object || !width || !height) return;
  if (m_gameDraws.size() >= kMaxGameDraws) return;
  GameDraw d;
  d.surfaceBind = true;
  d.surfaceBindIsDepth = isDepth;
  d.targetObject = object;
  d.targetWidth = width;
  d.targetHeight = height;
  d.targetBase = edramBase;
  d.targetColorFormat = colorFormat;
  m_gameDraws.push_back(std::move(d));
}
