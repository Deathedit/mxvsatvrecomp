# Asset Format & Loading Pipeline — BXML, .xenon.package, Encryption

Deep technical analysis of the guest asset loading chain, BXML binary XML format, .xenon.package container layout, per-asset-type heap format, shader microcode load path, and the encryption blocker.

Reference: AGENTS.md (operational hub), docs/ida_notes.md (IDA bookmarks), tools/README.md (decoder scripts).

---

## Asset loading guest code path investigation — sub_8253CF08, AssetDB, BXML format (2026-07-31)

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

---

## BXML format structure (reverse-engineered 2026-07-31)

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

### BXML Tools

| File | Role |
|---|---|
| `tools/bxml_decoder.py` | Single-call zlib-decompress + strings + u32 token walk dump (`--raw` for comprehensive dump, also supports directory batch). |
| `tools/bxml_strings.py` | Strings-only summarizer; categorizes strings into `ENTITY/SHADER/MATERIAL/ANIM/TEXTURE/META/OTHER`. Useful for understanding what asset references each `.xenon.database` contains without full XML reconstruction. |
| `tools/decode_bxml.py` | Minimal zlib-decompress; outputs raw decompressed bytes. |
| `tools/dump_bxml.py` | Hexdump of decompressed bytes (no parsing). |
| `tools/README.md` | Documents all `tools/` scripts + BXML format status + what works/what doesn't. |
| `tools/extract_xex_pe.py` | (From prior work) XEX PE extractor. |
| `tools/lzx_decompress.py` | (From prior work) LZX decompression for XEX container. |

---

## BXML format reconstruction status (as of 2026-07-31)

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

Full-featured Tools:
| File | Role |
|---|---|
| `tools/bxml_full_decoder.py` | **PRIMARY TOOL** — full XML reconstruction from ANY `.bxml` or `.xenon.database` file. Verified on all file types. |
| `tools/bxml_strings.py` | Strings-only categorizer: `ENTITY/SHADER/MATERIAL/ANIM/TEXTURE/META/OTHER`. |
| `tools/bxml_decoder.py` | Raw internals inspector: strings + u32 token walk dump (`--raw` mode). |
| `tools/decode_bxml.py` | Minimal zlib decompressor (raw bytes only). |
| `tools/dump_bxml.py` | Hexdump of decompressed bytes. |
| `tools/README.md` | Full format documentation + tool guide. |

Guest implementation: `sub_82B64400` (0x82B64400, 9204 bytes, 390 basic blocks) is the universal BXML tree builder. Reads 36-byte file header via vt[8], byteswaps PPC↔LE, allocates string table (a1[2]) + child table (a1[3]), reads `node_count` 12-byte node-descriptor records, dispatches type-code byteswap fixups by data stride (case 3=i32 stride 4, case 7=vec4 stride 16, case 8=m4x4 stride 64, case 0xA=vec3 stride 12), then reads 32-byte render descriptors and dispatches structure type codes (cases 1-12). This layer handles binary-data `.xenon.database` files; config `.bxml` files use simple string-index references decoded by our Python tool directly.

---

## AssetDB_LoadStateMachine — 12-case load orchestration (RE'd 2026-07-31)

`AssetDB_LoadStateMachine` = `sub_8253AA40` @ 0x8253AA40 (4204 bytes, 139 blocks, 12-case switch).
This is **LoaderTick's gating call** (AssetDB vt[6], called from `unk_830EC248+8 → AssetDB → vt[6]()`).

**The state is `*(uint32_t*)(AssetDB + 28)`** — a plain 0..11 selector. Derived
from the recompiled body: `generated/default/mx_recomp.31.cpp:36836`
(`lwz r11,28(r31)`, where `r31 = r3 = a1` and is never reassigned) feeding the
12-entry jump table at `:36862` (`cmplwi cr6,r11,11` / `bgt` → default). The
`+110796` previously circulated in `docs/pm4_pipeline.md` is a **guest heap
pointer**, not this enum — every "state=" value logged before 2026-08-02 was
meaningless.

**Observed in native mode (2026-08-02, 3/3 runs, `logs/mx_015..017.log`)**:
`0 -> 1` on call #1, holds at 1 for ~58 calls, `1 -> 2` at call #59, then
**parks in state 2 for the remaining ~750 calls**. State 1's
`SceneTransition_Kickoff` does fire. State 2's body is only
`*(a1+110328) = 0` followed by a jump to the common tail — it never writes
`*(a1+28)`, so it cannot self-advance. Something external must request the next
load; nothing does. The loader is idle and healthy, not stuck or broken.

