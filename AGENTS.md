# MX vs. ATV Alive — ReXGlue D3D12 Rendering Backend

Static recompilation of the Xbox 360 build (`assets/default.xex`) via ReXGlue,
with a host D3D12 renderer replacing the Xenos GPU. The `rexgpu-xenos` plugin is
**not** used; native mode is the only supported profile.

## Build & Run

C++-only change (~15s):
```
cmake --build out/build/win-amd64-debug --target mx
```

After editing `mx_config.toml` (mid-ASM hooks, function sizes) — ~70s codegen plus a full rebuild:
```
rexglue codegen --force mx_manifest.toml
cmake --preset win-amd64-debug
cmake --build out/build/win-amd64-debug --target mx
```

The build writes to the build dir; the **root `mx.exe` is what runs** against the
assets in this directory, so copy it before running:
```
cp out/build/win-amd64-debug/mx.exe ./mx.exe
./mx.exe --game_data_root=assets --user_data_root=userdata
```

Both arguments are required — without them the process exits immediately with
code 1 and writes no useful log. Each run appends a new `logs/mx_NNN.log`.

Requires:
- Visual Studio 2022 Clang 18+ at `C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\Llvm\x64\bin`
- Ninja at `C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja`
- ReXGlue SDK 0.9.0 at `C:\rexglue-sdk`
- FFmpeg 8.0 in `ffmpeg/` (bin/, include/, lib/)
- FFmpeg DLLs: `avcodec-63.dll`, `avformat-63.dll`, `avutil-61.dll`, `swscale-10.dll`, `swresample-7.dll`

**PATH fix if cmake can't find tools:**
```
$env:PATH = "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\Llvm\x64\bin;C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;$env:PATH"
```

**Build notes:**
- CMakeLists.txt must NOT have `-U_DLL` on host source files (causes RuntimeLibrary mismatch MTd vs MDd)
- Mid-ASM hook functions exported via `LINKER:/EXPORT:NativeXxx` in CMakeLists.txt
- `cmake --preset` resets the cache. To keep crash symbolization, re-pass
  `-DCMAKE_CXX_FLAGS_DEBUG="-O0 -gline-tables-only" -DCMAKE_EXE_LINKER_FLAGS="-Wl,/MAP:mx.map"`

**Two crash classes here have been races, not logic bugs** — a rebuild alone
changed the outcome. One clean run proves nothing; verify 3/3 over 30s.

---

## Architecture

### Files

`src/` is grouped by layer: `app/` (ReXGlue application), `gfx/` (host D3D12 +
video), `gpu/` (Xenos PM4 understanding), `hooks/` (guest function hooks).
`src/` is on the include path, so cross-layer includes name their layer —
`#include "gpu/pm4_parser.h"`, `#include "gfx/d3d12_renderer.h"`.

| File | Role |
|---|---|
| `src/main.cpp` | Entry point (`REX_DEFINE_APP`) |
| **app** | |
| `src/app/mx_app.h/.cpp` | `MxApp`: OnPreSetup installs the graphics system (or sets plugin mode) and the crash reporter, OnPostSetup hands it the HWND |
| `src/app/graphics_system.h/.cpp` | `D3D12GraphicsSystem` + the host render thread (Bink playlist -> game frames) |
| **gfx** | |
| `src/gfx/d3d12_renderer.h` | `D3D12Renderer` interface — implemented across the three `.cpp` below |
| `src/gfx/d3d12_device.cpp` | Device, adapter, swapchain, RTVs, command list/allocators, fence, BeginFrame/EndFrame |
| `src/gfx/d3d12_video.cpp` | Video pipeline: fullscreen quad + texture sample, Bink frame upload |
| `src/gfx/d3d12_game.cpp` | Game pipeline (triangle PSO / translated draws), game RT + depth, PresentGameFrame copy |
| `src/gfx/d3d12_shaders.h` | HLSL source for both pipelines |
| `src/gfx/d3d12_internal.h` | LogError/LogInfo/CompileShader shared by the three gfx TUs (internal) |
| `src/gfx/bink_player.h/.cpp` | FFmpeg Bink video + audio decoder |
| **gpu** | |
| `src/gpu/pm4_parser.h/.cpp` | PM4 command buffer parser: Type-0/2/3 decode, 30 opcodes, 65 reg names, ring wrap, dump |
| `src/gpu/pm4_translator.h/.cpp` | PM4 -> `DrawCall` translation (draw opcodes, vertex fetch consts, shader constants) |
| `src/gpu/xenos_gpu_state.h/.cpp` | Xenos GPU register shadow: 66 named registers, ApplyType0Write, ApplyType3Packet, Snapshot/DumpDiff |
| **hooks** | |
| `src/hooks/native_bridge.h/.cpp` | `NativeGraphics` singleton (guest mem base, renderer ptr, draw-call queue) + `g_plugin_mode` |
| `src/hooks/hook_common.h` | Shared hook internals: `LogEngSlot8`, `IsBinkPlaying` (internal) |
| `src/hooks/midasm_stubs.cpp` | All 11 exported mid-ASM hook targets (must stay at global namespace — see file header) |
| `src/hooks/hooks_frame.cpp` | VdSwap (PM4 parse/translate), XenosWait, Begin/EndFrame, GpuState, FramePendingPoll |
| `src/hooks/hooks_wait.cpp` | Wait, NtSetEvent, ErrorRecovery |
| `src/hooks/hooks_gameloop.cpp` | MainLoop, RenderPipeline |
| `src/hooks/hooks_loading.cpp` | SetupRenderer, Transition, LoaderTick |
| `src/hooks/hooks_boot.cpp` | Bootstrap, GraphicsInit, EngineInit, PostGfxInit, TexManager, BindTexture, GpuAlloc, Cleanup1/2 |
| `src/hooks/hooks_plugin_diag.cpp` | RendererDispatch, LazyInit, Timing, sub_82B6D230, LoadStateMachine, VtableCtor. Mostly plugin-gated; Timing and LoadStateMachine are active in native mode |
| **build** | |
| `mx_config.toml` | Function sizes + `[[midasm_hook]]` definitions |
| `CMakeLists.txt` | Source list by layer, `src/` include dir, linker exports for mid-ASM hooks |

