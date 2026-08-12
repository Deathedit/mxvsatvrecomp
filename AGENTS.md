# MX vs. ATV Alive — ReXGlue native port

Static recompilation of the Xbox 360 build (`assets/default.xex`) via ReXGlue,
with a host D3D12 renderer replacing the Xenos GPU. The `rexgpu-xenos` plugin is
**not** used; native mode is the only supported profile.

> **Rewritten 2026-08-05, pruned again 2026-08-12.** The 08-05 rewrite cut
> ~4,000 lines of dated investigation logs whose "Current State" described a
> renderer several rounds out of date. By 08-12 it had drifted the same way —
> 1,286 lines, two thirds of them dated narrative, with the translated shader
> path absent entirely. That narrative now lives in
> [docs/renderer_history.md](docs/renderer_history.md); this file is the map.
>
> **This drift is the failure mode to watch.** Twice now the file has been
> read as current while describing a renderer that no longer existed. When you
> land a change that contradicts something here, fix it here in the same
> commit.

**This file holds:** how to build and run, the architecture, the cvars and their
defaults, and the method. Two companions hold the rest:

- **[docs/guest_binary.md](docs/guest_binary.md)** — addresses, vtables, struct
  offsets, register layouts, the IDA workflow. Properties of a shipped 2010
  binary; they never change. Where a section here drops a table, that is where
  it went.
- **[docs/renderer_history.md](docs/renderer_history.md)** — the dated
  investigation trail, 2026-08-05 to 08-07. Good derivations, superseded
  conclusions. Read it for how something was found, never for what is true now.

---

## Current state — 2026-08-12

**The renderer now runs the game's own shaders.** `shader_hlsl.cpp` translates
Xenos microcode to HLSL; ~73% of freeroam draws run guest pixel shaders. The
stand-in path is the fallback, not the norm.

Solved since this file was last accurate, each verified rather than inferred:

| | cause | commit |
|---|---|---|
| Black main menu | D3D9 legacy multiply — `0 * INF` is `+0` on Xenos, NaN on host | `28d4853` |
| White rider gear | `k_16_16(_16_16)` render targets are signed -32..32, not UNORM | `c3240a1` |
| Collapsed exposure | guest reads the 1x1 resolve out of guest RAM, so it needs writing back | `cd7b293` |
| Small textures wrong | 16px-and-under textures keep their base level in a packed mip tail | `4e03dd2` |
| Worker draws unshaded | `DeviceState` was thread-local; three record workers saw no bound shader | `2706801` |
| Heap corruption | three guest record workers, hooks now serialised, each with its own list | `7ff0ccf`, `71a6f01` |

**Performance, measured 2026-08-12 in the main menu.** The render tick was
1815 ms, of which 1031 ms was `AddGameDraw` and 743 ms was retirement — 97.7%
allocator, GPU idle. `AddGameDraw` was creating up to 9 committed UPLOAD-heap
resources per draw at ~683 µs each. Suballocating from a persistently-mapped ring
(`db4fea9`) took the tick to **57–82 ms** and `CreateCommittedResource` to zero.

**What is in front now.** A ~200 ms frame, of which `LOOP BY REASON` attributes
121 ms to 133 draws with no pixel shader — 144,097 vertices on the CPU
interpreter. Those draws have no pixel shader *bound at all*, which is legal:
the guest's PM4 emitter carries the pixel microcode inside the **vertex** shader
object at `vs + vs[226] + 872`, gated on `vs[218] & 0x20`. Not yet verified that
`[226]` is pixel rather than alternate-vertex microcode — probe it before wiring
it up.

**Present is capped at ~30 fps** by `Present(1,0)` plus a fixed 16 ms sleep, and a
tick with no new guest draws is skipped entirely (`8bcb09a`) — the swapchain is
flip-discard, so the last frame stays on screen. Frame rate must be read from log
timestamps, not from `FRAME COST` buckets.

---

## Build & Run

C++-only change (~15s):

