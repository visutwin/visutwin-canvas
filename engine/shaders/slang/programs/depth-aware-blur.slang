// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 07.10.2026
//
// A bilateral blur of the SSAO term that respects depth discontinuities, so ambient
// occlusion does not halo across silhouettes. Separable: the direction is a uniform, and
// the pass runs once horizontally and once vertically: a 9-tap box (filterSize 4) whose
// centre is pre-seeded outside the bilateral weight and taken again by the loop. Depth is
// POINT sampled: these passes reconstruct view distances, and a bilinear tap straddling a
// silhouette returns a depth belonging to neither surface.
import vtquad;
#include "bindings.slang"

struct BlurUniforms
{
    float4 invResAndDir;   // xy = 1/resolution, zw = blur direction
    float4 params;         // x = filterSize, y = cameraNear, z = cameraFar
};
VT_DECLARE_QUAD_UNIFORMS(BlurUniforms, u)
VT_DECLARE_QUAD_INPUT0(sourceTexture)
VT_DECLARE_QUAD_DEPTH_INPUT1(depthTexture)

float getLinearDepth(float rawDepth, float cameraNear, float cameraFar)
{
    return (cameraNear * cameraFar) / (cameraFar - rawDepth * (cameraFar - cameraNear));
}

float bilateralWeight(float depth, float sampleDepth)
{
    const float diff = (sampleDepth - depth);
    return max(0.0, 1.0 - diff * diff);
}

[shader("vertex")]
QuadVarying blurVertex(QuadVertexIn input)
{
    return quadPassThrough(input);
}

[shader("fragment")]
float4 blurFragment(QuadVarying input) : SV_Target0
{
    const float2 sourceInvResolution = u.invResAndDir.xy;
    const float2 direction = u.invResAndDir.zw;
    const int filterSize = int(u.params.x);
    const float cameraNear = u.params.y;
    const float cameraFar = u.params.z;

    const float2 uv = clamp(input.uv, float2(0.0, 0.0), float2(1.0, 1.0));

    const float depth = getLinearDepth(depthTexture.Sample(uv), cameraNear, cameraFar);
    float totalWeight = 1.0;
    const float color = sourceTexture.Sample(uv).r;
    float sum = color * totalWeight;

    // Uniform weights over 2 x filterSize + 1 taps, the centre taken again inside the loop
    // (its bilateral weight is 1), so it counts twice.
    for (int i = -filterSize; i <= filterSize; i++) {
        const float2 position = uv + direction * float(i) * sourceInvResolution;
        const float tapColor = sourceTexture.Sample(position).r;
        const float textureDepth = getLinearDepth(depthTexture.Sample(position), cameraNear, cameraFar);
        const float bilateral = bilateralWeight(depth, textureDepth);
        sum += tapColor * bilateral;
        totalWeight += bilateral;
    }

    return float4(sum / totalWeight, 0.0, 0.0, 1.0);
}
