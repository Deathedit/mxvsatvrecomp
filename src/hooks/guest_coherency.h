#pragma once

#include <cstdint>

// COHERENCY REQUESTS THE GUEST MAKES ABOUT ITS OWN MEMORY.
//
// The guest writes COHER_BASE_HOST / COHER_SIZE_HOST / COHER_STATUS_HOST and
// then spins on a WAIT_REG_MEM polling COHER_STATUS_HOST (0x0A31) until the GPU
// clears it. At that poll the three registers name one request, and two status
// bits mean opposite things (Xenia's registers.h, COHER_STATUS_HOST):
//
//   vc_action_ena (bit 24)  "make this range visible to me NOW" -- the driver's
//                           post-resolve flush.
//   tc_action_ena (bit 25)  alone, the OPPOSITE: the guest saying it wrote that
//                           memory ITSELF. Xenia's note is exact -- "copying
//                           over it would destroy what the guest produced".
//
// We had been ignoring both. Measured live over ~2 minutes: 22,624 polls, VC
// 9,651, TC 11,210, both 821.
//
// AND ONE TC RANGE IS EXACTLY OUR VT FEEDBACK WRITEBACK. Our destination
// 0xFA2DC000 is physical 0x1A2DD000 size 0x4000, and the guest claims
// `base 0x1A2DD000 size 0x4000` as its own 1,438 times a run -- the same 16 KB,
// byte for byte. We were copying stale readback over it every frame.
//
// Why that is not cosmetic: the guest's feedback walk (sub_82AF5D38) stamps
// entries in that region and clears them as requests retire. Destroying its
// bookkeeping means requests never retire, the pending queues stay full, and
// the residency budget -- min(2, 5 - (pending + 3) / 4), zero once ~17 pages
// queue -- collapses and stays collapsed. That is a chain from "we ignore a
// status bit" to "the terrain page table never converges".
namespace mx::coherency {

// From the PM4 parse, on a poll whose status has TC set and VC clear. Ranges
// are guest PHYSICAL addresses, as the registers carry them.
void NoteGuestWroteRange(uint32_t phys_base, uint32_t size);

// True when [addr, addr+bytes) lies inside a range the guest has claimed.
// `addr` is a guest address in any mirror; it is normalised here so callers do
// not each get the physical translation subtly wrong.
//
// CONSERVATIVE ON PURPOSE: only a range that FULLY covers the write suppresses
// it. A partial overlap means our understanding of the boundary is wrong, and
// dropping a write on a guess is how a diagnostic becomes a bug.
bool GuestOwnsRange(uint32_t addr, uint32_t bytes);

// Counted so "suppressed 0" and "never asked" stay distinguishable, and so the
// suppression can be judged rather than assumed. Reported by the writeback.
void NoteSuppressedWriteback(bool suppressed);
uint64_t SuppressedWritebacks();
uint64_t ConsideredWritebacks();
uint32_t ClaimedRangeCount();

}  // namespace mx::coherency
