"""Read .texture assets: dimensions, format, mip table -- and decode them to PNG.

A `.texture` asset is a small big-endian header followed by the texture in the
CONSOLE'S OWN LAYOUT: guest formats (DXT1/DXT4_5/DXN/...), tiled with the Xenos
address swizzle, big-endian. This reads the header, and optionally untiles and
decodes level 0 into a PNG you can actually look at.

Why bother, when the runtime already decodes textures: because at runtime a
texture is a guest ADDRESS and a fetch constant, with no name attached
([[shader-handles-are-not-stable]]). Here they have names. "Which asset is the
2048x2048 the terrain samples" is a question only the files can answer.

    python tools/texture_reader.py out/tex/FR_Dunes --summary
    python tools/texture_reader.py out/tex/FR_Dunes/FR_DU_Disp.texture
    python tools/texture_reader.py <file.texture> --png out/png
    python tools/texture_reader.py out/tex/FR_Dunes --png out/png --grep Nm

THE HEADER, all big-endian u32, verified against 264 assets:

    +0x00  magic 0x77DDCC00
    +0x04  6              version; 6 in every asset seen
    +0x08  0
    +0x0C  1
    +0x10  levels         mip count (log2(max dim) + 1 for a full chain)
    +0x14  8              10 and 0x18 also occur, meaning unknown
    +0x18  0  (x4 dwords, through +0x24)
    +0x28  levels         repeated
    +0x2C  width
    +0x30  height
    +0x34  format word    see below
    +0x38  levels x (size_bytes, pitch_bytes)
           then the pixel data, at 0x38 + 8 * levels

The level table's PITCH is exact -- `width_blocks * bytes_per_block` for every
asset checked. Its SIZE is not the stored size: it is computed as
`pitch * height_in_TEXELS`, which for a 4x4-block format overstates by 4x. Do
not use it to walk levels; derive offsets from the geometry instead.

THE FORMAT WORD is only partly understood, and this file says which parts:

  * bits 0-5   the guest texture format, `xenos::TextureFormat`. VERIFIED two
               ways -- the names line up (every 49/DXN asset is called *Nrml*,
               *NRML* or *NM*) and so do the byte counts (DXT1 lands on 0.67
               bytes/texel, which is 0.5 x the 4/3 mip-chain factor; DXT4_5 and
               DXN on 1.33; 8_8_8_8 on 4.0).
  * bit 8      TILED. Not derived here: [[guest-texture-format-table]] pinned it
               from the engine's own paired render-target tables at 0x82D54414
               (tiled) and 0x82D5448C (linear), which are identical except for
               this bit. It agrees with the assets -- the 8x8 and 1x1 textures
               have it clear, everything large has it set.
  * the rest   REPORTED RAW. Eight distinct words occur across 264 assets and
               they share too much for the remaining fields to be separable
               from this sample. `--words` prints the census. Guessing here
               would produce a plausible-looking column that is wrong, which is
               worse than a blank one.

LAYOUT, checked by PREDICTING each file's data size and comparing -- run it
yourself with `--verify`. Over all 6896 extracted textures:

    exact                     6872   99.65%
    trailing extra (<256 B)     19    0.28%
    MISMATCH                     5    0.07%

predict_data_size carries the rules and how each was fitted. Level 0 always
starts at the data offset, so --png depends on NONE of it; the model exists
because a reader that cannot predict its own input has not been checked.

The three outcomes are kept apart on purpose. "Trailing extra" is real bytes
past the last level -- GL_FR_Sky_DU_Chrom carries 201, and the amount varies
per file -- which the model cannot predict and should not pretend to. Folding
those into either column would misreport them.

The 5 that MISMATCH are ONE ASSET -- Ve_EuroTeamTruck_FMF_Side (512x257),
duplicated across five packages. Counting files overstates the evidence: there
is one unexplained shape, not five.

A non-power-of-two height is NOT the trigger on its own: 1280x430 and 1280x720
have one and predict exactly. Those are single-level, so they never exercise the
mip chain. The trigger is a LINEAR texture with MIPS and a dimension that is not
a power of two.

FOUND EMPIRICALLY, by decoding candidate offsets and scoring each against level
0 box-filtered to half size. On Ve_EuroTeamTruck_FMF_Side (512x257, 8_8_8_8):

    level  dims       offset    span      match
    0      512x257         0    589824    -        = pitch 2048 x align32(257)
    1      256x128    589824    262144    err 0.00
    2      128x64     851968     65536    err 0.00
    3      64x32      917504     -        err 0.00

The slack after each level's content is ZERO -- 0 of 131072 bytes non-zero
after level 1 -- so it is padding, not a second slice and not data being missed.

LEVEL 0 IS SOLVED: align256(pitch) x align32(rows), for all three shapes. Read
straight off the padding boundaries, since the slack is zero-filled and a run of
zeros therefore marks exactly where a level's content stops:

    640x360    content ends  115200, zeros to  122880  =  1280 x 96
    512x257    content ends  526336, zeros to  589824  =  2048 x 288
    4095x511   content ends 8372220, zeros to 8388608  = 16384 x 512

The pitch is ALIGNED UP to 256, not floored at it -- 4095 texels of 8_8_8_8 is
16380 bytes, which floors to itself and aligns to 16384. Those 4 bytes times 512
rows were the whole discrepancy on the 4095x511 assets.

4095x511 IS NOW EXACT under the same rule applied to every level -- no special
case, no align64K. What had looked like a separate rule for later levels was the
floor-vs-align bug above, in the rows.

640x360 IS NOW EXACT TOO. Its levels were never mis-counted -- the packed tail
starts exactly where the `dimension <= 16` rule says. What was wrong is the mip
PITCH, and measuring it settled it:

    level  dims      pitch measured   err vs next-best
    0      640x360   1280             (from the padding boundary, 1280 x 96)
    1      320x180   1024              0.782 vs 82.6
    2      160x90     512              1.635 vs 84.1

1024 is the pitch of a 512-wide level and 512 that of a 256-wide one. MIP LEVELS
USE THE WIDTH ROUNDED UP TO A POWER OF TWO; level 0 uses its own. For a
power-of-two texture the two are identical, which is why this never showed until
a 640-wide asset with mips turned up.

ONE SHAPE REMAINS: 512x257, predicted 770048, actual 950272. Every one of its
widths is ALREADY a power of two, so the pitch rule cannot be what it needs. Its
levels sit at 589824, 851968, 917504 and each one after level 0 occupies exactly
TWICE what this model computes -- 131072 in 262144, 32768 in 65536, 8192 in
16384, all confirmed zero-filled. Its anomaly is in the ROWS, and one asset is
not enough to tell what rule produces it.

METHOD NOTE, because the first attempt failed and looked like it worked:
Vignetting was the obvious test file and is useless for this -- it is a smooth
gradient, so every candidate offset matches about as well as every other, and
the scan's "best" hit was inside level 0 itself. A search like this needs an
image with STRUCTURE; the FMF logo gives err 0.00 against a next-best of 11.24.

WHAT --png DECODES, and how each was checked. DXT1, DXT4_5, DXN, 8_8_8_8, 8,
16 and 16_16_16_16_EXPAND. Sampled across all ten (format, tiled) combinations
-- every format-29 asset plus a spread of the rest, 330 textures -- 330 decoded,
0 exceptions. A format with no entry in BLOCKS is still REFUSED with a reason
rather than decoded by guesswork.

  DXT4_5   Corona is white in every texel (one distinct R) with a radial alpha
           whose mean falls monotonically 98.5 -> 9.3 from centre to rim. A
           tiling error cannot leave a radial gradient radial, so this checks
           the untiling and the alpha decoder at once.
  DXT1     FR_DU_Truck comes out as a legible truck atlas -- side, top, bed,
           grille, tailgate -- with a chrome grille grey and tail lights red,
           so no channel is swapped.
  DXN      FR_DU_JettyRocks01_Nm centres both channels on 127, and ZERO
           sampled texels have x^2 + y^2 > 1, which a wrong decode breaks
           immediately.
  16       FR_DU_Disp is a coherent dune heightfield.

  16_16_16_16_EXPAND is HALF-FLOAT despite the name. Alpha reads exactly 1.0
           across all 16384 texels of FR_DU_ReflectionMap (0x3C00 is fp16 1.0
           and nothing else), RGB spans 0..1.77, neighbouring texels vary
           smoothly, and the decoded image is a dunes reflection cubemap -- sand,
           horizon, sky. HDR above 1.0 is CLIPPED for the 8-bit PNG: fine to look
           at, wrong to measure from.

Cube assets store 6 faces and count them in `levels` -- see face_count, which
derives the mip count from the DIMENSIONS because `levels % 6 == 0` misreads a
32x32 texture as a cube. Only face 0 is decoded; nothing here splits faces.
"""

