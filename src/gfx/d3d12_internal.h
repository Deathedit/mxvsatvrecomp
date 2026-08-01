#pragma once

// Helpers shared by the three D3D12Renderer translation units
// (d3d12_device.cpp, d3d12_video.cpp, d3d12_game.cpp). Not part of the public
// renderer interface — do not include outside src/gfx/.

#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstring>

#include <rex/logging.h>

namespace mx::gfx {

// Preferred GPU vendor when several discrete adapters are present.
inline constexpr const wchar_t* kAdapterNamePrefix = L"NVIDIA";

inline void LogError(const char* msg) {
  char buf[512] = {};
  snprintf(buf, sizeof(buf), "[D3D12Renderer] ERROR: %s", msg);
  OutputDebugStringA(buf);
  REXLOG_ERROR("{}", buf);
}

inline void LogInfo(const char* msg) {
  char buf[512] = {};
  snprintf(buf, sizeof(buf), "[D3D12Renderer] %s", msg);
  OutputDebugStringA(buf);
  REXLOG_INFO("{}", buf);
}

inline Microsoft::WRL::ComPtr<ID3DBlob> CompileShader(const char* source,
                                                      const char* target,
                                                      const char* entry) {
  Microsoft::WRL::ComPtr<ID3DBlob> blob;
  Microsoft::WRL::ComPtr<ID3DBlob> errors;
  HRESULT hr = D3DCompile(source, strlen(source), nullptr, nullptr, nullptr,
                          entry, target, 0, 0, &blob, &errors);
  if (FAILED(hr)) {
    if (errors) {
      LogError(static_cast<const char*>(errors->GetBufferPointer()));
    }
    return nullptr;
  }
  return blob;
}

}  // namespace mx::gfx