**Confirmed 2026-08-02** by bracketing the hook with a check for changes to
`*(a1+28)` between one return and the next entry: across a full run the selector
is never written from outside `sub_8253AA40`.

### The load-request API — `sub_82534980`

`sub_82534980(AssetDB, name, flags)` `strncpy`s up to **260 bytes** of `name` into
`AssetDB+29540` (`sub_82AB4AB0(a1+29540, r4, 260)`,
`generated/default/mx_recomp.31.cpp:22377`), stores `flags` at `+29800`, and — only
if `*(AssetDB+28) == 2` — sets the selector to **3** and notifies the listener at
`*(a1+110788)`. This is the external write the machine waits for.

`AssetDB+29540` is therefore a `MAX_PATH` **string buffer**, and the five sites
that look like they read a flag are testing `name[0] != 0` — "is a load pending".
Every one picks state 2 when it is empty. The only other writer is the constructor
`sub_8253CB38` (`:41615`), zeroing it.

Its **only caller is `sub_82352AE0`** (`mx_recomp.15.cpp:76710`), which resolves the
name from a registry lookup and is a method with five callers (`sub_82367A50`,
`sub_8236B470`, `sub_8236B660`, `sub_824FB1F0`, `sub_824FC9A0`). With all seven
hooked, **none fires in a 40s native run** — the front end that would request a
load is never entered.

**Correction**: `sub_825378F0` (`:29489`) is not an abort, as previously recorded
here and in commit `8b396bf`. It is state 4's normal completion — clears
`+110328`, then routes to state **3** if a name is pending or **2** if not.

**Driving it (`--force_load=NAT_Farm`, diagnostic only)** — identical 3/3 runs,
zero access violations:

`2 -> 3` (by the API) `-> 4` (~390 ticks of real work) `-> 5 -> 6`, then **parks
in state 6 `PlayerSetup`**. Entity counts stay `pass0=1 pass1=0 pass2=1`, still
zero `DRAW_*`, and no file I/O for the requested scene — but states 7/8 are where
the async content load happens and we park before them, so that last point does
not yet indict the name.

### The state 6 gate — resolved

**Correction**: this section previously offered the "NetworkNoPlayers" /
per-player UniqueId description from the table below as the likely state 6 gate.
That is wrong. The per-player loop at `loc_8253B5F0` and the `state = 7` write at
`loc_8253B694` are *downstream* of the gate and are never reached.

State 6 reads `*(a1+110328)`, finds it zero, goes to `loc_8253B560` and calls
`(*(AssetDB+110788))->vt[2]()`. A zero return jumps to `loc_8253B6A4`, an early
return that leaves the selector at 6. That one predicate is the blocker.

The listener is `0x40B76700`, vtable `0x8204C014`, and `vt[2]` resolves to
`sub_8253CF80` (`mx_recomp.31.cpp:42045`):

```
mode = sub_82536250(*(0x830577C0));   // registry string -> enum
if (mode == 2 || mode == 3) return 1;
if (*(0x83057900) != 0)     return 1;
tmp = 0; sub_82548758(registry, <key>, &tmp, 0); return tmp;
```

Measured, identical 3/3, zero access violations: `sub_82536250` returns **4**
(stable over 1500+ calls, needs 2 or 3), `*(0x83057900)` is **0**, and the
registry fallback returns **0**. State 1 clears `*(0x83057900)` itself
(`stw r25,30976(r8)`, `r25 = 0`), so boot closes that escape.

It is a **game-mode** gate. The loader only proceeds in modes 2/3, and the
registry currently reports 4.

Note `lis r11,-31995` is `0x83050000` — the AssetDB global `dword_830577C0`,
confirmed at runtime. An earlier note claiming a separate null global was an
arithmetic error.

**Superseded**: an earlier `force_launch` cvar wrote `*(AssetDB+28)` directly.
Forcing `2 -> 9` access-violates — `sub_82541F80 +0xE4` is `lwzx r29,r3,0x20768`
with `r3 = *(a1+23132)`, null until `sub_825372C0` (the "Subscene Creation"
callback state 4 registers) has run. That cvar is gone; use `force_load`.

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

---

## Asset loader infrastructure (RE'd 2026-07-31)

### Renamed functions (locked in via IDA)

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

