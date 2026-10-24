// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 24.10.2026
//
// The gizmo line batch (framework/gizmo/meshLineBatch.h): one unit cylinder instance per line,
// unlit, alpha blended. An instance is the 64-byte instancing record: its first three vectors
// are the rows of the line's world transform (the cylinder placed along the line, already
// pulled toward the camera), and the fourth packs the visible and the occluded colour, two
// 8-bit channels a float (high * 256 + low, exact in a float). The occluded pass (feature word
// 0 bit 0) takes the occluded colour. The colours are gamma encoded; a linear HDR target takes
// them decoded.
//
// The vertex entry is named ...InstancedVertex, so a Vulkan pipeline takes it as the instanced
// stage.
import vtdraw;
#include "bindings.slang"

struct GizmoLineData
{
    float4 output;  // x: 1 for a linear HDR target
};
VT_DECLARE_MATERIAL_UNIFORMS(GizmoLineData, gizmoLine)

static const uint kGizmoLineOccluded = 1u << 0u;

// The instance vectors, at the instancing attributes of each backend.
#if defined(VT_TARGET_METAL)
#define VT_GIZMO_LINE_INSTANCE0 6
#define VT_GIZMO_LINE_INSTANCE1 7
#define VT_GIZMO_LINE_INSTANCE2 8
#define VT_GIZMO_LINE_INSTANCE3 9
#else
#define VT_GIZMO_LINE_INSTANCE0 5
#define VT_GIZMO_LINE_INSTANCE1 6
#define VT_GIZMO_LINE_INSTANCE2 7
#define VT_GIZMO_LINE_INSTANCE3 8
#endif

struct VertexIn
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(VT_GIZMO_LINE_INSTANCE0)]] float4 row0 : INSTANCE0;
    [[vk::location(VT_GIZMO_LINE_INSTANCE1)]] float4 row1 : INSTANCE1;
    [[vk::location(VT_GIZMO_LINE_INSTANCE2)]] float4 row2 : INSTANCE2;
    [[vk::location(VT_GIZMO_LINE_INSTANCE3)]] float4 colors : INSTANCE3;
};

struct Varyings
{
    float4 position : SV_Position;
    nointerpolation float4 color;
};

// Two 8-bit channels packed as high * 256 + low.
float2 unpackChannels(float packed)
{
    const float high = floor(packed / 256.0);
    return float2(high, packed - high * 256.0) / 255.0;
}

[shader("vertex")]
Varyings gizmoLineInstancedVertex(VertexIn input)
{
    const float4 p = float4(input.position, 1.0);
    const float3 world = float3(dot(input.row0, p), dot(input.row1, p), dot(input.row2, p));
    float4 clip = mul(float4(world, 1.0), drawViewProjection());
    clip.z = clamp(clip.z, -abs(clip.w), abs(clip.w));
    clip.z = 0.5 * (clip.z + clip.w);

    Varyings output;
    output.position = clip;
    const bool occluded = (vtFeatureMask0 & kGizmoLineOccluded) != 0u;
    const float2 rg = unpackChannels(occluded ? input.colors.z : input.colors.x);
    const float2 ba = unpackChannels(occluded ? input.colors.w : input.colors.y);
    output.color = float4(rg, ba);
    return output;
}

[shader("fragment")]
float4 gizmoLineFragment(Varyings input) : SV_Target0
{
    const float4 color = input.color;
    if (color.a < 1.0 / 255.0) {
        discard;
    }
    const float3 rgb = gizmoLine.output.x > 0.5 ? pow(max(color.rgb, float3(0.0)), float3(2.2)) : color.rgb;
    return float4(rgb, color.a);
}
