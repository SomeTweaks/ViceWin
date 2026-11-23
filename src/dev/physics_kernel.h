#pragma once

#include <cstdint>

namespace vicewindev {

// The GPU physics kernel every physics test runs. It is the single source of
// truth for the simulation semantics; physics_reference.cpp mirrors it on the
// CPU operation-for-operation so the comparison measures FMA contraction and
// nothing else.
//
// Buffer contract (RWStructuredBuffer<float4>, register u0):
//   two float4 per particle, stride 32 bytes:
//     [0] = position.xyz, mass
//     [1] = velocity.xyz, unused
// Root constants (b0): { count, steps, dt bits, pad }.
const char* PhysicsKernelHlsl();

// [numthreads(x,1,1)] of the kernel; dispatch group count is
// ceil(count / thread_group_x).
uint32_t PhysicsKernelThreadGroupX();

}  // namespace vicewindev
