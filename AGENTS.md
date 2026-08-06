# MX vs. ATV Alive — ReXGlue native port

Static recompilation of the Xbox 360 build (`assets/default.xex`) via ReXGlue,
with a host D3D12 renderer replacing the Xenos GPU. The `rexgpu-xenos` plugin is
**not** used; native mode is the only supported profile.

> **This file was rewritten on 2026-08-05.** The previous version had grown to
> ~4,000 lines of dated investigation logs, and its "Current State" section
> described a renderer several rounds out of date. Everything below is either
> verified in this session or is structural fact (file layout, addresses
> cross-checked from more than one source). The old text and the four deleted
> `docs/*.md` files remain in git history if something is wanted back — but
> treat anything recovered from there as a claim to re-verify, not as fact.

---

## Build & Run

C++-only change (~15s):

```bash
cmake --build out/build/win-amd64-debug --target mx
```

The build writes to the build dir; the **root `mx.exe` is what runs** against the
assets in this directory, so copy it before running.

After editing `mx_config.toml` (mid-ASM hooks, function sizes) — ~70s codegen
plus a full rebuild:

```bash
rexglue codegen --force mx_manifest.toml && cmake --preset win-amd64-debug && cmake --build out/build/win-amd64-debug --target mx
```

**Adding a whole-function hook needs no codegen.** `REX_IMPORT` + `REX_FUNC` in a
`src/hooks/*.cpp` file is enough — the recompiler emits a `DEFINE_REX_FUNC` for
every guest function and the hook overrides it. Only mid-ASM hooks and function
sizes live in `mx_config.toml`.

### PATH

```bash
$env:PATH = "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\Llvm\x64\bin;C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;$env:PATH"
```

**The `CMake\CMake\bin` entry is required and is easy to miss** — the previous
version of this file listed only the Ninja directory, and `cmake` is not on PATH
without it. `cmake.exe` lives at
`…\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe`.

Also requires Visual Studio 2022 Clang 18+, ReXGlue SDK 0.9.0 at
`C:\rexglue-sdk`, FFmpeg 8.0 in `ffmpeg/`, and the FFmpeg DLLs already checked
in at the repo root.

### Running

```bash
./mx.exe --game_data_root=assets --user_data_root=userdata --skip_intro=true --force_load=NAT_Farm --registry_override=ReadyToLaunch=1
```

`--game_data_root` and `--user_data_root` are mandatory — without them the
process exits immediately with code 1 and writes no useful log. Each run appends
a new `logs/mx_NNN.log`.

**HLE is the only render path since 2026-08-06.** `hle_render` and
`pm4_translate` were cvars for one week and are now gone with the translator
they selected between — there is no PM4 fallback to pass a flag for. The D3D9
description is the sole source of draws.

The old run line here also carried `--hle_shader_exec=1` under the claim that
**"the HLE render path does nothing without it". That was wrong.**
`hle_shader_exec` gates only a sampled measurement, and its one use sits behind
`hle_capture`, so passing it without `--hle_capture=true` does nothing at all —
which is how three verification runs came to be missing every `hle-render` and
`stageF` line. Rendering is unconditional
(`BuildAndQueueDraw` -> `ApplyShaderOutputs`, `hooks_d3d9.cpp:976`). Add
`--hle_capture=true --hle_shader_exec=1` when you want the diagnostics, and
expect them to cost.

**Always run with `--force_load`**, and say so when reporting numbers. Without
it a run never leaves the front end, and front-end geometry transcodes fine —
`not-transcoded` reads 33% instead of the real 93.8%, and scene-only failures
do not appear at all. `force_load` fires about 115 seconds after start, so a
run shorter than ~2½ minutes never reaches it; ~400s gives a usable sample.
`ST_Southwest` also works.

### Runtime cvars

`mx.toml` or `--flag=value`. Defined in `src/app/graphics_system.cpp` and
`src/hooks/hooks_d3d9.cpp`:

`clear_magenta`, `d3d9_hooks_passthrough`, `d3d9_page_cache_verify`,
`force_load`, `hide_colored_draws`, `hide_colorless_draws`, `hle_capture`,
`hle_main_viewport_only`, `hle_shader_exec`, `hle_shader_verts`,
`registry_override`, `skip_intro`.

Seven more (`alu_execute`, `skip_untransformable_draws`, `tint_by_color_source`,
`transcode_confirmed_formats_only`, `transcode_trust_export`,
`vertex_transcode`, `vfetch_use_shader_slot`) were defined in the PM4 translator
and went with it, as did `hle_render`, `pm4_translate` and `main_surface_only`.

`force_load` and `registry_override` are **diagnostic levers, not fixes** — see
"Why there is no menu" below.

**Two crash classes here have been races, not logic bugs** — a rebuild alone
changed the outcome. One clean run proves nothing; verify 3/3.

---

## Architecture

`src/` is grouped by layer and is on the include path, so cross-layer includes
name their layer: `#include "gpu/pm4_parser.h"`.

| Path | Role |
|---|---|
| `src/main.cpp` | Entry point (`REX_DEFINE_APP`) |
| `src/app/mx_app.*` | `MxApp`: installs the graphics system and crash reporter, hands over the HWND |
| `src/app/graphics_system.*` | `D3D12GraphicsSystem` + host render thread (Bink playlist → game frames); most debug cvars are defined here |
| `src/gfx/d3d12_renderer.h` | `D3D12Renderer` interface, implemented across the three `.cpp` below |
| `src/gfx/d3d12_device.cpp` | Device, adapter, swapchain, RTVs, command lists, fence, Begin/EndFrame |
| `src/gfx/d3d12_video.cpp` | Fullscreen quad + texture sample, Bink frame upload |
| `src/gfx/d3d12_game.cpp` | Game pipeline, per-frame draw list, game RT + depth, HLE offscreen targets, compositor |
| `src/gfx/bink_player.*` | FFmpeg Bink video + audio |
| `src/gpu/pm4_parser.*` | PM4 command buffer parser (Type-0/2/3, ring wrap, dump) |
| `src/gpu/hle_types.*` | Shared HLE data model (`DrawCall`, textures, topology) + the two primitive expansions |
| `src/gpu/xenos_gpu_state.*` | Xenos register shadow, snapshot/diff |
| `src/gpu/d3d9_layout.*` | Guest `D3DVERTEXELEMENT9` array → host input layout |
| `src/gpu/d3d9_draw.*` | HLE draw assembly from bound streams |
| `src/gpu/d3d9_state.*` | D3D9 device state shadow (render targets, resolves) |
| `src/gpu/d3d9_texture.*` | Texture profile / bound-texture resolution |
| `src/gpu/shader_ucode.*` | Xenos microcode decode: vertex fetches, pixel texture profiles |
| `src/gpu/shader_alu.*` | Vertex shader ALU interpreter |
| `src/hooks/hooks_d3d9.cpp` | The D3D9 HLE layer — the largest file in the project |
| `src/hooks/hooks_frame.cpp` | VdSwap, XenosWait, Begin/EndFrame, GpuState |
| `src/hooks/hooks_boot.cpp` | Bootstrap, GraphicsInit, EngineInit, TexManager, GpuAlloc |
| `src/hooks/hooks_loading.cpp` | SetupRenderer, Transition, LoaderTick |
| `src/hooks/hooks_gameloop.cpp` | MainLoop, RenderPipeline |
| `src/hooks/hooks_plugin_diag.cpp` | Registry hooks, load-state-machine probes, script-VM probes |
| `src/hooks/midasm_stubs.cpp` | Exported mid-ASM hook targets (must stay at global namespace) |
| `src/hooks/native_bridge.*` | `NativeGraphics` singleton, `g_plugin_mode` |

Input is handled entirely by ReXGlue's built-in `SDLInputDriver` at kernel level;
there is no manual input hook.

---

## SOLVED: it was the frame-pacing stub (2026-08-06)

**The native `sub_82B70370` stub was the blocker.** Unstubbing it takes the
script VM from 28 dispatches to 686 and the front end starts running. Everything
in the section below is still an accurate description of the *symptoms*; the
cause is here.

