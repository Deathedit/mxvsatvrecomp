#pragma once

#include <cstdint>

namespace mx::hle {
struct DrawCall;
}  // namespace mx::hle

// Complete D3D9 HLE draws that had to wait for the current frame's PM4 shader
// packets. Called by VdSwap after translating the frame range and before the
// accumulated HLE draw list is published to the render thread.
void FinalizePendingD3D9Draws(uint8_t* base);

// Per-frame VirtualQuery cost inside this layer. Called once per swap: reports
// the calls and accumulated time since the last swap, then resets. Native frame
// time is ~85% this file (see AGENTS.md); this says how much of that is the
// page-readability syscall.
void ReportHostPageQueryStats();

// Guest D3D9 draw calls so far, counted in BOTH native and plugin mode.
//
// Every other draw counter in this layer sits after MX_D3D9_PLUGIN_PASSTHROUGH
// and so reads zero under --gpu_plugin=xenos, which makes the two modes
// incomparable on the one number that decides where the missing main-menu
// backdrop lives. Exposed as a function rather than the atomic itself because
// hooks_frame.cpp cannot include the internal header.
uint64_t GuestDrawCalls();

// The exits that make FRAME DRAWS' `guest` exceed `accepted + refused`. Without
// these the gap is 6.9% of a level run with nothing attributing it, and the one
// population that WAS counted printed only under --hle_capture.
void UnbuiltDrawReasons(uint64_t& no_viewport, uint64_t& shader_failed,
                        uint64_t& nocode_queue_full, uint64_t& skips);

// The BuildHleDraw skip reasons, ranked, zero rows omitted. Measured to be the
// WHOLE of the gap, so this is the row that names the work.
std::string UnbuiltSkipBreakdown();

// HLE draws this layer ACCEPTED into the frame draw list and REFUSED, both
// cumulative.
//
// Counted in FinishHleDraw, where a built draw becomes one the renderer will
// issue. The first cut used the DEFERRED queue and reported `queued 0` on a
// native run whose capture contains 340 host draws -- that queue only holds
// draws with no shader code yet, and is legitimately zero on a normal frame. It
// sits at a push_back, which is what made it look like the submission point.
//
// The pair exists to be compared against GuestDrawCalls() in the same run and
// the same counter family. A capture showed the menu backdrop is not a draw we
// render wrongly -- it is not in the frame at all -- so:
//
//   guest == accepted           the guest never submits the background, and the
//                               defect is guest-state, not translation.
//   guest >  accepted+refused   guest draws vanish before BuildAndQueueDraw.
//   refused > 0                 we build them and throw them away; the skip
//                               histogram says which gate.
//
// Do NOT compare either against FRAME COST. That line counts shader-output
// ATTEMPTS and only prints on cost-gated frames; pitting it against a
// guest-entry counter across two modes is the exact cross-counter error
// recorded in backdrop-is-not-missing-draws. These three are one family: all
// count whole draws, all cumulative, differing only in how far down the pipe.
//
// Plain uint64_t behind a function, written on guest draw threads without a
// lock, so a read can lag by a draw or two. Fine for a per-frame delta.
uint64_t HleDrawsAccepted();
uint64_t HleDrawsRefused();

// One line of glyph-cache state: how often the Scaleform flush was CALLED, how
// many of those carried rects, and how many GetTexture calls failed.
//
// Declared here rather than in hooks_d3d9_internal.h because the caller is the
// frame hook, which does not include that header and must not start to.
//
// A separate reporter rather than another line inside the flush hook because
// the thing being diagnosed is a run with ZERO flushes, and a line that prints
// only when a flush happens renders that case as silence.
void ReportGlyphCache();

// PHASE 1 CHECK for the stencil plumbing. Call from the CONSUMER of a DrawCall,
// not from where its fields are filled in: the point is to test that the state
// survives the deferred-draw queue intact, which is the trip Phase 2 depends on
// and the only place it can be lost. Declared out here for the same reason as
// ReportGlyphCache -- the app layer needs it and must not include the internal
// header.
void NotePlumbedStencil(const mx::hle::DrawCall& dc);

// Is [addr, addr+bytes) readable guest memory? Wraps HostPageReadable, which
// lives in hooks_d3d9_internal.h -- declared out here for the same reason as
// ReportGlyphCache: the frame hook needs it and must not include that header.
//
// Probes both ends, so a range straddling a page boundary is not called
// readable on the strength of its first byte.
bool GuestRangeReadable(uint8_t* base, uint32_t addr, uint32_t bytes);

// Resolve a guest PHYSICAL address to one that is actually readable, returning
// 0 when none is. A bare physical address usually is not the readable one --
// the same mirrors CopyTexturePhysical and GuestTextureFingerprint walk apply
// here. Xenia gets this for free from Memory::TranslatePhysical; we do not.
uint32_t ResolveGuestRange(uint8_t* base, uint32_t addr, uint32_t bytes);
