// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 02.09.2026
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

// Point-sampled depth (AGENTS.md "Depth taps in a quad pass must be POINT
// sampled"): a bilinear tap across a silhouette returns a depth belonging to
// neither surface. The Vulkan backend binds a nearest sampler for every depth
// texture of a quad pass; this is the Metal twin of that.
constexpr sampler depthPointSampler(coord::normalized, filter::nearest,
                                    mip_filter::none, address::clamp_to_edge);

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
    // The quad's v runs DOWN the screen (row 0 at the top), so NDC y is 1 - 2v.
    const float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    const float3 rayDir = normalize(
        (u.invView * float4(ndc * u.projScale.xy, -1.0, 0.0)).xyz);

    // Distance along the ray to the scene surface. The depth buffer stores distance along the
    // view axis, so divide by the ray's projection onto the forward vector.
    const float rawDepth = depthTexture.sample(depthPointSampler, uv);
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

    // The quad's v runs DOWN the screen (row 0 at the top), so NDC y is 1 - 2v.
    vec2 ndc = vec2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
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

// Point-sampled depth (AGENTS.md "Depth taps in a quad pass must be POINT
// sampled"): a bilinear tap across a silhouette returns a depth belonging to
// neither surface. The Vulkan backend binds a nearest sampler for every depth
// texture of a quad pass; this is the Metal twin of that.
constexpr sampler depthPointSampler(coord::normalized, filter::nearest,
                                    mip_filter::none, address::clamp_to_edge);

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

    const float depth = getLinearDepth(depthTexture.sample(depthPointSampler, uv), cameraNear, cameraFar);

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
            depthTexture.sample(depthPointSampler, uvs[i]), cameraNear, cameraFar);
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

    /**
     * Local light uniforms, one block per light.
     * 352 bytes, every member a float4 / float4x4 so the MSL and std140 layouts agree.
     */
    struct alignas(16) FogLocalUniforms
    {
        float invView[16];          // offset   0
        float lightProjMatrix[16];  // offset  64  a spot: world -> its atlas slot (shadow and cookie)
        float cameraPosition[4];    // offset 128  xyz
        float cameraForward[4];     // offset 144  xyz
        float projScale[4];         // offset 160  xy, z = camera near, w = camera far
        float tint[4];              // offset 176  xyz
        float fogParams[4];         // offset 192  density, height base, height falloff, max distance
        float marchParams[4];       // offset 208  anisotropy, steps, noise offset, extinction
        float lightPosRange[4];     // offset 224  xyz position, w range
        float lightSphere[4];       // offset 240  xyz centre, w radius of the volume marched
        float lightColor[4];        // offset 256  xyz, pre-scaled by intensity and exposure
        float lightDir[4];          // offset 272  xyz spot axis, w 1 spot / 0 omni
        float lightSpot[4];         // offset 288  inner cos, outer cos, shadow intensity, cookie intensity
        float lightAtten[4];        // offset 304  x 1 linear falloff, y cookie channel, z atlas resolution
        float lightAtlas[4];        // offset 320  an omni's atlas rect (x, y, size, edge pixels)
        float omniDepth[4];         // offset 336  an omni's shadow near, far, relative bias
    };
    static_assert(sizeof(FogLocalUniforms) == 352);

    // The in-scattering of one clustered spot or omni light, added into the fog texture.
    // The pass draws a fullscreen triangle per light,
    // scissored to the screen bounds of its volume, and marches each pixel's ray over the
    // part inside the light's bounding sphere — clipped to the cone for a spot — sampling
    // the clustered shadow and cookie atlases. Their lookups are twins of the cluster
    // loop's (common-shadow-pcf: a spot projects into its rect with no shader bias, an
    // omni face stores perspective depth with a RELATIVE bias), one tap a step offset on
    // a spiral.
    constexpr const char* LOCAL_MSL = R"(
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

struct FogLocalUniforms {
    float4x4 invView;
    float4x4 lightProjMatrix;
    float4 cameraPosition;
    float4 cameraForward;
    float4 projScale;
    float4 tint;
    float4 fogParams;
    float4 marchParams;
    float4 lightPosRange;
    float4 lightSphere;
    float4 lightColor;
    float4 lightDir;
    float4 lightSpot;
    float4 lightAtten;
    float4 lightAtlas;
    float4 omniDepth;
};

vertex FogVarying fogLocalVertex(ComposeVertexIn in [[stage_in]])
{
    FogVarying out;
    out.position = float4(in.position, 1.0);
    out.uv = in.uv0;
    return out;
}

constexpr sampler depthPointSampler(coord::normalized, filter::nearest,
                                    mip_filter::none, address::clamp_to_edge);
