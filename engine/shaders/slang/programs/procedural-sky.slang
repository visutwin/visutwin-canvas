// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 12.10.2026
//
// The visible procedural sky, drawn on the scene's sky mesh (Scene::setSkyMaterial) in
// place of the environment: the day model, the night model, or their crossfade, per pixel.
// The mesh follows the camera, so the pre-transform position is the view ray; it is drawn
// a hair inside the far plane, as the engine's own infinite sky is. The output follows the
// forward pass's: linear HDR for a camera frame, otherwise exposed, tone mapped and
// gamma-encoded.
import vtdraw;
import vtproceduralsky;
#include "bindings.slang"
#define VT_TONEMAP_OPERATORS_ONLY
#include "forward/common-tonemap.slang"

VT_DECLARE_MATERIAL_UNIFORMS(ProceduralSkyData, sky)

struct VertexIn
{
    [[vk::location(0)]] float3 position : POSITION;
};

struct Varyings
{
    float4 position : SV_Position;
    float3 viewDir : TEXCOORD0;
};

[shader("vertex")]
Varyings proceduralSkyVertex(VertexIn input)
{
    Varyings output;
    float4 clip = mul(float4(input.position, 1.0), mul(drawModel(), drawViewProjection()));
    clip.z = clip.w - 1.0e-7;
    output.position = clip;
    output.viewDir = input.position;
    return output;
}

[shader("fragment")]
float4 proceduralSkyFragment(Varyings input) : SV_Target0
{
    // World to sky space (X negated), the space the environment atlas is looked up in.
    float3 dir = normalize(input.viewDir);
    dir.x = -dir.x;

    // The blend is the same for every pixel, so only one model runs outside the crossfade.
    const float nightBlend = sky.night.x;
    float3 radiance;
    if (nightBlend <= 0.0) {
        radiance = proceduralDaySky(sky, dir);
    } else if (nightBlend >= 1.0) {
        radiance = proceduralNightSky(sky, dir);
    } else {
        radiance = lerp(proceduralDaySky(sky, dir), proceduralNightSky(sky, dir), nightBlend);
    }
    radiance *= sky.output.x;

    if ((uint(sky.output.w + 0.5) & SKY_FLAG_LINEAR_OUTPUT) != 0u) {
        return float4(max(radiance, float3(0.0)), 1.0);
    }
    const float3 mapped = toneMapExposed(radiance, sky.output.y, int(sky.output.z + 0.5));
    return float4(pow(max(mapped, float3(0.0)), float3(1.0 / 2.2)), 1.0);
}
