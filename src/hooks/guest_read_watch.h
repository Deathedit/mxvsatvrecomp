#pragma once

#include <cstddef>
#include <cstdint>

// GUEST READ WATCH -- does the guest CPU actually LOAD from a buffer we write?
//
// The surface readback path writes resolve results back into guest memory on the
// theory that the guest reads them with a plain load. For the terrain HEIGHT
// snapshot that is 99 KB a frame, and the only consumer we can SEE binds the
// host snapshot instead, so the write has no demonstrated reader at all.
//
// It cannot be answered by logging: guest-reads-resolves-from-memory is the
// record of exactly this trap -- the exposure value came back through an
// ordinary load, with no LockRect and no call we hook. A load leaves no trace
// unless the MEMORY ITSELF reports it.
//
// So this arms a PAGE_GUARD on the pages behind the buffer. The next access of
// any kind raises STATUS_GUARD_PAGE_VIOLATION, Windows clears the guard bit for
// that page automatically, and execution resumes -- one report per page per arm,
// self-limiting by construction.
//
// READS AND WRITES ARE BOTH REPORTED, because our own writeback is a writer and
// the texture fingerprint is a reader: if the guard could not tell them apart it
// would answer its own question wrong. The caller resolves the faulting RIP
// against PPCFuncMappings and only counts an access as the GUEST's when the RIP
// lands inside recompiled code.
namespace mx::watch {

// Arm the pages covering [guest_addr, guest_addr + bytes) via their host
// mapping. Call AFTER writing, so the next toucher is somebody else. `what` is
// a short label carried into the report. Safe to call every frame -- the arm is
// throttled internally and stops for good once the budget is spent.
void ArmGuestReadWatch(void* host_addr, size_t bytes, uint32_t guest_addr,
                       const char* what);

enum class GuardHit {
  kNotOurs,  // some other guard page; the handler must pass it along
  kOurs,     // inside a watched range -- caller reports and continues
};

// Called from the vectored exception handler on STATUS_GUARD_PAGE_VIOLATION.
// Fills `guest_addr` and `what` when the address is inside a watched range.
GuardHit OnGuardPageHit(uint64_t fault_addr, uint32_t* guest_addr,
                        const char** what);

// True while the watch still has report budget left. The handler uses this to
// stop formatting messages once the answer is in.
bool GuestReadWatchActive();

// Counted so the report can say "N accesses, M of them from guest code" rather
// than printing a first sighting and going quiet.
//
// `host_rva` is the faulting RIP as a module-relative address, recorded for the
// accesses NOT attributed to guest code. That is the hole this closes: a read the
// guest makes through a runtime helper -- a memcpy intrinsic emitted as plain
// host code -- has no PPCContext and lands outside every recompiled function, so
// it is indistinguishable from our own writeback by attribution alone. Distinct
// SITES tell them apart.
void NoteGuestReadWatchAccess(bool from_guest_code, bool is_write,
                              uint64_t host_rva);

}  // namespace mx::watch
