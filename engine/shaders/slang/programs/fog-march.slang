// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 07.10.2026
//
// The volumetric fog march: each low-resolution pixel's view ray is marched through
// exponential height fog up to the scene depth (or the maximum distance), accumulating
// sun in-scattering shadowed by the directional cascades and attenuated by Beer-Lambert.
// rgb = in-scattered light, a = transmittance. Inputs: 0 scene depth (point sampled),
// 1 the cascaded shadow map. The quad's v runs DOWN the screen, so NDC y is 1 - 2v. The
// depth buffer stores distance along the view axis, so the ray length divides by the ray's
// projection on the forward vector. The shadow tap differs per backend (VT_SHADOW_COMPARE).
import vtquad;
import vtfog;
#include "bindings.slang"

struct FogUniforms
{
    float4x4 invView;
    float4x4 shadowMatrixPalette[4];
    float4 cameraPosition;
    float4 cameraForward;
    float4 projScale;
    float4 tint;
    float4 lightColor;
    float4 lightDirection;
    float4 ambient;
    float4 fogParams;              // x density, y height base, z height falloff, w max distance
    float4 scatterParams;          // x anisotropy, y steps, z noise offset, w shadow intensity
    float4 shadowCascadeDistances;
    float4 shadowParams;           // x cascade count, y bias, z has shadows, w shadow distance
    float4 cameraParams;           // x near, y far, z extinction
};
VT_DECLARE_QUAD_UNIFORMS(FogUniforms, u)
VT_DECLARE_QUAD_DEPTH_INPUT0(depthTexture)
VT_DECLARE_QUAD_SHADOW_INPUT1(shadowTexture)
VT_DECLARE_QUAD_COMPARE_SAMPLER

// The cascade is the first whose split distance exceeds the view depth, as the forward
// pass picks it.
float sampleFogShadow(float3 worldPos, float viewDepth)
{
    if (viewDepth >= u.shadowParams.w) {
        return 1.0;
    }
    const float4 comparisons = step(u.shadowCascadeDistances, float4(viewDepth, viewDepth, viewDepth, viewDepth));
    const int cascadeIndex = int(min(dot(comparisons, float4(1.0, 1.0, 1.0, 1.0)), u.shadowParams.x - 1.0));

    const float4 shadowCoord = mul(float4(worldPos, 1.0), u.shadowMatrixPalette[cascadeIndex]);
    if (shadowCoord.w <= 0.0) {
        return 1.0;
    }
    const float3 coord = shadowCoord.xyz / shadowCoord.w;
    // Outside the cascade's atlas region contributes no shadow.
    if (any(coord.xy < float2(0.0, 0.0)) || any(coord.xy > float2(1.0, 1.0))) {
        return 1.0;
    }
    return VT_SHADOW_COMPARE(shadowTexture, coord.xy, coord.z - u.shadowParams.y);
}

[shader("vertex")]
QuadVarying fogVertex(QuadVertexIn input)
{
    return quadPassThrough(input);
}

[shader("fragment")]
float4 fogFragment(QuadVarying input) : SV_Target0
{
    const float2 uv = clamp(input.uv, float2(0.0, 0.0), float2(1.0, 1.0));
    const float cameraNear = u.cameraParams.x;
    const float cameraFar = u.cameraParams.y;
    const float extinction = u.cameraParams.z;

    const float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    const float3 rayDir = normalize(mul(float4(ndc * u.projScale.xy, -1.0, 0.0), u.invView).xyz);

    const float rawDepth = depthTexture.Sample(uv);
    const float sceneDepth = fogLinearDepth(rawDepth, cameraNear, cameraFar);
    const float rayDot = max(dot(rayDir, u.cameraForward.xyz), 0.001);
    const float rayLength = min(sceneDepth / rayDot, u.fogParams.w);

    const float stepCount = max(u.scatterParams.y, 1.0);
    const float dt = rayLength / stepCount;

    // Per-pixel dither, cycled per frame so TAA can accumulate it away.
    const float noise = frac(fogNoise(input.position.xy) + u.scatterParams.z);

    // The light direction is constant along the ray, so the phase term is evaluated once.
    const float3 sunLight = u.lightColor.xyz * fogPhase(dot(rayDir, u.lightDirection.xyz), u.scatterParams.x);
    const bool hasShadows = u.shadowParams.z > 0.5;

    float3 inscatter = float3(0.0, 0.0, 0.0);
    float transmittance = 1.0;
    for (float i = 0.0; i < stepCount; i += 1.0) {
        const float t = (i + noise) * dt;
        const float3 pos = u.cameraPosition.xyz + rayDir * t;

        // Exponential height fog, constant below the base height.
        const float density = u.fogParams.x * exp(-u.fogParams.z * max(pos.y - u.fogParams.y, 0.0));

        float shadow = 1.0;
        if (hasShadows) {
            shadow = lerp(1.0, sampleFogShadow(pos, t * rayDot), u.scatterParams.w);
        }

        // Accumulate in-scattered light, then attenuate through this slab (Beer-Lambert).
        const float3 radiance = sunLight * shadow + u.ambient.xyz;
        inscatter += transmittance * u.tint.xyz * radiance * (density * dt);
        transmittance *= exp(-extinction * density * dt);

        if (transmittance < 0.005) {
            break;
        }
    }
    return float4(inscatter, transmittance);
}
