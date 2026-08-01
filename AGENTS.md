# MX vs. ATV Alive — ReXGlue D3D12 Rendering Backend

## Build

```
cmake --preset win-amd64-debug
cmake --build out/build/win-amd64-debug --target mx
```

**Regenerate after config changes:**
```
rexglue codegen --force mx_manifest.toml
cmake --preset win-amd64-debug
cmake --build out/build/win-amd64-debug --target mx
```

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
| `src/app/mx_app.h/.cpp` | `MxApp`: OnPreSetup installs the graphics system (or sets plugin mode), OnPostSetup hands it the HWND |
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
| `src/hooks/hooks_plugin_diag.cpp` | Plugin-mode-only diagnostics: RendererDispatch, LazyInit, Timing, LoadStateMachine, VtableCtor |
| **build** | |
| `mx_config.toml` | Function sizes + `[[midasm_hook]]` definitions |
| `CMakeLists.txt` | Source list by layer, `src/` include dir, linker exports for mid-ASM hooks |

Input is handled entirely by ReXGlue's built-in `SDLInputDriver`
(`XamInputGetState` / `XamInputGetCapabilities` at kernel level) — there is no
`src/native_input.*` and no manual `REX_FUNC` input hook.

### Host Pipeline
```
MxApp::OnPreSetup  -> D3D12GraphicsSystem, clears gpu_plugin
MxApp::OnPostSetup -> HWND, InitializeRenderer (D3D12 + SDL gamepad)
EngineInit hook    -> SetGuestMemory(base)
RenderThread       -> Bink playlist -> UploadVideoFrame -> BeginFrame -> RenderVideoFrame -> EndFrame
                   -> After Bink: BeginFrame -> RenderGameFrame (triangle) -> EndFrame
SDL Audio Thread   -> Bink ring buffer -> SDL audio
```

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
| +8 | NULL (Bootstrap) → populated by vt[17] → overridden to `eng` self-ref by MainLoop hook | AssetDB_block assigned by SetupRenderer vt[17] (`sub_82B43AC8`), then overwritten to `eng` at MainLoop #1 to keep MainLoop's vt[36] call landing on Bootstrap's `nullsub_1` (AssetDB's vt[36] = ASCII "SP_EVENT" string data → would crash). |
| +12 | 0x40B78860 | Engine subsystem (SceneManager) — runtime vtable 0x82141754, vt[0]=0x82B671D0 |
| +16 | 0x830EC248 | Transition renderer (545KB, vtable 0x8214518C) |
| +20 | 0x40BB8ED0 | Graphics device handle |
| +28-+36 | ptrs | Render pass entity lists |

