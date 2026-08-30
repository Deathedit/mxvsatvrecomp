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
> **Reconciled again 2026-08-17**, against the 54 commits since 08-12. Four
> claims in "Current state" had become false — the stand-in rate, the ~30 fps
> Present cap, an unverified `vs[226]` probe, and a "what is in front now" that
> described a frame no longer being drawn. The architecture table still listed
> two hooks that had been deleted.
>
> **This drift is the failure mode to watch.** Three times now the file has been
> read as current while describing a renderer that no longer existed. When you
> land a change that contradicts something here, fix it here in the same
> commit — and see "Write comments that cannot go stale" under Method for the
> phrasing that avoids the problem instead of paying it down later.

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

## Current state — 2026-08-17

**The renderer runs the game's own shaders, and the translator is no longer the
bottleneck.** `shader_hlsl.cpp` turns Xenos microcode into HLSL; the stand-in
path is the exception. Where a defect used to be "this shader did not
translate", it is now almost always "this shader translated and was handed the
wrong resource".

Solved since 2026-08-12, each verified by pixels or by a reference, not inferred:

| | cause | commit |
|---|---|---|
| Rider colour missing | a scalar ALU result was dropped when the write mask was clear | `0304a48` |
| No mipmaps | only the base level was uploaded; the guest's chain is at `fetch.mip_address` | `c7740b4` |
| Red screen / unlit scene | `tfetch3D` untranslated, and only 16 of 32 fetch constants were read | `5e176bd`, `fde31ea` |
| Bike drawn behind the UI | `RB_DEPTHCONTROL` was never read; `z_write` was fabricated from `z_enable` | `93c0119` |
| Filled quads / brake rotor | the alpha test was not honoured | `93c0119` |
| Water surface flat | `setp_*_push` (opcodes 20–23) refused the whole shader | `dcd5a62` |
| UI atlas 200ms/frame | an unreachable skip guard in the coverage path | `e9573a5` |
| Ground missing 16% of frame | `0xFFFF` read as vertex 65535 instead of a strip cut | primitive restart |

**Performance, measured 2026-08-12 in the main menu — still the last full
profile.** The render tick was 1815 ms, of which 1031 ms was `AddGameDraw` and
743 ms was retirement — 97.7% allocator, GPU idle. `AddGameDraw` was creating up
to 9 committed UPLOAD-heap resources per draw at ~683 µs each. Suballocating from
a persistently-mapped ring (`db4fea9`) took the tick to **57–82 ms** and
`CreateCommittedResource` to zero.

**Present no longer paces the guest.** `kPresentSyncInterval` is 0 and the fixed
16 ms sleep is gone (`2e7f74b`); the earlier "~30 fps cap" in this file described
`Present(1,0)` plus that sleep and is obsolete. A tick with no new guest draws is
still skipped (`8bcb09a`) — the swapchain is flip-discard, so the last frame
stays on screen. **Frame rate must be read from log timestamps, not from `FRAME
COST` buckets**, and a renderer that has died reports as a plausible-looking low
frame rate rather than as an error.

**What is in front now — the open defects are all one shape.** Content is
produced correctly and then not consumed correctly:

- the **grey intro**: the Bink video decodes and draws real colour, the guest's
  own clear to `0xFF808080` is correct, and then a final fullscreen composite
  samples something degenerate and returns *bit-identical output at three
  widely separated UVs*
- the **menu backdrop**: `FE_Smoke` draws, resolves to a 1280x430 texture, and
  is never sampled
- **banded depth resolves**: both bands drawn, but the resolve names a third
  surface object no draw ever bound

None of these are translation failures. They are resource identity — we bind by
D3D9 object handle and then repair the mapping with heuristics (`aliasedSource`,
`msaaPartner`, `contains`, `blank_exact`, `addr_match`, the binding scoring
switch, `SLOT MAP`). Each heuristic works until it doesn't.

**A replacement for that layer is being evaluated — see "The Xenia Edge shader
bridge" below.** Read it before starting new work on the translator or on the
binding heuristics; both are candidates for deletion.

**Three shaders currently translate WRONG rather than not at all**, which is
worse than a refusal because they render confidently: `ps_267BFA20`,
`ps_267D2620` and `vs_27084F60` walk `kCondExecPred` blocks as plain execs, so
bodies the console gated on `p0` run unconditionally.

