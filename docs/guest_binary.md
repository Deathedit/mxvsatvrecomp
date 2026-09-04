# The guest binary — addresses, structures, and how they were read

Static-analysis reference for `assets/default.xex`, the Xbox 360 build that
[AGENTS.md](../AGENTS.md) describes porting. Everything here is a property of a
shipped 2010 binary: unlike the host-side facts in AGENTS.md, none of it changes
when we edit our own code.

> **This file is not the authority — the IDB is.** Every entry below is
> re-derivable, and the derivation is recorded next to the fact precisely so it
> can be re-checked rather than trusted. When something here disagrees with the
> binary, the binary wins. This project has been burned more than once by a note
> that was true when written and never re-verified; see **Traps** at the end for
> the specific ones.

Split out of AGENTS.md on 2026-08-06. Note it is *not* the `docs/ida_notes.md`
deleted in `30c252b` — that file's content was explicitly marked untrusted, and
none of it was carried over. Content here came from AGENTS.md's verified body.

---

## Working with the IDB

Listed first because it is how a reader checks anything below it.

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

Existing scripts in `tools/`: `ida_dump_param_gen.py`, `ida_dump_vs_const255.py`,
`ida_dump_const_emitters.py`, `ida_dump_frontend_vtables.py`,
`ida_dump_script_api.py`, `ida_dump_execute_script.py`,
`ida_dump_patch_vertex_shader.py`, `ida_dump_render_targets.py`,
`ida_dump_texture_bind_order.py`, `ida_dump_d3d9.py`.

---

## Global anchors

- Base `0x82000000`, XEX `assets/default.xex`. IDB at `assets/default.xex.i64`.
- Engine state pointer `dword_830BE400`; AssetDB at `*(0x830577C0)`.
- `dword_830B03EC` (GPU physical base) stays 0 in native mode and is not a
  blocker for asset loading.

---

## D3D9

D3D9 is linked statically into the XEX, so there is no import table — but static
linking removes the imports, not the functions. Entry points were located by
matching COFF symbols from a genuine Xbox 360 `d3d9.lib` (machine `0x01F2`,
PowerPC BE); `tools/match_d3d9.py` regenerates the patterns. **FLIRT/FLAIR does
not work here and is not needed** — the COFF symbol tables parse directly and
give more.

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

| `DrawVerticesUP` | `0x82555B88` | | |

IDA's auto-analysis defines no function at the two constant setters; the
addresses are confirmed by the byte match and by their own arithmetic, below.

**`DrawVerticesUP` is a third draw entry point** and is easy to miss — it does
not call either of the other two. See the Bink section below; roughly 30
functions across the engine draw through it.

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
| `device + 10572` | `PA_CL_VTE_CNTL` (0x2206) shadow | derived below |
| `device + 1920` / `+ 6016` | VS / PS constant flush shadows | `DrawVertices`: `sub_82564B00(device, dirty, 0x4000, device + 1920)` |
| `device + 1152 + off` | register shadow patched by shader objects | both shader setters walk an AND/OR list from the shader object |

### Pixel shader object

Not the device — the `D3DPixelShader*`. `D3DPixelShader` is only the 24-byte
`D3DResource` base, so IDA renders `pShader[1].Identifier` as +0x28 and
`pShader[2].ReadFence` as +0x3C. **The IDA-rendered names are misleading here;
the offsets are what matter.**

| Offset | What | Read from |
|---|---|---|
| `ps + 0x18` | code allocation | `sub_825506B0` stores it there; `CreatePixelShader` (`0x82552148`) copies `a1[2]` bytes from `a1 + a1[1]` into it |
| `ps + 0x28` | copy of the source header | `CreatePixelShader`: `sub_82BFB9D8(v7 + 5, a1, a1[1])` |
| `ps + 0x30` | code **allocation** size — a bound, not the program length | |
| `ps + 0x3C` | offset of the constant-patch list within the +0x28 header | `SetPixelShader` walks `(ps + 0x28) + *(ps + 0x3C)` |
| `ps + 0x40` | offset of the program info block | shader flush `sub_82565928` |
| `*(ps + 0x40) + 0x28` | **byte offset of the CF stream inside the code allocation** | `sub_82565928`: `v22 = *((char*)v8 + v8[16] + 40) + v8[6]` |
| `*(ps + 0x40) + 0x2C` | program length in bytes (`>> 2` for dwords) | same, `*v23 = *(... + 44) >> 2` |

