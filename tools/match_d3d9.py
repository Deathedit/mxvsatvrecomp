"""Emit IDA byte patterns for XDK D3D9 functions, to locate them in the XEX.

The title statically links D3D9 v2.0.20209.3, so the library's code is present
in the XEX but nameless. `assets/d3d9/d3d9.lib` is a matching XDK build --
proven by `D3DDevice_Swap` being 0x684 bytes, the exact size of the already
confirmed `sub_82566B58`.

The library is compiled with function-level linking, so every symbol sits at
offset 0 of its own COMDAT `.text` section and a function's bytes are exactly
that section's raw data. The only bytes the linker rewrites are the relocation
sites, so wildcarding those leaves a pattern that matches the linked code.

Usage:
    python tools/match_d3d9.py swap.obj D3DDevice_Swap
    python tools/match_d3d9.py --all          # every target in TARGETS
"""

import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
D3D9 = ROOT / "assets" / "d3d9"
OBJS = D3D9 / "objs_d3d9"
SYMBOLS = D3D9 / "d3d9_symbols.txt"

# Functions worth matching, in priority order. Everything here is >= 128 bytes;
# below that a byte match is not evidence, because the SetRenderState_* leaves
# are 20-56 bytes and several are byte-identical to each other.
TARGETS = [
    ("swap.obj", "D3DDevice_Swap"),  # the control -- must land on 0x82566B58
    ("draw.obj", "D3DDevice_DrawIndexedVertices"),
    ("draw.obj", "D3DDevice_DrawVertices"),
    ("lazy.obj", "?PatchVertexShaderToMatchVertexDeclaration@D3D@@"),
    ("shader.obj", "D3DDevice_SetVertexShader"),
    ("shader.obj", "XGSetVertexDeclaration"),
    ("shader.obj", "D3DDevice_CreateVertexDeclaration"),
    ("state.obj", "D3DDevice_SetStreamSource"),
    ("texture.obj", "D3DDevice_SetTexture"),
]

IMAGE_REL_PPC_REL24 = 0x06


def load_symbols():
    """obj -> {mangled name: section index}, from the extracted symbol dump."""
    table = {}
    for line in SYMBOLS.read_text(encoding="latin-1").splitlines():
        parts = line.split("\t")
        if len(parts) != 4:
            continue
        obj, sec, _off, name = parts
        table.setdefault(obj, {}).setdefault(name, int(sec[3:]))
    return table


def section(obj_bytes, index):
    """Raw data and relocations of a 1-based COFF section index."""
    opt = struct.unpack_from("<H", obj_bytes, 16)[0]
    off = 20 + opt + (index - 1) * 40
    _vsize, _vaddr, size, praw, prel, _pln, nrel, _nln, _chars = struct.unpack_from(
        "<IIIIIIHHI", obj_bytes, off + 8
    )
    data = obj_bytes[praw : praw + size]
    relocs = [
        struct.unpack_from("<IIH", obj_bytes, prel + i * 10) for i in range(nrel)
    ]
    return data, relocs


def pattern(data, relocs):
    """IDA byte pattern with every relocated instruction word wildcarded.

    REL24 only rewrites bits 6-29, but masking the whole word costs almost
    nothing at these reloc densities (23 in 285 instructions for
    DrawIndexedVertices) and cannot be wrong about which bits move.
    """
    masked = bytearray(data)
    wild = [False] * len(data)
    for vaddr, _sym, kind in relocs:
        word = vaddr & ~3
        if word + 4 > len(data):
            continue
        if kind != IMAGE_REL_PPC_REL24:
            print(f"  note: relocation type {kind:#x} at {vaddr:#x}", file=sys.stderr)
        for i in range(word, word + 4):
            wild[i] = True
    return " ".join("??" if wild[i] else f"{masked[i]:02X}" for i in range(len(data)))


def resolve(symbols, obj, name):
    """Exact name, else unique prefix match (the mangled names are long)."""
    names = symbols[obj]
    if name in names:
        return names[name]
    hits = [k for k in names if k.startswith(name)]
    if len(hits) == 1:
        return names[hits[0]]
    raise KeyError(f"{obj}: {name!r} matched {len(hits)} symbols")


def emit(symbols, obj, name):
    index = resolve(symbols, obj, name)
    data, relocs = section((OBJS / obj).read_bytes(), index)
    pat = pattern(data, relocs)
    literal = sum(1 for tok in pat.split() if tok != "??")
    print(f"### {obj} sec{index} {name}")
    print(f"# {len(data)} bytes, {len(relocs)} relocs, {literal} literal")
    print(pat)
    print()


def main():
    symbols = load_symbols()
    if len(sys.argv) == 2 and sys.argv[1] == "--all":
        for obj, name in TARGETS:
            emit(symbols, obj, name)
    elif len(sys.argv) == 3:
        emit(symbols, sys.argv[1], sys.argv[2])
    else:
        print(__doc__)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
