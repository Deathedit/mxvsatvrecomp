#!/usr/bin/env python3
"""
MX vs ATV Alive .tree reader (SpeedTree 5 asset).

Container:
    0x00  "TREE"            big-endian header
    0x04  u32 version (2)
    0x08  u32 ...
    0x00c  4 x 488-byte geometry group descriptors (0x00c + 4*488 == 0x7ac)
    0x7ac "BXML"            little-endian property chunk (UNCOMPRESSED here,
                            unlike the zlib .bxml containers tools/bxml_decoder
                            handles)
    tail  geometry: one vertex+index block per non-empty group, IN SLOT ORDER

Geometry group descriptor (at 0x00c + 488*slot):
    +0  index bytes u32   +4 vertex bytes u32   +12 stride u16, flags u16
    +16 LOD0 draw count u32   +20 0x00010000

Slot -> group -> topology, all four confirmed over the 129 shipped assets
(sizes reconcile exactly and every index is in range on 129/129):

    0 branch  stride 36  triangle STRIP, 0xFFFF strip cuts
    1 frond   stride 36  triangle list
    2 3dleaf  stride 36  triangle list
    3 leaf    stride 32  NO index buffer, 4 verts per card (count %% 4 == 0)

The +16 count is the FIRST LOD's draw count, not the buffer size: it equals the
whole buffer on every numLods==1 group (133/133) and is smaller when the group
carries more LODs, which are concatenated into the one block.

Vertex, stride 36 (half floats, big-endian):
    [0..2] position xyz   [4..6] LOD-morph delta xyz (zero when numLods==1,
    non-zero otherwise -- this is what g_TreeLerps blends)   [8..9] UV
    [12..17] packed normal/tangent, not decoded

Vertex, stride 32 (half floats): [0..3] card ANCHOR xyzw (= bbox center),
    [4..5] corner offset x/y in world units, [8..9] UV, [10..11] packed.
    LeafVertexShader is the only entry point declaring gViewInverse (c0)
    because it expands these cards to face the camera.

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

Usage:  python tools/tree_reader.py <file.tree|dir> [--raw] [--obj DIR]
"""
import struct, sys, os, glob

GEOM_SLOTS = [(0x00c, 'branch', 'strip'), (0x1f4, 'frond', 'list'),
              (0x3dc, '3dleaf', 'list'), (0x5c4, 'leaf', 'quads')]
RESTART = 0xFFFF


def half(u):
    """IEEE 754 binary16 -> float."""
    s, e, m = (u >> 15) & 1, (u >> 10) & 0x1F, u & 0x3FF
    if e == 0:
        v = m * 2.0 ** -24
    elif e == 31:
        return float('nan')
    else:
        v = (1024 + m) * 2.0 ** (e - 25)
    return -v if s else v


def geometry(blob):
    """Yield one dict per non-empty geometry block, in tail order."""
    off = 0x7ac + struct.unpack_from('>I', blob, 8)[0]
    for base, kind, topo in GEOM_SLOTS:
        ib, vb = struct.unpack_from('>2I', blob, base)
        stride, flags = struct.unpack_from('>2H', blob, base + 12)
        count = struct.unpack_from('>I', blob, base + 16)[0]
        if not vb or not stride or vb % stride or ib % 2:
            continue
        nv = vb // stride
        verts = [[half(h) for h in
                  struct.unpack_from('>%dH' % (stride // 2), blob, off + i * stride)]
                 for i in range(nv)]
        idx = list(struct.unpack_from('>%dH' % (ib // 2), blob, off + vb)) if ib else []
        yield dict(slot=base, kind=kind, topo=topo, stride=stride, flags=flags,
                   lod0_count=count, verts=verts, indices=idx, offset=off)
        off += vb + ib


def triangles(b):
    """Index triples for one block, strip cuts and quad cards resolved."""
    idx, out = b['indices'], []
    if b['topo'] == 'quads':
        for q in range(0, len(b['verts']), 4):
            out += [(q, q + 1, q + 2), (q, q + 2, q + 3)]
    elif b['topo'] == 'list':
        out = [tuple(idx[i:i + 3]) for i in range(0, len(idx) - 2, 3)]
    else:
        run = []
        for x in idx + [RESTART]:
            if x == RESTART:
                for i in range(len(run) - 2):
                    out.append((run[i], run[i + 2], run[i + 1]) if i & 1
                               else (run[i], run[i + 1], run[i + 2]))
                run = []
            else:
                run.append(x)
    return out


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


def read_tree(path, raw=False, obj_dir=None):
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
    acc = 0
    for g in geometry(blob):
        xs = [v[0] for v in g['verts']]
        ys = [v[1] for v in g['verts']]
        zs = [v[2] for v in g['verts']]
        acc += len(g['verts']) * g['stride'] + len(g['indices']) * 2
        print(f"    {g['kind']:7} stride {g['stride']} {g['topo']:5} "
              f"{len(g['verts']):5} verts {len(g['indices']):6} idx "
              f"{len(triangles(g)):5} tris  lod0 {g['lod0_count']}")
        print(f'            x[{min(xs):8.2f},{max(xs):8.2f}] '
              f'y[{min(ys):8.2f},{max(ys):8.2f}] z[{min(zs):8.2f},{max(zs):8.2f}]')
    if acc != len(blob) - tail:
        print(f'    ! {len(blob) - tail - acc} tail bytes unaccounted for')
    if obj_dir:
        write_obj(path, blob, obj_dir)


def write_obj(path, blob, obj_dir):
    """Dump each block as Wavefront OBJ. Leaf cards are emitted anchor+corner
    in the XY plane -- the runtime orients them at the camera, so their shape
    is right and their facing is not."""
    os.makedirs(obj_dir, exist_ok=True)
    stem = os.path.splitext(os.path.basename(path))[0]
    for g in geometry(blob):
        out = os.path.join(obj_dir, stem + '.' + g['kind'] + '.obj')
        with open(out, 'w') as f:
            p_ = lambda line: print(line, file=f)
            p_(f"# {stem} {g['kind']} stride {g['stride']} {g['topo']}")
            for v in g['verts']:
                if g['stride'] == 32:
                    p_(f'v {v[0]+v[4]:.5f} {v[1]+v[5]:.5f} {v[2]:.5f}')
                else:
                    p_(f'v {v[0]:.5f} {v[1]:.5f} {v[2]:.5f}')
            for v in g['verts']:
                p_(f'vt {v[8]:.5f} {v[9]:.5f}')
            for a, b_, c in triangles(g):
                p_(f'f {a+1}/{a+1} {b_+1}/{b_+1} {c+1}/{c+1}')
        print(f'    -> {out}')



if __name__ == '__main__':
    argv = sys.argv[1:]
    obj_dir = None
    if '--obj' in argv:
        i = argv.index('--obj')
        obj_dir = argv[i + 1]
        del argv[i:i + 2]
    args = [a for a in argv if not a.startswith('--')]
    raw = '--raw' in sys.argv
    targets = []
    for a in args:
        targets += glob.glob(os.path.join(a, '*.tree')) if os.path.isdir(a) else [a]
    for t in targets:
        read_tree(t, raw, obj_dir)
        print()
