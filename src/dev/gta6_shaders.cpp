#include "gta6_shaders.h"

#include <cstdint>
#include <string>
#include <vector>

namespace vicewindev {

namespace {

// ---------------------------------------------------------------------------
// 1) Deferred lighting: 4 G-buffer loads, 64 lights, Cook-Torrance BRDF.
// ---------------------------------------------------------------------------
constexpr const char kShaderDeferredLighting[] = R"hlsl(struct PSIn {
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD0;
};

struct GpuLight {
    float4 position_radius;   // xyz world position, w influence radius
    float4 color_intensity;   // rgb colour, w intensity
    float4 direction_type;    // xyz spot direction, w 0 = point, 1 = spot
    float4 cone_params;       // x cos outer, y cos inner, z specular boost, w falloff
};

Texture2D<float4> g_albedo : register(t0, space0);
Texture2D<float4> g_normal : register(t1, space0);
Texture2D<float4> g_material : register(t2, space0);
Texture2D<float> g_depth : register(t3, space0);
Texture2D<float> g_shadow_atlas : register(t4, space0);
StructuredBuffer<GpuLight> g_lights : register(t5, space0);
SamplerState g_linear_sampler : register(s0, space0);
SamplerComparisonState g_shadow_sampler : register(s1, space0);

cbuffer FrameConstants : register(b0, space0) {
    float4x4 g_inv_view_projection;
    float4x4 g_light_view_projection;
    float4 g_camera_position;
    float4 g_screen_size;       // xy pixels, zw inverse pixels
    float4 g_shadow_texel;      // xy atlas texel size
    float4 g_lighting_params;   // x ambient, y shadow strength, z exposure, w frame
    uint g_light_count;
    float g_shadow_bias;
    float g_specular_boost;
    uint g_frame_index;
};

static const float2 kPoissonDisk[8] = {
    float2(-0.613392, 0.617481),
    float2(0.170019, -0.040254),
    float2(-0.299417, 0.791925),
    float2(0.645680, 0.493210),
    float2(-0.651784, 0.717887),
    float2(0.421003, 0.027070),
    float2(-0.817194, -0.271096),
    float2(-0.705374, -0.668203)
};

float DistributionGgx(float n_dot_h, float alpha) {
    float alpha_sq = alpha * alpha;
    float denominator = n_dot_h * n_dot_h * (alpha_sq - 1.0) + 1.0;
    return alpha_sq / max(3.14159265 * denominator * denominator, 1e-6);
}

float VisibilitySmithGgxCorrelated(float n_dot_v, float n_dot_l, float alpha) {
    float alpha_sq = alpha * alpha;
    float lambda_v = n_dot_l * sqrt(n_dot_v * n_dot_v * (1.0 - alpha_sq) + alpha_sq);
    float lambda_l = n_dot_v * sqrt(n_dot_l * n_dot_l * (1.0 - alpha_sq) + alpha_sq);
    return 0.5 / max(lambda_v + lambda_l, 1e-5);
}

float3 FresnelSchlick(float v_dot_h, float3 f0) {
    float fresnel = pow(1.0 - saturate(v_dot_h), 5.0);
    return f0 + (1.0 - f0) * fresnel;
}

float SampleShadow(float3 world_position, float3 normal) {
    float4 light_clip = mul(float4(world_position + normal * 0.035, 1.0), g_light_view_projection);
    float3 projected = light_clip.xyz / max(light_clip.w, 1e-5);
    float2 shadow_uv = projected.xy * float2(0.5, -0.5) + 0.5;
    if (shadow_uv.x < 0.0 || shadow_uv.x > 1.0 || shadow_uv.y < 0.0 || shadow_uv.y > 1.0) {
        return 1.0;
    }
    float compare = projected.z - g_shadow_bias;
    float visibility = 0.0;
    [unroll(8)]
    for (uint tap = 0; tap < 8; ++tap) {
        visibility += g_shadow_atlas.SampleCmpLevelZero(
            g_shadow_sampler, shadow_uv + kPoissonDisk[tap] * g_shadow_texel.xy, compare);
    }
    return visibility * 0.125;
}

float4 main(PSIn input) : SV_TARGET {
    int2 pixel = (int2)input.pos.xy;
    float4 albedo_sample = g_albedo.Load(int3(pixel, 0));
    float4 normal_sample = g_normal.Load(int3(pixel, 0));
    float4 material_sample = g_material.Load(int3(pixel, 0));
    float4 depth_sample = g_depth.Load(int3(pixel, 0));

    float2 ndc = input.uv * 2.0 - 1.0;
    float4 world_h = mul(float4(ndc, depth_sample.x, 1.0), g_inv_view_projection);
    float3 world_position = world_h.xyz / max(world_h.w, 1e-6);

    float3 normal = normalize(normal_sample.xyz * 2.0 - 1.0);
    float3 view_dir = normalize(g_camera_position.xyz - world_position);
    float roughness = clamp(material_sample.x, 0.045, 1.0);
    float metallic = saturate(material_sample.y);
    float occlusion = saturate(material_sample.z);
    float3 base_color = albedo_sample.rgb;
    float3 diffuse_color = base_color * (1.0 - metallic);
    float3 f0 = lerp(float3(0.04, 0.04, 0.04), base_color, metallic);
    float alpha = roughness * roughness;
    float n_dot_v = saturate(dot(normal, view_dir)) + 1e-4;
    float ambient_scale = g_lighting_params.x * g_lighting_params.z;

    half3 radiance_accum = half3(0.0h, 0.0h, 0.0h);

    [loop]
    for (uint light_index = 0; light_index < 64u; ++light_index) {
        if (light_index >= g_light_count) {
            break;
        }

        GpuLight light = g_lights[light_index];
        float3 to_light = light.position_radius.xyz - world_position;
        float distance_sq = max(dot(to_light, to_light), 1e-4);
        float radius = max(light.position_radius.w, 1e-3);
        if (distance_sq > radius * radius) {
            continue;
        }

        float distance = sqrt(distance_sq);
        float3 light_dir = to_light / distance;
        float window = saturate(1.0 - pow(distance / radius, 4.0));
        float attenuation = window * window / (distance_sq + 1.0);

        if (light.direction_type.w > 0.5) {
            float cos_angle = dot(-light_dir, normalize(light.direction_type.xyz));
            attenuation *= smoothstep(light.cone_params.x, light.cone_params.y, cos_angle);
        }

        float n_dot_l = saturate(dot(normal, light_dir));
        if (n_dot_l <= 0.0 || attenuation <= 0.0) {
            continue;
        }

        float3 half_vector = normalize(light_dir + view_dir);
        float n_dot_h = saturate(dot(normal, half_vector));
        float v_dot_h = saturate(dot(view_dir, half_vector));

        float distribution = DistributionGgx(n_dot_h, alpha);
        float visibility = VisibilitySmithGgxCorrelated(n_dot_v, n_dot_l, alpha);
        float3 fresnel = FresnelSchlick(v_dot_h, f0);
        float3 specular = distribution * visibility * fresnel * light.cone_params.z;
        float3 diffuse = diffuse_color * (1.0 - fresnel) * 0.31830988;

        float shadow = SampleShadow(world_position, normal);
        float3 radiance = light.color_intensity.rgb * light.color_intensity.w;
        float3 contribution = (diffuse + specular * g_specular_boost) *
            radiance * (attenuation * n_dot_l * shadow * occlusion);

        radiance_accum = radiance_accum + half3(contribution);
    }

    half3 ambient = half3(diffuse_color * (0.035 * occlusion)) * half3(ambient_scale, ambient_scale, ambient_scale);
    half3 color = radiance_accum + ambient;
    half3 mapped = color / (color + 1.0h);
    mapped = pow(max(mapped, 0.0h), 1.0h / 2.2h);

    return float4(float3(mapped), 1.0);
}
)hlsl";

