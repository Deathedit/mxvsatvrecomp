# PM4 Pipeline & Plugin-Mode Data Capture

Deep technical analysis of the PM4 packet parser, translator, plugin-mode runtime state capture, GPU cvars, and the SDK/Xenia reference files. Operationally, the parser lives in `src/pm4_parser.{h,cpp}` and the translator in `src/pm4_translator.{h,cpp}`.

Reference: AGENTS.md (operational hub), docs/ida_notes.md (IDA bookmarks).

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

### Render Targets

sub_82629560 -> sub_8254DED8 -> sub_82469998 -> GpuAlloc

Device offsets (handles, not addresses):
- +56: Depth surface (0x04)
- +104: Color render target (0x04)
- +2388: 640-height buffer

Actual memory at 0xBEDA0000 (GpuAlloc #1, 15MB).

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

**Opcode table** rewritten to match Xenia's `xenos.h::Type3Opcode` enum (see `C:\rexglue-sdk\include\rex\graphics\xenos.h` line ~1573). Key opcodes:

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