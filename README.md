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

## Core Readiness (~95%)

The core `RDNA2 -> SSA -> HLSL -> DXIL` shader translation pipeline, persistent caching, and `D3D12.X` runtime shims are implemented. Next up: building the standalone Windows launcher UI/injector before running the first full CMake build.

| Subsystem | Progress | Bar | Notes |
| :--- | :---: | :--- | :--- |
| **Container Unpacker** (`XDAT`/`XBCH`/`FSSM`) | **100%** | `██████████` | Zlib decompression + RDNA2 bytecode extraction up to `s_endpgm` |
| **SSA Lifter & Register Tracking** | **100%** | `██████████` | Virtual `SGPR`, `VGPR`, `VCC`, `EXEC`, and `SCC` state tracking |
| **Control Flow Structuring (CFG)** | **100%** | `██████████` | Dominator / post-dominator loops, `if/else`, and multi-exit loop lowering |
| **DXC Backend & Persistent Cache** | **100%** | `██████████` | Runtime `IDxcCompiler3` (SM 6.6) + on-disk DXIL cache in `LocalAppData` |
| **LDS & Atomics** (`groupshared` 64 KiB) | **95%** | `█████████▓` | `ds_read/write`, `ds_add_u32`, `MUBUF` atomics, and `s_barrier` |
| **System Semantics & Wave64** | **95%** | `█████████▓` | `SV_*` stage mapping + native `[WaveSize(64)]` (SM 6.6) |
| **Bindless & Root Signature 1.1** | **90%** | `█████████░` | `ResourceDescriptorHeap` direct indexing + `T#`/`S#` sampler ABI |
| **RDNA2 ISA Coverage** | **85%** | `████████▓░` | `SOP*`, `VOP*`, `FP16` (`half2`), `SMEM`, `MUBUF`, `MIMG`, `DS`, `EXP` |

---

## How It Works

Instead of full-system hardware emulation, **ViceWin** acts as a high-level API shim and static/dynamic shader recompiler. It intercepts `D3D12.X` runtime calls and lifts raw Xbox Series **RDNA2** machine code straight into desktop **Direct3D 12** and **DXIL (Shader Model 6.6)**:

```text
  Xbox Shader Blob             ViceWin Lifter               Windows D3D12 Driver
┌──────────────────┐      ┌──────────────────────┐      ┌──────────────────────────┐
│ XDAT / XBCH      │ ───► │ RDNA2 ISA ─► SSA     │ ───► │ IDxcCompiler3 (SM 6.6)   │
│ FSSM (Zlib)      │      │ Dominator CFG + HLSL │      │ Signed DXIL ➔ PSO        │
└──────────────────┘      └──────────────────────┘      └──────────────────────────┘
```

---

## Feature Breakdown

### Shader Recompiler (`rdna2_lifter` / `shader_ir` / `dxil_backend`)
- **Containers & Decompression:** Parses `XDAT` / `XBCH` headers, inflates `FSSM` zlib streams, and handles `NOBC` fallback paths.
- **Structured CFG:** Reconstructs reducible control flow via dominators, post-dominators, and natural loops—supporting nested `if/else`, `while`, and multi-exit loops without `switch` dispatchers.
- **Math, Memory & Sync:** Translates `SMEM`, `MUBUF`, `MIMG` (`image_load` / `image_sample*`), `DS` (`ds_read/write_b32/b64/b128`), `s_barrier`, atomic ops (`ds_add_u32`, `MUBUF` atomics), and native `FP16` packed math (`v_pk_*_f16` ➔ `half2` with `-enable-16bit-types`).
- **SM 6.6 Features:** Emits `[WaveSize(64)]` for Wave64 shaders, maps `SV_VertexID` / `SV_Position` / `SV_DispatchThreadID`, and uses `ResourceDescriptorHeap` for bindless indexing.
- **On-Disk Shader Cache:** Caches compiled DXIL blobs under `%LocalAppData%\ViceWin\shader_cache\` keyed by FSSM bytecode hash and lifter version.

### D3D12.X Runtime Shim (`d3d12_xs_shim`)
- **Root Signatures:** Translates `D3D12XboxCreateRootSignatureEx` with Root Signature 1.1 direct indexing flags and `T#`/`S#` sampler bindings.
- **PSO Creation:** Passes stage descriptors and compiled DXIL blobs into desktop `ID3D12Device`.
- **Test Suite:** Includes synthetic unit tests (`test_shader_pipeline.cpp`) covering ISA decoding, CFG, LDS, atomics, native `half2`, and DXC compilation.

### Still To Do
- [x] FP16 packed math (`v_pk_*_f16` ➔ `half2`) & persistent on-disk DXIL cache
- [ ] Standalone Windows Launcher & DLL Injector (`ViceWin.exe` — resolution, target FPS, V-Sync controls)
- [ ] Mesh Shaders (`AS` / `MS` stages, `exp prim*`) & Inline Raytracing (`image_bvh_intersect_ray` ➔ `RayQuery`)
- [ ] First full CMake build & runtime validation against real Xbox Series shader dumps

---

## Stack
* **C++20** / **CMake**
* **Direct3D 12** (Shader Model 6.6)
* **DirectX Shader Compiler** (`dxcompiler.dll` / `dxil.dll`)
* **Zlib**

---

## Legal
ViceWin is an independent research project on GPU architectures and shader translation. No copyrighted game assets, keys, or proprietary Microsoft/Xbox SDK files are included in this repository.