Input is handled entirely by ReXGlue's built-in `SDLInputDriver`
(`XamInputGetState` / `XamInputGetCapabilities` at kernel level) — there is no
`src/native_input.*` and no manual `REX_FUNC` input hook.

### Host Pipeline
```
MxApp::OnPreSetup  -> crash reporter, D3D12GraphicsSystem, clears gpu_plugin
MxApp::OnPostSetup -> HWND, InitializeRenderer (D3D12 + SDL gamepad)
EngineInit hook    -> SetGuestMemory(base)
RenderThread       -> Bink playlist -> UploadVideoFrame -> BeginFrame -> RenderVideoFrame -> EndFrame
                   -> After Bink: BeginFrame -> RenderGameFrame (triangle) -> PresentGameFrame -> EndFrame
SDL Audio Thread   -> Bink ring buffer -> SDL audio
```

The Bink playlist is host-side and hardcoded (`graphics_system.h`:
`THQ_Logo_wSound.bik`, `Attract.ENG.bik`). It does not go through the guest.
`SetBinkPlaying()` tells the `RenderPipeline` hook to stand down from the
swapchain while it runs.

### D3D12 Renderer
- **Video pipeline**: fullscreen quad VS + texture-sampled PS, SRV descriptor heap, Bink frame upload via copy queue
- **Game pipeline**: colored triangle (VS with position+color input, MVP constant buffer; PS pass-through), VB/IB/CB in upload heap, CBV descriptor heap, 3 indices
- **Game RT**: dedicated 1280x720 render target + D32 depth buffer; game RTV/DSV descriptor heaps
- **PresentGameFrame**: transition barriers + CopyTextureRegion game RT → swapchain backbuffer

---

## Guest Architecture

### Binary
- Base: `0x82000000`, XEX: `assets/default.xex`
- GPU plugin (`rexgpu-xenosd.dll`) disabled

### Engine State: `dword_830BE400`
Central engine state pointer set by Bootstrap:

| Offset | Value | Role |
|--------|-------|------|
| +0 | 0x400EA4E0 | Bootstrap engine object (80 bytes, vtable 0x82139C44, vt[36]=nullsub_1 @ 0x82426FF0) |
| +8 | 0x40BCF740 | AssetDB block. Written by SetupRenderer's vt[17] (`sub_82B43AC8`), which now runs naturally |
| +12 | 0x40B78860 | SceneManager — runtime vtable 0x82141754, vt[0]=0x82B671D0, vt[3]=0x82B62910 |
| +16 | 0x830EC248 | Transition renderer (545KB, vtable 0x8214518C) |
| +20 | 0x40BB8ED0 | Graphics device handle |
| +28-+36 | ptrs | Render pass entity lists |
| +52 | 0x408929D0 | Scene manager returned by `engine->vt[2]()`; vtable 0x8214207C, vt[32]=0x82B6E298 |

Sub-entities used by LoaderTick's entity loops live at **+0x1C / +0x20 / +0x24**
(observed 0x4081D7F0 / 0x4082F350 / 0x4080D3A0). The loops are guarded on
`*(sub+0x3C)`.

**eng+12's vtable is shorter than 36 entries.** Slot 36 reads past the function
table into adjacent string data ("Load", "Game", "Real_World", "SP_EVENT", …), so
`vt[36]` resolves to `0x4C6F6164` — ASCII, not a function. This is why eng+12 is
not the game-tick dispatcher.

### Transition renderer (`0x830EC248`) event handles
| Offset | Handle | Used by |
|---|---|---|
| +0x190 | 0xF80001E8 | Signalled by the renderer block at 0x82B70FF4 |
| +0x194 | 0xF80001F0 | Waited on by LoaderTick's first instruction; signalled per-frame by our MainLoop hook |
| +0x2DC | 0xF80001CC | Signalled by the Transition loop after each LoaderTick |
| +0x2E0 | 0xF80001D0 | Waited on by the renderer block at 0x82B70FA8 |

