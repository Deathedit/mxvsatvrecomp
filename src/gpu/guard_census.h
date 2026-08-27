#pragma once

// GUARD CENSUS — phase 1 of docs/strict_mode.md.
//
// The renderer manufactures a plausible value wherever it cannot obtain the
// real one. Before any of that can be turned off, each site has to say how
// often it fires AND out of how many opportunities. This header exists to make
// the second half impossible to forget.
//
// THE API SHAPE IS THE POINT. `Note(g, fired)` is called on EVERY opportunity,
// not only when the guard fires, so the denominator is collected structurally
// rather than by remembering to. A counter without a denominator is not a
// measurement, and this project has lost multiple sessions to exactly that:
//
//   - a stand-in counter read as "80% of draws lost", against its own comment
//     warning that reaching it no longer means the draw is lost
//   - a rejected-shader dump capped at 16, which made 75 rejections look like 16
//   - "VFETCH coverage: 30573 of 42", a compile count divided by a population
//     it was never counting
//
// Reporting rules, all learned the hard way and enforced by Report():
//   - zero included, always. "Never fired" and "fired and changed nothing" are
//     different outcomes and a suppressed line looks like neither.
//   - the population is printed next to the fires, never on its own.
//   - one line for the whole census, so it cannot drift into per-guard formats.

#include <cstdint>
#include <string>

namespace mx::gpu::guard {

// Class B sites from docs/strict_mode.md: machinery that INVENTS output.
// Class A (HostPageReadable, PlausibleGuestPtr — refusing to act on untrusted
// input) is deliberately absent. It models reality and is not the subject.
// kFirstUseClear was here and is GONE, 2026-08-26 -- the first guard actually
// REMOVED under docs/strict_mode.md rather than measured. Run 1453 gave it
// 27,316 opportunities with the preserve path forced on and it needed none of
// them (0/27316) while the frame rendered clean and the terrain deformation
// buffer finally accumulated. A guard reading 0/N with N that large has no
// fallback role left, so it was deleted along with its cvar rather than
// default-off, which would only have preserved the option of reintroducing a
// known defect.
enum class Guard : uint32_t {
  kStandInDraw,             // a tex*col colour when no shader resolved
  kScratchColourTarget,     // a colour attachment for a depth-only draw
  kBlankTexturePayload,     // a blank decode for a texture that read empty
  kConstantNanToZero,       // OverlayNonFinite NaN -> 0
  kMaterialGateFill,        // FillMaterialGate PM4 substitution
  kVertexZeroFill,          // vertices past the end of a buffer
  // NOT A DEFECT COUNT -- see the long note at its Note() site in
  // hooks_d3d9.cpp. Dominated by scratch registers that input_mask flags as
  // inputs. A new pair appearing is worth seeing; the absolute level is not.
  kInterpolatorZeroFill,    // mov oN, 0 for exports the guest VS never wrote
  // A SUBSET of kConstantNanToZero, counted separately: guest c392..c395, which
  // land in the pixel bank as xe_c[136..139]. OverlayNonFinite's own comment
  // claims that block is what the whole NaN repair exists for -- "the legal,
  // loading and start screens all live in that prefix, which is why they have
  // no background: a NaN interpolator saturates the backdrop draw to white".
  //
  // Worth its own row because strict=8 suppressed 1.76M substitutions with no
  // visible change in a level, which says nothing about the boot screens. This
  // asks whether the documented mechanism is still live AT ALL, rather than
  // inferring it from an aggregate over 346 million components.
  //
  // APPENDED, not inserted. hle_strict is keyed on enum position and inserting
  // here would shift every bit below -- exactly the drift that sent run 1461 at
  // an inert guard. New entries go on the end, always.
  kConstantNanBackdrop,
  kCount
};

// Call on EVERY opportunity. `fired` says whether the guard invented anything
// this time; `weight` counts a batch (vertices, dwords) as one opportunity of
// that size rather than forcing a loop.
void Note(Guard g, bool fired, uint64_t weight = 1);

// One line, every guard, zero included.
std::string Report();

//---------------------------------------------------------------------------
// STRICT MODE -- phase 2 of docs/strict_mode.md.
//
// A BITMASK, deliberately, not a bool. All-off produces a black screen, which
// is one bit of information; a bitmask lets you binary-search which guard is
// holding which defect up. Bit N disables Guard(N).
//
// Set from the app layer once per frame rather than read here, so src/gpu keeps
// no cvar dependency -- it has none today and this is not worth introducing one
// for.
//
// NOT EVERY GUARD IS SWITCHABLE, and pretending otherwise would be its own kind
// of invention. Three are, because turning them off degrades visibly without
// breaking the pipeline:
//
//   kStandInDraw          the draw is skipped -- it disappears, which is a
//                         diagnostic
//   kBlankTexturePayload  the blank is not recorded, so the decode is retried
//   kConstantNanToZero    the NaN reaches the shader; corruption is the signal
//
// Four are NOT wired, and the reasons are structural rather than caution:
//
//   kScratchColourTarget  every PSO declares NumRenderTargets = 1, so binding
//                         no RTV is invalid work against the pipeline, not a
//                         diagnostic
//   kVertexZeroFill       the fill covers a read PAST THE END of the guest
//                         buffer; not doing it is an out-of-bounds read
//   kInterpolatorZeroFill not ours to disable -- it is the HLSL struct
//                         initialiser, emitted by FXC, with no host-side switch
//   kMaterialGateFill     already inert; the substitution was stripped and only
//                         the measurement remains
//
// Strict(g) is false for all four, always, and the call sites say so.
void SetStrictMask(uint32_t mask);

// True when this guard is disabled and must NOT invent.
bool Strict(Guard g);

// Human-readable name, stable across builds — it is grepped out of logs.
const char* Name(Guard g);

}  // namespace mx::gpu::guard
