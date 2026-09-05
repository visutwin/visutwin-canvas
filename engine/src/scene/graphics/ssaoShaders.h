// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Scalable Ambient Obscurance (spiral-tap SAO from the depth buffer) in both
// languages, driven through QuadRender — one implementation rather than a pass
// class per backend. Texture: quad slot 0 = scene depth.
//
#pragma once

#include <cstdint>

namespace visutwin::canvas::ssao_shaders
{
    /**
     * Must match the SsaoParams/SsaoUniforms block in both shaders below. Every
     * vec2 is preceded by an explicit pad so it lands 8-byte aligned, which is
     * what MSL and std140 both require — that keeps one C++ struct valid for both.
     */
    struct alignas(16) SsaoUniforms
    {
        float aspect = 1.0f;                      // offset  0
        float _pad0 = 0.0f;                       // offset  4
        float invResolution[2] = {0.0f, 0.0f};    // offset  8
        float sampleCount[2] = {12.0f, 1.0f};     // offset 16  x = count, y = 1/count
        float spiralTurns = 10.0f;                // offset 24
        float _pad1 = 0.0f;                       // offset 28
        float angleIncCosSin[2] = {0.0f, 0.0f};   // offset 32
        float maxLevel = 0.0f;                    // offset 40
        float invRadiusSquared = 0.0f;            // offset 44
        float minHorizonAngleSineSquared = 0.0f;  // offset 48
        float bias = 0.001f;                      // offset 52
        float peak2 = 0.0f;                       // offset 56
        float intensity = 0.0f;                   // offset 60
        float power = 6.0f;                       // offset 64
        float projectionScaleRadius = 0.0f;       // offset 68
        float randomize = 0.0f;                   // offset 72
        float cameraNear = 0.1f;                  // offset 76
        float cameraFar = 1000.0f;                // offset 80
    };
    static_assert(sizeof(SsaoUniforms) == 96);