### Vertex shader object

The same structure, different offsets, read out of `sub_82565928`'s VS branch
(raw disassembly at `0x82566234`, **not** the decompiler's folded arithmetic) and
confirmed against `D3D_PatchVertexShaderToMatchVertexDeclaration` (`0x82564C50`),
which indexes the identical `0x380 + variant*8` field:

| Offset | What | Read from |
|---|---|---|
| `vs + 0x20` | code allocation | `lwz r8, 0x20(r30)` — added to the CF offset to form the program address |
| `vs + 0x380 + variant*8` | offset of the info block | `slwi r10, r11, 3` with `r11 = variant + 112`, then `lwzx r11, r10, r30` |
| `info + 0x368` | **byte offset of the CF stream inside the code allocation** | `lwz r11, 0x368(r11)` then `add r11, r11, r8` |
| `info + 0x36C` | program length in bytes (`>> 2` for dwords) | `lwz r11, 0x36C(r11)` / `srwi r11, r11, 2` |
| `info + 0x384` | number of patchable vfetch instructions | the patcher's `v7` |

`variant` is the same 0/1 selector `sub_82565928` computes; the patcher takes it
as its own argument.

**The CF stream does not start at the beginning of the code allocation.** Big
shaders carry a prologue that reads as zeros; small ones start at 0. That is not
a fixed 64-byte header to be inferred from a histogram — it is the `+0x368` /
`+0x28` field above, and `sub_82565928` is what hands the resulting address to
the hardware.

### `SQ_PROGRAM_CNTL` bit layout

`vs_num_reg[5:0]`, `ps_num_reg[13:8]`, `param_gen[18]`, `gen_index_pix[19]`,
`vs_export_count[23:20]`, `vs_export_mode[26:24]`, `ps_export_mode[31:27]`.

### `PA_CL_VTE_CNTL` and the register shadow

`PA_CL_VTE_CNTL` (`0x2206`) says whether the GPU applies the viewport transform
itself. Its shadow follows the pattern established for `SQ_PROGRAM_CNTL`: the
draw-time flush issues `sub_82564768(device, 0, 8704, device + 10548)` with
`8704 = 0x2200 = RB_DEPTHCONTROL`, and `sub_82564768` sends register `base + i`
from `shadow + i*4`, so **`0x2206` lives at `device + 10572`**.

That derivation contradicted an earlier note in AGENTS.md claiming VTE_CNTL is
`0x300`, so it was checked before being acted on. Dumping the surrounding dwords
settled it: 17 dwords below `+10572` are `640.0, 640.0, -90.0, 90.0, 1.0`, which
are `PA_CL_VPORT_XSCALE/XOFFSET/YSCALE/YOFFSET/ZSCALE` (`0x210F..0x2113`); the
`0x2100` block's own shadow base (`device + 10444`) puts XSCALE at
`10444 + 15*4 = 10504`, exactly there. 640 is half of 1280. **The offset is
right and the `0x300` note was stale.**

### `LOAD_ALU_CONSTANT` emitted by the shader object

`sub_825656A0`, called from the draw-time flush as
`sub_825656A0(device, vs + 0x368, *(vs + 0x20))`, walks a table in the shader
object and emits one PM4 Type-3 packet per entry with header **`0xC0022F00`** —
opcode `0x2F`, LOAD_ALU_CONSTANT — body `[source_address, 4 * reg, dword_count]`:

```
H = vs + 0x368;  P = H + *(H + 0x14)
P + 0x10  u32   list byte length;  entries at P + 0x14
entry: u16 reg_index, u16 dword_count, u32 data_offset   (8 bytes)
        terminated by dword_count == 0
source = *(vs + 0x20) + data_offset
```

Every shader publishes one entry covering **c252..c255**, holding screen-space
scale/bias — `(0.5, -0.5, 0, 0)`, `(0, 1, 0.5, -0.5)`, `(1, 2, 0.5, -0.5)`.
`4 * 252 = 0x3F0`, and `0x4000 + 0x3F0 = 0x43F0`, which is the
`LOAD_ALU_CONSTANT reg=0x43F0 dwords=16` seen on the ring.

Note `sub_82564B00` flushes `device + 0x780` verbatim — that is a *different*
LOAD_ALU_CONSTANT packet. Proving one publisher is a copy does not enumerate the
publishers.

---

## Vertex and texture formats

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
differing only there. **The format size table is the guest's own, at
`0x8204E188`**, indexed by the vfetch format field; illegal formats are 0.

### The `GPUTEXTUREFORMAT` name table

The guest carries its own complete 64-entry table: a pointer array at
**`0x82d24378`** (near-duplicate at `0x82d59d00`) indexing the `FMT_*` strings in
`0x820a9d00`-`0x820a9fd0` and `0x8205b5ec`-`0x8205b6d0`. Both tables belong to
the HLSL compiler embedded in the XEX (`sub_8263F9C0`, `sub_82C1BB88`), so they
are guest *diagnostics*, not the runtime texture path — but the table is
authoritative for the enum, and decoding it confirmed index-for-index, from the
game's own binary rather than from Xenia, that `fetch.format` indexes the
ordering `xenos.h` assumes.

It is transcribed into `GuestTextureFormatName` in
[d3d9_texture.cpp](../src/gpu/d3d9_texture.cpp). Index 20 is spelled `FMT_4_5`
in the guest, not `FMT_DXT4_5`; the value still matches `k_DXT4_5 = 20`.

---

## The script layer

The front end is driven by an embedded **Lua 5.1** VM.

### Binding tables

The bindings are generated by **SWIG**. The module init registers `swig_type`
and `swig_equals`, and `sub_824A8998` is `SWIG_Lua_set_immutable` — it raises
`"This variable is immutable"`. Knowing that names the whole layout instead of
guessing at it table by table, and it means **every SWIG array is
NUL-terminated, never counted**: a length is the measured distance to the
terminator, not a bound the game itself uses.

`MXRavage_Xenon_00cb` (`0x824F1D80`) is the module init. At `0x824F1E10` it
calls `sub_824A8BA0(L, "Engine")`, which creates the global `Engine` table, and
only then walks the registration tables.

Four of the five functions above `sub_82352AE0` have **zero** direct callers and
are not virtual methods either. They are entries in the **module function table
at `0x8203F2E0`** — 228 pairs of `const char*` and code pointer, registered one
at a time by `sub_824A8DC0` from the loop at `0x824F1E28`. The 228 is the
distance to the terminator at `0x8203FA00`; the last entry is `FormatSpeed`.
The vocabulary is a scripting API.

| Binding | Address |
|---|---|
| `[6] LoadAssetDB` | `0x824AF3C0` |
| `[10] ExecuteScriptAsset` | `0x824AF838` |
| `[40] GetUIState` | `0x824BBA40` |
| `[50] LoadUIAssetPackage` | `0x824CBF90` |
| `[53] LoadUIAssetDatabasePackage` | `0x824CC218` |
| `[66] StartWorldLoad` | `0x824CD280` — reaches `sub_82534980` |
| `[67] EnableWorld` | `0x824CD308` |
| `[110] SwitchToUIWorld` | `0x824D0F18` |

Also `IsUIAssetPackageLoaded` at `sub_824CC120`.

`ExecuteScriptAsset` validates one `char const*` argument and passes it to
`sub_824F91E8`, whose **`r3` is therefore the script asset name** in plain guest
memory.

#### The other four populations

The module function table is one of five places a `lua_CFunction` can come from,
so an address missing from it was never evidence that it is not a binding.

**Module attributes** — `0x82D1C858`, `{name, getter, setter}` × 12 bytes,
registered by `sub_824A8CE0`. One entry: `MAX_INFO_LEN`, getter `0x824DAB80`,
setter `SWIG_Lua_set_immutable`.

**Classes** — reached at runtime, not statically. `swig_types[]` at
`0x83016900` is `.bss`, filled during init; each `swig_type_info` carries the
class at `+16` (`clientdata`), and `sub_824A9580`
(`SWIG_Lua_class_register`) plus its member pass `sub_824A9358` register it.
Layouts read out of those two functions and confirmed against a live struct:

| struct | fields |
|---|---|
| `swig_type_info` | `+16` `clientdata` → `swig_lua_class*` |
| `swig_lua_class` | `+0` name, `+8` constructor, `+16` methods, `+20` attributes, `+24` bases, `+28` base_names |
| `swig_lua_method` | 8 bytes, `{name, func}` |
| `swig_lua_attribute` | 12 bytes, `{name, getter, setter}` |

This supersedes the earlier claim of "a second `(name, func)` table in `.data`
around `0x82D1B21C`, bounds not established". It is not a second module table:
it is the **methods array of the `VariableCollection` class**, whose base is
`0x82D1B208` and whose class struct is at `0x82D1B260`. Its bound is the
terminator, like every other SWIG array. It holds `sub_824B1C20`
`GetVariableString` — which validates its arguments through the same
`sub_82A9F4F8` reporter as `ExecuteScriptAsset` and names itself
`VariableCollection_GetVariableString` in its own error strings — the int twin
`sub_824B1788` `GetVariableInt`, and `sub_824B1210` `SetVariableVector3`.

**Module metamethods** — installed on the `Engine` metatable by
`sub_824A8BA0`, so no table holds them and no scan reaches them:
`__index` `0x824A89E8`, `__newindex` `0x824A8AC0` (`SWIG_Lua_module_set`; it
consults `.set` and otherwise falls through to a plain `rawset`, so a script
line like `Engine.Foo = …` dispatches it), plus `swig_type` `0x824A9A98` and
`swig_equals` `0x824A9AD8`.

**Class metamethods** — installed by `sub_824A9580`: `__index` `0x824A8E18`,
`__newindex` `0x824A8FC8`, `__gc` `0x824A9120`.

`BindingName` in `src/hooks/hooks_plugin_diag.cpp` resolves all five.

Registry getter call sites, identified from `lr` captured at entry:
`0x82536294` → `sub_82536250` (the loader's own gate); `0x824AA590` →
`sub_824AA568`, called only by `sub_824B1C20`; `0x824AA518` → `sub_824AA4F8`,
called only by `sub_824B1788`.

### Lua internals

`sub_82AA7638` is Lua's `luaD_precall` (identified from `ExecuteScriptAsset`'s
caller `lr=0x82AA78F4`), and its `r4` is the `func` StkId, so the callee is
readable before it runs:

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

Error paths: `sub_82AA9D48` owns `"stack overflow"`; `sub_82A9F4F8` is the
`luaL_error`-style reporter used for argument mismatches.

### `PlayerMode` vocabulary

Five values — `SplitScreen`, `SinglePlayer`, `Online`, `LAN`, `None` — so
`"None"` is index 4. The *failure* value is 5.

---

## Bink

Bink is statically linked into the XEX the same way D3D9 is: a `BINKCONS` data
segment at `0x821CD1D0` and library code from about `0x82CEB650` to `0x82CF0508`.
The guest carries a complete decoder.

| Symbol | Address | How it was identified |
|---|---|---|
| `BinkOpen(path, flags)` | `sub_82CEB7C8` | Owns `"Not a Bink file."` (`0x82144B9C`) and `"Error reading Bink header."` (`0x82144B28`), referenced nowhere else. Two callers total, so it is *the* choke point |
| `XenonBinkVideoManager::Open` | `sub_8234E0A8` | vtable `0x82017510` slot [1]; formats `"game:\%s.bik"` |
| `XenonBinkVideoManager::Open` | `sub_8234E290` | slot [2]; formats `"%s.bik"` |
| `BinkAsset::Init` | `sub_8234CBB8` | Reads `"Texture To Override"`, then resolves `"Bink Video Asset"` via `sub_82AB8210` and requests fourcc `1651076715` = `0x62696E6B` = `'bink'` |
| `XenonBinkVideo` vtable | `0x820172BC` | 12 entries; HBINK at object `+144`, its critical section at `+156` |

### The frame composite — and the draw entry point it uses

`sub_8234D630` is the `XenonBinkVideo` frame method (vtable slot [8]). Per
frame, under the object's own critical section at `+156`:

1. `D3DDevice_Clear` on a render target,
2. `sub_8234C7C0(device, obj + 192, w, h, …)` — the composite draw,
3. `D3DDevice_Resolve(device, 0x100, &rect, destTexture, …)`.

`sub_8234C7C0` is a **YUV→RGB shader composite over separate single-channel
planes**, not a packed-YUV texture sample:

- `D3DDevice_SetTexture` on **samplers 0, 1, 2** from `a2 + 136 / 140 / 144`, a
  32-byte-stride plane set indexed by `*(a2 + 20)` — Y, Cr, Cb.
- An **optional alpha plane** on sampler 3 from `a2 + 148`. Its presence selects
  the second pixel shader.
- Draws a 4-vertex triangle strip, stride 20, then unbinds all four samplers.

The three shader handles live in globals, which makes the draw identifiable at
runtime with no heuristic:

| Global | Holds |
|---|---|
| `0x82DD7130` | `D3DPixelShader*` — YUV, no alpha plane |
| `0x82DD7134` | `D3DPixelShader*` — YUV + alpha plane |
| `0x82DD7138` | `D3DVertexShader*` — the composite's vertex shader |

**So `k_Cr_Y1_Cb_Y0_REP` (11) and `k_Y1_Cr_Y0_Cb_REP` (12) are never requested
by this title.** Anyone reading "Bink" and reaching for the packed YUV formats
is about to write dead code.

### `D3DDevice_DrawVerticesUP` — a third draw entry point

The composite draws through **`sub_82555B88`**, which is neither of the two
draw entry points listed above:

```
sub_82555B88(device, primType, primCount, vertexData, stride)
  p = sub_825556C8()                      // reserve ring space
  sub_82BDAAF0(p, vertexData, primCount * stride)   // inline vertex copy
  *(device + 48) = *(device + 13652)
```

It writes vertex data straight into the command buffer and **never calls
`DrawVertices` (`0x825561B0`) or `DrawIndexedVertices` (`0x825565C8`)**.

It is not a niche path: **40 xrefs from roughly 30 distinct functions**, spread
across `0x8246Axxx`, `0x829Exxxx`, `0x82ACxxxx`-`0x82AFxxxx`, `0x82B0xxxx` and
`0x82B1xxxx` — UI and particles as well as video. Any host layer that hooks only
the two named draws is blind to all of it.

### `BeginVertices` / `EndVertices` — a FOURTH draw path, still unhooked

`sub_825556C8` above is not merely a ring-space reservation: **it emits the PM4
draw packet itself**, `v25[5] = primType & 0x3F | (count << 16) | 0x80`, and
returns a guest pointer for the caller to write inline vertices into.
`sub_825556B8` closes it. So a caller that uses the pair DIRECTLY never enters
`DrawVerticesUP` either, and is invisible to a layer hooking the three named
draws.

The engine's UI does exactly that, via `sub_82B27390` (reached from
`sub_82B296B0`, which sets the D3D9 state):

