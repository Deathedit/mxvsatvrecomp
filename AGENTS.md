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
| `src/gfx/d3d12_game.cpp` | Game pipeline (triangle PSO / translated draws), the per-frame `m_gameDraws` list, game RT + depth, PresentGameFrame copy |
| `src/gfx/d3d12_shaders.h` | HLSL source for both pipelines |
| `src/gfx/d3d12_internal.h` | LogError/LogInfo/CompileShader shared by the three gfx TUs (internal) |
| `src/gfx/bink_player.h/.cpp` | FFmpeg Bink video + audio decoder |
| **gpu** | |
| `src/gpu/pm4_parser.h/.cpp` | PM4 command buffer parser: Type-0/2/3 decode, 30 opcodes, 65 reg names, ring wrap, dump |
| `src/gpu/pm4_translator.h/.cpp` | PM4 -> `DrawCall` translation. Shadows the fetch constant file (0x4800..0x48BF) and the context registers (0x2000..0x2FFF) from Type0 writes, infers vertex stride, reads buffers through the physical-address aliasing windows, builds the viewport transform, expands RectangleList |
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

### The PM4 was never present-only — we were reading the wrong bytes (2026-08-02)

**Every "zero `DRAW_*`" result recorded before 2026-08-02 in this file is void.**
They were not measurements of the guest's command stream. Two defects in the
`VdSwap` hook meant no run had ever parsed a range that could contain a draw:

1. **The wrong range.** The hook captured `write_before` at VdSwap entry and
   `write_after` at exit and parsed `[before, after)` — which by construction is
   only what VdSwap itself emits. That is why every dump showed
   `SET_LOOP_CONST` carrying `0x53574150` (ASCII **"SWAP"**), `DISPLAY_TIMING`,
   `DISP_TG_CTL`, `MC_BASE_ADDR` + `WAIT_REG_MEM`, and ~13500 Type-2 filler. It
   is a present sequence. A draw could not have appeared there.
2. **Five swaps.** `parse_limit` was 5 in native mode; every later checkpoint
   (300 / 600 / 1000, `>= 1200`) was `is_plugin`-gated. The load completes around
   swap ~600, so native mode stopped looking long before anything loaded.

The bytes being skipped were visible in the logs all along. Swap #3 started at
`0xBEBB325C` and wrote 54624 bytes, ending at `0xBEBC07BC` — but swap #4 started
at `0xBEBC34DC`. **`0x2D20` = 11552 bytes per frame**, written by the guest
between swaps, identical for #2→#3, #3→#4 and #4→#5, never parsed.

#### The two ranges

`hooks_frame.cpp` now parses both, labels them separately, and feeds both to one
translator in write order:

| Range | Span | Content |
|---|---|---|
| **frame** | `[prev_after, write_before)` | everything the guest wrote since the last swap — the actual frame |
| **swap** | `[write_before, write_after)` | what VdSwap emits — the present sequence, all that was ever parsed before |

Native mode parses **every** swap. `ApplyPackets`/`XenosGpuState`, the
`pm4_dump_*` files, and the opcode histograms stay on the old sparse schedule;
the first swap that yields a non-zero draw count logs unconditionally.

#### Ring layout, established empirically

The struct fields the old wrap arithmetic used are not what it assumed —
`dev+44` reads `0x00000000` and `dev+52` reads `0xBEBA0000`, but packets parse at
`0xBEBB3260` and the pointer runs to `0xBED7FD7C`, so `ring_size = end - base`
was garbage. Measured from the pointer itself over an 841-swap run:

| | |
|---|---|
| base | ~`0xBEB90000` (post-wrap `write_after` is `0xBEB9003C`) |
| end | ~`0xBED80000` (last pre-wrap `write_before` is `0xBED7FD7C`) |
| size | ~`0x1F0000` (1.875 MB) |
| wrap cadence | every ~30 swaps |

These are inferred from observation, not read from a field. The hook therefore
**skips the frame range on a wrap** rather than parse from guessed bounds — one
frame in thirty, which changes nothing about the result. Do not hardcode the
numbers above into wrap arithmetic without finding the real fields first.

#### What is actually in the frame range

Measured with `--skip_intro=true --force_load=NAT_Farm
--registry_override=ReadyToLaunch=1`, identical 3/3 (logs `mx_041/042/043`),
zero access violations:

| | boot, no forcing (`mx_040`) | after the load completes |
|---|---|---|
| frame packets | 305 | ~10168 |
| draw calls / frame | 3–7 | **350–363** |
| `DRAW_INDX` (0x22) | 1 | **453** |
| `DRAW_INDX_2` (0x36) | 2 | **62** |
| `INDIRECT_BUFFER` (0x3F) | 6 | 20 |

So geometry is emitted, it scales ~50x with the load, and it is **inline, not
behind indirect buffers** — the translator's refusal to chase opcode 0x3F is not
what is hiding it. Draws appear from swap #2 even with the loader parked, which
is why this needed a control run to separate "the load produced geometry" from
"the ring always had geometry."

#### The vertex fetch constants, and where they actually live (2026-08-02)

Resolved. Two separate defects, both in `pm4_translator.cpp`, and the second was
not visible until the first was fixed.

**1. The constants arrive as Type0 writes, and the translator threw them away.**
`TranslatePackets` opened with `if (pkt.type != PacketType::Type3) continue;`,
and the only place `m_vtxBufAddr`/`m_vtxStride` were ever set was
`HandleSetConstant` for `SET_CONSTANT` (0x2D) type=1 — **which this game never
emits**. The post-load Type3 histogram is `0x22 0x27 0x2B 0x2F 0x36 0x3B 0x3C
0x3F 0x46 0x58 0x60`, no 0x2D anywhere. The translator now shadows registers
`0x4800..0x48BF`, the 192-dword shader fetch constant file, from Type0 writes
(241 writes to `0x4800` and 200 to `0x48BA` per post-load frame).

Layout, per `xe_gpu_vertex_fetch_t` — two dwords, `type` distinguishing them
from the 6-dword texture fetches sharing the file:

| dword | fields |
|---|---|
| 0 | `type [1:0]` (3 = vertex, 2 = texture), `address [31:2]` |
| 1 | `endian [1:0]`, `size [25:2]` in dwords |

`XenosGpuState`'s register table called `0x4800` **`HW_MODE_TABLE`**, which made
every vertex fetch in every dump look like display state. Corrected to
`SHADER_FETCH_CONST`.

