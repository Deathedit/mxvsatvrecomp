#pragma once

// The one symbol hooks_d3d9_pm4.cpp publishes that hooks_d3d9_internal.h does
// not already carry.
//
// Its own header, rather than two more lines in internal.h, because internal.h
// is the measured 62-symbol boundary between hooks_d3d9.cpp and the entry
// points and every seam that widens it makes the next one easier to widen. This
// header has exactly two users: hooks_d3d9_pm4.cpp defines it, and
// hooks_d3d9_entry.cpp's ExecuteCommandBuffer hook calls it.

#include <cstdint>

namespace mx::hooks::d3d9 {

// Census of one command-buffer execution: counts the run, walks its block list
// and logs the indirect buffers it points at. Measuring only.
void NoteCommandBufferExec(uint32_t cmdbuf, uint8_t* base);

}  // namespace mx::hooks::d3d9
