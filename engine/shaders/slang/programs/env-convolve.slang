// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 07.10.2026
//
// The importance-sampled convolution of an environment (input 0: an equirect, or a cube
// sampled along L unchanged as the reproject bake samples it) into an equirect rect of the
// atlas: Lambert for the ambient, GGX for the reflection levels. The sample table
// (tangent x, y, z and the source mip per entry) arrives on input 1 as an RGBA32F data
// texture, read with an unfiltered texel FETCH: Apple GPUs cannot linearly filter a 32-bit
// float format, and the table is data, not an image. The caller marks an unusable sample
// with a non-positive z.
//
// @variant 2d: SRC_2D=1
// @variant cube: SRC_CUBE=1
import vtquad;
import vtenv;
#include "bindings.slang"

struct ConvolveUniforms
{
    float4 uvMod;
    uint encodeRgbp;
    uint decodeSrgb;
    uint numSamples;
    uint weightByNoL;
};
VT_DECLARE_QUAD_UNIFORMS(ConvolveUniforms, u)
#if defined(SRC_CUBE)
VT_DECLARE_QUAD_CUBE_INPUT0(sourceTexture)
#else
VT_DECLARE_QUAD_INPUT0(sourceTexture)
#endif
VT_DECLARE_QUAD_TEXEL_INPUT1(sampleTable)

[shader("vertex")]
QuadVarying convolveVertex(QuadVertexIn input)
{
    return quadPassThrough(input);
}

[shader("fragment")]
float4 convolveFragment(QuadVarying input) : SV_Target0
{
    const float2 uv = input.uv * u.uvMod.xy + u.uvMod.zw;
    const float3 N = normalize(uvToDirEquirect(uv));
    const float3 up = (abs(N.y) > 0.999) ? float3(1.0, 0.0, 0.0) : float3(0.0, 1.0, 0.0);
    const float3 T = normalize(cross(up, N));
    const float3 B = cross(N, T);
    float3 sum = float3(0.0, 0.0, 0.0);
    float weight = 0.0;
    for (uint i = 0u; i < u.numSamples; ++i) {
        const float4 s = sampleTable.Load(int3(int(i), 0, 0));
        if (s.z <= 0.0) {
            continue;
        }
        const float3 L = T * s.x + B * s.y + N * s.z;
#if defined(SRC_CUBE)
        float3 color = sourceTexture.SampleLevel(L, s.w).rgb;
#else
        float3 color = sourceTexture.SampleLevel(dirToUvEquirect(L), s.w).rgb;
#endif
        color = decodeIfSrgb(color, u.decodeSrgb != 0u);
        const float w = (u.weightByNoL != 0u) ? s.z : 1.0;
        sum += color * w;
        weight += w;
    }
    sum /= max(weight, 1e-4);
    return (u.encodeRgbp != 0u) ? packRgbp(sum) : float4(sum, 1.0);
}
