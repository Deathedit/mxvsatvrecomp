# Measuring the floating bike from a RenderDoc capture

The bike floats above the terrain. Answering "by how much, and against what
ground" needs the terrain's own height under the bike, from the same data the
GPU draws. This is how that was measured, and why the runtime probe that tried
to do the same thing could not.

**Result, from `1142.rdc`:** the two wheels' contact points sit **1.937** and
**1.828** world units above the terrain.

## The method

Everything below is one capture and about ten tool calls.

**1. Find a terrain draw and read its constants.** `get_cbuffer_contents(ev, vs, 0)`
gives the whole `xe_c` bank. A terrain draw is identifiable by `c204`
(`gMeshResolution`), which no other shader in the 5144-entry corpus declares.
For event 17229 in `1142.rdc`:

    c201 gVertexOffset    (-459, 1491.5)  extent 2048
    c204 gMeshResolution  1
    c220 gWaterModifiers  (-459, 1491.5)   -- the same origin, independently
    c8   gCameraPos       (-459.089, 616.225, 1491.661)
    c76..c79 gWorldViewProjection

The ring origin is the camera XZ snapped to the grid: the clipmap follows the
viewer. `c217 g_HFMapSize` reads `(2048, 2048, 1/2048, 1/2048)` here -- a real
map size, where a runtime read of the same register returned a colour, because
the ALU constant file is global and returns whatever shader wrote last.

**2. Export geometry and recover world positions.** `export_mesh(ev, json, vs-out)`
returns CLIP-space xyz with **w dropped**, which looks unusable and is not:

    clip.xyz = M . (world, 1)

is three equations in three unknowns, so a 3x3 solve per vertex recovers world
position exactly. `M` is the first three rows of `gWorldViewProjection`.

**This validates itself.** Block 17229 comes back as exactly 16x16 world units
centred on the camera XZ at 0.25 spacing -- geometry that was never fed to the
solver. If the matrix or the row order were wrong, that would not happen.

**3. Fit a plane, do not take the nearest vertex.** The terrain is dunes. On a
slope a vertex a quarter-unit away in XZ sits measurably above or below the
point under the bike, and that error is the size of the effect being measured.
Least squares over the 12 nearest vertices, evaluated at the bike's XZ. Report
the plane's rms: 0.0008-0.010 here, so the surface really is planar at this
scale and the number can be trusted.

## Identifying a wheel without any names

The bike is many draws and the shader cannot name them -- 846 shaders share the
generic static VS signature `{c4, c8, c14, c15, c26, c64}`. Geometry can:

    part     thickness  radius  % at rim  normal Y   verdict
    d18322   0.321      1.132   95%       0.00       tyre
    d18336   0.415      1.108   80%       0.00       tyre
    d18042   0.245      0.782   21%       0.04       thin disc, likely a rotor
    d18028   0.883      1.318    1%       0.29       solid part, not a wheel

A tyre is a thin disc whose vertices sit at near-constant radius: 80-95% within
15% of the outer radius, normal Y = 0 (upright), radius matching the game's own
`FrontTire_Radius` 1.17 / `RearTire_Radius` 1.04, and the disc centre exactly
one radius above its own lowest vertex. Four independent properties agreeing.

**Axle separation is 4.61 units, a motocross wheelbase** -- which confirms both
that these are one bike's two wheels and that one world unit is about one foot.

## What the measurement says

    wheel     axle                        contact  ground   gap
    d18322    (-445.84, 611.25, 1500.55)  610.134  608.197  1.937
    d18336    (-448.89, 611.27, 1504.01)  610.192  608.364  1.828

**The float is uniform.** The axles sit at 611.25 and 611.27 -- level to 0.02 --
and the 0.11 difference between the gaps is entirely explained by the terrain
being 0.167 lower under one wheel. The bike is displaced vertically as a RIGID
BODY.

That rules out the suspension: front and rear have different specs (rest 3.6 vs
4.2, travel 1.7 vs 1.2), so a suspension fault would appear as a front/rear
asymmetry, and there is none. It also fits the defect being intermittent, since
a geometry constant would not vary between runs.

Measuring a non-wheel part first gave 2.67 and 2.82 and briefly looked like it
superseded the older 1.9 +/- 0.3 pixel measurement. It did not; those were
bodywork above the wheels. **Identify the part before believing the number.**

## Why the runtime probe could not do this

Several days of probe iterations tried to read terrain height out of the draw's
vertex buffer. The capture settles why that was never going to work: the terrain
vertex shader binds **`xe_tex0` with a sampler** alongside `xe_vb`, so height is
SAMPLED FROM A TEXTURE in the vertex shader and is not in the vertex data at
all. `HFB_1tex` having no height sampler is real but it is a different terrain
variant from the one drawing the ground.

Two further things the capture gives for free that the probe had to infer, and
got wrong:

- **Which ring a draw belongs to.** The capture holds bound constants per draw.
  The probe tried to pair PM4 constant writes with draws by ordering; those are
  independent pipelines, so the pairing carried no information -- 5060 arms with
  4881 overwritten, and 143 of 179 claims landing on the wrong ring.
- **Which draw is terrain.** Identifiable directly, where the probe needed a
  geometric qualifier that was wrong in both directions before it was right.

The pattern worth remembering: each probe fix surfaced a NEW structural unknown
rather than converging. That is the signal that the instrument is wrong, not its
parameters.
