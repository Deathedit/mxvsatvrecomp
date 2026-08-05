#pragma once

// HLSL source for the two pipelines owned by D3D12Renderer.
//   - kVideoVS / kVideoPS : fullscreen triangle + texture sample (Bink frames)
//   - kGameVS  / kGamePS  : position+color vertex layout with an MVP constant
//                           buffer (currently the placeholder triangle, and the
//                           target for translated PM4 draw calls)

namespace mx::gfx::shaders {

inline constexpr const char* kVideoVS = R"(
struct VSOutput {
  float4 pos : SV_Position;
  float2 uv  : TEXCOORD0;
};
VSOutput main(uint vid : SV_VertexID) {
  VSOutput o;
  o.uv = float2((vid << 1) & 2, vid & 2);
  o.pos = float4(o.uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
  return o;
}
)";

inline constexpr const char* kVideoPS = R"(
Texture2D    g_tex : register(t0);
SamplerState g_smp : register(s0);
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
  return g_tex.Sample(g_smp, uv);
}
)";

inline constexpr const char* kGameVS = R"(
struct VSInput {
  float3 pos : POSITION;
  float4 col : COLOR;
  float2 uv  : TEXCOORD0;
};
struct VSOutput {
  float4 pos : SV_POSITION;
  float4 col : COLOR;
  float2 uv  : TEXCOORD0;
};
cbuffer GameCB : register(b0) {
  // row_major is load-bearing. Pm4Translator builds this matrix row-major and
  // the renderer memcpy's the 16 floats straight in; HLSL packs a cbuffer
  // float4x4 column-major by default, which would silently transpose it.
  row_major float4x4 mvp;
};
VSOutput main(VSInput input) {
  VSOutput o;
  o.pos = mul(mvp, float4(input.pos, 1.0));
  o.col = input.col;
  o.uv = input.uv;
  return o;
}
)";

inline constexpr const char* kGameTexturePS = R"(
Texture2D    g_tex : register(t0);
SamplerState g_smp : register(s0);
float4 main(float4 pos : SV_POSITION, float4 col : COLOR,
            float2 uv : TEXCOORD0) : SV_TARGET {
  return g_tex.Sample(g_smp, uv) * col;
}
)";

inline constexpr const char* kGamePS = R"(
float4 main(float4 pos : SV_POSITION, float4 col : COLOR) : SV_TARGET {
  return col;
}
)";

}  // namespace mx::gfx::shaders
