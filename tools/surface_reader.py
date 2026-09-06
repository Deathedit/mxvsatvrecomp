"""Read `.surface`, the geometry container: parts, declarations, indices, vertices.

`.model` is a skeleton and part-name descriptor; the actual vertices live in
`.surface`. 2510 files, 170.9 MB, and nothing in the tree read them.

The format is solved. `--verify` decodes 2485 of the 2502 parseable files
completely -- for each, the walk lands exactly on EOF and every one of its
11086 parts indexes only its own vertices. The 17 that do not are copies of
one asset, BillboardQuad.surface, whose declaration carries an element at
offset 0x10000 and yields a nonsense stride; it is one degenerate file, not a
second variant.

CONTAINER

    0x00  u32  magic 0x99BBCC00
    0x04  u32  version, 4 everywhere
    0x08  u32  PART COUNT
    0x0C  u32  0
    0x10  u32  0
    0x14  u32  0xFFFFFFFF -- a separator, not a field; see below
    0x18       the part table

A part record is 344 bytes followed by an 8-byte separator, `00000000
FFFFFFFF`, so records repeat every 352 -- except that the LAST record has no
separator after it. That detail is the whole reason the geometry region was
hard to find: it begins at `0x18 + count*352 - 8`, eight bytes before the naive
end of the table. The separator is what makes it checkable, and it holds: the
dword at +0x15C is 0xFFFFFFFF in all 8601 non-final part records across the
corpus and in none of the 2502 final ones, because in the final record those
bytes are already the first index data.

Per part record, relative to its own base:

    +0x00 u32  flags; bit 2 of the low half is set on skinned parts
    +0x04 u32  INDEX COUNT
    +0x08 u32  VERTEX COUNT
    +0x0C u32  INDEX BLOCK BYTES, always 2*indexCount + 4. This is a real
               stored size, not a derived one -- the block is four bytes longer
               than the indices it declares, in every part of every file.
    +0x10      the vertex DECLARATION: 12-byte records of
               (u32 offset, u32 type, u32 usage<<16 | index), terminated by a
               record whose OFFSET field is 0x00FF0000
    +0x100 u32 stream count, 1 in every part seen
    +0x104 u32 VERTEX BLOCK BYTES, exactly vertexCount * stride
    +0x144 u32 BONE PALETTE COUNT, 0 on unskinned parts

The `type` dword is a GUEST D3D9 VERTEX TYPE -- the same encoding
`mx::hle::DecodeVertexType` parses in src/gpu/d3d9_layout.cpp, with the format
in bits 0-5. So the stride is derived with the runtime's own kFormatSizeDwords
table rather than a second one invented here, and a runtime reader can reuse
ReadElement / DecodeVertexType / BuildInputLayout unchanged.

GEOMETRY, from `0x18 + count*352 - 8`, per part, in part order:

    bone palette   bonePaletteCount bytes, one u8 per palette entry
    indices        indexBlockBytes, i.e. 2*indexCount + 4
    vertices       vertexBlockBytes

The bone palette is why the skinned meshes resisted: 583 of the 587 parts that
would not decode had stride 36 and a BLENDINDICES element, and the palette sits
in front of their indices. Its length is not implied by anything in the vertex
data -- it is the +0x144 field, and reading it is what took the corpus from 76%
to 99.3%.

Indices are 16-bit big-endian, PART-LOCAL (0 .. vertexCount-1, never absolute
into a shared buffer), and TRIANGLE STRIPS with degenerate repeats: a typical
block opens 0, 1, 2, 2, 3, 3, 4, 5, 6, 6, 7, 7, 8, 9. Note the four trailing
bytes of the block are inside no part's declared index count and are left in
`index_tail` rather than silently dropped.

Usage:
    py -3 tools/surface_reader.py out/all/ATV/ATV_Bliz.surface
    py -3 tools/surface_reader.py --verify out/all
"""

import argparse
import glob
import os
import struct

