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

## Phase 0: name the stencil clear bit. BLOCKING.

Nothing downstream is safe until this is answered: a test against a wrongly
cleared buffer makes geometry VANISH rather than degrade.

`hooks_d3d9_entry.cpp:1865` establishes that **D3DCLEAR_ZBUFFER is 0x10 on this
hardware, not the 0x2 a PC D3D9 header says**, and states plainly that bits 1..6
are still not named. Run 1445 saw seven flag values: `0xF, 0x1F, 0x30, 0x3F,
0x60`.

Working hypothesis, to be confirmed and not assumed: **0x20 is
D3DCLEAR_STENCIL**. `0x30 == 0x10|0x20` is the classic depth+stencil pair, and
0x20 is adjacent to a ZBUFFER bit that has already moved from its PC value.

Settle it from the reference. `D3DDevice_Clear`'s body is NOT reachable through
the decompiler (IDA's bounds stop at `0x8255B284` on a misdecoded `vcmpneb.`),
so IDA cannot answer this one; check xenia-edge and the SDK headers.

Then carry `clear_stencil` on `DrawCall` and stop clearing stencil
unconditionally with depth.

**Pass:** the bit is named from a reference and the flag distribution agrees.

## Phase 1: plumb the registers, change nothing

Carry through to `DrawCall` and log a census of what arrives. No PSO change, no
behaviour change.

- `depth_control` bit 0 -> `stencil_enable`
- `depth_control` bits 8-19 -> func + fail/zpass/zfail ops, front and back
- `RB_STENCILREFMASK` (0x2201) -> ref, read mask, write mask
- gate on `RB_MODECONTROL.edram_mode` in {4, 5}, as the census already does

**Pass:** the plumbed values reproduce the 18 configs the census already
reports. If they disagree the plumbing is wrong, and no pixel has moved yet.

## Phase 2: stencil WRITE only

`StencilEnable = TRUE`, func forced to `ALWAYS`, the guest's ops applied,
`OMSetStencilRef` per draw. The buffer gets stamped; nothing tests it.

**Pass: the frame is pixel-identical.** A stencil write cannot change colour, so
any visual change here is a plumbing bug, caught before it can be confused with
a stencil-test effect.

## Phase 3: stencil TEST

Apply the guest's real func. **This is the first step that can change pixels.**

Judge on the whole frame, not on the defect being chased: a working fix was once
reverted for missing its target (`judge-a-change-on-the-whole-frame`).

**Pass:** no geometry vanishes. Vanishing geometry means Phase 0 got the clear
wrong. Go back to it rather than papering over it.

## Phase 4: two-sided

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
