// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2026
//
// One mip level of one face of a gamma-encoded cube map, as the LINEAR average of the source
// cube's level-0 texels it covers (input 0): each is decoded, the block averaged, the result
// encoded again. A blit's mip chain averages the encoded values, which darkens a bright
// feature against a dark surround at every level; an sRGB-format texture averages in linear
// space, and so does this. Taps land on the source texels' centres (cubeFaceUvToDir is the
// hardware lookup's face mapping), where the bilinear sample is the texel itself.
import vtquad;
import vtenv;
#include "bindings.slang"

struct MipUniforms
{
    uint face;         // 0..5
    uint level;        // the target level; each target texel covers 2^level source texels a side
    uint sourceSize;   // the source's level-0 face size
    uint pad0;
};
VT_DECLARE_QUAD_UNIFORMS(MipUniforms, u)
VT_DECLARE_QUAD_CUBE_INPUT0(sourceCube)

[shader("vertex")]
QuadVarying cubeMipsVertex(QuadVertexIn input)
{
    return quadPassThrough(input);
}

[shader("fragment")]
float4 cubeMipsFragment(QuadVarying input) : SV_Target0
{
    const uint block = 1u << u.level;
    const uint targetSize = max(u.sourceSize >> u.level, 1u);
    const uint2 texel = min(uint2(clamp(input.uv, 0.0, 1.0) * float(targetSize)), uint2(targetSize - 1u));
    const float invSize = 1.0 / float(u.sourceSize);

    float3 sum = float3(0.0, 0.0, 0.0);
    float alpha = 0.0;
    for (uint y = 0u; y < block; ++y) {
        for (uint x = 0u; x < block; ++x) {
            const float2 uv = (float2(texel * block + uint2(x, y)) + 0.5) * invSize;
            const float4 s = sourceCube.SampleLevel(cubeFaceUvToDir(u.face, uv), 0.0);
            sum += pow(max(s.rgb, float3(0.0, 0.0, 0.0)), float3(2.2, 2.2, 2.2));
            alpha += s.a;
        }
    }
    const float count = float(block * block);
    return float4(pow(sum / count, float3(1.0 / 2.2, 1.0 / 2.2, 1.0 / 2.2)), alpha / count);
}
