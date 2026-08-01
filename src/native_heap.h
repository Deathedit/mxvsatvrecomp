#pragma once

#include <Windows.h>
#include <d3d12.h>
#include <cstdint>
#include <cstddef>
#include <unordered_map>
#include <vector>

struct ID3D12Device;
struct ID3D12Heap;
struct ID3D12Resource;

namespace mx::native {

class GpuMemoryHeap {
 public:
  bool Initialize(ID3D12Device* device, uint64_t size);
  void Shutdown();

  ID3D12Resource* Allocate(uint64_t size, uint64_t alignment,
                           D3D12_RESOURCE_STATES initialState,
                           const D3D12_RESOURCE_DESC& desc);

 private:
  ID3D12Device* m_device = nullptr;
  ID3D12Heap* m_heap = nullptr;
  uint64_t m_size = 0;
  uint64_t m_offset = 0;
};

}  // namespace mx::native
