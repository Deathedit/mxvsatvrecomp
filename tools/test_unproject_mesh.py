"""Prove tools/unproject_mesh.py recovers world positions, by ROUND TRIP.

An unproven measurement tool is worse than no tool: it produces numbers that
look authoritative. This one recovers world positions from clip-space vertices
by solving `clip.xyz = M . (world, 1)` per vertex, and every conclusion drawn
from it -- "the light volume does / does not contain this point" -- rests on
that solve being right.

It needs no capture to check. Generate KNOWN world points, project them with a
known matrix, drop w exactly as RenderDoc's vs-out export does, and require the
tool to give the original points back. The fixture is the real light volume
measured in menu.rdc: a sphere of radius 5 at (1.759, 99.441, 0.564), which is
gWorld = xe_c[64..67] from event 8950, and 117 vertices because that is what
DL_PointSphere.surface holds.

THE MUTATION CASE IS THE POINT. Six of these assertions pass on a tool that is
merely self-consistent. The last two feed the TRANSPOSED matrix and require the
answers to go WRONG -- if a transposed matrix recovers the same positions, the
solve is not reading the matrix at all and every other assertion here is
vacuous. [[prove-the-test-can-fail]].

Usage, from the repo root:

    python tools/test_unproject_mesh.py
"""

import json
import math
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import unproject_mesh as um  # noqa: E402

# gWorld from menu.rdc event 8950: uniform scale 5 at this centre.
CENTRE = (1.759, 99.441, 0.564)
RADIUS = 5.0
NVERTS = 117  # DL_PointSphere.surface

# An arbitrary but non-degenerate transform. Deliberately NOT symmetric and not
# axis-aligned: a symmetric matrix would survive the transpose mutation below
# and quietly make that check vacuous.
M = [[1.2, 0.3, -0.5, 4.0],
     [0.1, 1.5, 0.2, -3.0],
     [0.0, 0.2, 1.1, 2.0],
     [0.0, 0.0, 1.0, 0.0]]

fails = []


def chk(ok, what, detail=''):
    print('  %-4s %s%s' % ('ok' if ok else 'FAIL', what,
                           '' if ok else '  <- ' + detail))
    if not ok:
        fails.append(what)


def sphere_points(n):
    """Fibonacci sphere: even coverage, deterministic, no numpy."""
    pts = []
    ga = math.pi * (3.0 - math.sqrt(5.0))
    for i in range(n):
        y = 1.0 - (2.0 * i + 1.0) / n
        r = math.sqrt(max(0.0, 1.0 - y * y))
        a = ga * i
        pts.append((CENTRE[0] + RADIUS * math.cos(a) * r,
                    CENTRE[1] + RADIUS * y,
                    CENTRE[2] + RADIUS * math.sin(a) * r))
    return pts


def project(world, rows):
    """clip.xyz = M . (world, 1), w DROPPED -- what vs-out actually gives."""
    return [sum(rows[r][c] * world[c] for c in range(3)) + rows[r][3]
            for r in range(3)]


def main():
    world = sphere_points(NVERTS)
    clip = [project(w, M) for w in world]

    out = tempfile.mkdtemp(prefix='unproj')
    mesh_path = os.path.join(out, 'mesh.json')
    mat_path = os.path.join(out, 'mat.json')
    with open(mesh_path, 'w', encoding='utf-8') as f:
        json.dump([{'x': c[0], 'y': c[1], 'z': c[2]} for c in clip], f)
    with open(mat_path, 'w', encoding='utf-8') as f:
        json.dump(M, f)

    # The loaders are part of what is being tested: a tool that solves
    # correctly but mis-reads RenderDoc's JSON is still wrong in use.
    rows = um.load_matrix(mat_path, False)
    got_clip = um.load_clip(mesh_path)
    chk(len(got_clip) == NVERTS, 'loader reads every vertex',
        'got %d of %d' % (len(got_clip), NVERTS))

    rec = um.unproject(got_clip, rows)
    chk(len(rec) == NVERTS, 'solver returns every vertex',
        'got %d' % len(rec))

    err = max(math.dist(a, b) for a, b in zip(world, rec))
    chk(err < 1e-6, 'ROUND TRIP recovers the original points',
        'max error %.3g' % err)

    # CENTRE AND RADIUS ARE APPROXIMATE, and not because the solve is.
    #
    # The tool reports the CENTROID OF THE VERTICES, not the volume's true
    # centre, and the two differ on any coarse mesh: 117 points on a sphere put
    # the centroid ~0.004 off a radius of 5, which is 0.07%. That is mesh
    # discretisation, and it does not shrink with a better solver. The ROUND
    # TRIP assertion above is the one that tests the arithmetic, and it holds to
    # 1e-6; these two only confirm the summary is a fair description of the
    # cloud. Tolerance set to the discretisation error, not tighter -- a
    # tolerance a tool cannot meet is a test that has to be ignored, and an
    # ignored test is worse than none.
    ctr = [sum(p[i] for p in rec) / len(rec) for i in range(3)]
    cerr = math.dist(ctr, CENTRE)
    chk(cerr < 0.01, 'centroid is within discretisation of the true centre',
        '(%.4f, %.4f, %.4f), off by %.3g' % (ctr[0], ctr[1], ctr[2], cerr))

    d = [math.dist(p, ctr) for p in rec]
    chk(abs(max(d) - RADIUS) < 0.01, 'radius matches gWorld scale',
        'max %.5f want %.1f' % (max(d), RADIUS))

    # The tool's own self-check, which the docstring tells users to read.
    sph = min(d) / max(d)
    chk(sph > 0.9, 'sphericity reports SPHERE for a sphere', '%.4f' % sph)

    # Containment, the question the tool exists to answer. 0.271 of the radius
    # is the bike's measured position inside this volume in menu.rdc.
    near = tuple(CENTRE[i] + (RADIUS * 0.271 if i == 0 else 0) for i in range(3))
    far = tuple(CENTRE[i] + (RADIUS * 3.0 if i == 0 else 0) for i in range(3))
    chk(math.dist(near, ctr) <= max(d), 'point at 27% of radius reads INSIDE')
    chk(math.dist(far, ctr) > max(d), 'point at 3x radius reads OUTSIDE')

    # MUTATION. Feed the transposed matrix; the recovery MUST break. If this
    # passes, the solve is ignoring the matrix and nothing above means anything.
    trows = um.load_matrix(mat_path, True)
    trec = um.unproject(got_clip, trows)
    terr = max(math.dist(a, b) for a, b in zip(world, trec)) if trec else 1e9
    chk(terr > 1.0, 'MUTATION: a transposed matrix gives WRONG positions',
        'max error only %.3g' % terr)
    tctr = ([sum(p[i] for p in trec) / len(trec) for i in range(3)]
            if trec else [0, 0, 0])
    chk(math.dist(tctr, CENTRE) > 1.0,
        'MUTATION: transposed centre is not the real centre')

    print('')
    if fails:
        print('FAILED (%d): %s' % (len(fails), '; '.join(fails)))
    else:
        print('all passed')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