import argparse
import binascii
import collections
import glob
import math
import os
import struct
import sys
import zlib

MAGIC = 0x77DDCC00
HEADER_FIXED = 0x38

# xenos::TextureFormat, transcribed from GuestTextureFormatName in
# src/gpu/d3d9_texture.cpp so the two cannot disagree about an index.
FORMAT_NAMES = [
    "1_reverse", "1", "8", "1_5_5_5", "5_6_5", "6_5_5", "8_8_8_8",
    "2_10_10_10", "8_A", "8_B", "8_8", "CR", "Y1", "SHADOW", "8_8_8_8_A",
    "4_4_4_4", "10_11_11", "11_11_10", "DXT1", "DXT2_3", "DXT4_5", "DXV",
    "24_8", "24_8_FLOAT", "16", "16_16", "16_16_16_16", "16_EXPAND",
    "16_16_EXPAND", "16_16_16_16_EXPAND", "16_FLOAT", "16_16_FLOAT",
    "16_16_16_16_FLOAT", "32", "32_32", "32_32_32_32", "32_FLOAT",
    "32_32_FLOAT", "32_32_32_32_FLOAT", "32_AS_8", "32_AS_8_8", "16_MPEG",
    "16_16_MPEG", "8_INTERLACED", "32_AS_8_INTERLACED",
    "32_AS_8_8_INTERLACED", "16_INTERLACED", "16_MPEG_INTERLACED",
    "16_16_INTERLACED", "DXN", "8_8_8_8_AS_16_16_16_16",
    "DXT1_AS_16_16_16_16", "DXT2_3_AS_16_16_16_16", "DXT4_5_AS_16_16_16_16",
    "2_10_10_10_AS_16_16_16_16", "10_11_11_AS_16_16_16_16",
    "11_11_10_AS_16_16_16_16", "32_32_32_FLOAT", "DXT3A", "DXT5A", "CTX1",
    "compiler_61", "compiler_62", "compiler_63",
]

