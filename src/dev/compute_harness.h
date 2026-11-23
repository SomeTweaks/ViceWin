#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "scarlettx/core/status.h"

namespace vicewindev {

// Status lives in scarlettx; alias it so the signatures below keep the plain
// name this dev tool's call sites use.
using scarlettx::Status;

struct ComputeRunResult {
    std::vector<float> output;   // Same element count as the input.
    uint64_t dispatch_ns = 0;    // Wall-clock GPU pass time measured around the fence wait; 0 if unmeasured.
    std::string adapter;         // "WARP" or the adapter description.
};

struct ComputeRunDesc {
    uint32_t particle_count = 0;   // = number of elements in the StructuredBuffer
    uint32_t steps = 0;            // root constants b0.y
    float dt = 1.0f / 240.0f;      // root constants b0.z
    bool prefer_hardware = false;  // true: try a discrete adapter first, WARP as fallback
    uint32_t thread_group_x = 64;  // [numthreads(x,1,1)] of the kernel
};

// Compiles the PSO from dxil (D3D12_SHADER_BYTECODE), creates the buffers,
// dispatches ceil(particle_count/thread_group_x,1,1) groups once (the kernel
// runs its own `steps` loop), and copies the result back. Root signature:
//   [0] root constants b0 (4 x uint32: count, steps, dt_bits, pad)
//   [1] root UAV u0 (StructuredBuffer, stride 16 bytes)
Status RunComputeDxil(const std::vector<uint8_t>& dxil,
                      const std::vector<float>& input,
                      const ComputeRunDesc& desc,
                      ComputeRunResult& out_result);

}  // namespace vicewindev
