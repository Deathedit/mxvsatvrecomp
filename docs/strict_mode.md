# Strict mode: stop inventing output

## The problem

The renderer is full of machinery that manufactures a plausible value when it
cannot obtain the real one. Each piece was added to make a symptom go away, and
each one now sits between us and a signal.

**Guards convert *absent* into *plausible*, and plausible is undebuggable.**

The concrete cost, measured 2026-08-26 while chasing the flat ground: the
terrain draw outputs `0.034`, not `0.0`. A draw producing nothing tells you its
inputs are missing; a draw producing something tells you it is being scaled
down. Those are different bugs and the number says which — unless every layer in
the chain is working to ensure we always produce *something*. Hours went into
resolving an ambiguity we manufactured.

The same session found a real NaN in the terrain shader (instruction 201,
`rcp r0.x` with `r0.x = 0`) sitting downstream of **three** independent layers of
NaN suppression: `OverlayNonFinite` for constants, the per-op `lt abs(x), l(0)`
guards the translator emits into DXBC, and the `xe_colorscale` output clamp.
Three walls, and the actual defect walked through all of them.

## The classification

Not all guards are the disease. The distinction is what makes this tractable.

| class | rule | examples | strict mode |
|---|---|---|---|
| **A. Refuse bad input** | declines to act on untrusted data | `HostPageReadable` (145 sites), `PlausibleGuestPtr` (20) | **keep** |
| **B. Invent output** | manufactures a value we do not have | table below | **disable** |
| **C. Cache / perf** | same output, less work | texture cache, PSO cache | untouched |

Class A is modelling reality: guest pointers really are untrusted, and the
2026-08-19 access violation in the video probe is what happens without it.
Conflating A with B turns this into a week of crashes for no information.

## The B inventory

Counts are `grep` site counts in `src/`, 2026-08-26.

| guard | sites | what it invents |
|---|---|---|
| stand-in draw path | 31 | a `tex*col` colour when no shader resolved — **62,285 of 225,000 draws** in run 1448 |
| scratch colour target | 15 | a colour attachment for depth-only draws |
| cross-thread PS fallback | 13 | *a* pixel shader when the draw carries none (`PixelShaderForDeviceStrict`) |
| first-use clears | 13 | a clear the guest never issued — this is what broke terrain-deformation accumulation |
| blank texture payloads | 12 | a blank decode for a texture that read empty |
| constant substitution | 12 | `OverlayNonFinite` NaN→0, `FillMaterialGate` PM4 fill |
| vertex zero-fill | 11 | vertices past the end of a buffer — **58,138** in one run |
| interpolator zero-fill | — | `mov o7, 0` for exports the guest VS never wrote |
| output clamp | 6 | `xe_colorscale` clamping the final colour |
| shader NaN guards | — | `lt abs(x), l(0)` emitted per-op into the DXBC |

Retired predecessor worth remembering: `hle_sanitize_constants` zeroed every
non-finite constant on every draw, forever. That is the endpoint of this
tendency.

## Phases

### Phase 1 — instrumentation parity

Every B guard reports **fires and denominator**, in one consistent format.

Several already do, inconsistently, and that inconsistency is expensive: in one
evening a stand-in counter whose own comment warned it no longer meant "lost
draws" was read as an 80% loss; a dump capped at 16 made 75 rejections look like
16; and an `attempt % 250` cadence made a per-draw counter look like a schedule.

**This phase has the highest return and is the one that gets skipped.** It also
stands alone — it retires leads without disabling anything, because a guard that
turns out to fire zero times is already an answer. The stand-in *scorer* was
exactly that: real, and inert.

Rules for the format, all of them learned the hard way:

- print the total **and** the population it is a fraction of; a count without a
  denominator is not a measurement
- print **zero included** — "never fired" and "fired and changed nothing" are
  different outcomes and a suppressed line looks like neither
- one line per distinct *finding*, not per call; throttle on the finding
  changing, never on a draw counter
- never cap a population without saying the cap in the line

### Phase 2 — the switch

`hle_strict` as a **BITMASK, not a bool**. This is the load-bearing design
decision: all-off produces a black screen, which is one bit of information. A
bitmask lets you binary-search which guard is holding which defect up. If we
build a bool we have wasted the work.

Sits beside the existing `hle_diag` / `hle_capture` naming.

### Phase 3 — triage

One bit at a time, log plus capture for each. Output that goes black, NaN, or
crashes is a **located** bug. Rank by draw count.

## Expected return

The open leads that look guard-shaped: the dark ground, the 58k zero-filled
vertices, the terrain NaN at `rcp r0.x`, the `unbound by sampler` slots, the 26
`no texture fetch` pixel shaders.

Honest estimate: Phase 3 resolves two or three and reclassifies the rest. That is
a guess, and the point of the exercise is to stop making them.

## Risks

- **All-off is uninformative.** Hence the bitmask.
- **Some B guards are load-bearing for stability, not appearance.** The
  cross-thread PS fallback took the menu 4.45 → 9.88 fps by giving 80,000 draws a
  translated shader. Disabling it is a diagnostic run, never a shipping default.
- **Phase 1 looks like busywork.** It is where the cost was actually paid.

Two to three days total.