    constexpr const char* SSAO_MSL = R"(
#include <metal_stdlib>
using namespace metal;

struct ComposeVertexIn {
    float3 position [[attribute(0)]];
    float3 normal [[attribute(1)]];
    float2 uv0 [[attribute(2)]];
    float4 tangent [[attribute(3)]];
    float2 uv1 [[attribute(4)]];
};

struct SsaoVarying {
    float4 position [[position]];
    float2 uv;
};

struct SsaoUniforms {
    float aspect;
    float2 invResolution;
    float2 sampleCount; // x=count, y=1/count
    float spiralTurns;
    float2 angleIncCosSin;
    float maxLevel;
    float invRadiusSquared;
    float minHorizonAngleSineSquared;
    float bias;
    float peak2;
    float intensity;
    float power;
    float projectionScaleRadius;
    float randomize;
    float cameraNear;
    float cameraFar;
};

vertex SsaoVarying ssaoVertex(ComposeVertexIn in [[stage_in]])
{
    SsaoVarying out;
    out.position = float4(in.position, 1.0);
    out.uv = in.uv0;
    return out;
}

static inline float getLinearDepth(float rawDepth, float cameraNear, float cameraFar)
{
    // Standard depth [0,1]: near=0, far=1 (vertex shader maps via clip.z = 0.5*(clip.z + clip.w)).
    // Returns positive linear view-space distance from camera.
    return (cameraNear * cameraFar) / (cameraFar - rawDepth * (cameraFar - cameraNear));
}

static constant float kLog2LodRate = 3.0;

// Random number between 0 and 1 using interleaved gradient noise
// Point-sampled depth. These passes reconstruct view-space positions from depth,
// and a bilinear tap straddling a silhouette returns a depth that belongs to
// NEITHER surface — a position in mid-air that the kernel then treats as real.
// The Vulkan side binds a nearest sampler for the same reason.
constexpr sampler ssaoDepthSampler(coord::normalized, filter::nearest,
                                   mip_filter::none, address::clamp_to_edge);

static inline float random(float2 fragCoord)
{
    const float3 m = float3(0.06711056, 0.00583715, 52.9829189);
    return fract(m.z * fract(dot(fragCoord, m.xy)));
}

static inline float3 computeViewSpacePositionFromDepth(float2 uv, float linearDepth, float aspect)
{
    return float3((0.5 - uv) * float2(aspect, 1.0) * linearDepth, linearDepth);
}

static inline float3 faceNormal(float3 dpdx, float3 dpdy)
{
    return normalize(cross(dpdx, dpdy));
}

// Compute normals directly from the depth texture (full resolution normals)
static inline float3 computeViewSpaceNormal(float3 position, float2 uv, float2 invResolution,
    float aspect, depth2d<float> depthTexture, sampler linearSampler, float cameraNear, float cameraFar)
{
    float2 uvdx = uv + float2(invResolution.x, 0.0);
    float2 uvdy = uv + float2(0.0, invResolution.y);
    float depthDx = depthTexture.sample(ssaoDepthSampler, uvdx);
    float depthDy = depthTexture.sample(ssaoDepthSampler, uvdy);
    float3 px = computeViewSpacePositionFromDepth(uvdx, getLinearDepth(depthDx, cameraNear, cameraFar), aspect);
    float3 py = computeViewSpacePositionFromDepth(uvdy, getLinearDepth(depthDy, cameraNear, cameraFar), aspect);
    float3 dpdx = px - position;
    float3 dpdy = py - position;
    return faceNormal(dpdx, dpdy);
}

// Spiral tap position (fast path)
static inline float2 startPosition(float noise)
{
    float angle = ((2.0 * M_PI_F) * 2.4) * noise;
    return float2(cos(angle), sin(angle));
}

static inline float3 tapLocationFast(float i, float2 p, float noise, float invSampleCount)
{
    float radius = (i + noise + 0.5) * invSampleCount;
    return float3(p, radius * radius);
}

static inline float2x2 tapAngleStep(float2 angleIncCosSin)
{
    return float2x2(angleIncCosSin.x, angleIncCosSin.y, -angleIncCosSin.y, angleIncCosSin.x);
}

static inline void computeAmbientOcclusionSAO(
    thread float& occlusion, float i, float ssDiskRadius,
    float2 uv, float3 origin, float3 normal,
    float2 tapPosition, float noise, float invSampleCount,
    float2 invResolution, float invRadiusSquared, float minHorizonAngleSineSquared,
    float bias, float peak2, float aspect,
    depth2d<float> depthTexture, sampler linearSampler, float cameraNear, float cameraFar)
{
    float3 tap = tapLocationFast(i, tapPosition, noise, invSampleCount);

    float ssRadius = max(1.0, tap.z * ssDiskRadius); // at least 1 pixel screen-space radius

    float2 uvSamplePos = uv + float2(ssRadius * tap.xy) * invResolution;

    float occlusionDepth = getLinearDepth(depthTexture.sample(ssaoDepthSampler, uvSamplePos), cameraNear, cameraFar);
    float3 p = computeViewSpacePositionFromDepth(uvSamplePos, occlusionDepth, aspect);

    // now we have the sample, compute AO
    float3 v = p - origin;        // sample vector
    float vv = dot(v, v);       // squared distance
    float vn = dot(v, normal);  // distance * cos(v, normal)

    // discard samples that are outside of the radius
    float w = max(0.0, 1.0 - vv * invRadiusSquared);
    w = w * w;

    // discard samples that are too close to the horizon
    w *= step(vv * minHorizonAngleSineSquared, vn * vn);

    occlusion += w * max(0.0, vn + origin.z * bias) / (vv + peak2);
}

static inline float scalableAmbientObscurance(
    float2 uv, float3 origin, float3 normal, float2 fragCoord,
    float2 sampleCount, float2 angleIncCosSin, float projectionScaleRadius,
    float2 invResolution, float invRadiusSquared, float minHorizonAngleSineSquared,
    float bias, float peak2, float aspect, float randomizeValue,
    depth2d<float> depthTexture, sampler linearSampler, float cameraNear, float cameraFar)
{
    float noise = random(fragCoord) + randomizeValue;
    float2 tapPos = startPosition(noise);
    float2x2 angleStep = tapAngleStep(angleIncCosSin);

    // Choose the screen-space sample radius proportional to the projected area of the sphere
    // DEVIATION: upstream uses -(projInfo.z / position.z) with negative Z (OpenGL -Z convention).
    // Our view-space Z is positive (linearDepth), so we use positive division directly.
    float ssDiskRadius = projectionScaleRadius / origin.z;

    float occlusion = 0.0;
    for (float i = 0.0; i < sampleCount.x; i += 1.0) {
        computeAmbientOcclusionSAO(occlusion, i, ssDiskRadius, uv, origin, normal, tapPos, noise,
            sampleCount.y, invResolution, invRadiusSquared, minHorizonAngleSineSquared,
            bias, peak2, aspect, depthTexture, linearSampler, cameraNear, cameraFar);
        tapPos = angleStep * tapPos;
    }
    return occlusion;
}

fragment float4 ssaoFragment(
    SsaoVarying in [[stage_in]],
    depth2d<float> depthTexture [[texture(0)]],
    sampler linearSampler [[sampler(0)]],
    constant SsaoUniforms& uniforms [[buffer(3)]])
{
    const float2 uv = clamp(in.uv, float2(0.0), float2(1.0));

    float rawDepth = depthTexture.sample(ssaoDepthSampler, uv);
    float depth = getLinearDepth(rawDepth, uniforms.cameraNear, uniforms.cameraFar);
    float3 origin = computeViewSpacePositionFromDepth(uv, depth, uniforms.aspect);
    // DEVIATION: upstream reconstructs positions with negative Z (depth = -getLinearScreenDepth),
    // so cross(dpdx, dpdy) naturally yields normals pointing towards the camera (-Z).
    // Our Metal path uses positive depth (distance from camera), so the cross product
    // produces normals pointing away (+Z).  Negate to match upstream convention.
    float3 normal = -computeViewSpaceNormal(origin, uv, uniforms.invResolution, uniforms.aspect,
        depthTexture, linearSampler, uniforms.cameraNear, uniforms.cameraFar);

    float occlusion = 0.0;
    if (uniforms.intensity > 0.0) {
        occlusion = scalableAmbientObscurance(uv, origin, normal, in.position.xy,
            uniforms.sampleCount, uniforms.angleIncCosSin, uniforms.projectionScaleRadius,
            uniforms.invResolution, uniforms.invRadiusSquared, uniforms.minHorizonAngleSineSquared,
            uniforms.bias, uniforms.peak2, uniforms.aspect, uniforms.randomize,
            depthTexture, linearSampler, uniforms.cameraNear, uniforms.cameraFar);
    }

    // occlusion to visibility
    float ao = max(0.0, 1.0 - occlusion * uniforms.intensity);
    ao = pow(ao, uniforms.power);

    return float4(ao, ao, ao, 1.0);
}
)";

    constexpr const char* SSAO_GLSL = R"(#version 450