// ---------------------------------------------------------------------------
// 2) Volumetric fog: 96 step raymarch through a 3D density volume.
// ---------------------------------------------------------------------------
constexpr const char kShaderVolumetricFog[] = R"hlsl(struct PSIn {
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD0;
};

Texture3D<float4> g_fog_volume : register(t0, space0);   // rgb tint, a density
Texture2D<float> g_scene_depth : register(t1, space0);
SamplerState g_volume_sampler : register(s0, space0);
SamplerState g_depth_sampler : register(s1, space0);

cbuffer FogConstants : register(b0, space0) {
    float4x4 g_inv_view_projection;
    float4 g_camera_position;
    float4 g_fog_params;          // x density scale, y absorption, z scattering, w max distance
    float4 g_anisotropy;          // x forward g, y back g, z ambient scale, w time
    float4 g_light_direction[4];  // xyz direction to light, w intensity
    float4 g_light_color[4];
    float4 g_volume_transform;    // xyz world minimum, w inverse extent
    float4 g_wind;                // xyz wind direction, w speed
    float4 g_ambient;
};

float HenyeyGreenstein(float cos_theta, float g) {
    float g_sq = g * g;
    float denominator = max(1.0 + g_sq - 2.0 * g * cos_theta, 1e-4);
    return (1.0 - g_sq) / (12.5663706 * pow(denominator, 1.5));
}

float InterleavedGradientNoise(float2 pixel) {
    return frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
}

float4 main(PSIn input) : SV_TARGET {
    int2 pixel = (int2)input.pos.xy;
    float scene_depth = g_scene_depth.Load(int3(pixel, 0));
    float2 ndc = input.uv * 2.0 - 1.0;

    float4 near_h = mul(float4(ndc, 0.0, 1.0), g_inv_view_projection);
    float4 far_h = mul(float4(ndc, scene_depth, 1.0), g_inv_view_projection);
    float3 near_point = near_h.xyz / max(near_h.w, 1e-6);
    float3 far_point = far_h.xyz / max(far_h.w, 1e-6);

    float3 ray_vector = far_point - near_point;
    float ray_length = min(length(ray_vector), g_fog_params.w);
    float3 ray_dir = ray_vector / max(length(ray_vector), 1e-5);

    const uint kStepCount = 96u;
    float step_length = ray_length / (float)kStepCount;
    float jitter = InterleavedGradientNoise(input.pos.xy);
    float3 wind_offset = g_wind.xyz * (g_wind.w * g_anisotropy.w);

    float3 accumulated = 0.0;
    float transmittance = 1.0;

    [loop]
    for (uint step_index = 0; step_index < kStepCount; ++step_index) {
        float distance_along_ray = ((float)step_index + jitter) * step_length;
        float3 sample_position = mad(ray_dir, distance_along_ray, near_point);
        float3 volume_uvw = (sample_position - g_volume_transform.xyz) * g_volume_transform.w;
        float4 volume = g_fog_volume.SampleLevel(g_volume_sampler, saturate(volume_uvw + wind_offset), 0.0);

        float density = max(volume.a, 0.0) * g_fog_params.x;
        float extinction = density * g_fog_params.z + g_fog_params.y;
        if (extinction > 1e-5) {
            float3 in_scatter = g_ambient.rgb * (density * g_anisotropy.z) + volume.rgb * density;

            [unroll(4)]
            for (uint light_index = 0; light_index < 4; ++light_index) {
                float3 light_dir = normalize(g_light_direction[light_index].xyz);
                float cos_theta = dot(ray_dir, light_dir);
                float phase = HenyeyGreenstein(cos_theta, g_anisotropy.x) * 0.65 +
                    HenyeyGreenstein(cos_theta, g_anisotropy.y) * 0.35;
                in_scatter += g_light_color[light_index].rgb *
                    (g_light_direction[light_index].w * phase * density);
            }

            float step_transmittance = exp(-extinction * step_length);
            accumulated += transmittance * in_scatter * (1.0 - step_transmittance) / max(extinction, 1e-5);
            transmittance *= step_transmittance;

            if (transmittance < 0.002) {
                break;
            }
        }
    }

    float3 fog_color = g_ambient.rgb * (1.0 - transmittance) + accumulated;
    return float4(fog_color, saturate(1.0 - transmittance));
}
)hlsl";

// ---------------------------------------------------------------------------
// 3) SSAO / GTAO: 64 Kimura spiral samples, depth reconstruction, 5x5 resolve.
// ---------------------------------------------------------------------------
constexpr const char kShaderSsaoGtao[] = R"hlsl(struct PSIn {
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD0;
};

Texture2D<float> g_depth : register(t0, space0);
Texture2D<float4> g_view_normal : register(t1, space0);   // view space normal, xyz * 0.5 + 0.5
SamplerState g_point_sampler : register(s0, space0);
SamplerState g_linear_sampler : register(s1, space0);

cbuffer SsaoConstants : register(b0, space0) {
    float4x4 g_projection;
    float4x4 g_inv_projection;
    float4 g_screen_size;      // xy pixels, zw inverse pixels
    float4 g_ao_params;        // x radius, y bias, z intensity, w power
    float4 g_ao_temporal;      // x history blend, y frame, z rotation, w normal power
};

float2 KimuraSpiral(uint index, uint sample_count, float rotation) {
    float radial = sqrt(((float)index + 0.5) / (float)sample_count);
    float angle = mad((float)index, 2.39996323, rotation);
    return float2(cos(angle), sin(angle)) * radial;
}

float3 ReconstructViewPosition(float2 uv, float depth) {
    float2 ndc = uv * 2.0 - 1.0;
    float4 view_h = mul(float4(ndc, depth, 1.0), g_inv_projection);
    return view_h.xyz / max(view_h.w, 1e-6);
}

float InterleavedGradientNoise(float2 pixel) {
    return frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
}

