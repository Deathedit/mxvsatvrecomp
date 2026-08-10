#include "app/graphics_system.h"

#include <SDL3/SDL_init.h>
#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_gamepad.h>
#include <rex/cvar.h>
#include <rex/logging.h>

#include <chrono>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include <windows.h>
#include <renderdoc/renderdoc_app.h>

#include "gpu/hle_types.h"
#include "gpu/d3d9_layout.h"
#include "hooks/native_bridge.h"

// The one vertex stride the game PSO's input layout actually describes —
// POSITION float3 at offset 0, COLOR float4 at offset 12, TEXCOORD float2 at
// offset 28, so 36 bytes. See the gate in RenderThreadFunc.
static constexpr uint32_t kSupportedStride = mx::hle::kHostVertexStride;

// hide_colorless_draws and hide_colored_draws were two halves of one
// diagnostic, splitting the frame by colour source so a screenshot could show
// the scene without the colourless overpaint, or the overpaint on its own. Both
// are gone (2026-08-07): every draw is submitted regardless of colour source.
//
// What retired them: they hid a defect rather than fixing it. Draws with no
// colour attribute are written opaque white and paint *over* textured draws
// instead of substituting for missing ones, so `hide_colorless_draws=true` was
// passed on every run and the shipped default was the untested configuration.
// Keeping the filter would have made that permanent.
//
// The overpaint is NOT a transform bug, though this comment claimed it was
// until the claim was measured (46404e1). Colourless draws are 4-vertex
// fullscreen quads with a raw NDC extent of 2.01 and none outside the cube —
// geometrically exact, not "small geometry smeared across the viewport". 91.5%
// of them sample a render target: they are compositor passes whose colour was
// always going to come from that target, and they paint white because the
// target is empty. The fix is the resolve snapshot, not anything here.

// The D3D9 -> D3D12 path describes every draw from the API calls that produced
// it, rather than reconstructing it from the PM4 ring.
//
// hle_render and pm4_translate were the cvars that selected between the two
// sources. Both are gone with the translator (2026-08-06): the D3D9 description
// is the only source of draws, so there is nothing to select and no fallback.
//
// What retired them: with pm4_translate=false the HLE path held 87.66-87.67%
// applied and 309.3 verts/draw against PM4-on's 87.71%/308.9, and the one
// dependency that gate missed — the pixel shader CF offset — was replaced by
// the field the guest's own emitter reads (sub_82565928), which decodes more
// shaders than the ring ever did (40/49 against 37/61, 0 ambiguous).

// Capture only: no PSOs, no uploads, no shader translation, nothing submitted.
// It gates the per-draw scoring, the coverage report and the dump — including
// every `hle-render` and `stageF` line, which is why those are absent from a
// run that passes hle_shader_exec without this.
REXCVAR_DEFINE_BOOL(hle_capture, false, "Debug",
                    "Score every D3D9 draw against the state shadow and report "
                    "what fraction is fully described, plus the first few "
                    "resolved draws to d3d9_dump_decls.txt. Capture only — it "
                    "submits nothing and renders nothing");

// Samples the guest's own vertex shader microcode, from the D3D9 side, on the
// vertices of the draws hle_capture describes. hle_render independently runs
// every referenced vertex and renders it. hle_shader_exec only controls the
// sampled measurement against the clip volume and its report.
//
// A divisor rather than a bool because the interpreter's cost is the open
// question and a fixed sampling rate cannot answer it: at 64 the measurement is
// a sliver of one draw, at 1 it is what using the thing actually costs. 0 is
// off, and off is the default — this changes nothing that is submitted.
REXCVAR_DEFINE_UINT32(hle_shader_exec, 0, "Debug",
                      "Execute the bound guest vertex shader for one D3D9 draw "
                      "in N (0 = off, 1 = every draw) and report where the "
                      "exported positions land. Requires hle_capture. Capture "
                      "only — it renders nothing");

