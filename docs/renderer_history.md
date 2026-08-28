# Renderer investigation history

Moved out of `AGENTS.md` on 2026-08-12. These sections were written between
2026-08-05 and 2026-08-07, before the translated shader path existed, and they
had grown to two thirds of a file whose job is to be a map. They are kept in
full because their *derivations* and *symptom trails* are still good and are
expensive to re-derive — several record a probe that could not have seen the
thing it was read as ruling out, which is the most reusable lesson here.

**Do not read any conclusion below as a description of what is on screen
today.** `AGENTS.md` "Current state" is the authority on that; where the two
disagree, this file is the older one. Specifically superseded: the colourless
overpaint as the cause of the white frame (it was the D3D9 legacy multiply,
`28d4853`), and every draw count taken before `DrawVerticesUP` was hooked.

Configurations referenced here that no longer exist: `hide_colorless_draws`,
`hide_colored_draws`, `skip_intro`, `hle_render`, `pm4_translate`,
`native_res_viewport`, `clear_magenta`, `legacy_mvp_tiebreak`, and
`rdoc_capture_frame`. Unknown cvars are tolerated, so an old run line carrying
one still runs and silently does nothing.

---

## SOLVED: it was the frame-pacing stub (2026-08-06)

**The native `sub_82B70370` stub was the blocker.** Unstubbing it takes the
script VM from 28 dispatches to 686 and the front end starts running. Everything
in the section below is still an accurate description of the *symptoms*; the
cause is here.

| native, 60 s run | stubbed | real timing |
|---|---|---|
| VM dispatches | 28 | **686** (3/3: mx_473, mx_474, mx_475) |
| script assets | 2 | **4** — adds `IG_PlayerListHelper` |
| registry string keys | 1 | **4** — adds `IntroMode`, `GameContentInstall`, `GameSessionNotification` |
| `BinkOpen` | 0 | **3** — THQ_Logo, Attract.ENG, FE_Smoke, all OK |
| bindings reached | — | `LoadUIAssetDatabasePackage`, `LoadUIAssetPackage`, `IsUIAssetPackageLoaded`, `LoadAssetDB`, `LoadAssetPackage`, `SendUIEvent`, `CastUIContainer`, `CastUIFlashComponent` |
| lua errors | 0 | 0 |

Those are the plugin's numbers. **The native/plugin divergence is closed.**

With `--hide_colorless_draws=true` the front end renders 11,250 draws / 33,750
vertices with **every skip counter at zero** — no-code 0, decode 0, stream 0,
constants 0, vertex 0. *(Measured under a configuration that no longer exists:
that cvar was retired 2026-08-07 and every draw is now submitted. The skip
counters are the point here and are unaffected; the draw count is not
reproducible as written.)*

**Both hazards the stub was written for are false**, read out of `sub_82B70370`
rather than assumed:

- *"`a1+20` drives a busy-wait."* The guest's own test is
  `if (*(float*)(a1+20) != 3.4028235e38 && dt < target)`. `a1+20` reads
  `0x7F7FFFFF` — exactly that FLT_MAX sentinel — so the guest disables the spin
  itself. Logged and confirmed at runtime: `FLT_MAX=true`.
- *"`a1+32` is an unbounded store offset."* It is `v9 = *(a1+32) + 9;
  *(float*)(4*v9 + a1) = dt;` with `if (v10 >= 5) *(a1+32) = 0` — a bounded
  5-entry ring at `a1+36..a1+52`, guarded by `if (*(a1+28))`. Observed 0.

The stub wrote a fixed `1/60` to `a1+24` and nothing else. The real function also
maintains a 5-sample smoothing sum and, decisively, **total elapsed time** — full
field map in [docs/guest_binary.md](docs/guest_binary.md). A front end that
advances on elapsed time had
nothing to advance on — the measured symptom was `f1` arriving at
`RendererDispatch` as exactly `0.00` in native and varying under the plugin.

The stub is deleted outright rather than left behind a cvar: it was wrong, not
a trade-off, and git has it. `legacy_mvp_tiebreak` went the same way in the same
commit, its A/B having been settled.

**Why this took so long to find, worth remembering:** the stub dates from before
the D3D9 HLE layer, like the four other workarounds retired on 2026-08-06, and
its comment stated both hazards as fact. Nobody re-derived them. The
instrumentation was also one-sided — the VM-dispatch probe was rate-limited to
four lines, so "4 dispatches" looked like a hard floor, and the
`RendererDispatch` probe logged only under `g_plugin_mode`, so native's `f1=0.00`
was never visible. **Two probes that could not have detected the bug were used as
evidence that the bug was not there.**

## Why there is no menu (2026-08-05) — superseded, kept for the symptom trail

**The front end is script-driven, and the script VM stops 1.6 seconds into
boot.** This supersedes every earlier explanation in this file's history,
including the `PlayerMode` and load-state-machine readings, which are symptoms.

The chain, each step measured:

- Tracing the load-request API upward through the recompiled sources dead-ends.
  Four of the five functions above `sub_82352AE0` have **zero** direct callers,
  and they are not virtual methods either.
- They are entries in a **name → function binding table** of 228 `const char*`
  and code-pointer pairs. The vocabulary is a scripting API. Addresses and slot
  indices: [docs/guest_binary.md](docs/guest_binary.md).

