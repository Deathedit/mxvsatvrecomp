// D3DDevice_Resolve and D3DDevice_Clear.
//
// Split verbatim out of hooks_d3d9_entry.cpp. Resolve is the largest single
// function in the D3D9 layer at ~750 lines, and with Clear it was a third of a
// file whose other 30 hooks average forty.
//
// They belong together and apart from the rest: both are ORDERED EVENTS on a
// render target rather than draws. Resolve is the EDRAM to system-memory
// bridge -- the internal helper reads the destination texture's fetch
// descriptor at +0x1C, which is what proves it and not SetTexture is the
// bridge. Clear is modelled only in its full-surface colour form, because the
// front-end atlas clears a scratch target and resolves it three times without
// ever issuing a draw, and without that event the host has no source resource
// for those resolves.
//
// This cut publishes nothing. The range defines only its two REX_IMPORT /
// REX_FUNC pairs and touches none of the anonymous-namespace helpers left in
// hooks_d3d9_entry.cpp, so the split moved code out of the link rather than
// adding to it.

#include "gpu/health.h"
#include "hooks/hook_common.h"

// For the small-destination writeback: the SAME tiled address function the
// decoder uses, run in the other direction. See the note at its call site.
#include <rex/graphics/pipeline/texture/util.h>
namespace tu = rex::graphics::texture_util;

#include <array>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>

#include "gpu/d3d9_state.h"
#include "gpu/d3d9_texture.h"
#include "gpu/hle_types.h"

#include "hooks/guest_read_watch.h"
#include "hooks/hooks_d3d9_internal.h"

// extern "C" bodies cannot live inside the namespace, so they reach into it
// wholesale -- the same seam hooks_d3d9_entry.cpp uses.
using namespace mx::hooks::d3d9;

