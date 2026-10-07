// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 07.10.2026
//
// The in-scattering of one clustered spot or omni light, added into the fog texture. The
// pass draws a fullscreen triangle per light, scissored to the screen bounds of its
// volume, and marches each pixel's ray over the part inside the light's bounding sphere —
// clipped to the cone for a spot — sampling the clustered shadow and cookie atlases. The
// lookups are twins of the cluster loop's: a spot projects into its rect with no shader
// bias, an omni face stores perspective depth with a RELATIVE bias, one tap a step offset
// on a spiral. The samples are not contiguous, so the transmittance is analytic. Only the
// in-scattered light is added; the blend keeps the transmittance the directional march
// wrote. Inputs: 0 scene depth (point sampled), 1 shadow atlas, 2 cookie atlas.
import vtquad;
import vtfog;
#include "bindings.slang"

struct FogLocalUniforms
{
    float4x4 invView;
    float4x4 lightProjMatrix;   // a spot: world -> its atlas slot (shadow and cookie)
    float4 cameraPosition;      // xyz
    float4 cameraForward;       // xyz
    float4 projScale;           // xy, z = camera near, w = camera far
    float4 tint;                // xyz
    float4 fogParams;           // density, height base, height falloff, max distance
    float4 marchParams;         // anisotropy, steps, noise offset, extinction
    float4 lightPosRange;       // xyz position, w range
    float4 lightSphere;         // xyz centre, w radius of the volume marched
    float4 lightColor;          // xyz, pre-scaled by intensity and exposure
    float4 lightDir;            // xyz spot axis, w 1 spot / 0 omni
    float4 lightSpot;           // inner cos, outer cos, shadow intensity, cookie intensity
    float4 lightAtten;          // x 1 linear falloff, y cookie channel, z atlas resolution
    float4 lightAtlas;          // an omni's atlas rect (x, y, size, edge pixels)
    float4 omniDepth;           // an omni's shadow near, far, relative bias
};
VT_DECLARE_QUAD_UNIFORMS(FogLocalUniforms, u)
VT_DECLARE_QUAD_DEPTH_INPUT0(depthTexture)
VT_DECLARE_QUAD_SHADOW_INPUT1(shadowAtlas)
VT_DECLARE_QUAD_INPUT2(cookieAtlas)
VT_DECLARE_QUAD_COMPARE_SAMPLER

// Falloffs, twins of the forward pass's.
float fogFalloffLinear(float range, float3 lightVec)
{
    return max((range - length(lightVec)) / max(range, 1e-4), 0.0);
}

float fogFalloffInvSquared(float range, float3 lightVec)
{
    const float sqrDist = dot(lightVec, lightVec);
    const float invRadius = 1.0 / max(range, 1e-4);
    const float window = saturate(1.0 - (sqrDist * invRadius * invRadius) * (sqrDist * invRadius * invRadius));
    return 16.0 / (sqrDist + 1.0) * window * window;
}

// An omni light's cube face in the atlas.
float2 fogCubemapAtlasCoordinates(float4 rect, float resolution, float3 dir)
{
    const float3 vAbs = abs(dir);
    float ma;
    float2 uv;
    float2 tileOffset;
    if (vAbs.z >= vAbs.x && vAbs.z >= vAbs.y) {
        ma = 0.5 / vAbs.z;
        uv = float2(dir.z < 0.0 ? -dir.x : dir.x, dir.y);
        tileOffset = float2(2.0, dir.z < 0.0 ? 1.0 : 0.0);
    } else if (vAbs.y >= vAbs.x) {
        ma = 0.5 / vAbs.y;
        uv = float2(dir.x, dir.y < 0.0 ? dir.z : -dir.z);
        tileOffset = float2(1.0, dir.y < 0.0 ? 1.0 : 0.0);
    } else {
        ma = 0.5 / vAbs.x;
        uv = float2(dir.x < 0.0 ? dir.z : -dir.z, dir.y);
        tileOffset = float2(0.0, dir.x < 0.0 ? 1.0 : 0.0);
    }
    uv = uv * ma + 0.5;
    const float faceSize = rect.z / 3.0;
    const float offset = rect.w / max(resolution * faceSize, 1.0);
    uv = uv * (1.0 - 2.0 * offset) + offset;
    return uv * faceSize + tileOffset * faceSize + rect.xy;
}

