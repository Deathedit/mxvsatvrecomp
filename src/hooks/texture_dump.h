#pragma once

// Write every decoded guest texture to logs/texdump as a PNG, so a texture can
// be LOOKED AT rather than reasoned about.
//
// The dump sits at the D3D9 stage, immediately after DecodeHleTexture2D, so what
// lands on disk is exactly the bytes the shader samples: guest memory, untiled,
// endian-swapped, mip level 0, with the fetch constant's swizzle applied the
// same way the SRV applies it.
//
// WHAT IT CANNOT SEE. A texture only reaches this path when it is BOUND TO A
// SAMPLER. A resolve destination the guest renders into and never samples --
// which is what the menu backdrop is believed to be -- produces no binding and
// therefore no file. A missing texture here means "never sampled", not "never
// produced".
//
// Bink's video planes are deliberately NOT dumped: they are new guest memory
// every video frame under a per-frame key, so dumping them would write four
// files 30 times a second and reach the cap before the menu appears.
//
// Off unless `texture_dump = true`. It encodes on the calling thread and will
// hitch on the first draw that binds a texture it has not seen.

#include <cstdint>

namespace mx::hle {
struct HleTextureSource;
struct HleTexturePayload;
}  // namespace mx::hle

namespace mx::diag {

// Cheap enough to call on every decode: reads the cvar once and caches it.
bool TextureDumpEnabled();

// One PNG per distinct (key, content_version) pair, capped. `site` names the
// decode path that produced it ("slot", "prepare", "bink") and is recorded in
// the index rather than in the filename.
//
// `payload.key` and `payload.content_version` must already be set -- call this
// after the caller has assigned them, not straight off the decoder.
void DumpDecodedTexture(const mx::hle::HleTextureSource& source,
                        const mx::hle::HleTexturePayload& payload,
                        const char* site, uint32_t sampler);

}  // namespace mx::diag