### GPU Memory (GpuAlloc return addresses)
| Alloc | Size | Address | Purpose |
|-------|------|---------|---------|
| #1 | 0x00F00000 (15MB) | 0xBEDA0000 | Main pool - render targets + depth |
| #2 | 0x00730000 (7.3MB) | 0xBE0C0000 | Secondary render targets |
| #3 | 0x00398000 (3.6MB) | 0xBDD20000 | Additional buffers |
| #4-15 | 0x00080000 each | 0xBDCA0000-down | Shader caches |

`dword_830B03EC` (GPU physical base) **stays 0x00000000** in native mode. This is
not a blocker for asset loading — the loader runs to completion with it at zero.

### Key Flags
| Address | Name | Meaning |
|---------|------|---------|
| 0x82D57994 | byte_82D57994 | 0=loading complete, 1=render path |
| 0x82D571BC | byte_82D571BC | Event handling flag |
| 0x830C2150 | pass 0 entity count | Populated by loading |
| 0x830C4560 | pass 1 entity count | Populated by loading |
| 0x830C6970 | pass 2 entity count | Populated by loading |

---

## Guest Entry Point Flow

```
xstart (0x82BFD3C0)
 +- System init
 +- EngineInit (0x82BA7F58)
     +- Bootstrap (0x82ABB838) -> alloc 80B -> dword_830BE400
     +- sub_82BA7AB8 -> display res (1280x720)
     +- sub_8253CF08 -> AssetDB (111KB)
     +- SetupRenderer (0x82B71148)
     |   +- NtCreateEvent x4 -> transition renderer +0x190/194/2DC/2E0
     |   +- GraphicsInit (0x82AEBF40) -> GpuAlloc #1-15
     |   +- PostGfxInit (0x82AE9658) -> device vtable calls
     |   +- vt[6] -> creates the scene manager at eng+52
     |   +- vt[17] (sub_82B43AC8) -> writes eng+8 = AssetDB block
     |   +- TexManager + BindTexture
     |   +- Transition thread created at 0x82B716B0
     +- while (MainLoop != 0) { }  (forced r3=1)
     +- Cleanup1 (stubbed) -> Cleanup2 (stubbed)
     +- EngineInit returns -> sleep loop
```

### MainLoop (`sub_82B70760`, decompiled)
```c
int MainLoop(a1) {
    v2 = 1;
    if (byte_82D57994) {
        // spin-wait: Wait(500ms), break if event signaled
        if (v7) { sub_8255D430(stubbed); RenderPipeline(hooked); }
        else v2 = 0;
    } else v2 = 0;
    (*(eng+8)->vtable[36])();   // eng+8 = the real AssetDB
    return v2;
}
```

### LoaderTick (`sub_82B70DE8`, decompiled)
```c
int LoaderTick(a1) {            // a1 = Transition renderer
    Wait(*(a1+0x194), -1);      // per-frame handshake; our MainLoop hook signals it
    sub_82B70370(a1);           // timing — STUBBED in native mode
    // engine->vt[2]() -> scene manager, ->vt[32](dt) -> update scene
    // sub_82373660 (TexManager), sub_82B6D230(dt), entity loops @0x82B70E54
    result = *(a1+8)->vtable[6]();       // sub_8253AA40 = AssetDB_LoadStateMachine
    if (result) {
        // 0x82B70EC8..0x82B710BC — renderer block. Runs natively except for
        // the one skipped instruction `bl sub_82B34998` at 0x82B70EF4.
        return 1;
    }
    return result;              // 0 = no work
}
```

### Transition (`sub_82B710D0`, decompiled)
```c
void Transition() {
    v0 = *(dword_830BE400 + 16);
    do { v1 = LoaderTick(v0); v2 = v1; v0->frame_count++;
         if (byte_82D57994) NtSetEvent(v0+0x2DC);
    } while (v2);
    byte_82D57994 = 0;
}
```

### RenderPipeline (`sub_82B70578`, decompiled)
```c
void RenderPipeline(a1) {
    sub_82ABF828(renderer);  // BeginFrame (stubbed)
    // 3 render passes, entity iteration, draw calls
    sub_82ABF930(renderer);  // EndFrame -> VdSwap (fires every frame in native mode)
}
```

---

## Mid-ASM Hooks (`mx_config.toml`)

Injected at PPC instruction addresses: the instruction at `address` is **replaced**
with a C++ call plus `goto jump_address`.

**They are unconditional.** Always fire, always jump, no conditional behavior, and
they fire in **plugin mode too** — `g_plugin_mode` gates only the C++ `REX_FUNC`
hooks, never these. One binary = one profile. Any plugin-mode result recorded in
`docs/` came from a build with these commented out, which is a different binary,
not "the plugin fixing it".