# (block_width, block_height, bytes_per_block) for the formats these assets
# actually use. Anything absent is reported but not decoded, rather than
# decoded with a guessed block size.
BLOCKS = {
    2: (1, 1, 1),      # 8
    6: (1, 1, 4),      # 8_8_8_8
    18: (4, 4, 8),     # DXT1
    19: (4, 4, 16),    # DXT2_3
    20: (4, 4, 16),    # DXT4_5
    24: (1, 1, 2),     # 16
    29: (1, 1, 8),     # 16_16_16_16_EXPAND
    49: (4, 4, 16),    # DXN
}


def tiled_offset_2d(x, y, pitch_blocks, bpb_log2):
    """Xenos 2D tiled address, in bytes, for a block at (x, y).

    Transcribed from XeniaTiledOffset2D in src/gpu/d3d9_texture.cpp, which the
    runtime checks against the Xbox SDK's own GetTiledOffset2D on every start
    and has never disagreed with. That check is the reason this can be copied
    with confidence rather than re-derived.
    """
    tile = 32
    pitch = (pitch_blocks + (tile - 1)) & ~(tile - 1)
    macro = ((x >> 5) + (y >> 5) * (pitch >> 5)) << (bpb_log2 + 7)
    micro = ((x & 7) + ((y & 0xE) << 2)) << bpb_log2
    offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4)
    return (((offset & ~0x1FF) << 3) + ((y & 16) << 7) +
            ((offset & 0x1C0) << 2) +
            ((((((y & 8) >> 2) + (x >> 3)) & 3)) << 6) + (offset & 0x3F))