| native, 60 s run | stubbed | real timing |
|---|---|---|
| VM dispatches | 28 | **686** (3/3: mx_473, mx_474, mx_475) |
| script assets | 2 | **4** — adds `IG_PlayerListHelper` |
| registry string keys | 1 | **4** — adds `IntroMode`, `GameContentInstall`, `GameSessionNotification` |
| `BinkOpen` | 0 | **3** — THQ_Logo, Attract.ENG, FE_Smoke, all OK |
| bindings reached | — | `LoadUIAssetDatabasePackage`, `LoadUIAssetPackage`, `IsUIAssetPackageLoaded`, `LoadAssetDB`, `LoadAssetPackage`, `SendUIEvent`, `CastUIContainer`, `CastUIFlashComponent` |
| lua errors | 0 | 0 |

Those are the plugin's numbers. **The native/plugin divergence is closed.**

With `--hide_colorless_draws=true` the front end renders 11,250 draws / 33,750
vertices with **every skip counter at zero** — no-code 0, decode 0, stream 0,
constants 0, vertex 0.

**Both hazards the stub was written for are false**, read out of `sub_82B70370`
rather than assumed:

- *"`a1+20` drives a busy-wait."* The guest's own test is
  `if (*(float*)(a1+20) != 3.4028235e38 && dt < target)`. `a1+20` reads
  `0x7F7FFFFF` — exactly that FLT_MAX sentinel — so the guest disables the spin
  itself. Logged and confirmed at runtime: `FLT_MAX=true`.
- *"`a1+32` is an unbounded store offset."* It is `v9 = *(a1+32) + 9;
  *(float*)(4*v9 + a1) = dt;` with `if (v10 >= 5) *(a1+32) = 0` — a bounded
  5-entry ring at `a1+36..a1+52`, guarded by `if (*(a1+28))`. Observed 0.

The stub wrote a fixed `1/60` to `a1+24` and nothing else. The real function also
maintains `a1+56` (the 5-sample smoothing sum), **`a1+60`, total elapsed time**,
`a1+64`, `a1+104` and `a1+112`. A front end that advances on elapsed time had
nothing to advance on — the measured symptom was `f1` arriving at
`RendererDispatch` as exactly `0.00` in native and varying under the plugin.

The stub is deleted outright rather than left behind a cvar: it was wrong, not
a trade-off, and git has it. `legacy_mvp_tiebreak` went the same way in the same
commit, its A/B having been settled.

**Why this took so long to find, worth remembering:** the stub dates from before
the D3D9 HLE layer, like the four other workarounds retired on 2026-08-06, and
its comment stated both hazards as fact. Nobody re-derived them. The
instrumentation was also one-sided — the VM-dispatch probe was rate-limited to
four lines, so "4 dispatches" looked like a hard floor, and the
`RendererDispatch` probe logged only under `g_plugin_mode`, so native's `f1=0.00`
was never visible. **Two probes that could not have detected the bug were used as
evidence that the bug was not there.**

## Why there is no menu (2026-08-05) — superseded, kept for the symptom trail

**The front end is script-driven, and the script VM stops 1.6 seconds into
boot.** This supersedes every earlier explanation in this file's history,
including the `PlayerMode` and load-state-machine readings, which are symptoms.

The chain, each step measured:

- Tracing the load-request API upward through the recompiled sources dead-ends.
  Four of the five functions above `sub_82352AE0` have **zero** direct callers,
  and they are not virtual methods either.
- They are entries in a **name → function binding table at `0x8203F2E0`** — 228
  pairs of `const char*` and code pointer, registered from `sub_824F1C98`
  (site 0x824F1E1C). The vocabulary is a scripting API.

| Binding | Address | |
|---|---|---|
| `[6] LoadAssetDB` | `0x824AF3C0` | |
| `[10] ExecuteScriptAsset` | `0x824AF838` | **fires twice, then never** |
| `[40] GetUIState` | `0x824BBA40` | |
| `[50] LoadUIAssetPackage` | `0x824CBF90` | never fires |
| `[53] LoadUIAssetDatabasePackage` | `0x824CC218` | never fires |
| `[66] StartWorldLoad` | `0x824CD280` | never fires — reaches `sub_82534980` |
| `[67] EnableWorld` | `0x824CD308` | |
| `[110] SwitchToUIWorld` | `0x824D0F18` | never fires |

- `ExecuteScriptAsset` validates one `char const*` argument and passes it to
  `sub_824F91E8`, whose `r3` is therefore the script asset name in plain guest
  memory. Hooking it names the two assets that run:

```
native: script asset #1 "RSLibrary" (ptr=0x2040A270) from lr=0x824AF8B0
native: script asset #2 "UI_Helper" (ptr=0x2040A5D0) from lr=0x824AF8B0
```

- Both are **libraries**, not drivers. The `MXUI` database holds **57 unique
  script assets**, including `IN_BootStrapper`, `FE_Title`, `FE_Home`,
  `IN_Loading`, `IG_WorldLoading`, `SH_LuaDataProvider`.
- **Careful about what this does and does not prove.** `ExecuteScriptAsset` is a
  *script binding* — a script called it twice. So a root script **is** running;
  something started it, and it loaded two libraries and then stopped. The probe
  shows only that no *third* asset is executed **by name** through
  `sub_824F91E8`. It does not show that `IN_BootStrapper` never runs — the root
  script could well be `IN_BootStrapper` itself, stalling after its first two
  statements. Which of those it is has not been measured.
- The VM's own native-call dispatcher (`sub_82AA7638`, identified from
  `ExecuteScriptAsset`'s caller `lr=0x82AA78F4`) fires **4 times in an 80-second
  run, all within the first 1.6 seconds**, then goes silent. The probe is
  rate-limited to one line per 5s after the first four, so the silence is real.
- Corroborating: the guest reads **exactly one registry key** in a whole run —
  `PlayerMode`, by the loader's own gate. `Location`, the key naming the scene to
  load, is never read.

**The script layer is a Lua VM.** `sub_82AA7638` is the call handler and carries
the familiar strings (`"stack overflow"` via `sub_82AA9D48`); `sub_82A9F4F8` is
the `luaL_error`-style reporter that `ExecuteScriptAsset` uses for argument
mismatches; `SH_LuaDataProvider` is in the asset list. So the next move is to
hook those two error paths and ask the direct question: **is the root script
throwing?** A script that dies on statement three looks exactly like this from
the outside.

**Disproved, so nobody retries it:** the one active mid-ASM hook
(`NativeSkipRendererDispatch`, skipping `bl sub_82B34998` at 0x82B70EF4) is
**not** the cause. That block contains three `bctrl` indirect calls per
LoaderTick, which made it a strong suspect. Re-enabling it changes nothing —
still 4 VM dispatches, still the same two script assets, still no error.

Worth keeping from that experiment: **the block no longer crashes without the
skip.** Its premise was "this needs the Xenos GPU", written before the D3D9 HLE
layer existed. Draw counts are unchanged and there is no fault. That is one run,
not the 3/3 this file demands, so verify before relying on it.

**Do not reverse-engineer the AssetDB for this.** ReXGlue handles asset loading
fine, and the runtime confirms it — the only failed opens in a full run are four
DLC camera `.bxml` files and `\Device\Image`, which Xenia also shows. The
encrypted `.xenon.package` heaps are a limitation of the *offline* tools in
`tools/`, not evidence that the guest cannot read its own packages.

`PlayerMode = "None"` is index 4 of the game's own five-value vocabulary
(`SplitScreen`, `SinglePlayer`, `Online`, `LAN`, `None`; the *failure* value is
5). It is the expected value before a menu has chosen a mode — a symptom, not a
cause.

### Audio and input are downstream of this, not separate bugs (2026-08-06)

Neither works in native mode, and the natural suspicion was that native mode
broke ReXGlue's handlers. It did not. Measured by hooking the guest's own XDK
wrappers around the import thunks — the thunks are defined in the runtime
library and cannot be redefined, but the wrappers are ordinary recompiled
functions (`sub_82C08EC0` → `XamInputGetState`, `sub_82C08ED0` →
`XamInputGetCapabilities`, `sub_82C87F78` → `XAudioRegisterRenderDriverClient`,
`sub_82C87B98` → `XAudioSubmitRenderDriverFrame`, `sub_82C4C268` →
`XMACreateContext`).