| # | Address | Name | Jump To | Status |
|---|---------|------|---------|--------|
| 6 | 0x82B70EF4 | NativeSkipRendererDispatch | 0x82B70EF8 | **ACTIVE — the only one.** Skips exactly one instruction: `bl sub_82B34998`, LoaderTick's GPU renderer dispatch, which the D3D12 backend replaces |
| 1 | 0x82B70854 | NativeGameTickSkip | 0x82B70874 | disabled — MainLoop vt[36]; eng+8 holds the real AssetDB, no workaround needed |
| 2 | 0x82B71290 | NativeSetupDeviceSkip | 0x82B712A4 | disabled — vt[6] **creates** the scene manager at eng+52 |
| 3 | 0x82B712C4 | NativeSkipVtable8 | 0x82B712D8 | disabled — vt[8] is a harmless 4-byte alloc + vtable install at eng+0x38 |
| 4 | 0x82B71304 | NativeSkipVtable17 | 0x82B71314 | disabled — vt[17] populates eng+8 = AssetDB block |
| 5 | 0x82B71324 | NativeSkipRendererInit | 0x82B71690 | disabled — this 876-byte band runs the real renderer init that creates the sub-entities at eng+0x1C/0x20/0x24 and spawns the guest worker threads |
| 7 | 0x82B70E18 | NativeSkipLoaderEarly | 0x82B70EC8 | disabled — holds the engine/scene/entity block and the vt[6] gate into the asset loader |
| 8 | 0x82B70DFC | NativeSkipLoaderAll | 0x82B70EC8 | disabled — deleted the asset-loader call site entirely |

All 11 export symbols remain in `CMakeLists.txt`, so any of these can be
re-enabled without a codegen round. `NativeSkipLoaderRenderer`
(0x82B70EC8 → 0x82B710BC) is the pre-2026-08-02 form of #6 and is kept commented
in `mx_config.toml`; it deleted the whole ~500-byte renderer block to avoid the
single GPU-bound call that #6 now skips.

`NativePreDispatchLog` and `NativePostLazyInitLog` are unused bisection stubs,
exported and ready to drop into `mx_config.toml`.

---

## Native asset loading

`AssetDB_LoadStateMachine` (`sub_8253AA40`, AssetDB vt[6]) runs every tick,
`LoaderTick` ticks past #500 returning r3=1, and the guest issues real asset I/O
against `game:\Cameras\*.bxml`. No GPU plugin.

The `0xC000000F` failures on PROTruck / Buggy / UTV / PRO2Truck cameras are
**normal** — those files are absent from the dump and the same failures occur
under the GPU plugin.

Getting here was mostly **deletion**. Each of these host-side fakes was a
workaround for a problem an earlier workaround created, and each blocked real
loading:

| Removed fake | Why it blocked loading |
|---|---|
| `eng+8 = eng` self-ref (`hooks_gameloop.cpp`) | Overwrote the real AssetDB; MainLoop's vt[36] then read garbage → AV at guest 0x4D5854F1 |
| Blanket INFINITE-wait success (`hooks_wait.cpp`) | A single process-wide 3s timer made **every** infinite wait a no-op, releasing threads into a half-built registry → racy AV inside `sub_82AFF560` (a `RtlEnterCriticalSection`-guarded registry walk). Flaky: 2 of 3 runs |
| `LoaderTick` r3=0 cap at iter 101 (`hooks_loading.cpp`) | Fabricated "loading complete" and killed the Transition loop mid-load |
| mid-ASM hooks #2 / #5 / #7 / #8 | See the hook table |

Two things are genuinely added on the host side:

- **`sub_82B70370` (timing) is stubbed in native mode.** It busy-waits on
  `*(0x830EC248+20)`, which reads `0x7F7FFFFF` (`FLT_MAX`), and stores through an
  unbounded ring index at `+32`. The stub writes a fixed 1/60 dt to `+24`.
- **MainLoop signals the loader's frame event.** `LoaderTick` opens with
  `Wait(*(tr+0x194), -1)` — handle 0xF80001F0 — a per-frame handshake the guest
  renderer thread would satisfy. Hook #6 skips that renderer, so
  `hooks_gameloop.cpp` calls guest `NtSetEvent` (0x82BFB748) on that handle once
  per frame. Without it the Transition thread parks forever after tick 1.

### The renderer block runs natively

Hook #6 skips only `bl sub_82B34998`. The rest of `0x82B70EC8..0x82B710BC` — the
delta-time load, the `dword_830BE190` lazy-init, the entity loops over
`eng+0x1C..0x24`, the `tr+0x2E0` / `tr+0x190` event handshake, and the closing
`engine[0xC]->vt[3]()` — executes every iteration with no stubs.
`SkipRendererDispatch` reaches #800-900 over a 30s run, zero access violations,
3/3 clean.

Two reads that matter for future work:

| Read | Value | Consequence |
|---|---|---|
| `dword_830BE190` before LoaderTick | `0x40B84F40` — already populated | The lazy-init `bctrl` at `0x82B70EE8` is branched past entirely and never executes |
| `*(dword_830BE190)` (vtable) | **`0x8213F7A4`** | The real vtable, all functions. **Not** the stub `0x8213F70C` whose slots dispatch to the fatal `sub_82BDB190` |

