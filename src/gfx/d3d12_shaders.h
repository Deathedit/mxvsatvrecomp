#pragma once

// HLSL source for the pipeline owned by D3D12Renderer.
//   - kGameVS / kGamePS : position+color+uv vertex layout with an MVP constant
//                         buffer, the target for translated guest draw calls.
//
// The kVideoVS / kVideoPS fullscreen-blit pair was REMOVED 2026-08-06 with the
// host FFmpeg intro player; the guest decodes and draws its own video.

namespace mx::gfx::shaders {

inline constexpr const char* kGameVS = R"(
struct VSInput {
  // Clip space, straight from the guest vertex shader's position export. The
  // w component is not decoration: the rasteriser needs it to clip against the
  // near plane and to interpolate perspective-correctly.
  float4 pos : POSITION;
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
  o.pos = mul(mvp, input.pos);
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

// The pixel stage for a guest DEPTH pass, paired with a translated vertex stage.
//
// SetPixelShader(NULL) is legal on the console for a pass that writes only
// depth, and this game uses it heavily -- 70,000 such draws in one run, every
// one binding RB_COLOR_MASK 0. Without a pixel stage to pair with, the whole
// draw used to fall back to the software vertex interpreter.
//
// Returning zero is not a colour decision. TranslatedPSO already sets
// RenderTargetWriteMask to 0 for these draws, so nothing this returns can reach
// the target -- and the hooks side only routes a draw here when it has
// established exactly that. If this output is ever visible, the gate upstream is
// wrong, not this value.
//
// Declaring only SV_Position is deliberate: a pixel shader's input signature
// must be a SUBSET of the vertex stage's output, and every translated vertex
// stage emits SV_Position plus its interpolators. Naming just the one both
// shapes always carry means this pairs with any of them and needs no variant.
inline constexpr const char* kTranslatedDepthOnlyPS = R"(
float4 main(float4 pos : SV_Position) : SV_TARGET { return 0; }
)";

// Bink's frame composite. The guest binds three single-channel planes -- Y at
// full resolution, Cr and Cb at half -- and optionally a fourth alpha plane,
// then runs its own YUV->RGB shader.
//
// All four planes sample with the same normalized uv: the chroma planes being
// half-size is handled by the sampler, not by scaling the coordinates. That
// holds only because PrepareBinkPlanes crops the chroma planes to exactly half
// the luma extent first -- the guest rounds their allocation up, and sampling
// the padding rows gave zero chroma, which the conversion turns into a saturated
// green line along the bottom edge of every video. Do not remove that crop
// without scaling the chroma uv here instead.
//
// BT.601 with the usual 16-235 luma and 16-240 chroma ranges, which is what Bink
// encodes. If video comes out washed out or too contrasty, this range handling
// is the first suspect, not the coefficients.
inline constexpr const char* kGameYuvPS = R"(
Texture2D    g_y     : register(t0);
Texture2D    g_cr    : register(t1);
Texture2D    g_cb    : register(t2);
Texture2D    g_alpha : register(t3);
SamplerState g_smp   : register(s0);
float4 main(float4 pos : SV_POSITION, float4 col : COLOR,
            float2 uv : TEXCOORD0) : SV_TARGET {
  float y  = g_y.Sample(g_smp, uv).r;
  float cr = g_cr.Sample(g_smp, uv).r;
  float cb = g_cb.Sample(g_smp, uv).r;
  y  = (y - 0.0627451) * 1.164383;
  cr = cr - 0.5;
  cb = cb - 0.5;
  float3 rgb;
  rgb.r = saturate(y + 1.596027 * cr);
  rgb.g = saturate(y - 0.391762 * cb - 0.812968 * cr);
  rgb.b = saturate(y + 2.017232 * cb);
  // t3 is always bound: a 1x1 white texture stands in when the guest supplies
  // no alpha plane, which is cheaper than a second PSO variant or an extra
  // root parameter carrying a flag.
  float a = g_alpha.Sample(g_smp, uv).r;
  // AddGameDraw folds the guest Bink shader's c0 modulation into this COLOR
  // seed. The UP/FVF quad itself has no diffuse element, so without that fold
  // `col` would always be white and the guest modulation would be ignored.
  return float4(rgb, a) * col;
}
)";

}  // namespace mx::gfx::shaders
