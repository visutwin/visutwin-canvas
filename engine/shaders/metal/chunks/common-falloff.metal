// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
static inline float getFalloffWindow(float lightRadius, float3 lightDir)
{
    const float sqrDist = dot(lightDir, lightDir);
    const float invRadius = 1.0 / max(lightRadius, 1e-4);
    return square(saturate(1.0 - square(sqrDist * square(invRadius))));
}

static inline float getFalloffLinear(float lightRadius, float3 lightDir)
{
    const float d = length(lightDir);
    return max((lightRadius - d) / max(lightRadius, 1e-4), 0.0);
}

static inline float getFalloffInvSquared(float lightRadius, float3 lightDir)
{
    const float sqrDist = dot(lightDir, lightDir);
    float falloff = 1.0 / (sqrDist + 1.0);
    const float invRadius = 1.0 / max(lightRadius, 1e-4);

    falloff *= 16.0;
    falloff *= square(saturate(1.0 - square(sqrDist * square(invRadius))));
    return falloff;
}

static inline float getSpotEffect(float3 lightSpotDir, float lightInnerConeAngle, float lightOuterConeAngle, float3 lightDirNorm)
{
    const float cosAngle = dot(lightDirNorm, lightSpotDir);
    return smoothstep(lightOuterConeAngle, lightInnerConeAngle, cosAngle);
}

// Cascade split distances mark the far boundary of each cascade slice.
// step(distances, depth) produces 1.0 for each cascade whose far boundary is <= depth,
// and dot(comparisons, vec4(1)) counts how many cascades the fragment is past.
static inline int getShadowCascadeIndex(float4 distances, int count, float depth) {
    const float4 comparisons = step(distances, float4(depth));
    return min(int(dot(comparisons, float4(1.0))), count - 1);
}

// Upstream ditherShadowCascadeIndex (shadowCascades.js): over the stretch of a
// cascade from blendFactor x its end distance to its end, move a growing,
// pseudo-randomly dithered share of the fragments to the NEXT cascade, so the
// seam between two shadow resolutions dissolves instead of drawing a line.
// fragCoord is the pixel position; the hash is upstream's.
static inline int ditherShadowCascadeIndex(int cascadeIndex, float4 distances, int count,
                                           float blendFactor, float depth, float2 fragCoord) {
    if (cascadeIndex < count - 1) {
        const float currentRangeEnd = distances[cascadeIndex];
        const float transitionStart = blendFactor * currentRangeEnd;
        if (depth > transitionStart) {
            const float transitionFactor = smoothstep(transitionStart, currentRangeEnd, depth);
            const float dither = fract(sin(dot(fragCoord, float2(12.9898, 78.233))) * 43758.5453);
            if (dither < transitionFactor) {
                cascadeIndex += 1;
            }
        }
    }
    return cascadeIndex;
}

// Uses 4 hardware comparison samples with bilinear interpolation to cover a 3×3 texel region.
// Each sample_compare with filter::linear performs a 2×2 PCF automatically; carefully chosen
// offsets and weights combine four such lookups into a full 3×3 kernel (4 taps, not 9).
//_getShadowPCF3x3().
