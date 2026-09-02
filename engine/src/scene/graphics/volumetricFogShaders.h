// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Shader sources for the volumetric fog march and its depth-aware combine,
// in both languages the engine speaks. Selected by
// GraphicsDevice::shaderLanguage() and driven through QuadRender, so the effect
// has ONE implementation rather than a pass class per backend.
//
#pragma once

#include <cstdint>

namespace visutwin::canvas::volumetric_fog
{
    /**
     * March uniforms. 512 bytes — every member is a float4/float4x4 so the MSL
     * and std140 layouts agree without manual padding. Must match the FogUniforms
     * block declared in both shader sources below.
     */
    struct alignas(16) FogUniforms
    {
        float invView[16];                 // offset   0
        float shadowMatrixPalette[4][16];  // offset  64
        float cameraPosition[4];           // offset 320  xyz
        float cameraForward[4];            // offset 336  xyz
        float projScale[4];                // offset 352  xy
        float tint[4];                     // offset 368  xyz
        float lightColor[4];               // offset 384  xyz
        float lightDirection[4];           // offset 400  xyz
        float ambient[4];                  // offset 416  xyz
        float fogParams[4];                // offset 432  x=density y=heightBase z=heightFalloff w=maxDistance
        float scatterParams[4];            // offset 448  x=anisotropy y=steps z=noiseOffset w=shadowIntensity
        float shadowCascadeDistances[4];   // offset 464
        float shadowParams[4];             // offset 480  x=cascadeCount y=bias z=hasShadows w=shadowDistance
        float cameraParams[4];             // offset 496  x=near y=far z=extinction
    };
    static_assert(sizeof(FogUniforms) == 512);

    /// Combine uniforms. xy = fog resolution, zw = 1/resolution; cameraParams x=near y=far.
    struct alignas(16) FogCombineUniforms
    {
        float textureSize[4];
        float cameraParams[4];
    };
    static_assert(sizeof(FogCombineUniforms) == 32);

