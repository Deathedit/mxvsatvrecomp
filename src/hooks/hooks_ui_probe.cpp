// The UI render-list probes: ENQUEUE, DRAIN, EMIT, RENDER ENTRY and DRAW GATE.
//
// Split verbatim out of hooks_plugin_diag.cpp, which had grown past 2600 lines.
// Nothing was renamed, reordered or re-gated. The only change is that the
// cluster's anonymous namespaces became `namespace mx::hooks`, so the two
// report functions can be called from the visit hook that stayed behind --
// internal linkage does not cross a TU. Checked before doing it: none of the 71
// symbols defined here appears anywhere else in src/.
//
// Together these follow one UI draw from the component that owns it down to the
// guest D3D9 draw counter, and they are what established that the engine's UI
// draws through BeginVertices/EndVertices -- a fourth draw entry point this port
// never hooked. See the memory note ui-draws-bypass-hooked-entry-points and the
// "FOURTH draw path" section of docs/guest_binary.md.

#include "hooks/hook_common.h"
#include "hooks/hooks_d3d9.h"  // GuestDrawCalls
#include "hooks/hooks_ui_probe.h"

#include <rex/cvar.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

using namespace mx::hooks;

//-----------------------------------------------------------------------------
// The UI render-list ENQUEUE - sub_8229BAF8
//
// The engine's generic vector push:
//
//     sub_8229BAF8(vec, valuePtr)
//         if (vec[1] >= vec[2]) grow();
//         ((uint32_t*)vec[0])[vec[1]++] = *valuePtr;
//
//     vec[0] data pointer, vec[1] count, vec[2] capacity.
//
// It is the LAST step of a UI component's draw, after both gates:
//
//     sub_8236DB10(this);                     dirty check -> slot 15 rebuild
//     if (sub_8236DB70(this, 4))              the visibility gate: bit 1<<4 of
//                                             +172 on this AND every ancestor
//         sub_8229BAF8(uiRenderList, &this);  ENQUEUE
//
// so an entry here is the closest thing the guest has to "this component will
// draw". The intro logo's consumer - component 0x21A25860, material
// "1280_720_VideoRenderTarget" - passes both gates and still yields no draw
// (FRAME DRAWS reports guest == accepted, so nothing is lost on our side), and
// this probe splits the two remaining causes:
//
//   it never appears here      its draw slot is not being called at all, and
//                              the question moves to whoever walks the tree.
//   it appears but no draw     the DRAIN of this list, or the draw-item -> PM4
//                              path below it, is dropping the entry.
//
// THE LIST ADDRESS IS RESOLVED AT RUN TIME, and that is not a detail. The guest
// LOADS the global and adds the offset:
//
//     lis  r11, dword_82DD7E6C@ha
//     lwz  r11, dword_82DD7E6C@l(r11)     r11 = *(uint32_t*)0x82DD7E6C
//     addi r3, r11, 0x19C                 list = THAT VALUE + 412
//
// Hex-Rays renders the call as `sub_8229BAF8(dword_82DD7E6C + 412, ...)`, which
// reads like an address-of. Comparing r3 against the literal 0x82DD8008 would
// match nothing, on every frame, forever - a counter that cannot fire, which is
// a failure mode this project has already lost a session to. The global is
// re-read on each call rather than cached, so a root object that moves cannot
// leave a stale address silently matching nothing.
//
// sub_8229BAF8 is called from 60+ sites for many unrelated vectors and is HOT,
// so the fast path is one guest load and one compare, with no lock. The total
// push count is an atomic and is reported alongside the filtered count, because
// "the UI list got 0 pushes" and "this function is never called" are completely
// different diagnoses - the same rule as the ENGINE TEX HEADER probe below.
namespace mx::hooks {

constexpr uint32_t kUiRootGlobal = 0x82DD7E6C;
constexpr uint32_t kUiRenderListOff = 0x19C;  // 412

std::atomic<uint32_t> g_uiRenderList{0};  // last resolved address, for the report
std::atomic<uint64_t> g_enqAllPushes{0};  // pushes into ANY vector

struct EnqueueRow {
  uint32_t value = 0;  // the component pointer pushed
  uint64_t pushes = 0;
  uint32_t last_caller = 0;
  uint32_t last_index = 0;
  bool video = false;  // matches a registered video component
};

std::mutex g_enqMu;
std::vector<EnqueueRow> g_enqRows;
uint64_t g_enqUiPushes = 0;
uint64_t g_enqDroppedRows = 0;
uint64_t g_enqRestarts = 0;  // pushes landing at index 0 = the list was drained
uint32_t g_enqHighWater = 0;

// Entries are recorded as RAW POINTERS, not names. Naming them here would mean
// reading the material table or the UI inventory from this hot hook, adding a
// lock-order edge into two other collectors for no diagnostic gain: UI
// INVENTORY already prints name <-> address for every component, so
// cross-referencing is a text search. The one exception is the `video` flag,
// which comes from g_videoCompFast - an array built for exactly this kind of
// lock-free prefilter.
bool IsWatchedVideoComponent(uint32_t component) {
  if (!component) return false;
  for (auto& slot : g_videoCompFast) {
    const uint32_t c = slot.load(std::memory_order_acquire);
    if (!c) break;
    if (c == component) return true;
  }
  return false;
}

// Takes g_enqMu and nothing else. Called from the per-frame UI visit hook while
// that hook holds NO other lock, so this introduces no lock-order edge.
void ReportUiEnqueue(bool force) {
  static std::chrono::steady_clock::time_point s_last{};
  static size_t s_lastRows = 0;
  const auto now = std::chrono::steady_clock::now();
  std::lock_guard<std::mutex> lk(g_enqMu);
  const bool grew = g_enqRows.size() != s_lastRows;
  if (!force && !grew && now - s_last < std::chrono::seconds(3)) return;
  s_last = now;
  s_lastRows = g_enqRows.size();
  std::string rows;
  for (const auto& r : g_enqRows)
    rows += fmt::format(" [0x{:08X}{} pushes{} lastIdx{} caller0x{:08X}]",
                        r.value, r.video ? " VIDEO" : "", r.pushes,
                        r.last_index, r.last_caller);
  REXLOG_INFO("native: UI ENQUEUE list 0x{:08X}; {} pushes into it of {} vector "
              "pushes total; {} distinct entries ({} dropped), {} list "
              "restarts, high water {} --{}",
              g_uiRenderList.load(std::memory_order_relaxed), g_enqUiPushes,
              g_enqAllPushes.load(std::memory_order_relaxed), g_enqRows.size(),
              g_enqDroppedRows, g_enqRestarts, g_enqHighWater,
              rows.empty() ? " (none)" : rows);
}

}  // namespace mx::hooks

