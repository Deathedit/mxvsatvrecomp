"""Decode .xenon.package files.

WHAT A PACKAGE IS, measured against MXUI.xenon.package (33 MB, 1533 assets):

  * The sibling .xenon.database is a BXML tree naming every asset and giving
    its heap extent. Offsets in <Heap> are RELATIVE to the owning <Package>'s
    heapOffset. The heaps tile the package with no gaps and no overlap.
  * A heap is a container: a length, then one or more LZX streams in Xbox 360
    XCompress chunk framing. See xcompress.py, which owns that format.
  * Decompressed asset bytes are homogeneous by type -- every uicmpnt starts
    'BXML', every texture 0x77DDCC00, every swfx 00 00 00 01, every script a
    big-endian length followed by Lua source.

This file has been wrong in three separate ways, each of which produced output
rather than an error, so its history is worth keeping:

  1. It assumed one BXML block per heap start, printed "NO BXML signature
     found" 1404 times, and extracted nothing from any UI package.
  2. It mis-walked the database, returning 71 duplicate 'MXUI' entries with
     identical offsets. Only the first was ever used.
  3. It walked the database tree by record ORDER (first pre-order, then
     breadth-first) instead of the explicit first-child index. That produced a
     plausible 324-asset table -- a fifth of the real 1533 -- with heap offsets
     that pointed into the middle of other assets.

Usage:
    python tools/package_decoder.py <file.xenon.package>
    python tools/package_decoder.py <file.xenon.package> --search SmokeVideo
    python tools/package_decoder.py <file.xenon.package> --extract "*.layer"

--search decompresses each asset and greps its decoded form, which is the only
way to find a name stored compressed. A plain grep of the package cannot see
it. Decompression is pure Python and not fast: a whole 33 MB package takes a
few minutes, so --search and --extract take a glob to narrow the work.
"""

import argparse
import collections
import fnmatch
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bxml_full_decoder import decode_bxml, decode_bxml_bytes  # noqa: E402
from xcompress import decompress_asset, split_streams  # noqa: E402

BXML_MAGIC = b'BXML'


def attr_by_name(node, name):
    for k, v in node.attrs:
        if k == name:
            return v
    return None


def parse_database(db_path):
    """The asset table: every asset with its ABSOLUTE heap extent and codec.

    Structure:

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

    An asset NAME is not unique: the same asset is repeated verbatim in each
    localized package, so MXUI lists 1533 assets but only 75 distinct uicmpnt.
    Duplicates decompress byte-identically, which is a free check on the heap
    extents being right.

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


def asset_text(raw):
    """A searchable/printable rendering of one decompressed asset.

    uicmpnt and bxml assets are BXML and get decoded to XML. Everything else is
    handed back as latin-1, which never raises and leaves ASCII runs (Lua
    source, resource names) greppable inside otherwise binary data.
    """
    if raw[:4] == BXML_MAGIC:
        try:
            return '\n'.join(decode_bxml_bytes(raw).to_xml())
        except Exception as exc:
            return '<<BXML decode failed: %s>>' % exc
    return raw.decode('latin-1')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('package', nargs='?',
                    default=r'C:\Users\VM\Desktop\mx\assets\Database\MXUI.xenon.package')
    ap.add_argument('--search', default='',
                    help='case-insensitive substring to look for in decompressed assets')
    ap.add_argument('--extract', default='',
                    help='glob over asset names; writes each match under <out-dir>/<stem>/')
    ap.add_argument('--filter', default='*',
                    help='glob over asset names, limiting --search (default all)')
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
        by_type = collections.Counter(a['type'] for a in assets)
        covered = sum(a['size'] for a in assets)
        over = [a for a in assets if a['offset'] + a['size'] > len(data)]
        gaps = sum(1 for x, y in zip(sorted(assets, key=lambda a: a['offset']),
                                     sorted(assets, key=lambda a: a['offset'])[1:])
                   if x['offset'] + x['size'] != y['offset'])
        # The coverage/gap/EOF triple is the check that caught two rounds of
        # wrong heap offsets. Keep it printed even when it is boring.
        lines.append('Assets: %d  (%s)' % (len(assets),
                     ', '.join('%s=%d' % kv for kv in by_type.most_common())))
        lines.append('Heaps cover %d of %d bytes (%.1f%%), %d gaps, %d past EOF%s'
                     % (covered, len(data), 100.0 * covered / max(1, len(data)),
                        gaps, len(over),
                        '  -- OFFSETS ARE WRONG' if (over or gaps) else ''))
        lines.append('')
        lines.append('%-40s %-9s %-11s %-9s %s'
                     % ('asset', 'type', 'offset', 'size', 'codec'))
        for a in sorted(assets, key=lambda x: x['offset']):
            lines.append('%-40s %-9s %-11d %-9d %s'
                         % (a['name'], a['type'], a['offset'], a['size'],
                            a['codec'] or 'raw'))
        lines.append('')

    # --- decompress ---------------------------------------------------------
    # Only what was asked for: LZX here is pure Python and a whole package is
    # minutes of work.
    needle = args.search.lower()
    hits = []
    done = failed = 0
    extract_dir = os.path.join(args.out_dir, stem)
    seen_names = set()
    for a in sorted(assets, key=lambda x: x['offset']):
        name = a['name'] or 'unnamed_%d' % a['offset']
        want_extract = args.extract and fnmatch.fnmatch(name, args.extract)
        want_search = needle and fnmatch.fnmatch(name, args.filter)
        if not (want_extract or want_search):
            continue
        # Assets repeat verbatim across localized packages; decode each once.
        if name in seen_names:
            continue
        seen_names.add(name)
        try:
            raw = decompress_asset(data, a['offset'], a['size'])
        except Exception as exc:
            failed += 1
            lines.append('DECOMPRESS FAILED %s: %s' % (name, exc))
            continue
        done += 1
        if want_extract:
            os.makedirs(extract_dir, exist_ok=True)
            with open(os.path.join(extract_dir, name), 'wb') as fp:
                fp.write(raw)
        if want_search:
            for i, ln in enumerate(asset_text(raw).splitlines()):
                if needle in ln.lower():
                    hits.append((name, i, ln.strip()))

    summary = ['Decompressed %d assets, %d failed' % (done, failed)]
    if args.extract:
        summary.append("Extracted '%s' to %s" % (args.extract, extract_dir))
    if needle:
        summary.append("Search '%s': %d hits" % (args.search, len(hits)))
        for name, i, ln in hits[:60]:
            summary.append('  %s:%d: %s' % (name, i, ln[:160]))
    lines.append('')
    lines.extend(summary)

    os.makedirs(args.out_dir, exist_ok=True)
    out_path = os.path.join(args.out_dir, stem + '_package_manifest.txt')
    with open(out_path, 'w', encoding='utf-8') as fp:
        fp.write('\n'.join(lines))

    print('\n'.join(lines[:5]))
    print('\n'.join(summary[:40]))
    print('Wrote %s (%d lines)' % (out_path, len(lines)))


if __name__ == '__main__':
    main()