**IMPORTANT — eng+12 is NOT the game-tick dispatcher.** Runtime dump at MainLoop frame 1 verified via REX_LOG:
```
eng+12 = 0x40B78860, vtable 0x82141754
vt[0]  = 0x82B671D0   (valid function)
vt[36] = 0x4C6F6164   <- ASCII string "Load", NOT a function pointer
```
The SceneManager vtable at eng+12 is **shorter than 36 entries** — slot 36 reads past the function table into adjacent string data ("Game", "Real_World", "Destroy RealWorld Done", "SP_EVENT", "MP_CUSTOM_EVENT", "MP_PLAYLIST_EVENT", "SS_EVENT"). Calling vt[36] would jump to `0x4C6F6164` and crash. The real game-tick code that populates entities lives elsewhere (behind LoaderTick's renderer block, skipped by mid-ASM hook #6), not in this vtable.

### GPU Memory (GpuAlloc return addresses)
| Alloc | Size | Address | Purpose |
|-------|------|---------|---------|
| #1 | 0x00F00000 (15MB) | 0xBEDA0000 | Main pool - render targets + depth |
| #2 | 0x00730000 (7.3MB) | 0xBE0C0000 | Secondary render targets |
| #3 | 0x00398000 (3.6MB) | 0xBDD20000 | Additional buffers |
| #4-15 | 0x00080000 each | 0xBDCA0000-down | Shader caches |

GPU physical base tracked by `dword_830B03EC` (starts at 0).

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
     |   +- NtCreateEvent x4 -> engine+0x190/194/2DC/2E0
     |   +- GraphicsInit (0x82AEBF40) -> GpuAlloc #1-15
     |   +- PostGfxInit (0x82AE9658) -> device vtable calls -> MID-ASM SKIPPED
     |   +- TexManager + BindTexture
     |   +- Transition thread created at 0x82B716B0
     +- while (MainLoop != 0) { }  (forced r3=1)
     +- Cleanup1 (stubbed) -> Cleanup2 (stubbed)
     +- EngineInit returns -> sleep loop
```

### MainLoop (decompiled)
```c
int MainLoop(a1) {
    v2 = 1;
    if (byte_82D57994) {
        // spin-wait: Wait(500ms), break if event signaled
        if (v7) { sub_8255D430(stubbed); RenderPipeline(hooked); }
        else v2 = 0;
    } else v2 = 0;
    // vtable[36] -> SKIPPED (eng+8 is NULL, would crash without workaround)
    return v2;
}
```

### LoaderTick (decompiled)
```c
int LoaderTick(a1) {  // a1 = Transition renderer
    Wait(*(a1+0x194), -1);
    sub_82B70370(a1);  // timing
    // vtable[2] on engine -> scene manager
    // vtable[32] on result -> update scene
    // Entity loops (SceneManager::Tick, signal events)
    result = *(a1+8)->vtable[6]();  // gating check — SKIPPED by hooks
    if (result) {
        // Renderer block (XenonRenderer) -> SKIPPED by hooks
        return 1;
    }
    return result;  // 0 = no work
}
```

### Transition Logic
```c
void Transition() {
    v0 = *(dword_830BE400 + 16);
    do { v1 = LoaderTick(v0); v2 = v1; v0->frame_count++;
         if (byte_82D57994) NtSetEvent(v0+0x2DC);
    } while (v2);
    byte_82D57994 = 0;  // Clears render flag!
}
```

### RenderPipeline Logic
```c
void RenderPipeline(a1) {
    sub_82ABF828(renderer);  // BeginFrame (stubbed)
    // 3 render passes, entity iteration, draw calls
    sub_82ABF930(renderer);  // EndFrame -> VdSwap (fires every frame in native mode)
}
```

---

## Mid-ASM Hooks (mx_config.toml)

Inject at PPC instruction addresses. Replace instruction with C++ call + goto jump_address.

| # | Address | Name | Jump To | Skips |
|---|---------|------|---------|-------|
| 1 | 0x82B70854 | NativeGameTickSkip | 0x82B70874 | MainLoop vtable[36] crash (DISABLED — commented out, eng+8 self-ref workaround handles it) |
| 2 | 0x82B71290 | NativeSetupDeviceSkip | 0x82B712A4 | SetupRenderer vtable[6] block |
| 3 | 0x82B712C4 | NativeSkipVtable8 | 0x82B712D8 | SetupRenderer vtable[8] (DISABLED — vt[8] is harmless 4-byte alloc + vtable install at eng+0x38) |
| 4 | 0x82B71304 | NativeSkipVtable17 | 0x82B71314 | SetupRenderer vtable[17] (DISABLED — let vt[17] run naturally so eng+8 = AssetDB_block is populated) |
| 5 | 0x82B71324 | NativeSkipRendererInit | 0x82B71690 | Renderer init -> Transition thread |
| 6 | 0x82B70EC8 | NativeSkipLoaderRenderer | 0x82B710BC | LoaderTick renderer block |
| 7 | 0x82B70E18 | NativeSkipLoaderEarly | 0x82B70EC8 | LoaderTick vtable+entities |
| 8 | 0x82B70DFC | NativeSkipLoaderAll | 0x82B70EC8 | LoaderTick everything |

**Hooks #2, #5, #6, #7, #8 active** (5 hooks in config). Hooks #1, #3, #4 disabled. All 8 export symbols remain in CMakeLists.txt.

**IMPORTANT**: Mid-ASM hooks are unconditional. Always fire, always jump. No conditional behavior.

---

## C++ Hook Functions

### Frame Lifecycle
| Hook | Behavior |
|------|----------|
| sub_82566B58 (VdSwap) | Counter sync: 0x82D21818->0x83144208. PM4 parser + XenosGpuState update for first 5 swaps. Dumps diff to `gpu_state_diff.txt`. |
| sub_82BFBF30 (XenosWait) | Counter sync |
| sub_8255D430 (BeginFrameXenos) | Stubbed |
| sub_8255D470 (EndFrameXenos) | Stubbed |
| sub_8255D520 (GpuState) | Call orig first 3 times for logging |
| sub_82ABF828 (BeginFrame) | Stubbed |
| sub_82ABF930 (EndFrame) | CaptureGameFrame(CPU readback). Call orig every frame (fires VdSwap). `sub_8255CFE0` stub breaks GPU frame-pending spin loop so VdSwap #2+ complete. PresentGameFrame skipped during Bink. |

### Wait / Events
| Hook | Behavior |
|------|----------|
| sub_82BFB740 (Wait) | 500ms->SUCCESS. -1->orig_Wait 3s then SUCCESS |
| sub_82BFB748 (NtSetEvent) | Calls orig_SetEvent (un-stubbed!) |
| sub_82BFBF48 (ErrorRecovery) | Stubbed |

### Game Loop
| Hook | Behavior |
|------|----------|
| sub_82B70760 (MainLoop) | Frame 1: dump eng+12/vtable info. Set eng+8=eng (self-ref override — vt[17] populated eng+8 with AssetDB_block, but that would make MainLoop's vt[36] call jump to AssetDB's vt[36] = "SP_EVENT" string data; overriding to eng lands vt[36] on Bootstrap's nullsub_1 instead). Set byte_82D57994=1, clear @ frame 600. Call orig, force r3=1. Sleep(16). |
| sub_82B70578 (RenderPipeline) | Call orig every frame (no cap). Skip orig while Bink playing (host render thread owns swapchain during Bink). |

### Loading
| Hook | Behavior |
|------|----------|
| sub_82B710D0 (Transition) | Call orig_Transition |
| sub_82B70DE8 (LoaderTick) | REX_HOOK_RAW + call orig. Force r3=0 after iteration 101. Mid-ASM hooks #6-#8 skip entity/renderer code. |
| sub_82B71148 (SetupRenderer) | Call orig. Mid-ASM hooks #2 + #5 skip crash points (#3, #4 disabled — vt[8] harmless, vt[17] populates eng+8). C++ fallback replicates `*(eng+8) = AssetDB_block` only if hook #4 is active (eng+8 still NULL after orig). |

### Diagnostic Logging
| Hook | Behavior |
|------|----------|
| sub_82ABB838 (Bootstrap) | Log entry |
| sub_82AEBF40 (GraphicsInit) | Log device state at +56/+104/+2388, gpu_phys after init |
| sub_82AE9658 (PostGfxInit) | Call orig |
| sub_82373660 (TexManager) | Call orig |
| sub_82B6F820 (BindTexture) | Call orig |
| sub_82AB7848 (GpuAlloc) | Log first 8 allocs: size -> address |

### Cleanup
| Hook | Behavior |
|------|----------|
| sub_82533D80 (Cleanup1) | Zero dword_830577C0 |
| sub_82B70BE8 (Cleanup2) | Stubbed: zero dword_830BE190 |

### Keep-Alive
| Hook | Behavior |
|------|----------|
| sub_82BA7F58 (EngineInit) | SetGuestMemory(base). Call orig. Sleep loop. |

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
- Location: `device + 48` (write) / `device + 52` (end), ring buffer
- ~64KB per VdSwap
- Parse first 5 VdSwap calls (15055 packets total: 289 Type3, 819 Type0, 13947 Type2)

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
- `Snapshot()` / `DumpDiff()`: captures register snapshot, diffs vs previous, dumps to `gpu_state_diff.txt`
- 32 registers captured from first VdSwap: resolution (720x1280), GPU base addr, shader constants

---

## Input

- ReXGlue SDLInputDriver handles `XamInputGetState`/`XamInputGetCapabilities` natively at kernel level
- No manual `REX_FUNC` hooks for input (removed, redundant with ReXGlue built-in driver)

---

## Build Process

### Codegen required (mid-ASM/config changes)
1. Edit mx_config.toml + native_graphics.cpp + CMakeLists.txt
2. `rexglue codegen --force mx_manifest.toml` (~75s)
3. `cmake --preset win-amd64-debug`
4. `cmake --build ... --target mx`

### C++ only
1. Edit any .cpp/.h file
2. `cmake --build ... --target mx` (~10s)
3. Copy the fresh binary to the project root before running:
   `Copy-Item out\build\win-amd64-debug\mx.exe mx.exe -Force`
   (the build outputs to the build dir; the root `mx.exe` is what `Start-Process` launches against the assets at this directory)

---

## Current State

### Working
- 60fps game loop (MainLoop Sleep(16), r3 forced 1)
- Bink intros (THQ Logo -> Attract) via FFmpeg + SDL audio
- SetupRenderer completes (5 mid-ASM hooks active, hooks #2 + #5 skip crash points)
- **RenderPipeline + EndFrame run every frame** — EndFrame #2+ hang FIXED via `sub_8255CFE0` stub (GPU frame-pending poll returns 0); VdSwap fires every frame, no caps
- **3D game pipeline**: colored triangle renders via game PSO (VS with vertex buffer + MVP constant buffer, PS pass-through color)
- **Game RT + depth**: dedicated 1280x720 render target + D32 depth buffer, PresentGameFrame copy to swapchain
- **PM4 parser**: 15055 packets decoded from first 5 VdSwaps, ring wrap handled, big-endian byteswap, validated
- **GPU state tracking**: 66 Xenos registers shadowed, 32 regs captured from first VdSwap, diff dumped to file
- **Input**: SDL gamepad init + Xbox 360 Controller, ReXGlue handles XamInput natively
- **LoaderTick**: orig runs 100 iterations (pre-entity code: Wait, timing, vtable calls), forces completion at iter 101
- **eng+8 populated** — vt[17] (`sub_82B43AC8`) runs naturally (mid-ASM hook #4 disabled), writes `*(eng+8) = AssetDB_block`. Self-ref override `eng+8 = eng` at MainLoop #1 still fires (lands MainLoop's vt[36] on Bootstrap's nullsub_1, NOT AssetDB's vt[36] = "SP_EVENT" string data → would crash).
- EngineInit sleep loop keeps process alive
- 5 mid-ASM hooks active (#2, #5, #6, #7, #8; #1, #3, #4 disabled) + ~25 C++ hooks
- Engine state diagnostic dump at frame 1 (eng+12 vtable, eng+8 self-ref workaround)
- Bootstrap vtable[36] = `nullsub_1` (0x82426FF0), harmless no-op

### Not Working
- **Game rendering**: GPU defanged, entities empty — triangle is placeholder
- **LoaderTick entity loading**: mid-ASM hooks skip entity code; entities never populated
- **Game input consumption**: guest never calls XamInputGetState (game tick never runs)
- **No menu/gameplay state**: LoaderTick completes but entities never loaded, game stays in MainLoop with triangle

### Blocked
- Entity loading needs working GPU render state (circular dependency). Entity code skipped by mid-ASM hooks #6-8.
- Binary `.xenon.package` heaps encrypted (entropy ≈7.98, unknown decryption routine; OpenSSL AES bundle is TLS-only).
- FATAL crash at 0x82327CF0 during gameplay (separate known issue).
- LoaderTick renderer block (`sub_82B34998`) structurally cannot complete without GPU plugin — vtable `off_8213F70C` dispatches to `sub_82BDB190` fatal terminators, `*(a1+1288)` uninitialized. See `docs/loader_render_block.md` for full analysis. Baseline caps LoaderTick at 101 iterations with r3=0.

### PATH 1 EXPERIMENT — cascade of hang points discovered (2026-07-31)

**Moved to `docs/loader_render_block.md`** — see PATH 1 EXPERIMENT section there for the full cascade-of-hangs post-mortem. Summary: pre-populating `dword_830BE190` requires confronting 5+ cascaded hang points; LoaderTick's natural renderer dispatch (`sub_82B34998`) hits a fatal terminator vtable (`off_8213F70C`) that is only overridden by the GPU plugin at runtime. Reverted; baseline (hook #6 skips the entire renderer block, LoaderTick cap at iter 101 with r3=0) is the working approach.

---

## Deep Technical Specs

Investigation logs and post-mortems moved to separate docs to keep AGENTS.md focused on operational essentials:

| Doc | Contents |
|-----|----------|
| `docs/loader_render_block.md` | LoaderTick renderer block analysis, PATH 1 EXPERIMENT cascade of hang points, sub_82B34998 structural post-mortem (fatality terminators by design), eng+8 writer trace (sub_82B43AC8 / vt[17]), Path 2 GPU plugin shim design, EndFrame #2 hang root cause + fix (sub_8255CFE0 GPU frame-pending poll) |
| `docs/asset_format.md` | BXML format structure + reconstruction status, .xenon.package layout, per-asset-type heap headers, AssetDB_LoadStateMachine 12-case state machine, asset loader infrastructure (renamed functions), .xenon.package encryption discovery (entropy validated ≈7.98), OpenSSL AES in guest binary (TLS-only), shader microcode load path, asset catalog (23,183 assets), decoder tools reference |
| `docs/pm4_pipeline.md` | PM4 parser (bit-field fix per Xenia, opcode table), Xenos GPU state shadow, plugin-mode data capture (runtime state at frame #600, vtable correction off_8213F7A4 vs off_8213F70C, GpuAlloc layout, steady-state behavior), gameplay PM4 frame structure, plugin cvar list (130 cvars), revised native backend strategy, PM4 translator verified working (auto + indexed draws, IB extraction blocker), ReXGlue SDK + Xenia reference files |
| `docs/ida_notes.md` | Consolidated IDA bookmark table (slots 0-28) grouped by investigation theme: LoadStateMachine states, asset loader, shader microcode, OpenSSL AES, Path 2 shim, eng+8 writer |
