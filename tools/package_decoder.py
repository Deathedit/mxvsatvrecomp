"""Decode .xenon.package files.

WHAT A PACKAGE ACTUALLY IS, measured against MXUI.xenon.package (33 MB):

  * The sibling .xenon.database declares the layout as a flat list of heaps
    (offset + size). MXUI declares 1470.
  * A heap is NOT a BXML block. It carries a 12-byte little-endian header --
    `u32 size-4`, `u32 0x00010000`, `u32 size-16` -- followed by an opaque
    payload. Most of a UI package is texture and asset data.
  * BXML blocks DO live in a package, but at ARBITRARY offsets rather than at
    heap starts, and zlib-compressed after a 28-byte header. MXUI has 16.

The previous version of this file assumed one BXML block per heap start. It
therefore printed "NO BXML signature found" 1404 times out of 1470 and extracted
nothing from any UI package -- MXUI_Streaming's manifest contained no content
either, which is what made the breakage easy to miss: it produced a file, just
an empty one.

It also mis-walked the database, returning 71 duplicate 'MXUI' entries with
identical offsets and decreasing heap counts (1470, 1468, 1466, ...). Only the
first was ever used.

Usage:
    python tools/package_decoder.py <file.xenon.package>
    python tools/package_decoder.py <file.xenon.package> --search VideoRenderTarget

--search greps the DECODED XML of every BXML block, which is the only way to
find a name that is stored compressed. A plain grep of the package file cannot
see it.
"""

import argparse
import collections
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bxml_full_decoder import decode_bxml, decode_bxml_bytes  # noqa: E402

BXML_MAGIC = b'BXML'
HEAP_HEADER_BYTES = 12
# The constant dword every heap header carries at +4. Checked rather than
# skipped: a heap that does not have it is not the shape this parser assumes,
# and saying so is more useful than decoding it as if it were.
HEAP_HEADER_TAG = 0x00010000


def attr_by_name(node, name):
    for k, v in node.attrs:
        if k == name:
            return v
    return None


def parse_database(db_path):
    """The asset table: every asset with its ABSOLUTE heap extent and codec.

    Structure, now that bxml_full_decoder walks records breadth-first:

        <Database>
          <Handles>  322 <Asset name type/>          -- names only, no heap
          <Packages>  71 <Package name file offset blockOffset heapOffset>
                            <Asset type name>
                              <Compress enabled codec="LZX"/>
                              <Heap offset size/>    -- offset is RELATIVE
                              <Block offset size/>

    Heap offsets are relative to the owning Package's heapOffset, which is why
    they looked like they only covered the first 660 KB of a 33 MB file when
    read as absolute.

    Returns [{'name','type','offset','size','codec'}], offset absolute.
    """
    root = decode_bxml(db_path)
    out = []
    packages = None
    for child in root.children:
        if child.name == 'Packages':
            packages = child
            break
    if packages is None:
        return out
    for pkg in packages.children:
        if pkg.name != 'Package':
            continue
        base = int(attr_by_name(pkg, 'heapOffset') or 0)
        for asset in pkg.children:
            if asset.name != 'Asset':
                continue
            heap = codec = None
            for c in asset.children:
                if c.name == 'Heap':
                    heap = (int(attr_by_name(c, 'offset')),
                            int(attr_by_name(c, 'size')))
                elif c.name == 'Compress':
                    codec = attr_by_name(c, 'codec')
            if heap:
                out.append({'name': attr_by_name(asset, 'name'),
                            'type': attr_by_name(asset, 'type'),
                            'offset': base + heap[0], 'size': heap[1],
                            'codec': codec})
    return out


def heap_header(data, offset, size):
    """The 12-byte header, or None when the heap does not have that shape."""
    if offset + HEAP_HEADER_BYTES > len(data):
        return None
    a, tag, b = struct.unpack('<III', data[offset:offset + HEAP_HEADER_BYTES])
    if tag != HEAP_HEADER_TAG:
        return None
    return {'declared_a': a, 'declared_b': b,
            'matches_size': (a == size - 4 and b == size - 16)}


def find_bxml_blocks(data):
    """Every BXML block in the package, wherever it sits."""
    out = []
    i = data.find(BXML_MAGIC)
    while i != -1:
        out.append(i)
        i = data.find(BXML_MAGIC, i + 1)
    return out