    constexpr const char* MARCH_MSL = R"(
#include <metal_stdlib>
using namespace metal;

struct ComposeVertexIn {
    float3 position [[attribute(0)]];
    float3 normal [[attribute(1)]];
    float2 uv0 [[attribute(2)]];
    float4 tangent [[attribute(3)]];
    float2 uv1 [[attribute(4)]];
};

struct FogVarying {
    float4 position [[position]];
    float2 uv;
};

struct FogUniforms {
    float4x4 invView;
    float4x4 shadowMatrixPalette[4];
    float4 cameraPosition;
    float4 cameraForward;
    float4 projScale;
    float4 tint;
    float4 lightColor;
    float4 lightDirection;
    float4 ambient;
    float4 fogParams;
    float4 scatterParams;
    float4 shadowCascadeDistances;
    float4 shadowParams;
    float4 cameraParams;
};

vertex FogVarying fogVertex(ComposeVertexIn in [[stage_in]])
{
    FogVarying out;
    out.position = float4(in.position, 1.0);
    out.uv = in.uv0;
    return out;
}

static inline float getLinearDepth(float rawDepth, float cameraNear, float cameraFar)
{
    // Standard depth [0,1]: near=0, far=1. Returns positive linear view-space distance.
    return (cameraNear * cameraFar) / (cameraFar - rawDepth * (cameraFar - cameraNear));
}

// Interleaved gradient noise, used to offset the ray-march samples and hide banding.
static inline float fogNoise(float2 fragCoord)
{
    const float3 magic = float3(0.06711056, 0.00583715, 52.9829189);
    return fract(magic.z * fract(dot(fragCoord, magic.xy)));
}

// Normalized Henyey-Greenstein phase function.
static inline float fogPhase(float cosTheta, float g)
{
    const float g2 = g * g;
    const float denom = 1.0 + g2 - 2.0 * g * cosTheta;
    return (1.0 - g2) / (12.56637 * denom * sqrt(max(denom, 1e-6)));
}

// Cascaded directional shadow lookup along the ray. Mirrors the forward pass's cascade
// selection: pick the first cascade whose split distance exceeds the view depth.
static inline float sampleFogShadow(float3 worldPos, float viewDepth,
    constant FogUniforms& u, depth2d<float> shadowMap, sampler shadowSampler)
{
    if (viewDepth >= u.shadowParams.w) {
        return 1.0;
    }

    const float4 comparisons = step(u.shadowCascadeDistances, float4(viewDepth));
    const int cascadeIndex = int(min(dot(comparisons, float4(1.0)), u.shadowParams.x - 1.0));

    const float4 shadowCoord = u.shadowMatrixPalette[cascadeIndex] * float4(worldPos, 1.0);
    if (shadowCoord.w <= 0.0) {
        return 1.0;
    }
    const float3 coord = shadowCoord.xyz / shadowCoord.w;

    // Outside the cascade's atlas region contributes no shadow.
    if (any(coord.xy < float2(0.0)) || any(coord.xy > float2(1.0))) {
        return 1.0;
    }

    return shadowMap.sample_compare(shadowSampler, coord.xy, coord.z - u.shadowParams.y);
}

fragment float4 fogFragment(
    FogVarying in [[stage_in]],
    depth2d<float> depthTexture [[texture(0)]],
    depth2d<float> shadowTexture [[texture(1)]],
    sampler linearSampler [[sampler(0)]],
    constant FogUniforms& u [[buffer(3)]])
{
    const float2 uv = clamp(in.uv, float2(0.0), float2(1.0));

    const float cameraNear = u.cameraParams.x;
    const float cameraFar = u.cameraParams.y;
    const float extinction = u.cameraParams.z;

    // World-space ray for this pixel.
    const float2 ndc = uv * 2.0 - 1.0;
    const float3 rayDir = normalize(
        (u.invView * float4(ndc * u.projScale.xy, -1.0, 0.0)).xyz);

    // Distance along the ray to the scene surface. The depth buffer stores distance along the
    // view axis, so divide by the ray's projection onto the forward vector.
    const float rawDepth = depthTexture.sample(linearSampler, uv);
    const float sceneDepth = getLinearDepth(rawDepth, cameraNear, cameraFar);
    const float rayDot = max(dot(rayDir, u.cameraForward.xyz), 0.001);
    const float rayLength = min(sceneDepth / rayDot, u.fogParams.w);

    const float stepCount = max(u.scatterParams.y, 1.0);
    const float dt = rayLength / stepCount;

    // Per-pixel dither, cycled per frame so TAA can accumulate it away.
    const float noise = fract(fogNoise(in.position.xy) + u.scatterParams.z);

    // The light direction is constant along the ray, so the phase term is evaluated once.
    const float3 sunLight = u.lightColor.xyz *
        fogPhase(dot(rayDir, u.lightDirection.xyz), u.scatterParams.x);

    const bool hasShadows = u.shadowParams.z > 0.5;

    // Metal's comparison sampler must be declared in the shader; depth compare is LESS so that
    // a fragment nearer than the stored caster depth is lit.
    constexpr sampler shadowSampler(coord::normalized, filter::linear,
        address::clamp_to_edge, compare_func::less_equal);

    float3 inscatter = float3(0.0);
    float transmittance = 1.0;

    for (float i = 0.0; i < stepCount; i += 1.0) {
        const float t = (i + noise) * dt;
        const float3 pos = u.cameraPosition.xyz + rayDir * t;

        // Exponential height fog, constant below the base height.
        const float density = u.fogParams.x *
            exp(-u.fogParams.z * max(pos.y - u.fogParams.y, 0.0));

        float shadow = 1.0;
        if (hasShadows) {
            shadow = mix(1.0, sampleFogShadow(pos, t * rayDot, u, shadowTexture, shadowSampler),
                u.scatterParams.w);
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
)";

    // DEVIATION from the MSL path: the shadow tap is a MANUAL depth comparison
    // rather than a hardware comparison sample. The Vulkan backend binds shadow
    // maps through a plain (non-comparison) sampler and compares in-shader
    // everywhere else — forward.frag does the same — so this stays consistent
    // with the rest of the backend. The practical difference is that Metal gets
    // bilinear PCF from the hardware compare while Vulkan gets a single tap.
    constexpr const char* MARCH_GLSL = R"(
#version 450

#ifdef VT_VERTEX_SHADER
layout(location = 0) in vec3 vertexPosition;
layout(location = 2) in vec2 vertexUv0;
layout(location = 0) out vec2 vUv;
void main() {
    vUv = vertexUv0;
    gl_Position = vec4(vertexPosition, 1.0);
}
#endif

#ifdef VT_FRAGMENT_SHADER
layout(set = 0, binding = 0) uniform FogUniforms {
    mat4 invView;
    mat4 shadowMatrixPalette[4];
    vec4 cameraPosition;
    vec4 cameraForward;
    vec4 projScale;
    vec4 tint;
    vec4 lightColor;
    vec4 lightDirection;
    vec4 ambient;
    vec4 fogParams;
    vec4 scatterParams;
    vec4 shadowCascadeDistances;
    vec4 shadowParams;
    vec4 cameraParams;
} u;

layout(set = 1, binding = 0) uniform sampler2D depthTexture;
layout(set = 1, binding = 1) uniform sampler2D shadowTexture;

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 fragColor;

float getLinearDepth(float rawDepth, float cameraNear, float cameraFar) {
    return (cameraNear * cameraFar) / (cameraFar - rawDepth * (cameraFar - cameraNear));
}

float fogNoise(vec2 fragCoord) {
    const vec3 magic = vec3(0.06711056, 0.00583715, 52.9829189);
    return fract(magic.z * fract(dot(fragCoord, magic.xy)));
}

float fogPhase(float cosTheta, float g) {
    float g2 = g * g;
    float denom = 1.0 + g2 - 2.0 * g * cosTheta;
    return (1.0 - g2) / (12.56637 * denom * sqrt(max(denom, 1e-6)));
}

float sampleFogShadow(vec3 worldPos, float viewDepth) {
    if (viewDepth >= u.shadowParams.w) {
        return 1.0;
    }
    vec4 comparisons = step(u.shadowCascadeDistances, vec4(viewDepth));
    int cascadeIndex = int(min(dot(comparisons, vec4(1.0)), u.shadowParams.x - 1.0));

    vec4 shadowCoord = u.shadowMatrixPalette[cascadeIndex] * vec4(worldPos, 1.0);
    if (shadowCoord.w <= 0.0) {
        return 1.0;
    }
    vec3 coord = shadowCoord.xyz / shadowCoord.w;
    if (any(lessThan(coord.xy, vec2(0.0))) || any(greaterThan(coord.xy, vec2(1.0)))) {
        return 1.0;
    }
    float stored = texture(shadowTexture, coord.xy).r;
    return (coord.z - u.shadowParams.y) <= stored ? 1.0 : 0.0;
}

void main() {
    vec2 uv = clamp(vUv, vec2(0.0), vec2(1.0));

    float cameraNear = u.cameraParams.x;
    float cameraFar = u.cameraParams.y;
    float extinction = u.cameraParams.z;

    vec2 ndc = uv * 2.0 - 1.0;
    vec3 rayDir = normalize((u.invView * vec4(ndc * u.projScale.xy, -1.0, 0.0)).xyz);

    float rawDepth = texture(depthTexture, uv).r;
    float sceneDepth = getLinearDepth(rawDepth, cameraNear, cameraFar);
    float rayDot = max(dot(rayDir, u.cameraForward.xyz), 0.001);
    float rayLength = min(sceneDepth / rayDot, u.fogParams.w);

    float stepCount = max(u.scatterParams.y, 1.0);
    float dt = rayLength / stepCount;

    float noise = fract(fogNoise(gl_FragCoord.xy) + u.scatterParams.z);

    vec3 sunLight = u.lightColor.xyz *
        fogPhase(dot(rayDir, u.lightDirection.xyz), u.scatterParams.x);

    bool hasShadows = u.shadowParams.z > 0.5;

    vec3 inscatter = vec3(0.0);
    float transmittance = 1.0;

    for (float i = 0.0; i < stepCount; i += 1.0) {
        float t = (i + noise) * dt;
        vec3 pos = u.cameraPosition.xyz + rayDir * t;

        float density = u.fogParams.x *
            exp(-u.fogParams.z * max(pos.y - u.fogParams.y, 0.0));

        float shadow = 1.0;
        if (hasShadows) {
            shadow = mix(1.0, sampleFogShadow(pos, t * rayDot), u.scatterParams.w);
        }

        vec3 radiance = sunLight * shadow + u.ambient.xyz;
        inscatter += transmittance * u.tint.xyz * radiance * (density * dt);
        transmittance *= exp(-extinction * density * dt);

        if (transmittance < 0.005) {
            break;
        }
    }

    fragColor = vec4(inscatter, transmittance);
}
#endif
)";

    constexpr const char* COMBINE_MSL = R"(
#include <metal_stdlib>
using namespace metal;

struct ComposeVertexIn {
    float3 position [[attribute(0)]];
    float3 normal [[attribute(1)]];
    float2 uv0 [[attribute(2)]];
    float4 tangent [[attribute(3)]];
    float2 uv1 [[attribute(4)]];
};

struct FogCombineVarying {
    float4 position [[position]];
    float2 uv;
};

struct FogCombineUniforms {
    float4 textureSize;   // xy = fog resolution, zw = 1/resolution
    float4 cameraParams;  // x = near, y = far
};

vertex FogCombineVarying fogCombineVertex(ComposeVertexIn in [[stage_in]])
{
    FogCombineVarying out;
    out.position = float4(in.position, 1.0);
    out.uv = in.uv0;
    return out;
}

static inline float getLinearDepth(float rawDepth, float cameraNear, float cameraFar)
{
    return (cameraNear * cameraFar) / (cameraFar - rawDepth * (cameraFar - cameraNear));
}

fragment float4 fogCombineFragment(
    FogCombineVarying in [[stage_in]],
    depth2d<float> depthTexture [[texture(0)]],
    texture2d<float> fogTexture [[texture(1)]],
    sampler linearSampler [[sampler(0)]],
    constant FogCombineUniforms& u [[buffer(3)]])
{
    const float2 uv = clamp(in.uv, float2(0.0), float2(1.0));
    const float cameraNear = u.cameraParams.x;
    const float cameraFar = u.cameraParams.y;

    const float depth = getLinearDepth(depthTexture.sample(linearSampler, uv), cameraNear, cameraFar);

    // The four nearest texel centres of the low-resolution fog texture.
    const float2 texel = uv * u.textureSize.xy - 0.5;
    const float2 base = (floor(texel) + 0.5) * u.textureSize.zw;
    const float2 f = fract(texel);

    float2 uvs[4];
    uvs[0] = base;
    uvs[1] = base + float2(u.textureSize.z, 0.0);
    uvs[2] = base + float2(0.0, u.textureSize.w);
    uvs[3] = base + u.textureSize.zw;

    const float4 bilinear = float4(
        (1.0 - f.x) * (1.0 - f.y),
        f.x * (1.0 - f.y),
        (1.0 - f.x) * f.y,
        f.x * f.y);

    // Depth-aware upsample: weight each low-resolution sample by how closely its depth matches
    // this pixel's, so fog does not leak across geometry edges.
    float4 sum = float4(0.0);
    float sumWeight = 0.0;
    for (int i = 0; i < 4; ++i) {
        const float sampleDepth = getLinearDepth(
            depthTexture.sample(linearSampler, uvs[i]), cameraNear, cameraFar);
        const float w = bilinear[i] / (1.0 + 16.0 * abs(sampleDepth - depth) / max(depth, 0.001));
        sum += fogTexture.sample(linearSampler, uvs[i]) * w;
        sumWeight += w;
    }

    // rgb = in-scattered light, a = transmittance. The blend state resolves this to
    // `scene * transmittance + inscatter`.
    return sum / max(sumWeight, 0.0001);
}
)";