`docs/loader_render_block.md`'s verdict that `sub_82B34998` is "structurally
unreachable" rests on an object carrying `off_8213F70C` — but that object was
pre-populated by hand from the main thread via `sub_82B3C7D0`. The naturally
constructed one is not. The dispatch is still skipped and still unproven, but the
structural argument against it does not hold as written.

## The guest render path

`MainLoop` calls `RenderPipeline` (`sub_82B70578`) only while
`byte_82D57994 != 0` — checked twice, at `0x82B707B0` and `0x82B7080C`; a zero
jumps straight to the vt[36] tail. The only guest write to that byte is in
`Transition`, which writes **0** on loader exit, so our forcing it to 1 in
`hooks_gameloop.cpp` is load-bearing.

**The `skip_intro` cvar is required to see any of this.** The host Bink playlist
is 47.4s (THQ 9.5s + Attract 37.9s, per ffprobe) and the RenderPipeline hook
returns early for its whole duration. Set `skip_intro = true` in `mx.toml` or
pass `--skip_intro=true`.

With that flag, per 30s run: RenderPipeline reaches #600, VdSwap #900, ~17728
bytes of PM4 per swap, 0 access violations, 3/3 clean.

### The PM4 is present-only

Steady-state swaps decode to **10 Type-3 + 12 Type-0 packets, ~13500 Type-2
filler**. The content is display and scanout state:

- `SET_LOOP_CONST` carrying `0x53574150` — ASCII **"SWAP"**
- `DISPLAY_TIMING`, `DISP_TG_CTL`, `DISP_DITHER`, `HW_MODE_TABLE`
- `MC_CTL` / `MC_BASE_ADDR` + `WAIT_REG_MEM` triplets, `EVENT_WRITE_SHD`

**No `DRAW_*` opcodes and no `INDIRECT_BUFFER`** in any captured swap. The
translator correctly reports 0 draw calls; it is not the problem, and the draws
are not hiding in an indirect buffer the parser fails to follow (it does not
follow them — opcode 0x3F is named but never chased — but none are emitted).

Entity counts are `pass0=1 pass1=0 pass2=1`, i.e. a near-empty scene.
`RenderPipeline` iterates those entities every frame and emits nothing
drawable.

**Cause identified 2026-08-02: the loader is idle, not stuck.** It reaches state
2 (`IdleClearRenderBusy`) and parks there — see "Not working / unverified". The
scene is empty because nothing ever asks it to load anything, so `RenderPipeline`
has nothing to draw. The next step is finding what should drive the state into 9
(`LaunchActivity`) or 10 (`SeriesAdvance`), which is front-end/game-flow work,
not loader or GPU work.

### Parser caveat

`Pm4Parser` accepts any Type-3 with `body_word_count <= 0x4000`. A misparsed
header at `0xBEBE0B80` (opcode 0x5D, count 8449) swallows the rest of the ring.
This does not currently hide draws — the swap ring genuinely has none — but it
will need tightening once real command streams are parsed.

### Draw-call plumbing

`VdSwap` → `Pm4Translator` → `NativeGraphics::SetDrawCalls` →
`RenderThreadFunc` → `D3D12Renderer::SetGameDrawData` → `RenderGameFrame`'s
`m_hasGameDrawData` branch. The consumer half was connected 2026-08-02 and is
**untested against real data** — no draw has ever reached it.

`DrawCall::mvp[16]` has **no path into the renderer**. `SetGameDrawData` takes
no matrix, so the first real geometry will render with the placeholder
transform — which looks identical to a broken translator. Fix that before
judging what appears on screen.

### Debugging aids

- **Crash reporter** (`src/app/mx_app.cpp`): `AddVectoredExceptionHandler` logs the
  faulting address, host RIP, RVA, and the translated guest address, then
  `rex::FlushLogging()`. The log silently drops its last lines on a hard fault,
  which made earlier bisects misleading — probes looked like they died before code
  that had actually already run.
- **Symbolizing a crash**: build with `-DCMAKE_CXX_FLAGS_DEBUG="-O0 -gline-tables-only"`
  and `-DCMAKE_EXE_LINKER_FLAGS="-Wl,/MAP:mx.map"`, then look the logged RVA up in
  `mx.map`. llvm-symbolizer does **not** resolve these; the map file does.
- **Guest address ranges**: guest base 0x100000000; heap `0x40xxxxxx`, code
  `0x82xxxxxx`, data `0x83xxxxxx`, GPU allocs `0xBExxxxxx`. A vtable slot outside
  `0x82xxxxxx` is not a function pointer.

---

## C++ Hook Functions

All are gated on `mx::native::g_plugin_mode`; the table describes **native-mode**
behavior.