float ComputeGtao(float2 uv, uint sample_count, float rotation) {
    float depth = g_depth.SampleLevel(g_point_sampler, uv, 0.0);
    if (depth <= 0.0) {
        return 1.0;
    }

    float3 view_position = ReconstructViewPosition(uv, depth);
    float3 view_normal = normalize(g_view_normal.SampleLevel(g_point_sampler, uv, 0.0).xyz * 2.0 - 1.0);
    float radius = g_ao_params.x;
    float bias = g_ao_params.y;

    float3 helper = abs(view_normal.z) < 0.9 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
    float3 tangent = normalize(cross(helper, view_normal));
    float3 bitangent = cross(view_normal, tangent);

    float occlusion = 0.0;

    [loop]
    for (uint sample_index = 0; sample_index < sample_count; ++sample_index) {
        float2 disk = KimuraSpiral(sample_index, sample_count, rotation);
        float3 sample_view = view_position +
            (tangent * disk.x + bitangent * disk.y) * radius +
            view_normal * (sqrt(max(1.0 - dot(disk, disk), 0.0)) * radius * 0.6);

        float4 clip = mul(float4(sample_view, 1.0), g_projection);
        float2 sample_ndc = clip.xy / max(clip.w, 1e-5);
        float2 sample_uv = sample_ndc * 0.5 + 0.5;
        if (sample_uv.x < 0.0 || sample_uv.x > 1.0 || sample_uv.y < 0.0 || sample_uv.y > 1.0) {
            continue;
        }

        float sample_depth = g_depth.SampleLevel(g_point_sampler, sample_uv, 0.0);
        float3 neighbour = ReconstructViewPosition(sample_uv, sample_depth);
        float3 delta = neighbour - view_position;
        float distance_to_sample = length(delta);
        if (distance_to_sample < 1e-4) {
            continue;
        }

        float3 delta_dir = delta / distance_to_sample;
        float normal_dot_delta = dot(view_normal, delta_dir);
        float range_check = smoothstep(0.0, 1.0, radius / max(distance_to_sample, 1e-4));
        float horizon = saturate(normal_dot_delta - bias);
        occlusion += horizon * range_check * range_check;
    }

    float ao = saturate(1.0 - (occlusion / (float)sample_count) * g_ao_params.z);
    return pow(ao, max(g_ao_params.w, 0.25));
}

float BilateralRefine(float2 uv, float center_depth, float center_ao, float rotation) {
    float2 texel = g_screen_size.zw;
    float weighted_ao = 0.0;
    float weight_sum = 0.0;

    [unroll(5)]
    for (int y = -2; y <= 2; ++y) {
        [unroll(5)]
        for (int x = -2; x <= 2; ++x) {
            float2 offset = float2((float)x, (float)y);
            float2 sample_uv = saturate(uv + offset * texel);

            if (x == 0 && y == 0) {
                float weight = 1.0;
                weighted_ao += center_ao * weight;
                weight_sum += weight;
                continue;
            }

            float sample_depth = g_depth.SampleLevel(g_point_sampler, sample_uv, 0.0);
            float coarse_ao = ComputeGtao(sample_uv, 8u, rotation + (float)(x + y * 5) * 0.37);

            float depth_weight = exp(-abs(sample_depth - center_depth) * 1200.0);
            float spatial_weight = exp(-dot(offset, offset) * 0.22);
            float weight = depth_weight * spatial_weight * 0.85;

            weighted_ao += coarse_ao * weight;
            weight_sum += weight;
        }
    }

    return saturate(weighted_ao / max(weight_sum, 1e-4));
}

float4 main(PSIn input) : SV_TARGET {
    int2 pixel = (int2)input.pos.xy;
    float depth = g_depth.Load(int3(pixel, 0));

    float rotation = InterleavedGradientNoise(input.pos.xy) * 6.2831853;
    float raw_ao = ComputeGtao(input.uv, 64u, rotation);
    float refined_ao = BilateralRefine(input.uv, depth, raw_ao, rotation);

    float3 view_normal = normalize(g_view_normal.Load(int3(pixel, 0)).xyz * 2.0 - 1.0);
    float ndotv = saturate(view_normal.z);
    refined_ao = lerp(refined_ao, 1.0, pow(1.0 - ndotv, max(g_ao_temporal.w, 1.0)) * 0.35);

    return float4(refined_ao, refined_ao, refined_ao, 1.0);
}
)hlsl";

// ---------------------------------------------------------------------------
// 4) TAA resolve: motion reprojection, YCoCg clamp, 5 tap Catmull-Rom.
// ---------------------------------------------------------------------------
constexpr const char kShaderTaaResolve[] = R"hlsl(struct PSIn {
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD0;
};

Texture2D<float4> g_current_color : register(t0, space0);
Texture2D<float4> g_history_color : register(t1, space0);
Texture2D<float2> g_velocity : register(t2, space0);
Texture2D<float> g_depth : register(t3, space0);
SamplerState g_linear_sampler : register(s0, space0);
SamplerState g_point_sampler : register(s1, space0);

cbuffer TaaConstants : register(b0, space0) {
    float4 g_screen_size;      // xy pixels, zw inverse pixels
    float4 g_taa_params;       // x min feedback, y max feedback, z clamp sigma, w motion scale
    float4 g_depth_params;     // x near, y far, z disocclusion scale, w frame index
};

float3 RgbToYcocg(float3 color) {
    float luma = dot(color, float3(0.25, 0.5, 0.25));
    float chroma_orange = dot(color, float3(0.5, 0.0, -0.5));
    float chroma_green = dot(color, float3(-0.25, 0.5, -0.25));
    return float3(luma, chroma_orange, chroma_green);
}

float3 YcocgToRgb(float3 color) {
    float luma = color.x;
    float chroma_orange = color.y;
    float chroma_green = color.z;
    return float3(luma + chroma_orange - chroma_green, luma + chroma_green, luma - chroma_orange - chroma_green);
}

float LinearizeDepth(float depth) {
    float near_plane = max(g_depth_params.x, 1e-3);
    float far_plane = max(g_depth_params.y, near_plane + 1e-3);
    return (near_plane * far_plane) / max(far_plane - depth * (far_plane - near_plane), 1e-5);
}

float3 SampleCatmullRom5(Texture2D<float4> texture_object, SamplerState sampler_object, float2 uv, float2 texel_size) {
    float2 position = uv / texel_size;
    float2 center_position = floor(position - 0.5) + 0.5;
    float2 fraction = position - center_position;

    float2 w0 = fraction * (-0.5 + fraction * (1.0 - 0.5 * fraction));
    float2 w1 = 1.0 + fraction * fraction * (-2.5 + 1.5 * fraction);
    float2 w2 = fraction * (0.5 + fraction * (2.0 - 1.5 * fraction));
    float2 w3 = fraction * fraction * (-0.5 + 0.5 * fraction);

    float2 w12 = w1 + w2;
    float2 offset12 = w2 / max(w12, 1e-5);
    float2 tc0 = (center_position - 1.0) * texel_size;
    float2 tc3 = (center_position + 2.0) * texel_size;
    float2 tc12 = (center_position + offset12) * texel_size;

    float3 result = 0.0;
    result += texture_object.SampleLevel(sampler_object, float2(tc12.x, tc0.y), 0.0).rgb * (w12.x * w0.y);
    result += texture_object.SampleLevel(sampler_object, float2(tc0.x, tc12.y), 0.0).rgb * (w0.x * w12.y);
    result += texture_object.SampleLevel(sampler_object, tc12, 0.0).rgb * (w12.x * w12.y);
    result += texture_object.SampleLevel(sampler_object, float2(tc3.x, tc12.y), 0.0).rgb * (w3.x * w12.y);
    result += texture_object.SampleLevel(sampler_object, float2(tc12.x, tc3.y), 0.0).rgb * (w12.x * w3.y);
    return result;
}

