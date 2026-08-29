"""Extract every asset out of .xenon.package files as real files on disk.

package_decoder.py already knows how to READ a package -- it owns the database
walk and the heap extents, and this file imports both rather than repeating
them. What it does not do is give you a directory you can browse: --extract
takes one glob, writes the raw decompressed bytes under the asset's bare name
with no extension, and handles one package per invocation. Finding
`FrontSuspension_SpringRestLength` meant decoding a package, reading a manifest,
extracting one asset by name and then running a second tool on it.

This does the whole tree in one command:

    python tools/extract_package.py assets/Database/MX_fork.xenon.package
    python tools/extract_package.py assets/Database --jobs 8
    python tools/extract_package.py assets/Database --filter "PU_MX125_*"

WHAT YOU GET. Each package becomes <out>/<stem>/, with every asset written
under a type-derived extension, plus two conveniences that are the point of the
exercise:

  * BXML assets are ALSO written decoded, as <name>.xml. That is the form the
    game's tuning data is readable in -- suspension lengths, tire radii, camera
    presets -- and it is what makes a package greppable with ordinary tools.
  * script assets are written as .lua with the big-endian length prefix
    stripped, after checking that the prefix agrees with the payload length. A
    mismatch is reported rather than silently trimmed, because "the first four
    bytes happen to look like a length" and "this is a length-prefixed script"
    are different claims.

DUPLICATES. The same asset repeats verbatim in each localized package --
MX_fork lists 29 assets but holds 8 distinct ones -- so by default each
(name, type) is decompressed and written ONCE and the repeats are taken on
trust.

`--verify-dupes` stops taking it on trust: it decompresses every instance and
compares digests, so a repeat that is NOT byte-identical gets reported. That
costs real time on a big package, which is why it is opt-in, but the check is
worth having occasionally because the whole toolchain treats an asset name as a
key -- package_decoder's --search included -- and a collision would mean it is
not one.

The first version of this file claimed to do that check while deduplicating
FIRST, so a repeat was skipped before it could ever be compared: the check
could not fire. What it DID produce was false alarms, because it hashed by name
while deduplicating by (name, type), and MX_tire genuinely holds a texture and
a material both called MX_Tires_Sedona. Two different payloads, two different
types, nothing wrong.

SPEED. LZX here is pure Python and single-threaded it is minutes per large
package, which is why package_decoder.py asks you to narrow the work. Assets
are independent, so this fans them out across processes; --jobs defaults to the
CPU count. MX_tire (2.7 MB, 173 assets) goes from about a minute to a few
seconds on this box. Use --filter when you know the name -- no amount of
parallelism beats not decompressing the other 165 assets.
"""

import argparse
import collections
import concurrent.futures as futures
import fnmatch
import hashlib
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bxml_full_decoder import decode_bxml_bytes  # noqa: E402
from shader_reader import read_shader, render as render_shader  # noqa: E402
from package_decoder import parse_database  # noqa: E402
from xcompress import decompress_asset  # noqa: E402

BXML_MAGIC = b'BXML'

# Type -> extension. The type string comes from the database, so this is a
# rename and not a guess about content; anything unlisted keeps its type as the
# extension, which is still better than no extension and makes the gap visible.
EXT = {
    'bxml': '.bxml',
    'uicmpnt': '.uicmpnt',
    'texture': '.texture',
    'material': '.material',
    'shader': '.shader',
    'surface': '.surface',
    'model': '.model',
    'script': '.lua',
    'swfx': '.swfx',
    'activity': '.activity',
}


def safe_name(name):
    """Asset names are engine identifiers, but they reach the filesystem here."""
    out = ''.join(c if (c.isalnum() or c in '._- ') else '_' for c in name)
    return out.strip() or 'unnamed'


def decompress_one(args):
    """Worker: returns (name, type, bytes) or (name, type, None, error)."""
    path, name, atype, offset, size = args
    try:
        with open(path, 'rb') as f:
            f.seek(offset)
            blob = f.read(size)
        return name, atype, decompress_asset(blob, 0, size), None
    except Exception as exc:  # noqa: BLE001 - reported per asset, not fatal
        return name, atype, None, str(exc)


def strip_script_prefix(raw, report):
    """A script is a big-endian length followed by Lua source.

    Checked, not assumed: if the prefix does not equal the remaining length the
    bytes are handed back untouched and the mismatch is reported. Trimming four
    bytes off something that is not length-prefixed corrupts it in a way that
    looks like a decompression bug later.
    """
    if len(raw) < 4:
        return raw
    n = int.from_bytes(raw[:4], 'big')
    if n == len(raw) - 4:
        return raw[4:]
    report.append('  script prefix %d != payload %d, left intact'
                  % (n, len(raw) - 4))
    return raw


