// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// TAA resolve — depth reprojection into the previous frame, Catmull-Rom or
// bilinear history fetch, neighbourhood colour clamping, 5% history blend
// (100% when the reprojection lands offscreen). One implementation over
// QuadRender. Textures: 0 source, 1 history, 2 scene depth.
//
#pragma once

#include <cstdint>

namespace visutwin::canvas::taa_shaders
{
    /**
     * Matches the TaaParams block in both shaders. Everything is a mat4 or a
     * vec4, so MSL and std140 agree with no padding to get wrong — the size and
     * the two flags share one vec4 rather than a float2 plus two uints, which is
     * how the Vulkan shader already packed them.
     */
    struct alignas(16) TaaUniforms
    {
        float viewProjectionPrevious[16];  // offset   0
        float viewProjectionInverse[16];   // offset  64
        float jitters[4];                  // offset 128
        float texSizeFlags[4];             // offset 144  xy = size, z = highQuality, w = historyValid
        float cameraParams[4];             // offset 160
    };
    static_assert(sizeof(TaaUniforms) == 176);

    constexpr const char* TAA_MSL = R"(
#include <metal_stdlib>
using namespace metal;

struct ComposeVertexIn {
    float3 position [[attribute(0)]];
    float3 normal [[attribute(1)]];
    float2 uv0 [[attribute(2)]];
    float4 tangent [[attribute(3)]];
    float2 uv1 [[attribute(4)]];
};

struct TaaVarying {
    float4 position [[position]];
    float2 uv;
};

struct TaaUniforms {
    float4x4 viewProjectionPrevious;
    float4x4 viewProjectionInverse;
    float4 jitters;
    float4 texSizeFlags;   // xy = texture size, z = highQuality, w = historyValid
    float4 cameraParams;
};

vertex TaaVarying taaVertex(ComposeVertexIn in [[stage_in]])
{
    TaaVarying out;
    out.position = float4(in.position, 1.0);
    out.uv = in.uv0;
    return out;
}

static inline float linearizeDepth(float z, float4 cameraParams)
{
    if (cameraParams.w == 0.0) {
        return (cameraParams.z * cameraParams.y) / (cameraParams.y + z * (cameraParams.z - cameraParams.y));
    }
    return cameraParams.z + z * (cameraParams.y - cameraParams.z);
}

static inline float delinearizeDepth(float linearDepth, float4 cameraParams)
{
    if (cameraParams.w == 0.0) {
        return (cameraParams.y * (cameraParams.z - linearDepth)) /
            (linearDepth * (cameraParams.z - cameraParams.y));
    }
    return (linearDepth - cameraParams.z) / (cameraParams.y - cameraParams.z);
}

static inline float2 reproject(float2 uv, float depth, constant TaaUniforms& uniforms)
{
    // DEVIATION: Metal depth buffer stores (ndcZ_gl + 1) / 2, undo to get OpenGL NDC Z
    depth = depth * 2.0 - 1.0;

    // DEVIATION: UV has Metal convention (V=0 at top), but the projection matrix uses
    // OpenGL convention (NDC Y=+1 at top). Convert: ndcX = uv.x*2-1, ndcY = (1-uv.y)*2-1 = 1-2*uv.y.
    float4 ndc = float4(uv.x * 2.0 - 1.0, 1.0 - 2.0 * uv.y, depth, 1.0);

    // Remove jitter from the current frame
    ndc.xy -= uniforms.jitters.xy;

    float4 worldPosition = uniforms.viewProjectionInverse * ndc;
    worldPosition /= worldPosition.w;

    float4 screenPrevious = uniforms.viewProjectionPrevious * worldPosition;
    // Convert back from NDC to Metal UV convention (flip Y back)
    float2 prevNdc = screenPrevious.xy / screenPrevious.w;
    return float2(prevNdc.x * 0.5 + 0.5, 0.5 - prevNdc.y * 0.5);
}

static inline float4 SampleTextureCatmullRom(
    texture2d<float> tex, sampler linearSampler, float2 uv, float2 texSize)
{
    float2 samplePos = uv * texSize;
    float2 texPos1 = floor(samplePos - 0.5) + 0.5;
    float2 f = samplePos - texPos1;

    float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    float2 w3 = f * f * (-0.5 + 0.5 * f);

    float2 w12 = w1 + w2;
    float2 offset12 = w2 / (w1 + w2);

    float2 texPos0 = (texPos1 - 1.0) / texSize;
    float2 texPos3 = (texPos1 + 2.0) / texSize;
    float2 texPos12 = (texPos1 + offset12) / texSize;

    float4 result = float4(0.0);
    result += tex.sample(linearSampler, float2(texPos0.x, texPos0.y), level(0.0)) * w0.x * w0.y;
    result += tex.sample(linearSampler, float2(texPos12.x, texPos0.y), level(0.0)) * w12.x * w0.y;
    result += tex.sample(linearSampler, float2(texPos3.x, texPos0.y), level(0.0)) * w3.x * w0.y;

    result += tex.sample(linearSampler, float2(texPos0.x, texPos12.y), level(0.0)) * w0.x * w12.y;
    result += tex.sample(linearSampler, float2(texPos12.x, texPos12.y), level(0.0)) * w12.x * w12.y;
    result += tex.sample(linearSampler, float2(texPos3.x, texPos12.y), level(0.0)) * w3.x * w12.y;

    result += tex.sample(linearSampler, float2(texPos0.x, texPos3.y), level(0.0)) * w0.x * w3.y;
    result += tex.sample(linearSampler, float2(texPos12.x, texPos3.y), level(0.0)) * w12.x * w3.y;
    result += tex.sample(linearSampler, float2(texPos3.x, texPos3.y), level(0.0)) * w3.x * w3.y;
    return result;
}

static inline float4 colorClamp(texture2d<float> sourceTexture, sampler linearSampler, float2 uv, float4 historyColor, float2 textureSize)
{
    float3 minColor = float3(9999.0);
    float3 maxColor = float3(-9999.0);
    for (float x = -1.0; x <= 1.0; ++x) {
        for (float y = -1.0; y <= 1.0; ++y) {
            float3 color = sourceTexture.sample(linearSampler, uv + float2(x, y) / textureSize).rgb;
            minColor = min(minColor, color);
            maxColor = max(maxColor, color);
        }
    }

    float3 clamped = clamp(historyColor.rgb, minColor, maxColor);
    return float4(clamped, historyColor.a);
}

fragment float4 taaFragment(
    TaaVarying in [[stage_in]],
    texture2d<float> sourceTexture [[texture(0)]],
    texture2d<float> historyTexture [[texture(1)]],
    depth2d<float> depthTexture [[texture(2)]],
    sampler linearSampler [[sampler(0)]],
    constant TaaUniforms& uniforms [[buffer(3)]])
{
    // TAA resolve (GLSL to Metal).
    const float2 uv = clamp(in.uv, float2(0.0), float2(1.0));

    // Current frame color
    const float4 srcColor = sourceTexture.sample(linearSampler, uv);

    // If no valid history, just pass through current frame
    if (uniforms.texSizeFlags.w < 0.5) {
        return srcColor;
    }

    // DEVIATION: upstream uses getLinearScreenDepth()/delinearizeDepth() from
    // screenDepthPS for the round-trip; the linearize->delinearize is an identity
    // on the raw hardware depth.  We skip the round-trip and use rawDepth directly
    // since reproject() only needs the original viewport [0,1] depth.
    float depth = depthTexture.sample(linearSampler, uv);

    // Reproject: find where this pixel was in the previous frame
    float2 historyUv = reproject(uv, depth, uniforms);

    // Sample history: Catmull-Rom (high quality) or bilinear
    float4 historyColor;
    if (uniforms.texSizeFlags.z > 0.5) {
        historyColor = SampleTextureCatmullRom(historyTexture, linearSampler, historyUv, uniforms.texSizeFlags.xy);
    } else {
        historyColor = historyTexture.sample(linearSampler, historyUv);
    }

    // Color clamping to handle disocclusion
    float4 historyColorClamped = colorClamp(sourceTexture, linearSampler, uv, historyColor, uniforms.texSizeFlags.xy);

    // Reject history samples that project outside the frame
    float mixFactor = (historyUv.x < 0.0 || historyUv.x > 1.0 ||
                       historyUv.y < 0.0 || historyUv.y > 1.0) ? 1.0 : 0.05;

    return mix(historyColorClamped, srcColor, mixFactor);
}
)";

    constexpr const char* TAA_GLSL = R"(#version 450