**Audio is a working pipe carrying silence.** The guest registers a render
driver client (r3=0), then submits **30,776 frames in 165s** — 187/s, exactly
the 360's 256-sample-at-48kHz frame rate, so the SDK is consuming and pacing it
in real time. `sub_82C87B98` is the XDK mixer, not a thin wrapper: it has
`sub_82C87950` fill a buffer at r1+1888 and passes that to the import
(mx_recomp.94.cpp:31435), and 8064-1888 = 6176 bytes of room fits one
256 x 6ch x float32 frame. Hooking that fill and scanning all 1536 floats gives
**peak = 0.000000 on every frame**, with and without `force_load`. Neighbouring
uninitialised stack reads as the 0xBCBCBCBC fill pattern, so exact zero means
the mixer really did write silence. The game is playing nothing.

**Input works end to end.** `XamInputGetState` returns success and the packet
number advances (1 → 16 → 17 across runs), so live pad state reaches the guest.
It is polled only ~18 times in 75s, from `sub_82B6DB28` (lr=0x82B6DBD4) — the
slow "is a pad connected" cadence, not a front end reading a stick.

So there is one bug here, not three. Do not open audio or controller work as a
separate thread until the front end runs.

**The plugin-mode reference, measured 2026-08-06 with the same probes.** With
the D3D9 HLE hooks guarded (below) so the plugin runs at full speed, the guest
gets far past where it stops natively:

| | native | plugin |
|---|---|---|
| script assets | 2 (`RSLibrary`, `UI_Helper`) | **8**, incl. `IG_PlayerListHelper`, `SH_GarageHelper`, `SH_XPHelper`, `FE_Home_Cameras`, `SH_CutsceneHelper` |
| VMDispatch | 4 | **16** |
| audio | 0 non-silent / 30,776 | **11,786 non-silent / 13,115, peak 0.216** |

`FE_Home_Cameras` is a front-end script. So the front end does run under the
plugin, and the audio path is not merely alive but ~90% non-silent. **The
divergence is in the script layer and it is the most informative open thread in
the project** — it is the first thing that separates the two modes at the level
of the actual blocker. Whatever native mode is missing, plugin mode has it.

### The D3D9 HLE hooks must no-op in plugin mode (2026-08-06)

`hooks_d3d9.cpp` is the native renderer, but until 2026-08-06 all 14 of its
hooks ran in **both** modes — the only hooks file without `g_plugin_mode`
guards (`hooks_boot.cpp` 9/9, `hooks_frame.cpp` 8/8, `hooks_gameloop.cpp` 2/2).
`hle_render` and `hle_shader_exec` defaulted off then, so the transcode and
CPU-shader paths were not running; the per-draw bookkeeping alone was enough, at ~1,480
draws a frame in a Debug build.

Measured cost: plugin-mode `MainLoop` fell from **~17.6/s** (2026-08-03, before
this file grew — every commit that grew it is dated 2026-08-05) to **~0.37/s**.
Restoring the guard brings it back to ~16.4/s.

Use `MX_D3D9_PLUGIN_PASSTHROUGH(orig)` on any hook added to that file. Every
hook there calls its original exactly once, so returning straight after it is
the complete plugin-mode behaviour.

**The same layer is ~85% of native frame time, where it is on the critical
path.** Native `MainLoop` bodies run 300ms rising to 3100ms. Timing each level
gives one chain, each step matching its parent to within milliseconds:

```
MainLoop sub_82B70760 -> RenderPipeline sub_82B70578 -> sub_82AFE978
  -> sub_82AFCA38 (4,416 lines, 31 SetTexture, 4 Resolve)
    -> sub_82AFA520 -> sub_82AF93C8, which recurses
```

Everything else on the path is cheap or free: `MainLoopPump` and the pre-calls
never reach 50ms, BeginFrame/EndFrame are stubbed natively, Resolve never
reaches 50ms, and VdSwap's whole hook peaks at 87ms with its own original under
50ms — so parsing ~11,600 PM4 packets a swap is not the cost.

Two measurements identify it as ours, not the guest's:

- **A Release build costs exactly what Debug does** — same 300ms bodies, same 27
  MainLoop iterations in 70s. Recompiled PPC compute would not behave that way.
- `--d3d9_hooks_passthrough=true` makes the 14 hooks pass through in native too.
  MainLoop bodies drop to **7–70ms**, a ~50x change. That flag **breaks
  rendering by design and the run crashes ~19s in** (the host renderer loses the
  state tracking while the PM4 translator keeps going) — it is an A/B
  instrument, not a mode.

### It was `VirtualQuery`, and it is fixed (2026-08-06)

Not the per-draw bookkeeping — that was a guess and it was wrong. `DeclFile()`
writes stop after `kMaxDrawsLogged = 16` draws, `ReportDrawCounts` early-returns
2499 times in 2500, and `REXCVAR_GET(x)` is a storage accessor.

`HostPageReadable` was **~100% of native frame time**: 502 calls a frame costing
3082ms of a 3128ms MainLoop body. Note the shape — **~6ms per call**, not many
cheap calls. A `VirtualQuery` is normally microseconds; six milliseconds is what
it costs against this process's address space, and that is also why a Release
build cost exactly what Debug did.

The fix is not to call it less by guesswork. `VirtualQuery` already returns the
whole contiguous run it found in `mbi.BaseAddress` / `mbi.RegionSize`, with
identical `State` and `Protect` throughout, so one query legitimately answers for
every address in that range. `HostPageReadable` now keeps an 8-entry MRU region
cache, cleared once per swap from the VdSwap hook so a commit or decommit is
picked up within a frame — a stale *positive* on a decommitted page is a crash,
which is the whole reason this function exists.

`--d3d9_page_cache_verify=true` re-queries the OS on every cache hit and logs
disagreements. **0 mismatches in 5,557 checks**, which is the correctness
argument, run rather than asserted.

| | before | after |
|---|---|---|
| MainLoop body | 300 → 3100ms | ~105ms |
| MainLoop in 70s | ~26 | **901–961** (3/3) |
| VirtualQuery per frame | 502 | 5–7 |

Native is now ~13/s against the plugin's ~16/s, from 0.37/s.

**And it did not move the front end: still 2 script assets and 4 VM dispatches,
3/3.** Frame starvation was not what held it back. The script-layer divergence
against plugin mode is a separate cause and remains the open question.

`--log_high_frequency_kernel_calls=true` does **not** gate these calls: a run
with it has the same 15 `[krnl]` lines as one without. Hook the wrappers.

### More stale workarounds retired (2026-08-06)

Both were the same vintage and shape as the mid-ASM skip above — written before
the D3D9 HLE layer, never revisited. Both are neutral over 3/3 runs (2 script
assets, 4 VM dispatches, no crash, no Lua error), so neither was the cause, but
both were arbitrary and are now gone.

- **The blanket 500 ms wait short-circuit** in `hooks_wait.cpp` returned
  SUCCESS from `NtWaitForSingleObjectEx` for *any* 500 ms wait, process-wide,
  never scoped to the renderer handshake it was written for.
- **`sub_82BFBF48` was stubbed to nothing** and labelled "error recovery". That
  name was a guess and it was wrong: it tail-calls `sub_82C01138`, which is a
  pure CRT thread-block read (`r13+336 ? 0 : *(*(r13+256) + 352)`), an
  errno-style pointer accessor with no side effects. Stubbing it left r3
  undefined at 156 call sites. Unstubbed.
- The `sub_82BFB748` (NtSetEvent) hook was deleted. It called the original
  unconditionally in both modes — eight log lines, no behaviour.

**Also not the cause: the intro skip.** `skip_intro` is host-side only; it
skips mx's own Bink playback loop in `D3D12GraphicsSystem::RenderThreadFunc`.
Its one guest-visible effect is `IsBinkPlaying()`, which makes the guest's
`RenderPipeline` stand down — so skipping the intro makes the guest run *more*.
Measured: a full run with the intro actually playing its 47.4s still gives 2
script assets and 4 VM dispatches, all inside the first 3.3s.

**The FFmpeg dependency is a native-mode substitute for a guest path that
works.** `MxApp::OnPreSetup` returns before creating `D3D12GraphicsSystem` when
a GPU plugin is set, so `RenderThreadFunc` — and with it the whole host Bink
player — never starts in plugin mode. The intro plays fine under the plugin
anyway (confirmed 2026-08-06), which means the guest decodes Bink itself and
`src/gfx/bink_player.cpp`, the five FFmpeg DLLs and the `ffmpeg/` tree are
covering for the *renderer*, not for a missing guest decoder.

That workaround predates the D3D9 HLE layer, like the others above. It is also
hardcoded to two English filenames (`graphics_system.h:51`) while
`assets/Videos/` holds DEU/FRA/ITA/SPA variants, so it cannot follow the
language setting. Whether the guest's own decoder now renders through HLE is the
test that would let the whole dependency go; it has not been run.

