<div align="center">

# 🌴 ViceWin

**Experimental Xbox Series X|S (`Scarlett`) Compatibility Layer & Shader Translator for Windows**

[![Platform](https://shields.io)](https://github.com)
[![Architecture](https://shields.io)](https://github.com)
[![Graphics API](https://shields.io)](https://github.com)
[![Language](https://shields.io)](https://github.com)
[![Status](https://shields.io)](https://github.com)

*A low-level binary & graphics translation layer focused on running Xbox Series X|S titles (built with next-gen engines like RAGE) natively on Windows PC via D3D12.*

</div>

---

## 🏗️ Architecture Overview

Unlike traditional full-system emulators, **ViceWin** operates as a high-level compatibility shim and static/dynamic shader recompiler, translating Xbox Series (`D3D12.X`) calls and native **AMD RDNA2** machine code directly into desktop **Direct3D 12** and **DXIL**.

```text
┌───────────────────────────┐      ┌──────────────────────────┐      ┌─────────────────────────┐
│  Xbox Series Container    │      │   ViceWin Shader Lifter  │      │   Windows D3D12 Driver  │
│  (XDAT / XBCH / FSSM)     │ ───► │  RDNA2 ISA ─► SSA ─► HLSL│ ───► │  DXC (SM 6.6) ─► DXIL   │
└───────────────────────────┘      └──────────────────────────┘      └─────────────────────────┘
```

---

## 📊 Implementation Progress & Roadmap

### 1. Shader Recompiler (`RDNA2 ISA ➔ SSA ➔ HLSL / DXIL`)

| Component / Feature | Status | Details |
| :--- | :---: | :--- |
| **Container Unpacking (`XDAT` / `XBCH` / `FSSM`)** | ✅ Done | Zlib decompression & raw RDNA2 bytecode extraction up to `s_endpgm` |
| **RDNA2 ISA Decoder** | 🟡 Partial | Supports basic `SOP*`, `VOP*`, `SMEM`, `MUBUF`, `MIMG`, and `EXP` encodings |
| **SSA Lifting & Register Tracking** | ✅ Done | Virtual tracking of `SGPR`, `VGPR`, `VCC`, `EXEC`, and `SCC` flags |
| **Basic ALU & Memory Loads** | ✅ Done | Scalar/vector float & int math, constant buffer & buffer loads |
| **Texture Sampling (`MIMG`)** | 🟡 Partial | `image_load` & `image_sample*` integrated with `T#` / `S#` descriptors |
| **Wave64 Support** | 🟡 Partial | 64-bit `EXEC`/`VCC` mask handling & `[WaveSize(64)]` (Shader Model 6.6) |
| **Control Flow Graph (CFG)** | 🔄 In Progress | Basic branching (`s_branch`, `s_cbranch_*`); migrating from `switch` to structured `if/else` & `while` |
| **Local Data Share (LDS / `groupshared`)** | ⏳ Planned | `ds_read_*`, `ds_write_*`, and `s_barrier` translation |
| **System Value Semantics (`SV_*`)** | ⏳ Planned | Stage-specific mapping (`SV_VertexID`, `SV_Position`, `SV_DispatchThreadID`, etc.) |
| **DXC Integration (`HLSL ➔ DXIL`)** | ⏳ Planned | Runtime compilation & signing via `dxcompiler.dll` (`IDxcCompiler3`) |

---

### 2. Graphics & Runtime Shim (`D3D12.X ➔ D3D12`)

| Subsystem | Status | Implementation Notes |
| :--- | :---: | :--- |
| **Root Signature ABI** | ✅ Done | Extended `D3D12XboxCreateRootSignatureEx` with `T#` / `S#` sampler bindings |
| **PSO Creation Shim** | 🟡 Partial | Stage & resource binding passthrough (`d3d12_xs_shim.cpp`), `NOBC` handling |
| **Synthetic Test Suite** | ✅ Done | Unit tests for ISA decoding, exports, and texture loads (`test_shader_pipeline.cpp`) |
| **First Build & Compilation** | ⏳ Pending | Core lifter architecture is being finalized prior to first full CMake build |

---

## 🛠️ Tech Stack & Dependencies

* **Language:** C++20
* **Build System:** CMake
* **Graphics Backend:** Direct3D 12 (Shader Model 6.6)
* **Compression:** Zlib (for `FSSM` shader stream decompression)
* **Shader Compiler:** DirectX Shader Compiler (`dxcompiler.dll` / `dxil.dll`)

---

## ⚠️ Disclaimer

> **ViceWin** is an independent educational and research project focused on graphics architecture, binary translation, and Direct3D 12 interoperability. It does **not** include any copyrighted game files, encryption keys, or proprietary console SDKs.
