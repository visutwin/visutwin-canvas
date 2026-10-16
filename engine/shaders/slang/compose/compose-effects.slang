// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2026
//
// The compose pass's hooks for the camera frame's compose effects, with no effect: each slot
// returns its colour unchanged, and no effect provides a debug view. programs/compose.slang
// includes this file; with effects, RenderPassCompose serves a generated one in its place
// (buildComposeEffects in scene/graphics/composeEffects.h), defining the same functions.

float4 composeSlotScene(float4 color, float2 uv)
{
    return color;
}

float3 composeSlotHdr(float3 color, float2 uv)
{
    return color;
}

float3 composeSlotLdr(float3 color, float2 uv)
{
    return color;
}

float3 composeSlotOutput(float3 color, float2 uv)
{
    return color;
}

float3 composeEffectDebug(uint view, float2 uv, float3 color)
{
    return float3(0.0, 0.0, 0.0);
}