def read_header(path):
    with open(path, 'rb') as f:
        buf = f.read()
    if len(buf) < HEADER_FIXED or struct.unpack('>I', buf[:4])[0] != MAGIC:
        raise ValueError('not a .texture (magic is not 0x77DDCC00)')
    g = lambda o: struct.unpack('>I', buf[o:o + 4])[0]  # noqa: E731
    levels = g(0x10)
    if not 1 <= levels <= 64:
        raise ValueError('implausible level count %d' % levels)
    data_off = HEADER_FIXED + 8 * levels
    if data_off > len(buf):
        raise ValueError('level table runs past end of file')
    table = [(g(HEADER_FIXED + 8 * i), g(HEADER_FIXED + 8 * i + 4))
             for i in range(levels)]
    word = g(0x34)
    fmt = word & 0x3F
    return {
        'path': path, 'buf': buf, 'version': g(0x04), 'levels': levels,
        'width': g(0x2C), 'height': g(0x30), 'word': word, 'format': fmt,
        'format_name': FORMAT_NAMES[fmt], 'tiled': bool(word & 0x100),
        'unknown14': g(0x14), 'table': table, 'data_offset': data_off,
        'data_size': len(buf) - data_off,
    }


def face_count(t):
    """6 for a cube, 1 otherwise.

    A cube stores `levels` = 6 x its mip count, so the mip count has to come
    from the DIMENSIONS to tell it apart from a 2D texture whose level count
    merely divides by six. Testing `levels % 6 == 0` instead reads a 32x32
    texture -- which has exactly 6 mips -- as six faces and overstates its size
    threefold.
    """
    m = max(t['width'], t['height'])
    mips = int(math.log2(m)) + 1 if m > 0 else 1
    return 6 if t['levels'] == 6 * mips and t['width'] == t['height'] else 1


