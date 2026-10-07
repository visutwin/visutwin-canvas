// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 07.10.2026
//
// The volumetric fog's depth-aware upsample onto the scene: the four nearest texel centres
// of the low-resolution fog texture, each weighted bilinearly and by how closely its depth
// matches this pixel's, so fog does not leak across geometry edges. rgb = in-scattered
// light, a = transmittance; the blend state resolves it to scene * transmittance +
// inscatter. Inputs: 0 scene depth (point sampled), 1 fog.
import vtquad;
import vtfog;
#include "bindings.slang"

struct FogCombineUniforms
{
    float4 textureSize;    // xy = fog resolution, zw = 1/resolution
    float4 cameraParams;   // x = near, y = far
};
VT_DECLARE_QUAD_UNIFORMS(FogCombineUniforms, u)
VT_DECLARE_QUAD_DEPTH_INPUT0(depthTexture)
VT_DECLARE_QUAD_INPUT1(fogTexture)

[shader("vertex")]
QuadVarying fogCombineVertex(QuadVertexIn input)
{
    return quadPassThrough(input);
}

[shader("fragment")]
float4 fogCombineFragment(QuadVarying input) : SV_Target0
{
    const float2 uv = clamp(input.uv, float2(0.0, 0.0), float2(1.0, 1.0));
    const float cameraNear = u.cameraParams.x;
    const float cameraFar = u.cameraParams.y;

    const float depth = fogLinearDepth(depthTexture.Sample(uv), cameraNear, cameraFar);

    const float2 texel = uv * u.textureSize.xy - 0.5;
    const float2 base = (floor(texel) + 0.5) * u.textureSize.zw;
    const float2 f = frac(texel);

    float2 uvs[4];
    uvs[0] = base;
    uvs[1] = base + float2(u.textureSize.z, 0.0);
    uvs[2] = base + float2(0.0, u.textureSize.w);
    uvs[3] = base + u.textureSize.zw;

    const float4 bilinear = float4(
        (1.0 - f.x) * (1.0 - f.y),
        f.x * (1.0 - f.y),
        (1.0 - f.x) * f.y,
        f.x * f.y);

    float4 sum = float4(0.0, 0.0, 0.0, 0.0);
    float sumWeight = 0.0;
    for (int i = 0; i < 4; ++i) {
        const float sampleDepth = fogLinearDepth(depthTexture.Sample(uvs[i]), cameraNear, cameraFar);
        const float w = bilinear[i] / (1.0 + 16.0 * abs(sampleDepth - depth) / max(depth, 0.001));
        sum += fogTexture.Sample(uvs[i]) * w;
        sumWeight += w;
    }
    return sum / max(sumWeight, 0.0001);
}
