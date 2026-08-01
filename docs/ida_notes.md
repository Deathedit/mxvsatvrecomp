# IDA Bookmarks & Notes

Consolidated IDA bookmarks installed across sessions. The IDA DB lives at `assets/default.xex.i64`, imagebase `0x82000000`.

Reference: AGENTS.md (operational hub), docs/loader_render_block.md, docs/asset_format.md, docs/pm4_pipeline.md.

---

## All bookmarks (slot 0-28)

| Slot | Address | Label | Source |
|------|---------|-------|--------|
| 0 | 0x8253ABFC | State0_Init (calls sub_82304158/alloc96B) | AssetDB_LoadStateMachine investigation |
| 1 | 0x8253AC8C | State1_WaitEvent_NtSetEventPlusSceneTransition | AssetDB_LoadStateMachine investigation |
| 2 | 0x8253AD3C | State2_IdleClearRenderBusy | AssetDB_LoadStateMachine investigation |
| 3 | 0x8253B00C | State3_DatabaseLoad_PlayerModeCheck_Loaded_Ready_Iteration | AssetDB_LoadStateMachine investigation |
| 4 | 0x8253B2A8 | State4_SubsceneCreate_eng+8 vt[16] registry | AssetDB_LoadStateMachine investigation |
| 5 | 0x8253B3E8 | State5_LoadingProgress_3subsystemRatioWait | AssetDB_LoadStateMachine investigation |
| 6 | 0x8253B504 | State6_PlayerSetup_UniqueId_NetworkNoPlayers | AssetDB_LoadStateMachine investigation |
| 7 | 0x8253B6D8 | State7_AsyncStart sub_8234EFD8 | AssetDB_LoadStateMachine investigation |
| 8 | 0x8253B738 | State8_AsyncWait sub_8234EFE8 | AssetDB_LoadStateMachine investigation |
| 9 | 0x8253AD4C | State9_LaunchActivity sub_82B09DA0 + scene transition | AssetDB_LoadStateMachine investigation |
| 10 | 0x8253ADF0 | State10_SeriesAdvance NeedSeriesAdvanceEvent | AssetDB_LoadStateMachine investigation |
| 11 | 0x8253B754 | State11_Finalize sub_82537C68 -> transition to State2 | AssetDB_LoadStateMachine investigation |
| 12 | 0x82538618 | SceneTransition_Kickoff: InRealWorld/InUIWorld flag flips + dword_830BE190 vt[17] + vt[27] + vt[10] | AssetDB_LoadStateMachine investigation |
| 13 | 0x82BAA650 | DatabaseAndPackageIndexLoader: parses .xenon.database XML, builds package index, supports SPUZlib/LZX/Zlib codecs | Asset loader investigation |
| 14 | 0x82B67128 | AssetFile_Open: generic file open dispatcher (23 callers - all asset file types) | Asset loader investigation |
| 15 | 0x82646D58 | SSM_StateCompiler_Dispatch: bundled Xbox 360 XDK shader state compiler (xdk-main-sep10) | Shader microcode investigation |
| 16 | 0x82AD0378 | ShaderAsset_Unpack: reads v7(4B ignored flag) + v8(4B sub-count) then per-sub dispatches type 0/1 | Shader microcode investigation |
| 17 | 0x82A71958 | `AES_set_decrypt_key` (e_aes.c — internal, called via EVP init + CMS KEKRI) | OpenSSL AES investigation |
| 18 | 0x82A727A8 | `AES_decrypt` (block, 1468B, inverse-Sbox @0x8211fd70) | OpenSSL AES investigation |
| 19 | 0x82A721D8 | `AES_encrypt` (block, referenced via OpenSSL FIPS table @0x821bfbc8) | OpenSSL AES investigation |
| 20 | 0x82A8ECE8 | `AES_cbc_decrypt` (EVP callback body, derives from AES_decrypt fn ptr) | OpenSSL AES investigation |
| 21 | 0x82A8EB40 | `AES_cbc_encrypt` (mirror, uses AES_encrypt fn ptr) | OpenSSL AES investigation |
| 22 | 0x82A93150 | `CMS_decrypt` (cms_lib.c — switch on content type 21/22/23/25/26) | OpenSSL AES investigation |
| 23 | 0x8211fd70 | AES inverse S-box + rcon + `"RC2 part of OpenSSL 1.0.0 29 Mar 2010"` string | OpenSSL AES investigation |
| 24 | 0x82B34998 | `RendererDispatchBlock` — sub_82B34998 (the dispatch LoaderTick calls) | Path 2 shim design |
| 25 | 0x82B38558 | `TerminatorVtableCtor` — installs off_8213F70C vtable at *a1; host hook overrides | Path 2 shim design |
| 26 | 0x82B2C9D0 | `TerminatorTlsGate` — *(a1+61104) == TLS slot; host hook returns 1 | Path 2 shim design |
| 27 | 0x82B307D8 | `NullDerefDispatch` — sub_82B2D030(*(a1+1288)) NULL crash; host hook no-op | Path 2 shim design |
| 28 | 0x82B43AC8 | `EngineSlot8_WriteAssetDB` — eng->vt[17] writes `*(eng+8) = AssetDB_ptr`; SKIPPED by mid-ASM hook #4 | eng+8 writer trace |

---

## Bookmark groups by investigation theme

### AssetDB_LoadStateMachine (slots 0-12)
Located per state in the 12-case switch at `sub_8253AA40`. See docs/asset_format.md (AssetDB_LoadStateMachine section).

### Asset loader infrastructure (slots 13-14)
- `DatabaseAndPackageIndexLoader` parses `.xenon.database` XML, builds in-memory package index.
- `AssetFile_Open` dispatches BXML file opens (22 callers; all config-type loaders).

See docs/asset_format.md (Asset loader infrastructure section).

### Shader microcode load path (slots 15-16)
- `SSM_StateCompiler_Dispatch` is the bundled Xbox 360 XDK shader state compiler.
- `ShaderAsset_Unpack` reads shader stage descriptors + dispatches vertex/pixel sub-stages.

See docs/asset_format.md (Shader microcode load path section).

### OpenSSL AES in guest binary (slots 17-23)
These are **TLS-only** (HttpClient for Xbox Live service traffic), NOT the asset decrypt routine. The S-box at `0x8211fd70` is the inverse S-box used for AES decryption in OpenSSL's FIPS-validated implementation.

See docs/asset_format.md (OpenSSL crypto bundle section).

### Path 2 GPU plugin shim (slots 24-27)
The `off_8213F70C` vtable analysis led to a shim design that was later found unnecessary — the real vtable is `off_8213F7A4` (no terminators). See docs/loader_render_block.md (Path 2 section) and docs/pm4_pipeline.md (vtable correction section).

### eng+8 writer (slot 28)
`sub_82B43AC8` (engine vt[17]) is the writer of `*(eng+8) = AssetDB`. Disabling mid-ASM hook #4 lets this run naturally in native mode. See docs/loader_render_block.md (eng+8 writer traced section).