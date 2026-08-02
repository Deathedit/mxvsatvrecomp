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
