#include "gpu/d3d9_state.h"

namespace mx::pm4 {

D3D9DeviceState& DeviceState() {
  static D3D9DeviceState s;
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
    case kEpSetViewport:      return "SetViewport";
    case kEpSetScissorRect:   return "SetScissorRect";
    case kEpSetRenderState:   return "SetRenderState_*";
    default:                  return "?";
  }
}

}  // namespace mx::pm4