| Binding | |
|---|---|
| `ExecuteScriptAsset` | **fires twice, then never** |
| `LoadUIAssetPackage` | never fires |
| `LoadUIAssetDatabasePackage` | never fires |
| `StartWorldLoad` | never fires — reaches the load-request API |
| `SwitchToUIWorld` | never fires |

- `ExecuteScriptAsset` validates one `char const*` argument and passes it to
  `sub_824F91E8`, whose `r3` is therefore the script asset name in plain guest
  memory. Hooking it names the two assets that run:

```
native: script asset #1 "RSLibrary" (ptr=0x2040A270) from lr=0x824AF8B0
native: script asset #2 "UI_Helper" (ptr=0x2040A5D0) from lr=0x824AF8B0
```

- Both are **libraries**, not drivers. The `MXUI` database holds **57 unique
  script assets**, including `IN_BootStrapper`, `FE_Title`, `FE_Home`,
  `IN_Loading`, `IG_WorldLoading`, `SH_LuaDataProvider`.
- **Careful about what this does and does not prove.** `ExecuteScriptAsset` is a
  *script binding* — a script called it twice. So a root script **is** running;
  something started it, and it loaded two libraries and then stopped. The probe
  shows only that no *third* asset is executed **by name** through
  `sub_824F91E8`. It does not show that `IN_BootStrapper` never runs — the root
  script could well be `IN_BootStrapper` itself, stalling after its first two
  statements. Which of those it is has not been measured.
- The VM's own native-call dispatcher (`sub_82AA7638`, identified from
  `ExecuteScriptAsset`'s caller `lr=0x82AA78F4`) fires **4 times in an 80-second
  run, all within the first 1.6 seconds**, then goes silent. The probe is
  rate-limited to one line per 5s after the first four, so the silence is real.
- Corroborating: the guest reads **exactly one registry key** in a whole run —
  `PlayerMode`, by the loader's own gate. `Location`, the key naming the scene to
  load, is never read.

**The script layer is a Lua VM** — call handler, error reporters and struct
offsets in [docs/guest_binary.md](docs/guest_binary.md); `SH_LuaDataProvider` is
in the asset list. So the next move is to
hook those two error paths and ask the direct question: **is the root script
throwing?** A script that dies on statement three looks exactly like this from
the outside.

**Disproved, so nobody retries it:** the one active mid-ASM hook
(`NativeSkipRendererDispatch`, skipping `bl sub_82B34998` at 0x82B70EF4) is
**not** the cause. That block contains three `bctrl` indirect calls per
LoaderTick, which made it a strong suspect. Re-enabling it changes nothing —
still 4 VM dispatches, still the same two script assets, still no error.

Worth keeping from that experiment: **the block no longer crashes without the
skip.** Its premise was "this needs the Xenos GPU", written before the D3D9 HLE
layer existed. Draw counts are unchanged and there is no fault. That is one run,
not the 3/3 this file demands, so verify before relying on it.

**Do not reverse-engineer the AssetDB for this.** ReXGlue handles asset loading
fine, and the runtime confirms it — the only failed opens in a full run are four
DLC camera `.bxml` files and `\Device\Image`, which Xenia also shows. The
encrypted `.xenon.package` heaps are a limitation of the *offline* tools in
`tools/`, not evidence that the guest cannot read its own packages.

`PlayerMode = "None"` is index 4 of the game's own five-value vocabulary
(`SplitScreen`, `SinglePlayer`, `Online`, `LAN`, `None`; the *failure* value is
5). It is the expected value before a menu has chosen a mode — a symptom, not a
cause.

### Audio and input are downstream of this, not separate bugs (2026-08-06)

Neither works in native mode, and the natural suspicion was that native mode
broke ReXGlue's handlers. It did not. Measured by hooking the guest's own XDK
wrappers around the import thunks — the thunks are defined in the runtime
library and cannot be redefined, but the wrappers are ordinary recompiled
functions (`sub_82C08EC0` → `XamInputGetState`, `sub_82C08ED0` →
`XamInputGetCapabilities`, `sub_82C87F78` → `XAudioRegisterRenderDriverClient`,
`sub_82C87B98` → `XAudioSubmitRenderDriverFrame`, `sub_82C4C268` →
`XMACreateContext`).

**Audio is a working pipe carrying silence.** The guest registers a render
driver client (r3=0), then submits **30,776 frames in 165s** — 187/s, exactly
the 360's 256-sample-at-48kHz frame rate, so the SDK is consuming and pacing it
in real time. `sub_82C87B98` is the XDK mixer, not a thin wrapper: it has
`sub_82C87950` fill a buffer at r1+1888 and passes that to the import
(mx_recomp.94.cpp:31435), and 8064-1888 = 6176 bytes of room fits one
256 x 6ch x float32 frame. Hooking that fill and scanning all 1536 floats gives
**peak = 0.000000 on every frame**, with and without `force_load`. Neighbouring
uninitialised stack reads as the 0xBCBCBCBC fill pattern, so exact zero means
the mixer really did write silence. The game is playing nothing.

**Input works end to end.** `XamInputGetState` returns success and the packet
number advances (1 → 16 → 17 across runs), so live pad state reaches the guest.
It is polled only ~18 times in 75s, from `sub_82B6DB28` (lr=0x82B6DBD4) — the
slow "is a pad connected" cadence, not a front end reading a stick.

So there is one bug here, not three. Do not open audio or controller work as a
separate thread until the front end runs.

**The plugin-mode reference, measured 2026-08-06 with the same probes.** With
the D3D9 HLE hooks guarded (below) so the plugin runs at full speed, the guest
gets far past where it stops natively:

| | native | plugin |
|---|---|---|
| script assets | 2 (`RSLibrary`, `UI_Helper`) | **8**, incl. `IG_PlayerListHelper`, `SH_GarageHelper`, `SH_XPHelper`, `FE_Home_Cameras`, `SH_CutsceneHelper` |
| VMDispatch | 4 | **16** |
| audio | 0 non-silent / 30,776 | **11,786 non-silent / 13,115, peak 0.216** |

`FE_Home_Cameras` is a front-end script. So the front end does run under the
plugin, and the audio path is not merely alive but ~90% non-silent. **The
divergence is in the script layer and it is the most informative open thread in
the project** — it is the first thing that separates the two modes at the level
of the actual blocker. Whatever native mode is missing, plugin mode has it.

### It was `VirtualQuery`, and it is fixed (2026-08-06)

Not the per-draw bookkeeping — that was a guess and it was wrong. `DeclFile()`
writes stop after `kMaxDrawsLogged = 16` draws, `ReportDrawCounts` early-returns
2499 times in 2500, and `REXCVAR_GET(x)` is a storage accessor.

`HostPageReadable` was **~100% of native frame time**: 502 calls a frame costing
3082ms of a 3128ms MainLoop body. Note the shape — **~6ms per call**, not many
cheap calls. A `VirtualQuery` is normally microseconds; six milliseconds is what
it costs against this process's address space, and that is also why a Release
build cost exactly what Debug did.

The fix is not to call it less by guesswork. `VirtualQuery` already returns the
whole contiguous run it found in `mbi.BaseAddress` / `mbi.RegionSize`, with
identical `State` and `Protect` throughout, so one query legitimately answers for
every address in that range. `HostPageReadable` now keeps an 8-entry MRU region
cache, cleared once per swap from the VdSwap hook so a commit or decommit is
picked up within a frame — a stale *positive* on a decommitted page is a crash,
which is the whole reason this function exists.

`--d3d9_page_cache_verify=true` re-queries the OS on every cache hit and logs
disagreements. **0 mismatches in 5,557 checks**, which is the correctness
argument, run rather than asserted.

| | before | after |
|---|---|---|
| MainLoop body | 300 → 3100ms | ~105ms |
| MainLoop in 70s | ~26 | **901–961** (3/3) |
| VirtualQuery per frame | 502 | 5–7 |

Native is now ~13/s against the plugin's ~16/s, from 0.37/s.

**And it did not move the front end: still 2 script assets and 4 VM dispatches,
3/3.** Frame starvation was not what held it back. The script-layer divergence
against plugin mode is a separate cause and remains the open question.

`--log_high_frequency_kernel_calls=true` does **not** gate these calls: a run
with it has the same 15 `[krnl]` lines as one without. Hook the wrappers.

### More stale workarounds retired (2026-08-06)

Both were the same vintage and shape as the mid-ASM skip above — written before
the D3D9 HLE layer, never revisited. Both are neutral over 3/3 runs (2 script
assets, 4 VM dispatches, no crash, no Lua error), so neither was the cause, but
both were arbitrary and are now gone.

- **The blanket 500 ms wait short-circuit** in `hooks_wait.cpp` returned
  SUCCESS from `NtWaitForSingleObjectEx` for *any* 500 ms wait, process-wide,
  never scoped to the renderer handshake it was written for.
- **`sub_82BFBF48` was stubbed to nothing** and labelled "error recovery". That
  name was a guess and it was wrong: it tail-calls `sub_82C01138`, which is a
  pure CRT thread-block read (`r13+336 ? 0 : *(*(r13+256) + 352)`), an
  errno-style pointer accessor with no side effects. Stubbing it left r3
  undefined at 156 call sites. Unstubbed.
- The `sub_82BFB748` (NtSetEvent) hook was deleted. It called the original
  unconditionally in both modes — eight log lines, no behaviour.

**Also not the cause: the intro skip.** `skip_intro` is host-side only; it
skips mx's own Bink playback loop in `D3D12GraphicsSystem::RenderThreadFunc`.
Its one guest-visible effect is `IsBinkPlaying()`, which makes the guest's
`RenderPipeline` stand down — so skipping the intro makes the guest run *more*.
Measured: a full run with the intro actually playing its 47.4s still gives 2
script assets and 4 VM dispatches, all inside the first 3.3s.

**The FFmpeg dependency was a native-mode substitute for a guest path that
works, and is now GONE (2026-08-06, `423b1da`).** `MxApp::OnPreSetup` returns
before creating `D3D12GraphicsSystem` when a GPU plugin is set, so
`RenderThreadFunc` — and with it the whole host Bink player — never started in
plugin mode, yet the intro played fine there. The guest decodes Bink itself.
The decisive evidence was direct: with both players present the intro visibly
played twice. Removing ours also removed the `IsBinkPlaying` gate that stood
the guest's entire render path down for 47.4 s, and vertices per frame went
from 96k to 1.6M.

That workaround predates the D3D9 HLE layer, like the others above. It is also
hardcoded to two English filenames (`graphics_system.h:51`) while
`assets/Videos/` holds DEU/FRA/ITA/SPA variants, so it cannot follow the
language setting. Whether the guest's own decoder now renders through HLE is the
test that would let the whole dependency go; it has not been run.

**That inference was right, and is now measured — see below.**

### The guest opens Bink under the plugin and never natively (2026-08-06)

