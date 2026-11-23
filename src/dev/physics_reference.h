#pragma once

#include <cstdint>
#include <vector>

namespace vicewindev {

struct PhysicsParams {
    uint32_t particle_count = 0;
    uint32_t steps = 0;
    float dt = 1.0f / 240.0f;
};

// Deterministic initial state: 2 float4 per particle (32 bytes):
// [0]=px,[1]=py,[2]=pz,[3]=mass(1.0); [4]=vx,[5]=vy,[6]=vz,[7]=0.
// Values come from a fixed LCG (seed=0x9E3779B9, state=state*1664525+1013904223),
// positions in [-12,12]x[1,18]x[-12,12], velocities in [-6,6] on each axis.
void BuildInitialState(uint32_t particle_count, std::vector<float>& out_state);

// Same integrator as the HLSL kernel: semi-implicit Euler with the very same
// bounds-check order, so a CPU/GPU diff isolates FMA contraction only.
// The w components (indices 3 and 7 of every particle) are copied through
// untouched; out_state always ends up the same size as initial_state.
void RunCpuReference(const PhysicsParams& params,
                     const std::vector<float>& initial_state,
                     std::vector<float>& out_state);

// Maximum |a-b| across all elements; returns 1e30f when the sizes differ.
float MaxAbsDifference(const std::vector<float>& a, const std::vector<float>& b);

}  // namespace vicewindev