float4 main(PSIn input) : SV_TARGET {
    int2 pixel = (int2)input.pos.xy;
    float2 texel = g_screen_size.zw;

    float3 current = g_current_color.Load(int3(pixel, 0)).rgb;
    float2 velocity = g_velocity.Load(int3(pixel, 0)) * g_taa_params.w;
    float2 reprojected_uv = input.uv - velocity;
    float depth = g_depth.Load(int3(pixel, 0));

    float3 mean = 0.0;
    float3 mean_squared = 0.0;

    [unroll(3)]
    for (int y = -1; y <= 1; ++y) {
        [unroll(3)]
        for (int x = -1; x <= 1; ++x) {
            float2 offset = float2((float)x, (float)y);
            float3 neighborhood = RgbToYcocg(
                g_current_color.SampleLevel(g_point_sampler, input.uv + offset * texel, 0.0).rgb);
            mean += neighborhood;
            mean_squared += neighborhood * neighborhood;
        }
    }

    mean /= 9.0;
    mean_squared /= 9.0;

    float3 sigma = sqrt(max(mean_squared - mean * mean, 0.0));
    float3 lower_bound = mean - sigma * g_taa_params.z;
    float3 upper_bound = mean + sigma * g_taa_params.z;

    float3 history_rgb = SampleCatmullRom5(g_history_color, g_linear_sampler, saturate(reprojected_uv), texel);
    float3 history_clamped = clamp(RgbToYcocg(history_rgb), lower_bound, upper_bound);

    float history_depth = g_depth.SampleLevel(g_point_sampler, saturate(reprojected_uv), 0.0);
    float depth_delta = abs(LinearizeDepth(depth) - LinearizeDepth(history_depth));
    float disocclusion = saturate(depth_delta / max(g_depth_params.z, 1e-3));

    float motion_length = length(velocity) / max(texel.x, 1e-6);
    float feedback = clamp(1.0 - motion_length * 0.35, g_taa_params.x, g_taa_params.y);
    feedback = lerp(feedback, 0.0, disocclusion);

    float in_bounds = (reprojected_uv.x >= 0.0 && reprojected_uv.x <= 1.0 &&
                       reprojected_uv.y >= 0.0 && reprojected_uv.y <= 1.0) ? 1.0 : 0.0;
    feedback *= in_bounds;

    float3 resolved = lerp(current, YcocgToRgb(history_clamped), feedback);
    return float4(max(resolved, 0.0), 1.0);
}
)hlsl";

// ---------------------------------------------------------------------------
// 5) Cluster light culling: 256 lights per cluster, packed half2 AABB.
// ---------------------------------------------------------------------------
constexpr const char kShaderClusterCull[] = R"hlsl(struct GpuLight {
    float4 center_radius;   // xyz centre, w radius
    float4 color_intensity; // rgb colour, w intensity
};

StructuredBuffer<uint4> g_cluster_bounds : register(t0, space0);   // packed half2 AABB pairs
StructuredBuffer<GpuLight> g_lights : register(t1, space0);
RWStructuredBuffer<uint> g_light_indices : register(u0, space0);   // flattened index pool
RWStructuredBuffer<uint> g_cluster_headers : register(u1, space0); // offset, count per cluster
RWStructuredBuffer<uint> g_allocator : register(u2, space0);       // single atomic cursor

cbuffer ClusterConstants : register(b0, space0) {
    uint g_cluster_count;
    uint g_light_count;
    uint g_indices_per_cluster;
    uint g_total_index_capacity;
};

groupshared uint g_visible_indices[256];
groupshared uint g_visible_count;
groupshared uint g_cluster_offset;

float UnpackHalfLow(uint packed_value) {
    return f16tof32(packed_value & 0xFFFFu);
}

float UnpackHalfHigh(uint packed_value) {
    return f16tof32(packed_value >> 16);
}

float3 UnpackClusterMinimum(uint4 packed_bounds) {
    return float3(
        UnpackHalfLow(packed_bounds.x),
        UnpackHalfHigh(packed_bounds.x),
        UnpackHalfLow(packed_bounds.y));
}

float3 UnpackClusterMaximum(uint4 packed_bounds) {
    return float3(
        UnpackHalfHigh(packed_bounds.y),
        UnpackHalfLow(packed_bounds.z),
        UnpackHalfHigh(packed_bounds.z));
}

[numthreads(64, 1, 1)]
void main(uint3 group_id : SV_GroupID, uint3 group_thread : SV_GroupThreadID) {
    uint cluster_index = group_id.x;
    uint lane = group_thread.x;

    if (lane == 0) {
        g_visible_count = 0u;
        g_cluster_offset = 0xFFFFFFFFu;
    }
    GroupMemoryBarrierWithGroupSync();

    if (cluster_index >= g_cluster_count) {
        return;
    }

    uint4 packed_bounds = g_cluster_bounds[cluster_index];
    float3 cluster_minimum = UnpackClusterMinimum(packed_bounds);
    float3 cluster_maximum = UnpackClusterMaximum(packed_bounds);

    [unroll(4)]
    for (uint batch = 0; batch < 4u; ++batch) {
        uint light_index = lane + batch * 64u;
        if (light_index >= g_light_count) {
            break;
        }

        GpuLight light = g_lights[light_index];
        float3 center = light.center_radius.xyz;
        float radius = light.center_radius.w;
        float3 closest_point = clamp(center, cluster_minimum, cluster_maximum);
        float3 delta = center - closest_point;

        if (dot(delta, delta) <= radius * radius) {
            uint slot = 0u;
            InterlockedAdd(g_visible_count, 1u, slot);
            if (slot < 256u) {
                g_visible_indices[slot] = light_index;
            }
        }
    }

    GroupMemoryBarrierWithGroupSync();

    uint visible_count = min(g_visible_count, 256u);
    if (lane == 0) {
        uint allocated = 0u;
        InterlockedAdd(g_allocator[0], visible_count, allocated);
        g_cluster_offset = allocated;
        g_cluster_headers[cluster_index * 2u] = allocated;
        g_cluster_headers[cluster_index * 2u + 1u] = visible_count;
    }
    GroupMemoryBarrierWithGroupSync();

    uint cluster_offset = g_cluster_offset;
    [loop]
    for (uint index = lane; index < visible_count; index += 64u) {
        uint destination = cluster_offset + index;
        if (destination < g_total_index_capacity) {
            g_light_indices[destination] = g_visible_indices[index];
        }
    }
}
)hlsl";

