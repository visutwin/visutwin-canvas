// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 07.10.2026
//
// The transform gizmo's unlit material: a flat colour, alpha-blended, with a depth it can
// override (the material block's depth.x, or negative for the interpolated one). The shape
// is kept inside the depth range rather than clipped, then the clip z is mapped from GL's
// [-1, 1] to [0, 1] as every engine vertex stage does.
import vtdraw;
#include "bindings.slang"

struct GizmoData
{
    float4 color;
    float4 depth;   // x: the depth to write, or negative for the interpolated one
};
VT_DECLARE_MATERIAL_UNIFORMS(GizmoData, gizmo)

struct VertexIn
{
    [[vk::location(0)]] float3 position : POSITION;
};

struct Varyings
{
    float4 position : SV_Position;
};

struct FragmentOut
{
    float4 color : SV_Target0;
    float depth : SV_Depth;
};

[shader("vertex")]
Varyings gizmoVertex(VertexIn input)
{
    Varyings output;
    float4 clip = mul(float4(input.position, 1.0), mul(drawModel(), drawViewProjection()));
    clip.z = clamp(clip.z, -abs(clip.w), abs(clip.w));
    clip.z = 0.5 * (clip.z + clip.w);
    output.position = clip;
    return output;
}

[shader("fragment")]
FragmentOut gizmoFragment(Varyings input)
{
    if (gizmo.color.a < 1.0 / 255.0) {
        discard;
    }
    FragmentOut output;
    output.color = gizmo.color;
    output.depth = gizmo.depth.x >= 0.0 ? gizmo.depth.x : input.position.z;
    return output;
}