REX_IMPORT(__imp__sub_8229BAF8, orig_VectorPush, void());
extern "C" REX_FUNC(sub_8229BAF8) {
  const uint32_t vec = ctx.r3.u32;
  const uint32_t valuePtr = ctx.r4.u32;
  const uint32_t caller = uint32_t(ctx.lr);

  // Read BEFORE the original: the count is what says where this entry landed,
  // and the original increments it. Index 0 is how a drained list announces
  // itself, which is the only view of the DRAIN this probe gets for free.
  const uint32_t index = vec ? REX_LOAD_U32(vec + 4) : 0;
  const uint32_t value = valuePtr ? REX_LOAD_U32(valuePtr) : 0;

  orig_VectorPush(ctx, base);

  // COUNT FIRST, FILTER SECOND - lock-free, so the population is known even
  // when the filter never matches.
  g_enqAllPushes.fetch_add(1, std::memory_order_relaxed);

  const uint32_t root = REX_LOAD_U32(kUiRootGlobal);
  if (!root) return;
  const uint32_t list = root + kUiRenderListOff;
  g_uiRenderList.store(list, std::memory_order_relaxed);
  if (vec != list) return;  // fast reject: some other vector

  std::lock_guard<std::mutex> lk(g_enqMu);
  ++g_enqUiPushes;
  if (index == 0) ++g_enqRestarts;
  if (index + 1 > g_enqHighWater) g_enqHighWater = index + 1;
  for (auto& r : g_enqRows) {
    if (r.value != value) continue;
    ++r.pushes;
    r.last_caller = caller;
    r.last_index = index;
    return;
  }
  if (g_enqRows.size() >= 64) {
    ++g_enqDroppedRows;
    return;
  }
  g_enqRows.push_back({value, 1, caller, index, IsWatchedVideoComponent(value)});
}

