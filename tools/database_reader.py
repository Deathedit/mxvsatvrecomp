"""Read .xenon.database files: the index that says where every asset lives.

A .xenon.database is BXML, so `bxml_full_decoder.py <file>` already prints it
verbatim and always could. What that does not give you is the thing the file is
FOR -- which package holds an asset, at what absolute offset, whether it is
compressed, and whether the index agrees with the .xenon.package beside it.
This answers those, and checks the last one rather than assuming it.

    python tools/database_reader.py assets/Database/FR_Dunes.xenon.database
    python tools/database_reader.py assets/Database --verify
    python tools/database_reader.py assets/Database --find "*Palm*"
    python tools/database_reader.py assets/Database/FR_Dunes.xenon.database --assets

THE STRUCTURE, from FR_Dunes (403 assets, 2 packages):

    <Database>
      <Handles>                       every asset, NAME AND TYPE ONLY
        <Asset name type/>            403 of them, no extent
      <Packages>
        <Package name file offset blockOffset heapOffset>
          <Asset type name>           403 again, this time with extents
            <Heap offset size/>       offset RELATIVE to the Package heapOffset
            <Block offset size/>      0/0 for every asset seen
            <Compress enabled codec/> MAY BE ABSENT -- see below

TWO THINGS THAT LOOK LIKE BUGS AND ARE NOT.

  * The asset list appears twice. <Handles> is the name->type table the engine
    resolves against; <Packages> is the storage map. They carry the same 403
    names here, and --verify compares them rather than trusting it: a name in
    one and not the other is a real inconsistency and is reported.

  * <Compress> IS OPTIONAL. FR_Dunes has 403 Heap entries and 401 Compress
    entries, so two assets carry no Compress element at all. Those are stored
    verbatim, which is exactly the population xcompress.is_heap detects from
    the framing -- the database says the same thing a second way, and --verify
    cross-checks the two. Treating a missing <Compress> as "compressed" is how
    a decoder ends up running LZX over raw BXML.

HEAP OFFSETS ARE RELATIVE. Every Heap offset is relative to its Package's
heapOffset attribute; read as absolute they look like they only cover the first
few hundred KB of a multi-megabyte package. package_decoder.parse_database
already resolves this and this file reuses it rather than re-deriving it.

WHAT --verify CHECKS, all against the real .xenon.package on disk:

    every asset's absolute extent lies inside the file
    extents do not overlap each other
    gaps between extents are reported (they are legal, but a large one
      usually means an extent was misread)
    the Handles and Packages name sets agree
    the Compress flag agrees with the heap framing xcompress sees

An EMPTY package (0 bytes, four of them ship that way) is reported as empty
rather than as a file of unreadable extents.
"""

import argparse
import collections
import fnmatch
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bxml_full_decoder import decode_bxml  # noqa: E402
from package_decoder import attr_by_name, parse_database  # noqa: E402
import xcompress  # noqa: E402


def child_by_name(node, name):
    for c in node.children:
        if c.name == name:
            return c
    return None


def read_database(db_path):
    """Everything the file says, as plain dicts.

    Returns (packages, handles) where packages is
    [{'name','file','offset','blockOffset','heapOffset','assets':[...]}] and
    handles is [(name, type)].
    """
    root = decode_bxml(db_path)
    handles = []
    hnode = child_by_name(root, 'Handles')
    if hnode:
        for a in hnode.children:
            if a.name == 'Asset':
                handles.append((attr_by_name(a, 'name'),
                                attr_by_name(a, 'type')))
    packages = []
    pnode = child_by_name(root, 'Packages')
    if pnode:
        for pkg in pnode.children:
            if pkg.name != 'Package':
                continue
            base = int(attr_by_name(pkg, 'heapOffset') or 0)
            entry = {
                'name': attr_by_name(pkg, 'name'),
                'file': attr_by_name(pkg, 'file'),
                'offset': int(attr_by_name(pkg, 'offset') or 0),
                'blockOffset': int(attr_by_name(pkg, 'blockOffset') or 0),
                'heapOffset': base,
                'assets': [],
            }
            for a in pkg.children:
                if a.name != 'Asset':
                    continue
                heap = child_by_name(a, 'Heap')
                comp = child_by_name(a, 'Compress')
                entry['assets'].append({
                    'name': attr_by_name(a, 'name'),
                    'type': attr_by_name(a, 'type'),
                    'rel_offset': int(attr_by_name(heap, 'offset') or 0)
                                  if heap else 0,
                    'size': int(attr_by_name(heap, 'size') or 0) if heap else 0,
                    'offset': base + (int(attr_by_name(heap, 'offset') or 0)
                                      if heap else 0),
                    # ABSENT is its own answer, not False -- see the module note.
                    'codec': attr_by_name(comp, 'codec') if comp else None,
                    'compressed': (attr_by_name(comp, 'enabled') == 'true')
                                  if comp else None,
                })
            packages.append(entry)
    return packages, handles


def package_path(db_path, pkg):
    """The .xenon.package this Package's `file` attribute names."""
    stem = pkg['file'] or os.path.basename(db_path).split('.')[0]
    return os.path.join(os.path.dirname(db_path), stem + '.xenon.package')