#ifdef VT_VERTEX_SHADER
layout(location = 0) in vec3 vertexPosition;
layout(location = 2) in vec2 vertexUv0;
layout(location = 0) out vec2 vUv;
void main() { vUv = vertexUv0; gl_Position = vec4(vertexPosition, 1.0); }
#endif

#ifdef VT_FRAGMENT_SHADER
layout(location = 0) in vec2 vUv;

// TAA resolve — port of metalTaaPass.cpp taaFragment: depth reprojection into
// the previous frame, Catmull-Rom or bilinear history fetch, neighborhood
// color clamping, 5% history blend (100% on offscreen reprojection).

layout(set = 1, binding = 0) uniform sampler2D sourceTex;
layout(set = 1, binding = 1) uniform sampler2D historyTex;
layout(set = 1, binding = 2) uniform sampler2D depthTex;

layout(std140, set = 0, binding = 0) uniform TaaParams {
    mat4 viewProjectionPrevious;
    mat4 viewProjectionInverse;
    vec4 jitters;        // xy = current-frame NDC jitter
    vec4 texSizeFlags;   // xy = texture size, z = highQuality, w = historyValid
    vec4 cameraParams;
} pc;

layout(location = 0) out vec4 outColor;

vec2 reproject(vec2 uv, float depth) {
    // Depth buffer stores (ndcZ_gl + 1) / 2 — undo to OpenGL NDC Z.
    depth = depth * 2.0 - 1.0;

    // UV is top-left-origin (Metal convention, matched by our viewport flip);
    // projection matrices use GL NDC (+Y up): ndcY = 1 - 2*uv.y.
    vec4 ndc = vec4(uv.x * 2.0 - 1.0, 1.0 - 2.0 * uv.y, depth, 1.0);
    ndc.xy -= pc.jitters.xy;

    vec4 worldPosition = pc.viewProjectionInverse * ndc;
    worldPosition /= worldPosition.w;

    vec4 screenPrevious = pc.viewProjectionPrevious * worldPosition;
    vec2 prevNdc = screenPrevious.xy / screenPrevious.w;
    return vec2(prevNdc.x * 0.5 + 0.5, 0.5 - prevNdc.y * 0.5);
}

