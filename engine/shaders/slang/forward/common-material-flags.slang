// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 07.10.2026
//

// Material flag bits (subset of the engine MaterialUniforms flags).
static const uint FLAG_HAS_BASECOLOR = 1u << 0;
static const uint FLAG_ALPHA_TEST   = 1u << 1;
static const uint FLAG_HAS_NORMAL   = 1u << 2;
static const uint FLAG_DOUBLE_SIDED = 1u << 3;
static const uint FLAG_BASE_UV1     = 1u << 4;   // per-map UV-set selection (uv1 when set)
static const uint FLAG_NORMAL_UV1   = 1u << 5;
static const uint FLAG_HAS_METALROUGH = 1u << 6;
static const uint FLAG_METALROUGH_UV1 = 1u << 7;
static const uint FLAG_SKYBOX         = 1u << 8;
static const uint FLAG_HAS_OCCLUSION  = 1u << 9;
static const uint FLAG_OCCLUSION_UV1  = 1u << 10;
static const uint FLAG_HAS_EMISSIVE   = 1u << 11;
static const uint FLAG_EMISSIVE_UV1   = 1u << 12;
static const uint FLAG_OCCLUDE_DIRECT = 1u << 13;   // Material::setOccludeDirect
static const uint FLAG_HAS_HEIGHTMAP  = 1u << 17;

// Specular occlusion modes (scene/constants.h SPECOCC_*, Material::setOccludeSpecular).
static const uint SPECOCC_NONE = 0u;
static const uint SPECOCC_AO = 1u;
static const uint SPECOCC_GLOSSDEPENDENT = 2u;

float2 applyUvTransform(float2 uv, float4 row0, float4 row1) {
    float3 h = float3(uv, 1.0);
    return float2(dot(h, row0.xyz), dot(h, row1.xyz));
}

// Distance attenuation: inverse-square with a smooth range window, or linear
// falloff when coneParams.z != 0 (matches the engine's falloffModeLinear).
// A range of 0 is clamped, so it extinguishes the light instead of removing the
// limit.
float distanceAttenuation(float dist, float range, float linearFalloff) {
    range = max(range, 1e-4);
    float t = clamp(dist / range, 0.0, 1.0);
    if (linearFalloff > 0.5) {
        return clamp(1.0 - t, 0.0, 1.0);
    }
    // 16 / (d^2 + 1), NOT 1 / d^2. The +1 keeps the curve finite at the light and
    // the 16 restores the magnitude that softening costs; without them a light at
    // four units reads 1/16 instead of 16/17, roughly a fifteenth.
    float sqrDist = dist * dist;
    float invSq = 16.0 / (sqrDist + 1.0);
    float window = clamp(1.0 - t * t * t * t, 0.0, 1.0);
    return invSq * window * window;
}

// Spot cone falloff. SMOOTHSTEP between the two cone cosines. A plain ramp,
// squared or not, is dimmer
// through the whole penumbra — a squared ramp gives half the light at the middle
// of it — and only agrees at the two ends.
float getSpotEffect(float innerConeCos, float outerConeCos, float cosAngle) {
    return smoothstep(outerConeCos, innerConeCos, cosAngle);
}

