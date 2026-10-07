// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 07.10.2026
//
// The bloom upsample: a 3x3 tent filter over the source mip, as in LearnOpenGL's
// physically based bloom. Combined with additive blending during upsampling it spreads
// each mip's contribution across a wider area and accumulates every mip into the first,
// a much smoother and brighter glow than a single bilinear read. The uv is clamped to
// [0, 1] on both backends; the Vulkan backend renders through a negative-height
// viewport, so the uv needs no flip.
//
// The first program of the single-source shader tree: one Slang source compiled to MSL
// for Metal and SPIR-V for Vulkan by the build (slang_shader_bundle.h), and by the
// runtime for an override (VISUTWIN_SLANG_RUNTIME).
import vtquad;
#include "bindings.slang"

VT_DECLARE_QUAD_INPUT0(sourceTexture)

[shader("vertex")]
QuadVarying upsampleVertex(QuadVertexIn input)
{
    return quadPassThrough(input);
}

[shader("fragment")]
float4 upsampleFragment(QuadVarying input) : SV_Target0
{
    uint width;
    uint height;
    sourceTexture.GetDimensions(width, height);
    const float2 texel = float2(1.0 / float(width), 1.0 / float(height));
    const float2 uv = clamp(input.uv, float2(0.0, 0.0), float2(1.0, 1.0));
    const float x = texel.x;
    const float y = texel.y;

    const float3 a = sourceTexture.Sample(uv + float2(-x,  y)).rgb;
    const float3 b = sourceTexture.Sample(uv + float2( 0,  y)).rgb;
    const float3 c = sourceTexture.Sample(uv + float2( x,  y)).rgb;
    const float3 d = sourceTexture.Sample(uv + float2(-x,  0)).rgb;
    const float3 e = sourceTexture.Sample(uv                 ).rgb;
    const float3 f = sourceTexture.Sample(uv + float2( x,  0)).rgb;
    const float3 g = sourceTexture.Sample(uv + float2(-x, -y)).rgb;
    const float3 h = sourceTexture.Sample(uv + float2( 0, -y)).rgb;
    const float3 i = sourceTexture.Sample(uv + float2( x, -y)).rgb;

    float3 value = e * 0.25;
    value += (b + d + f + h) * 0.125;
    value += (a + c + g + i) * 0.0625;
    return float4(value, 1.0);
}