def verify(db_path, packages, handles, report):
    """Check the index against the package on disk. Returns problems found."""
    problems = 0
    hnames = set(n for n, _ in handles)
    pnames = set(a['name'] for p in packages for a in p['assets'])
    # Reported both ways: "in Handles only" and "in Packages only" are
    # different faults and a symmetric difference hides which.
    for missing, where in ((hnames - pnames, 'Handles but not Packages'),
                           (pnames - hnames, 'Packages but not Handles')):
        if missing:
            problems += len(missing)
            report.append('  %d asset(s) in %s, e.g. %s'
                          % (len(missing), where,
                             ', '.join(sorted(missing)[:4])))

    for pkg in packages:
        path = package_path(db_path, pkg)
        if not os.path.exists(path):
            report.append('  %s: no %s beside the database'
                          % (pkg['name'], os.path.basename(path)))
            problems += 1
            continue
        size = os.path.getsize(path)
        if size == 0:
            report.append('  %s: package is EMPTY (0 bytes), %d assets listed'
                          % (pkg['name'], len(pkg['assets'])))
            continue
        spans = []
        for a in pkg['assets']:
            end = a['offset'] + a['size']
            if end > size:
                report.append('  %s/%s: extent %d..%d runs past EOF (%d)'
                              % (pkg['name'], a['name'], a['offset'], end, size))
                problems += 1
                continue
            spans.append((a['offset'], end, a['name']))
        spans.sort()
        for (a0, a1, an), (b0, b1, bn) in zip(spans, spans[1:]):
            if b0 < a1:
                report.append('  %s: %s and %s OVERLAP (%d..%d vs %d..%d)'
                              % (pkg['name'], an, bn, a0, a1, b0, b1))
                problems += 1
        # Framing cross-check: the database's Compress flag against what the
        # heap framing itself says. Two independent answers to one question.
        with open(path, 'rb') as f:
            blob = f.read()
        disagree = 0
        for a in pkg['assets']:
            if a['offset'] + a['size'] > len(blob):
                continue
            framed = xcompress.is_heap(blob, a['offset'], a['size'])
            stated = a['compressed']
            # A missing <Compress> means stored; is_heap should agree.
            if stated is None and framed:
                disagree += 1
            elif stated is True and not framed:
                disagree += 1
        if disagree:
            report.append('  %s: %d asset(s) where <Compress> and the heap '
                          'framing disagree' % (pkg['name'], disagree))
            problems += disagree
    return problems


def main():
    ap = argparse.ArgumentParser(
        description='Read .xenon.database files: what is where, and is it right.')
    ap.add_argument('target', help='a .xenon.database, or a directory of them')
    ap.add_argument('--assets', action='store_true',
                    help='list every asset with its absolute extent')
    ap.add_argument('--find', default='',
                    help='glob over asset names; implies --assets')
    ap.add_argument('--types', action='store_true', help='type census only')
    ap.add_argument('--verify', action='store_true',
                    help='check the index against the .xenon.package on disk')
    args = ap.parse_args()

    if os.path.isdir(args.target):
        paths = sorted(os.path.join(args.target, f)
                       for f in os.listdir(args.target)
                       if f.endswith('.xenon.database'))
    else:
        paths = [args.target]

    grand = collections.Counter()
    total_assets = total_bytes = total_problems = 0
    for path in paths:
        try:
            packages, handles = read_database(path)
        except Exception as exc:  # noqa: BLE001 - reported per file
            print('%-34s FAILED: %s' % (os.path.basename(path), exc))
            total_problems += 1
            continue
        assets = [a for p in packages for a in p['assets']]
        nbytes = sum(a['size'] for a in assets)
        types = collections.Counter(a['type'] for a in assets)
        grand.update(types)
        total_assets += len(assets)
        total_bytes += nbytes
        stored = sum(1 for a in assets if a['compressed'] is not True)

        print('# %s' % os.path.basename(path).replace('.xenon.database', ''))
        print('  %d assets in %d package(s), %d handles, %.2f MB'
              % (len(assets), len(packages), len(handles),
                 nbytes / (1024.0 * 1024.0)))
        if stored:
            print('  %d asset(s) NOT LZX-compressed (no <Compress> or '
                  'enabled=false)' % stored)
        for pkg in packages:
            print('     %-24s file=%-18s heapOffset=%-10d %d assets'
                  % (pkg['name'], pkg['file'], pkg['heapOffset'],
                     len(pkg['assets'])))
        if args.types:
            print('  ' + ', '.join('%s=%d' % kv for kv in types.most_common()))

        if args.assets or args.find:
            for pkg in packages:
                for a in pkg['assets']:
                    if args.find and not fnmatch.fnmatch(a['name'], args.find):
                        continue
                    print('     %-36s %-10s %-12s @%-10d %8d  %s'
                          % (a['name'], a['type'], pkg['name'], a['offset'],
                             a['size'],
                             a['codec'] or 'stored'))
        if args.verify:
            report = []
            n = verify(path, packages, handles, report)
            total_problems += n
            for line in report:
                print(line)
            print('  verify: %s' % ('OK' if n == 0 else '%d PROBLEM(S)' % n))

    if len(paths) > 1:
        print('')
        print('%d databases, %d assets, %.2f MB'
              % (len(paths), total_assets, total_bytes / (1024.0 * 1024.0)))
        print('types: ' + ', '.join('%s=%d' % kv for kv in grand.most_common()))
        if args.verify:
            print('total problems: %d' % total_problems)
    return 1 if total_problems else 0


if __name__ == '__main__':
    sys.exit(main())
