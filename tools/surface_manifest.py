"""Name every guest mesh: content -> the .surface part it came from.

At runtime a draw carries bound streams -- a host pointer, a stride, a size --
and nothing that says which mesh it is. `tools/surface_reader.py` decodes the
2510 `.surface` assets into parts with real names, so this joins them by
CONTENT, the same way tools/shader_manifest.py joins microcode and
tools/texture_manifest.py joins pixels.

WHY THE RAW BIG-ENDIAN BYTES. `HleStream::host` is documented as "guest bytes,
still big-endian", and a .surface vertex block is exactly that representation --
the asset stores what the guest uploads. So hashing raw needs no decode on
either side and has nothing to disagree about. Converting first would mean this
tool's half-float reading and the runtime's transcode must agree, and a join
that does not depend on that is strictly better. This is the same reasoning
texture_manifest.py records for hashing tiled level 0 rather than decoded
pixels, and it is there because comparing two DIFFERENT decodes once read as 0%
agreement.

BOTH BUFFERS. A part's indices are a separate guest buffer from its vertices,
so each is hashed and each is an independent join. If only one side matches,
that is worth knowing: it says the runtime is finding the mesh but re-packing
one of the two buffers, which is a different problem from not finding it.

AN INDEX BLOCK IS HASHED AT BOTH LENGTHS, 2n and 2n+4. The guest uploads the
whole stored block including its four trailing bytes -- measured, not assumed:
HR_SWP_BCK_Tire_THQ part 0 binds a 31630-byte index buffer and the asset's
2*indexCount is 31626. Hashing only 2n made every index full-key miss, so every
index match in the first run came from the 4096-byte prefix instead -- 118
census rows where byPrefix equalled namedIndex exactly, with no exceptions,
which is what gave it away. Worse than the misses it caused: an index block
under 4096 bytes gets no prefix key, so those parts were not merely unmatched,
they were UNREACHABLE.

A PREFIX KEY TOO, and computed FIRST. A draw need not cover a whole buffer, and
several parts may be packed into one. The full-block key only matches when the
runtime hashes exactly the same extent; the prefix key survives when it does
not. It is emitted before any size reasoning, because the last time a fallback
key was computed inside the size check it was meant to survive, it never saw
the population that needed it and measured as dead when it was unreachable.

Names are `<asset>::p<N>` for a vertex block and `<asset>::p<N>:idx` for its
indices. A key claimed by several parts OF THE SAME ASSET keeps the asset name
and loses the part number, because that is a repeated mesh -- ATV_FRSand has
one block shared by parts 0, 20 and 40 -- and "ATV_FRSand" is a true and useful
answer where dropping it is not. Only a key claimed by DIFFERENT assets is real
ambiguity, and those are dropped and reported. Resolving asset-level collisions
by dropping them cost 1035 keys on the first run, which is the same mistake
tools/material_table.py made when it keyed by basename and silently lost half
the table.

Usage:
    py -3 tools/surface_manifest.py --assets out/all
"""

import argparse
import glob
import importlib.util
import os
import sys

FNV64_OFFSET = 1469598103934665603
FNV64_PRIME = 1099511628211
MASK64 = (1 << 64) - 1

# Long enough to discriminate, short enough that neither side has to agree
# about where a block ends. Same size the texture manifest uses.
PREFIX_BYTES = 4096


def load_surface_reader():
    here = os.path.dirname(os.path.abspath(__file__))
    spec = importlib.util.spec_from_file_location(
        "surface_reader", os.path.join(here, "surface_reader.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def fnv64(buf):
    """Byte-wise FNV-1a 64. The runtime hashes guest bytes the same way."""
    h = FNV64_OFFSET
    for b in buf:
        h = ((h ^ b) * FNV64_PRIME) & MASK64
    return h


def add(claims, key, label):
    """Record that `label` claims `key`. Ambiguity is resolved at emit time."""
    claims.setdefault(key, set()).add(label)


def resolve(claims):
    """Turn claims into names, collapsing same-asset collisions to the asset."""
    names, collisions, collapsed = {}, {}, 0
    for key, labels in claims.items():
        if len(labels) == 1:
            names[key] = next(iter(labels))
            continue
        # `<asset>::p<N>` and `<asset>::p<N>:idx` -- the asset is everything
        # before the first "::", and the tag is whether ":idx" is on the end.
        assets = {l.split("::", 1)[0] for l in labels}
        tags = {l.endswith(":idx") for l in labels}
        if len(assets) == 1:
            asset = next(iter(assets))
            names[key] = asset + (":idx" if tags == {True} else "")
            collapsed += 1
        else:
            collisions[key] = labels
    return names, collisions, collapsed


def main():
    ap = argparse.ArgumentParser(
        description="Join guest meshes to the .surface assets by content")
    ap.add_argument("--assets", default="out/all")
    ap.add_argument("--out", default="userdata/surface_names.txt")
    args = ap.parse_args()

    sr = load_surface_reader()
    paths = sorted(glob.glob(os.path.join(args.assets, "**", "*.surface"),
                             recursive=True))
    if not paths:
        sys.exit("no .surface assets under %s" % args.assets)

    claims = {}
    files = parts_n = undecoded = degenerate = prefix_keys = 0
    for path in paths:
        blob = open(path, "rb").read()
        parts = sr.read_parts(blob)
        if parts is None:
            undecoded += 1
            continue
        try:
            geom = sr.read_geometry(blob, parts)
        except ValueError:
            # read_geometry refuses a walk that lost sync rather than handing
            # back plausible garbage, and a mesh named from garbage would be
            # worse than an unnamed one.
            undecoded += 1
            continue
        files += 1
        asset = os.path.splitext(os.path.basename(path))[0]
        for i, (p, g) in enumerate(zip(parts, geom)):
            parts_n += 1
            vtx = blob[g["vertex_at"]:g["vertex_at"] + g["vertex_bytes"]]
            idx_at = g["vertex_at"] - p["index_bytes"]
            idx = blob[idx_at:idx_at + p["index_count"] * 2]
            idx_full = blob[idx_at:idx_at + p["index_bytes"]]
            for tag, data in (("", vtx), (":idx", idx), (":idx", idx_full)):
                if not data:
                    continue
                # A block of one repeated byte identifies nothing; it would
                # collect every zero-filled buffer in the game under one name.
                if len(set(data)) <= 1:
                    degenerate += 1
                    continue
                label = "%s::p%d%s" % (asset, i, tag)
                add(claims, fnv64(data), label)
                if len(data) > PREFIX_BYTES:
                    add(claims, fnv64(data[:PREFIX_BYTES]), label)
                    prefix_keys += 1

    names, collisions, collapsed = resolve(claims)

    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as f:
        for k in sorted(names):
            f.write("%016X\t%s\n" % (k, names[k]))

    print("assets        %d" % len(paths))
    print("decoded       %d files, %d parts   (undecoded %d)"
          % (files, parts_n, undecoded))
    print("unique keys   %d   (%d of them a 4KB prefix key)"
          % (len(names), prefix_keys))
    print("collapsed     %d key(s) claimed by several parts of ONE asset, "
          "named by the asset" % collapsed)
    print("ambiguous     %d key(s) claimed by DIFFERENT assets, dropped"
          % len(collisions))
    print("degenerate    %d block(s) of a single repeated byte, skipped"
          % degenerate)
    if collisions:
        for k, v in list(collisions.items())[:3]:
            print("   e.g. %016X -> %s" % (k, sorted(v)[:3]))
    print("\nwrote %s   (%.0f KB)"
          % (args.out, os.path.getsize(args.out) / 1e3))


if __name__ == "__main__":
    main()