Bink is statically linked into the XEX the same way D3D9 is; the guest carries a
complete decoder. Segment, symbols, vtables and open flags:
[docs/guest_binary.md](docs/guest_binary.md).

Probes on `BinkOpen`, both manager `Open`s and `BinkAsset::Init` are in
`hooks_plugin_diag.cpp` and are mode-neutral.
**4 native runs, 2 plugin runs, `--skip_intro=true`, no `--force_load`:**

| | native 4/4 | plugin 2/2 |
|---|---|---|
| `BinkOpen` | **0** | 3, all returning a live HBINK |
| `BinkMgr::Open` | **0** | 5–6 |
| `BinkAsset::Init` | 1 | 1 |
| → resolved asset handle | **`0x00000000`** | `0x2345D2A0` |
| script assets executed | 2 | 4 |
| registry keys read | 1 | 4 |

Under the plugin the guest opens `game:\Videos\THQ_Logo_wSound.bik`,
`Attract.ENG.bik` and `FE_Smoke.bik` itself, in that order, roughly 3 s in. The
observed flags match the static read of the manager's two branches exactly.

**Natively not one video is ever opened.** `BinkAsset::Init` still fires — which
is what proves the probe is live rather than absent — but its asset handle comes
back null, meaning `sub_82AB8210("Bink Video Asset")` returned nothing, so the
`'bink'` request in that function never happens.

`FE_Smoke` is a *front-end* video. Together with the registry keys below, that
puts the plugin inside the front end and native nowhere near it, so **Bink is
downstream of the script-layer divergence, not a cause of it.** It is, however,
a far sharper marker of that divergence than the script-asset count.

**Registry reads are the sharpest marker of all.** Native reads exactly one
string key in a whole run; the plugin reads four:

| Key | native | plugin |
|---|---|---|
| `PlayerMode` | ✓ | ✓ |
| `GameContentInstall` | — | ✓ `"installed"` |
| `IntroMode` | — | ✓ `"full"` |
| `GameSessionNotification` | — | ✓ |

**The probes now log `lr`, and it names the callers outright.** Both registry
getters capture `lr` at entry — the call clobbers it and both log after — so the
return address is the caller, not an inference from which function mentions the
key in `.rdata`.

| Read | from `lr` | which is |
|---|---|---|
| native `PlayerMode` | `0x82536294` | `sub_82536250` — the loader's own gate |
| plugin, every extra string key | `0x824AA590` | `sub_824AA568`, called only by `sub_824B1C20` |
| plugin, every int key | `0x824AA518` | `sub_824AA4F8`, called only by `sub_824B1788` |

Those callers are the Lua bindings `GetVariableString` and `GetVariableInt`,
registered in a second `(name, func)` table distinct from the 228-entry one —
see [docs/guest_binary.md](docs/guest_binary.md), whose **Traps** section also
records the `.pdata` block an earlier version of this text misread as that
table.

