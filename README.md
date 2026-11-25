


<div align="center">
<h1> ViceWin🌴 </h1>
</div>
<div align="center">

**Xbox Series X|S HLE compatibility layer and RDNA2 to DXIL shader recompiler for Windows**


JOIN OUR TEAM'S DISCORD CHANNEL https://discord.gg/VDYNTQKC8


![Windows](https://img.shields.io/badge/Platform-Windows_11_x64-0078D4?logo=windows&logoColor=white)
![C++20](https://img.shields.io/badge/Language-C++20-00599C?logo=cplusplus&logoColor=white)
![CMake](https://img.shields.io/badge/Build-CMake_3.20+-064F8C?logo=cmake&logoColor=white)
![D3D12](https://img.shields.io/badge/Graphics-DirectX_12_SM_6.6-5C2D91)
![GTA VI](https://img.shields.io/badge/GTA_VI-Legacy_of_the_South-FFB000?style=for-the-badge)
![Xbox](https://img.shields.io/badge/Xbox-Series_X%7CS-107C10?style=for-the-badge&logo=xbox&logoColor=white)

</div>

ViceWin is a native Windows compatibility layer for Xbox Series X|S builds. It focuses on
translation and high-level emulation instead of full-system hardware emulation.

The runtime maps Scarlett PE64 executables into host memory, intercepts Xbox-specific calls,
and translates console APIs and shaders into native Win32, DirectX 12, and Shader Model 6.6
interfaces.

> [!NOTE]
> ViceWin is developed and tuned primarily on AMD RDNA 3.5 hardware. Other DirectX 12
> Shader Model 6.6 GPUs may work, but performance and behavior can vary.

---

## Bring-up and telemetry

<div align="center">

![ViceWin telemetry status](status.png)

<p align="center">
  <img src="Scr.png" width="32%" />
  <img src="Scr1.png" width="32%" />
  <img src="Sc2r.png" width="32%" />
</p>



## Highlights

- **Native PE64 loading** — maps Scarlett executables with TLS and `.pdata` support
- **High-level emulation** — Win32, audio, input, memory, and D3D12 compatibility layers
- **Shader translation** — RDNA2 ISA to SSA to HLSL/DXIL Shader Model 6.6
- **Runtime tracing** — automatic logging, crash reporting, and IAT traces
- **DXC integration** — DirectX Shader Compiler with DXIL caching
- **Lightweight architecture** — no heavy hardware emulation or proprietary game data

---

## How it works

```mermaid
flowchart LR
    subgraph Package["Xbox GDK package"]
        A1["MicrosoftGame.Config"]
        A2["game_scarlett.exe"]
        A3["XDAT / XBCH / FSSM"]
    end

    subgraph ViceWin["ViceWin host"]
        B1["PE64 mapper"]
        B2["HLE runtime + VEH"]
        B3["RDNA2 → DXIL"]
    end

    subgraph Windows["Windows 11"]
        C1["Win32 / XAudio2 / XInput"]
        C2["Memory compatibility"]
        C3["IDxcCompiler3 + DXIL cache"]
    end

    Package ==> ViceWin ==> Windows
```

---

## Requirements

| Component | Requirement |
| --- | --- |
| Operating system | Windows 11 x64 |
| Compiler | Microsoft Visual C++ 2022 |
| Language standard | C++20 |
| Build system | CMake 3.20 or newer |
| Graphics | DirectX 12 GPU with Shader Model 6.6 support |
| Windows SDK | `dxcompiler.dll` and `dxil.dll` |

---



## What we have now

| Component | Now |
| --- | --- |
| Grand Theft Auto VI | 1 frame then crashes |
| nothing else rn | ¬ |


---

## Build

```powershell
cmake -S . -B build
cmake --build build --config Release
```

## Run tests

```powershell
ctest --test-dir build --output-on-failure
```

---

## Launch a package

1. Open `build\Release\vicewin.exe`, or run:

   ```powershell
   .\scarlettx_host.exe --in-process <path-to-exe>
   ```

2. Select an extracted package containing `MicrosoftGame.Config` and the `.exe` file.
3. Find runtime logs and crash/IAT traces in `vicewin.log`.

> [!WARNING]
> Use only game files, packages, binaries, and decryption material that you are legally
> authorized to access and run.

---

## Credits

The initial HLE runtime scaffolding, PE64 loader foundation, and Win32 GDK shim architecture
were adapted from [Freminet/ScarlettX](https://github.com/Freminet/ScarlettX). Many thanks to
Freminet for providing a clean-room Xbox Series X|S foundation.

---

## Legal

ViceWin is an independent, non-commercial research project focused on GPU architecture and
binary/shader translation.

This repository does not contain copyrighted game files, decryption keys, or proprietary
Microsoft/Xbox SDK binaries.