constexpr sampler atlasCompareSampler(coord::normalized, filter::linear,
                                      address::clamp_to_edge, compare_func::less_equal);
constexpr sampler cookieSampler(coord::normalized, filter::linear, address::clamp_to_edge);

static inline float fogNoise(float2 fragCoord)
{
    const float3 magic = float3(0.06711056, 0.00583715, 52.9829189);
    return fract(magic.z * fract(dot(fragCoord, magic.xy)));
}

static inline float fogPhase(float cosTheta, float g)
{
    const float g2 = g * g;
    const float denom = 1.0 + g2 - 2.0 * g * cosTheta;
    return (1.0 - g2) / (12.56637 * denom * sqrt(max(denom, 1e-6)));
}

// Falloffs, twins of common-falloff.metal.
static inline float fogFalloffLinear(float range, float3 lightVec)
{
    return max((range - length(lightVec)) / max(range, 1e-4), 0.0);
}

static inline float fogFalloffInvSquared(float range, float3 lightVec)
{
    const float sqrDist = dot(lightVec, lightVec);
    const float invRadius = 1.0 / max(range, 1e-4);
    const float window = saturate(1.0 - (sqrDist * invRadius * invRadius) * (sqrDist * invRadius * invRadius));
    return 16.0 / (sqrDist + 1.0) * window * window;
}

// An omni light's cube face in the atlas (twin of common-shadow-pcf's
// getCubemapFaceCoordinates / getCubemapAtlasCoordinates).
static inline float2 fogCubemapAtlasCoordinates(float4 rect, float resolution, float3 dir)
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

static inline float volSampleShadow(float3 pos, float3 lightVec, float spiral, constant FogLocalUniforms& u,
                                    depth2d<float> atlas)
{
    const float resolution = u.lightAtten.z;
    const float2 tapOffset = float2(cos(spiral), sin(spiral)) / resolution;
    if (u.lightDir.w > 0.0) {
        const float4 projPos = u.lightProjMatrix * float4(pos, 1.0);
        const float3 coord = projPos.xyz / projPos.w;
        return atlas.sample_compare(atlasCompareSampler, coord.xy + tapOffset, coord.z, level(0));
    }
    const float2 uv = fogCubemapAtlasCoordinates(u.lightAtlas, resolution, lightVec);
    const float3 absDir = abs(lightVec);
    const float d = max(absDir.x, max(absDir.y, absDir.z)) * (1.0 - u.omniDepth.z);
    const float compareValue = u.omniDepth.y * (d - u.omniDepth.x) / max((u.omniDepth.y - u.omniDepth.x) * d, 1e-6);
    return atlas.sample_compare(atlasCompareSampler, uv + tapOffset, compareValue, level(0));
}

static inline float3 volSampleCookie(float3 pos, float3 lightVec, constant FogLocalUniforms& u,
                                     texture2d<float> cookieAtlas)
{
    float2 uv;
    if (u.lightDir.w > 0.0) {
        const float4 projected = u.lightProjMatrix * float4(pos, 1.0);
        uv = projected.xy / max(projected.w, 1e-6);
    } else {
        uv = fogCubemapAtlasCoordinates(u.lightAtlas, u.lightAtten.z, lightVec);
    }
    const float4 texel = cookieAtlas.sample(cookieSampler, uv, level(0));
    float3 channel = texel.rgb;
    const uint cookieChannel = uint(u.lightAtten.y + 0.5);
    if (cookieChannel == 1u) channel = float3(texel.r);
    else if (cookieChannel == 2u) channel = float3(texel.g);
    else if (cookieChannel == 3u) channel = float3(texel.b);
    else if (cookieChannel == 4u) channel = float3(texel.a);
    return mix(float3(1.0), channel, u.lightSpot.w);
}