```
sub_82B27390(geom)
  n = geom[1]
  p = sub_825556C8(dev, geom[2], n, 12)   // BeginVertices -> write pointer
  memcpy(p, geom[3], 12 * n)
  sub_825556B8(dev)                       // EndVertices
```

Measured consequence: every stage of the UI submit passes and
`GuestDrawCalls()` never moves — 0 of 2816 render entries moved it.

**HOOKED, unconditionally, since 2026-08-26.** `hooks_d3d9_entry.cpp` hooks
both entry points and builds a `DrawCall` at `EndVertices`, when the guest's
inline vertices actually exist. It was briefly parked on `begin-vertices-hook`
(`de8bf3b`) and then behind `--d3d9_begin_vertices`; both are gone.

**The intro logo needed this AND a shader fix**, and neither half shows anything
alone. With the hook off the draw is never submitted; with it on but
per-instruction ALU predication missing, the draw is submitted and blends to
nothing because its alpha translates to a compile-time 0. See the memory notes
`ui-draws-bypass-hooked-entry-points` and `instruction-level-predication`.

One caveat carried forward: enabling this coincided with an intermittent guest
fault at `sub_8234CE20` (2 in 5 runs) that then stopped reproducing and was
never explained. If guest-side faults reappear around front-end construction,
suspect this first.

