"""BXML strings-only summarizer. Extract just the strings section from any BXML file.

Use this to understand what asset references, entity names, materials, shaders,
textures, and animations are declared in a given .bxml or .xenon.database file.

Usage:
    python bxml_strings.py <file.bxml|database>           # print all strings
    python bxml_strings.py <file.bxml|database> --count    # print only counts
    python bxml_strings.py <dir>                            # batch summarize
"""
import sys, re
from pathlib import Path
from collections import Counter

# Reuse the parsing from bxml_decoder.py
sys.path.insert(0, str(Path(__file__).parent))
from bxml_decoder import read_bxml_file, parse_strings

def summarize_file(path, print_strings=True):
    """Print a human-readable summary of strings in a BXML file."""
    try:
        bin_d = read_bxml_file(str(path))
    except Exception as e:
        print(f'# {path.name}: ERROR {e}')
        return

    strings, _ = parse_strings(bin_d)
    if not strings:
        print(f'# {path.name}: no strings found ({len(bin_d)} bytes decompressed)')
        return

    # Heuristic categorization: detect category from string prefix/pattern
    categories = {
        'entity':  [],   # names matching *_Veh, *_PopTent, Nat_*, Ve_*, GL_*
        'asset':   [],   # Database, Block, Package, Heap, Asset, Compress, etc.
        'shader':  [],   # HFB_*, HFT*, Shader*, etc.
        'material':[],   # Template_*, T_Diffuse*, T_*
        'anim':    [],   # SC_*, *Anim*, Crowd*
        'texture': [],   # *_Atlas_*, *_Atlas_Normal, *_Atlas_Diffuse
        'meta':    [],   # file, offset, size, type, heapOffset, blockOffset
        'other':   [],
    }

    meta_keys = {'Database', 'Block', 'Package', 'Packages', 'Heap', 'Asset',
                 'Compress', 'file', 'offset', 'size', 'type', 'heapOffset',
                 'blockOffset', 'anim', 'material', 'shader', 'texture', 'surface',
                 'codec', 'cell', 'name', 'enabled', 'bxml'}

    patterns = [
        ('entity',   re.compile(r'^(Ve_|GL_|Nat_|NAT_|Sky)', re.I)),
        ('shader',   re.compile(r'^(HFB_|HFT|SharedVertexShader|.*Shader$)', re.I)),
        ('material', re.compile(r'^(Template_|T_)')),
        ('anim',     re.compile(r'^(SC_|Crowd|.*Anim)', re.I)),
        ('texture',  re.compile(r'.*(_Atlas_|Atlas_Diffuse|Atlas_Normal)', re.I)),
    ]

    seen = [s for _, s in strings]
    for s in seen:
        if s in meta_keys:
            categories['meta'].append(s)
            continue
        placed = False
        for cat, pat in patterns:
            if pat.search(s):
                categories[cat].append(s)
                placed = True
                break
        if not placed:
            categories['other'].append(s)

    print(f'# === {path.name} ({len(bin_d)} bytes decompressed, {len(strings)} strings) ===')
    for cat, items in categories.items():
        if not items:
            continue
        unique_sorted = sorted(set(items))
        print(f'#')
        print(f'# {cat.upper()} ({len(unique_sorted)} unique){" " * 3}')
        if print_strings:
            # Show first 20 + ... if more
            shown = unique_sorted[:20]
            for s in shown:
                print(f'    {s}')
            if len(unique_sorted) > 20:
                print(f'    ... ({len(unique_sorted)-20} more)')

def main():
    if len(sys.argv) < 2:
        print(f'Usage: {sys.argv[0]} <file|dir> [--count]')
        sys.exit(1)
    path = Path(sys.argv[1])
    print_strings = '--count' not in sys.argv

    if path.is_dir():
        files = sorted(list(path.rglob('*.bxml')) + list(path.rglob('*.xenon.database')))
        for f in files:
            print()
            summarize_file(f, print_strings=print_strings)
    elif path.is_file():
        summarize_file(path, print_strings=print_strings)
    else:
        print(f'Not a file or directory: {path}')
        sys.exit(1)

if __name__ == '__main__':
    main()