// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 07.10.2026
//
// Scalable Ambient Obscurance: spiral taps over the scene depth (input 0), occlusion
// to visibility, raised to the power. Every depth tap is POINT sampled and its uv snapped
// to the centre of the texel it reads, so the position reconstructed from that texel's
// depth is the point the depth was rendered at: unsnapped, a flat surface reconstructs as
// a staircase and occludes ITSELF (a raw factor of 0.54 on a plane that should read 1.0,
// which the depth-aware blur turned into stripes across every flat wall).
//
// DEVIATION: upstream reconstructs positions with negative Z; here the view depth is
// positive, so the cross product of the position derivatives points away from the camera
// and is negated, and the sample radius divides by +z.
import vtquad;
#include "bindings.slang"

// The C++ SsaoUniforms (ssaoShaders.h) as six vec4s, so no compiler's vec2 packing rule
// can move a field.
struct SsaoUniforms
{
    float4 v0;   // aspect, pad, invResolution.xy
    float4 v1;   // sampleCount (x = count, y = 1/count), spiralTurns, pad
    float4 v2;   // angleIncCosSin.xy, maxLevel, invRadiusSquared
    float4 v3;   // minHorizonAngleSineSquared, bias, peak2, intensity
    float4 v4;   // power, projectionScaleRadius, randomize, cameraNear
    float4 v5;   // cameraFar
};
VT_DECLARE_QUAD_UNIFORMS(SsaoUniforms, u)
VT_DECLARE_QUAD_DEPTH_INPUT0(depthTexture)

static const float PI = 3.14159265359;

float getLinearDepth(float rawDepth)
{
    const float n = u.v4.w;
    const float f = u.v5.x;
    return (n * f) / (f - rawDepth * (f - n));
}

// Interleaved gradient noise.
float randomIGN(float2 fragCoord)
{
    const float3 m = float3(0.06711056, 0.00583715, 52.9829189);
    return frac(m.z * frac(dot(fragCoord, m.xy)));
}

float3 viewPosFromDepth(float2 uv, float linearDepth, float aspect)
{
    return float3((0.5 - uv) * float2(aspect, 1.0) * linearDepth, linearDepth);
}

float2 snapToDepthTexelCenter(float2 uv)
{
    uint width;
    uint height;
    depthTexture.GetDimensions(width, height);
    const float2 size = float2(float(width), float(height));
    return (floor(uv * size) + 0.5) / size;
}

float3 viewNormal(float3 position, float2 uv, float2 invRes, float aspect)
{
    const float2 uvdx = snapToDepthTexelCenter(uv + float2(invRes.x, 0.0));
    const float2 uvdy = snapToDepthTexelCenter(uv + float2(0.0, invRes.y));
    const float3 px = viewPosFromDepth(uvdx, getLinearDepth(depthTexture.Sample(uvdx)), aspect);
    const float3 py = viewPosFromDepth(uvdy, getLinearDepth(depthTexture.Sample(uvdy)), aspect);
    return normalize(cross(px - position, py - position));
}

[shader("vertex")]
QuadVarying ssaoVertex(QuadVertexIn input)
{
    return quadPassThrough(input);
}

[shader("fragment")]
float4 ssaoFragment(QuadVarying input) : SV_Target0
{
    const float aspect = u.v0.x;
    const float2 invRes = u.v0.zw;
    const float2 uv = snapToDepthTexelCenter(clamp(input.uv, float2(0.0, 0.0), float2(1.0, 1.0)));

    const float depth = getLinearDepth(depthTexture.Sample(uv));
    const float3 origin = viewPosFromDepth(uv, depth, aspect);
    const float3 normal = -viewNormal(origin, uv, invRes, aspect);

    float occlusion = 0.0;
    const float intensity = u.v3.w;
    if (intensity > 0.0) {
        const float noise = randomIGN(input.position.xy) + u.v4.z;
        const float angle = (2.0 * PI * 2.4) * noise;
        float2 tapPos = float2(cos(angle), sin(angle));
        // The column-major mat2(c, s, -s, c) times the tap position, spelled out.
        const float c = u.v2.x;
        const float s = u.v2.y;

        const float ssDiskRadius = u.v4.y / origin.z;
        const float sampleCount = u.v1.x;
        const float invSampleCount = u.v1.y;
        const float invRadiusSquared = u.v2.w;
        const float minHorizonAngleSineSquared = u.v3.x;
        const float bias = u.v3.y;
        const float peak2 = u.v3.z;

        for (float i = 0.0; i < sampleCount; i += 1.0) {
            const float radius = (i + noise + 0.5) * invSampleCount;
            const float ssRadius = max(1.0, radius * radius * ssDiskRadius);
            const float2 uvSamplePos = snapToDepthTexelCenter(uv + ssRadius * tapPos * invRes);

            const float occlusionDepth = getLinearDepth(depthTexture.Sample(uvSamplePos));
            const float3 p = viewPosFromDepth(uvSamplePos, occlusionDepth, aspect);

            const float3 v = p - origin;
            const float vv = dot(v, v);
            const float vn = dot(v, normal);

            float w = max(0.0, 1.0 - vv * invRadiusSquared);
            w = w * w;
            w *= step(vv * minHorizonAngleSineSquared, vn * vn);
            // The bias subtracts a depth-proportional amount, so a sample barely above the
            // tangent plane (depth noise on a flat surface) occludes nothing.
            occlusion += w * max(0.0, vn - origin.z * bias) / (vv + peak2);

            tapPos = float2(c * tapPos.x + (-s) * tapPos.y, s * tapPos.x + c * tapPos.y);
        }
    }

    float ao = max(0.0, 1.0 - occlusion * intensity);
    ao = pow(ao, u.v4.x);
    return float4(ao, ao, ao, 1.0);
}