Open flags, read statically out of `sub_8234E0A8`: `a4=1` gives `0x00102400`
(`0x2000|0x100400`); `a4=0` gives `0x01100400`, the branch that first calls
`sub_82CEB3F0(10485760)`.

RTTI is present for `BinkAsset`, `BinkAssetIO`, `BinkAssetHandle`,
`XenonBinkVideo` and `XenonBinkVideoManager`. Neither manager `Open` has a single
code xref — both are reached only through the vtable at `0x82017510`, so static
call-graph walking will not find their callers.

---

## Asset loading

**Reverse-engineering the AssetDB was a wrong turn**, and a long one — the state
machine is not stuck, it idles because the layer above it never asks for
anything. Retained only because `force_load` depends on it.

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

---

## Engine misc

### Frame pacing — `sub_82B70370`

The field map, read out of the function rather than assumed. (This matters: a
host stub was written against two *asserted* hazards, both of which the code
contradicts. See AGENTS.md for that story.)

| Offset | What |
|---|---|
| `a1+20` | busy-wait control. Reads `0x7F7FFFFF`; the guest's own test is `if (*(float*)(a1+20) != 3.4028235e38 && dt < target)`, so this value **disables** the spin |
| `a1+24` | output frame delta |
| `a1+28` | guard for the ring below |
| `a1+32` | ring index: `v9 = *(a1+32) + 9; *(float*)(4*v9 + a1) = dt;` with `if (v10 >= 5) *(a1+32) = 0` |
| `a1+36..a1+52` | the bounded 5-entry sample ring |
| `a1+56` | 5-sample smoothing sum |
| `a1+60` | **total elapsed time** |
| `a1+64`, `a1+104`, `a1+112` | also maintained |