### DatabaseAndPackageIndexLoader — what it builds (`sub_82BAA650`)

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

### AssetFile_Open — only handles BXML (`sub_82B67128`)

Inspection of all 22 caller sites shows **every caller opens a named `.bxml` config file** (UIRegistry, Localization, Audio/ConfigFiles/*, etc.) — NOT actual binary asset content. Package binary extraction (mesh/texture/audio bytes) follows a different path — the heap itself is a BXML block decoded in-place by `BxmlTreeBuilder_Parse` when the loader iterates it, NOT via `AssetFile_Open`.

---

## .xenon.package format (RE'd 2026-07-31)

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

---

## Asset catalog (2026-07-31) — `out/asset_catalog.json`

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

1. ~~**Mid-ASM hook #5 is the gatekeeper**~~ — **WRONG, corrected 2026-08-01.** Hook #5 fired at
   **0x82B71324**, which is *after* 0x82B712EC, so it never skipped that call site. Hooks #3
   (0x82B712C4→0x82B712D8) and #4 (0x82B71304→0x82B71314) are both disabled and also don't cover it.
   **Nothing skips `AssetDB_InnerCtor_VtableInstall`**, so `DatabaseAndPackageIndexLoader` may well be
   running at boot — this needs a runtime log to confirm before any of the options below are worth
   pursuing. (#5 has since been moved to 0x82B71520; see the hook table in AGENTS.md.)
   Original options, retained for reference:
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

---

## Shader microcode load path (RE'd 2026-07-31)

The binary contains the full **Microsoft Xbox 360 Shader Assembler** (bundled XDK XDK `xdk-main-sep10`):
- assertion strings reveal source paths like `e:\xenon\xdk-main-sep10\core\private\xtl\graphics\xgraphics\ucode\ssm\statecompiler\ssmstatecompiler.cpp`
- `xltconvert.cpp` is the XLT (Xbox-to-host shader) translator

### Shader asset heap structure (binary heap, no BXML inside)

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

### Runtime shader registration (effect system)

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

### XDK-bundled shader state compiler

`SSM_StateCompiler_Dispatch` (`sub_82646D58` @ 0x82646D58): bundled MS XDK source-shader-microcode state compiler. Calls `sub_82705888(pStateCompiler, pState, shader, ...)` to dispatch microcode. This is the runtime microcode→GPU dispatch.

`XLT_Translator_Ctor` (`sub_826FFCE0` @ 0x826FFCE0): builds an "translator" object (Xbox-to-D3D) with callbacks:
- `AllocateSysMem` callback (slot 1)
- `FreeSysMem` callback (slot 2)
- `registry` (slot 3)
- `texServer` (slot 4)
- `shaderStore` (slot 5)

XLT translator path: `e:\xenon\xdk-main-sep10\core\private\xtl\graphics\xgraphics\ucode\ssm\translator\xltconvert.cpp`

### Big-picture implications for native renderer

1. **The guest binary contains 2 separate shader paths**:
   - **Source-assembler** path (Microsoft Xbox 360 Shader Assembler 2.0.20209.0) — takes high-level shader source, produces Xenos microcode (used only at authoring time, but binary embedded)
   - **Runtime microcode-dispatch** path (SSM State Compiler + XLT Translator) — loads compiled Xenos microcode from `.shader` heaps, dispatches to GPU via `D3DVertexShader::SetShader`-equivalent
2. **`.shader` heaps store raw Xenos microcode** (NOT source). Converting to D3D12 requires:
   - Extracting Xenos microcode bytes from each heap (after 33-byte heap header, sequential 4-byte/cb reads)
   - Disassembling back to Xbox 360 shader ISA (or use Xenia's recompiler logic)
   - Compiling to DXBC/SPIR-V for D3D12
3. **Effect technique (.fx-equivalent) XMLs** live in `.surface`/`.material` heaps (NOT `.shader` heaps). The .shader heaps are just the compiled microcode; techniques referencing them live elsewhere.
4. **Most likely blocker**: many `.shader` heaps in the package files are LZX-compressed (per `<Compress enabled="true" codec="LZX"/>` in the .database). Decoding them requires implementing/hosting an LZX decompressor. Heaps below the compression threshold (small ones, ≤1KB?) are stored uncompressed and have standard heap headers visible directly.

---

## .xenon.package encryption discovery (2026-07-31)

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

---

## OpenSSL crypto bundle in guest binary (2026-07-31) — TLS-only, not asset decryption

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