**That inference was right, and is now measured — see below.**

### The guest opens Bink under the plugin and never natively (2026-08-06)

Bink is statically linked into the XEX the same way D3D9 is: a `BINKCONS` data
segment at `0x821CD1D0` and library code from about `0x82CEB650` to `0x82CF0508`.
The guest carries a complete decoder.

| Symbol | Address | How it was identified |
|---|---|---|
| `BinkOpen(path, flags)` | `sub_82CEB7C8` | Owns `"Not a Bink file."` (`0x82144B9C`) and `"Error reading Bink header."` (`0x82144B28`), referenced nowhere else. Two callers total, so it is *the* choke point |
| `XenonBinkVideoManager::Open` | `sub_8234E0A8` | vtable `0x82017510` slot [1]; formats `"game:\%s.bik"` |
| `XenonBinkVideoManager::Open` | `sub_8234E290` | slot [2]; formats `"%s.bik"` |
| `BinkAsset::Init` | `sub_8234CBB8` | Reads `"Texture To Override"`, then resolves `"Bink Video Asset"` and requests fourcc `1651076715` = `0x62696E6B` = `'bink'` |
| `XenonBinkVideo` vtable | `0x820172BC` | 12 entries; HBINK at object `+144`, its critical section at `+156` |

Probes on the first four are in `hooks_plugin_diag.cpp` and are mode-neutral.
**4 native runs, 2 plugin runs, `--skip_intro=true`, no `--force_load`:**

| | native 4/4 | plugin 2/2 |
|---|---|---|
| `BinkOpen` | **0** | 3, all returning a live HBINK |
| `BinkMgr::Open` | **0** | 5–6 |
| `BinkAsset::Init` | 1 | 1 |
| → resolved asset handle | **`0x00000000`** | `0x2345D2A0` |
| script assets executed | 2 | 4 |
| registry keys read | 1 | 4 |

Under the plugin the guest opens `game:\Videos\THQ_Logo_wSound.bik`,
`Attract.ENG.bik` and `FE_Smoke.bik` itself, in that order, roughly 3 s in. The
flags confirm the static read of `sub_8234E0A8`: `a4=1` gives `0x00102400`
(`0x2000|0x100400`) and `a4=0` gives `0x01100400`, the branch that first calls
`sub_82CEB3F0(10485760)`.

**Natively not one video is ever opened.** `BinkAsset::Init` still fires — which
is what proves the probe is live rather than absent — but its asset handle comes
back null, meaning `sub_82AB8210("Bink Video Asset")` returned nothing, so the
`'bink'` request in that function never happens.

`FE_Smoke` is a *front-end* video. Together with the registry keys below, that
puts the plugin inside the front end and native nowhere near it, so **Bink is
downstream of the script-layer divergence, not a cause of it.** It is, however,
a far sharper marker of that divergence than the script-asset count.

**Registry reads are the sharpest marker of all.** Native reads exactly one
string key in a whole run; the plugin reads four:

| Key | native | plugin |
|---|---|---|
| `PlayerMode` | ✓ | ✓ |
| `GameContentInstall` | — | ✓ `"installed"` |
| `IntroMode` | — | ✓ `"full"` |
| `GameSessionNotification` | — | ✓ |

**The probes now log `lr`, and it names the callers outright.** Both registry
getters capture `lr` at entry — the call clobbers it and both log after — so the
return address is the caller, not an inference from which function mentions the
key in `.rdata`.

| Read | from `lr` | which is |
|---|---|---|
| native `PlayerMode` | `0x82536294` | `sub_82536250` — the loader's own gate |
| plugin, every extra string key | `0x824AA590` | `sub_824AA568`, called only by `sub_824B1C20` |
| plugin, every int key | `0x824AA518` | `sub_824AA4F8`, called only by `sub_824B1788` |

`sub_824B1C20` is the **Lua binding `GetVariableString`**: it validates its
arguments through the same `sub_82A9F4F8` reporter as `ExecuteScriptAsset` and
names itself `VariableCollection_GetVariableString` in its own error strings.
`sub_824B1788` is the int twin, `GetVariableInt`. Both are registered in a
second `(name, func)` table in `.data` around `0x82D1B21C`, distinct from the
228-entry one at `0x8203F2E0`.

**Correction:** an earlier version of this section put those two in a table at
`0x821A1740`/`0x821A1750`. That is `.pdata` — function address plus unwind
flags — not a binding table. Same mistake as reading `0x82198B50` as a vtable;
the giveaway both times is a second dword like `0x400003A3` that is not a
pointer. Check that before reading any address pair as `(x, func)`.

So this is not a settings file being consulted. **It is the front-end script
reading its own state**, and under the plugin it reads a recognisable boot
sequence: `GameContentInstall`, `UILoaded`, `IntroMode`, `InitialLoadCompleteFlag`,
`LaunchActivity`, `TableLoadError`, `InvitePending`, `InviteProcessing`,
`GameSessionNotification`. Natively that binding is called **zero** times; the
one native read comes from the loader, not from any script.

That makes `sub_824B1C20` and `sub_824B1788` a live progress trace of the front
end, and the sharpest instrument yet for the script-layer divergence — every
call is a statement the script actually reached.

**A process-hygiene warning, learned the expensive way.** The first pass at this
measurement reported "zero Bink calls in both modes" and was wrong twice over.
First, the build writes `out/build/win-amd64-debug/mx.exe` and nothing copies it
to the repo root, so `./mx.exe` ran a stale binary that did not contain the
probes at all — check with `grep -c "<a new format string>" mx.exe` before
believing any new probe's silence. Second, a cancelled background run script kept
running (only its output pipeline had been killed) and launched a second
`mx.exe`, so `ls -t logs/*.log | head -1` attributed *its* log to the run just
started. Identify a run's log by diffing the directory listing, never by mtime,
and confirm with `Get-Process mx` that exactly one is running.

### Native and plugin run the same 28 script calls, then native stops (2026-08-06)

`sub_82AA7638` is Lua's `luaD_precall`, and its `r4` is the `func` StkId, so the
callee is readable before it runs:

| offset | meaning |
|---|---|
| `*(func + 8) == 6` | `TValue.tt`, `LUA_TFUNCTION` |
| `*(func + 0)` | the `Closure` |
| `*(closure + 6)` | `isC` — a C binding rather than Lua bytecode |
| `*(closure + 16)` | the C function pointer, for `isC` closures |

Lua 5.1 offsets, confirmed against this binary rather than assumed: the
function's own Lua-closure branch reads Proto fields at `+73` numparams, `+74`
is_vararg, `+75` maxstacksize, `+12` code, and reports `"stack overflow"` at
exactly the 20000 limit.

**The old "4 VM dispatches" figure was a rate-limiting artifact.** The previous
probe logged the first four and then one line per 5 s, so the true count was
never seen. It is **28** natively and **203** under the plugin in a 60 s run.

**The two sequences are identical for all 28 calls** — same kinds, same function
pointers, same order, including `ExecuteScriptAsset` twice, `GetUIVariables` and
`GetMXTableHelper`. Native then stops. The plugin continues:

```
#28  C   cfunc=0x829E8FA8                              <- native's last
#29  lua
#30  C   cfunc=0x829E8FA8
#31  C   LoadUIAssetDatabasePackage  (sub_824CC218)
#32  C   LoadUIAssetPackage          (sub_824CBF90)
#37  C   IsUIAssetPackageLoaded      (sub_824CC120)
```

So the two bindings listed as "never fires" above are not merely absent — they
are **the very next thing the script would do**.

**Native's #28 returns.** Its thread goes on to open camera `.bxml` files and
write vertex declarations, so the script layer is not hung inside a C binding.
The VM is simply never entered again.

**The dispatches split by thread, and that is the lead:**

| | native | plugin |
|---|---|---|
| main VM thread | t18112 — 26 | t2936 — 163 |
| second thread | *(none)* | t18072 — 38 |
| early thread | t17628 — 2 | t17176 — 2 |

**The thread is the Transition thread**, and it exists in both modes. Under the
plugin it is `t18072`, whose first line is `Transition #1` and which makes 38
dispatches. Natively it is `t2408`, which runs the same `LoadStateMachine` /
`LoaderTick` / `Timing` / `XamInputGetState` cycle and makes **zero**. The
question is therefore not "did the script throw" — no `lua error` or
`lua runerror` line appears in either mode — but **why the Transition thread
never enters the VM natively.**