    constexpr const char* COMBINE_GLSL = R"(
#version 450

#ifdef VT_VERTEX_SHADER
layout(location = 0) in vec3 vertexPosition;
layout(location = 2) in vec2 vertexUv0;
layout(location = 0) out vec2 vUv;
void main() {
    vUv = vertexUv0;
    gl_Position = vec4(vertexPosition, 1.0);
}
#endif

#ifdef VT_FRAGMENT_SHADER
layout(set = 0, binding = 0) uniform FogCombineUniforms {
    vec4 textureSize;
    vec4 cameraParams;
} u;

layout(set = 1, binding = 0) uniform sampler2D depthTexture;
layout(set = 1, binding = 1) uniform sampler2D fogTexture;

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 fragColor;

float getLinearDepth(float rawDepth, float cameraNear, float cameraFar) {
    return (cameraNear * cameraFar) / (cameraFar - rawDepth * (cameraFar - cameraNear));
}

void main() {
    vec2 uv = clamp(vUv, vec2(0.0), vec2(1.0));
    float cameraNear = u.cameraParams.x;
    float cameraFar = u.cameraParams.y;

    float depth = getLinearDepth(texture(depthTexture, uv).r, cameraNear, cameraFar);

    vec2 texel = uv * u.textureSize.xy - 0.5;
    vec2 base = (floor(texel) + 0.5) * u.textureSize.zw;
    vec2 f = fract(texel);

    vec2 uvs[4];
    uvs[0] = base;
    uvs[1] = base + vec2(u.textureSize.z, 0.0);
    uvs[2] = base + vec2(0.0, u.textureSize.w);
    uvs[3] = base + u.textureSize.zw;

    vec4 bilinear = vec4(
        (1.0 - f.x) * (1.0 - f.y),
        f.x * (1.0 - f.y),
        (1.0 - f.x) * f.y,
        f.x * f.y);

    vec4 sum = vec4(0.0);
    float sumWeight = 0.0;
    for (int i = 0; i < 4; ++i) {
        float sampleDepth = getLinearDepth(
            texture(depthTexture, uvs[i]).r, cameraNear, cameraFar);
        float w = bilinear[i] / (1.0 + 16.0 * abs(sampleDepth - depth) / max(depth, 0.001));
        sum += texture(fogTexture, uvs[i]) * w;
        sumWeight += w;
    }

    fragColor = sum / max(sumWeight, 0.0001);
}
#endif
)";
}
