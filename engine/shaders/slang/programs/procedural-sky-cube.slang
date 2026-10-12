// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 12.10.2026
//
// One face (extra.y) of the procedural sky's lighting cube: the DAY model only, linear,
// each channel capped at 64 — what an RGBM-encoded bake can hold — so the sun disk adds a
// bounded highlight to the reflections instead of a 6000-count spike. The face mapping is
// the hardware lookup's, so the cube holds sky(d) along every direction d, in sky space.
import vtquad;
import vtenv;
import vtproceduralsky;
#include "bindings.slang"

VT_DECLARE_QUAD_UNIFORMS(ProceduralSkyData, sky)

[shader("vertex")]
QuadVarying proceduralSkyCubeVertex(QuadVertexIn input)
{
    return quadPassThrough(input);
}

[shader("fragment")]
float4 proceduralSkyCubeFragment(QuadVarying input) : SV_Target0
{
    const float2 uv = clamp(input.uv, float2(0.0, 0.0), float2(1.0, 1.0));
    const float3 dir = cubeFaceUvToDir(uint(sky.extra.y + 0.5), uv);
    return float4(min(proceduralDaySky(sky, dir), float3(64.0)), 1.0);
}
