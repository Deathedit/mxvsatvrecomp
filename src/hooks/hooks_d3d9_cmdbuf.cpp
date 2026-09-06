// Command-buffer record and replay -- the fourth unhooked submission path.
//
// Split verbatim out of hooks_d3d9.cpp. The guest records a buffer once and
// executes it per instance, so those draws reach the ring as PM4 and touch no
// D3D9 draw entry point; replaying them here is what puts them back in our
// pipeline. The packet DECODE for the same buffers lives in hooks_d3d9_pm4.cpp.
//
// Priced by measuring both directions across the seam:
//
//   420 lines out
//   7 names defined here are used elsewhere -- ALL SEVEN already published
//   2 names used here are defined elsewhere -- the two in hooks_d3d9_cmdbuf.h
//
// FinishHleDraw stays behind deliberately, though it sat directly below this
// block. It is draw submission, not replay: hooks_d3d9_resolve.cpp,
// hooks_frame.cpp and the entry points all call it, and taking it would have
// dragged NoteShaderlessDraw and the accept/refuse counters across too --
// 4 imports instead of 2.

#include "hooks/hook_common.h"

#include <rex/graphics/format/ucode.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "gpu/d3d9_draw.h"
#include "gpu/d3d9_state.h"
#include "gpu/hle_types.h"
#include "gpu/shader_ucode.h"
#include "gpu/xenos_gpu_state.h"
#include "hooks/hooks_d3d9_cmdbuf.h"
#include "hooks/hooks_d3d9_shared.h"

