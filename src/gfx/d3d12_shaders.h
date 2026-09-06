#pragma once

// HLSL source for the pipeline owned by D3D12Renderer.
//   - kGameVS / kGamePS : position+color+uv vertex layout with an MVP constant
//                         buffer, the target for translated guest draw calls.
//
// The kVideoVS / kVideoPS fullscreen-blit pair was REMOVED with the host FFmpeg
// intro player; the guest decodes and draws its own video.

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
// Returning zero is not a colour decision: TranslatedPSO already sets
// RenderTargetWriteMask to 0 for these draws, so nothing this returns can reach
// the target. If this output is ever visible, the gate upstream is wrong, not
// this value.
//
// Declaring only SV_Position is deliberate: a pixel shader's input signature
// must be a SUBSET of the vertex stage's output, and every translated vertex
// stage emits SV_Position plus its interpolators. Naming just the one both
// shapes always carry means this pairs with any of them and needs no variant.
// NATIVE PIXEL SHADERS -- step 5 of the native-renderer plan.
//
// A substitute replaces one NAMED pass, and it is chosen by the pass's asset
// name ("<asset>::<EntryPoint>" from userdata/shader_names.txt), never by a
// shader handle. Handles are guest addresses the runtime recycles onto
// different microcode within a run -- 9,968 of 10,074 compiles -- so keying on
// one both splits a shader across rows and merges several into one.
//
// THE CONTRACT a substitute must satisfy, from TranslatedPSO and the b1 layout
// in d3d12_game_drawlist.cpp:
//
//   * root signature m_translatedRootSig, 7 parameters
//   * the input signature must be a SUBSET of the vertex stage's output, which
//     is SV_Position plus i0..i7 : TEXCOORD. Naming fewer is fine; naming one
//     the vertex stage does not emit will not link.
//   * cbuffer b1 is xe_c[256], xe_texinv[16], xe_texsign[16], xe_param_gen,
//     xe_alphatest, colour scale -- in that order.
//   * textures arrive on t0.. in guest sampler-slot order, samplers on s0..
//
// The registry is EMPTY on purpose. Which pass to write first is a question
// with a measured answer -- the per-pass draw census this rung adds -- and
// picking one before reading it would be choosing by guesswork the way the
// texture stand-in scorer did.
// THE LEGACY MULTIPLY, which every native substitute needs and which is the
// reason "native" does not mean "free of Xenos semantics".
//
// The translator emits XeMul, not `*`. On Xenos a multiply with either operand
// zero or denormal returns exactly +0 rather than propagating INF or NaN --
// D3D9 legacy behaviour -- and this project has already paid for that once:
// it is what made the main menu render black. A substitute written with plain
// `*` is therefore a BEHAVIOUR CHANGE, not a reimplementation, and it diverges
// exactly where the guest relies on the quirk.
//
// It matters here specifically. Bloom multiplies samples from an HDR scene
// target by weights, so an INF sample against a zero weight is reachable:
// plain `*` gives NaN, the guest gives 0, and a NaN in a bloom tap spreads
// across the frame. The first version of these two shaders used `*`.
inline constexpr const char* kNativePrelude = R"(
float XeMul(float a, float b) {
  return (abs(a) < 1.175494351e-38 || abs(b) < 1.175494351e-38) ? 0.0 : a * b;
}
float4 XeMul(float4 a, float4 b) {
  return float4(XeMul(a.x, b.x), XeMul(a.y, b.y),
                XeMul(a.z, b.z), XeMul(a.w, b.w));
}
)";