float volSampleShadow(float3 pos, float3 lightVec, float spiral)
{
    const float resolution = u.lightAtten.z;
    const float2 tapOffset = float2(cos(spiral), sin(spiral)) / resolution;
    if (u.lightDir.w > 0.0) {
        const float4 projPos = mul(float4(pos, 1.0), u.lightProjMatrix);
        const float3 coord = projPos.xyz / projPos.w;
        return VT_SHADOW_COMPARE(shadowAtlas, coord.xy + tapOffset, coord.z);
    }
    const float2 uv = fogCubemapAtlasCoordinates(u.lightAtlas, resolution, lightVec);
    const float3 absDir = abs(lightVec);
    const float d = max(absDir.x, max(absDir.y, absDir.z)) * (1.0 - u.omniDepth.z);
    const float compareValue = u.omniDepth.y * (d - u.omniDepth.x) / max((u.omniDepth.y - u.omniDepth.x) * d, 1e-6);
    return VT_SHADOW_COMPARE(shadowAtlas, uv + tapOffset, compareValue);
}

float3 volSampleCookie(float3 pos, float3 lightVec)
{
    float2 uv;
    if (u.lightDir.w > 0.0) {
        const float4 projected = mul(float4(pos, 1.0), u.lightProjMatrix);
        uv = projected.xy / max(projected.w, 1e-6);
    } else {
        uv = fogCubemapAtlasCoordinates(u.lightAtlas, u.lightAtten.z, lightVec);
    }
    const float4 texel = cookieAtlas.SampleLevel(uv, 0.0);
    float3 channel = texel.rgb;
    const uint cookieChannel = uint(u.lightAtten.y + 0.5);
    if (cookieChannel == 1u) {
        channel = float3(texel.r, texel.r, texel.r);
    } else if (cookieChannel == 2u) {
        channel = float3(texel.g, texel.g, texel.g);
    } else if (cookieChannel == 3u) {
        channel = float3(texel.b, texel.b, texel.b);
    } else if (cookieChannel == 4u) {
        channel = float3(texel.a, texel.a, texel.a);
    }
    return lerp(float3(1.0, 1.0, 1.0), channel, u.lightSpot.w);
}

// Optical depth of the height fog over a part of the ray entirely below or entirely above
// the base height; h0 is the camera's height above the base.
float volFogSegmentDepth(float s0, float s1, float h0, float rayDirY)
{
    if (s1 <= s0) {
        return 0.0;
    }
    const float density = u.fogParams.x;
    const float falloff = u.fogParams.z;
    const float hMid = h0 + rayDirY * (s0 + s1) * 0.5;
    if (hMid <= 0.0) {
        return density * (s1 - s0);
    }
    const float scale = falloff * rayDirY;
    if (abs(scale) < 1e-6) {
        return density * exp(-falloff * hMid) * (s1 - s0);
    }
    return density * (exp(-falloff * (h0 + rayDirY * s0)) - exp(-falloff * (h0 + rayDirY * s1))) / scale;
}

float volFogOpticalDepth(float t, float rayDirY)
{
    const float h0 = u.cameraPosition.y - u.fogParams.y;
    const float sCross = abs(rayDirY) > 1e-6 ? clamp(-h0 / rayDirY, 0.0, t) : 0.0;
    return (volFogSegmentDepth(0.0, sCross, h0, rayDirY) + volFogSegmentDepth(sCross, t, h0, rayDirY)) *
        u.marchParams.w;
}

float2 volClipCone(float2 span, float root, float gradient, float axial)
{
    if (axial < 0.0) {
        return span;
    }
    return gradient > 0.0 ? float2(max(span.x, root), span.y) : float2(span.x, min(span.y, root));
}

[shader("vertex")]
QuadVarying fogLocalVertex(QuadVertexIn input)
{
    return quadPassThrough(input);
}

