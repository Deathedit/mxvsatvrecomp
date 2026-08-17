# tools/ — MX vs ATV Alive BXML, asset bundle & shader decoders

Scripts supporting the native Windows port of MX vs. ATV Alive via
ReXGlue SDK 0.9.0 static recompilation. All formats are now fully
reverse-engineered.

## Format status

| Format | Status | Tool |
|--------|--------|------|
| `.bxml` (config files) | Fully decoded → XML | `bxml_full_decoder.py` |
| `.xenon.database` (asset manifest) | Fully decoded → XML w/ typed attrs | `bxml_full_decoder.py` |
| `.xenon.package` (asset bundle) | Fully decompressed to asset bytes | `package_decoder.py` + `xcompress.py` |

All 130 databases in `assets/Database/` decode cleanly, and every package
decompresses with 100% heap coverage, no gaps and nothing past EOF.

⚠️ `out/asset_catalog.json` and anything else generated before 2026-08-17
is WRONG and should be regenerated. Two bugs, both of which produced
plausible output rather than an error:

- The database tree was walked by record ORDER instead of the explicit
  first-child index, which mis-parented every file. MXUI listed 324
  assets; it really has 1533.
- Package heaps were never actually decompressed. See `xcompress.py`.

## Primary tools

### `bxml_full_decoder.py` — Full BXML → XML decoder

```
python bxml_full_decoder.py path/to/Engine.bxml           # decode to stdout
python bxml_full_decoder.py assets/                       # batch all .bxml + .xenon.database
```

Tree reconstruction uses each record's explicit first-child index
(field[4]) — the record order is NOT pre-order and NOT breadth-first, and
this decoder assumed each of those in turn before measuring. Verified by
reachability: across all 130 `.xenon.database` files every record is
reached exactly once, with no edge pointing backwards or past the end.

Emitters are iterative, so a deep tree cannot blow the recursion limit.
Verified on:

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

### `package_decoder.py` — .xenon.package asset extractor

```
python package_decoder.py path/to/MXUI.xenon.package                    # manifest only (fast)
python package_decoder.py path/to/MXUI.xenon.package --extract "*.layer"
python package_decoder.py path/to/MXUI.xenon.package --search SmokeVideo --filter "*.layer"
```

Cross-references the sibling `.xenon.database` for the asset table, then
decompresses via `xcompress.py`. Always writes
`<stem>_package_manifest.txt` into `out/`; `--extract` also writes the
decompressed bytes of each matching asset to `out/<stem>/`.

The manifest header reports coverage, gap count and past-EOF count. Those
three numbers are the check that caught two rounds of wrong heap offsets
— read them before trusting anything below.

Decompression is pure Python and slow (a 33 MB package is minutes), so
`--extract` and `--search` take a glob and decode only what matches.
`--search` greps decompressed assets, decoding BXML ones to XML first; a
plain grep of the package file finds nothing, because everything is
compressed.

### `xcompress.py` — the Xbox 360 chunk container inside a heap

Not a CLI; used by `package_decoder.py`. Owns the three layers between a
heap extent and asset bytes: the heap's stream list, the XCompress chunk
framing (`0xFF`-prefixed, or a bare BE16 length implying a full 0x8000
frame), and the LZX window size of **17 bits**. All three were determined
by measurement, and the file records what each wrong guess looked like —
in every case, silently truncated output rather than an error.

### `bxml_strings.py` — Strings-only summarizer

```
python bxml_strings.py path/to/NAT_Farm.xenon.database           # show all strings by category
python bxml_strings.py path/to/NAT_Farm.xenon.database --count    # just counts
python bxml_strings.py assets/                                    # batch summarize
```

Categorizes strings into `ENTITY/SHADER/MATERIAL/ANIM/TEXTURE/META/OTHER`.
Useful for understanding what asset references each `.xenon.database`
contains without full XML reconstruction.

### `xenos_shader_disasm.py` — Xenos microcode disassembler

