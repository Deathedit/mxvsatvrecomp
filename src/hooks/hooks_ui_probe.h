// The small surface the UI render-list probes share with hooks_plugin_diag.cpp.
//
// Measured, not guessed: the probe cluster in hooks_ui_probe.cpp needs exactly
// PlausibleGuestPtr, kCompDrawItem and g_videoCompFast from the diagnostics
// file, and the per-frame visit hook that stays there needs to call back into
// ReportUiEnqueue / ReportUiDrain. Nothing else crosses.
//
// The two report functions must keep being called from the visit hook BEFORE
// its `!watched` early-out. They have to print even when nothing was enqueued
// -- "no pushes" is the outcome they exist to detect, and a report driven from
// the hooked function itself could never say it.

#pragma once

#include <atomic>
#include <cstdint>

namespace mx::hooks {

// The UI component field carrying its persistent draw item.
//
// NOTE the offset collision seen elsewhere in this investigation: 236 is the
// draw item on a COMPONENT, and there is an unrelated +236 on the ITEM that
// gates the draw. Different objects, same number.
constexpr uint32_t kCompDrawItem = 236;

// A guest pointer worth dereferencing.
//
// THIS HAD AN UPPER BOUND OF 0x80000000 AND IT MANUFACTURED A ZERO. Heap
// objects sit at 0x2xxx_xxxx (0x21855..., 0x239..., 0x25E...), so the bound
// looked right -- but VTABLES live in the module image at 0x82xx_xxxx
// (imagebase 0x82000000). The submitter's vtable resolved to 0x8213E2B8, the
// guard rejected it, and the log reported `submitFn=0x00000000`, which reads
// exactly like "the game has no submit function" rather than "the probe
// refused to look". Only the raw vtable value being printed alongside it gave
// the lie away.
//
// A validity guard that fails closed must never be able to fabricate the same
// value the measurement is looking for. Keep the low bound (it rejects null and
// small garbage) and drop the high one -- the guest address space is 32-bit and
// the image is legitimately at the top of it.
inline bool PlausibleGuestPtr(uint32_t p) {
  return p >= 0x10000000u && (p & 3u) == 0u;
}

// Lock-free prefilter for the per-frame visit hook. sub_8236DB10 runs for EVERY
// UI component every frame; taking the probe mutex there would put a contended
// lock on the UI's hot path to watch five objects. Written under the mutex at
// registration (load time only), read without it.
extern std::atomic<uint32_t> g_videoCompFast[16];

// Defined in hooks_ui_probe.cpp, called from the visit hook in
// hooks_plugin_diag.cpp. See the note above about WHERE they are called.
void ReportUiEnqueue(bool force);
void ReportUiDrain(bool force);

}  // namespace mx::hooks
