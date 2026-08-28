// D3D12Renderer -- translated-shader texture and sampler binding.
//
// Split verbatim out of d3d12_game.cpp. Nothing here was renamed or
// reordered; see d3d12_game_internal.h for the one linkage change.
#include "gfx/d3d12_renderer.h"

#include "gfx/d3d12_game_internal.h"
#include "gfx/d3d12_internal.h"
#include "gfx/d3d12_shaders.h"
#include "gpu/d3d9_layout.h"
#include "gpu/hle_types.h"
#include "gpu/shader_hlsl.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

using mx::gfx::CompileShader;
using mx::gfx::LogError;
using mx::gfx::LogInfo;

// xe_texsign for the VERTEX stage, which lives at the very end of that stage's
// cbuffer -- after xe_vf, so that the fixed vfOffset the fetch path writes to
// does not move. See the declaration in EmitShaderHlsl.
//
// The default is 1.0, NOT the 0 the surrounding memset leaves. The shader
// computes `v * xe_texsign + (1 - xe_texsign)`, so a zero scale turns every
// sample into a constant 1.0 -- white -- rather than into an unmodified sample.
// A plain unsigned fetch needs a scale of exactly one.
void D3D12Renderer::FillVertexTextureSigns(const GameDraw& d, uint8_t* cb,
                                           uint32_t cbBytes,
                                           uint32_t constDwords) {
  const uint32_t at = constDwords * 4 + kTranslatedSamplerSlots * 16 +
                      mx::hle::HlslShader::kMaxVertexFetches * 16;
  if (!cb || at + kTranslatedSamplerSlots * 16 > cbBytes) return;
  auto* sign = reinterpret_cast<float*>(cb + at);
  for (uint32_t s = 0; s < kTranslatedSamplerSlots; ++s) {
    // Two bits per component, host order, exactly as the pixel path decodes it:
    // 0 is plain unsigned (scale 1) and kUnsignedBiased is 2*c-1 (scale 2).
    const uint8_t signs =
        s < d.vertexSamplerCount ? d.vertexSamplerSigns[s] : 0u;
    for (uint32_t c = 0; c < 4; ++c)
      sign[s * 4 + c] = ((signs >> (c * 2)) & 3u) == 2u ? 2.0f : 1.0f;
  }
}

