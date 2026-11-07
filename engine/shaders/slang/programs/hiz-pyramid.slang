// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 07.11.2026
//
// One level of a hierarchical depth (Hi-Z) pyramid, an RG32F mip chain holding in each texel
// the nearest (r) and the farthest (g) depth of the texels it covers in the level above
// (HiZPyramidGenerator). Level 0 is half the depth map's size, reduced from the depth itself;
// each further level is half the one above. A destination texel covers the 2x2 source texels
// at twice its coordinate and, in the last column or row of an odd-sized source, the one
// beyond them too, so every source texel is covered and a level's values are exact (texels
// are read by coordinate, never filtered).
//
// The variants: `depth` reduces a depth texture (point-sampled at its texel centres, the read
// a depth input allows on both backends), `float` an R32F texture holding depth in red, `reduce`
// a level of the pyramid, and `copy` carries a level rendered into the scratch texture back
// into the pyramid's chain, since a texture cannot be sampled while one of its levels is the
// attachment. The quad's uv has v = 0 at the TOP, as every texture's row 0.
//
// @variant depth: HIZ_DEPTH=1
// @variant float: HIZ_FLOAT=1
// @variant reduce: HIZ_REDUCE=1
// @variant copy: HIZ_COPY=1
import vtquad;
#include "bindings.slang"

struct HiZUniforms
{
    int4 source;   // xy: the source's size, z: the source level (reduce), w: unused
    int4 target;   // xy: the size of the level written
};
VT_DECLARE_QUAD_UNIFORMS(HiZUniforms, u)
#if defined(HIZ_DEPTH)
VT_DECLARE_QUAD_DEPTH_INPUT0(sourceDepth)
#else
VT_DECLARE_QUAD_TEXEL_INPUT0(sourceTexture)
#endif

[shader("vertex")]
QuadVarying hiZVertex(QuadVertexIn input)
{
    return quadPassThrough(input);
}

// The nearest and farthest depth of one source texel.
float2 sourceTexel(int2 texel)
{
#if defined(HIZ_DEPTH)
    const float depth = sourceDepth.SampleLevel((float2(texel) + 0.5) / float2(u.source.xy), 0.0);
    return float2(depth, depth);
#elif defined(HIZ_FLOAT)
    const float depth = sourceTexture.Load(int3(texel, 0)).r;
    return float2(depth, depth);
#else
    return sourceTexture.Load(int3(texel, u.source.z)).rg;
#endif
}

[shader("fragment")]
float4 hiZFragment(QuadVarying input) : SV_Target0
{
    const int2 target = u.target.xy;
    const int2 texel = min(int2(input.uv * float2(target)), target - int2(1, 1));
#if defined(HIZ_COPY)
    return float4(sourceTexture.Load(int3(texel, 0)).rg, 0.0, 1.0);
#else
    const int2 size = u.source.xy;
    const int2 first = texel * 2;
    // the 2x2 block, and the source's last column or row when this is the last texel and
    // the source is odd (a floor-sized level leaves it to nobody otherwise)
    int2 last = min(first + int2(1, 1), size - int2(1, 1));
    if (texel.x == target.x - 1) {
        last.x = size.x - 1;
    }
    if (texel.y == target.y - 1) {
        last.y = size.y - 1;
    }
    float2 range = float2(1.0, 0.0);
    for (int y = first.y; y <= last.y; y++) {
        for (int x = first.x; x <= last.x; x++) {
            const float2 value = sourceTexel(int2(x, y));
            range = float2(min(range.x, value.x), max(range.y, value.y));
        }
    }
    return float4(range, 0.0, 1.0);
#endif
}
