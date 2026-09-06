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

TWO PREFIX KEYS TOO, 4096 and 512 bytes, computed FIRST. A draw need not cover
a whole buffer, and several parts may be packed into one. The full-block key
only matches when the runtime hashes exactly the same extent; a prefix key
survives when it does not. The short one exists because 4096 cannot reach a
part smaller than 4096, and most parts in the corpus are -- so the long prefix
alone covered only the parts least likely to need it. It is emitted before any size reasoning, because the last time a fallback
key was computed inside the size check it was meant to survive, it never saw
the population that needed it and measured as dead when it was unreachable.

Names are `<asset>::p<N>` for a vertex block and `<asset>::p<N>:idx` for its
indices. A key claimed by several parts OF THE SAME ASSET keeps the asset name
and loses the part number, because that is a repeated mesh -- ATV_FRSand has
one block shared by parts 0, 20 and 40 -- and "ATV_FRSand" is a true and useful
answer where dropping it is not.

A key claimed by DIFFERENT assets is JOINED, not dropped. Dropping is what
texture_manifest.py does, and for pixels it is right: two textures sharing a
key means one of them would get the wrong name. Geometry is not like that.
Identical bytes ARE the same mesh, shipped in two packages -- two ATV models
sharing a frame -- so "ATV_FRSand|ATV_Gliz" is true, and dropping it is the
only answer that is false.

That distinction is not academic. Dropping cost 1440 keys and took real vehicle
geometry with it: 6 of ATV_FRSand's 60 parts had no key at all, 5 of
HR_MX_EarthF's 19 and 7 of HR_MX_NeptuneF's 19. The runtime missed them exactly
as often as that predicts -- thousands of replayed draws under
FR_SandATV_Frame, Template_MX_ATV_DynPlastic_FNL and Template_MX_FrameRefl_FNL
matching nothing -- and it read as a join failure when it was this policy.

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
# A second, shorter prefix. 4096 cannot reach a part smaller than 4096, and a
# part packed inside a larger guest buffer -- which is what the long prefix
# exists for -- is usually one of those. Most parts in the corpus are under 4KB,
# so without this the fallback covers only the parts least likely to need it.
SHORT_PREFIX = 512


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


# A joined name lists at most this many assets before it is truncated. The name
# is for a human reading a log line, and eleven ATV variants sharing one bolt
# would otherwise produce a name longer than the line it sits on.
MAX_JOINED = 3


def resolve(claims):
    """Turn claims into names. Nothing is dropped; ambiguity is spelled out."""
    names, collapsed, joined = {}, 0, 0
    for key, labels in claims.items():
        if len(labels) == 1:
            names[key] = next(iter(labels))
            continue
        # `<asset>::p<N>` and `<asset>::p<N>:idx` -- the asset is everything
        # before the first "::", and the tag is whether ":idx" is on the end.
        assets = sorted({l.split("::", 1)[0] for l in labels})
        tag = ":idx" if {l.endswith(":idx") for l in labels} == {True} else ""
        if len(assets) == 1:
            names[key] = assets[0] + tag
            collapsed += 1
        else:
            shown = "|".join(assets[:MAX_JOINED])
            if len(assets) > MAX_JOINED:
                shown += "|+%d" % (len(assets) - MAX_JOINED)
            names[key] = shown + tag
            joined += 1
    return names, joined, collapsed


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
                for n in (PREFIX_BYTES, SHORT_PREFIX):
                    if len(data) > n:
                        add(claims, fnv64(data[:n]), label)
                        prefix_keys += 1

    names, joined, collapsed = resolve(claims)

    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as f:
        for k in sorted(names):
            f.write("%016X\t%s\n" % (k, names[k]))

    print("assets        %d" % len(paths))
    print("decoded       %d files, %d parts   (undecoded %d)"
          % (files, parts_n, undecoded))
    # `prefix_keys` counts ADDITIONS, not survivors, and both prefix sizes --
    # so it legitimately exceeds the number of unique keys. Said plainly here
    # because the earlier wording claimed 27829 of 15480 keys were prefixes,
    # which is not a thing that can be true.
    print("unique keys   %d   (%d prefix additions, 4KB and 512B, before "
          "dedup)" % (len(names), prefix_keys))
    print("collapsed     %d key(s) claimed by several parts of ONE asset, "
          "named by the asset" % collapsed)
    print("joined        %d key(s) claimed by DIFFERENT assets, named by all "
          "of them" % joined)
    print("degenerate    %d block(s) of a single repeated byte, skipped"
          % degenerate)
    print("\nwrote %s   (%.0f KB)"
          % (args.out, os.path.getsize(args.out) / 1e3))


if __name__ == "__main__":
    main()