bool D3D12Renderer::BindTranslatedTextures(const GameDraw& d,
                                           D3D12_GPU_DESCRIPTOR_HANDLE& out,
                                           bool vertex) {
  // Which stage's slots this call is filling. The descriptor block, the
  // snapshot lookup and every failure counter are identical either way -- only
  // the source arrays differ, and the root parameter the caller binds the
  // result to.
  const uint32_t stageSamplerCount =
      vertex ? d.vertexSamplerCount : d.pixelSamplerCount;
  const uint32_t stageSamplerArrayMask =
      vertex ? d.vertexSamplerArrayMask : d.pixelSamplerArrayMask;
  const auto& stageSampledObjects =
      vertex ? d.vertexSampledObjects : d.pixelSampledObjects;
  const auto& stageTextures = vertex ? d.vertexTextures : d.pixelTextures;
  const auto& stageSampledSwizzles =
      vertex ? d.vertexSampledSwizzles : d.pixelSampledSwizzles;
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
  for (uint32_t i = 0; i < stageSamplerCount; ++i) {
    if (const uint32_t object = stageSampledObjects[i]) {
      // A resolve result: sample the snapshot the guest resolved into, which is
      // the same resource the stand-in path samples for this object.
      auto it = m_gameSnapshots.find(object);
      // Stamped here, on the bind, because this is the one place that means the
      // snapshot was actually handed to a draw. EvictGameSnapshots reclaims on
      // this stamp, so anything not stamped here is by definition unreferenced.
      if (it != m_gameSnapshots.end() && it->second.resource) {
        it->second.lastUsedFrame = m_gameFrame;
      }
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
        const std::string link = fmt::format(
            "{}{:08X}/{}/{:08X}/{}", vertex ? "V" : "P",
            vertex ? d.vertexShaderHandle : d.pixelShaderHandle, i, object,
            kindName);
        if (s_missingLinks.size() < 64 && s_missingLinks.insert(link).second) {
          REXLOG_INFO(
              "d3d12: translated snapshot MISSING: {} 0x{:08X}, target "
              "0x{:08X} {}x{}, compact slot {} of {}, destination texture "
              "0x{:08X}, class {}",
              vertex ? "VS" : "PS",
              vertex ? d.vertexShaderHandle : d.pixelShaderHandle,
              d.targetObject, d.targetWidth,
              d.targetHeight, i, stageSamplerCount, object, kindName);
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
      slots[i].format = SrvFormatForResource(it->second.resource->GetDesc().Format);
      // A SNAPSHOT keeps the DEFAULT identity mapping. The guest's swizzle is
      // recorded (stageSampledSwizzles, printed by SLOT MAP) but deliberately
      // NOT applied here.
      //
      // TRIED AND REVERTED 2026-08-14, with a screenshot: applying it turned the
      // menu's yellow rider gear and bike CYAN -- a clean red<->blue swap, which
      // is precisely what the 03012 slots ask for. The swizzle was not being
      // dropped by accident; identity is already right for these slots.
      //
      // Why: a snapshot is a host render-target COPY, already in host channel
      // order, because the guest colour format was resolved when the host target
      // was created. The guest swizzle is expressed against the guest format's
      // channel order, so applying it on top double-applies the correction. This
      // is exactly the composition Xenia does with GetHostFormatSwizzle -- guest
      // swizzle composed THROUGH the host format's own swizzle, not applied raw
      // -- and we have no host-format swizzle table. Until we do, identity is
      // the correct behaviour for a snapshot and the recorded swizzle is a
      // diagnostic only.
      //
      // The decoded-texture branch below is different and unaffected: it uploads
      // guest bytes in guest channel order, so the guest swizzle applies raw.
      continue;
    }
    // The other two ways a slot refuses. These used to increment a counter and
    // return with no line at all, which is a worse silence than the snapshot
    // case above: the bind is ALL-OR-NOTHING, so one unsatisfiable slot drops
    // the stage's whole table, and an aggregate "bind failed N" cannot say
    // which shader, which slot, or which of the three reasons. Same bounded
    // distinct-tuple shape as the snapshot line, so a steady state costs
    // nothing.
    const auto refused = [&](const char* why) {
      static std::unordered_set<std::string> s_refusedLinks;
      const std::string link = fmt::format(
          "{}{:08X}/{}/{}", vertex ? "V" : "P",
          vertex ? d.vertexShaderHandle : d.pixelShaderHandle, i, why);
      if (s_refusedLinks.size() < 64 && s_refusedLinks.insert(link).second) {
        REXLOG_INFO(
            "d3d12: translated slot REFUSED ({}): {} 0x{:08X}, compact slot {} "
            "of {}, target 0x{:08X} {}x{} — whole {} table dropped",
            why, vertex ? "VS" : "PS",
            vertex ? d.vertexShaderHandle : d.pixelShaderHandle, i,
            stageSamplerCount, d.targetObject, d.targetWidth, d.targetHeight,
            vertex ? "vertex" : "pixel");
      }
    };
    const auto& tex = stageTextures[i];
    if (!tex) {
      ++m_translatedNoTexture;
      refused("no texture described");
      return false;
    }
    uint32_t unusedDescriptor = 0;
    // Uploads the texture and gives it its cached descriptor. That descriptor
    // is not the one bound here — it lives in a different heap — but the upload
    // and the resource it creates are exactly what this needs.
    if (!EnsureGameTexture(tex, unusedDescriptor)) {
      ++m_translatedUploadFailed;
      refused("upload failed");
      return false;
    }
    auto it = m_gameTextures.find(tex->key);
    if (it == m_gameTextures.end() || !it->second.resource) {
      ++m_translatedUploadFailed;
      refused("uploaded but absent from cache");
      return false;
    }
    slots[i].resource = it->second.resource.Get();
    slots[i].format = SrvFormatForResource(it->second.resource->GetDesc().Format);
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
  // Once per shader handle, bounded, so it costs nothing after the first
  // sighting of each. What it answers: whether the stage's sampler count agrees
  // with what the shader declares, and for every slot, whether it came from a
  // resolve snapshot or a CPU texture, and at what format and extent.
  //
  // Keyed on the handle of the stage BEING FILLED. It used to key on
  // `d.pixelShaderHandle` for both stages, which made it silent for exactly the
  // draws that needed it: the terrain depth prepass runs the depth-only
  // pixel stand-in and carries no pixel handle, so its VERTEX slot -- the
  // heightmap the whole terrain's world Y comes from -- was censused zero
  // times. That heightmap reads a constant 1.0 (measured in gameplay-8.rdc:
  // every one of 4225 vertices solves back to world Y = 1.000 against a camera
  // at Y = 616), and naming the resource actually bound is the one thing this
  // side can answer that a capture cannot. Two separate caps so a flood of
  // pixel shaders cannot starve the vertex ones.
  const uint32_t censusHandle =
      vertex ? d.vertexShaderHandle : d.pixelShaderHandle;
  if (censusHandle) {
    static std::unordered_set<uint32_t> s_censusedPs, s_censusedVs;
    auto& censused = vertex ? s_censusedVs : s_censusedPs;
    // 64 was a menu-sized cap and it SATURATED before a level's shaders
    // ever bound: mx_1420 printed 68 census lines, all of them menu, and
    // the terrain material -- the one under investigation -- was censused
    // zero times. Same shape as measure-with-a-level-loaded. A run
    // translates ~300 shaders, so 512 covers a level with headroom and
    // still cannot run away.
    if (censused.size() < 512 && censused.insert(censusHandle).second) {
      std::string slotDesc;
      for (uint32_t i = 0; i < stageSamplerCount && i < kTranslatedSamplerSlots;
           ++i) {
        if (!slots[i].resource) {
          slotDesc += fmt::format(" [{}]=NONE", i);
          continue;
        }
        const D3D12_RESOURCE_DESC rd = slots[i].resource->GetDesc();
        // A snapshot slot used to print a HOST pointer and nothing else,
        // which names the resource only within one process. The guest object
        // is what the resolve log, SLOT MAP and get_resource_usage sweeps are
        // all keyed on, so printing it here is what lets a census line be
        // joined to the rest without brute-forcing a capture's resource list.
        slotDesc += fmt::format(
            " [{}]={} {}x{} fmt{} res={}{}{}", i,
            slots[i].useSwizzle ? "tex" : "snap", uint32_t(rd.Width), rd.Height,
            uint32_t(slots[i].format),
            static_cast<const void*>(slots[i].resource),
            (!slots[i].useSwizzle && i < stageSampledObjects.size())
                ? fmt::format(" object 0x{:08X}", stageSampledObjects[i])
                : std::string(),
            slots[i].useSwizzle ? "" : " (no swizzle)");
        // PROBE: the decoded bytes this slot will actually sample, read from the
        // payload rather than matched up in a capture afterwards.
        //
        // Every previous attempt to name this resource went through RenderDoc's
        // resource list, and twice picked the wrong one of two same-size
        // same-format candidates -- once an all-zero texture whose alpha of 0
        // renders WHITE in the exporter, which is exactly the trap
        // rdc-png-alpha-reads-as-white records. The guest address, the swizzle
        // and the sampled value are all known from the two sides of this
        // question; only the bytes in between were ever inferred. This closes
        // that, and it is the last inference in the chain.
        //
        // Four texels spread across the base level, printed as raw bytes in
        // decoded order (BEFORE the SRV swizzle, which the census prints
        // separately). The terrain heightmap samples near the middle, so the
        // centre texel is the one to read against a measured vertex height.
        if (const auto& payload = stageTextures[i]) {
          const uint32_t bpp =
              payload->width ? payload->row_pitch / payload->width : 0;
          if (bpp && bpp <= 16 && !payload->data.empty()) {
            const uint32_t pts[4][2] = {{payload->width / 2, payload->height / 2},
                                        {payload->width / 4, payload->height / 4},
                                        {0, 0},
                                        {payload->width - 1, payload->height - 1}};
            std::string probe;
            for (const auto& pt : pts) {
              const size_t off = size_t(pt[1]) * payload->row_pitch +
                                 size_t(pt[0]) * bpp;
              if (off + bpp > payload->data.size()) continue;
              probe += fmt::format(" ({},{})=", pt[0], pt[1]);
              for (uint32_t b = 0; b < bpp; ++b)
                probe += fmt::format("{:02X}", payload->data[off + b]);
              // What the SHADER's .x actually receives, resolved here rather
              // than left to be matched against a capture afterwards. The probe
              // bytes and the sampled value were previously measured in
              // DIFFERENT runs, which is not a comparison at all -- addresses
              // and handles differ per run. Applying the slot's own swizzle to
              // the slot's own bytes states the answer in one line.
              const uint32_t sx = (slots[i].swizzle >> 0) & 7u;
              if (sx >= 6u) probe += "->x:KEEP";
              else if (sx == 5u) probe += "->x:ONE";
              else if (sx == 4u) probe += "->x:ZERO";
              else if (sx < bpp) probe += fmt::format("->x:{:02X}",
                                                      payload->data[off + sx]);
              else probe += "->x:PAST-END";
            }
            slotDesc += fmt::format(" probe[{}]{}", i, probe);
            // The endian swap's input and output, from the decode itself. If
            // these are byte-reverses of each other then the swap is what put
            // the dead channel where the swizzle reads, and the fix is in
            // SwapBlock; if they are equal, the decode is faithful and the
            // texture genuinely has a dead channel.
            if (payload->probe_bytes) {
              std::string raw, swapped;
              for (uint32_t b = 0; b < payload->probe_bytes; ++b) {
                raw += fmt::format("{:02X}", payload->probe_raw[b]);
                swapped += fmt::format("{:02X}", payload->probe_swapped[b]);
              }
              slotDesc += fmt::format(" centre raw={} swapped={} endian={}", raw,
                                      swapped, payload->probe_endian);
            }
          }
        }
      }
      REXLOG_INFO("d3d12: {} 0x{:08X} slot census: count {}, array mask 0x{:X};{}",
                  vertex ? "VS" : "PS", censusHandle, stageSamplerCount,
                  stageSamplerArrayMask, slotDesc);
    }
  }

  auto cpu = m_translatedSrvHeap->GetCPUDescriptorHandleForHeapStart();
  cpu.ptr += SIZE_T(block) * kTranslatedSamplerSlots * m_gameSrvDescriptorSize;
  for (uint32_t i = 0; i < kTranslatedSamplerSlots; ++i) {
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    // Slots past what the shader declares still need a valid descriptor: the
    // table's range covers all of them whether or not they are sampled. They
    // repeat slot 0 rather than being left undefined.
    const uint32_t from = i < stageSamplerCount ? i : 0;
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
    if ((stageSamplerArrayMask >> from) & 1u) {
      const UINT16 arraySize =
          s.resource ? s.resource->GetDesc().DepthOrArraySize : 1;
      if (arraySize < 2) ++m_translatedArraySlotNot2DArray;
      srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
      srv.Texture2DArray.MipLevels = UINT(-1);
      srv.Texture2DArray.ArraySize = std::max<UINT>(arraySize, 1);
    } else {
      srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
      srv.Texture2D.MipLevels = UINT(-1);
    }
    srv.Format = s.format;
    // Guest GPUSWIZZLE values are 0-3 = XYZW, 4 = constant 0, 5 = constant 1,
    // 7 = KEEP (fetch instructions only). D3D12_SHADER_COMPONENT_MAPPING defines
    // only 0-5, so 6 and 7 are undefined here and were being handed to the
    // driver raw. Xenia sanitises them the same way, and says why:
    //
    //   // Get rid of 6 and 7 values (to prevent host GPU errors if the game has
    //   // something broken) the simple way - by changing them to 4 (0) and 5 (1).
    //   host_swizzle_component = guest_swizzle_component & 0b101;
    //       -- texture_cache.cc, TextureCache::GuestToHostSwizzle
    //
    // `& 5` maps 6 -> 4 and 7 -> 5 and leaves 0-5 alone. Note what that means
    // for a KEEP component: it becomes a forced 1.0, on Xenia as much as here.
    const auto host_component = [](uint32_t c) -> UINT {
      return c >= 4u ? UINT(c & 5u) : UINT(c);
    };
    srv.Shader4ComponentMapping =
        s.useSwizzle
            ? D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
                  host_component((s.swizzle >> 0) & 7u),
                  host_component((s.swizzle >> 3) & 7u),
                  host_component((s.swizzle >> 6) & 7u),
                  host_component((s.swizzle >> 9) & 7u))
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
                                           D3D12_GPU_DESCRIPTOR_HANDLE& out,
                                           bool vertex) {
  const uint32_t stageSamplerCount =
      vertex ? d.vertexSamplerCount : d.pixelSamplerCount;
  const auto& stageSampledObjects =
      vertex ? d.vertexSampledObjects : d.pixelSampledObjects;
  const auto& stageTextures = vertex ? d.vertexTextures : d.pixelTextures;
  const auto& stageSamplerSigns =
      vertex ? d.vertexSamplerSigns : d.pixelSamplerSigns;
  const auto& stageSampledSwizzles =
      vertex ? d.vertexSampledSwizzles : d.pixelSampledSwizzles;
  if (!m_samplerHeap) return false;

  // The configuration first, as a key. Slots past what the shader declares
  // repeat slot 0, matching how BindTranslatedTextures fills the SRV table --
  // the range covers all sixteen whether or not the shader samples them, so
  // every one needs a defined descriptor.
  // The variants ARE the key. They used to be packed a few bits each into a
  // uint64_t, which ran out at five bits across sixteen slots.
  std::array<uint8_t, kSamplerBlockSlots> key{};
  for (uint32_t i = 0; i < kSamplerBlockSlots; ++i) {
    const uint32_t slot = i < stageSamplerCount ? i : 0;
    uint32_t variant = 0;
    // A resolve snapshot used to get a HARDCODED clamped POINT sampler, on the
    // reasoning that it "is a host render target sampled 1:1" and that there is
    // "no fetch constant to read a mode off". The second half was simply wrong
    // -- ResolvePixelSlotTexture reads that fetch constant for the swizzle --
    // and the first half is true only for a full-screen post-process copy.
    //
    // The terrain ATLAS comes through here too, and it is sampled with computed
    // UVs that both wrap and MINIFY, so both halves of the hardcode hurt it:
    //
    //   clamp   tile index 10 gives U = 1.283, pinned to the right edge, which
    //           read an empty tile -- the black ground.
    //   point   nearest-neighbour on a minified 2048x2048 atlas -- the hard
    //           corduroy aliasing across every dune in ground-tiles-2.rdc.
    //
    // Both now come from the guest's own fetch constant, packed into the top
    // bits of the swizzle word: 12 clamp U, 13 clamp V, 14 point filter. For a
    // 1:1 copy point and linear are the same sample, so nothing that motivated
    // the hardcode changes.
    if (slot < stageSampledObjects.size() && stageSampledObjects[slot]) {
      const uint16_t packed = slot < stageSampledSwizzles.size()
                                  ? stageSampledSwizzles[slot]
                                  : uint16_t(0);
      if (packed & (1u << 12)) variant |= kSamplerClampU;
      if (packed & (1u << 13)) variant |= kSamplerClampV;
      // Bit 15 says the word came from a fetch constant. Without it this slot
      // was bound by one of the partial-snapshot paths, which never write the
      // word -- so there is no filter to honour and it keeps the POINT it has
      // always had. A zero word must not read as "the guest asked for linear".
      if (!(packed & (1u << 15)) || (packed & (1u << 14)))
        variant |= kSamplerPoint;
    } else if (slot < stageTextures.size() && stageTextures[slot]) {
      variant = SamplerVariantFor(*stageTextures[slot]);
    }
    key[i] = uint8_t(variant);
  }

  if (auto it = m_samplerBlocks.find(key); it != m_samplerBlocks.end()) {
    out = m_samplerHeap->GetGPUDescriptorHandleForHeapStart();
    out.ptr += UINT64(SamplerBlockBase(it->second)) * m_samplerDescriptorSize;
    return true;
  }
  if (m_samplerBlockNext >= kSamplerBlockCount) {
    // Out of distinct configurations. Fall back to the reserved region rather
    // than failing the draw: its slot 0 is the plain linear-wrap variant, which
    // is what every slot got before this function existed. The region is at
    // least a block wide, so reading a whole table out of it stays in bounds.
    ++m_samplerBlockExhausted;
    out = m_samplerHeap->GetGPUDescriptorHandleForHeapStart();
    return true;
  }

  const uint32_t block = m_samplerBlockNext++;
  auto cpu = m_samplerHeap->GetCPUDescriptorHandleForHeapStart();
  cpu.ptr += SIZE_T(SamplerBlockBase(block)) * m_samplerDescriptorSize;
  for (uint32_t i = 0; i < kSamplerBlockSlots; ++i) {
    const D3D12_SAMPLER_DESC sd = SamplerVariantDesc(key[i]);
    m_device->CreateSampler(&sd, cpu);
    cpu.ptr += SIZE_T(m_samplerDescriptorSize);
  }
  m_samplerBlocks.emplace(key, block);
  out = m_samplerHeap->GetGPUDescriptorHandleForHeapStart();
  out.ptr += UINT64(SamplerBlockBase(block)) * m_samplerDescriptorSize;
  return true;
}