MAGIC = 0x99BBCC00
PART_TABLE_OFFSET = 0x18
PART_STRIDE = 352
# The final part record is not followed by the 8-byte separator every other
# record carries, so the geometry begins this far before the naive table end.
REGION_BACKUP = 8
DECL_TERMINATOR = 0x00FF0000

# Lifted from kFormatSizeDwords in src/gpu/d3d9_layout.cpp, which was itself
# lifted from the XEX at 0x8204E188. Values are DWORDS; multiply by 4.
FORMAT_SIZE_DWORDS = [
    0, 0, 0, 0, 0, 0, 1, 1,
    0, 0, 0, 0, 0, 0, 0, 0,
    1, 1, 0, 0, 0, 0, 0, 0,
    0, 1, 2, 0, 0, 0, 0, 1,
    2, 1, 2, 4, 1, 2, 4, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 3, 0, 0, 0, 0, 0, 0,
]

USAGE_NAMES = {
    0: "POSITION", 1: "BLENDWEIGHT", 2: "BLENDINDICES", 3: "NORMAL",
    4: "PSIZE", 5: "TEXCOORD", 6: "TANGENT", 7: "BINORMAL",
    8: "TESSFACTOR", 9: "POSITIONT", 10: "COLOR", 11: "FOG",
    12: "DEPTH", 13: "SAMPLE",
}


def u32(buf, off):
    return struct.unpack_from(">I", buf, off)[0]


def read_parts(buf):
    """Every part's header, declaration and derived stride, or None."""
    if len(buf) < PART_TABLE_OFFSET or u32(buf, 0) != MAGIC:
        return None
    count = u32(buf, 8)
    if count == 0 or PART_TABLE_OFFSET + count * PART_STRIDE > len(buf):
        return None

    parts = []
    for i in range(count):
        base = PART_TABLE_OFFSET + i * PART_STRIDE
        flags, index_count, vertex_count, index_bytes = struct.unpack_from(
            ">4I", buf, base)

        elements = []
        off = base + 0x10
        # Bounded by the record: the terminator is trusted to be present, but
        # not trusted to be found before the next part begins.
        while off + 12 <= base + PART_STRIDE:
            e_off, e_type, e_usage = struct.unpack_from(">3I", buf, off)
            if e_off == DECL_TERMINATOR:
                break
            elements.append({
                "offset": e_off,
                "type": e_type,
                "format": e_type & 0x3F,
                "usage": (e_usage >> 16) & 0xFF,
                "usage_index": (e_usage >> 8) & 0xFF,
            })
            off += 12

        stride = 0
        for e in elements:
            stride = max(stride,
                         e["offset"] + FORMAT_SIZE_DWORDS[e["format"]] * 4)

        parts.append({
            "flags": flags,
            "index_count": index_count,
            "index_bytes": index_bytes,
            "vertex_count": vertex_count,
            "elements": elements,
            "stride": stride,
            "stream_count": u32(buf, base + 0x100),
            "vertex_block_bytes": u32(buf, base + 0x104),
            "bone_palette_count": u32(buf, base + 0x144),
        })
    return parts


def read_geometry(buf, parts):
    """Walk the geometry region and return each part's palette, indices, verts.

    Raises ValueError rather than returning something plausible: a walk that
    does not land exactly on EOF has lost sync, and every part after the break
    would decode as garbage that still looks like numbers.
    """
    off = PART_TABLE_OFFSET + len(parts) * PART_STRIDE - REGION_BACKUP
    out = []
    for p in parts:
        pal_n = p["bone_palette_count"]
        n = p["index_count"]
        need = pal_n + p["index_bytes"] + p["vertex_block_bytes"]
        if off + need > len(buf):
            raise ValueError("part %d runs past EOF" % len(out))
        palette = list(buf[off:off + pal_n])
        off += pal_n
        indices = list(struct.unpack_from(">%dH" % n, buf, off))
        # The block is four bytes longer than the declared indices. Kept rather
        # than dropped, because "we know it is there and it is not an index" is
        # a different claim from "we did not notice it".
        tail = buf[off + n * 2:off + p["index_bytes"]]
        off += p["index_bytes"]
        vertex_at = off
        off += p["vertex_block_bytes"]
        if indices and max(indices) >= p["vertex_count"]:
            raise ValueError("part %d index %d exceeds vertex count %d"
                             % (len(out), max(indices), p["vertex_count"]))
        out.append({"bone_palette": palette, "indices": indices,
                    "index_tail": tail, "vertex_at": vertex_at,
                    "vertex_bytes": p["vertex_block_bytes"]})
    if off != len(buf):
        raise ValueError("walk ended at %d, file is %d" % (off, len(buf)))
    return out


