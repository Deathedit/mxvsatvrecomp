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
| File | Role |
|---|---|
| `src/main.cpp` | Entry point |
| `src/mx_app.h` | App class + D3D12GraphicsSystem + Bink render thread + gamepad init |
| `src/d3d12_renderer.h/.cpp` | D3D12 renderer: swapchain, video pipeline, game pipeline (triangle PSO), game RT+depth, PresentGameFrame copy |
| `src/bink_player.h` | FFmpeg Bink video + audio decoder |
| `src/native_graphics.h` | NativeGraphics class: guest mem base, game frame buffer, CaptureGameFrame (legacy readback) |
| `src/native_graphics.cpp` | All guest function hooks (~25 C++ hooks, 8 mid-ASM stubs) |
| `src/native_input.h/.cpp` | GamepadState singleton (XInput-compatible struct, SDL gamepad init in renderer) |
| `src/native_heap.h/.cpp` | Guest heap management |
| `src/pm4_parser.h/.cpp` | PM4 command buffer parser: Type-0/2/3 decode, 30 opcodes, 65 reg names, ring wrap, dump |
| `src/xenos_gpu_state.h/.cpp` | Xenos GPU register shadow: 66 named registers, ApplyType0Write, ApplyType3Packet, Snapshot/DumpDiff |
| `mx_config.toml` | Function sizes + `[[midasm_hook]]` definitions |
| `CMakeLists.txt` | Linker exports for mid-ASM hooks |

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
| +8 | NULL → workaround: set to eng | AssetDB (never assigned by SetupRenderer/Bootstrap). Self-ref workaround lets MainLoop's vt[36] call land on Bootstrap's no-op. |
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
    sub_82ABF930(renderer);  // EndFrame -> VdSwap (fires once for PM4 capture)
}
```

---

## Mid-ASM Hooks (mx_config.toml)

Inject at PPC instruction addresses. Replace instruction with C++ call + goto jump_address.

| # | Address | Name | Jump To | Skips |
|---|---------|------|---------|-------|
| 1 | 0x82B70854 | NativeGameTickSkip | 0x82B70874 | MainLoop vtable[36] crash (DISABLED — commented out, eng+8 workaround replaces it) |
| 2 | 0x82B71290 | NativeSetupDeviceSkip | 0x82B712A4 | SetupRenderer vtable[6] block |
| 3 | 0x82B712C4 | NativeSkipVtable8 | 0x82B712D8 | SetupRenderer vtable[8] |
| 4 | 0x82B71304 | NativeSkipVtable17 | 0x82B71314 | SetupRenderer vtable[17] |
| 5 | 0x82B71324 | NativeSkipRendererInit | 0x82B71690 | Renderer init -> Transition thread |
| 6 | 0x82B70EC8 | NativeSkipLoaderRenderer | 0x82B710BC | LoaderTick renderer block |
| 7 | 0x82B70E18 | NativeSkipLoaderEarly | 0x82B70EC8 | LoaderTick vtable+entities |
| 8 | 0x82B70DFC | NativeSkipLoaderAll | 0x82B70EC8 | LoaderTick everything |

**Hooks #2-#8 active** (7 hooks in config). Hook #1 disabled (eng+8 self-reference workaround handles vtable[36] crash). All 8 export symbols remain in CMakeLists.txt.

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
| sub_82ABF930 (EndFrame) | CaptureGameFrame(CPU readback). Call orig once (fires VdSwap for PM4 capture). Silent after. |

### Wait / Events
| Hook | Behavior |
|------|----------|
| sub_82BFB740 (Wait) | 500ms->SUCCESS. -1->orig_Wait 3s then SUCCESS |
| sub_82BFB748 (NtSetEvent) | Calls orig_SetEvent (un-stubbed!) |
| sub_82BFBF48 (ErrorRecovery) | Stubbed |

### Game Loop
| Hook | Behavior |
|------|----------|
| sub_82B70760 (MainLoop) | Frame 1: dump eng+12/vtable info. Set eng+8=eng (self-ref workaround for NULL AssetDB). Set byte_82D57994=1, clear @ frame 600. Call orig, force r3=1. Sleep(16). |
| sub_82B70578 (RenderPipeline) | Call orig for rp <= 10. Silent after. |

### Loading
| Hook | Behavior |
|------|----------|
| sub_82B710D0 (Transition) | Call orig_Transition |
| sub_82B70DE8 (LoaderTick) | REX_HOOK_RAW + call orig. Force r3=0 after iteration 101. Mid-ASM hooks #6-#8 skip entity/renderer code. |
| sub_82B71148 (SetupRenderer) | Call orig. Mid-ASM hooks skip crash points. |

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

### PM4 Parser (`src/pm4_parser.h/.cpp`)
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

### Xenos GPU State (`src/xenos_gpu_state.h/.cpp`)
- Tracks 66 named Xenos registers in `regs_` map
- `ApplyType0Write(reg_base, data, count)`: writes count consecutive registers
- `ApplyType3Packet(pkt)`: handles SET_CONFIG_REG, SET_CONTEXT_REG, SET_ALU_CONST, etc.
- `Snapshot()` / `DumpDiff()`: captures register snapshot, diffs vs previous, dumps to `gpu_state_diff.txt`
- 32 registers captured from first VdSwap: resolution (720x1280), GPU base addr, shader constants

---

## Render Targets

sub_82629560 -> sub_8254DED8 -> sub_82469998 -> GpuAlloc

Device offsets (handles, not addresses):
- +56: Depth surface (0x04)
- +104: Color render target (0x04)
- +2388: 640-height buffer

Actual memory at 0xBEDA0000 (GpuAlloc #1, 15MB).

---

## Input

- `GamepadState` singleton in `src/native_input.h/.cpp` holds XInput-compatible gamepad struct
- SDL gamepad subsystem initialized in `InitializeRenderer` (SDL_InitSubSystem + SDL_GetGamepads)
- ReXGlue SDLInputDriver handles `XamInputGetState`/`XamInputGetCapabilities` natively at kernel level
- No manual `REX_FUNC` hooks for input (removed, redundant with ReXGlue built-in driver)

---

## LoaderTick: Why It Cannot Work

1. a1 = 0x830EC248 (valid, not NULL)
2. a1+8 = 0x407F2190 (valid vtable 0x8204C08C)
3. Events at +190/+194 = 0 (renderer init skipped)
4. Mid-ASM hooks skip ALL entity code
5. Without entity code: no entities, no draw calls
6. Removing hooks: entity code crashes accessing GPU state
7. Circular: loading needs GPU, GPU needs entities

**Current approach**: REX_HOOK_RAW calls orig with 100-iteration limit (entities/renderer skipped by mid-ASM), forces r3=0 to complete loading.

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
- SetupRenderer completes (7 mid-ASM hooks, hooks #2-#5 skip crash points)
- RenderPipeline runs orig x10 (no crash), EndFrame fires VdSwap once for PM4 capture
- **3D game pipeline**: colored triangle renders via game PSO (VS with vertex buffer + MVP constant buffer, PS pass-through color)
- **Game RT + depth**: dedicated 1280x720 render target + D32 depth buffer, PresentGameFrame copy to swapchain
- **PM4 parser**: 15055 packets decoded from first 5 VdSwaps, ring wrap handled, big-endian byteswap, validated
- **GPU state tracking**: 66 Xenos registers shadowed, 32 regs captured from first VdSwap, diff dumped to file
- **Input**: SDL gamepad init + Xbox 360 Controller, ReXGlue handles XamInput natively
- **LoaderTick**: orig runs 100 iterations (pre-entity code: Wait, timing, vtable calls), forces completion at iter 101
- EngineInit sleep loop keeps process alive
- 7 mid-ASM hooks (all active in config) + ~25 C++ hooks
- Engine state diagnostic dump at frame 1 (eng+12 vtable, eng+8 self-ref workaround)
- `eng+8 = eng` workaround prevents vtable[36] crash without hook #1
- Bootstrap vtable[36] = `nullsub_1` (0x82426FF0), harmless no-op

### Not Working
- **Game rendering**: GPU defanged, entities empty — triangle is placeholder
- **LoaderTick entity loading**: mid-ASM hooks skip entity code; entities never populated
- **Game input consumption**: guest never calls XamInputGetState (game tick never runs)
- **eng+8 (AssetDB slot) is NULL** at runtime — never assigned by SetupRenderer or Bootstrap
- **AssetDB vtable incomplete**: only ~18 entries, vt[36] reads string data ("SP_EVENT") — not a function pointer. (Note: this refers to the Bootstrap/AssetDB dispatch table at `0x8204C08C`, distinct from the eng+12 SceneManager vtable at `0x82141754` whose vt[36] is "Load" string data.)
- **No menu/gameplay state**: LoaderTick completes but entities never loaded, game stays in MainLoop with triangle

### Blocked
- Entity loading requires working GPU render state (circular dependency)
- AssetDB assignment path not yet traced through Bootstrap/SetupRenderer vtable calls
- ~~Correct game tick vtable[36] likely lives in engine subsystem at eng+12 — needs investigation~~ **DISPROVEN**: eng+12 vtable at 0x82141754 is shorter than 36 entries; vt[36]=0x4C6F6164 is ASCII "Load" string data, not a function. The Bootstrap `eng+8=eng` workaround reaching Bootstrap's vt[36]=nullsub_1 is the only currently-known safe path.
- **AssetDB-via-dword_830577C0 path also DISPROVEN**: runtime dump at MainLoop frame 1 showed `dword_830577C0 = 0x407F2190`, vtable `0x8204C08C`. That vtable's vt[36] = `0x53505F45` (ASCII bytes for `"SP_E"`-prefix of `"SP_EVENT"` string, big-endian read) — NOT a function pointer. Writing `eng+8 = dword_830577C0` would crash MainLoop's vt[36] call jumping to `0x53505F45`.
- ~~The actual game-tick code (the LoadTick code that populates entities) lives in the LoaderTick renderer block skipped by mid-ASM hook #6~~ **CORRECTED**: hook #6's range (0x82B70EC8 → 0x82B710BC) skips the LoaderTick **renderer block** (lazy-init `dword_830BE190` via off_82D5648C, then `sub_82B34998` heavy renderer dispatch + 5 entity loops + final scene-manager vtable call + r3=1 return). The earlier worry that removing hook #6 risks `*(a1+8)->vtable[6]()` crashing is **WRONG**: LoaderTick reads `(a1+8)` where `a1` is the **transition renderer** (parameter from Transition), NOT the engine. Transition renderer's slot+8 = `dword_830EC250`, which EngineInit populates with AssetDB at `stw r3, dword_830EC250@l(r30)` (0x82ba7fe4). At runtime: `*(dword_830EC250) = 0x407F2190` (AssetDB), vt[6] = `sub_8253AA40` (a real function). **LoaderTick's gating call is safe to execute.** Risk shifts to the renderer block itself: `sub_82B34998` calls into Xenos GPU dispatch functions that the GPU plugin would normally serve; if any of those reaches a stubbed native graphics hook (BeginFrame/EndFrame/etc.), the stubs no-op and the dispatch returns — likely safe but unverified.
- **Hook #6 removal EXPERIMENTED (2026-07-31) and reverted**: disabling the `NativeSkipLoaderRenderer` mid-ASM hook (so LoaderTick's natural renderer block at 0x82B70EC8 runs) caused the process to hang and die inside the renderer block. Symptom: the LoaderTick wrapper's `__imp__sub_82B70DE8(ctx, base)` call **never returned** — no `native: LoaderTick #N r3=...` log line ever fired (the previous baseline showed LoaderTick #1..#5 quickly then #101). The Transition thread stalling cascades: MainLoop reached frame 1, fired VdSwap #1 (15055 packets, 0 draws), began frame 2's `RenderPipeline #2 → orig_EndFrame(EndFrame#2)`, and the process terminated while inside `orig_EndFrame` #2.
- **Hook #6 hang point CONFIRMED via bisection (2026-07-31)**: Installed three mid-ASM debug hooks (NativePostLazyInitLog @ 0x82B70EEC, NativePreDispatchLog @ 0x82B70EF0, NativeSkipRendererDispatch @ 0x82B70EF4) with hook #6 disabled. **None of the three stubs ever fired**, proving execution stalls BEFORE reaching 0x82B70EEC — the only code path between 0x82B70EC8 (renderer block entry) and 0x82B70EEC is the `bctrl sub_82B3C7D0` indirect call at 0x82B70EE8 (first-time lazy-init alloc via off_82D5648C). The alloc chain `sub_82B3C7D0 → sub_82AB73C0(61568) → sub_82AB7110 → RtlEnterCriticalSection → sub_82AB48F0 → sub_82C01B00 → __imp__MmQueryStatistics → ...` runs fine when invoked from main-thread context earlier in setup (Bootstrap's `sub_82AB73C0(80)` and SetupRenderer's `sub_82AB73C0(76)` work), but appears to hang when first invoked from the Transition thread context inside LoaderTick. Why: TBD — possibly thread-local state isn't fully set up in the new thread, or the delay/lock-wait path inside MmQueryStatistics/RtlEnterCriticalSection blocks indefinitely under that context.
- **Why `dword_830BE190` is NULL when LoaderTick runs**: SetupRenderer's natural code includes its own lazy-init for `dword_830BE190` via `off_82D5648C()` at 0x82B71334, but this code is BETWEEN mid-ASM hook #5's fire-address (0x82B71324) and its jump_address (0x82B71690) — so hook #5 (`NativeSkipRendererInit`) skips it. The lazy-init is therefore deferred to the first LoaderTick call. With hook #6 active, the lazy-init bctrl never runs (hook #6 jumps past it). With hook #6 disabled, the bctrl runs in the Transition thread and hangs.
- **Strategic next step (3 candidate paths)**:
  1. **Pre-populate `dword_830BE190` from C++ side**, via a `REX_FUNC` hook on `sub_8253CF08` (AssetDB init, called by EngineInit at 0x82BA7FD0 just before SetupRenderer's `bl sub_82B71148`) — after orig_AssetDB init returns, dispatch a synchronous call into `off_82D5648C()` (= sub_82B3C7D0) so the alloc happens in the main thread context BEFORE Transition is created. Result: when LoaderTick hit the bctrl, `dword_830BE190` is non-zero, bne skips the alloc, executes loc_82B70EF0 directly. Then we keep hook #6 disabled and watch the entity loops run (potentially hang elsewhere — bisection would continue inside sub_82B34998's callees).
  2. **Disable hook #5 too**: let SetupRenderer's natural init run, populating `dword_830BE190` itself. Risk: SetupRenderer's skipped band (0x82B71290..0x82B71690) does all kinds of device/renderer init that was previously tagged "skip crash points" — disabling hook #5 alone might hang SetupRenderer at a different point. Trial-and-error if revisited.
  3. **Write a host-side shim for sub_82B3C7D0**: install a `REX_FUNC(sub_82B3C7D0)` C++ hook that pre-allocates the 60KB via host heap (RtlAllocateHeap / malloc) at a stable guest address, returns that guest address in `ctx.r3`, and writes it to `dword_830BE190` via `REX_STORE_U32(0x830BE190, address)`. Bypass the entire alloc dispatch chain. Less surgical risk; risky if the cached state struct needs further initialization by `sub_82B39878` (the constructor called after sub_82AB73C0 returns non-zero).
- **Reverting note**: Hook #6 was re-enabled after the experiment. Project is back to working baseline — 60fps game loop, Bink intros play, LoaderTick #1→#101 with broadened r3 cap.
- Bisection recipe if revisited: write a "bounds check" mid-ASM hook at `sub_82B33EC0` entry or `sub_82B307D8` entry that pins `r3 = result` (skips their bodies), then enable hook #6 again to see if LoaderTick completes. Each of `sub_82B33EC0`, `(*a1->vt[8])(...)`, `sub_82B307D8`, `(*a1->vt[7])(...)`, `sub_82B2C498` is a candidate gating point.

### PATH 1 EXPERIMENT — cascade of hang points discovered (2026-07-31)

Attempted Path 1 (pre-populate `dword_830BE190` from main-thread context) and bisected through FIVE cascaded hang points before hitting the same fundamental blocker (entity rendering needs `sub_82B34998` to work). The path itself is implementable but requires confronting each hang — and the natural renderer dispatch (`sub_82B34998`) cannot be made to complete under the no-GPU-plugin profile. Full experiment reverted; baseline restored.

**Sequence of cascading failure points (all discovered in this experiment)**:

| Stage | Fix applied | Outcome (next hang exposed) |
|--------|-------------|-----------------------------|
| 0. Baseline-disabled hook #6 | n/a (just pre-populate via `REX_CALL_INDIRECT_FUNC(0x82B3C7D0)` in SetupRenderer hook, then disable hook #6) | SetupRenderer itself hung immediately after pre-population completed |
| 1. Register pollution | Save/restore `r0/r3-r12/lr/ctr` around the `REX_CALL_INDIRECT_FUNC` call (sub_82B3C7D0 trashes volatiles — orig_SetupRenderer saw garbage EngineInit inputs) | SetupRenderer reached GraphicsInit/TexManager/skip-vtable[8]/skip-vtable[17] then hung in the 16 bytes between 0x82B71314 and 0x82B71324 |
| 2. Hook #5 fire-address | Move hook #5 address from 0x82B71324 → 0x82B71314 (BEFORE the cached-check; otherwise `dword_830BE190 != 0` routes `bne` TO loc_82B71338, around hook #5's fire addr — letting natural SetupRenderer code at 0x82B71338..0x82B7168C run, which hangs) | SetupRenderer now RETURNED ✓; Transition thread started ✓, but orig_LoaderTick never returned (no LoaderTick #N logs) |
| 3. Hook #8 register-setup skip | Disable hook #8 (NativeSkipLoaderAll @ 0x82B70DFC). It REPLAC `mr r23, r3` (preserves transition renderer ptr) AND skips over `lis r21, dword_830BE400@ha` at 0x82B70E14. The renderer block needs BOTH: `lfs f31, 0x18(r23)` at 0x82B70ECC and `lwz r11, dword_830BE400@l(r21)` at 0x82B70EFC + 0x82B70F64. With r23+r21 garbage, hang was immediate at `lfs f31, 0x18(r23)`. Hook #7 (NativeSkipLoaderEarly @ 0x82B70E18) takes over — fires AFTER r23+r21 setup, jumps to 0x82B70EC8 | Still no PreDispatch #1 — orig_LoaderTick still stuck |
| 4. Wait hook | Always return SUCCESS from `sub_82BFB740`. Previously hook tried `orig_Wait` first call (with -1 timeout it blocks FOREVER — the guest event at +0x194 is never signaled because SetupRenderer's natural NtSetEvent code was skipped by hook #5). Subsequent calls aged past 3s fallback OK; first call hung | Still no PreDispatch #1 — orig_LoaderTick still stuck after Wait returned |
| 5. Timing function | Stub `sub_82B70370` to no-op return. The natural `sub_82B70370` calls QPC primitives (sub_82BFC728/QPC, sub_82BFC748/perf-freq) and runs a busy-wait loop `while (v7 < *(a1+20))` until timing threshold met — under our recomp with non-real QPC values, this loop spins forever. LoaderTick never reached hook #7 (NativeSkipLoaderEarly at 0x82B70E18) | **PreDispatch #1 finally fired** at 18:13:21.201 — execution reached loc_82B70EF0. Cached-check `bne` correctly takes the branch past the lazy-init. PreDispatch #2 never fired → `bl sub_82B34998` at 0x82B70EF4 hangs on first call |
| 6. Renderer dispatch | Install `NativeSkipRendererDispatch` mid-ASM hook at 0x82B70EF4 with `jump_address = 0x82B70EF8` (skips the `bl sub_82B34998` instruction itself) — fires + logs once then continues into post-dispatch body | **SkipRendererDispatch #1 fired**, but #2 never did — the post-dispatch body (0x82B70EF8..0x82B710BC) hangs on its first pass somewhere |

**Post-dispatch body unreached** (full disasm logged in IDA at 0x82B70EF8..0x82B710BC):

| Address | Call/code | Notes |
|---------|-----------|-------|
| 0x82B70EF8..0x82B70F1C | entity loop 3: `lwz r11, dword_830BE400@l(r21); lwzx r3, r31, r11; lwz r10, 0x3C(r3); cmpwi cr6; beq loc_82B70F14; bl sub_82B237B0` (loop: r31 from 0x1C to 0x24 by 4) | Calls `sub_82B237B0` for engine sub-entities with `*0x3C != 0` — untested |
| 0x82B70F48 | `bl sub_82B67D98 (lazy-init dword_82D6F144)` | untested |
| 0x82B70F54 | `bl sub_82BDC040` | untested |
| 0x82B70F5C | `bl sub_82B676B8` | untested |
| 0x82B70F60..0x82B70F78 | entity loop 4: `bl sub_82AFF120` (no conditional — fires for every entity from r31=0x1C..0x24) | untested |
| 0x82B70F7C..0x82B70FA0 | entity loop 5: `lwz r10, 0x3C(r3); cmpwi cr6, r10, 0; beq loc_82B70F98; bl sub_82B0A0D0` (conditional like loop 3) | untested |
| 0x82B70FA8..0x82B70FAC | `lwz r3, 0x2E0(r23); bl sub_82BFB740` — Wait on event at +0x2E0 (different event from +0x194) | likely safe — our hook now returns SUCCESS unconditionally |
| 0x82B70FDC, 0x82B70FE8 | second `bl sub_82B67D98`, `bl sub_82BDC040` (lazy-init path, conditional on dword_82D6F144 flag bit 0) | appears to be called twice before/after Wait; both untested |
| 0x82B70FF0 | `bl sub_82B676B8` — second call | untested |
| 0x82B70FF4..0x82B71004 | `lwz r3, 0x190(r23); bl sub_82BFB748` (NtSetEvent on event at +0x190) + `bl sub_82BFBF48` (ErrorRecovery fallback if NtSetEvent returns 0) | sub_82BFBF48 is our stubbed ErrorRecovery |
| 0x82B71014..0x82B710A0 | outer scan loop iterating engine sub-entities (`r24 = 0x24 down by 4`): for each iteration, `lwzx r31, r24, r11; bl sub_82B1F410` if entity has `*0x3C != 0`; then nested per-iteration bl `sub_82B6FF78`, `sub_823EDD40` | many untested callees |
| 0x82B710A4..0x82B710B8 | final scene-manager vtable call: `lwz r11, dword_830BE400@l(r21); lwz r3, 0xC(r11); lwz r11, 0(r3); lwz r10, 0xC(r11); mtctr r10; bctrl` (engine[0xC]->vt[3]()) | engine[0xC] = SceneManager; engine[0xC]->vt[3] untested |
| 0x82B710BC | `li r3, 1; addi r1, r1, 0xC0; lfd f31, var_68(r1); b __restgprlr_21` — end of renderer block epilogue, returns r3=1 | safe |

**Decision: REVERT**. Implementing all the necessary stubs to navigate the post-dispatch body is multi-iteration work; the dispatch call (`sub_82B34998`) itself is GPU-plugin-dependent entity rendering — making it work requires replacing it with a host-side renderer, the same as we already do via our D3D12 game-RT triangle. The pre-population cascade taught us where every hang is, but completing LoaderTick naturally requires either:
  - (a) Stubbing every entity-render callee to no-op + replacing sub_82B34998 entirely with our D3D12 game RT path, then trimming entity code at the scene manager vt[3] gating call (return truthy → render path) — OR
  - (b) Replacing LoaderTick's body wholesale from the C++ hook (don't call orig_LoaderTick at all — directly compute the result for Transition wrapper's loop) — simpler but bypasses all natural entity code (consistent with current baseline behavior: LoaderTick iterates 100 times then r3=0 cap).

The current baseline (option b essentially — hook #6 skips the entire renderer block) remains the best of the working options.

### sub_82B34998 structural post-mortem — vtable dispatches are FATAL terminators by design (2026-07-31)

Following Path 1's collapse to "bl sub_82B34998 hangs", drilled into sub_82B34998's internal callees and vtable dispatches. Decoded the vtable `off_8213F70C` (installed by the `sub_82B38558` constructor that runs after our pre-populated `sub_82B3C7D0` alloc):

**vtable off_8213F70C entries** (big-endian u32 read from data section):

| Slot | Offset | Address | Function / role |
|------|--------|---------|-----------------|
| vt[0] | +0  | 0x82B3C828 | (function) |
| vt[1] | +4  | 0x82B38830 | |
| vt[2] | +8  | 0x82B2C270 | |
| vt[3] | +12 | **0x82BDB190** | **terminator** |
| vt[4] | +16 | 0x82B35FE0 | |
| vt[5] | +20 | 0x82B2C2E8 | |
| vt[6] | +24 | **0x82BDB190** | **terminator** |
| vt[7] | +28 | **0x82BDB190** | **terminator** ← called by sub_82B34998's `(*a1->vt[7])(a1, f1)` at 0x82B34A24 |
| vt[8] | +32 | 0x82B2C4C8 | `sub_82B2C4C8` — iterates 4 entity slots, calls vt[15]/vt[16] |
| vt[9] | +36 | 0x82426FF0 | `nullsub_1` |
| vt[10] | +40 | 0x82B38A48 | |
| vt[11] | +44 | 0x82B38CC0 | |
| vt[12] | +48 | 0x82B38D90 | |
| vt[13] | +52 | 0x82B2C738 | |
| vt[14]–vt[19] | +56–+76 | **0x82BDB190** ×6 | **terminators** |

**Fatality chain of `sub_82BDB190`** (called as vt[7] / vt[15] / vt[16]):
```
sub_82BDB190 (76 bytes, __noreturn)
 ├─ if (dword_83132F10) dword_83132F10();   ← indirect function ptr (likely NULL/handler hook)
 ├─ sub_82BE62F0(25);                        ← calls sub_82BE62A8 then sub_82C09198
 ├─ v0 = sub_82BDEDE0(0, 1);
 └─ sub_82BDED90(v0);    ← void __noreturn
      ├─ if (sub_82BF0820()) sub_82BF0838(22);     ← raise guard 0x16
      ├─ if (dword_82D584D0 & 2) sub_82BDAA28(3, 0x40000015, 1);
                                                    ↑ STATUS_FATAL_APP_EXIT (0x40000015) — KeBugCheckEx-style
      └─ sub_82BEE290(3);                          ← final exit
```

**Conclusion**: The `off_8213F70C` vtable is a stub/proxy vtable shipped by the original game — its vt[3,6,7,14-19] entries deliberately dispatch to a kernel killed-fatal handler (`sub_82BDB190` → `sub_82BDAA28(3, STATUS_FATAL_APP_EXIT, 1)`). The original design intent: these slots would be **overridden by `rexgpu-xenosd.dll`** (the Xenos GPU plugin) stapling real render implementations in their place. Without the GPU plugin present, calling ANY of these slots crashes the application with `STATUS_FATAL_APP_EXIT` (0x40000015).

**What sub_82B34998 does to itself**:
- Reads `dword_830BE190` (now our pre-populated 60KB block) as its `a1`
- Calls `sub_82B2C9D0` (gating check — TLS read, returns BOOL)
- If gating OK:
  - Calls `sub_82B33EC0(a1, f1)` — iterates `*(a1+72) + 1` entities (constructor sets `*(a1+72)=1`, so 2 iters), calling `sub_82B33E78(a1+76 + i*96, f1)` per entity. (`sub_82B33E78` = `sub_82B33E18` + `sub_82B33D40` — both are float-arm/lerp math + per-entity update callbacks. Not GPU-bound directly.)
  - Calls `(*a1->vt[8])(a1, f1)` = `sub_82B2C4C8(a1, f1)` — iterates 4 entity slots at `a1+1352` (i.e., `(a1+1296, 1300, ...)`), dispatching `(*a1->vt[15])(a1, v4-14)` or `(*a1->vt[16])(a1, v4-14)` based on flags. **vt[15] AND vt[16] are both `sub_82BDB190` (terminator).**
  - Calls `sub_82B307D8(a1, f1)` — wraps `sub_82B2D030(*(a1+1288))`. `*(a1+1288)` is uninitialized (constructor only sets up to a1+76 explicitly via `sub_82B32178(a1+19)`). Likely NULL or garbage → `sub_82B2D030(0)` → reads `*(int*)0+8` = NULL-ptr crash.
  - Calls `(*a1->vt[7])(a1, f1)` = `sub_82BDB190(a1, f1)` → **fatal exit immediately**.
  - Calls `sub_82B2C498(a1)` — wraps `(*(int(*)())((*(int*)a1) + 68))()` if non-NULL. Constructor set `*(a1+68)=0`, so this is a no-op returning `a1`.
- If gating fails: `sub_82B36298(a1, 2, ...)` — calls `sub_82B35340()` if `*(a1+61036)` is 0 (likely 0 — uninitialized). `sub_82B35340` is a recursive depth-4 graph that eventually reaches even more terminators + `sub_82BFBF30` (XenosWait).

**Final structural verdict**: `sub_82B34998` CANNOT be made to complete naturally in our no-GPU-plugin profile. It is a GPU-plugin-only code path by design. Three independent reasons:

1. **vt[8] → vt[15]/vt[16] terminators**: `sub_82B2C4C8` iterates entity slots and dispatches the terminator regardless of state — fatal.
2. **vt[7] = terminator**: called unconditionally after the (also-fatal) vt[8] dispatch — fatal if execution somehow survived.
3. **`*(a1+1288)` uninitialized** (NULL/garbage): `sub_82B2D030(NULL)` immediately reads `*(int*)NULL+8` — null pointer crash.

These would only be replaced by GPU plugin's real vtable slots in the original Xbox 360 + Xenos + rexgpu-xenosd.dll profile. In our pure-host D3D12 renderer, sub_82B34998 is structurally unreachable.

### Implications for future work

- **No more bisection inside `sub_82B34998` will help** — the vtable dispatches are designed fatal. Stubs would have to replace the vtable itself (`*a1 = &our_custom_vtable`), then implement each vt slot with our D3D12 game-RT renderer. That's a wholesale rewrite of the renderer block as a guest-callable surface — equivalent to writing a Xenos GPU plugin.
- The current baseline (hook #6 skips the entire renderer block at 0x82B70EC8 → 0x82B710BC, our C++ LoaderTick cap ends at iter 101 with r3=0) remains the best working approach.
- The post-dispatch body (0x82B70EF8..0x82B710BC, 0x82B70EF4 onwards after SkipRendererDispatch) is a separate code path with its own entity loops using different globals (`dword_830BE400+0x1C..+0x24` — engine sub-entities, NOT the 60KB block) — its calls do not hit the terminator vtable. Whether the post-dispatch body itself can complete natural execution (or hangs at one of its own sub_xxx callees identified earlier: `sub_82B237B0`, `sub_82B67D98`, `sub_82BDC040`, `sub_82B676B8`, `sub_82AFF120`, `sub_82B0A0D0`, `sub_82B6FF78`, `sub_823EDD40`, `sub_82B1F410`, engine->vt[3]) is still an OPEN question — the Path 1 experiment with SkipRendererDispatch showed `SkipRendererDispatch #1` fired but `#2` never did, suggesting the post-dispatch body itself hung on first pass too. That bisection is deferred.

### Asset loading guest code path investigation — sub_8253CF08, AssetDB, BXML format (2026-07-31)

User wants a fully native host renderer — no `rexgpu-xenosd.dll`. Investigation moved to understanding how the guest builds its scene/asset data so we can do it host-side instead. Started RE-ing the AssetDB init chain called by EngineInit.

**`sub_8253CF08` (AssetDB getter)** at 0x8253CF08: standard lazy-init alloc pattern — `if (!dword_830577C0) { sub_82AB73C0(111104); sub_8253CB38(result); } return dword_830577C0;`
  - Allocates 108KB for AssetDB struct, calls constructor `sub_8253CB38` (= thunk to `sub_82BAC228` installs vtable `off_82145434`, then `sub_82AB73C0(12896)` for inner heap, then continues init)

**AssetDB vtable `off_8204C08C`** (decoded big-endian):
- `vt[0] = sub_825378A0`
- `vt[1] = vt[2] = nullsub_1` (0x82426FF0)
- `vt[3] = sub_825341E0` ← EngineInit calls this immediately after AssetDB init (returns `"MXGame" + screen 1280x720`)
- `vt[4] = sub_8253C2B0`, `vt[5] = sub_82537148`, `vt[6] = sub_8253AA40` (LoaderTick's gating call), `vt[7] = sub_82537290`
- Strings just past vt entries: "Game", "Real_World", "Destroy RealWorld Done", "MP_CUSTOM_EVENT", "MP_PLAYLIST_EVENT", "SS_EVENT", "SP_EVENT"

**`Engine.bxml` does NOT live inside AssetDB init** — it is parsed inside `SetupRenderer` (the `Engine.bxml` literal is only xref'd at 0x82ba8000 - EngineInit passes it as second arg to `bl sub_82B71148` SetupRenderer). SetupRenderer's middle band (0x82B71338..0x82B7168C, skipped by mid-ASM hook #5) is where Engine.bxml gets parsed. Hook #5's skipped band reads many registry refs by string name visible in IDA disasm: `"Database\\"`, `"EngineDependencies"`, `"Game"`, `"VSync"`, `"30HZ"/"60HZ"`, `"Renderer\\Profile"`, `"FPS"`, `"DeltaSecondsClamp"`, etc.

### Asset directory layout

`Engine.bxml` (198-byte XML) is the root manifest listing 3 child registries:
- `MXRegistry.bxml` (13KB decompressed) — main game config tree (tuning parameters, AI settings, audio refs, etc.)
- `RendererRegistry.bxml` (1KB decompressed) — renderer config (windowed/displax dimensions, debug flags, FPS profile)
- `UIRegistry.bxml` (2KB decompressed) — UI config + 30 UISound bindings

`/Database/` directory contains ~200 pairs of `*.xenon.database` (small BXML-format metadata) + `*.xenon.package` (large asset binary bundle) files. Track packages (e.g., `NAT_Farm.xenon.package` = 69MB) contain geometry/texture/audio; vehicle packages (`MX.xenon.package` = 3MB, `HR_MX_graphickit.xenon.package` = 115MB) contain rider/vehicle geometry. Total asset volume well over 1GB.

User has the *decoded* XML form alongside many BXML files (e.g., `Engine.bxml.xml`, `MXRegistry.bxml.xml`, `RendererRegistry.bxml.xml`, `UIRegistry.bxml.xml`, `Cameras/MX_Camera.bxml.xml`, `EffectsTable.bxml.xml`, `CausesTable.bxml.xml`, `KeyBind_Core.bxml.xml`, `RdbTables.bxml.xml`). User does NOT have decoded XML for `.xenon.database` files — those need to be parsed by our own decoder.

### BXML format structure (reverse-engineered 2026-07-31)

Files use a custom **tokenized binary XML format** (not raw compressed XML). Tested against 9 reference pairs (8 in `assets/` plus one in `assets/Cameras/`).

**File container (36 bytes header + zlib):**
```
offset  size  field                  sample / role
------  ----  --------------------- ----------------------------------------
0x00    4     magic                  "BXML"
0x04    4     version/flags          0x000103EA = 66538 (same on all files)
0x08    4     string_count           = number of strings in strings table
0x0C    4     strings_section_size   = byte size of strings section in
                                       decompressed payload (verified:
                                       matches strings_end offset exactly)
0x10    4     streaming_flag         0 (most files), 48 (RendererRegistry)
0x14    4     aux_count              attr-bearing blocks / attr triplets —
                                       seems to count "Stream A length / 6"
                                       but not perfectly consistent
0x18    4     node_count             = number of XML elements in file ✓
                                       Verified on every single reference:
                                       Engine=4, Rdb=6, RendReg=22, UI=34
                                       all match XML element count exactly
0x1C    4     zero                   always 0
0x20    4     compressed_size        = zlib stream length ✓
0x24..      zlib-deflate raw stream (signature 78 9C...)
```

**Decompressed payload** has two regions:
1. **Strings section** (exactly `strings_section_size` bytes): null-terminated ASCII strings concatenated (tag names, attribute names, attribute values, text content). Order is non-obvious (not strictly DFS) but each string offsets uniquely into the table.
2. **Token stream** (immediately after strings section, runs to end): u32 LE values.

**Token stream** has two sub-streams separated by `0xFFFFFFFF` sentinel:

- **Stream A (attributes)** — front half. Pattern per attribute entry (3 tokens): `[name_idx, value_idx, 0x00010000]` where `0x00010000` is the END_MARKER. Verified on RdbTables (10 triples = 5 `<Table>` × 2 attrs) and UIRegistry (56 triples = 28 `<UISound>` × 2 attrs).

- **Stream B (element tree)** — back half. Recursive sentinel-delimited encoding. Each leaf-with-text is `[element_name, text_idx, SENTINEL]` (smallest 3-token record); self-closing leaf is `[element_name, SENTINEL]`; parent node is `[element_name, ...children..., SENTINEL]`. Tokens between element-name and SENTINEL include small numeric counts/markers that sometimes coincide with low string indices (str[0] is ambiguous with count=0 end marker) — making them ambiguous without context. Compact ratio ≈ 7-8 tree tokens per XML node (verified: UIRegistry 270 tree tokens / 34 nodes ≈ 7.94, Engine 30 tree tokens / 4 nodes = 7.5, RdbTables 46 tree tokens / 6 nodes ≈ 7.67).

**Guest implementation:** `sub_82B64400` (0x82B64400, 9204 bytes, 390 basic blocks) is the universal BXML tree builder. Reads the 36-byte file header via vt[8] (PPC code byteswaps LE→BE on read since the on-disk format is LE). Allocates string table (a1[2], sized `4 * string_count`), child table (a1[3]), string offset table (a1[5]). Reads `node_count` 12-byte node record entries (`[size_u32, data_ptr_u32, type_code_u32]`), byteswaps each; for type_code low u16 == 1, data_ptr is raw, else data_ptr is translated to `a1[5] + data_ptr` (string-table offset). Iterates records with the first switch dispatcher that byteswaps each node's typed data payload by stride (case 3 = i32 stride 4, case 6 = u64 stride 8, case 7 = vec4 stride 16, case 8 = m4x4 stride 64, case 0xA = vec3 stride 12) — this layer is for binary-data BXMLs (`.xenon.database`), not config-only `.bxml` files.

### Tools placed at `tools/`

| File | Role |
|---|---|
| `tools/bxml_decoder.py` | Single-call zlib-decompress + strings + u32 token walk dump (`--raw` for comprehensive dump, also supports directory batch). |
| `tools/bxml_strings.py` | Strings-only summarizer; categorizes strings into `ENTITY/SHADER/MATERIAL/ANIM/TEXTURE/META/OTHER`. Useful for understanding what asset references each `.xenon.database` contains without full XML reconstruction. |
| `tools/decode_bxml.py` | Minimal zlib-decompress; outputs raw decompressed bytes. |
| `tools/dump_bxml.py` | Hexdump of decompressed bytes (no parsing). |
| `tools/README.md` | Documents all `tools/` scripts + BXML format status + what works/what doesn't. |
| `tools/extract_xex_pe.py` | (From prior work) XEX PE extractor. |
| `tools/lzx_decompress.py` | (From prior work) LZX decompression for XEX container. |

### BXML format reconstruction status (as of 2026-07-31)

**FULLY CRACKED** — full XML tree reconstruction working for ALL BXML files
(both config `.bxml` AND binary-data `.xenon.database` files).

Verified working:
- Container: 36-byte file header + zlib raw deflate ✓
- File header (all u32 LE): magic, version, string_count, strings_section_size, streaming_flag, aux_count, node_count, zero, compressed_size ✓
- Post-strings layout: `streaming_flag bytes binary data` + `aux_count × 12-byte attr descriptors` + `node_count × 32-byte node records`
  - Verified formula: `post_strings_size = streaming_flag + 12 * aux_count + 32 * node_count` on ALL files ✓
- Strings section (null-terminated ASCII, exactly strings_section_size bytes) ✓

**Attr descriptor format** (12 bytes each: `[name_str_idx(u32), value(u32), type_code(u32)]`):
- `type_code` high u16 = data type (1=string, 3/4/5=i32, 6/9=u64, 7/0xC=vec4, 8=m4x4, 0xA=vec3, 0xB=bool)
- `type_code` low u16 = flag (0 = value is string index, 1 = value is byte offset into binary data section)
- For flag=0: attribute value = `strings[value]`
- For flag=1: attribute value = typed read from `binary_data[value : value+stride]` where stride depends on data type (4 for i32/bool, 8 for u64, 12 for vec3, 16 for vec4, 64 for m4x4) ✓

**32-byte node record** (8 u32 LE fields):
  - field[0] = element_name string index
  - field[1] = text_content string index (-1 = no text)
  - field[2] = reserved (always 0)
  - field[3] = has_text flag (0 = no text, 1 = has text)
  - field[4] = total_node_count (validation field, same for all nodes in file)
  - field[5] = child_count (number of child elements)
  - field[6] = attr_start index (offset into the flat attr descriptor list)
  - field[7] = attr_count (number of attribute pairs for this node)

**Tree traversal**: depth-first pre-order with explicit child_count.

**Verified on ALL file types**:
- Engine.bxml (4 nodes, config) ✓
- RdbTables.bxml (6 nodes, 10 attrs, config) ✓
- UIRegistry.bxml (34 nodes, 56 attrs, config) ✓
- RendererRegistry.bxml (22 nodes, streaming=48) ✓
- EngineDependencies.xenon.database (304 nodes, 605 attrs, 1212B binary) ✓ — full shader/material/texture dependency tree
- RiderPhysics.xenon.database (14 nodes, 25 attrs, 52B binary) ✓ — Package/Block/Heap/Compress with integer offsets
- NAT_Farm.xenon.database (5247 nodes, 10494 attrs, 20992B binary) ✓ — full track asset manifest (entities, animations, sound banks, materials, textures, scripts)
- AIData.xenon.database (79 nodes, 155 attrs, 312B binary) ✓

Tools:
| File | Role |
|---|---|
| `tools/bxml_full_decoder.py` | **PRIMARY TOOL** — full XML reconstruction from ANY `.bxml` or `.xenon.database` file. Verified on all file types. |
| `tools/bxml_strings.py` | Strings-only categorizer: `ENTITY/SHADER/MATERIAL/ANIM/TEXTURE/META/OTHER`. |
| `tools/bxml_decoder.py` | Raw internals inspector: strings + u32 token walk dump (`--raw` mode). |
| `tools/decode_bxml.py` | Minimal zlib decompressor (raw bytes only). |
| `tools/dump_bxml.py` | Hexdump of decompressed bytes. |
| `tools/README.md` | Full format documentation + tool guide. |

Guest implementation: `sub_82B64400` (0x82B64400, 9204 bytes, 390 basic blocks) is the universal BXML tree builder. Reads 36-byte file header via vt[8], byteswaps PPC↔LE, allocates string table (a1[2]) + child table (a1[3]), reads `node_count` 12-byte node-descriptor records, dispatches type-code byteswap fixups by data stride (case 3=i32 stride 4, case 7=vec4 stride 16, case 8=m4x4 stride 64, case 0xA=vec3 stride 12), then reads 32-byte render descriptors and dispatches structure type codes (cases 1-12). This layer handles binary-data `.xenon.database` files; config `.bxml` files use simple string-index references decoded by our Python tool directly.

### Eng+8 vs transition-renderer+8 — critical distinction
- **`dword_830BE400` (global)** stores the engine HEAP pointer (Bootstrap-allocated 80-byte object at e.g. 0x400EA4E0). `dword_830BE400 + 8` (MainLoop's read) = `*(engine_heap_obj + 8)` = engine heap **slot 2**. NULL per Bootstrap; workaround sets it to the engine pointer itself so the vt[36] call lands on Bootstrap's nullsub_1 no-op (the correct no-op handler for the bootstrap state — entities populate via a different path, not via this vtable call).
- **`unk_830EC248` (global)** is the transition renderer (545KB struct located at static data address 0x830EC248, NOT heap). `unk_830EC248 + 8 = dword_830EC250` (transition renderer's slot+8). EngineInit populates this slot with AssetDB at 0x82ba7fe4. LoaderTick's `(a1+8)->vtable[6]()` reads this slot → AssetDB vt[6] = `sub_8253AA40` (real function).
- SetupRenderer's `a1` is `unk_830EC248` (transition renderer), NOT the engine. Its body accesses `*(r31+8)` (= transition renderer + 8 = AssetDB) at 0x82B713f8, 0x82B71438, 0x82B71520, 0x82B71540 — these reads succeed because EngineInit populates the slot before SetupRenderer is called.
- MainLoop's `eng+8` (slot 2 of engine heap object) has NO legitimate writer in the binary — leaving it NULL and using the Bootstrap self-ref workaround appears to be the only viable path (matches the no-op vt[36] handler Bootstrap exposes).

### Eng+8 writer — investigation status
- Bootstrap (`sub_82ABB838` → `sub_82ABB3C8`) confirmed to NEVER write slot 2 (byte +8). Settles slots 12/13/14/19 to 0, installs vtable 0x82139C44 at +0, caches eng in `dword_830B08C0`. Leaves slot 2 at whatever the heap allocator returned (= 0).
- `sub_82ABB3A0` (Bootstrap's slot-15 subobject init) only writes the XVIDEO_MODE subobject at slots 60–72 (display-mode default `{1, 640, 480, 329060}`), doesn't touch slot 2.
- SetupRenderer's middle band (skipped by mid-ASM hook #5) READS `eng+8` as a vtable dispatcher at 0x82B713f8, 0x82B71438, 0x82B71520, 0x82B71540 — but never WRITES it. The skipped band assumes `eng+8` is already populated by the time it runs.
- **Investigated the 9 pre-hook-#2 helpers in SetupRenderer body** (`sub_82B64228`, `sub_82BFBF38`, `sub_82B668D0`, `sub_82B60678`, `sub_82B67590`, `sub_82B601E8`, `sub_82B601A8`, `sub_82BDF080`, `sub_82BFBCD0`) + SetupRenderer's first-line `eng->vt[19] = sub_82311410`. None writes `*(eng_slot_2)`. `sub_82B64228` writes `a1[2] = 0` for its own **stack-local** registry object (a1 = `r1+var_4B0`, NOT eng). SetupRenderer's `a1` is `unk_830EC248` (transition renderer), so the `lwz r3, 8(r31)` reads in SetupRenderer access **transition_renderer+8 = `dword_830EC250`** (AssetDB, populated by EngineInit), NOT `dword_830BE400 + 8` (engine slot 2). The AGENTS.md "SetupRenderer reads eng+8" note was wrong mid-investigation — SetupRenderer reads transition_renderer+8.
- **Conclusion**: MainLoop's `*(dword_830BE400 + 8)` actually IS NULL legitimately — Bootstrap never populates engine's slot 2 because the no-op `vt[36] = nullsub_1` at Bootstrap vtable 0x82139C44 IS the intended handler for the bootstrap state. The game-tick semantics here are "do nothing further at Bootstrap engine level"; real per-frame entity population lives in the LoaderTick renderer block (mid-ASM hook #6 skipped it; see "Blocked" above). Investigating "eng+8 writer" further is futile — there's no legitimate writer because the slot is supposed to remain NULL (matching the no-op vt[36] handler). Self-ref workaround `eng+8 = eng` is the correct fix.

### AssetDB_LoadStateMachine — 12-case load orchestration (RE'd 2026-07-31)

`AssetDB_LoadStateMachine` = `sub_8253AA40` @ 0x8253AA40 (4204 bytes, 139 blocks, 12-case switch).
This is **LoaderTick's gating call** (AssetDB vt[6], called from `unk_830EC248+8 → AssetDB → vt[6]()`).

**KEY FINDING**: The state machine is **pure orchestration** — it does NOT load file content itself. It polls flags set by a separate background **LoadingThread** (synchronized via `LoadingThreadEvent` / `LoadingThreadEarlyOutEvent` events created by AssetDB's constructor `AssetDB_Ctor_LoadingThreadEvents` @ 0x8253CB38).

| State | Addr | Name | Role |
|-------|------|------|------|
| 0 | 0x8253ABFC | Init | `sub_82304158` → `sub_82466FA0` allocs 96B, registers content-package handlers |
| 1 | 0x8253AC8C | WaitEvent | `Wait(a1+110800, 1)` then `SceneTransition_Kickoff` (kickoff #1) |
| 2 | 0x8253AD3C | IdleClearRenderBusy | `*(a1+110328) = 0` (clears render-busy flag) |
| 3 | 0x8253B00C | DatabaseLoad | Reads "PlayerMode" registry; iterates "PlayerLocal"/"Loaded"/"Ready" flags; fires "Loading Progress" |
| 4 | 0x8253B2A8 | SubsceneCreate | `eng+8->vt[16]("Subscene Creation", sub_825372C0, ...)` |
| 5 | 0x8253B3E8 | LoadingProgress | Polls 3 ratios: `0.25*sub_822FEE90 + 0.5*sub_82534160 + 0.25*sub_822FD550` |
| 6 | 0x8253B504 | PlayerSetup | Per-player UniqueId assignment; "NetworkNoPlayers" check |
| 7 | 0x8253B6D8 | AsyncStart | `AsyncStartFlag_Set(*(a1+29812))` — just sets `*(*(AsyncStart)+596) = 1` |
| 8 | 0x8253B738 | AsyncWait | `AsyncStartFlag_Poll(*(a1+29812))` — just reads `*(*(AsyncStart)+596)` |
| 9 | 0x8253AD4C | LaunchActivity | `sub_82B09DA0(eng+32, 1)` + `SceneTransition_Kickoff` (kickoff #2) |
| 10 | 0x8253ADF0 | SeriesAdvance | Fires "NeedSeriesAdvanceEvent" / "MXSeriesManager::AdvanceEvent" |
| 11 | 0x8253B754 | Finalize | `sub_82537C68(a1)` then transition to state 2 |

**States 7/8 are NOT loaders** — they flip/poll a single status flag on the AsyncStart object owned by AssetDB at `*(a1+29812)`. The actual decompression work runs on the background LoadingThread referenced by the "LoadingThreadEvent" / "LoadingThreadEarlyOutEvent" sync events.

### Asset loader infrastructure (RE'd 2026-07-31)

#### Renamed functions (locked in via IDA)

| Address | Name | Role |
|---------|------|------|
| 0x8253AA40 | `AssetDB_LoadStateMachine` | LoaderTick's gate (vt[6]); 12-state machine above |
| 0x8253CB38 | `AssetDB_Ctor_LoadingThreadEvents` | Alloc 108KB AssetDB + inner 12896B heap, creates the 2 LoadingThread sync events |
| 0x8253CF08 | `AssetDB_GetInstance` (lazy-init getter) | lazy-init alloc + ctor — cached in `dword_830577C0` |
| 0x82BAC228 | inner AssetDB ctor (installs vtable `off_82145434`, then `sub_82AB73C0(12896)` for inner heap) | called from `AssetDB_Ctor_LoadingThreadEvents` |
| 0x82BAB700 | `AssetDB_InnerCtor_VtableInstall` | Installs vtable `off_8214518C`; called by SetupRenderer at 0x82B712EC (skipped by mid-ASM hook #5) |
| 0x82BAB128 | `AssetDB_InnerCtor_PackageRegistryInit` | Calls `DatabaseAndPackageIndexLoader` — builds in-memory package index |
| 0x82BAA650 | **`DatabaseAndPackageIndexLoader`** | Opens `.xenon.database`, parses XML, builds per-package descriptor w/ heap offsets + codec IDs (see below) |
| 0x82B67128 | **`AssetFile_Open`** | Generic BXML file open dispatcher (22 callers — every config-type loader) |
| 0x82B64400 | `BxmlTreeBuilder_Parse` | Universal BXML parser (9204B, 390 blocks, called by 30+ per-type walkers) |
| 0x82B2C300 | `BxmlFileLoader_OpenByName` | Opens `.bxml` config by name via SceneManager (`%s.bxml` format) |
| 0x82B71148 | `SetupRenderer` | Already known; confirmed caller of `AssetDB_InnerCtor_VtableInstall` at 0x82B712EC |
| 0x82304158 | (State 0's content-package event registration helper) | Registers "ContentPackageMounted/Unmounted/Corrupt/LicenseChange" callbacks |
| 0x824661A8 | `ContentPackage_MountDLC` | Mounts XContent packages (DLC); fires `ContentPackageMounted` |
| 0x82465FD0 | `ContentPackage_IndexFilename` | Per-file: indexes presence in registry (does NOT load content) |
| 0x82533C00 | `AssetDirWalker_Init` | Initializes dir walker with extension list split by `;` |
| 0x825338B0 | `AssetDirWalker_Iterate` | FindFirstFile/FindNextFile loop, callbacks per match |
| 0x82538618 | **`SceneTransition_Kickoff`** | Called from states 1/6/9/11 — flips InRealWorld/InUIWorld; dispatches on dword_830BE190 (the Path 1 frog) |
| 0x82534348 | `EngineSubsystem_RenderState_Reset` | Zeros per-entity render state (+44/+20/+268/+244) |
| 0x8234EFD8 | `AsyncStartFlag_Set` | `*(AsyncStart+596) = 1` (State 7) |
| 0x8234EFE8 | `AsyncStartFlag_Poll` | `return *(AsyncStart+596)` (State 8) |
| 0x82536DE0 | `AssetDB_Dtor_ReleaseSubsystemSingletons` | Releases AsyncStart, singletons |
| 0x82AC0A78 | `BxmlLoader_ShaderTemplateRegister` | Walker example: parses `<Type>`, `<Name>`, `<Index>` attrs |

#### DatabaseAndPackageIndexLoader — what it builds (`sub_82BAA650`)

Called once from `AssetDB_InnerCtor_PackageRegistryInit` (which is invoked from SetupRenderer at 0x82B712EC, **inside the band skipped by mid-ASM hook #5**).

1. Builds path `"<dir><name>.xenon.database"` via string format+concat
2. Opens the .xenon.database file via `AssetFile_Open` (`sub_82B67128`)
3. Parses XML tree using BXML walker API (`sub_82B64228` init → `sub_82B667F8` find-elem → `sub_82B668D0` iterate children → `sub_82B601E8` next sibling → `sub_82B64328` fini)
4. Counts Handles, Packages, walks `<Package>` subtree
5. Per package parses: `name`, `file` (the .xenon.package path), per-`<Heap>` `{offset, size, blockOffset, heapOffset}`, `<Compress>{enabled, codec}`
6. **Codec enum** resolved via string compare (`sub_82BDFA10`):
   - "SPUZlib" → 0
   - "LZX" → 1
   - "Zlib" → 2
7. Allocates runtime index: `4 * (40 * package_count + 7 * heap_count + handle_count + 88)` bytes
8. Structured with CriticalSection locks for thread-safe lookup

This **exactly matches** the heap layout we already decode via `bxml_full_decoder.py` — the runtime index that this function builds at runtime is what our tool can produce offline.

#### AssetFile_Open — only handles BXML (`sub_82B67128`)

Inspection of all 22 caller sites shows **every caller opens a named `.bxml` config file** (UIRegistry, Localization, Audio/ConfigFiles/*, etc.) — NOT actual binary asset content. Package binary extraction (mesh/texture/audio bytes) follows a different path — the heap itself is a BXML block decoded in-place by `BxmlTreeBuilder_Parse` when the loader iterates it, NOT via `AssetFile_Open`.

### .xenon.package format (RE'd 2026-07-31)

**Two sibling files** per asset collection:
- `<Name>.xenon.database` — small BXML metadata describing the package layout
- `<Name>.xenon.package` — large binary blob bundle holding actual asset data

**Package file = concatenation of heaps** (sized per `.xenon.database` `<Heap>` entries):

```
Each heap (sized by .database Heap/size attr):
  offset  size  field                  notes
  ------  ----  ---------------------  ----------------------------------------
  0x00    4     heap_size_m1 (u32 LE)  = (heap_size - 4)
  0x04    4     0x00010000             (flag/version — same on all heaps)
  0x08    4     heap_size_m16 (u32 LE) = (heap_size - 16)
  0x0C    4     tail-checksum u32      (heap_size | ~heap_size pattern)
  0x10    17    per-resource descriptor format varies by asset TYPE (see below)
  0x21..        resource payload:
                  - BXML heaps: 36-byte BXML header + zlib stream (offset 33 = 0x21 absolute inside heap)
                  - Binary heaps (mesh/texture/etc): raw binary in per-TYPE format
```

**Verified on RiderPhysics.xenon.package** (4062B = 2 heaps @ 1563B + 2499B — both BXML heaps decode cleanly to readable physics XML).

**Verified on NAT_Farm.xenon.package** (66MB, 1049 heaps from .database):
- Heaps 7+ (off=71678 etc.) contain BXML @ offset 33, zlib @ offset 69 — these are celib/surface/timeline/etc XML heaps that decode cleanly
- Heaps 1-6 (Default_bank00 soundbnk, NAT_Farm_DriveOn soundbnk, National script, NAT_PODIUM_Rider animset) have **NO BXML** — they're raw-compression binary heaps (LZX codec per .database `<Compress>`)

### Per-asset-type header bytes [16..33] (sampled from NAT_Farm)

Each asset heap has a 17-byte descriptor at offset 16 (= 0x10) inside its heap, distinct per asset type. Sampled first bytes (HEX):

| Type | First descriptor bytes [0x10..0x21] | Notes |
|------|--------------------------------------|-------|
| bxml | varies (looks like encrypted hash) | only 2 instances |
| celib | `d1 00 30 10 2c 37 00 00 00 7c 00 00 00 10 00 00 00` | starts with `0xXX 00 30 10` |
| soundbnk | `16 07 20 b7 37 91 0d 00 d0 00 00 14 62 00 56 00 00` | starts with `0xXX 07 20 b7` or `0xXX 00 10 51` |
| script | `e8 07 20 b7 0f 95 0d 00 c0 00 00 24 61 00 56 00 00` | nearly same prefix as soundbnk (sibling format) |
| animset | `3c 05 20 3d a6 91 4c 00 63 22 00 50 24 00 00 00 00` | |
| anim | `f4 00 30 40 2e 30 01 00 00 05 00 00 00 2a 00 00 00` | starts with `0xXX 00 30 40/30 70/10 42/30 70` |
| surface | varies | |
| model | `95 fd 66 12 87 02 6b b9 29 40 88 11 85 0e b4 bd 35` | non-magic prefix |
| texture | varies | raw DXT bytes likely |
| material | varies | |
| shader | varies | |
| collis | varies | |

These descriptors are per-TYPE schema headers — each requires its own parser. The 17-byte prefix possibly encodes version + flags + sub-struct size.

### Asset catalog (2026-07-31) — `out/asset_catalog.json`

Generated unified asset catalog by scanning **all 130 `.xenon.database` files** in `assets/Database/` with the (newly-made iterative) `bxml_full_decoder.py`:

- **Total assets cataloged: 23,183**
- **Per-asset entry**: `{name, type, package_offset, heap_size}` — enough to byte-address any asset in any package

**Global asset type histogram**:

| Type | Count | Role |
|------|-------|------|
| texture | 7611 | DXT-compressed GPU textures |
| material | 4730 | Shader parameter bindings |
| shader | 2481 | Compiled Xbox 360 GPU microcode (Xenos shader ISA) |
| model | 1833 | Skeletal meshes (vertices+indices) |
| surface | 1661 | Shader surface bindings |
| anim | 1595 | Skeletal animation keyframes |
| collis | 545 | Collision hulls |
| swfx | 350 | Software FX |
| script | 328 | Game logic scripts |
| uicmpnt | 298 | UI components |
| bxml | 231 | Pure XML configs (decode ✓ via bxml_full_decoder) |
| soundbnk/sound/sounddat | 613 | Audio banks (FMOD/XMA) |
| animset/timeline | 273 | Animation blend trees |
| celib | 116 | Cause+effect libraries (decode ✓ — see NAT_Farm heaps 7-13) |
| localiz | 70 | Localization strings |
| activity | 18 | Game activities |
| bink | 11 | Bink video files |
| particle/forest/water/grass/etc | ~50 | Various FX |

### Implications & strategic next steps

1. **Mid-ASM hook #5 is the gatekeeper** — it skips the `AssetDB_InnerCtor_VtableInstall` call site in SetupRenderer @ 0x82B712EC, which means `DatabaseAndPackageIndexLoader` never runs at boot under our profile. Options to populate the runtime package index:
   - Disable hook #5 entirely (risky — SetupRenderer's other init code was earlier tagged "skip crash points")
   - Pre-call `DatabaseAndPackageIndexLoader` from a C++ hook in `EngineInit` (Path-1-style pre-population)
   - **Skip the guest loader entirely** — use `bxml_full_decoder.py` to build an equivalent host-side index offline, then hook `AssetFile_Open` to redirect reads to host-side decoded content

2. **`bxml_full_decoder.py` produces the same data** that `DatabaseAndPackageIndexLoader` would build at runtime — package names + per-heap `{offset, size, codec}` tuples. We can produce a host-side index that's a 1:1 match of the guest's runtime index layout.

3. **Per-TYPE heap parsers needed next** — `bxml`, `celib`, `surface` are decoded (XML-bearing). Need to write per-TYPE parsers for `texture` (DXT), `model` (Xenos vertex/index buffer), `shader` (Xenos microcode), `anim`, `soundbnk` (XMA), `bink` (Bink container). The per-TYPE header descriptor at heap offset 16-32 identifies type Dispatcher.

### Updated `tools/` directory

| File | Role |
|---|---|
| `tools/bxml_full_decoder.py` | **PRIMARY TOOL** — full XML reconstruction from ANY `.bxml` or `.xenon.database` file. Iterative DFS (handles 23K-asset trees). Verified on all 130 databases. |
| `tools/package_decoder.py` | **NEW** — extracts and decodes EVERY heap inside a `.xenon.package` file (cross-references sibling `.xenon.database` for heap layout). XML heaps produce full trees; binary heaps get hexdump + element histogram. |
| `tools/bxml_strings.py` | Strings-only categorizer: `ENTITY/SHADER/MATERIAL/ANIM/TEXTURE/META/OTHER`. |
| `tools/bxml_decoder.py` | Raw internals inspector: strings + u32 token walk dump (`--raw` mode). |
| `tools/decode_bxml.py` | Minimal zlib decompressor (raw bytes only). |
| `tools/dump_bxml.py` | Hexdump of decompressed bytes. |
| `tools/README.md` | Full format documentation + tool guide. |
| `tools/extract_xex_pe.py` | (prior work) XEX PE extractor. |
| `tools/lzx_decompress.py` | (prior work) LZX decompression for XEX container. |

### Generated artifacts in `out/`

| File | Content |
|------|---------|
| `out/asset_catalog.json` | Master 130-database × 23183-asset index (name, type, package_offset, heap_size) |
| `out/NAT_Farm_database.xml` | 6301-line XML reconstruction of the 5247-node NAT_Farm asset manifest |
| `out/NAT_Farm_package_manifest.txt` | Per-heap decode summary for the 66MB NAT_Farm.xenon.package (1049 heaps) |

### IDA bookmarks installed (slot 0-14)

| Slot | Address | Label |
|------|---------|-------|
| 0 | 0x8253ABFC | State0_Init (calls sub_82304158/alloc96B) |
| 1 | 0x8253AC8C | State1_WaitEvent_NtSetEventPlusSceneTransition |
| 2 | 0x8253AD3C | State2_IdleClearRenderBusy |
| 3 | 0x8253B00C | State3_DatabaseLoad_PlayerModeCheck_Loaded_Ready_Iteration |
| 4 | 0x8253B2A8 | State4_SubsceneCreate_eng+8 vt[16] registry |
| 5 | 0x8253B3E8 | State5_LoadingProgress_3subsystemRatioWait |
| 6 | 0x8253B504 | State6_PlayerSetup_UniqueId_NetworkNoPlayers |
| 7 | 0x8253B6D8 | State7_AsyncStart sub_8234EFD8 |
| 8 | 0x8253B738 | State8_AsyncWait sub_8234EFE8 |
| 9 | 0x8253AD4C | State9_LaunchActivity sub_82B09DA0 + scene transition |
| 10 | 0x8253ADF0 | State10_SeriesAdvance NeedSeriesAdvanceEvent |
| 11 | 0x8253B754 | State11_Finalize sub_82537C68 -> transition to State2 |
| 12 | 0x82538618 | SceneTransition_Kickoff: InRealWorld/InUIWorld flag flips + dword_830BE190 vt[17] + vt[27] + vt[10] |
| 13 | 0x82BAA650 | DatabaseAndPackageIndexLoader: parses .xenon.database XML, builds package index, supports SPUZlib/LZX/Zlib codecs |
| 14 | 0x82B67128 | AssetFile_Open: generic file open dispatcher (23 callers - all asset file types) |
| 15 | 0x82646D58 | SSM_StateCompiler_Dispatch: bundled Xbox 360 XDK shader state compiler (xdk-main-sep10) |
| 16 | 0x82AD0378 | ShaderAsset_Unpack: reads v7(4B ignored flag) + v8(4B sub-count) then per-sub dispatches type 0/1 |

### Shader microcode load path (RE'd 2026-07-31)

The binary contains the full **Microsoft Xbox 360 Shader Assembler** (bundled XDK XDK `xdk-main-sep10`):
- assertion strings reveal source paths like `e:\xenon\xdk-main-sep10\core\private\xtl\graphics\xgraphics\ucode\ssm\statecompiler\ssmstatecompiler.cpp`
- `xltconvert.cpp` is the XLT (Xbox-to-host shader) translator

#### Shader asset heap structure (binary heap, no BXML inside)

`.shader` heaps contain raw compiled Xenos microcode (NOT parsed as BXML). The guest unpacker `ShaderAsset_Unpack` (`sub_82AD0378` @ 0x82AD0378) reads sequentially:

1. 4 bytes: `v7` — version/flag (ignored; both VS-counter at +1108 and PS-counter at +2192 zeroed unconditionally)
2. 4 bytes: `v8` — sub-resource count `N`
3. For each i in 0..N:
   - 4 bytes: `sub_type` (0=VertexShader, 1=PixelShader)
   - Dispatches to `ShaderAsset_LoadVertexStage` (`sub_82AC0BA8`) or `ShaderAsset_LoadPixelStage` (`sub_82AC0D20`)

Each LoadStage handler reads:
- 4 bytes: `stage_desc_size` (variable; fits in 72-byte stride slot at `a1+28+72*N` for VS or `a1+1112+72*N` for PS)
- `stage_desc_size` bytes: vertex/pixel stage descriptor (DESCRIPTOR format TBD — Xenos shader stage header)
- 4 bytes: `microcode_dword_count` `M`
- (separate alloc via `sub_82AB73C0(4*((M>>2)+1))`): raw Xenos microcode (`4*M` bytes)

#### Runtime shader registration (effect system)

`ShaderAsset_Open` (`sub_82ADDE78` @ 0x82ADDE78) verifies the asset's BXML header:
1. Opens the heap as BXML stream
2. Parses tree via `BxmlTreeBuilder_Parse`
3. Walks `<ShaderHeader Version="1"/>` — if Version=1, dispatches to `ShaderAsset_Unpack` for the binary blob AFTER the BXML portion

Effect techniques (higher-level file, NOT the .shader heap) reference shaders by asset NAME via XML:
```
<Pass>
  <Shaders>
    <VertexShader asset="VS_Basic" entryPoint="main"/>
    <PixelShader  asset="PS_Basic" entryPoint="main"/>
  </Shaders>
</Pass>
```

`Effect_PassBindShaders` (`sub_82AECCB0` @ 0x82AECCB0) reads those XML bindings, resolves asset name `eng+8->vt[30](name, ...)` → returns asset object with `*(asset+16) = stage_descriptor_ptr`. Offset+88 is a lock flag (set 1 during use, restored 0 after).

Lookup is by entryPoint name (string compare via `sub_82BDFA10`):
- `ShaderManager_LookupVS_ByEntryPoint` (`sub_82AC0E98`): linear scan of VS array at `asset+28`, stride 72, count `*(asset+1108)`
- `ShaderManager_LookupPS_ByEntryPoint` (`sub_82AC0F68`): linear scan of PS array at `asset+1112`, stride 72, count `*(asset+2192)`

On match: calls lock/addref via `sub_82556D68(*(slot+92_VS) or *(slot+1176_PS))`, returns stage descriptor pointer.

#### XDK-bundled shader state compiler

`SSM_StateCompiler_Dispatch` (`sub_82646D58` @ 0x82646D58): bundled MS XDK source-shader-microcode state compiler. Calls `sub_82705888(pStateCompiler, pState, shader, ...)` to dispatch microcode. This is the runtime microcode→GPU dispatch.

`XLT_Translator_Ctor` (`sub_826FFCE0` @ 0x826FFCE0): builds an "translator" object (Xbox-to-D3D) with callbacks:
- `AllocateSysMem` callback (slot 1)
- `FreeSysMem` callback (slot 2)
- `registry` (slot 3)
- `texServer` (slot 4)
- `shaderStore` (slot 5)

XLT translator path: `e:\xenon\xdk-main-sep10\core\private\xtl\graphics\xgraphics\ucode\ssm\translator\xltconvert.cpp`

#### Big-picture implications for native renderer

1. **The guest binary contains 2 separate shader paths**:
   - **Source-assembler** path (Microsoft Xbox 360 Shader Assembler 2.0.20209.0) — takes high-level shader source, produces Xenos microcode (used only at authoring time, but binary embedded)
   - **Runtime microcode-dispatch** path (SSM State Compiler + XLT Translator) — loads compiled Xenos microcode from `.shader` heaps, dispatches to GPU via `D3DVertexShader::SetShader`-equivalent
2. **`.shader` heaps store raw Xenos microcode** (NOT source). Converting to D3D12 requires:
   - Extracting Xenos microcode bytes from each heap (after 33-byte heap header, sequential 4-byte/cb reads)
   - Disassembling back to Xbox 360 shader ISA (or use Xenia's recompiler logic)
   - Compiling to DXBC/SPIR-V for D3D12
3. **Effect technique (.fx-equivalent) XMLs** live in `.surface`/`.material` heaps (NOT `.shader` heaps). The .shader heaps are just the compiled microcode; techniques referencing them live elsewhere.
4. **Most likely blocker**: many `.shader` heaps in the package files are LZX-compressed (per `<Compress enabled="true" codec="LZX"/>` in the .database). Decoding them requires implementing/hosting an LZX decompressor. Heaps below the compression threshold (small ones, ≤1KB?) are stored uncompressed and have standard heap headers visible directly.

### .xenon.package encryption discovery (2026-07-31)

After attempting LZX decompression on non-BXML binary heaps with our existing `tools/lzx_decompress.py` (XEX2 LZX decoder), the decoder produced nonsensical repeating-byte output. Investigation revealed a different reality than the codec label suggested:

**The `<Compress codec="LZX" enabled="true">` element in `.xenon.database` is uniform across all heaps of all 130 databases** — every asset regardless of type has it set identically. Therefore `codec="LZX"` is NOT a per-heap compression declaration; it appears to be a build-pipeline label that's not actually consumed at runtime (or applies at Package Block level, not Heap level).

**Actual heap storage modes** (sampled across ATV_shock, NAT_Farm packages):

| Mode | Header pattern | Notes |
|------|----------------|-------|
| **Structured (cleartext)** | `H[0..3]=heap_size-4 (LE), H[4..7]=0x00010000, H[8..11]=heap_size-16 (LE), H[12..15]=checksum` | BXML-bearing types (celib/bxml/surface/material/script/timeline) AND some binary types (anim, soundbnk small ones, animset, colorlut). The 17-byte descriptor at [16..33] follows the per-TYPE patterns noted earlier. Payload@33 is either plain BXML block (with embedded zlib stream) or uncompressed native binary. |
| **Encrypted** | All 16 bytes [0..15] look high-entropy random | Most binary types: model, material, shader, surface (sometimes), texture, tdf, particle, packdata, bounce, collis, activity, bxml-IAmVeryHeavyPhy. The 33-byte heap header is unreadable; payload@33 is also high-entropy. |

**What works vs. blocked**:

| Asset category | Storage | Extractable today? |
|----------------|---------|---------------------|
| `bxml` config files | plaintext BXML (zlib-compressed internally) | YES via `bxml_full_decoder.py` |
| `celib` (cause+effect libraries) | BXML cleartext | YES via `package_decoder.py` |
| `surface` (shader surface bindings) | some heaps structured (BXML-bearing), some encrypted | PARTIAL — varies per heap |
| `material` (shader parameter bindings) | mostly encrypted | NO (encrypted) |
| `script` (game logic) | mostly structured (BXML-bearing cleartext) | YES |
| `timeline` | structured (BXML-bearing) | YES |
| `animset` | structured (cleartext binary) | partially — heap header readable, payload won't decode without per-TYPE parser |
| `anim` (skeletal animations) | mostly structured (cleartext) | partially — needs per-TYPE binary parser |
| `soundbnk` / `sound` | some structured (small ones cleartext), some encrypted | PARTIAL — varies per heap |
| `texture` | mostly encrypted | NO |
| `model` (vertex/index buffer geometry) | mostly encrypted | NO |
| `shader` (compiled Xenos microcode) | mostly encrypted | NO |
| `collis` (collision hulls) | encrypted | NO |
| `tdf` / `particle` / `packdata` / `bounce` | encrypted | NO |
| `colorlut` | structured (cleartext) | partially — needs per-TYPE binary parser |

**Implication for native D3D12 renderer**:

Game-critical assets for visible rendering — `model` (geometry), `texture` (DXT pixel data), `shader` (Xenos microcode), `material` (shader bindings) — are stored **encrypted** in the on-disk `.xenon.package` files. The original game decrypts them at runtime using a key embedded in the Xbox 360 XEX binary's security header. We need to:

1. Find the decryption key (likely embedded in the XEX2 header blocks we extracted as `default.xex` from `default.xex` PE wrapper)
2. Reverse the per-heap decryption: figure out the cipher (likely AES-128 in CBC mode with a heap-offset-derived IV, given Xbox 360's content security model)
3. Either:
   - Decrypt heaps host-side and re-write `package_decoder.py` to output cleartext heaps, OR
   - Pre-decrypt all package files once offline and cache the decrypted versions

**Conclusion of LZX investigation**: the LZX decompressor experiment was misleading. The genuinely blocking issue is heap encryption, not compression. Our existing `tools/lzx_decompress.py` works correctly for XEX2 LZX (used in the XEX binary itself) but is not applicable to `.xenon.package` heaps. Future work to extract binary assets requires XEX2 security-header analysis to find the per-package AES key, then implementing the decryption layer.

### OpenSSL crypto bundle in guest binary (2026-07-31) — TLS-only, not asset decryption

**Found indexed AES primitives in the guest binary.** Searching IDA for the standard AES inverse S-box pattern (`52 09 6A D5 30 36 A5 38 BF 40 A3 9E 81 F3 D7 FB`) returns a hit at **`0x8211fd70`**. Reading 512 bytes there reveals:

| Data @ addr | Contents |
|---|---|
| 0x8211fd70 | 256-byte AES **inverse S-box** (used for decryption) |
| 0x8211fe70 | 10-word AES **round constants** (`01,02,04,08,10,20,40,80,1B,36` as LE u32) |
| 0x8211fe98 | String `"RC2 part of OpenSSL 1.0.0 29 Mar 2010"` |
| 0x8211ffa8 | Forward/alternate S-box variant (likely OpenSSL T-table-derived) |

Following xrefs to the inverse S-box + rcon yielded the full **OpenSSL AES stack** statically linked into the guest:

| Address | Role | Source string evidence |
|---|---|---|
| 0x82A71958 | `AES_set_decrypt_key` (1492B, FIPS rcon dispatch) | — |
| 0x82A721D8 | `AES_encrypt` (block, 1492B, T-tables) | — |
| 0x82A727A8 | `AES_decrypt` (block, 1468B, inverse S-box) | — |
| 0x82A8ECE8 | `AES_cbc_decrypt` body (XOR prev + dispatch, takes AES_decrypt via fn ptr) | — |
| 0x82A8EB40 | `AES_cbc_encrypt` body (mirror of above) | — |
| 0x82A6EF80 | EVP dispatch: `a6? sub_82A8EB40(enc) : sub_82A8ECE8(dec)` | — |
| 0x82A4AFC8 | `EVP_CipherUpdate` wrapper (>1 GB blocks via 0x40000000 chunking) | — |
| 0x82A4B138 | `EVP_CipherInit_ex` stub (called via data ptr @ 0x821bee90) | — |
| 0x82A1B350 | `ssl_get_cipher_by_name` (loads AES-128-CBC etc. into `dword_830AF694...`) | `ssl_ciph.c` |
| 0x82A93150 | `CMS_decrypt` (SOURCE switch 21/22/23/25/26 → wraps sub_82A7F5F8) | **`cms_lib.c`** |
| 0x82A7F5F8 | CMS_EnvelopedData decrypt (iterates RecipientInfos) | **`cms_env.c`** |
| 0x82A7F4D8 | CMS_KEKRI decrypt (recipient type 2, AES_set_decrypt_key on KEK) | `cms_env.c:656` |
| 0x82A7F370 | CMS_KTRI decrypt (recipient type 0, RSA-based key transport) | `cms_env.c:315` |
| 0x821bee00 + 0x821bee90 | EVP_CIPHER tables — function_ptr + FIPS-integrity-hash 8-byte anchor pairs | `e_aes.c` |
| 0x821bfbc0+ | OpenSSL **FIPS binding table** — (ptr, hash) for every bound symbol: `AES_set_decrypt_key`, `AES_cbc_decrypt`, `AES_encrypt`, `AES_decrypt`, ... | — |

**Critical**: **the OpenSSL bundle is `_projects\MXUsers\JonP\HttpClient\HttpClient\Source\openssl-1.0.0`**, NOT asset code. Every CMS / EVP / EVP_CIPHER access reveals strings like:
- `C:\_projects\MXUsers\JonP\HttpClient\HttpClient\Source\openssl-1.0.0\crypto\cms\cms_env.c`
- `C:\_projects\MXUsers\JonP\HttpClient\HttpClient\Source\openssl-1.0.0\crypto\evp\e_aes.c`
- `C:\_projects\MXUsers\JonP\HttpClient\HttpClient\Source\openssl-1.0.0\ssl\ssl_ciph.c`

Strings at `http://xlsp-mxalive/xenon/*` (MOTD, Playlist, OnlineStore, DataMiner, GameUpdater) confirm the HttpClient is for **Live/TLS service traffic** (DLC, leaderboards, store) — NOT asset decryption.

So the OpenSSL AES functions visible in the binary are TLS-only. They are **not** the function that decrypts on-disk `.xenon.package` heaps.

### Heap encryption validated via entropy analysis

Sampled 1500 heaps across 6 packages (ATV_shock, NAT_Farm, HR_MX_graphickit, RiderPhysics, AIData, EngineDependencies). Classified by header byte at offset [4..8]:

- **"Structured" header** — `H1 (bytes 4..8) == 0x00 00 01 00` magic — 304 of 1500 heaps
- **"Non-structured"** — high-entropy first bytes — 1196 of 1500 heaps

(Of 304 structured, most are BXML-bearing or soundbnk/colorlut/script/celib/bxml plaintext heaps — those decode cleanly.)

Entropy (Shannon 8-bit) distribution per asset type:

| Package | Type | Structured count | Structured avg entropy | Raw count | Raw avg entropy |
|---|---|---|---|---|---|
| ATV_shock | material | 2 | 7.80 | 6 | 7.68 |
| ATV_shock | model | 8 | 7.73 | 42 | 7.61 |
| ATV_shock | shader | 2 | 7.65 | 12 | 7.66 |
| ATV_shock | surface | 7 | 7.59 | 41 | 7.64 |
| ATV_shock | texture | 1 | 7.93 | 9 | 7.61 |
| **ATV_shock** | **all** | | **7.6-7.9** | | **7.6-7.9** ← consistent → **NOT encrypted** |
| NAT_Farm | anim | 3 | 7.28 | 76 | **7.98** |
| NAT_Farm | animset | 1 | 7.15 | 6 | **7.97** |
| NAT_Farm | celib | 6 | 7.47 | 2 | **7.99** |
| NAT_Farm | collis | 0 | — | 34 | **7.93** |
| NAT_Farm | material | 0 | — | 163 | **7.96** |
| NAT_Farm | model | 0 | — | 72 | **7.99** |
| NAT_Farm | particle | 0 | — | 1 | **7.996** |
| NAT_Farm | bxml | 0 | — | 2 | **7.996** |
| NAT_Farm | shave | 0 | — | 33 | **7.86** |
| NAT_Farm | texture | 0 | — | 196 | **7.59** |
| **NAT_Farm** | **all** | | **7.15-7.47** | | **7.93-7.99** ← **definitely ENCRYPTED** (max possible = 8.0) |
| HR_MX_graphickit | shader | 41 | 7.96 | 80 | **7.97** |
| HR_MX_graphickit | texture | 45 | 7.96 | 318 | **7.86** |
| **HR_MX_graphickit** | **all** | | **7.96-7.99** | | **7.86-7.97** | ← encrypted |

**Conclusion**:

1. **Small packages (ATV_shock)** ship unencrypted — structured-and-raw heaps share similar entropy (7.6-7.9) since they're both plaintext-per-type binary data.
2. **Large packages (NAT_Farm 66MB, HR_MX_graphickit 115MB)** encrypt most non-BXML heaps with **entropy ≈7.98-7.996 bits/byte** (close to ideal random/AES output). Per-package/per-heap encryption keys; not a global XOR.
3. The encryption is NOT a per-heap fixed-XOR key (the `H1` magic bytes are NOT consistent across encrypted heaps — distinct across all examined encrypted heaps). Implies per-heap IV (CBC) or per-heap nonce (CTR).
4. The decryption routine is NOT the visible OpenSSL AES (that's TLS-only via HttpClient). It's either:
   - A custom AES/XOR routine elsewhere in the binary (not yet found — would require scanning for other 16-byte keys + cipher constants, or trace `NtReadFile`/file-read wrappers that mutate returned buffers)
   - Or it might run entirely in the XBOX 360 OS kernel (NT-calls) outside the guest binary's reach on real hardware
   - Or there's a small embedded micro-cipher in the guest binary that we haven't isolated yet (likely — game entertainment software generally doesn't trust OS-level crypto)

### Where to look next for the asset decrypt routine (next session)

| Angle | Approach |
|---|---|
| Layered NtReadFile wrapper | Look for code wrapping `NtReadFile`/`NtCreateFile`; the OS-level filter decrypts synchronously on every read, returning plaintext buffers. Trace via calls to `sub_82B67128` (AssetFile_Open) and inspect the layer between open and use. |
| Per-asset-type loader init | Each asset type has its own `*_Asset_Open` (e.g., `ShaderAsset_Open` 0x82ADDE78). Inject a probe that prints the input heap bytes to confirm whether we receive CIPHERTEXT or PLAINTEXT — definitively settles the "where does decryption happen" question. |
| Non-OpenSSL AES search | The visible OpenSSL AES uses S-box tables at `0x8211fd70`. Search the binary for the OTHER AES variant — a T-table implementation (pre-computed values like `0xC6 0x63 0x63 0x63 ...` or `0xA5 0x63 0x63 0x63 ...` T0/T1/T2/T3). A guest-side asset decrypt routine likely uses a different cipher (RC4 / XTEA / a custom XOR / LCG). |
| File-read pattern via `RtlAllocateHeap`-released decrypted buffer | Search for `RtlAllocateHeap` followed by `memcpy(buf_in, decrypted_buf)` — common pattern for in-process decrypt-then-copy-out. Less specific but feasible starting point. |
| Game-tested playbook | The most pragmatic approach: launch the game with verbose hooks, then when the game is quietly pre-loading shaders, dump the heap buffers seen by `ShaderAsset_Unpack` to confirm whether they're ciphertext or plaintext — establishing the known-plaintext attack surface. |

### IDA bookmarks installed (slot 17-23, AES routines)

| Slot | Address | Label |
|---|---|---|
| 17 | 0x82A71958 | `AES_set_decrypt_key` (e_aes.c — internal, called via EVP init + CMS KEKRI) |
| 18 | 0x82A727A8 | `AES_decrypt` (block, 1468B, inverse-Sbox @0x8211fd70) |
| 19 | 0x82A721D8 | `AES_encrypt` (block, referenced via OpenSSL FIPS table @0x821bfbc8) |
| 20 | 0x82A8ECE8 | `AES_cbc_decrypt` (EVP callback body, derives from AES_decrypt fn ptr) |
| 21 | 0x82A8EB40 | `AES_cbc_encrypt` (mirror, uses AES_encrypt fn ptr) |
| 22 | 0x82A93150 | `CMS_decrypt` (cms_lib.c — switch on content type 21/22/23/25/26) |
| 23 | 0x8211fd70 | AES inverse S-box + rcon + `"RC2 part of OpenSSL 1.0.0 29 Mar 2010"` string |

### Path 2 — GPU plugin shim design (2026-07-31)

**Goal**: Replace the fatal `off_8213F70C` vtable dispatches inside `sub_82B34998` (the LoaderTick renderer block at 0x82B70EF4) with no-op stubs, so the natural entity/render code can execute past the original fatal-exit terminators. Path 2 builds on top of Path 1 (the pre-population cascade), which still needs to be enabled for sub_82B34998 to ever run.

#### `sub_82B34998` call graph (post-RE)

```c
int RendererDispatchBlock(int a1, ..., float a22)
{
  if (!dword_830BE190)
    dword_830BE190 = off_82D5648C();        // [Path 1] lazy-init — hangs in Transition thread
  if (!TerminatorTlsGate(a1))               // [Path 2] *(a1+61104) == TLS slot check
    return sub_82B36298(a1, 2, ...);        // failure-path fallback (likely safe)
  sub_82B33EC0(a1, a22);                    // entity update iteration (per-entity math, safe)
  (*(*a1 + 32))(a1, a22);                   // vt[8] = sub_82B2C4C8 (entity slot dispatcher)
                                           //   internally dispatches vt[15]/vt[16] (BOTH terminators)
  NullDerefDispatch(a1, a22);              // *(a1+1288) → sub_82B2D030(NULL) crashes
  (*(*a1 + 28))(a1, a22);                   // vt[7] = sub_82BDB190 fatal terminator
  return sub_82B2C498(a1);                  // returns a1 (no-op if *(a1+68)==0)
}
```

#### Vtable `off_8213F70C` slot classification (post-RE)

| Slot | Offset | Address | Role | Shim treatment |
|------|---------|---------|------|----------------|
| 0  | 0  | 0x82B3C828 | function (unused by dispatch path) | keep as-is |
| 1  | 4  | 0x82B38830 | function (unused by dispatch path) | keep as-is |
| 2  | 8  | 0x82B2C270 | function (unused by dispatch path) | keep as-is |
| 3  | 12 | **0x82BDB190** | **terminator** | → nullsub_1 |
| 4  | 16 | 0x82B35FE0 | function (unused by dispatch path) | keep as-is |
| 5  | 20 | 0x82B2C2E8 | function (unused by dispatch path) | keep as-is |
| 6  | 24 | **0x82BDB190** | **terminator** | → nullsub_1 |
| 7  | 28 | **0x82BDB190** | **terminator** (called by dispatch path) | → nullsub_1 |
| 8  | 32 | 0x82B2C4C8 | entity slot iterator (dispatches vt[15]/vt[16]) | keep as-is |
| 9  | 36 | 0x82426FF0 | nullsub_1 already | keep as-is |
| 10 | 40 | 0x82B38A48 | function (unused by dispatch path) | keep as-is |
| 11 | 44 | 0x82B38CC0 | function (unused by dispatch path) | keep as-is |
| 12 | 48 | 0x82B38D90 | function (unused by dispatch path) | keep as-is |
| 13 | 52 | 0x82B2C738 | function (unused by dispatch path) | keep as-is |
| 14 | 56 | **0x82BDB190** | **terminator** | → nullsub_1 |
| 15 | 60 | **0x82BDB190** | **terminator** (dispatched by vt[8] body) | → nullsub_1 |
| 16 | 64 | **0x82BDB190** | **terminator** (dispatched by vt[8] body) | → nullsub_1 |
| 17 | 68 | **0x82BDB190** | **terminator** | → nullsub_1 |
| 18 | 72 | **0x82BDB190** | **terminator** | → nullsub_1 |
| 19 | 76 | **0x82BDB190** | **terminator** | → nullsub_1 |

9 of 20 slots are fatal (`vt[3,6,7,14-19]`); the dispatch path reaches only 3 (`vt[7,15,16]`), but all 9 are replaced defensively.

#### Shim hooks installed in `src/native_graphics.cpp`

Three REX_FUNC hooks added under the `// GPU renderer shim (Path 2)` section. **Inert in baseline** (mid-ASM hook #6 skips sub_82B34998 entirely so these never fire until Path 1 is enabled):

| Hook | Guest addr | Role |
|------|-----------|------|
| `sub_82B2C9D0` | 0x82B2C9D0 | TLS gate bypass — always return 1 (truthy), so the success path runs |
| `sub_82B307D8` | 0x82B307D8 | NULL-deref bypass — no-op return instead of `sub_82B2D030(*(a1+1288))` crash |
| `sub_82B38558` | 0x82B38558 | Constructor post-hook — after orig installs `off_8213F70C`, allocate 80-byte guest heap, copy vtable, replace 9 fatal slots with `nullsub_1` (0x82426FF0), and overwrite `*a1` with the custom vtable pointer |

The vtable allocation uses `REX_CALL_INDIRECT_FUNC(0x82AB73C0)` (sub_82AB73C0, the same heap allocator EngineInit/SetupRenderer use). The replaced slots point to `nullsub_1` (0x82426FF0, 4-byte `blr` no-op).

#### Enablement recipe (next iteration)

To activate the shim with Path 1 cascade:

1. Edit `mx_config.toml`:
   - Comment out the `NativeSkipLoaderRenderer` mid-ASM hook (#6) — lines ~1641-1646
   - Move `NativeSkipRendererInit` (#5) fire-address from `0x82B71324` → `0x82B71314`
   - Comment out `NativeSkipLoaderAll` (#8) — lines ~1651-1654
2. Edit `src/native_graphics.cpp`:
   - In the `sub_82B71148` (SetupRenderer) hook, after `orig_SetupRenderer` returns, restore registers r0/r3-r12/lr/ctr around a `REX_CALL_INDIRECT_FUNC(0x82B3C7D0)` to pre-populate `dword_830BE190` from main-thread context (avoids the lazy-init Transition-thread hang)
   - In the `sub_82BFB740` (Wait) hook, always return SUCCESS
   - Add a hook for `sub_82B70370` (timing) that no-op returns (avoids the busy-wait spin)
3. `rexglue codegen --force mx_manifest.toml` (~75s)
4. `cmake --build out/build/win-amd64-debug --target mx`
5. Copy `mx.exe` to root, run, watch log for `shim vtable installed @0x...` then `LoaderTick #N r3=...`

If `sub_82B34998` returns cleanly, expect LoaderTick to progress to the post-dispatch body (0x82B70EF8..0x82B710BC). The Path 1 experiment showed this body hangs on its first pass — see AGENTS.md post-dispatch body table (sub_82B237B0, sub_82B67D98, sub_82BDC040, sub_82B676B8, sub_82AFF120, sub_82B0A0D0, sub_82B6FF78, sub_823EDD40, sub_82B1F410, engine->vt[3]). Each will likely need its own stub — iterate per the bisect recipe at the end of the "PATH 1 EXPERIMENT" section.

#### IDA bookmarks added (slot 24-27, shim-related)

| Slot | Address | Label |
|------|---------|-------|
| 24 | 0x82B34998 | `RendererDispatchBlock` — sub_82B34998 (the dispatch LoaderTick calls) |
| 25 | 0x82B38558 | `TerminatorVtableCtor` — installs off_8213F70C vtable at *a1; host hook overrides |
| 26 | 0x82B2C9D0 | `TerminatorTlsGate` — *(a1+61104) == TLS slot; host hook returns 1 |
| 27 | 0x82B307D8 | `NullDerefDispatch` — sub_82B2D030(*(a1+1288)) NULL crash; host hook no-op |

---

## `eng+8` writer traced (2026-07-31, session 3)

### Discovery via plugin-mode triangulation

Added `LogEngSlot8(base, where)` helper in `native_graphics.cpp` and called it at every plugin-mode hook point (Bootstrap, EngineInit, SetupRenderer ENTER/RETURNED, GraphicsInit ENTER/RETURNED, PostGfxInit, TexManager, BindTexture, LazyInit, VtableCtor). Captured this chronological trace during a 30s plugin run:

```
[Bootstrap ENTER]    eng+8 = 0x00000000 (eng=0x00000000)
[Bootstrap RETURNED] eng+8 = 0x00000000 (eng=0x00000000)   ← Bootstrap doesn't write it
[SetupRenderer ENTER]eng+8 = 0x00000000 (eng=0x400EA4E0)   ← Engine exists, slot empty
[GraphicsInit ENTER] eng+8 = 0x00000000 (eng=0x400EA4E0)
[PostGfxInit ENTER]  eng+8 = 0x00000000
[GraphicsInit RETURNED]eng+8 = 0x00000000
[BindTexture]        eng+8 = 0x00000000  ← Last NULL sighting (at 0x82B712B0)
[LazyInit ENTER]     eng+8 = 0x40BCF740  ← First NON-NULL sighting (at 0x82B71330)
[SetupRenderer RETURNED]eng+8 = 0x40BCF740  ✓
```

The eng+8 write happens between BindTexture (0x82B712B0) and LazyInit (0x82B71330). Disassembly of that SetupRenderer band:
- 0x82B712B4–0x82B712C0: 2nd TexManager + 2nd BindTexture
- **0x82B712C4–0x82B712D4: `eng->vt[8](eng)`** (skipped by mid-ASM hook #3 `NativeSkipVtable8`)
- 0x82B712D8–0x82B712F8: `sub_82AB73C0(0x85280)` alloc 545KB + `AssetDB_InnerCtor_VtableInstall` constructor (installs vtable `off_8214518C`)
- **0x82B71304–0x82B71310: `eng->vt[17](eng, assetdb_block)`** (skipped by mid-ASM hook #4 `NativeSkipVtable17`)
- 0x82B71314–0x82B71330: test `dword_830BE190`, then LazyInit (`sub_82B3C7D0`)

### Writer function: `sub_82B43AC8` (vt[17])

Decompiled:

```c
int sub_82B43AC8(int a1, int a2)            // a1 = engine, a2 = 545KB AssetDB block
{
  *(a1 + 8) = a2;                            // ← THE CRITICAL WRITE: eng+8 = AssetDB
  v3 = sub_82B43A48();                        // lazy-init returns dword_830BE430 (dispatcher singleton)
  return (*(*v3 + 4))(v3, *(a1 + 8));         // dispatcher->vt[1](dispatcher, AssetDB)
}
```

The dispatcher singleton `dword_830BE430` is constructed by `sub_82526BF8` (called from `sub_82B43A48` lazy-init):
- `sub_82AB73C0(4)` — allocate 4-byte block
- Install vtable `off_82049B8C` at `*block`
- Returns block as singleton

### Dispatcher vt[1]: `sub_82526D10` — AssetDB subsystem registration

The dispatcher `off_82049B8C`'s slot 1 (at offset 4) = `sub_82526D10`. This is a **massive** function — calls ~18 subsystem-manager singletons, gets each manager's interface via `mgr->vt[2](mgr)`, then registers each interface with the AssetDB via `assetdb->vt[27](assetdb, interface, ...)`.

Pattern (repeating for each subsystem):
```c
v3 = sub_82BABD08();                          // global registry
v4 = sub_8237CF88(v3);                         // manager A singleton (e.g., Texture)
v6 = (*(*v4 + 8))(v4);                         // manager->vt[2] — get interface
v8 = (*(*a2 + 108))(a2, v7, v6);               // assetdb->vt[27] — register interface
```

The 18 subsystem manager lookups (in order):
| # | Manager getter (sub_xxx) | Likely subsystem |
|---|--------------------------|------------------|
| 1 | `sub_8237CF88` | ? |
| 2 | `sub_823F9DA8` | ? |
| 3 | `sub_823FADC8` | ? |
| 4 | `sub_823F54C0` | ? |
| 5 | `sub_82470B48` | ? |
| 6 | `sub_82470A68` | ? |
| 7 | `sub_8229C408` | ? |
| 8 | `sub_824A82E0` | ? |
| 9 | `sub_8237D070` | ? |
| 10 | `sub_822EB3A8` | ? |
| 11 | `sub_8234C0E0` | ? |
| 12 | `sub_8242D650` | ? |
| 13 | `sub_823A03E0` | ? |
| 14 | `sub_82331370` | ? |
| 15 | `sub_82520960` | ? |
| 16 | `sub_824444F0` | ? |
| 17 | `sub_82444C00` | ? |
| 18 | `sub_82526C90` | ? |

### Implications for native backend

**Discovery**: Native mode skips BOTH:
- The 545KB AssetDB block alloc (at 0x82B712D8) — runs (no hook between TexManager/BindTexture and the alloc)
- The `AssetDB_InnerCtor_VtableInstall` constructor (at 0x82B712EC) — runs (between hooks #3 and #4)
- BUT `vt[17]` at 0x82B71310 is **skipped by mid-ASM hook #4** — so:
  - `eng+8` is never written (stays NULL — confirmed bug)
  - `sub_82526D10` (18-subsystem AssetDB registration) never runs — AssetDB has no subsystem handlers

**Two-part fix needed**:
1. **Restore `eng+8` write** — either:
   - Disable mid-ASM hook #4 (let `sub_82B43AC8` run naturally), OR
   - Replicate `*(eng+8) = assetdb_block` from C++ in SetupRenderer hook (need to recover the 545KB block ptr)
2. **Restore AssetDB subsystem registration** — either:
   - Let `sub_82526D10` run (via option 1 above), OR
   - Stub each `assetdb->vt[27]` registration call to no-op (risky — 18 subsystems)

**Recommended next step**: **Disable mid-ASM hook #4 only** (keep #2, #3, #5-8 active). Re-codegen, run native mode, observe whether `sub_82B43AC8` + `sub_82526D10` complete without crash. The 18 subsystem-manager lookups depend on registry state populated earlier — may need additional stubs if they crash.

**Alternative**: Write a C++ shim that, after `orig_SetupRenderer` returns:
1. Re-allocates the 545KB block via `sub_82AB73C0(0x85280)` and calls `AssetDB_InnerCtor_VtableInstall` (no-ops if already done — SetupRenderer ran that path)
2. Reads the block ptr from a known location (or re-allocates and tracks it ourselves)
3. Does `REX_STORE_U32(eng + 8, block_ptr)` directly
4. Skips `sub_82526D10` (the subsystem registration) — accept that assets won't load but the engine won't crash on the missing slot

The second alternative is safer for incremental progress — gets eng+8 populated without risking 18-subsystem slowdowns/crashes.

### IDA bookmark

| Slot | Address | Label |
|---|---|---|
| 28 | 0x82B43AC8 | `EngineSlot8_WriteAssetDB` — eng->vt[17] writes `*(eng+8) = AssetDB_ptr`; SKIPPED by mid-ASM hook #4 |

### Native mode baseline restored (2026-07-31, session 3 followup)

After confirming vt[17] is the eng+8 writer, **disabled mid-ASM hook #4** (NativeSkipVtable17) in `mx_config.toml` so vt[17] runs naturally. All other mid-ASM hooks (#2 SetupDeviceSkip, #3 SkipVtable8, #5 SkipRendererInit, #6 SkipLoaderRenderer, #7 SkipLoaderEarly, #8 SkipLoaderAll) restored and active. Hook #1 (NativeGameTickSkip) remains disabled.

Verified via native-mode run (no plugin):

```
native: SetupRenderer ENTER (0x82B71148)
native: SetupRenderer RETURNED
native: eng+8 already populated (0x40BCF720)        ← vt[17] ran naturally; populated!
native: LoaderTick #1 r3=1
native: LoaderTick #2 r3=1
native: LoaderTick #3 r3=1
native: LoaderTick #4 r3=1
native: LoaderTick #5 r3=1
native: LoaderTick #101 r3=0                       ← cap reached cleanly
native: VdSwap #1 wrote 64704 bytes at guest 0xBEB9057C
native: RenderPipeline #1 — orig returned
native: MainLoop #1                                 ← MainLoop iterating!
native: RenderPipeline #2 — calling orig
native: EndFrame #2 — calling orig (fires VdSwap)   ← hangs here, no VdSwap #2
```

**Status**:

- ✅ `eng+8` populated to real AssetDB block in native mode (`0x40BCF720` — note slight address difference vs plugin's `0x40BCF740` due to heap layout variance, but valid block).
- ✅ LoaderTick runs through cap (101 iterations cleanly) — no entity/renderer block hang (still skipped by hooks #6-#8).
- ✅ MainLoop iterates — sleeps 16ms per frame per our hook.
- ✅ First render frame completes: VdSwap #1 fires (64704 bytes of PM4 = 14249 packets = matches plugin's swap #1 size at boot).
- ⚠️ **EndFrame #2 hangs** — second render frame calls `orig_EndFrame` but never returns (VdSwap #2 doesn't fire). Likely orig_EndFrame's internal GPU wait state — our BeginFrame/EndFrame stubs return immediately but orig_EndFrame may spin waiting for the GPU command processor to ack. The first EndFrame works because the very first VdSwap triggers fallback paths; subsequent EndFrames expect accumulated state we don't maintain.
- ❓ Current `eng+8` value (`0x40BCF720`) differs slightly from plugin (`0x40BCF740`) — both are HeapAlloc'd AssetDB blocks. Same vtable was likely installed (off_8214518C).

**The `eng+8 = eng` self-ref workaround in MainLoop hook (frame 1) is now OBSOLETE** — eng+8 is already set by the time MainLoop fires, so we shouldn't override it. Leave the workaround line for safety in case anything regresses, but it's a no-op now since sett呗`eng+8` to itself doesn't break the already-set value.

### Mid-ASM hooks status after this change

| # | Address | Name | Status | Effect |
|---|---------|------|--------|--------|
| 1 | 0x82B70854 | NativeGameTickSkip | Disabled | MainLoop vt[36] crash handled by eng+8 write workaround (now redundant) |
| 2 | 0x82B71290 | NativeSetupDeviceSkip | **Active** | Skip vtable[6] device setup |
| 3 | 0x82B712C4 | NativeSkipVtable8 | **Active** | Skip vtable[8] (4-byte alloc, harmless) |
| 4 | 0x82B71304 | NativeSkipVtable17 | **DISABLED** | Let vt[17] write eng+8 + register subsystems |
| 5 | 0x82B71324 | NativeSkipRendererInit | **Active** | Skip renderer init band -> Transition thread |
| 6 | 0x82B70EC8 | NativeSkipLoaderRenderer | **Active** | Skip LoaderTick renderer block (sub_82B34998) |
| 7 | 0x82B70E18 | NativeSkipLoaderEarly | **Active** | Skip LoaderTick vtable+entities |
| 8 | 0x82B70DFC | NativeSkipLoaderAll | **Active** | Skip LoaderTick everything (rlr21 setup) |

Active hooks: #2, #3, #5, #6, #7, #8 (6 of 8). Disabling #4 is the key change that fixed eng+8.

### Next blocker: EndFrame #2 hang

The first frame completes (VdSwap #1 fires, PM4 captured). The second frame calls `orig_EndFrame` (sub_82ABF930) which internally triggers VdSwap #2 + GPU wait — but never returns. Investigate:

1. **Decompile sub_82ABF930** to find the wait point — likely an internal loop polling a register or event that we stub incorrectly.
2. **Add a sub_82ABF930 diagnostic hook** that probes state before each major call (BeginFrame, render pass loop, EndFrame swap) to see which step never returns.
3. **Fix the EndFrame hang** — likely need to skip the 2nd-Nth EndFrame calls in our hook (same as we do for RenderPipeline: only call orig on first 10 iterations) OR figure out what the 2nd EndFrame needs.

### EndFrame #2 hang — ROOT CAUSE FOUND AND FIXED (2026-08-01)

**Investigation**: Installed REX_FUNC diagnostic hooks for EndFrame's 3 inner calls (`sub_82566B50` / `sub_8255CE98` / `sub_825599A8`) + VdSwap ENTER log. Observed:

Frame #1: all 3 inner calls return → VdSwap #1 ENTER → completes (64704 bytes) → EndFrame returns.
Frame #2: all 3 inner calls return → VdSwap #2 ENTER → **never returns** (no "wrote" log).

**Root cause**: `orig_VdSwap` (`sub_82566B58`, 1668 bytes, 80 blocks) contains a **spin loop** at `0x82567178`:
```c
do {
  result = sub_8255CFE0(...);  // poll GPU frame-pending state
  if (result == 0) break;       // not pending → exit loop
  // check: *(r31+0x4188) - *(r31+0x4190) >= 0xF (15)
  // if < 15, loop back
} while (...);
```

`sub_8255CFE0` returns 1 ("GPU still pending") when a GPU counter at `*(*(r1+256)+88)` hasn't advanced by >= 0x1388 (5000) units since the last frame. Without a GPU, this counter **never advances** → infinite spin loop. Frame #1 skips the loop entirely (bit 29 of `*(r31+0x5E88)` is 0 on first call); frame #2+ enters the loop after frame #1 sets that bit.

**Fix**: REX_FUNC hook on `sub_8255CFE0` that stubs to `ctx.r3.u32 = 0` ("not pending") in native mode. This breaks the spin loop, allowing VdSwap #2+ to complete.

**Result**: Native mode now runs consecutive render frames without hanging. 24 VdSwaps in 7 seconds (every frame fires), RenderPipeline calls orig every frame, MainLoop #421+ at 60fps. `RenderPipeline` and `EndFrame` hooks restored to run every frame (diagnostic caps removed).

### Previous workaround (now obsolete)

The previous approach capped `orig_RenderPipeline` to `rp <= 1` — only the first frame called orig, skipping all subsequent frames to avoid the EndFrame #2 hang. This is no longer needed; every frame now calls orig_RenderPipeline → orig_EndFrame → orig_VdSwap without hanging.

### Native mode baseline (2026-08-01)

- ✅ **EndFrame #2+ hang FIXED** — `sub_8255CFE0` stub breaks GPU frame-pending spin loop
- ✅ **eng+8 populated** — vt[17] runs naturally (hook #4 disabled), sets `*(eng+8) = AssetDB_block`
- ✅ **vt[8] runs naturally** — hook #3 disabled (4-byte alloc + vtable install at eng+0x38, harmless)
- ✅ **60fps MainLoop** — Sleep(16) per frame, RenderPipeline + EndFrame every frame
- ✅ **VdSwap fires every frame** — 24+ swaps in 7 seconds, PM4 captured per swap
- ✅ **Mid-ASM hooks #2,5,6,7,8 active** — #1,3,4 disabled

### Previous notes (pre-fix, kept for reference)

---

## Plugin-Mode Data Capture (2026-07-31, session 2)

### Methodology

Added `g_plugin_mode` passthrough guards to all 23 REX_FUNC hooks in `native_graphics.cpp`. Each guard calls `orig()` then logs entry/exit with key state — capturing exactly what the Xenia-derived GPU plugin (`rexgpu-xenosd.dll`) does at every critical guest function. Additional diagnostic hooks added for: `sub_82B34998` (RendererDispatch), `sub_82B3C7D0` (LazyInit), `sub_82B70370` (Timing), `sub_8253AA40` (AssetDB_LoadStateMachine), `sub_82B38558` (VtableCtor). Required `rexglue codegen --force` to register the 5 new REX_FUNC hooks with the recomp codegen.

### PM4 parser bug FIXED

**Bug**: Our parser extracted Type3 fields as `opcode = (hdr >> 16) & 0x3FFF; count = (hdr & 0xFF) + 1` — wrong bits.

**Fix** (per Xenia's `packet_disassembler.cc`): Xenos Type3 format is:
```
tt cccc cccc cccc cccc ?ooo oooo ??????p
  [31:30] type     = 3
  [29:16] COUNT    = 14-bit, N-1 data words
  [14:8]  OPCODE   = 7-bit
  [0]     predicate = 1 bit
```
New code: `opcode = (hdr >> 8) & 0x7F; body_word_count = ((hdr >> 16) & 0x3FFF) + 1; predicate = hdr & 1`

**Invalid-packet guards**: swapped `opcode==0x3FFF` for `opcode==0x7F`, `body_word_count > 128` for `> 0x4000`.

**Opcode table** rewritten to match Xenia's `xenos.h::Type3Opcode` enum (see `C:\rexglue-sdk\include\rex\graphics\xenos.h` line ~1573). Key o pcodes:

| Opcode | Name | Role |
|--------|------|------|
| 0x10 | NOP | Skip N words |
| 0x21 | REG_RMW | Register read/modify/write |
| 0x22 | DRAW_INDX | Indexed draw |
| 0x23 | VIZ_QUERY | Visibility query begin/end |
| 0x25 | SET_STATE | Fetch state sub-blocks + shader DMAs |
| 0x26 | WAIT_FOR_IDLE | Wait for GPU idle |
| 0x27 | IM_LOAD | Load shader instruction memory (pointer-based) |
| 0x2D | SET_CONSTANT | Load constant into chip (reg-base dispatch) |
| 0x2E | LOAD_CONSTANT_CONTEXT | Load constants from memory |
| 0x2F | LOAD_ALU_CONSTANT | Load ALU constants from memory |
| 0x34 | DRAW_INDX_BIN | Indexed bin draw |
| 0x35 | DRAW_INDX_2_BIN | Inline indexed bin draw |
| 0x36 | DRAW_INDX_2 | Inline indexed draw |
| 0x3B | INVALIDATE_STATE | Invalidate state pointers |
| 0x3C | WAIT_REG_MEM | Wait until register/memory == value |
| 0x3D | MEM_WRITE | Write N words to memory |
| 0x3E | REG_TO_MEM | Read register → memory |
| 0x3F | INDIRECT_BUFFER | Jump to sub-buffer |
| 0x46 | EVENT_WRITE | Generate event |
| 0x54 | INTERRUPT | Generate interrupt |
| 0x56 | SET_SHADER_CONSTANTS | Incremental shader constant update |
| 0x58 | EVENT_WRITE_SHD | VS/PS done event |
| 0x5B | EVENT_WRITE_ZPD | Z-pass done event |
| 0x5E | CONTEXT_UPDATE | Update current context |
| 0x60..0x63 | SET_BIN_MASK_LO/HI, SET_BIN_SELECT_LO/HI | Bin mask/select registers |
| 0x64 | XE_SWAP | **Xenia-specific VdSwap marker** — what our VdSwap hook intercepts |

**Source of truth**: Xenia canary (`https://github.com/xenia-canary/xenia-canary`) branch `canary_experimental`, path `src/xenia/gpu/`:
- `xenos.h` — Type3Opcode enum, register definitions, format enums
- `packet_disassembler.h/.cc` — packet bit fields + action dispatch
- `ucode.h` — shader microcode ISA (ControlFlowOpcode, FetchOpcode, AluScalarOpcode, AluVectorOpcode, vertex/texture fetch instruction layouts)
- `command_processor.cc` — full PM4 dispatch implementation
- `registers.h` — Xenos register file
- `d3d12/command_processor.cc` — D3D12 backend (what the plugin actually does)
- `d3d12/dxbc_shader_translator.cc` — Xenos microcode → DXBC translator (5 files, ~10K lines)

### Captured runtime state (with plugin, gameplay frame #600)

All values dumped from `SetupRenderer RETURNED — dumping state` + `MainLoop #60` / `#600` hooks:

#### Engine state (`dword_830BE400 = 0x400EA4E0`)
| Offset | Value | Role |
|--------|-------|------|
| +0 | 0x82139C44 | Bootstrap vtable |
| +4 | 0x00000002 | counter |
| **+8** | **0x40BCF740** | **AssetDB-equivalent** — NOT NULL in plugin (was NULL in our native) |
| +12 | 0x40B78860 | SceneManager (unchanged) |
| +16 | 0x830EC248 | Transition renderer (self-ptr) |
| +20 | 0x40BB8ED0 | Graphics device handle |
| +24 | 0 | null |
| +28 | 0x4081D7F0 | Render entity list 0 (was NULL at #60, populated by #600) |
| +32 | 0x4082F350 | Render entity list 1 (NULL at #60) |
| +36 | 0x4080D3A0 | Render entity list 2 |
| +40 | 0 | null |

**Critical**: `eng+8 = 0x40BCF740` in plugin — our self-ref workaround (`eng+8=eng`) is wrong. The plugin populates a real object. Need to trace what SetupRenderer's vtable[6/8/17] writes there.

#### Transition renderer (`unk_830EC248`) events
| Offset | Value |
|--------|-------|
| +0x190 | 0xF80001EC (event handle) |
| +0x194 | 0xF80001F4 |
| +0x2DC | 0xF80001D0 |
| +0x2E0 | 0xF80001D4 |

All 4 events are real kernel handles — SetupRenderer's skipped-band code creates them via `NtCreateEvent`. Our native path skips this code (mid-ASM hooks #2-5) leaving them 0; that's why our native LoaderTick can't wait on the events.

#### `dword_830BE190` (60KB renderer block) — **vtable correction**
- Object: `0x40B84F40`
- **Vtable: `0x8213F7A4`** (NOT `0x8213F70C` as in our Path 2 analysis)

Plugin's vtable slots (all real functions, NO `sub_82BDB190` terminators in any slot):

| Slot | Address | Found-by-us-as |
|------|---------|----------------|
| 0  | 0x82B3C918 | (was 0x82B3C828) |
| 1  | 0x82B39910 | (was 0x82B38830) |
| 2  | 0x82B2DF90 | (was 0x82B2C270) |
| 3  | 0x82B36548 | (was 0x82BDB190 — terminator!) |
| 4  | 0x82B36878 | (was 0x82B35FE0) |
| 5  | 0x82B2DF98 | (was 0x82B2C2E8) |
| 6  | 0x82B36880 | (was 0x82BDB190 — terminator!) |
| **7** | **0x82B333E0** | (was 0x82BDB190 — terminator!) |
| 8  | 0x82B2E058 | (was 0x82B2C4C8) |
| 9  | 0x82B36AB0 | (was 0x82426FF0 nullsub_1) |
| 10..13 | real functions | (different from ours) |
| 14 | 0x82B2E170 | (was 0x82BDB190 — terminator!) |
| 15 | 0x82B2E248 | (was 0x82BDB190 — terminator!) |
| 16 | 0x82B2E2B0 | (was 0x82BDB190 — terminator!) |
| 17..19 | real functions | (all were terminators in our analysis) |

**Implication**: Our Path 2 vtable shim (replacing 9 "terminator" slots with `nullsub_1`) is **unnecessary and wrong**. The real vtable is `0x8213F7A4`, and its slots are real functions — likely an audio/FMOD/system dispatch table rather than a render-only one. The `0x8213F70C` vtable we analyzed earlier was a different, fallback instantiation.

#### Entity counts
| Global | #60 | #600 (gameplay) |
|--------|-----|----------------|
| pass0 (0x830C2150) | 0 | 0x00000001 |
| pass1 (0x830C4560) | 0 | 0 |
| pass2 (0x830C6970) | 1 | 0x00000001 |

#### Engine sub-entities (`eng+0x1C..+0x24`) at #600
| Offset | Ptr | vt | +0x3C |
|--------|-----|----|----|
| +0x1C | 0x4081D7F0 | 0x00000001 | 0x00000001 |
| +0x20 | 0x4082F350 | 0x00000000 | 0 (NULL vtable!) |
| +0x24 | 0x4080D3A0 | 0x00000001 | 0x00000001 |

### GpuAlloc layout (30 allocs captured)
First 15 match our native baseline exactly. Plugin does additional allocs for shader/texture caches during gameplay:

| # | Size | Address | Thread |
|---|------|---------|--------|
| 1 | 0x00F00000 | 0xBEDA0000 | main (boot) |
| 2 | 0x00730000 | 0xBE0C0000 | main |
| 3 | 0x00398000 | 0xBDD20000 | main |
| 4-15 | 0x00080000 | 0xBDCA..0xBD72 descending | main |
| 16 | 0x00040000 | 0xFD6DF000 | GPU thread |
| 17 | 0x00000100 | 0xFE7EF000 | GPU thread |
| 18..25 | 0x2000..0x2A | various 0xFD68..0xFE7E | GPU thread (shader + texture caches) |

Plugin uses a different high-address pool (`0xFD..`/`0xFE..`) for runtime caches — that's plugin-internal memory, not guest-visible.

### RendererDispatch / LoadStateMachine — plugin steady-state behavior

Captured over 3000 LoaderTick iterations (gameplay):

- **LoaderTick** returns `r3=1` every iteration — never capped. Plugin processes ~1000 LoaderTicks/45s gameplay.
- **LoadStateMachine** (`sub_8253AA40`) always returns `r3=1`, never advances state (stays in active state — single state at offset 110796 bytes into AssetDB).
- **RendererDispatch** (`sub_82B34998`) called twice per LoaderTick, executes cleanly, returns the `dword_830BE190` object ptr. Never fatal — `off_8213F7A4` vtable dispatches all complete normally.
- **Timing** (`sub_82B70370`) called once per LoaderTick — completes in <1ms.
- **LazyInit** (`sub_82B3C7D0`) called exactly once during SetupRenderer (boot), then never again — block is cached.
- **VtableCtor** (`sub_82B38558`) called once during SetupRenderer, installs vtable `0x8213F7A4`.

### PM4 frame structure (gameplay, frame #600)

`pm4_dump_plugin_600.txt` — 5767 packets:
- Type3: 520 (real commands)
- Type0: 47 (register writes)
- Type2: 5200 (NOPs, padding)

**Opcode histogram (gameplay frame)**:

| Count | Opcode | Role |
|-------|--------|------|
| 1 | DRAW_INDX_2_BIN (0x35) | The one visible draw call — bin/visibility-coded inline index draw |
| 3 | SET_SAMPLER | Texture sampler state (sampler constants) |
| 3 | WAIT_REG_MEM | GPU/CPU sync (wait for register == value) |
| 2 | EVENT_WRITE_SHD | VS/PS done event |
| 1 | SET_BOOL_CONST | Boolean shader constant |
| 1 | SET_ALU_CONST | ALU shader constant (float vec4s) |
| 1 | SET_CONTEXT_REG | Context register write |
| 1 | SET_CONFIG_REG | Config register write |
| 1 | REG_RMW | Register read-modify-write |
| 1 | NOP | |

**Plus**: massive `??? (0x70)` and `??? (0x00)` Type3 packets — opcodes NOT in Xenia's enum. `0x70` carries 16380-word data blocks of `float` vec4 constants (looks like shader constant uploads / vertex data), `0x00` carries similar large blocks. These are likely vendor-extension opcodes Xenia classified as unknown.

**Draw structure**: 1 `DRAW_INDX_2_BIN` per frame. The game uses binning/visibility-query rendering — a separate visibility pass precomputes which bins contribute, then DRAW_INDX_2_BIN executes the visible subset. This explains why our native path sees 0 draws — without populated bin/visibility state, the bin path is skipped.

### Plugin cvar list (130 total, all registered post-plugin-load)

Full dump from `OnPostSetup — dumping all cvars`. Key GPU cvars available via `mx.toml` or CLI `--cvar_name=value`:

**GPU rendering fixes** (per user-discovered settings):

| Cvar | Type | Role | CLI example |
|------|------|------|-------------|
| `gpu_allow_invalid_fetch_constants` | bool | Bypass "invalid texture fetch constant" warnings — fixes missing textures | `--gpu_allow_invalid_fetch_constants=true` |
| `readback_resolve` | string | "full" = full readback resolve (fixes black screen in gameplay at perf cost) | `--readback_resolve=full` |
| `d3d12_readback_resolve` | bool | D3D12-specific readback resolve toggle | `--d3d12_readback_resolve=true` |
| `query_occlusion_fake_sample_count` | int32 | Occlusion query sample count override (-1 = always-fail; fixes brightness) | `--query_occlusion_fake_sample_count=-1` |
| `occlusion_query_enable` | bool | Enable/disable host occlusion queries | `--occlusion_query_enable=true` |

**Other notable GPU cvars**:

| Cvar | Role |
|------|------|
| `vsync` | VSync toggle |
| `resolution_scale` | Resolution scale (1 = 1280×720 native) |
| `draw_resolution_scale_x/y` | Per-axis draw resolution scaling |
| `async_shader_compilation` | Async shader compile (prevents hitches) |
| `half_pixel_offset` | Half-pixel offset for DX9-style rendering |
| `clear_memory_page_state` | Memory page state clearing |
| `gamma_render_target_as_unorm16` | Gamma handling |
| `native_2x_msaa` | Native 2x MSAA |
| `snorm16_render_target_full_range` | snorm16 RT range |
| `mrt_edram_used_range_clamp_to_min` | EDRAM range clamp |
| `direct_host_resolve` | Direct host resolve (skip guest resolve path) |
| `d3d12_bindless` | D3D12 bindless resources |
| `d3d12_tiled_shared_memory` | D3D12 tiled memory |
| `d3d12_dxbc_disasm` | DXBC disassembly output |
| `dump_shaders` | Shader dump path |
| `trace_gpu_prefix` / `trace_gpu_stream` | GPU trace output |
| `swap_post_effect` | Post-processing effect on swap |
| `texture_cache_memory_limit_hard/soft/render_to_texture` | Texture cache limits |
| `anisotropic_override` | Aniso filter override |
| `gpu_3d_to_2d_texture` | 3D→2D texture conversion |
| `non_seamless_cube_map` | Cube map seamless sampling toggle |
| `force_convert_quad_lists_to_triangle_lists` | Quad-list→tri-list conversion |
| `execute_unclipped_draw_vs_on_cpu[_with_scissor]` | CPU VS execution for unclipped draws |
| `use_fuzzy_alpha_epsilon` | Alpha test fuzzy epsilon |

### mx.toml config file

Auto-loaded from next to `mx.exe`. Confirmed log line: `Loaded config: mx.toml`.

Current contents (working configuration for plugin mode with texture/brightness/black-screen fixes):

```toml
gpu_plugin = "xenos"
gpu_allow_invalid_fetch_constants = true
readback_resolve = "full"
occlusion_query_enable = true
query_occlusion_fake_sample_count = -1
vsync = false
resolution_scale = 1
```

### Known plugin gameplay issues

1. **`k_4_4_4_4 signed` texture format unsupported** — plugin emits `[error] [gpu] Unsupported texture formats used in the frame: * k_4_4_4_4 signed` ~1500×/min during gameplay. `gpu_allow_invalid_fetch_constants=true` is supposed to bypass this but does not silence the error (the format itself is unsupported by the D3D12 backend, not just the fetch-constant check).
2. **FATAL: Call to invalid or unregistered function at guest address 0x82327CF0** — crashes during gameplay. Per user, this is a separate known issue not yet debugged.
3. **`Cameras\*.bxml` not found** for vehicle types not in this asset dump (PROTruck, Buggy, UTV, PRO2Truck) — non-fatal, game continues.

### Revised native backend strategy

**Previous strategy (now obsolete)**: Path 1 pre-populate `dword_830BE190` + Path 2 vtable shim replacing 9 "terminator" slots.

**New strategy informed by plugin data**:

1. **`off_8213F7A4` is the real vtable** — no shim needed. Our Path 2 analysis was looking at the wrong vtable (`off_8213F70C`). The real block's vtable has all-real-function slots. Whatever `0x8213F70C` is, it's a different object — possibly a debug/fallback variant.
2. **`eng+8` is legitimately populated** (the plugin puts `0x40BCF740` there). Need to trace what SetupRenderer's vtable[6/8/17] writes — it's not the AssetDB at `tr+8` (which is `0x407F2190` = AssetDB `*dword_830577C0`); it's a different object. Likely the "scene DB" or "render state DB".
3. **Entity lists populate from somewhere other than where we looked**. Plugin shows `eng+0x1C/+0x24` populated with valid vtables by frame 600. Our native path skips the code that populates them (mid-ASM hooks #2-5 + #6-8 skip all of SetupRenderer's and LoaderTick's init bands).
4. **`sub_82B34998` runs cleanly with `off_8213F7A4`** — no fatal terminators. The path that crashed for us was hitting the `off_8213F70C` fallback vtable because the real vtable at `0x8213F7A4` was never installed (we skipped the constructor `sub_82B38558` that installs it via mid-ASM hook skipping).

**Next steps**:

1. **Trace what populates `eng+8`** — set up a guest-memory write watchpoint on `dword_830BE400+8` (the engine's slot 2). Plugin writes `0x40BCF740` early in SetupRenderer. Need to find which SetupRenderer vtable[6/8/17] call installs it and replicate that in native mode.
2. **Re-enable `sub_82B38558` (VtableCtor)** in native mode — let it install `off_8213F7A4` naturally. Remove Path 2 vtable shim. The constructor is called by LazyInit; if LazyInit runs from the main thread (via pre-population in SetupRenderer hook), the constructor installs the correct vtable.
3. **Trace what populates entity lists** (`eng+0x1C/+0x20/+0x24`). These are populated between MainLoop #60 (all NULL/uninitialized) and #600 (all populated). Somewhere in the 540 frames between, the LoadStateMachine or a sub-component of LoaderTick fills them.
4. **PM4 translator rewrite** — current `Pm4Translator` produces 0 draws from 12K-22K packet frames. Once state is correct, draws should appear via DRAW_INDX_2_BIN packets. Translator needs: parse DRAW_INDX_2_BIN's index_count + prim_type, extract inline indices, translate vertex fetch constants, build D3D12 draw.
5. **Texture fetch constant parser** — the `??? (0x70)` Type3 packets carry vec4 shader constants. The big `SET_SAMPLER` and `SET_ALU_CONST` blocks carry sampler + ALU register state. These need full Xenos register name resolution to map to D3D12 root signatures.

### Files this session

| File | Change |
|------|--------|
| `src/pm4_parser.h` | Fixed `Pm4Opcode` enum (Xenia values); fixed field types (`uint8_t opcode`); `OpcodeName(uint32_t)` signature |
| `src/pm4_parser.cpp` | Fixed Type3 bit-field extraction (`opcode=(hdr>>8)&0x7F`, `count=((hdr>>16)&0x3FFF)+1`, `predicate=hdr&1`); updated opcode name table to Xenia's; fixed invalid-packet guards (`opcode==0x7F`, `count>0x4000`) |
| `src/native_graphics.cpp` | Added `g_plugin_mode` passthrough+logging to all 23 REX_FUNC hooks; added 5 new REX_FUNC hooks for `sub_82B34998/sub_82B3C7D0/sub_82B70370/sub_8253AA40/sub_82B38558`; unified prefix variable (`tag` = "plugin"/"native") for log lines |
| `src/mx_app.h` | Added `#include <rex/cvar.h>`; added cvar list dump in `OnPostSetup` |
| `mx.toml` | NEW — auto-loaded config with `gpu_plugin`, `gpu_allow_invalid_fetch_constants`, `readback_resolve=full`, `occlusion_query` settings |
| `pm4_dump_plugin_*.txt` | Updated dumps with fixed parser — opcodes now resolve correctly |
| `gpu_state_diff_plugin_*.txt` | Updated GPU state diffs |

### ReXGlue SDK reference files

All paths under `C:\rexglue-sdk\include\rex\graphics\`:

| File | Contents |
|------|----------|
| `xenos.h` | Type3Opcode enum, register defs, format enums, `MakePacketType3` bit layout |
| `flags.h` | All GPU cvar declarations (130+ total) |
| `d3d12/command_processor.h` | D3D12 backend interface (what plugin implements) |
| `d3d12/pipeline_cache.h` | PSO cache |
| `d3d12/shader.h` | Shader cache |
| `format/dxbc.h` | DXBC format constants |
| `format/ucode.h` | Xenos microcode format |
| `pipeline/shader/dxbc_translator.h` | Xenos→DXBC translator interface |
| `pipeline/shader/spirv_translator.h` | Xenos→SPIR-V translator interface |
| `vulkan/command_processor.h` | Vulkan backend (if REX_HAS_VULKAN) |

### Xenia upstream references

Branch `canary_experimental` of `https://github.com/xenia-canary/xenia-canary`:

| Path | Role |
|------|------|
| `src/xenia/gpu/xenos.h` | Original Type3Opcode enum (ReXGlue's `xenos.h` is derived from this) |
| `src/xenia/gpu/packet_disassembler.{h,cc}` | PM4 packet bit fields (reference for our parser) |
| `src/xenia/gpu/command_processor.{h,cc}` | Full PM4 dispatch (the dispatcher Xenia uses) |
| `src/xenia/gpu/ucode.h` | Shader microcode ISA (ControlFlow/Fetch/Alu opcodes + layouts) |
| `src/xenia/gpu/registers.{h,cc}` | Xenos register file (full register definitions) |
| `src/xenia/gpu/register_table.inc` | Register name → offset mapping |
| `src/xenia/gpu/d3d12/command_processor.cc` | D3D12 backend (~30K lines) — what the plugin actually does per opcode |
| `src/xenia/gpu/d3d12/d3d12_shader_translator.cc` | Xenos microcode → DXBC (~5K lines + 5 split files) |
| `src/xenia/gpu/d3d12/d3d12_texture_cache.cc` | Texture cache (DXT → D3D12) |
| `src/xenia/gpu/d3d12/d3d12_render_target_cache.cc` | Render target cache |

---

## PM4 Translator — VERIFIED WORKING (2026-08-01)

### Status

The PM4 translator in `src/pm4_translator.{h,cpp}` is **fully working** for structure extraction. Verified by running with `gpu_plugin = "xenos"` and parsing every 100th swap from swap 1200+ during real 3D gameplay.

### What works

1. **PM4 parser bit fields**: `opcode=(hdr>>8)&0x7F`, `count=((hdr>>16)&0x3FFF)+1`, `predicate=hdr&1` — matches Xenia's `packet_disassembler.cc` exactly.
2. **Opcode table**: Xenia's `Type3Opcode` enum — all draw + state opcodes resolved correctly.
3. **Auto draws** (`DRAW_INDX` / `DRAW_INDX_2` with `src_sel=2`): produce synthetic sequential indices 0..N-1. Bink video quads use this (prim=8 RectangleList, idx=3; prim=6 TriangleFan, idx=4).
4. **Indexed draws** (`DRAW_INDX` with `src_sel=0`, body_words=4): parse `body[0]`=viz_query, `body[1]`=draw header, `body[2]`=ib_guest_base, `body[3]`=size/endianness. Draw header: `index_count=dword>>16, prim_type=dword&0x3F, src_sel=(dword>>6)&0x3, index_32bit=(dword>>11)&1`.
5. **Gameplay captures seen**: 60-82 draws per swap at swap 1200+ (real 3D scene geometry — TriangleList, idx counts 90-2625).

### What's blocked — plugin IB data inaccessible from host CPU

IB guest addresses in gameplay PM4 fall in range `0x1163F000`-`0x1CD93000` (91 distinct addresses observed). These are Xenos physical memory addresses above the 512MB main RAM (`0x20000000`). The plugin (`rexgpu-xenosd.dll`) manages this range via its own `D3D12SharedMemory` class:

- **`SharedMemory::kBufferSize = 512MB`** (log2=29) — covers the entire Xenos physical address space
- The plugin maintains its own page-state tracking (separate from Windows `VirtualAlloc`)
- IB data is uploaded to a D3D12 `ID3D12Resource* buffer_` (GPU-side only) via `RequestRange()` + `UploadRanges()`
- **`buffer_` is NOT CPU-readable** — it's a host GPU buffer for shader/index fetch
- `RequestRange(start, length)` is the only public API to make a range "valid" — but it triggers upload to the GPU buffer, not back to CPU memory
- From our hook context (`REX_FUNC` in `native_graphics.cpp`), we have `base` (EngineInit's guest memory base pointer), and `base + 0x1CD54000` is `MEM_RESERVE` (reserved but not committed) per `VirtualQuery`
- Calling `VirtualAlloc(MEM_COMMIT)` on these pages **crashes the plugin** — it corrupts the plugin's page-state tracking (it thinks the page is still uncommitted and its own writes race with our commit)
- There is **no public API** to obtain the `SharedMemory` instance or `ID3D12Resource*` from the host app side — `SharedMemory` is constructed inside the plugin's `D3D12CommandProcessor`, which is plugin-internal

### Conclusion for native backend

- **Can't extract IB index data from plugin mode**. The plugin's IB data lives in its own D3D12 GPU buffer; no CPU-readable path is exposed.
- **Auto draws (Bink quads) work fine** — they don't reference external IBs, indices are inline in the packet (or sequential for `src_sel=2`).
- The translator is complete enough to **handle native-mode PM4** (where we own the GPU memory and can read everything). For native mode, `base + ib_addr` will work because our `GpuAlloc` heap lives in committed main RAM (`0xBEDA0000` pool), not the plugin's external GPU buffer.
- **Move on**: stop chasing plugin IB extraction. Focus on (a) native-mode render loop integration, or (b) vertex fetch / shader / texture parsers built against gameplay PM4 dumps (which we have: `pm4_dump_plugin_*.txt`).

### Files updated this session

| File | Change |
|------|--------|
| `src/pm4_translator.h` | `DrawCall` struct with `indices` (vector<uint8_t>), `vertex_count`, `mvp`, `prim_type`, `binned`, `index_16bit`, `valid` |
| `src/pm4_translator.cpp` | Auto-draw path (`src_sel=2`) produces sequential indices; indexed-draw path parses `body[1..3]` per Xenia format; `VirtualQuery` page probe before memcpy; byteswap 16/32-bit indices BE→LE; debug logging for indexed draws + first 4 indices |
| `src/native_graphics.cpp` | Sparse VdSwap logging (every 100th + 1-20); sparse parse (every 100th ≥1200); skipped ApplyPackets gpu_state tracking for high swap counts; `should_dump_file` limit |
| `mx.toml` | Unchanged — `gpu_plugin=xenos`, `readback_resolve=full`, `query_occlusion_fake_sample_count=-1` |

### Gameplay PM4 opcode histogram (swap 1200+, 3D scene)

| Count | Opcode | Role |
|-------|--------|------|
| 224 | DRAW_INDX (0x22) | Indexed draws — `src_sel=0`, IB at external guest address, idx=90-2625, prim=6 (TriangleList) |
| 12 | DRAW_INDX_2 (0x36) | Auto draws — `src_sel=2`, prim=8 (RectangleList, Bink overlay), idx=3 |
| 91 | IM_LOAD (0x27) | Shader microcode loads (VertexShader + PixelShader) |
| 51 | LOAD_ALU_CONSTANT (0x2F) | ALU float constants from memory |
| 20 | EVENT_WRITE (0x46) | GPU events |
| 12 | EVENT_WRITE_SHD (0x58) | VS/PS done events |
| 5 | INDIRECT_BUFFER (0x3F) | Sub-buffer dispatch |
| 3 | WAIT_REG_MEM (0x3C) | GPU/CPU sync |
| 3 | NOP (0x10) | Padding |
| 1 | SET_CONFIG_REG | Config register write |
| 1 | REG_RMW (0x21) | Register read-modify-write |

**Total**: ~450 Type3 packets per gameplay swap (2982 total packets including Type0 register writes + Type2 NOPs).

### Gameplay IB address distribution (91 unique addresses)

Sampled from swap 1200+ during 3D gameplay:
- `0x1163F000`, `0x13251000`, `0x134A4000`, `0x134A8000`, `0x134EE000`, `0x1365F000`
- `0x138AB000`-`0x13999000` (cluster ~30 addresses — main geometry IBs)
- `0x1A081000`, `0x1A082400` (separate cluster — possibly UI/overlay)
- `0x1C653000`-`0x1CF7E000` (cluster ~50 addresses — secondary geometry)
- `0x3F800000` appears occasionally (garbage — float `1.0` misinterpreted as address)

All real IB addresses fall above `0x20000000` (512MB main RAM), in the plugin's `SharedMemory::kBufferSize=512MB` range.

### IDA bookmarks (translator-related)

None added — existing bookmarks sufficient.