namespace mx::hooks::d3d9 {

//===========================================================================
// COMMAND-BUFFER RECORD AND REPLAY -- the fourth unhooked submission path.
//
// sub_823F82D0 renders vegetation record-once / replay-per-instance:
//
//     if (!latch) {
//         cmdbuf = sub_8255DDF0(10240, 0);
//         sub_8255D850(0, 2, 0, 0, 0, &dev);  // a RECORDING device
//         *(globals + 22428) = dev;           // installed as current
//         sub_8255E9A0(dev, cmdbuf, 16, ...); // begin recording
//         sub_823F6960(...);                  // the 3D tree draw, ONCE
//         sub_825601B8(dev);                  // end recording
//     }
//     for (each instance) {
//         sub_82550208(real_dev, 0, 64, &a, &b, 4);
//         memcpy(a, matrix, 64); memcpy(b, matrix, 64);
//         sub_825605D8(real_dev, cmdbuf, 0);  // REPLAY
//     }
//
// On hardware the recorded draw does not execute; every visible instance comes
// from the replay. We capture at RECORD time rather than interpreting the PM4,
// because the recorded stream is register level while our translation path is
// D3D9 level and the guest makes all of those D3D9 calls on the recording
// device. The per-instance transform is written to the REAL device's constant
// bank between replays, and sub_825605D8's r3 IS that device, so re-reading
// device+0x780 at replay time is what separates the instances.
//===========================================================================
namespace {

struct CmdBufRecording {
  std::vector<mx::hle::DrawCall> draws;
  uint64_t replays = 0;
};

// Keyed by command buffer, because the recording DEVICE is destroyed
// (sub_8255D3A8) as soon as recording ends and cannot key anything that has to
// outlive the recording. Both are guest addresses and are valid only within a
// run -- see the shader-handle rule.
std::map<uint32_t, CmdBufRecording> g_cmdBufRec;
std::map<uint32_t, uint32_t> g_recDevice;   // recording device -> cmdbuf
std::recursive_mutex g_cmdBufRecMu;

// A recorded model is small: the palm is 2 draws and the largest buffer
// measured is 27. The cap stops a runaway recording growing without bound, and
// its overflow is COUNTED rather than silently dropped.
constexpr size_t kMaxRecordedDraws = 256;

uint64_t g_cmdBufCaptured = 0;      // draws captured at record time
uint64_t g_cmdBufCapOverflow = 0;   // draws past kMaxRecordedDraws
uint64_t g_cmdBufDeferred = 0;      // recorded draws that took the pending path
uint64_t g_cmdBufReplayed = 0;      // draws re-issued at replay time
uint64_t g_cmdBufReplayRefused = 0; // re-issued draws FinishHleDraw rejected
uint64_t g_cmdBufReplayNoRec = 0;   // replays of a buffer holding no capture
uint64_t g_cmdBufConstRecorded = 0; // replays with no patch in the buffer
uint64_t g_cmdBufConstPatched = 0;  // constants overlaid per instance
uint64_t g_cmdBufConstLiveBase = 0; // replays based on the live bank
uint64_t g_cmdBufConstNoLive = 0;   // replays that could not read it
uint64_t g_cmdBufConstUnpaired = 0; // draws with no matching overlay
uint64_t g_cmdBufPixelLiveBase = 0; // pixel banks refreshed live
uint64_t g_cmdBufPixelNoLive = 0;   // pixel banks left at record time
uint64_t g_cmdBufLightSlotFixed = 0;      // 1x1 slots rebound to a snapshot
uint64_t g_cmdBufLightSlotNoSnapshot = 0; // 1x1 slots with no live snapshot
constexpr uint32_t kXeSqProgramCntl = 0x2180;
constexpr uint32_t kXeSqContextMisc = 0x2181;
uint64_t g_cmdBufTexFromBuffer = 0;      // slots bound from the buffer
uint64_t g_cmdBufTexBufferShort = 0;     // buffer described some, filled fewer
uint64_t g_cmdBufTexNoBufferBinding = 0; // buffer described no slot



}  // namespace

void BeginCmdBufRecording(uint32_t device, uint32_t cmdbuf) {
  if (!device || !cmdbuf) return;
  std::lock_guard<std::recursive_mutex> lk(g_cmdBufRecMu);
  g_recDevice[device] = cmdbuf;
  // A re-record replaces the previous content. The guest re-records only when
  // its own latch says the recording is stale, so keeping the old draws would
  // replay geometry it has already discarded.
  g_cmdBufRec[cmdbuf].draws.clear();
}

void EndCmdBufRecording(uint32_t device) {
  std::lock_guard<std::recursive_mutex> lk(g_cmdBufRecMu);
  g_recDevice.erase(device);
}

uint32_t CmdBufForDevice(uint32_t device) {
  if (!device) return 0;
  std::lock_guard<std::recursive_mutex> lk(g_cmdBufRecMu);
  auto it = g_recDevice.find(device);
  return it == g_recDevice.end() ? 0u : it->second;
}

// True means the draw belonged to a recording and must NOT be issued now.
// False means "not recording" and the caller submits it normally.
bool CaptureDrawIfRecording(uint32_t device, mx::hle::DrawCall& dc) {
  std::lock_guard<std::recursive_mutex> lk(g_cmdBufRecMu);
  auto it = g_recDevice.find(device);
  if (it == g_recDevice.end()) return false;
  auto& rec = g_cmdBufRec[it->second];
  if (rec.draws.size() >= kMaxRecordedDraws) {
    ++g_cmdBufCapOverflow;
    return true;
  }
  rec.draws.push_back(dc);
  ++g_cmdBufCaptured;
  return true;
}

void NoteCmdBufDeferredDraw() { ++g_cmdBufDeferred; }


uint32_t ReplayCmdBuf(
    uint32_t cmdbuf, uint32_t device, uint8_t* base,
    const std::vector<std::vector<CmdBufConstOverlay>>& ov) {
  std::vector<mx::hle::DrawCall> copies;
  {
    std::lock_guard<std::recursive_mutex> lk(g_cmdBufRecMu);
    auto it = g_cmdBufRec.find(cmdbuf);
    if (it == g_cmdBufRec.end() || it->second.draws.empty()) {
      ++g_cmdBufReplayNoRec;
      return 0;
    }
    ++it->second.replays;
    copies = it->second.draws;
  }
  // THE RECORDED TARGET IS THE IMPOSTOR, NOT THE SCENE. sub_823F82D0 retargets
  // to an off-screen surface before recording and patches the real target into
  // the buffer before each replay, so a captured DrawCall carries the record-time
  // surface. DeviceState is thread-local and the replay runs on the guest's own
  // render thread, so by here it holds the LIVE target.
  const auto& live_st = DeviceState();
  // Thread-local FIRST -- when the replay does run on the binding thread that
  // is the freshest answer -- then the per-device mirror, which is the only
  // one that survives a replay on another thread.
  mx::hle::RenderTargetBinding live_rt = live_st.render_target[0];
  bool rt_from_device = false;
  if (!live_rt.valid && RenderTargetForDevice(device, live_rt, false))
    rt_from_device = true;
  mx::hle::RenderTargetBinding live_depth = live_st.depth_stencil;
  if (!live_depth.valid) RenderTargetForDevice(device, live_depth, true);
  float replay_vp[16];
  uint32_t replay_vp_w = 0, replay_vp_h = 0;
  const bool have_replay_vp =
      BuildViewportMvp(device, base, replay_vp, &replay_vp_w, &replay_vp_h);

  uint32_t issued = 0;
  for (auto& dc : copies) {
    if (live_rt.valid) {

      static uint64_t s_rtShown = 0;
      if (s_rtShown < 4 && live_rt.object != dc.render_target_object) {
        ++s_rtShown;
        REXLOG_INFO("d3d9: CMDBUF REPLAY cb 0x{:08X} retarget: recorded "
                    "0x{:08X} {}x{} -> live 0x{:08X} {}x{}",
                    cmdbuf, dc.render_target_object, dc.render_target_width,
                    dc.render_target_height, live_rt.object, live_rt.width,
                    live_rt.height);
      }
      dc.render_target_object = live_rt.object;
      dc.render_target_surface_info = live_rt.surface_info;
      dc.render_target_color_info = live_rt.color_info;
      dc.render_target_width = live_rt.width;
      dc.render_target_height = live_rt.height;
      dc.surface_base = live_rt.color_info & 0xFFFu;
      dc.surface_pitch = live_rt.surface_info & 0x3FFFu;
    }
    if (live_depth.valid) {
      dc.depth_target_object = live_depth.object;
      dc.depth_target_width = live_depth.width;
      dc.depth_target_height = live_depth.height;
    }
    if (have_replay_vp) {
      std::copy_n(replay_vp, 16, dc.mvp);
      dc.viewport_width = replay_vp_w;
      dc.viewport_height = replay_vp_h;
    }
    // THE CONSTANT BANK: LIVE STATE AS THE BASE, RECORDED WRITES ON TOP, which
    // is what the console does. Three earlier versions each got half of it:
    //
    //   live base, no overlay   -> lost the model's own constants: shards
    //   recorded base, none     -> lost the CAMERA; every vertex came out (0,0,0)
    //   recorded base, flat     -> draw 1 got draw 27's constants: shards
    //
    // The per-instance matrix needs no special case: sub_82550208 writes it to
    // device + 0x780 + reg*16, which IS this live bank.
    {
      std::array<uint32_t, kD3d9ConstRegs * 4> live;
      if (CaptureVertexConstants(device, base, dc.vertex_shader_handle, live)) {
        dc.vertex_constants.assign(live.begin(), live.end());
        ++g_cmdBufConstLiveBase;
      } else {
        // Recorded bank stands. Counted, because a draw transformed by a stale
        // camera is a different failure from one transformed by none.
        ++g_cmdBufConstNoLive;
      }
      // THE PIXEL BANK IS LIVE TOO. With the recorded base the palm's pixel
      // constants are xe_c[0..253] ALL ZERO and the recorded buffer carries no
      // pixel-constant packets at all, so they exist only on the real device --
      // exactly like the camera on the vertex side.
      //
      // KNOWN CONSEQUENCE: the rider and bike are replayed draws too, so they now
      // read live pixel constants and the game's dirt effect switches on. The
      // dirt is a real feature, so the value itself is wrong somewhere upstream.
      if (!dc.pixel_constants.empty()) {
        constexpr uint32_t kPixelConstBase = 0x1780;
        const uint32_t bytes = uint32_t(dc.pixel_constants.size()) * 4;
        if (device && HostPageReadable(REX_RAW_ADDR(device + kPixelConstBase)) &&
            HostPageReadable(
                REX_RAW_ADDR(device + kPixelConstBase + bytes - 4))) {
          // WHICH REGISTERS ACTUALLY CHANGE, for the draw that carries the dirt.
          // 1753 is the rider, by index count. Printed once per register so the
          // list is the answer, not a sample.
          const bool trace = dc.index_count == 1753;
          for (size_t i = 0; i < dc.pixel_constants.size(); ++i) {
            const uint32_t live =
                REX_LOAD_U32(device + kPixelConstBase + uint32_t(i) * 4);
            if (trace && live != dc.pixel_constants[i]) {
              static std::set<uint32_t> s_shown;
              const uint32_t reg = uint32_t(i) / 4;
              if (s_shown.size() < 24 && s_shown.insert(reg).second) {
                REXLOG_INFO("d3d9: RIDER PS CONST c{} differs -- recorded "
                            "0x{:08X} live 0x{:08X}",
                            reg, dc.pixel_constants[i], live);
              }
            }
            dc.pixel_constants[i] = live;
          }
          // THE RAW READ IS ONLY HALF THE BANK, exactly as at capture. Six NaNs
          // were measured in the live pixel file for the rider draw, and a NaN
          // through that shader is the speckling that looked like dirt. The
          // capture path repairs this and the replay skipped it;
          // CaptureVertexConstants does the vertex equivalent internally.
          ApplyPixelShaderLoadTable(dc.pixel_shader_handle, device, base,
                                    dc.pixel_constants);
          ++g_cmdBufPixelLiveBase;
        } else {
          ++g_cmdBufPixelNoLive;
        }
      }
      // THE TEXTURES COME FROM THE BUFFER, not from either device -- the live
      // device puts a rock texture on the rider, the recording device binds a
      // bush atlas to the palm's leaf draw. The recorded buffer writes its own
      // texture fetch constants and that is what the console's replay binds
      // from. Only slots the buffer describes are re-bound.
      if (dc.pixel_shader_handle && issued < ov.size()) {
        const TranslatedShader* t =
            TranslatedPixelShader(dc.pixel_shader_handle);
        if (t && t->sampler_count) {
          const std::vector<uint32_t>* by_sampler[mx::hle::kMaxSamplers] = {};
          for (const auto& blk : ov[issued]) {
            if (blk.is_fetch && blk.first_const < mx::hle::kMaxSamplers &&
                blk.dwords.size() >= 6)
              by_sampler[blk.first_const] = &blk.dwords;
          }
          mx::hle::DrawCall probe = dc;
          uint32_t described = 0, filled = 0;
          for (uint32_t k = 0; k < t->sampler_count &&
                               k < mx::hle::DrawCall::kMaxPixelTextures;
               ++k) {
            const uint32_t gs = t->slot_guest[k];
            const std::vector<uint32_t>* f =
                gs < mx::hle::kMaxSamplers ? by_sampler[gs] : nullptr;
            if (!f) continue;
            ++described;
            if (ResolvePixelSlotTexture(probe, k, gs, device, base,
                                        /*vertex=*/false,
                                        /*stage_handle_hint=*/0, f->data()))
              ++filled;
          }
          if (described && filled == described) {
            dc.pixel_textures = probe.pixel_textures;
            dc.pixel_sampled_objects = probe.pixel_sampled_objects;
            ++g_cmdBufTexFromBuffer;
          } else if (described) {
            ++g_cmdBufTexBufferShort;
          } else {
            ++g_cmdBufTexNoBufferBinding;
          }
        }
      }
      // THE LIGHT BUFFER SLOT MUST BE RESOLVED AT REPLAY TIME. T_EcoLeaves
      // samples samplerLightBuffer at its PARAM_GEN screen position,
      // unnormalized, so that slot's xe_texinv has to be 1/extent of a 1280x720
      // target; on the palm frond it read (1,1), because the light buffer is a
      // RESOLVE SNAPSHOT that does not exist at RECORD time.
      //
      // NARROW ON PURPOSE: re-resolving EVERY slot against the live device is
      // what put a rock texture on the rider. Only a slot currently sampling a
      // 1x1 is a candidate, and only if its live resolution yields a snapshot.
      if (dc.pixel_shader_handle) {
        if (const TranslatedShader* t =
                TranslatedPixelShader(dc.pixel_shader_handle)) {
          for (uint32_t k = 0; k < t->sampler_count &&
                               k < mx::hle::DrawCall::kMaxPixelTextures;
               ++k) {
            const bool has_object =
                k < dc.pixel_sampled_objects.size() && dc.pixel_sampled_objects[k];
            if (has_object) continue;  // already a snapshot
            const auto& cur =
                k < dc.pixel_textures.size() ? dc.pixel_textures[k] : nullptr;
            if (!cur || cur->width > 1 || cur->height > 1) continue;
            mx::hle::DrawCall probe = dc;
            if (!ResolvePixelSlotTexture(probe, k, t->slot_guest[k], device,
                                         base))
              continue;
            if (k >= probe.pixel_sampled_objects.size() ||
                !probe.pixel_sampled_objects[k]) {
              ++g_cmdBufLightSlotNoSnapshot;
              continue;
            }
            dc.pixel_sampled_objects[k] = probe.pixel_sampled_objects[k];
            dc.pixel_textures[k] = probe.pixel_textures[k];
            if (k < dc.pixel_sampled_swizzles.size() &&
                k < probe.pixel_sampled_swizzles.size())
              dc.pixel_sampled_swizzles[k] = probe.pixel_sampled_swizzles[k];
            if (k < dc.pixel_sampler_signs.size() &&
                k < probe.pixel_sampler_signs.size())
              dc.pixel_sampler_signs[k] = probe.pixel_sampler_signs[k];
            ++g_cmdBufLightSlotFixed;
          }
        }
      }

      // PARAM_GEN COMES FROM THE BUFFER. The hardware fills one interpolator with
      // the pixel position and the vertex shader does not export it; the palm
      // leaf's pixel shader uses that slot as its colour UV. dc.pixel_param_gen
      // was captured from the RECORDING device, which reports it off, and so does
      // the live device -- but the recorded buffer programs SQ_PROGRAM_CNTL
      // itself, so that is the authoritative source. Silence leaves the recorded
      // value alone.
      if (issued < ov.size()) {
        uint32_t pc_raw = 0, cm_raw = 0;
        bool have_pc = false, have_cm = false;
        for (const auto& blk : ov[issued]) {
          if (!blk.is_reg || blk.dwords.empty()) continue;
          if (blk.first_const == kXeSqProgramCntl) {
            pc_raw = blk.dwords[0];
            have_pc = true;
          } else if (blk.first_const == kXeSqContextMisc) {
            cm_raw = blk.dwords[0];
            have_cm = true;
          }
        }
        if (have_pc) {
          const bool gen = (pc_raw & (1u << 18)) != 0;
          const uint32_t pos = have_cm ? ((cm_raw >> 8) & 0xFFu) : 0xFFu;
          // The SDK limits the destination to the sixteen interpolators;
          // malformed state disables it rather than indexing out of range.
          dc.pixel_param_gen = (gen && pos < 16) ? pos + 1 : 0;
        }
      }
      // The buffer's own writes for THIS draw, in stream order.
      if (issued < ov.size()) {
        for (const auto& blk : ov[issued]) {
          // Constants 256+ are the PIXEL bank -- the collector records both
          // ranges and flags which by index, so one loop routes each to its
          // own file rather than silently dropping half of them.
          const bool is_pixel = blk.first_const >= 256;
          auto& bank = is_pixel ? dc.pixel_constants : dc.vertex_constants;
          if (bank.empty()) continue;
          const size_t reg = is_pixel ? blk.first_const - 256 : blk.first_const;
          const size_t at = reg * 4;
          if (at >= bank.size()) continue;
          const size_t n = std::min(blk.dwords.size(), bank.size() - at);
          std::copy_n(blk.dwords.begin(), n, bank.begin() + at);
          g_cmdBufConstPatched += uint64_t(n / 4);
        }
      } else if (!ov.empty()) {
        // More captured draws than recorded DRAW_INDX packets: the two are out
        // of step and pairing them by position would be a guess.
        ++g_cmdBufConstUnpaired;
      }
    }
    // Before FinishHleDraw, which has its own refusals: a replayed draw is an
    // opportunity whether or not it survives, and gating the count on the far
    // side would measure the refusals rather than the replays.
    ++g_meshNames.replayDraws;
    if (dc.mesh_name) {
      ++g_meshNames.replayNamed;
    } else {
      // Per REPLAYED draw, not per distinct mesh: the question here is what
      // share of the frame's re-issued drawing runs unnamed geometry, and a
      // buffer replayed nine thousand times is nine thousand draws.
      ++ReplayMissByShader()[dc.vs_runtime_generated
                                 ? "(runtime-generated shader)"
                             : dc.vs_name ? dc.vs_name->c_str()
                                          : "(shader unnamed)"];
    }
    if (FinishHleDraw(dc)) {
      ++issued;
    } else {
      ++g_cmdBufReplayRefused;
    }
  }
  g_cmdBufReplayed += issued;
  return issued;
}

void ReportCmdBufReplay() {
  std::lock_guard<std::recursive_mutex> lk(g_cmdBufRecMu);
  std::string rows;
  for (const auto& [cb, rec] : g_cmdBufRec)
    if (!rec.draws.empty())
    {
      // NAME THE GEOMETRY, do not just count it: a buffer holding "2 draws" and a
      // buffer holding "the palm" are different claims, and matching on COUNT
      // (2 == 2) is what made the wrong one look right. 696 is the 3D leaf and
      // 85/86 the branch.
      std::string counts;
      for (const auto& d : rec.draws)
        counts += fmt::format("{}{}", counts.empty() ? "" : ",",
                              d.index_count);
      rows += fmt::format(" [0x{:08X} {} draw(s) x{} replays idx:{}]", cb,
                          rec.draws.size(), rec.replays, counts);
    }
  // Printed even at zero: "nothing was recorded" and "this report is not
  // wired" are different findings, and a suppressed line looks like neither.
  REXLOG_INFO("d3d9: CMDBUF RECORD/REPLAY -- captured {} draw(s) ({} past the "
              "{} cap, {} deferred to the pending path and NOT captured); "
              "replayed {} ({} refused by FinishHleDraw, {} replays had no "
              "recording); vs {} live / {} stale, ps {} live / {} stale, "
              "{} overlaid, {} unpaired; textures {} from buffer / {} short / "
              "{} undescribed; 1x1 slots {} -> snapshot, {} still none:{}",
              g_cmdBufCaptured, g_cmdBufCapOverflow, kMaxRecordedDraws,
              g_cmdBufDeferred, g_cmdBufReplayed, g_cmdBufReplayRefused,
              g_cmdBufReplayNoRec, g_cmdBufConstLiveBase,
              g_cmdBufConstNoLive, g_cmdBufPixelLiveBase,
              g_cmdBufPixelNoLive, g_cmdBufConstPatched,
              g_cmdBufConstUnpaired, g_cmdBufTexFromBuffer,
              g_cmdBufTexBufferShort, g_cmdBufTexNoBufferBinding,
              g_cmdBufLightSlotFixed, g_cmdBufLightSlotNoSnapshot,
              rows.empty() ? " none" : rows);
}

}  // namespace mx::hooks::d3d9
