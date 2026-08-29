"""Recover WORLD positions from a RenderDoc vs-out mesh, and test containment.

RenderDoc's `export_mesh(ev, format="json", stage="vs-out")` gives post-transform
vertices as CLIP-space xyz with **w dropped**. That looks unusable and is not:

    clip.xyz = M . (world, 1)

is three equations in three unknowns, so a 3x3 solve per vertex recovers world
position exactly. `M` is the first three ROWS of the matrix the vertex shader
applied -- gWorldViewProjection for object geometry, gWorld x gViewProjection
for a light volume.

Written for one question: **do the deferred light volumes actually cover the
geometry they are supposed to light?** [[menu-frame-graph]] records 16 of 17
light volumes contributing exactly zero at one pixel on the bike, and treats
that as a defect. It may not be one -- a light volume that passes depth and
stencil still outputs zero everywhere outside its own radius, which is most of
the screen. Deciding that needs the volumes' world extents and the shaded
point's world position in the same space, which is what this produces.

    python tools/unproject_mesh.py volume.json --matrix mat.json
    python tools/unproject_mesh.py volume.json --matrix mat.json --contains 12,34,56

THE MATRIX comes from `get_cbuffer_contents(ev, "vs", 0)`; pass its 16 floats
as JSON (a flat list, or 4 nested rows). Row-major as the shader applies it: if
the recovered box is nonsense, try --transpose before doubting anything else.

**IT VALIDATES ITSELF, and you should check that it did.** For a light volume
the recovered points must form a recognisable shape -- a sphere for a point
light (DL_PointSphere is 117 verts), a box for spot and bounce (19). The report
prints sphericity: the ratio of min to max vertex distance from the centre,
which is ~1.0 for a sphere and ~0.58 for a box. If a point light's volume does
not come back spherical, the matrix or its row order is wrong and no
containment answer from it means anything. That check is the reason to trust
the numbers; the same idea caught a wrong matrix in ground_truth_from_capture.

PROVEN, by `python tools/test_unproject_mesh.py` -- a round trip that projects
known world points with a known matrix, drops w as RenderDoc does, and requires
this to give them back. It holds to 1e-6, and the test's own mutation case
(feeding the TRANSPOSED matrix, which must produce wrong answers) is what stops
that from being a tautology. Run it before trusting a number out of here.

ONE APPROXIMATION, which the round trip is exact about but the SUMMARY is not:
`centre` is the CENTROID OF THE VERTICES, not the volume's true centre, and on
a coarse mesh they differ -- 117 points on a sphere put it ~0.004 off a radius
of 5, 0.07%. Fine for containment, wrong if you need the light's exact origin;
for that read gWorld (xe_c[64..67]) directly, which states it.
"""

import argparse
import json
import math
import sys


def solve3(a, b):
    """Gaussian elimination with partial pivoting. Returns None if singular."""
    m = [list(a[r]) + [b[r]] for r in range(3)]
    for col in range(3):
        piv = max(range(col, 3), key=lambda r: abs(m[r][col]))
        if abs(m[piv][col]) < 1e-12:
            return None
        m[col], m[piv] = m[piv], m[col]
        for r in range(3):
            if r == col:
                continue
            f = m[r][col] / m[col][col]
            for c in range(col, 4):
                m[r][c] -= f * m[col][c]
    return [m[r][3] / m[r][r] for r in range(3)]


def load_matrix(path, transpose):
    with open(path, encoding='utf-8') as f:
        raw = json.load(f)
    flat = []

    def flatten(x):
        if isinstance(x, list):
            for i in x:
                flatten(i)
        elif isinstance(x, (int, float)):
            flat.append(float(x))
        elif isinstance(x, dict):
            for k in ('value', 'values', 'data', 'floats'):
                if k in x:
                    flatten(x[k])
                    return
            for v in x.values():
                flatten(v)
    flatten(raw)
    if len(flat) < 16:
        raise ValueError('need 16 floats for the matrix, found %d' % len(flat))
    flat = flat[:16]
    rows = [flat[i * 4:(i + 1) * 4] for i in range(4)]
    if transpose:
        rows = [[rows[c][r] for c in range(4)] for r in range(4)]
    return rows