def predict_data_size(t):
    """What the payload SHOULD measure, from the geometry alone.

    Nothing in the reader needs this -- level 0 sits at the data offset the
    header states. It exists to be CHECKED against the real size, because a
    layout model that is never tested is a guess, and the packed-mip-tail rule
    below was wrong twice before it was right. `--verify` runs it over a whole
    directory.

    The rules, each fitted to the 6896 extracted textures rather than assumed:

      tiled   each level padded to 32x32 BLOCKS.
      linear  each row padded to 256 bytes, and rows padded to 32 -- EXCEPT for
              a single-level texture, which is stored tight: a 1x1 8_8_8_8 is
              256 bytes, not 8192.
      DXN     linear rows pad to 512, not 256. BC5 is two BC4 planes and each
              appears to take the minimum separately; every DXN linear texture
              came out at exactly twice the 256 prediction.
      tail    levels stop once a dimension reaches 16; the rest share that
              level's padding ([[packed-mip-tail]]).
      cube    all of the above per face, times six.
    """
    if t['format'] not in BLOCKS:
        return None
    bw, bh, bpb = BLOCKS[t['format']]
    faces = face_count(t)
    levels = max(1, t['levels'] // faces)
    min_row = 512 if t['format'] == 49 else 256
    w, h, total, n = t['width'], t['height'], 0, 0
    while n < levels:
        # Mip levels use the width ROUNDED UP TO A POWER OF TWO; level 0 uses
        # its own. Measured on Vignetting (640x360 DXT1) by decoding each level
        # at candidate pitches: level 1 is 320 wide but its pitch is 1024, the
        # pitch of a 512-wide level (err 0.782 against 82 for the next best),
        # and level 2 is 160 wide at pitch 512, a 256-wide level's (err 1.635
        # against 84).
        ww = w if n == 0 else (1 if w <= 1 else 1 << ((w - 1).bit_length()))
        wb = (ww + bw - 1) // bw
        hb = (h + bh - 1) // bh
        if t['tiled']:
            total += ((wb + 31) & ~31) * ((hb + 31) & ~31) * bpb
        else:
            # Gated on the texture's OWN level count, not the per-face one: a
            # 1x1 cube has six levels and IS padded, while a 1x1 2D texture has
            # one and is not.
            # BOTH are ALIGNED UP, not floored. `max(32, hb)` and `max(256,
            # pitch)` are the same mistake twice: they only differ from an
            # alignment when the value is already above the minimum but not a
            # multiple of it, which is exactly the non-power-of-two case this
            # model kept missing. 511 rows floors to 511 and aligns to 512;
            # 4095 texels of 8_8_8_8 is 16380 bytes, which floors to itself and
            # aligns to 16384. Together they account for the entire 30720-byte
            # shortfall on GL_TO_TNB_NW, and the padding boundaries confirm both
            # -- level 0's zeros end at exactly 16384 x 512 = 8388608.
            rows = hb if t['levels'] == 1 else (hb + 31) // 32 * 32
            pitch = (wb * bpb + min_row - 1) // min_row * min_row
            total += pitch * rows
        n += 1
        if w <= 16 or h <= 16:
            break
        w = max(1, w // 2)
        h = max(1, h // 2)
    return total * faces


def untile_level0(t):
    """Level 0 as tightly packed blocks, tiling and pitch resolved."""
    if t['format'] not in BLOCKS:
        raise ValueError('no block geometry for format %s' % t['format_name'])
    bw, bh, bpb = BLOCKS[t['format']]
    wb = (t['width'] + bw - 1) // bw
    hb = (t['height'] + bh - 1) // bh
    # The asset states its own pitch, so nothing here has to reconstruct it
    # from a fetch constant. Floor it at the real width: a stated pitch below
    # the image is a corrupt header, not a narrow image.
    pitch_blocks = max(t['table'][0][1] // bpb, wb)
    bpb_log2 = bpb.bit_length() - 1
    src = t['buf']
    base = t['data_offset']
    out = bytearray(wb * hb * bpb)
    for y in range(hb):
        for x in range(wb):
            if t['tiled']:
                off = base + tiled_offset_2d(x, y, pitch_blocks, bpb_log2)
            else:
                off = base + (y * pitch_blocks + x) * bpb
            if off + bpb > len(src):
                raise ValueError('level 0 runs past end of file')
            d = (y * wb + x) * bpb
            out[d:d + bpb] = src[off:off + bpb]
    return bytes(out), wb, hb


def _swap16(b):
    """Xbox stores these block formats as big-endian 16-bit words."""
    a = bytearray(b)
    a[0::2], a[1::2] = b[1::2], b[0::2]
    return bytes(a)


def _c565(c):
    r = (c >> 11) & 0x1F
    g = (c >> 5) & 0x3F
    b = c & 0x1F
    return ((r * 255 + 15) // 31, (g * 255 + 31) // 63, (b * 255 + 15) // 31)


def _dxt_colors(block, punchthrough):
    c0, c1 = struct.unpack('<HH', block[:4])
    bits = struct.unpack('<I', block[4:8])[0]
    a, b = _c565(c0), _c565(c1)
    if c0 > c1 or not punchthrough:
        c2 = tuple((2 * a[i] + b[i]) // 3 for i in range(3))
        c3 = tuple((a[i] + 2 * b[i]) // 3 for i in range(3))
        alpha3 = 255
    else:
        c2 = tuple((a[i] + b[i]) // 2 for i in range(3))
        c3 = (0, 0, 0)
        alpha3 = 0
    pal = (a, b, c2, c3)
    return pal, bits, alpha3


def decode_blocks(fmt, data, wb, hb):
    """Blocks -> RGBA rows. Returns (pixels, width, height)."""
    w, h = wb * 4, hb * 4
    px = bytearray(w * h * 4)

    def put(bx, by, i, rgb, alpha):
        x, y = bx * 4 + (i & 3), by * 4 + (i >> 2)
        o = (y * w + x) * 4
        px[o:o + 4] = bytes((rgb[0], rgb[1], rgb[2], alpha))

    for by in range(hb):
        for bx in range(wb):
            if fmt == 18:  # DXT1
                blk = _swap16(data[(by * wb + bx) * 8:(by * wb + bx) * 8 + 8])
                pal, bits, a3 = _dxt_colors(blk, True)
                for i in range(16):
                    s = (bits >> (2 * i)) & 3
                    put(bx, by, i, pal[s], a3 if s == 3 else 255)
            elif fmt in (19, 20):  # DXT2_3 / DXT4_5
                o = (by * wb + bx) * 16
                ab = _swap16(data[o:o + 8])
                blk = _swap16(data[o + 8:o + 16])
                pal, bits, _ = _dxt_colors(blk, False)
                a0, a1 = ab[0], ab[1]
                abits = int.from_bytes(ab[2:8], 'little')
                for i in range(16):
                    s = (bits >> (2 * i)) & 3
                    ai = (abits >> (3 * i)) & 7
                    if ai == 0:
                        av = a0
                    elif ai == 1:
                        av = a1
                    elif a0 > a1:
                        av = ((8 - ai) * a0 + (ai - 1) * a1) // 7
                    elif ai == 6:
                        av = 0
                    elif ai == 7:
                        av = 255
                    else:
                        av = ((6 - ai) * a0 + (ai - 1) * a1) // 5
                    put(bx, by, i, pal[s], av)
            elif fmt == 49:  # DXN / BC5: two independent alpha blocks (R, G)
                o = (by * wb + bx) * 16
                chans = []
                for half in (0, 8):
                    hb_ = _swap16(data[o + half:o + half + 8])
                    r0, r1 = hb_[0], hb_[1]
                    rbits = int.from_bytes(hb_[2:8], 'little')
                    vals = []
                    for i in range(16):
                        ri = (rbits >> (3 * i)) & 7
                        if ri == 0:
                            v = r0
                        elif ri == 1:
                            v = r1
                        elif r0 > r1:
                            v = ((8 - ri) * r0 + (ri - 1) * r1) // 7
                        elif ri == 6:
                            v = 0
                        elif ri == 7:
                            v = 255
                        else:
                            v = ((6 - ri) * r0 + (ri - 1) * r1) // 5
                        vals.append(v)
                    chans.append(vals)
                for i in range(16):
                    put(bx, by, i, (chans[0][i], chans[1][i], 128), 255)
            else:
                raise ValueError('no decoder for format %d' % fmt)
    return bytes(px), w, h


def decode_linear(fmt, data, wb, hb):
    """Uncompressed formats, already untiled to tight rows."""
    px = bytearray(wb * hb * 4)
    for i in range(wb * hb):
        o = i * 4
        if fmt == 6:      # 8_8_8_8, stored big-endian
            a, r, g, b = data[i * 4:i * 4 + 4]
            px[o:o + 4] = bytes((r, g, b, a))
        elif fmt == 2:    # 8
            v = data[i]
            px[o:o + 4] = bytes((v, v, v, 255))
        elif fmt == 24:   # 16
            v = struct.unpack('>H', data[i * 2:i * 2 + 2])[0] >> 8
            px[o:o + 4] = bytes((v, v, v, 255))
        elif fmt == 29:   # 16_16_16_16_EXPAND -- HALF-FLOAT, despite the name
            # Named EXPAND in the Xenos enum, which suggests fixed point, but the
            # DATA is IEEE fp16 and says so four ways. On FR_DU_ReflectionMap:
            # alpha is exactly 1.0 across all 16384 texels (0x3C00 is fp16 1.0
            # and nothing else); RGB spans 0..1.77, an HDR range; adjacent texels
            # vary smoothly (0.244, 0.245, 0.246, 0.248) as a sky gradient does;
            # and every value is finite. Read as u16 these are 15360 and ~40000,
            # which is meaningless.
            #
            # HDR above 1.0 is CLIPPED here, because the destination is an 8-bit
            # PNG. That loses the top of the range -- fine for looking at, wrong
            # for measuring, and the reason to read the halves directly if a
            # number matters.
            h = struct.unpack('>4e', data[i * 8:i * 8 + 8])
            px[o:o + 4] = bytes(
                max(0, min(255, int(c * 255.0 + 0.5))) for c in h)
        else:
            raise ValueError('no decoder for format %d' % fmt)
    return bytes(px), wb, hb


def _png(path, raw, w, h, depth, colour):
    """Minimal PNG. zlib is stdlib; this avoids a Pillow dependency."""
    def chunk(tag, data):
        return (struct.pack('>I', len(data)) + tag + data +
                struct.pack('>I', binascii.crc32(tag + data) & 0xFFFFFFFF))

    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n')
        f.write(chunk(b'IHDR',
                      struct.pack('>IIBBBBB', w, h, depth, colour, 0, 0, 0)))
        f.write(chunk(b'IDAT', zlib.compress(raw, 6)))
        f.write(chunk(b'IEND', b''))


def write_png(path, px, w, h):
    """8-bit RGBA."""
    stride = w * 4
    _png(path, b''.join(b'\x00' + px[y * stride:(y + 1) * stride]
                        for y in range(h)), w, h, 8, 6)


def write_png16(path, be_samples, w, h):
    """16-bit greyscale, for the 16-bit formats -- WITHOUT discarding 8 bits.

    FR_DU_Disp is what forced this: 6958 distinct heights across the map, which
    an 8-bit PNG collapses to 59. A displacement map quantised to 59 steps is a
    picture of the terrain rather than the terrain, and getting at the numbers
    is the whole reason to read these assets.

    PNG stores 16-bit samples big-endian, which is how the asset already holds
    them, so the payload passes through untouched.
    """
    stride = w * 2
    _png(path, b''.join(b'\x00' + be_samples[y * stride:(y + 1) * stride]
                        for y in range(h)), w, h, 16, 0)


def to_png(t, out_path):
    data, wb, hb = untile_level0(t)
    bw = BLOCKS[t['format']][0]
    if t['format'] == 24:  # 16: keep all sixteen bits, see write_png16
        write_png16(out_path, data, t['width'], t['height'])
        return t['width'], t['height']
    if bw == 4:
        px, w, h = decode_blocks(t['format'], data, wb, hb)
    else:
        px, w, h = decode_linear(t['format'], data, wb, hb)
    # Block formats round up to a multiple of four; crop back to the real size.
    if (w, h) != (t['width'], t['height']):
        cw, ch = min(w, t['width']), min(h, t['height'])
        px = b''.join(px[(y * w) * 4:(y * w + cw) * 4] for y in range(ch))
        w, h = cw, ch
    write_png(out_path, px, w, h)
    return w, h


def describe(t):
    return ('%-38s %5dx%-5d lv%-3d %-22s %s word=%08X data=%d @0x%X'
            % (os.path.basename(t['path'])[:38], t['width'], t['height'],
               t['levels'], t['format_name'],
               'tiled ' if t['tiled'] else 'linear', t['word'],
               t['data_size'], t['data_offset']))


def collect(target):
    if os.path.isdir(target):
        return sorted(glob.glob(os.path.join(target, '**', '*.texture'),
                                recursive=True))
    return [target]


def main():
    ap = argparse.ArgumentParser(
        description='Read .texture assets and decode them to PNG')
    ap.add_argument('target', help='a .texture file, or a directory of them')
    ap.add_argument('--summary', action='store_true', help='counts only')
    ap.add_argument('--grep', default='', help='only names containing this')
    ap.add_argument('--png', default='',
                    help='decode level 0 of each match into this directory')
    ap.add_argument('--words', action='store_true',
                    help='census of the format word, to study its unknown bits')
    ap.add_argument('--verify', action='store_true',
                    help='check each payload against predict_data_size')
    args = ap.parse_args()

    paths = collect(args.target)
    if args.grep:
        paths = [p for p in paths if args.grep.lower() in
                 os.path.basename(p).lower()]
    if args.png:
        os.makedirs(args.png, exist_ok=True)

    words = {}
    verify = collections.Counter()
    verify_bad = set()
    ok = failed = wrote = 0
    for p in paths:
        try:
            t = read_header(p)
        except Exception as exc:  # noqa: BLE001 - reported per file
            print('%-38s FAILED: %s' % (os.path.basename(p)[:38], exc))
            failed += 1
            continue
        ok += 1
        words.setdefault(t['word'], [0, t['format_name'],
                                     os.path.basename(p)])[0] += 1
        if args.verify:
            want = predict_data_size(t)
            got = t['data_size']
            if want is None:
                verify['no geometry'] += 1
            elif got == want:
                verify['exact'] += 1
            elif 0 < got - want < 256:
                verify['trailing extra (<256 B)'] += 1
            else:
                verify['MISMATCH'] += 1
                verify_bad.add((t['width'], t['height'], t['levels'],
                                t['format_name'],
                                'tiled' if t['tiled'] else 'linear',
                                got, want))
        if not (args.summary or args.words):
            print(describe(t))
        if args.png:
            dst = os.path.join(args.png,
                               os.path.basename(p)[:-len('.texture')] + '.png')
            try:
                w, h = to_png(t, dst)
                wrote += 1
                print('  -> %s  %dx%d' % (dst, w, h))
            except Exception as exc:  # noqa: BLE001
                print('  -- no PNG (%s)' % exc)

    if args.verify:
        # Three outcomes, not two. "actual exceeds predicted by under 256 bytes"
        # is trailing data past the last level -- real bytes the model cannot
        # predict and should not pretend to -- and folding it into either
        # `exact` or `MISMATCH` would misreport it.
        print('%-24s %6s' % ('layout prediction', 'count'))
        for k in ('exact', 'trailing extra (<256 B)', 'MISMATCH', 'no geometry'):
            if verify[k]:
                print('  %-22s %6d  (%.2f%%)'
                      % (k, verify[k], 100.0 * verify[k] / max(1, ok)))
        for row in sorted(verify_bad)[:12]:
            print('    %5d x %-5d lv%-3d %-20s %-6s actual %9d predicted %9d'
                  % row)

    if args.words:
        print('%-10s %5s  %-24s %s' % ('word', 'n', 'format (bits 0-5)',
                                       'example'))
        for word, (n, name, ex) in sorted(words.items()):
            print('%08X %5d  %-24s %s %s'
                  % (word, n, name, 'tiled ' if word & 0x100 else 'linear', ex))

    print('')
    print('%d assets read, %d failed%s'
          % (ok, failed, ', %d PNGs written' % wrote if args.png else ''))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