`sub_82B34998` is RendererDispatch; its `f1` is the frame delta argument.

### The KPCR — `r13`

`r13` is the per-thread KPCR pointer, and guest code indexes it directly, so
these offsets turn up in ordinary decompiled functions rather than in anything
that looks like kernel code.

| Offset | What | Read out of |
|---|---|---|
| `+0x00` | pointer to this thread's TLS block | the device lookup below |
| `+0x70` | stack base | written per thread by the runtime |
| `+0x74` | stack end (`stack_end_ptr`) | `rex/ppc/stack.h stack_limit_from_pcr` |
| `+0x100` | pointer to the thread block holding the Win32 last-error slot | `sub_82C01100` / `sub_82C01138` |
| `+0x150` | non-zero suppresses the last-error read and write | the same pair |

`*(*(r13 + 0x100) + 0x160)` is the last-error value itself: `sub_82C01100` is
`SetLastError(RtlNtStatusToDosError(status))` and `sub_82C01138` is
`GetLastError`, both gated on `+0x150`.

**The current D3D device is thread-local**, and this is the guest's own idiom
for fetching it — four sites use it verbatim (`sub_82B70760` MainLoop,
`sub_82B70290`, `sub_82B70300`, `sub_82B70BE8`):

```c
dev = *(*(r13) + 0x579C);            // this thread's TLS block, +0x579C
if (!dev) dev = *(dword_830B08C0 + 0x4C);   // global fallback
```

