"""Name every guest texture: content -> the .texture asset it came from.

At runtime a texture is a fetch constant. `HleTextureKey` hashes those six
dwords, so the cache key encodes an ADDRESS, a format and an extent -- never the
picture. Two textures that look nothing alike share a key space, and the same
picture at a different address is a different key.

The assets have names, and 6896 `.texture` files carry the pixels. This joins
them by CONTENT, the same way tools/shader_manifest.py joins microcode.

WHY LEVEL 0, RAW, AND UNTOUCHED. The asset stores the guest's own bytes --
tiled, big-endian, exactly the representation the fetch constant points at -- so
hashing them raw needs no decode on either side and has nothing to disagree
about. Untiling first would mean the tool's `tiled_offset_2d` and the runtime's
`XeniaTiledOffset2D` must agree; they are transcriptions of each other, but a
join that does not depend on that is strictly better. And the runtime's decode
also applies SwapBlock and ConvertBlockChannels, which the tool does not, so
comparing DECODED bytes would compare two different things and read as 0%
agreement.

Level 0 alone, because the runtime's mip chain is normalised (a fetch with
mip_address 0 collapses to one level, kBaseMap suppresses the tail) while the
asset always carries the full authored chain. Level 0 is the part both sides
always hold.

Formats: all 6896 assets are one of six formats and the reader covers every one
(DXT1 4779, DXT4_5 1236, DXN 623, 16_16_16_16_EXPAND 110, 8_8_8_8 93, 16 55).

Usage:
    py -3 tools/texture_manifest.py --assets out/all
"""

import argparse
import glob
import importlib.util
import os
import sys

FNV64_OFFSET = 1469598103934665603
FNV64_PRIME = 1099511628211
MASK64 = (1 << 64) - 1

# The fallback key's length. Long enough to discriminate (2985 distinct
# keys over the corpus), short enough that neither side has to agree
# about where level 0 ends.
PREFIX_BYTES = 4096


def load_texture_reader():
    here = os.path.dirname(os.path.abspath(__file__))
    spec = importlib.util.spec_from_file_location(
        "texture_reader", os.path.join(here, "texture_reader.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def fnv64(buf):
    """Byte-wise FNV-1a 64. The runtime hashes guest bytes the same way."""
    h = FNV64_OFFSET
    for b in buf:
        h = ((h ^ b) * FNV64_PRIME) & MASK64
    return h


def level0_bytes(tr, t, blob):
    """The raw, tiled level-0 payload as the asset stores it.

    The level table's PITCH is exact; its SIZE field is not -- it is computed as
    pitch * height in TEXELS and overstates a block format fourfold. So the
    height is taken in blocks and the size derived, never read.
    """
    fmt = t["format"]
    if fmt not in tr.BLOCKS:
        return None
    bw, bh, bpb = tr.BLOCKS[fmt]
    if not t["table"]:
        return None
    pitch = t["table"][0][1]
    rows = (t["height"] + bh - 1) // bh
    size = pitch * rows
    start = t["data_offset"]
    if size <= 0 or start + size > len(blob):
        return None
    return blob[start:start + size]


def main():
    ap = argparse.ArgumentParser(
        description="Join guest textures to the .texture assets by content")
    ap.add_argument("--assets", default="out/all")
    ap.add_argument("--out", default="userdata/texture_names.txt")
    args = ap.parse_args()

    tr = load_texture_reader()
    paths = sorted(glob.glob(os.path.join(args.assets, "**", "*.texture"),
                             recursive=True))
    if not paths:
        sys.exit(f"no .texture assets under {args.assets}")

    names = {}          # hash -> set of labels, resolved below
    skipped = degenerate = 0
    for p in paths:
        blob = open(p, "rb").read()
        try:
            t = tr.read_header(p)
        except Exception:
            skipped += 1
            continue
        payload = level0_bytes(tr, t, blob)
        if payload is None:
            skipped += 1
            continue
        # An all-one-byte level 0 is a flat colour. Many assets share one, and a
        # key that maps to forty names is not an identification -- counted and
        # dropped rather than shipped as a lie.
        if len(set(payload)) <= 1:
            degenerate += 1
            continue
        label = os.path.splitext(
            os.path.relpath(p, args.assets).replace(os.sep, "/"))[0]
        # TWO KEYS, because the two sides may not agree on where level 0 ENDS.
        # The asset's stored pitch and the fetch constant's pitch need not be
        # the same number -- Xenos aligns a tiled pitch -- and a size-derived
        # range is only as good as that assumption.
        #
        # The full hash discriminates better (3364 keys, 94 ambiguous) than a
        # 4096-byte prefix (2985, 215), so it is preferred; the prefix cannot
        # disagree about the end of the level and is the fallback. Both map to
        # the same name, and the runtime counts which one hit, so a single run
        # says which assumption holds instead of another round of guessing.
        names.setdefault(fnv64(payload), set()).add(label)
        if len(payload) > PREFIX_BYTES:
            names.setdefault(fnv64(payload[:PREFIX_BYTES]), set()).add(label)

    # SEVERAL ASSETS SHARING ONE CONTENT IS USUALLY NOT AMBIGUITY. Measured over
    # 3458 distinct level-0 contents: 2626 belong to one asset, 738 are ONE
    # texture shipped in several packages (`ATV/Mud_Noise2`,
    # `ATVFR/Mud_Noise2`, `ATVFR_aarm/Mud_Noise2`), and only 94 are genuinely
    # two different assets that happen to share a level 0 -- an `HR_` variant,
    # or `ATV_Aarms_Stock_1` beside `_2`.
    #
    # So a group whose basenames agree is named by that basename; only the 94
    # are dropped. Treating every group as a collision threw away 28% of the
    # usable map, and the tell was that the "colliding" names were identical.
    collisions = {}
    resolved = {}
    duplicated = 0
    for key, labels in names.items():
        bases = {os.path.basename(x) for x in labels}
        if len(bases) == 1:
            if len(labels) > 1:
                duplicated += 1
            resolved[key] = next(iter(bases))
        else:
            collisions[key] = labels
    names = resolved

    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as f:
        for k in sorted(names):
            f.write("%016X\t%s\n" % (k, names[k]))

    print(f"assets        {len(paths)}")
    print(f"unique keys   {len(names)}   "
          f"({duplicated} of them one texture shipped in several packages)")
    print(f"ambiguous     {len(collisions)} key(s) whose assets have DIFFERENT "
          f"names, dropped")
    print(f"flat level 0  {degenerate} (single byte value, cannot identify)")
    print(f"skipped       {skipped} (no block info or truncated)")
    if collisions:
        for k, v in list(collisions.items())[:3]:
            print(f"   e.g. {k:016X} -> {sorted(v)[:3]}")
    print(f"\nwrote {args.out}   ({os.path.getsize(args.out) / 1e3:.0f} KB)")


if __name__ == "__main__":
    main()