def load_clip(path):
    """Pull xyz per vertex out of RenderDoc's mesh JSON, whatever it nests in."""
    with open(path, encoding='utf-8') as f:
        raw = json.load(f)
    verts = raw
    for key in ('vertices', 'verts', 'data', 'positions'):
        if isinstance(verts, dict) and key in verts:
            verts = verts[key]
    out = []
    for v in verts:
        if isinstance(v, dict):
            for key in ('POSITION', 'SV_Position', 'position', 'pos'):
                if key in v:
                    v = v[key]
                    break
        if isinstance(v, dict):
            xyz = [v.get(k) for k in ('x', 'y', 'z')]
        elif isinstance(v, list) and len(v) >= 3:
            xyz = v[:3]
        else:
            continue
        if all(isinstance(c, (int, float)) for c in xyz):
            out.append([float(c) for c in xyz])
    return out


def unproject(clip_pts, rows):
    a = [rows[i][:3] for i in range(3)]
    world = []
    for c in clip_pts:
        w = solve3(a, [c[i] - rows[i][3] for i in range(3)])
        if w:
            world.append(w)
    return world


def report(world, contains):
    n = len(world)
    if not n:
        print('no vertices recovered -- matrix singular, or the JSON had no xyz')
        return 2
    ctr = [sum(p[i] for p in world) / n for i in range(3)]
    d = [math.dist(p, ctr) for p in world]
    lo = [min(p[i] for p in world) for i in range(3)]
    hi = [max(p[i] for p in world) for i in range(3)]
    rmin, rmax = min(d), max(d)
    print('vertices          %d' % n)
    print('centre            (%.3f, %.3f, %.3f)' % tuple(ctr))
    print('radius            min %.3f  max %.3f  mean %.3f'
          % (rmin, rmax, sum(d) / n))
    print('aabb              (%.2f %.2f %.2f) .. (%.2f %.2f %.2f)'
          % (lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]))
    print('extent            %.2f x %.2f x %.2f'
          % (hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]))
    # The self-check. Do not skip reading this line.
    sph = rmin / rmax if rmax else 0.0
    shape = ('SPHERE' if sph > 0.9 else
             'BOX' if 0.5 < sph < 0.75 else 'NEITHER -- suspect the matrix')
    print('sphericity        %.3f  -> %s' % (sph, shape))

    if contains is not None:
        dist = math.dist(contains, ctr)
        inside_box = all(lo[i] - 1e-4 <= contains[i] <= hi[i] + 1e-4
                         for i in range(3))
        print('')
        print('test point        (%.3f, %.3f, %.3f)' % tuple(contains))
        print('distance to ctr   %.3f   (volume max radius %.3f)' % (dist, rmax))
        print('INSIDE aabb       %s' % ('YES' if inside_box else 'NO'))
        print('INSIDE sphere     %s' % ('YES' if dist <= rmax else 'NO'))
        if not inside_box:
            print('-> this volume CANNOT light that point. Zero output here is'
                  ' correct behaviour, not a defect.')
    return 0


def main():
    ap = argparse.ArgumentParser(
        description='Recover world positions from a RenderDoc vs-out mesh')
    ap.add_argument('mesh', help='vs-out JSON from export_mesh')
    ap.add_argument('--matrix', required=True,
                    help='JSON with the 16 floats the VS applied')
    ap.add_argument('--transpose', action='store_true',
                    help='treat the matrix as column-major')
    ap.add_argument('--contains', default='',
                    help='world point "x,y,z" to test against this volume')
    args = ap.parse_args()

    rows = load_matrix(args.matrix, args.transpose)
    clip = load_clip(args.mesh)
    if not clip:
        print('no vertices in %s' % args.mesh)
        return 2
    pt = None
    if args.contains:
        pt = [float(v) for v in args.contains.replace(' ', '').split(',')]
        if len(pt) != 3:
            raise ValueError('--contains wants x,y,z')
    return report(unproject(clip, rows), pt)


if __name__ == '__main__':
    sys.exit(main())