### Frame Lifecycle
| Hook | Behavior |
|------|----------|
| sub_82566B58 (VdSwap) | Counter sync 0x82D21818 → 0x83144208. PM4 parse + XenosGpuState update for the first 5 swaps; logs swap size / ring wrap at sparse checkpoints |
| sub_82BFBF30 (XenosWait) | Counter sync only |
| sub_8255D430 (BeginFrameXenos) | Stubbed |
| sub_8255D470 (EndFrameXenos) | Stubbed |
| sub_8255D520 (GpuState) | Calls orig for the first 3 calls only, with logging; no-op after |
| sub_82ABF828 (BeginFrame) | Stubbed |
| sub_82ABF930 (EndFrame) | Calls orig every frame (this is what fires VdSwap) |
| sub_8255CFE0 (FramePendingPoll) | Returns 0. Breaks the GPU frame-pending spin loop in VdSwap at 0x82567178 so EndFrame #2+ complete |

### Wait / Events
| Hook | Behavior |
|------|----------|
| sub_82BFB740 (Wait) | `r4 == 500` → return SUCCESS immediately. Everything else, **including INFINITE**, calls orig |
| sub_82BFB748 (NtSetEvent) | Calls orig — loading depends on events actually firing. Logs the first 8 |
| sub_82BFBF48 (ErrorRecovery) | Stubbed |

### Game Loop
| Hook | Behavior |
|------|----------|
| sub_82B70760 (MainLoop) | Sets `byte_82D57994=1`, clears at frame 600. Signals the loader's frame event `*(0x830EC248+0x194)` via guest NtSetEvent. Calls orig, forces r3=1, `Sleep(16)` |
| sub_82B70578 (RenderPipeline) | Calls orig every frame. Skipped while Bink plays (host render thread owns the swapchain) |

### Loading
| Hook | Behavior |
|------|----------|
| sub_82B71148 (SetupRenderer) | Calls orig. Replicates vt[17]'s `*(eng+8) = AssetDB_block` **only if** eng+8 is still NULL afterwards — with hook #4 disabled it is not, so this path is dormant |
| sub_82B710D0 (Transition) | Read-only state dump (engine, scene manager, sub-entities, event handles, `dword_830BE190` + vtable), then calls orig |
| sub_82B70DE8 (LoaderTick) | `REX_HOOK_RAW` + call orig. No cap, no forced result — logs iterations 1-5 and every 500th |

### Diagnostics (`hooks_plugin_diag.cpp`)
| Hook | Behavior |
|------|----------|
| sub_82B70370 (Timing) | **Stubbed in native mode** — writes 1/60 to `*(a1+24)` instead of busy-waiting on `FLT_MAX` |
| sub_8253AA40 (LoadStateMachine) | Logs entry/exit in both modes, then calls orig |
| sub_82B34998, sub_82B3C7D0, sub_82B6D230, sub_82B38558 | Plugin-mode logging; pass through in native mode |

### Boot / Cleanup / Keep-Alive
| Hook | Behavior |
|------|----------|
| sub_82ABB838 (Bootstrap) | Log entry |
| sub_82AEBF40 (GraphicsInit) | Log device state at +56/+104/+2388, gpu_phys after init |
| sub_82AE9658 (PostGfxInit) | Call orig |
| sub_82373660 (TexManager) | Call orig |
| sub_82B6F820 (BindTexture) | Call orig |
| sub_82AB7848 (GpuAlloc) | Log first 8 allocs: size -> address |
| sub_82533D80 (Cleanup1) | Zero dword_830577C0 |
| sub_82B70BE8 (Cleanup2) | Stubbed: zero dword_830BE190 |
| sub_82BA7F58 (EngineInit) | SetGuestMemory(base), call orig, then sleep loop to keep the process alive |

---

## GPU Pipeline (PM4 Analysis)

### PM4 Parser (`src/gpu/pm4_parser.h/.cpp`)
- Decodes Type-0 (16-bit reg base + 14-bit count), Type-2 (NOP), Type-3 (14-bit opcode)
- **Big-endian byteswap**: all guest dwords must be `_byteswap_ulong()` before parsing
- 30 named opcodes (NOP, INDIRECT_BUFFER, WAIT_REG_MEM, DRAW_INDEX*, SET_CONFIG_REG, SET_CONTEXT_REG, SET_ALU_CONST, SET_RESOURCE, SET_SAMPLER, etc.)
- 65 register names
- Validate: Type-0 count ≤ 256, base ≤ 0x30000; Type-3 count ≤ 128, opcode ≠ 0x3FFF
- Sentinel filtering: 0xDEADBEEF, 0x77777777, 0x00000000
- Ring buffer wrap handling
- DumpPackets writes decoded packets to `pm4_dump.txt`

### Command Buffer
- Location: `device + 48` (write) / `device + 52` (end), ring buffer; base at `device + 44`
- ~64KB per VdSwap
- Parses the first 5 VdSwap calls in native mode (15055 packets: 289 Type3, 819 Type0, 13947 Type2)

### PM4 Header Format
```
Type-3: [31:30]=3, [29:16]=IT_OPCODE, [15:8]=sub, [7:0]=COUNT
Type-2: [31:30]=2, rest=filler
Type-0: [31:30]=0, [29:0]=register base
```

