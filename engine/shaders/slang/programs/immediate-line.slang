// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2026
//
// Immediate lines (Immediate, WireRenderer): world-space segments with a colour per vertex, in
// the 28-byte point layout (position at attribute 0, colour at 5). The colours are gamma
// encoded; a linear HDR target (a camera frame's scene) takes them decoded, a gamma target as
// they are. The material block says which, per draw.
import vtdraw;
#include "bindings.slang"

struct ImmediateLineData
{
    float4 output;   // x: 1 when the target is linear HDR
};
VT_DECLARE_MATERIAL_UNIFORMS(ImmediateLineData, lineData)

struct VertexIn
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(5)]] float4 color : COLOR;
};

struct Varyings
{
    float4 position : SV_Position;
    float4 color : COLOR0;
};

[shader("vertex")]
Varyings immediateLineVertex(VertexIn input)
{
    Varyings output;
    float4 clip = mul(float4(input.position, 1.0), mul(drawModel(), drawViewProjection()));
    clip.z = 0.5 * (clip.z + clip.w);
    output.position = clip;
    output.color = input.color;
    return output;
}

[shader("fragment")]
float4 immediateLineFragment(Varyings input) : SV_Target0
{
    const float3 rgb = lineData.output.x > 0.5 ? pow(max(input.color.rgb, float3(0.0)), float3(2.2)) : input.color.rgb;
    return float4(rgb, input.color.a);
}