// A cvar rather than the constant it replaces for the same reason as the one
// above: the cost scales with it, so the run matrix has to vary it without a
// rebuild, or the four configurations are four different binaries and the
// timings are not comparable.
REXCVAR_DEFINE_UINT32(hle_shader_verts, 8, "Debug",
                      "How many vertices of each executed draw to run the "
                      "guest vertex shader on. Only has effect when "
                      "hle_shader_exec is non-zero");

// The GPU vertex path is a replacement for the CPU interpreter, not an
// addition to it, so the only honest way to judge it is the same scene with and
// without. Both configurations have to be ONE binary or the comparison is
// between two builds rather than between two paths.
REXCVAR_DEFINE_BOOL(hle_gpu_vertex, true, "Debug",
                    "Run the guest's own vertex shader on the GPU for draws "
                    "whose vertex AND pixel shaders both translate. Off keeps "
                    "every draw on the CPU interpreter");

// The vertex FETCH, as opposed to the vertex shader. With hle_gpu_vertex alone
// the shader runs on the GPU but the CPU still unpacks every attribute of every
// vertex into input registers -- measured at 145ms of a 159ms frame over
// 289,000 vertices. This makes the shader read the guest's raw vertex buffer
// and decode it itself.
//
// Default on, because it is strictly an accelerated form of the same path: a
// draw it refuses falls back to hle_gpu_vertex rather than failing. Off is the
// A/B, and the two must produce the same picture.
REXCVAR_DEFINE_BOOL(hle_gpu_vertex_fetch, true, "Debug",
                    "Let the translated vertex shader fetch and decode the "
                    "guest vertex buffer itself, instead of the CPU unpacking "
                    "attributes per vertex. Requires hle_gpu_vertex");

// TEX_FORMAT_COMP / GPUSIGN. Off leaves every xe_texsign at 1.0, which is the
// exact behaviour of every build before this one, so a suspected regression is
// one run to bisect rather than a rebuild -- the same reason hle_gpu_vertex has
// a switch. Worth having because this change and the D3D9-legacy-multiply
// change landed back to back and both touch every translated pixel shader.
REXCVAR_DEFINE_BOOL(hle_texture_signs, true, "Debug",
                    "Apply the fetch constant's kUnsignedBiased texture sign "
                    "(2*c-1) in the pixel shader");

// HLE does not yet create a host target for every guest render target. Until
// it does, mixing the 129x129 shadow pass and other off-screen viewports into
// the 1280x720 scene produces the long white wedges seen in ST_Southwest.
// This selector comes from D3D9's resolved, render-target-clamped viewport.
REXCVAR_DEFINE_BOOL(hle_main_viewport_only, false, "Debug",
                    "In HLE rendering, submit only draws using the resolved "
                    "1280x720 D3D9 viewport. Diagnostic fallback now that "
                    "separate render targets are modelled");


// Trigger a RenderDoc capture at a chosen presented frame.
//
// Captures were hand-timed before this: launch under RenderDoc, wait for
// --force_load to fire at ~115s, and press F12 at the right moment. That makes
// an A/B depend on hitting the same frame twice by hand, and the frame number
// is exactly what has to match for two captures to be comparable.
//
// Does nothing unless the process is running under RenderDoc, since the module
// handle is only present when its layer is loaded.
REXCVAR_DEFINE_UINT32(rdoc_capture_frame, 0, "Debug",
                      "Trigger a RenderDoc capture at this presented frame "
                      "number (0 = off). Only has effect when running under "
                      "RenderDoc");

