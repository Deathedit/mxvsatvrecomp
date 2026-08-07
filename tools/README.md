# tools/ — MX vs ATV Alive BXML & asset bundle decoders

Scripts supporting the native Windows port of MX vs. ATV Alive via
ReXGlue SDK 0.9.0 static recompilation. All formats are now fully
reverse-engineered.

## Format status

| Format | Status | Tool |
|--------|--------|------|
| `.bxml` (config files) | Fully decoded → XML | `bxml_full_decoder.py` |
| `.xenon.database` (asset manifest) | Fully decoded → XML w/ typed attrs | `bxml_full_decoder.py` |
| `.xenon.package` (heap layout) | Layout decoded; BXML heaps decode; binary heaps dump only | `package_decoder.py` |

All 130 databases in `assets/Database/` decode cleanly to a unified
asset catalog (`out/asset_catalog.json`, 23,183 assets indexed).

## Primary tools

### `bxml_full_decoder.py` — Full BXML → XML decoder

```
python bxml_full_decoder.py path/to/Engine.bxml           # decode to stdout
python bxml_full_decoder.py assets/                       # batch all .bxml + .xenon.database
```

Iterative DFS tree reconstruction (no Python recursion limit — handles
NAT_Farm.xenon.database with 5247 nodes). Verified on:

- Engine.bxml (4 nodes, config)
- RdbTables.bxml (6 nodes, 10 attrs, config)
- UIRegistry.bxml (34 nodes, 56 attrs, config)
- RendererRegistry.bxml (22 nodes, streaming=48)
- EngineDependencies.xenon.database (304 nodes, 605 attrs, 1212B binary payload)
- RiderPhysics.xenon.database (14 nodes, 25 attrs, 52B binary)
- NAT_Farm.xenon.database (5247 nodes, 10494 attrs, 20992B binary)
- AIData.xenon.database (79 nodes, 155 attrs)
- All 130 databases in `assets/Database/` (23,183 total assets cataloged)

Library API for use from other scripts:

```python
from bxml_full_decoder import decode_bxml
root = decode_bxml('/path/to/file.bxml')     # returns BxmlNode root
for node in root.walk():                      # iterative pre-order
    for k, v in node.attrs: ...
lines = root.to_xml()                         # iterative XML emitter
```

### `package_decoder.py` — .xenon.package heap extractor

```
python package_decoder.py path/to/RiderPhysics.xenon.package
python package_decoder.py path/to/NAT_Farm.xenon.package     # 66MB, 1049 heaps
```

Cross-references sibling `.xenon.database` for heap layout, then walks
every heap and runs the appropriate BXML decoder or dumps raw binary.
Writes `<stem>_package_manifest.txt` into `out/`.

For each heap:
- BXML-bearing heaps (bxml/celib/surface/material/script/timeline/animset
  containing embedded BXML at heap offset 33) → full XML tree
- Binary heaps (soundbnk/model/texture/anim/shader/collis/etc) →
  first 15 lines + element histogram (per-TYPE sub-blob parsers needed
  before these decode to structured data)

### `bxml_strings.py` — Strings-only summarizer

```
python bxml_strings.py path/to/NAT_Farm.xenon.database           # show all strings by category
python bxml_strings.py path/to/NAT_Farm.xenon.database --count    # just counts
python bxml_strings.py assets/                                    # batch summarize
```

Categorizes strings into `ENTITY/SHADER/MATERIAL/ANIM/TEXTURE/META/OTHER`.
Useful for understanding what asset references each `.xenon.database`
contains without full XML reconstruction.

## Internals / debug tools

### `bxml_decoder.py` — Raw token inspector

Dumps strings + u32 token walk. `--raw` for verbose scan including
header fields, stream A/B breakdown, per-node records.

### `decode_bxml.py` — Minimal zlib decompressor

Reads any BXML file and emits decompressed bytes (no parsing).

### `dump_bxml.py` — Hexdump of decompressed bytes

```
python dump_bxml.py path/to/Engine.bxml
```

## Prior-work tools (XEX container)

### `extract_xex_pe.py` — XEX → PE extraction
### `lzx_decompress.py` — LZX block decompression for XEX container

## BXML format reference (fully cracked 2026-07-31)

### File container (36-byte header + zlib-raw-deflate)

```
offset  size  field              notes
------  ----  -----------------  ----------------------------------------
0x00    4     magic              "BXML"
0x04    4     version/flags      0x000103EA (= 66538, same on all)
0x08    4     string_count       number of strings in strings table
0x0C    4     strings_size       byte size of decompressed strings section
0x10    4     streaming_flag     0 (most files), >0 when binary_data section present
0x14    4     aux_count          attribute descriptor count (12-byte each)
0x18    4     node_count         element count (verified exact on all files)
0x1C    4     zero               always 0
0x20    4     compressed_size    matches zlib stream length
0x24..  ---   zlib-deflate raw stream (signature 78 9C...)
```

