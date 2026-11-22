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

## Core Readiness (~92%)

Most of the `RDNA2 -> SSA -> HLSL -> DXIL` shader translation pipeline and `D3D12.X` runtime shims are written. Currently finalizing edge-case hardware features before running the first full CMake build and runtime tests.

| Subsystem | Progress | Bar | Notes |
| :--- | :---: | :--- | :--- |
| **Container Unpacker** (`XDAT`/`XBCH`/`FSSM`) | **100%** | `██████████` | Zlib decompression + RDNA2 bytecode extraction up to `s_endpgm` |
| **SSA Lifter & Register Tracking** | **100%** | `██████████` | Virtual `SGPR`, `VGPR`, `VCC`, `EXEC`, and `SCC` state tracking |
| **Control Flow Structuring (CFG)** | **100%** | `██████████` | Dominator / post-dominator loops, `if/else`, and multi-exit loop lowering |
| **LDS & Atomics** (`groupshared` 64 KiB) | **95%** | `█████████▓` | `ds_read/write`, `ds_add_u32`, `MUBUF` atomics, and `s_barrier` |
| **System Semantics & Wave64** | **95%** | `█████████▓` | `SV_*` stage mapping + native `[WaveSize(64)]` (SM 6.6) |
| **Bindless & Root Signature 1.1** | **90%** | `█████████░` | `ResourceDescriptorHeap` direct indexing + `T#`/`S#` sampler ABI |
| **DXC Compiler Backend** (`HLSL -> DXIL`) | **90%** | `█████████░` | Runtime `IDxcCompiler3` compilation & shader cache key versioning |
| **RDNA2 ISA Coverage** | **80%** | `████████░░` | `SOP*`, `VOP*`, `SMEM`, `MUBUF`, `MIMG`, `DS`, `EXP` (Mesh/DXR next) |

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

### Shader Recompiler (`rdna2_lifter` / `shader_ir`)
- **Containers & Decompression:** Parses `XDAT` / `XBCH` headers, inflates `FSSM` zlib streams, and handles `NOBC` fallback paths.
- **Structured CFG:** Reconstructs reducible control flow via dominators, post-dominators, and natural loops—supporting nested `if/else`, `while`, and multi-exit loops without ugly `switch` dispatchers.
- **Memory & Sync:** Translates `SMEM`, `MUBUF`, `MIMG` (`image_load` / `image_sample*`), `DS` (`ds_read/write_b32/b64/b128`), `s_barrier`, and atomic ops (`ds_add_u32`, `MUBUF` atomics).
- **SM 6.6 Features:** Emits `[WaveSize(64)]` for Wave64 shaders, maps `SV_VertexID` / `SV_Position` / `SV_DispatchThreadID`, and uses `ResourceDescriptorHeap` for bindless indexing.

### D3D12.X Runtime Shim (`d3d12_xs_shim`)
- **Root Signatures:** Translates `D3D12XboxCreateRootSignatureEx` with Root Signature 1.1 direct indexing flags and `T#`/`S#` sampler bindings.
- **PSO Creation:** Passes stage descriptors and compiled DXIL blobs into desktop `ID3D12Device`.
- **Test Suite:** Includes synthetic unit tests (`test_shader_pipeline.cpp`) covering ISA decoding, CFG, LDS, atomics, and DXC compilation.

### Still To Do
- [ ] Mesh Shaders (`AS` / `MS` stages, `exp prim*`) & Inline Raytracing (`image_bvh_intersect_ray` ➔ `RayQuery`)
- [ ] FP16 packed math (`v_pk_*_f16`) & persistent on-disk DXIL cache
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
