"""Make .shader assets readable: entry points, targets, and CONSTANT REGISTERS.

A `.shader` asset is a `JHM\\0` container: a small compressed BXML header
(`<ShaderHeaderMetaData><ShaderHeader Version="1"/>`), then a big-endian table
of named entry points, each carrying a shader token stream and a D3DX constant
table.

WHY THIS IS WORTH HAVING. The constant table gives **name -> register index**
for every shader constant in the game, statically. That is the mapping the
renderer work has been inferring from behaviour: `xe_c[85]`, `c138`, `c85.w`
are register numbers with no names attached at runtime, and
[[shader-handles-are-not-stable]] rules out carrying names across runs by
handle. Here they are, in the asset, next to the shader they belong to:

    c8    x1  float4   gCameraPos
    c4    x4  float4   gViewProjection
    c64   x4  float4   gWorld

FINDING THE TABLE. It is a D3DXSHADER_CONSTANTTABLE -- 7 big-endian dwords:
Size, Creator, Version, Constants, ConstantInfo, Flags, Target -- followed by
20-byte records of (NameOffset, RegisterSet, RegisterIndex, RegisterCount,
Reserved, TypeInfo, DefaultValue). All offsets are relative to the table base.

But it is NOT wrapped in the usual 'CTAB' fourcc, and searching for one finds
nothing. What locates it instead is the Version field: a shader version token
(0xFFFE0300 for vs_3_0, 0xFFFF0300 for ps_3_0) sits at base+8 by definition of
the struct. So this scans for version tokens and treats each as a candidate
base of -8, then ACCEPTS it only if the rest of the header is self-consistent:
Size 28, ConstantInfo 28, a sane constant count, and Creator/Target offsets
that land on printable strings inside the asset.

That check is the whole reason to trust the output. A version token also
appears inside the token stream itself, and the first cut of this analysis
mistook one for a record; requiring the surrounding header to validate rejects
those without needing to know where the token stream ends.

Verified end to end on Template_MX_Wheel_Fnl: Creator "2.0.20209.3", Target
"vs_3_0", 6 constants whose name offsets resolve to gCameraPos, gDistanceFog,
gDistanceFogColorDelta, gDistanceFogColorStart, gViewProjection and gWorld --
all six matching the strings independently found in the file.

Usage:
    python tools/shader_reader.py <file.shader>
    python tools/shader_reader.py <dir-of-extracted-assets>
    python tools/shader_reader.py <dir> --grep gBoneMatrix
"""

import argparse
import os
import struct
import sys

VERSION_TOKENS = {0xFFFE0300: 'vs_3_0', 0xFFFF0300: 'ps_3_0',
                  0xFFFE0200: 'vs_2_0', 0xFFFF0200: 'ps_2_0'}

REGISTER_SET = {0: 'bool', 1: 'int4', 2: 'float4', 3: 'sampler'}

# D3DXPARAMETER_TYPE, only the ones that turn up here.
# D3DXPARAMETER_TYPE is ZERO-based. Writing it one-based labelled every float4
# constant "int" and every plain sampler "sampler1D" -- wrong in a way that
# reads as real information, which is worse than a blank column.
PARAM_TYPE = {0: 'void', 1: 'bool', 2: 'int', 3: 'float', 4: 'string',
              5: 'texture', 6: 'texture1D', 7: 'texture2D', 8: 'texture3D',
              9: 'textureCube', 10: 'sampler', 11: 'sampler1D',
              12: 'sampler2D', 13: 'sampler3D', 14: 'samplerCUBE'}


def cstr(buf, off, limit=128):
    if not 0 <= off < len(buf):
        return None
    end = buf.find(b'\x00', off, off + limit)
    if end < 0:
        return None
    try:
        return buf[off:end].decode('ascii')
    except UnicodeDecodeError:
        return None


def printable(s):
    return bool(s) and all(32 <= ord(c) < 127 for c in s)