[shader("fragment")]
float4 fogLocalFragment(QuadVarying input) : SV_Target0
{
    const float2 uv = clamp(input.uv, float2(0.0, 0.0), float2(1.0, 1.0));
    const float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    const float3 rayDir = normalize(mul(float4(ndc * u.projScale.xy, -1.0, 0.0), u.invView).xyz);

    const float rawDepth = depthTexture.SampleLevel(uv, 0.0);
    const float cameraNear = u.projScale.z;
    const float cameraFar = u.projScale.w;
    const float sceneDepth = (cameraNear * cameraFar) / (cameraFar - rawDepth * (cameraFar - cameraNear));
    const float rayDot = max(dot(rayDir, u.cameraForward.xyz), 0.001);
    const float sceneT = min(sceneDepth / rayDot, u.fogParams.w);

    // March only the part of the ray inside the light's bounding sphere.
    const float3 sphereToCam = u.cameraPosition.xyz - u.lightSphere.xyz;
    const float halfB = dot(sphereToCam, rayDir);
    const float c = dot(sphereToCam, sphereToCam) - u.lightSphere.w * u.lightSphere.w;
    const float discriminant = halfB * halfB - c;
    if (discriminant <= 0.0) {
        discard;
    }
    const float rootOffset = sqrt(discriminant);
    float t0 = max(-halfB - rootOffset, 0.0);
    float t1 = min(-halfB + rootOffset, sceneT);

    // A spot: clip to the cone's slab and surface.
    if (u.lightDir.w > 0.0) {
        const float3 apexToCam = u.cameraPosition.xyz - u.lightPosRange.xyz;
        const float axisStart = dot(apexToCam, u.lightDir.xyz);
        const float axisRate = dot(rayDir, u.lightDir.xyz);
        if (abs(axisRate) > 1e-6) {
            const float tApex = -axisStart / axisRate;
            const float tRange = (u.lightPosRange.w - axisStart) / axisRate;
            t0 = max(t0, min(tApex, tRange));
            t1 = min(t1, max(tApex, tRange));
        } else if (axisStart < 0.0 || axisStart > u.lightPosRange.w) {
            discard;
        }
        const float cosSqr = u.lightSpot.y * u.lightSpot.y;
        const float qa = axisRate * axisRate - cosSqr;
        const float qb = axisRate * axisStart - cosSqr * dot(rayDir, apexToCam);
        const float qc = axisStart * axisStart - cosSqr * dot(apexToCam, apexToCam);
        float2 span = float2(t0, t1);
        if (abs(qa) > 1e-6) {
            const float discriminantCone = qb * qb - qa * qc;
            if (discriminantCone > 0.0) {
                const float rootOffsetCone = sqrt(discriminantCone);
                const float rootA = (-qb - rootOffsetCone) / qa;
                const float rootB = (-qb + rootOffsetCone) / qa;
                span = volClipCone(span, rootA, qa * rootA + qb, axisStart + rootA * axisRate);
                span = volClipCone(span, rootB, qa * rootB + qb, axisStart + rootB * axisRate);
            }
        } else if (abs(qb) > 1e-6) {
            const float root = -0.5 * qc / qb;
            span = volClipCone(span, root, qb, axisStart + root * axisRate);
        }
        t0 = span.x;
        t1 = span.y;
    }
    if (t1 <= t0) {
        discard;
    }

    const float stepCount = max(u.marchParams.y, 1.0);
    const float dt = (t1 - t0) / stepCount;
    const float noise = frac(fogNoise(input.position.xy) + u.marchParams.z);

    float3 inscatter = float3(0.0, 0.0, 0.0);
    for (float i = 0.0; i < stepCount; i += 1.0) {
        const float t = t0 + (i + noise) * dt;
        const float3 pos = u.cameraPosition.xyz + rayDir * t;
        const float density = u.fogParams.x * exp(-u.fogParams.z * max(pos.y - u.fogParams.y, 0.0));

        const float3 lightVec = pos - u.lightPosRange.xyz;
        const float3 lightDirNorm = normalize(lightVec);
        float atten = u.lightAtten.x > 0.0 ? fogFalloffLinear(u.lightPosRange.w, lightVec)
                                           : fogFalloffInvSquared(u.lightPosRange.w, lightVec);
        if (u.lightDir.w > 0.0) {
            atten *= smoothstep(u.lightSpot.y, u.lightSpot.x, dot(lightDirNorm, u.lightDir.xyz));
        }
        if (atten > 0.00001) {
            if (u.lightSpot.z > 0.0) {
                atten *= lerp(1.0, volSampleShadow(pos, lightVec, (i + noise) * 2.39996), u.lightSpot.z);
            }
            float3 radiance = u.lightColor.xyz;
            if (u.lightSpot.w > 0.0) {
                radiance *= volSampleCookie(pos, lightVec);
            }
            const float transmittance = exp(-volFogOpticalDepth(t, rayDir.y));
            const float phase = fogPhase(dot(rayDir, -lightDirNorm), u.marchParams.x);
            inscatter += transmittance * u.tint.xyz * radiance * (atten * phase * density * dt);
        }
    }
    return float4(inscatter, 1.0);
}