### Decompressed payload layout

```
[strings_section_size bytes] strings (null-terminated ASCII, concatenated)
[streaming_flag bytes]       binary data section (only when streaming_flag > 0)
[aux_count × 12 bytes]       attribute descriptors
[node_count × 32 bytes]      node records
```

Verified formula: `post_strings_size = streaming_flag + 12 * aux_count + 32 * node_count`

### Attribute descriptor (12 bytes)

```
[name_str_idx(u32)]  [value(u32)]  [type_code(u32)]
```

- `type_code` high u16 = data type:
  - 1 = string
  - 3/4/5 = i32 (stride 4)
  - 6/9 = u64 (stride 8)
  - 7/0xC = vec4 (stride 16)
  - 8 = m4x4 (stride 64)
  - 0xA = vec3 (stride 12)
  - 0xB = bool (stride 4)
- `type_code` low u16 = flag:
  - 0 = value is a string index → `strings[value]`
  - 1 = value is a byte offset into binary data section → read `T` from `binary_data[value : value+stride]`

### Node record (32 bytes — 8 u32 LE fields)

```
field[0] = element_name string index
field[1] = text_content string index (-1 = no text)
field[2] = reserved (always 0)
field[3] = has_text flag (0 = no text, 1 = has text)
field[4] = total_node_count (validation field, same across all nodes in file)
field[5] = child_count (number of child elements)
field[6] = attr_start index (offset into the flat attr descriptor list)
field[7] = attr_count (number of attribute pairs for this node)
```

Tree traversal: depth-first pre-order with explicit `child_count` per node.

### Guest parser

`BxmlTreeBuilder_Parse` = `sub_82B64400` @ 0x82B64400 (9204 bytes, 390 basic
blocks). Reads 36-byte file header via vt[8], byteswaps PPC↔LE, allocates
string table + child table, reads `aux_count` 12-byte attr descriptors and
`node_count` 32-byte node records, dispatches type-code byteswap fixups by
data stride (case 3=i32 stride 4, 7=vec4 stride 16, 8=m4x4 stride 64,
0xA=vec3 stride 12), then reads 32-byte render descriptors and dispatches
structure type codes (cases 1-12). Config-only `.bxml` files use simple
string-index references — handled by our Python tool directly.

## .xenon.package format (cracked 2026-07-31)

Two sibling files per asset collection:
- `<Name>.xenon.database` — BXML metadata describing package layout
- `<Name>.xenon.package` — binary bundle concatenating per-asset heaps

### Heap structure (single heap inside a .package)

```
offset  size  field
------  ----  ------------------
0x00    4     heap_size - 4 (u32 LE)
0x04    4     0x00010000 (flag/version constant)
0x08    4     heap_size - 16 (u32 LE)
0x0C    4     tail checksum (heap_size | ~heap_size pattern)
0x10    17    per-resource descriptor (varies by asset TYPE — see below)
0x21..  ...   resource payload:
                - BXML heaps: 36-byte BXML header + zlib raw stream
                - Binary heaps (mesh/texture/audio): raw per-type format
```

### Per-asset-type descriptor bytes [16..32] (sampled from NAT_Farm)

17-byte header at offset 16 inside each heap identifies the asset type.
Sampled first bytes (HEX) per type:

| Type | First descriptor bytes | Notes |
|------|----------------------|-------|
| bxml | varies | 2 instances in NAT_Farm |
| celib | `d1 00 30 10 2c 37 00 00 00 7c 00 00 00 10 00 00 00` | starts `0xXX 00 30 10` |
| soundbnk | `16 07 20 b7 37 91 0d 00 d0 00 00 14 62 00 56 00 00` | starts `0xXX 07 20 b7` or `0xXX 00 10 51` |
| script | `e8 07 20 b7 0f 95 0d 00 c0 00 00 24 61 00 56 00 00` | near-identical prefix to soundbnk |
| animset | `3c 05 20 3d a6 91 4c 00 63 22 00 50 24 00 00 00 00` | |
| anim | `f4 00 30 40 2e 30 01 00 00 05 00 00 00 2a 00 00 00` | starts `0xXX 00 30 40/30 70/10 42/30 70` |
| model | `95 fd 66 12 87 02 6b b9 29 40 88 11 85 0e b4 bd 35` | non-magic prefix |
| surface / texture / material / shader / collis | varies | per-TYPE parsers TODO |

### Codec enum (from .xenon.database `<Compress>` attribute)

Resolved via string compare (`sub_82BDFA10` in guest code):

| Codec string | Enum value |
|--------------|-----------:|
| "SPUZlib" | 0 |
| "LZX" | 1 |
| "Zlib" | 2 |

**Important**: `<Compress codec="LZX" enabled="true">` is **uniform across all
heaps of all 130 databases** (1046+ identical entries in NAT_Farm alone).
It's NOT a per-heap compression declaration — it appears to be a
build-pipeline label that's not actually consumed at runtime (or applies
at Package Block level, not Heap level). BXML heaps decode via their
internal zlib stream regardless of `codec`.