//-----------------------------------------------------------------------------
// 0x8255CE98 -- D3DDevice_Resolve.
//
// r4 low three bits select colour target 0..3 or depth target 4; r6 is the
// destination D3DBaseTexture. The internal helper reads that texture's fetch
// descriptor at +0x1C, proving this call -- not SetTexture -- is the EDRAM to
// system-memory bridge.
//
// 0x8255B258 -- D3DDevice_Clear. Only the measured full-surface colour form is
// modelled. The front-end default-texture atlas binds a 256x256 scratch target,
// clears it, and resolves it three times without issuing a draw; without this
// ordered event the host has no source resource for those resolves. Partial
// rectangle and depth/stencil clears still pass through to the guest.
//-----------------------------------------------------------------------------
REX_IMPORT(__imp__sub_8255B258, orig_Clear, void());
extern "C" REX_FUNC(sub_8255B258) {
  MX_D3D9_HLE_LOCK;
  const uint32_t rect_count = ctx.r4.u32;
  const uint32_t rects = ctx.r5.u32;
  const uint32_t flags = ctx.r6.u32;
  const uint32_t color = ctx.r7.u32;
  const auto& target = DeviceState().render_target[0];

  // CLEAR CENSUS -- every call, BEFORE any bit test. The CLEAR line below lives
  // inside the `flags & 1` branch, so it can only report clears that touch
  // colour, and the question is whether the guest asks for depth clears we drop.
  //
  // That question is live. freeroam.rdc: the D32S8 depth target is Cleared
  // exactly ONCE in the whole frame and then used as the DepthStencilTarget by
  // six separate passes with no clear between them. The ground draw has its
  // three textures bound, a translated shader and a shaderOut of sand, and is
  // discarded `depthTestFailed` against depth it did not write.
  //
  // Reported as a HISTOGRAM with a total, so the answer carries its own
  // denominator. LET THE FLAGS NAME THEMSELVES: one run showed seven distinct
  // values -- 0x1, 0xF, 0x1F, 0x20, 0x30, 0x3F, 0x60 -- and 0x60 alone was 18280
  // of 28000 calls, which no assumed bit layout accounted for. The call carries
  // the answer instead:
  //
  //   D3DDevice_Clear(pDevice, Count, pRects, Flags, Color, Z, Stencil,
  //                   EDRAMClear)
  //
  // so a flag value whose calls carry Z=1.0 is a DEPTH clear whatever its bit
  // pattern.
  {
    struct FlagStat {
      uint64_t calls = 0;
      double first_z = 0.0;
      uint32_t first_r8 = 0, first_r9 = 0, first_r10 = 0;
      uint64_t r9_nonzero = 0;
      bool z_varies = false;
    };
    // ARGUMENT SLOTS ARE NOT ASSUMED. Reading Z as *(float*)&ctx.f1 gives 0 for
    // every call -- that is the LOW half of a double, since rex::ppc's FP
    // register is a union, and Z = 1.0 has a zero low word. Read f64.
    //
    // The integer slots were wrong too: r8 as Stencil produced 47185920 for
    // flags 0x30, 0x3F AND 0x60 alike, and one identical value across three
    // unrelated groups is a leftover register. PowerPC ABIs differ over whether
    // a float argument also consumes its GPR slot, so print r8, r9 and r10 raw.
    static std::mutex s_mu;
    static std::map<uint32_t, FlagStat> s_byFlags;
    static uint64_t s_total = 0, s_fullSurface = 0;
    const double z = ctx.f1.f64;
    const uint32_t r8 = ctx.r8.u32;
    const uint32_t r9 = ctx.r9.u32;
    const uint32_t r10 = ctx.r10.u32;
    std::lock_guard<std::mutex> lk(s_mu);
    ++s_total;
    if (rect_count == 0 && rects == 0) ++s_fullSurface;
    FlagStat& st = s_byFlags[flags];
    const bool fresh = ++st.calls == 1;
    if (fresh) {
      st.first_z = z;
      st.first_r8 = r8;
      st.first_r9 = r9;
      st.first_r10 = r10;
    } else if (z != st.first_z) {
      st.z_varies = true;
    }
    if (r9) ++st.r9_nonzero;
    if ((fresh && s_byFlags.size() <= 32) || (s_total % 4000) == 0) {
      std::string hist;
      for (const auto& [f, v] : s_byFlags) {
        hist += fmt::format(
            " [0x{:X} x{} z={:g}{} r8=0x{:X} r9=0x{:X}(nz{}) r10=0x{:X}]", f,
            v.calls, v.first_z, v.z_varies ? "(varies)" : "", v.first_r8,
            v.first_r9, v.r9_nonzero, v.first_r10);
      }
      REXLOG_INFO("d3d9: CLEAR CENSUS {} calls, {} whole-surface, {} distinct "
                  "flag values; handled today: only those with bit0 and a "
                  "valid colour target --{}",
                  s_total, s_fullSurface, s_byFlags.size(), hist);
    }
  }

  // D3DCLEAR_TARGET is bit 0. Count zero and a null rectangle pointer are the
  // whole-target form used by the measured atlas initializer.
  if ((flags & 1u) && rect_count == 0 && rects == 0 && target.valid) {
    mx::hle::DrawCall clear{};
    clear.clear_color_target = true;
    clear.clear_color = color;
    clear.render_target_object = target.object;
    clear.render_target_surface_info = target.surface_info;
    clear.render_target_color_info = target.color_info;
    clear.render_target_width = target.width;
    clear.render_target_height = target.height;
    clear.surface_base = target.color_info & 0xFFFu;
    mx::hle::HleFrameDraws().push_back(std::move(clear));
    // Keyed on (target, COLOUR), not on the target alone. Deduping by target
    // logs only the FIRST colour each surface was ever cleared to, so a run
    // whose targets are first cleared to black reads as "this game only ever
    // clears to 0x00000000" -- which was once used to argue a mid-grey clear
    // could not be the guest's.
    static std::set<std::pair<uint32_t, uint32_t>> s_logged;
    if (s_logged.insert({target.object, color}).second &&
        s_logged.size() <= 64) {
      REXLOG_INFO("d3d9: CLEAR target 0x{:08X} {}x{} color=0x{:08X} "
                  "flags=0x{:X}",
                  target.object, target.width, target.height, color, flags);
    }
  }
  // D3DCLEAR_ZBUFFER is 0x10 on this hardware, NOT the 0x2 a PC D3D9 header
  // would tell you. Narrow on purpose: five of the seven flag values carry
  // Z=1.0, but Z is an ARGUMENT rather than a flag, so this gates on 0x10 alone.
  //
  // 0x60 is deliberately EXCLUDED even though it carries Z=1.0 and is 16139 of
  // 24000 calls: it is the only value whose r9 is set on every call, and r9 is
  // the EDRAMClear argument, so it is most likely the EDRAM tile clear.
  //
  // Bits 1..6 are still not named. IDA's bounds for D3DDevice_Clear stop on a
  // misdecoded instruction so the body is unreachable through the decompiler,
  // and the constants are in no header in this tree or the SDK.
  {
    const auto& depth = DeviceState().depth_stencil;
    // 0x20 is D3DCLEAR_STENCIL. PROVEN from the guest's own clear emitter
    // sub_8255A510:
    //
    //     if ( (Flags & 0x10) != 0 )  v41 |= 1u;          // depth
    //     if ( (Flags & 0x20) != 0 ) {
    //         v41 |= 4u;                                  // stencil
    //         *v44++ = 8461;                              // 0x210D
    //         *v44 = 0x00FF0000 | (Stencil & 0xFF);       // RB_STENCILREFMASK
    //     }
    //
    // The caller-side decode agrees: sub_8255AAB0 loops bits 0..3 over the four
    // render targets and masks off 0xF0 after the first, so bits 0-3 are the MRT
    // colour targets and 0x10/0x20/0x40/0x80 are the depth-stencil group.
    //
    // STENCIL IS r9, NOT r8, and this file said otherwise for a long time. On
    // this ABI a float argument consumes its integer register slot, so Z in f1
    // RESERVES r8 and the two integer args land in r9 and r10. Reading r8 logged
    // `s=0` on every line, which looked correct: 0x2D00000 & 0xFF is 0.
    //
    // PROVEN by following the value through four frames rather than inferring it
    // from the ABI (r9 being 0/1 is predicted equally well by
    // EDRAMClear-as-a-BOOL):
    //
    //   D3DDevice_Clear   8255b270  mr  r27, r9
    //                     8255b2b8  mr  r8, r27          -> sub_8255B130
    //   sub_8255B130                r8 untouched          -> sub_8255AAB0
    //   sub_8255AAB0      prologue  mr  r22, r8
    //                     8255afdc  stw r22, ...(r1)      ; r1 + 0x5C
    //                     8255b000  bl  sub_8255A510
    //   sub_8255A510      8255a5d0  lwz r29, ...(r1)      ; same slot
    //                     8255a5e4  insrwi r30, r29, 8,16
    //                               and the RB_STENCILREFMASK write below it
    const uint32_t stencil_value = ctx.r9.u32;
    const bool want_depth = (flags & 0x10u) != 0;
    // 0x60 IS HONOURED. It was excluded twice, on two wrong readings:
    //
    //   1. "the EDRAM tile clear", because r9 was set on every call. r9 is the
    //      Stencil ARGUMENT, so that was a stencil value of 1 read as a boolean.
    //      r10, the real EDRAMClear, is zero on all 20,000 calls.
    //   2. "the 0x40 path is not a stencil clear". Reading sub_8255A510 again
    //      says it is:
    //
    //          if ( (Flags & 0x20) != 0 ) {
    //              v41 |= 4u;                                    // stencil
    //              if ( (Flags & 0x40) != 0 )
    //                  v41 = (Stencil << 8) & 0xFF00 | v41 & 0xFFFF00DF;
    //              *v44 = 0x00FF0000 | (Stencil & 0xFF);         // REFMASK
    //          }
    //
    //      0x40 does NOT suppress the clear -- bit 2 of v41 is set before the
    //      branch and stays set -- it only moves the value into bits 8-15. So
    //      0x60 is "clear stencil to 1", 13,370 times a run.
    //
    // Dropping them left the plane stuck at 0, and a terrain testing NotEqual-0
    // against 0 fails everywhere -- the broken ground.
    const bool want_stencil = (flags & 0x20u) != 0;
    if ((want_depth || want_stencil) && rect_count == 0 && rects == 0 &&
        depth.valid) {
      mx::hle::DrawCall dclear{};
      dclear.clear_depth_target = want_depth;
      dclear.clear_stencil_target = want_stencil;
      dclear.clear_stencil = uint8_t(stencil_value & 0xFFu);
      // The guest's own Z, not a hardcoded 1.0. It is 1.0 in every call
      // measured, but reading it costs nothing and a reversed-depth pass would
      // otherwise be cleared to the wrong end.
      dclear.clear_depth = float(ctx.f1.f64);
      dclear.depth_target_object = depth.object;
      dclear.depth_target_width = depth.width;
      dclear.depth_target_height = depth.height;
      dclear.depth_target_base = depth.color_info & 0xFFFu;
      mx::hle::HleFrameDraws().push_back(std::move(dclear));
      // Keyed on (target, flags), not on the target alone: a surface cleared
      // depth-only and later depth+stencil is two different behaviours and
      // deduping by object would log only whichever came first.
      static std::set<std::pair<uint32_t, uint32_t>> s_logged;
      if (s_logged.insert({depth.object, flags}).second &&
          s_logged.size() <= 24) {
        REXLOG_INFO("d3d9: DEPTH/STENCIL CLEAR target 0x{:08X} {}x{} z={:g} "
                    "depth={} stencil={} s={} flags=0x{:X}",
                    depth.object, depth.width, depth.height,
                    double(dclear.clear_depth), want_depth, want_stencil,
                    stencil_value & 0xFFu, flags);
      }
    }
  }
  orig_Clear(ctx, base);
}