// ---------------------------------------------------------------------------
// 6) Character skinning: 4 bones per vertex, dual quaternion blend, half weights.
// ---------------------------------------------------------------------------
constexpr const char kShaderCharacterSkinning[] = R"hlsl(struct SkinnedVertex {
    float4 position;      // xyz rest position, w unused
    float4 normal;        // xyz rest normal, w tangent sign
    float4 uv;            // xy texture coordinates, zw unused
    uint4 bone_indices;   // four bone influences
    float4 bone_weights;  // matching weights, not normalised
};

struct GpuBone {
    float4 rotation;      // quaternion xyzw
    float4 translation;   // xyz bind translation
};

struct DualQuaternion {
    float4 real;
    float4 dual;
};

StructuredBuffer<SkinnedVertex> g_skinned_vertices : register(t0, space0);
StructuredBuffer<GpuBone> g_bones : register(t1, space0);
RWStructuredBuffer<float4> g_output_positions : register(u0, space0);
RWStructuredBuffer<float4> g_output_normals : register(u1, space0);

cbuffer SkinningConstants : register(b0, space0) {
    uint g_vertex_count;
    uint g_bone_count;
    float g_weight_epsilon;
    uint g_dispatch_padding;
};

DualQuaternion BuildDualQuaternion(GpuBone bone) {
    DualQuaternion result;
    result.real = bone.rotation;

    float4 q = bone.rotation;
    float3 t = bone.translation.xyz;
    float3 cross_term = float3(
        t.y * q.z - t.z * q.y,
        t.z * q.x - t.x * q.z,
        t.x * q.y - t.y * q.x);

    result.dual = 0.5 * float4(
        q.w * t.x + cross_term.x,
        q.w * t.y + cross_term.y,
        q.w * t.z + cross_term.z,
        -dot(t, q.xyz));
    return result;
}

DualQuaternion NegateDualQuaternion(DualQuaternion value) {
    DualQuaternion result;
    result.real = -value.real;
    result.dual = -value.dual;
    return result;
}

DualQuaternion BlendDualQuaternions(uint4 bone_indices, half4 bone_weights) {
    DualQuaternion accumulator;
    accumulator.real = float4(0.0, 0.0, 0.0, 0.0);
    accumulator.dual = float4(0.0, 0.0, 0.0, 0.0);

    [unroll(4)]
    for (uint influence = 0; influence < 4u; ++influence) {
        uint bone_index = min(bone_indices[influence], g_bone_count - 1u);
        float weight = (float)bone_weights[influence];
        if (weight <= 0.0) {
            continue;
        }

        DualQuaternion sample_quaternion = BuildDualQuaternion(g_bones[bone_index]);
        if (dot(accumulator.real, sample_quaternion.real) < 0.0) {
            sample_quaternion = NegateDualQuaternion(sample_quaternion);
        }

        accumulator.real += sample_quaternion.real * weight;
        accumulator.dual += sample_quaternion.dual * weight;
    }

    float length_real = max(length(accumulator.real), g_weight_epsilon);
    accumulator.real /= length_real;
    accumulator.dual /= length_real;
    accumulator.dual -= accumulator.real * dot(accumulator.real, accumulator.dual);
    return accumulator;
}

float3 TransformPoint(DualQuaternion dual_quaternion, float3 position) {
    float3 real_vector = dual_quaternion.real.xyz;
    float real_scalar = dual_quaternion.real.w;
    float3 dual_vector = dual_quaternion.dual.xyz;

    float3 translated = position +
        2.0 * cross(real_vector, cross(real_vector, position) + real_scalar * position);
    float3 offset = 2.0 * (real_scalar * dual_vector + cross(real_vector, dual_vector));
    return translated + offset;
}

float3 RotateVector(DualQuaternion dual_quaternion, float3 direction) {
    float3 real_vector = dual_quaternion.real.xyz;
    float real_scalar = dual_quaternion.real.w;
    return direction + 2.0 * cross(real_vector, cross(real_vector, direction) + real_scalar * direction);
}

[numthreads(64, 1, 1)]
void main(uint3 dispatch_id : SV_DispatchThreadID) {
    uint vertex_index = dispatch_id.x;
    if (vertex_index >= g_vertex_count) {
        return;
    }

    SkinnedVertex vertex = g_skinned_vertices[vertex_index];

    half4 weights = half4(vertex.bone_weights);
    half weight_sum = weights.x + weights.y + weights.z + weights.w;
    weights = weights / max(weight_sum, half(g_weight_epsilon));

    DualQuaternion blended = BlendDualQuaternions(vertex.bone_indices, weights);

    float3 skinned_position = TransformPoint(blended, vertex.position.xyz);
    float3 skinned_normal = normalize(RotateVector(blended, vertex.normal.xyz));
    half skinned_tangent_sign = half(vertex.normal.w);

    g_output_positions[vertex_index] = float4(skinned_position, 1.0);
    g_output_normals[vertex_index] = float4(skinned_normal, (float)skinned_tangent_sign);
}
)hlsl";

// ---------------------------------------------------------------------------
// 7) Terrain mesh shader: 32 procedural triangles from a heightmap patch.
// ---------------------------------------------------------------------------
constexpr const char kShaderTerrainMesh[] = R"hlsl(struct TerrainVertex {
    float4 position : SV_POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
};

Texture2D<float> g_heightmap : register(t0, space0);
SamplerState g_height_sampler : register(s0, space0);

cbuffer TerrainConstants : register(b0, space0) {
    float4x4 g_view_projection;
    float4 g_patch_origin;   // xyz world origin, w patch index
    float4 g_patch_size;     // x world size, y height scale, z uv scale, w lod
};

[numthreads(64, 1, 1)]
[outputtopology("triangle")]
void main(
    uint3 group_id : SV_GroupID,
    uint3 group_thread : SV_GroupThreadID,
    out vertices TerrainVertex vertices[25],
    out indices uint3 triangles[32]) {

    const uint kQuadsPerAxis = 4u;
    const uint kVertexCount = 25u;
    const uint kTriangleCount = 32u;

    SetMeshOutputCounts(kVertexCount, kTriangleCount);

    uint lane = group_thread.x;
    float cell_size = g_patch_size.x / (float)kQuadsPerAxis;
    float2 patch_origin_xz = g_patch_origin.xz + float2((float)group_id.x, (float)group_id.y) * g_patch_size.x;

    if (lane < kVertexCount) {
        uint vertex_x = lane % 5u;
        uint vertex_z = lane / 5u;
        float2 local_xz = float2((float)vertex_x, (float)vertex_z) * cell_size;
        float2 world_xz = patch_origin_xz + local_xz;
        float2 height_uv = world_xz * g_patch_size.z;

        float texel_step = g_patch_size.z * cell_size;
        float height = g_heightmap.SampleLevel(g_height_sampler, height_uv, 0.0);
        float height_x = g_heightmap.SampleLevel(g_height_sampler, height_uv + float2(texel_step, 0.0), 0.0);
        float height_z = g_heightmap.SampleLevel(g_height_sampler, height_uv + float2(0.0, texel_step), 0.0);

        float slope_x = (height_x - height) * g_patch_size.y / max(cell_size, 1e-4);
        float slope_z = (height_z - height) * g_patch_size.y / max(cell_size, 1e-4);
        half3 normal_half = normalize(half3(-slope_x, 1.0h, -slope_z));

        float3 world_position = float3(world_xz.x, height * g_patch_size.y, world_xz.y);

        TerrainVertex output_vertex;
        output_vertex.position = mul(float4(world_position, 1.0), g_view_projection);
        output_vertex.normal = float3(normal_half);
        output_vertex.uv = height_uv;
        vertices[lane] = output_vertex;
    }

    if (lane < kTriangleCount) {
        uint quad_index = lane >> 1u;
        uint triangle_in_quad = lane & 1u;
        uint quad_x = quad_index % kQuadsPerAxis;
        uint quad_z = quad_index / kQuadsPerAxis;

        uint index_0 = quad_x + quad_z * 5u;
        uint index_1 = index_0 + 1u;
        uint index_2 = index_0 + 5u;
        uint index_3 = index_2 + 1u;

        triangles[lane] = (triangle_in_quad == 0u) ? uint3(index_0, index_2, index_1)
                                                   : uint3(index_1, index_2, index_3);
    }
}
)hlsl";

