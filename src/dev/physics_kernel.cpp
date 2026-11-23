#include "physics_kernel.h"

namespace vicewindev {

namespace {

// Deterministic, branch-free-per-particle integrator. The exact operation
// order matters: physics_reference.cpp mirrors it and the GPU/CPU comparison
// tolerates only FMA-contraction differences.
constexpr const char* kPhysicsHlsl = R"hlsl(
RWStructuredBuffer<float4> g_state : register(u0);

cbuffer PhysicsConstants : register(b0)
{
    uint g_count;
    uint g_steps;
    float g_dt;
    float g_pad;
};

static const float3 kGravity = float3(0.0f, -9.81f, 0.0f);
static const float3 kBoundsMin = float3(-16.0f, 0.0f, -16.0f);
static const float3 kBoundsMax = float3(16.0f, 24.0f, 16.0f);

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    const uint index = tid.x;
    if (index >= g_count)
    {
        return;
    }

    float4 p = g_state[index * 2 + 0];
    float4 v = g_state[index * 2 + 1];
    float3 position = p.xyz;
    float3 velocity = v.xyz;

    for (uint step = 0; step < g_steps; ++step)
    {
        velocity = velocity + kGravity * g_dt;
        position = position + velocity * g_dt;

        // Same order as the CPU reference: ground first (with friction),
        // then the four walls, then the ceiling.
        if (position.y < kBoundsMin.y)
        {
            position.y = kBoundsMin.y;
            velocity.y = -velocity.y * 0.6f;
            velocity.x *= 0.98f;
            velocity.z *= 0.98f;
        }
        if (position.x < kBoundsMin.x)
        {
            position.x = kBoundsMin.x;
            velocity.x = -velocity.x * 0.72f;
        }
        if (position.x > kBoundsMax.x)
        {
            position.x = kBoundsMax.x;
            velocity.x = -velocity.x * 0.72f;
        }
        if (position.z < kBoundsMin.z)
        {
            position.z = kBoundsMin.z;
            velocity.z = -velocity.z * 0.72f;
        }
        if (position.z > kBoundsMax.z)
        {
            position.z = kBoundsMax.z;
            velocity.z = -velocity.z * 0.72f;
        }
        if (position.y > kBoundsMax.y)
        {
            position.y = kBoundsMax.y;
            velocity.y = -velocity.y * 0.72f;
        }
    }

    g_state[index * 2 + 0] = float4(position, p.w);
    g_state[index * 2 + 1] = float4(velocity, v.w);
}
)hlsl";

}  // namespace

const char* PhysicsKernelHlsl() {
    return kPhysicsHlsl;
}

uint32_t PhysicsKernelThreadGroupX() {
    return 64;
}

}  // namespace vicewindev