**The alpha-test stand-in count is not a regression — resolved 2026-08-17 from
`mx_1279` without a new run.** It reads `STAND-IN 0` after `93c0119` and
`STAND-IN 125` later, and both are from the *same run*: the counter held at 0
for four and a half minutes across 296,529 honoured draws, then went 0 -> 14 ->
125 in three seconds and stayed flat. The ALU constant file crosses
`483 -> 494 distinct constants` in the same window, so a new shader set entered
the frame and brought 125 draws (0.03%) the alpha test cannot honour.

Worth keeping as a worked example: **two readings of one counter, four minutes
apart in one run, look exactly like a regression across two runs.** Trace a
counter's whole timeline before comparing endpoints, and see
[[measure-the-right-population]].

---

## Build & Run

C++-only change (~15s):

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
rexglue codegen --force mx_manifest.toml && cmake --preset win-amd64-release && cmake --build out/build/win-amd64-release --target mx
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
./mx.exe --game_data_root=assets --user_data_root=userdata
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
| `d3d9_page_cache_persist` | **on** | Keep the page-readability cache across frames instead of clearing every swap |
| `hle_diag` | off | Per-draw and per-vertex diagnostics: transform probe, prim-type and vfetch censuses, fetch addressing self-check. They cost real frame time |
| `hle_capture` | off | Score every draw against the state shadow and report what fraction is fully described. Capture only — submits nothing |
| `hle_shader_exec` | 0 | Execute the bound guest vertex shader for one draw in N. Requires `hle_capture` |
| `hle_shader_verts` | 8 | How many vertices per executed draw. Only with `hle_shader_exec` |
| `d3d9_page_cache_verify` | off | Re-query the OS on every cache hit and log mismatches. Slow; correctness check |
| `d3d9_hooks_passthrough` | off | Pass the D3D9 hooks through in native mode. **Breaks rendering by design** — an A/B instrument, not a mode |
| `d3d9_diag_row_heartbeat` | 16 | Draw reports an unchanged population may pass before `UP CALLERS`, the per-config stencil rows and the per-declaration `decl-draws` rows dump anyway. A new call site or config still prints immediately; `0` = only on change, `1` = every report |
| `d3d9_diag_frame_every` | 30 | Frames between the per-swap lines (`FRAME DRAWS`, `UNBUILT *`, `RING vs HLE`, `ALU LOAD`). Per-frame deltas accumulate across the gap rather than being dropped, and the first five swaps always print; `0`/`1` = every swap |
| `dev` | — | Developer switches, comma separated. `menu` sets the guest's `DEFINE_BuildConfig` Lua global to `DEBUG`, which its own scripts gate the dev menu on. `print` captures the guest's `print()` to `logs/guest_print.log` (retail binds print to a no-op stub, so this is the only way to see it). `native:census` lists the callers of `sub_829E8FA8` without changing anything; `native:<hex>` answers 1 for that one return address (`0x82AB6638` is the DebugOverlay enable check); `native:all` crashes. `bind:<Button>=<Action>` adds a debug input binding, off unless asked for |
| `registry_override` | — | Force a registry key, e.g. `ReadyToLaunch=1`. A diagnostic lever, not a fix |

**Seven were retired on 2026-08-12**, by the same test that retired four on
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

**`hle_sanitize_constants` was retired last, and froze OFF rather than on.** It
zeroed every Inf and NaN in the pixel constant bank before upload — 1,159,248
components across 32,191 draws in a single run, about 36 of the bank's 1,024
components on every draw.

Its own comment called it *"EXPERIMENT, not a claimed fix"* and set the exit
condition: *"If the picture comes back, they were garbage and this is the fix.
If it does not, they were meaningful and this is ruled out."* That experiment was
never cleanly concluded — the picture did come back, but from the legacy
multiply fix (`28d4853`) landing separately, which confounded the result, and the
flag stayed on by inertia.

Measured 2026-08-12 with `--hle_sanitize_constants=false` (mx_1043): **no visible
difference**, with over a million components per run no longer zeroed. So nothing
indexes those registers, exactly as the comment predicted — the guest leaves that
part of its bank uninitialised and its shaders never read it.

It froze **off**, not on, because not zeroing is what the console does. Where a
shader *did* index such a register, hardware would read `+Inf` and we were
substituting `0` — making us the ones diverging. Keeping it would have been
picture quality bought with correctness, the same trade `hide_colorless_draws`
was retired for.

