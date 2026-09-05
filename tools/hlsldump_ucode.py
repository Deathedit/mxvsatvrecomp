"""Recover microcode blobs from the runtime's own HLSL dumps.

The shaders the asset walk cannot name are the ones the guest PATCHES at load:
their bytes no longer match the file they came from, so no hash of asset content
reaches them. But the runtime dumps the microcode it actually ran, as hex dwords
inside logs/hlsldump/*.txt:

    === GUEST MICROCODE (24 dwords) ===
    ; 0000: 00193002 00001200 C4000000 ...

This turns those back into raw little-endian blobs -- the same form
tools/shader_manifest.py takes via --ucode -- so a shader that was unnamed in one
run can be named in the next. Coverage becomes self-improving: play, dump,
re-run, and the map grows.

Verified against the SHADER NAMES census of 2026-09-05: all twelve code_keys the
report named as unnamed were present in the dumps.

Usage:
    py -3 tools/hlsldump_ucode.py --dumps logs/hlsldump --out out/ucode_runtime
    py -3 tools/shader_manifest.py --assets out/all --ucode out/ucode_runtime
"""

import argparse
import glob
import os
import re
import struct

MICROCODE_RE = re.compile(r"=== GUEST MICROCODE \((\d+) dwords\) ===(.*?)\n\n",
                          re.S)


def parse_dump(path):
    """The dwords of one dump, or None if it carries no microcode section."""
    text = open(path, "rb").read().decode("utf-8", "replace")
    m = MICROCODE_RE.search(text)
    if not m:
        return None
    want = int(m.group(1))
    dwords = []
    for line in m.group(2).splitlines():
        line = line.strip()
        if not line.startswith(";"):
            continue
        _, _, rest = line.partition(":")
        for tok in rest.split():
            if len(tok) == 8:
                try:
                    dwords.append(int(tok, 16))
                except ValueError:
                    pass
    # A truncated section is worse than none: it would hash to a key no shader
    # ever presents and quietly pad the manifest with a name that never matches.
    return dwords[:want] if len(dwords) >= want else None


def main():
    ap = argparse.ArgumentParser(
        description="Extract microcode blobs from logs/hlsldump")
    ap.add_argument("--dumps", default="logs/hlsldump")
    ap.add_argument("--out", default="out/ucode_runtime")
    args = ap.parse_args()

    paths = sorted(glob.glob(os.path.join(args.dumps, "*.txt")))
    if not paths:
        print(f"no dumps under {args.dumps}")
        return
    os.makedirs(args.out, exist_ok=True)

    written = skipped = 0
    for p in paths:
        dwords = parse_dump(p)
        if not dwords:
            skipped += 1
            continue
        stem = os.path.splitext(os.path.basename(p))[0]
        # The extension shader_manifest matches on, and the stage from the
        # dump's own prefix so vertex and pixel stay distinguishable.
        ext = "vert" if stem.startswith("vs_") else "frag"
        dest = os.path.join(args.out, f"{stem}.ucode.bin.{ext}")
        with open(dest, "wb") as f:
            f.write(struct.pack("<%dI" % len(dwords), *dwords))
        written += 1

    print(f"dumps {len(paths)}   wrote {written}   "
          f"skipped {skipped} (no microcode section, or truncated)")
    print(f"-> {args.out}")


if __name__ == "__main__":
    main()
