#pragma once

// The seam between hooks_d3d9.cpp and hooks_d3d9_cmdbuf.cpp.
//
// Two declarations. Everything the replay block EXPORTS was already published
// in hooks_d3d9_internal.h -- BeginCmdBufRecording, EndCmdBufRecording,
// CmdBufForDevice, CaptureDrawIfRecording, NoteCmdBufDeferredDraw,
// ReplayCmdBuf, ReportCmdBufReplay -- so this header carries only the two things
// the block IMPORTS from the draw-submission code it left behind.
//
// Its own header rather than two more lines in hooks_d3d9_internal.h, which is
// the measured 62-symbol boundary to the entry points and is not the boundary
// being cut here.

#include <array>
#include <cstdint>

#include "hooks/hooks_d3d9_internal.h"  // kD3d9ConstRegs

namespace mx::hooks::d3d9 {

// Both defined in hooks_d3d9.cpp, beside the device state shadow they read.

// The viewport transform the guest's own registers describe. Replay needs it
// because a recorded buffer carries the viewport that was live when it was
// recorded, not the one live now.
bool BuildViewportMvp(uint32_t device, uint8_t* base, float out[16],
                      uint32_t* out_width, uint32_t* out_height);

// Snapshot the live vertex ALU constant bank for one draw. A recorded buffer's
// type-0 writes layer ON TOP of this, which is why replay needs the live bank
// rather than only the recorded deltas.
bool CaptureVertexConstants(uint32_t device, uint8_t* base, uint32_t shader,
                            std::array<uint32_t, kD3d9ConstRegs * 4>& out);

}  // namespace mx::hooks::d3d9
