#!/usr/bin/env python3
"""Build contact sheets from logs/texdump so a run's textures can be scanned by
eye instead of opened one at a time.

Pure stdlib: PNG in, PNG out, zlib for both. The dump writes 8-bit non-interlaced
RGB/RGBA, which is the only shape this reads -- it is a companion to
src/hooks/texture_dump.cpp, not a general PNG library, and it says so rather than
silently mis-decoding something else.

    python tools/texdump_sheet.py                     every colour dump
    python tools/texdump_sheet.py --match 1024x1024   only those
    python tools/texdump_sheet.py --alpha             the _alpha companions

Each sheet prints its own tile order, so a tile's position names its file.
"""

import argparse
import os
import struct
import sys
import zlib

SIG = b"\x89PNG\r\n\x1a\n"


def read_png(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != SIG:
        raise ValueError("not a PNG")
    at = 8
    width = height = 0
    channels = 0
    idat = []
    while at + 8 <= len(data):
        (length,) = struct.unpack(">I", data[at:at + 4])
        kind = data[at + 4:at + 8]
        body = data[at + 8:at + 8 + length]
        at += 12 + length
        if kind == b"IHDR":
            width, height, depth, colour, _, _, interlace = struct.unpack(
                ">IIBBBBB", body)
            if depth != 8 or interlace != 0 or colour not in (2, 6):
                raise ValueError(
                    "unsupported: depth %d colour %d interlace %d"
                    % (depth, colour, interlace))
            channels = 3 if colour == 2 else 4
        elif kind == b"IDAT":
            idat.append(body)
        elif kind == b"IEND":
            break
    raw = zlib.decompress(b"".join(idat))
    stride = width * channels
    out = bytearray(height * stride)
    prev = bytearray(stride)
    pos = 0
    for y in range(height):
        filt = raw[pos]
        pos += 1
        line = bytearray(raw[pos:pos + stride])
        pos += stride
        bpp = channels
        if filt == 1:
            for i in range(bpp, stride):
                line[i] = (line[i] + line[i - bpp]) & 0xFF
        elif filt == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif filt == 3:
            for i in range(stride):
                left = line[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + ((left + prev[i]) >> 1)) & 0xFF
        elif filt == 4:
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                b = prev[i]
                c = prev[i - bpp] if i >= bpp else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pred = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pred) & 0xFF
        elif filt != 0:
            raise ValueError("bad filter %d" % filt)
        out[y * stride:(y + 1) * stride] = line
        prev = line
    return width, height, channels, out


def write_png_rgb(path, width, height, rgb):
    raw = bytearray()
    stride = width * 3
    for y in range(height):
        raw.append(0)
        raw += rgb[y * stride:(y + 1) * stride]

    def chunk(kind, body):
        return (struct.pack(">I", len(body)) + kind + body +
                struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF))

    with open(path, "wb") as f:
        f.write(SIG)
        f.write(chunk(b"IHDR",
                      struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(bytes(raw), 6)))
        f.write(chunk(b"IEND", b""))


def thumbnail(width, height, channels, pixels, size):
    """Nearest neighbour into a `size` box, letterboxed on a dark background so
    the aspect ratio survives -- a 4096x512 impostor sheet and a 512x512 atlas
    must not both come out square."""
    scale = min(size / width, size / height)
    tw, th = max(1, int(width * scale)), max(1, int(height * scale))
    ox, oy = (size - tw) // 2, (size - th) // 2
    tile = bytearray(b"\x20" * (size * size * 3))
    for y in range(th):
        sy = min(height - 1, y * height // th)
        row = sy * width * channels
        dst = ((y + oy) * size + ox) * 3
        for x in range(tw):
            sx = min(width - 1, x * width // tw)
            s = row + sx * channels
            d = dst + x * 3
            tile[d] = pixels[s]
            tile[d + 1] = pixels[s + 1]
            tile[d + 2] = pixels[s + 2]
    return tile


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default="logs/texdump")
    ap.add_argument("--out", default="logs/texdump/sheets")
    ap.add_argument("--match", default="", help="substring the filename must contain")
    ap.add_argument("--alpha", action="store_true",
                    help="the _alpha companions instead of the colour images")
    ap.add_argument("--tile", type=int, default=192)
    ap.add_argument("--cols", type=int, default=8)
    ap.add_argument("--rows", type=int, default=8)
    args = ap.parse_args()

    names = sorted(n for n in os.listdir(args.dir) if n.endswith(".png"))
    names = [n for n in names if n.endswith("_alpha.png") == args.alpha]
    if args.match:
        names = [n for n in names if args.match in n]
    if not names:
        print("no matching PNGs in %s" % args.dir)
        return 1
    os.makedirs(args.out, exist_ok=True)

    per = args.cols * args.rows
    sheet_w, sheet_h = args.cols * args.tile, args.rows * args.tile
    for start in range(0, len(names), per):
        batch = names[start:start + per]
        sheet = bytearray(b"\x10" * (sheet_w * sheet_h * 3))
        for i, name in enumerate(batch):
            try:
                w, h, ch, px = read_png(os.path.join(args.dir, name))
            except Exception as exc:  # a bad file must not lose the sheet
                print("  skip %s: %s" % (name, exc), file=sys.stderr)
                continue
            tile = thumbnail(w, h, ch, px, args.tile)
            tx, ty = (i % args.cols) * args.tile, (i // args.cols) * args.tile
            for y in range(args.tile):
                d = ((ty + y) * sheet_w + tx) * 3
                s = y * args.tile * 3
                sheet[d:d + args.tile * 3] = tile[s:s + args.tile * 3]
        index = start // per
        path = os.path.join(args.out, "sheet_%02d.png" % index)
        write_png_rgb(path, sheet_w, sheet_h, sheet)
        print("%s  (%d tiles, row-major %dx%d)" %
              (path, len(batch), args.cols, args.rows))
        for i, name in enumerate(batch):
            print("   r%dc%d  %s" % (i // args.cols, i % args.cols, name))
    return 0


if __name__ == "__main__":
    sys.exit(main())
