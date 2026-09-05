"""Name every guest shader: microcode content -> asset, entry point, constants.

At runtime a shader is a bare handle, and handles are addresses that vary per
run. The names exist, but only in the assets: 1157 `.shader` files, each a
container of NAMED entry points, each carrying a D3DX constant table that names
every constant register and sampler slot.

This walks the assets and emits a key for every entry point in them, so coverage
does not depend on what some capture happened to contain. An earlier version
searched the assets FOR blobs taken from a dump; that named 245 shaders, which is
4.8% of the 5144 entry points sitting in the files.

THE CONTAINER, as far as it needs to be understood. A `.shader` is `JHM\0`
followed by BXML blocks -- one `ShaderHeader`, then a `ShaderMetaData` per
shader, each immediately followed by that shader's payload: the entry name, a
D3DX constant table, and the microcode. Measured against 161 blobs located by
content, THE MICROCODE IS EXACTLY THE SUFFIX OF ITS BLOCK -- 161 of 161 with
zero trailing bytes. (An earlier test said 45 of 292 and was simply wrong.)

What the container does not say is where the blob STARTS: the length lives in a
guest runtime struct the loader fills, not in the file, and it is not encoded
before the blob either -- checked, 7 coincidences in 161. So every dword-aligned
suffix of a block is a candidate, filtered by two conditions on the first two
dwords that were tuned to keep ALL 161 ground truths:

    (dword0 >> 24) <= 0x0F
    (dword1 >> 24) has a zero low nibble and is <= 0x70

That takes 1,169,982 candidate suffixes down to 30,072, and a candidate at the
wrong offset simply hashes to a key no shader ever presents. A first cut of the
filter kept only 151 of 161 and would have silently dropped ten real shaders,
which is why `--ucode` exists: pass a directory of microcode blobs and this
re-derives them from the assets and FAILS if any is missing or misnamed.

The key is `code_key` -- the FNV-1a 64 over microcode DWORDS that
ReportHlslCoverage already computes once per shader at translation time. Reusing
it makes the runtime side a lookup on a value it is already holding, never a hash
on the draw path (a per-draw FNV measured 4.6 ms a frame elsewhere in this tree).
It folds whole dwords, not bytes; a per-byte FNV over the same buffer names
nothing at all.

Two files are written:
  * `--names` -- `<code_key>\t<asset>::<entry>` per line, what the runtime loads.
    Many keys per entry point, because every candidate offset gets one.
  * `--out` -- one JSON record per entry point, carrying the named constants and
    samplers that later work needs.

Usage:
    py -3 tools/shader_manifest.py --assets out/all
    py -3 tools/shader_manifest.py --assets out/all --ucode <dir>   # with self-test
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

# The shortest microcode the runtime will accept is 8 dwords; below that a
# candidate is noise regardless of what its header looks like.
MIN_BLOB_BYTES = 64


def load_shader_reader():
    """tools/shader_reader.py already decodes entry names and constant tables."""
    here = os.path.dirname(os.path.abspath(__file__))
    spec = importlib.util.spec_from_file_location(
        "shader_reader", os.path.join(here, "shader_reader.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def swap32(buf):
    """Between the assets' big-endian dwords and the runtime's little-endian."""
    n = len(buf) - len(buf) % 4
    return b"".join(buf[i:i + 4][::-1] for i in range(0, n, 4))


def code_key_be(be_bytes):
    """FNV-1a 64 over the microcode as the RUNTIME holds it.

    REX_LOAD_U32 byte-swaps guest memory into host order, so the runtime's
    code[] is little-endian dwords. The assets store big-endian, so the bytes
    are swapped before folding -- and it folds whole dwords, matching
    `code_key ^= code[i]` in ReportHlslCoverage.
    """
    n = len(be_bytes) // 4
    h = FNV64_OFFSET
    for dword in struct.unpack(">%dI" % n, be_bytes[:n * 4]):
        h = ((h ^ dword) * FNV64_PRIME) & MASK64
    return h


def bxml_blocks(buf):
    """Offsets of every BXML block, plus end-of-file, so blocks pair up."""
    offs = []
    o = buf.find(b"BXML")
    while o >= 0:
        offs.append(o)
        o = buf.find(b"BXML", o + 4)
    offs.append(len(buf))
    return offs


def plausible(buf, start, end):
    """Could `buf[start:end]` be a microcode blob?

    Tuned to keep every one of the 161 ground truths -- see the module docstring
    for what a tighter first cut cost.
    """
    if end - start < MIN_BLOB_BYTES or (end - start) % 4:
        return False
    d0, d1 = struct.unpack(">II", buf[start:start + 8])
    if (d0 >> 24) > 0x0F:
        return False
    top = d1 >> 24
    return (top & 0x0F) == 0 and top <= 0x70


def main():
    ap = argparse.ArgumentParser(
        description="Name guest shaders from the .shader assets")
    ap.add_argument("--assets", default="out/all")
    ap.add_argument("--out", default="out/shader_manifest.json")
    ap.add_argument("--names", default="userdata/shader_names.txt")
    ap.add_argument("--ucode", default=None,
                    help="directory of microcode blobs; enables the self-test")
    ap.add_argument("--glob", default="*.ucode.bin.*")
    args = ap.parse_args()

    sr = load_shader_reader()

    paths = sorted(glob.glob(os.path.join(args.assets, "**", "*.shader"),
                             recursive=True))
    if not paths:
        sys.exit(f"no .shader assets under {args.assets}")
    assets = {p: open(p, "rb").read() for p in paths}
    print(f"assets    {len(assets)} .shader files, "
          f"{sum(len(b) for b in assets.values()) / 1e6:.1f} MB")

    names = {}      # code_key -> "asset::entry"
    records = {}    # "asset::entry" -> the rich record
    candidates = 0
    entry_points = 0

    for path, buf in assets.items():
        rel = os.path.relpath(path, args.assets).replace("\\", "/")
        entry_offsets = sr.find_entry_names(buf)
        try:
            _, tables = sr.read_shader(path)
        except Exception:
            tables = []
        by_entry = {}
        for t in tables:
            by_entry.setdefault(t.get("entry"), t)

        blocks = bxml_blocks(buf)
        for i in range(len(blocks) - 1):
            block_start, block_end = blocks[i], blocks[i + 1]
            # The entry name for this block is the last one declared before it.
            before = [n for o, n in entry_offsets if o < block_end]
            if not before:
                continue
            entry = before[-1]
            label = f"{rel}::{entry}"

            if label not in records:
                entry_points += 1
                table = by_entry.get(entry, {})
                cmap, smap = {}, {}
                for c in table.get("constants", []):
                    is_sampler = (c.get("set") == "sampler" or
                                  str(c.get("type", "")).startswith("sampler"))
                    (smap if is_sampler else cmap)[str(c["reg"])] = c["name"]
                records[label] = {
                    "asset": rel,
                    "entry": entry,
                    "profile": table.get("profile", ""),
                    "constants": cmap,
                    "samplers": smap,
                }

            # EVERY BYTE OFFSET, not every fourth. The blobs are dword
            # sequences but they are NOT stored on the file's dword grid --
            # measured, 174 of 185 byte-exact blobs start at an unaligned
            # offset. Forcing 4-alignment excludes almost all of them, which
            # the self-test caught by dropping from 17 derived to 12.
            #
            # plausible() still requires a multiple-of-4 LENGTH, so with the end
            # fixed only one residue class survives -- the accepted count is the
            # same, the loop is just four times longer.
            for start in range(block_start, block_end - MIN_BLOB_BYTES):
                if not plausible(buf, start, block_end):
                    continue
                candidates += 1
                names.setdefault(code_key_be(buf[start:block_end]), label)

    print(f"entry points {entry_points}")
    print(f"candidates   {candidates} suffixes -> {len(names)} distinct keys")

    os.makedirs(os.path.dirname(args.names) or ".", exist_ok=True)
    with open(args.names, "w", encoding="utf-8") as f:
        for k in sorted(names):
            f.write("%016X\t%s\n" % (k, names[k]))
    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as f:
        json.dump(records, f, indent=1, sort_keys=True)

    print(f"\nwrote {args.names}   ({os.path.getsize(args.names) / 1e6:.1f} MB)")
    print(f"wrote {args.out}   ({os.path.getsize(args.out) / 1e3:.0f} KB)")

    if not args.ucode:
        print("\nno --ucode given, so nothing checked this against real blobs")
        return

    # SELF-TEST. Re-derive known blobs from the assets and fail loudly if the
    # filter dropped one. A manifest that silently loses shaders looks exactly
    # like a manifest that is working.
    blobs = sorted(glob.glob(os.path.join(args.ucode, args.glob)))
    found = absent = tooshort = 0
    missing = []
    for bp in blobs:
        le = open(bp, "rb").read()
        if len(le) < MIN_BLOB_BYTES:
            tooshort += 1
            continue
        key = code_key_be(swap32(le))
        if key in names:
            found += 1
        else:
            absent += 1
            if len(missing) < 8:
                missing.append(os.path.basename(bp))
    total = found + absent
    print(f"\nSELF-TEST against {args.ucode}")
    print(f"  re-derived {found}/{total} ({found * 100.0 / max(total, 1):.1f}%), "
          f"{tooshort} under {MIN_BLOB_BYTES} bytes")
    if absent:
        print(f"  NOT DERIVED ({absent}) -- patched at load, or the filter is "
              f"too tight. First few: {', '.join(missing)}")

    # A blob the assets cannot reproduce is one the guest PATCHED at load, so no
    # hash of asset bytes will ever name it. But the dump holds the post-patch
    # bytes, so it can still be named -- located by a window from the MIDDLE of
    # the blob (the heads are shared, and anchoring there names the wrong entry)
    # and attributed to the last entry name declared before the hit.
    #
    # Additive on purpose: the asset walk covers every entry point in the game,
    # this covers the patched shaders that have actually been observed. Dropping
    # it to keep the tool "pure" would lose coverage the old dump-only manifest
    # already had.
    added = 0
    for bp in blobs:
        le = open(bp, "rb").read()
        if len(le) < MIN_BLOB_BYTES:
            continue
        key = code_key_be(swap32(le))
        if key in names:
            continue
        be = swap32(le)
        mid = (len(be) // 2) & ~3
        needle = be[mid:mid + 64]
        for path, buf in assets.items():
            off = buf.find(needle)
            if off < 0:
                continue
            rel = os.path.relpath(path, args.assets).replace("\\", "/")
            before = [n for o, n in sr.find_entry_names(buf) if o < off]
            if before:
                names[key] = f"{rel}::{before[-1]}"
                added += 1
            break
    if added:
        with open(args.names, "w", encoding="utf-8") as f:
            for k in sorted(names):
                f.write("%016X\t%s\n" % (k, names[k]))
        print(f"  added {added} patched-shader keys from the dump; "
              f"{len(names)} keys total")


if __name__ == "__main__":
    main()
