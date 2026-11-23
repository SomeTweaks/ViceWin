<div align="center">

# 🌴 ViceWin

**Xbox Series X|S (`Scarlett`) Compatibility Layer & RDNA2 ➔ DXIL Shader Recompiler for Windows**

![Platform](https://shields.io)
![Target](https://shields.io)
![Backend](https://shields.io)
![C++](https://shields.io)

> **Note:** Built and tuned primarily on **AMD RDNA3.5** hardware. YMMV on other GPU architectures.

</div>

![ViceWin Telemetry Status](status.png)

## How It Works

Instead of full-system hardware emulation, **ViceWin** acts as a native Win32-backed HLE compatibility layer and static/dynamic shader recompiler. The launcher detects the Xbox package root via `MicrosoftGame.Config`, prepares an isolated runtime directory, and starts `ScarlettX host`—mapping PE64 imports directly to real Windows 11 APIs and lifting raw **RDNA2** machine code into desktop **Direct3D 12** and **DXIL (Shader Model 6.6)**:

```text
  Xbox GDK Package             ViceWin Host & HLE Layer           Windows 11 / D3D12
┌────────────────────┐      ┌──────────────────────────────┐      ┌──────────────────────────┐
│ MicrosoftGame.Cfg  │ ───► │ PE64 Mapper (TLS + .pdata)   │ ───► │ Win32 / XAudio2 / XInput │
│ XDAT / XBCH / FSSM │      │ RDNA2 ISA ─► Dominator CFG   │      │ IDxcCompiler3 (SM 6.6)   │
└────────────────────┘      └──────────────────────────────┘      └──────────────────────────┘
```

---

## Feature Breakdown

### Launcher, PE64 Loader & Diagnostics (`main.cpp` / `pe_mapper.cpp` / `logger.cpp`)
- **Non-Destructive Package Launch:** Finds the package root via `MicrosoftGame.Config`, syncs updated binaries into an app-owned directory (leaving original game folders untouched), and spawns `ScarlettX host`.
- **Full PE64 Initialization:** Handles `IMAGE_DIRECTORY_ENTRY_TLS` callbacks prior to entry point, registers x64 `.pdata` exception tables via `RtlAddFunctionTable`, enforces section page protections, and resolves imports strictly via explicit `.def` tables without fake `stub_thunk` fallbacks.
- **Diagnostics & Offline Shader Validator:** Writes detailed runtime traces to `vicewin.log` and includes `vicewin_dump_tester.exe` to batch-validate `XDAT`/`XBCH` shader archives on disk.

### Shader Recompiler (`rdna2_lifter` / `shader_ir` / `dxil_backend`)
- **Structured CFG & Next-Gen Pipelines:** Reconstructs reducible control flow via dominators and natural loops (`if/else`, `while`, multi-exit loops), supports Amplification/Mesh shaders (`AS`/`MS`), and preserves AMD per-node BVH ray-intersection semantics.
- **Math, Interpolation & Memory:** Translates `SOP*`, `SOPK`, `VOP*`, pixel interpolation (`VINTRP`), native `FP16` packed math (`v_pk_*_f16` ➔ `half2` with `-enable-16bit-types`), `SMEM`, `MUBUF`, `MIMG`, `DS` (`groupshared` up to 64 KiB), `s_barrier`, and atomics.
- **SM 6.6 & On-Disk Cache:** Emits `[WaveSize(64)]`, maps `SV_*` stage inputs, parses Root Signature v1.0/1.1 bindless flags (`ResourceDescriptorHeap`), and caches compiled DXIL blobs under `%LocalAppData%\ViceWin\shader_cache\`.

### Win32-Backed System HLE (`kernelx` / `xgameruntime` / `dstorage_xs` / `xcrdapi`)
- **Memory & Threading:** Implements 64 KiB-aligned `XMem` allocation with a 13.5 GiB title memory profile and virtual 8-core / 16-thread Xbox Series CPU affinity mapping.
- **Storage & Decompression:** Provides `DirectStorage` COM queues and real Zlib / raw-DEFLATE stream decompression via `xcrdapi`.
- **Audio, Input, Saves & Media:** Routes `XAudio2` / `AcpHal` to `XAudio2_9.dll`, `GameInput` (including rumble) to `XInput1_4.dll`, `Media Foundation` to `System32`, `XNetworking` to `iphlpapi`, and `XGameSave` containers to `%LocalAppData%\ViceWin\saves\`.

### Milestones
- [x] Core RDNA2 ➔ SSA ➔ HLSL ➔ DXIL (SM 6.6) lifter, Mesh Shaders (`AS`/`MS`), BVH decoding & disk cache
- [x] Standalone Win32 Launcher (`MicrosoftGame.Config` detection, isolated runtime sync, `vicewin.log`)
- [x] Zero-stub PE64 Loader (TLS, `.pdata`, `.def` exports) & Win32-backed HLE (`XMem`, `DirectStorage`, `XAudio2`, `GameInput`, `XGameSave`)
- [ ] First full CMake build & unit test suite execution
- [ ] Offline shader cache pre-warming via `vicewin_dump_tester.exe` & live runtime validation

---

## Stack
* **C++20** / **CMake**
* **Direct3D 12** (Shader Model 6.6)
* **DirectX Shader Compiler** (`dxcompiler.dll` / `dxil.dll`)
* **Win32 API / XInput / XAudio2 / Media Foundation / Zlib**

---

## Legal
ViceWin is an independent research project on GPU architectures and binary/shader translation. No copyrighted game assets, keys, or proprietary Microsoft/Xbox SDK files are included in this repository.
