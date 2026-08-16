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
