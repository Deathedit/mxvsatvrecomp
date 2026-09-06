"""Read `.surface`, the geometry container -- part table, declarations, layout.

`.model` is a skeleton and part-name descriptor; the actual vertices live in
`.surface`. 2510 files, 170.9 MB, and nothing in the tree reads them.

Everything below was measured over the whole corpus rather than inferred from
one file, and where a rule does not hold everywhere the fraction is given.

CONTAINER

    0x00  u32  magic 0x99BBCC00
    0x04  u32  version, 4 everywhere
    0x08  u32  PART COUNT
    0x18       the part table: `count` records of 352 bytes each

Per part record, relative to its own base:

    +0x00 u32  flags: 0, or 0x00030004 on parts whose stride is 36
    +0x04 u32  INDEX COUNT
    +0x08 u32  VERTEX COUNT
    +0x0C u32  2*indexCount + 4, in every part of every file -- derived, and
               useful only as a consistency check
    +0x10      the vertex DECLARATION: 12-byte records of
               (u32 offset, u32 type, u32 usage<<16 | index), terminated by a
               record whose OFFSET field is 0x00FF0000
    +0x100 u32 stream count, 1 in every part seen
    +0x104 u32 VERTEX BLOCK BYTES, exactly vertexCount * stride

The `type` dword is a GUEST D3D9 VERTEX TYPE -- the same encoding
`mx::hle::DecodeVertexType` parses in src/gpu/d3d9_layout.cpp, with the format
in bits 0-5. So the stride is derived with the runtime's own kFormatSizeDwords
table rather than a second one invented here, and a runtime reader can reuse
ReadElement / DecodeVertexType / BuildInputLayout unchanged.

GEOMETRY, and this part is NOT solved. The reader stops at the part table on
purpose; read this before assuming otherwise.

What the bytes after the table are made of is established. Index data is
16-bit, PART-LOCAL (0 .. vertexCount-1, never absolute into a shared buffer),
and triangle strips with degenerate repeats. In ATV_Bliz.surface the region
opens with part 0's strip -- 3,3,4,4,4,5,6,7,6,8,9,8,... using all 40 of its
vertices -- and 67 entries in, that stops and a 14-u16 period begins, matching
that part's 28-byte stride. Read as the declaration says (POSITION is fmt 32,
four halves) the period puts 0x3C00, half 1.0, in .w on every vertex, at
exactly one of the two phases periodicity allows. So for that file, part 0's
vertices directly follow part 0's indices, and the region is interleaved per
part rather than all-indices-then-all-vertices.

The SIZES are established in aggregate. Summing vertexCount*stride and
2*indexCount over all parts accounts for the region with a slack of
4*(parts-2) on 1898 of 2502 files (76%). Single-part files come out at -4,
which agrees with part 0 of ATV_Bliz measuring 2n-4 while its part 1 measures
2n+4.

What is NOT established is where any individual block begins, and the failure
is not a few bytes -- it is the model. A search over per-part index block sizes
in {-8,-4,0,+4,+8,+12,+16}, requiring every part's indices to fall in range and
the walk to land on EOF, solves ATV_Bliz and three files from a directory-wide
sample, and rejects everything else. The block starts it does find are not even
4-byte aligned, which no real vertex buffer would tolerate. Applying the
aggregate rule (2n-4 for part 0, 2n+4 after) lands on EOF for those same 1898
files but leaves only 61.4% of their parts decoding in range -- the total is
right and the distribution is wrong.

The likeliest resolutions, untested and in the order worth trying: a per-part
block offset or size lives in the unexplored bytes of the 352-byte part record
(+0x18..0x100 and +0x108..0x160, so far only sampled as non-zero), which would
end the question outright; or the region is not one packed run and carries its
own directory; or some parts share a vertex block, which the stream count of 1
everywhere would be consistent with.

So `blocks()` reports the region split under the interleaved model and marks
every entry after part 0 as approximate. It deliberately does NOT hand back
index or vertex arrays: on this evidence they would be silently wrong for most
files, and silently-wrong geometry is worse than none.

Usage:
    py -3 tools/surface_reader.py out/all/ATV/ATV_Bliz.surface
    py -3 tools/surface_reader.py --verify out/all
"""

import argparse
import glob
import os
import struct

MAGIC = 0x99BBCC00
PART_STRIDE = 352
DECL_TERMINATOR = 0x00FF0000
PART_TABLE_OFFSET = 0x18

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
        flags, index_count, vertex_count, index_bytes_plus4 = struct.unpack_from(
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
            "vertex_count": vertex_count,
            "index_bytes_plus4": index_bytes_plus4,
            "elements": elements,
            "stride": stride,
            "vertex_block_bytes": u32(buf, base + 0x104),
            "stream_count": u32(buf, base + 0x100),
        })
    return parts