> **Corrected 2026-08-28.** "A second `(name, func)` table" is the wrong frame,
> and it kept that table's bounds "unestablished" for three weeks. The bindings
> are generated by **SWIG**, so the array at `0x82D1B208` is not a second module
> table at all — it is the `methods` field of the `VariableCollection`
> **class** (`swig_lua_class` at `0x82D1B260`), and its bound is the NUL
> terminator, like every SWIG array. The measurement above is unaffected: both
> callers and everything read from them still stand. What changed is only what
> the table *is*, and that there are five populations of binding rather than
> two — the layouts are in
> [docs/guest_binary.md](docs/guest_binary.md#binding-tables).

So this is not a settings file being consulted. **It is the front-end script
reading its own state**, and under the plugin it reads a recognisable boot
sequence: `GameContentInstall`, `UILoaded`, `IntroMode`, `InitialLoadCompleteFlag`,
`LaunchActivity`, `TableLoadError`, `InvitePending`, `InviteProcessing`,
`GameSessionNotification`. Natively that binding is called **zero** times; the
one native read comes from the loader, not from any script.

That makes `sub_824B1C20` and `sub_824B1788` a live progress trace of the front
end, and the sharpest instrument yet for the script-layer divergence — every
call is a statement the script actually reached.

**A process-hygiene warning, learned the expensive way.** The first pass at this
measurement reported "zero Bink calls in both modes" and was wrong twice over.
First, the build writes `out/build/win-amd64-debug/mx.exe` and nothing copies it
to the repo root, so `./mx.exe` ran a stale binary that did not contain the
probes at all — check with `grep -c "<a new format string>" mx.exe` before
believing any new probe's silence. Second, a cancelled background run script kept
running (only its output pipeline had been killed) and launched a second
`mx.exe`, so `ls -t logs/*.log | head -1` attributed *its* log to the run just
started. Identify a run's log by diffing the directory listing, never by mtime,
and confirm with `Get-Process mx` that exactly one is running.

### Native and plugin run the same 28 script calls, then native stops (2026-08-06)

The VM's call handler is Lua's `luaD_precall`, and the callee is readable before
it runs — the `TValue`, `Closure` and `Proto` offsets, confirmed against this
binary rather than assumed, are in
[docs/guest_binary.md](docs/guest_binary.md).

**The old "4 VM dispatches" figure was a rate-limiting artifact.** The previous
probe logged the first four and then one line per 5 s, so the true count was
never seen. It is **28** natively and **203** under the plugin in a 60 s run.

**The two sequences are identical for all 28 calls** — same kinds, same function
pointers, same order, including `ExecuteScriptAsset` twice, `GetUIVariables` and
`GetMXTableHelper`. Native then stops. The plugin continues:

```
#28  C   cfunc=0x829E8FA8                              <- native's last
#29  lua
#30  C   cfunc=0x829E8FA8
#31  C   LoadUIAssetDatabasePackage  (sub_824CC218)
#32  C   LoadUIAssetPackage          (sub_824CBF90)
#37  C   IsUIAssetPackageLoaded      (sub_824CC120)
```

So the two bindings listed as "never fires" above are not merely absent — they
are **the very next thing the script would do**.

**Native's #28 returns.** Its thread goes on to open camera `.bxml` files and
write vertex declarations, so the script layer is not hung inside a C binding.
The VM is simply never entered again.

**The dispatches split by thread, and that is the lead:**

| | native | plugin |
|---|---|---|
| main VM thread | t18112 — 26 | t2936 — 163 |
| second thread | *(none)* | t18072 — 38 |
| early thread | t17628 — 2 | t17176 — 2 |

**The thread is the Transition thread**, and it exists in both modes. Under the
plugin it is `t18072`, whose first line is `Transition #1` and which makes 38
dispatches. Natively it is `t2408`, which runs the same `LoadStateMachine` /
`LoaderTick` / `Timing` / `XamInputGetState` cycle and makes **zero**. The
question is therefore not "did the script throw" — no `lua error` or
`lua runerror` line appears in either mode — but **why the Transition thread
never enters the VM natively.**

Under the plugin the VM entry lands inside `LoaderTick`, on iteration 7, in the
window immediately after `sub_82B34998` (RendererDispatch) returns.

**Two wrong readings on the way there, both corrected by measurement:**

- *"Native never calls RendererDispatch."* False. That hook logged only under
  `g_plugin_mode` and called the original silently in native, so its absence
  from a native log meant nothing. It is now mode-neutral. Native calls it and
  it returns non-null (`0x21294134` / `0x212859A0`).
- *"The plugin has a second thread calling it and native does not."* False. The
  plugin's call numbers skip (`#1, #3, #5…`) purely because two threads
  interleave on one counter — and **both** modes have two such threads.

**Most Transition-thread probes are still plugin-only**, so do not read any
other line-by-line difference on that thread as a finding until the probe in
question has been checked for a `g_plugin_mode` guard.

**What does differ, measured on the now-neutral probe: `f1` is exactly `0.00` on
every native call to RendererDispatch**, while the plugin passes varying values
(`-2.00`, `2.00`, `±3.7e19`). `f1` is the frame delta. Native's `Timing`
(`sub_82B70370`) is **stubbed** — it writes `1/60` to `a1+24` and never calls the
original — so a zero dt reaching this call is consistent with that stub being
incomplete. A front end that advances on elapsed time would never advance.

That is a lead, not a conclusion: dt is one measured difference on one call, and
nothing yet connects it to the VM entry. Test it before believing it.

Caveat on naming: identical-code folding makes several trivial bindings share
one address (see **Traps** in [docs/guest_binary.md](docs/guest_binary.md)), so a
`cfunc` value alone cannot name such a binding. The native/plugin *sequence
diff* is what carries the result, and it needs no names.

### Draws land outside the clip volume on z (2026-08-06)

A RenderDoc capture (`mx_2026.08.05_23.55.39_frame2967.rdc`, EID 831,
`DrawIndexedInstanced(1810, 1)`) shows, on every row of the mesh viewer:

| | X | Y | Z | W |
|---|---|---|---|---|
| VS Input `POSITION` | 11.34206 | 11.01484 | 8.23549 | — |
| VS Output `SV_POSITION` | −0.98228 | 0.96940 | **8.23549** | **1.00** |

D3D clips on `0 <= z <= w`, so the draw rasterises nothing.

The shader is not at fault. `kGameVS` does `mul(mvp, float4(pos, 1.0))` and
`BuildViewportMvp` is an **inverse viewport**: row 3 is left identity so `w` is
always 1, and row 2 is `1/zs`, which every run reports at the `zs == 1`
fallback. Untransformed z therefore passes straight through.

The viewport is right, too. Solving `(11.34 − xo)/xs = −0.98228` and
`(11.01 − yo)/ys = 0.96940` gives exactly `xs=640 xo=640 ys=−360 yo=360`, and
the log prints those verbatim. **The input is what is wrong** — a 1810-vertex
mesh whose window-space bounding box is 0.68 x 4.41 pixels in the top-left
corner is in model space, not window space.

Two independent instruments agree, from opposite ends:

- `d3d9: stageI` scores `in-clip 0%` on every shader it calls `window-like
  100%`. That metric does bound z (`hooks_d3d9.cpp`), which is why it sees
  what the PM4 classifier could not.
- `transcode: done 5000 passthrough 53232 (no shader 0, no position 53224,
  read failed 8)`. **`no position` is the exit** — `PickPositionAttribute`
  finds nothing. `no shader` and `read failed` are ~0. Position format 57 is
  the only one that ever transcodes; `packed` colour is `0:0` on every row of
  the colour x surface table.

**A gate that measures one subsystem does not clear the others.** The run above
was read as "PM4 contributes nothing to HLE rendering". That was true of
geometry and **false of pixel shaders**, which had a separate PM4 dependency
through `CapturedPixelShaders()` — 14 shaders decoded only with the ring live,
all 14 via its exact key. It was caught only by checking a counter the gate had
not been designed around. When retiring a subsystem, enumerate its consumers
first and measure each; a headline number from one of them proves nothing about
the rest. (That dependency is now gone — see the pixel shader object table in
[docs/guest_binary.md](docs/guest_binary.md).)

**The untranscoded share depends entirely on `--force_load`, so always state
which.** Measured at equal `done 5000`: with `--force_load=NAT_Farm`,
`passthrough` is 53232 and the class table reads **93.6-93.8% of draws and
99.9% of vertices** not-transcoded (3/3 runs, stable to a fraction of a
percent). Without it, `passthrough` is 2779 and the share is 33%. Passthroughs
are a property of loaded scene content, not of the pipeline — a front-end-only
run transcodes fine and will make this look nine times better than it is.

`ClassifyTransformedDraw` computed z and threw it away, testing only x and y,
so this draw scored `kPartial` — a clean bill of health. It now has a
`kDepthClipped` class, and `passthrough` draws are counted as `kNotTranscoded`
rather than being absent from the table entirely. Counted only; nothing is
skipped unless `--skip_untransformable_draws=true`.

**`kDepthClipped` reads 0 in every run, and that is correct — it is on the
wrong path for the draw that motivated it.** `ClassifyTransformedDraw` is
called only from `TranscodeVertices`, i.e. the PM4 path, which carries ~15k
vertices in a loaded scene. The captured draw comes through the **HLE** path
(`d3d9_draw.cpp`), which carries **10.67M** — `d3d9: HLE shader output ...
applied 37018 draws / 10671287 vertices`. Do not add a second z classifier
there: stageI's `in-clip` already bounds z on that path and already reports
`0%`. The two paths are separately instrumented and the HLE one was never
blind.

The `kNotTranscoded` class still earned its place — it is what made the
99.9%-of-vertices figure visible at all.

**The mesh shape in RenderDoc is not evidence of anything.** Two captures of
EID 831 that look wildly different — a figure and a twisted spike — differ only
by the Mesh Viewer's **Axis Mapping** dropdown (`Y-up, right handed` vs `Y-up,
left handed`). The bounding box is identical in both. Check that dropdown
before reading a shape as a bug.

**`ripgrep` skips `logs/*.log` as binary.** The Grep tool reported zero matches
for `NDC prim=` in a file containing thirty of them, and zero for
`transcode: done` across the whole directory when two runs have it. Every
negative result above was re-established with `grep -a`. Use `grep -a` on these
logs, always — a silent no-match here reads exactly like a feature that never
ran.

---

### The transform choice is a register, not a contest (2026-08-06)

`ApplyShaderOutputs` used to pick between identity and the viewport inverse with
`if (identity_in_clip > viewport_in_clip)` — a strict `>`, so ties went to
viewport. With `in-clip` commonly 0 for both candidates, most draws defaulted
rather than won: measured, that rule disagreed with the hardware on **87789 of
106132 draws (82.7%)**.

The hardware states the answer. **`PA_CL_VTE_CNTL`** says whether the GPU applies
the viewport transform itself, which is exactly the question. Its shadow offset
was derived from the draw-time flush and then corroborated against the viewport
scale/bias dwords sitting beside it — derivation in
[docs/guest_binary.md](docs/guest_binary.md). It contradicted an earlier note in
this file claiming a different register address, so it was checked before being
acted on; the earlier note was stale.

The register reads **0x43F**, one value across every draw: all six viewport
enables set, `vtx_w0_fmt` set. The GPU applies the viewport, so the shader
exports clip space and the transform here is **identity**.

Applied held at 88.84%.

**RenderDoc A/B, frame 3000, NAT_Farm** (`legacy_mvp_tiebreak` existed so both
sides come from one binary). Captured twice: once as-is, once with
`hide_colorless_draws=true` to remove the overpaint. *That cvar was retired
2026-08-07, so the second capture cannot be reproduced as written — the
conclusion below survives, the method does not.*

As-is, both frames are a fullscreen quad split by one diagonal, in the *same*
place — only the colours differ. The present is dominated by the compositor, so
it cannot see what the transform did. Comparing backbuffers was the wrong
experiment.

With colourless draws hidden, neither frame shows recognisable geometry — both
are flat fills. But the fills differ in a way that matters: under the old rule
the compositor's fullscreen triangles covered a **corner sliver**, and under the
VTE-derived identity they cover the **entire viewport**, which is what a
fullscreen pass should do. The flat colour is the separate single-texel UV
collapse, not a transform error.

So the register reading is **supported, not proven**: the fullscreen pass now
lands full-screen, and no world geometry is visible either way because the
compositor still paints one texel over everything. **The UV collapse is now the
thing in front — it is what makes the frame unreadable, whichever transform is
in use.**

### Bink never reaches the renderer: we hook two draws, the guest has three (2026-08-07)

The request was "do the YUV formats for Bink". **The guest does not use packed
YUV texture formats** — its video path is a YUV->RGB *shader* composite over
three single-channel plane textures (four with alpha), so `k_Cr_Y1_Cb_Y0_REP`
and `k_Y1_Cr_Y0_Cb_REP` would have been dead code. Guest side in
[docs/guest_binary.md](docs/guest_binary.md).

The guest keeps its Bink shader handles in globals, so the composite draw can be
matched exactly rather than guessed at. Probing it gave a result none of the
expected outcomes covered — 3/3 runs, byte-identical:

| | native |
|---|---|
| Bink shaders created | **yes** — both pixel shaders and the vertex shader |
| Bink VS bound in captured draws | **24** |
| Bink PS seen by `SetPixelShader` | yes |
| composite draws reaching the HLE draw hook | **0** |
| guest formats rejected on the mapped path | 0 |

**The cause is `D3DDevice_DrawVerticesUP` (`sub_82555B88`), a third draw entry
point we do not hook.** It copies inline vertex data straight into the command
ring and never calls `DrawVertices` or `DrawIndexedVertices` — the only two
draws hooked. So every UP draw is invisible to the renderer.

**This is much bigger than Bink.** That entry point has 40 xrefs from about 30
distinct functions across the engine — UI and particles as well as video. Any
count of "draws the guest issues" taken from our hooks is an undercount by an
unmeasured margin, and every such figure in this file predates knowing that.

**Hooked in `b5073ea`.** `DrawVerticesUP` is semantically "bind stream 0 to
this pointer with this stride and draw", so `BuildAndQueueDraw` takes an
optional `UpVertexData` and synthesises stream 0 from it rather than carving a
second path through `BuildHleDraw`. There is no fetch constant and so no endian
field; 8in32 is used, which is what every bound stream in this game measurably
carries. The vertex pointer is often a caller stack local, so it is read inside
the call while that frame is live, bounds-checked at both ends, and copied.

**It is 45-49% of all draws** — 6806/6813/7379 of a 12500-15000 total across
three 60 s runs. Every draw count quoted anywhere else in this file predates the
hook and is an undercount by roughly half. New geometry appears on screen with
it in place.

With the hook in, the Bink composite is fully visible and matches the decompile
exactly, 3/3 runs identical:

| | sampler 0 | sampler 1 | sampler 2 | sampler 3 |
|---|---|---|---|---|
| no alpha | `FMT_8` 1280x720 | `FMT_8` 640x360 | `FMT_8` 640x360 | none |
| with alpha | `FMT_8` 640x216 | `FMT_8` 320x112 | `FMT_8` 320x112 | `FMT_8` 640x216 |

Y at full resolution, chroma at half, **all `k_8`** — which `319a5c2` already
decodes as `kR8` but deliberately semantic-rejects as "single-channel, not base
colour". Correct for a mask, wrong for a luma plane. That gate and the
single-SRV root signature are what still stand between this and a visible video.

**A note on why this took a probe to find.** The resolved-render-target early
return in `PrepareDrawTexture` discarded its `DescribeHleTexture2D` failure, and
74% of texture attempts take that branch — so an undecodable plane format would
have logged nothing at all. "No YUV format has ever been rejected" was
therefore not evidence of anything until that path was made to speak. It now
feeds the same per-format tally, tagged `mapped`. Third instance in this branch
of a probe that could not have seen the thing it was being read as ruling out.

### The overpaint — measured 2026-08-07, and it is not what it was called

> **Dated.** Measured before the translated shader path existed. The
> conclusions about the resolve snapshot and about reading raw extent rather
> than coverage are still good and are why this is kept; the claim that this is
> what whites out the screen has been superseded — see "Current state".

Probes in `46404e1`, removed in `7394f22`; recover by hash if they are wanted
again. Measured on a plain front-end run with no `--force_load`.

|            | draws | verts | coverage | raw NDC extent | outside the cube |
|------------|-------|-------|----------|----------------|------------------|
| colourless | 1824  | 4.3   | 100.00%  | **2.01**       | 0                |
| coloured   | 338   | 4.0   | 75.92%   | **705.28**     | 186              |

**The colourless draws are geometrically exact.** A raw extent of 2.01 NDC is a
fullscreen quad to two decimals, and not one of the 1824 falls outside the clip
cube. **1669 of 1824 — 91.5% — sample a render target.** They are compositor
passes; their colour was always going to come from that target, and they paint
white because the target is empty.

**So the fix is the resolve snapshot, not the transcode.** Two long-standing
claims are wrong and should not be repeated:

- *"Colourless draws are 16 vertices averaging 52% coverage, small geometry
  smeared across the viewport by a bad transform; a fullscreen pass would be a
  handful of draws."* Every part of that is refuted above. It predated both
  transform fixes on this branch (`fd96c50`, `332eda4`) and was never
  remeasured.
- *"#32 before #30 — until those draws have a real colour, #30's snapshots have
  no visible consumer."* **Inverted.** #30 is what gives them the colour.

**Read raw extent, not coverage.** A clamped coverage figure cannot tell a
legitimate fullscreen quad from geometry blown far outside the screen — both
report 100%. That ambiguity is how the original measurement was misread.

Two side findings, both dead ends for the overpaint but worth not re-deriving:

- The `ps=0x216A8C20` binding census closes itself. All eight `host-format`
  rejections are 32x32 `FMT_8`/`FMT_16` on s9/s10 — single-channel lookups
  correctly excluded as base colours, so that bucket is policy working, not a
  gap. The other four are one 2048x2048 `FMT_8_8_8_8` atlas at `0x1A2E3000`,
  which is **readable and entirely zero** (0 of 262144 sampled bytes) on first
  touch — not a streaming race, so invalidating the empty memoisation changes
  nothing.
- **186 of 338 coloured draws carry raw extents up to 1280**, i.e. screen space
  passing through untransformed. A real defect, unrelated to the overpaint, and
  not what whites out the screen.

### State

With `--force_load=ST_Southwest --registry_override=ReadyToLaunch=1`:
**1482 / 1500 draws applied, 308,669 vertices**, skipping 12 stream + 6 vertex.
Geometry reaches the screen but is scrambled.

**FIXED 2026-08-06 — the UV collapse.** Two vfetch instructions are allowed to
target the same destination register and fill different components of it;
`FetchDestinationSwizzle::kKeep` (7) is what marks a component as not written.
The interpreter's attribute seeding ignored the destination swizzle and wrote
all four components, so the second fetch overwrote the first outright. Observed
directly: `attr[1] fmt=31 -> r0 = (22.969, -16.234, 0, 1)`, a real texcoord,
replaced by `attr[2] fmt=6 -> r0 = (0, 0, 0, 0)`. The shader then exported a
zero UV and the draw sampled one texel.

Honouring the swizzle takes **real collapses from 26 to 0** (draws sampling a
1024x1024 texture with a dead UV). Read the metric by texture extent or it
misleads badly: total "collapsed" only moves 89 to 79, because the remainder are
all **1x1** textures — the auto-exposure reduction chain, 64x64 -> 16x8 -> 4x2
-> 1x1 — where a UV sweep is meaningless by construction and collapse is
correct. Likewise 49 draws *left* the clean `(0,0)..(1,1)` bucket and that is
also correct: they are the same 1024x1024 world textures, now showing tiling
UVs beyond [0,1] instead of a perfect 0..1 sweep that only existed because the
register had been clobbered. The fullscreen post-process passes (160x90,
320x180, 640x360, 1280x720) keep unit range throughout.

**Confirmed visually.** Frame 3000, NAT_Farm, `hide_colorless_draws=true`: a sky
gradient with cloud texture, blue ground, and a tan strip — recognisable
textured content. The same configuration produced a flat brown fill before this
fix. A large black wedge across the middle remains, and is the next defect.

*That cvar was retired 2026-08-07, so this is no longer visible on screen — the
overpaint now covers it. The textured content is still there underneath and is
still what the fix produced; confirming it again needs RenderDoc on the draws
themselves, not a screenshot.*

Without `hide_colorless_draws` the present is unchanged, because the compositor
passes already had unit UVs and were untouched by the swizzle change; what this
fixed was the 1024x1024 world draws sitting underneath the overpaint.

The original text follows, for the record. **The fullscreen post-process
triangles sample a single texel.** Their vertex shader is one instruction,
`MAD export0 = r0 * c255 + c255`, and c255 reads back `(0,0,0,0)`. The constant
file is populated (70 live vec4 across indices 0..218) and the read offset is
confirmed by the setter's own arithmetic, so the slot is genuinely never
published through the device shadow. The PM4 side showed the other publisher:
`LOAD_ALU_CONSTANT write reg=0x43F0 dwords=16` is register `0x4000 + 252*4`,
covering slots 252–255.

**The fix this file used to propose — source the constant file from the
`LOAD_ALU_CONSTANT` shadow — is no longer available.** That shadow lived in the
PM4 translator, deleted in 4dd1790. The ring is still parsed, but nothing
tracks constants from it, and reinstating that would be a PM4 dependency on the
render path, which is the thing the pure-HLE work removed.

**c255 is published by the shader itself, through a ring packet.** This file
said so originally, then two rounds of my own analysis wrongly concluded nothing
published it. Both are recorded here because the mistakes are instructive.

A routine called from the draw-time flush walks a table in the shader object and
emits one PM4 Type-3 LOAD_ALU_CONSTANT packet per entry — the emitter, the packet
header and the guest table layout are in
[docs/guest_binary.md](docs/guest_binary.md).

Measured: every shader publishes one entry covering **c252..c255**, holding
screen-space scale/bias — `(0.5, -0.5, 0, 0)`, `(0, 1, 0.5, -0.5)`,
`(1, 2, 0.5, -0.5)` — which is the exact
`LOAD_ALU_CONSTANT reg=0x43F0 dwords=16` this file cited from the start.

None of it passes through `device + 0x780`, which is the only place
`CaptureVertexConstants` looked. `OverlayShaderConstants` now applies it after
the device file, matching hardware order (the load is emitted at draw time,
after any `SetVertexShaderConstantF`). The data is read from the shader object
in guest memory — the packet only carries an address — so **no PM4 is involved**
and the earlier retraction of the PM4-shadow fix still stands on its own terms.

Result: constant reads returning zero went **72 of 128 probes to 0 of 90**, the
live range went 0..218 to **0..255**, and c255 reads back `(0.5, -0.5, 0, 0)`.
Applied held at 88.85%. **stageI did not move** — 5 shaders at >=90% clip-like,
24% of execs, identical before and after. The constants were wrong and are now
right; that was not what the clip-volume problem is.

**Two ways I got this wrong, both worth remembering.** First I argued the ring
and the device shadow were the same memory, because `sub_82564B00` flushes
`device + 0x780` verbatim. True of *that* packet, and irrelevant: a second,
unrelated LOAD_ALU_CONSTANT exists. Proving one publisher is a copy does not
enumerate the publishers. Second, a probe did walk the right table but parsed it
with the neighbouring list's layout, read `reg_index` 0xFC as a byte offset,
compared it against c255's byte offset 0x12F0, and reported "none covers c255".
`0xFC` is c252 and the count covered c255. A field read in the wrong units is
indistinguishable from a negative result, and it survived because the answer it
gave was the one I already believed.

---
