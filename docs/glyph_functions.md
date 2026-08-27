# Scaleform GFx glyph path — function map

The XEX is stripped: **no function name in the IDB contains "glyph"**. This set was
derived from the 8 format strings that mention glyphs, then expanded along calls.
IDB: `assets/default.xex.probe.i64` — all 19 verified functions are now named
`GFx_*` in the database. `0x829AAB18` is left as `sub_` on purpose: it calls the
composition loop but was never decompiled, so any name would be a guess.

Trap noted while deriving this: the "data xrefs" at 0x821B4xxx/0x821B8xxx that look
like vtables are `.pdata` unwind records (pairs of `BeginAddress, 0x4000xxxx`).
Do not read them as vtables.

## Text layout — decides whether a glyph is emitted at all

| Addr | IDB name | Role |
|---|---|---|
| `0x829A9838` | `GFx_TextLine_ComposeGlyphs` | **Line composition loop.** Per character: resolve font -> glyph index -> advance -> emit a record. Owner of the `Missing "%s" glyph '%c' (0x%x)` log. |
| `0x829AAB18` | `sub_829AAB18` (deliberate) | Calls the loop above. NOT decompiled — 285 blocks, complexity 157, reached from 11 sites, so it is bigger than a driver. Left unnamed rather than guessed. |
| `0x829A3C00` | `GFx_Font_BuildTextureGlyphList` | Walks glyph indices 0..GetGlyphCount, builds the texture-glyph list for a font. |

`0x829A9838` is the refusal point: `a1[323]` is the glyph index from vtable slot `+12`
(GetGlyphIndex). When it comes back `-1` the "Missing glyph" branch runs *once per
movie* (`*(a1[0]+319) |= 0x10` latches it), and no quad is produced. Everything
downstream is skipped, so an absent letter leaves no trace in the render path.

Font vtable slots used here: `+12` GetGlyphIndex, `+24` GetGlyphShape,
`+28` GetTextureGlyphData, `+32` GetAdvance, `+36` GetKerningAdjustment,
`+48` GetGlyphBounds, `+56` GetGlyphCount.

## Glyph cache — raster and vector

| Addr | IDB name | Role |
|---|---|---|
| `0x828AC620` | `GFx_GlyphCache_BuildLineGlyphs` | **Main cache dispatcher** (0x1378). Two passes (`v81` = 1 shadow / 0 outline), builds the per-glyph quad and UVs. Calls both cache paths below. |
| `0x828A8C40` | `GFx_GlyphCache_GetRasterGlyph` | Raster path. Fast route via `0x82945A40` when the font has a baked texture-glyph array; otherwise falls to `0x8293F248`. Logs *"Increase raster glyph cache capacity - TextureConfig"*. |
| `0x828AC150` | `GFx_GlyphCache_GetVectorGlyph` | Vector path. LRU over `a1+2312`; evicts when `>= *(a1+2504)`. Logs *"Increase vector glyph cache capacity - SetMaxVectorCacheSize()"*. |
| `0x828AD998` | `GFx_GlyphCache_GetVectorGlyph_Locked` | Critical-section wrapper around `0x828AC150`. |
| `0x8293F248` | `GFx_RasterCache_FindOrRasterize` | Raster cache lookup; MRU-relinks the hit, else rasterizes via `0x8293E720`. |
| `0x8293E720` | `GFx_RasterCache_RasterizeGlyph` | **Rasterizer.** Renders the glyph outline into a cache-texture slot; handles padding, blur/shadow (`0x8293C308`/`0x8293C6F0`), knockout, distance-field. |
| `0x8293E5B8` | `GFx_RasterCache_AllocSlot` | Allocates the slot (three fit strategies: `0x8293DFA8`, `0x8293E008`, `0x8293E1C0`). |

Both capacity warnings clear a latch byte (`a1+37` raster, `a1+38` vector) so they
print **once**. A cache that is silently full looks identical to one that is fine.

## Font-side texture glyphs

