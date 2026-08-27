# Stencil: what is left, and the order to do it in

## Why this is worth doing

Run 1534, freeroam. Measured, not guessed:

```
STENCIL census — 487261 draws reached the read (0 could not);
  enable bit set 247157, of which 247157 are in an edram_mode that honours it;
  18 distinct configs
```

**50.7% of level draws use stencil and we honour none of it.** The 18 configs
are not noise; they are a coherent scheme:

| Role | Config | Draws |
|---|---|---|
| write 0   | `Always` / zpass `Replace`, ref 0   | 116,909 |
| write 255 | `Always` / zpass `Replace`, ref 255 | 14,914 |
| test >128 | `Greater`, ref 128                  | 27,786 |
| test != 0 | `NotEqual`, ref 0                   | ~3,500 |
| test == 1 | `Equal`, ref 1                      | 3,837 |
| shadow volumes | front zfail `IncrSat`, back fail+zpass `IncrSat` | 27,786 |

The game stamps a mask and then tests against it. **With no stencil buffer every
test passes**, so draws meant to be confined to a region paint everywhere. That
is a mechanism that can ERASE content, not merely add to it, which is why it is
worth trying against the missing-image symptoms.

## What is already done: verify, do not redo

The memory note `depth-format-change-killed-the-device` records the format
change as REVERTED. **That note is stale.** `d8e6170` ("gfx: give the game depth
surface a stencil plane", 2026-08-16) is an ancestor of HEAD, and today's
487k-draw freeroam run is on it with a healthy device.

- `kGameDepthFormat = D32_FLOAT_S8X24_UINT` (`d3d12_renderer.h:342`)
- `kGameDepthResourceFormat = R32G8X24_TYPELESS` (`:351`)
- `kGameDepthSrvFormat = R32_FLOAT_X8X24_TYPELESS` (`:353`)
- `kGameDepthClearFlags = DEPTH | STENCIL` (`:356`), and all four clear sites go
  through it (`d3d12_device.cpp:431`, `d3d12_game_frame.cpp:206/591/818/1256`)

The plane exists and is cleared to 0 alongside depth. The depth half is
deliberately unchanged, still 32-bit float, so that work could be judged on
"nothing moved". **The dangerous, device-killing half is behind us.**

## What is missing

Every one of these is absent, not partial:

- `StencilEnable` is never set true. The only mention in `src/gfx` is a log line
  at `d3d12_game.cpp:976`.
- No `OMSetStencilRef` call anywhere in the tree.
- No `StencilFunc`, `FrontFace`, `BackFace`, `StencilReadMask` or
  `StencilWriteMask` in any PSO.
- `graphics_system.cpp:458/460` reads **only bits 1 and 2** of `depth_control`.
  Bit 0 is STENCIL_ENABLE; bits 8-19 are the func and the three ops.
- `RB_STENCILREFMASK` (0x2201) is read by the census in `hooks_d3d9.cpp` but
  never reaches `DrawCall`.
- `DrawCall` carries `clear_depth` but **no `clear_stencil`**.

## Phase 0: name the stencil clear bit. DONE (`227e410`).

Nothing downstream is safe until this is answered: a test against a wrongly
cleared buffer makes geometry VANISH rather than degrade.

`hooks_d3d9_entry.cpp:1865` establishes that **D3DCLEAR_ZBUFFER is 0x10 on this
hardware, not the 0x2 a PC D3D9 header says**, and states plainly that bits 1..6
are still not named. Run 1445 saw seven flag values: `0xF, 0x1F, 0x30, 0x3F,
0x60`.

**ANSWER: `0x20` is `D3DCLEAR_STENCIL`.** Proven, not extrapolated.

Neither xenia-edge nor the SDK carries `D3DCLEAR`, and `D3DDevice_Clear` really
is unreachable through Hex-Rays. But the flag decoding is not in it: it is a
thin wrapper over `sub_8255B130` -> `sub_8255AAB0` -> `sub_8255A510`, and the
last two decompile cleanly.

`sub_8255AAB0` loops bits 0..3 over the four render targets at `device+12616`
and masks off `0xF0` after the first, so the depth-stencil half clears once:
**bits 0-3 are the MRT colour targets**, `0x10/0x20/0x40/0x80` are the
depth-stencil group.

`sub_8255A510` decides it:

```c
if ( (Flags & 0x10) != 0 )  v41 |= 1u;              // depth
if ( (Flags & 0x20) != 0 ) {
    v41 |= 4u;                                      // stencil
    *v44++ = 8461;                                  // 8461 == 0x210D
    *v44 = 0x00FF0000 | (Stencil & 0xFF);           // RB_STENCILREFMASK
}
```

**Passed.** Every observed flag value decodes: `0xF` colours, `0x1F`
colours+depth, `0x20` stencil alone, `0x30` depth+stencil, `0x3F` all three,
`0x60` stencil+EDRAM.

Carried through in `227e410`: `DrawCall` gained `clear_stencil_target` /
`clear_stencil`, `AddGameDepthClear` gained the plane pair, and the guest-driven
`ClearDepthStencilView` builds its flags from what the guest asked rather than
the unconditional `kGameDepthClearFlags` (which stays right for our own
first-use clears). EDRAM clears are excluded as the depth gate already excludes
them -- `0x60` carries `0x20` and is 16,139 of 24,000 calls -- and the exclusion
is counted, not silent.

## Phase 1: plumb the registers, change nothing

Carry through to `DrawCall` and log a census of what arrives. No PSO change, no
behaviour change.

- `depth_control` bit 0 -> `stencil_enable`
- `depth_control` bits 8-19 -> func + fail/zpass/zfail ops, front and back
- `RB_STENCILREFMASK` (0x2201) -> ref, read mask, write mask
- gate on `RB_MODECONTROL.edram_mode` in {4, 5}, as the census already does

**Pass:** the plumbed values reproduce the 18 configs the census already
reports. If they disagree the plumbing is wrong, and no pixel has moved yet.

## Phase 2: stencil WRITE only. DONE (`df3c0b2`).

**`StencilEnable` comes from the GUEST, per draw. Never blanket-on.** An earlier
draft of this plan said `StencilEnable = TRUE`, and that was wrong in three
separate ways:

- **It corrupts the thing Phase 3 tests.** The guest sets the enable bit on
  122,894 of 218,250 draws. The other ~107,000 have it CLEAR, and enabling
  stencil on them applies their `fail`/`zpass`/`zfail` ops -- which WRITE. That
  stamps the mask from draws the console never lets touch it, so Phase 3 would
  then test against a buffer this phase corrupted.
- **It blows the PSO cache.** `kMaxBlendPSOs` is 128 and overflow silently drops
  a draw to its opaque pipeline. Every draw needing a stencil variant is the
  fastest way there; gating on the guest bit keeps it to the ~14 real variants.
- **It costs work for nothing** on half the frame.

So the gate is exactly the census's own definition, and for the same reason:

```
StencilEnable = (depth_control & 1) && edram_mode in {4, 5}
```

Note `edram_mode` is part of it. Outside `kColorDepth(4)` / `kDepthOnly(5)` the
hardware ignores the register, so a draw can have the enable bit set and mean
nothing by it.

With the gate right, the rest of the phase is: guest's ops, guest's read/write
masks, `OMSetStencilRef` from the guest ref, and **func forced to `ALWAYS`**.
Forcing the func is what makes this phase safe -- no draw can be rejected, so
nothing can vanish -- and it is the ONLY thing here that is temporary.

**Pass: the frame is pixel-identical.** A stencil write cannot change colour, so
any visual change is a plumbing bug, caught before it can be confused with a
stencil-test effect.

**FIRST "PASS" WAS FALSE.** Run 1542 reported clean and was inert -- see the
translated-path section at the end. The numbers below are all real; what they
were not is evidence that stencil did anything.

**PASSED**, run 1542:

```
STENCIL PSOs: 111613 draws carried stencil, 9 distinct states interned,
  0 refused past the cap; blend PSOs 1 of 128, by-format PSOs 2
```

No visual change, no device removals, no PSO failures, frame times in band. 9
states rather than 18 because `func` is forced and `ref` is out of the key, so
configs differing only in those collapse -- Phase 3 raises it.

Standing cross-check worth keeping: `STENCIL PLUMBED` effective and the
renderer's draws-carried count gate identically and should be equal. Run 1542
had 113,249 vs 111,613, a gap well inside one report interval. If it ever
exceeds that, draws are being lost between the two points.

**What this phase does NOT validate:** the stencil buffer's CONTENTS. With every
func forced to `ALWAYS`, ops run on pixels the console would have rejected, so
the mask is deliberately wrong here -- wrong and unobserved. It becomes correct
the moment Phase 3 restores the real funcs, since the plane is cleared per frame
anyway.

## Phase 3: stencil TEST. DONE (`aa7e9a2`), after two clear defects of our own.

Apply the guest's real func. **This is the first step that can change pixels.**

Judge on the whole frame, not on the defect being chased: a working fix was once
reverted for missing its target (`judge-a-change-on-the-whole-frame`).

**Pass:** no geometry vanishes. Vanishing geometry means Phase 0 got the clear
wrong. Go back to it rather than papering over it.

**PASSED**, run 1549, on the connected path:

```
151719 draws carried stencil (80001 with a comparison that can REJECT),
  9 states, 0 refused, 0 WITH NO DSV, blend PSOs 1 of 128
TranslatedPSO stencil: idx 1 enable 1 func 6/6 ops 4/4/4 masks FF/FF dsvfmt 20
```

`func 6` is D3D12 `NOT_EQUAL`, the guest's `kNotEqual`. Nothing vanished: bike,
rider, every menu row, both XP bars. White halos on that screen were checked
against the user and are pre-existing.

**This is the stronger result, and it subsumes the Phase 2 re-check.** If the
masks were being WRITTEN wrongly the tests would reject the wrong fragments and
something would have disappeared. Nothing did, so the write side is right too.

Also confirms the enum conversion end to end on real data: guest ops
3/4/0 (`kIncrementClamp`/`kDecrementClamp`/`kKeep`) arrive as D3D12 4/5/1
(`INCR_SAT`/`DECR_SAT`/`KEEP`), and `kNotEqual` 5 arrives as 6.

## Phase 4: two-sided. ALREADY DONE, folded into Phase 2.

Kept as a heading because the plan named it separately and it is not obvious
from the commits that it is finished. Two-sided was implemented in Phase 2
rather than deferred, because the alternative was WRONG rather than merely
incomplete: D3D12 always reads `BackFace`, so leaving it at defaults applies
`KEEP`/`ALWAYS` to every back-facing triangle. With `RB_DEPTHCONTROL` bit 7
clear the guest means the front state for both faces, so front is copied into
back; with it set, the back fields are read from bits 23-31.

Original note, still true about the risk:

Only 3 of the 18 configs set `backface_enable` -- the shadow volumes. A separate
phase because two-sided interacts with the cull-mode / `FrontCounterClockwise`
flip noted at `d3d12_renderer.h:151`, which also flips `SV_IsFrontFace`.

## Cross-cutting hazards

**The PSO cache cap is the sharpest.** `kMaxBlendPSOs = 128`, and past the cap a
draw *silently falls back to its opaque PSO*, losing its blending. Stencil adds
~14 PSO variants (18 configs minus those differing only in ref, which is
`OMSetStencilRef` and not PSO state), so the product can blow the cap and the
failure mode is a wrong picture, not an error. Raise the cap and **log
occupancy** before Phase 2, then check it after.

**Use `MX_D3D12_DEBUG=2`.** GPU-based validation covers descriptor and
resource-state errors and is the instrument never yet used on this problem. It
needs no rebuild. The earlier claim that DRED is unavailable on this machine was
FALSE: check the current run's DRED line rather than trusting a past run.

**Count occurrences before editing.** The earlier attempt changed one of two
depth-resolve sites because an `Edit` took its uniqueness from surrounding
context, leaving the band-stitched path copying planar depth into an R32_FLOAT
snapshot 944 times a run. Grep every site and state the count.

**One thing at a time.** The failed attempt moved three formats, four clear
sites and two resource paths together, and the fault could have been in any.


## The path that made phases 2 and 3 inert

Both phases reported clean and did nothing. There are THREE pipeline builders:
`OpaquePSO` (plus the 32 eager `m_gamePSOs`), `BlendedPSO`, and **`TranslatedPSO`**
-- and in a level nearly every draw has a translated guest shader, so nearly
every draw took the one that was never wired.

Every upstream check passed the whole time: state built, interned, on the draw,
in the key, applied before anything could overwrite it, DSV `D32S8`, `DSVFormat`
matching, both draw sites patched. **A chain of green checks is not a working
feature.**

What caught it was `--d3d9_stencil_force_never`, which forces `kNever` on every
stencil draw. NEVER cannot pass a fragment under any circumstances, so the
screen must break:

| build | mutation | result |
|---|---|---|
| before `da5c9be` | force NEVER | screen unchanged -> **inert** |
| after `da5c9be` | force NEVER | screen **visibly broken** -> live |

A counter proving state was BUILT says nothing about whether it was APPLIED.

Also confirmed by logging what is actually passed to
`CreateGraphicsPipelineState` -- `enable 1 func 1/1 masks FF/FF dsvfmt 20` --
because the RenderDoc MCP's `get_pipeline_state` has no `depthStencilState`
field and cannot answer it.

**Open, small:** 476 stencil draws are issued with no depth attachment at all
and can never be tested.

**Both phases now need re-validating on the connected path**, in this order:

1. `--d3d9_stencil_force_never=false --d3d9_stencil_test=false` -- Phase 2,
   writes only, every func ALWAYS. **Pass: pixel-identical.**
2. `--d3d9_stencil_test=true` (default) -- Phase 3, the guest's real
   comparisons. **Pass: nothing vanishes.**


## The two clear defects the test exposed

Phase 3 went live, the menu was fine, and then the TERRAIN broke. The comparison
was innocent throughout -- `terrain.rdc` draw 20009 shows the terrain draw
PASSING, no `stencilTestFailed` anywhere in the pixel history, and the pipeline
carrying exactly the state the guest programmed. **The test was reading a buffer
we had corrupted, in two separate places, and it needed both fixed.**

**1. Our per-frame clear wiped the plane** (`9cb4851`). The first-use depth clear
exists so depth does not accumulate across frames -- OUR schedule. Applying it to
stencil destroyed a mask the guest had built: the guest clears stencil where it
wants (`0x20`, `0x30`) and deliberately withholds it where it does not (`0x1F`
is depth with no stencil, and it issues that). Split into
`kGameDepthClearFlags` (creation only, both planes) and
`kGameDepthFrameClearFlags` (per-frame, depth only).

**2. We dropped every `0x60` clear** (`444ecb0`) -- 13,370 of 20,000 calls. I
excluded it twice, on two wrong readings: first as "the EDRAM tile clear",
because `r9` was set (`r9` is the Stencil argument, so that was a value of 1 read
as a boolean); then as "the `0x40` path is not a stencil clear", with the
decompilation in front of me:

```c
if ( (Flags & 0x20) != 0 ) {
    v41 |= 4u;                                    // set BEFORE the branch
    if ( (Flags & 0x40) != 0 )
        v41 = (Stencil << 8) & 0xFF00 | v41 & 0xFFFF00DF;
```

`0x40` is a MODIFIER of the stencil clear, not an alternative to it -- it only
moves the value into bits 8-15. `0x60` is "clear stencil to 1". Without those the
plane sat at 0 all frame and a terrain testing `NotEqual-0` failed everywhere.

**Verified by A/B: terrain correct with the test on, once both landed.** Default
is back ON, and the TERRAIN is the regression test -- it is what broke, and it
broke visibly.
