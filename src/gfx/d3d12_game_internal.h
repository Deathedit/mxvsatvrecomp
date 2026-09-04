// Helpers shared between the d3d12_game_*.cpp translation units.
//
// These five functions and two counters used to sit in an anonymous namespace in
// d3d12_game.cpp. Splitting that file put their definitions and their callers in
// different TUs, and internal linkage does not cross a TU. So they are declared
// here and defined, once, in d3d12_game.cpp.
//
// This is the ONLY linkage change the split required; everything else moved
// verbatim. Checked before making it: none of these seven names appears anywhere
// else in src/.

#pragma once

#include "gfx/d3d12_internal.h"

#include <cstdint>
#include <string>

// An HRESULT with its name where there is one. See the long comment at the
// definition -- a bare "creation failed" with the code discarded once cost a
// session.
const char* HrName(HRESULT hr);
std::string HrText(HRESULT hr);

// The SRV format to view a resource with, which differs from the resource
// format for the typeless depth cases.
DXGI_FORMAT SrvFormatForResource(DXGI_FORMAT resourceFormat);

// Guest blend factor / op to D3D12. False means the guest value has no mapping,
// which the caller reports rather than guessing at.
bool ToD3D12Blend(uint32_t guest, bool alpha_channel, D3D12_BLEND& out);
bool ToD3D12BlendOp(uint32_t guest, D3D12_BLEND_OP& out);

// One-shot log of a guest colour format per target object, and the topology
// CLASS a topology belongs to (D3D12 wants both at PSO creation). Both already
// had external linkage in d3d12_game.cpp -- they were never in the anonymous
// namespace -- so these are declarations only, not a linkage change.
void LogGuestColorFormat(uint32_t object, uint32_t width, uint32_t height,
                         uint32_t guestColorFormat);
D3D12_PRIMITIVE_TOPOLOGY_TYPE TopologyTypeOf(D3D12_PRIMITIVE_TOPOLOGY topo);

// Blend PSO refusals, reported by the PSO cache.
extern uint64_t g_blendUnmapped;  // draws whose state did not translate
extern uint64_t g_blendBudget;    // draws refused because the cache was full
