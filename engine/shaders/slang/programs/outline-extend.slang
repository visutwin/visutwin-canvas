// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 07.10.2026
//
// The outline renderer's separable extend: dilates the silhouette with a 5-tap max and
// marks colour discontinuities in alpha. Run horizontally (h) then vertically (v); the
// offset direction and the source-alpha multiplier are compiled into each variant, and the
// half-texel step comes from the texture size.
//
// @variant h: DIR_X=1.0 DIR_Y=0.0 SRC_MULT=0.0
// @variant v: DIR_X=0.0 DIR_Y=1.0 SRC_MULT=1.0
import vtquad;
#include "bindings.slang"

VT_DECLARE_QUAD_INPUT0(source)

[shader("vertex")]
QuadVarying outlineExtendVertex(QuadVertexIn input)
{
    return quadPassThrough(input);
}

[shader("fragment")]
float4 outlineExtendFragment(QuadVarying input) : SV_Target0
{
    uint width;
    uint height;
    source.GetDimensions(width, height);
    const float2 texel = float2(1.0 / float(width), 1.0 / float(height));
    const float2 offset = float2(DIR_X, DIR_Y) * texel * 0.5;
    const float srcMultiplier = SRC_MULT;

    float4 texelValue = source.Sample(input.uv);
    const float4 firstTexel = texelValue;
    float diff = texelValue.a * srcMultiplier;

    float4 pixel = source.Sample(input.uv + offset * -2.0);
    texelValue = max(texelValue, pixel);
    diff = max(diff, length(firstTexel.rgb - pixel.rgb));

    pixel = source.Sample(input.uv + offset * -1.0);
    texelValue = max(texelValue, pixel);
    diff = max(diff, length(firstTexel.rgb - pixel.rgb));

    pixel = source.Sample(input.uv + offset * 1.0);
    texelValue = max(texelValue, pixel);
    diff = max(diff, length(firstTexel.rgb - pixel.rgb));

    pixel = source.Sample(input.uv + offset * 2.0);
    texelValue = max(texelValue, pixel);
    diff = max(diff, length(firstTexel.rgb - pixel.rgb));

    return float4(texelValue.rgb, min(diff, 1.0));
}