//-----------------------------------------------------------------------------
// The UI render-list DRAIN - sub_822F9800
//
// The matching element accessor for the push:
//
//     sub_822F9800(vec, i)  { return ((uint32_t*)vec[0])[i]; }
//     sub_8229BAF8(vec, v)  { ((uint32_t*)vec[0])[vec[1]++] = *v; }
//
// so whatever consumes the UI render list has to come through here with
// `vec == UIManager + 0x19C`. That matters because the drain could NOT be
// pinned statically:
//
//   - `dword_82DD7E6C` is the **UIManager** singleton (named from its dtor
//     sub_82390A10, which does `sub_8251C428(a1 + 412)` - the vector destructor
//     for this very list - and then clears the global).
//   - The four sites that form `root + 0x19C` are all ENQUEUES
//     (sub_8237A6D0, sub_8237B1D0, sub_8237B408, sub_82395068). Nothing else
//     forms the address, so the consumer reaches the vector through `this`
//     rather than through the global, and an address-formation search cannot
//     find it.
//   - Searching for the count field `0x1A0(rX)` across the UI range returned
//     five sites, none of them a drain, and both UIManager vtables have a
//     destructor in slot 0.
//
// So instead of guessing, this probe makes the CALLER NAME ITSELF: the link
// register on a filtered call is the drain, recorded per distinct caller. The
// same trick already worked for slot 15 in the VIDEO COMPONENT RENDER probe,
// where static hunting for the virtual call site produced 40+ candidates and
// not one was right.
//
// Same runtime-resolution rule as the enqueue probe: the list is
// `*(uint32_t*)0x82DD7E6C + 0x19C`, re-read per call, never the literal
// `0x82DD7E6C + 412`.
//
// Read it against UI ENQUEUE, which shares the population:
//
//   a component enqueued but never read here   the drain skips it - the defect
//                                              is the drain's own filter
//   read here but still no draw                the consumer runs and what it
//                                              does per entry is the next step,
//                                              and `caller` names it
//   zero reads, callers empty                  nothing drains this list at all
namespace mx::hooks {

struct DrainCallerRow {
  uint32_t caller = 0;
  uint64_t reads = 0;
};

struct DrainValueRow {
  uint32_t value = 0;
  uint64_t reads = 0;
  bool video = false;
};

// ---- the EMIT, sub_82B268A8 -------------------------------------------
//
// The last step before a UI item becomes a draw:
//
//     v4 = *(submitter + 104);                  // the installed item
//     if ((*(v4 + 212) & 0xFF000000) != 0) {    // the gate
//         sub_82B26860(*(short*)(v4 + 248), ..) // sort key
//         dword_830BC7E8++                      // append to the pool
//         sub_82AF1990(ctx + 120)               // insert into the queue
//     }
//
// The question this answers: does EMITTED imply DRAWN? The menu's UI pass
// contains ~70 draws (rdc/mmmmmm.rdc, events 9168..10093). If the emit
// fires ~70 times per frame there, then everything emitted becomes a draw
// and the video's emit -- which is proven to happen, with every gate passing
// -- should too, so the contradiction lies further down. If the emit fires
// MORE than the draw count, the renderer is dropping emitted items and the
// difference between kept and dropped is the defect.
//
// This deliberately does not depend on the image-family population being
// large enough to reason about: it compares two counts on the SAME frames.
// ---- the per-entry RENDER ACTION, sub_82B2B120 -------------------------
//
// Slot 1 of the queued entry's own vtable (off_8213E258, pre-installed into all
// 500 pool slots by the initialiser sub_82CE4B50). This is where a queued UI
// record either becomes a draw or silently does not:
//
//     // entry+4 = submitter, entry+8 = item, as stored by the emit
//     (*(submitter->vtbl + 12))(submitter, item, 0);   // re-install
//     sub_82B28AA0(submitter);
//     if (!*(item + 236)) sub_82B267C0();              // fallback
//     if ( *(item + 236)) {                            // THE DRAW GATE
//         v4 = *(submitter + 96);
//         if (!v4 || *(int*)(v4 + 4) > 0)
//             sub_82B2AF40(submitter + 16, submitter, v4, 0);   // the draw
//     }
//
// UI EMIT already proved 1710 items are queued with every earlier gate passing,
// while the intro frame contains zero UI draws. This closes the last span:
// entries in vs draws out, on the same frames, with the reason separated rather
// than inferred.
//
// NOTE the offset collision: 236 is kCompDrawItem on a COMPONENT, and this is
// +236 on the ITEM. Different objects, same number, unrelated -- hence its own
// constant rather than reusing kCompDrawItem.
constexpr uint32_t kEntrySubmitter = 4;
constexpr uint32_t kEntryItem = 8;
constexpr uint32_t kItemDrawGate = 236;
constexpr uint32_t kSubmitterGeom = 96;
constexpr uint32_t kItemMaterial = 164;
constexpr uint32_t kMaterialSub = 16;
constexpr uint32_t kMaterialEntryCount = 20;
constexpr uint32_t kMaterialEntryArray = 24;
constexpr uint32_t kMaterialEntryResource = 16;
constexpr uint32_t kMaxMaterialEntries = 64;
// The outermost gate in sub_82B296B0, which is the function that actually sets
// D3D9 state and draws:
//
//     v12 = *(submitter + 104);          // the item
//     v13 = *(v12 + 164);                // material
//     v14 = *(v12 + 168);
//     if ((a3 || v14) && v13) { ...every SetTexture, every draw... }
//
// with a3 = *(submitter + 96), which sub_82B2B120 explicitly allows to be null.
// So both a3 AND +168 null means the whole body is skipped in silence -- no
// state, no draw, no return value to notice.
//
// v13 is NOT a candidate: sub_82B267C0 only sets item+236 when *(item+164) is
// non-null, and 906/906 came back READY, so the material is definitely there.
// That leaves (a3 || v14) as the only way this can fail, which is the
// prediction being tested.
constexpr uint32_t kItemAltGeom = 168;

std::mutex g_entryMu;
uint64_t g_entryCalls = 0;      // queued entries that reached the action
uint64_t g_entryDrawn = 0;      // item+236 non-null -> the draw ran
uint64_t g_entryNoGate = 0;     // item+236 NULL -> silently no draw
uint64_t g_entryBadItem = 0;    // entry carried no usable item pointer
uint64_t g_entryGeomEmpty = 0;  // gate passed but submitter+96 count <= 0
uint32_t g_entryLastItem = 0;
uint32_t g_entryLastGate = 0;

// READ AFTER THE ORIGINAL. The first cut of this probe sampled item+236 on
// ENTRY and reported "881 no-gate", which is not a defect at all: the item is
// bump-allocated fresh every frame, so +236 is null on entry in a WORKING frame
// too. sub_82B2B120 calls the builder sub_82B267C0 first and only then tests
// the field. Sampling before the call measured a value that is null by
// construction -- a probe that always fires. The post value is the answer.
uint64_t g_entryReady = 0;     // +236 non-null AFTER the builder ran -> drew
uint64_t g_entryNotReady = 0;  // still null after the builder -> no draw

// Why the builder leaves it null, read straight out of sub_82B267C0:
//
//     mat = *(item + 164);  sub = *(mat + 16);
//     *(item + 236) = 1;                       // optimistic
//     n = *(sub + 20);  arr = *(sub + 24);
//     for (i = 0; *(*(arr + i) + 16); i += 4)  // EVERY entry needs +16
//         if (++k >= n) return;                // all present -> stays 1
//     *(item + 236) = 0;                       // one was null -> not ready
//
// So it is a material-RESIDENCY test: each of the material's n entries must
// carry a non-null +16. On the video component the material is
// "1280_720_VideoRenderTarget", and +16 on a render-target asset is the texture
// object itself (sub_8253E1B0 reads it that way). Walking the same array here
// names WHICH entry is missing instead of leaving it to inference.
uint32_t g_entryLastMat = 0;
uint32_t g_entryLastMatCount = 0;
uint32_t g_entryFirstBadIdx = 0xFFFFFFFFu;
uint32_t g_entryLastBadEntry = 0;
uint64_t g_entryWalkAborted = 0;  // array looked implausible; nothing inferred

// Does the guest's UI draw actually reach D3D9?
//
// Every stage from the component down to sub_82B2AF40 now measures as passing,
// and FRAME DRAWS reports guest == accepted, so the only span left is whether
// sub_82B296B0 (below the draw call) ever enters a D3D9 entry point.
// GuestDrawCalls() counts guest D3D9 draws in BOTH modes, so bracketing the
// original answers it directly.
//
// HOW FAR TO TRUST THE DELTA. It is a process-wide counter and this title
// submits draws from several threads (see device-state-is-thread-local), so a
// non-zero delta MAY include another thread's draws and is only suggestive.
// A delta of ZERO is the strong direction: it means no guest draw at all
// occurred anywhere while this entry ran, which no concurrent thread can fake.
// The two are counted separately rather than averaged, and the sum is kept so a
// handful of large deltas cannot masquerade as one-per-entry.
uint64_t g_entryDrawDelta = 0;    // summed
uint64_t g_entryWithDraw = 0;     // READY entries where the counter moved
uint64_t g_entryZeroDraw = 0;     // READY entries where it did NOT move
uint64_t g_entryMaxDelta = 0;

// The sub_82B296B0 entry gate, split so the answer is not inferred from a
// single combined counter: which of the two operands is null matters for the
// fix, and "both null" is the only combination that kills the draw.
uint64_t g_gateGeomNull = 0;   // *(submitter + 96) == 0
uint64_t g_gateAltNull = 0;    // *(item + 168)     == 0
uint64_t g_gateBothNull = 0;   // -> body skipped entirely
uint64_t g_gateMatNull = 0;    // *(item + 164) == 0; expected to stay 0
uint32_t g_gateLastAlt = 0;
uint32_t g_gateLastMat2 = 0;

constexpr uint32_t kSubmitterCurrentItem = 104;
constexpr uint32_t kItemEmitGate = 212;

std::mutex g_emitMu;
uint64_t g_emitCalls = 0;      // every call, gate or no gate
uint64_t g_emitPassed = 0;     // gate passed -> queued
uint64_t g_emitNoItem = 0;     // submitter had no installed item
uint64_t g_emitThisFrame = 0;  // reset when the drain restarts at index 0
uint64_t g_emitLastFrame = 0;
uint64_t g_emitMaxFrame = 0;
uint64_t g_drainPasses = 0;    // times the drain started at index 0
uint32_t g_emitLastItem = 0;
uint32_t g_emitLastGate = 0;

std::mutex g_drainMu;
std::vector<DrainCallerRow> g_drainCallers;  // who consumes the list
std::vector<DrainValueRow> g_drainValues;    // which components it reads
std::atomic<uint64_t> g_drainAllReads{0};    // accessor calls on ANY vector
uint64_t g_drainListReads = 0;               // accessor calls on OUR list
uint64_t g_drainDroppedValues = 0;
uint32_t g_drainMaxIndex = 0;

// Takes g_drainMu and nothing else, like ReportUiEnqueue. Called from the
// per-frame UI visit hook while that hook holds no other lock.
void ReportUiDrain(bool force) {
  static std::chrono::steady_clock::time_point s_last{};
  static size_t s_lastCallers = 0;
  const auto now = std::chrono::steady_clock::now();
  std::lock_guard<std::mutex> lk(g_drainMu);
  const bool grew = g_drainCallers.size() != s_lastCallers;
  if (!force && !grew && now - s_last < std::chrono::seconds(3)) return;
  s_last = now;
  s_lastCallers = g_drainCallers.size();
  std::string callers;
  for (const auto& c : g_drainCallers)
    callers += fmt::format(" [caller0x{:08X} reads{}]", c.caller, c.reads);
  std::string values;
  for (const auto& v : g_drainValues)
    values += fmt::format(" [0x{:08X}{} reads{}]", v.value,
                          v.video ? " VIDEO" : "", v.reads);
  {
    std::lock_guard<std::mutex> nlk(g_entryMu);
    REXLOG_INFO("native: UI RENDER ENTRY {} entries = {} READY(drew) + {} "
                "NOT-READY + {} bad-item + {} geom-empty ({} null on entry, "
                "which is normal); last item 0x{:08X} mat 0x{:08X} "
                "entries {} first-missing {} bad-entry 0x{:08X} aborted {}",
                g_entryCalls, g_entryReady, g_entryNotReady,
                g_entryBadItem, g_entryGeomEmpty, g_entryNoGate,
                g_entryLastItem, g_entryLastMat, g_entryLastMatCount,
                g_entryFirstBadIdx == 0xFFFFFFFFu
                    ? std::string("none")
                    : std::to_string(g_entryFirstBadIdx),
                g_entryLastBadEntry, g_entryWalkAborted);
    REXLOG_INFO("native: UI RENDER DRAW of {} READY entries: {} moved the guest "
                "D3D9 draw counter, {} did NOT; total delta {}, max {} "
                "(delta is process-wide and this title draws on several "
                "threads, so >0 is suggestive; ==0 is the strong signal)",
                g_entryReady, g_entryWithDraw, g_entryZeroDraw,
                g_entryDrawDelta, g_entryMaxDelta);
    REXLOG_INFO("native: UI DRAW GATE (sub_82B296B0 needs (geom96 || alt168) && "
                "mat164) over {} READY: geom96 null {}, alt168 null {}, BOTH "
                "null {} <- body skipped, mat164 null {}; last alt168 0x{:08X} "
                "mat164 0x{:08X}",
                g_entryReady, g_gateGeomNull, g_gateAltNull, g_gateBothNull,
                g_gateMatNull, g_gateLastAlt, g_gateLastMat2);
  }
  {
    std::lock_guard<std::mutex> elk(g_emitMu);
    REXLOG_INFO("native: UI EMIT {} calls = {} queued + {} gate-failed + {} "
                "no-item; per frame last {} max {} over {} drain passes; "
                "last item 0x{:08X} gate 0x{:08X}",
                g_emitCalls, g_emitPassed,
                g_emitCalls - g_emitPassed - g_emitNoItem, g_emitNoItem,
                g_emitLastFrame, g_emitMaxFrame, g_drainPasses,
                g_emitLastItem, g_emitLastGate);
  }
  REXLOG_INFO("native: UI DRAIN list 0x{:08X}; {} reads of it of {} accessor "
              "calls total, max index {}; {} callers --{}; {} values ({} "
              "dropped) --{}",
              g_uiRenderList.load(std::memory_order_relaxed), g_drainListReads,
              g_drainAllReads.load(std::memory_order_relaxed), g_drainMaxIndex,
              g_drainCallers.size(), callers.empty() ? " (none)" : callers,
              g_drainValues.size(), g_drainDroppedValues,
              values.empty() ? " (none)" : values);
}

}  // namespace mx::hooks

