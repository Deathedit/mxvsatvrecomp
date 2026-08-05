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