def blocks(parts):
    """Where each part's data begins, to the accuracy currently established.

    `index_at` is exact for part 0 and approximate for the rest, because the
    index block size carries a per-part slack of a few bytes (see the module
    docstring). `vertex_bytes` is exact, and the walk is known to end on EOF.
    Returned so a caller can see the shape without being handed indices that
    would be silently off by an entry or two.
    """
    out = []
    off = PART_TABLE_OFFSET + len(parts) * PART_STRIDE
    for p in parts:
        index_at = off
        off += p["index_count"] * 2
        out.append({"index_at": index_at, "index_exact": not out,
                    "vertex_at": off, "vertex_bytes": p["vertex_block_bytes"]})
        off += p["vertex_block_bytes"]
    return out


def describe(path):
    buf = open(path, "rb").read()
    parts = read_parts(buf)
    if parts is None:
        print("%s: not a .surface, or truncated" % path)
        return
    print("%s  %d bytes, %d parts"
          % (os.path.basename(path), len(buf), len(parts)))
    for i, (p, b) in enumerate(zip(parts, blocks(parts))):
        print("  part %3d  idx %6d  vtx %6d  stride %3d  vtxBytes %8d"
              "  flags 0x%08X"
              % (i, p["index_count"], p["vertex_count"], p["stride"],
                 p["vertex_block_bytes"], p["flags"]))
        print("      idx @ %d%s,  vtx @ ~%d"
              % (b["index_at"], "" if b["index_exact"] else " (+/- a few)",
                 b["vertex_at"]))
        for e in p["elements"]:
            name = USAGE_NAMES.get(e["usage"], "usage%d" % e["usage"])
            print("      +%3d  fmt %2d  %s%s"
                  % (e["offset"], e["format"], name,
                     e["usage_index"] if e["usage_index"] else ""))


def verify(root):
    """Report what the format model does and does not explain, over the corpus."""
    files = sorted(glob.glob(os.path.join(root, "**", "*.surface"),
                             recursive=True))
    parsed = unparsed = 0
    stride_ok = derived_ok = 0
    accounted = slack_is_4n = 0
    for path in files:
        buf = open(path, "rb").read()
        parts = read_parts(buf)
        if parts is None:
            unparsed += 1
            continue
        parsed += 1
        if all(p["vertex_block_bytes"] == p["vertex_count"] * p["stride"]
               for p in parts if p["stride"]):
            stride_ok += 1
        if all(p["index_bytes_plus4"] == p["index_count"] * 2 + 4
               for p in parts):
            derived_ok += 1
        table_end = PART_TABLE_OFFSET + len(parts) * PART_STRIDE
        vertex_bytes = sum(p["vertex_block_bytes"] for p in parts)
        index_bytes = sum(p["index_count"] * 2 for p in parts)
        # The vertex blocks are exact, so whatever is left over is the index
        # data plus its slack. A slack inside a few bytes per part means the
        # model accounts for the file; anything larger means it does not.
        #
        # The bound is symmetric because the slack really can be NEGATIVE: every
        # single-part file comes out at -4. An earlier version bounded it below
        # by zero and so reported fewer files accounted for than files matching
        # the slack rule, which is impossible and was the tell.
        slack = len(buf) - table_end - vertex_bytes - index_bytes
        if abs(slack) <= 8 * len(parts) + 8:
            accounted += 1
        if slack == 4 * (len(parts) - 2):
            slack_is_4n += 1
    print("files             %d" % len(files))
    print("parsed            %d   (unparseable %d)" % (parsed, unparsed))
    print("stride agrees     %d   (+0x104 == vertexCount * derived stride, in "
          "every part)" % stride_ok)
    print("index bytes       %d   (+0x0C == 2*indexCount + 4)" % derived_ok)
    print("region accounted  %d   (vertex blocks + index counts fill the file, "
          "to a few bytes per part)" % accounted)
    print("slack == 4(n-2)   %d   (the one slack rule fitting a majority)"
          % slack_is_4n)
    print("\nIndex BLOCK SIZE is the open question; see the module docstring.")


def main():
    ap = argparse.ArgumentParser(description="Read .surface geometry containers")
    ap.add_argument("path")
    ap.add_argument("--verify", action="store_true",
                    help="treat path as a root and check the format model")
    args = ap.parse_args()
    if args.verify:
        verify(args.path)
    else:
        describe(args.path)


if __name__ == "__main__":
    main()
