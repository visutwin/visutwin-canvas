// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 07.10.2026
//
// The gizmos' unlit material: a flat colour, alpha-blended, with a depth it can override (the
// material block's depth.x, or negative for the interpolated one). The shape is kept inside
// the depth range rather than clipped, then the clip z is mapped from GL's [-1, 1] to [0, 1]
// as every engine vertex stage does. Feature word 0 bit 0 (an icon glyph) scales the alpha by
// the coverage of the base colour map, its filtered edges sharpened to a pixel wide as
// geometry draws them. The colour is gamma encoded; a linear HDR target takes it decoded.
import vtdraw;
#include "bindings.slang"

struct GizmoData
{
    float4 color;
    float4 depth;   // x: the depth to write, or negative for the interpolated one; y: 1 for a linear target
};
VT_DECLARE_MATERIAL_UNIFORMS(GizmoData, gizmo)

VT_DECLARE_baseColorMap

static const uint kGizmoTexture = 1u << 0u;

struct VertexIn
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(2)]] float2 uv0 : TEXCOORD0;
};

struct Varyings
{
    float4 position : SV_Position;
    float2 uv;
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
    output.uv = (vtFeatureMask0 & kGizmoTexture) != 0u ? input.uv0 : float2(0.0);
    return output;
}

[shader("fragment")]
FragmentOut gizmoFragment(Varyings input)
{
    float4 color = gizmo.color;
    if ((vtFeatureMask0 & kGizmoTexture) != 0u) {
        const float coverage = baseColorMap.Sample(input.uv).a;
        color.a *= saturate((coverage - 0.5) / max(fwidth(coverage), 1e-4) + 0.5);
    }
    if (color.a < 1.0 / 255.0) {
        discard;
    }
    FragmentOut output;
    const float3 rgb = gizmo.depth.y > 0.5 ? pow(max(color.rgb, float3(0.0)), float3(2.2)) : color.rgb;
    output.color = float4(rgb, color.a);
    output.depth = gizmo.depth.x >= 0.0 ? gizmo.depth.x : input.position.z;
    return output;
}