// ---------------------------------------------------------------------------
// 8) Amplification shader: 8 meshlet AABBs per group, DispatchMesh payload.
// ---------------------------------------------------------------------------
constexpr const char kShaderGpuCullingAmp[] = R"hlsl(struct MeshletBounds {
    float4 center_radius;   // xyz bounding centre, w bounding radius
    float4 aabb_min;        // xyz minimum corner
    float4 aabb_max;        // xyz maximum corner
};

StructuredBuffer<MeshletBounds> g_meshlets : register(t0, space0);

cbuffer CullConstants : register(b0, space0) {
    float4 g_frustum_planes[6];   // xyz plane normal, w plane distance
    uint g_meshlet_count;
    uint g_group_stride;
    float g_lod_radius_bias;
    uint g_cascade_index;
};

struct AmplificationPayload {
    uint meshlet_indices[8];
    uint meshlet_count;
    uint cascade_index;
    uint padding;
};

groupshared AmplificationPayload g_payload;
groupshared uint g_visible_meshlets[8];
groupshared uint g_visible_count;

bool MeshletInsideFrustum(MeshletBounds bounds) {
    if (bounds.center_radius.w < g_lod_radius_bias) {
        return false;
    }

    float3 aabb_minimum = bounds.aabb_min.xyz;
    float3 aabb_maximum = bounds.aabb_max.xyz;

    [unroll(6)]
    for (uint plane_index = 0; plane_index < 6u; ++plane_index) {
        float4 plane = g_frustum_planes[plane_index];

        float3 positive_vertex = float3(
            plane.x >= 0.0 ? aabb_maximum.x : aabb_minimum.x,
            plane.y >= 0.0 ? aabb_maximum.y : aabb_minimum.y,
            plane.z >= 0.0 ? aabb_maximum.z : aabb_minimum.z);

        float distance_to_plane = dot(plane.xyz, positive_vertex) + plane.w;
        if (distance_to_plane < 0.0) {
            return false;
        }
    }

    return true;
}

[numthreads(64, 1, 1)]
void main(
    in uint3 group_id : SV_GroupID,
    in uint3 group_thread : SV_GroupThreadID,
    in payload AmplificationPayload payload) {

    uint lane = group_thread.x;

    if (lane == 0u) {
        g_visible_count = 0u;
    }
    GroupMemoryBarrierWithGroupSync();

    uint base_meshlet = group_id.x * 8u;

    if (lane < 8u) {
        uint meshlet_index = base_meshlet + lane;
        if (meshlet_index < g_meshlet_count) {
            MeshletBounds bounds = g_meshlets[meshlet_index];
            if (MeshletInsideFrustum(bounds)) {
                uint slot = 0u;
                InterlockedAdd(g_visible_count, 1u, slot);
                if (slot < 8u) {
                    g_visible_meshlets[slot] = meshlet_index;
                }
            }
        }
    }

    GroupMemoryBarrierWithGroupSync();

    uint visible_count = min(g_visible_count, 8u);

    [loop]
    for (uint index = lane; index < visible_count; index += 64u) {
        g_payload.meshlet_indices[index] = g_visible_meshlets[index];
    }

    if (lane == 0u) {
        g_payload.meshlet_count = visible_count;
        g_payload.cascade_index = g_cascade_index;
        g_payload.padding = 0u;
    }

    GroupMemoryBarrierWithGroupSync();

    // DXIL requires the DispatchMesh call to dominate every exit of an
    // amplification entry point, so it is unconditional; a zero meshlet count
    // dispatches nothing.
    DispatchMesh(visible_count, 1u, 1u, g_payload);
}
)hlsl";

// ---------------------------------------------------------------------------
// 9) Ray traced GI: 8 rays per pixel, 2 bounces, inline ray queries.
// ---------------------------------------------------------------------------
constexpr const char kShaderRaytracedGi[] = R"hlsl(struct PrimitiveSurface {
    float4 normal_roughness;   // xyz geometric normal, w roughness
    float4 albedo;             // rgb albedo, w emissive strength
};

RaytracingAccelerationStructure g_scene : register(t0, space0);
RWTexture2D<float4> g_gi_output : register(u0, space0);
Texture2D<float4> g_gbuffer_normal : register(t1, space0);
Texture2D<float4> g_gbuffer_albedo : register(t2, space0);
Texture2D<float> g_gbuffer_depth : register(t3, space0);
StructuredBuffer<PrimitiveSurface> g_surfaces : register(t4, space0);

cbuffer GiConstants : register(b0, space0) {
    float4x4 g_inv_view_projection;
    float4 g_camera_position;
    float4 g_screen_size;      // xy pixels, zw inverse pixels
    float4 g_sun_direction;    // xyz direction to sun, w intensity
    float4 g_sun_color;
    float4 g_gi_params;        // x ray bias, y max distance, z sky scale, w frame index
};

float HashUint(uint value) {
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    value ^= value >> 16;
    return (float)value * (1.0 / 4294967295.0);
}

float3 PreethamLiteSky(float3 direction, float3 sun_direction, float3 sun_color) {
    float up = saturate(direction.y * 0.5 + 0.5);
    float3 horizon = float3(0.68, 0.78, 0.92);
    float3 zenith = float3(0.16, 0.32, 0.62);
    float3 sky = lerp(horizon, zenith, pow(up, 0.65));

    float sun_alignment = saturate(dot(direction, sun_direction));
    float disc = pow(sun_alignment, 900.0) * 12.0;
    float glow = pow(sun_alignment, 18.0) * 0.35;
    sky += sun_color * (disc + glow);

    float ground = saturate(-direction.y * 2.0);
    sky = lerp(sky, float3(0.09, 0.07, 0.05), ground * 0.85);
    return sky;
}

