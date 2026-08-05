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
./mx.exe --game_data_root=assets --user_data_root=userdata --skip_intro=true --hle_render=true --hide_colorless_draws=true --hle_shader_exec=1
```

`--game_data_root` and `--user_data_root` are mandatory — without them the
process exits immediately with code 1 and writes no useful log. Each run appends
a new `logs/mx_NNN.log`.

**The HLE render path does nothing without `--hle_render=true
--hle_shader_exec=1`.** A run with only `--skip_intro` produces D3D9 state
tracking and resolves but zero HLE draws, which reads exactly like a regression.

To reach a loaded scene, add `--force_load=ST_Southwest
--registry_override=ReadyToLaunch=1`. **`force_load` fires about 115 seconds
after start**, so a run shorter than ~2½ minutes never reaches it.

### Runtime cvars

`mx.toml` or `--flag=value`. Defined in `src/app/graphics_system.cpp` and
`src/hooks/hooks_d3d9.cpp`:

`alu_execute`, `clear_magenta`, `force_load`, `hide_colored_draws`,
`hide_colorless_draws`, `hle_capture`, `hle_main_viewport_only`, `hle_render`,
`hle_shader_exec`, `hle_shader_verts`, `main_surface_only`, `registry_override`,
`skip_intro`, `skip_untransformable_draws`, `tint_by_color_source`,
`transcode_confirmed_formats_only`, `transcode_trust_export`,
`vertex_transcode`, `vfetch_use_shader_slot`.

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
| `src/gpu/pm4_translator.*` | PM4 → `DrawCall`; shadows fetch constants and context registers |
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

## Why there is no menu (2026-08-05)

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
  `IN_Loading`, `IG_WorldLoading`, `SH_LuaDataProvider`. **`IN_BootStrapper`
  never runs**, so nothing drives the front end.
- The VM's own native-call dispatcher (`sub_82AA7638`, identified from
  `ExecuteScriptAsset`'s caller `lr=0x82AA78F4`) fires **4 times in an 80-second
  run, all within the first 1.6 seconds**, then goes silent. The probe is
  rate-limited to one line per 5s after the first four, so the silence is real.
- Corroborating: the guest reads **exactly one registry key** in a whole run —
  `PlayerMode`, by the loader's own gate. `Location`, the key naming the scene to
  load, is never read.

**Open question, and the obvious next move.** The binary `.xenon.package` heaps
are encrypted (entropy ≈7.98, routine unknown). If the UI scripts live in those
heaps, that blocker is not a side issue — it is the reason there is no menu. Not
yet measured: whether `IN_BootStrapper` fails to load or is never asked for.

`PlayerMode = "None"` is index 4 of the game's own five-value vocabulary
(`SplitScreen`, `SinglePlayer`, `Online`, `LAN`, `None`; the *failure* value is
5). It is the expected value before a menu has chosen a mode — a symptom, not a
cause.

---

## D3D9 HLE rendering

The working approach. D3D9 is linked statically into the XEX, so there is no
import table — but static linking removes the imports, not the functions.
Entry points were located by matching COFF symbols from a genuine Xbox 360
`d3d9.lib` (machine `0x01F2`, PowerPC BE); `tools/match_d3d9.py` regenerates the
patterns. **FLIRT/FLAIR does not work here and is not needed** — the COFF symbol
tables parse directly and give more.

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

`SQ_PROGRAM_CNTL` bits: `vs_num_reg[5:0]`, `ps_num_reg[13:8]`, `param_gen[18]`,
`gen_index_pix[19]`, `vs_export_count[23:20]`, `vs_export_mode[26:24]`,
`ps_export_mode[31:27]`. **`param_gen` is measured false** for the compositor
shaders, so PS r0 is a real interpolator there.

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

### State

With `--force_load=ST_Southwest --registry_override=ReadyToLaunch=1`:
**1482 / 1500 draws applied, 308,669 vertices**, skipping 12 stream + 6 vertex.
Geometry reaches the screen but is scrambled.

**Known defect — the compositor UV collapse.** The fullscreen post-process
triangles sample a single texel. Their vertex shader is one instruction,
`MAD export0 = r0 * c255 + c255`, and c255 reads back `(0,0,0,0)`. The constant
file is populated (70 live vec4 across indices 0..218) and the read offset is
confirmed by the setter's own arithmetic, so the slot is genuinely never
published through the device shadow. The PM4 side shows the other publisher:
`LOAD_ALU_CONSTANT write reg=0x43F0 dwords=16` is register `0x4000 + 252*4`,
covering slots 252–255. **The fix is to source the HLE constant file from the
`LOAD_ALU_CONSTANT` shadow as well as from `device + 0x780`.** Not yet done.

---

## Guest architecture

- Base `0x82000000`, XEX `assets/default.xex`. IDB at `assets/default.xex.i64`.
- Engine state pointer `dword_830BE400`; AssetDB at `*(0x830577C0)`.
- `dword_830B03EC` (GPU physical base) stays 0 in native mode and is not a
  blocker for asset loading.

### The asset load state machine

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
