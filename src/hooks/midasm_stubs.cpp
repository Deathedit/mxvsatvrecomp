// Mid-ASM hook targets.
//
// These are injected at PPC instruction addresses by the [[midasm_hook]]
// entries in mx_config.toml: the instruction at the address is replaced with a
// call to one of these functions plus a jump to the hook's jump_address. See
// the mid-ASM hook table in AGENTS.md for the address/skip mapping.
//
// IMPORTANT: every function here must stay at global namespace with an
// unchanged signature. The generated code declares them dllimport and
// CMakeLists.txt resolves them via `LINKER:/EXPORT:<name>`. Wrapping them in a
// namespace changes the mangled name and breaks that export.
//
// IMPORTANT: mid-ASM hooks are unconditional — they always fire and always
// jump. There is no conditional behavior available here.

#include <rex/logging.h>

//=============================================================================
// Mid-ASM hook: skip game tick virtual call at 0x82B70854
//=============================================================================

void NativeGameTickSkip() {}
void NativeSetupDeviceSkip() {}
void NativeSkipVtable8() { REXLOG_INFO("native: skip vtable[8]"); }
void NativeSkipVtable17() { REXLOG_INFO("native: skip vtable[17]"); }
void NativeSkipRendererInit() { REXLOG_INFO("native: skip renderer init -> Transition thread"); }
// Superseded 2026-08-02 by NativeSkipRendererDispatch below — this one deleted
// LoaderTick's whole renderer block (0x82B70EC8..0x82B710BC) to avoid a single
// GPU-bound call. Kept exported so mx_config.toml can restore that baseline.
void NativeSkipLoaderRenderer() {}

// Bisection stubs (2026-07-31). NativeSkipRendererDispatch is now hook #6 and
// fires every LoaderTick; the other two remain unused but exported.
//
// Their 2026-07-31 finding — "when hook #6 is disabled, NONE of these three
// stubs fire, so execution stalls in `bctrl sub_82B3C7D0` at 0x82B70EE8 (the
// lazy-init alloc)" — DID NOT REPRODUCE on 2026-08-02. dword_830BE190 is
// already populated by the time LoaderTick runs, so the `bne` at 0x82B70EE0
// branches past that bctrl and it never executes. The original stall belonged
// to the era when hooks #2/#5 were active and had to pre-populate the slot by
// hand from the main thread.

static int g_post_lazy_init_reaches = 0;
void NativePostLazyInitLog() {
  ++g_post_lazy_init_reaches;
  if (g_post_lazy_init_reaches <= 20) {
    REXLOG_INFO("native: PostLazyInit #{} (reached 0x82B70EEC, lazyinit returned)",
                g_post_lazy_init_reaches);
  }
}

static int g_pre_dispatch_reaches = 0;
void NativePreDispatchLog() {
  ++g_pre_dispatch_reaches;
  if (g_pre_dispatch_reaches <= 20) {
    REXLOG_INFO("native: PreDispatch #{} (reached loc_82B70EF0, just before bl sub_82B34998)",
                g_pre_dispatch_reaches);
  }
}

static int g_renderer_dispatch_skips = 0;
void NativeSkipRendererDispatch() {
  ++g_renderer_dispatch_skips;
  if (g_renderer_dispatch_skips <= 5 || (g_renderer_dispatch_skips % 100) == 0) {
    REXLOG_INFO("native: SkipRendererDispatch #{} (sub_82B34998 call bypassed)",
                g_renderer_dispatch_skips);
  }
}

void NativeSkipLoaderEarly() {}
void NativeSkipLoaderAll() {}
