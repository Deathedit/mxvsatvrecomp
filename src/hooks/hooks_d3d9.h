#pragma once

#include <cstdint>

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
// Every other draw counter in this layer sits after
// MX_D3D9_PLUGIN_PASSTHROUGH and so reads zero under --gpu_plugin=xenos, which
// makes the two modes incomparable on the one number that decides where the
// missing main-menu backdrop lives. See the note at its definition in
// hooks_d3d9_internal.h. Exposed as a function rather than the atomic itself
// because hooks_frame.cpp cannot include the internal header.
uint64_t GuestDrawCalls();

// HLE draws this layer ACCEPTED into the frame draw list and REFUSED, both
// cumulative.
//
// Counted in FinishHleDraw, which is where a built draw becomes one the
// renderer will issue. The first cut of this used the DEFERRED queue
// (g_pendingQueued) and reported `queued 0` on a native run whose capture
// contains 340 host draws -- that queue only holds draws with no shader code
// yet, waiting on the frame's PM4 packets, and is legitimately zero on a normal
// frame. It sits at a push_back, which is what made it look like the draw
// submission point. Check a counter against something already known to be true
// before drawing a conclusion from it.
//
// The pair exists to be compared against GuestDrawCalls() in the same run and
// the same counter family. mxmenu.rdc showed the menu backdrop is not a draw we
// render wrongly -- it is not in the frame at all -- and the open question is
// whether the guest ever submitted it. Three outcomes:
//
//   guest == accepted           the guest never submits the background, and
//                               the defect is guest-state, not translation.
//   guest >  accepted+refused   guest draws vanish before BuildAndQueueDraw.
//   refused > 0                 we build them and throw them away; the skip
//                               histogram says which gate.
//
// Do NOT compare either of these against FRAME COST. That line counts
// shader-output ATTEMPTS and only prints on cost-gated frames; pitting it
// against a guest-entry counter across two modes is the exact cross-counter
// error recorded in backdrop-is-not-missing-draws. These two and
// GuestDrawCalls() are one family: all three count whole draws, all three are
// cumulative, and the only difference is how far down the pipe the draw got.
//
// Plain uint64_t behind a function, like GuestDrawCalls: written on guest draw
// threads without a lock, so a read can lag by a draw or two. Fine for a
// per-frame delta, and not worth an atomic on the draw path.
uint64_t HleDrawsAccepted();
uint64_t HleDrawsRefused();

// One line of glyph-cache state: how often the Scaleform flush was CALLED, how
// many of those carried rects, and how many GetTexture calls failed.
//
// Declared here rather than in hooks_d3d9_internal.h because the caller is the
// frame hook, which does not include the internal header -- and must not start
// to, given that header's include-order requirement.
//
// The reason it is a separate reporter and not another line inside the flush
// hook: the thing being diagnosed is a run with ZERO flushes, and a line that
// prints only when a flush happens renders that case as silence. Call it on a
// swap cadence so every one of these being zero is itself a readable result.
void ReportGlyphCache();
