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
// THAT THEORY WAS TESTED AND REJECTED -- corrected 2026-08-30, and the
// paragraph below is kept only because the reasoning is worth not repeating.
//
// The theory ran: the guest's feedback walk (sub_82AF5D38) stamps entries in
// that region and clears them as requests retire, so destroying its bookkeeping
// means requests never retire, the pending queues stay full, and the residency
// budget -- min(2, 5 - (pending + 3) / 4), zero once ~17 pages queue --
// collapses and stays collapsed. A chain from "we ignore a status bit" to "the
// terrain page table never converges".
//
// It does not hold, and the reference is why. Xenia records a claimed range as
// a POSITIVE signal: `if (cpu_read || private_ring || coherency) return
// kToGuestRam` -- a range the guest works with is one it makes SURE to copy to
// guest RAM. So our feedback destination being claimed every frame argues for
// KEEPING the writeback, not dropping it. The decision and its quotation live
// at the call site (hooks_d3d9_entry.cpp, above NoteClaimedWriteback).
//
// NOTHING IN THIS MODULE CHANGES BEHAVIOUR. It parses, records and counts; no
// write is suppressed and no flush is issued, for VC or TC. It stays because
// the measurement is what killed the theory above, and because reading these
// bits backwards has already cost one regression ([[guest-coherency-requests]])
// -- not because a policy is built on it. Do not read a counter here as
// evidence that something is being acted on.
namespace mx::coherency {

// From the PM4 parse, on a poll whose status has TC set and VC clear. Ranges
// are guest PHYSICAL addresses, as the registers carry them.
void NoteGuestWroteRange(uint32_t phys_base, uint32_t size);

// True when [addr, addr+bytes) lies inside a range the guest has claimed.
// `addr` is a guest address in any mirror; it is normalised here so callers do
// not each get the physical translation subtly wrong.
//
// CONSERVATIVE ON PURPOSE: only a range that FULLY covers the write would
// count. A partial overlap means our understanding of the boundary is wrong,
// and acting on a guess is how a diagnostic becomes a bug.
bool GuestOwnsRange(uint32_t addr, uint32_t bytes);

// How many writebacks landed in a range the guest had claimed, out of how many
// were looked at. NOTHING IS SUPPRESSED -- these were named Suppressed* until
// 2026-08-30, which read as a policy that does not exist; the write proceeds on
// the next line at the call site either way. Counted so "claimed 0" and "never
// asked" stay distinguishable.
void NoteClaimedWriteback(bool claimed);
uint64_t ClaimedWritebacks();
uint64_t ConsideredWritebacks();
uint32_t ClaimedRangeCount();

}  // namespace mx::coherency