REX_IMPORT(__imp__sub_82B2B120, orig_UiRenderEntry, void());
extern "C" REX_FUNC(sub_82B2B120) {
  const uint32_t entry = ctx.r3.u32;
  uint32_t submitter = 0, item = 0, gate = 0, geom = 0;
  bool have_item = false;
  if (PlausibleGuestPtr(entry)) {
    submitter = REX_LOAD_U32(entry + kEntrySubmitter);
    item = REX_LOAD_U32(entry + kEntryItem);
    have_item = PlausibleGuestPtr(item);
    if (have_item) gate = REX_LOAD_U32(item + kItemDrawGate);
    if (PlausibleGuestPtr(submitter))
      geom = REX_LOAD_U32(submitter + kSubmitterGeom);
  }

  const uint64_t draws_before = GuestDrawCalls();
  orig_UiRenderEntry(ctx, base);
  const uint64_t draws_after = GuestDrawCalls();
  const uint64_t draw_delta =
      draws_after > draws_before ? draws_after - draws_before : 0;

  // AFTER: sub_82B2B120 runs the builder before testing the field, so this
  // is the value the draw decision actually used.
  const uint32_t gate_post =
      have_item ? REX_LOAD_U32(item + kItemDrawGate) : 0;

  std::lock_guard<std::mutex> lk(g_entryMu);
  ++g_entryCalls;
  if (!have_item) {
    ++g_entryBadItem;
    return;
  }
  g_entryLastItem = item;
  g_entryLastGate = gate;
  if (!gate) ++g_entryNoGate;  // pre-state only: null here is normal

  if (!gate_post) {
    ++g_entryNotReady;
    // The builder gave up. Walk the same array it walks and record the first
    // entry whose +16 is null -- that is the resource the material is waiting
    // on. Bounded, and an implausible array is COUNTED as aborted rather than
    // guessed at, so a walk that could not run never looks like a clean result.
    const uint32_t mat = PlausibleGuestPtr(item)
                             ? REX_LOAD_U32(item + kItemMaterial)
                             : 0;
    g_entryLastMat = mat;
    const uint32_t sub = PlausibleGuestPtr(mat)
                             ? REX_LOAD_U32(mat + kMaterialSub)
                             : 0;
    if (!PlausibleGuestPtr(sub)) {
      ++g_entryWalkAborted;
      return;
    }
    const uint32_t n = REX_LOAD_U32(sub + kMaterialEntryCount);
    const uint32_t arr = REX_LOAD_U32(sub + kMaterialEntryArray);
    g_entryLastMatCount = n;
    if (n > kMaxMaterialEntries || !PlausibleGuestPtr(arr)) {
      ++g_entryWalkAborted;
      return;
    }
    for (uint32_t k = 0; k < n; ++k) {
      const uint32_t e = REX_LOAD_U32(arr + k * 4);
      if (!PlausibleGuestPtr(e)) {
        ++g_entryWalkAborted;
        return;
      }
      if (!REX_LOAD_U32(e + kMaterialEntryResource)) {
        g_entryFirstBadIdx = k;
        g_entryLastBadEntry = e;
        return;
      }
    }
    return;
  }

  // Ready. The guest reached the draw call, but still skips when the geometry
  // object exists and reports a count <= 0, so that is split out rather than
  // folded into the drawn count.
  if (geom && PlausibleGuestPtr(geom) &&
      static_cast<int32_t>(REX_LOAD_U32(geom + 4)) <= 0) {
    ++g_entryGeomEmpty;
    return;
  }
  ++g_entryReady;
  // The sub_82B296B0 gate, evaluated on the same operands the guest uses.
  {
    const uint32_t alt = REX_LOAD_U32(item + kItemAltGeom);
    const uint32_t mat2 = REX_LOAD_U32(item + kItemMaterial);
    g_gateLastAlt = alt;
    g_gateLastMat2 = mat2;
    if (!geom) ++g_gateGeomNull;
    if (!alt) ++g_gateAltNull;
    if (!geom && !alt) ++g_gateBothNull;
    if (!mat2) ++g_gateMatNull;
  }
  // Only counted on the READY path: a skipped entry is not expected to draw, so
  // folding those in would dilute the very ratio being measured.
  g_entryDrawDelta += draw_delta;
  if (draw_delta > g_entryMaxDelta) g_entryMaxDelta = draw_delta;
  if (draw_delta)
    ++g_entryWithDraw;
  else
    ++g_entryZeroDraw;
}

