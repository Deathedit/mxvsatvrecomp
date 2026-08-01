#include "native_heap.h"

#include <rex/logging.h>

// TODO(plan.md Phase 1): wire GpuMemoryHeap into D3D12Renderer so per-frame
// upload buffers in SetGameDrawData and UploadVideoFrame recycle from a
// single ID3D12Heap plus CreatePlacedResource, instead of one
// CreateCommittedResource per call. This file is scaffolded but no caller
// instantiates GpuMemoryHeap yet.

namespace mx::native {

bool GpuMemoryHeap::Initialize(ID3D12Device* device, uint64_t size) {
  if (m_heap) return true;

  D3D12_HEAP_DESC heapDesc = {};
  heapDesc.SizeInBytes = size;
  heapDesc.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
  heapDesc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
  heapDesc.Properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
  heapDesc.Properties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

  if (FAILED(device->CreateHeap(&heapDesc, IID_PPV_ARGS(&m_heap)))) {
    REXLOG_ERROR("GpuMemoryHeap: CreateHeap failed");
    return false;
  }

  m_device = device;
  m_size = size;
  m_offset = 0;

  REXLOG_INFO("GpuMemoryHeap: {} MB heap created", size / (1024 * 1024));
  return true;
}

void GpuMemoryHeap::Shutdown() {
  if (m_heap) { m_heap->Release(); m_heap = nullptr; }
  m_device = nullptr;
  m_size = 0;
  m_offset = 0;
}

ID3D12Resource* GpuMemoryHeap::Allocate(uint64_t size, uint64_t alignment,
                                        D3D12_RESOURCE_STATES initialState,
                                        const D3D12_RESOURCE_DESC& desc) {
  if (!m_heap || !m_device) return nullptr;

  if (alignment == 0) alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
  m_offset = (m_offset + alignment - 1) & ~(alignment - 1);

  if (m_offset + size > m_size) {
    REXLOG_ERROR("GpuMemoryHeap: out of memory ({} MB used, {} MB total)",
                 m_offset / (1024 * 1024), m_size / (1024 * 1024));
    return nullptr;
  }

  ID3D12Resource* resource = nullptr;
  HRESULT hr = m_device->CreatePlacedResource(
      m_heap, m_offset, &desc, initialState, nullptr,
      IID_PPV_ARGS(&resource));
  if (FAILED(hr)) {
    REXLOG_ERROR("GpuMemoryHeap: CreatePlacedResource failed");
    return nullptr;
  }

  uint64_t alignedSize = (size + alignment - 1) & ~(alignment - 1);
  m_offset += alignedSize;

  return resource;
}

}  // namespace mx::native