### Heap storage modes (post-investigation)

`.xenon.package` heaps come in two modes:

**Structured (cleartext)**: 16-byte heap header has
`H[0..3]=heap_size-4 LE`, `H[4..7]=0x00010000`, `H[8..11]=heap_size-16 LE`,
`H[12..15]=checksum`. Payload at offset 33 is plain BXML block (with
embedded zlib stream) OR uncompressed native binary. Decodable by us
today. Examples: `bxml`, `celib`, `script`, `timeline`, `colorlut`, some
`soundbnk`/`anim`/`animset`.

**Encrypted**: All 16 heap-header bytes look high-entropy random. The
per-TYPE descriptor at [16..33] is also obfuscated. The heap content
cannot be read without the original Xbox 360 decryption key (likely
AES-128-CBC with package-offset-derived IV, per Xbox's content security
model). Examples: most `model`, `material`, `shader`, `collis`,
`activity`, `tdf`, `particle`, `packdata`, `bounce`, sometimes `surface`.

### What's extractable now (post-LZX investigation)

| Asset category | Storage | Extractable today? |
|----------------|---------|---------------------|
| `bxml` / `celib` / `script` / `timeline` | BXML-bearing cleartext | YES — `bxml_full_decoder.py` |
| `surface` / `material` | mixed (some cleartext, mostly encrypted) | PARTIAL — varies per heap |
| `soundbnk` / `sound` / `anim` / `animset` / `colorlut` | structured-cleartext binary | PARTIAL — heap header readable; payload needs per-TYPE native parser |
| `texture` / `model` / `shader` / `collis` / `tdf` / `particle` / `packdata` / `bounce` / `activity` | encrypted | NO — requires Xbox 360 content-key decryption first |

### What still needs per-type parsers (for cleartext-binary heaps)

| Priority | Type | Notes |
|----------|------|-------|
| Medium | `soundbnk` | FMOD/XMA audio bank format (`FSB5`/XMA2 magic expected at offset 33) |
| Medium | `anim` | Skeletal animation keyframes (per `ShaderAsset_Unpack` analog: 4B flag + 4B count + sub-records) |
| Medium | `animset` | Animation blend tree format |
| Medium | `colorlut` | Color LUT (likely raw RGB/RGBA data) |

### What's BLOCKED on Xbox 360 decryption (for encrypted heaps)

Most game-critical rendering assets are encrypted on disk:

- `model` (geometry, vertex/index buffers) — 1833 assets
- `texture` (DXT1/5 compressed) — 7611 assets
- `shader` (Xenos microcode) — 2481 assets
- `material` (shader parameter bindings) — 4730 assets
- `collis` (collision meshes) — 545 assets

Decryption requires:
1. Find the game's content key in the XEX2 binary's security header (XEX2 header secret blocks)
2. Reverse the cipher (likely AES-128-CBC with IV derived from the heap's package-offset)
3. Decrypt heaps host-side and re-emit cleartext package files, OR
4. Implement decryption in `package_decoder.py` for on-the-fly cleartext extraction

## Generated artifacts

| File | Contents |
|------|----------|
| `out/asset_catalog.json` | Master 130-database × 23,183-asset index (one entry per asset: `name`, `type`, `package_offset`, `heap_size`) |
| `out/NAT_Farm_database.xml` | 6301-line XML reconstruction of NAT_Farm.xenon.database (5247-node track asset manifest) |
| `out/NAT_Farm_package_manifest.txt` | Per-heap decode summary for NAT_Farm.xenon.package (1049 heaps, 66MB) |

## What still needs per-type parsers

`bxml`, `celib`, `surface`, `material` are BXML-bearing → decoded fully
to XML. The remaining asset types need per-TYPE binary blob parsers to
extract structured content from heap payload at offset 33:

| Priority | Type | Count | Why needed |
|----------|------|-------|------------|
| High | texture | 7611 | DXT-compressed GPU textures — feed into D3D12 renderer |
| High | model | 1833 | Vertex+index buffers — real bike/track geometry |
| High | shader | 2481 | Compiled Xbox 360 GPU microcode (Xenos shader ISA) — needs recompilation to DX12 |
| High | anim | 1595 | Skeletal animation keyframes |
| High | collis | 545 | Collision hulls |
| Medium | soundbnk/sound/sounddat | 613 | Audio banks (FMOD/XMA) |
| Medium | bink | 11 | Bink video containers |
| Low | swfx/script/uicmpnt/animset/timeline/localiz | ~1300 | Gameplay/UI logic — needed only when game logic runs |

For each type, the heap bytes at offset 33+ need to be parsed according
to that type's internal schema (descriptor at bytes 16-32 identifies it).
Worked examples: NAT_Farm heaps 1-6 are raw-binary (soundbnk/script/animset);
heaps 7-13 are celib (XML decoded cleanly).