```
python tools/xenos_shader_disasm.py logs/hlsldump/ps_2146CCA0.txt   # disassemble
python tools/xenos_shader_disasm.py --verify logs/hlsldump/*.txt    # check vs C++
python tools/xenos_shader_disasm.py --selftest                      # unit checks
python tools/xenos_shader_disasm.py --scan-file <binary>            # scan raw file
```

Decodes Xbox 360 GPU shader microcode to readable assembly. Every bit layout
and opcode table is transcribed from Xenia's `src/xenia/gpu/ucode.h` and
`ucode.cc` — control flow is 48-bit packed two per three dwords, ALU and fetch
instructions are 3 dwords each.

Primary input is the renderer's own dump: `hooks_d3d9.cpp` writes a
`=== GUEST MICROCODE (n dwords) ===` block into each `logs/hlsldump/*.txt`
beside the emitted HLSL. This tool reads that section directly, which is what
makes a translation checkable against its *input* rather than against itself.

**Verified**: `--verify` cross-checks three independent facts per shader —
position export, export register mask, distinct sampler count — against the
header the C++ translator wrote. Agrees on all 160 dumps carrying microcode
(92 ps + 68 vs; the 96 `vsfetch_*` variants carry HLSL only). `--selftest`
covers swizzle relativity, CF pack/unpack round-trip, and rejection of
all-zero, 0xFFFFFFFF-fill, and real PowerPC code.

Also runs inside IDA (File > Script file...) to scan **non-executable**
segments for embedded microcode. It skips `.text`/`BINK` outright; scanning PPC
code is what made an earlier attempt report 586,594 phantom "shader blocks".
Annotation is off by default (`ANNOTATE`), and `STRIP = True` removes only the
comments it wrote.

Note on coverage: the 2481 packaged `shader` assets live in LZX-compressed
`.xenon.package` heaps, so scanning cannot see them. They now decompress
(`package_decoder.py --extract`), but the per-type `shader` payload format
is not parsed yet, so runtime dumps remain the reliable source.

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

### Heap storage (RETRACTED: nothing here is encrypted)

**Every heap is LZX-compressed, and all of them now decompress.** This
section previously claimed that most heaps were encrypted with an Xbox
360 content key and listed key recovery as blocked work. That was wrong.

The evidence for "encrypted" was that heap headers and payloads looked
like high-entropy random bytes. They do — that is what compressed data
looks like. Entropy cannot tell encryption from compression, and it was
the only test applied. What actually made the heaps unreadable was three
container layers being mis-parsed at once; see `xcompress.py`.

The claim was also self-refuting and it went unnoticed: the same section
recorded that `bxml`/`script`/`celib` heaps were readable cleartext. A
content-key scheme that leaves the script and UI assets in the clear and
encrypts only the textures is not a scheme anyone ships.

Real layout, verified against all 13 packages — 100% heap coverage, no
gaps, nothing past EOF:

```
HEAP     u32 heap_len (= heap_size - 4)
         one or more STREAMs, filling exactly to heap_size - 4
         u32 trailer
STREAM   u32 0x00010000
         u32 stream_len            -- includes the 5-byte terminator
         XCompress chunk framing, then five zero bytes
CHUNK    0xFF, u16 BE uncompressed, u16 BE compressed
         or bare u16 BE compressed, uncompressed implied 0x8000
LZX      window_bits = 17, 16-bit LE bitstream words
```

Decompressed bytes are homogeneous per asset type, which is the check
that the container is right:

| Type | First 4 bytes | Contents |
|------|---------------|----------|
| `uicmpnt` | `BXML` | UI component tree — layers, components, script + material bindings |
| `texture` | `77 DD CC 00` | header with BE width/height at +0x2C, then pixel data |
| `swfx` | `00 00 00 01` | Scaleform |
| `localiz` | `00 00 00 02` | localized string tables |
| `script` | BE u32 length | Lua **source**, not bytecode |

### What still needs per-type parsers

The container is solved; the per-type payload formats are not. `texture`
is partly known (dimensions confirmed against the runtime `sub_826295E8`
hook). `model`, `shader`, `collis`, `soundbnk`, `anim`, `animset` and
`colorlut` decompress to bytes nobody has parsed yet.

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