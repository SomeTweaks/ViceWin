# ViceWin DevBench — GTA VI-class shader and physics report

- mode: full
- physics device: WARP
- shader variants per base: 24

## Provenance and verification

Parts of this bench were written by delegated child agents and are listed here with the evidence that proves them, so nothing is presented as done on the strength of a claim:

- `gta6_shaders.cpp` (10-shader GTA VI corpus) — child agent. Verified: the corpus test compiles and validates all 10 sources through DXC. Two real HLSL defects surfaced at integration and were fixed: `RayDesc` has no `Flags` member, and an amplification payload must be declared `in payload` with `DispatchMesh` dominating every exit.
- `compute_harness.cpp` (D3D12 compute runner) — child agent. Verified: the physics dispatch runs on WARP and matches the CPU reference bit-exactly. One compile defect fixed (`D3D12_COMMAND_LIST_TYPE_COMPUTE`).
- `physics_reference.cpp` (CPU reference integrator) — child agent. Verified: mirrors the kernel operation-for-operation; measured residual 0.000000 over 262144x720 particle-steps.
- `physics_kernel.cpp`, `main.cpp`, `tests.cpp`, CMake wiring — written here; the kernel is the contract both sides are checked against.
- Not covered by this bench: retail package decryption, GameOS syscall numbers, RDNA2 ISA gaps and Oodle/Kraken. The bench proves the PC-side shader/physics stack, not that the retail title boots.

| test | result | time | summary |
|---|---|---|---|
| stress | PASS | 29155 ms | full GTA VI soak passed: heavy shaders, 96+ variants, 262144x720 physics, cache and fuzz |

## stress

- [corpus] 10/10 GTA VI-class shaders compiled in 406 ms (worst ssao_gtao 163 ms)
-   PASS deferred_lighting [ps_6_6, 16-bit] 25 ms, 9836 bytes
-   PASS volumetric_fog [ps_6_6] 20 ms, 7984 bytes
-   PASS ssao_gtao [ps_6_6] 163 ms, 47248 bytes
-   PASS taa_resolve [ps_6_6] 22 ms, 8280 bytes
-   PASS cluster_cull_cs [cs_6_6] 16 ms, 7120 bytes
-   PASS character_skinning_cs [cs_6_6, 16-bit] 19 ms, 7768 bytes
-   PASS terrain_mesh [ms_6_6, 16-bit] 15 ms, 5868 bytes
-   PASS gpu_culling_amp [as_6_6] 15 ms, 6032 bytes
-   PASS raytraced_gi_cs [cs_6_6] 58 ms, 26932 bytes
-   PASS hair_strands_cs [cs_6_6, 16-bit] 53 ms, 26544 bytes
- [soak] 384/384 variants compiled in 27600 ms (avg 72 ms)
-   deferred_lighting: 96 variants, avg 23 ms
-   volumetric_fog: 96 variants, avg 27 ms
-   ssao_gtao: 96 variants, avg 205 ms
-   taa_resolve: 96 variants, avg 33 ms
- [physics] GPU matches the CPU reference, max diff 0.000000 over 262144x720 particle-steps
-   kernel compile: 21 ms, 4376 bytes
-   particles=262144 steps=720 dt=0.004167
-   adapter: WARP
-   wall: 244 ms, gpu fence: 194 ms
-   max |gpu - cpu| = 0.000000 (tolerance 0.050)
-   first particle: gpu=(-10.1207, 5.3733, -12.5040) cpu=(-10.1207, 5.3733, -12.5040)
- [cache] 3/3 containers round-tripped through the disk cache
-   PASS deferred_lighting: store 0.79 ms, hit 0.18 ms (hit re-validates the cached DXIL)
-   PASS volumetric_fog: store 0.54 ms, hit 0.17 ms (hit re-validates the cached DXIL)
-   PASS ssao_gtao: store 0.81 ms, hit 0.37 ms (hit re-validates the cached DXIL)
- [validator] 465 garbage blobs rejected, 47 raw XS2 markers classified, real container accepted
-   random containers rejected: 465/512
-   raw XS2 payloads classified and refused as DXIL: 47
-   real DXIL container accepted