// The separable bloom blur, both directions. bloom.shader::BlurHPS and
// ::BlurVPS, 5280 draws between them on a level run.
//
// THE GUEST SHADER, from tools/shader_code.py, three ALU and three fetches:
//
//     tfetch2D r1, r0, tf0 [offset=-1.5,0]    (BlurVPS: 0,-1.5)
//     tfetch2D r2, r0, tf0                    centre
//     tfetch2D r0, r0, tf0 [offset=+1.5,0]    (BlurVPS: 0,+1.5)
//     mul  r0, r0, c103
//     mad  r0, r2.xzwy, c102.xzwy, r0.xzwy
//     mad  export0, r1, c101, r0.xwyz
//
// The swizzles look alarming and are a no-op END TO END. As dst->src maps,
// .xzwy is [0,2,3,1] and .xwyz is [0,3,1,2]; composing them gives the
// identity, so the permutation instruction 6 applies is exactly undone by the
// read in instruction 7. Every operand of that mad carries the same swizzle,
// which is what makes it a permutation of the whole expression rather than a
// channel shuffle. So the result is a plain weighted sum:
//
//     tap(-3)*c101 + tap(0)*c102 + tap(+3)*c103
//
// and c101..c103 is exactly g_avSampleWeights, which the asset's constant
// table declares as c101 x3.
//
// WHY THIS IS NOT EQUIVALENT TO WHAT WE RENDER TODAY. The translator does not
// carry tfetch texel offsets -- there is no offset argument anywhere in
// shader_hlsl.cpp's Sample emission -- so all three taps currently read the
// SAME texel and the blur is a weighted copy. This shader is therefore a fix,
// not a reimplementation, and turning it on should visibly change bloom. If it
// does not, the substitution did not reach the GPU.
//
// THE OFFSETS ARE HALF-TEXELS, from the SDK: `offset_x() { return
// data_.offset_x * 0.5f; }` (ucode.h:816). The raw field reads +/-3, so the
// taps are at +/-1.5 TEXELS. The first version of this shader used HLSL's
// integer offset argument with int2(+/-3, 0), which is both the wrong distance
// and a form that cannot express a half texel at all. Applied to the
// coordinate instead, scaled by xe_texinv, exactly as the translator now does.
inline const std::string kNativeBloomBlurH = std::string(kNativePrelude) + R"(
cbuffer XeShaderConstants : register(b1) {
  float4 xe_c[256];
  float4 xe_texinv[16];
  float4 xe_texsign[16];
  uint4  xe_param_gen;
  uint4  xe_alphatest;
  float4 xe_colorscale;
};
Texture2D<float4> xe_tex0 : register(t0);
SamplerState      xe_smp0 : register(s0);
struct XeInterpolants { float4 pos : SV_Position; float4 i0 : TEXCOORD0; };
struct XePsOut { float4 c0 : SV_Target0; };
XePsOut main(XeInterpolants xe_in, bool xe_front : SV_IsFrontFace) {
  float2 uv = xe_in.i0.xy;
  XePsOut o;
  float2 t = xe_texinv[0].xy;
  o.c0 = XeMul(xe_tex0.Sample(xe_smp0, uv + float2(-1.5, 0.0) * t), xe_c[101])
       + XeMul(xe_tex0.Sample(xe_smp0, uv),                          xe_c[102])
       + XeMul(xe_tex0.Sample(xe_smp0, uv + float2( 1.5, 0.0) * t), xe_c[103]);
  // The translator's OUTPUT EPILOGUE, reproduced because it is not part of
  // the guest shader and a substitute that omits it is wrong on exactly the
  // targets that need it most. xe_colorscale.x is 1/32 for the signed
  // fixed-point k_16_16 targets and 1.0 elsewhere; the clamp is gated on .y
  // being non-zero, which the renderer sets only for the guest formats whose
  // range must be enforced. Dropping either would look right on the common
  // path and wrong on a bloom target that happens to be fixed point.
  o.c0 = XeMul(o.c0, xe_colorscale.xxxx);
  if (xe_colorscale.y > 0.0) {
    o.c0.rgb = min(max(o.c0.rgb, 0.0), xe_colorscale.y);
    o.c0.a   = min(max(o.c0.a,   0.0), xe_colorscale.z);
  }
  return o;
}
)";

inline const std::string kNativeBloomBlurV = std::string(kNativePrelude) + R"(
cbuffer XeShaderConstants : register(b1) {
  float4 xe_c[256];
  float4 xe_texinv[16];
  float4 xe_texsign[16];
  uint4  xe_param_gen;
  uint4  xe_alphatest;
  float4 xe_colorscale;
};
Texture2D<float4> xe_tex0 : register(t0);
SamplerState      xe_smp0 : register(s0);
struct XeInterpolants { float4 pos : SV_Position; float4 i0 : TEXCOORD0; };
struct XePsOut { float4 c0 : SV_Target0; };
XePsOut main(XeInterpolants xe_in, bool xe_front : SV_IsFrontFace) {
  float2 uv = xe_in.i0.xy;
  XePsOut o;
  float2 t = xe_texinv[0].xy;
  o.c0 = XeMul(xe_tex0.Sample(xe_smp0, uv + float2(0.0, -1.5) * t), xe_c[101])
       + XeMul(xe_tex0.Sample(xe_smp0, uv),                          xe_c[102])
       + XeMul(xe_tex0.Sample(xe_smp0, uv + float2(0.0,  1.5) * t), xe_c[103]);
  // The translator's OUTPUT EPILOGUE, reproduced because it is not part of
  // the guest shader and a substitute that omits it is wrong on exactly the
  // targets that need it most. xe_colorscale.x is 1/32 for the signed
  // fixed-point k_16_16 targets and 1.0 elsewhere; the clamp is gated on .y
  // being non-zero, which the renderer sets only for the guest formats whose
  // range must be enforced. Dropping either would look right on the common
  // path and wrong on a bloom target that happens to be fixed point.
  o.c0 = XeMul(o.c0, xe_colorscale.xxxx);
  if (xe_colorscale.y > 0.0) {
    o.c0.rgb = min(max(o.c0.rgb, 0.0), xe_colorscale.y);
    o.c0.a   = min(max(o.c0.a,   0.0), xe_colorscale.z);
  }
  return o;
}
)";

inline const std::string* NativePixelShader(const std::string& pass_name) {
  struct Entry { const char* pass; const std::string* hlsl; };
  // Chosen from the NATIVE SHADERS census, not by inspection: bloom is only
  // 3.4% of named draws, but it is three ALU and one sampler with no light
  // buffer, so it proves the mechanism before anything risky rides on it.
  static const Entry kNative[] = {
      {"EngineDependencies/bloom.shader::BlurHPS", &kNativeBloomBlurH},
      {"EngineDependencies/bloom.shader::BlurVPS", &kNativeBloomBlurV},
  };
  for (const Entry& e : kNative)
    if (pass_name == e.pass) return e.hlsl;
  return nullptr;
}

// A deliberately wrong substitute, for the mutation test. A census can report a
// substitution that never reached the GPU; forcing an impossible output and
// confirming the screen changes is what distinguishes "it ran" from "the
// counter moved". This is what d3d9_stencil_force_never established about
// TranslatedPSO, where the counters were vacuous.
inline constexpr const char* kNativeMutantPS = R"(
float4 main(float4 pos : SV_Position) : SV_TARGET { return float4(1, 0, 1, 1); }
)";

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
// green line along the bottom edge. Do not remove that crop without scaling the
// chroma uv here instead.
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