Under the plugin the VM entry lands inside `LoaderTick`, on iteration 7, in the
window immediately after `sub_82B34998` (RendererDispatch) returns.

**Two wrong readings on the way there, both corrected by measurement:**

- *"Native never calls RendererDispatch."* False. That hook logged only under
  `g_plugin_mode` and called the original silently in native, so its absence
  from a native log meant nothing. It is now mode-neutral. Native calls it and
  it returns non-null (`0x21294134` / `0x212859A0`).
- *"The plugin has a second thread calling it and native does not."* False. The
  plugin's call numbers skip (`#1, #3, #5…`) purely because two threads
  interleave on one counter — and **both** modes have two such threads.

**Most Transition-thread probes are still plugin-only**, so do not read any
other line-by-line difference on that thread as a finding until the probe in
question has been checked for a `g_plugin_mode` guard.

**What does differ, measured on the now-neutral probe: `f1` is exactly `0.00` on
every native call to RendererDispatch**, while the plugin passes varying values
(`-2.00`, `2.00`, `±3.7e19`). `f1` is the frame delta. Native's `Timing`
(`sub_82B70370`) is **stubbed** — it writes `1/60` to `a1+24` and never calls the
original — so a zero dt reaching this call is consistent with that stub being
incomplete. A front end that advances on elapsed time would never advance.

That is a lead, not a conclusion: dt is one measured difference on one call, and
nothing yet connects it to the VM entry. Test it before believing it.

Caveat on naming: `0x829E8FA8` is a bare `return 0` with hundreds of xrefs, so
identical-code folding makes several trivial bindings share one address. A
`cfunc` value alone cannot name such a binding; the native/plugin *sequence
diff* is what carries the result, and it needs no names.

### Draws land outside the clip volume on z (2026-08-06)

A RenderDoc capture (`mx_2026.08.05_23.55.39_frame2967.rdc`, EID 831,
`DrawIndexedInstanced(1810, 1)`) shows, on every row of the mesh viewer:

| | X | Y | Z | W |
|---|---|---|---|---|
| VS Input `POSITION` | 11.34206 | 11.01484 | 8.23549 | — |
| VS Output `SV_POSITION` | −0.98228 | 0.96940 | **8.23549** | **1.00** |

D3D clips on `0 <= z <= w`, so the draw rasterises nothing.

The shader is not at fault. `kGameVS` does `mul(mvp, float4(pos, 1.0))` and
`BuildViewportMvp` is an **inverse viewport**: row 3 is left identity so `w` is
always 1, and row 2 is `1/zs`, which every run reports at the `zs == 1`
fallback. Untransformed z therefore passes straight through.

The viewport is right, too. Solving `(11.34 − xo)/xs = −0.98228` and
`(11.01 − yo)/ys = 0.96940` gives exactly `xs=640 xo=640 ys=−360 yo=360`, and
the log prints those verbatim. **The input is what is wrong** — a 1810-vertex
mesh whose window-space bounding box is 0.68 x 4.41 pixels in the top-left
corner is in model space, not window space.

Two independent instruments agree, from opposite ends:

- `d3d9: stageI` scores `in-clip 0%` on every shader it calls `window-like
  100%`. That metric does bound z (`hooks_d3d9.cpp`), which is why it sees
  what the PM4 classifier could not.
- `transcode: done 5000 passthrough 53232 (no shader 0, no position 53224,
  read failed 8)`. **`no position` is the exit** — `PickPositionAttribute`
  finds nothing. `no shader` and `read failed` are ~0. Position format 57 is
  the only one that ever transcodes; `packed` colour is `0:0` on every row of
  the colour x surface table.

**A gate that measures one subsystem does not clear the others.** The run above
was read as "PM4 contributes nothing to HLE rendering". That was true of
geometry and **false of pixel shaders**, which had a separate PM4 dependency
through `CapturedPixelShaders()` — 14 shaders decoded only with the ring live,
all 14 via its exact key. It was caught only by checking a counter the gate had
not been designed around. When retiring a subsystem, enumerate its consumers
first and measure each; a headline number from one of them proves nothing about
the rest. (That dependency is now gone — see the pixel shader object table.)

**The untranscoded share depends entirely on `--force_load`, so always state
which.** Measured at equal `done 5000`: with `--force_load=NAT_Farm`,
`passthrough` is 53232 and the class table reads **93.6-93.8% of draws and
99.9% of vertices** not-transcoded (3/3 runs, stable to a fraction of a
percent). Without it, `passthrough` is 2779 and the share is 33%. Passthroughs
are a property of loaded scene content, not of the pipeline — a front-end-only
run transcodes fine and will make this look nine times better than it is.

`ClassifyTransformedDraw` computed z and threw it away, testing only x and y,
so this draw scored `kPartial` — a clean bill of health. It now has a
`kDepthClipped` class, and `passthrough` draws are counted as `kNotTranscoded`
rather than being absent from the table entirely. Counted only; nothing is
skipped unless `--skip_untransformable_draws=true`.

**`kDepthClipped` reads 0 in every run, and that is correct — it is on the
wrong path for the draw that motivated it.** `ClassifyTransformedDraw` is
called only from `TranscodeVertices`, i.e. the PM4 path, which carries ~15k
vertices in a loaded scene. The captured draw comes through the **HLE** path
(`d3d9_draw.cpp`), which carries **10.67M** — `d3d9: HLE shader output ...
applied 37018 draws / 10671287 vertices`. Do not add a second z classifier
there: stageI's `in-clip` already bounds z on that path and already reports
`0%`. The two paths are separately instrumented and the HLE one was never
blind.

The `kNotTranscoded` class still earned its place — it is what made the
99.9%-of-vertices figure visible at all.