namespace {
RENDERDOC_API_1_0_0* RenderDocApi() {
  static RENDERDOC_API_1_0_0* api = [] () -> RENDERDOC_API_1_0_0* {
    HMODULE module = GetModuleHandleA("renderdoc.dll");
    if (!module) return nullptr;
    auto get_api =
        reinterpret_cast<pRENDERDOC_GetAPI>(GetProcAddress(module, "RENDERDOC_GetAPI"));
    RENDERDOC_API_1_0_0* out = nullptr;
    if (get_api && get_api(eRENDERDOC_API_Version_1_0_0,
                           reinterpret_cast<void**>(&out)) == 1)
      return out;
    return nullptr;
  }();
  return api;
}

// Counts presented frames and fires once. TriggerCapture records the *next*
// frame to begin, so the capture lands on the frame after the trigger; the
// number that matters is that both sides of an A/B use the same cvar value.
void MaybeTriggerRenderDocCapture() {
  const uint32_t want = REXCVAR_GET(rdoc_capture_frame);
  if (!want) return;
  static uint64_t s_presented = 0;
  static bool s_fired = false;
  ++s_presented;
  if (s_fired || s_presented < want) return;
  RENDERDOC_API_1_0_0* api = RenderDocApi();
  if (!api) {
    static bool s_warned = false;
    if (!s_warned) {
      s_warned = true;
      REXLOG_INFO("rdoc: capture requested at frame {} but RenderDoc is not "
                  "attached — launch under renderdoccmd or the UI",
                  want);
    }
    return;
  }
  s_fired = true;
  api->TriggerCapture();
  REXLOG_INFO("rdoc: triggered capture at presented frame {}", s_presented);
}
}  // namespace

