# GTA VI retail boot — status and provenance

Date: 2026-11-23. Package: `F:\GTA6.Xbox.v1.0.2.1-CYBERLEEK` (TitleId 7952987A,
`game_scarlett_master.exe`, 114.6 MB, 68 imported modules, 587 symbols).

## Where the boot gets to

```
[loader] Mapped ...\game_scarlett_master.exe at 0x140000000 (relocated=no, imports=587)
[loader] Entering native PE entry point at 00000001456A1EC8
[pe_mapper] Patched 1846 guest syscall sites to ud2
```

All imports resolve, the image maps at its preferred base, and execution enters
the guest entry point. After entry the title runs without logging: it has not
reached its first raw syscall yet (no `unknown console syscall` line appeared in
45 s).

## Modules closed in this pass

| Module | Symbols | Implementation |
|---|---|---|
| `xmem.dll` | 12 | real physical-page/mapping/working-set implementations in `kernelx_memory.cpp`, registered in the HLE registry under `xmem.dll` and exported from `xgameruntime.dll` |
| `chad.dll` | 5 | new `src/shims/xdsp_shim.cpp`; DSP graph reported active, writes only into `VirtualQuery`-verified writable pointers |
| `xg_xs.dll` | 2 | `XGCreateTextureComputer` / `XGSuggestSwizzleMode` aliases over the measured `XGComputeOptimalSwizzleMode` / `XGCreateTexture2DComputer` |
| `XboxBvhBuilder.dll` | 7 (mangled C++) | new `src/shims/xbox_bvh_builder_class.cpp`; export dump confirms all 7 mangled names match the imports. Prebuild info and copy forward to real D3D12 |
| `libHttpClient.GDK.dll` | 24 | offline getters/setters (delegated, see below) |
| `Bink2Scarlett.dll` | 16 | video shims without a decoder (delegated, see below) |
| `xapu.dll` | 5 | APU command-stream surface in the AcpHal shim + redirect `xapu.dll → AcpHal.dll` |

Launcher: passes `--in-process` (a console dump cannot be loaded by a child
process), and no longer refuses to start when `dxcompiler.dll` or the VC++
runtime is absent from the release folder.

## Delegated work — what was carried, and how it was verified

Two child agents were dispatched; both settled as completed and both results are
in the tree and exercised by the boot:

- **`Bink2Scarlett.dll` shim** (child agent, `src/shims/bink2_scarlett_shim.cpp`).
  Verified: export dump of the built DLL lists 28 exports including
  `BinkOpenWithOptions` and `BinkShouldSkip`; the boot resolves all 23 Bink
  imports before mapping. **Limit, by design:** there is no Bink decoder —
  `BinkOpenWithOptions` returns `nullptr` and `BinkShouldSkip` returns TRUE, so
  the title's video streams are skipped and logged. Resolution is proven;
  playback is not implemented.
- **`libHttpClient.GDK.dll` shim** (child agent,
  `src/shims/libhttpclient_shim.cpp`). Verified: export dump lists 43 exports
  including `HCCleanupAsync` and `HCTraceImplMessage`; the boot advanced past
  that module (the next stop moved to `xapu.dll`). **Limit, by design:** all
  network calls complete offline with zero headers/bodies; no HTTP transport.
  The child also noted that the new entry points are absent from
  `include/scarlettx/network/httpclient_abi.h`, so C++ consumers inside the tree
  cannot see their prototypes yet — a header follow-up, not a boot blocker.

Nothing delegated was left uncovered: the modules those two children carried are
the modules that stopped being blockers in the boot.

## Known limits of what I wrote myself

- `XboxBvhBuilder::BuildRaytracingAccelerationStructureBatched2X` is logged and
  skipped (the Xbox batched descriptor layout is not decoded), and
  `Rehydrate...` returns S_OK without work. Ray-traced effects therefore stay
  off; `GetPrebuildInfo` and `Copy` do call the real D3D12 API.
- `chad.dll`/`xapu.dll` report success so the audio engine falls back to the
  xaudio2 software path; no DSP processing happens.

## Next blockers (in order)

1. Bind console syscall numbers from the log: run for 2–3 minutes, then
   `vicewindev.exe --scan-log vicewin.log` and fill `SCARLETTX_SYSCALL_MAP`.
2. RDNA2 lifter coverage for RAGE shaders (`image_bvh_intersect_ray`,
   `image_gather4`, VOP3/VOP3P/DS gaps).
3. Encrypted retail content: `XvdContainer` still refuses encrypted `.rpf`/
   XVD images.
