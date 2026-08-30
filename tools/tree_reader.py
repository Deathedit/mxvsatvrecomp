#!/usr/bin/env python3
"""
MX vs ATV Alive .tree reader (SpeedTree 5 asset).

Container:
    0x00  "TREE"            big-endian header
    0x04  u32 version (2)
    0x08  u32 ...
    ...   runtime blob (serialized SpeedTree structures, pointers nulled)
    0x7ac "BXML"            little-endian property chunk (UNCOMPRESSED here,
                            unlike the zlib .bxml containers tools/bxml_decoder
                            handles)
    tail  geometry: billboard / leaf-card vertices and triangle indices

BXML chunk header (7x u32 LE after the magic):
    [0] version         [1] string count   [2] string table bytes
    [3] value pool bytes[4] attribute count[5] node count  [6] reserved

[0] is 0x3ea in every asset, so it is a version and NOT a length -- the chunk
length is the TREE header's u32 at 0x08, and the geometry tail therefore starts
at 0x7ac + that.

Attribute record (12 B): name_idx u32, value u32, count u16, type u16
Node record      (32 B): name_idx u32, 0xFFFFFFFF, u32, u32,
                         first_child u32, n_child u32, first_attr u32, n_attr u32

Node fields are CHILD-first. Assuming attr-first yields a document that still
looks plausible (Root sprouts lowLodDistance); SpeedTree5 and BoundingBox are
the nodes that tell the two apart.

Usage:  python tools/tree_reader.py <file.tree|dir> [--raw]
"""
import struct, sys, os, glob

# value pool types
T_STRING = 1
T_BOOL   = 3
T_INT    = 4
T_FLOAT  = 5
T_VEC4   = 7
T_VEC3   = 10


class Bxml:
    def __init__(self, blob, off):
        (self.version, n_str, str_bytes, pool_bytes,
         self.n_attr, self.n_node, _) = struct.unpack_from('<7I', blob, off + 4)
        p = off + 32
        self.strings = blob[p:p + str_bytes].split(b'\0')
        self.strings = [s.decode('ascii', 'replace') for s in self.strings if s]
        p += str_bytes
        self.pool = blob[p:p + pool_bytes]
        p += pool_bytes
        self.attrs = [struct.unpack_from('<IIHH', blob, p + 12 * i)
                      for i in range(self.n_attr)]
        p += 12 * self.n_attr
        self.nodes = [struct.unpack_from('<8I', blob, p + 32 * i)
                      for i in range(self.n_node)]
        if len(self.strings) != n_str:
            print(f'  ! string count {len(self.strings)} != header {n_str}')

    def s(self, i):
        return self.strings[i] if i < len(self.strings) else f'<str {i}>'

    def value(self, val, count, ty):
        f = lambda o: struct.unpack_from('<f', self.pool, o)[0]
        i = lambda o: struct.unpack_from('<i', self.pool, o)[0]
        if ty == T_STRING: return self.s(val)
        if ty == T_BOOL:   return bool(i(val))
        if ty == T_INT:    return i(val)
        if ty == T_FLOAT:  return f(val)
        if ty == T_VEC4:   return tuple(f(val + 4 * k) for k in range(4))
        if ty == T_VEC3:   return tuple(f(val + 4 * k) for k in range(3))
        return f'<type {ty} count {count} @{val}>'

    def dump(self, indent='  '):
        def walk(ni, depth):
            name, _, _, _, c0, nc, a0, na = self.nodes[ni]
            pad = indent * (depth + 1)
            props = []
            for a in range(a0, a0 + na):
                if a >= len(self.attrs):
                    continue
                nm, val, cnt, ty = self.attrs[a]
                v = self.value(val, cnt, ty)
                if isinstance(v, float):
                    v = f'{v:g}'
                elif isinstance(v, tuple):
                    v = '(' + ', '.join(f'{x:g}' for x in v) + ')'
                props.append(f'{self.s(nm)}={v}')
            print(f'{pad}<{self.s(name)}' +
                  (' ' + ' '.join(props) if props else '') + '>')
            for c in range(c0, c0 + nc):
                if c < len(self.nodes):
                    walk(c, depth + 1)
        walk(0, 0)


def read_tree(path, raw=False):
    blob = open(path, 'rb').read()
    if blob[:4] != b'TREE':
        print(f'{path}: not a TREE (got {blob[:4]!r})')
        return
    ver, f8, fc = struct.unpack_from('>3I', blob, 4)
    off = blob.find(b'BXML')
    print(f'=== {os.path.basename(path)}  {len(blob)} B  '
          f'version {ver}  hdr[8]=0x{f8:x} hdr[12]=0x{fc:x}  BXML@0x{off:x}')
    if off < 0:
        return
    b = Bxml(blob, off)
    print(f'  strings {len(b.strings)}  attrs {b.n_attr}  nodes {b.n_node}  '
          f'pool {len(b.pool)} B  bxml 0x{f8:x}  bxml_version 0x{b.version:x}')
    b.dump()
    tail = off + f8
    print(f'  geometry tail @0x{tail:x}  {len(blob) - tail} B')
    if raw:
        for i, s in enumerate(b.strings):
            print(f'    [{i:2}] {s}')
        for i, (nm, val, cnt, ty) in enumerate(b.attrs):
            print(f'    attr[{i:2}] {b.s(nm):20} val={val:<5} cnt={cnt} ty={ty}'
                  f'  -> {b.value(val, cnt, ty)}')
        for i, n in enumerate(b.nodes):
            print(f'    node[{i:2}] {b.s(n[0]):20} '
                  f'child {n[4]}+{n[5]}  attr {n[6]}+{n[7]}')


if __name__ == '__main__':
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    raw = '--raw' in sys.argv
    targets = []
    for a in args:
        targets += glob.glob(os.path.join(a, '*.tree')) if os.path.isdir(a) else [a]
    for t in targets:
        read_tree(t, raw)
        print()
