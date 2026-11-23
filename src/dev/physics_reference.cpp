#include "physics_reference.h"

#include <cmath>

namespace vicewindev {

namespace {

// Mirror of the HLSL constant block, spelled as scalars so that every
// component operation below can be written in the exact source order.
constexpr float kGravityX = 0.0f;
constexpr float kGravityY = -9.81f;
constexpr float kGravityZ = 0.0f;

constexpr float kBoundsMinX = -16.0f;
constexpr float kBoundsMinY = 0.0f;
constexpr float kBoundsMinZ = -16.0f;

constexpr float kBoundsMaxX = 16.0f;
constexpr float kBoundsMaxY = 24.0f;
constexpr float kBoundsMaxZ = 16.0f;

// Restitution factors, straight from the kernel.
constexpr float kGroundBounce = 0.6f;   // Y-min: vertical bounce
constexpr float kGroundFriction = 0.98f;  // Y-min: tangential damping on X and Z
constexpr float kWallBounce = 0.72f;      // every other plane

// Initial-state layout and ranges (2 float4 per particle).
constexpr size_t kFloatsPerParticle = 8u;
constexpr float kPosMinX = -12.0f;
constexpr float kPosMaxX = 12.0f;
constexpr float kPosMinY = 1.0f;
constexpr float kPosMaxY = 18.0f;
constexpr float kPosMinZ = -12.0f;
constexpr float kPosMaxZ = 12.0f;
constexpr float kVelMin = -6.0f;
constexpr float kVelMax = 6.0f;

constexpr uint32_t kLcgSeed = 0x9E3779B9u;
constexpr uint32_t kLcgMultiplier = 1664525u;
constexpr uint32_t kLcgIncrement = 1013904223u;

// lo + (hi - lo) * r: written out instead of std::lerp so the arithmetic is
// fixed and identical on every build of this tool.
inline float Lerp(float lo, float hi, float r) {
    return lo + (hi - lo) * r;
}

// Fixed LCG shared by every run, so two runs of the tool produce bit-identical
// input buffers and any diff against the GPU kernel comes from the kernel only.
struct Lcg {
    uint32_t state;

    explicit Lcg(uint32_t seed) : state(seed) {}

    // Advances the generator, then maps the top 24 bits to [0, 1).
    float Next() {
        state = state * kLcgMultiplier + kLcgIncrement;
        return static_cast<float>(state >> 8) / static_cast<float>(1u << 24);
    }
};

}  // namespace

void BuildInitialState(uint32_t particle_count, std::vector<float>& out_state) {
    out_state.assign(static_cast<size_t>(particle_count) * kFloatsPerParticle, 0.0f);

    Lcg rng(kLcgSeed);

    for (uint32_t i = 0; i < particle_count; ++i) {
        const size_t base = static_cast<size_t>(i) * kFloatsPerParticle;

        // Draw order is part of the contract: px, py, pz, vx, vy, vz.
        const float px = Lerp(kPosMinX, kPosMaxX, rng.Next());
        const float py = Lerp(kPosMinY, kPosMaxY, rng.Next());
        const float pz = Lerp(kPosMinZ, kPosMaxZ, rng.Next());
        const float vx = Lerp(kVelMin, kVelMax, rng.Next());
        const float vy = Lerp(kVelMin, kVelMax, rng.Next());
        const float vz = Lerp(kVelMin, kVelMax, rng.Next());

        out_state[base + 0] = px;
        out_state[base + 1] = py;
        out_state[base + 2] = pz;
        out_state[base + 3] = 1.0f;  // mass: carried through, never integrated
        out_state[base + 4] = vx;
        out_state[base + 5] = vy;
        out_state[base + 6] = vz;
        out_state[base + 7] = 0.0f;  // padding: carried through
    }
}

void RunCpuReference(const PhysicsParams& params,
                     const std::vector<float>& initial_state,
                     std::vector<float>& out_state) {
    // Copy first: particles outside the processed range, the w components and a
    // trailing partial particle all pass through unchanged, and out_state keeps
    // exactly the size of initial_state.
    out_state = initial_state;

    const size_t available = initial_state.size() / kFloatsPerParticle;
    size_t count = static_cast<size_t>(params.particle_count);
    if (count > available) {
        // The state buffer is shorter than the parameter claims: never read past
        // the end, silently process the particles that actually have storage.
        count = available;
    }

    const float dt = params.dt;

    for (size_t i = 0; i < count; ++i) {
        const size_t base = i * kFloatsPerParticle;

        float position_x = initial_state[base + 0];
        float position_y = initial_state[base + 1];
        float position_z = initial_state[base + 2];
        float velocity_x = initial_state[base + 4];
        float velocity_y = initial_state[base + 5];
        float velocity_z = initial_state[base + 6];

        for (uint32_t s = 0; s < params.steps; ++s) {
            // Semi-implicit Euler: velocity first, then position, in the same
            // order and with the same operands as velocity = velocity + kGravity
            // * dt; position = position + velocity * dt;. Keeping the per-axis
            // form (including the x and z gravity terms, which are exactly zero)
            // means no reassociation can creep in on either side of the compare.
            velocity_x = velocity_x + kGravityX * dt;
            velocity_y = velocity_y + kGravityY * dt;
            velocity_z = velocity_z + kGravityZ * dt;

            position_x = position_x + velocity_x * dt;
            position_y = position_y + velocity_y * dt;
            position_z = position_z + velocity_z * dt;

            // Bounds order is load-bearing and must stay exactly as in the
            // kernel: Y-min (with X/Z friction), X-min, X-max, Z-min, Z-max,
            // Y-max. A different order changes how many clamps and reflections a
            // corner hit applies in one step, so the CPU and GPU would diverge
            // by more than FMA rounding - which would make this reference useless
            // as a cross-check.
            if (position_y < kBoundsMinY) {
                position_y = kBoundsMinY;
                velocity_y = -velocity_y * kGroundBounce;
                velocity_x = velocity_x * kGroundFriction;
                velocity_z = velocity_z * kGroundFriction;
            }
            if (position_x < kBoundsMinX) {
                position_x = kBoundsMinX;
                velocity_x = -velocity_x * kWallBounce;
            }
            if (position_x > kBoundsMaxX) {
                position_x = kBoundsMaxX;
                velocity_x = -velocity_x * kWallBounce;
            }
            if (position_z < kBoundsMinZ) {
                position_z = kBoundsMinZ;
                velocity_z = -velocity_z * kWallBounce;
            }
            if (position_z > kBoundsMaxZ) {
                position_z = kBoundsMaxZ;
                velocity_z = -velocity_z * kWallBounce;
            }
            if (position_y > kBoundsMaxY) {
                position_y = kBoundsMaxY;
                velocity_y = -velocity_y * kWallBounce;
            }
        }

        // Only xyz are written back; indices 3 and 7 keep the values that the
        // copy at the top of the function already put there.
        out_state[base + 0] = position_x;
        out_state[base + 1] = position_y;
        out_state[base + 2] = position_z;
        out_state[base + 4] = velocity_x;
        out_state[base + 5] = velocity_y;
        out_state[base + 6] = velocity_z;
    }
}

float MaxAbsDifference(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size()) {
        return 1e30f;
    }

    float max_diff = 0.0f;
    for (size_t i = 0; i < a.size(); ++i) {
        const float diff = std::fabs(a[i] - b[i]);
        // A NaN on either side compares false and leaves max_diff unchanged;
        // callers that care about NaN should check for it separately.
        if (diff > max_diff) {
            max_diff = diff;
        }
    }
    return max_diff;
}

}  // namespace vicewindev
