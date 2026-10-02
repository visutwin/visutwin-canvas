// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// The GPU lightmapper's post-bake quad passes, one source per language with the pass
// picked by a define:
//
//   LM_DILATE       upstream lightmapper/frag/dilate.js: an unbaked texel takes the first
//                   baked one of its eight neighbours, so bilinear filtering across a chart
//                   edge does not pull in the black outside the unwrap.
//   LM_DENOISE      upstream lightmapper/frag/bilateralDeNoise.js: a 15 x 15 bilateral
//                   filter over the baked texels only, its spatial kernel and range
//                   normaliser computed on the CPU (lightmapFilters.h).
//   LM_AMBIENT_AO   upstream bakeLmEnd.js under LIT_LIGHTMAP_BAKING_ADD_AMBIENT: the
//                   accumulated ambient visibility (slot 0) is shaped by the occlusion
//                   contrast and brightness, saturated, and multiplies the ambient light
//                   baked for the same texel (slot 1).
//   LM_COPY         a plain copy, for moving a result back into the lightmap's own target.
//
// A texel is BAKED when its alpha is above zero. DEVIATION: this port's lightmaps are
// RGBA16F, which is upstream's HDR mode, whose `isUsed` tests the colour instead
// (`any(rgb > 0)`) and so reads a baked texel that came out black — fully shadowed, no
// ambient — as empty, dilating over it and leaving it out of the denoise. The bake clears
// its targets to alpha 0 and writes alpha 1, which makes alpha the coverage, as it is in
// upstream's default RGBM mode (`rgbm.a > 0`).
//
// Layout, shared with every quad effect: sources on fragment slots 0 and 1 (MSL texture(0)
// and texture(1) / GLSL set 1 bindings 0 and 1, which are the first two entries of the
// material binding list), the uniforms on the material slot (MSL buffer(3) / GLSL set 0
// binding 0). Every tap lands on a texel centre, so the filter mode cannot change a value;
// MSL still declares a nearest sampler of its own.
//
#pragma once

namespace visutwin::canvas::lightmap_filter_shaders
{
    constexpr const char* LIGHTMAP_FILTER_MSL = R"(
#include <metal_stdlib>
using namespace metal;

struct QuadVertexIn {
    float3 position [[attribute(0)]];
    float3 normal   [[attribute(1)]];
    float2 uv0      [[attribute(2)]];
    float4 tangent  [[attribute(3)]];
    float2 uv1      [[attribute(4)]];
};

struct LightmapFilterVarying {
    float4 position [[position]];
    float2 uv;
};

struct LightmapFilterUniforms {
    float2 pixelOffset;
    float2 sigmas;
    float bZnorm;
    float occlusionContrast;
    float occlusionBrightness;
    float pad0;
    float4 kernelWeights[4];  // `kernel` is an MSL keyword
};

vertex LightmapFilterVarying lightmapFilterVertex(QuadVertexIn in [[stage_in]])
{
    LightmapFilterVarying out;
    out.position = float4(in.position, 1.0);
    out.uv = in.uv0;
    return out;
}

constexpr sampler lightmapFilterSampler(coord::normalized, filter::nearest, address::clamp_to_edge);

static inline bool isUsed(float4 pixel)
{
    return pixel.a > 0.0;
}

static inline float4 tap(texture2d<float> source, float2 uv)
{
    return source.sample(lightmapFilterSampler, uv, level(0));
}

#ifdef LM_DENOISE
static inline float normpdf3(float3 v, float sigma)
{
    return 0.39894 * exp(-0.5 * dot(v, v) / (sigma * sigma)) / sigma;
}

static inline float kernelAt(constant LightmapFilterUniforms& u, int index)
{
    return u.kernelWeights[index >> 2][index & 3];
}
#endif

fragment float4 lightmapFilterFragment(
    LightmapFilterVarying in [[stage_in]],
    constant LightmapFilterUniforms& u [[buffer(3)]],
    texture2d<float> source [[texture(0)]]
#ifdef LM_AMBIENT_AO
    , texture2d<float> ambientSource [[texture(1)]]
#endif
    )
{
    const float2 uv = in.uv;
#if defined(LM_DILATE)
    const float2 o = u.pixelOffset;
    float4 c = tap(source, uv);
    c = isUsed(c) ? c : tap(source, uv - o);
    c = isUsed(c) ? c : tap(source, uv + float2(0.0, -o.y));
    c = isUsed(c) ? c : tap(source, uv + float2(o.x, -o.y));
    c = isUsed(c) ? c : tap(source, uv + float2(-o.x, 0.0));
    c = isUsed(c) ? c : tap(source, uv + float2(o.x, 0.0));
    c = isUsed(c) ? c : tap(source, uv + float2(-o.x, o.y));
    c = isUsed(c) ? c : tap(source, uv + float2(0.0, o.y));
    c = isUsed(c) ? c : tap(source, uv + o);
    return c;
#elif defined(LM_DENOISE)
    const float4 pixel = tap(source, uv);
    // Lightmap-specific: unbaked texels pass through untouched, which keeps them unbaked
    // for the dilate that follows.
    if (!isUsed(pixel)) {
        return pixel;
    }
    const float bSigma = u.sigmas.y;
    const float3 pixelHdr = pixel.rgb;
    float3 accumulatedHdr = float3(0.0);
    float accumulatedFactor = 0.000001;
    const int kSize = (15 - 1) / 2;
    for (int i = -kSize; i <= kSize; ++i) {
        for (int j = -kSize; j <= kSize; ++j) {
            const float2 coord = uv + float2(float(i), float(j)) * u.pixelOffset;
            const float4 pix = tap(source, coord);
            if (isUsed(pix)) {
                const float3 hdr = pix.rgb;
                float factor = kernelAt(u, kSize + j) * kernelAt(u, kSize + i);
                factor *= normpdf3(hdr - pixelHdr, bSigma) * u.bZnorm;
                accumulatedHdr += factor * hdr;
                accumulatedFactor += factor;
            }
        }
    }
    return float4(accumulatedHdr / accumulatedFactor, 1.0);
#elif defined(LM_AMBIENT_AO)
    const float4 occlusion = tap(source, uv);
    const float4 ambient = tap(ambientSource, uv);
    float3 ao = ((occlusion.rgb - 0.5) * max(u.occlusionContrast + 1.0, 0.0)) + 0.5;
    ao += float3(u.occlusionBrightness);
    ao = saturate(ao);
    return float4(ao * ambient.rgb, ambient.a);
#else
    return tap(source, uv);
#endif
}
)";

    constexpr const char* LIGHTMAP_FILTER_GLSL = R"(
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
layout(set = 0, binding = 0) uniform LightmapFilterUniforms {
    vec2 pixelOffset;
    vec2 sigmas;
    float bZnorm;
    float occlusionContrast;
    float occlusionBrightness;
    float pad0;
    vec4 kernelWeights[4];
} u;
layout(set = 1, binding = 0) uniform sampler2D source;
#ifdef LM_AMBIENT_AO
layout(set = 1, binding = 1) uniform sampler2D ambientSource;
#endif
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 fragColor;