// Optical depth of the height fog over a part of the ray entirely below or entirely
// above the base height; h0 is the camera's height above the base.
static inline float volFogSegmentDepth(float s0, float s1, float h0, float rayDirY, constant FogLocalUniforms& u)
{
    if (s1 <= s0) return 0.0;
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

static inline float volFogOpticalDepth(float t, float rayDirY, constant FogLocalUniforms& u)
{
    const float h0 = u.cameraPosition.y - u.fogParams.y;
    const float sCross = abs(rayDirY) > 1e-6 ? clamp(-h0 / rayDirY, 0.0, t) : 0.0;
    return (volFogSegmentDepth(0.0, sCross, h0, rayDirY, u) + volFogSegmentDepth(sCross, t, h0, rayDirY, u)) *
        u.marchParams.w;
}

static inline float2 volClipCone(float2 span, float root, float gradient, float axial)
{
    if (axial < 0.0) return span;
    return gradient > 0.0 ? float2(max(span.x, root), span.y) : float2(span.x, min(span.y, root));
}

fragment float4 fogLocalFragment(
    FogVarying in [[stage_in]],
    depth2d<float> depthTexture [[texture(0)]],
    depth2d<float> shadowAtlas [[texture(1)]],
    texture2d<float> cookieAtlas [[texture(2)]],
    constant FogLocalUniforms& u [[buffer(3)]])
{
    const float2 uv = clamp(in.uv, float2(0.0), float2(1.0));
    const float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    const float3 rayDir = normalize((u.invView * float4(ndc * u.projScale.xy, -1.0, 0.0)).xyz);

    // Distance along the ray to the scene surface.
    const float rawDepth = depthTexture.sample(depthPointSampler, uv);
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
    if (discriminant <= 0.0) discard_fragment();
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
            discard_fragment();
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
    if (t1 <= t0) discard_fragment();

    const float stepCount = max(u.marchParams.y, 1.0);
    const float dt = (t1 - t0) / stepCount;
    const float noise = fract(fogNoise(in.position.xy) + u.marchParams.z);

    float3 inscatter = float3(0.0);
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
                atten *= mix(1.0, volSampleShadow(pos, lightVec, (i + noise) * 2.39996, u, shadowAtlas), u.lightSpot.z);
            }
            float3 radiance = u.lightColor.xyz;
            if (u.lightSpot.w > 0.0) {
                radiance *= volSampleCookie(pos, lightVec, u, cookieAtlas);
            }
            // The samples are not contiguous, so the transmittance is analytic.
            const float transmittance = exp(-volFogOpticalDepth(t, rayDir.y, u));
            const float phase = fogPhase(dot(rayDir, -lightDirNorm), u.marchParams.x);
            inscatter += transmittance * u.tint.xyz * radiance * (atten * phase * density * dt);
        }
    }
    // Only the in-scattered light is added; the blend keeps the transmittance the
    // directional march wrote.
    return float4(inscatter, 1.0);
}
)";

    // DEVIATION from the MSL path, as the directional march: the shadow taps are MANUAL
    // depth comparisons (the Vulkan backend binds no comparison sampler), single taps
    // where Metal's hardware compare filters bilinearly.
    constexpr const char* LOCAL_GLSL = R"(
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
layout(set = 0, binding = 0) uniform FogLocalUniforms {
    mat4 invView;
    mat4 lightProjMatrix;
    vec4 cameraPosition;
    vec4 cameraForward;
    vec4 projScale;
    vec4 tint;
    vec4 fogParams;
    vec4 marchParams;
    vec4 lightPosRange;
    vec4 lightSphere;
    vec4 lightColor;
    vec4 lightDir;
    vec4 lightSpot;
    vec4 lightAtten;
    vec4 lightAtlas;
    vec4 omniDepth;
} u;

layout(set = 1, binding = 0) uniform sampler2D depthTexture;
layout(set = 1, binding = 1) uniform sampler2D shadowAtlas;
layout(set = 1, binding = 2) uniform sampler2D cookieAtlas;

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 fragColor;

float fogNoise(vec2 fragCoord) {
    const vec3 magic = vec3(0.06711056, 0.00583715, 52.9829189);
    return fract(magic.z * fract(dot(fragCoord, magic.xy)));
}

float fogPhase(float cosTheta, float g) {
    float g2 = g * g;
    float denom = 1.0 + g2 - 2.0 * g * cosTheta;
    return (1.0 - g2) / (12.56637 * denom * sqrt(max(denom, 1e-6)));
}

float fogFalloffLinear(float range, vec3 lightVec) {
    return max((range - length(lightVec)) / max(range, 1e-4), 0.0);
}

float fogFalloffInvSquared(float range, vec3 lightVec) {
    float sqrDist = dot(lightVec, lightVec);
    float invRadius = 1.0 / max(range, 1e-4);
    float x = sqrDist * invRadius * invRadius;
    float window = clamp(1.0 - x * x, 0.0, 1.0);
    return 16.0 / (sqrDist + 1.0) * window * window;
}