**2. GPU addresses are physical; the committed pages are in the aliasing
window.** With the constants decoding correctly, all 27199 vertex reads in a run
still failed the page-commit probe. The fetch constants name `0x1EBB02BC`, but
what ReXGlue has committed is `0xBEBB02BC` — exactly `phys | 0xA0000000`, and
inside the same span the PM4 ring itself occupies. The bare physical page is
reserved-but-uncommitted. `ReadGuestRange` now tries the bare address, then the
`0xA0000000` / `0xC0000000` / `0xE0000000` windows (with
`PhysicalHostOffset`'s `+0x1000` above `0xE0000000`), and logs which one
resolved. **Every buffer in every run resolved via `0xA0000000`.**

This also applied to the index-buffer path, whose comment asserted
"host_ptr = guest_mem_base + addr (no masking)". That was wrong for anything the
GPU points at. Both paths now share `ReadGuestRange`, including the commit probe
— which must never `VirtualAlloc`, see its comment.

**Stride is not in the fetch constant.** On Xenos it lives in the shader's
`vfetch` instruction. It is inferred as `size_bytes / vertex_count`, accepted
only on an exact division landing in **8..64 bytes**. Slot choice is the
lowest-indexed slot that validates, and an ambiguous pick is logged (6 per run).

#### What came out

`--skip_intro=true --force_load=NAT_Farm --registry_override=ReadyToLaunch=1`,
3/3 identical, zero access violations (logs `mx_048/049/050`; `mx_051` is the
boot-only control):

| | control | forced |
|---|---|---|
| draws carrying vertex data | 6244 / 6244 (100%) | 28652 / 46364 (**61%**) |
| stride distribution | 28, 8, 12 | 8 (7164), 28 (3339), 16 (3259), 38, 12, 64, 44 |
| topology | — | `prim=8` rect (10914), `prim=13` (3837), `prim=6` fan (107) |

The first accepted buffer decodes exactly as it should — slot 0,
`0x1EBB02BC`, stride 28, `prim=8` RectangleList:

```
v[0] -0.500  -0.500  1.000 | 0.504 0.504 0.504 1.000
v[1] 639.500 -0.500  1.000 | 0.504 0.504 0.504 1.000
v[2] 639.500 359.500 1.000 | 0.504 0.504 0.504 1.000
```

Screen-space positions at 640x360 with a grey RGBA — `pos.xyz` (12 B) +
`color.rgba` (16 B) = the inferred 28. The stride inference, the endian swap, the
slot choice and the window resolution are all confirmed by that one dump.

**`RenderThread: first translated draw` fires** — "3 verts (84 B, stride 28), 3
indices". The draw-call bridge and the `DrawCall::mvp` path, both untested since
they were written, have now carried real data.

Open, and deliberately not chased in that round:

- **The 39% with no vertex data have no recorded reason.** The candidate logging
  is capped at the first 20 draws, so the rejection breakdown past that is not
  instrumented. Only 26 `stride out of range` were logged.
- **Stride inference is a heuristic.** A buffer shared across draws yields a
  multiple of the true stride. The 84-byte dump validates one case, not the
  7164 draws at stride 8.
- **Cost.** Forced runs reach `MainLoop #421` against `#601` before the change;
  the control reaches `#721` against `#841`. ~30% post-load, ~15% at boot, from
  the per-draw `VirtualQuery` and copy.
- The real fix for stride and vertex format is the shader microcode — `IM_LOAD`
  (0x27, 363/frame) and `IM_LOAD_IMMEDIATE` (0x2B, 70/frame) carry the `vfetch`
  instructions. That is a disassembler and a round of its own.

#### The zero matrix, and the guest's own viewport (2026-08-02)

With vertex data resolved, nothing still reached the screen. The cause was not
the vertex data and not a bad guess about the transform — **there was no
transform**. `Pm4Translator::m_mvp` was written in exactly two places,
`HandleSetConstant` (`SET_CONSTANT` 0x2D type=0) and `HandleSetShaderConstants`
(0x56). Neither opcode occurs in any captured frame, and no Type0 write anywhere
in the dumps touches `0x4000..0x41FF`, the ALU float constant file. So `m_mvp`
was **identically zero**, memcpy'd into every `DrawCall`, bound in preference to
the identity fallback, and multiplied into every vertex by `kGameVS`. Every
translated vertex collapsed to the origin. No vertex data, however correct, could
have produced a pixel.

Every earlier note reading "geometry is off-screen, the MVP guess is the first
suspect" is **void** on that basis — the matrix was not a guess that happened to
be wrong, it was zero.

> **Amended 2026-08-02.** The two facts above are correct — no `SET_CONSTANT`,
> no `SET_SHADER_CONSTANTS`, no Type0 write to `0x4000..0x41FF` — but the
> conclusion drawn from them, that the game never writes the ALU constant file
> at all, is **wrong**. It writes it through a third door: `LOAD_ALU_CONSTANT`
> (0x2F), ~44000 times a run, from guest memory. That does not rescue `m_mvp` —
> what arrives is per-shader scalar constants, not a transform, and only eight
> vec4 registers of the file are ever touched. See "The ALU constants are not
> the world matrix".

**The replacement is read from the guest, not inferred.** The viewport registers
arrive in every frame as the tail of a cnt=21 Type0 write to `0x2100`:

| index | register | boot | post-load |
|---|---|---|---|
| `0x210F` | `PA_CL_VPORT_XSCALE` | 640 | 384 |
| `0x2110` | `PA_CL_VPORT_XOFFSET` | 640 | 384 |
| `0x2111` | `PA_CL_VPORT_YSCALE` | -360 | -512 |
| `0x2112` | `PA_CL_VPORT_YOFFSET` | 360 | 512 |
| `0x2113` | `PA_CL_VPORT_ZSCALE` | 1.0 | 1.0 |
| `0x2114` | `PA_CL_VPORT_ZOFFSET` | 0.0 | 0.0 |

1280x720 and a 768x1024 offscreen pass. `PA_SC_WINDOW_SCISSOR_BR` at `0x2082`
independently reads `0x02D00500` = 1280x720, and `RB_SURFACE_INFO` at `0x2000`
carries pitch 1280 / 800. Three registers agree.

The guest's vertices are already in window coordinates, so the transform is the
**inverse** of `window = ndc * SCALE + OFFSET`. `BuildViewportMvp` builds it
row-major; `kGameVS` declares its cbuffer matrix `row_major` to match, because
HLSL packs a cbuffer `float4x4` column-major by default and would otherwise
transpose it silently. `YSCALE` is negative and that negation is the only thing
flipping window-y-down to NDC-y-up — there is no second flip.

`Pm4Translator` now shadows the whole context register block `0x2000..0x2FFF`
alongside the fetch constant file, both fed from Type0 writes.

**The register name table was scaled wrong.** `xenos_gpu_state.cpp`'s `kRegNames`
used byte offsets from `0x2000` up where PM4 Type0 `reg_base` is a *dword* index,
so every entry named a register four slots from the one it labelled — `0x2080`
printed as `RB_DISP_OUTPUT` when its observed value makes it
`PA_SC_WINDOW_OFFSET`. This is why almost every Type0 line in every dump read
`???`. Those entries are deleted rather than rescaled; only names confirmed
against an observed value remain.

**Topology.** `prim_type` was parsed and then discarded — the renderer hardcoded
`TRIANGLELIST`. `DrawCall::topology` now carries a `HostTopology` (deliberately
the `D3D_PRIMITIVE_TOPOLOGY` values, `static_assert`ed in `d3d12_game.cpp`).
`PrimitiveType` in `pm4_translator.h` had **fan and strip swapped** against
Xenia's ordering — fan is 5, strip is 6 — which would have drawn every fan as a
strip. RectangleList (`prim=8`) has no D3D12 equivalent and is expanded in the
translator: each group of 3 vertices is a rect whose implied 4th corner is
`v3 = v0 + v2 - v1`, emitted as 6 indices.

**Batching.** `SetGameDrawData` held exactly one draw, and `graphics_system.cpp`
`break`'d after the first — so however many draws a frame produced, at most one
could ever be submitted. Replaced by `AddGameDraw`/`ClearGameDraws` over a
`std::vector<GameDraw>`, capped at 256/frame.

#### What came out — geometry on screen

3/3 forced runs (`mx_052`, `mx_056`, `mx_057`) plus a boot-only control
(`mx_055`), zero access violations in all four.

The NDC instrument is byte-identical across all three runs and the control:

```
window(  0.00,   0.00, 1.00) -> clip(-1.0000, 1.0000, 1.0000, 1.0000)
window(640.00,   0.00, 1.00) -> clip( 0.0000, 1.0000, 1.0000, 1.0000)
window(640.00, 360.00, 1.00) -> clip( 0.0000, 0.0000, 1.0000, 1.0000)
```

A 640x360 rect occupying the top-left quadrant of a 1280x720 target — exactly
what the viewport arithmetic predicts. **And that is what appears on screen**: a
solid grey quad in the top-left quarter of the window. This is the first native
guest geometry MX has rendered.

| | control | forced |
|---|---|---|
| submitted / skipped per frame | 5 / 3 | 85-95 / 231-234 |
| draws carrying vertex data | — | 57067-88754 of 96629-148054 (~59%) |
| skipped strides (cumulative) | none | 36: ~12500, 16: ~4000, 12: ~420, 20: ~25 |
| `MainLoop` reached | #661 | #601, #601, #661 |

`prim_type` distribution post-load: **13 (QuadList) 47607**, 6 (TriangleStrip)
31271, 8 (RectangleList) 17654. QuadList is the plurality and currently has no
host topology, so it is counted and dropped.

Viewports change per render pass within a frame — 1280x720, 768x1024, 640x720,
320x360, 256x256, 129x129, 64x64, 1x1 — so a draw's transform is only right if
the register shadow is current when that draw is translated. It is, because both
are fed in ring order.

Open, and deliberately not chased in that round:

- **The screen is only correct early.** Screenshots at t+22s show the grey quad;
  at t+32s and t+70s the window is black. Later draws almost certainly overpaint
  it, but that is not proven.
- **The game render target is never cleared.** `BeginFrame`'s
  `ClearRenderTargetView` sits in an `else` branch that stops firing once
  `m_hasGamePipeline` is true, so content accumulates across frames. This is
  visible in the control screenshot as the placeholder triangle and the guest
  quad both present at once, from different frames.
  *(Fixed 2026-08-02 — see "The black screen is drawn, not absent". The reading
  of the control screenshot above was itself wrong: the two were not from
  different frames of one animation but from alternating frames, one of which
  drew only the placeholder. See "The placeholder triangle was drawn on 1 host
  frame in 5".)*
- **Only stride 28 is submitted** — the one layout the game PSO's input layout
  (`POSITION` float3 @0, `COLOR` float4 @12) describes and the only one the
  vertex dump validated. Everything else is counted, not drawn. Stride 36 is the
  largest skipped group at ~12500 per run.
- **The stride heuristic produces garbage for some draws.** `prim=13` vertices
  log window positions around `1e20` and NaN. They are gated out by the stride
  check, not by anything that understands them.
- QuadList, the plurality topology, is dropped. Expanding it is 6 indices per 4
  vertices with no synthesized vertex — cheap, but it belongs with the vertex
  format work, since its stride is not 28.

#### The former blocker: no vertex data (resolved above)

**Every one of those draws is `src_sel == 2`, an auto-draw** — still true, and
still the reason indices are synthesized. What has changed is that the translator
now resolves the vertex fetch constant, so `DrawCall::vertices` is populated for
61% of them and `RenderThread: first translated draw` fires. Vertex counts are
real mesh sizes (24, 36, 40, 48, 52, 56, 60, 64, 68 at `prim=13`, plus `prim=8`
rectangles that are UI), and `m_vtxStride`'s hardcoded 32 fallback is no longer
what feeds the renderer.

Only 4 draws in a whole run carried an index-buffer address, and all 4 were
rejected as out-of-range — by the same wrong physical-address assumption that
blocked the vertex path, since fixed.

Cost: the forced runs reach `MainLoop #601` in 35s versus `#841` for the
boot-only control. Most of that is the load succeeding — the frame range grows
from 305 to ~10168 packets, 33x the parse work — not instrumentation overhead
per se, but if it becomes a problem the parse cadence is the dial.

#### The black screen is drawn, not absent (2026-08-02)

`BeginFrame`'s `ClearRenderTargetView` sat in the final `else` of a three-way
chain whose middle arm tested `!m_gameDraws.empty() || m_hasGamePipeline`.
`CreateGamePipeline` sets `m_hasGamePipeline` true and `Initialize` hard-fails
if it does not, so in any renderer that started successfully the middle arm
always won and **the clear was dead code**. `m_gameRT` had been accumulating
every frame ever drawn.

This voids the interpretation of every screenshot taken before this date. The
boot-only control that appeared to show the placeholder triangle and a guest
quad simultaneously was showing two different frames at once; with the clear
restored it shows the dark-blue clear and the quad, and no triangle.

The clear is now unconditional, and `--clear_magenta=true` swaps it for magenta.
That flag immediately settled the open question of why the screen goes black
after ~30s:

| | t+8s | t+25s |
|---|---|---|
| forced, magenta clear | magenta background, grey quad top-left | **entirely black** |

The magenta clear demonstrably works at t+8. At t+25 nothing of it survives —
while the log shows 85–95 draws still being submitted every frame. **The guest
is actively painting the whole target black; the black screen is drawn, not
absent.** It was never accumulation and never a missing clear. What is on screen
is the guest's own clear geometry, with the content that should follow it landing
in the ~230 draws/frame the stride-28 gate rejects.

#### The placeholder triangle was drawn on 1 host frame in 5 (2026-08-02)

Reported by the user immediately after the clear landed: the placeholder
triangle still appears, flashing. A screenshot samples one frame, so stills had
shown it gone; only the eye catches a strobe. Two independent mechanisms:

`RenderThreadFunc` (`src/app/graphics_system.cpp`) ticks on a fixed 16ms sleep
while the guest swaps at its own rate, and `GetDrawCalls` moves-and-clears. A
tick that lands between two guest swaps therefore got an empty list, called
`ClearGameDraws` anyway, and `RenderGameFrame` fell back to the placeholder
triangle — by design, per the old comment on `ClearGameDraws`, "rather than
replaying stale geometry". Second, a tick whose draws were *all* rejected by the
stride-28 gate blanked the list just as effectively.

**Measured: 501 host ticks with new draws to 120 without, consistent across
3 runs (120 / 102 / 98).** So roughly one host frame in five had no geometry —
enough to read as a flash, and it had been happening all along. It was invisible
only because the render target accumulated: the triangle piled onto geometry
that never went away, which is exactly what made the pre-fix control screenshot
look like "two frames of one animation at once". That earlier reading was wrong.

Both are fixed. `ClearGameDraws` is now called only when the new frame has at
least one submittable draw, so an empty or fully-skipped tick re-presents the
last good frame; and `m_hasEverDrawnGame` latches on the first `AddGameDraw`,
retiring the placeholder permanently once the guest has drawn anything. Draw
counts are unchanged (85–89 submitted / 231–232 skipped, `MainLoop` #601,
`LoaderTick` #500, 0 AVs across 3 runs).

Two lessons, both general: **a still cannot disprove a strobe**, and this render
thread is decoupled from the guest's frame rate, so anything keyed to "this
frame" must tolerate ticks with no guest frame in them.

#### Every render pass is flattened into one target (2026-08-02)

Reported by the user right after the flashing was fixed: the window no longer
flashes, but now cycles through different colours in sequence.

Nothing about which guest surface a draw targets ever reached the renderer.
`Pm4Translator` reads only the `PA_CL_VPORT_*` registers; `RB_COLOR_INFO`
(0x2001) and `RB_SURFACE_INFO` (0x2000) sat unused in the wholesale
0x2000..0x2FFF shadow. So every draw went into the same D3D12 target regardless
of the pass it belonged to, and whichever pass painted last decided the image.

`LogSurface` now counts them. **A run touches 16 distinct colour surfaces**
across three base addresses:

```
2D0/1280 : 62884 draws      000/160 : 4181     000/640 : 560
2D0/800  : 34071            000/80  : 2582     000/400 : 606
2D0/160  : 27001            000/0   : 1025     000/320 : 552
2D0/640  :  3384            5A0/640 :   40     000/800 : 606
2D0/80   :  2490            2D0/320 :   16     000/1280, 000/2609 : 1 each
```
(base in 4KB tiles from `RB_COLOR_INFO[11:0]`, pitch from
`RB_SURFACE_INFO[13:0]`; format was 0 for all 16.)

This is very likely also the real mechanism behind "the guest paints the target
black": an auxiliary buffer that clears to black overpaints the main scene,
rather than the scene itself being black.

**A pitch-1280 gate was tried and made things worse — do not retry it.**
Submitting only draws whose surface pitch matched the 1280x720 output dropped
submitted draws from 85-89/frame to **5**, because the stride-28 draws we can
actually render mostly do *not* live on the pitch-1280 surface — its 62884 draws
are overwhelmingly strides the stride gate rejects anyway. It also destabilised
the run: **2 of 4 gated runs took an access violation on the translator thread
during load, against 0 of 4 ungated**, most plausibly because a render thread
doing 5 draws instead of 85 (each costing three `CreateCommittedResource`) spins
far faster and shifts the timing of a known race. The cvar `main_surface_only`
is kept, defaulted **false**, with that note attached.

The mechanism is right and the selector was wrong. The real fix is to honour the
surface binding — render passes into separate targets and present the one the
guest actually swaps — which is a considerably larger job than a filter, and
needs the shader/vertex-format work first so that the main pass has more than 5
drawable draws in it.

#### The stride is readable, and the heuristic is 96% right (2026-08-02)

The vertex layout is now read out of the shader instead of guessed.
`src/gpu/shader_ucode.cpp` walks the control flow of a vertex shader blob and
enumerates its vertex fetches; `tools/ucode_test.cpp` pins it to two real
captured shaders. Both microcode doors are handled: `IM_LOAD_IMMEDIATE` (0x2B,
inline in the ring) and `IM_LOAD` (0x27, from guest memory — its addresses
resolve through the existing `ReadGuestRange` window search, which had been
flagged as a risk and was not one).

**Binding model confirmed.** The last vertex shader loaded before a draw is the
one it uses. Over 85000 draws per run, `no shader` was **1** — every draw but
one had a decoded shader bound.

**The headline, 2 clean runs:**

|  | run A | run B |
|---|---|---|
| agree / disagree (raw) | 70592 / 14408 | 70584 / 14416 |
| **slot matched** | **70332 / 2598** | **70327 / 2605** |
| slot missed | 260 / 11810 | 257 / 11811 |

Raw agreement is 83%, but that number is misleading and should not be quoted.
When the heuristic picks a fetch slot the shader never fetches from, the
comparison falls back to `attrs[0]` — some other slot's stride — so those
"disagreements" are an artifact of the comparison, not evidence about the
decoder. Among the draws where both name the same slot, **agreement is 96.4%**.

The disagreements are concentrated in one pattern, slot-matched only:

```
h8/v16 : 2560     heuristic says 8, shader says 16   <- 98% of all disagreement
h8/v28 :   20
h28/v36:   18
```

A factor of two, which is exactly the failure the existing comment in
`CollectVertexFetches` predicts: a buffer shared between draws divides to a
multiple or a fraction of the true stride. The shader is right and the division
is wrong.

**The slot miss is a bug, not noise.** 12000 draws per run — 14% — have the
heuristic reading from a fetch slot the bound shader does not use at all. Those
draws are being read out of the wrong buffer. The shader names its slot
(`fetch_constant_index`), so this is fixable, and it is probably worth more than
the stride correction.

**Strides and position formats the skip histogram never showed.** The renderer's
skipped-stride histogram only counts draws that got as far as being rejected;
the vfetch histogram covers every draw:

```
strides       8:23264  12:869  16:9768  20:283  28:27591  36:22689  48:510  52:26
pos formats  32:43609  37:23264  38:10278  57:7490  31:359
```

Format 32 is `k_16_16_16_16_FLOAT` — **half-float positions are the plurality**,
at 43609 against 7490 for the `k_32_32_32_FLOAT` (float3) the PSO's input layout
declares. Any transcode round has to handle 16-bit floats first, not last.

Nothing downstream consumes any of this yet: `dc.vertex_stride` is still the
division guess and submitted/skipped stayed at 85/230-232, unchanged.

##### Correction to the previous entry's crash claim

The entry above on the surface gate says "2 of 4 gated runs took an access
violation against 0 of 4 ungated". The second half has not held up: run mx_079,
with the gate off, crashed the same way (3 AVs, dead at `LoaderTick #5`).
Across ungated runs mx_074-082 the rate is **1 in 9**, not 0. The gate may still
aggravate it — 2 in 4 against 1 in 9 — but it does not cause it, and this is the
pre-existing intermittent crash class AGENTS.md already warns about rather than
something that round introduced. The reason for keeping the gate off stands on
the draw count alone (85-89/frame to 5).

#### The ALU constants are not the world matrix (2026-08-02)

`LOAD_ALU_CONSTANT` (0x2F) is now handled and shadowed into a 512-vec4 file.
The claim it was chased for — that the world transform is one of the matrices
the game loads through it — **is wrong**, and the probe that tested it is the
evidence.

**What the game actually loads.** ~44000 loads per run, into only *three*
constant-file indices:

```
0x3F0 : 19400 loads (size 16)
0x7F0 : 24310 loads (size 16)
0x3E0 :   290 loads (size 32)
```

Eight vec4 registers out of 512. A world matrix does not live in a slot that is
rewritten 44000 times a run.

**What is in them.** Dumped raw, every sample has two or three rows identically
zero, and the live values are shader math constants rather than matrix elements:

```
c0x3F0 row0 = 0 0 0 0            c0x7F0 row0 = 0 0 0 0
c0x3F0 row1 = 0 0 0 0            c0x7F0 row1 = 0 1.0000 -0.3333 0.3333
c0x3F0 row2 = 4.0000 0.1592 0 0  c0x7F0 row2 = 0.1100 0.3000 0.5900 0.6667
c0x3F0 row3 = 0.2500 1.0000 0 0  c0x7F0 row3 = 0.2500 0 0 0
```

`0.1592` is 1/(2π). Alongside 4.0, 0.25, 0.75, 1/3, 2/3, 1.5 and an
0.11/0.30/0.59 triple that is a luminance weight vector. These are a per-shader
scalar constant block written into two rolling scratch slots, and the contents
change from draw to draw.

**The probe.** Each draw's first vertices transformed by both candidates in both
layouts, with the viewport inverse as control, sampled across `prim` 8, 6 and 13
after the post-load state is reached. Nothing lands in clip space. The best
showing is 2 of 3 vertices, and those are degenerate — x and y come out
identically 0 because the matrix rows that would produce them are zero.

So: **the world transform is computed in the shader from these constants, not
supplied whole.** The plan anticipated this outcome and it caps the next round
correctly — reaching world geometry needs real shader ALU translation, not a
matrix lookup. The viewport inverse remains the only transform we have, and it
is right only for the window-space UI rects it was derived from.

**A caveat on the QuadList sample.** The probe reads "the attribute at offset 0"
as position. For `prim=13` that is a `k_16_16_16_16_FLOAT` decoding to values
like `(0, 1.750, 0, 1.875)` and `(0, -1.750, 0, 0)` — a w of 0 on some vertices,
and a range of about ±1.75. That may not be a position at all; it is equally
consistent with two half2 pairs, or corner offsets. Picking position by offset
alone is not yet justified for these shaders, and no conclusion about QuadList
world coordinates should be drawn from it.

**Cost, and where it actually is.** `MainLoop` reaches #481 rather than #601,
consistently across 3 runs, with draw counts and the stride verdict unchanged.
That is *not* the probe's logging, which is capped at 24 blocks for the whole
run: Stage 2 (mx_078, microcode decode wired up, no 0x2F handling) still reached
#601, and the drop appears only once `HandleLoadAluConstant` lands. The cost is
its ~44000 `ReadGuestRange` calls per run — a `VirtualQuery` plus a 64-byte
memcpy each. If the ALU shadow is kept, caching by (address, size) is the
obvious dial, since the same three indices are reloaded constantly.

#### The fetch slot comes from the shader now (2026-08-03)

`AttachVertices` chose its vertex buffer with "lowest-indexed slot that
validated" — a tie-break with nothing behind it. Measured against the decoded
microcode, it read a buffer the bound shader never fetches from on 14% of draws.
The vfetch instruction names its fetch constant index, so the tie-break is
replaced by the answer. Cvar `vfetch_use_shader_slot`, default on; off restores
the old rule so the two can be compared on one build.

Matching is against the **set** of slots the shader uses, not "the position
attribute's slot". Identifying position needs a rule for picking it out of the
attribute list, and the only candidate — "the attribute at offset 0" — is not
justified yet (see the QuadList caveat in the ALU entry). Slot membership needs
no such rule.

**A/B on one build, same command line, at frame #501:**

|  | shader slot (on) | lowest validated (off) |
|---|---|---|
| submitted | **108** | 85 |
| skipped | 219 | 232 |
| skipped strides | `12:301 20:178 36:13505` | `12:443 16:4248 20:26 36:13169` |

Reproduced at 97 / 97 / 108 submitted over three runs against a stable 85.
**+14 to +27% more draws reach the screen**, and the entire stride-16 skipped
population — 4248 draws — **disappears** (to 1 and 10 in the confirming runs).
That is the Stage 2 prediction landing: `h8/v16` was 98% of all slot-matched
disagreement, and those bogus stride-16 readings simply do not exist once the
right buffer is read. Slot-missed disagreement fell from 11810 to ~2200.

~7560 draws a run are corrected. About 2200 remain where the shader names a slot
that no *validated* fetch carries; those still fall back to the old rule rather
than being dropped, to keep this change to one variable, but they are counted
and their pixels are not to be trusted.

Cost: `MainLoop` #541 against #601 with the cvar off on the same build. 0 access
violations across the four runs. **The screen is still black** — that is the
guest painting it black, which no amount of correct vertex fetching addresses.

#### The transcode works; identifying position does not (2026-08-03)

Guest vertices are now rewritten into the one layout the game PSO declares —
POSITION float3 @0, COLOR float4 @12, stride 28 — using the formats decoded from
the shader. `ReadVertexAttribute` handles k_32_FLOAT, k_32_32_FLOAT,
k_32_32_32_FLOAT, k_32_32_32_32_FLOAT, k_16_16_FLOAT, k_16_16_16_16_FLOAT,
k_16_16, k_16_16_16_16, k_8_8_8_8, k_2_10_10_10, k_10_11_11 and k_11_11_10;
anything else reports failure rather than rendering a guess, and is counted.

**The endian bug found on the way.** `AttachVertices` applied a 32-bit byteswap
for any non-zero fetch endian, justified in a comment on the grounds that
16-bit formats could not be identified without the shader. Both modes occur —
13 of the first 52 logged fetches are 8in16 (1) and 26 are 8in32 (2) — so every
8in16 buffer was being swapped wrongly. `ApplyFetchEndian` now applies the swap
the mode actually calls for.

**The result, and it is not a win.**

|  | transcode off | confirmed formats only | all formats |
|---|---|---|---|
| submitted / frame | 97 | **100** | 282 |
| skipped | 220 | 215-217 | 47 |
| screen | black | black | **entirely white** |

Transcoding everything triples the draws that reach the screen and turns the
window white. That is not the transcode being wrong — it is the rule for
choosing *which attribute is the position* being wrong. `PickPositionAttribute`
takes the lowest-offset attribute in a format that could hold coordinates, and
the only format that rule has ever been confirmed on is k_32_32_32_FLOAT, from a
real vertex hex dump. For k_16_16_16_16_FLOAT — 34387 of 65000 transcoded draws
— the ALU probe had already shown those decode to w=0 and a range of about
±1.75, and this is that doubt cashing out: fed through as positions they produce
screen-covering geometry.

`transcode_confirmed_formats_only` therefore defaults **on**, which leaves the
screen unchanged and submitted at 100. The machinery is built, tested and
measured; it is gated on a fact we do not have yet.

**The fact we need, and where it is.** The vertex shader exports its position
through a specific export register, and that export is in the microcode we
already decode. Fixture 1 contains `C80F803E ... E2010100` — `0x3E` is 62, the
Xenos position export — alongside `C80F8000`, an export to 0. Decoding ALU
export instructions far enough to learn which GPR feeds export 62, then matching
that GPR against the `dest_reg` each vfetch writes, replaces the offset guess
with the shader's own answer. That is a bounded piece of work on data already in
hand, and it is the thing standing between this transcode and every stride and
format rendering. It also settles the `dest_reg` question the ground-truth
fixtures raised: position is at `dest_reg` 1 in both, colour at 0.

Remaining gaps at the safe default: ~15000 draws a run still skipped for
topology (QuadList `prim=13` and TriangleFan have no host equivalent and are
dropped), and ~2000 transcode read failures where the declared attribute does
not fit the stride actually read.

0 access violations across 3 forced runs plus control; `MainLoop` #541,
`LoaderTick` #500.

#### The export 62 decode: right attribute, wrong space (2026-08-03)

> **Annotates the entry above.** Its closing claim — that decoding export 62 is
> "the thing standing between this transcode and every stride and format
> rendering" — is now measured, and it is **wrong**. The decode landed and works;
> the screen did not change. What it removed was the *identification* blocker,
> and that turned out not to be the only one.

`DecodeVertexShaderFetches` now walks ALU instructions as well as fetches and
tracks, per GPR, which fetched attributes reach the export to register 62. A
vfetch defines its destination; an ALU instruction unions its sources' taint
into its destination; an export to 62 unions into the answer. Branches are still
not followed and loops not unrolled.

**Ground truth, decoded by hand before the code was written.** Both fixtures end
with `C80F803E 00000000 E2010100`. In word 0, bits[5:0] = 62 and bit 15
(`export_data`) is set. In word 2, `src1_sel` = 1 (temp) and `src1_reg` = 1 — so
the position export reads GPR 1, which is exactly the `dest_reg` of the
float3 vfetch in both. `C80F8000 ... E2000000` exports GPR 0, the colour. This
is the shader stating its own layout.

**The operand-count trap, which the fixtures caught.** The first version
consulted all three source operands because `kAluVectorOpcodeInfos` — the SDK
table that would say how many an opcode reads — is `extern const` and lives in
the sealed plugin DLL. That marked colour as feeding position in *both*
fixtures: the export is a two-operand op whose unused `src3` field still names
temp register 0. `kVectorOperandCount` in `shader_ucode.cpp` is transcribed from
the per-opcode signatures documented in `AluVectorOpcode` itself. Over-approximating
was not the safe direction it looked like.

**What it bought, measured over 65000 transcoded draws:**

- **97% of draws now have a shader-identified position** (48340 from the export
  trace against 1468 from the fallback guess).
- **Transcode read failures fell from ~2000 a run to 17-21.** The old guess was
  picking attributes that did not fit the stride; the shader's answer does.
- Every decoded shader that exported to 62 traced it back to a fetch. None
  computed its position purely from constants.

**What it did not buy: any pixels.**

|  | guess, confirmed formats | export trace, all formats | export trace, confirmed formats |
|---|---|---|---|
| submitted / frame | 100 | 253 | **100** |
| skipped | 215-217 | 34 | 215-217 |
| screen | black | **white, spikes off the top-left** | black |

Trusting the export in whatever format it declares does raise submitted draws to
253 and empties the skipped-stride histogram — and whites the window out with
degenerate triangles fanning from the corner, identical at t+80s and t+105s.

**So the earlier diagnosis was half right.** k_16_16_16_16_FLOAT really is what
the position export reads, in 35655 of 65000 draws — the doubt recorded above
about those attributes being texcoords was wrong, and the trace refutes it. The
remaining defect is not *which* attribute but *what space*: half-float positions
are compressed model space that the shader expands with a per-object scale and
bias, and the ALU probe already established this game computes that transform in
the shader rather than supplying a matrix we can read. Raw half-floats through
the viewport inverse land far outside the frustum, which is the corner spray.

`transcode_confirmed_formats_only` therefore still defaults **on**, and now
applies to export-traced positions too, not just guessed ones. `transcode_trust_export`
(default on) is the A/B knob for the trace itself.

**The next blocker is a shader ALU translator, not another heuristic.** That is
a materially larger piece of work than this was, and worth saying plainly rather
than discovering a third time.

Verification: `ucode_test` passes both fixtures, a new negative fixture (export
destination changed from 62 to an interpolator, which must fall back to the
guess and report that it guessed), and the malformed-blob cases. 3 forced runs
clean, 0 AVs, `RenderThread` #501 at 100/215 in two of three — the third logged
one outlier frame at 2/37. **One earlier run did take an access violation**
(`read at 0xFFFFFFFFFFFFFFFF`, host RIP in the plugin DLL near `0x7FFED67D5xxx`);
the same signature appears in mx_018, mx_070, mx_073 and mx_079, all predating
this work, so it is the known race rather than a new fault — but it is 1 in 7
runs of this build against a recorded 1 in 9, which does not separate the two.

#### The ALU constant file is not empty — we were dropping it (2026-08-03)

> **Voids, for the second and final time, "the game never writes the ALU
> constant file."** The previous annotation narrowed that claim to "true of
> `SET_CONSTANT` and of Type0 writes to `0x4000..0x41FF`". The narrowing was the
> bug: the ALU constant file is `0x4000..0x47FF`, 512 vec4, and the check covered
> the first 128. Worse, `ApplyType0Write` clipped incoming Type0 writes into the
> fetch-constant and context-register files only, so anything landing in the ALU
> range was discarded whatever its address.

Measured in one forced run, with all three doors instrumented:

| Door | Writes / run | Status before |
|---|---|---|
| Type0 into `0x4000..0x47FF` | **78697** | **discarded — no shadow existed** |
| `SET_CONSTANT` type 0 | 0 | computed `reg_index`, then ignored it |
| `LOAD_ALU_CONSTANT` (0x2F) | 57303 | shadowed, and working |

**Live vec4 slots once all three are honoured: 224-239 of 512.** Observed Type0
writes include `reg=0x4000 dwords=16` and `reg=0x4400 dwords=80`.

**A misreading to not repeat.** The `alu: load ... row0=(0,0,0,0)` line — every
one of 54000 loads — was read as "the constant file is empty". It logs the first
four dwords of each sixteen-dword load, not the load. 51712 of 57303 loads carry
a non-zero payload; the guest read through `ReadGuestRange` was never broken.
The instrument answered a narrower question than the one being asked of it.

This unblocks the shader ALU translator, which needs `c[n]` to be real. The
Stage 3 conclusion that the world transform is computed in-shader rather than
supplied as a readable matrix still stands — but the shader computing it now has
constants to compute it from.

#### The shader ALU interpreter: built, measured, not yet trusted (2026-08-03)

`src/gpu/shader_alu.{h,cpp}` executes a vertex shader's ALU on the CPU and
returns the clip-space position it exports to register 62. An interpreter
rather than an HLSL translation because everything needed is already CPU-side —
microcode, fetched attributes, constant file — and the transcode already walks
every vertex; emitting HLSL would mean a runtime compiler, a PSO per shader and
a rewritten draw path to answer the same question.

Implements the vector set (add/mul/max/min/set*/frc/trunc/floor/mad/cnd*/dp4/
dp3/dp2add/max4/dst/maxa) and the scalar set (adds/muls/subs/max/min/set*/
frcs/truncs/floors/exp/log*/rcp*/rsq*/sqrt/sin/cos/*_prev/retain_prev), the
component-relative swizzle rule, source negate and absolute, co-issue, the
export write-mask scheme (v=1 s=1 is constant 1, v=0 s=0 with scalar_dest_rel
is constant 0) and clamps. Refuses jumps, calls and loops outright: the decoder
can over-approximate an attribute set safely, but taking a branch the shader
would not, or running a loop body once when it runs eight times, yields a
confidently wrong position.

**Measured on 4000 sampled executions from a forced run:**

| Outcome | Count |
|---|---|
| executed ok | 2452 |
| **relative addressing** (`c[a0 + n]`) | **1012** |
| unsupported scalar op | 536 — opcodes 42/44/46, the `mulsc`/`addsc`/`subsc` family |

Of those that executed: 1135 land inside the clip volume, 1315 outside, 2
non-finite. By position format, in/out: `32:212/649  37:756/363  57:167/303`.

**`alu_execute` therefore defaults off.** 46% in-range is not a result to render
on — and note that out-of-range is not automatically wrong, since off-screen and
culled geometry legitimately lands there, which is exactly why this number
cannot settle the question on its own.

**A placement bug worth remembering.** The probe was first written inside
`TranscodeVertices` after the `transcode_confirmed_formats_only` gate, so it
only ever saw the k_32_32_32_FLOAT draws that already worked — the format-32
majority the interpreter exists for was filtered out before measurement. The
first histogram was 100% `posfmt=57` and looked encouraging. An instrument
placed downstream of the filter it is meant to justify removing measures
nothing. `ProbeAluExecution` now runs before the gate.

**The two remaining gaps are named and counted**, which is the point of the
histogram: implement `a0`-relative constant addressing (the `MaxA`/`MovA` side
effect plus `c[a0+n]` reads, 25% of failures) and the constant-operand scalar
family. Both were left out deliberately — the `sc` operand encoding is a
special case, and guessing at it produces exactly the confidently-wrong
positions this whole line of work keeps running into.

> **Both gaps were closed in `28228fa`, and only one of them was real.** See the
> next section. `a0` converted zero failures: every relative refusal in this game
> is `aL`-relative, not `a0`-relative.

#### The black screen is the guest's own clear, drawn correctly (2026-08-03)

Not a bug in the present path. Established by running with
`--clear_magenta=true` and sampling the window's pixels at intervals:

| t | game window |
|---|---|
| 6, 10, 14s | **magenta** — our clear reaches the screen |
| 20s | magenta gone, black |
| 30s | entirely black |

So the clear works and the copy to the backbuffer works. Between 14s and 20s the
guest starts submitting real frames, and one of them paints the whole target
black *on top of* the magenta. That draw is visible in the log: `prim=8`
(RectangleList), stride 28, expanded to a full-screen quad spanning clip
`(-1.0008, 1.0014)`. It is the guest's own clear, and we replay it faithfully —
`d3d12_device.cpp` says as much in the comment above `ClearRenderTargetView`.

**The screen is black because the guest's clear lands and nothing lands on top of
it.** The content is in the ~217 draws skipped per frame for stride and the
~15,000 skipped per run for topology. Any future "the screen is black" theory
has to beat this one first. Note also that a black screen is *not* the clear
colour (`0.05/0.08/0.18` = `(13,20,46)`) — sampling the actual pixel values, not
eyeballing a screenshot, is what separated the two.

#### Every frame used to render and present twice (2026-08-03)

Fixed in `10e71c8`. `BeginFrame` calls `RenderGameFrame` internally and
`EndFrame` calls `PresentGameFrame` internally; the render thread called both
again in between. Per tick the GPU actually saw:

```
BeginFrame   backbuf PRESENT->RT, gameRT PSR->RT, clear, RenderGameFrame  (draws)
             RenderGameFrame                                              (draws again)
Present #1   gameRT RT->COPY_SOURCE, copy, gameRT COPY_SOURCE->PSR
EndFrame ->  Present #2   gameRT RT->COPY_SOURCE   <-- gameRT is in PSR. Invalid.
```

`PresentGameFrame`'s barriers are directional, so the second call declared a
`StateBefore` the first had already moved away from. No `DeviceRemoved` ever
appeared in a log — the runtime tolerated it silently, which is why it survived
so long. The intro-video loop had the same shape with `RenderVideoFrame`. The
three frame internals are now **private** so it cannot recur.

#### D3D12 command lists do not keep your resources alive (2026-08-03)

A comment in `d3d12_game.cpp` claimed "D3D12's internal command-list tracking
keeps the underlying memory alive until the GPU finishes the last command using
it". **That is false, and it was load-bearing.** It is a D3D11 guarantee;
D3D12 command lists do not reference-count the resources they reference, and
lifetime is entirely the application's job.

On that assumption `ClearGameDraws` released every draw's vertex, index and
constant buffer outright, once per frame, from the render thread *before*
`BeginFrame` — while the previous frame's command list was still in flight. Up
to 256 draws × 3 UPLOAD-heap resources per frame, freed under the GPU.

Buffers now move to a fenced retirement deque (`RetiredFrame` in
`d3d12_renderer.h`) and are released in `MoveToNextFrame` once `m_fence` has
passed the value signalled for the submission that last used them.

**Do not read this as "the intermittent AV is fixed."** The rate is ~1 run in 8
and this session saw 1 in 8 (`mx_116`), with the same recorded signature — read
at `0xFFFFFFFFFFFFFFFF` on a non-translator thread, a host-side pointer. n=8
cannot separate 1-in-8 from 0. The lifetime fix is correct on the code reading;
it is not verified by the run count.

#### Closing the ALU gaps: the scalar family was real, `a0` was not (2026-08-03)

> **Half of this section's headline is wrong — corrected below in "The `a0`/`aL`
> selector was inverted".** `a0` *was* real. It converted zero failures because
> one condition in `Src()` had the a0/aL selector backwards, so the a0 arithmetic
> was never once executed. Everything here about `mulsc`/`addsc`/`subsc` stands;
> everything about `a0` "buying nothing" and about `aL` being what this game uses
> does not. The evidence that should have caught it was already in the same log
> line — see below.

**`mulsc`/`addsc`/`subsc` (opcodes 42..47) are implemented, and that closed the
whole unsupported-opcode population — 536 to 0.** Their operand encoding is the
special case the earlier note warned about, and it is worth writing down because
nothing about it is guessable:

- `src3_reg` names a **constant register** directly, 8 bits, not a Src()-shaped
  operand. Do not route these through the normal source path — its swizzle and
  negate handling assumes the other encoding and silently yields wrong operands.
- The **temp** register it combines with is scattered. One bit of its index lives
  in the opcode field itself, which is the entire reason each operation has a
  `_0` and a `_1` form. The SDK reassembles it:
  `scalar_const_reg_op_src_temp_reg() = (scalar_opc & 1) | (src3_sel << 1) | (src3_swiz & 0x3C)`.
- Both operands are scalars, component-selected by `src3_swiz & 3`.

Get that temp index backwards and the interpreter reads register `n^1` — another
live value, so the result is plausible and wrong. `tools/ucode_test.cpp` guards
it by asserting `mulsc0` reads r0 and `mulsc1` reads r1 with different seeds.

**`a0` is modelled but bought nothing here.** Written by `maxa` (from `src0.w`),
`maxas` (from `src0.x`, round-to-nearest) and `maxasf` (floor), clamped to
`[-256, 255]` — it is a signed 9-bit register. Consumed by `c[a0+n]` reads.

The trap, and the SDK states it outright at `ucode.h:2045`: *"Temporary registers
can have only absolute and aL-relative indices, not a0-relative."* So a relative
**temp source** and a relative **destination** are `aL`, never `a0`. An
implementation that offsets those by `a0` compiles, passes casual inspection, and
reads the wrong register. `AluStatus::kRelativeAddressing` was split into
`kLoopRelative` specifically so the measurement could tell the two apart —
without the split the result below would have read as "25% became 28%, the fix
did nothing" with no way to see why.

`aL` is the **loop** counter, and the interpreter deliberately walks every exec
block once rather than unrolling, so there is no honest value to give it. It
stays refused. Reaching those shaders means implementing `kLoopStart`/`kLoopEnd`
for real — a larger piece of work than either item here, and now the single
biggest remaining gap.

Measured, 4000 sampled executions, three runs plus a default control:

| | before (`53c44dd`) | after (`28228fa`) |
|---|---|---|
| ok | 2452 (61%) | 2882–2888 (**72%**) |
| relative addressing | 1012 (25%) | *split* |
| `aL`-relative (loop) | — | 1112–1118 (28%) |
| unsupported scalar op | 536 (13%) | **0** |
| unsupported vector op | 0 | 0 |

Clip range: 1135/1315 in/out became 1337–1427 / 1456–1521. By position format,
in/out, from the default-config run: `31:18/8  32:313/660  37:776/342
38:184/142  57:136/304`. For `k_16_16_16_16_FLOAT` (format 32) — the 35,655-draw
majority and the whole reason the interpreter exists — in-range went from 25% to
about 30%. Better, and still not the answer.

**What that points at.** 28% of shaders index constants by a loop counter. That
is the shape of skinning or instanced per-object transforms, which is exactly the
missing per-object space the export-62 work identified. The remaining
out-of-range positions and the loop-relative refusals are plausibly the same
finding seen twice.

Defaults are unchanged and deliberately so: `alu_execute=false`,
`transcode_confirmed_formats_only=true`, 108 submitted / 218 skipped per frame.
With the gate off it is 268/47.

#### The `a0`/`aL` selector was inverted (2026-08-03)

`is_const_address_register_relative()` means relative to **the address
register**, which is `a0`. The enum states the mapping outright
(`ucode.h:191-199`):

```cpp
enum class AddressingMode : uint32_t {
  kRelative = 0,   // c[aL + 5]
  kAbsolute = 1,   // c[a0 + 5]
};
```

`Src()` tested that condition without the `!`. It therefore refused every `a0`
read — the case the interpreter implements — and applied `a0_` to every `aL`
read, a case that never occurs here. Un-inverting it took the interpreter from
**72% to 100%** of sampled executions, with `aL`-relative and every other failure
class going to **0**. Nothing else changed.

**The evidence was already in the log line that reported the null result.**
`kUnsupportedCf` was `0` across all 4000 samples, and the interpreter refuses
`kLoopStart`/`kLoopEnd` before reaching any ALU instruction — so zero means *no
shader in this game contains a loop*. `aL` is the loop counter. 1112 instructions
could not have been indexing by a register nothing ever starts a loop to set.
Two numbers in one line contradicted each other and it took a round to notice.
`shader_alu.h` now records that the two are a cross-check: a non-zero
`kLoopRelative` against a zero `kUnsupportedCf` is a decode error, not a loop.

**The method note, which is the part worth keeping.** *A fix that converts
exactly zero failures is evidence about the instrument, not about the game.*
Both times this interpreter has produced a surprising null result, the null was
the bug — first the histogram sampled downstream of the filter it existed to
justify removing, now this. A large, plausible number is harder to doubt than an
impossible one, which is precisely why it survived.

This is also the second bit-level fact in two rounds that read plausibly
backwards, after `ucode.h:2045` on relative temp indices. Both cost real work.

#### QuadList was half the frame, and was being dropped (2026-08-03)

A `prim_type` histogram over a full run:

| prim | topology | count | was |
|---|---|---|---|
| **13** | **QuadList** | **40,755** | **dropped — `kUndefined`** |
| 6 | TriangleStrip | 26,731 | submitted |
| 8 | RectangleList | 18,822 | expanded and submitted |
| 0 | invalid | 3 | dropped |

QuadList is the largest single population, larger than the other two combined,
and every one fell through `MapTopology`'s `default:` case. `ExpandQuadList`
handles it: unlike a rectangle a quad has all four corners, so vertices pass
through untouched and only the index buffer is rebuilt, six indices per quad on
the **v0-v2 diagonal**. It maps through the incoming indices rather than assuming
the sequential ones an auto-draw synthesizes, so it is correct for a real
`DRAW_INDX` too — measured, every QuadList draw in this game is an auto-draw with
resolvable vertex data, so neither counter fired.

After this the only unmapped topology left is 3 draws of `prim_type=0`.
**TriangleFan does not occur once in a full run**, so it stays unimplemented on
purpose — there is no measured population to justify the code.

#### 100% executing, and still nothing recognisable on screen (2026-08-03)

Both fixes landed and were measured. Neither puts geometry on the screen, and
that is the honest headline of the round.

| | before (`28228fa`) | after (`4fc00ed`) |
|---|---|---|
| ok | 2888 (72%) | **4000 (100%)** |
| `aL`-relative (loop) | 1112 (28%) | **0** |
| clip in / out / non-finite | 1427 / 1456 / 5 | 2053 / 1934 / 13 |
| format 32 in-range | 32% | **39%** |

**~48% of computed positions are still outside the clip volume even at 100%
execution.** The interpreter is no longer the limit.

A 2×2 over the two cvars, sampling actual pixel values rather than eyeballing:

| | gate on | gate off |
|---|---|---|
| **ALU off** | black (defaults, unchanged) | white, with coloured streaks at t+25 |
| **ALU on** | black — *identical to defaults* | white |

Two things fall out of that table:

- **The white is the format gate, not the ALU.** Turning the gate off submits
  draws whose positions are wrong; they smear into long thin triangles radiating
  from one point, in real interpolated vertex colours, and by t+45 cover the
  screen. That signature — many triangles converging on a common point with the
  third vertex flung away — is what a missing per-object transform looks like.
- **`alu_execute` on with the gate on is bit-identical to defaults**, because the
  gate returns before the interpreter is ever consulted
  (`pm4_translator.cpp:1037` vs `:1063`). Only format-57 draws reach the ALU in
  that configuration, and they are a small minority. Worth knowing before reading
  any future A/B of that cvar as a null result.

So the remaining problem is upstream of the interpreter: the ALU constant file is
missing whatever supplies the per-object space, most likely matrices arriving
through a door we do not shadow, or arriving *after* the draw that reads them,
which a per-draw snapshot cannot capture. That is the next lever, and it is the
third time this same missing per-object transform has been the answer.

Per-frame submitted counts vary a lot run to run now (5 to 321 at frame #301 on
defaults), because how much content has loaded by a given frame is not stable —
do not read a single frame line as a regression signal.

Defaults remain unchanged: `alu_execute=false`,
`transcode_confirmed_formats_only=true`. **AV: 1 crashing run in 9 this session**,
same recorded signature (read at `0xFFFFFFFFFFFFFFFF`, host-side pointer,
non-translator thread), consistent with the recorded ~1-in-8 and still not
verified either way at this sample size.

#### The interpreter exports window space, not clip space (2026-08-03)

Every non-degenerate position the ALU exports reads like this:

```
pos=(640.0000 0.0000 1.0000 w=1.0000)
pos=(1280.0000 0.0000 0.0000 w=1.0000)
pos=(0.0000 0.0000 1.0000 w=1.0000)
pos=(639.5000 -0.5000 1.0000 w=1.0000)
```

`640`, `1280`, `360`, `w=1`, with half-pixel offsets. That is **1280x720 window
space in the D3D9 pixel-centre convention** — these shaders do the viewport
transform themselves. Under this game's viewport (`xs=640 xo=640 ys=-360
yo=360`) they map to exact clip corners: `(0,1)`, `(1,1)`, `(-1,1)`.

The transcode believed the opposite. It asserted the answer was already clip
space, perspective-divided it and handed the renderer an **identity** MVP,
discarding `BuildViewportMvp` — which computes precisely the inverse the output
wanted. **The fix was deleting that special case**, not adding a transform; the
ALU path and the fetched-position path now agree about what space they are in.

Scored over 4000 executions *before* deleting anything, in-range by position
format, as-clip against after-the-inverse:

| format | as clip | after inverse |
|---|---|---|
| 31 | 44% | 78% |
| **32** | **32%** | **74%** |
| 37 | 70% | 87% |
| 38 | 65% | 74% |
| 57 | 34% | 88% |

Overall 47% to 81%. Every format improves and format 32 — the 35,655-draw
majority — more than doubles.

**The space buckets are genuinely mixed, so this is the better of two
interpretations rather than a clean sweep:** clip-like 1203, window-like 901,
neither 1083, degenerate 764. It was chosen on the per-format numbers.

Two things worth keeping about how that was measured. The first pass could not
be trusted, because `(0,0,0,w=0)` is the single most common export and it sits
inside the unit cube — so it counted as clip-like while being evidence of
nothing, *and* inflated the viewport-inverse count, since with `xo/xs == 1` the
origin maps to exactly `(-1,+1)`. Separating it out was what made the verdict
readable. And the clip/window tie is deliberately given to clip, so the bias runs
against the hypothesis being tested rather than for it.

**It did not put geometry on the screen.** The 2x2 over the two cvars is
unchanged from before the fix — gate on black, gate off white, at t+25/45/70.
That is consistent rather than contradictory: 764 degenerate plus 1203 clip-like
is **49% of executions landing at or near the top-left corner** after the
inverse, in range but collapsed. Which is exactly the convergence point of the
streaks in the pre-fix capture. A position can be in range and still be wrong,
and in-range alone is not a success criterion.

So the round fixed a real defect and did not fix the screen. What remains is the
19% of executions exporting `(0,0,0,w=0)` outright: the shader computes nothing,
which points back at the ALU constant file still lacking the per-object
transform. `no shader 0` in the transcode summary rules out a missing-shader
explanation — every draw has one.

AV: **0 in 9 runs** this round, against a recorded ~1-in-8 and 1-in-9 last round.
Still not measurable at this n; do not read it as fixed.

#### How to screenshot this thing (2026-08-03)

Took several attempts to make trustworthy, so: capture the **`SDL_app`** window,
not `MainWindowHandle` — that returns a different window and yields a
full-desktop grab. **Raise it first**, because `CopyFromScreen` reads the desktop
and will happily capture whatever is on top of it. And **bucket sampled pixel
values** instead of eyeballing: the three outcomes are black, the clear colour
`(13,20,46)` and white, and two of them look alike at a glance.

Always run the defaults config as a control in the same sweep. When an
override run came back 100% white it looked exactly like a broken capture; the
defaults run reproducing the recorded black in the same session is the only
reason the white was known to be real.

#### The constant file is not the problem (2026-08-03)

A negative result, measured, and it closes off the most attractive remaining
hypothesis.

**`SQ_VS_CONST` (0x2307) reads `base=0 size=255`. `SQ_PS_CONST` (0x2308) reads
`base=256 size=255`.** Both in vec4. So the vertex stage is based at **zero**,
`Const()`'s absolute indexing is already right, and **no rebasing is needed**.

That the two decode to exactly the two 256-vec4 banks also validates the bit
layout, which is worth stating because it came from Xenia and not from the SDK —
`register_table.inc` names both registers but carries no bitfield for either:

```
bits [8:0]   base   (vec4)
bits [20:12] size   (vec4)
```

It also settles what `LOAD_ALU_CONSTANT`'s two destinations are. It writes only
dword `0x0`, `0x3E0`, `0x3F0` and `0x7F0`; `0x3F0` is vec4 252 and `0x7F0` is
vec4 508 — the last four vec4 of bank 0 and bank 1 respectively. With VS based
at 0 and PS at 256, **`0x3F0` is the vertex matrix and `0x7F0` the pixel one**,
one of each per draw. Nothing is landing in the wrong bank.

**And the shaders are getting real constants.** `Const()` is the single choke
point for every constant read — plain, `a0`-relative and `mulsc` alike — so it
now counts reads, all-zero reads and the index range, split by whether the
execution went on to produce nothing:

| | n | reads/exec | zero reads/exec | index range |
|---|---|---|---|---|
| produced a position | 3249 | 10.4 | **0.1** | 0..255 |
| degenerate `(0,0,0,w=0)` | 704 | 14.2 | **0.8** | 0..255 |

Degenerate executions read 8x more zeros, which is a real correlation — but 0.8
of 14.2 reads is not a shader starved of constants. They also read *more*
constants than the ones that succeed, so they are not shorter or simpler
shaders. And the distribution of the highest index each execution touches —
`67:12 79:922 255:2012` — says the majority **do** reach `c255`, exactly where
the per-object matrix arrives.

So: the matrices arrive, in the right bank, at the right index, non-zero, and
the shaders read them. **The remaining defect is downstream of the constant
file** — in the arithmetic or in the vertex inputs, not in what the shader was
given. Three rounds of "the per-object transform is missing" can be retired as
an explanation.

**Screenshot honesty note.** The override run in this round showed 9.1% coherent
green with a clean curved silhouette, against 100% white the round before. That
is **not** an improvement from this work — the commit is read-only by
construction and cannot change rendering. It is run-to-run variance in how much
content has loaded by a given timestamp, which the per-frame submitted counts
(5 to 321 at frame #301) already showed is large. Do not read a single capture
as a trend.

ALU status stayed `ok:4000`, viewport-inverse in-range 3263/690 (83%),
format 32 at 1594. AV 0 in this round's runs.

#### Skipping the broken draws does not clear the screen (2026-08-03)

The theory was that the white frame is broken geometry painted over good
geometry, so not submitting the provably-broken draws would reveal what is
underneath. **It does not.** `skip_untransformable_draws` works mechanically and
changes nothing visible.

Each transcoded draw is classified against the same `BuildViewportMvp` the
renderer will apply:

| class | draws | vertices | |
|---|---|---|---|
| partial | 14642 | 3,282,769 | at least one vertex lands — never skipped |
| degenerate | 1006 | 42,654 | every vertex at the origin |
| out-of-range | 536 | 2,135 | nothing near the clip volume |
| **mixed-origin** | **3816** | **12,336** | some vertices at exactly the origin, some not |

**`mixed-origin` is the interesting class and it had to be discovered.** The
first cut only counted a draw degenerate when *every* vertex sat at the origin,
which found 7.5% of draws and 1.5% of vertices — far too little to explain a
white screen, and a skip built on it would have done nothing. Real breakage looks
different: some vertices collapsed to exactly `(0,0,0)` and the rest not. Those
draws average **3.0 vertices** — single triangles with one corner pinned at the
origin, which is exactly the streak fan. They are 77% format 37.

Together the three bad classes are **26.8% of draws but only 1.7% of vertices**,
which is why skipping looked worth doing. 5358 draws were skipped in the
transcode and 5273 at the renderer, 238 submitted against 79 skipped per frame.
The screen stayed 100% white at t+30/50/75 in both runs.

**So the white is coming from the `partial` draws — the bulk geometry, 3.28M
vertices — not from a broken minority.** That relocates the problem and is the
real result of the round: it is not "a few bad draws smear over good ones", it is
that the main geometry is itself mis-transformed. The four-times-the-frustum
bound calls a draw partial if *any* vertex lands, so a draw with one good vertex
and the rest at ±1000 still counts as partial and still stretches. Tightening
that bound was deliberately not done — it deletes real geometry crossing the
frustum edge and makes the screen look cleaner while showing less.

**The cvar stays off by default and is a mitigation, not a fix.** The draws it
removes are still transformed wrongly; they are merely not drawn. Any screenshot
taken with it on has to be read that way.

**Running each configuration twice is what kept this honest.** The first
skip-on run showed 24.9% non-white and looked like a clear win. It was the grey
`rgb(128,128,128)` boot quadrant that appears in both configurations at early
timestamps, plus single-pixel colours — the second run was 100% white. One
capture is not a result, and this is the second time this session a single
sample nearly became a false conclusion.

Also worth re-learning: `cat logs/*.log | grep ... | tail` is **not** in
timestamp order across rotated files. It briefly showed `transcode skip: 0` and
suggested the cvar was not being parsed, when the skip had in fact engaged. Pipe
through `sort` on the timestamp prefix, as recorded earlier in this file.

### Every visible pixel comes from a draw with no colour attribute (2026-08-03)

The white frame was **colour, not geometry**. Measured, not argued.

`tint_by_color_source` (default off, `pm4_translator.cpp`) discards the real
vertex colour and writes a flat hue per source — packed green, fallback blue,
none magenta. One screenshot then reports pixel area directly instead of
inferring it from counts. Across two runs and five captures spanning t=40s to
t=165s, every capture read the same:

    bars(black)=25.58%  GREEN(packed)=0.00%  BLUE(fallback)=0.00%  MAGENTA(none)=74.42%

74.42% is the whole drawn region (2560/3440). **Magenta is 100% of it, green is
0.00% at full resolution** — 4.95M pixels examined, not a sample grid.

The counters say the opposite of the picture, and both are right:

    packed 18626 draws (6809241 vtx) | fallback 9179 (130532 vtx) formats 38:9179
    | none 12195 (174757 vtx) — 2.5% of vertices

**2.5% of vertices paint 100% of pixels.** Vertex count is not pixel area, and
here the two point as far apart as they can. Stage 2 stopped at the counters and
read "95.7% of vertices carry real packed colour" as the colour hypothesis being
dead; that reading was wrong, and only the tint could show it. Any future
argument from draw or vertex share about what is *visible* is invalid for the
same reason.

So the 6.8M properly-coloured vertices exist and are submitted, and none of them
survive to the screen — they are behind, outside, or degenerate. That does not
retract last round's "the bulk geometry is mis-transformed"; it narrows what has
to be fixed first. The white was `float c[4] = {1,1,1,1}` — the default in
`TranscodeVertices` when `PickColorAttribute` returns null — covering everything,
and opaque white is the worst possible default precisely because it is
indistinguishable from real geometry and hides what is behind it.

Note the fallback column: all 9179 fallback draws pick **format 38**
(k_32_32_32_32_FLOAT). A four-float attribute that is not the position is at
least as likely to be a normal or a texcoord as a colour, so a non-null colour is
not evidence of correctness. None of them are visible either way.

Changing the default colour, widening the colour search, and depth ordering were
all deliberately left out of this round.

### The host window size never reaches the guest (2026-08-03)

Asked whether the 21:9 window should be 16:9, since the 360 supports 21:9. It
does not matter to the guest and it did matter to every screenshot.

The guest renders **1280x720** regardless — its own viewport registers read
`xs=640 xo=640 ys=-360 yo=360` — and we hook **none** of `VdQueryVideoMode`,
`XGetVideoMode` or `VdGetCurrentDisplayInformation`. They exist only in
`generated/default/mx_init.h`. There is no path by which the host window size
could inform the guest, so nothing about the aspect ratio can cause a
guest-space defect.

But `m_viewport` was sized from the window client rect, so clip `[-1,1]` was
stretched across 3440x1440 — guest aspect 1.778 against window aspect 2.389.
**Every screenshot before this was 34% too wide.** `CreateViewportAndScissor`
now fits the largest 16:9 rect and centres it, verified by geometry rather than
by eye: `client 3440x1440 (aspect 2.389) -> 16:9 drawn region 2560x1440 at
(440,0)`, and independently by pixel share at 74.42%/25.58%.

Do **not** resize `m_gameRT` to match. `PresentGameFrame` uses
`CopyTextureRegion`, which requires the game RT and the backbuffer to be the same
size; a smaller RT needs a scaled blit instead.

The bars were dark blue at first, which looked like a bug and effectively was
one: the clear ran full-window with `GameClearColor`, our *debug* colour, so it
painted territory the guest does not own and read as image. Two clears now —
black everywhere, then the debug colour scoped to `m_scissorRect`. Black is the
letterbox convention because it cannot be mistaken for content. One consequence:
the defaults control is now 100% black end to end, so bars and guest-painted
black are no longer distinguishable in a capture. Verify the pillarbox from the
log line, not from a defaults screenshot.

### Scoring a capture: two ways to get a wrong number (2026-08-03)

Both hit in one session, both cheap to avoid.

**Sample grids miss thin geometry.** A 6-pixel grid reported 0% green on a frame
described from live view as having green artifacting. Full-resolution scoring
confirmed a true 0.00% in every capture — but the grid could not have told the
difference, and it is the wrong instrument for anything thin. `bucket.ps1` uses
`LockBits` and reads every pixel; 5M `GetPixel` calls are far too slow for that.

**PowerShell variable names are case-insensitive.** `$B` for the blue channel
silently clobbered `$b`, the bucket hashtable, and every increment then failed
with "property cannot be found" — thousands of errors and no counts. The
channels are `$cr/$cg/$cb` now. Scoring also moved out of `shot.ps1` into
`bucket.ps1`, run against the saved PNG, so a bucketing bug costs a rescore
rather than another 165-second game run.

Also: `Get-ChildItem logs\*.log | ... | Sort-Object` over 15MB of logs does not
finish inside a 10-minute timeout. Filter with `Select-String -Path` first, then
sort only the matches.

### Surface routing cannot fix the overpaint — the populations share surfaces (2026-08-03)

A round that stopped at its first stage, on purpose, because the measurement
came out against the plan.

The theory was good enough to be worth testing: 16 guest colour surfaces are
flattened into one host target, the last pass to paint wins, and the tint had
just shown the last painter everywhere is a colourless draw. `graphics_system.cpp`
had already written down the intended fix — *honour the surface binding, present
the one the guest swaps, do not guess by pitch*.

So before gating anything, `LogSurface` cross-tabulated colour source against
surface (`DrawCall::color_source`). Two runs, ≥150s, reproducing closely:

```
surface     packed (draws:verts)   none (draws:verts)
2D0/1280    9215:3333610           4656:191292      <- and 9315:3361288 / 4628:181916
2D0/160     9822:3748638           4082:16328
2D0/800     3666:1217112            498:1992
```

**The colourless draws are not on a surface of their own.** They sit on the same
surfaces as the packed geometry, and their largest population by vertices is on
`2D0/1280` — the main surface — next to 3.3M packed vertices. Any gate that
keeps the main surface keeps the overpaint with it; any gate that drops the
overpaint drops the scene.

So `present_surface_only` was **not written**. The stop condition was in the plan
before the measurement existed, which is the only reason it was honoured rather
than rationalised into a threshold to tune.

This does **not** retire "a colourless pass overpaints the main scene" — the
tint still shows exactly that. It retires *surface identity* as the way to tell
the two apart. Whatever distinguishes them is within a surface: draw order,
depth, or the draws themselves being wrong. Note the PSO leaves
`DepthStencilState` zeroed (`d3d12_game.cpp:11`), so **draw order alone decides
what wins**, and order within a surface is exactly what a surface gate cannot
touch.

Two things worth keeping from the table:

- **`kNotTranscoded` had to be split from `kNone`.** `TranscodeVertices` returns
  early in several places without resolving any colour; folding those into
  "no colour attribute" would have inflated the very population being measured.
  On `2D0/1280` they are 20167 draws and 10.8M vertices — larger than every
  other class combined, and they would have swamped the result.
- **`main_surface_only`'s premise was half right.** `2D0/1280` really is the
  plurality surface (~35400 draws), but most of it is untranscoded, which is why
  gating on it collapsed submissions to 5/frame. The table now says that
  directly instead of leaving it as a surprise.

### The white frame is ~12700 small draws smeared across the viewport (2026-08-03)

The scene was underneath the whole time.

Two probes settled it. First, viewport coverage: for every transcoded draw,
transform all its vertices with `dc.mvp` (the same math as `LogNdc`), take the
NDC bounding box, clamp it, aggregate by colour source. Two runs, matching to
four decimals:

```
              draws    verts      mean box   >50% alone   behind eye
none          12760    203607     0.5169     6609         205
packed        19546    7143234    0.0045     24           13
fallback       7694    130048     0.2135     1114         1
```

A colourless draw averages **16 vertices** and its box covers **52% of the
viewport**; a packed-colour draw covers **0.45%**. That is 115x the area on a
hundredth of the vertices, and 6609 draws each cover more than half the screen.

**That rules out a fullscreen pass** — one of those is a handful of draws per
frame, not thousands. It is small geometry smeared across the viewport by a bad
transform, the same defect the `kMixedOrigin` class named earlier.

Then the direct test. `hide_colorless_draws` (default off) drops draws whose
shader has no colour attribute. Against a control run with identical flags:

```
hiding off:  bars 25.58%  white 74.42%  (all 3 captures, all 3 timestamps)
hiding on:   bars 52.22%  clear 22.74%  white 0.00%  green 5.43%  orange present
```

**White goes from 100% of the drawn region to 0.00%**, and real coloured
geometry appears — green terrain and `rgb(249,125,17)` orange. Reproduced across
two runs; the second matched the first to the decimal and to the same 913046
grey pixels.

So the ordering of the last three rounds' conclusions is: the frame is white
because ~12700 badly-transformed colourless draws paint over everything, and
they win because **depth test is off** (`d3d12_game.cpp:11` leaves
`DepthStencilState` zeroed) so draw order alone decides. The colour default of
opaque white is what makes the smear *invisible as a defect* — it looks exactly
like a blank screen.

An **upper bound, not coverage**: bounding boxes overestimate non-rectangular
primitives and overlapping draws double-count, so the sum can exceed 1.0. Only
the comparison between rows is signal. The log line says so itself, because
reading a share as pixel area is the mistake that made the previous round's
first conclusion wrong.

`kNotTranscoded` is excluded from the table — those keep the guest stride and
the renderer's stride-28 gate drops them, so they reach no pixels. On `2D0/1280`
they are 20167 draws against 4628 colourless and would have swamped it.

Note also, unchanged from an older observation: the content is only there
**early**. At t=60s there is geometry; by t=110s green has fallen to 0.04% and
only the grey boot quadrant remains. Whatever causes that is separate and still
open.

### Why not high-level D3D9 interception (2026-08-03)

Asked whether hooking the title's D3D9 calls and translating those to D3D12
would be faster than PM4 → D3D12. It is the right question — HLE would hand us
vertex declarations directly, which is exactly what several rounds have been
spent guessing at (`PickColorAttribute`, the stride heuristic, the format-38
fallback).

> **Superseded 2026-08-03** — see *"The D3D9 entry points are located, and the
> draws are not inlined"* below. The header reading here is correct; the
> conclusion drawn from it is not. Static linking removes the import table, not
> the functions. Eight D3D9 entry points now have confirmed guest addresses and
> the game calls them directly.

It is not available as a hook point. Confirmed from the XEX header itself
(`XexTool`), not inferred:

```
Import Libraries          Static Libraries
  0) xam.xex                0) D3D9     v2.0.20209.3
  1) xboxkrnl.exe           1) D3DX9    v2.0.20209.0
                            3) XGRAPHC  v2.0.20209.3
```

**Two imports, neither of them D3D.** D3D is linked statically, so there is no
import table to hook, and the recompiler emitted no symbols — `grep` over
`generated/` finds no `D3DDevice`, `DrawIndexedPrimitive`,
`SetVertexDeclaration` or any related name. Every candidate is an anonymous
`sub_xxxxxxxx`.

What the header does give, and it is worth keeping: **the D3D9 code is present
as a contiguous block, from a known XDK — 2.0.20209**, built with
`LINK v10.0.10224` / `C1,C2 v16.0.10224` (VS2010). Static libraries link
contiguously, so one confirmed D3D function localizes the whole region. And an
exact-version `d3d9.lib` from that XDK would make FLIRT/FLAIR signatures an
exact compiler and version match rather than a fuzzy one — the difference
between naming a few functions and naming the library.

Other useful header facts: base `0x82000000`, entry `0x82BFD3C0`, code
`0x821D0000`–`0x82D00000`, data to `0x83150000`, and the title module carries an
**export table at `0x82CEA8B8`** (unusual for a game, unexamined).

Using this path would still mean identifying the entry points before any benefit
arrives, and the 360 D3D API inlines much of its command-buffer writing into the
caller, so a chunk of pipeline state would only ever exist as PM4 regardless.

**Two corrections to what this file assumed, both found while checking the
above.**

**1. 281 imports are already named.** The recompiler resolved the whole import
table; the names live in `generated/default/mx_recomp*.cpp` as `__imp__VdSwap`,
`__imp__XamContentCreateEx` and so on — *not* in `mx_init.h`, which only carries
`DECLARE_REX_FUNC(sub_XXXXXXXX)`, and not in `mx_config.toml`, whose
`[functions]` section is `0xADDR = { size = N }` boundary hints with no names at
all. All 20 `Vd*` entry points are there and are directly hookable by name. IDA
is not needed for anything in the import table.

**2. `sub_82566B58` is not `VdSwap` — it is the function that calls it.** The
kernel `VdSwap` is the thunk at `0x82CE9F98` (the thunk table runs
`0x82CE9ED8`–`0x82CEA168`, 0x10 bytes each). `sub_82566B58` contains
`bl 0x82ce9f98` returning to `0x82566E1C`, and the next declared function after
`0x82566B58` is `0x825671E0`, so that call is inside its body.

Nothing but D3D9's present path calls `VdSwap`, and D3D9 is statically linked,
so **`sub_82566B58` is D3D9's swap — the first confirmed D3D9 function, and by
the contiguity argument an anchor into the D3D9 block around `0x8256xxxx`.** The
functions it calls nearby (`sub_825598A0`, `sub_82559AE0`, `sub_82571780`) sit in
the same range and are D3D9 candidates.

Hooking there remains correct — it is the frame boundary with the ring state we
need. Only the name was wrong, and it is corrected in `hooks_frame.cpp`.

PM4 is the boundary that actually exists in this binary, and it is where Xenia
works too. Keeping it.

### There are no static vertex declarations to find (2026-08-03)

First use of the IDA MCP (`idalib`, headless, driving a **copy** of the database).
The round was built on a hypothesis that turned out false, and stopped there.

**The hypothesis.** `PickColorAttribute` guesses, and the stride is a heuristic.
D3D9 vertex declarations are `D3DVERTEXELEMENT9` arrays and games usually store
them as constant tables, terminated by `D3DDECL_END()` = `{0xFF, 0,
D3DDECLTYPE_UNUSED, 0, 0, 0}` — big-endian `00 FF 00 00 11 00 00 00`. If those
were in the XEX they would give stream, offset, type and semantic for every
layout, replacing the guess with ground truth.

**They are not there.** The exact sentinel returns **zero matches** binary-wide.
A tolerant `00 FF 00 00 ?? 00 00 00` returns 57, none of which decode as element
arrays — the two table-shaped clusters are a 16-byte record table at
`0x82D30CE0` and colour/config data at `0x82D5A0E0`. `FF 00 00 00 11 ...` and
`FF FF 00 00 11 ...` also return nothing.

**What is there instead:** `sub_8257A1B0`, a semantic-*string* parser. It takes
`"TEXCOORD3"`, splits the trailing digit as the usage index (rejecting >15),
uppercases, and matches the full `D3DDECLUSAGE` list in enum order — POSITION 0,
BLENDWEIGHT 1, BLENDINDICES 2, NORMAL 3, PSIZE 4, TEXCOORD 5, TANGENT 6,
BINORMAL 7, TESSFACTOR 8, POSITIONT 9, COLOR 10, FOG 11, DEPTH 12, SAMPLE 13,
VFACE 14, VPOS 15 — plus `DIFFUSE`→COLOR0 and `SPECULAR`→COLOR1, and returns
`E_INVALIDARG` otherwise. That `DIFFUSE`/`SPECULAR` aliasing identifies it as
**D3DX9's `D3DXDeclaratorFromSemantic`**, matching `D3DX9 v2.0.20209.0` in the
XEX header. Its semantic name table sits at `0x82059074`–`0x82059118`.

**So declarations are constructed at runtime from strings, and the XEX cannot
hand us the layouts statically.** The PM4 and microcode inference we already
have is the available source of truth — `PickColorAttribute` is not failing for
want of a table that exists somewhere; there is no table.

Worth being precise about the consequence: on Xenos the vertex shader's `vfetch`
instructions carry **format and offset but not semantic** — semantics are bound
at shader-compile time and do not survive into the microcode. That is the actual
reason the colour attribute has to be guessed, and it will not be fixed by
reading the binary harder.

> **Amended 2026-08-03.** Every observation above stands, and so does the
> reasoning — declarations really are absent from the XEX as static data. What
> was missing is *where they are instead*: `D3DDevice_CreateVertexDeclaration`
> builds them at runtime, and `D3D::PatchVertexShaderToMatchVertexDeclaration`
> binds semantics to shader inputs at draw time. Semantics do not survive into
> the microcode precisely **because that runtime function applies them**. Both
> functions are now located — see the section below. "Reading the binary harder"
> was the wrong instrument; reading it at the right layer is not.

Stage 1 orientation, incidentally confirmed: D3D9's swap is `sub_82566B58` and
this D3DX9 helper is `sub_8257A1B0`, so the graphics libraries occupy roughly
`0x82559000`–`0x8257B000`.

**Known-answer test, and it passed.** Before trusting anything, `xrefs_to
0x82CE9F98` (the `VdSwap` thunk) was checked against the recompiled source. IDA
reports exactly **one** xref in the whole binary, at `0x82566E18`, inside
`sub_82566B58` (size `0x684`, so it ends at `0x825671DC` and the next function
begins at `0x825671E0`) — matching the prediction to the byte. A single caller
for `VdSwap` also independently confirms that function is D3D9's swap.

#### Using the IDA MCP

`idalib-mcp` runs headless against a `.i64` with no IDA window. Setup took three
fixes, none of them obvious from the error message:

1. **`idalib` must be activated** —
   `python "<IDA>\idalib\python\py-activate-idalib.py" -d "<IDA>"`. Works on
   Python 3.13 despite IDA 9 generally targeting older; no downgrade needed.
2. **`uv`'s managed Python 3.11 was broken** — the download had landed but the
   minor-version link directory was missing, so every launch died with
   `Missing expected target directory`. `uv python install 3.11 --reinstall`
   repaired it.
3. **The plugin project had no `.venv`**, so the `idalib-mcp` console script
   that `pyproject.toml` declares was never installed — that is the
   `Failed to spawn: idalib-mcp / program not found` error. `uv sync` fixed it.

The plugin lives under
`AppData\Roaming\Claude\local-agent-mode-sessions\<id>\<id>\rpm\plugin_*`, **not**
`~/.claude/plugins/` — that directory holds only an empty `ida-pro-mcp-inline`
and looking there gives a false "plugin is not installed" reading.

**Always open a copy.** `idb_open` with `mode: force_headless` on
`assets/default.xex.probe.i64`; the original was left untouched and verified
unchanged afterwards.

### The colourless draws are not a depth pre-pass — and the output merger is entirely unmodeled (2026-08-03)

Two facts, one of which is a refuted hypothesis and the other of which is a
standing defect nothing in this file previously stated.

#### The renderer has no output merger at all

`CreateGamePipeline` (`d3d12_game.cpp:91-113`) sets a rasterizer state and a
write mask and stops. `BlendEnable` is FALSE, `DepthStencilState` is zeroed, and
D3D12 has no alpha test to leave off. Searching `src/gpu` for `blend`,
`RB_COLOR_MASK`, `RB_COLORCONTROL`, `RB_MODECONTROL` returned **zero matches** —
the translator had never read one of these registers.

So **every guest draw is composited fully opaque, in draw order, writing all four
channels.** That is not one missing feature, it is the whole back end of the
guest pipeline, and it stayed invisible because it still produces plausible
geometry. Record it here because it bounds what any future rendering round can
expect to achieve without it.

#### The hypothesis, and why it was worth testing

A shader exporting only a position has no colour attribute, so the transcode
writes `{1,1,1,1}` opaque white. A **depth pre-pass** is exactly that draw:
position-only, covering the world, writing no colour on the guest. That reading
explained the flat white, the large bounding boxes, and — uniquely — why
`ClassifyTransformedDraw` insists these draws transform *fine*. It predicted
`RB_COLOR_MASK == 0` for the colourless population.

#### It is wrong

Two runs, `NoteOutputMerger` sampling the first 7500 transcoded draws:

```
run 1   none      draws 801  mask 0xF:801            ALL-OFF 0  depth write 0  blend non-trivial 514
        packed    draws  30  mask 0xF:30             ALL-OFF 0  depth write 0  blend non-trivial  30
        fallback  draws 6669 mask 0x0:1180 0xF:5489  ALL-OFF 1180  depth write 4405  blend non-trivial 2

run 2   none      draws 786  mask 0x0:6 0xF:780      ALL-OFF 6  depth write 0  blend non-trivial 506
        packed    draws  19  mask 0xF:19             ALL-OFF 0  depth write 0  blend non-trivial  19
        fallback  draws 6695 mask 0x0:1161 0xF:5534  ALL-OFF 1161  depth write 4293  blend non-trivial 3
```

**The colourless draws write all four colour channels** (100% and 99.2%), and
**never write depth** (0 of 801, 0 of 786). A depth pre-pass writes depth and
masks colour; these do the exact opposite of both. The hypothesis is dead, and
the round stopped at its stated stop condition rather than hunting for another
register that would fit.

The register file is genuinely live, so this is a real negative and not a
measurement artifact: `RB_COLOR_MASK` is written 20119 / 19443 times per run,
`RB_DEPTHCONTROL` ~78000, `RB_MODECONTROL` ~40000.

#### What the same table did turn up

- **~64% of the colourless draws have a non-trivial blend equation** (514/801,
  506/786) — i.e. the guest composites them and we overwrite instead. This is a
  *different* mechanism reaching the same symptom, and it is now the strongest
  remaining lead for the white frame.
- **~17.5% of the fallback draws have `RB_COLOR_MASK == 0`** (1180/6669,
  1161/6695): the guest writes no colour for them and we paint them anyway. A
  real, actionable population — just not the predicted one.
- **Alpha test is never enabled anywhere**, 0 across every row of both runs. That
  rules it out of this problem entirely.
- **`edram_mode` 5 and 6 occur** (mode 5 on ~4000 fallback draws) and the SDK's
  `EdramMode` names only `kNoOperation = 0` and `kColorDepth = 4`. Left as raw
  numbers rather than guessed at.
- The population mix early in a run is nothing like the full-run totals — packed
  is 19-30 draws here against 19546 across a whole run. Do not compare a 7500-draw
  window against a full-run figure.

#### Two instrument traps this round hit

Both would have produced a confident wrong answer.

1. **`Clear()` wipes the register shadow every frame** (`hooks_frame.cpp:212`)
   and the packets are replayed. A register set once at init and never re-set
   therefore reads zero in every frame we look at — and a colour mask of zero is
   precisely the finding being hunted. `DrawCall::om_seen` records which
   registers had actually been written this frame before the draw, so
   "guest set it to zero" is distinguishable from "nobody wrote it". Any future
   probe reading a context register needs the same guard.
2. **A 20000-draw reporting modulus reports nothing.** A 150s run reaches
   5000-10000 transcoded draws, so the first probe logged not one line and
   looked like the code was never reached. `NoteOutputMerger` reports every 2500.

### The D3D9 entry points are located, and the draws are not inlined (2026-08-03)

This supersedes *"Why not high-level D3D9 interception"* above and amends *"There
are no static vertex declarations to find"*. Both of those read the evidence
correctly; both drew a conclusion wider than the evidence supported.

**The input.** A matching XDK `d3d9.lib`, now at `assets/d3d9/` (gitignored —
proprietary XDK material, same class as `assets/default.xex`). It holds 43
PowerPC objects, all machine `0x01F2` (`IMAGE_FILE_MACHINE_POWERPCBE`), and
**2507 defined symbols** dumped to `assets/d3d9/d3d9_symbols.txt` as
`obj⇥section⇥offset⇥mangled_name`.

**FLIRT is a dead end and should not be revisited.** The `pcf.exe` in the
IDASignMaker package is a VC6-era x86 build that rejects PowerPC objects with
`bad coff magic` at every `-g` value (762 / 498 / 0x1F2 / 0762 / big-endian
forms), and no official FLAIR is installed. It is also unnecessary — the COFF
symbol tables parse directly and byte matching is more precise than signatures.

**Why byte matching works here.** The library is compiled with function-level
linking, so **every symbol sits at offset `0x0` of its own COMDAT `.text`
section** and a function's bytes are exactly that section's raw data — no
boundary guessing. The only bytes the linker rewrites are relocation sites, and
density is low: `DrawIndexedVertices` is 23 relocations across 285 instructions.
`tools/match_d3d9.py` emits an IDA pattern with all four bytes of each relocated
instruction word wildcarded (`VirtualAddress & ~3`); `IMAGE_REL_PPC_REL24`
(`0x06`) dominates, with `0x10`/`0x11`/`0x12` appearing at the odd `lis`/`ori`
pair.

**Known-answer test, passed before anything unknown was matched.**
`swap.obj`'s `D3DDevice_Swap` is **1668 bytes = `0x684`**, the exact recorded
size of `sub_82566B58` — the one function already confirmed as D3D9's swap via
the sole binary-wide `VdSwap` xref. Its pattern matches **`0x82566B58` and
nothing else**. Same size and same bytes, from an independently obtained
library: the XDK build is the right one.

**The addresses.** Every target matched, each to exactly one address. Renamed in
`assets/default.xex.probe.i64`.

| guest addr | function | bytes | xrefs |
|---|---|---|---|
| `0x8254B7C0` | `D3DDevice_SetStreamSource` | 288 | **63** |
| `0x8254E748` | `D3DDevice_SetTexture` | 396 | **307** |
| `0x825508A8` | `D3DDevice_SetVertexShader` | 460 | **84** |
| `0x82550A90` | `XGSetVertexDeclaration` | 236 | 2 (D3D9-internal) |
| `0x82550B80` | `D3DDevice_CreateVertexDeclaration` | 128 | **18** |
| `0x825561B0` | `D3DDevice_DrawVertices` | 1044 | **34** |
| `0x825565C8` | `D3DDevice_DrawIndexedVertices` | 1140 | **19** |
| `0x82564C50` | `D3D::PatchVertexShaderToMatchVertexDeclaration` | 1572 | 3 (D3D9-internal) |
| `0x82566B58` | `D3DDevice_Swap` | 1668 | 1 |

**The draws are real calls.** This was the question that gated the whole route —
XDK D3D9 inlines heavily in release builds, and had the draws been compiled into
game code as direct command-buffer writes there would be nothing to hook. They
were not: 53 call sites for the two draw entry points, spread across game code
at `0x821E`, `0x8232`, `0x823E`, `0x823F`, `0x8243`, `0x8247`. **A hook point
exists.**

`PatchVertexShaderToMatchVertexDeclaration` and `XGSetVertexDeclaration` have
only D3D9-internal callers, which is expected — they are lazy-state internals
reached from `SetVertexShader` and `CreateVertexDeclaration`, not game-facing
API. That is a fine hook point too; it is simply not one the game calls.

**What this changes.** `SetVertexDeclaration`/`CreateVertexDeclaration` and
`SetStreamSource` carry the stream layout, offsets, types and semantics that
`PickColorAttribute`, the stride heuristic and the format-38 fallback have been
guessing at for six rounds. `SetTexture`'s 307 call sites are the texture
binding the renderer currently models not at all.

**What it does not change yet.** A located address is not a hook.
`sub_82566B58` was located rounds ago and still is not hooked. Nothing under
`src/` was touched this round, and no measurement was taken — wiring any of this
up is a separate round with its own before/after.

**Reproducing it.** `assets/d3d9/` is gitignored and not in the repository; the
lib is required to regenerate any pattern. `python tools/match_d3d9.py --all`
emits every pattern, `python tools/match_d3d9.py <obj> <symbol>` emits one.
Feed the first ~40 tokens to `find_bytes`; a pattern that returns more than one
match is not an identification. Do not trust a match under 128 bytes — the ~100
`D3DDevice_SetRenderState_*` leaves in `state.obj` are 20-56 bytes with zero
relocations, and several are byte-identical to each other.

### The vertex declarations, read from the guest at runtime (2026-08-03)

First use of the D3D9 hooks. `src/hooks/hooks_d3d9.cpp` hooks
`D3DDevice_CreateVertexDeclaration` (`0x82550B80`), `DrawIndexedVertices`
(`0x825565C8`) and `DrawVertices` (`0x825561B0`). All three pass through to the
original and change no guest state.

**The hooks work and carry the whole draw load.** 170,000 draws in a 165s run —
63,308 `DrawIndexedVertices`, 106,692 `DrawVertices`. That closes the caveat
left by the previous round: 53 static call sites really are the game's draw
path, and the guest runs normally with the hooks in place.

**`D3DVERTEXELEMENT9` is 12 bytes on Xenon, not the 8 of the PC struct.** Both
`CreateVertexDeclaration` and `XGSetVertexDeclaration` walk the array with
`lhzu r9, 0xC`. This is one reason the earlier static search found nothing — it
looked for the 8-byte sentinel. (Correcting the stride does not resurrect that
search: four different 12-byte `D3DDECL_END` forms also return zero matches, so
the declarations really are runtime-only.)

The layout, confirmed by the decode below rather than assumed:

```
  0  Stream      u16   (0xFF terminates the array)
  2  Offset      u16
  4  Type        u32   Xenon packed format descriptor
  8  Method      u8
  9  Usage       u8    D3DDECLUSAGE
 10  UsageIndex  u8
 11  padding     u8    observed as FF/7C/78/3B/60 — the runtime never reads it
```

**23 declarations exist, and that is the whole population.** A first run hit 23
against a cap of 24; the cap was raised to 512 and the run extended from 100s to
140s, producing byte-identical output. Element counts 1-8, five of them
two-stream.

Example — declaration #12, a skinned mesh, stride 36:

| offset | usage | type |
|---|---|---|
| 0 | 0 POSITION | `00 1A 23 60` |
| 8 | 3 NORMAL | `00 2A 21 87` |
| 12 | 2 BLENDINDICES | `00 1A 22 86` |
| 16 | 1 BLENDWEIGHT | `00 1A 20 86` |
| 20 | 5 TEXCOORD0 | `00 2C 23 5F` |
| 24 | 5 TEXCOORD1 | `00 2C 23 5F` |
| 28 | 10 COLOR | `00 18 28 86` |
| 32 | 6 TANGENT | `00 2A 21 87` |

**Why the decode is trustworthy.** Two independent confirmations, neither fitted
to the data:

1. The usage numbers land exactly on the `D3DDECLUSAGE` enum order recovered two
   rounds ago from D3DX9's semantic-string parser (`sub_8257A1B0`) — POSITION 0,
   BLENDWEIGHT 1, BLENDINDICES 2, NORMAL 3, TEXCOORD 5, TANGENT 6, COLOR 10.
2. Declaration #2's offsets run 0, 8, 12, 16, 20, 24 — **stride 28**, precisely
   the stride the PM4 translator has been special-casing since the draw-
   submission round. Two paths built from different evidence agree.

**So colour is no longer a guess.** For the stride-28 layout it is at byte
offset 20, format `0x00182886`; for the stride-36 layout at offset 28. Compare
against `PickColorAttribute`.

**Two things this exposes.**

- **Multi-stream declarations exist** — #11, #15, #16, #17, #22 have
  `max_stream=1`, typically position alone in stream 1 and everything else in
  stream 0. The translator's stride heuristic assumes one interleaved stream, so
  those draws were never going to be read correctly.
- **The `Type` dwords are undecoded.** They are Xenon packed format descriptors,
  not `D3DDECLTYPE` enum values. Element *sizes* fall out of the offset deltas
  (`00 18 28 86` = 4 bytes, `00 1A 23 60` = 8, `00 1A 23 A6` = 16), which is
  enough to act on before the encoding is cracked. Do not guess the rest.

**Instrument trap, hit and fixed.** The first run logged every declaration and
lost all of them: declarations are built during load, and the rotating log
(3 x 5MB) retained only the last ~50s of a 165s run. Anything created early must
be written to a non-rotating file. The dump is `d3d9_dump_decls.txt` next to the
executable, gitignored alongside `pm4_dump_*.txt`. **Check the dump exists and
has content before reading anything into a negative result** — this is the third
round to lose a probe's output this way.

### 12 of 23 declarations have no COLOR element (2026-08-03)

Decoding the captured declarations by `Usage` changes the diagnosis that six
rounds of colour work rested on.

| declarations | usages |
|---|---|
| #13, #14 | POSITION TEXCOORD4 TEXCOORD0 NORMAL TANGENT TEXCOORD2 |
| #15, #16 | POSITION TEXCOORD0 TEXCOORD1 [TEXCOORD2] |
| #9 | POSITION TEXCOORD0 |
| #1, #3, #5, #10, #20, #21, #23 | POSITION only |

**11 of 23 carry a COLOR element; 12 do not.** The ones that do not include the
normal-mapped, multi-texcoord meshes — real scene geometry, not helpers.

**So the ~12700 `kNone` draws are very likely not `PickColorAttribute`
failing.** That geometry has no vertex colour to find; its colour comes from
textures sampled in a pixel shader. This renderer runs no pixel shader
(`kGamePS` is `return col;`) and binds no texture, so it writes them opaque
white. **The missing pixel-shader and texture path is the leading explanation
for the white frame**, and it is not something an HLE renderer would fix by
itself.

Stated as strongly as the evidence allows and no further: the correlation that
would turn "very likely" into "measured" was attempted and did not complete —
see below.

**Strategic conclusion, so it is not re-litigated.** The D3D9 route is viable
and its inputs are reachable; it is *not* the current bottleneck. Do not start a
D3D9 → D3D12 renderer to fix the white frame. HLE hands over declarations, which
a ~200-line pass-through hook already delivered; it does not hand over a pixel
shader.

#### The render states are not inlined either

Eight `SetRenderState_*` leaves matched uniquely from `state.obj`. These are
fully-literal patterns — zero relocations — so the matches are as strong as
matching gets. Caller counts are game code, and this is the output merger the
previous round found is modelled *not at all*:

| function | address | callers |
|---|---|---|
| `SetRenderState_ZEnable` | `0x82549AD8` | 79 |
| `SetRenderState_AlphaBlendEnable` | `0x82549448` | 76 |
| `SetRenderState_SrcBlend` | `0x82549568` | 46 |
| `SetRenderState_DestBlend` | `0x825495F8` | 45 |
| `SetRenderState_BlendOp` | `0x825494D8` | 34 |
| `SetRenderState_ColorWriteEnable` | `0x8254A078` | 19 |
| `SetRenderState_SeparateAlphaBlendEnable` | `0x825497D8` | 6 |
| `SetRenderState_BlendFactor` | `0x82549900` | **0 — never called** |

#### The declaration is NOT a plain pointer on the device — two confirmations

The draw entry points receive `D3DDevice*` but not the declaration, and it is
not recoverable by reading the device:

1. **Memory probe, negative.** Scanning `device + 0..0x2000` for a dword equal
   to a declaration observed being created finds **no offset that holds across
   8 probed draws**. `0x2000` covers the whole struct — `DrawIndexedVertices`
   itself indexes `device + 0x1780`.
2. **The code agrees.** `sub_82565550`, one of the two callers of
   `PatchVertexShaderToMatchVertexDeclaration`, reaches its state through an
   indexed relative-offset table (`r28 = (r10 + 0x70) * 8`, then
   `lwzx r11, r28, r5; add r11, r11, r5`) rather than a fixed field.

**Next attempt should hook `PatchVertexShaderToMatchVertexDeclaration`
(`0x82564C50`) directly** — it *receives* `const CVertexDeclaration*` as an
argument on the lazy-state path at draw time, which is precisely the binding
wanted, with no struct spelunking. Its 3 xrefs are D3D9-internal, which is why
it is reached at draw time rather than called by the game.

#### Two instrument traps, both hit

- **The guest arena is sparse.** The first probe dereferenced each device dword
  looking for XGSetVertexDeclaration's `0x00100005` magic and crashed the guest
  with an access violation at `0x030013A0`. **Never dereference a value read out
  of guest memory unless its target is already known good.** The fix was to
  compare against declarations recorded as they were created — no speculative
  reads at all, and stronger evidence besides.
- **A negative from an under-scoped probe is not a negative.** The second run
  scanned only `0x400` of a `>=0x1780` struct and reported "no offset holds a
  declaration". That conclusion was an artifact of the window, not a fact.
  State the scanned range with any negative result.

### The declaration-per-draw binding works, and the no-colour draws are position-only (2026-08-03)

**Route:** hook `PatchVertexShaderToMatchVertexDeclaration` (`0x82564C50`),
which *receives* the declaration on the lazy-state path at draw time. The
device-struct route is a dead end — see the previous section.

**The declaration is argument `r5`**, and that is established by comparing every
argument register against declarations watched being created by
`CreateVertexDeclaration`, not by reading the mangled signature. It agrees with
the signature, but a misread would have been invisible in the output.

**140,000 draws attributed, zero unattributed**, 2328 patch calls, no access
violations.

| | draws |
|---|---|
| declarations **with** COLOR | 102,500 |
| declarations **without** COLOR | 37,500 |
| unattributed | **0** |

**The no-colour population is dominated by one position-only declaration.**

| id | elements | colour | draws |
|---|---|---|---|
| 0 | **1 (POSITION only)** | no | **36,352** |
| 1 | 6 | yes | 101,924 |
| 2 | 1 (POSITION only) | no | 912 |
| 15 | 4 | no | 176 |
| 14 | 3 | no | 53 |

**36,352 of 37,500 no-colour draws — 97% — use a declaration with a single
POSITION element.** A position-only layout *cannot* carry vertex colour, so
`PickColorAttribute` was never going to find one. That settles the question the
round was asked, and by firmer evidence than the `kNone` count would have given:
it is a property of the declaration, not a statistic.

Combined with the output-merger round (colour mask `0xF`, depth write never,
~64% non-trivial blend), position-only geometry that writes all four colour
channels and blends is consistent with screen-space effect or particle passes
coloured entirely by a pixel shader. **We run no pixel shader, so they come out
opaque white.**

#### The renderer drops most draws on stride alone

From the same run's `RenderThread` line, cumulative skipped strides:

```
8:3943   12:354   16:4934   36:21992   38:994   44:756   64:753
```

**21,992 draws skipped for being stride 36** — which is exactly declaration #12,
the 8-element skinned mesh (POSITION NORMAL BLENDINDICES BLENDWEIGHT TEXCOORD0
TEXCOORD1 COLOR TANGENT). The translator submits stride-28 draws and drops
everything else. Under a declaration-driven path stride 36 is simply another
input layout; there is nothing special to solve.

### The full D3D9 entry-point table (2026-08-03)

Thirty functions located, each matching exactly one address. Method and proof in
the earlier D3D9 section; patterns regenerate with `tools/match_d3d9.py`.

| function | address | function | address |
|---|---|---|---|
| `DrawIndexedVertices` | `0x825565C8` | `SetRenderTarget` | `0x8254C060` |
| `DrawVertices` | `0x825561B0` | `SetDepthStencilSurface` | `0x8254C3B0` |
| `SetStreamSource` | `0x8254B7C0` | `SetViewport` | `0x8254BF50` |
| `SetIndices` | `0x8254B8E0` | `SetScissorRect` | `0x8254B678` |
| `SetVertexShader` | `0x825508A8` | `Clear` | `0x8255B258` |
| `SetPixelShader` | `0x825506E8` | `Resolve` | `0x8255CE98` |
| `CreateVertexShader` | `0x82552330` | `SetTexture` | `0x8254E748` |
| `CreatePixelShader` | `0x82552148` | `CreateTexture` | `0x8254E3C8` |
| `SetVertexShaderConstantFN` | `0x82550320` | `CreateVertexDeclaration` | `0x82550B80` |
| `SetPixelShaderConstantFN` | `0x825503F8` | `XGSetVertexDeclaration` | `0x82550A90` |
| `PatchVertexShaderToMatch…` | `0x82564C50` | `Swap` | `0x82566B58` |

Plus the eight `SetRenderState_*` leaves in the previous section.

**Not located, and why:** `SetVertexDeclaration` (20 bytes), `SetViewportF`
(32) and `D3DDevice_Resolve`'s smaller siblings are under the 128-byte
threshold where a byte match stops being evidence. `SetVertexDeclaration` is not
needed — `PatchVertexShaderToMatchVertexDeclaration` supplies the same binding.

`SetVertexShaderConstantFN` and `SetPixelShaderConstantFN` matched but IDA would
not rename them: auto-analysis had not defined a function at those addresses.
The addresses are confirmed by the match; the IDB simply has no function object
there yet.

### The Type dword is decoded, from the function that consumes it (2026-08-03)

First HLE step. `src/gpu/d3d9_layout.{h,cpp}` turns a guest `D3DVERTEXELEMENT9`
array into a host input layout; `tools/d3d9_layout_test.cpp` checks it against
all 23 captured declarations.

**None of this is inferred from the data.** An earlier attempt to guess the bit
layout by trying rotations and swizzle positions produced nonsense. The answer
is in `D3D::PatchVertexShaderToMatchVertexDeclaration` (`0x82564C50`), the
function that *reads* Type and writes the matching fields of the shader's vfetch
instruction:

```c
dword1 = (((t << 12) & 0x3F000) | (t & 0x300)) << 4 | dword1 & 0xBFC0CFFF;
```

The preserved mask clears exactly bits `[21:16]`, `[13:12]` and `[30]` of vfetch
dword1, which by `ucode.h` are `format`, `fomat_comp_all`, `num_format_all` and
`is_mini_fetch`. Equating the two sides gives the field layout outright:

| Type bits | meaning |
|---|---|
| `[5:0]` | `xenos::VertexFormat` |
| `[8]` | `fomat_comp_all` — 1 = signed |
| `[9]` | `num_format_all` — 0 = normalized, 1 = integer |
| `[21:10]` | vfetch destination swizzle, **x in `[12:10]`, w in `[21:19]`** |

**The two bits at `[9:8]` are what make the layout correct rather than
plausible.** `COLOR` is `0x00182886` and `BLENDINDICES` is `0x001A2286` — the
same format 6 (`k_8_8_8_8`), differing only there. One is `R8G8B8A8_UNORM`, the
other `R8G8B8A8_UINT`. A decode ignoring those bits would turn every blend index
into a fraction and every byte-packed normal into an unsigned value, and the
result would still look like geometry.

**The size table is the guest's own**, lifted verbatim from `0x8204E188`.
`PatchVertexShaderToMatchVertexDeclaration` indexes it by the vfetch format field
to compare attribute extents. Entries are **dwords**; every format the runtime
considers illegal is 0. This replaces a by-hand derivation from a previous round
— that derivation happened to agree on all ten formats present in the captures,
but this is the runtime's answer for all 64.

**The swizzle's component order was not read out of that arithmetic.** The shift
chain is dense enough that taking a direction from it is not evidence, and the
first write-up here had x and w the wrong way round. The captures settle it,
because only one reading makes all four component counts come out right:
4-component formats give `0x688` = `(x,y,z,w)`, 3-component `0xA88` =
`(x,y,z,1)`, 2-component `0xB08` = `(x,y,0,1)`, 1-component `0xB20` =
`(x,0,0,1)`, and every 8_8_8_8 `COLOR` gives `0x60A` = `(z,y,x,w)`, which is
D3DCOLOR's BGRA arriving as RGBA. Values are 0-3 for xyzw, 4 for constant 0, 5
for constant 1. **The swizzle stays in the shader**, where D3D9 itself puts it;
a DXGI format that reordered components would apply it twice.

**The one format DXGI cannot hold** is signed normalized `k_2_10_10_10`, which
this title uses for `NORMAL` and `TANGENT`. There is no `R10G10B10A2_SNORM`.
The decoder passes the raw bits through as `R10G10B10A2_UINT` and sets
`Unpack::kSnorm2_10_10_10` so the shader must finish the conversion. Picking
`UNORM` instead would halve and bias every normal — and look fine.

**Test result: all 23 declarations pass.** Each element's decoded size equals the
gap to the next element's offset in the same stream, and declaration #2 lands on
stride 28 and #12 on 36 — the two numbers the PM4 translator measured
independently. Offsets from the game, sizes from the runtime's table, strides
from a different pipeline: three unrelated sources agreeing. The test also
asserts the decode *rejects* format 0, format 5, `k_11_11_10`, a normalized
32-bit integer format, an out-of-range usage and a non-dword-aligned offset.

```
clang++ -std=c++23 -I src -I C:/rexglue-sdk/include \
    -o d3d9_layout_test.exe tools/d3d9_layout_test.cpp src/gpu/d3d9_layout.cpp
```

### The state shadow works; the declaration binding does not (2026-08-03)

`src/gpu/d3d9_state.{h,cpp}` plus fifteen more hooks in `hooks_d3d9.cpp`:
`SetStreamSource`, `SetIndices`, `SetVertexShader`, `SetPixelShader`,
`SetTexture`, `SetViewport`, `SetScissorRect` and the eight `SetRenderState_*`
leaves. Behind `hle_capture`, default **off**; nothing is submitted or rendered.
Signatures come from the typed decompilations, not the PC headers — several
differ, and `DrawVertices(pDevice, PrimitiveType, StartVertex, VertexCount)` was
read rather than assumed once its arguments started mattering.

**Resource fields are read at Set time and never at draw time.**
`D3DVertexBuffer` is `D3DResource` (24 bytes) then a Xenos vertex fetch constant
at `+0x18`; `D3DIndexBuffer` is `Address` at `+0x18` and `Size` at `+0x1C`. The
fetch constant is decoded the way `Pm4Translator::CollectVertexFetches` already
decodes it — `address = dword0 & ~3`, `size_bytes = ((dword1 >> 2) & 0xFFFFFF) *
4` — because that decode is the validated one. A first pass copied
`SetStreamSource`'s own `0x1FFFFFFF` mask instead and left the two type bits in
the address. **The index width is bit 31 of the object's `Common` dword**, not a
separate field: `DrawIndexedVertices` branches on `if (*pIndexBuffer < 0)` and
takes `4 * StartIndex` on that side against `2 * StartIndex` on the other.

**What a 165s run says.** 165,000 draws (61,839 indexed, 103,161 not). 95% are
"fully described" by the seen-flag criterion; the cross-checks are what matter:

| check | result |
|---|---|
| index buffer holds the draw's index range | **61,839 / 61,839 — every indexed draw** |
| vertex buffer holds the draw's vertex range | 19,291 / 103,482 |
| bound stride vs the layout's own minimum | exact 37,076, padded 121,647, **too small 6,743** |

The index side agreeing on every single indexed draw is strong evidence that the
address, size and width decode is right. The vertex side is not, and the named
failures say why:

```
STRIDE TOO SMALL: declaration id 1 stream 0 needs 28 bytes, bound stride is 8
VB DOES NOT HOLD RANGE: declaration id 2 stream 0 start_vertex=13748 count=108
                        stride=48 needs 665088B, buffer is 338592B
```

**The declaration attributed to each draw is stale.** It comes from
`PatchVertexShaderToMatchVertexDeclaration`, which fires only when the lazy state
is dirty: **2,508 calls against 165,000 draws**, about one update per 66 draws.
The previous round reported "140,000 draws attributed, 0 unattributed" — that
count was right, and the conclusion drawn from it was not. Attribution to a value
that has not been refreshed is not attribution.

Two competing explanations were **eliminated**, both from evidence already on
disk rather than from new runs:

- *Multiple devices.* Five distinct pointers appear in `r3`, but `0x40BC5F80`
  takes **1,197,869** calls and the other four take **201 between them** — three
  of them exactly 25 calls each, only from `SetTexture`, `SetScissorRect` and
  `SetRenderState_*`, at init. 0.05% of calls cannot explain 81% of draws.
- *Multiple threads.* Every `d3d9: draws` line within a single run carries one
  thread id.

One device, one thread, a stale declaration.

**Stop condition reached, as the plan required.** The description is not yet
trustworthy enough to render from, the reason is specific rather than diffuse,
and nothing was built on top of it.

**The next step, and why it should work.** The device struct needs rescanning for
the current declaration — and the earlier scan that found nothing was
under-scoped **for the second time**. It covered `device + 0..0x2000`, but
`SetStreamSource` writes `+13440` (`0x3480`) and `DrawIndexedVertices` reads the
bound index buffer from `+12612` (`0x3144`), so the struct is at least `0x3480`
bytes and less than half of it was searched. Rescanning `0..0x4000` against the
known-declaration set is the same safe comparison as before — no unknown pointer
is dereferenced — just correctly scoped.

> **Resolved 2026-08-03, and the rescan was never needed.** See the next
> section: the offset is `0x2ED8`, and it was read out of
> `D3DDevice_SetVertexDeclaration` rather than searched for.

**Instrument traps confirmed again this round.** A bare "MORE THAN ONE devices"
flag said nothing; recording the pointers and which entry points touched each one
turned it into a dismissal in one run. Likewise a bare count of stride failures
would not have distinguished a wrong decode from a stale declaration — the eight
named cases did.

### The declaration is at device + 0x2ED8, and the library says so (2026-08-03)

The stale-declaration blocker above is gone. Every draw now reads its own
declaration, and the three numbers that were wrong are right.

**The planned rescan was the wrong move, and it was not needed.** The plan was to
search `device + 0..0x4000` for a field holding a known declaration. Instead:
`D3DDevice_SetVertexDeclaration` is 20 bytes and does nothing else.

```
stw   r4, 0x2ed8(r3)      device->pVertexDeclaration = pDecl
ld    r11, 0x10(r3)
oris  r11, r11, 0x8       mark the lazy state dirty
std   r11, 0x10(r3)
blr
```

`D3DDevice_GetVertexDeclaration` reads the same field back (`lwz r31,
0x2ed8(r3)`), so the offset is confirmed by a second function that does not
share the first's encoding. This is the same method that decoded the `Type`
dword: **read the field out of the code that touches it.** Two scans had already
"proved" the declaration was not on the device — both covered `0..0x2000`, and
`0x2ED8` is outside that. The conclusion was never sound; the scans were just
too short.

Note that `SetVertexDeclaration` was written off earlier as unlocatable — at 20
bytes a byte-pattern match is not evidence. That is still true and did not
matter: **the function's bytes are readable in `d3d9.lib` whether or not its
address in the XEX is known.** The offset is a constant in the instruction
stream, so the library answers the question by itself.

**Reading `device + 0x2ED8` is safe; following its value is not.** The device
pointer is the draw's own `r3`, D3D9 is reading the same struct on either side of
the hook, and the offset is well inside it. The value read is only ever compared
against declarations watched being built by `CreateVertexDeclaration` — the same
rule that the access violation at `0x030013A0` established.

**What changed, over 177,500 draws:**

| | before (patch hook) | after (device + 0x2ED8) |
|---|---|---|
| fully described | 158,257 / 165,000 (95%) | **177,500 / 177,500 (100%)** |
| stride TOO SMALL | 6,743 | **0** |
| index buffer holds the range | 61,839 / 61,839 | 66,726 / 66,726 |

**`null=0 unknown=0`.** Not one draw in 177,500 found the field empty, and not
one found a pointer that `CreateVertexDeclaration` had not produced. Reproduced
on a second run at 175,000 draws. A wrong offset would fail loudly here, because
matching a 32-bit pointer against the known set by accident is not something that
happens 177,500 times.

**The stride failures were entirely the stale declaration, not a bad decode.**
`STRIDE TOO SMALL` going 6,743 → 0 with no change to `d3d9_layout.cpp` settles a
question the previous round could not: the layout decoder was always right, and
it was being handed the wrong declaration. That instrument was worth building —
a bare completeness percentage would have moved 95% → 100% and said nothing about
which of the two was at fault.

**How stale the old source was, measured: 143,226 of 177,500 draws (81%)** had a
declaration bound that differed from the one
`PatchVertexShaderToMatchVertexDeclaration` last saw. The earlier round's
"140,000 draws attributed, 0 unattributed" was a correct count of a meaningless
quantity.

#### What is still open, stated precisely

**The vertex-range check did not improve: `vb 20,125 / 210,799`.** It is not the
declaration, and it should not be read as one problem with the vertex path. The
named failures are a single recognisable shape:

```
VB DOES NOT HOLD RANGE: declaration id 14 stream 1 start_vertex=13748 count=108
                        stride=16 needs 221696B, buffer is 64B
```

Declaration 14 is three `k_32_32_32_32_FLOAT` elements — `POSITION`, `TEXCOORD0`
on stream 0, `TEXCOORD1` on stream 1 — and stream 1's whole buffer is **64 bytes,
four float4s**, while `StartVertex` is in the thousands.

**The check assumes every stream is indexed by the same vertex index. That is a
D3D12 input-assembler assumption, and Xenos does not work that way** — the vertex
shader issues explicit `vfetch` instructions with whatever index register it
computes, so a four-entry stream fetched by something like `index % 4` is legal
and normal. A 64-byte buffer being read at vertex 13,748 is not the game being
broken; it is most likely the host model being wrong.

That is a hypothesis with an obvious test — read the shader's `vfetch` for stream
1 and see what indexes it — and it is **not** established here. What is
established: the failure is confined to small auxiliary streams, not to stream 0
geometry, and it does not implicate the declaration, the stride, or the fetch
constant decode. **It does mean a stream like this cannot be modelled as a plain
D3D12 input slot**, which matters for the PSO step and should be settled before
it.


### The vertex range check was the thing that was wrong (2026-08-03)

Stage 0 of HLE step 2. The open question was why only **20,125 of 210,799**
stream checks found the vertex buffer holding the draw's range, while index
buffers passed 66,726/66,726. Two hypotheses, and the probe separated them.

**Eliminated: a second binding path.** `blocks.obj` carries
`?SetStreamSource@D3DDevice@@QAAJIPAUD3DVertexBuffer@@II@Z` beside the
`D3DDevice_SetStreamSource` that is hooked, along with state-block variants of
`SetTexture`, `SetRenderState`, `SetVertexDeclaration` and `D3DStateBlock_Apply`
— so binds could plausibly have been reaching the device unseen. They are not:

```
stream 0: device fetch constant vs our snapshot — same 172500  differ 0
stream 1: device fetch constant vs our snapshot — same  97932  differ 0
```

**270,432 comparisons against the device's own fetch constant — the value the
GPU actually fetches through — and not one disagreement.** Nothing bypasses the
hook.

**The bind-age correlation looked like evidence and was not.** Draws that pass
sit a mean of 3 draws from their last bind; draws that fail sit at 36, worst 75.
That is exactly what a stale shadow would produce, and it is not one — the
device says so. It is simply that draws far into a batch are draws into a large
shared buffer at a high `StartVertex`. **A correlation that survives one test
and dies to a direct measurement is why the direct measurement was worth
building.**

**So the description was never wrong; the check was.** It computes
`(StartVertex + count) * stride` and requires the buffer to hold it, which
assumes every stream is indexed by one common vertex index. That is a **D3D12
input-assembler assumption, and Xenos does not honour it** — the vertex shader
issues its own `vfetch` with whatever index it computes, so a 64-byte four-entry
stream read at `StartVertex` 13,748 is legal. Stream 1 fails **97,172 of
97,172** — never once passing — which is the shape of a stream that is not
indexed like stream 0 at all, not the shape of an intermittent bug.

**Stated honestly: this is the surviving hypothesis, not a demonstrated one.**
The direct test is to decode the bound vertex shader's `vfetch` for stream 1 and
read how it is indexed. What *is* demonstrated is the part Stage 2 depends on —
the address, size, endian and stride are right, confirmed against the device
270,432 times.

#### The fetch constant file is at `device + 0x6F4 + (0x11 - stream) * 8`

Found by intersection, not by reading the code — and the code then confirmed it.
At `SetStreamSource` the exact dwords are known, so every device offset holding
one is a candidate; intersecting the candidate sets over 64 different binds left
**exactly one offset out of 4,096: `0x77C`** for stream 0's dword1.

That retro-fits `SetStreamSource`'s own arithmetic. A first reading of the
unlinked COMDAT dismissed `subfic r11, r4, 0x11` as dead because
`addi r11, r4, 0xde` overwrites it — but `(0x11 - stream) * 8 + 0x6F4` is
`0x77C` exactly. **Two methods that failed separately agree completely.** The
static read alone produced `device + stream*8`, which collides with the
lazy-state qword at `+0x10`, and was correctly not built on.

`dword0` had no surviving offset because D3D9 ORs a flag bit in after masking
(`rlwinm r11, r11, 0, 19, 19` then `add`), which none of the four candidate
maskings included.

#### Two instrument traps, one of them self-inflicted

- **A guessed scan bound is not a bound.** The probe scanned `device + 0..0x4000`
  and faulted at guest `0x1D00B000`. It was re-bounded to `0x3484` — justified by
  `SetStreamSource` writing `+0x3480`, which proves that offset mapped *for some
  device* and nothing about this one — and faulted at the identical address. A
  `VirtualQuery` guard per page then reported the struct **readable to the full
  `0x4000`**: the scan had never been the problem, and two rounds were spent
  "fixing" it. Bisecting by disabling the other new read found the real one in
  one run.
- **The real fault was the index-buffer walk**, from the `0x1FFFFFFF` mask in
  `SetIndices`. The reason it is wrong took two attempts to state correctly:

  > **First diagnosis, wrong:** "the same mask already found wrong for vertex
  > buffers, which clears the top three bits instead of the bottom two."
  > `DrawIndexedVertices` does exactly `rlwinm r11, r11, 0, 3, 31` on the
  > address, so D3D9 applies that very mask.
  >
  > **Correct:** D3D9 applies it *to produce a physical address for the GPU*.
  > Every read on this side goes through the guest's **virtual** space, where
  > the buffer lives at the unmasked address. Masking relocates it — an index
  > buffer at `0xF3B64000` was recorded as `0x13B64000`, which is the `ib=` value
  > sitting in the logs the whole time. The vertex path escaped this because it
  > uses `& ~3`, keeping the high bits.

  The **66,726/66,726 "index buffer holds its range" result does not contradict
  this, and could not**: it compares a count against `Size` and never
  dereferences `Address`, so a relocated address passes it every time. A check
  that cannot fail on the thing it appears to validate is worth less than it
  looks — and this one read as reassurance for two rounds.

### Parser caveat

`Pm4Parser` accepts any Type-3 with `body_word_count <= 0x4000`. A misparsed
header at `0xBEBE0B80` (opcode 0x5D, count 8449) swallows the rest of the ring.
Now that real command streams are being parsed this matters: a post-load frame
also logs 70 `DRAW_INDX invalid header` and 42 `DRAW_INDX_2 body too small`
warnings, which is the same desync showing up as lost draws.

Since the register and opcode tables were replaced with the SDK's (below), this
desync is now the *only* thing left unnamed in a dump, which makes it easy to
find: post-load frame 600 has 11 unresolved Type0 register indices out of 4258
and 4 unresolved Type3 opcodes out of 1426. Every one is an artifact — register
indices past the 0x5002 top of the file (`0x8271`, `0xAAAB`, `0x8770`) and
opcodes that do not exist (`0x00`, `0x38`). Nothing legitimate is unnamed.

### The SDK ships Xenia's GPU layer, header-only (2026-08-02)

`C:\rexglue-sdk\include\rex\graphics\` is Xenia's GPU code rebranded to
`rex::graphics`. **The bit layouts and tables are header-only and free** — no
linking, since `rexgpu-xenos.lib` is a 2-symbol plugin import stub and the
plugin is `LoadLibrary`'d, never linked. Usable today:

- `format/ucode.h` — `VertexFetchInstruction` (every accessor: fetch constant
  index, format, stride, offset, signedness), `ControlFlowInstruction`,
  `UnpackControlFlowInstructions`, `IsControlFlowOpcodeExec`
- `xenos.h` — `VertexFormat`, `PrimitiveType` (**`kQuadList = 0x0D`**), `Endian`,
  `GetVertexFormatComponentCount`, and the full `PM4_*` opcode list
- `register_table.inc` — 3434 dword-indexed register names as an X-macro

Sealed inside the plugin DLL and **not** usable: `PacketDisassembler`,
`RegisterFile::GetRegisterInfo`, `Shader::AnalyzeUcode`,
`ParseVertexFetchInstruction`, `PrimitiveProcessor`, `CommandProcessor`,
`SharedMemory`. So the decoding tables come free; the drivers that walk them
have to be written here.

Two tables were replaced with the SDK's as a result:

- **`kRegNames`** in `xenos_gpu_state.cpp`. All ten names previously confirmed
  one at a time against observed values — `RB_SURFACE_INFO`, the
  `PA_SC_WINDOW_*` scissor trio, the `PA_CL_VPORT_*` block — **matched the SDK
  exactly**, which independently corroborates the viewport transform derived
  from them. Note the SDK table is *not* sorted (two out-of-order pairs), so a
  binary search would silently miss entries; the lookup is a direct-indexed
  table of `0x5003` pointers built once.
- **`kOpcodeNames`** in `pm4_parser.cpp`. Its trailing "legacy aliases (AMD R600
  naming)" block for `0x60`–`0x6F` was **wrong for this hardware**. Xenos reuses
  that range for the binning registers and the swap packet: `0x60` was printing
  as `SET_CONFIG_REG` when it is `SET_BIN_MASK_LO`, and `0x64` as
  `SET_LOOP_CONST` when it is `XE_SWAP` — which is why exactly one `0x64` shows
  up per frame in every dump. `0x65`–`0x6F` are not Xenos opcodes at all.

Related, and still uncorrected because neither opcode occurs in any capture:
`XenosGpuState::ApplyType3Packet` has cases labelled `SET_CONTEXT_REG` (0x21)
and `SET_CONFIG_REG` (0x20). Xenos has neither. `0x21` is `REG_RMW`, whose body
is a (mask, or-value) pair rather than a base plus a run, and `0x20` is not an
opcode. Register writes arrive as Type0 packets, which `ApplyType0Write` already
handles correctly.

### Draw-call plumbing

`VdSwap` → `Pm4Translator` → `NativeGraphics::SetDrawCalls` →
`RenderThreadFunc` → `D3D12Renderer::AddGameDraw` → `RenderGameFrame`'s loop
over `m_gameDraws`. End to end and carrying real geometry as of 2026-08-02.

Both of the old caveats here are **void**: the chain is no longer untested, and
`DrawCall::mvp[16]` reaches the renderer's constant buffer. The matrix itself is
now built from the guest's viewport registers rather than the ALU constant file
— see "The zero matrix, and the guest's own viewport".

`RenderThreadFunc` gates submission on `vertex_stride == 28`, the only layout
the game PSO's input layout describes. Everything else is counted into a
per-stride histogram and skipped.

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
| sub_82566B58 (VdSwap) | Counter sync 0x82D21818 → 0x83144208. Parses the frame range `[prev_after, write_before)` and the swap range `[write_before, write_after)` on **every** swap in native mode; XenosGpuState + file dumps + histograms stay sparse |
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
- **PM4 parser**: every swap parsed in native mode, both the frame and swap
  ranges, big-endian byteswap; **350–363 draw calls per frame after a load**
- **GPU state tracking**: 66 Xenos registers shadowed, 32 captured from the first VdSwap
- **Input**: SDL gamepad + Xbox 360 Controller, ReXGlue handles XamInput natively
- **Asset loading**: LoadStateMachine ticks every LoaderTick, real file I/O reaching the VFS
- **LoaderTick renderer block**: runs natively apart from the one skipped dispatch
- EngineInit sleep loop keeps the process alive

### Not working / unverified
- **Game rendering**: guest geometry renders. A 640x360 grey rect lands in the
  top-left quadrant of the 1280x720 target, exactly where the viewport
  arithmetic predicts, 3/3 runs plus control. What is **not** working: only
  stride-28 draws are submitted (85-95 of ~320 per frame) and QuadList — the
  plurality topology — is dropped. See "The zero matrix, and the guest's own
  viewport" below.
  The cause is no longer unknown: as of 2026-08-02 the microcode **is** decoded
  and the real stride, slot and formats are read from the shader — but nothing
  downstream consumes them yet, so the division heuristic still drives the draw
  path. See "The stride is readable, and the heuristic is 96% right", which also
  records that 14% of draws read from the wrong fetch slot entirely and that
  half-float positions outnumber float3 six to one.
  The other two items formerly listed here are resolved: the game RT **is** now
  cleared every frame (it never was, so no screenshot before 2026-08-02 showed a
  single frame), and the black window is **the guest painting black**, not an
  absence of drawing — see "The black screen is drawn, not absent". The
  placeholder triangle's flashing, which the clear fix exposed rather than
  caused, is also fixed — see "The placeholder triangle was drawn on 1 host
  frame in 5"
- **Every guest render pass is flattened into one host target.** 16 distinct
  colour surfaces per run, all overpainting each other; the last one to paint
  wins, which is why the window cycles colours. Filtering by surface pitch was
  tried and made it worse — see "Every render pass is flattened into one target"
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
zero `INDIRECT_BUFFER` (**void — that was the swap range only, and only swaps
1-5; see "The PM4 was never present-only"**), and no file I/O for the requested
scene. That last point
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

#### The registry chokepoint, and `registry_override` (2026-08-02)

**Correction**: the section above calls `sub_82536250` a *game-mode* gate whose
argument is the AssetDB. Both halves are wrong. `sub_82536250` **ignores its
argument entirely** — it reads the registry object from `*(0x82D6605C)`, fetches
one key, and maps the string through `sub_82533D20`. And the vocabulary is
**network mode**, not game mode. Named at runtime, not inferred:

| Index | Name | |
|---|---|---|
| 0 | `SplitScreen` | |
| 1 | `SinglePlayer` | |
| 2 | `Online` | gate passes |
| 3 | `LAN` | gate passes |
| 4 | `None` | **measured** |

(`sub_82533D20` scans the 5-entry table at `0x82D1F810` and returns 5 if nothing
matches; `sub_82536250` returns 1 if the registry read itself fails.)

All guest settings come from `MXRegistry.bxml` through a getter family that
hashes the key with `sub_82473360` and differs only by value type:

| Getter | Type | Signature |
|---|---|---|
| `sub_825487C8` | string | `(registry, key, out, size, 0)`, non-zero if found |
| `sub_82548758` | int | `(registry, key, out, 0)`, written through `out` |
| `sub_82548EA8` | int set | `(registry, key, value, 0)` |

Three keys are now named, all dumped at runtime — **an earlier reading of this
guessed `UISceneName`/`startMode` from `.rdata` spacing and the shipped
`MXRegistry.bxml.xml`; the dump falsified it**, and none of the three keys ship
in that file at all (they are written at runtime):

| Address | Key | Read by |
|---|---|---|
| `0x8200C864` | `Location` | `sub_82352AE0` — the scene name it requests |
| `0x8200C870` | `PlayerMode` | `sub_82536250` — measured `"None"` |
| `0x8204C630` | `ReadyToLaunch` | the state 6 gate's third term |

So the gate reads as *"are we Online/LAN — if not, has anything declared
ReadyToLaunch"*. Its passing branch **writes `ReadyToLaunch = 1` back** via
`sub_82548EA8`, which makes it sticky. Forcing `PlayerMode` to `Online` would
assert a network session that does not exist; `ReadyToLaunch` is the launch
confirmation a front end would set, and is the honest lever.

**`registry_override = "k=v,k=v"`** (`mx.toml` or `--registry_override=...`,
empty = off) substitutes values as they are read, in both getters. `tools/` has
bxml decoders but no encoder, so this is the only way to change a setting.

**Measured**, `--skip_intro=true --force_load=NAT_Farm
--registry_override=ReadyToLaunch=1`, identical 3/3, zero access violations:

```
2 -> 3 -> 4 (~380 ticks) -> 5 -> 6 -> 7 -> 8 -> 11 -> 2
```

The first complete load in this effort — the sequence runs to the end and returns
to idle. The control (same command, no `registry_override`) parks at 6 with the
gate returning 0 and `ReadyToLaunch` reading 0, so the causation is isolated.

**Entity counts moved for the first time**: `pass0=1 pass1=0 pass2=1` flips to
`pass0=0 pass1=1 pass2=1` about one second after the load completes, and stays.
Still **zero `DRAW_*` and zero `INDIRECT_BUFFER`** — the entity block advanced a
pass, it did not produce geometry.

> **Void.** Re-measuring this same configuration with the frame range parsed
> gives **350-363 draw calls per frame**, 453 `DRAW_INDX` and 62 `DRAW_INDX_2`,
> against 3-7 draws pre-load. The load did produce geometry; the hook was
> reading the present sequence. See "The PM4 was never present-only".

`--force_load=UI_World` also completes but spends **1 tick** in state 4 against
~380 for `NAT_Farm`: it loads essentially nothing. `NAT_Farm` is the right
target, which fits the key being named `Location`.

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

### The device struct's constant files, and why Stage 4 is blocked (2026-08-04)

#### The vertex shader constant file is at `device + 0x780`

Read off `D3DDevice_SetVertexShaderConstantFN`'s own arithmetic (`shader.obj`,
`0x82550320`), which is three instructions before it starts storing:

```asm
addi   r10, r4, 0x78          ; StartRegister + 0x78
rlwinm r10, r10, 4, 0, 27     ; * 16 — one vec4 per register
add    r10, r10, r3           ; + the device
```

**Register N is at `device + 0x780 + N * 16`.** `SetPixelShaderConstantFN`
(`0x825503F8`) is byte-for-byte the same function with `0x178` in place of
`0x78`, giving `0x1780`.

That makes three independently-derived offsets that tile the struct without
overlapping: the vertex fetch constants end at `0x780`, the two 256-register
constant files occupy `0x780..0x1780` and `0x1780..0x2780`, and the vertex
declaration sits at `0x2ED8`. Three separate readings agreeing on a layout is
what makes this a struct map rather than three lucky offsets.

**Not hooked, deliberately** — the fourth time reading the field has beaten
hooking the setter. The device holds the live value whichever path wrote it,
including the state-block path in `blocks.obj` that bypasses every hook we have.

The first draw's file, dumped verbatim:

```
c0..c3 = identity
c4 = 1.35799 0       0        0
c5 = 0       2.41421 0        0
c6 = 0       0       1.00013 -2.00027
c7 = 0       0       1        0
```

`2.41421` is `cot(22.5°)`, `1.35799` is that over 16:9, and row 3 being
`(0,0,1,0)` makes `w = z`. That is `D3DXMatrixPerspectiveFovLH` for a 45°
vertical FOV at `zn = 2`, stored transposed — exactly what `mul(mvp, float4(pos,
1))` wants. Later draws carry full view-projection matrices at `c12..c15`, whose
w-row `(0, -0.5823, 0.8130, 12.3056)` has a unit direction vector in it.

#### There is no single MVP register, and the viewport inverse explains nothing

Over 10,316 built draws, each scored against all 62 four-register windows in
`c0..c63`, both layouts, plus two controls:

| candidate | won | explains |
|---|---|---|
| identity (control) | 0 | **0** |
| viewport inverse (control, what PM4 renders with today) | 0 | **0** |
| `c1` row-major | 2,080 | 2,080 |
| `c15` row-major | 842 | 2,762 |
| `c3` col-major | 560 | 1,120 |

**Only 55% of draws are explained by any candidate at all.** But cut by bound
vertex shader, the winner is usually unique and total — `vs 0x2146FAA0`: 600/600
on `c15` row-major, one distinct winner; `0x22D3E460`: 400/400 on `c1`; and so
on for most handles. **The register is a property of the shader**, which is why
no run-wide register can win and why a scored answer would be fitting rather
than reading.

So Stage 4 does not land. Selecting a matrix by score is exactly the
"plausible-looking wrong geometry" failure this effort keeps avoiding; the
transform has to be read out of the bound shader's microcode, the way the
declaration and the fetch constants were. What *is* settled is that PM4's
premise — positions already in window coordinates, corrected by the viewport
inverse — holds for **zero** of the HLE-decoded draws.

#### Two ways the probe lied before it told the truth

Worth keeping, because both look like data and are not:

1. **The report mutated its own counters.** It zeroed each winner as it printed,
   assuming it ran once; it runs every 2,500 draws, so each report ate the
   previous one's leaders and the top candidate appeared to change every few
   seconds. The instability was the instrument.
2. **A matrix that collapses everything to a point is inside the clip volume
   under any input.** The first per-draw run had one candidate explaining all
   10,266 draws while both controls explained none — a rank-1 matrix, obvious in
   its numbers and invisible in its score. This is the `(0,0,0)` degenerate-input
   trap moved from the input to the transform. Fixed by requiring that distinct
   inputs stay distinct after projection; the explained rate fell 99% → 55%,
   which is the honest number.

### Running the guest's vertex shader from the D3D9 side (2026-08-04)

The register above cannot be picked, so the transform is read by executing the
shader that computes it. This records where the microcode lives, what it costs
to run, and what running it actually produced — which is **not** a validation.

#### The microcode is at `SH_pPhysical + 0x40`, and only at draw time

`Promote(D3DVertexShader*)` is a bare `blr`, so the handle **is** the
`CVertexShader`. `SH_pPhysical = *(handle + 0x20)`, and the microcode begins
`0x40` bytes into it (17 of 19 located shaders; `0x80` for 2). Ring keys end in
`040`, agreeing.

**It is filled lazily.** All 33 handles read as zeros at `SetVertexShader` and
non-zero at draw time. A probe that reads at bind finds nothing and concludes
the wrong thing — which is what the first three attempts did.

Read at the **unmasked** (virtual) address. D3D9 masks with `0x1FFFFFFF` to make
a physical address for the GPU; `REX_RAW_ADDR` routes masked and unmasked to
different host memory, and reading the masked one is four of this effort's
access violations.

#### Three dead ends, kept because rediscovering them is the expensive part

1. **The blob at `handle + 0x368` is not the code.** It is an XDK container —
   magic `0x102A1101`, total size at `+0x36C`, then section offsets. Best
   alignment against the ring's microcode averaged 23% agreement, and the
   byteswapped control scored identically, which is what "no signal" looks like.
   Blobs are 45–128 dwords against shaders of 27–30.
2. **The handle does not name its ring key.** Raw, masked and page-aligned all
   scored 0 hits against 41 address-shaped keys. A repeating `+0x1040`
   nearest-neighbour delta on 25 of 48 handles looked like a mapping and was
   allocator page spacing: 33 handles landed on a ring key and **0** read back
   that microcode. Arithmetic proposed it; content killed it.
3. **There is no draw-count correspondence between D3D9 and the ring.** 0 of 660
   frames equal. Ring `0x22`=146,546, `0x36`=24,223, `0x35`=12, `0x34`=0 against
   150,558 D3D9 draws (56,424 indexed). No subset matches, so "the Nth ring draw
   is the Nth D3D9 draw" is not available as a bridge.

#### The `+0x40` copy is the unpatched template

`DecodeVertexShaderFetches` decodes 19 of 19 located shaders, 0 refusals. Its
attribute list agrees with PM4's on **count, `dest_reg` and `fetch_slot`**, and
has `format`, `offset` and `stride` blank. That is the template before
`PatchVertexShaderToMatchVertexDeclaration` rewrites the vfetch dwords — so the
probe takes attributes from PM4's decode, and the declaration-to-vfetch pairing
rule remains unread. Agreement against the ring is bimodal (0-19%: 3, 40-59%: 4,
80-99%: 27), so the misses are shaders the ring had not loaded, not disputes.

#### Constants: `device + 0x780`, no rebase

`AluInputs::Const(i)` reads `alu_consts[i*4]` and D3D9 register N sits at
`+0x780 + N*16`, so the indexing already matches and the caller's `SQ_VS_CONST`
base does not apply. 86% of 1.9M constant reads found non-zero data, which is
the check that matters: a shader computing confidently from an empty file is the
failure that looks like success.

#### It executes cleanly, and that is the whole of the good news

87,169 vertices: **status `kOk` for every one, zero blocking opcodes, zero
degenerate exports.** The interpreter runs this title's shaders end to end.

#### Cost — per draw, not per vertex

165s runs, `hle_capture` + `hle_render`, late-run windows, 0 access violations
in all four:

| `hle_shader_exec` | verts | ms/frame | interpreter ms/frame | share |
|---|---|---|---|---|
| 0 (baseline) | — | 2872–3006 | 0 | 0% |
| 64 | 8 | 2879–3041 | 13–19 | ~0.5% |
| 1 | 8 | 3713–3908 | 844–1033 | 22–26% |
| 1 | 64 | 3981–4087 | 943–1134 | 24–28% |

**7.4× the vertices cost 1.1× the time.** Marginal cost is ~26 µs/vertex against
~13 ms fixed per executed draw, so ~90% of the bill is per-draw overhead. The
only O(1024) per-draw work is the constant-file copy; naming the culprit exactly
was deliberately left alone rather than optimised on a guess.

**The 22–28% share badly understates the problem.** These frames are 3–4
*seconds* long in a debug build. Against a real 16.7 ms frame, ~1,000 ms/frame
of interpreter is roughly 60× the entire budget. Per-vertex interpretation is
not shippable on this path; the number says so plainly.

Frame time is reported **windowed, not run-wide**. The first version reported a
cumulative mean that climbed 219 → 808 ms/frame as the run went on, because this
title genuinely degrades — so a run-wide mean mostly measures how long the run
had been going, and comparing two configs on it compares their durations.

#### Where the positions land — and why this does not validate anything

Buckets are `max(|x/w|, |y/w|)`; the `<=1` row is **not** stageC's in-clip count,
which also bounds z. 87,169 vertices, same vertices through both transforms:

| bucket | executed shader | viewport inverse (control) |
|---|---|---|
| `<=1` | 26,964 (31%) | 33,224 (38%) |
| `1-2` | 12,327 (14%) | 51,691 (59%) |
| `2-10` | 32,744 (38%) | 1,932 (2%) |
| `10-100` | 2,576 (3%) | 322 (0.4%) |
| `>100` | 3,036 (3.5%) | 0 |
| `w<=0` | 9,522 (11%) | 0 |

**Read this against the hope, not with it.** Executing the shader puts *fewer*
vertices on screen than the viewport inverse does, with a long tail the control
does not have: 38% at 2–10×, 3.5% past 100×, 11% behind the eye.

But the control is not trustworthy either, and the instrument said so at the
time: **950 of its draws collapsed every vertex to a single point** (the
`kSpreadEpsilon` guard, carried over from the register scoring).

> **CORRECTED — the control column above is invalid.** The next section
> establishes that the viewport shadow reads `65535x65535` for most of the run,
> so the "viewport inverse" this control applied was built from a nonsense
> extent and divided every position by 32767. That is *why* 950 draws collapsed
> to a point. **Do not read the right-hand column as a reference for anything**;
> the executed-shader column stands, and is re-reported correctly in the next
> section.

So: neither distribution establishes a correct transform. What is established is
that the microcode is reachable, decodable and runnable from a D3D9 handle.
6,078 of 9,101 entered draws had no located shader at all, so this is a
measurement over a minority of the population.

*(Both bridges this section originally listed are resolved in the next section;
`fetch_slot` inverts to a D3D9 stream, and PM4's attribute decode turns out to
be the same data by a different route rather than a substitute for it.)*

**Stage 4 stays blocked.** `hle_render` unchanged, `hle_shader_exec` defaults to
0, `DrawCall::mvp` keeps the viewport inverse.

### The declaration-to-vfetch pairing rule (2026-08-04)

Read out of `D3D::PatchVertexShaderToMatchVertexDeclaration` (`0x82564C50`),
which was the last unread step between a D3D9 declaration and the shader inputs
it feeds. Arguments:

```
r3 CVertexShader*   r4 destination microcode   r5 CVertexDeclaration*
r6 const BYTE* strides (per stream, in DWORDS)  r7 variant index
```

#### The shader carries a binding table

```
blob  = this + *(this + (variant + 0x70) * 8) + 0x368     ; GetUCode(variant)
count = blob[0x1C]
table = blob + 4 * (blob[0x18] + 9)
```

One dword per vfetch:

| bits | meaning |
|---|---|
| `[11:0]` | vfetch instruction index — patched triple goes to `dest + 12 * index` |
| `[15:12]` | `D3DDECLUSAGE` |
| `[19:16]` | usage index |

**The pairing is by semantic, not by position.** For each vfetch, a linear
search over the declaration for the element whose `usage` (byte 9) and
`usage_index` (byte 10) equal the key's; first match wins. That is why the
template's format/offset/stride read blank — they are unbound, not defaulted.

From the matched element:

| vfetch field | value | instruction |
|---|---|---|
| fetch constant `[26:20]` | `95 - element.stream` | `subfic r20, r5, 0x5F` |
| format `[21:16]`, signed `[12]`, integer `[13]` | from the Type dword | as `kType*` already record |
| offset `[30:8]` | `element.offset / 4` | `rlwinm r8, r8, 6, 1, 23` |
| stride `[7:0]` | `strides[element.stream]` | `lbzx r5, r5, r6` |

No match writes fetch constant 95 as well, plus a canned format (`0x60000`) and
swizzle (`0x9250`). **So a decoded `fetch_slot` of 95 is ambiguous between
"stream 0" and "unbound"** — the canned bits are what tell them apart.

#### Verified by prediction, not by reading

The probe predicts all three dwords *before* the call and compares against what
D3D9 wrote *after* it. Per field, over 53 slots in 41 patch calls:

| field | agree |
|---|---|
| stride `strides[stream]` | **53/53 (100%)** |
| fetch constant `95 - stream` | 52/53 (98%) |
| format | 49/53 (92%) |
| signed/integer | 49/53 (92%) |
| offset `elem.offset/4` | 49/53 (92%) |
| swizzle `[11:0]` — **unmodelled** | 40/53 (75%) |
| coalesce count `[29:27]` — **unmodelled** | 45/53 (84%) |

**53 of 53 slots bound, 0 unbound, 0 calls rejected for a bad table.** The
structure is right or nothing would have matched at all.

Two things are deliberately unmodelled, and they are where the whole-dword
agreement (66–92%) is lost: a **second pass** that coalesces adjacent fetches
(it writes `[29:27]` and *reorders* triples, which also explains the residual
4/53 on the fields above), and the swizzle chain through `word_8204E178` that
rewrites `[11:0]` to match the format's component count — visible as `0x688`
(xyzw) becoming `0xA88` (xyz1).

#### The patched microcode goes into the command ring, not the shader object

`r4` pointed at `0xBED1B570` on 38 of 39 calls — the ring, where packets parse
from. Only 1 wrote to `SH_pPhysical + 0x40`.

This confirms Stage B rather than overturning it: the `+0x40` copy really is the
template, and **PM4's attribute decode was never a substitute for the
declaration — it is the patched result, read back off the ring.** The two routes
agree because they are the same data.

#### What changed in the probe

`fetch_slot` now inverts to a stream (`95 - fetch_slot`) per attribute, instead
of every attribute being read from stream 0. Attributes from different streams
are fetched from their own buffers.

**Draws skipped for "bound stride disagrees with the shader's" went from 598-742
to 0**, with 0 failing to invert — which is the strongest evidence the mapping
holds, because it is a check the code did not have to pass.

The geometry did not change: 15,340 vertices, 36% in the clip volume, histogram
within noise of the previous run. **Removing the bridges did not move the
result**, so whatever is wrong with the transform is not the stream mapping and
not the attribute source.

### The exports are clip space, and the viewport shadow is wrong (2026-08-04)

Two findings, one of which invalidates a control used in the section above.

#### The HLE-executed position is clip space, not window coordinates

The suspicion was that the clip-volume test had been the wrong yardstick all
along: the PM4 path established, on the ring's own executions, that this title's
shaders export **window coordinates** — `(640, 0, 1, w=1)`, `(1280, 0, 0, w=1)`
— which is the whole reason `BuildViewportMvp` and the viewport inverse exist.

It is not what the D3D9 path produces. The first eight exports, verbatim:

```
pos = 9.93374 12.2319 9.31915 w=29.9977
pos = 7.77483 5.78460 8.52087 w=25.8431
pos = 7.92958 6.69047 8.85351 w=26.4269
pos = 13.3802 21.4615 10.6591 w=35.9452
```

Divided through, the first is `(0.331, 0.408, 0.311)` — inside the unit cube,
with `w ≈ 30` reading as a point thirty units from the camera. That is textbook
perspective output, and it matches the `c4..c7` projection matrix the constant
file dump showed back in Stage 3.

**So the clip test was the right yardstick and 36% is a real number.** The
`2-10` bucket is genuinely off-screen geometry, not a units error.

> **CORRECTED — this verdict was an average over three unrelated populations.**
> Stage I broke the same measurement down per shader. 41% of all executions come
> from a *single one-attribute shader* (`0x215F4B60`) that is 100% clip-like and
> 100% in-clip — a fullscreen quad, which cannot be anything else. Every
> multi-attribute shader drawing world geometry at 1280x720 reads **0% in-clip
> and mostly window-like**, which is what the PM4 path said all along. The eight
> raw exports quoted above are real, but they are not representative: they were
> whichever draw the probe reached first. See "The space verdict was one shader"
> below.

Worth stating plainly: **the two paths disagree about the space, running the
same shaders through the same interpreter.** The difference is the constants —
PM4 feeds `m_aluConsts` from `LOAD_ALU_CONSTANT`, the D3D9 path reads
`device + 0x780`. The D3D9 constant file produces a projection; the ring's copy
produces pre-transformed window coordinates. Which is *correct* is not settled
here, but the D3D9 side is the one whose output looks like a projection matrix
was involved.

The `window-like` classification (23%) is **not a finding** — see below, it was
computed against a 65535-wide viewport and would accept nearly any position.

#### `SetViewport` is called with eight different extents, mostly 65535x65535

Every distinct extent over a 165 s run:

| extent | calls |
|---|---|
| `65535x65535` | **9,130** |
| `1280x720` | 5,658 |
| `1280x640`, `1280x80`, `640x720`, `640x360`, `128x32`, `64x64` | ~153 each |
| `256x256` | 6 |

The struct read is correct — `D3DDevice_SetViewport` (`0x8254BF50`) takes six
dwords, X/Y/Width/Height as integers then MinZ/MaxZ as floats, and the
decompiler names them. **The bug is the model, not the read**: the shadow is
last-write-wins over a title that uses eight viewports, and the most frequent
one is a full-surface reset.

Consequences, all real:

1. **`BuildViewportMvp` produces a nonsense matrix most of the time** — `xs =
   32767.5`, so every position is divided by 32767 and collapses toward the
   origin.
2. **The Stage D2 "viewport inverse" control was built from it**, which is
   exactly why 950 of its draws tripped the collapse guard. That column is
   corrected above and must not be used as a reference.
3. **`DrawCall::mvp` for HLE-rendered draws is built from it too**
   (`BuildAndQueueDraw` sets `in.mvp = vp`), so `hle_render` has been drawing
   with a broken transform whenever the last viewport set was the full-surface
   one.

The fix is not a different offset. It is that a per-draw viewport has to be
captured per draw — the same staleness class as the declaration lag already
documented, and it wants the same treatment.

### Executing the shader that was actually bound (2026-08-04)

Draws were matched to microcode by ≥90% content similarity against PM4's cache.
That heuristic could pick a near-identical wrong variant, and it failed outright
on ~63% of draws — so every number before this came from a 37% minority.

The patch hook has the real thing: `r4` is where D3D9 writes the patched
microcode and `r3` names the shader. An exact key, no similarity involved.

| shader source | before | after |
|---|---|---|
| patch hook (exact) | — | **5,271 (99%)** |
| content match ≥90% | 2,912 (37%) | 0 |
| none | 4,886 (62%) | 28 (0.5%) |

Decode of the captured code: **5,271 matched the binding table's fetch count, 0
decoded a different count, 0 refused.** Vertices executed nearly doubled,
15,548 → 29,331.

#### `dest` is the CF section start, and the first guess was wrong

The capture needs the CF section, because `DecodeVertexShaderFetches` expects an
array that starts there. The first attempt assumed the 0x40-byte gap Stage A
found inside `SH_pPhysical`. **Every decode refused with "exec target at address
0"** — the self-check earning its place, because a wrong start would otherwise
have decoded into plausible nonsense.

So the start was *searched* instead, with a checkable answer: the binding table
states how many vfetches the shader has, and only the true CF start decodes to
exactly that many. The result is `+0` on 43 of 47 shaders — **`dest` is the CF
start**, and vfetch indices are relative to it. (`-3` ×3 and `-85` ×1 are search
false positives; preferring an already-established offset keeps them from
winning once the layout is known.)

#### A cached answer that is never re-checked is an assumption

Worth keeping, because it produced a report that looked fine and was not. The
first version cached the resolved offset per shader and reused it blind on later
captures. The draw-time decode then failed on ~7,770 captures while the source
tally still counted those shaders as resolved. Fixed by verifying the cached
offset decodes before reusing it, and re-searching when it does not — one decode
in the common case.

#### Attributes: 12% disagree, and that is not yet explained

Our decode of D3D9's patched output against PM4's decode of the ring's copy —
the same bytes by two routes:

```
1,547 agree   204 disagree (12%)   3,520 had no PM4 peer to compare against
```

The disagreement is real and unexplained. It is small enough not to dominate and
large enough not to dismiss; the likely candidate is the second pass that
reorders fetch triples, which is still unmodelled (see the pairing section).

#### The in-clip fraction did not move

**36%, on 29,331 vertices instead of 15,548.** Nearly doubling the sample and
replacing a heuristic with exact code changed nothing. That is worth as much as
a fix would have been: 36% is a property of this measurement, not an artefact of
the 37% minority it used to be computed over. Whatever is wrong with the
transform is not the shader identification either.

### The viewport comes off the device, clamped (2026-08-04)

`D3DDevice_SetViewport` (`0x8254BF50`) forwards to `sub_8254BCE8`, which stores
six floats on the device — and **clamps Width and Height against the render
target first**, bounding `X + Width` and `Y + Height` by the surface extent it
reads from `0x24(r9)`:

```asm
stfs f31, 0x3218(r31)   ; X
stfs f30, 0x321C(r31)   ; Y
stfs f26, 0x3220(r31)   ; Width    (clamped)
stfs f27, 0x3224(r31)   ; Height   (clamped)
stfs f29, 0x3228(r31)   ; MinZ
stfs f28, 0x322C(r31)   ; MaxZ
```

That clamp is the whole bug. The argument shadow recorded `65535x65535` on 3,628
of ~6,300 calls — a full-surface reset — and last-write-wins meant most draws
inherited it. **D3D9 never uses that value**; it clamps to the real surface.

**Sixth time reading the device field has beaten shadowing the call.** Same
reason each time: the device holds the resolved value, whatever path produced it
and whatever the caller passed.

#### What it measured

```
viewport source — device +0x3218 4036, argument shadow fallback 0
device disagreed with the shadow's extent on 2897 of them
```

**72% of draws were using a wrong viewport.** The device copy was readable every
time, so the fallback never fired.

The decisive before/after is the control's collapse counter, because it is the
same code path with the same inputs and only the viewport changed:

| | before | after |
|---|---|---|
| control draws collapsed to a point | 1,410 | **6** |

An `xs` of 32767.5 divided every position toward the origin, which is exactly
what the `kSpreadEpsilon` guard was catching. With the real extent the control
stops degenerating, so the Stage D2 comparison has a reference again.

Also visible directly in the dump — the same draw, both sources:

```
pos = -1 1 0 w=1    device viewport 0,0 160x90    arg shadow 65535x65535
pos =  1 1 0 w=1    device viewport 0,0 160x90    arg shadow 65535x65535
pos = -1 -1 0 w=1   device viewport 0,0 160x90    arg shadow 65535x65535
```

Those three exports are the corners of a full-screen quad in clip space, on a
160x90 render target.

#### What this does *not* explain

The in-clip fraction read 58% in this run against 36% before, and **that is not
attributable to this fix**. The executed position comes from the shader and the
constant file; it does not touch the viewport at all, so no mechanism connects
them. The two runs sampled different draws — the earlier one was dominated by
world geometry, this one by small render-target passes. Reported here so the
next reader does not mistake the coincidence for a result.

### The space verdict was one shader (2026-08-04)

The fraction of HLE-executed positions landing inside the clip volume sat at 36%
across four independent improvements — the stream mapping, the attribute source,
the sample size, and exact shader identification — then read 58% in the run after
the viewport fix, which cannot have caused it. Five changes, no verdict.

The reason none of that could be judged is not a bug:

> **Nobody knew what the in-clip fraction was supposed to be.** Real scenes cull,
> render shadow maps and run off-screen passes, so 100% is wrong and 36% might be
> right. Every number collected compared the HLE path against itself.

So Stage I stopped asking how big the failure is and asked where it is: the same
counters, attributed to the shader handle that produced them
(`ShaderScore` / `g_shaderScore`, `hooks_d3d9.cpp`). 300 s run, `mx_255.log`, 22
shaders, zero access violations, per-shader sums equal to the globals they were
taken beside, and zero handle-identity changes.

| shader | attrs | execs | rt | clip-like | window-like | neither | in-clip |
|---|---|---|---|---|---|---|---|
| `0x215F4B60` | **1** | 2,922 | 1280x720 | **100%** | 0% | 0% | **100%** |
| `0x216C9620` | 3 | 1,912 | **129x129** | 30% | 3% | 66% | 27% |
| `0x2160E720` | 3 | 824 | 768x384 | 77% | 0% | 22% | 74% |
| `0x22D3DC20` | 5 | 232 | 1280x720 | 5% | 69% | 25% | **0%** |
| `0x22D46320` | 6 | 136 | 1280x720 | 0% | 35% | 64% | **0%** |
| `0x22D4AAE0` | 6 | 136 | 1280x720 | 22% | 62% | 15% | **0%** |
| `0x22D65B60` | 4 | 136 | 1280x720 | 0% | **100%** | 0% | **0%** |
| `0x22D41BE0` | 6 | 88 | 1280x720 | 0% | 54% | 45% | **0%** |
| `0x22D53760` | 6 | 88 | 1280x720 | 11% | 71% | 17% | **0%** |
| `0x22D5C3E0` | 7 | 88 | 1280x720 | 0% | 28% | 71% | **0%** |

The aggregate — "clip-like 59%" — is an average over three populations that have
nothing to do with each other:

1. **`0x215F4B60`, one attribute, 41% of every execution in the run.** A shader
   with a single vertex attribute is a fullscreen quad or a blit. It exports
   clip space because its positions *are* clip space, hardcoded. It cannot be
   wrong, and it carried essentially the whole 59%.
2. **`0x216C9620` at 129x129, another 27%.** A shadow or cube-map pass with its
   own projection. Two shaders are 68% of the entire measurement.
3. **Every multi-attribute shader drawing world geometry at 1280x720 is 0%
   in-clip**, without exception, and reads window-like far more often than
   clip-like.

**That third row is the finding.** The D3D9 path and the PM4 path were never
actually in disagreement about the space — PM4 said window coordinates for the
world geometry, and so does this, once the quad shader stops outvoting it. The
"exports are clip space" section above was reading a fullscreen quad.

It also explains why four improvements moved nothing: they were all correct, and
they all improved a number whose value was set by a shader that was already
right.

#### What this changes

- **The global in-clip percentage is retired as a headline.** It averages a
  post-process quad, a shadow pass and world geometry into one figure that
  describes none of them. Read the per-shader table.
- **Shader class predicts the outcome.** Attribute count and render-target
  extent are on every row for exactly this reason: 1 attr at 1280x720 is a
  quad, 3 attrs at 129x129 is a shadow map, 4–7 attrs at 1280x720 is the world.
- **The unexplained 12% attribute disagreement is not evenly spread either** —
  the `ATTR DISAGREE` dumps show HLE reading `stride=0 fmt=0` where PM4 has real
  values, which is a decode returning nothing rather than something different.

#### The seed for hand verification

Every shader records its first execution in full so the worst one can be checked
by hand without a second run. `0x216C9620`, 1,277 of 1,912 executions in no
modelled space:

```
attr0  r1 (position) stream 0  fetch 95  fmt 0x20  off 0   stride 28
       raw BA 0D E5 D8 00 3C 53 49  ->  0.000349522 -156.625 1 10.6484
attr1  r0            stream 0  fetch 95  fmt 0x1F  off 12  stride 28
       raw 5B C2 10 2C 00 00 00 00  ->  -3.17773 0.0634766 0 1
attr2  r0            stream 0  fetch 95  fmt 0x06  off 20  stride 28
       raw FF FF FF 00              ->  1 1 1 0
position = -16.1811 11.9436 0.497487 w=1
```

A position input of `(0.00035, -156.6, 1, 10.65)` is not a plausible object-space
vertex, and `w=1` out of a shader whose input `w` is 10.65 is worth explaining.
Whether the fault is the `0x20` format decode or the shader is **not concluded
here** — it is written down so the next step starts from a specific vertex with
specific bytes rather than from a percentage.

### The position input is (x, y, z, 1) read as (x, y, 1, z) (2026-08-04)

Chasing the Stage I seed found a located defect, and not the one predicted.

#### The format decode is correct — that hypothesis is dead

`0x216C9620`'s position attribute is `fmt 0x20` = 32 = `k_16_16_16_16_FLOAT`.
Decoding its eight bytes by hand against the IEEE half format reproduces what the
probe printed, digit for digit:

```
raw BA 0D E5 D8 00 3C 53 49
little-endian halves:  0.000349522  -156.625  1  10.6484
probe printed       :  0.000349522  -156.625  1  10.6484
```

The other two attributes (`0x1F` = `k_16_16_FLOAT`, `0x06` = `k_8_8_8_8`) check
out the same way. **`ReadVertexAttribute` is right, the format table is right,
and the byte order within each component is right.** The defect is one level up:
what the components *mean*.

Also worth recording: ranking "worst" by raw `neither` count picked the 129x129
shadow pass, which is the **least** diagnostic shader in the table — a cascade
covers a slice of the world and the whole scene is drawn against it, so geometry
outside its frustum is what that pass is supposed to produce. The shaders worth
reading are the 1280x720 ones at 0% in-clip.

#### The third component never moves

Dumping the position attribute's per-component range over every execution, for
the six busiest shaders:

| shader | rt | `.x` | `.y` | `.z` | `.w` |
|---|---|---|---|---|---|
| `0x216C9620` | 129x129 | -0.005 .. 0.083 | -668.5 .. 183.6 | **1 .. 1** | -646 .. 570.5 |
| `0x2160E720` | 768x384 | 0.002 .. 5.570 | -4.605 .. 1.788 | **1 .. 1** | -0.705 .. 3.768 |
| `0x22D3E460` | 1280x720 | 2.395 .. 4.797 | -4.605 .. 0.284 | **1 .. 1** | -0.705 .. 3.768 |
| `0x22D46B60` | 1280x720 | 3.059 .. 4.789 | -2.211 .. 0.178 | **1 .. 1** | -0.002 .. 0.226 |
| `0x22D4B320` | 1280x720 | 0.103 .. 5.445 | -0.262 .. 0.443 | **1 .. 1** | -0.401 .. 0.288 |

3,032 executions across five unrelated shaders and `.z` is bit-identical `1.0`
every time (bytes 4-5 read `00 3C`, half `0x3C00`), while `.w` swings across six
hundred units. **A homogeneous position has a constant `w`, not a constant `z`.**
The components are being delivered in the wrong order.

Swapping the two halves within each dword puts the constant where it belongs:

| shader | as read now | halves swapped within each dword |
|---|---|---|
| `0x216C9620` | (0.00035, -156.6, **1**, 10.65) | (-156.6, 0.00035, 10.65, **w=1**) |
| `0x2160E720` | (4.559, -2.373, **1**, -0.057) | (-2.373, 4.559, -0.057, **w=1**) |
| `0x22D3E460` | (4.496, -2.076, **1**, -0.705) | (-2.076, 4.496, -0.705, **w=1**) |
| `0x22D46B60` | (4.563, 0.178, **1**, 0.226) | (0.178, 4.563, 0.226, **w=1**) |
| `0x22D4B320` | (0.263, 0.185, **1**, 0.201) | (0.185, 0.263, 0.201, **w=1**) |

Every one lands on `w = 1.0` exactly. Five shaders do not agree on that by
accident.

#### The mechanism, and the part that is not yet settled

`ApplyFetchEndian` (`shader_ucode.cpp:162`) implements `endian == 2` as a literal
8-in-32 swap:

```cpp
} else if (endian == 2) {
  for (size_t i = 0; i + 3 < bytes; i += 4) {
    std::swap(data[i], data[i + 3]);
    std::swap(data[i + 1], data[i + 2]);
  }
}
```

Reversing all four bytes of a dword that holds **two 16-bit components**
byte-swaps each half *and* reverses their order. For big-endian halves `[A][B]`
the correct result is `A, B` (an 8-in-16 swap); 8-in-32 yields `B, A`. That is
exactly the permutation the data shows, and it would affect every 16-bit format —
`k_16_16` (25), `k_16_16_16_16` (26), `k_16_16_FLOAT` (31), `k_16_16_16_16_FLOAT`
(32).

**Not concluded: whether that is the whole cause.** `VertexAttribute::dest_swizzle`
is decoded (`shader_ucode.cpp:553`) and then **never applied anywhere** — not in
`ReadVertexAttribute`, not in the interpreter. An unapplied vfetch swizzle is a
second mechanism that could reorder components, and it has not been ruled out.
The endian explanation matches the observed permutation mechanically and exactly;
the swizzle one is simply untested.

#### Blast radius — why this is not a drive-by fix

`ApplyFetchEndian` is not probe-only. `CopyVertex` (`d3d9_draw.cpp:56`) uses it to
build the vertices that are actually **rendered**, so changing it changes what
appears on screen, not just what the probe reports. It needs its own step, with
the 23 declaration fixtures and `ucode_test` run against it, and the swizzle
question settled first.

### Fixed: the endian swap width is the format's, not the mode's (2026-08-04)

The defect located in the section above, fixed and measured.

#### The swizzle question, settled first

Two mechanisms could have produced the observed component rotation: the 8in32
swap over 16-bit components, or `dest_swizzle` being decoded and never applied.
Both predict the same corrected output, so the reordering alone could not choose
between them. Undoing the swap to recover what is actually in guest memory does:

```
guest bytes 31 ED 34 37 32 72 3C 00  ->  (0.185181, 0.263428, 0.201416, 1)
```

Memory order is already `(x, y, z, w=1)`. Nothing needs reordering, so no
swizzle is involved — **the endian width is the whole defect.** (`dest_swizzle`
is still decoded and unapplied on the vfetch path; it is simply not this bug.
`ReadHleElement` does apply the declaration's swizzle, and always did.)

#### The fix

The swap unit must be the format's own packed unit, not the mode's nominal
width — `VertexFormatUnitBytes` returns 2 for formats 25/26/31/32 and 4 for the
rest, and `ApplyFetchEndianFor` narrows the mode to it.

That width depends on the format, so **the swap cannot be applied once over a
whole vertex** — a vertex mixes 16-bit positions with 32-bit colours. Both
callers stopped pre-swapping and now pass the mode down:

| was | now |
|---|---|
| `CopyVertex` swapped the vertex, `ReadHleElement(…, out)` | `CopyVertex` copies guest bytes, `ReadHleElement(…, s.endian, out)` |
| PM4 swapped `dc.vertices` in one pass | `dc.vertex_endian` carries the mode; `ReadVertexAttribute(…, dc.vertex_endian, …)` |

PM4 had the identical bug on the path that actually renders, so it is fixed
there too rather than left in the live path while the disabled one is corrected.

#### Verified

`tools/d3d9_layout_test.cpp` gains the real failing vertex as a fixture — shader
`0x22D4B320` vertex 0, guest bytes, `endian=2` — asserting the homogeneous
result and, separately, that `w` did not land in `z`. **The test was confirmed to
fail without the fix** (`got 0.263428, want 0.185181`), because a regression test
that passes either way is not one. A second fixture asserts 8in32 over
`k_8_8_8_8` still reverses the whole dword, so the narrowing cannot be applied
to every format. All 23 declaration fixtures and `ucode_test` pass.

#### Measured — 300 s run, `mx_258.log`, zero access violations

The input is now a homogeneous position on **every** shader, over thousands of
executions:

```
.z  [-646 .. 570.5]        (was CONSTANT 1)
.w  [1 .. 1]   CONSTANT    (was the varying one)
```

And the output classification, per shader:

| shader | rt | neither before | neither after | in-clip before | in-clip after |
|---|---|---|---|---|---|
| `0x2160E720` | 768x384 | 22% | **0%** | 74% | **100%** |
| `0x22D3E460` | 1280x720 | 25% | **0%** | 0% | **69%** |
| `0x22D46B60` | 1280x720 | 64% | **0%** | 0% | 0% (100% window-like) |
| `0x22D4B320` | 1280x720 | 15% | **0%** | 0% | 0% (100% window-like) |
| `0x22D5CC20` | 1280x720 | 71% | **0%** | 0% | 0% |
| `0x216D3920` | 129x129 | 43% | **4%** | 17% | **45%** |

Globally: `neither` 2,038 → **1,208**; shaders that are ≥90% `neither` **4 → 0**;
shaders ≥90% clip-like 2 → **5** (55% of executions). No shader is stuck in an
unmodelled space any more.

**Every world shader now classifies cleanly as clip-like or window-like, and the
1280x720 ones are overwhelmingly window-like** — which is what the PM4 path
concluded from the ring, and what the aggregate hid.

#### What is left

All remaining `neither` is essentially **one shader**: `0x216C9620`, the 129x129
shadow pass, still 62% (1,912 executions, ~1,185 of the global 1,208). Its input
is a clean homogeneous position, so this is no longer a decode question. A
cascade covers a slice of the world and the whole scene is drawn against it, so
some of that is expected — how much is not established.

The in-clip figure moved 58% → 62% globally, but that number is still an average
over a post-process quad, a shadow pass and world geometry. **Read the per-shader
table, not the total.**