float3 CosineHemisphere(float3 normal, float r1, float r2) {
    float phi = 6.2831853 * r1;
    float cos_theta = sqrt(1.0 - r2);
    float sin_theta = sqrt(r2);
    float3 helper = abs(normal.z) < 0.95 ? float3(0.0, 0.0, 1.0) : float3(0.0, 1.0, 0.0);
    float3 tangent = normalize(cross(helper, normal));
    float3 bitangent = cross(normal, tangent);
    float3 local_dir = float3(cos(phi) * sin_theta, sin(phi) * sin_theta, cos_theta);
    return normalize(tangent * local_dir.x + bitangent * local_dir.y + normal * local_dir.z);
}

[numthreads(64, 1, 1)]
void main(uint3 dispatch_id : SV_DispatchThreadID) {
    uint2 pixel = dispatch_id.xy;
    uint2 screen_size = uint2((uint)g_screen_size.x, (uint)g_screen_size.y);
    if (pixel.x >= screen_size.x || pixel.y >= screen_size.y) {
        return;
    }

    float2 uv = (float2(pixel) + 0.5) * g_screen_size.zw;
    float depth = g_gbuffer_depth.Load(int3(pixel, 0));
    float4 normal_sample = g_gbuffer_normal.Load(int3(pixel, 0));
    float4 albedo_sample = g_gbuffer_albedo.Load(int3(pixel, 0));

    float3 normal = normalize(normal_sample.xyz * 2.0 - 1.0);
    float3 albedo = albedo_sample.rgb;

    float2 ndc = uv * 2.0 - 1.0;
    float4 world_h = mul(float4(ndc, depth, 1.0), g_inv_view_projection);
    float3 world_position = world_h.xyz / max(world_h.w, 1e-6);

    float3 sun_direction = normalize(g_sun_direction.xyz);
    float3 sun_color = g_sun_color.rgb * g_sun_direction.w;
    float2 pixel_coords = float2(pixel);
    float pixel_noise = frac(52.9829189 * frac(dot(pixel_coords, float2(0.06711056, 0.00583715))));

    float3 indirect = 0.0;

    [unroll(8)]
    for (uint ray_index = 0; ray_index < 8u; ++ray_index) {
        float r1 = frac(((float)ray_index + pixel_noise) * 0.7548776662);
        float r2 = frac(((float)ray_index + pixel_noise) * 0.5698402910 + HashUint(dispatch_id.x + dispatch_id.y * 37u) * 0.5);

        float3 ray_direction = CosineHemisphere(normal, r1, r2);
        float3 origin = world_position + normal * g_gi_params.x;
        float3 throughput = albedo;

        [unroll(2)]
        for (uint bounce = 0; bounce < 2u; ++bounce) {
            RayDesc ray;
            ray.Origin = origin;
            ray.Direction = ray_direction;
            ray.TMin = g_gi_params.x;
            ray.TMax = g_gi_params.y;

            RayQuery<RAY_FLAG_NONE> query;
            query.TraceRayInline(g_scene, RAY_FLAG_NONE, 0xFF, ray);

            while (query.Proceed()) {
                if (query.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE) {
                    query.CommitNonOpaqueTriangleHit();
                }
            }

            if (query.CommittedStatus() != COMMITTED_TRIANGLE_HIT) {
                indirect += throughput * PreethamLiteSky(ray_direction, sun_direction, sun_color) * g_gi_params.z;
                break;
            }

            float hit_distance = query.CommittedRayT();
            float3 hit_position = origin + ray_direction * hit_distance;
            uint primitive_index = query.CommittedPrimitiveIndex();
            PrimitiveSurface surface = g_surfaces[primitive_index];

            float3 hit_normal = normalize(surface.normal_roughness.xyz);
            float3 hit_albedo = surface.albedo.rgb;
            float roughness = saturate(surface.normal_roughness.w);

            float sun_visibility = saturate(dot(hit_normal, sun_direction));
            float3 direct_light = sun_color * (sun_visibility * 0.85);
            float3 ambient_light = PreethamLiteSky(hit_normal, sun_direction, sun_color) * 0.1;

            indirect += throughput * hit_albedo * (direct_light + ambient_light) * (1.0 - roughness * 0.6);

            throughput *= hit_albedo * 0.45;
            origin = hit_position + hit_normal * g_gi_params.x;

            float3 bounce_dir = CosineHemisphere(hit_normal, frac(r1 + 0.37), frac(r2 + 0.61));
            ray_direction = bounce_dir;
        }
    }

    float3 result = indirect * (1.0 / 8.0);
    g_gi_output[pixel] = float4(max(result, 0.0), 1.0);
}
)hlsl";

// ---------------------------------------------------------------------------
// 10) Hair strands: 16 segments per strand, Marschner-lite shading, half colour.
// ---------------------------------------------------------------------------
constexpr const char kShaderHairStrands[] = R"hlsl(struct HairStrand {
    float4 root;     // xyz root position, w strand length
    float4 tip;      // xyz tip position, w strand thickness
    float4 params;   // x melanin, y redness, z roughness, w random seed
};

StructuredBuffer<HairStrand> g_strands : register(t0, space0);
RWStructuredBuffer<float4> g_segment_output : register(u0, space0);

cbuffer HairConstants : register(b0, space0) {
    uint g_strand_count;
    uint g_segment_count;
    float4 g_camera_position;
    float4 g_key_direction;   // xyz direction to key light, w intensity
    float4 g_key_color;
    float4 g_rim_direction;
    float4 g_rim_color;
    float4 g_hair_params;     // x specular shift, y ambient, z scatter, w exposure
};

float HairLongitudinalTerm(float h_dot_t, float shift, float width) {
    float delta = (h_dot_t - shift) / max(width, 1e-3);
    return exp(-delta * delta);
}

float HairAzimuthalTerm(float sin_theta_h, float roughness) {
    float ratio = max(sin_theta_h, 0.0) / max(roughness, 0.05);
    return exp(-ratio * ratio);
}

float3 ShadeStrand(
    float3 tangent,
    float3 normal,
    float3 view_dir,
    float3 light_dir,
    float3 base_color,
    float roughness,
    float shift,
    float scatter) {

    float3 half_vector = normalize(view_dir + light_dir);
    float h_dot_t = dot(half_vector, tangent);
    float sin_theta_h = sqrt(max(1.0 - h_dot_t * h_dot_t, 0.0));

    float azimuthal = HairAzimuthalTerm(sin_theta_h, roughness);
    float longitudinal_r = HairLongitudinalTerm(h_dot_t, shift, 0.22);
    float longitudinal_tt = HairLongitudinalTerm(h_dot_t, -shift * 0.6, 0.38) * scatter;

    float3 specular = base_color * (azimuthal * (longitudinal_r + longitudinal_tt));

    float n_dot_l = saturate(dot(normal, light_dir));
    float n_dot_v = saturate(dot(normal, view_dir));
    specular *= 1.0 / max(4.0 * n_dot_l * n_dot_v, 0.08);

    float t_dot_l = dot(tangent, light_dir);
    float3 diffuse = base_color * (0.25 * (0.5 + 0.5 * t_dot_l));
    return specular + diffuse;
}

