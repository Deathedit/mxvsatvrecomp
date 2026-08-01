# LoaderTick & Render Block — Why Natural Entity Loading Cannot Work

Deep technical analysis of the LoaderTick renderer block, the `sub_82B34998` dispatch, the eng+8 writer trace, and the EndFrame #2 hang. Investigation notes preserved here; AGENTS.md holds the operational summary.

Reference: AGENTS.md (operational hub), docs/ida_notes.md (IDA bookmarks).

---

## LoaderTick: Why It Cannot Work

1. a1 = 0x830EC248 (valid, not NULL)
2. a1+8 = 0x407F2190 (valid vtable 0x8204C08C)
3. Events at +190/+194 = 0 (renderer init skipped)
4. Mid-ASM hooks skip ALL entity code
5. Without entity code: no entities, no draw calls
6. Removing hooks: entity code crashes accessing GPU state
7. Circular: loading needs GPU, GPU needs entities

**Current approach**: REX_HOOK_RAW calls orig with 100-iteration limit (entities/renderer skipped by mid-ASM), forces r3=0 to complete loading.

---

## PATH 1 EXPERIMENT — cascade of hang points discovered (2026-07-31)

Attempted Path 1 (pre-populate `dword_830BE190` from main-thread context) and bisected through FIVE cascaded hang points before hitting the same fundamental blocker (entity rendering needs `sub_82B34998` to work). The path itself is implementable but requires confronting each hang — and the natural renderer dispatch (`sub_82B34998`) cannot be made to complete under the no-GPU-plugin profile. Full experiment reverted; baseline restored.

**Sequence of cascading failure points (all discovered in this experiment)**:

| Stage | Fix applied | Outcome (next hang exposed) |
|--------|-------------|-----------------------------|
| 0. Baseline-disabled hook #6 | n/a (just pre-populate via `REX_CALL_INDIRECT_FUNC(0x82B3C7D0)` in SetupRenderer hook, then disable hook #6) | SetupRenderer itself hung immediately after pre-population completed |
| 1. Register pollution | Save/restore `r0/r3-r12/lr/ctr` around the `REX_CALL_INDIRECT_FUNC` call (sub_82B3C7D0 trashes volatiles — orig_SetupRenderer saw garbage EngineInit inputs) | SetupRenderer reached GraphicsInit/TexManager/skip-vtable[8]/skip-vtable[17] then hung in the 16 bytes between 0x82B71314 and 0x82B71324 |
| 2. Hook #5 fire-address | Move hook #5 address from 0x82B71324 → 0x82B71314 (BEFORE the cached-check; otherwise `dword_830BE190 != 0` routes `bne` TO loc_82B71338, around hook #5's fire addr — letting natural SetupRenderer code at 0x82B71338..0x82B7168C run, which hangs) | SetupRenderer now RETURNED ✓; Transition thread started ✓, but orig_LoaderTick never returned (no LoaderTick #N logs) |
| 3. Hook #8 register-setup skip | Disable hook #8 (NativeSkipLoaderAll @ 0x82B70DFC). It REPLAC `mr r23, r3` (preserves transition renderer ptr) AND skips over `lis r21, dword_830BE400@ha` at 0x82B70E14. The renderer block needs BOTH: `lfs f31, 0x18(r23)` at 0x82B70ECC and `lwz r11, dword_830BE400@l(r21)` at 0x82B70EFC + 0x82B70F64. With r23+r21 garbage, hang was immediate at `lfs f31, 0x18(r23)`. Hook #7 (NativeSkipLoaderEarly @ 0x82B70E18) takes over — fires AFTER r23+r21 setup, jumps to 0x82B70EC8 | Still no PreDispatch #1 — orig_LoaderTick still stuck |
| 4. Wait hook | Always return SUCCESS from `sub_82BFB740`. Previously hook tried `orig_Wait` first call (with -1 timeout it blocks FOREVER — the guest event at +0x194 is never signaled because SetupRenderer's natural NtSetEvent code was skipped by hook #5). Subsequent calls aged past 3s fallback OK; first call hung | Still no PreDispatch #1 — orig_LoaderTick still stuck after Wait returned |
| 5. Timing function | Stub `sub_82B70370` to no-op return. The natural `sub_82B70370` calls QPC primitives (sub_82BFC728/QPC, sub_82BFC748/perf-freq) and runs a busy-wait loop `while (v7 < *(a1+20))` until timing threshold met — under our recomp with non-real QPC values, this loop spins forever. LoaderTick never reached hook #7 (NativeSkipLoaderEarly at 0x82B70E18) | **PreDispatch #1 finally fired** at 18:13:21.201 — execution reached loc_82B70EF0. Cached-check `bne` correctly takes the branch past the lazy-init. PreDispatch #2 never fired → `bl sub_82B34998` at 0x82B70EF4 hangs on first call |
| 6. Renderer dispatch | Install `NativeSkipRendererDispatch` mid-ASM hook at 0x82B70EF4 with `jump_address = 0x82B70EF8` (skips the `bl sub_82B34998` instruction itself) — fires + logs once then continues into post-dispatch body | **SkipRendererDispatch #1 fired**, but #2 never did — the post-dispatch body (0x82B70EF8..0x82B710BC) hangs on its first pass somewhere |

**Post-dispatch body unreached** (full disasm logged in IDA at 0x82B70EF8..0x82B710BC):

| Address | Call/code | Notes |
|---------|-----------|-------|
| 0x82B70EF8..0x82B70F1C | entity loop 3: `lwz r11, dword_830BE400@l(r21); lwzx r3, r31, r11; lwz r10, 0x3C(r3); cmpwi cr6; beq loc_82B70F14; bl sub_82B237B0` (loop: r31 from 0x1C to 0x24 by 4) | Calls `sub_82B237B0` for engine sub-entities with `*0x3C != 0` — untested |
| 0x82B70F48 | `bl sub_82B67D98 (lazy-init dword_82D6F144)` | untested |
| 0x82B70F54 | `bl sub_82BDC040` | untested |
| 0x82B70F5C | `bl sub_82B676B8` | untested |
| 0x82B70F60..0x82B70F78 | entity loop 4: `bl sub_82AFF120` (no conditional — fires for every entity from r31=0x1C..0x24) | untested |
| 0x82B70F7C..0x82B70FA0 | entity loop 5: `lwz r10, 0x3C(r3); cmpwi cr6, r10, 0; beq loc_82B70F98; bl sub_82B0A0D0` (conditional like loop 3) | untested |
| 0x82B70FA8..0x82B70FAC | `lwz r3, 0x2E0(r23); bl sub_82BFB740` — Wait on event at +0x2E0 (different event from +0x194) | likely safe — our hook now returns SUCCESS unconditionally |
| 0x82B70FDC, 0x82B70FE8 | second `bl sub_82B67D98`, `bl sub_82BDC040` (lazy-init path, conditional on dword_82D6F144 flag bit 0) | appears to be called twice before/after Wait; both untested |
| 0x82B70FF0 | `bl sub_82B676B8` — second call | untested |
| 0x82B70FF4..0x82B71004 | `lwz r3, 0x190(r23); bl sub_82BFB748` (NtSetEvent on event at +0x190) + `bl sub_82BFBF48` (ErrorRecovery fallback if NtSetEvent returns 0) | sub_82BFBF48 is our stubbed ErrorRecovery |
| 0x82B71014..0x82B710A0 | outer scan loop iterating engine sub-entities (`r24 = 0x24 down by 4`): for each iteration, `lwzx r31, r24, r11; bl sub_82B1F410` if entity has `*0x3C != 0`; then nested per-iteration bl `sub_82B6FF78`, `sub_823EDD40` | many untested callees |
| 0x82B710A4..0x82B710B8 | final scene-manager vtable call: `lwz r11, dword_830BE400@l(r21); lwz r3, 0xC(r11); lwz r11, 0(r3); lwz r10, 0xC(r11); mtctr r10; bctrl` (engine[0xC]->vt[3]()) | engine[0xC] = SceneManager; engine[0xC]->vt[3] untested |
| 0x82B710BC | `li r3, 1; addi r1, r1, 0xC0; lfd f31, var_68(r1); b __restgprlr_21` — end of renderer block epilogue, returns r3=1 | safe |

**Decision: REVERT**. Implementing all the necessary stubs to navigate the post-dispatch body is multi-iteration work; the dispatch call (`sub_82B34998`) itself is GPU-plugin-dependent entity rendering — making it work requires replacing it with a host-side renderer, the same as we already do via our D3D12 game-RT triangle. The pre-population cascade taught us where every hang is, but completing LoaderTick naturally requires either:
  - (a) Stubbing every entity-render callee to no-op + replacing sub_82B34998 entirely with our D3D12 game RT path, then trimming entity code at the scene manager vt[3] gating call (return truthy → render path) — OR
  - (b) Replacing LoaderTick's body wholesale from the C++ hook (don't call orig_LoaderTick at all — directly compute the result for Transition wrapper's loop) — simpler but bypasses all natural entity code (consistent with current baseline behavior: LoaderTick iterates 100 times then r3=0 cap).

The current baseline (option b essentially — hook #6 skips the entire renderer block) remains the best of the working options.

---

## sub_82B34998 structural post-mortem — vtable dispatches are FATAL terminators by design (2026-07-31)

Following Path 1's collapse to "bl sub_82B34998 hangs", drilled into sub_82B34998's internal callees and vtable dispatches. Decoded the vtable `off_8213F70C` (installed by the `sub_82B38558` constructor that runs after our pre-populated `sub_82B3C7D0` alloc):

**vtable off_8213F70C entries** (big-endian u32 read from data section):

| Slot | Offset | Address | Function / role |
|------|--------|---------|-----------------|
| vt[0] | +0  | 0x82B3C828 | (function) |
| vt[1] | +4  | 0x82B38830 | |
| vt[2] | +8  | 0x82B2C270 | |
| vt[3] | +12 | **0x82BDB190** | **terminator** |
| vt[4] | +16 | 0x82B35FE0 | |
| vt[5] | +20 | 0x82B2C2E8 | |
| vt[6] | +24 | **0x82BDB190** | **terminator** |
| vt[7] | +28 | **0x82BDB190** | **terminator** ← called by sub_82B34998's `(*a1->vt[7])(a1, f1)` at 0x82B34A24 |
| vt[8] | +32 | 0x82B2C4C8 | `sub_82B2C4C8` — iterates 4 entity slots, calls vt[15]/vt[16] |
| vt[9] | +36 | 0x82426FF0 | `nullsub_1` |
| vt[10] | +40 | 0x82B38A48 | |
| vt[11] | +44 | 0x82B38CC0 | |
| vt[12] | +48 | 0x82B38D90 | |
| vt[13] | +52 | 0x82B2C738 | |
| vt[14]–vt[19] | +56–+76 | **0x82BDB190** ×6 | **terminators** |

**Fatality chain of `sub_82BDB190`** (called as vt[7] / vt[15] / vt[16]):
```
sub_82BDB190 (76 bytes, __noreturn)
 ├─ if (dword_83132F10) dword_83132F10();   ← indirect function ptr (likely NULL/handler hook)
 ├─ sub_82BE62F0(25);                        ← calls sub_82BE62A8 then sub_82C09198
 ├─ v0 = sub_82BDEDE0(0, 1);
 └─ sub_82BDED90(v0);    ← void __noreturn
      ├─ if (sub_82BF0820()) sub_82BF0838(22);     ← raise guard 0x16
      ├─ if (dword_82D584D0 & 2) sub_82BDAA28(3, 0x40000015, 1);
                                                     ↑ STATUS_FATAL_APP_EXIT (0x40000015) — KeBugCheckEx-style
      └─ sub_82BEE290(3);                          ← final exit
```

**Conclusion**: The `off_8213F70C` vtable is a stub/proxy vtable shipped by the original game — its vt[3,6,7,14-19] entries deliberately dispatch to a kernel killed-fatal handler (`sub_82BDB190` → `sub_82BDAA28(3, STATUS_FATAL_APP_EXIT, 1)`). The original design intent: these slots would be **overridden by `rexgpu-xenosd.dll`** (the Xenos GPU plugin) stapling real render implementations in their place. Without the GPU plugin present, calling ANY of these slots crashes the application with `STATUS_FATAL_APP_EXIT` (0x40000015).

**What sub_82B34998 does to itself**:
- Reads `dword_830BE190` (now our pre-populated 60KB block) as its `a1`
- Calls `sub_82B2C9D0` (gating check — TLS read, returns BOOL)
- If gating OK:
  - Calls `sub_82B33EC0(a1, f1)` — iterates `*(a1+72) + 1` entities (constructor sets `*(a1+72)=1`, so 2 iters), calling `sub_82B33E78(a1+76 + i*96, f1)` per entity. (`sub_82B33E78` = `sub_82B33E18` + `sub_82B33D40` — both are float-arm/lerp math + per-entity update callbacks. Not GPU-bound directly.)
  - Calls `(*a1->vt[8])(a1, f1)` = `sub_82B2C4C8(a1, f1)` — iterates 4 entity slots at `a1+1352` (i.e., `(a1+1296, 1300, ...)`), dispatching `(*a1->vt[15])(a1, v4-14)` or `(*a1->vt[16])(a1, v4-14)` based on flags. **vt[15] AND vt[16] are both `sub_82BDB190` (terminator).**
  - Calls `sub_82B307D8(a1, f1)` — wraps `sub_82B2D030(*(a1+1288))`. `*(a1+1288)` is uninitialized (constructor only sets up to a1+76 explicitly via `sub_82B32178(a1+19)`). Likely NULL or garbage → `sub_82B2D030(0)` → reads `*(int*)0+8` = NULL-ptr crash.
  - Calls `(*a1->vt[7])(a1, f1)` = `sub_82BDB190(a1, f1)` → **fatal exit immediately**.
  - Calls `sub_82B2C498(a1)` — wraps `(*(int(*)())((*(int*)a1) + 68))()` if non-NULL. Constructor set `*(a1+68)=0`, so this is a no-op returning `a1`.
- If gating fails: `sub_82B36298(a1, 2, ...)` — calls `sub_82B35340()` if `*(a1+61036)` is 0 (likely 0 — uninitialized). `sub_82B35340` is a recursive depth-4 graph that eventually reaches even more terminators + `sub_82BFBF30` (XenosWait).

**Final structural verdict**: `sub_82B34998` CANNOT be made to complete naturally in our no-GPU-plugin profile. It is a GPU-plugin-only code path by design. Three independent reasons:

1. **vt[8] → vt[15]/vt[16] terminators**: `sub_82B2C4C8` iterates entity slots and dispatches the terminator regardless of state — fatal.
2. **vt[7] = terminator**: called unconditionally after the (also-fatal) vt[8] dispatch — fatal if execution somehow survived.
3. **`*(a1+1288)` uninitialized** (NULL/garbage): `sub_82B2D030(NULL)` immediately reads `*(int*)NULL+8` — null pointer crash.

These would only be replaced by GPU plugin's real vtable slots in the original Xbox 360 + Xenos + rexgpu-xenosd.dll profile. In our pure-host D3D12 renderer, sub_82B34998 is structurally unreachable.

### Implications for future work

- **No more bisection inside `sub_82B34998` will help** — the vtable dispatches are designed fatal. Stubs would have to replace the vtable itself (`*a1 = &our_custom_vtable`), then implement each vt slot with our D3D12 game-RT renderer. That's a wholesale rewrite of the renderer block as a guest-callable surface — equivalent to writing a Xenos GPU plugin.
- The current baseline (hook #6 skips the entire renderer block at 0x82B70EC8 → 0x82B710BC, our C++ LoaderTick cap ends at iter 101 with r3=0) remains the best working approach.
- The post-dispatch body (0x82B70EF8..0x82B710BC, 0x82B70EF4 onwards after SkipRendererDispatch) is a separate code path with its own entity loops using different globals (`dword_830BE400+0x1C..+0x24` — engine sub-entities, NOT the 60KB block) — its calls do not hit the terminator vtable. Whether the post-dispatch body itself can complete natural execution (or hangs at one of its own sub_xxx callees identified earlier: `sub_82B237B0`, `sub_82B67D98`, `sub_82BDC040`, `sub_82B676B8`, `sub_82AFF120`, `sub_82B0A0D0`, `sub_82B6FF78`, `sub_823EDD40`, `sub_82B1F410`, engine->vt[3]) is still an OPEN question — the Path 1 experiment with SkipRendererDispatch showed `SkipRendererDispatch #1` fired but `#2` never did, suggesting the post-dispatch body itself hung on first pass too. That bisection is deferred.

---

## Eng+8 vs transition-renderer+8 — critical distinction

- **`dword_830BE400` (global)** stores the engine HEAP pointer (Bootstrap-allocated 80-byte object at e.g. 0x400EA4E0). `dword_830BE400 + 8` (MainLoop's read) = `*(engine_heap_obj + 8)` = engine heap **slot 2**. NULL per Bootstrap; workaround sets it to the engine pointer itself so the vt[36] call lands on Bootstrap's nullsub_1 no-op (the correct no-op handler for the bootstrap state — entities populate via a different path, not via this vtable call).
- **`unk_830EC248` (global)** is the transition renderer (545KB struct located at static data address 0x830EC248, NOT heap). `unk_830EC248 + 8 = dword_830EC250` (transition renderer's slot+8). EngineInit populates this slot with AssetDB at 0x82ba7fe4. LoaderTick's `(a1+8)->vtable[6]()` reads this slot → AssetDB vt[6] = `sub_8253AA40` (real function).
- SetupRenderer's `a1` is `unk_830EC248` (transition renderer), NOT the engine. Its body accesses `*(r31+8)` (= transition renderer + 8 = AssetDB) at 0x82B713f8, 0x82B71438, 0x82B71520, 0x82B71540 — these reads succeed because EngineInit populates the slot before SetupRenderer is called.
- MainLoop's `eng+8` (slot 2 of engine heap object) has NO legitimate writer in the binary — leaving it NULL and using the Bootstrap self-ref workaround appears to be the only viable path (matches the no-op vt[36] handler Bootstrap exposes).

### Eng+8 writer — investigation status
- Bootstrap (`sub_82ABB838` → `sub_82ABB3C8`) confirmed to NEVER write slot 2 (byte +8). Settles slots 12/13/14/19 to 0, installs vtable 0x82139C44 at +0, caches eng in `dword_830B08C0`. Leaves slot 2 at whatever the heap allocator returned (= 0).
- `sub_82ABB3A0` (Bootstrap's slot-15 subobject init) only writes the XVIDEO_MODE subobject at slots 60–72 (display-mode default `{1, 640, 480, 329060}`), doesn't touch slot 2.
- SetupRenderer's middle band (skipped by mid-ASM hook #5) READS `eng+8` as a vtable dispatcher at 0x82B713f8, 0x82B71438, 0x82B71520, 0x82B71540 — but never WRITES it. The skipped band assumes `eng+8` is already populated by the time it runs.
- **Investigated the 9 pre-hook-#2 helpers in SetupRenderer body** (`sub_82B64228`, `sub_82BFBF38`, `sub_82B668D0`, `sub_82B60678`, `sub_82B67590`, `sub_82B601E8`, `sub_82B601A8`, `sub_82BDF080`, `sub_82BFBCD0`) + SetupRenderer's first-line `eng->vt[19] = sub_82311410`. None writes `*(eng_slot_2)`. `sub_82B64228` writes `a1[2] = 0` for its own **stack-local** registry object (a1 = `r1+var_4B0`, NOT eng). SetupRenderer's `a1` is `unk_830EC248` (transition renderer), so the `lwz r3, 8(r31)` reads in SetupRenderer access **transition_renderer+8 = `dword_830EC250`** (AssetDB, populated by EngineInit), NOT `dword_830BE400 + 8` (engine slot 2). The AGENTS.md "SetupRenderer reads eng+8" note was wrong mid-investigation — SetupRenderer reads transition_renderer+8.
- **Conclusion**: MainLoop's `*(dword_830BE400 + 8)` actually IS NULL legitimately — Bootstrap never populates engine's slot 2 because the no-op `vt[36] = nullsub_1` at Bootstrap vtable 0x82139C44 IS the intended handler for the bootstrap state. The game-tick semantics here are "do nothing further at Bootstrap engine level"; real per-frame entity population lives in the LoaderTick renderer block (mid-ASM hook #6 skipped it; see "Blocked" above). Investigating "eng+8 writer" further is futile — there's no legitimate writer because the slot is supposed to remain NULL (matching the no-op vt[36] handler). Self-ref workaround `eng+8 = eng` is the correct fix.

---

## Path 2 — GPU plugin shim design (2026-07-31)

**Goal**: Replace the fatal `off_8213F70C` vtable dispatches inside `sub_82B34998` (the LoaderTick renderer block at 0x82B70EF4) with no-op stubs, so the natural entity/render code can execute past the original fatal-exit terminators. Path 2 builds on top of Path 1 (the pre-population cascade), which still needs to be enabled for sub_82B34998 to ever run.

### `sub_82B34998` call graph (post-RE)

```c
int RendererDispatchBlock(int a1, ..., float a22)
{
  if (!dword_830BE190)
    dword_830BE190 = off_82D5648C();        // [Path 1] lazy-init — hangs in Transition thread
  if (!TerminatorTlsGate(a1))               // [Path 2] *(a1+61104) == TLS slot check
    return sub_82B36298(a1, 2, ...);        // failure-path fallback (likely safe)
  sub_82B33EC0(a1, a22);                    // entity update iteration (per-entity math, safe)
  (*(*a1 + 32))(a1, a22);                   // vt[8] = sub_82B2C4C8 (entity slot dispatcher)
                                           //   internally dispatches vt[15]/vt[16] (BOTH terminators)
  NullDerefDispatch(a1, a22);              // *(a1+1288) → sub_82B2D030(NULL) crashes
  (*(*a1 + 28))(a1, a22);                   // vt[7] = sub_82BDB190 fatal terminator
  return sub_82B2C498(a1);                  // returns a1 (no-op if *(a1+68)==0)
}
```

### Vtable `off_8213F70C` slot classification (post-RE)

| Slot | Offset | Address | Role | Shim treatment |
|------|---------|---------|------|----------------|
| 0  | 0  | 0x82B3C828 | function (unused by dispatch path) | keep as-is |
| 1  | 4  | 0x82B38830 | function (unused by dispatch path) | keep as-is |
| 2  | 8  | 0x82B2C270 | function (unused by dispatch path) | keep as-is |
| 3  | 12 | **0x82BDB190** | **terminator** | → nullsub_1 |
| 4  | 16 | 0x82B35FE0 | function (unused by dispatch path) | keep as-is |
| 5  | 20 | 0x82B2C2E8 | function (unused by dispatch path) | keep as-is |
| 6  | 24 | **0x82BDB190** | **terminator** | → nullsub_1 |
| 7  | 28 | **0x82BDB190** | **terminator** (called by dispatch path) | → nullsub_1 |
| 8  | 32 | 0x82B2C4C8 | entity slot iterator (dispatches vt[15]/vt[16]) | keep as-is |
| 9  | 36 | 0x82426FF0 | nullsub_1 already | keep as-is |
| 10 | 40 | 0x82B38A48 | function (unused by dispatch path) | keep as-is |
| 11 | 44 | 0x82B38CC0 | function (unused by dispatch path) | keep as-is |
| 12 | 48 | 0x82B38D90 | function (unused by dispatch path) | keep as-is |
| 13 | 52 | 0x82B2C738 | function (unused by dispatch path) | keep as-is |
| 14 | 56 | **0x82BDB190** | **terminator** | → nullsub_1 |
| 15 | 60 | **0x82BDB190** | **terminator** (dispatched by vt[8] body) | → nullsub_1 |
| 16 | 64 | **0x82BDB190** | **terminator** (dispatched by vt[8] body) | → nullsub_1 |
| 17 | 68 | **0x82BDB190** | **terminator** | → nullsub_1 |
| 18 | 72 | **0x82BDB190** | **terminator** | → nullsub_1 |
| 19 | 76 | **0x82BDB190** | **terminator** | → nullsub_1 |

9 of 20 slots are fatal (`vt[3,6,7,14-19]`); the dispatch path reaches only 3 (`vt[7,15,16]`), but all 9 are replaced defensively.

### Shim hooks installed in `src/native_graphics.cpp`

Three REX_FUNC hooks added under the `// GPU renderer shim (Path 2)` section. **Inert in baseline** (mid-ASM hook #6 skips sub_82B34998 entirely so these never fire until Path 1 is enabled):

| Hook | Guest addr | Role |
|------|-----------|------|
| `sub_82B2C9D0` | 0x82B2C9D0 | TLS gate bypass — always return 1 (truthy), so the success path runs |
| `sub_82B307D8` | 0x82B307D8 | NULL-deref bypass — no-op return instead of `sub_82B2D030(*(a1+1288))` crash |
| `sub_82B38558` | 0x82B38558 | Constructor post-hook — after orig installs `off_8213F70C`, allocate 80-byte guest heap, copy vtable, replace 9 fatal slots with `nullsub_1` (0x82426FF0), and overwrite `*a1` with the custom vtable pointer |

The vtable allocation uses `REX_CALL_INDIRECT_FUNC(0x82AB73C0)` (sub_82AB73C0, the same heap allocator EngineInit/SetupRenderer use). The replaced slots point to `nullsub_1` (0x82426FF0, 4-byte `blr` no-op).

### Enablement recipe (next iteration)

To activate the shim with Path 1 cascade:

1. Edit `mx_config.toml`:
   - Comment out the `NativeSkipLoaderRenderer` mid-ASM hook (#6) — lines ~1641-1646
   - Move `NativeSkipRendererInit` (#5) fire-address from `0x82B71324` → `0x82B71314`
   - Comment out `NativeSkipLoaderAll` (#8) — lines ~1651-1654
2. Edit `src/native_graphics.cpp`:
   - In the `sub_82B71148` (SetupRenderer) hook, after `orig_SetupRenderer` returns, restore registers r0/r3-r12/lr/ctr around a `REX_CALL_INDIRECT_FUNC(0x82B3C7D0)` to pre-populate `dword_830BE190` from main-thread context (avoids the lazy-init Transition-thread hang)
   - In the `sub_82BFB740` (Wait) hook, always return SUCCESS
   - Add a hook for `sub_82B70370` (timing) that no-op returns (avoids the busy-wait spin)
3. `rexglue codegen --force mx_manifest.toml` (~75s)
4. `cmake --build out/build/win-amd64-debug --target mx`
5. Copy `mx.exe` to root, run, watch log for `shim vtable installed @0x...` then `LoaderTick #N r3=...`

If `sub_82B34998` returns cleanly, expect LoaderTick to progress to the post-dispatch body (0x82B70EF8..0x82B710BC). The Path 1 experiment showed this body hangs on its first pass — see post-dispatch body table above. Each will likely need its own stub — iterate per the bisect recipe at the end of the "PATH 1 EXPERIMENT" section.

**NOTE**: The Path 2 shim was later found to be unnecessary — see "eng+8 writer traced" section below. The real vtable is `0x8213F7A4` (all real functions, NO terminators), not `off_8213F70C`. The shim code remains in `native_graphics.cpp` but is inert in baseline.

---

## eng+8 writer traced (2026-07-31, session 3)

### Discovery via plugin-mode triangulation

Added `LogEngSlot8(base, where)` helper in `native_graphics.cpp` and called it at every plugin-mode hook point (Bootstrap, EngineInit, SetupRenderer ENTER/RETURNED, GraphicsInit ENTER/RETURNED, PostGfxInit, TexManager, BindTexture, LazyInit, VtableCtor). Captured this chronological trace during a 30s plugin run:

```
[Bootstrap ENTER]    eng+8 = 0x00000000 (eng=0x00000000)
[Bootstrap RETURNED] eng+8 = 0x00000000 (eng=0x00000000)   ← Bootstrap doesn't write it
[SetupRenderer ENTER]eng+8 = 0x00000000 (eng=0x400EA4E0)   ← Engine exists, slot empty
[GraphicsInit ENTER] eng+8 = 0x00000000 (eng=0x400EA4E0)
[PostGfxInit ENTER]  eng+8 = 0x00000000
[GraphicsInit RETURNED]eng+8 = 0x00000000
[BindTexture]        eng+8 = 0x00000000  ← Last NULL sighting (at 0x82B712B0)
[LazyInit ENTER]     eng+8 = 0x40BCF740  ← First NON-NULL sighting (at 0x82B71330)
[SetupRenderer RETURNED]eng+8 = 0x40BCF740  ✓
```

The eng+8 write happens between BindTexture (0x82B712B0) and LazyInit (0x82B71330). Disassembly of that SetupRenderer band:
- 0x82B712B4–0x82B712C0: 2nd TexManager + 2nd BindTexture
- **0x82B712C4–0x82B712D4: `eng->vt[8](eng)`** (skipped by mid-ASM hook #3 `NativeSkipVtable8`)
- 0x82B712D8–0x82B712F8: `sub_82AB73C0(0x85280)` alloc 545KB + `AssetDB_InnerCtor_VtableInstall` constructor (installs vtable `off_8214518C`)
- **0x82B71304–0x82B71310: `eng->vt[17](eng, assetdb_block)`** (skipped by mid-ASM hook #4 `NativeSkipVtable17`)
- 0x82B71314–0x82B71330: test `dword_830BE190`, then LazyInit (`sub_82B3C7D0`)

### Writer function: `sub_82B43AC8` (vt[17])

Decompiled:

```c
int sub_82B43AC8(int a1, int a2)            // a1 = engine, a2 = 545KB AssetDB block
{
  *(a1 + 8) = a2;                            // ← THE CRITICAL WRITE: eng+8 = AssetDB
  v3 = sub_82B43A48();                        // lazy-init returns dword_830BE430 (dispatcher singleton)
  return (*(*v3 + 4))(v3, *(a1 + 8));         // dispatcher->vt[1](dispatcher, AssetDB)
}
```

The dispatcher singleton `dword_830BE430` is constructed by `sub_82526BF8` (called from `sub_82B43A48` lazy-init):
- `sub_82AB73C0(4)` — allocate 4-byte block
- Install vtable `off_82049B8C` at `*block`
- Returns block as singleton

### Dispatcher vt[1]: `sub_82526D10` — AssetDB subsystem registration

The dispatcher `off_82049B8C`'s slot 1 (at offset 4) = `sub_82526D10`. This is a **massive** function — calls ~18 subsystem-manager singletons, gets each manager's interface via `mgr->vt[2](mgr)`, then registers each interface with the AssetDB via `assetdb->vt[27](assetdb, interface, ...)`.

Pattern (repeating for each subsystem):
```c
v3 = sub_82BABD08();                          // global registry
v4 = sub_8237CF88(v3);                         // manager A singleton (e.g., Texture)
v6 = (*(*v4 + 8))(v4);                         // manager->vt[2] — get interface
v8 = (*(*a2 + 108))(a2, v7, v6);               // assetdb->vt[27] — register interface
```

The 18 subsystem managers lookups (in order):
| # | Manager getter (sub_xxx) | Likely subsystem |
|---|--------------------------|------------------|
| 1 | `sub_8237CF88` | ? |
| 2 | `sub_823F9DA8` | ? |
| 3 | `sub_823FADC8` | ? |
| 4 | `sub_823F54C0` | ? |
| 5 | `sub_82470B48` | ? |
| 6 | `sub_82470A68` | ? |
| 7 | `sub_8229C408` | ? |
| 8 | `sub_824A82E0` | ? |
| 9 | `sub_8237D070` | ? |
| 10 | `sub_822EB3A8` | ? |
| 11 | `sub_8234C0E0` | ? |
| 12 | `sub_8242D650` | ? |
| 13 | `sub_823A03E0` | ? |
| 14 | `sub_82331370` | ? |
| 15 | `sub_82520960` | ? |
| 16 | `sub_824444F0` | ? |
| 17 | `sub_82444C00` | ? |
| 18 | `sub_82526C90` | ? |

### Implications for native backend

**Discovery**: Native mode skips BOTH:
- The 545KB AssetDB block alloc (at 0x82B712D8) — runs (no hook between TexManager/BindTexture and the alloc)
- The `AssetDB_InnerCtor_VtableInstall` constructor (at 0x82B712EC) — runs (between hooks #3 and #4)
- BUT `vt[17]` at 0x82B71310 is **skipped by mid-ASM hook #4** — so:
  - `eng+8` is never written (stays NULL — confirmed bug)
  - `sub_82526D10` (18-subsystem AssetDB registration) never runs — AssetDB has no subsystem handlers

**Two-part fix needed**:
1. **Restore `eng+8` write** — either:
   - Disable mid-ASM hook #4 (let `sub_82B43AC8` run naturally), OR
   - Replicate `*(eng+8) = assetdb_block` from C++ in SetupRenderer hook (need to recover the 545KB block ptr)
2. **Restore AssetDB subsystem registration** — either:
   - Let `sub_82526D10` run (via option 1 above), OR
   - Stub each `assetdb->vt[27]` registration call to no-op (risky — 18 subsystems)

**Recommended next step**: **Disable mid-ASM hook #4 only** (keep #2, #3, #5-8 active). Re-codegen, run native mode, observe whether `sub_82B43AC8` + `sub_82526D10` complete without crash. The 18 subsystem-manager lookups depend on registry state populated earlier — may need additional stubs if they crash.

**Alternative**: Write a C++ shim that, after `orig_SetupRenderer` returns:
1. Re-allocates the 545KB block via `sub_82AB73C0(0x85280)` and calls `AssetDB_InnerCtor_VtableInstall` (no-ops if already done — SetupRenderer ran that path)
2. Reads the block ptr from a known location (or re-allocates and tracks it ourselves)
3. Does `REX_STORE_U32(eng + 8, block_ptr)` directly
4. Skips `sub_82526D10` (the subsystem registration) — accept that assets won't load but the engine won't crash on the missing slot

The second alternative is safer for incremental progress — gets eng+8 populated without risking 18-subsystem slowdowns/crashes.

---

## Native mode baseline restored (2026-07-31, session 3 followup)

After confirming vt[17] is the eng+8 writer, **disabled mid-ASM hook #4** (NativeSkipVtable17) in `mx_config.toml` so vt[17] runs naturally. All other mid-ASM hooks (#2 SetupDeviceSkip, #3 SkipVtable8, #5 SkipRendererInit, #6 SkipLoaderRenderer, #7 SkipLoaderEarly, #8 SkipLoaderAll) restored and active. Hook #1 (NativeGameTickSkip) remains disabled.

Verified via native-mode run (no plugin):

```
native: SetupRenderer ENTER (0x82B71148)
native: SetupRenderer RETURNED
native: eng+8 already populated (0x40BCF720)        ← vt[17] ran naturally; populated!
native: LoaderTick #1 r3=1
native: LoaderTick #2 r3=1
native: LoaderTick #3 r3=1
native: LoaderTick #4 r3=1
native: LoaderTick #5 r3=1
native: LoaderTick #101 r3=0                       ← cap reached cleanly
native: VdSwap #1 wrote 64704 bytes at guest 0xBEB9057C
native: RenderPipeline #1 — orig returned
native: MainLoop #1                                 ← MainLoop iterating!
native: RenderPipeline #2 — calling orig
native: EndFrame #2 — calling orig (fires VdSwap)   ← hangs here, no VdSwap #2
```

**Status**:

- ✅ `eng+8` populated to real AssetDB block in native mode (`0x40BCF720` — note slight address difference vs plugin's `0x40BCF740` due to heap layout variance, but valid block).
- ✅ LoaderTick runs through cap (101 iterations cleanly) — no entity/renderer block hang (still skipped by hooks #6-#8).
- ✅ MainLoop iterates — sleeps 16ms per frame per our hook.
- ✅ First render frame completes: VdSwap #1 fires (64704 bytes of PM4 = 14249 packets = matches plugin's swap #1 size at boot).
- ⚠️ **EndFrame #2 hangs** — second render frame calls `orig_EndFrame` but never returns (VdSwap #2 doesn't fire). Likely orig_EndFrame's internal GPU wait state — our BeginFrame/EndFrame stubs return immediately but orig_EndFrame may spin waiting for the GPU command processor to ack. The first EndFrame works because the very first VdSwap triggers fallback paths; subsequent EndFrames expect accumulated state we don't maintain.
- ❓ Current `eng+8` value (`0x40BCF720`) differs slightly from plugin (`0x40BCF740`) — both are HeapAlloc'd AssetDB blocks. Same vtable was likely installed (off_8214518C).

**The `eng+8 = eng` self-ref workaround in MainLoop hook (frame 1) is now OBSOLETE** — eng+8 is already set by the time MainLoop fires, so we shouldn't override it. Leave the workaround line for safety in case anything regresses, but it's a no-op now since setting `eng+8` to itself doesn't break the already-set value.

---

## EndFrame #2 hang — ROOT CAUSE FOUND AND FIXED (2026-08-01)

**Investigation**: Installed REX_FUNC diagnostic hooks for EndFrame's 3 inner calls (`sub_82566B50` / `sub_8255CE98` / `sub_825599A8`) + VdSwap ENTER log. Observed:

Frame #1: all 3 inner calls return → VdSwap #1 ENTER → completes (64704 bytes) → EndFrame returns.
Frame #2: all 3 inner calls return → VdSwap #2 ENTER → **never returns** (no "wrote" log).

**Root cause**: `orig_VdSwap` (`sub_82566B58`, 1668 bytes, 80 blocks) contains a **spin loop** at `0x82567178`:
```c
do {
  result = sub_8255CFE0(...);  // poll GPU frame-pending state
  if (result == 0) break;       // not pending → exit loop
  // check: *(r31+0x4188) - *(r31+0x4190) >= 0xF (15)
  // if < 15, loop back
} while (...);
```

`sub_8255CFE0` returns 1 ("GPU still pending") when a GPU counter at `*(*(r1+256)+88)` hasn't advanced by >= 0x1388 (5000) units since the last frame. Without a GPU, this counter **never advances** → infinite spin loop. Frame #1 skips the loop entirely (bit 29 of `*(r31+0x5E88)` is 0 on first call); frame #2+ enters the loop after frame #1 sets that bit.

**Fix**: REX_FUNC hook on `sub_8255CFE0` that stubs to `ctx.r3.u32 = 0` ("not pending") in native mode. This breaks the spin loop, allowing VdSwap #2+ to complete.

**Result**: Native mode now runs consecutive render frames without hanging. 24 VdSwaps in 7 seconds (every frame fires), RenderPipeline calls orig every frame, MainLoop #421+ at 60fps. `RenderPipeline` and `EndFrame` hooks restored to run every frame (diagnostic caps removed).

### Previous workaround (now obsolete)

The previous approach capped `orig_RenderPipeline` to `rp <= 1` — only the first frame called orig, skipping all subsequent frames to avoid the EndFrame #2 hang. This is no longer needed; every frame now calls orig_RenderPipeline → orig_EndFrame → orig_VdSwap without hanging.