#ifdef VT_VERTEX_SHADER
layout(location = 0) in vec3 vertexPosition;
layout(location = 2) in vec2 vertexUv0;
layout(location = 0) out vec2 vUv;
void main() { vUv = vertexUv0; gl_Position = vec4(vertexPosition, 1.0); }
#endif

#ifdef VT_FRAGMENT_SHADER
layout(location = 0) in vec2 vUv;

// Scalable Ambient Obscurance — port of metalSsaoPass.cpp ssaoFragment
// (spiral-tap SAO from the depth buffer, upstream algorithm).

layout(set = 1, binding = 0) uniform sampler2D depthTex;

layout(std140, set = 0, binding = 0) uniform SsaoParams {
    // Mirrors SsaoUniforms in renderPassSsao.cpp field for field; the explicit
    // pads keep each vec2 8-byte aligned, which std140 and MSL agree on.
    float aspect;
    float _pad0;
    vec2 invResolution;
    vec2 sampleCount;          // x = count, y = 1/count
    float spiralTurns;
    float _pad1;
    vec2 angleIncCosSin;
    float maxLevel;
    float invRadiusSquared;
    float minHorizonAngleSineSquared;
    float bias;
    float peak2;
    float intensity;
    float power;
    float projectionScaleRadius;
    float randomize;
    float cameraNear;
    float cameraFar;
} pc;