def positions(buf, part, geom):
    """Decode POSITION as float triples, for the fmt-32 (four halves) case."""
    pos = [e for e in part["elements"]
           if e["usage"] == 0 and e["format"] == 32]
    if not pos:
        return None
    base = geom["vertex_at"] + pos[0]["offset"]
    return [struct.unpack_from(">4e", buf, base + k * part["stride"])[:3]
            for k in range(part["vertex_count"])]


def describe(path):
    buf = open(path, "rb").read()
    parts = read_parts(buf)
    if parts is None:
        print("%s: not a .surface, or truncated" % path)
        return
    print("%s  %d bytes, %d parts" % (os.path.basename(path), len(buf),
                                      len(parts)))
    try:
        geom = read_geometry(buf, parts)
    except ValueError as e:
        print("  GEOMETRY DID NOT DECODE: %s" % e)
        geom = None
    for i, p in enumerate(parts):
        print("  part %3d  idx %6d  vtx %6d  stride %3d  bones %2d  "
              "flags 0x%08X"
              % (i, p["index_count"], p["vertex_count"], p["stride"],
                 p["bone_palette_count"], p["flags"]))
        for e in p["elements"]:
            name = USAGE_NAMES.get(e["usage"], "usage%d" % e["usage"])
            print("      +%3d  fmt %2d  %s%s"
                  % (e["offset"], e["format"], name,
                     e["usage_index"] if e["usage_index"] else ""))
        if geom:
            g = geom[i]
            print("      strip  %s ..." % g["indices"][:12])
            pos = positions(buf, p, g)
            if pos:
                print("      vtx 0  (%.3f, %.3f, %.3f)" % pos[0])
            if g["bone_palette"]:
                print("      bones  %s" % g["bone_palette"])


def verify(root):
    """Decode every file, and report what the format does and does not explain."""
    files = sorted(glob.glob(os.path.join(root, "**", "*.surface"),
                             recursive=True))
    parsed = unparsed = decoded = 0
    parts_tot = 0
    failures = []
    for path in files:
        buf = open(path, "rb").read()
        parts = read_parts(buf)
        if parts is None:
            unparsed += 1
            continue
        parsed += 1
        try:
            read_geometry(buf, parts)
        except ValueError as e:
            failures.append((os.path.basename(path), str(e)))
            continue
        decoded += 1
        parts_tot += len(parts)
    print("files              %d" % len(files))
    print("parsed             %d   (unparseable %d)" % (parsed, unparsed))
    print("FULLY DECODED      %d   (%.1f%% -- exact EOF, and every index "
          "inside its own part)" % (decoded, 100.0 * decoded / max(parsed, 1)))
    print("parts decoded      %d" % parts_tot)
    if failures:
        names = sorted({n for n, _ in failures})
        print("\nfailed             %d file(s), %d distinct name(s): %s"
              % (len(failures), len(names), ", ".join(names[:4])))
        print("                   e.g. %s" % failures[0][1])


def main():
    ap = argparse.ArgumentParser(description="Read .surface geometry containers")
    ap.add_argument("path")
    ap.add_argument("--verify", action="store_true",
                    help="treat path as a root and decode the whole corpus")
    args = ap.parse_args()
    if args.verify:
        verify(args.path)
    else:
        describe(args.path)


if __name__ == "__main__":
    main()
