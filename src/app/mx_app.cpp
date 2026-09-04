#include "app/mx_app.h"

#include <rex/cvar.h>
#include <rex/graphics/flags.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

#include "hooks/guest_read_watch.h"
#include "hooks/native_bridge.h"
#include "mx_init.h"

std::unique_ptr<rex::ui::WindowedApp> MxApp::Create(
    rex::ui::WindowedAppContext& ctx) {
  return std::unique_ptr<MxApp>(new MxApp(ctx, "mx", PPCImageConfig));
}

namespace {

// Which recompiled guest function does a host RIP land in?
//
// A host RVA on its own is unusable: the release build ships no PDB, so the
// first fault of this kind cost an afternoon of PE archaeology to turn one
// address into a guest function to open in IDA. The recompiler already publishes
// that table.
//
// PPCFuncMappings is sorted by GUEST address, not by host, so this is a linear
// sweep for the greatest host entry at or below the RIP, plus the smallest above
// it. Those two bracket the function, which makes the answer a containment
// result rather than a nearest-neighbour guess: a RIP outside [best, next) is
// not in recompiled code at all, and saying so is the point.
struct GuestFuncHit {
  uint32_t guest = 0;
  uint64_t host = 0;
  uint64_t next_host = 0;
  // Guest bytes of the function itself, from the next entry in GUEST order.
  // Without it the offset below cannot be sanity-checked -- see the note at
  // the report.
  uint32_t guest_size = 0;
};

GuestFuncHit ResolveGuestFunction(uint64_t rip) {
  GuestFuncHit hit;
  for (const PPCFuncMapping* m = PPCFuncMappings; m->guest; ++m) {
    const uint64_t host = reinterpret_cast<uint64_t>(m->host);
    if (host <= rip) {
      if (host > hit.host) {
        hit.host = host;
        hit.guest = static_cast<uint32_t>(m->guest);
      }
    } else if (!hit.next_host || host < hit.next_host) {
      hit.next_host = host;
    }
  }
  // Second sweep for the next GUEST address, which gives the function's size.
  if (hit.guest) {
    uint32_t next_guest = 0;
    for (const PPCFuncMapping* m = PPCFuncMappings; m->guest; ++m) {
      const uint32_t g = static_cast<uint32_t>(m->guest);
      if (g > hit.guest && (!next_guest || g < next_guest)) next_guest = g;
    }
    if (next_guest) hit.guest_size = next_guest - hit.guest;
  }
  return hit;
}

bool Readable(const void* p, size_t bytes) {
  MEMORY_BASIC_INFORMATION mbi{};
  if (!VirtualQuery(p, &mbi, sizeof(mbi))) return false;
  if (mbi.State != MEM_COMMIT) return false;
  const DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                         PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                         PAGE_EXECUTE_WRITECOPY;
  if ((mbi.Protect & readable) == 0) return false;
  if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
  const uint64_t end = reinterpret_cast<uint64_t>(mbi.BaseAddress) + mbi.RegionSize;
  return reinterpret_cast<uint64_t>(p) + bytes <= end;
}

// Recompiled functions are PPCFunc(PPCContext&, uint8_t* base) and this build
// passes them in rdi/rsi. Rather than trust that, the guest memory base is the
// discriminator: whichever argument register pair has the base in its second
// slot is the live one, and if neither does we are not in recompiled code and
// print nothing rather than dereferencing a guess inside an exception handler.
const PPCContext* GuestContextFrom(const CONTEXT* c, uint64_t gbase) {
  if (!gbase) return nullptr;
  const uint64_t pairs[][2] = {{c->Rdi, c->Rsi}, {c->Rcx, c->Rdx}};
  for (const auto& p : pairs) {
    if (p[1] != gbase) continue;
    const auto* ctx = reinterpret_cast<const PPCContext*>(p[0]);
    if (Readable(ctx, sizeof(PPCContext))) return ctx;
  }
  return nullptr;
}

// Crash reporter. The log loses its last lines on a hard fault, which made
// bisecting the native LoaderTick path unreliable -- a probe would appear to die
// before a log line that had actually already executed. This catches the fault,
// records exactly what address was touched, and flushes before the process dies.
// Registered first (last arg 1) so it runs before anything else.
LONG CALLBACK CrashReporter(EXCEPTION_POINTERS* info) {
  const auto* rec = info->ExceptionRecord;

  // GUARD PAGE -- the guest read watch, NOT a crash. Handled first and returns
  // CONTINUE_EXECUTION, because Windows has already cleared the guard bit for
  // the faulting page and the instruction will now succeed.
  //
  // The discriminator that makes this probe worth anything: the faulting RIP is
  // resolved against PPCFuncMappings, and the access only counts as the GUEST's
  // when it lands inside recompiled code. Our own writeback writes this buffer
  // and the texture fingerprint reads it, so without that test the watch would
  // answer its own question with our own traffic.
  if (rec->ExceptionCode == STATUS_GUARD_PAGE_VIOLATION) {
    const uint64_t fault = static_cast<uint64_t>(rec->ExceptionInformation[1]);
    const uint64_t modbase_watch =
        reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr));
    uint32_t guest_addr = 0;
    const char* what = "";
    if (mx::watch::OnGuardPageHit(fault, &guest_addr, &what) ==
        mx::watch::GuardHit::kOurs) {
      const bool is_write = rec->ExceptionInformation[0] == 1;
      const uint64_t rip = reinterpret_cast<uint64_t>(rec->ExceptionAddress);
      const GuestFuncHit hit = ResolveGuestFunction(rip);
      // THE FULL ATTRIBUTION RULE, not just containment. `hit.guest &&
      // (!hit.next_host || rip < next_host)` immediately reported our OWN
      // writeback as "FROM GUEST CODE" -- at +0x9014E5C5C past a 0x60-byte
      // function, i.e. 38 GB -- because when next_host is 0 (a RIP above every
      // mapping, which is where host code lives) that test accepts anything.
      //
      // Both signals the crash report already documents are required here: the
      // host/guest size ratio, and the absence of a PPCContext in the argument
      // registers, which every recompiled function has.
      const uint64_t off = hit.guest ? rip - hit.host : 0;
      const bool implausible =
          !hit.guest_size || off > uint64_t(hit.guest_size) * 24ull;
      const PPCContext* wctx = GuestContextFrom(info->ContextRecord,
                                                reinterpret_cast<uint64_t>(
          mx::native::NativeGraphics::Get().GetGuestMemory()));
      const bool in_guest_code = hit.guest && hit.next_host &&
                                 rip < hit.next_host && !implausible && wctx;
      mx::watch::NoteGuestReadWatchAccess(in_guest_code, is_write,
                                          rip - modbase_watch);
      if (in_guest_code) {
        REXLOG_INFO(
            "native: READ WATCH {} of {} at guest 0x{:08X} -- FROM GUEST CODE, "
            "recompiled 0x{:08X} +0x{:X} (guest size 0x{:X})",
            is_write ? "WRITE" : "READ", what, guest_addr, hit.guest, off,
            hit.guest_size);
      }
      return EXCEPTION_CONTINUE_EXECUTION;
    }
    // Somebody else's guard page (a stack growth, most likely). Not ours to
    // consume -- pass it along untouched.
    return EXCEPTION_CONTINUE_SEARCH;
  }

  if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
    const char* op = rec->ExceptionInformation[0] == 0   ? "read"
                     : rec->ExceptionInformation[0] == 1 ? "write"
                                                         : "execute";
    uint64_t fault = static_cast<uint64_t>(rec->ExceptionInformation[1]);
    uint64_t gbase = reinterpret_cast<uint64_t>(
        mx::native::NativeGraphics::Get().GetGuestMemory());
    uint64_t modbase = reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr));
    uint64_t rip = reinterpret_cast<uint64_t>(rec->ExceptionAddress);
    REXLOG_ERROR("*** ACCESS VIOLATION: {} at 0x{:016X}, host RIP=0x{:016X} (RVA 0x{:X})", op,
                 fault, rip, rip - modbase);
    const bool in_guest =
        gbase && fault >= gbase && fault < gbase + 0x100000000ull;
    const uint32_t guest_fault =
        in_guest ? static_cast<uint32_t>(fault - gbase) : 0;
    if (in_guest) {
      REXLOG_ERROR("***   -> guest address 0x{:08X} (base 0x{:016X})",
                   guest_fault, gbase);
    } else {
      REXLOG_ERROR("***   -> NOT in guest range (base 0x{:016X}) — host-side pointer", gbase);
    }

    // WHERE THE FAULT IS, AND HOW MUCH THAT CLAIM IS WORTH.
    //
    // The containment test is `rip < next_host`, and next_host is the next
    // mapped function in HOST order. That is not a bound on THIS function's
    // body: anything the linker placed in the gap -- runtime helpers, host code,
    // padding -- is inside [host, next_host) and gets attributed here anyway.
    //
    // Not hypothetical. The recurring UI-thread fault in this build (a read at
    // guest 0x4C69746C, "Litl" in ASCII -- a string used as a pointer) reported
    // for its entire history as
    //
    //     -> in recompiled guest function 0x8236EB30 (+0x1078 of host code)
    //
    // while that function is 0x7C guest bytes long: ~34 host bytes per guest
    // BYTE, an order out. And the register dump never printed for it, meaning
    // GuestContextFrom found no PPCContext -- which recompiled functions always
    // have. Two independent signals that the RIP was not in recompiled code at
    // all, and the line asserted otherwise across 17 crashes.
    //
    // So the claim now carries its evidence: the offset is reported against the
    // function's guest size, and the absence of a PPCContext is stated rather
    // than left as a missing line.
    const GuestFuncHit hit = ResolveGuestFunction(rip);
    const PPCContext* ctx = GuestContextFrom(info->ContextRecord, gbase);
    if (hit.guest && (!hit.next_host || rip < hit.next_host)) {
      const uint64_t off = rip - hit.host;
      // Recompiled x64 runs roughly 5-15 bytes of host per guest byte. Well past
      // that means the RIP is in whatever sits after the function, not in it.
      // Deliberately generous -- this only has to catch the order-of-magnitude
      // case.
      const bool implausible =
          hit.guest_size && off > uint64_t(hit.guest_size) * 24ull;
      if (implausible || !ctx) {
        REXLOG_ERROR(
            "***   -> host RIP is +0x{:X} past recompiled 0x{:08X} (guest size "
            "0x{:X}){}{} -- ATTRIBUTION UNRELIABLE, treat as host code, not as "
            "this guest function",
            off, hit.guest, hit.guest_size,
            implausible ? "; offset too large for that size" : "",
            ctx ? "" : "; no PPCContext in argument registers");
      } else {
        REXLOG_ERROR(
            "***   -> in recompiled guest function 0x{:08X} (+0x{:X} of host "
            "code, guest size 0x{:X})",
            hit.guest, off, hit.guest_size);
      }
    } else {
      REXLOG_ERROR("***   -> not inside any recompiled guest function (nearest below 0x{:08X})",
                   hit.guest);
    }

    // The registers are the whole point of catching this: they name which
    // pointer was bad, and every field of it the guest had already loaded.
    if (ctx) {
      // Indexed by hand because PPCContext stores r3 first, ahead of r0, so
      // the registers are not an array and pointer arithmetic would silently
      // report the wrong one.
      const PPCRegister* gpr[32] = {
          &ctx->r0,  &ctx->r1,  &ctx->r2,  &ctx->r3,  &ctx->r4,  &ctx->r5,
          &ctx->r6,  &ctx->r7,  &ctx->r8,  &ctx->r9,  &ctx->r10, &ctx->r11,
          &ctx->r12, &ctx->r13, &ctx->r14, &ctx->r15, &ctx->r16, &ctx->r17,
          &ctx->r18, &ctx->r19, &ctx->r20, &ctx->r21, &ctx->r22, &ctx->r23,
          &ctx->r24, &ctx->r25, &ctx->r26, &ctx->r27, &ctx->r28, &ctx->r29,
          &ctx->r30, &ctx->r31};
      for (int i = 0; i < 32; i += 4) {
        REXLOG_ERROR("***   r{:<2}=0x{:08X}  r{:<2}=0x{:08X}  r{:<2}=0x{:08X}  r{:<2}=0x{:08X}",
                     i, gpr[i]->u32, i + 1, gpr[i + 1]->u32, i + 2,
                     gpr[i + 2]->u32, i + 3, gpr[i + 3]->u32);
      }
      REXLOG_ERROR("***   lr=0x{:08X} ctr=0x{:08X} last-indirect=0x{:08X}",
                   static_cast<uint32_t>(ctx->lr), ctx->ctr.u32,
                   ctx->last_indirect_target);
      // Name the base pointer the faulting access was derived from. A load is
      // almost always reg+displacement, so the register holding the object is
      // a little below the fault, not equal to it.
      if (in_guest) {
        for (int i = 0; i < 32; ++i) {
          const uint32_t v = gpr[i]->u32;
          if (v && guest_fault >= v && guest_fault - v < 0x1000) {
            REXLOG_ERROR("***   -> faulting access is r{} (0x{:08X}) + 0x{:X}",
                         i, v, guest_fault - v);
          }
        }
      }
    }
    rex::FlushLogging();
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