layout(location = 0) out vec4 outColor;

const float PI = 3.14159265359;

float getLinearDepth(float rawDepth) {
    float n = pc.cameraNear, f = pc.cameraFar;
    return (n * f) / (f - rawDepth * (f - n));
}

// Interleaved gradient noise.
float randomIGN(vec2 fragCoord) {
    const vec3 m = vec3(0.06711056, 0.00583715, 52.9829189);
    return fract(m.z * fract(dot(fragCoord, m.xy)));
}

vec3 viewPosFromDepth(vec2 uv, float linearDepth, float aspect) {
    return vec3((0.5 - uv) * vec2(aspect, 1.0) * linearDepth, linearDepth);
}

vec3 viewNormal(vec3 position, vec2 uv, vec2 invRes, float aspect) {
    vec2 uvdx = uv + vec2(invRes.x, 0.0);
    vec2 uvdy = uv + vec2(0.0, invRes.y);
    vec3 px = viewPosFromDepth(uvdx, getLinearDepth(texture(depthTex, uvdx).r), aspect);
    vec3 py = viewPosFromDepth(uvdy, getLinearDepth(texture(depthTex, uvdy).r), aspect);
    return normalize(cross(px - position, py - position));
}

void main() {
    float aspect = pc.aspect;
    vec2 invRes = pc.invResolution;
    vec2 uv = clamp(vUv, vec2(0.0), vec2(1.0));

    float depth = getLinearDepth(texture(depthTex, uv).r);
    vec3 origin = viewPosFromDepth(uv, depth, aspect);
    // Negated: positive-depth reconstruction flips the cross-product direction
    // relative to upstream's -Z convention (same DEVIATION as the Metal port).
    vec3 normal = -viewNormal(origin, uv, invRes, aspect);

    float occlusion = 0.0;
    float intensity = pc.intensity;
    if (intensity > 0.0) {
        float noise = randomIGN(gl_FragCoord.xy) + pc.randomize;
        float angle = (2.0 * PI * 2.4) * noise;
        vec2 tapPos = vec2(cos(angle), sin(angle));
        mat2 angleStep = mat2(pc.angleIncCosSin.x, pc.angleIncCosSin.y, -pc.angleIncCosSin.y, pc.angleIncCosSin.x);

        float ssDiskRadius = pc.projectionScaleRadius / origin.z;
        float sampleCount = pc.sampleCount.x;
        float invSampleCount = pc.sampleCount.y;

        for (float i = 0.0; i < sampleCount; i += 1.0) {
            float radius = (i + noise + 0.5) * invSampleCount;
            float ssRadius = max(1.0, radius * radius * ssDiskRadius);
            vec2 uvSamplePos = uv + ssRadius * tapPos * invRes;

            float occlusionDepth = getLinearDepth(texture(depthTex, uvSamplePos).r);
            vec3 p = viewPosFromDepth(uvSamplePos, occlusionDepth, aspect);

            vec3 v = p - origin;
            float vv = dot(v, v);
            float vn = dot(v, normal);

            float w = max(0.0, 1.0 - vv * pc.invRadiusSquared);
            w = w * w;
            w *= step(vv * pc.minHorizonAngleSineSquared, vn * vn);
            occlusion += w * max(0.0, vn + origin.z * pc.bias) / (vv + pc.peak2);

            tapPos = angleStep * tapPos;
        }
    }

    float ao = max(0.0, 1.0 - occlusion * intensity);
    ao = pow(ao, pc.power);
    outColor = vec4(ao, ao, ao, 1.0);
}
#endif
)";
}
