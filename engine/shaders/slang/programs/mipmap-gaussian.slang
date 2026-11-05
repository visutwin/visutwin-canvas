// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 05.11.2026
//
// One mip level of a 2D texture from the level above it, for the Gaussian mipmap filter
// (MIPMAPFILTER_GAUSSIAN, GaussianMipmapGenerator): 13 bilinear taps of the source level,
// weighted to approximate a Gaussian blur over about 6x6 of its texels, so the small levels a
// rough reflection or refraction samples stay smooth where a box chain shows blocks that flicker
// as the scene moves. The `copy` variant samples the source level once: it carries a level
// rendered into the scratch texture back into the texture's own chain, since a texture cannot
// be sampled while one of its levels is the attachment.
//
// @variant gaussian: MIPMAP_GAUSSIAN=1
// @variant copy: MIPMAP_COPY=1
import vtquad;
#include "bindings.slang"

struct MipmapUniforms
{
    float4 source;   // xy: the source level's texel size, z: the source level, w: unused
};
VT_DECLARE_QUAD_UNIFORMS(MipmapUniforms, u)
VT_DECLARE_QUAD_INPUT0(sourceTexture)

[shader("vertex")]
QuadVarying mipmapVertex(QuadVertexIn input)
{
    return quadPassThrough(input);
}

// A tap of the source level, clamped to its edge texels: the taps reach two texels past the
// level's edge, and the texture's own sampler (which Vulkan reads a quad input through) may wrap.
float4 tap(float2 uv, float x, float y)
{
    const float2 texel = u.source.xy;
    const float2 at = clamp(uv + float2(x, y) * texel, 0.5 * texel, 1.0 - 0.5 * texel);
    return sourceTexture.SampleLevel(at, u.source.z);
}

[shader("fragment")]
float4 mipmapFragment(QuadVarying input) : SV_Target0
{
    const float2 uv = input.uv;
#if defined(MIPMAP_COPY)
    return tap(uv, 0.0, 0.0);
#else
    return tap(uv, 0.0, 0.0) * 0.125 +
        (tap(uv, -2.0, 2.0) + tap(uv, 2.0, 2.0) + tap(uv, -2.0, -2.0) + tap(uv, 2.0, -2.0)) * 0.03125 +
        (tap(uv, 0.0, 2.0) + tap(uv, -2.0, 0.0) + tap(uv, 2.0, 0.0) + tap(uv, 0.0, -2.0)) * 0.0625 +
        (tap(uv, -1.0, 1.0) + tap(uv, 1.0, 1.0) + tap(uv, -1.0, -1.0) + tap(uv, 1.0, -1.0)) * 0.125;
#endif
}