Hex-Rays renders `*(r13)` as an uninitialised local (`int v6; // r13`), because
nothing in the function writes r13 — read the disassembly, not the pseudocode:
`lwz r11, 0(r13)` / `li r10, 0x579C` / `lwzx r31, r10, r11`.

Under recompilation r13 is set per thread and is never 0: `XThread::Create`
allocates a 0x2D8-byte block per thread and hands it to `ThreadState`, whose
constructor writes it into that thread's own `PPCContext::r13` (offset 104;
`r1` is offset 16). Only `XThread::Create` and `XThread::Restore` construct a
`ThreadState`, and `rex/hook.h` and `rex/ppc/function.h` propagate r13 into
hook and nested-call frames.

### Other named functions

- `sub_82B70760` MainLoop → `sub_82B70578` RenderPipeline → `sub_82AFE978` →
  `sub_82AFCA38` (4,416 decompiled lines, 31 `SetTexture`, 4 `Resolve`) →
  `sub_82AFA520` → `sub_82AF93C8`, which recurses.
- `bl sub_82B34998` at `0x82B70EF4` — the one mid-ASM hook site.
- `sub_82B6D230` iterates `(v[1]-v[0])>>2` elements calling `sub_82B6A448(elem,
  dt)`. `sub_82B3C7D0` is an accessor for `dword_830BE190` with a null assert; it
  allocates nothing.