```bash
cmake --build out/build/win-amd64-debug --target mx
```

Release, for anything where frame time is the measurement:

```bash
cmake --build out/build/win-amd64-release --target mx
```

Both build trees already exist; `win-amd64-relwithdebinfo` is configured too.
Note that a Debug build here has historically cost the *same* as Release on the
paths that mattered — when that is true it is evidence the cost is a host API
call rather than compute, which is how the `VirtualQuery` defect was identified.

The build writes to the build dir; the **root `mx.exe` is what runs** against the
assets in this directory, so copy it before running. Check the copy landed with
`grep -c` for a string you just changed — a session was lost to a stale root
binary whose probes were simply absent.

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

Also requires Visual Studio 2022 Clang 18+ and ReXGlue SDK 0.9.0 at
`C:\rexglue-sdk`. The FFmpeg 8.0 dependency was REMOVED 2026-08-06 — neither
the `ffmpeg/` tree nor the five av*/sw* DLLs are needed, and both are deleted.
(`rexruntime.dll` links FFmpeg statically; it never loaded those DLLs.)

### Running

```bash
./mx.exe --game_data_root=assets --user_data_root=userdata --force_load=NAT_Farm --registry_override=ReadyToLaunch=1
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
`src/hooks/hooks_d3d9.cpp`. **The default matters** — several of these are on,
so they describe what a plain run already does, not what you can turn on.

| cvar | default | |
|---|---|---|
| `hle_sanitize_constants` | **on** | Zero any non-finite pixel shader constant before upload |
| `d3d9_page_cache_persist` | **on** | Keep the page-readability cache across frames instead of clearing every swap |
| `hle_diag` | off | Per-draw and per-vertex diagnostics: transform probe, prim-type and vfetch censuses, fetch addressing self-check. They cost real frame time |
| `hle_capture` | off | Score every draw against the state shadow and report what fraction is fully described. Capture only — submits nothing |
| `hle_shader_exec` | 0 | Execute the bound guest vertex shader for one draw in N. Requires `hle_capture` |
| `hle_shader_verts` | 8 | How many vertices per executed draw. Only with `hle_shader_exec` |
| `d3d9_page_cache_verify` | off | Re-query the OS on every cache hit and log mismatches. Slow; correctness check |
| `d3d9_hooks_passthrough` | off | Pass the D3D9 hooks through in native mode. **Breaks rendering by design** — an A/B instrument, not a mode |
| `force_load` | — | Load a scene directly. A diagnostic lever, not a fix — see "Why there is no menu" |
| `registry_override` | — | Force a registry key, e.g. `ReadyToLaunch=1`. Same caveat |

**Six were retired on 2026-08-12**, by the same test that retired four on
2026-08-07: none had ever been flipped, so the other branch was dead weight that
still had to be reasoned about wherever it was read, and the shipped default was
a configuration nobody had tested.

| retired | was | frozen at |
|---|---|---|
| `hle_gpu_vertex` | on | on — the CPU interpreter is still exercised every frame by draws that do not qualify, so nothing went untested by dropping the flag |
| `hle_gpu_vertex_fetch` | on | on |
| `hle_shader_fetch_constants` | on | on — off meant ignoring constants shaders DMA themselves |
| `hle_ps_device_fallback` | on | on — off meant losing the bound shader on worker threads |
| `hle_texture_signs` | on | on — off meant rendering `kUnsignedBiased` without the 2*c−1 expansion |
| `hle_main_viewport_only` | off | **deleted** — its own help text called it superseded once render targets were modelled per surface |

The first five all had one read site and an "off" that only meant *be wrong*, so
freezing them changes no behaviour. `hle_main_viewport_only` defaulted off, so
deleting its branch changes none either.

**`hle_sanitize_constants` was NOT retired, deliberately.** Its help text is
stale — it says the menu's 3D layer is black because a shader takes +Inf into a
multiply, which was the hypothesis and was wrong; the cause was the D3D9 legacy
multiply, where `0 * INF` is `+0` on Xenos and NaN on host. But it defaults
**on** and is actively firing (`CONSTANTS sanitized: 3287 draws, 114270
components` in mx_1040), so removing it would be *choosing a behaviour*, not
deleting a dead branch. Run once with `--hle_sanitize_constants=false` and
compare before deciding.

Seven more (`alu_execute`, `skip_untransformable_draws`, `tint_by_color_source`,
`transcode_confirmed_formats_only`, `transcode_trust_export`,
`vertex_transcode`, `vfetch_use_shader_slot`) were defined in the PM4 translator
and went with it, as did `hle_render`, `pm4_translate` and `main_surface_only`.

Four bring-up cvars were retired on **2026-08-07**, each freezing the one
behaviour it was always run with: `native_res_viewport` (the guest's 1280x720
now always drawn 1:1 in the centre of the window, pillarboxed, whenever the
window is at least that big) and `clear_magenta` (the dark blue clear is the
only one), plus `hide_colorless_draws` and `hide_colored_draws` — **frozen in
the *submit* direction**, so every draw now reaches the renderer regardless of
colour source. That last pair is the important one: see the colourless
overpaint in [docs/renderer_history.md](docs/renderer_history.md).

**Unknown cvars are tolerated.** An old command line carrying
`--hide_colorless_draws=true` still runs and the flag silently does nothing —
which changes what the run renders. Check run lines against this list before
trusting a result.

`force_load` and `registry_override` are **diagnostic levers, not fixes** — see
"Why there is no menu" in
[docs/renderer_history.md](docs/renderer_history.md).

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
| `src/gfx/d3d12_game.cpp` | Game pipeline, per-frame draw list, game RT + depth, HLE offscreen targets, compositor |
| `src/gpu/pm4_parser.*` | PM4 command buffer parser (Type-0/2/3, ring wrap, dump) |
| `src/gpu/hle_types.*` | Shared HLE data model (`DrawCall`, textures, topology) + the two primitive expansions |
| `src/gpu/xenos_gpu_state.*` | Xenos register shadow, snapshot/diff |
| `src/gpu/d3d9_layout.*` | Guest `D3DVERTEXELEMENT9` array → host input layout |
| `src/gpu/d3d9_draw.*` | HLE draw assembly from bound streams |
| `src/gpu/d3d9_state.*` | D3D9 device state shadow (render targets, resolves) |
| `src/gpu/d3d9_texture.*` | Texture profile / bound-texture resolution |
| `src/gpu/shader_ucode.*` | Xenos microcode decode: vertex fetches, pixel texture profiles |
| `src/gpu/shader_hlsl.*` | **Xenos microcode → HLSL.** The translated shader path — what makes a draw run the guest's own shader instead of a stand-in |
| `src/gpu/shader_alu.*` | Vertex shader ALU interpreter (the CPU fallback for untranslated draws) |
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

### Two shader paths, and which one a draw takes

Every draw runs one of two ways, and most confusion about this renderer comes
from not knowing which:

- **Translated** — `shader_hlsl.cpp` turns the guest's Xenos microcode into HLSL
  and the draw runs the game's own shader. This is the real path.
- **Stand-in** — the draw runs one of four fixed host shaders (`kGameVS`,
  `kGamePS`, `kGameTexturePS`, `kGameYuvPS`, in `d3d12_shaders.h`).
  `kGameTexturePS` computes `tex * col` and nothing else, so a draw on this path
  renders *something* but not what the game asked for.

A draw falls to the stand-in when either stage fails to translate. **The gate
needs both stages**, which has a cost that is easy to miss: a pixel shader that
does not translate also disqualifies its draw from the GPU vertex path, so all
of its vertices drop to the CPU interpreter in `shader_alu.cpp`. One untranslated
stage therefore makes a draw both wrong and slow, and the slowness shows up
attributed to vertex work rather than to the shader.

`LOOP BY REASON` in the log is the breakdown of why draws fell back, with the
vertex count and milliseconds each reason cost.

---

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

## D3D9 HLE rendering

The working approach. D3D9 is linked statically into the XEX, so there is no
import table; how the entry points were located anyway, and the addresses
themselves, are in [docs/guest_binary.md](docs/guest_binary.md).

### Entry points and device offsets

The 22 guest D3D9 entry-point addresses, the device struct offsets with their
PPC derivations, and the pixel- and vertex-shader object field maps are all in
[docs/guest_binary.md](docs/guest_binary.md). All were read out of the consuming
code, never guessed.

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
not a fixed 64-byte header to be inferred from a histogram — it is a field in
the shader object, and the shader flush is what hands the resulting address to
the hardware.

This replaced two workarounds: searching the blob for what PM4 had loaded, and
failing that, trying every offset and accepting a unique valid decode. Result
with PM4 off entirely: **40 of 49 pixel shaders decoded against PM4's 37 of
61, zero "ambiguous CF offset" (was 14), and every remaining rejection is a
genuine "no texture fetch"**. The seven previously-PM4-only shaders that
appeared in both runs produce byte-identical bindings.

`SQ_PROGRAM_CNTL`'s bit layout is in
[docs/guest_binary.md](docs/guest_binary.md). **`param_gen` is measured false**
for the compositor shaders, so PS r0 is a real interpolator there.

### Guest texture formats

The guest carries its own complete 64-entry `GPUTEXTUREFORMAT` name table, and
decoding it confirmed index-for-index — from the game's own binary rather than
from Xenia — that `fetch.format` indexes the ordering `xenos.h` assumes. Table
addresses and provenance: [docs/guest_binary.md](docs/guest_binary.md). It is
transcribed into `GuestTextureFormatName` in
[d3d9_texture.cpp](src/gpu/d3d9_texture.cpp).

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

- **`--skip_intro` no longer exists.** It only ever gated mx's own FFmpeg host
  player, which was REMOVED 2026-08-06 along with the whole `ffmpeg/`
  dependency -- the guest opens and decodes its own Bink videos, and while both
  existed the intro visibly played twice. The guest intro plays regardless and
  eats roughly the first 45 s of any run, so budget 150 s, not 60 s. Unknown
  cvars are tolerated, so an old command line with `--skip_intro=true` still
  runs; the flag simply does nothing.
- `--game_data_root=assets --user_data_root=userdata` are mandatory; without
  them the process exits immediately.
- **The colourless overpaint — DATED 2026-08-07, do not read as current.** As
  written: draws that resolve no colour are written opaque white and paint over
  the scene, so no texture is visible in a default run. This predates the whole
  translated shader path and the six fixes listed under "Current state" — in
  particular the legacy multiply (`28d4853`), which is what actually blacked out
  the menu and was at the time being attributed to this. Whether a colourless
  overpaint still occurs has not been measured since. The measurements are in
  [docs/renderer_history.md](docs/renderer_history.md); treat them as a symptom
  trail, not a description of what is on screen.
  (`--hide_colorless_draws=true`, the workaround referenced throughout, was
  retired 2026-08-07 and no longer exists.)
- The front end does not always reach the UI within the window: roughly half of
  otherwise-identical runs produce no texture uploads. Zero uploads is not
  evidence of a regression; zero *rejections* is the signal to read.

### The `Type` dword in a vertex declaration

Its bit layout, and the guest's own format size table, are in
[docs/guest_binary.md](docs/guest_binary.md) — both read out of
`PatchVertexShaderToMatchVertexDeclaration`, the function that consumes them.

Signed-normalized `k_2_10_10_10` (used for `NORMAL` and `TANGENT`) has no DXGI
equivalent. It passes through as `R10G10B10A2_UINT` with
`Unpack::kSnorm2_10_10_10` so the shader finishes the conversion.

### Render targets — instrumented, and the cap was three short

Each guest colour surface gets its own host target. The device offsets are in
[docs/guest_binary.md](docs/guest_binary.md); `EnsureGameRenderTarget` in
`src/gfx/d3d12_game.cpp` owns the routing, capped at `kMaxGameRenderTargets`
(256 since 2026-08-12; it was 64).

**The failure mode is silent:** when the cap is exhausted, or an object is
reused at a new size, `EnsureGameRenderTarget` returns nullptr and the caller
falls back to the main target — reinstating the overpainting this exists to
prevent. `game RT routing:` counts it, splitting draws that never wanted an
offscreen target from those that asked and were refused. `m_gameRenderTargets`
is **never evicted**, so once the cap trips it stays tripped for the rest of the
run.

**This section previously read "Measured healthy: OVERPAINT 0, refusals 0,
22/64 live targets, flat over a 420s run." That was true and it was
misleading** — it had only ever been measured on a menu-only run. A loaded level
wants **67**, so the old cap of 64 was three short:

| `--force_load=NAT_Farm` | cap 64 (mx_1036) | cap 256 (mx_1037) |
|---|---|---|
| peak live targets | 64/64, pinned | **67**/256 |
| OVERPAINT | 2765 | **0** |
| refused: budget | 5530 | **0** |
| srv | 301/1024 | 299/1024 |

That overpaint is what made `--force_load` useless for looking at level
geometry: level draws were refused their own targets and painted onto the shared
one. **Measure this subsystem with a level loaded** — the front end fits
comfortably and will report healthy no matter how wrong the cap is.

Note the D3D9 hook logs ~53 distinct render-target *objects* — only some become
routable targets, so that number is not the one to compare against the cap.

## Guest architecture

Base address, engine state pointers, the asset load state machine that
`force_load` depends on, and the known external blockers are all in
[docs/guest_binary.md](docs/guest_binary.md).

**Reverse-engineering the AssetDB was a wrong turn**, and a long one. ReXGlue
handles asset loading; the guest's file I/O works. The state machine is not
stuck — it idles because the layer above it never asks for anything, and that
layer is the Lua front end. Do not resume this thread. It is documented only
because `force_load` depends on it, and `force_load` is what gets a scene on
screen for rendering work.

---

## Tooling

`tools/` holds BXML/package decoders (all three formats decode; the binary
package heaps only dump) and the IDA scripts (listed in
[docs/guest_binary.md](docs/guest_binary.md)). All 130 databases decode to
`out/asset_catalog.json`, 23,183 assets indexed — that catalog is how the 57-asset
MXUI script list above was obtained.

**RenderDoc integration was removed on 2026-08-12 (`9e76958`).** The
`--rdoc_capture_frame=N` cvar and the in-application capture trigger are gone.
Several findings below were made with it and are still valid as findings; the
method is simply no longer available in-tree. Nothing in this file's run lines
should carry that flag — unknown cvars are tolerated, so it would run and
silently do nothing.

### Headless IDA

Batch invocation, the work-on-a-copy rule, the IDA 9 API gotchas and the ten
`ida_dump_*.py` scripts are in [docs/guest_binary.md](docs/guest_binary.md).

---

## Method

The one thing this project keeps re-learning, recorded because it has paid off
every time and guessing has failed first every time:

**Read the field out of the code that consumes it.** The `Type` bit layout, the
vertex declaration offset, the constant file layout and `SQ_PROGRAM_CNTL`'s
shadow offset were each read out of the guest function that uses them, and the
format size table was lifted verbatim from the guest's own table — every
derivation is recorded beside its result in
[docs/guest_binary.md](docs/guest_binary.md). In each case an earlier attempt to
infer the answer from data produced something plausible and wrong.

Two corollaries:

- **A function too small to locate by pattern is still fully readable in the
  lib.** Not being able to find its address is not the same as not being able to
  learn from it.
- **Instrument to name failures, not to count them.** The stride-failure count
  sat unexplained for rounds; building the probe that said *which* draw failed
  resolved it immediately. The same move found the c255 constant and the script
  VM.