*Caveat on scope: this was tested in the front end. If a loaded level ever shows
non-finite constants reaching a shader that reads them, the symptom would be NaN
output, and the instrument for it is the `NONFINITE` probe in `1078e5f`.*

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
| `src/hooks/hooks_boot.cpp` | Bootstrap, GraphicsInit, TexManager, GpuAlloc |
| `src/hooks/hooks_loading.cpp` | SetupRenderer, Transition, LoaderTick |
| `src/hooks/hooks_gameloop.cpp` | RenderPipeline |
| `src/hooks/hooks_wait.cpp` | Two guest-wait passthroughs, nothing else |
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

### The Xenia Edge shader bridge — evaluated 2026-08-17, not yet adopted

**Before starting work on `shader_hlsl.cpp`, `shader_alu.cpp`, `shader_ucode.cpp`
or the binding heuristics, read this. All of them are deletion candidates.**

The sibling project at `C:\Users\VM\Desktop\sr` links **Xenia's real
`DxbcShaderTranslator`** rather than hand-writing one.
`tools/xenia_edge_bridge/xenia_edge_shader_bridge.cpp` is 266 lines of C ABI over
`DxbcShader` + `DxbcShaderTranslator`, built as `shaders.dll` from 13 Xenia
source files (tree at `C:/xenia-edge`) plus ~60 lines of stubs. It emits SM 5.1
DXBC directly, so FXC never runs.