| Addr | IDB name | Role |
|---|---|---|
| `0x82945A40` | `GFx_Font_GetTextureGlyph_DummyOnOOB` | `GetTextureGlyph(i)` — 40-byte stride at `a1+24`, count at `a1+28`. **Out-of-range returns a shared static dummy** at `0x830AA3C8` (all-zero UVs), not null. |
| `0x829494B8` | `GFx_Font_SetTextureGlyph` | `SetTextureGlyph(i, src)` — grows the array, then `0x82943C88`. |
| `0x82943C88` | `GFx_TextureGlyph_Copy` | Copies a TextureGlyph: texture ref + 4 UVs + 2 offsets. |
| `0x82949D88` | `GFx_Font_BindFontTexture` | Binds a font-texture id to a font. |
| `0x82945A08` | `GFx_TextureGlyph_ResolveTexture` | Resolves a texture-glyph record to its texture. |

`0x82945A40`'s dummy return is worth remembering: an index past the end yields a
glyph with zero-area UVs that samples texel (0,0) rather than failing loudly.

## Loaders (SWF/GFX tag parsers)

| Addr | IDB name | Role |
|---|---|---|
| `0x82924518` | `GFx_Load_DefineFontTextureInfo` | DefineFontTextureInfo — parses the baked atlas, `TEXGLYPH[%d]` uv bounds, `PadPixels`/nominal size/`numTexGlyphs`. |
| `0x82947418` | `GFx_Load_DefineFont2_3_Native` | DefineFont2 / DefineFont3 -> **native font builder**. |
| `0x82949E10` | `GFx_Load_DefineFont_Shape` | DefineFont / DefineFont2 / DefineFont3 -> **GFxShape objects**. |
| `0x8299C1D8` | `GFx_Load_DefineText` | DefineText / DefineText2 — static text, `GlyphRecords: count = %d`. |
| `0x82902768` | `GFx_Import_ResolveResources` | Import resolution; special-cases the `_glyphs` export from `gfxfontlib.swf`. |

### Two loaders, overlapping tags

`0x82947418` and `0x82949E10` **both** claim tags 48 (DefineFont2) and 75
(DefineFont3); they are separate entries in the tag table (dispatched from
`0x82921948` and `0x82921980`). They are not variants of one function — they
produce different things:

- `0x82947418` walks each glyph's path and emits it into a native font object
  (`0x829DAB38` create, `0x829DB4D0` init, `0x829DB328` finish) via
  MoveTo `0x829DAF78` / LineTo `0x829DAFE8` / CurveTo `0x829DB0C8` /
  EndGlyph `0x829DB618`. Output is font units; coordinates are scaled by
  `emScale/1024` where `emScale` comes from `*(*(*(a2+16)+8)+36) + 12`.
- `0x82949E10` builds a `GFxShape` per glyph (vtable `off_820DE9F0`) into the
  array at `font+32`, and — only when HasLayout — a 12-byte bounds record per
  glyph at `font+48`.

Which one runs decides whether glyphs later resolve through the shape path or
the native path, so identify the loader before reasoning about a glyph's shape.

### Field encodings both loaders share

- **DefineFont3 is 20x.** Tag 75 scales advance/ascent/descent/leading/kerning
  by `0.05`; tag 48 by `1.0`. Miss this and every DefineFont3 metric is 20x too
  large.
- **Offset table width** follows the wide-offset flag: 32-bit if set, else
  16-bit. **Code table width** follows bit 14 of `font+20`: 16-bit if set,
  else 8-bit. Kerning pair codes follow the same bit 14.
- **DefineFont (tag 10)** has no glyph count; `0x82949E10` derives it as
  `offset[0] >> 1`.

### Refusal points these loaders add

Three more places where a glyph quietly does not exist, on top of the four in
`0x829A9838`:

1. **First offset reads 0** — both loaders set `font+20 |= 0x1000`, skip shape
   parsing entirely, and report zero glyphs. No error is logged.
2. **A zero offset mid-table** (`0x82949E10`, tag 10 path) truncates the glyph
   list at that point. Also silent.
3. **Degenerate glyph bounds** (`0x82949E10`, HasLayout path): if the shape's
   bounds come back inverted or empty, the loader writes an **all-zero** bounds
   record rather than skipping the glyph. Absent becomes a zero-size glyph —
   the same failure shape as `0x82945A40`'s dummy return.

The only loader refusal that *is* logged is the kerning-table overrun in
`0x82949E10`: `Error: Corrupted file %s, kerning table of the font '%s' is
longer than tagLength`. It aborts the kerning loop but keeps the font.