**The mesh shape in RenderDoc is not evidence of anything.** Two captures of
EID 831 that look wildly different — a figure and a twisted spike — differ only
by the Mesh Viewer's **Axis Mapping** dropdown (`Y-up, right handed` vs `Y-up,
left handed`). The bounding box is identical in both. Check that dropdown
before reading a shape as a bug.

**`ripgrep` skips `logs/*.log` as binary.** The Grep tool reported zero matches
for `NDC prim=` in a file containing thirty of them, and zero for
`transcode: done` across the whole directory when two runs have it. Every
negative result above was re-established with `grep -a`. Use `grep -a` on these
logs, always — a silent no-match here reads exactly like a feature that never
ran.

---

## D3D9 HLE rendering

The working approach. D3D9 is linked statically into the XEX, so there is no
import table — but static linking removes the imports, not the functions.
Entry points were located by matching COFF symbols from a genuine Xbox 360
`d3d9.lib` (machine `0x01F2`, PowerPC BE); `tools/match_d3d9.py` regenerates the
patterns. **FLIRT/FLAIR does not work here and is not needed** — the COFF symbol
tables parse directly and give more.

### The transform choice is a register, not a contest (2026-08-06)

`ApplyShaderOutputs` used to pick between identity and the viewport inverse with
`if (identity_in_clip > viewport_in_clip)` — a strict `>`, so ties went to
viewport. With `in-clip` commonly 0 for both candidates, most draws defaulted
rather than won: measured, that rule disagreed with the hardware on **87789 of
106132 draws (82.7%)**.

The hardware states the answer. **PA_CL_VTE_CNTL (0x2206)** says whether the GPU
applies the viewport transform itself, which is exactly the question. Its shadow
follows the pattern already established for `SQ_PROGRAM_CNTL`: the draw-time
flush issues `sub_82564768(device, 0, 8704, device + 10548)` with 8704 = 0x2200
= RB_DEPTHCONTROL, and `sub_82564768` sends register `base + i` from
`shadow + i*4`, so **0x2206 lives at `device + 10572`**.

That derivation contradicted this file's note that VTE_CNTL is 0x300, so it was
checked before being acted on. Dumping the surrounding dwords settled it — 17
dwords below `+10572` are `640.0, 640.0, -90.0, 90.0, 1.0`, which are
`PA_CL_VPORT_XSCALE/XOFFSET/YSCALE/YOFFSET/ZSCALE` (0x210F..0x2113); the 0x2100
block's own shadow base (`device + 10444`) puts XSCALE at `10444 + 15*4 = 10504`,
exactly there. 640 is half of 1280. **The offset is right and the 0x300 note is
stale.**

The register reads **0x43F**, one value across every draw: all six viewport
enables set, `vtx_w0_fmt` set. The GPU applies the viewport, so the shader
exports clip space and the transform here is **identity**.

Applied held at 88.84%.

**RenderDoc A/B, frame 3000, NAT_Farm** (`legacy_mvp_tiebreak` existed so both
sides come from one binary). Captured twice: once as-is, once with
`hide_colorless_draws=true` to remove the overpaint.

As-is, both frames are a fullscreen quad split by one diagonal, in the *same*
place — only the colours differ. The present is dominated by the compositor, so
it cannot see what the transform did. Comparing backbuffers was the wrong
experiment.

With colourless draws hidden, neither frame shows recognisable geometry — both
are flat fills. But the fills differ in a way that matters: under the old rule
the compositor's fullscreen triangles covered a **corner sliver**, and under the
VTE-derived identity they cover the **entire viewport**, which is what a
fullscreen pass should do. The flat colour is the separate single-texel UV
collapse, not a transform error.

So the register reading is **supported, not proven**: the fullscreen pass now
lands full-screen, and no world geometry is visible either way because the
compositor still paints one texel over everything. **The UV collapse is now the
thing in front — it is what makes the frame unreadable, whichever transform is
in use.**

### Entry points

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
| `SetVertexShaderConstantF` | `0x82550320` | `CreateVertexDeclaration` | `0x82550B80` |
| `SetPixelShaderConstantF` | `0x825503F8` | `XGSetVertexDeclaration` | `0x82550A90` |
| `PatchVertexShaderToMatch…` | `0x82564C50` | `Swap` | `0x82566B58` |

IDA's auto-analysis defines no function at the two constant setters; the
addresses are confirmed by the byte match and by their own arithmetic (below).

### Device offsets

All read out of the consuming code, never guessed:

| Offset | What | Read from |
|---|---|---|
| `device + 0x780 + i*16` | VS float constant `i` | `SetVertexShaderConstantF`: `addi r10,r4,0x78` / `slwi r10,r10,4` / `add r10,r10,r3` |
| `device + 0x1780 + i*16` | PS float constant `i` | the twin setter at `0x825503F8`, `addi r10,r4,0x178` |
| `device + 0x480 + s*24` | texture fetch constant, sampler `s` | |
| `device + 0x2ED8` | current vertex declaration | `SetVertexDeclaration`'s `stw r4,0x2ed8(r3)` |
| `device + 0x3218` | viewport (clamped) | |
| `device + 0x3244` | current pixel shader | |
| `device + 10528` | `SQ_PROGRAM_CNTL` (0x2180) shadow | `DrawVertices`: `sub_82564768(device, 0, 8576, device + 10528)` |
| `device + 1920` / `+ 6016` | VS / PS constant flush shadows | `DrawVertices`: `sub_82564B00(device, dirty, 0x4000, device + 1920)` |
| `device + 1152 + off` | register shadow patched by shader objects | both shader setters walk an AND/OR list from the shader object |

Pixel shader object (not the device — the `D3DPixelShader*`). `D3DPixelShader`
is only the 24-byte `D3DResource` base, so IDA renders `pShader[1].Identifier`
as +0x28 and `pShader[2].ReadFence` as +0x3C:

| Offset | What | Read from |
|---|---|---|
| `ps + 0x18` | code allocation | `sub_825506B0` stores it there; `CreatePixelShader` (`0x82552148`) copies `a1[2]` bytes from `a1 + a1[1]` into it |
| `ps + 0x28` | copy of the source header | `CreatePixelShader`: `sub_82BFB9D8(v7 + 5, a1, a1[1])` |
| `ps + 0x30` | code **allocation** size — a bound, not the program length | |
| `ps + 0x3C` | offset of the constant-patch list within the +0x28 header | `SetPixelShader` walks `(ps + 0x28) + *(ps + 0x3C)` |
| `ps + 0x40` | offset of the program info block | shader flush `sub_82565928` |
| `*(ps + 0x40) + 0x28` | **byte offset of the CF stream inside the code allocation** | `sub_82565928`: `v22 = *((char*)v8 + v8[16] + 40) + v8[6]` |
| `*(ps + 0x40) + 0x2C` | program length in bytes (`>> 2` for dwords) | same, `*v23 = *(... + 44) >> 2` |

Vertex shader object. The same structure, different offsets, read out of
`sub_82565928`'s VS branch (raw disassembly at 0x82566234, not the decompiler's
folded arithmetic) and confirmed against
`D3D_PatchVertexShaderToMatchVertexDeclaration` (0x82564C50), which indexes the
identical `0x380 + variant*8` field:

| Offset | What | Read from |
|---|---|---|
| `vs + 0x20` | code allocation | `lwz r8, 0x20(r30)` — added to the CF offset to form the program address |
| `vs + 0x380 + variant*8` | offset of the info block | `slwi r10, r11, 3` with `r11 = variant + 112`, then `lwzx r11, r10, r30` |
| `info + 0x368` | **byte offset of the CF stream inside the code allocation** | `lwz r11, 0x368(r11)` then `add r11, r11, r8` |
| `info + 0x36C` | program length in bytes (`>> 2` for dwords) | `lwz r11, 0x36C(r11)` / `srwi r11, r11, 2` |
| `info + 0x384` | number of patchable vfetch instructions | the patcher's `v7` |

`variant` is the same 0/1 selector `sub_82565928` computes; the patcher takes it
as its own argument.

**The patched code is usually NOT in the shader's own allocation.** An earlier
version of this section claimed the opposite, reasoning that the patcher writes
to `a2 + 12*instruction_index` while `sub_82565928` hands the GPU
`*(vs + 0x20) + *(info + 0x368)`, so the two had to be the same buffer.
Measured: **the shader's allocation is the patched buffer in only 41 of 2561
captures.** The rest patch into the command ring or another pool. The field is
therefore the *canonical* program, not the live patched code, and cannot be
used as a source for it.

**What is universally true is that the CF stream starts exactly at the patch
destination** — 24 of 24 distinct shaders, then 2561 of 2561 captures, with the
fetch decode succeeding at that offset every time.

`CapturePatchedCode` used to read a 128-dword window backwards from that
destination and scan *upward* for the first offset whose fetch decode yielded
the expected count, so any false positive before the true start won. That cost
3 of 24 shaders (offsets -3, -3, -85). Trying `dest` first — still verified by
the same decode, not assumed — removed the failure mode: **off-dest resolutions
3/24 -> 0/2561, applied 87.62% -> 88.82%, `skipped stream` 8270 -> 0.**

It did **not** explain the c255 read. Shaders whose ALU probe reads index 255
went 91/136 to 81/128, which is noise. That stays open.

**The c255 reads are real, and the ALU index decode is not at fault.** Checked
in this order and all negative: the index is `src_reg & 0xFF` off the SDK's
Xenia-derived accessors, which are correct; the exec walk honours the sequence
bits so fetches are not decoded as ALU; and two genuine over-counting paths were
found and fixed (`Src(alu, 2)` evaluated for the src1-only opcodes kFrc/kTrunc/
kFloor/kMax4, and `VectorOp` running under `is_export()` with `vmask == 0`,
where nothing consumes the result). Fixing both moved incidence 81/128 to
72/128 — i.e. not at all.

What the measurement does show is that the index range is **bimodal**, with only
two values across 128 probes: `76..79` (56 shaders, ordinary) and `255..255`
(72 shaders, reading exactly one constant and nothing else). The second
population is the one-instruction compositor shader this file describes. So
those shaders really do read c255, which nothing writes, and hardware would have
read zero from it too.

That makes the remaining possibilities narrow: either c255 is published by a
D3D9 path that writes the ring directly rather than the device shadow (the
flush in `sub_82565928` does make such writes), or the constant file the
interpreter is handed is not the one those draws execute against.

**The CF stream does not start at the beginning of the code allocation.** Big
shaders carry a prologue that reads as zeros; small ones start at 0. That is
not a fixed 64-byte header to be inferred from a histogram — it is the field
above, and `sub_82565928` is what hands the resulting address to the hardware.

This replaced two workarounds: searching the blob for what PM4 had loaded, and
failing that, trying every offset and accepting a unique valid decode. Result
with PM4 off entirely: **40 of 49 pixel shaders decoded against PM4's 37 of
61, zero "ambiguous CF offset" (was 14), and every remaining rejection is a
genuine "no texture fetch"**. The seven previously-PM4-only shaders that
appeared in both runs produce byte-identical bindings.

`SQ_PROGRAM_CNTL` bits: `vs_num_reg[5:0]`, `ps_num_reg[13:8]`, `param_gen[18]`,
`gen_index_pix[19]`, `vs_export_count[23:20]`, `vs_export_mode[26:24]`,
`ps_export_mode[31:27]`. **`param_gen` is measured false** for the compositor
shaders, so PS r0 is a real interpolator there.

### Guest texture formats

The guest carries its own complete 64-entry `GPUTEXTUREFORMAT` name table: a
pointer array at **`0x82d24378`** (near-duplicate at `0x82d59d00`) indexing the
`FMT_*` strings in `0x820a9d00`-`0x820a9fd0` and `0x8205b5ec`-`0x8205b6d0`. Both
belong to the HLSL compiler embedded in the XEX (`sub_8263F9C0`,
`sub_82C1BB88`), so they are guest diagnostics, not the runtime texture path --
but the table is authoritative for the enum, and decoding it confirmed
index-for-index, from the game's own binary rather than from Xenia, that
`fetch.format` indexes the ordering `xenos.h` assumes. It is transcribed into
`GuestTextureFormatName` in [d3d9_texture.cpp](src/gpu/d3d9_texture.cpp).
Index 20 is spelled `FMT_4_5` in the guest, not `FMT_DXT4_5`; the value still
matches `k_DXT4_5 = 20`.

**The formats this game actually uses are a very short list.** Measured after
`ac278db` made the rejection name the format, three runs per configuration:

| Configuration | Formats the decoder was asked for and could not handle |
|---|---|
| front end, no `--force_load` | `FMT_4_4_4_4` only -- 3/3, byte-identical fetch words |
| `--force_load=NAT_Farm` | `FMT_8` (91), `FMT_16` (4), `FMT_4_4_4_4` (2) |
| after fixing those | `FMT_32_FLOAT` (71/90/79), which the earlier rejections had been hiding |

All four are handled as of `319a5c2`; both configurations now reject nothing.
`k_4_4_4_4` is the front end's own format and the only one it ever asks for --
independently corroborated, since it is also the format Xenia complains about
in plugin mode. The other three are single-channel and are decoded but never
bound as base colour.

Two traps in that decoder, both fixed, both worth remembering:

- `SwapBlock` worked in 4-byte units, so **any 2-byte format got no endian swap
  at all**. It was invisible for as long as `k_16_FLOAT` was the only 2-byte
  format and the semantic gate refused to bind it.
- The tiling helpers take a bytes-per-block *log2*, so a non-power-of-two block
  (`k_32_32_32_FLOAT` is 12 bytes) would mis-address every block silently. Now
  rejected explicitly.

Fixing one format can reveal another: a rejected candidate ends the binding
scan, so formats behind it are never reached. Re-measure after every addition
rather than assuming the list is now empty.

### Running these measurements

- `--skip_intro` only ever gated **mx's own FFmpeg host player**. The guest now
  opens its own Bink videos natively, so the intro plays regardless and eats
  roughly the first 45 s of any run. Budget 150 s, not 60 s.
- `--game_data_root=assets --user_data_root=userdata` are mandatory; without
  them the process exits immediately.
- `--hide_colorless_draws=true` on every run. Without it **no texture is
  visible on screen at all** -- the colourless overpaint covers textured draws
  rather than substituting for missing ones. This makes the shipped default
  (`false`) the untested configuration; it should be flipped.
- The front end does not always reach the UI within the window: roughly half of
  otherwise-identical runs produce no texture uploads. Zero uploads is not
  evidence of a regression; zero *rejections* is the signal to read.

### The `Type` dword in a vertex declaration

From `PatchVertexShaderToMatchVertexDeclaration`, the function that consumes it:

| Type bits | meaning |
|---|---|
| `[5:0]` | `xenos::VertexFormat` |
| `[8]` | `fomat_comp_all` — 1 = signed |
| `[9]` | `num_format_all` — 0 = normalized, 1 = integer |
| `[21:10]` | vfetch destination swizzle, x in `[12:10]`, w in `[21:19]` |

Bits `[9:8]` are what make this correct rather than merely plausible: `COLOR` is
`0x00182886` and `BLENDINDICES` is `0x001A2286` — same format 6 (`k_8_8_8_8`),
differing only there. The format size table is the guest's own, at `0x8204E188`,
indexed by the vfetch format field; illegal formats are 0.

Signed-normalized `k_2_10_10_10` (used for `NORMAL` and `TANGENT`) has no DXGI
equivalent. It passes through as `R10G10B10A2_UINT` with
`Unpack::kSnorm2_10_10_10` so the shader finishes the conversion.

### Render targets — done, and instrumented

Each guest colour surface gets its own host target. The device offsets are in
the Device offsets section; `EnsureGameRenderTarget` in
`src/gfx/d3d12_game.cpp` owns the routing, capped at `kMaxGameRenderTargets`
(64).

**The failure mode is silent:** when the cap is exhausted, or an object is
reused at a new size, `EnsureGameRenderTarget` returns nullptr and the caller
falls back to the main target — reinstating the overpainting this exists to
prevent. `game RT routing:` counts it, splitting draws that never wanted an
offscreen target from those that asked and were refused. Measured healthy:
**OVERPAINT 0, refusals 0, 22/64 live targets**, flat over a 420s run. Note the
D3D9 hook logs ~53 distinct render-target *objects* — only 22 become routable
targets, so that number is not the one to compare against the cap.

### State

With `--force_load=ST_Southwest --registry_override=ReadyToLaunch=1`:
**1482 / 1500 draws applied, 308,669 vertices**, skipping 12 stream + 6 vertex.
Geometry reaches the screen but is scrambled.

**FIXED 2026-08-06 — the UV collapse.** Two vfetch instructions are allowed to
target the same destination register and fill different components of it;
`FetchDestinationSwizzle::kKeep` (7) is what marks a component as not written.
The interpreter's attribute seeding ignored the destination swizzle and wrote
all four components, so the second fetch overwrote the first outright. Observed
directly: `attr[1] fmt=31 -> r0 = (22.969, -16.234, 0, 1)`, a real texcoord,
replaced by `attr[2] fmt=6 -> r0 = (0, 0, 0, 0)`. The shader then exported a
zero UV and the draw sampled one texel.

Honouring the swizzle takes **real collapses from 26 to 0** (draws sampling a
1024x1024 texture with a dead UV). Read the metric by texture extent or it
misleads badly: total "collapsed" only moves 89 to 79, because the remainder are
all **1x1** textures — the auto-exposure reduction chain, 64x64 -> 16x8 -> 4x2
-> 1x1 — where a UV sweep is meaningless by construction and collapse is
correct. Likewise 49 draws *left* the clean `(0,0)..(1,1)` bucket and that is
also correct: they are the same 1024x1024 world textures, now showing tiling
UVs beyond [0,1] instead of a perfect 0..1 sweep that only existed because the
register had been clobbered. The fullscreen post-process passes (160x90,
320x180, 640x360, 1280x720) keep unit range throughout.

**Confirmed visually.** Frame 3000, NAT_Farm, `hide_colorless_draws=true`: a sky
gradient with cloud texture, blue ground, and a tan strip — recognisable
textured content. The same configuration produced a flat brown fill before this
fix. A large black wedge across the middle remains, and is the next defect.

Without `hide_colorless_draws` the present is unchanged, because the compositor
passes already had unit UVs and were untouched by the swizzle change; what this
fixed was the 1024x1024 world draws sitting underneath the overpaint.

The original text follows, for the record. **The fullscreen post-process
triangles sample a single texel.** Their vertex shader is one instruction,
`MAD export0 = r0 * c255 + c255`, and c255 reads back `(0,0,0,0)`. The constant
file is populated (70 live vec4 across indices 0..218) and the read offset is
confirmed by the setter's own arithmetic, so the slot is genuinely never
published through the device shadow. The PM4 side showed the other publisher:
`LOAD_ALU_CONSTANT write reg=0x43F0 dwords=16` is register `0x4000 + 252*4`,
covering slots 252–255.

**The fix this file used to propose — source the constant file from the
`LOAD_ALU_CONSTANT` shadow — is no longer available.** That shadow lived in the
PM4 translator, deleted in 4dd1790. The ring is still parsed, but nothing
tracks constants from it, and reinstating that would be a PM4 dependency on the
render path, which is the thing the pure-HLE work removed.

**c255 is published by the shader itself, through a ring packet.** This file
said so originally, then two rounds of my own analysis wrongly concluded nothing
published it. Both are recorded here because the mistakes are instructive.

`sub_825656A0`, called from the draw-time flush as
`sub_825656A0(device, vs + 0x368, *(vs + 0x20))`, walks a table in the shader
object and emits one PM4 Type-3 packet per entry with header **0xC0022F00** —
opcode 0x2F, LOAD_ALU_CONSTANT — body `[source_address, 4 * reg, dword_count]`:

```
H = vs + 0x368;  P = H + *(H + 0x14)
P + 0x10  u32   list byte length;  entries at P + 0x14
entry: u16 reg_index, u16 dword_count, u32 data_offset   (8 bytes)
        terminated by dword_count == 0
source = *(vs + 0x20) + data_offset
```

Measured: every shader publishes one entry covering **c252..c255**, holding
screen-space scale/bias — `(0.5, -0.5, 0, 0)`, `(0, 1, 0.5, -0.5)`,
`(1, 2, 0.5, -0.5)`. `4 * 252 = 0x3F0`, and `0x4000 + 0x3F0 = 0x43F0` — the
exact `LOAD_ALU_CONSTANT reg=0x43F0 dwords=16` this file cited from the start.

None of it passes through `device + 0x780`, which is the only place
`CaptureVertexConstants` looked. `OverlayShaderConstants` now applies it after
the device file, matching hardware order (the load is emitted at draw time,
after any `SetVertexShaderConstantF`). The data is read from the shader object
in guest memory — the packet only carries an address — so **no PM4 is involved**
and the earlier retraction of the PM4-shadow fix still stands on its own terms.

Result: constant reads returning zero went **72 of 128 probes to 0 of 90**, the
live range went 0..218 to **0..255**, and c255 reads back `(0.5, -0.5, 0, 0)`.
Applied held at 88.85%. **stageI did not move** — 5 shaders at >=90% clip-like,
24% of execs, identical before and after. The constants were wrong and are now
right; that was not what the clip-volume problem is.

**Two ways I got this wrong, both worth remembering.** First I argued the ring
and the device shadow were the same memory, because `sub_82564B00` flushes
`device + 0x780` verbatim. True of *that* packet, and irrelevant: a second,
unrelated LOAD_ALU_CONSTANT exists. Proving one publisher is a copy does not
enumerate the publishers. Second, a probe did walk the right table but parsed it
with the neighbouring list's layout, read `reg_index` 0xFC as a byte offset,
compared it against c255's byte offset 0x12F0, and reported "none covers c255".
`0xFC` is c252 and the count covered c255. A field read in the wrong units is
indistinguishable from a negative result, and it survived because the answer it
gave was the one I already believed.

---

## Guest architecture

- Base `0x82000000`, XEX `assets/default.xex`. IDB at `assets/default.xex.i64`.
- Engine state pointer `dword_830BE400`; AssetDB at `*(0x830577C0)`.
- `dword_830B03EC` (GPU physical base) stays 0 in native mode and is not a
  blocker for asset loading.

### The asset load state machine — a detour, kept only for `force_load`

**Reverse-engineering the AssetDB was a wrong turn**, and a long one. ReXGlue
handles asset loading; the guest's file I/O works. The state machine is not
stuck — it idles because the layer above it never asks for anything, and that
layer is the Lua front end. Do not resume this thread. What follows is retained
only because `force_load` depends on it, and `force_load` is what gets a scene
on screen for rendering work.

`sub_8253AA40`, 12-case switch, state at `*(AssetDB + 28)`. Idles in state 2.
`sub_82534980(AssetDB, name, flags)` is the load-request API: it `strncpy`s up to
260 bytes of `name` into `AssetDB + 29540` (a `MAX_PATH` buffer, **not** a flag —
the places that appear to test a boolean are testing `name[0] != 0`) and, if the
state is 2, selects state 3.

With `force_load` + `ReadyToLaunch=1` the full sequence runs:
`2 → 3 → 4 (~380 ticks) → 5 → 6 → 7 → 8 → 11 → 2`.

The state 6 gate is `sub_8253CF80`: passes if network mode is Online(2) or
LAN(3), else if `*(0x83057900)` is set (state 1 clears it), else the registry's
`ReadyToLaunch`. Registry keys: `Location` `0x8200C864`, `PlayerMode`
`0x8200C870`, `ReadyToLaunch` `0x8204C630`. Getters: `sub_825487C8` (string),
`sub_82548758` (int), `sub_82548EA8` (int set).

### Known external blockers

- Binary `.xenon.package` heaps are encrypted (entropy ≈7.98, routine unknown;
  the guest's OpenSSL AES is TLS-only). **Possibly the root cause of the missing
  front end** — see above.
- FATAL crash at `0x82327CF0` during gameplay.

---

## Tooling

`tools/` holds BXML/package decoders (all three formats decode; the binary
package heaps only dump) and the IDA scripts. All 130 databases decode to
`out/asset_catalog.json`, 23,183 assets indexed — that catalog is how the MXUI
script list above was obtained.

### Scripted RenderDoc captures

`--rdoc_capture_frame=N` triggers a capture at presented frame N through the
in-application API, so captures no longer depend on pressing F12 at the right
moment ~115s into a run. The frame number is exactly what has to match for two
captures to be comparable, so hand-timing was the weakest part of any A/B.

```bash
renderdoccmd capture -d <repo> -c <out_prefix> ./mx.exe --game_data_root=assets --user_data_root=userdata --skip_intro=true --force_load=NAT_Farm --registry_override=ReadyToLaunch=1 --rdoc_capture_frame=3000
```

RenderDoc appends the frame number, giving `<out_prefix>_frame3000.rdc`.
`renderdoccmd thumb <file> -o out.png` extracts the embedded thumbnail, which is
enough to compare presents without opening the UI. The cvar does nothing unless
the process is running under RenderDoc.

### Headless IDA

IDA Pro v9 at `C:\Users\VM\Desktop\IDA Pro v9`; `hexppc.dll` is present, so the
PPC decompiler works.

```bash
cp assets/default.xex.i64 "$SCRATCH/work.i64" && "C:/Users/VM/Desktop/IDA Pro v9/idat.exe" -A -L"$SCRATCH/out.log" -S"tools/ida_dump_param_gen.py" "$SCRATCH/work.i64"
```

**Always work on a copy** — `idat` writes to the IDB. `-L<file>` is required:
`ida_kernwin.msg()` output does not reach stdout in batch mode.

API notes for IDA 9 (each of these cost a failed run): use
`idc.get_operand_value` and `idc.generate_disasm_line`, not the `ida_bytes`
spellings. `ida_funcs.add_func` will not always define a function at a matched
address; fall back to raw disassembly.

Existing scripts: `ida_dump_param_gen.py`, `ida_dump_vs_const255.py`,
`ida_dump_const_emitters.py`, `ida_dump_frontend_vtables.py`,
`ida_dump_script_api.py`, `ida_dump_execute_script.py`,
`ida_dump_patch_vertex_shader.py`, `ida_dump_render_targets.py`,
`ida_dump_texture_bind_order.py`, `ida_dump_d3d9.py`.

---

## Method

The one thing this project keeps re-learning, recorded because it has paid off
every time and guessing has failed first every time:

**Read the field out of the code that consumes it.** The `Type` bit layout came
from `PatchVertexShaderToMatchVertexDeclaration`'s own arithmetic; the format
size table was lifted verbatim from `0x8204E188`; the vertex declaration offset
came from `SetVertexDeclaration`'s single `stw`; the constant file layout came
from `SetVertexShaderConstantF`'s three instructions; `SQ_PROGRAM_CNTL`'s shadow
offset came from the argument `DrawVertices` passes. In each case an earlier
attempt to infer the answer from data produced something plausible and wrong.

Two corollaries:

- **A function too small to locate by pattern is still fully readable in the
  lib.** Not being able to find its address is not the same as not being able to
  learn from it.
- **Instrument to name failures, not to count them.** The stride-failure count
  sat unexplained for rounds; building the probe that said *which* draw failed
  resolved it immediately. The same move found the c255 constant and the script
  VM.