Measured 2026-08-17 against **our** corpus,
`C:\Users\VM\Desktop\xenia_dump\shaders_dump_mx` — 226 pixel + 75 vertex.
(`shaders_dump` beside it is Saints Row's, not ours.)

    translated 301/301   REJECTED 0   valid DXBC on all 301
    62ms total — median 0.14ms, p95 0.59ms, max 1.40ms

Confirmed against a second, independent corpus — **our own** `logs/hlsldump`,
which is what the runtime actually encountered rather than what Xenia happened
to trace. 302 of its 420 files carry a `=== GUEST MICROCODE ===` section:

    translated 302/302   (pixel 184/184, vertex 118/118)   REJECTED 0

Different population from the Xenia dump (226/75 there, 184/118 here), same
result. Against FXC's 18–145 ms per shader, either corpus translates end to end
in less time than a single FXC compile — which would retire the persistent DXBC
cache along with the translator.

The harness is `scratchpad/bridge_corpus.py`: ctypes over `shaders.dll`, no
compiler needed, and it reads both corpus layouts. It is falsifiable —
byte-swapping the microcode takes it red at once.

**The translator is not the real prize.** `sr` had Bink and most of its UI
running in 1–2 hours, because of `d3d12_renderer.cpp:1499`: the *shader* declares
which fetch constant it reads, the *fetch constant* names the guest address and
format, and the texture is decoded from there. Nothing is guessed. That removes
the *need* for our binding heuristics rather than improving them.

Carry these into any adoption:

- **The bridge access-violates on malformed microcode** — it does not return an
  error. Its `try/catch` catches C++ exceptions; an AV is SEH and goes past it.
  Harmless in `sr` (microcode from files), not here (live guest memory).
- `sr` fills `std::array<uint32_t,148> system_constants` **by raw index**, rest
  zero, with no compile-time check. Add `static_assert`s against Xenia's struct.
- **Byte-exactness against Xenia is NOT established and must not be claimed.**
  The bridge is deliberately bindful; Xenia's dump is bindless, so `RDEF` and
  `SHEX` differ by construction. `ISGN`/`OSGN`/`SFI0` *are* byte-identical, so
  the interface matches and only binding strategy diverges.
- Step 0 proves **no coverage gap**. It does not prove the output renders
  correctly — that lives in the binding contract.

Order if adopted: (1) bridge + guest root signature, (2) texture binding by fetch
constant — where the deletions land, (3) shared-memory SRV/UAV, separable and
last. Expect (1)–(2) to take out two-thirds of the renderer with nothing on
screen for a stretch.

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

**The translated-PSO cache has the same shape and the same silent failure.**
`kMaxTranslatedPSOs` was 256 and is **4096** since `b38e350`; when it fills,
every new shader/state combination from that point renders as a stand-in. It now
says so once, loudly, instead of failing quietly. `mx_1264` was the evidence:
331,785 of 1,312,382 draws — 25% — were lost to the old cap. Runs since have
plateaued around 205, so the raised cap is untested by anything but the number
that justified it.

Both caps share a lesson worth stating once: **a resource cap that falls back to
something that still paints is invisible in the picture.** It shows up as the
wrong thing being drawn, never as an error, and only a counter that names the
refusal will find it.

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

### Where runtime dumps land

**Everything goes under `logs/`, one directory per kind.** Until 2026-08-17 the
PM4 and vertex-declaration dumps wrote next to the executable instead, where
137 files (14 MB) had accumulated in the project root — invisible because
`.gitignore` covered them.

| directory | what | lifetime |
|---|---|---|
| `logs/pm4dump/` | `<tag>_frame_NN.txt`, `<tag>_swap_NN.txt` — parsed PM4 for spot-check swaps | wiped once per process |
| `logs/hlsldump/` | per-shader HLSL, guest microcode, DXBC disassembly | wiped once per process |
| `logs/decldump/decls.txt` | vertex declarations captured during load | truncated per run |
| `logs/constdump/` | ALU constant files, per shader | accumulates |
| `logs/texdump/` | decoded guest textures as PNG, plus `index.txt` | wiped once per process |

`logs/texpng/` was listed here until 2026-08-17 and **nothing has ever written
it** — the row survived from a dump that was removed. The real texture dump is
`logs/texdump/`, off unless `texture_dump = true`; see `src/hooks/texture_dump.h`
for what it can and cannot see (only textures that reach a sampler).

The two "wiped once per process" directories are wiped **lazily, at the first
dump of a run** — so a run that dumps nothing leaves the previous run's files
intact to be read. Both are named in ways that repeat or collide across runs,
which is what makes the wipe necessary; a stale file from an earlier binary
being read as evidence about the current one has cost real time before.

**PM4 dumps are written on swaps 1–20, 300, 600, 1000, then every 500** —
not every frame.

**In-tree RenderDoc integration was removed on 2026-08-12 (`9e76958`).** The
`--rdoc_capture_frame=N` cvar and the in-application capture trigger are gone.
Nothing in this file's run lines should carry that flag — unknown cvars are
tolerated, so it would run and silently do nothing.

**RenderDoc itself is very much still the tool of choice**, via the external UI
and the RenderDoc MCP: `.rdc` files open and replay, and `pixel_history`,
`debug_pixel`, `pick_pixel` and `export_render_target` all work against them.
Ask for a capture instead of adding instrumentation — several of 08-15..08-17's
findings came straight out of one. Two traps worth carrying:
`pick_pixel(eventId)` shows state *after* that event, so use `pixel_history`
when the question is "which event wrote this pixel"; and an exported PNG shows
alpha-0 pixels as white, so confirm colours with `pick_pixel`, never from an
image.

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

### Write comments that cannot go stale

This project moves fast enough that any comment describing how things *are* is
wrong within days, and a confidently wrong comment costs more than no comment.
The fix is not to write fewer of them — it is to write the kind that stays true.

**Never write a bare present-tense claim about behaviour. Date it, and say what
was measured.**

    BAD   // ~73% of freeroam draws run guest pixel shaders.
    GOOD  // Measured 2026-08-12 (mx_1043, menu): 73% of draws ran guest
          // pixel shaders; the rest fell to the stand-in.

The second never becomes false — it is a record of an observation, and a reader
five days later can see its age and decide whether to re-measure. The first
silently rots into a lie. Every comment in this codebase that has had to be
retracted was of the first kind; the dated measurements have all held up.

The same rule is why this file's "Current state" section carries a date in its
heading, and why that date is the first thing to distrust. **This file has now
been read as current while describing a renderer that no longer existed three
times** — 08-05, 08-12, and again on 08-17, when it still claimed a ~30 fps
Present cap that had been removed and a `vs[226]` probe that had since been done.
When you land a change that contradicts something here, fix it here in the same
commit.

Two things that follow:

- **Explain *why*, not *what*.** The what is in the code and changes with it;
  the why is a fact about the guest or the hardware and usually does not. A
  comment recording that Xbox `D3DBLEND` is not the PC enum, or that
  `D3DCLEAR_ZBUFFER` is `0x10`, will be true forever.
- **A comment that records a dead end is worth as much as one that records a
  fix**, and it does not rot either. Several sections here exist only to stop
  the same wrong turn being taken a third time.
