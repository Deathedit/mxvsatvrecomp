#include "gpu/d3d9_state.h"

namespace mx::hle {

// Thread-local, because the guest's parallel record path gives each of its three
// workers its OWN D3D9 device (dword_830B2C60[0..2]) on its own thread. A single
// shared instance meant worker 1's SetTexture / SetRenderTarget / SetViewport
// overwrote worker 0's between that worker's bind and its draw. The workers
// never migrate threads (sub_82AC8CC8 is their thread proc and loops for the
// life of the process), so thread-local is exactly per-device.
D3D9DeviceState& DeviceState() {
  static thread_local D3D9DeviceState s;
  return s;
}

const char* RenderStateName(uint32_t id) {
  switch (id) {
    case kRsZEnable:                   return "ZEnable";
    case kRsAlphaBlendEnable:          return "AlphaBlendEnable";
    case kRsSrcBlend:                  return "SrcBlend";
    case kRsDestBlend:                 return "DestBlend";
    case kRsBlendOp:                   return "BlendOp";
    case kRsColorWriteEnable:          return "ColorWriteEnable";
    case kRsSeparateAlphaBlendEnable:  return "SeparateAlphaBlendEnable";
    case kRsBlendFactor:               return "BlendFactor";
    default:                           return "?";
  }
}

const char* EntryPointName(uint32_t id) {
  switch (id) {
    case kEpDraw:             return "Draw*";
    case kEpSetStreamSource:  return "SetStreamSource";
    case kEpSetIndices:       return "SetIndices";
    case kEpSetVertexShader:  return "SetVertexShader";
    case kEpSetPixelShader:   return "SetPixelShader";
    case kEpSetTexture:       return "SetTexture";
    case kEpSetRenderTarget:  return "SetRenderTarget";
    case kEpSetDepthStencil:  return "SetDepthStencilSurface";
    case kEpResolve:          return "Resolve";
    case kEpSetViewport:      return "SetViewport";
    case kEpSetScissorRect:   return "SetScissorRect";
    case kEpSetRenderState:   return "SetRenderState_*";
    default:                  return "?";
  }
}

uint64_t& D3D9DrawCounter() {
  static uint64_t n = 0;
  return n;
}

uint64_t& D3D9IndexedDrawCounter() {
  static uint64_t n = 0;
  return n;
}

uint64_t& D3D9FrameCount() {
  static uint64_t n = 0;
  return n;
}

uint64_t& D3D9FrameNanos() {
  static uint64_t n = 0;
  return n;
}

}  // namespace mx::hle