def extract_package(pkg_path, out_root, name_filter, types, jobs, want_xml,
                    verify_dupes):
    stem = os.path.basename(pkg_path).replace('.xenon.package', '')
    db_path = pkg_path.replace('.xenon.package', '.xenon.database')
    if not os.path.exists(db_path):
        return stem, ['  no sibling .xenon.database -- skipped'], 0, 0

    assets = parse_database(db_path)
    jobs_list = []
    seen = set()
    for a in sorted(assets, key=lambda x: x['offset']):
        name = a['name'] or 'unnamed_%d' % a['offset']
        if not fnmatch.fnmatch(name, name_filter):
            continue
        if types and a['type'] not in types:
            continue
        key = (name, a['type'])
        # Without --verify-dupes a repeat is skipped here and never compared;
        # with it, every instance is decompressed so the comparison is real.
        if key in seen and not verify_dupes:
            continue
        seen.add(key)
        jobs_list.append((pkg_path, name, a['type'], a['offset'], a['size']))

    if not jobs_list:
        # An EMPTY package and a filter that excluded everything are different
        # facts and must not print the same line. Four of the 130 packages ship
        # as 0 bytes with a database listing no assets -- HR_ATV_brakes,
        # HR_ATV_frame, HR_MX_brakes, RiderSharedData -- and reporting those as
        # "nothing matched" invites someone to go looking for the filter bug
        # that is not there.
        if not assets:
            return stem, ['  EMPTY package: %d bytes, database lists no assets'
                          % os.path.getsize(pkg_path)], 0, 0
        return stem, ['  nothing matched (%d assets, all filtered out)'
                      % len(assets)], 0, 0

    out_dir = os.path.join(out_root, stem)
    os.makedirs(out_dir, exist_ok=True)
    report = []
    digests = {}
    dupes = [0]
    written = failed = 0

    with futures.ProcessPoolExecutor(max_workers=jobs) as pool:
        for name, atype, raw, err in pool.map(decompress_one, jobs_list):
            if raw is None:
                failed += 1
                report.append('  FAILED %s: %s' % (name, err))
                continue
            # Keyed by (name, TYPE). Hashing by name alone reported a
            # collision for every asset that exists as both a texture and a
            # material, which MX_tire has several of and which is not a defect.
            key = (name, atype)
            digest = hashlib.sha1(raw).hexdigest()
            if key in digests:
                if digests[key] != digest:
                    report.append('  DUPLICATE DIFFERS %s (%s): the asset name '
                                  'is not a key' % (name, atype))
                dupes[0] += 1
                continue  # already written; nothing further to do
            digests[key] = digest

            if atype == 'script':
                raw = strip_script_prefix(raw, report)
            base = os.path.join(out_dir, safe_name(name))
            with open(base + EXT.get(atype, '.' + (atype or 'bin')), 'wb') as f:
                f.write(raw)
            written += 1

            # A .shader is a JHM container, not BXML: its constant tables are
            # written beside it as .txt by shader_reader.
            if want_xml and atype == 'shader':
                try:
                    path = base + EXT.get(atype, '.shader')
                    _b, tables = read_shader(path)
                    if tables:
                        text = render_shader(path, tables, '')
                        with open(base + '.txt', 'w', encoding='utf-8') as f:
                            f.write(os.linesep.join(text) + os.linesep)
                except Exception as exc:  # noqa: BLE001
                    report.append('  shader read failed %s: %s' % (name, exc))

            if want_xml and raw[:4] == BXML_MAGIC:
                try:
                    text = '\n'.join(decode_bxml_bytes(raw).to_xml())
                    with open(base + '.xml', 'w', encoding='utf-8') as f:
                        f.write(text + '\n')
                except Exception as exc:  # noqa: BLE001
                    report.append('  BXML decode failed %s: %s' % (name, exc))

    by_type = collections.Counter(j[2] for j in jobs_list)
    report.insert(0, '  %d written, %d failed, %d distinct%s  (%s)'
                  % (written, failed, len(digests),
                     ', %d repeats verified identical' % dupes[0]
                     if dupes[0] else '',
                     ', '.join('%s=%d' % kv for kv in by_type.most_common())))
    return stem, report, written, failed


def main():
    ap = argparse.ArgumentParser(
        description='Extract assets from .xenon.package files to disk.')
    ap.add_argument('target',
                    help='a .xenon.package, or a directory holding them')
    ap.add_argument('--out', default=os.path.join('out', 'extracted'),
                    help='output root (default out/extracted)')
    ap.add_argument('--filter', default='*',
                    help='glob over asset names (default all)')
    ap.add_argument('--types', default='',
                    help='comma-separated asset types to keep (default all)')
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 4,
                    help='parallel decompressors (default: CPU count)')
    ap.add_argument('--no-xml', action='store_true',
                    help='skip decoding BXML assets to .xml')
    ap.add_argument('--verify-dupes', action='store_true',
                    help='decompress every repeat and check it is identical '
                         '(slower; the default trusts them)')
    args = ap.parse_args()

    if os.path.isdir(args.target):
        packages = sorted(os.path.join(args.target, f)
                          for f in os.listdir(args.target)
                          if f.endswith('.xenon.package'))
    else:
        packages = [args.target]
    if not packages:
        print('no .xenon.package files under %s' % args.target)
        return 1

    types = set(t.strip() for t in args.types.split(',') if t.strip())
    total_w = total_f = 0
    for pkg in packages:
        stem, report, written, failed = extract_package(
            pkg, args.out, args.filter, types, args.jobs, not args.no_xml,
            args.verify_dupes)
        total_w += written
        total_f += failed
        print('# %s' % stem)
        for line in report:
            print(line)

    print('')
    print('%d packages, %d assets written, %d failed -> %s'
          % (len(packages), total_w, total_f, args.out))
    return 1 if total_f else 0


if __name__ == '__main__':
    sys.exit(main())
