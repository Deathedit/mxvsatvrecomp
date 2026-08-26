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
enum class Guard : uint32_t {
  kStandInDraw,             // a tex*col colour when no shader resolved
  kScratchColourTarget,     // a colour attachment for a depth-only draw
  kCrossThreadPixelShader,  // *a* pixel shader when the draw carries none
  kFirstUseClear,           // a clear the guest never issued
  kBlankTexturePayload,     // a blank decode for a texture that read empty
  kConstantNanToZero,       // OverlayNonFinite NaN -> 0
  kMaterialGateFill,        // FillMaterialGate PM4 substitution
  kVertexZeroFill,          // vertices past the end of a buffer
  kInterpolatorZeroFill,    // mov oN, 0 for exports the guest VS never wrote
  kOutputClamp,             // xe_colorscale clamping the final colour
  kCount
};

// Call on EVERY opportunity. `fired` says whether the guard invented anything
// this time; `weight` counts a batch (vertices, dwords) as one opportunity of
// that size rather than forcing a loop.
void Note(Guard g, bool fired, uint64_t weight = 1);

// One line, every guard, zero included.
std::string Report();

// Human-readable name, stable across builds — it is grepped out of logs.
const char* Name(Guard g);

}  // namespace mx::gpu::guard