def node_to_lines(node, out):
    """BxmlNode.to_xml() already emits attributes AND node text, iteratively so
    deep trees do not blow the recursion limit. The first cut of this file
    hand-rolled a walker that dropped `text` -- which is exactly where an asset
    NAME sits, so a --search for one found nothing while appearing to work.
    """
    out.extend(node.to_xml())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('package', nargs='?',
                    default=r'C:\Users\VM\Desktop\mx\assets\Database\MXUI.xenon.package')
    ap.add_argument('--search', default='',
                    help='case-insensitive substring to look for in decoded XML')
    ap.add_argument('--out-dir', default=r'C:\Users\VM\Desktop\mx\out')
    args = ap.parse_args()

    pkg_path = args.package
    stem = os.path.basename(pkg_path).replace('.xenon.package', '')
    db_path = pkg_path.replace('.xenon.package', '.xenon.database')
    with open(pkg_path, 'rb') as f:
        data = f.read()

    lines = ['# %s' % os.path.basename(pkg_path),
             'Size: %d bytes' % len(data)]

    # --- asset table -------------------------------------------------------
    assets = []
    if os.path.exists(db_path):
        try:
            assets = parse_database(db_path)
        except Exception as exc:
            lines.append('Database unreadable: %s' % exc)
    else:
        lines.append('No sibling .xenon.database')

    if assets:
        by_codec = collections.Counter(a['codec'] for a in assets)
        covered = sum(a['size'] for a in assets)
        over = [a for a in assets if a['offset'] + a['size'] > len(data)]
        lines.append('Assets with heaps: %d  (%s)  covering %d of %d bytes '
                     '(%.1f%%)%s'
                     % (len(assets),
                        ', '.join('%s=%d' % (k or 'raw', v)
                                  for k, v in by_codec.most_common()),
                        covered, len(data), 100.0 * covered / max(1, len(data)),
                        '' if not over else
                        '  -- %d run past EOF, offsets are WRONG' % len(over)))
        lines.append('')
        lines.append('%-40s %-9s %-11s %-9s %s'
                     % ('asset', 'type', 'offset', 'size', 'codec'))
        for a in sorted(assets, key=lambda x: x['offset']):
            lines.append('%-40s %-9s %-11d %-9d %s'
                         % (a['name'], a['type'], a['offset'], a['size'],
                            a['codec'] or 'raw'))
        lines.append('')

    # --- BXML blocks -------------------------------------------------------
    blocks = find_bxml_blocks(data)
    lines.append('BXML blocks found: %d' % len(blocks))
    needle = args.search.lower()
    hits = []
    decoded_ok = decoded_fail = 0
    for off in blocks:
        try:
            root = decode_bxml_bytes(data[off:])
        except Exception as exc:
            decoded_fail += 1
            lines.append('\n=== BXML @%d -- FAILED: %s' % (off, exc))
            continue
        decoded_ok += 1
        sc, ss, sf, ac, nc = struct.unpack('<IIIII', data[off + 8:off + 28])
        body = []
        node_to_lines(root, body)
        body = [ln for ln in body if ln is not None]
        lines.append('\n=== BXML @%d  root <%s>  %d strings, %d nodes, %d lines'
                     % (off, root.name, sc, nc, len(body)))
        lines.extend(body)
        if needle:
            for i, ln in enumerate(body):
                if needle in ln.lower():
                    hits.append((off, i, ln.strip()))

    lines.append('\nDecoded %d BXML blocks, %d failed' % (decoded_ok, decoded_fail))
    if needle:
        lines.append("Search '%s': %d hits" % (args.search, len(hits)))
        for off, i, ln in hits[:40]:
            lines.append('  @%d line %d: %s' % (off, i, ln))

    os.makedirs(args.out_dir, exist_ok=True)
    out_path = os.path.join(args.out_dir, stem + '_package_manifest.txt')
    with open(out_path, 'w', encoding='utf-8') as fp:
        fp.write('\n'.join(lines))

    # Summary to stdout: the counts, plus search hits if any were asked for.
    print('\n'.join(lines[:6]))
    print('Decoded %d/%d BXML blocks' % (decoded_ok, len(blocks)))
    if needle:
        print("Search '%s': %d hits" % (args.search, len(hits)))
        for off, i, ln in hits[:20]:
            print('  @%d line %d: %s' % (off, i, ln))
    print('Wrote %s (%d lines)' % (out_path, len(lines)))


if __name__ == '__main__':
    main()
