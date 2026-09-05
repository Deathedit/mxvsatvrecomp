// D3D12Renderer -- guest resources: textures, Bink planes, render and depth
// targets, snapshots, the pooled surface allocator and luminance readback.
//
// Split verbatim out of d3d12_game.cpp.
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

// Uploads Bink's plane set into reusable host textures and writes their SRVs
// into the reserved descriptors at the head of the heap. A plane's resource is
// recreated only when its dimensions change, so steady-state playback creates
// nothing per frame; only the staging copy happens each time.
bool D3D12Renderer::EnsureYuvPlanes(const GameDraw& draw,
                                    uint32_t& descriptorBase) {
  if (!m_gameSrvHeap) {
    ++m_yuvRefusedNoHeap;
    return false;
  }
  if (draw.planeCount < 3) {
    ++m_yuvRefusedTooFewPlanes;
    return false;
  }
  const uint32_t kPlanes = kMaxDrawPlanes;
  // One block per composite draw per frame in flight. Beyond the budget the draw
  // is refused rather than aliasing an earlier draw's descriptors. Counted
  // because the refusal is otherwise invisible and drops a video draw in exactly
  // the way a missing-video defect looks: two concurrent streams is 2 per frame
  // against kMaxYuvDrawsPerFrame, so this should read zero.
  if (m_yuvDrawsThisFrame >= kMaxYuvDrawsPerFrame) {
    ++m_yuvRefusedBudget;
    return false;
  }
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
      // dropping them here: the GPU may still be reading last frame's plane, and
      // a command list does not keep the resources it references alive. Through
      // RetireResource rather than an inline RetiredFrame -- the inline form was
      // copied to three sites and every copy carried the same off-by-one fence.
      RetireResource(std::move(plane.resource));
      for (auto& up : plane.upload) {
        RetireResource(std::move(up));
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
  ++m_yuvPrepared;
  return true;
}

// Fills an existing game-texture resource from a decoded payload, and records
// which content_version it now holds. Split out of EnsureGameTexture so the
// first fill and every later refill are the same code; refills exist because
// Scaleform repacks its glyph atlas under a stable cache key.
//
// The upload buffer is per frame in flight: a refill recorded this frame must
// not write over the staging bytes an earlier frame's CopyTextureRegion may
// still be reading. The destination resource needs no such care because the
// copies are ordered against each other on the same queue.
bool D3D12Renderer::UploadGameTexture(GameTexture& entry,
                                      const mx::hle::HleTexturePayload& src) {
  if (!entry.resource || src.data.empty() || !src.row_pitch) return false;
  const D3D12_RESOURCE_DESC td = entry.resource->GetDesc();

  // One subresource per (level, slice). A cube arrives as six tightly packed 2D
  // images in `src.data`; the host wants each at its own aligned footprint
  // offset. The two sides nest OPPOSITELY: the guest stores array slices inside
  // a level and the payload keeps that order, while D3D12 numbers subresources
  // mip + slice * MipLevels, slices outermost.
  const uint32_t slices = std::max<uint32_t>(td.DepthOrArraySize, 1);
  const uint32_t levels = std::max<uint32_t>(td.MipLevels, 1);
  const uint32_t subresources = slices * levels;
  if (levels > std::size(src.levels)) return false;
  // Heap-allocated, deliberately. These were fixed arrays of 6 * 14 -- six
  // slices, because six is a cube and a cube was the only array texture that
  // existed. A true 3D VOLUME has up to 1024 slices, and a colour-grading LUT of
  // 16 or 32 sailed past the guard and returned false: 428 `upload-failed` in
  // the first run with volumes enabled. Sized from the resource rather than from
  // a constant, and reused across calls to keep the allocation off the path.
  static thread_local std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints;
  static thread_local std::vector<UINT> rowCounts;
  static thread_local std::vector<UINT64> rowByteCounts;
  footprints.assign(subresources, {});
  rowCounts.assign(subresources, 0);
  rowByteCounts.assign(subresources, 0);
  UINT64 uploadBytes = 0;
  m_device->GetCopyableFootprints(&td, 0, subresources, 0, footprints.data(),
                                  rowCounts.data(), rowByteCounts.data(),
                                  &uploadBytes);

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
      RetireResource(std::move(upload));
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
  // The payload states its own geometry per level. It used to be reconstructed
  // arithmetically -- data.size() / row_pitch for the row count, divided by the
  // slice count -- which is only ever right while the buffer holds one level,
  // and would have quietly mangled every array texture once a chain was
  // appended to it.
  for (uint32_t l = 0; l < levels; ++l) {
    const mx::hle::HleTextureLevelData& lv = src.levels[l];
    if (!lv.row_pitch || !lv.rows) continue;
    const size_t sliceBytes = size_t(lv.row_pitch) * lv.rows;
    for (uint32_t s = 0; s < slices; ++s) {
      const uint32_t sub = l + s * levels;
      const uint32_t copyRows = std::min<uint32_t>(rowCounts[sub], lv.rows);
      const size_t copyBytes =
          std::min<size_t>(lv.row_pitch, size_t(rowByteCounts[sub]));
      const size_t srcOffset = size_t(lv.offset) + size_t(s) * sliceBytes;
      if (srcOffset + sliceBytes > src.data.size()) continue;
      const uint8_t* srcSlice = src.data.data() + srcOffset;
      for (uint32_t y = 0; y < copyRows; ++y) {
        std::memcpy(mapped + footprints[sub].Offset +
                        size_t(y) * footprints[sub].Footprint.RowPitch,
                    srcSlice + size_t(y) * lv.row_pitch, copyBytes);
      }
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

  for (uint32_t sub = 0; sub < subresources; ++sub) {
    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = entry.resource.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = sub;
    D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
    srcLoc.pResource = upload.Get();
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcLoc.PlacedFootprint = footprints[sub];
    m_commandList->CopyTextureRegion(&dst, 0, 0, 0, &srcLoc, nullptr);
  }

  std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
  m_commandList->ResourceBarrier(1, &barrier);

  entry.uploadedVersion = src.upload_version;
  return true;
}

bool D3D12Renderer::EnsureGameTexture(
    const std::shared_ptr<const mx::hle::HleTexturePayload>& texture,
    uint32_t& descriptorIndex) {
  if (!texture || texture->data.empty() || !m_gameSrvHeap) return false;
  if (auto it = m_gameTextures.find(texture->key); it != m_gameTextures.end()) {
    descriptorIndex = it->second.descriptorIndex;
    // The LRU stamp. Also what makes this entry un-evictable for the rest of
    // the frame being recorded, which is the point: it is about to be sampled.
    it->second.lastUsedFence = m_fenceValue;
    // The guest rewrote this texture under a stable key -- a Scaleform glyph
    // atlas repack, or the terrain's virtual-texture index map being repaged.
    // Refill the existing resource rather than making a new one: the key does
    // not change, so a new resource would leak one per rewrite, and the
    // descriptor already published in the heap points here.
    //
    // Compared on upload_version, a hash of the DECODED BYTES, NOT on
    // content_version -- that one is a 2 KB sample of guest memory, blind to a
    // sparse write. Gating the refill on it meant every re-decode the flat-retry
    // backoff forced was computed, cached and then thrown away here.
    if (it->second.uploadedVersion != texture->upload_version)
      UploadGameTexture(it->second, *texture);
    return true;
  }
  // A miss is about to claim a slot, so make sure there is one to claim. Done
  // here rather than per frame because this is the only place demand grows,
  // and an eviction pass that runs when nothing was inserted is pure cost.
  EvictGameTexturesToHighWater();
  if (m_freeGameSrvDescriptors.empty() &&
      m_nextGameSrvDescriptor >= kMaxGameTextures) {
    // Now genuinely unrecoverable rather than merely unhandled: eviction ran
    // and freed nothing, which means a single frame is binding more distinct
    // textures than the heap holds. Rate-limited rather than once-only -- the
    // old `static bool` reported the first occurrence in a run and hid every
    // later one, so a transient exhaustion and a permanent one read alike.
    static uint64_t s_full = 0;
    if (++s_full == 1 || (s_full % 1000) == 0) {
      char message[192];
      std::snprintf(message, sizeof(message),
                    "game texture cache full (%llu times); %zu live, %llu "
                    "evicted, %llu passes freed nothing",
                    static_cast<unsigned long long>(s_full),
                    m_gameTextures.size(),
                    static_cast<unsigned long long>(m_gameTextureEvictions),
                    static_cast<unsigned long long>(m_gameTextureEvictBlocked));
      LogError(message);
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
    case mx::hle::HostTextureFormat::kRg8:
      format = DXGI_FORMAT_R8G8_UNORM;
      break;
    // The G-buffer formats. Each is the reference's own choice for the guest
    // format behind it (d3d12_texture_cache.h host_formats_), and none of them
    // is an integer view: an integer DXGI format cannot be Sample()d, only
    // Load()ed, and every one of these is bound to a sampler.
    case mx::hle::HostTextureFormat::kRg16Float:
      format = DXGI_FORMAT_R16G16_FLOAT;
      break;
    case mx::hle::HostTextureFormat::kRg16Unorm:
      format = DXGI_FORMAT_R16G16_UNORM;
      break;
    case mx::hle::HostTextureFormat::kRg16Snorm:
      format = DXGI_FORMAT_R16G16_SNORM;
      break;
    case mx::hle::HostTextureFormat::kR16Snorm:
      format = DXGI_FORMAT_R16_SNORM;
      break;
    case mx::hle::HostTextureFormat::kRgba16Unorm:
      format = DXGI_FORMAT_R16G16B16A16_UNORM;
      break;
    case mx::hle::HostTextureFormat::kRgba16Snorm:
      format = DXGI_FORMAT_R16G16B16A16_SNORM;
      break;
    case mx::hle::HostTextureFormat::kRg32Float:
      format = DXGI_FORMAT_R32G32_FLOAT;
      break;
    case mx::hle::HostTextureFormat::kRgb10A2Unorm:
      format = DXGI_FORMAT_R10G10B10A2_UNORM;
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
  // As many levels as the guest supplied, which is often fewer than a full
  // chain -- mip_max_level is the guest's own cap, and a texture it stops at
  // level 2 should clamp there rather than be invented past it.
  td.MipLevels = UINT16(std::max<uint32_t>(texture->level_count, 1));
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

  // A recycled slot before a fresh one, so the heap stops being a ratchet.
  // Claimed only now, after the resource exists and the upload succeeded --
  // claiming earlier leaks the slot on every failure, which is the bug the note
  // in CreatePooledSurface records.
  if (!m_freeGameSrvDescriptors.empty()) {
    entry.descriptorIndex = m_freeGameSrvDescriptors.back();
    m_freeGameSrvDescriptors.pop_back();
  } else if (m_nextGameSrvDescriptor < kMaxGameTextures) {
    entry.descriptorIndex = m_nextGameSrvDescriptor++;
  } else {
    return false;
  }
  entry.lastUsedFence = m_fenceValue;
  D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
  srv.Format = format;
  srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  // Same sanitisation as the snapshot SRV branch above: guest GPUSWIZZLE is 3
  // bits per component, so 6 and 7 are representable, but
  // D3D12_SHADER_COMPONENT_MAPPING defines only 0-5. DescribeHleTexture2D stores
  // the fetch swizzle raw, so this used to hand the driver an undefined
  // component mapping. `& 5` maps 6 -> 4 (constant 0) and 7 -> 5 (constant 1).
  const auto host_component = [](uint32_t c) -> UINT {
    return c >= 4u ? UINT(c & 5u) : UINT(c);
  };
  srv.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
      host_component((texture->swizzle >> 0) & 7u),
      host_component((texture->swizzle >> 3) & 7u),
      host_component((texture->swizzle >> 6) & 7u),
      host_component((texture->swizzle >> 9) & 7u));
  srv.Texture2D.MipLevels = UINT(-1);  // however many the resource has
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

// Unload the least recently used guest textures until the cache is back under
// the high-water mark. The cache had no unload path at all -- find and emplace,
// no erase anywhere -- so every distinct fetch constant a session ever saw held
// its SRV slot until the device died.
//
// LRU by the fence a texture was last bound on, which is monotonic and already
// maintained. Textures touched by the frame being recorded are NOT evictable:
// their descriptors are referenced by a command list that has not been submitted
// yet. That is also why eviction can legitimately fail.
void D3D12Renderer::EvictGameTexturesToHighWater() {
  // DESCRIPTOR PRESSURE, not texture count. The SRV heap is shared: render
  // targets, snapshots and the video planes hold slots too -- measured at 117 in
  // a menu session, srv 824 against 707 cached textures. Thresholding on
  // m_gameTextures.size() against a figure derived from the whole heap leaves
  // the real margin unknown, and in that session it was 11 slots rather than 128.
  const uint32_t in_use =
      m_nextGameSrvDescriptor - uint32_t(m_freeGameSrvDescriptors.size());
  if (in_use <= kGameTextureHighWater) return;

  // Never the frame being recorded; see above.
  const uint64_t in_flight = m_fenceValue;
  std::vector<std::pair<uint64_t, uint64_t>> candidates;  // (lastUsed, key)
  candidates.reserve(m_gameTextures.size());
  for (const auto& [key, entry] : m_gameTextures)
    if (entry.lastUsedFence < in_flight)
      candidates.emplace_back(entry.lastUsedFence, key);

  const size_t want = in_use - kGameTextureHighWater;
  if (candidates.size() < want) ++m_gameTextureEvictBlocked;
  const size_t take = std::min(want, candidates.size());
  if (!take) return;
  std::partial_sort(candidates.begin(), candidates.begin() + take,
                    candidates.end());

  for (size_t i = 0; i < take; ++i) {
    auto it = m_gameTextures.find(candidates[i].second);
    if (it == m_gameTextures.end()) continue;
    // Resource and descriptor retire together, on the same fence. RetireResource
    // stamps m_fenceValue + 1 -- the frame being recorded -- so the slot comes
    // back only once that submission has completed.
    const uint32_t index = it->second.descriptorIndex;
    RetireResource(std::move(it->second.resource));
    for (auto& up : it->second.upload) RetireResource(std::move(up));
    const uint64_t pending = m_fenceValue + 1;
    RetiredFrame& r = (!m_retired.empty() && m_retired.back().fence == pending)
                          ? m_retired.back()
                          : m_retired.emplace_back(RetiredFrame{pending, {}, {}});
    r.srv.push_back(index);
    m_gameTextures.erase(it);
    ++m_gameTextureEvictions;
  }
}

void D3D12Renderer::RetireResource(
    Microsoft::WRL::ComPtr<ID3D12Resource>&& res) {
  if (!res) return;
  // Tag with the fence the frame BEING RECORDED will signal -- m_fenceValue + 1
  // -- not m_fenceValue itself. MoveToNextFrame does `++m_fenceValue` and *then*
  // signals it, so while a frame is being recorded m_fenceValue names the
  // PREVIOUS submission: tagging with it told DrainRetired the resource was
  // reclaimable while the command list still referencing it was executing.
  //
  // Found by DRED: CopyTextureRegion faulting on a page whose allocations were
  // all RECENTLY FREED, type 34 (RESOURCE). Intermittent because retirement only
  // happens when a target is REPLACED.
  const uint64_t pending = m_fenceValue + 1;
  RetiredFrame& r = (!m_retired.empty() && m_retired.back().fence == pending)
                        ? m_retired.back()
                        : m_retired.emplace_back(RetiredFrame{pending, {}});
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
  const HRESULT hr = m_device->CreateCommittedResource(
      &hp, D3D12_HEAP_FLAG_NONE, &rd, spec.initialState, spec.clear,
      IID_PPV_ARGS(&entry.resource));
  if (FAILED(hr)) {
    // Everything the call was asked for, because a format combination is the
    // likeliest reason for a small surface to be refused and the caller's own
    // message only knows the extent. Rate-limited: the caller retries the same
    // surface every frame, so an unbounded log buries the rest of the run.
    static uint32_t s_logged = 0;
    if (++s_logged <= 16) {
      char message[256];
      std::snprintf(message, sizeof(message),
                    "CreatePooledSurface: %ux%u FAILED hr=%s — resource fmt %u,"
                    " srv fmt %u, flags 0x%X, state 0x%X, clear %s",
                    width, height, HrText(hr).c_str(),
                    uint32_t(spec.resourceFormat), uint32_t(spec.srvFormat),
                    uint32_t(spec.flags), uint32_t(spec.initialState),
                    spec.clear ? "yes" : "no");
      LogError(message);
    }
    return false;
  }
  // Recorded HERE, from what the resource was actually created with, rather than
  // by each caller. EnsureGameRenderTarget set it and EnsureGameSnapshot did
  // not, so every snapshot claimed to be R8G8B8A8_UNORM -- the struct default --
  // whatever it really was. That is half of why an HDR resolve reached
  // CopyTextureRegion with a mismatched destination.
  entry.format = spec.resourceFormat;
  // Claimed only once the resource exists. Claiming before the call leaks a
  // descriptor on every failure, and the caller retries the same object every
  // frame -- that drained the heap to 1024/1024 in about twenty seconds.
  //
  // A recycled slot before a fresh one, matching EnsureGameTexture. Without this
  // the free list EvictGameSnapshots pushes to would only ever be drained by
  // textures.
  if (reuseSrvIndex != UINT32_MAX) {
    entry.srvIndex = reuseSrvIndex;
  } else if (!m_freeGameSrvDescriptors.empty()) {
    entry.srvIndex = m_freeGameSrvDescriptors.back();
    m_freeGameSrvDescriptors.pop_back();
  } else {
    entry.srvIndex = m_nextGameSrvDescriptor++;
  }

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

// See the field notes in the header. Called on every colour-target bind, clear
// and draw, so it must stay cheap: two small map lookups and a linear scan of
// a per-base owner list that is expected to hold one or two entries.
void D3D12Renderer::NoteEdramOwnership(uint32_t object, uint32_t width,
                                       uint32_t height, uint32_t edramBase,
                                       DXGI_FORMAT format) {
  auto& owners = m_edramOwners[edramBase];
  EdramOwner* self = nullptr;
  for (auto& o : owners) {
    if (o.object == object && o.width == width && o.height == height &&
        o.format == format) {
      self = &o;
      break;
    }
  }
  if (!self) {
    owners.push_back({object, width, height, format, 0});
    self = &owners.back();
  }
  ++self->binds;

  // A TAKEOVER is a bind at a base whose previous owner was a different object.
  // Counted rather than "how many objects share a base", because the fix has to
  // transfer contents at the moment ownership changes, and a base with two
  // owners that never alternate needs no transfer at all.
  //
  // SIZED, one freeroam session:
  //
  //   edram aliasing: 3 bases, 3 shared by >1 object;
  //                   64194 takeovers (17029 same-extent, 0 format-differs)
  //
  // Every base in the run is shared. On the console all those objects ARE the
  // same physical memory; here each object owns a separate D3D12 texture and
  // sees nothing. The 17029 SAME-EXTENT takeovers are the tractable subset -- a
  // straight CopyResource at the moment of takeover would carry the contents
  // across -- while the remainder change extent and would need a real EDRAM
  // model.
  //
  // This is the actual shape of what "the render target extent is wrong" turned
  // out to be: the extent is the guest's own surface size and is not wrong; the
  // ALIASING is unmodelled.
  const auto last = m_edramLastOwner.find(edramBase);
  if (last != m_edramLastOwner.end() && last->second.object != object) {
    const EdramOwner& prev = last->second;
    ++m_edramTakeovers;
    if (prev.width == width && prev.height == height) {
      ++m_edramTakeoverSameExtent;
      if (prev.format != format) {
        ++m_edramTakeoverFormatDiff;
      } else {
        // Same size, same format: a straight CopyResource carries the contents
        // across with no reinterpretation. Recorded rather than performed here
        // -- this runs before the new owner's texture necessarily exists, and
        // the copy has to land at first use so it is not undone by the
        // per-frame clear.
        m_edramPendingSource[object] = prev.object;
      }
    }
  }
  m_edramLastOwner[edramBase] = EdramOwner{object, width, height, format, 0};
}

D3D12Renderer::GameRenderTarget* D3D12Renderer::EnsureGameRenderTarget(
    uint32_t object, uint32_t width, uint32_t height, uint32_t edramBase,
    DXGI_FORMAT format) {
  if (!object || !width || !height || width > 8192 || height > 8192 ||
      !m_gameRtvHeap || !m_gameSrvHeap)
    return nullptr;
  NoteEdramOwnership(object, width, height, edramBase, format);
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
      // which made the object unroutable for the REST OF THE RUN -- every later
      // draw onto it fell back to the main target and overpainted the scene, the
      // exact bug offscreen routing exists to prevent. Replace instead: both
      // descriptor slots are reusable in place, and the old resource goes
      // through the retirement list because the GPU may still be reading it.
      ++m_rtRejectResized;
      reuseRtvIndex = it->second.rtvIndex;
      reuseSrvIndex = it->second.srvIndex;
      RetireResource(std::move(it->second.resource));
      m_gameRenderTargets.erase(it);
    } else {
      // The guest asked for this target by object address, which is the
      // definition of still-in-use. EvictGameRenderTargets reclaims on this
      // stamp, and stamping here also guarantees no pointer handed to a caller
      // can refer to an entry a later sweep considers idle.
      it->second.lastUsedFrame = m_gameFrame;
      return &it->second;
    }
  }
  // Reclaim dead targets before declaring the budget spent. Once per frame at
  // the high water and always at the hard cap, matching the snapshot path.
  if (reuseSrvIndex == UINT32_MAX &&
      m_gameRenderTargets.size() >= kTargetHighWater &&
      (m_targetSweepFrame != m_gameFrame ||
       m_gameRenderTargets.size() >= kMaxGameRenderTargets)) {
    m_targetSweepFrame = m_gameFrame;
    const bool atCap = m_gameRenderTargets.size() >= kMaxGameRenderTargets;
    if (EvictGameRenderTargets() == 0 && atCap) ++m_rtEvictBlocked;
  }
  // Budget exhausted. Counted separately from every other refusal because the
  // consequence is invisible: the caller falls back to the main target and the
  // draw overpaints the scene, which is exactly the bug offscreen routing was
  // built to fix.
  if (reuseSrvIndex == UINT32_MAX &&
      (m_gameRenderTargets.size() >= kMaxGameRenderTargets ||
       (m_freeGameSrvDescriptors.empty() &&
        m_nextGameSrvDescriptor >= kMaxGameTextures))) {
    ++m_rtRejectBudget;
    return nullptr;
  }

  GameRenderTarget entry;
  entry.width = width;
  entry.height = height;
  entry.edramBase = edramBase;
  entry.format = format;
  // A recycled slot before a fresh one. NOT `m_gameRenderTargets.size() + 1`:
  // that was unique only while the map could never shrink, and eviction makes
  // it collide.
  if (reuseRtvIndex != UINT32_MAX) {
    entry.rtvIndex = reuseRtvIndex;
  } else if (!m_freeGameRtvIndices.empty()) {
    entry.rtvIndex = m_freeGameRtvIndices.back();
    m_freeGameRtvIndices.pop_back();
  } else {
    entry.rtvIndex = m_nextGameRtvIndex++;
  }
  entry.lastUsedFrame = m_gameFrame;

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

  // Clear once, HERE, at creation -- not only when a draw first lands on it. The
  // per-frame clear below is gated on usedThisFrame, deliberately, so a target
  // carries its contents across frames, which is what a resolve source needs.
  // But it means a target created and never drawn into is never cleared at all,
  // and CreateCommittedResource does not guarantee zeroed memory, so the resolve
  // branch would copy undefined GPU memory into a snapshot.
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
// buffer. That is not only wrong for depth testing: the guest RESOLVES its depth
// surface to a texture to reconstruct world position in the lighting pass, and
// with nothing to copy every one of those resolves missed.
//
// R32_TYPELESS so one resource serves both views: D3D12 will not give a
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
  // EDRAM ALIASING -- two guest objects at the SAME EDRAM BASE are the same
  // memory and must be the same host surface.
  //
  // This is not a corner case, it is the deferred lighting. The menu's scene
  // depth and its light-accumulation depth are different guest objects at
  // different sizes but identical registers:
  //
  //   0x2123C208  1280x720  surface=0x14000500  base=0x000   G-buffer depth
  //   0x2123CAF4  1280x640  surface=0x14000500  base=0x000   light band 1
  //   0x2123CB24  1280x80   surface=0x14000500  base=0x280   light band 2
  //
  // 640 + 80 = 720: one EDRAM surface the guest views as two bands. Keyed by
  // OBJECT we handed the light pass a fresh, empty depth buffer cleared to 1.0,
  // so every light-volume fragment passed LEqual against the far plane. The
  // volume count is entirely depth-driven -- increment on back faces, decrement
  // on front -- so both fired everywhere and cancelled exactly, and EVERY
  // deferred light was discarded. [[edram-aliasing-unmodelled]] recorded this as
  // "measurably harmless"; it was the whole light pass.
  //
  // The viewport comes from the COLOUR target, not this one, so handing back a
  // TALLER surface does not disturb the band's rasterisation.
  uint32_t bandOwner = 0, bandRow = 0;
  if (auto ait = m_gameDepthAliases.find(object);
      ait != m_gameDepthAliases.end()) {
    auto oit = m_gameDepthTargets.find(ait->second);
    // Revalidated rather than trusted: the owner can be retired or resized out
    // from under an alias, and a stale pointer here would be a use-after-free
    // rather than a wrong picture.
    if (oit != m_gameDepthTargets.end() && oit->second.edramBase == edramBase &&
        oit->second.width == width && oit->second.height >= height) {
      ++m_depthAliasHits;
      return &oit->second;
    }
    m_gameDepthAliases.erase(ait);
  }
  for (auto& [ownerObject, owner] : m_gameDepthTargets) {
    if (ownerObject == object || owner.width != width) continue;
    if (owner.edramBase == edramBase && owner.height >= height) {
      m_gameDepthAliases[object] = ownerObject;
      ++m_depthAliasHits;
      char m[192];
      std::snprintf(m, sizeof(m),
                    "depth EDRAM alias: 0x%08X %ux%u base 0x%X -> owner "
                    "0x%08X %ux%u",
                    object, width, height, edramBase, ownerObject, owner.width,
                    owner.height);
      LogInfo(m);
      return &owner;
    }
    // A band ABOVE the owner's base. Its rows start partway down the owner, so
    // it cannot share the resource the way a base-aligned band can -- see
    // GameRenderTarget::bandDepthOwner. It keeps its own surface and takes a
    // per-frame copy of the owner's rows instead. One EDRAM tile is 80x16
    // samples, hence the row arithmetic.
    if (owner.edramBase < edramBase && owner.height > height) {
      // TILES PER ROW, ROUNDED UP. A tile is 80x16 samples, so a surface 768
      // wide occupies ceil(768/80) = 10 tiles per row and 400 tiles is 40 tile
      // rows = 640 pixel rows. The old form -- delta * 80 * 16 / width -- divides
      // by the width rather than by whole tiles, so it is only right when the
      // width is a multiple of 80: it produced 666 instead of 640 for the 768-
      // wide shadow map, and 666 + 384 > 1024 rejected the band.
      const uint32_t tilesPerRow = (width + 79u) / 80u;
      const uint32_t rows =
          tilesPerRow ? (edramBase - owner.edramBase) / tilesPerRow * 16u : 0u;
      if (rows + height <= owner.height) {
        bandOwner = ownerObject;
        bandRow = rows;
      }
    }
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
  spec.resourceFormat = kGameDepthResourceFormat;
  spec.srvFormat = kGameDepthSrvFormat;
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

  entry.bandDepthOwner = bandOwner;
  entry.bandDepthRow = bandRow;

  auto [it, inserted] = m_gameDepthTargets.emplace(object, std::move(entry));
  if (!inserted) return nullptr;
  if (bandOwner) {
    char b[192];
    std::snprintf(b, sizeof(b),
                  "depth EDRAM band: 0x%08X %ux%u base 0x%X is row %u of owner "
                  "0x%08X -- depth copied per frame",
                  object, width, height, edramBase, bandRow, bandOwner);
    LogInfo(b);
  }
  char message[192];
  std::snprintf(message, sizeof(message),
                "game depth target: object 0x%08X %ux%u cache %zu", object,
                width, height, m_gameDepthTargets.size());
  LogInfo(message);
  return &it->second;
}

// A snapshot is an offscreen surface like any other -- same struct, same
// creation, same budget -- so EnsureGameSnapshot defers to
// EnsureGameRenderTarget's storage. The only difference is the key: destination
// texture object, not source target object, which is what stops six resolves out
// of one shared scratch surface from aliasing each other.

// Hand the frame-old 1x1 exposure result to the guest. Called at the top of a
// frame, so the slot about to be reused has already been waited out by
// MoveToNextFrame and the map is guaranteed non-blocking. Only the R channel is
// published: the reduction targets are R16G16_FLOAT and the guest's destination
// is a single FMT_16_FLOAT texel.
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
  // DIAG: whether the GPU's own answer is non-zero.
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

// Copy a freshly written 1x1 snapshot into this frame's readback buffer. The
// snapshot is in PIXEL_SHADER_RESOURCE when this runs -- the resolve path just
// put it there -- and is returned to it, because the composite samples it later
// in the same frame. SurfaceTallyFor is linear over a handful of slots.
D3D12Renderer::SurfaceReadbackTally* D3D12Renderer::SurfaceTallyFor(
    uint32_t destObject) {
  SurfaceReadbackTally* dullest = nullptr;
  for (auto& t : m_surfaceTally) {
    if (t.destObject == destObject) return &t;
    if (!t.destObject) {
      t.destObject = destObject;
      return &t;
    }
    // EVICT THE UNINFORMATIVE, never a row that carries an outcome. Boot fills
    // this table with short-lived destinations that are ineligible every time,
    // and first-come-first-served let them hold every slot: one run overflowed
    // 73,185 times and the deform destination -- the one the table exists to
    // describe -- had no row at all. A row that has ever been served or lost the
    // slot is an outcome and stays; among the rest the least-seen goes.
    if (t.won || t.lostBusy) continue;
    if (!dullest || t.seen < dullest->seen) dullest = &t;
  }
  ++m_surfaceTallyOverflow;
  if (!dullest) return nullptr;
  *dullest = SurfaceReadbackTally{};
  dullest->destObject = destObject;
  return dullest;
}

// Copy a freshly resolved SMALL REGION back into this frame's readback buffer,
// so the D3D9 layer can put it into guest memory where the guest reads it.
//
// THE REGION, NOT THE SNAPSHOT. `snap` is the DESTINATION snapshot and is sized
// to the destination TEXTURE, so reading its extent describes the guest's whole
// resource, not the rectangle this resolve moved. That was invisible while the
// VT feedback buffer was the only caller -- 64x64 into a 64x64 destination --
// and it is the entire reason the terrain deformation never came back: its
// resolve puts a 128x32 tile into a 2048x2048 accumulation, the footprint came
// to 16 MB, and the size check below returned WITHOUT INCREMENTING ANY COUNTER.
//
// One readback per frame. If a second eligible destination appears the tally now
// says so explicitly (lostBusy).
void D3D12Renderer::QueueSurfaceReadback(GameRenderTarget* snap,
                                         uint32_t destObject,
                                         uint32_t destWidth,
                                         uint32_t destHeight, uint32_t destX,
                                         uint32_t destY, uint32_t regionW,
                                         uint32_t regionH) {
  if (!snap || !snap->resource || !destObject) return;
  if (!destWidth || !destHeight) return;
  SurfaceReadbackTally* tally = SurfaceTallyFor(destObject);
  if (tally) ++tally->seen;
  // Every exit below is one of these three, so `seen` is a real denominator. The
  // EXTENT is recorded here too, not only on a win: set in the won branch alone,
  // every refused destination printed `0x0` and the census could not say WHAT
  // was being refused.
  auto reject = [&](uint32_t reason, uint32_t w = 0, uint32_t h = 0) {
    if (tally) {
      ++tally->ineligible;
      tally->lastReason = reason;
      if (w && h) {
        tally->attemptedW = w;
        tally->attemptedH = h;
      }
    }
  };
  // 1x1 belongs to the luminance path, which carries semantics this one must
  // not duplicate.
  if (destWidth * destHeight <= 1) {
    reject(1);
    return;
  }
  const D3D12_RESOURCE_DESC sd = snap->resource->GetDesc();
  // Typeless cannot be a buffer copy source -- the footprint has no way to say
  // what the bytes mean, and the runtime defers the complaint to Close(), which
  // then fails every frame and takes the command list with it.
  if (sd.Format == DXGI_FORMAT_R32_TYPELESS ||
      sd.Format == DXGI_FORMAT_R24G8_TYPELESS ||
      sd.Format == DXGI_FORMAT_R32G8X24_TYPELESS ||
      sd.Format == DXGI_FORMAT_UNKNOWN) {
    reject(2);
    return;
  }
  // The rectangle the resolve wrote, at the destpoint it wrote it to. A caller
  // that does not know its region passes 0 and gets the whole surface, which is
  // what every caller did before the deformation existed.
  const uint32_t sx = std::min<uint32_t>(destX, uint32_t(sd.Width));
  const uint32_t sy = std::min<uint32_t>(destY, sd.Height);
  uint32_t copyW = regionW ? regionW : uint32_t(sd.Width);
  uint32_t copyH = regionH ? regionH : sd.Height;
  copyW = std::min<uint32_t>(copyW, uint32_t(sd.Width) - sx);
  copyH = std::min<uint32_t>(copyH, sd.Height - sy);
  if (!copyW || !copyH) {
    reject(3);
    return;
  }
  // A sanity bound against a pathological pitch only; the byte tests decide.
  if (copyW > 4096u || copyH > 4096u) {
    ++m_surfaceReadbackTooBig;
    reject(4, copyW, copyH);
    return;
  }
  // The footprint of the REGION, described as a texture of its own. Asking
  // GetCopyableFootprints about a synthetic desc rather than computing a pitch
  // by hand keeps the alignment rules and the per-format texel size where the
  // runtime already knows them.
  D3D12_RESOURCE_DESC rd = sd;
  rd.Width = copyW;
  rd.Height = copyH;
  rd.DepthOrArraySize = 1;
  rd.MipLevels = 1;
  rd.SampleDesc.Count = 1;
  rd.SampleDesc.Quality = 0;
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout = {};
  UINT numRows = 0;
  UINT64 rowSize = 0, totalBytes = 0;
  m_device->GetCopyableFootprints(&rd, 0, 1, 0, &layout, &numRows, &rowSize,
                                  &totalBytes);
  if (!totalBytes || totalBytes > kSurfaceReadbackBytes) {
    ++m_surfaceReadbackTooBig;
    reject(5, copyW, copyH);
    if (tally) tally->attemptedBytes = uint32_t(totalBytes);
    return;
  }
  // AGAINST THE CPU BUFFER, not the GPU one. kSurfaceReadbackBytes is the 64 KB
  // upload-heap resource; kMaxSurfaceReadbackBytes is the 16 KB array the bytes
  // are memcpy'd into, and DrainSurfaceReadback CLAMPS to it. Gating on the
  // larger let a 16-64 KB readback through to be silently truncated. Nothing hit
  // it while 64x64x4 was the only caller, because that is exactly 16 KB.
  if (totalBytes > mx::hle::kMaxSurfaceReadbackBytes) {
    ++m_surfaceReadbackTooBig;
    reject(6, copyW, copyH);
    if (tally) tally->attemptedBytes = uint32_t(totalBytes);
    return;
  }
  // LAST, so that `lostBusy` counts only callers that would otherwise have been
  // served. With the busy test first, an ineligible caller arriving after the
  // slot was taken was indistinguishable from a starved one.
  auto& slots = m_surfaceSlots[m_frameIndex];
  SurfaceSlot* claimed = nullptr;
  bool anyBuffer = false;
  for (auto& sl : slots) {
    if (!sl.buffer) continue;
    anyBuffer = true;
    if (!sl.pending) {
      claimed = &sl;
      break;
    }
  }
  if (!anyBuffer) {
    reject(7);
    return;
  }
  if (!claimed) {
    ++m_surfaceReadbackRefused;
    if (tally) ++tally->lostBusy;
    return;
  }
  auto& rb = claimed->buffer;
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
  D3D12_BOX box = {};
  box.left = sx;
  box.top = sy;
  box.front = 0;
  box.right = sx + copyW;
  box.bottom = sy + copyH;
  box.back = 1;
  m_commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
  D3D12_RESOURCE_BARRIER post = pre;
  post.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
  post.Transition.StateAfter = snap->state;
  m_commandList->ResourceBarrier(1, &post);
  claimed->pending = 1;
  claimed->destObject = destObject;
  // The COPIED region, not the destination's extent -- the writeback walks
  // these and places them at the destpoint.
  claimed->width = copyW;
  claimed->height = copyH;
  claimed->destX = sx;
  claimed->destY = sy;
  claimed->srcFormat = uint32_t(sd.Format);
  claimed->rowPitch = layout.Footprint.RowPitch;
  claimed->texelBytes = layout.Footprint.RowPitch && layout.Footprint.Width
                            ? uint32_t(rowSize / layout.Footprint.Width)
                            : 0;
  claimed->byteCount = uint32_t(totalBytes);
  if (tally) {
    ++tally->won;
    tally->width = copyW;
    tally->height = copyH;
  }
}

void D3D12Renderer::DrainSurfaceReadback() {
  // Kept in step here rather than in the header, which does not pull in
  // hle_types.h. A mismatch would silently publish into the wrong slot.
  static_assert(kSurfaceSlots == mx::hle::kSurfaceReadbackSlots,
                "renderer slot count must match the shared readback array");
  auto& slots = m_surfaceSlots[m_frameIndex];
  const SurfaceSlot* last = nullptr;
  bool any = false;
  for (uint32_t i = 0; i < kSurfaceSlots; ++i) {
    auto& sl = slots[i];
    if (!sl.pending) continue;
    sl.pending = 0;
    if (!sl.buffer) continue;
    const uint32_t bytes =
        std::min(sl.byteCount, uint32_t(mx::hle::kMaxSurfaceReadbackBytes));
    void* mapped = nullptr;
    D3D12_RANGE readRange = {0, bytes};
    if (FAILED(sl.buffer->Map(0, &readRange, &mapped)) || !mapped) continue;
    {
      std::lock_guard<std::mutex> lk(mx::hle::g_surfaceReadbackMutex);
      auto& s = mx::hle::g_surfaceReadback[i];
      s.destObject = sl.destObject;
      s.width = sl.width;
      s.height = sl.height;
      s.rowPitch = sl.rowPitch;
      s.bytesPerTexel = sl.texelBytes;
      s.byteCount = bytes;
      s.destX = sl.destX;
      s.destY = sl.destY;
      s.srcFormat = sl.srcFormat;
      std::memcpy(s.bytes, mapped, bytes);
      // Non-zero and monotonic, and written LAST inside the lock: a consumer
      // that has already acted on this seq skips the slot, which is how two
      // destinations delivered in the same frame both get seen.
      s.seq = ++m_surfaceSeq;
      if (!s.seq) s.seq = ++m_surfaceSeq;
    }
    D3D12_RANGE noWrite = {0, 0};
    sl.buffer->Unmap(0, &noWrite);
    if (m_surfaceReadbacks < 8) {
      char msg[192];
      std::snprintf(msg, sizeof(msg),
                    "SURFACE readback #%llu dest 0x%08X %ux%u pitch %u texel "
                    "%uB %u bytes",
                    static_cast<unsigned long long>(m_surfaceReadbacks),
                    sl.destObject, sl.width, sl.height, sl.rowPitch,
                    sl.texelBytes, bytes);
      LogInfo(msg);
    }
    ++m_surfaceReadbacks;
    last = &sl;
    any = true;
  }
  if (!any) return;
  // UNCAPPED, and with the refusals beside the successes. The per-readback line
  // above stops after 8, and destinations that queue every frame consume all
  // eight before the interesting one appears; worse, the two refusal counters
  // were once incremented and PRINTED NOWHERE.
  //
  // `refused-busy` is the count of callers that were ELIGIBLE and found every
  // slot taken. A large number here is the case for more slots.
  if ((m_surfaceReadbacks % 240) < kSurfaceSlots && last) {
    char msg[224];
    std::snprintf(msg, sizeof(msg),
                  "SURFACE readback census: %llu queued, %llu refused-busy, "
                  "%llu refused-too-big; last dest 0x%08X %ux%u at (%u,%u) "
                  "texel %uB",
                  static_cast<unsigned long long>(m_surfaceReadbacks),
                  static_cast<unsigned long long>(m_surfaceReadbackRefused),
                  static_cast<unsigned long long>(m_surfaceReadbackTooBig),
                  last->destObject, last->width, last->height, last->destX,
                  last->destY, last->texelBytes);
    LogInfo(msg);
    // PER DESTINATION, because the totals above cannot answer the only question
    // that matters: for a destination that is never written, is it losing a slot
    // or is it ineligible -- and at which gate. `seen` is every call;
    // ineligible + lostBusy + won accounts for all of it.
    //
    // reason: 1 destination is a single texel (luminance path's business)
    //         2 source format cannot be a copy source
    //         3 copied region empty after clamping to the snapshot
    //         4 copied region edge implausible
    //         5 footprint exceeds the 64 KB GPU readback buffer
    //         6 footprint exceeds the 16 KB CPU buffer
    //         7 no readback resource for this frame index
    for (const auto& t : m_surfaceTally) {
      if (!t.destObject) continue;
      char row[192];
      std::snprintf(row, sizeof(row),
                    "  readback dest 0x%08X %ux%u (attempted %ux%u, %u B) | "
                    "seen %llu = won %llu + ineligible %llu (last reason %u) + "
                    "lost-busy %llu",
                    t.destObject, t.width, t.height, t.attemptedW,
                    t.attemptedH, t.attemptedBytes,
                    static_cast<unsigned long long>(t.seen),
                    static_cast<unsigned long long>(t.won),
                    static_cast<unsigned long long>(t.ineligible), t.lastReason,
                    static_cast<unsigned long long>(t.lostBusy));
      LogInfo(row);
    }
    if (m_surfaceTallyOverflow) {
      char row[96];
      std::snprintf(row, sizeof(row), "  readback tally overflow %llu",
                    static_cast<unsigned long long>(m_surfaceTallyOverflow));
      LogInfo(row);
    }
  }
  // Bumped last, so a reader that checks the sequence first cannot pair a new
  // sequence with half-written bytes.
  mx::hle::g_surfaceReadbackSeq.fetch_add(1, std::memory_order_release);
}

void D3D12Renderer::QueueLuminanceReadback(GameRenderTarget* snap,
                                           uint32_t destObject) {
  if (!snap || !snap->resource || !destObject) return;
  // ONE copy per frame, which is the shape that was measured working. A
  // four-slot version -- four placed footprints into one buffer at 512-byte
  // offsets -- failed Close every time, and rather than keep guessing at why,
  // this rotates across destinations instead: each frame samples the next 1x1
  // resolve in turn. The adaptation filters over time anyway.
  if (m_luminancePending[m_frameIndex]) return;
  const D3D12_RESOURCE_DESC sd = snap->resource->GetDesc();
  // A typeless resource cannot be the source of a buffer copy -- the footprint
  // has no way to say what the bytes mean. Depth snapshots are R32_TYPELESS, and
  // a 1x1 depth resolve would otherwise record a copy the runtime rejects at
  // Close, which kills the command list for the rest of the run. The reduction
  // chain is R16G16_FLOAT, so nothing legitimate is turned away.
  if (sd.Format == DXGI_FORMAT_R32_TYPELESS ||
      sd.Format == DXGI_FORMAT_R24G8_TYPELESS ||
      sd.Format == DXGI_FORMAT_R32G8X24_TYPELESS ||
      sd.Format == DXGI_FORMAT_UNKNOWN)
    return;
  // The caller checked the RESOLVE was 1x1; this checks the RESOURCE is, since
  // EnsureGameSnapshot grows and never shrinks. Copying a whole subresource
  // means the two have to agree.
  if (sd.Width != 1 || sd.Height != 1 || sd.DepthOrArraySize != 1 ||
      sd.MipLevels != 1 || sd.SampleDesc.Count != 1)
    return;
  // Ask the device for the footprint rather than deriving one. Hand-computing a
  // 256-byte row gets the pitch right but D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT
  // is 512, so the destination is too small for a placed footprint -- and the
  // debug layer never flags it, because the runtime defers the complaint to
  // Close(), which then fails every frame and takes the command list with it.
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

// Reclaim snapshots nothing has sampled for kSnapshotIdleFrames.
//
// Idle-only, never least-recently-used-to-make-room. The steady-state live count
// is ~33 against a cap of 128, so everything above that is a dead map's
// leftovers, and evicting the coldest LIVE entry would thrash a snapshot the
// guest still samples. If a sweep frees nothing the caller still refuses, and
// m_snapshotEvictBlocked says so.
//
// EvictGameRenderTargets does the same for offscreen colour targets, idle-only
// for one more reason besides: a target holds ACCUMULATED CONTENT. Both
// descriptor slots go back to their free lists, which is why rtvIndex stopped
// being derived from the map's size.
uint32_t D3D12Renderer::EvictGameRenderTargets() {
  const size_t before = m_gameRenderTargets.size();
  for (auto it = m_gameRenderTargets.begin();
       it != m_gameRenderTargets.end();) {
    const bool idle = m_gameFrame > it->second.lastUsedFrame &&
                      (m_gameFrame - it->second.lastUsedFrame) >=
                          kSnapshotIdleFrames;
    // Never evict a target this frame has already touched: RenderGameFrame
    // holds raw pointers into this map across the draw loop.
    if (!idle || it->second.usedThisFrame) {
      ++it;
      continue;
    }
    if (it->second.resource) RetireResource(std::move(it->second.resource));
    m_freeGameSrvDescriptors.push_back(it->second.srvIndex);
    m_freeGameRtvIndices.push_back(it->second.rtvIndex);
    it = m_gameRenderTargets.erase(it);
    ++m_rtEvictions;
  }
  return uint32_t(before - m_gameRenderTargets.size());
}

uint32_t D3D12Renderer::EvictGameSnapshots() {
  const size_t before = m_gameSnapshots.size();
  for (auto it = m_gameSnapshots.begin(); it != m_gameSnapshots.end();) {
    // Unsigned, so compare rather than subtract: a snapshot stamped on a later
    // frame than m_gameFrame would otherwise wrap to a huge age and be evicted
    // on the spot.
    const bool idle = m_gameFrame > it->second.lastUsedFrame &&
                      (m_gameFrame - it->second.lastUsedFrame) >=
                          kSnapshotIdleFrames;
    if (!idle) {
      ++it;
      continue;
    }
    // The descriptor goes back to the shared pool, and the resource goes to the
    // retirement list rather than being released inline -- the GPU may still be
    // reading it this frame. Releasing inline is what made every later create
    // of that size fail; see RetireResource.
    if (it->second.resource) RetireResource(std::move(it->second.resource));
    m_freeGameSrvDescriptors.push_back(it->second.srvIndex);
    it = m_gameSnapshots.erase(it);
    ++m_snapshotEvictions;
  }
  return uint32_t(before - m_gameSnapshots.size());
}

D3D12Renderer::GameRenderTarget* D3D12Renderer::EnsureGameSnapshot(
    uint32_t destTexture, uint32_t width, uint32_t height,
    DXGI_FORMAT format) {
  if (!destTexture || !width || !height) return nullptr;
  // GROW to cover, never resize to match. A snapshot is assembled from one or
  // more resolve bands: the scene arrives as 1280x640 then 1280x80 at y=640, two
  // EDRAM bands of one 1280x720 surface, and sizing to whichever band arrived
  // last made the whole scene an 80-line strip.
  //
  // An entry that already covers the request is returned untouched, so the
  // steady state after the first frame is no allocation at all -- which also
  // ends the 2x-per-frame create-and-destroy of a 3.5MB committed resource.
  uint32_t reuseSrvIndex = UINT32_MAX;
  Microsoft::WRL::ComPtr<ID3D12Resource> growFrom;
  D3D12_RESOURCE_STATES growFromState = D3D12_RESOURCE_STATE_COMMON;
  uint32_t growWidth = 0, growHeight = 0;
  if (auto it = m_gameSnapshots.find(destTexture); it != m_gameSnapshots.end()) {
    // FORMAT, not only extent. One guest destination texture can be resolved
    // into from sources of different formats over a run, and this cache returned
    // the first snapshot ever made for it -- so a later resolve out of an
    // R16G16B16A16 HDR target copied into an R8G8B8A8 snapshot, which
    // CopyTextureRegion rejects outright. An invalid call makes the whole command
    // list fail to Close, which killed the renderer for the rest of the run --
    // the 0.40 fps menu. Recovery alone does not fix it: the same copy is
    // re-issued every frame.
    const bool format_ok = it->second.format == format;
    if (format_ok && it->second.width >= width && it->second.height >= height)
      return &it->second;
    ++m_rtRejectResized;
    // WHICH destination, and WHICH of the two reasons. `resized` lumps "grew to
    // cover another band" together with "destroyed on a format change" and names
    // neither. That distinction is the whole question for an ATLAS: a band is
    // rewritten every frame and survives either way, while the terrain atlas
    // accumulates over ~1900 frames from NINE resolves.
    {
      static std::set<uint32_t> s_seen;
      if (s_seen.insert(destTexture).second && s_seen.size() <= 24) {
        REXLOG_INFO(
            "[D3D12Renderer] snapshot 0x{:08X} RECREATED: {}x{} fmt {} -> "
            "{}x{} fmt {} ({}) -- content {}",
            destTexture, it->second.width, it->second.height,
            uint32_t(it->second.format), width, height, uint32_t(format),
            format_ok ? "grew to cover" : "FORMAT CHANGED",
            format_ok ? "carried forward" : "DISCARDED");
      }
    }
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
  // Reclaim dead entries before declaring the budget spent; without this the map
  // is a ratchet. Once per frame at the high water, and unconditionally at the
  // hard cap: the once-per-frame guard keeps the scan off the per-resolve path,
  // but a frame that reaches the cap having already swept must still get a second
  // chance rather than refuse.
  if (reuseSrvIndex == UINT32_MAX &&
      m_gameSnapshots.size() >= kSnapshotHighWater &&
      (m_snapshotSweepFrame != m_gameFrame ||
       m_gameSnapshots.size() >= kMaxGameSnapshots)) {
    m_snapshotSweepFrame = m_gameFrame;
    // Counted as blocked ONLY at the hard cap. Above the high water a sweep
    // that frees nothing is ordinary -- the working set is simply large and
    // nothing has aged out yet -- so counting those would make this climb
    // constantly and mean nothing. At the cap it means the very next snapshot
    // gets refused, which is the thing worth seeing.
    const bool atCap = m_gameSnapshots.size() >= kMaxGameSnapshots;
    if (EvictGameSnapshots() == 0 && atCap) ++m_snapshotEvictBlocked;
  }
  // Counted apart from m_rtRejectBudget. Sharing that counter put snapshot
  // refusals in the "game RT routing" line, where they read as an offscreen
  // target being refused -- 1894 of them sat there being read as something else
  // while the post-process chain went unresolved.
  if (reuseSrvIndex == UINT32_MAX &&
      (m_gameSnapshots.size() >= kMaxGameSnapshots ||
       (m_freeGameSrvDescriptors.empty() &&
        m_nextGameSrvDescriptor >= kMaxGameTextures))) {
    ++m_snapshotRejectBudget;
    return nullptr;
  }

  GameRenderTarget entry;
  // Stamped at creation, not left at 0: a fresh snapshot would otherwise
  // read as maximally idle and be the first thing the next sweep evicted.
  entry.lastUsedFrame = m_gameFrame;
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
  snapSpec.srvFormat = SrvFormatForResource(format);
  if (!CreatePooledSurface(entry, width, height, snapSpec, reuseSrvIndex)) {
    // Loudly, and without having spent a descriptor -- CreatePooledSurface
    // claims the index only after the resource exists. Claiming it before leaked
    // one on every failure, silently, because this path used to return with no
    // log; the caller retries the same texture next frame, so it drained the
    // heap to 1024/1024 in about twenty seconds.
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
  // resolved into this texture -- the band that triggered the growth covers only
  // its own slice, so without this the rest of the image would be undefined
  // memory every time the extent changes. The old resource is already retired,
  // so it stays alive until the fence passes.
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