vec2 fogCubemapAtlasCoordinates(vec4 rect, float resolution, vec3 dir) {
    vec3 vAbs = abs(dir);
    float ma;
    vec2 uv;
    vec2 tileOffset;
    if (vAbs.z >= vAbs.x && vAbs.z >= vAbs.y) {
        ma = 0.5 / vAbs.z;
        uv = vec2(dir.z < 0.0 ? -dir.x : dir.x, dir.y);
        tileOffset = vec2(2.0, dir.z < 0.0 ? 1.0 : 0.0);
    } else if (vAbs.y >= vAbs.x) {
        ma = 0.5 / vAbs.y;
        uv = vec2(dir.x, dir.y < 0.0 ? dir.z : -dir.z);
        tileOffset = vec2(1.0, dir.y < 0.0 ? 1.0 : 0.0);
    } else {
        ma = 0.5 / vAbs.x;
        uv = vec2(dir.x < 0.0 ? dir.z : -dir.z, dir.y);
        tileOffset = vec2(0.0, dir.x < 0.0 ? 1.0 : 0.0);
    }
    uv = uv * ma + 0.5;
    float faceSize = rect.z / 3.0;
    float offset = rect.w / max(resolution * faceSize, 1.0);
    uv = uv * (1.0 - 2.0 * offset) + offset;
    return uv * faceSize + tileOffset * faceSize + rect.xy;
}

float volSampleShadow(vec3 pos, vec3 lightVec, float spiral) {
    float resolution = u.lightAtten.z;
    vec2 tapOffset = vec2(cos(spiral), sin(spiral)) / resolution;
    if (u.lightDir.w > 0.0) {
        vec4 projPos = u.lightProjMatrix * vec4(pos, 1.0);
        vec3 coord = projPos.xyz / projPos.w;
        return coord.z <= textureLod(shadowAtlas, coord.xy + tapOffset, 0.0).r ? 1.0 : 0.0;
    }
    vec2 uv = fogCubemapAtlasCoordinates(u.lightAtlas, resolution, lightVec);
    vec3 absDir = abs(lightVec);
    float d = max(absDir.x, max(absDir.y, absDir.z)) * (1.0 - u.omniDepth.z);
    float compareValue = u.omniDepth.y * (d - u.omniDepth.x) / max((u.omniDepth.y - u.omniDepth.x) * d, 1e-6);
    return compareValue <= textureLod(shadowAtlas, uv + tapOffset, 0.0).r ? 1.0 : 0.0;
}

vec3 volSampleCookie(vec3 pos, vec3 lightVec) {
    vec2 uv;
    if (u.lightDir.w > 0.0) {
        vec4 projected = u.lightProjMatrix * vec4(pos, 1.0);
        uv = projected.xy / max(projected.w, 1e-6);
    } else {
        uv = fogCubemapAtlasCoordinates(u.lightAtlas, u.lightAtten.z, lightVec);
    }
    vec4 texel = textureLod(cookieAtlas, uv, 0.0);
    vec3 channel = texel.rgb;
    uint cookieChannel = uint(u.lightAtten.y + 0.5);
    if (cookieChannel == 1u) channel = vec3(texel.r);
    else if (cookieChannel == 2u) channel = vec3(texel.g);
    else if (cookieChannel == 3u) channel = vec3(texel.b);
    else if (cookieChannel == 4u) channel = vec3(texel.a);
    return mix(vec3(1.0), channel, u.lightSpot.w);
}

float volFogSegmentDepth(float s0, float s1, float h0, float rayDirY) {
    if (s1 <= s0) return 0.0;
    float density = u.fogParams.x;
    float falloff = u.fogParams.z;
    float hMid = h0 + rayDirY * (s0 + s1) * 0.5;
    if (hMid <= 0.0) {
        return density * (s1 - s0);
    }
    float scale = falloff * rayDirY;
    if (abs(scale) < 1e-6) {
        return density * exp(-falloff * hMid) * (s1 - s0);
    }
    return density * (exp(-falloff * (h0 + rayDirY * s0)) - exp(-falloff * (h0 + rayDirY * s1))) / scale;
}

float volFogOpticalDepth(float t, float rayDirY) {
    float h0 = u.cameraPosition.y - u.fogParams.y;
    float sCross = abs(rayDirY) > 1e-6 ? clamp(-h0 / rayDirY, 0.0, t) : 0.0;
    return (volFogSegmentDepth(0.0, sCross, h0, rayDirY) + volFogSegmentDepth(sCross, t, h0, rayDirY)) *
        u.marchParams.w;
}

vec2 volClipCone(vec2 span, float root, float gradient, float axial) {
    if (axial < 0.0) return span;
    return gradient > 0.0 ? vec2(max(span.x, root), span.y) : vec2(span.x, min(span.y, root));
}