def find_constant_tables(buf):
    """Every self-consistent D3DXSHADER_CONSTANTTABLE in the buffer."""
    out = []
    for i in range(0, len(buf) - 28):
        tok = struct.unpack('>I', buf[i:i + 4])[0]
        if tok not in VERSION_TOKENS:
            continue
        base = i - 8
        if base < 0:
            continue
        size, creator, version, count, info, flags, target = struct.unpack(
            '>7I', buf[base:base + 28])
        # The self-consistency gate. Everything here is a fact about the struct
        # rather than a guess about the container, which is what makes a hit
        # trustworthy without knowing the surrounding layout.
        # A shader with NO constants is legal, and its table says so with
        # count == 0 AND ConstantInfo == 0 -- there is nothing for the offset to
        # point at. Demanding ConstantInfo == 28 unconditionally silently
        # dropped NormalPixelShader from Template_MX_Wheel_Fnl while the other
        # three entry points decoded perfectly, which is exactly the shape of
        # bug that gets shipped: the output looked complete.
        if size != 28 or count >= 256:
            continue
        if info != (28 if count else 0):
            continue
        if base + info + count * 20 > len(buf):
            continue
        creator_s = cstr(buf, base + creator)
        target_s = cstr(buf, base + target)
        if not printable(creator_s) or not printable(target_s):
            continue

        consts = []
        ok = True
        for k in range(count):
            o = base + info + k * 20
            noff, = struct.unpack('>I', buf[o:o + 4])
            rset, ridx, rcnt, _resv = struct.unpack('>4H', buf[o + 4:o + 12])
            toff, _dflt = struct.unpack('>2I', buf[o + 12:o + 20])
            name = cstr(buf, base + noff)
            if not printable(name):
                ok = False
                break
            ptype = ''
            if 0 < base + toff + 4 <= len(buf) - 4:
                _cls, pt = struct.unpack('>2H', buf[base + toff:base + toff + 4])
                ptype = PARAM_TYPE.get(pt, '')
            consts.append({'name': name, 'set': REGISTER_SET.get(rset, rset),
                           'reg': ridx, 'count': rcnt, 'type': ptype})
        if not ok:
            continue
        out.append({'base': base, 'target': target_s, 'creator': creator_s,
                    'profile': VERSION_TOKENS[tok], 'flags': flags,
                    'constants': consts})
    return out


def find_entry_names(buf):
    """(offset, name) for the length-prefixed entry-point names.

    A big-endian u32 length followed by exactly that many bytes of printable
    ASCII ending in NUL. Loose enough to find them without knowing the table
    layout, strict enough that ordinary data does not qualify.
    """
    out = []
    for i in range(0, len(buf) - 8):
        n, = struct.unpack('>I', buf[i:i + 4])
        if not 4 <= n <= 64 or i + 4 + n > len(buf):
            continue
        blob = buf[i + 4:i + 4 + n]
        if blob[-1:] != b'\x00':
            continue
        body = blob[:-1]
        if not body or not all(32 <= c < 127 for c in body):
            continue
        try:
            out.append((i + 4, body.decode('ascii')))
        except UnicodeDecodeError:
            pass
    return out


def read_shader(path):
    with open(path, 'rb') as f:
        buf = f.read()
    tables = find_constant_tables(buf)
    names = find_entry_names(buf)
    # Attribute each table to the nearest entry name BEFORE it. Names and their
    # tables appear in the same order, so "nearest preceding" is the whole rule;
    # a table with no name before it is reported as such rather than guessed.
    for t in tables:
        prev = [nm for off, nm in names if off < t['base']]
        t['entry'] = prev[-1] if prev else '<unnamed>'
    return buf, tables


def render(path, tables, grep):
    lines = []
    shown = 0
    for t in tables:
        if grep and not any(grep.lower() in c['name'].lower()
                            for c in t['constants']):
            continue
        shown += 1
        lines.append('  %s  [%s, %s, %d constants]'
                     % (t['entry'], t['profile'], t['creator'],
                        len(t['constants'])))
        for c in sorted(t['constants'], key=lambda x: (x['set'], x['reg'])):
            if grep and grep.lower() not in c['name'].lower():
                continue
            reg = ('s%d' if c['set'] == 'sampler' else 'c%d') % c['reg']
            lines.append('     %-6s x%-3d %-8s %-10s %s'
                         % (reg, c['count'], c['set'], c['type'], c['name']))
    if shown:
        lines.insert(0, '# %s' % os.path.basename(path))
    return lines


def main():
    ap = argparse.ArgumentParser(
        description='Decode .shader assets: entry points and constant registers')
    ap.add_argument('target', help='a .shader file, or a directory of them')
    ap.add_argument('--grep', default='',
                    help='only show constants whose name contains this')
    ap.add_argument('--out', default='', help='write to this file as well')
    args = ap.parse_args()

    paths = []
    if os.path.isdir(args.target):
        for root, _dirs, files in os.walk(args.target):
            paths += [os.path.join(root, f) for f in sorted(files)
                      if f.endswith('.shader')]
    else:
        paths = [args.target]

    all_lines = []
    tables_total = consts_total = empty = 0
    for p in sorted(paths):
        try:
            _buf, tables = read_shader(p)
        except Exception as exc:  # noqa: BLE001
            all_lines.append('# %s -- FAILED: %s' % (os.path.basename(p), exc))
            continue
        if not tables:
            # Counted, because "no constant table found" and "this file was
            # never read" must not look the same.
            empty += 1
            continue
        tables_total += len(tables)
        consts_total += sum(len(t['constants']) for t in tables)
        all_lines += render(p, tables, args.grep)

    print('\n'.join(all_lines))
    print('')
    print('%d files, %d constant tables, %d constants, %d files with none'
          % (len(paths), tables_total, consts_total, empty))
    if args.out:
        with open(args.out, 'w', encoding='utf-8') as f:
            f.write('\n'.join(all_lines) + '\n')
        print('-> %s' % args.out)
    return 0


if __name__ == '__main__':
    sys.exit(main())
