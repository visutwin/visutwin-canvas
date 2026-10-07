// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 07.10.2026
//
// The separable Gaussian blur of a VSM shadow map's moments. Taps are clamped to the
// fragment's cascade tile so the kernel cannot mix moments across the seams of a
// multi-cascade (quadrant) atlas. Alpha is written 1, the "rendered" flag, so a blurred
// texel is not mistaken for a cleared, synthetically lit one.
import vtquad;
#include "bindings.slang"

#define MAX_TAPS 25

struct VsmBlurUniforms
{
    float4 invResolutionAndDirection;   // xy = 1/resolution, zw = blur direction
    float4 params;                      // x = filterSize, y = cascade tile size
};
VT_DECLARE_QUAD_UNIFORMS(VsmBlurUniforms, uniforms)
VT_DECLARE_QUAD_INPUT0(sourceTexture)

[shader("vertex")]
QuadVarying vsmBlurVertex(QuadVertexIn input)
{
    return quadPassThrough(input);
}

[shader("fragment")]
float4 vsmBlurFragment(QuadVarying input) : SV_Target0
{
    const float2 invResolution = uniforms.invResolutionAndDirection.xy;
    const float2 direction = uniforms.invResolutionAndDirection.zw;
    const int filterSize = clamp(int(uniforms.params.x), 1, MAX_TAPS / 2);
    const float sigma = max(float(filterSize) / 3.0, 1.0);
    const float invSigma2 = 1.0 / (2.0 * sigma * sigma);

    const float tile = clamp(uniforms.params.y <= 0.0 ? 1.0 : uniforms.params.y, 0.0, 1.0);
    const float2 halfTexel = 0.5 * invResolution;
    const float2 tileMin = (tile < 1.0) ? floor(input.uv / tile) * tile : float2(0.0, 0.0);
    const float2 clampMin = tileMin + halfTexel;
    const float2 clampMax = tileMin + tile - halfTexel;

    float3 moments = float3(0.0, 0.0, 0.0);
    float totalWeight = 0.0;
    for (int i = -filterSize; i <= filterSize; ++i) {
        const float w = exp(-float(i * i) * invSigma2);
        const float2 offset = direction * float(i) * invResolution;
        moments += sourceTexture.Sample(clamp(input.uv + offset, clampMin, clampMax)).xyz * w;
        totalWeight += w;
    }
    moments /= max(totalWeight, 1e-6);
    return float4(moments, 1.0);
}
