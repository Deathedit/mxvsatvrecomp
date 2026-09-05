"""Name every guest shader: microcode content -> asset, entry point, constants.

At runtime a shader is a bare handle, and handles are addresses that vary per
run. The names exist, but only in the assets: 1157 `.shader` files, each a
container of NAMED entry points, each carrying a D3DX constant table that names
every constant register and sampler slot.

This joins the two by CONTENT. A shader's microcode appears byte-for-byte inside
its `.shader` asset, big-endian there against the little-endian dwords the
runtime sees, so the join is a dword swap and a substring search. Measured on the
301-blob corpus: 245 located, 0 of them landing in a region with no entry name
before it.

The key is `code_key` -- the FNV-1a 64 over microcode DWORDS that
ReportHlslCoverage already computes once per shader at translation time. Reusing
it rather than inventing a second hash makes the runtime side a map lookup on a
value it is already holding, never a hash on the draw path (an FNV per draw was
measured at 4.6 ms a frame elsewhere in this tree).

Two files are written: a JSON manifest for reading, carrying the named constants
and samplers that later work needs, and a flat `.names` map the runtime loads --
`<code_key>\t<asset>::<entry>` per line, because there is no JSON library in the
tree and this needs none.

WHAT IT DOES NOT COVER, and why the census matters more than the number: 47 of
292 blobs are in the assets but MODIFIED -- the guest patches shaders at load
(see tools/ida_dump_patch_vertex_shader.py; bloom.shader differs in 4 dwords of
21), so an exact key cannot reach them. They are not missing assets. A shader the
manifest cannot name must be COUNTED, not defaulted: otherwise the coverage
number only measures the shaders it already knew.

Usage:
    py -3 tools/shader_manifest.py --assets out/all --ucode <dir>
"""

import argparse
import glob
import importlib.util
import json
import os
import struct
import sys

FNV64_OFFSET = 1469598103934665603
FNV64_PRIME = 1099511628211
MASK64 = (1 << 64) - 1


def load_shader_reader():
    """tools/shader_reader.py already decodes entry names and constant tables."""
    here = os.path.dirname(os.path.abspath(__file__))
    spec = importlib.util.spec_from_file_location(
        "shader_reader", os.path.join(here, "shader_reader.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def swap32(buf):
    """Little-endian dwords to big-endian, the form the assets store."""
    n = len(buf) - len(buf) % 4
    return b"".join(buf[i:i + 4][::-1] for i in range(0, n, 4))


def code_key(le_bytes):
    """FNV-1a 64 over microcode DWORDS, identical to ReportHlslCoverage.

    It folds whole dwords, not bytes -- `code_key ^= code[i]` where code[i] is a
    uint32. A per-byte FNV over the same buffer gives a different value and would
    silently name nothing.
    """
    n = len(le_bytes) // 4
    h = FNV64_OFFSET
    for dword in struct.unpack("<%dI" % n, le_bytes[:n * 4]):
        h = ((h ^ dword) * FNV64_PRIME) & MASK64
    return "%016X" % h


def main():
    ap = argparse.ArgumentParser(
        description="Join guest microcode to the names in the .shader assets")
    ap.add_argument("--assets", default="out/all",
                    help="extracted game data (contains **/*.shader)")
    ap.add_argument("--ucode", required=True,
                    help="directory of raw microcode blobs, little-endian dwords")
    ap.add_argument("--out", default="out/shader_manifest.json")
    ap.add_argument("--names", default="userdata/shader_names.txt",
                    help="the flat map the runtime loads")
    ap.add_argument("--glob", default="*.ucode.bin.*",
                    help="pattern for the microcode blobs")
    args = ap.parse_args()

    sr = load_shader_reader()

    asset_paths = sorted(glob.glob(os.path.join(args.assets, "**", "*.shader"),
                                   recursive=True))
    if not asset_paths:
        sys.exit(f"no .shader assets under {args.assets}")
    assets = {p: open(p, "rb").read() for p in asset_paths}
    print(f"assets    {len(assets)} .shader files, "
          f"{sum(len(b) for b in assets.values()) / 1e6:.1f} MB")

    # Entry names and constant tables, decoded once per asset rather than once
    # per blob -- the search below is already the expensive half.
    entries = {}   # path -> [(offset, name)]
    consts = {}    # (path, entry) -> table
    for p, buf in assets.items():
        entries[p] = sr.find_entry_names(buf)
        try:
            _, tables = sr.read_shader(p)
        except Exception:
            tables = []
        for t in tables:
            consts.setdefault((p, t.get("entry")), t)

    blobs = sorted(glob.glob(os.path.join(args.ucode, args.glob)))
    print(f"microcode {len(blobs)} blobs")

    manifest = {}
    collisions = []
    named = short = unnamed = unmatched = 0
    for bp in blobs:
        le = open(bp, "rb").read()
        if len(le) < 64:
            short += 1
            continue
        be = swap32(le)
        # Anchor on the MIDDLE. Shaders share a header, so anchoring at the
        # front matches the wrong entry point and then reports two thirds of the
        # dwords "different" -- that is a false anchor, not a patched shader.
        mid = (len(be) // 2) & ~3
        needle = be[mid:mid + 64] or be[:64]

        hit = None
        for p, buf in assets.items():
            off = buf.find(needle)
            if off >= 0:
                hit = (p, off)
                break
        if hit is None:
            unmatched += 1
            continue

        path, off = hit
        before = [n for o, n in entries[path] if o < off]
        if not before:
            unnamed += 1
            continue
        entry = before[-1]
        table = consts.get((path, entry), {})

        cmap, smap = {}, {}
        for c in table.get("constants", []):
            if c.get("set") == "sampler" or str(c.get("type", "")).startswith("sampler"):
                smap[str(c["reg"])] = c["name"]
            else:
                cmap[str(c["reg"])] = c["name"]

        key = code_key(le)
        if key in manifest and manifest[key]["entry"] != entry:
            collisions.append((key, manifest[key]["entry"], entry))
        manifest[key] = {
            "asset": os.path.relpath(path, args.assets).replace("\\", "/"),
            "entry": entry,
            "profile": table.get("profile", ""),
            "constants": cmap,
            "samplers": smap,
            "dwords": len(le) // 4,
        }
        named += 1

    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=1, sort_keys=True)

    os.makedirs(os.path.dirname(args.names) or ".", exist_ok=True)
    with open(args.names, "w", encoding="utf-8") as f:
        for k in sorted(manifest):
            v = manifest[k]
            f.write("%s\t%s::%s\n" % (k, v["asset"], v["entry"]))

    total = named + unnamed + unmatched
    print(f"\nnamed     {named}/{total}")
    print(f"unnamed   {unnamed}   (found in an asset, no entry name before it)")
    print(f"unmatched {unmatched}   (patched at load, or asset not extracted)")
    print(f"skipped   {short}   (under 64 bytes)")
    if collisions:
        print(f"\nCOLLISIONS {len(collisions)} -- two entry points share a code_key:")
        for k, a, b in collisions[:5]:
            print(f"  {k}  {a} vs {b}")
    print(f"\nwrote {args.out}   ({os.path.getsize(args.out) / 1e3:.0f} KB)")
    print(f"wrote {args.names}   ({os.path.getsize(args.names) / 1e3:.0f} KB)")


if __name__ == "__main__":
    main()
