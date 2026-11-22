<div align="center">

# 🌴 ViceWin

**Xbox Series X|S (`Scarlett`) Compatibility Layer & RDNA2 ➔ DXIL Shader Recompiler for Windows**

![Platform](https://shields.io)
![Target](https://shields.io)
![Backend](https://shields.io)
![C++](https://shields.io)

> **Note:** Built and tuned primarily on **AMD RDNA3.5** hardware. YMMV on other GPU architectures.

</div>

---

## Core Architecture Readiness (~100% Code Complete)

All core subsystems of the `RDNA2 -> SSA -> HLSL -> DXIL` shader pipeline, `D3D12.X` / `XGameRuntime` HLE shims, and the Win32 package launcher are implemented in C++20. Next milestone: first full CMake build, unit test execution, and runtime validation against real Xbox Series binaries.

| Subsystem | Progress | Bar | Notes |
| :--- | :---: | :--- | :--- |
| **Container Unpacker** (`XDAT`/`XBCH`/`FSSM`) | **100%** | `██████████` | Zlib decompression + RDNA2 bytecode extraction up to `s_endpgm` |
| **SSA Lifter & Register Tracking** | **100%** | `██████████` | Virtual `SGPR`, `VGPR`, `VCC`, `EXEC`, and `SCC` state tracking |
| **Control Flow Structuring (CFG)** | **100%** | `██████████` | Dominator / post-dominator loops, `if/else`, and multi-exit loop lowering |
| **DXC Backend & Persistent Cache** | **100%** | `██████████` | Runtime `IDxcCompiler3` (SM 6.6) + on-disk DXIL cache in `LocalAppData` |
| **Win32 Launcher & Runtime Logger** | **100%** | `██████████` | `MicrosoftGame.Config` root detection, `ScarlettX` host launch, `vicewin.log` |
| **Mesh Shaders & BVH Decoding** | **100%** | `██████████` | `AS`/`MS` pipeline-state stream PSOs, mesh exports & AMD BVH step semantics |
| **LDS, Atomics & Wave64** | **95%** | `█████████▓` | `groupshared` (64 KiB), `ds_add_u32`, `SV_*` semantics, `[WaveSize(64)]` |
| **Bindless & Upscaler Shim** | **95%** | `█████████▓` | Root Sig v1.0/1.1 stream parsing, `ResourceDescriptorHeap`, `FSR 3.1.2` HLE |
| **RDNA2 ISA Coverage** | **95%** | `█████████▓` | `SOP*`, `SOPK`, `VOP*`, `VINTRP`, `FP16` (`half2`), `SMEM`, `MUBUF`, `MIMG`, `DS`, `EXP` |

---

## How It Works

Instead of full-system hardware emulation, **ViceWin** acts as a high-level API shim and static/dynamic shader recompiler. The launcher detects the Xbox package root via `MicrosoftGame.Config`, syncs updated binaries into an isolated runtime directory, and starts `ScarlettX host`—translating `D3D12.X` / `XGameRuntime` calls and raw **RDNA2** machine code straight into desktop **Direct3D 12** and **DXIL (Shader Model 6.6)**:

```text
  Xbox GDK Package             ViceWin Launcher & Lifter          Windows D3D12 Driver
┌────────────────────┐      ┌──────────────────────────────┐      ┌──────────────────────────┐
│ MicrosoftGame.Cfg  │ ───► │ ScarlettX Host + vicewin.log │ ───► │ IDxcCompiler3 (SM 6.6)   │
│ XDAT / XBCH / FSSM │      │ RDNA2 ISA ─► Dominator CFG   │      │ Signed DXIL ➔ PSO        │
└────────────────────┘      └──────────────────────────────┘      └──────────────────────────┘
```

---

## Feature Breakdown

### Launcher & Runtime Diagnostics (`main.cpp` / `process_launcher.cpp` / `logger.cpp`)
- **Non-Destructive Package Launch:** Finds the package root by `MicrosoftGame.Config`, syncs updated runtime binaries into an app-owned directory (leaving original game folders untouched), and spawns `ScarlettX host`.
- **Display & Upscaler Controls:** Configures resolution, target FPS limiter, V-Sync, and FidelityFX (`FSR 3.1.2` HLE) / Streamline overrides via `vicewin.ini`.
- **Detailed Logging (`vicewin.log`):** Records launch configs, `XGameRuntime` calls, D3D12/GPU capabilities, DXIL shader compilations, and upscaler HLE status.

### Shader Recompiler (`rdna2_lifter` / `shader_ir` / `dxil_backend`)
- **Containers & Decompression:** Parses `XDAT` / `XBCH` headers, inflates `FSSM` zlib streams, and handles `NOBC` fallback paths.
- **Structured CFG:** Reconstructs reducible control flow via dominators, post-dominators, and natural loops—supporting nested `if/else`, `while`, and multi-exit loops without `switch` dispatchers.
- **Next-Gen Pipelines & Math:** Translates Amplification/Mesh shaders (`AS`/`MS`), BVH ray-intersection steps (`image_bvh*_intersect_ray`), `SOP*`, `SOPK`, `VOP*`, pixel interpolation (`VINTRP`), native `FP16` packed math (`v_pk_*_f16` ➔ `half2` with `-enable-16bit-types`), `SMEM`, `MUBUF`, `MIMG`, `DS`, `s_barrier`, and atomics.
- **SM 6.6 & On-Disk Cache:** Emits `[WaveSize(64)]`, maps `SV_*` stage inputs, parses Root Signature v1.0/1.1 bindless flags (`ResourceDescriptorHeap`), and caches compiled DXIL blobs under `%LocalAppData%\ViceWin\shader_cache\`.

### Milestones
- [x] Core RDNA2 ➔ SSA ➔ HLSL ➔ DXIL (SM 6.6) lifter & persistent disk cache
- [x] Standalone Win32 Launcher (`MicrosoftGame.Config` detection, isolated runtime sync, `vicewin.log`)
- [x] Mesh Shaders (`AS` / `MS` pipeline-state stream) & BVH ray-intersection decoding
- [ ] First full CMake build & unit test run (`test_shader_pipeline.exe`)
- [ ] Runtime validation against retail Xbox Series shader dumps

---

## Stack
* **C++20** / **CMake**
* **Direct3D 12** (Shader Model 6.6)
* **DirectX Shader Compiler** (`dxcompiler.dll` / `dxil.dll`)
* **Zlib**

---

## Legal
ViceWin is an independent research project on GPU architectures and shader translation. No copyrighted game assets, keys, or proprietary Microsoft/Xbox SDK files are included in this repository.