### Guard Patterns
- 0xDEADBEEF: PM4 sentinel
- 0x77777777: Fill
- 0x80000000: Type-2 NOP

### Xenos GPU State (`src/gpu/xenos_gpu_state.h/.cpp`)
- Tracks 66 named Xenos registers in `regs_` map
- `ApplyType0Write(reg_base, data, count)`: writes count consecutive registers
- `ApplyType3Packet(pkt)`: handles SET_CONFIG_REG, SET_CONTEXT_REG, SET_ALU_CONST, etc.
- `Snapshot()` / `DumpDiff()`: captures register snapshot, diffs vs previous
- 32 registers captured from the first VdSwap: resolution (720x1280), GPU base addr, shader constants

---

## Current State

### Working
- 60fps game loop (MainLoop `Sleep(16)`, r3 forced 1)
- Bink intros (THQ Logo → Attract) via FFmpeg + SDL audio, host-side
- SetupRenderer completes with **no** mid-ASM hooks skipping any of it
- **Guest render path runs**: RenderPipeline every frame, VdSwap climbing to #900
  in a 30s run (~17.7KB of PM4 per swap). Requires `skip_intro` — see below
- **3D game pipeline**: colored triangle via game PSO, game RT + depth, PresentGameFrame copy to swapchain
- **PM4 parser**: 15055 packets decoded from the first 5 VdSwaps, ring wrap handled, big-endian byteswap, validated
- **GPU state tracking**: 66 Xenos registers shadowed, 32 captured from the first VdSwap
- **Input**: SDL gamepad + Xbox 360 Controller, ReXGlue handles XamInput natively
- **Asset loading**: LoadStateMachine ticks every LoaderTick, real file I/O reaching the VFS
- **LoaderTick renderer block**: runs natively apart from the one skipped dispatch
- EngineInit sleep loop keeps the process alive

### Not working / unverified
- **Game rendering**: no guest draws reach the screen. The guest emits PM4 every
  frame but it is **present-only** — see "The guest render path" below
- **The loader is idle, not stuck.** The state is `*(AssetDB + 28)` (derived from
  `mx_recomp.31.cpp:36836`; the old `+110796` was a heap pointer). It runs
  `0 -> 1` on call #1, `1 -> 2` at call #59, then parks in **state 2
  `IdleClearRenderBusy`** for the remaining ~750 calls — identical across 3/3
  runs. State 2's body is `*(a1+110328) = 0` then the common tail; it never
  writes the state, so it cannot self-advance. State 1's `SceneTransition_Kickoff`
  (`0x82538618`) does fire. **Nothing ever requests the next load** — confirmed
  at runtime, see `force_load` below
- **Entity population**: `pass0=1 pass1=0 pass2=1` for the whole run, in every
  configuration measured so far including the forced load
- **Game input consumption**: guest never calls `XamInputGetState`
- **No menu/gameplay state** reached — and the entire front-end call chain that
  would request a load is silent, see below

#### The load-request chain, and `force_load` (2026-08-02)

**`sub_82534980(AssetDB, name, flags)` is the guest's load-request API.** It
`strncpy`s up to **260 bytes** of `name` into `AssetDB+29540`
(`sub_82AB4AB0(a1+29540, r4, 260)`, `mx_recomp.31.cpp:22377`), stores `flags` at
`+29800`, and **if `*(AssetDB+28) == 2`** sets the selector to **3** and notifies
the listener at `*(a1+110788)`.

So `AssetDB+29540` is a `MAX_PATH` **string buffer, not a flag** — the five places
that appear to test a boolean are testing `name[0] != 0`, "is a load pending", and
all of them pick state 2 when it is empty. The only other writer is the
constructor `sub_8253CB38` (`:41615`), zeroing it.

**Correction to `8b396bf`**: that commit described state 4 as aborting via
`sub_825378F0`. It does not. `sub_825378F0` (`:29489`) is state 4's *normal*
completion — it clears `+110328`, then routes to state **3** if a name is pending
or **2** if not. The `4 -> 2` measured there was the machine correctly handling an
empty queue.

**The chain never runs.** `sub_82534980` has exactly one caller, `sub_82352AE0`
(`mx_recomp.15.cpp:76710`), which resolves the scene name from a registry lookup
and is a method with five callers (`sub_82367A50`, `sub_8236B470`, `sub_8236B660`,
`sub_824FB1F0`, `sub_824FC9A0`). All seven are hooked in
`src/hooks/hooks_plugin_diag.cpp`. In a 40s native run **not one of them fires** —
not even the five top-level callers. The front-end code that would ask for a load
is never entered at all.

**`force_load = "<scene>"`** (`mx.toml` or `--force_load=<scene>`, empty = off)
calls that API once, 30 ticks after the loader settles into state 2, with the name
placed in a scratch buffer carved from the guest stack. Measured with
`--skip_intro=true --force_load=NAT_Farm`, identical across 3/3 runs, zero access
violations:

```
force_load "NAT_Farm" at call #89  a1=0x407F2190 state=2
RequestLoad(sub_82534980) name="NAT_Farm" flags=0x00000000 state=2
RequestLoad returned — state 2 -> 3
LoadStateMachine #90   state 3 -> 4    (state 4 runs ~390 ticks)
LoadStateMachine #479  state 4 -> 5
LoadStateMachine #528  state 5 -> 6
                       parks in 6
```

The ordered sequence works: **3 `DatabaseLoad` → 4 `SubsceneCreate` → 5
`LoadingProgress` → 6 `PlayerSetup`**, then parks. State 4 occupying ~390 ticks
(~6.5s) is the first evidence of the loader doing sustained real work.

Still unmoved at state 6: entity counts `pass0=1 pass1=0 pass2=1`, zero `DRAW_*`,
zero `INDIRECT_BUFFER`, and no file I/O for the requested scene. That last point
matters — states 7/8 are where the async content load happens, and we park before
reaching them, so "no NAT_Farm files opened" does **not** yet mean the name is
wrong.

#### What state 6 is waiting on — resolved (2026-08-02)

**Correction**: an earlier note here offered the "NetworkNoPlayers" / per-player
UniqueId reading as the likely state 6 gate. That is wrong — the per-player loop
at `loc_8253B5F0` and the `state = 7` write at `loc_8253B694` are both
*downstream* of the gate and are never reached.

State 6 (`loc_8253B504`) reads `*(a1+110328)`, finds it zero, and goes to
`loc_8253B560`, which calls `(*(AssetDB+110788))->vt[2]()`. A **zero return jumps
to `loc_8253B6A4`** — an early return that leaves the selector at 6. That single
predicate is the whole blocker.

Runtime dump (identical 3/3): listener `+110788` = `0x40B76700`, vtable
`0x8204C014`, `vt[0]=0x8253CF60 vt[1]=0x82474838 vt[2]=0x8253CF80
vt[3]=0x8253D020` — all four in `0x82xxxxxx`, so all real code.

`sub_8253CF80` is:

```
mode = sub_82536250(*(0x830577C0));   // registry string -> enum, arg is the AssetDB
if (mode == 2 || mode == 3) return 1;
if (*(0x83057900) != 0)     return 1;
tmp = 0; sub_82548758(registry, <key>, &tmp, 0); return tmp;
```

All three terms measured, identical across 3/3 runs, zero access violations:

| Term | Measured | Needs |
|---|---|---|
| `sub_82536250(AssetDB)` | **4** (stable, 1500+ calls) | 2 or 3 |
| `*(0x83057900)` | **0** | non-zero |
| registry fallback | **0** | non-zero |

So the gate returns 0 forever. Note **state 1 clears `*(0x83057900)` itself**
(`stw r25,30976(r8)`, `r25 = 0`), so boot closes the second escape.

This is a **game-mode** gate, not a loader or player-signin one. Mode 4 is
whatever the registry currently says; the loader will only proceed in modes 2/3.

**Also corrected**: `lis r11,-31995` is `0x83050000`, not `0x830A0000` as first
recorded — so `sub_82352AE0` and the gate both read the familiar AssetDB global
`dword_830577C0`, confirmed at runtime by `GateMode` logging `a1=0x407F2190`.
The earlier claim that they used a different, null global was an arithmetic
error on my part.

`force_load` is diagnostic. It substitutes for a front end that never runs; it is
not a step toward correct behaviour.

### Known external blockers
- Binary `.xenon.package` heaps are encrypted (entropy ≈7.98, unknown routine; the OpenSSL AES bundle in the guest is TLS-only)
- FATAL crash at 0x82327CF0 during gameplay (separate known issue)

---

## Deep Technical Specs

Investigation logs and post-mortems live in separate docs to keep this file
operational.

| Doc | Contents |
|-----|----------|
| `docs/loader_render_block.md` | LoaderTick renderer block analysis, eng+8 writer trace (sub_82B43AC8 / vt[17]), EndFrame #2 hang root cause + fix (sub_8255CFE0). **Its PATH 1 EXPERIMENT cascade and `sub_82B34998` "fatal terminator" post-mortem are superseded** — see the inline notes in that file |
| `docs/asset_format.md` | BXML format + reconstruction status, .xenon.package layout, per-asset-type heap headers, AssetDB_LoadStateMachine 12-case state machine, asset loader infrastructure, package encryption findings, shader microcode load path, asset catalog (23,183 assets), decoder tools |
| `docs/pm4_pipeline.md` | PM4 parser (bit fields per Xenia, opcode table), Xenos GPU state shadow, plugin-mode data capture, gameplay PM4 frame structure, plugin cvar list, native backend strategy, PM4 translator status (auto + indexed draws, IB extraction blocker), ReXGlue SDK + Xenia reference files |
| `docs/ida_notes.md` | Consolidated IDA bookmark table (slots 0-28) by theme: LoadStateMachine states, asset loader, shader microcode, OpenSSL AES, eng+8 writer |