- XDK wrappers around import thunks: `sub_82C08EC0` → `XamInputGetState`,
  `sub_82C08ED0` → `XamInputGetCapabilities`, `sub_82C87F78` →
  `XAudioRegisterRenderDriverClient`, `sub_82C87B98` →
  `XAudioSubmitRenderDriverFrame`, `sub_82C4C268` → `XMACreateContext`. Input is
  polled from `sub_82B6DB28` (`lr=0x82B6DBD4`).
- `sub_82BFBF48` tail-calls `sub_82C01138`, a pure thread-block read —
  `r13+336 ? 0 : *(*(r13+256) + 352)`. It is `GetLastError` (see the KPCR
  section; the paired writer is `sub_82C01100`), it has no side effects, and it
  has **156 call sites**. `sub_82BFB748` is the `NtSetEvent` wrapper, returning
  1 on success. So the idiom `if (!sub_82BFB748(h)) sub_82BFBF48();`, which the
  frame and teardown paths are full of, is `if (!SetEvent(h)) GetLastError();`
  — the code is fetched and dropped, an assert compiled out of retail. It is
  **not** an error handler, and naming it one has now misled twice.

### Known external blockers

- Binary `.xenon.package` heaps are encrypted (entropy ≈7.98, routine unknown;
  the guest's OpenSSL AES is TLS-only). This limits the *offline* tools in
  `tools/`, not the guest — the runtime reads its own packages fine.
- FATAL crash at `0x82327CF0` during gameplay.

---

## Traps

Every one of these produced a confident wrong answer that survived because it
looked right.

**`.pdata` is not a table of `(x, func)` pairs.** An earlier reading put the
VariableCollection bindings in a table at `0x821A1740`/`0x821A1750`. That is
`.pdata` — function address plus unwind flags. Same mistake as reading
`0x82198B50` as a vtable. The giveaway both times is a second dword like
`0x400003A3` that is not a pointer. Check that before reading any address pair
as `(x, func)`.

**Identical-code folding makes some bindings unnameable.** `0x829E8FA8` is a
bare `return 0` with hundreds of xrefs, so several trivial bindings share that
one address. A `cfunc` value alone cannot name such a binding.

**IDA's rendered field names can mislead.** `D3DPixelShader` is only the 24-byte
`D3DResource` base, so the decompiler shows `pShader[1].Identifier` for what is
really `+0x28`. Trust the offset, not the name.

**Prefer raw disassembly to folded arithmetic.** The vertex shader object
offsets had to be read at `0x82566234` directly; the decompiler's folded
expression hid the `0x380 + variant*8` indexing.

**A field read in the wrong units is indistinguishable from a negative result.**
A probe walked the right `LOAD_ALU_CONSTANT` table but parsed it with the
neighbouring list's layout, read `reg_index` `0xFC` as a byte offset, compared it
against c255's byte offset `0x12F0`, and reported "none covers c255". `0xFC` is
c252 and the count covered c255. It survived because the answer it gave was the
one already believed.

**Read the field out of the code that consumes it.** The one method that has
paid off every time: the `Type` bit layout came from
`PatchVertexShaderToMatchVertexDeclaration`'s own arithmetic; the format size
table was lifted verbatim from `0x8204E188`; the vertex declaration offset came
from `SetVertexDeclaration`'s single `stw`; the constant file layout came from
`SetVertexShaderConstantF`'s three instructions; `SQ_PROGRAM_CNTL`'s shadow
offset came from the argument `DrawVertices` passes. In each case an earlier
attempt to infer the answer from data produced something plausible and wrong.