REX_IMPORT(__imp__sub_82B268A8, orig_UiEmit, void());
extern "C" REX_FUNC(sub_82B268A8) {
  const uint32_t submitter = ctx.r3.u32;
  // Read BEFORE the original: it is what consumes the installed item, and
  // the gate word is the thing being measured.
  uint32_t item = 0, gate = 0;
  if (PlausibleGuestPtr(submitter))
    item = REX_LOAD_U32(submitter + kSubmitterCurrentItem);
  const bool have_item = PlausibleGuestPtr(item);
  if (have_item) gate = REX_LOAD_U32(item + kItemEmitGate);

  orig_UiEmit(ctx, base);

  std::lock_guard<std::mutex> lk(g_emitMu);
  ++g_emitCalls;
  if (!have_item) {
    ++g_emitNoItem;
    return;
  }
  g_emitLastItem = item;
  g_emitLastGate = gate;
  // Same predicate the guest uses, evaluated on the same word.
  if (gate & 0xFF000000u) {
    ++g_emitPassed;
    ++g_emitThisFrame;
  }
}

REX_IMPORT(__imp__sub_822F9800, orig_VectorAt, void());
extern "C" REX_FUNC(sub_822F9800) {
  const uint32_t vec = ctx.r3.u32;
  const uint32_t index = ctx.r4.u32;
  const uint32_t caller = uint32_t(ctx.lr);

  orig_VectorAt(ctx, base);

  // COUNT FIRST, FILTER SECOND - lock-free, so "the list is never read" is
  // distinguishable from "this accessor is never called".
  g_drainAllReads.fetch_add(1, std::memory_order_relaxed);

  const uint32_t root = REX_LOAD_U32(kUiRootGlobal);
  if (!root) return;
  const uint32_t list = root + kUiRenderListOff;
  if (vec != list) return;  // fast reject: some other vector

  // The value read out. Taken from guest memory rather than the return
  // register so this does not depend on how the recompiler surfaces r3.
  const uint32_t data = REX_LOAD_U32(vec);
  const uint32_t value = data ? REX_LOAD_U32(data + index * 4) : 0;

  // Frame roll. The drain walks 0..count-1 once per UIManager::Render, so a
  // read at index 0 is the start of a frame's drain -- the only per-frame
  // boundary available in this file without reaching into the swap hook.
  if (index == 0) {
    std::lock_guard<std::mutex> elk(g_emitMu);
    ++g_drainPasses;
    g_emitLastFrame = g_emitThisFrame;
    if (g_emitThisFrame > g_emitMaxFrame) g_emitMaxFrame = g_emitThisFrame;
    g_emitThisFrame = 0;
  }

  std::lock_guard<std::mutex> lk(g_drainMu);
  ++g_drainListReads;
  if (index > g_drainMaxIndex) g_drainMaxIndex = index;

  bool seen = false;
  for (auto& c : g_drainCallers) {
    if (c.caller != caller) continue;
    ++c.reads;
    seen = true;
    break;
  }
  if (!seen && g_drainCallers.size() < 16)
    g_drainCallers.push_back({caller, 1});

  for (auto& v : g_drainValues) {
    if (v.value != value) continue;
    ++v.reads;
    return;
  }
  if (g_drainValues.size() >= 64) {
    ++g_drainDroppedValues;
    return;
  }
  g_drainValues.push_back({value, 1, IsWatchedVideoComponent(value)});
}