void MxApp::OnPreSetup(rex::RuntimeConfig& config) {
  static bool s_handlerInstalled = false;
  if (!s_handlerInstalled) {
    AddVectoredExceptionHandler(1, CrashReporter);
    s_handlerInstalled = true;
  }
  REXLOG_INFO("MxApp::OnPreSetup");

  auto gs = std::make_unique<rex::system::D3D12GraphicsSystem>();
  m_graphicsSystem = gs.get();
  config.graphics = std::move(gs);
}

void MxApp::OnPostSetup() {
  REXLOG_INFO("MxApp::OnPostSetup");

  // Dump all registered cvars (post plugin load — includes GPU plugin cvars)

  /*
  REXLOG_INFO("MxApp::OnPostSetup — dumping all cvars:");
  for (const auto& name : rex::cvar::ListFlags()) {
    const auto* info = rex::cvar::GetFlagInfo(name);
    std::string cur = rex::cvar::GetFlagByName(name);
    bool nondef = rex::cvar::HasNonDefaultValue(name);
    REXLOG_INFO("  cvar: {} = '{}' (default='{}', nondef={})",
                name, cur,
                info ? info->default_value : std::string{"?"},
                nondef);
  }
  */

  rex::ReXApp::OnPostSetup();
  if (m_graphicsSystem == nullptr) {
    REXLOG_INFO("MxApp::OnPostSetup - no graphics system");
    return;
  }
  auto* w = window();
  if (w == nullptr) {
    REXLOG_INFO("MxApp::OnPostSetup - no window");
    return;
  }
  HWND hwnd = static_cast<HWND>(w->GetNativeWindowHandle());
  REXLOG_INFO("MxApp::OnPostSetup - hwnd=0x{:08X}", (uintptr_t)hwnd);
  mx::native::SetWindowHandle(hwnd);
  m_graphicsSystem->InitializeRenderer(hwnd);
  REXLOG_INFO("MxApp::OnPostSetup done");
}