namespace rex {
namespace system {

bool D3D12GraphicsSystem::InitializeRenderer(HWND hwnd) {
  m_hwnd = hwnd;
  m_renderer = std::make_unique<D3D12Renderer>();
  if (!m_renderer->Initialize(hwnd)) {
    m_renderer.reset();
    return false;
  }
  mx::native::SetRenderer(m_renderer.get());
  m_running = true;
  m_renderThread = std::thread([this]() { RenderThreadFunc(); });
  return true;
}

void D3D12GraphicsSystem::Shutdown() {
  m_running = false;
  if (m_renderThread.joinable()) m_renderThread.join();
  // Drop the bridge's non-owning alias before the renderer dies.
  mx::native::NativeGraphics::Get().Shutdown();
  m_renderer.reset();
  m_initialized = false;
}

void D3D12GraphicsSystem::RenderThreadFunc() {
  using namespace mx;
  // The host FFmpeg intro player was REMOVED 2026-08-06. The guest opens and
  // decodes its own Bink videos natively — proven by hooking BinkOpen, and
  // visible as the intro playing twice while both existed. Nothing here needs
  // to own the swapchain before the guest starts.
  while (m_running) {
    if (m_renderer) {
      // Hand any PM4 geometry the VdSwap hook translated this frame to the game
      // pipeline. GetDrawCalls moves-and-clears, so a tick with no VdSwap since
      // the last one gets an empty list — see the note below on why that must
      // re-present the previous frame rather than clear.
      //
      // DrawCall::mvp is the window-space -> NDC transform Pm4Translator built
      // from the guest's PA_CL_VPORT_* registers. It used to be the ALU
      // constant block, which this game never writes — so it was identically
      // zero and collapsed every vertex to the origin.
      auto draws = native::NativeGraphics::Get().GetDrawCalls();

      // This loop ticks on a fixed 16ms sleep, and the guest swaps at its own
      // rate, so most iterations find nothing new. Such a tick must re-present
      // the last frame we did get, not a cleared screen: GetDrawCalls
      // moves-and-clears, so an unconditional ClearGameDraws here threw away
      // the only geometry we had every time the two rates disagreed. Combined
      // with the placeholder-triangle fallback that used to sit in
      // RenderGameFrame, that made the post-load screen alternate between guest
      // geometry and the placeholder — invisible while the render target
      // accumulated, a visible flash once the clear was fixed. The placeholder
      // is gone; the re-present rule below is not, and still carries the frame.
      static uint64_t s_ticksWithDraws = 0, s_ticksEmpty = 0;
      if (draws.empty()) ++s_ticksEmpty; else ++s_ticksWithDraws;

      uint32_t submitted = 0, skipped = 0;
      // kSupportedStride (36) is the input layout the game PSO declares —
      // POSITION float3 @0, COLOR float4 @12, TEXCOORD float2 @28 — and
      // anything else would be reinterpreted as that layout and drawn as noise,
      // so it is still counted rather than drawn.
      //
      // What changed is what reaches here: the translator now transcodes guest
      // vertices into this layout using the shader's own declared formats, so a
      // draw arriving at stride 28 is one that was *converted* to it, not one
      // that happened to be it. A draw still arriving at some other stride is
      // one the transcode could not handle — no shader bound, no identifiable
      // position attribute, or a vertex format not implemented — and the
      // histogram below is now a list of those gaps rather than of the guest's
      // strides.
      static std::map<uint32_t, uint32_t> s_skippedStrides;
      static std::map<uint64_t, uint32_t> s_skippedViewports;
      static uint64_t s_skippedUntransformable = 0;
      // Filter first, bind second. The renderer's list is only replaced once we
      // know the new frame has something in it — a frame whose draws were all
      // skipped for stride would otherwise blank the screen just as surely as a
      // frame that never arrived.
      std::vector<const mx::hle::DrawCall*> submittable;
      for (const auto& d : draws) {
        // A resolve is not a draw. It has no geometry, so every gate below
        // would reject it — the first one silently, without even counting it —
        // and a dropped resolve leaves a stale snapshot, which looks like a
        // partial fix rather than a failure. It keeps its slot in
        // `submittable` because the snapshot must be taken at this point in the
        // draw order, not at the end of the frame.
        if (d.resolve_dest_texture) {
          submittable.push_back(&d);
          continue;
        }
        // A draw whose vertex shader fetches for itself carries NO host
        // vertices and no stride — the 36-byte transcode that used to produce
        // them is the CPU pass that path exists to remove. Its geometry is in
        // raw_vertex_bytes, so the two gates below have to let it through or
        // the saving turns into a blank frame.
        const bool fetch_draw =
            d.vertex_shader_hlsl && !d.raw_vertex_bytes.empty() &&
            d.raw_fetch_count && !d.vertex_constants.empty();
        // vertices are only populated when the translator resolved a vertex
        // fetch constant; index-only draws have nothing to bind.
        if (!d.valid || (!fetch_draw && d.vertices.empty()) ||
            d.index_count == 0)
          continue;
        if (d.topology == mx::hle::HostTopology::kUndefined) {
          ++skipped;
          continue;
        }
        // Set only when skip_untransformable_draws is on. A MITIGATION: the
        // draw is still transformed wrongly, this only stops it being painted
        // over the draws that come out right. Counted separately from the
        // other skip reasons so a screenshot can be read honestly against it.
        if (d.untransformable) {
          ++skipped;
          ++s_skippedUntransformable;
          continue;
        }
        if (!fetch_draw && d.vertex_stride != kSupportedStride) {
          ++skipped;
          ++s_skippedStrides[d.vertex_stride];
          continue;
        }
        // A frame touches ~16 distinct guest colour surfaces (measured) and we
        // have one host render target, so without a filter every off-screen
        // pass overpaints the main scene and whichever runs last decides what
        // is on screen. That is what made the window cycle through colours.
        //
        // The surface filter this used to run was main_surface_only, keyed on
        // RB_SURFACE_INFO — a PM4 register the D3D9 path never sees, so it is
        // gone with the translator. The viewport extent is the stand-in until
        // render targets are modelled from D3D9 state; it is a weaker signal
        // and it is off by default, because dropping draws on a guess would be
        // indistinguishable from "HLE produced nothing".
        if (REXCVAR_GET(hle_main_viewport_only) &&
            (d.viewport_width != 1280 || d.viewport_height != 720)) {
          ++skipped;
          ++s_skippedViewports[(uint64_t(d.viewport_width) << 32) |
                               d.viewport_height];
          continue;
        }
        submittable.push_back(&d);
        ++submitted;
      }

      // A non-empty handoff is a real guest frame even when every draw is
      // filtered, so the previous frame is retired here rather than inside the
      // submittable check below. Otherwise a frame whose draws were all skipped
      // — for stride, or for the viewport gate — leaves the host replaying the
      // previous frame's draw list while the guest swaps, which reads as a
      // frozen picture rather than as an empty one.
      if (!draws.empty()) {
        m_renderer->ClearGameDraws();
      }
      if (!submittable.empty()) {
        for (const auto* d : submittable) {
          if (d->resolve_dest_texture) {
            m_renderer->AddGameResolve(
                d->resolve_dest_texture, d->resolve_source_object,
                d->resolve_dest_x, d->resolve_dest_y, d->resolve_src_x1,
                d->resolve_src_y1, d->resolve_src_x2, d->resolve_src_y2,
                d->resolve_dest_width, d->resolve_dest_height,
                d->resolve_source_is_depth, d->resolve_source_base,
                d->resolve_source_width, d->resolve_source_height);
            continue;
          }
          // The guest vertex stage, when the hooks built one for this draw.
          // Pass-through only: every decision about whether a draw qualifies
          // was made where the microcode and the attributes are.
          D3D12Renderer::GpuVertexStage vertexStage;
          if (d->vertex_shader_hlsl && !d->raw_vertex_bytes.empty() &&
              d->raw_fetch_count && !d->vertex_constants.empty()) {
            // The fetch form. No inputs and no regs by construction — the
            // shader reads the raw buffer itself.
            vertexStage.handle = d->vertex_shader_handle;
            vertexStage.hlsl = d->vertex_shader_hlsl;
            vertexStage.constants = d->vertex_constants.data();
            vertexStage.constDwords =
                static_cast<uint32_t>(d->vertex_constants.size());
            vertexStage.rawBytes = d->raw_vertex_bytes.data();
            vertexStage.rawByteCount =
                static_cast<uint32_t>(d->raw_vertex_bytes.size());
            vertexStage.rawFetch =
                reinterpret_cast<const uint32_t*>(d->raw_fetch.data());
            vertexStage.rawFetchCount = d->raw_fetch_count;
          } else if (d->vertex_shader_hlsl && !d->vertex_inputs.empty() &&
                     d->vertex_input_count && !d->vertex_constants.empty()) {
            vertexStage.handle = d->vertex_shader_handle;
            vertexStage.hlsl = d->vertex_shader_hlsl;
            vertexStage.inputs = d->vertex_inputs.data();
            vertexStage.inputBytes =
                static_cast<uint32_t>(d->vertex_inputs.size());
            vertexStage.regs = d->vertex_input_regs.data();
            vertexStage.regCount = d->vertex_input_count;
            vertexStage.constants = d->vertex_constants.data();
            vertexStage.constDwords =
                static_cast<uint32_t>(d->vertex_constants.size());
          }
          m_renderer->AddGameDraw(d->vertices.data(),
                                  static_cast<uint32_t>(d->vertices.size()),
                                  d->vertex_stride, d->indices.data(),
                                  static_cast<uint32_t>(d->indices.size()),
                                  d->index_16bit, d->index_count, d->mvp,
                                  static_cast<uint32_t>(d->topology),
                                  (d->om_seen & (1u << 1)) != 0 &&
                                      (d->depth_control & (1u << 1)) != 0,
                                  (d->om_seen & (1u << 1)) != 0 &&
                                      (d->depth_control & (1u << 2)) != 0,
                                  (d->om_seen & (1u << 0)) == 0 ||
                                      (d->colour_mask & 0xFu) != 0,
                                  d->texture, d->render_target_object,
                                  d->render_target_width,
                                  d->render_target_height,
                                  d->sampled_render_target_object,
                                  d->sampled_texture_object,
                                  d->planes.data(), d->plane_count,
                                  d->yuv_has_alpha,
                                  // Only blend when the guest said so — a draw
                                  // that never set ALPHABLENDENABLE keeps the
                                  // opaque path it has always had.
                                  (d->om_seen & (1u << 2)) != 0 &&
                                      d->blend_enable != 0,
                                  d->src_blend, d->dest_blend, d->blend_op,
                                  // No COLOR element in the declaration, so the
                                  // {1,1,1,1} in the vertex buffer is a seed,
                                  // not data. Harmless when a texture
                                  // modulates it; opaque white when nothing
                                  // does.
                                  static_cast<uint8_t>(d->color_source),
                                  // Xenos address modes: 0 repeat and 1
                                  // mirrored repeat wrap, everything above
                                  // clamps. Mirroring is not modelled, so a
                                  // mirrored mode takes the nearer of the two.
                                  (d->clamp_x >= 2 ? 1u : 0u) |
                                      (d->clamp_y >= 2 ? 2u : 0u),
                                  d->pixel_shader_handle,
                                  d->pixel_shader_hlsl,
                                  d->interpolators.data(),
                                  static_cast<uint32_t>(
                                      d->interpolators.size()),
                                  d->pixel_constants.data(),
                                  static_cast<uint32_t>(
                                      d->pixel_constants.size()),
                                  d->pixel_sampler_count,
                                  d->pixel_textures.data(),
                                  d->pixel_sampled_objects.data(),
                                  vertexStage.handle ? &vertexStage : nullptr,
                                  d->pixel_sampler_array_mask,
                                  d->pixel_sampler_signs.data(),
                                  d->depth_target_object,
                                  d->depth_target_width,
                                  d->depth_target_height,
                                  d->depth_target_base, d->surface_base,
                                  (d->render_target_color_info >> 16) & 0xFu);
          static bool s_loggedFirst = false;
          if (!s_loggedFirst) {
            s_loggedFirst = true;
            REXLOG_INFO("RenderThread: first translated draw — {} verts ({} B, stride {}), {} indices, topology {}",
                        d->vertex_count, d->vertices.size(), d->vertex_stride,
                        d->index_count, static_cast<uint32_t>(d->topology));
            REXLOG_INFO("RenderThread: first translated draw target 0x{:08X} "
                        "{}x{} samples resolved target 0x{:08X}",
                        d->render_target_object, d->render_target_width,
                        d->render_target_height,
                        d->sampled_render_target_object);
          }
        }
      }
      static uint32_t s_frame = 0;
      if ((submitted || skipped) && (++s_frame % 100) == 1) {
        std::string hist;
        for (const auto& [stride, count] : s_skippedStrides)
          hist += fmt::format("{}:{} ", stride, count);
        std::string viewports;
        for (const auto& [key, count] : s_skippedViewports)
          viewports += fmt::format("{}x{}:{} ", uint32_t(key >> 32),
                                   uint32_t(key & 0xFFFFFFFF), count);
        REXLOG_INFO("RenderThread: frame #{} submitted {} draws, skipped {} "
                    "— skipped strides (cumulative) {} — host ticks with/without "
                    "new draws {}/{} — skipped viewports {} — skipped "
                    "untransformable (cumulative) {}",
                    s_frame, submitted, skipped, hist.empty() ? "none" : hist,
                    s_ticksWithDraws, s_ticksEmpty,
                    viewports.empty() ? "none" : viewports,
                    s_skippedUntransformable);
      }
      // BeginFrame and EndFrame own the whole frame: BeginFrame opens the
      // command list, transitions and clears the targets and then calls
      // RenderGameFrame itself; EndFrame calls
      // PresentGameFrame and then swaps. This loop used to call
      // RenderGameFrame and PresentGameFrame again in between, which was not a
      // harmless repeat — PresentGameFrame's barriers are directional. Its
      // first call leaves m_gameRT in PIXEL_SHADER_RESOURCE, so EndFrame's
      // call then declared StateBefore = RENDER_TARGET for a resource that was
      // not in it, an invalid transition, on top of drawing and copying the
      // whole frame twice.
      m_renderer->BeginFrame();
      MaybeTriggerRenderDocCapture();
      m_renderer->EndFrame();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
  }
}

}  // namespace system
}  // namespace rex