void main() {
    vec2 uv = clamp(vUv, vec2(0.0), vec2(1.0));
    vec2 ndc = vec2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    vec3 rayDir = normalize((u.invView * vec4(ndc * u.projScale.xy, -1.0, 0.0)).xyz);

    float rawDepth = textureLod(depthTexture, uv, 0.0).r;
    float cameraNear = u.projScale.z;
    float cameraFar = u.projScale.w;
    float sceneDepth = (cameraNear * cameraFar) / (cameraFar - rawDepth * (cameraFar - cameraNear));
    float rayDot = max(dot(rayDir, u.cameraForward.xyz), 0.001);
    float sceneT = min(sceneDepth / rayDot, u.fogParams.w);

    vec3 sphereToCam = u.cameraPosition.xyz - u.lightSphere.xyz;
    float halfB = dot(sphereToCam, rayDir);
    float c = dot(sphereToCam, sphereToCam) - u.lightSphere.w * u.lightSphere.w;
    float discriminant = halfB * halfB - c;
    if (discriminant <= 0.0) discard;
    float rootOffset = sqrt(discriminant);
    float t0 = max(-halfB - rootOffset, 0.0);
    float t1 = min(-halfB + rootOffset, sceneT);

    if (u.lightDir.w > 0.0) {
        vec3 apexToCam = u.cameraPosition.xyz - u.lightPosRange.xyz;
        float axisStart = dot(apexToCam, u.lightDir.xyz);
        float axisRate = dot(rayDir, u.lightDir.xyz);
        if (abs(axisRate) > 1e-6) {
            float tApex = -axisStart / axisRate;
            float tRange = (u.lightPosRange.w - axisStart) / axisRate;
            t0 = max(t0, min(tApex, tRange));
            t1 = min(t1, max(tApex, tRange));
        } else if (axisStart < 0.0 || axisStart > u.lightPosRange.w) {
            discard;
        }
        float cosSqr = u.lightSpot.y * u.lightSpot.y;
        float qa = axisRate * axisRate - cosSqr;
        float qb = axisRate * axisStart - cosSqr * dot(rayDir, apexToCam);
        float qc = axisStart * axisStart - cosSqr * dot(apexToCam, apexToCam);
        vec2 span = vec2(t0, t1);
        if (abs(qa) > 1e-6) {
            float discriminantCone = qb * qb - qa * qc;
            if (discriminantCone > 0.0) {
                float rootOffsetCone = sqrt(discriminantCone);
                float rootA = (-qb - rootOffsetCone) / qa;
                float rootB = (-qb + rootOffsetCone) / qa;
                span = volClipCone(span, rootA, qa * rootA + qb, axisStart + rootA * axisRate);
                span = volClipCone(span, rootB, qa * rootB + qb, axisStart + rootB * axisRate);
            }
        } else if (abs(qb) > 1e-6) {
            float root = -0.5 * qc / qb;
            span = volClipCone(span, root, qb, axisStart + root * axisRate);
        }
        t0 = span.x;
        t1 = span.y;
    }
    if (t1 <= t0) discard;

    float stepCount = max(u.marchParams.y, 1.0);
    float dt = (t1 - t0) / stepCount;
    float noise = fract(fogNoise(gl_FragCoord.xy) + u.marchParams.z);

    vec3 inscatter = vec3(0.0);
    for (float i = 0.0; i < stepCount; i += 1.0) {
        float t = t0 + (i + noise) * dt;
        vec3 pos = u.cameraPosition.xyz + rayDir * t;
        float density = u.fogParams.x * exp(-u.fogParams.z * max(pos.y - u.fogParams.y, 0.0));

        vec3 lightVec = pos - u.lightPosRange.xyz;
        vec3 lightDirNorm = normalize(lightVec);
        float atten = u.lightAtten.x > 0.0 ? fogFalloffLinear(u.lightPosRange.w, lightVec)
                                           : fogFalloffInvSquared(u.lightPosRange.w, lightVec);
        if (u.lightDir.w > 0.0) {
            atten *= smoothstep(u.lightSpot.y, u.lightSpot.x, dot(lightDirNorm, u.lightDir.xyz));
        }
        if (atten > 0.00001) {
            if (u.lightSpot.z > 0.0) {
                atten *= mix(1.0, volSampleShadow(pos, lightVec, (i + noise) * 2.39996), u.lightSpot.z);
            }
            vec3 radiance = u.lightColor.xyz;
            if (u.lightSpot.w > 0.0) {
                radiance *= volSampleCookie(pos, lightVec);
            }
            float transmittance = exp(-volFogOpticalDepth(t, rayDir.y));
            float phase = fogPhase(dot(rayDir, -lightDirNorm), u.marchParams.x);
            inscatter += transmittance * u.tint.xyz * radiance * (atten * phase * density * dt);
        }
    }
    fragColor = vec4(inscatter, 1.0);
}
#endif
)";
}