REX_IMPORT(__imp__sub_8255CE98, orig_Resolve, void());
extern "C" REX_FUNC(sub_8255CE98) {
  MX_D3D9_HLE_LOCK;
  auto& st = DeviceState();
  st.NoteDevice(ctx.r3.u32, mx::hle::kEpResolve);
  const uint32_t resolve_flags = ctx.r4.u32;
  const uint32_t source_slot = resolve_flags & 7u;
  const uint32_t dest_texture = ctx.r6.u32;
  // Decompiled signature (default.xex.probe.i64):
  //   D3DDevice_Resolve(pDevice, Flags, pSourceRect, pDestTexture, pDestPoint,
  //                     DestLevel, DestSliceOrFace, pClearColor, ...)
  // so r5 is the source rectangle and r7 the destination point. Both are LONG
  // pairs/quads (D3DRECT{x1,y1,x2,y2}, D3DPOINT{x,y}) and both may be null.
  const uint32_t source_rect_ptr = ctx.r5.u32;
  const uint32_t dest_point_ptr = ctx.r7.u32;
  int32_t src_rect[4] = {0, 0, 0, 0};
  bool have_src_rect = false;
  if (source_rect_ptr && HostPageReadable(REX_RAW_ADDR(source_rect_ptr)) &&
      HostPageReadable(REX_RAW_ADDR(source_rect_ptr + 12))) {
    for (uint32_t i = 0; i < 4; ++i)
      src_rect[i] = int32_t(REX_LOAD_U32(source_rect_ptr + i * 4));
    have_src_rect = src_rect[2] > src_rect[0] && src_rect[3] > src_rect[1];
  }
  int32_t dest_point[2] = {0, 0};
  bool have_dest_point = false;
  if (dest_point_ptr && HostPageReadable(REX_RAW_ADDR(dest_point_ptr)) &&
      HostPageReadable(REX_RAW_ADDR(dest_point_ptr + 4))) {
    dest_point[0] = int32_t(REX_LOAD_U32(dest_point_ptr));
    dest_point[1] = int32_t(REX_LOAD_U32(dest_point_ptr + 4));
    have_dest_point = dest_point[0] >= 0 && dest_point[1] >= 0;
  }
  const mx::hle::RenderTargetBinding* source = nullptr;
  if (source_slot < 4)
    source = &st.render_target[source_slot];
  else if (source_slot == 4)
    source = &st.depth_stencil;

  // A resolve that names a destination but cannot be recorded. Counted and
  // logged because it used to be silent, and it is one of the two ways a
  // resolved surface ends up sampled as black.
  if (dest_texture && (!source || !source->valid)) {
    ++g_resolveDroppedNoSource;
    static std::map<uint32_t, uint64_t> s_dropped;
    if (s_dropped[dest_texture]++ == 0) {
      REXLOG_INFO("d3d9: resolve DROPPED (no valid source): slot {} dest "
                  "0x{:08X}; source {} {}x{} -- this destination will decode "
                  "from guest memory and read black",
                  source_slot, dest_texture,
                  source ? "invalid" : "absent",
                  source ? source->width : 0, source ? source->height : 0);
    }
  }

  // The other half of the thread pairing above.
  {
    static std::set<uint64_t> s_seen;
    const uint64_t id =
        (uint64_t(GetCurrentThreadId()) << 32) | (source ? source->object : 0);
    if (s_seen.insert(id).second && s_seen.size() <= 32) {
      REXLOG_INFO("d3d9: RESOLVE thread {} source slot {} object 0x{:08X} "
                  "valid={}",
                  GetCurrentThreadId(), source_slot,
                  source ? source->object : 0, source && source->valid);
    }
  }

  uint32_t dest_extent_width = 0, dest_extent_height = 0;
  if (dest_texture && source && source->valid) {
    g_resolvedTextureTargets[dest_texture] = source->object;

    // The destination's own fetch constant, which is where its guest memory
    // address lives. The six dwords sit at +0x1C..+0x30 -- the same offsets
    // SetTexture copies from -- and DescribeHleTexture2D already turns them into
    // an address and an extent, so nothing here decodes a bitfield by hand.
    if (HostPageReadable(REX_RAW_ADDR(dest_texture + 0x1C)) &&
        HostPageReadable(REX_RAW_ADDR(dest_texture + 0x30))) {
      uint32_t dest_fetch[6] = {};
      for (uint32_t i = 0; i < 6; ++i)
        dest_fetch[i] = REX_LOAD_U32(dest_texture + 0x1C + i * 4);
      mx::hle::HleTextureSource dest_desc;
      if (mx::hle::DescribeHleTexture2D(dest_fetch, dest_desc, nullptr) &&
          dest_desc.address) {
        const uint32_t physical = GpuPhysicalAddress(dest_desc.address);
        auto& entry = g_resolvedTargetsByAddress[physical];
        const bool first = entry.dest_object == 0;
        entry.dest_object = dest_texture;
        entry.source_object = source->object;
        entry.width = dest_desc.width;
        entry.height = dest_desc.height;
        // Where this resolve lands in the destination. No rect means the whole
        // surface, which is the common case and must read as full coverage.
        {
          const uint32_t dx = have_dest_point ? dest_point[0]
                              : have_src_rect ? src_rect[0]
                                              : 0;
          const uint32_t dy = have_dest_point ? dest_point[1]
                              : have_src_rect ? src_rect[1]
                                              : 0;
          uint32_t w = have_src_rect && src_rect[2] > src_rect[0]
                           ? uint32_t(src_rect[2] - src_rect[0])
                           : source->width;
          uint32_t h = have_src_rect && src_rect[3] > src_rect[1]
                           ? uint32_t(src_rect[3] - src_rect[1])
                           : source->height;
          entry.reached_x = std::max(entry.reached_x, dx + w);
          entry.reached_y = std::max(entry.reached_y, dy + h);
          // The bounding box stays -- it is still worth SEEING in the census,
          // and it makes a scattered destination legible next to the real
          // coverage. It is no longer what the claim decision reads; MarkCoverage
          // is. Marked AFTER width/height are assigned above.
          entry.MarkCoverage(dx, dy, dx + w, dy + h);
          ++entry.resolves;
          g_resolveDestObjectPhys[dest_texture] = physical;
        }
        // SMALL DESTINATION WRITEBACK -- the terrain virtual-texture feedback
        // buffer, and anything else the guest resolves small and then LOADS
        // rather than samples.
        //
        // 0x1A2DD000 is 64x64, resolved once per frame, and no shader ever
        // touches it: the GPU writes page IDs there so the CPU can decide which
        // tiles to stream. Landing the resolve only in a host snapshot leaves
        // the guest reading whatever was at that address when it was allocated.
        //
        // Same moment as the 1x1 case above and for the same reason: the resolve
        // the guest just issued is the one it is about to read.
        //
        // NOT GATED ON THE DESTINATION'S EXTENT. It used to require 64x64,
        // conflating the region with the resource, so the terrain deformation's
        // 128x32 tile into a 2048x2048 accumulation was refused. `rb.destObject
        // == dest_texture` below is the real discriminator and it is exact.
        if (dest_desc.width > 1 && dest_desc.height > 1 &&
            dest_desc.bytes_per_block && dest_desc.address) {
          // EVERY SLOT, not "has the global sequence moved". With one seq, the
          // first destination to match stamped the frame consumed and any other
          // delivered in the same frame was skipped without being looked at.
          //
          // The acquire load below is kept for its fence, not as a gate: it
          // pairs with the release bump in DrainSurfaceReadback so the bytes a
          // slot's seq advertises are visible before the seq is.
          static uint32_t s_slotSeq[mx::hle::kSurfaceReadbackSlots] = {};
          (void)mx::hle::g_surfaceReadbackSeq.load(std::memory_order_acquire);
          for (uint32_t slot = 0; slot < mx::hle::kSurfaceReadbackSlots;
               ++slot) {
            uint32_t wrote = 0;
            uint32_t skipped_unwritable = 0;
            bool matched = false;
            // WHAT WE ACTUALLY DELIVER, over the exact byte the guest gates on.
            // The guest's page-table update (sub_82AF5D38) reads the LOW BYTE of
            // the big-endian dword as a mip level:
            //
            //     v92 = (unsigned __int8)*v91;
            //     if (v92 < v66) { ...refine this page... }
            //
            // so every texel whose low byte is >= the level count is skipped. If
            // all 4096 are skipped the update refines nothing, and "wrote 4096
            // texels" could never distinguish that from a healthy feed.
            uint32_t low_hist[256] = {};
            // Per-byte spread of the feedback texel, in GUEST byte order, over
            // the whole run rather than one writeback -- the interesting frames
            // are a minority.
            //
            // ROW/COL HIGH NIBBLES AS A DISTRIBUTION, not a min and a max. The
            // page table's coarse levels are exactly half resident with a knife
            // edge at the midpoint, and the guest decodes ROW from the HIGH
            // nibble of byte +0, so if it never sees rowHi 0 or 1 the top half
            // can never go resident. A min is one texel and cannot distinguish a
            // handful of strays from a real share. Same gate and population as
            // the byte spread below, so rowHi[0..3] must sum to b0's `seen`.
            //
            // THE LEVEL BYTE, AFTER THE GATE. Byte +3 is the LOD, and the guest
            // shifts BOTH coordinates by it and indexes THAT level's table, so a
            // constant LOD aims every request at one level of the pyramid. Its
            // raw range (00..FF) is the WRONG population, since the guest skips
            // any texel whose level is >= the level count. Bucketed 0..15
            // because the gate is `< 16`; a non-zero 16th bucket would mean the
            // gate moved.
            static uint64_t s_lod[16] = {};
            static uint64_t s_rowHi[4] = {}, s_colHi[4] = {};
            static uint64_t s_byteSeen[4] = {}, s_byteHigh[4] = {};
            static uint32_t s_byteMin[4] = {255, 255, 255, 255};
            static uint32_t s_byteMax[4] = {};
            {
              std::lock_guard<std::mutex> lk(mx::hle::g_surfaceReadbackMutex);
              const auto& rb = mx::hle::g_surfaceReadback[slot];
              // FORMAT CONVERSION, because a Xenos resolve converts. The old
              // guard demanded matching bytes-per-texel, which only the VT
              // feedback buffer satisfies; the terrain deformation resolves an
              // R32_FLOAT tile into a destination the guest fetches as FMT_8.
              //
              // The mapping is not a guess: sub_82AF7240 memsets that buffer to
              // 0x80 and the float accumulation's measured maximum is 0.5021 =
              // 128/255, so round(saturate(f) * 255) is the conversion and 0x80
              // is its neutral in both representations.
              const bool same_texel =
                  rb.bytesPerTexel == dest_desc.bytes_per_block;
              // DISABLED pending a correct mapping. Restore by deleting the
              // `false &&`.
              //
              // The R32_FLOAT deform tile is written raw as round(f * 255) into
              // a buffer the guest MEMSETS TO 0x80, which is defensible only if
              // the tile carries the accumulated height. In a real level it does
              // not -- freeroam measured a mean of 6-7 against a neutral of 128
              // -- and an earlier MENU run reading `byte 7A..80 mean 7F` proved
              // nothing, because in the menu the tile is neutral anyway.
              //
              // Until it is known whether the tile is the accumulation or a
              // DELTA to be combined with the previous half (the guest has an
              // hft_deform_copy pass never observed executing), writing it raw
              // is worse than not writing it.
              const bool float_to_unorm8 =
                  false &&
                  rb.bytesPerTexel == 4 && dest_desc.bytes_per_block == 1 &&
                  rb.srcFormat == uint32_t(DXGI_FORMAT_R32_FLOAT);
              matched = rb.seq && rb.seq != s_slotSeq[slot] &&
                        rb.destObject == dest_texture && rb.width &&
                        rb.height && rb.bytesPerTexel &&
                        (same_texel || float_to_unorm8);
              if (matched) {
                s_slotSeq[slot] = rb.seq;
                const uint32_t bpb = dest_desc.bytes_per_block;
                const uint32_t bpb_log2 = uint32_t(std::bit_width(bpb)) - 1u;
                const uint32_t w = std::min(rb.width, dest_desc.width);
                const uint32_t h = std::min(rb.height, dest_desc.height);
                for (uint32_t y = 0; y < h; ++y) {
                  for (uint32_t x = 0; x < w; ++x) {
                    // The READBACK's texel size, which is not the
                    // destination's once a conversion is in play.
                    const size_t srcOff =
                        size_t(y) * rb.rowPitch + size_t(x) * rb.bytesPerTexel;
                    if (srcOff + rb.bytesPerTexel > rb.byteCount) continue;
                    // AT THE DESTPOINT. The copied region is a sub-rect of the
                    // destination; every caller before the terrain deformation
                    // resolved to (0,0), so writing at the origin was right by
                    // accident rather than by rule.
                    const uint32_t dx = x + rb.destX;
                    const uint32_t dy = y + rb.destY;
                    if (dx >= dest_desc.width || dy >= dest_desc.height)
                      continue;
                    // The guest's own layout, tiled or linear, exactly as the
                    // DECODER reads it -- the same tu::GetTiledOffset2D, run in
                    // the other direction. Two address rules that disagree is
                    // the bug an address rule exists to prevent.
                    const uint32_t dstOff =
                        dest_desc.tiled
                            ? uint32_t(tu::GetTiledOffset2D(
                                  int32_t(dx), int32_t(dy),
                                  dest_desc.pitch_blocks, bpb_log2))
                            : (dy * dest_desc.pitch_blocks + dx) * bpb;
                    const uint32_t at = dest_desc.address + dstOff;
                    if (!HostPageReadable(REX_RAW_ADDR(at)) ||
                        !HostPageReadable(REX_RAW_ADDR(at + bpb - 1))) {
                      ++skipped_unwritable;
                      continue;
                    }
                    // Byte-reversed for the guest's endian, the same swap the
                    // upload path applies coming the other way.
                    uint8_t tmp[16];
                    if (float_to_unorm8) {
                      float f = 0.0f;
                      std::memcpy(&f, rb.bytes + srcOff, sizeof(f));
                      if (!(f > 0.0f)) f = 0.0f;  // also catches NaN
                      if (f > 1.0f) f = 1.0f;
                      tmp[0] = uint8_t(f * 255.0f + 0.5f);
                    } else {
                      std::memcpy(tmp, rb.bytes + srcOff, bpb);
                    }
                    if (bpb == 2 && dest_desc.endian != 0) {
                      std::swap(tmp[0], tmp[1]);
                    } else if (bpb == 4 && dest_desc.endian != 0) {
                      // R<->B FIRST, then the reversal. The endian reversal was
                      // right and was being applied to the wrong channel order:
                      // the host resource is R8G8B8A8_UNORM so `rb.bytes` runs
                      // R,G,B,A while the guest's k_8_8_8_8 surface is B,G,R,A,
                      // and reversing the host order alone produced A,B,G,R
                      // where the guest reads A,R,G,B.
                      //
                      // Established from the guest's own feedback walk:
                      //
                      //   lwzu   r10, 4(r5)          big-endian texel
                      //   clrlwi r11, r10, 24        byte +3
                      //   cmplw  r11, r22
                      //   bge    -> skip             +3 is the LEVEL
                      //   or     r8, ..., r6         ROW = (+0 hi)<<8 | +2
                      //   or     r9, ..., r20        COL = (+0 lo)<<8 | +1
                      //   srw    r8, r8, r11         both >>= level
                      //   mullw  r7, r6, r8          index = width*ROW + COL
                      //
                      // ps_hft_fback writes o0 = (page X, page Y, LOD, index),
                      // so the guest needs LEVEL <- o0.z (B) at byte +3 and X's
                      // low bits <- o0.x (R) at +1.
                      //
                      // CONFIRMED QUANTITATIVELY: under the wrong order X could
                      // only ever land in 520..776, and every resident
                      // page-table entry sits at x 512..531, 18-20 columns wide.
                      std::swap(tmp[0], tmp[2]);
                      std::swap(tmp[0], tmp[3]);
                      std::swap(tmp[1], tmp[2]);
                    }
                    std::memcpy(REX_RAW_ADDR(at), tmp, bpb);
                    // The low byte of the value the GUEST will load. tmp is
                    // already in guest byte order, so for a 4-byte texel the
                    // guest's (uint8)value is the last byte of tmp.
                    ++low_hist[tmp[bpb - 1]];
                    // EVERY byte, not just the one the mip gate reads. "usable
                    // 4881" says the mip byte is sane, which is enough to
                    // conclude the feed is ALIVE and not that it is CORRECT: the
                    // page x and y ride in the other bytes of the same texel.
                    //
                    // Why look: every resident page-table entry sits at x >=
                    // half the level width, at two different mips, while the
                    // ground being rendered samples x ~401. Two levels starting
                    // at exactly the midpoint is a coordinate with a high bit
                    // stuck on.
                    //
                    //   one byte always >= 0x80  -> the skew is in what WE
                    //                               deliver, guest innocent
                    //   all four span their range -> the addressing is the
                    //                               guest's, and IDA is next
                    //
                    // ONLY THE TEXELS THE GUEST ACTS ON, and cumulative across
                    // writebacks: measuring every texel under a cap of 8 only
                    // ever reported the MENU, where the surface is the guest's
                    // own 0xFF clear.
                    if (tmp[bpb - 1] < 16) {
                      for (uint32_t b = 0; b + 1 < bpb && b < 4; ++b) {
                        ++s_byteSeen[b];
                        if (tmp[b] & 0x80u) ++s_byteHigh[b];
                        if (tmp[b] < s_byteMin[b]) s_byteMin[b] = tmp[b];
                        if (tmp[b] > s_byteMax[b]) s_byteMax[b] = tmp[b];
                      }
                      // Byte +0 packs both high nibbles, exactly as the guest
                      // reads them: ROW = (+0 hi) << 8 | +2, COL = (+0 lo) << 8
                      // | +1. Bucketed to 4 each because a 1024-wide level needs
                      // 10 bits and the nibble carries the top two.
                      ++s_rowHi[(tmp[0] >> 4) & 3u];
                      ++s_colHi[tmp[0] & 3u];
                      ++s_lod[tmp[bpb - 1] & 15u];
                    }
                    ++wrote;
                  }
                }
              }
            }
            if (matched) {
              uint32_t distinct = 0, dominant = 0, dominant_n = 0, usable = 0;
              // THE ACTUAL DISTRIBUTION, not a borrowed gate. `usable` counts
              // texels whose low byte is < 16, which is the guest's MIP-LEVEL
              // test in the feedback walk. For the terrain deformation that byte
              // is a HEIGHT, and the same test reads as "more than half the tile
              // is near zero" -- true, and silent on whether zero is right. The
              // buffer's neutral is 0x80, so min/max/mean is what separates a
              // rut from a trench.
              uint32_t vmin = 255, vmax = 0;
              uint64_t vsum = 0, vcount = 0;
              for (uint32_t v = 0; v < 256; ++v) {
                if (!low_hist[v]) continue;
                ++distinct;
                if (v < vmin) vmin = v;
                if (v > vmax) vmax = v;
                vsum += uint64_t(v) * low_hist[v];
                vcount += low_hist[v];
                if (low_hist[v] > dominant_n) {
                  dominant_n = low_hist[v];
                  dominant = v;
                }
                // A mip level the guest would act on. 16 is generous: a
                // 1024x1024 page table has 11 levels, so anything under 16 is
                // at least plausible and everything above is certainly skipped.
                if (v < 16) usable += low_hist[v];
              }
              static uint32_t s_logged = 0;
              static uint64_t s_writebacks = 0;
              ++s_writebacks;
              if (s_logged++ < 8) {
                REXLOG_INFO(
                    "d3d9: SURFACE WRITEBACK dest 0x{:08X} addr 0x{:08X} "
                    "{}x{} bpb {} tiled {} pitch {} -- wrote {} texels, {} "
                    "unwritable | GUEST-VISIBLE low byte: {} distinct, "
                    "dominant 0x{:02X} x{}, {} of {} usable (< 16)",
                    dest_texture, dest_desc.address, dest_desc.width,
                    dest_desc.height, dest_desc.bytes_per_block,
                    dest_desc.tiled ? 1 : 0, dest_desc.pitch_blocks, wrote,
                    skipped_unwritable, distinct, dominant, dominant_n, usable,
                    wrote);
              }
              // WHICH DESTINATIONS actually get written, uncapped. The `SURFACE
              // WRITEBACK` line stops after 8, and two destinations that queue
              // every frame consumed all eight long before the terrain
              // deformation was ever eligible.
              //
              // PER DESTINATION, because `usable of wrote` is meaningless summed
              // across them: once the deform started landing, its 1-byte texels
              // -- ~0x00 almost everywhere, trivially satisfying the `< 16` mip
              // gate -- were counted under a heading that says "a mip the guest
              // would act on", and ~89% of one 2,842,930 numerator was deform
              // bytes.
              //
              // The byte spread below is not affected: its loop is `b + 1 < bpb`,
              // so a 1-byte texel contributes nothing to it.
              struct WroteTo {
                uint32_t addr;
                uint64_t count;
                uint64_t wrote;
                uint64_t usable;
                uint32_t bpb;
                uint32_t bmin;
                uint32_t bmax;
                uint64_t bsum;
                uint64_t bcount;
              };
              // READ WATCH, armed AFTER the write so the next toucher is
              // somebody else.
              //
              // Scoped to the 129x129 terrain HEIGHT buffer because that is the
              // one whose reader is in doubt: raising the readback cap to
              // deliver it stopped the tile churn but did not put the bike down,
              // and the only consumer we can see binds the host SNAPSHOT instead
              // of decoding this memory. A load leaves no trace unless the
              // memory reports it -- see guest-reads-resolves-from-memory.
              if (dest_desc.width == 129 && dest_desc.height == 129 &&
                  mx::watch::GuestReadWatchActive()) {
                const size_t span = size_t(dest_desc.width) *
                                    size_t(dest_desc.height) *
                                    size_t(dest_desc.bytes_per_block);
                mx::watch::ArmGuestReadWatch(
                    reinterpret_cast<void*>(REX_RAW_ADDR(dest_desc.address)),
                    span, dest_desc.address, "terrain height 129x129");
              }

              static WroteTo s_dests[8] = {};
              static bool s_destsInit = false;
              if (!s_destsInit) {
                for (auto& wd : s_dests) wd.bmin = 255;
                s_destsInit = true;
              }
              static uint64_t s_destOverflow = 0;
              bool placed = false;
              for (auto& wd : s_dests) {
                if (wd.addr == dest_desc.address || !wd.addr) {
                  wd.addr = dest_desc.address;
                  wd.bpb = dest_desc.bytes_per_block;
                  ++wd.count;
                  wd.wrote += wrote;
                  wd.usable += usable;
                  if (vcount) {
                    if (vmin < wd.bmin) wd.bmin = vmin;
                    if (vmax > wd.bmax) wd.bmax = vmax;
                    wd.bsum += vsum;
                    wd.bcount += vcount;
                  }
                  placed = true;
                  break;
                }
              }
              if (!placed) ++s_destOverflow;
              if ((s_writebacks % 120) == 0) {
                std::string dests;
                for (const auto& wd : s_dests) {
                  if (!wd.addr) continue;
                  dests += fmt::format(
                      " 0x{:08X} x{} bpb{} {}/{} usable, byte {:02X}..{:02X} "
                      "mean {:02X}",
                      wd.addr, wd.count, wd.bpb, wd.usable, wd.wrote, wd.bmin,
                      wd.bmax, wd.bcount ? uint32_t(wd.bsum / wd.bcount) : 0u);
                }
                if (s_destOverflow)
                  dests += fmt::format(" (+{} unplaced)", s_destOverflow);
                std::string spread;
                for (uint32_t b = 0; b < 4; ++b) {
                  if (!s_byteSeen[b]) continue;
                  spread += fmt::format(" b{}[{:02X}..{:02X} high {}/{}]", b,
                                        s_byteMin[b], s_byteMax[b],
                                        s_byteHigh[b], s_byteSeen[b]);
                }
                // The share, not the extremes. rowHi 0 and 1 address the
                // TOP HALF of the page table, which is exactly the half that
                // never goes resident.
                std::string lod;
                for (uint32_t l = 0; l < 16; ++l) {
                  if (!s_lod[l]) continue;
                  lod += fmt::format(" L{}={}", l, s_lod[l]);
                }
                const uint64_t rowTot = s_rowHi[0] + s_rowHi[1] + s_rowHi[2] +
                                        s_rowHi[3];
                // rowHi[0..3] must sum to the same total as b0's `seen`.
                // Comparability is the entire basis for reading the TOP HALF
                // percentage against the byte spread, so it is checked rather
                // than asserted in prose.
                mx::gpu::health::Equal("vt.rowhi_matches_b0", rowTot,
                                       s_byteSeen[0]);
                std::string nib;
                if (rowTot) {
                  nib = fmt::format(
                      " | LOD{} | rowHi {}/{}/{}/{} ({:.3f}% in the TOP HALF)"
                      " colHi {}/{}/{}/{}",
                      lod.empty() ? std::string(" (none)") : lod,
                      s_rowHi[0], s_rowHi[1], s_rowHi[2], s_rowHi[3],
                      100.0 * double(s_rowHi[0] + s_rowHi[1]) / double(rowTot),
                      s_colHi[0], s_colHi[1], s_colHi[2], s_colHi[3]);
                }
                REXLOG_INFO(
                    "d3d9: WRITEBACK census: {} writebacks | this frame {} "
                    "distinct low bytes, dominant 0x{:02X} | ACTED-ON byte "
                    "spread{}{} | destinations written{}",
                    s_writebacks, distinct, dominant,
                    spread.empty() ? std::string(" (none yet)") : spread, nib,
                    dests.empty() ? std::string(" (none)") : dests);
              }
            }
          }
        }
        if (dest_desc.width == 1 && dest_desc.height == 1) {
          // Write the GPU's answer where the guest is about to read it.
          // sub_82AFB8A8 resolves the 1x1 and then loads its bytes straight out
          // of guest memory rather than sampling them, so a host-only resolve
          // leaves it reading zero and its exposure comes out as a division by
          // zero. The value is the previous frame's, which is what the console's
          // own latency gives it anyway.
          //
          // FMT_16_FLOAT with endian 1 is an 8-in-16 swap, so the host's
          // little-endian half goes out byte-reversed.
          //
          // Written to EVERY 1x1 destination seen, not just the one this resolve
          // names: sub_82AFB8A8 ping-pongs two of them and READS the one it is
          // not resolving into. They are successive samples of one quantity, so
          // giving both the latest value costs one frame of history.
          if (dest_desc.bytes_per_block == 2)
            g_luminanceDestAddrs[dest_texture] = dest_desc.address;
          const uint32_t seq =
              mx::hle::g_luminanceReadbackSeq.load(std::memory_order_acquire);
          if (seq != g_luminanceWroteSeq) {
            g_luminanceWroteSeq = seq;
            uint32_t wrote = 0, offered = 0;
            std::string all;
            {
              std::lock_guard<std::mutex> lk(
                  mx::hle::g_luminanceReadbackMutex);
              offered = mx::hle::g_luminanceReadbackCount;
              // The newest reading, whichever destination it was resolved into.
              // Every 1x1 destination is a successive sample of ONE quantity —
              // the scene's average luminance — so the latest value is the right
              // answer for all of them.
              bool have_latest = false;
              uint32_t latest_bits = 0;
              for (uint32_t i = 0; i < offered; ++i) {
                const auto& r = mx::hle::g_luminanceReadbacks[i];
                if (!g_luminanceDestAddrs.count(r.destObject)) continue;
                latest_bits = r.bits;
                have_latest = true;
              }
              // EVERY known destination, which is what the note above has always
              // claimed and what the code did NOT do: it matched each readback
              // to its own destObject, so it wrote one of the three known
              // destinations and left the other two at whatever they held. The
              // guest was loading a stale or zero luminance, computing exposure
              // = g_KeyValue / 0 = +Inf, and parking 0x7F800000 in pixel
              // constant c100 -- every shader reading it then output NaN.
              if (have_latest) {
                // NEVER hand the guest a zero. sub_82AFB8A8 DIVIDES by this, so
                // a zero makes exposure +Inf, and the adaptation that consumes
                // it is a feedback filter: once `adapted` is Inf it stays Inf.
                //
                // Zero is our artefact, not the scene's -- it is what the
                // reduction chain reads before it has ever run. The floor is
                // g_MinLuminance (0.075), the value the guest's own pass clamps
                // to, so this cannot push exposure anywhere the guest would not.
                constexpr uint32_t kMinLuminanceHalf = 0x2CCD;  // 0.075
                if ((latest_bits & 0x7FFFu) == 0) {
                  latest_bits = kMinLuminanceHalf;
                  ++g_luminanceFloored;
                }
                const uint16_t be = uint16_t(((latest_bits & 0xFFu) << 8) |
                                             ((latest_bits >> 8) & 0xFFu));
                for (const auto& [obj, addr] : g_luminanceDestAddrs) {
                  if (!HostPageReadable(REX_RAW_ADDR(addr))) continue;
                  *reinterpret_cast<uint16_t*>(REX_RAW_ADDR(addr)) = be;
                  ++wrote;
                  all += fmt::format(" 0x{:08X}=0x{:04X}", addr, latest_bits);
                }
              }
            }
            static uint64_t s_wrote = 0;
            if ((++s_wrote % 600) == 1)
              REXLOG_INFO("d3d9: EXPOSURE writeback #{} seq {} wrote {} of {} "
                          "offered ({} dests known, {} floored):{}",
                          s_wrote, seq, wrote, offered,
                          g_luminanceDestAddrs.size(), g_luminanceFloored, all);
          }
        }
        // Carried to the renderer so the snapshot is sized to the destination
        // TEXTURE rather than to the region this one resolve covers.
        dest_extent_width = dest_desc.width;
        dest_extent_height = dest_desc.height;
        if (first) {
          // DestLevel and DestSliceOrFace, which this hook has never read. A
          // resolve into a level or slice lands at base + that subresource's
          // offset, so ignoring them is a candidate explanation for the atlas
          // whose sampled base sits exactly one 4 KB page above the base
          // recorded here.
          REXLOG_INFO("d3d9: resolve dest addr 0x{:08X} (phys 0x{:08X}) {}x{} "
                      "<- texture 0x{:08X} from surface 0x{:08X} ({}x{}); "
                      "level={} slice={} destpoint={} ({},{}) srcrect={} "
                      "({},{})..({},{})",
                      dest_desc.address, physical, dest_desc.width,
                      dest_desc.height, dest_texture, source->object,
                      source->width, source->height, ctx.r8.u32, ctx.r9.u32,
                      have_dest_point, dest_point[0], dest_point[1],
                      have_src_rect, src_rect[0], src_rect[1], src_rect[2],
                      src_rect[3]);
        }
      }
    }
    // Queue the resolve itself, not just the relationship.
    //
    // Recording the mapping alone left the renderer binding the source target's
    // one live surface to every draw that sampled any texture resolved out of
    // it, and one guest surface is a shared scratch buffer -- six distinct
    // textures were measured resolving from a single target in one run.
    //
    // A resolve has nothing that needs deferred shader finalisation, and putting
    // it in g_pendingHleDraws delayed it until VdSwap while ordinary draws went
    // straight into HleFrameDraws, REVERSING the guest command stream. Every
    // D3D9 hook holds HleGlobalMutex, so inserting directly here is both ordered
    // and synchronized with FinishHleDraw.
    mx::hle::DrawCall resolve{};
    resolve.resolve_dest_texture = dest_texture;
    resolve.resolve_source_object = source->object;
    resolve.resolve_source_is_depth = (source_slot == 4);
    resolve.resolve_source_base = source->color_info & 0xFFFu;
    resolve.resolve_source_width = source->width;
    resolve.resolve_source_height = source->height;
    resolve.resolve_dest_width = dest_extent_width;
    resolve.resolve_dest_height = dest_extent_height;
    // Without an explicit destination point, the source rectangle's origin is
    // the best available answer: a banded resolve names the band's place in the
    // full image there. Zero when neither is supplied, which is the whole
    // -surface case and correct for it.
    resolve.resolve_dest_x =
        have_dest_point ? dest_point[0] : (have_src_rect ? src_rect[0] : 0);
    resolve.resolve_dest_y =
        have_dest_point ? dest_point[1] : (have_src_rect ? src_rect[1] : 0);
    if (have_src_rect) {
      resolve.resolve_src_x1 = src_rect[0];
      resolve.resolve_src_y1 = src_rect[1];
      resolve.resolve_src_x2 = src_rect[2];
      resolve.resolve_src_y2 = src_rect[3];
    }
    NoteQueueThread(GetCurrentThreadId(), true);
    NoteResolvePosition(dest_texture, mx::hle::HleFrameDraws().size());
    mx::hle::HleFrameDraws().push_back(std::move(resolve));
    // D3DRESOLVE_CLEARRENDERTARGET (0x100) is not metadata on the copy: the
    // guest tests this bit after issuing the resolve and calls sub_8255BA10 to
    // clear the source EDRAM surface. Bink relies on exactly that sequence --
    // render FE_Smoke into the shared 1280x720 scratch, resolve its 1280x430
    // texture, then clear the scratch before the following swap resolve.
    if ((resolve_flags & 0x100u) && source_slot < 4) {
      mx::hle::DrawCall clear{};
      clear.clear_color_target = true;
      clear.clear_color_is_float = true;
      clear.render_target_object = source->object;
      clear.render_target_surface_info = source->surface_info;
      clear.render_target_color_info = source->color_info;
      clear.render_target_width = source->width;
      clear.render_target_height = source->height;
      clear.surface_base = source->color_info & 0xFFFu;

      // pClearColor is argument 8 (r10). A null pointer selects the runtime's
      // default float4 at 0x82012FC0, as transcribed from sub_8255BD48.
      uint32_t clear_ptr = ctx.r10.u32;
      if (!clear_ptr) clear_ptr = 0x82012FC0u;
      if (HostPageReadable(REX_RAW_ADDR(clear_ptr)) &&
          HostPageReadable(REX_RAW_ADDR(clear_ptr + 12))) {
        for (uint32_t i = 0; i < 4; ++i) {
          const uint32_t bits = REX_LOAD_U32(clear_ptr + i * 4);
          std::memcpy(&clear.clear_color_float[i], &bits, sizeof(bits));
        }
      }
      mx::hle::HleFrameDraws().push_back(std::move(clear));

      // Was a flat "first 12" cap, which spends its whole budget on whatever
      // happens at boot -- here, twelve identical (0,0,0,0) lines. Keyed on the
      // colour itself instead, so each distinct clear colour reports once
      // however late it first appears.
      static std::set<std::array<uint32_t, 4>> s_resolveClearColors;
      const auto& cc = mx::hle::HleFrameDraws().back().clear_color_float;
      std::array<uint32_t, 4> key{};
      for (uint32_t i = 0; i < 4; ++i) std::memcpy(&key[i], &cc[i], 4);
      if (s_resolveClearColors.insert(key).second &&
          s_resolveClearColors.size() <= 32) {
        const auto& c = mx::hle::HleFrameDraws().back().clear_color_float;
        REXLOG_INFO(
            "d3d9: resolve 0x{:08X} clears source 0x{:08X} after copy to "
            "({:.3f},{:.3f},{:.3f},{:.3f}) flags=0x{:X}",
            dest_texture, source->object, c[0], c[1], c[2], c[3],
            resolve_flags);
      }
    }
    static std::map<uint64_t, uint64_t> s_resolves;
    const uint64_t key = (uint64_t(source->object) << 32) | dest_texture;
    const bool first = s_resolves.emplace(key, 0).second;
    ++s_resolves[key];
    if (first) {
      uint32_t fetch0 = 0;
      if (HostPageReadable(REX_RAW_ADDR(dest_texture + 0x1C)))
        fetch0 = REX_LOAD_U32(dest_texture + 0x1C);
      // FLAGS, in full. Xenia applies an EXPONENT BIAS on resolve --
      // RB_COPY_DEST_INFO.copy_dest_exp_bias, a signed 6-bit field at bit +16
      // read into `exp_bias` in GetResolveInfo. Our resolve is a bitwise
      // CopyTextureRegion and `copy_dest_exp_bias` appears nowhere in this tree,
      // so if the guest ever asks for a scale we drop it and every surface
      // sampled from that resolve reads 2^bias too dark.
      //
      // That is the missing factor the rider's arithmetic demands: its material
      // needs the pre-pass luminance to exceed 1.03, and that surface measures
      // 0.109 and cannot exceed 0.619.
      //
      // The bit position is READ OUT OF THE GUEST, not guessed. Resolve's body
      // sub_8255BD48 builds RB_COPY_DEST_INFO and stores it at device+10788:
      //
      //   v81 = (((((8 * ((v77 << 8) & 0x100 | (v38 >> 26))) | v79 & 7) << 6)
      //           | v73 & 0x3F) << 7) | ((unsigned __int8)v67 >> 6);
      //
      // with v38 the Flags argument -- bits 7-12 destination format, 13-15
      // endian, and bits 16-21 = `v38 >> 26`. Xenia's registers.h puts
      // `int32_t copy_dest_exp_bias : 6` at +16, so the field is the TOP SIX
      // BITS OF Flags, signed.
      const int32_t copy_dest_exp_bias = int32_t(resolve_flags) >> 26;
      REXLOG_INFO(
          "d3d9: resolve slot {} target 0x{:08X} {}x{} -> texture "
          "0x{:08X} FLAGS=0x{:08X} EXP_BIAS={} (x{}) fetch0=0x{:08X} rect={} "
          "({},{})..({},{}) point={} ({},{})",
          source_slot, source->object, source->width, source->height,
          dest_texture, resolve_flags, copy_dest_exp_bias,
          std::exp2(float(copy_dest_exp_bias)), fetch0, have_src_rect,
          src_rect[0], src_rect[1], src_rect[2], src_rect[3], have_dest_point,
          dest_point[0], dest_point[1]);
    }
  }
  // sub_82AFCA38 calls Resolve four times and is where the native frame time
  // goes. Resolve is the EDRAM-to-memory bridge, so it is the call most likely
  // to block on the host side. Timed to confirm or clear it.
  {
    const auto _t0 = std::chrono::steady_clock::now();
    orig_Resolve(ctx, base);
    const auto _ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - _t0)
                         .count();
    if (_ms >= 50) {
      static uint64_t _n = 0;
      REXLOG_INFO("native: Resolve slow #{} {}ms", ++_n, _ms);
    }
  }
}
