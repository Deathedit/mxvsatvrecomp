"""Locate and disassemble the SHADER CODE inside .shader assets.

WHERE THE GAME KEEPS ITS SHADERS -- the question this file answers. Not in the
XEX ([[shaders-are-not-in-the-xex]] is right about that, and static scanning of
the XEX really is a dead end because it is LZX compressed). They are in the
`.shader` assets inside the `.xenon.package` files, as **Xenos microcode**, and
they can be disassembled statically WITH THEIR NAMES -- which the runtime
GUEST MICROCODE dumps cannot give, since guest shader handles are addresses that
vary per run.

THE CONTAINER, per entry point:

    0x102A1101 / 0x102A1100     magic, low byte 1 = vertex, 0 = pixel
    9 x u32                     section offsets, RELATIVE TO header+4
      slot 3 -> always 36       the D3DXSHADER_CONSTANTTABLE (see shader_reader)
      slot 0 -> literal block   float constants, THEN the microcode
    ...
    <literals><microcode>

THE MICROCODE START IS NOT AT A FIXED OFFSET. It follows the literal block, and
that block's size varies with how many constants the shader defines: +0x3C
validated 4/4 entry points of Template_MX_Wheel_Fnl and 9/12 of Water_Ocean, and
the three failures were "exec target outside blob" -- a wrong START, misparsed,
not a wrong end. The 60 bytes at slot 0 are floats (1.0, 0.5, 65280.0), not a
header with a length in it.

So this does not guess the size. It steps forward from slot 0 a dword at a time
and hands each candidate to the real decoder, taking the first that DECODES --
control flow that terminates properly, exec targets in range. The decoder is the
oracle, which is the only reason a scan like this can be trusted; it is the same
validation that `xenos_shader_disasm.py --scan-file` applies, aimed at a known
starting point instead of at the whole file.

Usage:
    python tools/shader_code.py out/shaders/FR_Dunes/Water_Ocean.shader
    python tools/shader_code.py out/shaders --summary
    python tools/shader_code.py <file> --entry SkinnedVertexShader
"""

import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from shader_reader import find_constant_tables, find_entry_names  # noqa: E402
from xenos_shader_disasm import decode_shader, disassemble  # noqa: E402

ENTRY_MAGIC = b'\x10\x2a\x11'

# How far past slot 0 the literal block can run before we give up. The largest
# seen is well under this; it is a bound on wasted work, not a claim.
MAX_LITERAL_DWORDS = 128


def entry_headers(buf):
    """Offsets of every entry-point header, in order."""
    return [i for i in range(len(buf) - 40)
            if buf[i:i + 3] == ENTRY_MAGIC and buf[i + 3] in (0, 1)]


def to_dwords(buf):
    n = len(buf) // 4
    return list(struct.unpack('>%dI' % n, buf[:n * 4])) if n else []


def find_microcode(buf, start, end):
    """First offset at or after `start` whose dwords decode as a shader.

    Returns (offset, Shader) or (None, None). The decoder raises on anything
    structurally wrong, so "it decoded" is the acceptance test -- no heuristic
    about what microcode looks like enters into it.
    """
    for k in range(MAX_LITERAL_DWORDS):
        off = start + k * 4
        if off >= end:
            break
        dwords = to_dwords(buf[off:end])
        if not dwords:
            break
        try:
            return off, decode_shader(dwords)
        except Exception:  # noqa: BLE001 - any failure means "not here"
            continue
    return None, None


def read_entries(path):
    with open(path, 'rb') as f:
        buf = f.read()
    heads = entry_headers(buf)
    if not heads:
        return buf, []
    bounds = heads[1:] + [len(buf)]
    names = find_entry_names(buf)
    tables = {t['base']: t for t in find_constant_tables(buf)}

    out = []
    for h, end in zip(heads, bounds):
        magic = buf[h + 3]
        slot0, = struct.unpack('>I', buf[h + 4:h + 8])
        # slot 3 is always 36 and lands on the constant table; used here to pick
        # up the entry's profile and constants without re-deriving them.
        ctab = tables.get(h + 4 + 36)
        prev = [nm for off, nm in names if off < h]
        code_off, shader = find_microcode(buf, h + 4 + slot0, end)
        out.append({
            'header': h,
            'kind': 'vertex' if magic == 1 else 'pixel',
            'name': prev[-1] if prev else '<unnamed>',
            'ctab': ctab,
            'code_off': code_off,
            'code_end': end,
            'shader': shader,
        })
    return buf, out


def main():
    ap = argparse.ArgumentParser(
        description='Locate and disassemble microcode inside .shader assets')
    ap.add_argument('target', help='a .shader file, or a directory of them')
    ap.add_argument('--entry', default='', help='only this entry point')
    ap.add_argument('--summary', action='store_true',
                    help='counts only: how many entry points yielded microcode')
    ap.add_argument('--out', default='', help='also write output here')
    args = ap.parse_args()

    paths = []
    if os.path.isdir(args.target):
        for root, _dirs, files in os.walk(args.target):
            paths += [os.path.join(root, f) for f in sorted(files)
                      if f.endswith('.shader')]
    else:
        paths = [args.target]

    lines = []
    found = missing = 0
    for p in sorted(paths):
        try:
            _buf, entries = read_entries(p)
        except Exception as exc:  # noqa: BLE001
            lines.append('# %s -- FAILED: %s' % (os.path.basename(p), exc))
            continue
        shown = []
        for e in entries:
            if args.entry and args.entry != e['name']:
                continue
            if e['shader'] is None:
                missing += 1
                shown.append('  %s [%s] -- NO MICROCODE FOUND' % (e['name'],
                                                                  e['kind']))
                continue
            found += 1
            if args.summary:
                continue
            head = '  %s [%s, code at 0x%X]' % (e['name'], e['kind'],
                                                e['code_off'])
            shown.append(head)
            if e['ctab']:
                for c in sorted(e['ctab']['constants'],
                                key=lambda x: (x['set'], x['reg'])):
                    reg = ('s%d' if c['set'] == 'sampler' else 'c%d') % c['reg']
                    shown.append('     ; %-6s x%-3d %s' % (reg, c['count'],
                                                           c['name']))
            for ln in disassemble(e['shader']):
                shown.append('     ' + ln)
        if shown:
            lines.append('# %s' % os.path.basename(p))
            lines += shown

    text = '\n'.join(lines)
    if not args.summary:
        print(text)
    print('')
    print('%d files, %d entry points with microcode, %d without'
          % (len(paths), found, missing))
    if args.out:
        with open(args.out, 'w', encoding='utf-8') as f:
            f.write(text + '\n')
        print('-> %s' % args.out)
    return 0


if __name__ == '__main__':
    sys.exit(main())