bool isUsed(vec4 pixel) {
    return pixel.a > 0.0;
}

vec4 tap(vec2 uv) {
    return textureLod(source, uv, 0.0);
}

#ifdef LM_DENOISE
float normpdf3(vec3 v, float sigma) {
    return 0.39894 * exp(-0.5 * dot(v, v) / (sigma * sigma)) / sigma;
}

float kernelAt(int index) {
    return u.kernelWeights[index >> 2][index & 3];
}
#endif

void main() {
    vec2 uv = vUv;
#if defined(LM_DILATE)
    vec2 o = u.pixelOffset;
    vec4 c = tap(uv);
    c = isUsed(c) ? c : tap(uv - o);
    c = isUsed(c) ? c : tap(uv + vec2(0.0, -o.y));
    c = isUsed(c) ? c : tap(uv + vec2(o.x, -o.y));
    c = isUsed(c) ? c : tap(uv + vec2(-o.x, 0.0));
    c = isUsed(c) ? c : tap(uv + vec2(o.x, 0.0));
    c = isUsed(c) ? c : tap(uv + vec2(-o.x, o.y));
    c = isUsed(c) ? c : tap(uv + vec2(0.0, o.y));
    c = isUsed(c) ? c : tap(uv + o);
    fragColor = c;
#elif defined(LM_DENOISE)
    vec4 pixel = tap(uv);
    // Lightmap-specific: unbaked texels pass through untouched, which keeps them unbaked
    // for the dilate that follows.
    if (!isUsed(pixel)) {
        fragColor = pixel;
        return;
    }
    float bSigma = u.sigmas.y;
    vec3 pixelHdr = pixel.rgb;
    vec3 accumulatedHdr = vec3(0.0);
    float accumulatedFactor = 0.000001;
    const int kSize = (15 - 1) / 2;
    for (int i = -kSize; i <= kSize; ++i) {
        for (int j = -kSize; j <= kSize; ++j) {
            vec2 coord = uv + vec2(float(i), float(j)) * u.pixelOffset;
            vec4 pix = tap(coord);
            if (isUsed(pix)) {
                vec3 hdr = pix.rgb;
                float factor = kernelAt(kSize + j) * kernelAt(kSize + i);
                factor *= normpdf3(hdr - pixelHdr, bSigma) * u.bZnorm;
                accumulatedHdr += factor * hdr;
                accumulatedFactor += factor;
            }
        }
    }
    fragColor = vec4(accumulatedHdr / accumulatedFactor, 1.0);
#elif defined(LM_AMBIENT_AO)
    vec4 occlusion = tap(uv);
    vec4 ambient = textureLod(ambientSource, uv, 0.0);
    vec3 ao = ((occlusion.rgb - 0.5) * max(u.occlusionContrast + 1.0, 0.0)) + 0.5;
    ao += vec3(u.occlusionBrightness);
    ao = clamp(ao, 0.0, 1.0);
    fragColor = vec4(ao * ambient.rgb, ambient.a);
#else
    fragColor = tap(uv);
#endif
}
#endif
)";
}