[numthreads(64, 1, 1)]
void main(uint3 dispatch_id : SV_DispatchThreadID) {
    uint strand_index = dispatch_id.x;
    if (strand_index >= g_strand_count) {
        return;
    }

    HairStrand strand = g_strands[strand_index];
    float3 root = strand.root.xyz;
    float3 tip = strand.tip.xyz;
    float3 axis = tip - root;
    float axis_length = length(axis);
    if (axis_length < 1e-5) {
        return;
    }

    float3 axis_dir = axis / axis_length;
    float melanin = saturate(strand.params.x);
    float redness = saturate(strand.params.y);
    float roughness = clamp(strand.params.z, 0.06, 0.9);
    float thickness = max(strand.tip.w, 1e-4);

    float3 blond = float3(0.78, 0.62, 0.36);
    float3 brown = float3(0.32 + 0.45 * redness, 0.22 + 0.30 * redness, 0.13);
    float3 base_color = lerp(blond, brown, saturate(0.35 + melanin));
    base_color = lerp(base_color, float3(0.05, 0.04, 0.03), melanin * melanin);

    float3 camera_position = g_camera_position.xyz;
    float3 key_direction = normalize(g_key_direction.xyz);
    float3 rim_direction = normalize(g_rim_direction.xyz);
    uint segment_count = min(g_segment_count, 16u);

    [unroll(16)]
    for (uint segment = 0; segment < 16u; ++segment) {
        if (segment >= segment_count) {
            break;
        }

        float t0 = (float)segment / (float)segment_count;
        float t1 = (float)(segment + 1u) / (float)segment_count;

        float3 start = root + axis_dir * (axis_length * t0);
        float3 end = root + axis_dir * (axis_length * t1) + float3(0.0, -0.06 * t1 * t1, 0.0);
        float3 center = (start + end) * 0.5;
        float3 tangent = normalize(end - start);
        float3 view_dir = normalize(camera_position - center);

        float3 side = cross(tangent, view_dir);
        float3 strand_normal = normalize(side + float3(1e-5, 1e-5, 1e-5));

        float shift = (strand.params.w - 0.5) * 0.14 + g_hair_params.x;

        float key_n_dot_l = saturate(dot(strand_normal, key_direction) + 0.35);
        float3 key_light = ShadeStrand(
            tangent,
            strand_normal,
            view_dir,
            key_direction,
            base_color,
            roughness,
            shift,
            g_hair_params.z) * g_key_color.rgb * (g_key_direction.w * key_n_dot_l);

        float rim_n_dot_l = saturate(dot(strand_normal, rim_direction) + 0.2);
        float3 rim_light = ShadeStrand(
            tangent,
            strand_normal,
            view_dir,
            rim_direction,
            base_color,
            roughness * 1.35,
            -shift,
            g_hair_params.z * 0.5) * g_rim_color.rgb * (g_rim_direction.w * rim_n_dot_l);

        half3 color = half3(key_light + rim_light) + half3(base_color * g_hair_params.y);
        half occlusion = half(1.0 - 0.4 * t1);
        half3 final_color = color * occlusion * half(g_hair_params.w);

        uint output_index = strand_index * g_segment_count + segment;
        float alpha = saturate(1.0 - t0 * 0.65) * saturate(thickness * 2048.0);
        g_segment_output[output_index] = float4(float3(final_color), alpha);
    }
}
)hlsl";

}  // namespace

static const std::vector<ShaderCase>& BuildCorpus() {
    static const std::vector<ShaderCase> corpus = []() {
        std::vector<ShaderCase> cases;
        cases.reserve(10);
        cases.push_back(ShaderCase{
            "deferred_lighting",
            "Отложенное освещение: 64 источника, GGX BRDF, сравнение теней, half-точность",
            "ps_6_6",
            "main",
            std::string(kShaderDeferredLighting),
            true,
        });
        cases.push_back(ShaderCase{
            "volumetric_fog",
            "Объёмный туман: 96 шагов raymarch, 3D-текстура плотности, HG-фазы, 4 источника",
            "ps_6_6",
            "main",
            std::string(kShaderVolumetricFog),
            false,
        });
        cases.push_back(ShaderCase{
            "ssao_gtao",
            "SSAO GTAO: 64 сэмпла по спирали Кимуры, реконструкция глубины и нормалей, bilateral 5x5",
            "ps_6_6",
            "main",
            std::string(kShaderSsaoGtao),
            false,
        });
        cases.push_back(ShaderCase{
            "taa_resolve",
            "TAA resolve: репроекция по motion vectors, клампинг 3x3, YCoCg, Catmull-Rom 5 тапов",
            "ps_6_6",
            "main",
            std::string(kShaderTaaResolve),
            false,
        });
        cases.push_back(ShaderCase{
            "cluster_cull_cs",
            "Кластерный куллинг: 256 источников на кластер, AABB-тест, InterlockedAdd, groupshared",
            "cs_6_6",
            "main",
            std::string(kShaderClusterCull),
            false,
        });
        cases.push_back(ShaderCase{
            "character_skinning_cs",
            "Скиннинг персонажа: 4 кости на вершину, двойные кватернионы DQS, half-веса",
            "cs_6_6",
            "main",
            std::string(kShaderCharacterSkinning),
            true,
        });
        cases.push_back(ShaderCase{
            "terrain_mesh",
            "Terrain mesh shader: 32 треугольника из heightmap, SetMeshOutputCounts, half-нормали",
            "ms_6_6",
            "main",
            std::string(kShaderTerrainMesh),
            true,
        });
        cases.push_back(ShaderCase{
            "gpu_culling_amp",
            "Amplification shader: 8 мешлетов на группу, frustum-тест AABB, DispatchMesh",
            "as_6_6",
            "main",
            std::string(kShaderGpuCullingAmp),
            false,
        });
        cases.push_back(ShaderCase{
            "raytraced_gi_cs",
            "Трассируемый GI: 8 лучей на пиксель, 2 отскока, RayQuery, небо Preetham-lite",
            "cs_6_6",
            "main",
            std::string(kShaderRaytracedGi),
            false,
        });
        cases.push_back(ShaderCase{
            "hair_strands_cs",
            "Волосы: 16 сегментов пряди, Marschner-lite, Karis-нормализация, half-цвет",
            "cs_6_6",
            "main",
            std::string(kShaderHairStrands),
            true,
        });
        return cases;
    }();
    return corpus;
}

const std::vector<ShaderCase>& Gta6ShaderCorpus() {
    return BuildCorpus();
}

ShaderCase MakeVariant(const ShaderCase& base, uint32_t variant) {
    ShaderCase copy = base;
    copy.id = base.id + "_v" + std::to_string(variant);
    copy.hlsl = "#define SX_VARIANT " + std::to_string(variant) + "\n" + base.hlsl;
    return copy;
}

}  // namespace vicewindev