vec4 sampleCatmullRom(vec2 uv, vec2 texSize) {
    vec2 samplePos = uv * texSize;
    vec2 texPos1 = floor(samplePos - 0.5) + 0.5;
    vec2 f = samplePos - texPos1;

    vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    vec2 w3 = f * f * (-0.5 + 0.5 * f);

    vec2 w12 = w1 + w2;
    vec2 offset12 = w2 / (w1 + w2);

    vec2 texPos0 = (texPos1 - 1.0) / texSize;
    vec2 texPos3 = (texPos1 + 2.0) / texSize;
    vec2 texPos12 = (texPos1 + offset12) / texSize;

    vec4 result = vec4(0.0);
    result += textureLod(historyTex, vec2(texPos0.x, texPos0.y), 0.0) * w0.x * w0.y;
    result += textureLod(historyTex, vec2(texPos12.x, texPos0.y), 0.0) * w12.x * w0.y;
    result += textureLod(historyTex, vec2(texPos3.x, texPos0.y), 0.0) * w3.x * w0.y;

    result += textureLod(historyTex, vec2(texPos0.x, texPos12.y), 0.0) * w0.x * w12.y;
    result += textureLod(historyTex, vec2(texPos12.x, texPos12.y), 0.0) * w12.x * w12.y;
    result += textureLod(historyTex, vec2(texPos3.x, texPos12.y), 0.0) * w3.x * w12.y;

    result += textureLod(historyTex, vec2(texPos0.x, texPos3.y), 0.0) * w0.x * w3.y;
    result += textureLod(historyTex, vec2(texPos12.x, texPos3.y), 0.0) * w12.x * w3.y;
    result += textureLod(historyTex, vec2(texPos3.x, texPos3.y), 0.0) * w3.x * w3.y;
    return result;
}

vec4 colorClamp(vec2 uv, vec4 historyColor, vec2 texSize) {
    vec3 minColor = vec3(9999.0);
    vec3 maxColor = vec3(-9999.0);
    for (float x = -1.0; x <= 1.0; x += 1.0) {
        for (float y = -1.0; y <= 1.0; y += 1.0) {
            vec3 color = texture(sourceTex, uv + vec2(x, y) / texSize).rgb;
            minColor = min(minColor, color);
            maxColor = max(maxColor, color);
        }
    }
    return vec4(clamp(historyColor.rgb, minColor, maxColor), historyColor.a);
}

void main() {
    vec2 texSize = pc.texSizeFlags.xy;
    vec2 uv = clamp(vUv, vec2(0.0), vec2(1.0));

    vec4 srcColor = texture(sourceTex, uv);

    if (pc.texSizeFlags.w < 0.5) {   // no valid history yet
        outColor = srcColor;
        return;
    }

    float depth = texture(depthTex, uv).r;
    vec2 historyUv = reproject(uv, depth);

    vec4 historyColor = (pc.texSizeFlags.z > 0.5)
        ? sampleCatmullRom(historyUv, texSize)
        : texture(historyTex, historyUv);

    vec4 historyColorClamped = colorClamp(uv, historyColor, texSize);

    float mixFactor = (historyUv.x < 0.0 || historyUv.x > 1.0 ||
                       historyUv.y < 0.0 || historyUv.y > 1.0) ? 1.0 : 0.05;

    outColor = mix(historyColorClamped, srcColor, mixFactor);
}
#endif
)";
}
