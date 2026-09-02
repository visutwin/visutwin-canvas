// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Compose-pass shaders in both languages the engine speaks, selected by
// GraphicsDevice::shaderLanguage() and driven through QuadRender — one
// implementation of the post chain instead of a pass class per backend.
//
// Chain order (mirrors upstream compose.js):
//   CAS -> SSAO -> DOF -> Bloom -> Fringing -> ColorEnhance -> Grading
//   -> ToneMap -> ColorLUT -> Vignette -> display gamma
//
// Textures (quad slots, both backends): 0 scene, 1 bloom, 2 ssao, 3 depth,
// 4 colorLUT, 5 colorLUT2. The Metal shader used to declare a coc and a blur
// texture for a multi-pass DOF path that has been commented out for a long
// time — only applyDofSinglePass runs — so those two dead bindings are gone.
//
#pragma once

#include <cstdint>

namespace visutwin::canvas::compose_shaders
{
    struct alignas(16) ComposeUniforms
    {
        uint32_t dofEnabled = 0u;
        uint32_t taaEnabled = 0u;
        uint32_t ssaoEnabled = 0u;
        uint32_t bloomEnabled = 0u;
        uint32_t blurTextureUpscale = 0u;
        float bloomIntensity = 0.01f;
        float dofIntensity = 1.0f;
        float sharpness = 0.0f;
        uint32_t tonemapMode = 0u;
        float exposure = 1.0f;
        float sceneTextureInvRes[2] = {0.0f, 0.0f};
        // Single-pass DOF parameters
        float dofFocusDistance = 1.0f;
        float dofFocusRange = 0.5f;
        float dofBlurRadius = 3.0f;
        float dofCameraNear = 0.01f;
        float dofCameraFar = 100.0f;
        float _pad0 = 0.0f;  // padding to maintain alignment
        // Vignette
        uint32_t vignetteEnabled = 0u;
        float vignetteInner = 0.5f;
        float vignetteOuter = 1.0f;
        float vignetteCurvature = 0.5f;
        float vignetteIntensity = 0.3f;
        float vignetteColorR = 0.0f;
        float vignetteColorG = 0.0f;
        float vignetteColorB = 0.0f;
        // Fringing
        float fringingIntensity = 0.0f;
        // Color grading
        uint32_t gradingEnabled = 0u;
        float gradingBrightness = 1.0f;
        float gradingContrast = 1.0f;
        float gradingSaturation = 1.0f;
        float gradingTintR = 1.0f;
        float gradingTintG = 1.0f;
        float gradingTintB = 1.0f;
        // Color enhance
        uint32_t colorEnhanceEnabled = 0u;
        float ceShadows = 0.0f;
        float ceHighlights = 0.0f;
        float ceVibrance = 0.0f;
        float ceDehaze = 0.0f;
        float ceMidtones = 0.0f;
        // Color LUT
        uint32_t lutEnabled = 0u;
        uint32_t lut2Enabled = 0u;
        float lutIntensity1 = 1.0f;
        float lutIntensity2 = 1.0f;
        float lutBlend = 0.0f;
    };

    // A block of scalars (plus one vec2) packs identically under MSL and
    // std140, so both shaders declare this same field list.
    static_assert(sizeof(ComposeUniforms) <= 512, "must fit kPerDrawUniformCapacity");

    constexpr const char* COMPOSE_MSL = R"(
#include <metal_stdlib>
using namespace metal;

struct ComposeVertexIn {
    float3 position [[attribute(0)]];
    float3 normal [[attribute(1)]];
    float2 uv0 [[attribute(2)]];
    float4 tangent [[attribute(3)]];
    float2 uv1 [[attribute(4)]];
};

struct ComposeVarying {
    float4 position [[position]];
    float2 uv;
};

struct ComposeUniforms {
    uint dofEnabled;
    uint taaEnabled;
    uint ssaoEnabled;
    uint bloomEnabled;
    uint blurTextureUpscale;
    float bloomIntensity;
    float dofIntensity;
    float sharpness;
    uint tonemapMode;
    float exposure;
    float2 sceneTextureInvRes;
    // Single-pass DOF parameters
    float dofFocusDistance;
    float dofFocusRange;
    float dofBlurRadius;
    float dofCameraNear;
    float dofCameraFar;
    float _pad0;  // padding to maintain 8-byte alignment for next field
    // Vignette (use float4 for color to match C++ alignment)
    uint vignetteEnabled;
    float vignetteInner;
    float vignetteOuter;
    float vignetteCurvature;
    float vignetteIntensity;
    float vignetteColorR;
    float vignetteColorG;
    float vignetteColorB;
    // Fringing (chromatic aberration)
    float fringingIntensity;
    // Color grading (pre-tonemap)
    uint gradingEnabled;
    float gradingBrightness;
    float gradingContrast;
    float gradingSaturation;
    float gradingTintR;
    float gradingTintG;
    float gradingTintB;
    // Color enhance (pre-tonemap)
    uint colorEnhanceEnabled;
    float ceShadows;
    float ceHighlights;
    float ceVibrance;
    float ceDehaze;
    float ceMidtones;
    // 3D color LUT (post-tonemap)
    uint lutEnabled;
    uint lut2Enabled;
    float lutIntensity1;
    float lutIntensity2;
    float lutBlend;
};

float3 toneMapLinear(float3 color, float exposure) {
    return color * exposure;
}

float3 toneMapAces(float3 color, float exposure) {
    const float tA = 2.51;
    const float tB = 0.03;
    const float tC = 2.43;
    const float tD = 0.59;
    const float tE = 0.14;
    float3 x = color * exposure;
    return (x * (tA * x + tB)) / (x * (tC * x + tD) + tE);
}

// https://modelviewer.dev/examples/tone-mapping
float3 toneMapNeutral(float3 color, float exposure) {
    color *= exposure;

    float startCompression = 0.8 - 0.04;
    float desaturation = 0.15;

    float x = min(color.r, min(color.g, color.b));
    float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
    color -= offset;

    float peak = max(color.r, max(color.g, color.b));
    if (peak < startCompression) return color;

    float d = 1.0 - startCompression;
    float newPeak = 1.0 - d * d / (peak + d - startCompression);
    color *= newPeak / peak;

    float g = 1.0 - 1.0 / (desaturation * (peak - newPeak) + 1.0);
    return mix(color, float3(newPeak), g);
}

// Uncharted 2 filmic operator (upstream TONEMAP_FILMIC).
float3 uncharted2Tonemap(float3 x) {
    const float A = 0.15, B = 0.50, C = 0.10, D = 0.20, E = 0.02, F = 0.30;
    return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
}

float3 toneMapFilmic(float3 color, float exposure) {
    const float W = 11.2;
    color = uncharted2Tonemap(color * exposure * 2.0);
    float3 whiteScale = 1.0 / uncharted2Tonemap(float3(W));
    return color * whiteScale;
}

// Hejl/Burgess-Dawson operator (upstream TONEMAP_HEJL).
float3 toneMapHejl(float3 color, float exposure) {
    color *= exposure;
    const float A = 0.22, B = 0.3, C = 0.1, D = 0.2, E = 0.01, F = 0.3;
    const float scl = 1.25;
    float3 h = max(float3(0.0), color - 0.004);
    return (h * ((scl * A) * h + scl * (C * B)) + scl * (D * E))
         / (h * (A * h + B) + (D * F))
         - scl * (E / F);
}

// ACES fit by Stephen Hill (upstream TONEMAP_ACES2) — RRT+ODT polynomial.
float3 RRTAndODTFit(float3 v) {
    float3 a = v * (v + 0.0245786) - 0.000090537;
    float3 b = v * (0.983729 * v + 0.4329510) + 0.238081;
    return a / b;
}

float3 toneMapAces2(float3 color, float exposure) {
    const float3x3 ACESInputMat = float3x3(
        float3(0.59719, 0.35458, 0.04823),
        float3(0.07600, 0.90834, 0.01566),
        float3(0.02840, 0.13383, 0.83777));
    const float3x3 ACESOutputMat = float3x3(
        float3( 1.60475, -0.53108, -0.07367),
        float3(-0.10208,  1.10813, -0.00605),
        float3(-0.00327, -0.07276,  1.07602));
    color *= exposure / 0.6;
    color = color * ACESInputMat;
    color = RRTAndODTFit(color);
    color = color * ACESOutputMat;
    return clamp(color, float3(0.0), float3(1.0));
}

float maxComp(float x, float y, float z) { return max(x, max(y, z)); }
float3 toSDR(float3 c) { return c / (1.0 + maxComp(c.r, c.g, c.b)); }
float3 toHDR(float3 c) { return c / (1.0 - maxComp(c.r, c.g, c.b)); }

float3 applyCas(float3 color, float2 uv, float sharpness,
                texture2d<float> sceneTexture, sampler s, float2 invRes) {
    float3 a = toSDR(sceneTexture.sample(s, uv + float2(0.0, -invRes.y)).rgb);
    float3 b = toSDR(sceneTexture.sample(s, uv + float2(-invRes.x, 0.0)).rgb);
    float3 c = toSDR(color);
    float3 d = toSDR(sceneTexture.sample(s, uv + float2(invRes.x, 0.0)).rgb);
    float3 e = toSDR(sceneTexture.sample(s, uv + float2(0.0, invRes.y)).rgb);

    float min_g = min(a.g, min(b.g, min(c.g, min(d.g, e.g))));
    float max_g = max(a.g, max(b.g, max(c.g, max(d.g, e.g))));
    float sharpening_amount = sqrt(min(1.0 - max_g, min_g) / max_g);
    float w = sharpening_amount * sharpness;
    float3 res = (w * (a + b + d + e) + c) / (4.0 * w + 1.0);
    return toHDR(max(res, float3(0.0)));
}

vertex ComposeVarying composeVertex(ComposeVertexIn in [[stage_in]])
{
    ComposeVarying out;
    out.position = float4(in.position, 1.0);
    out.uv = in.uv0;
    return out;
}

// Fringing (chromatic aberration): shift red/blue by distance-squared from center
// (upstream compose-fringing.js).
float3 applyFringing(float3 color, float2 uv, float intensity,
                     texture2d<float> sceneTexture, sampler s) {
    float2 centerDistance = uv - 0.5;
    float2 offset = intensity * centerDistance * centerDistance;
    color.r = sceneTexture.sample(s, uv - offset).r;
    color.b = sceneTexture.sample(s, uv + offset).b;
    return color;
}

// HDR color grading (upstream compose-grading.js); 1.0 = no change for all parameters.
float3 applyGrading(float3 color, float brt, float sat, float con, float3 tint) {
    color *= tint;
    color = color * brt;
    float grey = dot(color, float3(0.3, 0.59, 0.11));
    grey = grey / max(1.0, max(color.r, max(color.g, color.b)));  // normalize luminance in HDR
    color = mix(float3(grey), color, sat);
    return mix(float3(0.5), color, con);
}

// Color enhance (upstream compose-color-enhance.js): shadows/highlights, midtones,
// vibrance and dark-channel dehaze — all in HDR before tonemapping.
float3 applyColorEnhance(float3 color, float shadows, float highlights,
                         float vibrance, float dehaze, float midtones) {
    float lum = dot(color, float3(0.2126, 0.7152, 0.0722));

    // Shadows/Highlights: exponential curve, pow(2, param) = ±1 stop at ±1
    if (shadows != 0.0 || highlights != 0.0) {
        float logLum = clamp(log2(max(lum, 0.001)) / 10.0 + 0.5, 0.0, 1.0);
        float shadowWeight = pow(1.0 - logLum, 2.0);
        float highlightWeight = pow(logLum, 2.0);
        color *= pow(2.0, shadows * shadowWeight);
        color *= pow(2.0, highlights * highlightWeight);
    }

    // Midtones: localized exposure in log-luminance space
    if (midtones != 0.0) {
        const float pivot = 0.18;
        const float widthStops = 1.25;
        const float maxStops = 2.0;
        float y = max(dot(color, float3(0.2126, 0.7152, 0.0722)), 1e-6);
        float d = log2(y / pivot);
        float w = exp(-(d * d) / (2.0 * widthStops * widthStops));
        color *= exp2(midtones * maxStops * w);
    }

    // Vibrance: boost saturation of muted colors only
    if (vibrance != 0.0) {
        float minChannel = min(color.r, min(color.g, color.b));
        float maxChannel = max(color.r, max(color.g, color.b));
        float sat = (maxChannel - minChannel) / max(maxChannel, 0.001);
        lum = dot(color, float3(0.2126, 0.7152, 0.0722));
        float normalizedLum = lum / max(1.0, maxChannel);
        float3 grey = float3(normalizedLum) * maxChannel;
        float satBoost = vibrance * (1.0 - sat);
        color = mix(grey, color, 1.0 + satBoost);
    }

    // Dehaze: dark channel prior — haze lifts the minimum RGB channel
    if (dehaze != 0.0) {
        float maxChannel = max(color.r, max(color.g, color.b));
        float scale = max(1.0, maxChannel);
        float3 normalized = color / scale;
        float darkChannel = min(normalized.r, min(normalized.g, normalized.b));
        const float atmosphericLight = 0.95;
        float t = max(1.0 - dehaze * darkChannel / atmosphericLight, 0.1);
        float3 dehazed = (normalized - atmosphericLight) / t + atmosphericLight;
        color = dehazed * scale;
    }

    return max(float3(0.0), color);
}

// 3D color LUT via a 256x16 "horizontal strip" (unwrapped 16^3, Unreal format) —
// upstream compose-color-lut.js. Applied post-tonemap: the lookup coordinate is
// sRGB-encoded; sampled values are sRGB too (DEVIATION: the port loads LUTs as
// non-sRGB RGBA8, so the sample is decoded to linear here instead of by the sampler).
float3 sampleColorLUT(texture2d<float> lut, sampler s, float2 uv_l, float2 uv_h, float t) {
    float3 color_l = lut.sample(s, uv_l, level(0.0)).rgb;
    float3 color_h = lut.sample(s, uv_h, level(0.0)).rgb;
    float3 srgb = mix(color_l, color_h, t);
    return pow(max(srgb, float3(0.0)) + 0.0000001, float3(2.2));
}

float3 applyColorLUT(float3 color, texture2d<float> lut1, texture2d<float> lut2,
                     sampler s, uint lut2Enabled, float intensity1, float intensity2,
                     float blend) {
    const float LUT_N = 16.0;
    const float LUT_MAX = LUT_N - 1.0;
    const float LUT_HALF_PX_X = 0.5 / 256.0;
    const float LUT_HALF_PX_Y = 0.5 / LUT_N;
    const float LUT_R_SCALE = LUT_MAX / 256.0;
    const float LUT_G_SCALE = LUT_MAX / LUT_N;
    const float LUT_SLICE = 1.0 / LUT_N;

    // Encode linear → sRGB for the lookup coordinate; clamp for HDR inputs.
    float3 c = clamp(pow(max(color, float3(0.0)) + 0.0000001, float3(1.0 / 2.2)), 0.0, 1.0);

    float cell = c.b * LUT_MAX;
    float cell_l = floor(cell);
    float cell_h = ceil(cell);
    float t = fract(cell);

    float r_offset = LUT_HALF_PX_X + c.r * LUT_R_SCALE;
    float g_offset = LUT_HALF_PX_Y + c.g * LUT_G_SCALE;
    float2 uv_l = float2(cell_l * LUT_SLICE + r_offset, g_offset);
    float2 uv_h = float2(cell_h * LUT_SLICE + r_offset, g_offset);

    float3 lutColor1 = sampleColorLUT(lut1, s, uv_l, uv_h, t);
    if (lut2Enabled != 0u) {
        float3 lutColor2 = sampleColorLUT(lut2, s, uv_l, uv_h, t);
        float w1 = intensity1 * (1.0 - blend);
        float w2 = intensity2 * blend;
        return color + (lutColor1 - color) * w1 + (lutColor2 - color) * w2;
    }
    return mix(color, lutColor1, intensity1);
}

// Vignette: darken edges with configurable curvature and inner/outer radii
float3 applyVignette(float3 color, float2 uv, float inner, float outer,
                     float curvature, float intensity, float3 vigColor) {
    float2 curve = pow(abs(uv * 2.0 - 1.0), float2(1.0 / curvature));
    float edge = pow(length(curve), curvature);
    float vignette = 1.0 - intensity * smoothstep(inner, outer, edge);
    return mix(vigColor, color, vignette);
}

// Single-pass DOF using depth buffer
float3 applyDofSinglePass(float3 sharpColor, float2 uv, float2 invRes,
    texture2d<float> sceneTexture, depth2d<float> depthTexture, sampler s,
    float focusDistance, float focusRange, float blurRadius,
    float cameraNear, float cameraFar)
{
    float rawDepth = depthTexture.sample(s, uv);
    float linearDepth = (cameraNear * cameraFar) / (cameraFar - rawDepth * (cameraFar - cameraNear));

    // upstream-style CoC: far range starts at focusDistance + focusRange/2
    float farRange = focusDistance + focusRange * 0.5;
    float invRange = 1.0 / max(focusRange, 0.001);
    float cocFar = clamp((linearDepth - farRange) * invRange, 0.0, 1.0);

    if (cocFar < 0.005) return sharpColor;  // early out for in-focus pixels

    // Disc blur with 12 taps (Poisson-like distribution)
    const float2 offsets[12] = {
        float2(-0.326, -0.406), float2(-0.840, -0.074), float2(-0.696,  0.457),
        float2(-0.203,  0.621), float2( 0.962, -0.195), float2( 0.473, -0.480),
        float2( 0.519,  0.767), float2( 0.185, -0.893), float2( 0.507,  0.064),
        float2(-0.321, -0.882), float2(-0.860,  0.370), float2( 0.871,  0.414)
    };

    float2 step = cocFar * blurRadius * invRes;
    float3 sum = float3(0.0);
    float totalWeight = 0.0;

    for (int i = 0; i < 12; i++) {
        float2 sampleUV = clamp(uv + offsets[i] * step, float2(0.0), float2(1.0));

        // Read depth at sample position to compute its CoC
        float sampleRawDepth = depthTexture.sample(s, sampleUV);
        float sampleLinearDepth = (cameraNear * cameraFar) / (cameraFar - sampleRawDepth * (cameraFar - cameraNear));
        float sampleCoc = clamp((sampleLinearDepth - farRange) * invRange, 0.0, 1.0);

        // Weight: only blur samples that are also out of focus (prevents sharp foreground leaking)
        float w = sampleCoc;
        float3 tap = sceneTexture.sample(s, sampleUV).rgb;
        sum += tap * w;
        totalWeight += w;
    }

    float3 blurColor = (totalWeight > 0.0) ? sum / totalWeight : sharpColor;
    return mix(sharpColor, blurColor, cocFar);
}

// Compose pass order (mirrors upstream compose.js):
// CAS -> SSAO -> DOF -> Bloom -> Fringing -> ColorEnhance -> Grading -> ToneMap -> ColorLUT -> Vignette
fragment float4 composeFragment(
    ComposeVarying in [[stage_in]],
    texture2d<float> sceneTexture [[texture(0)]],
    texture2d<float> bloomTexture [[texture(1)]],
    texture2d<float> ssaoTexture [[texture(2)]],
    depth2d<float> depthTexture [[texture(3)]],
    texture2d<float> colorLUT [[texture(4)]],
    texture2d<float> colorLUT2 [[texture(5)]],
    sampler linearSampler [[sampler(0)]],
    constant ComposeUniforms& uniforms [[buffer(3)]])
{
    const float2 uv = clamp(in.uv, float2(0.0), float2(1.0));
    float3 result = sceneTexture.sample(linearSampler, uv).rgb;

    // 1. CAS (Contrast Adaptive Sharpening)
    if (uniforms.sharpness > 0.0) {
        result = applyCas(result, uv, uniforms.sharpness, sceneTexture, linearSampler, uniforms.sceneTextureInvRes);
    }

    // 2. SSAO
    if (uniforms.ssaoEnabled != 0u && ssaoTexture.get_width() > 0) {
        const float ssao = clamp(ssaoTexture.sample(linearSampler, uv).r, 0.0, 1.0);
        result *= ssao;
    }

    // 3. DOF (single-pass from depth buffer)
    if (uniforms.dofEnabled != 0u) {
        result = applyDofSinglePass(result, uv, uniforms.sceneTextureInvRes,
            sceneTexture, depthTexture, linearSampler,
            uniforms.dofFocusDistance, uniforms.dofFocusRange, uniforms.dofBlurRadius,
            uniforms.dofCameraNear, uniforms.dofCameraFar);
    }
    // Legacy multi-pass DOF (kept as dead code for future use):
    // if (uniforms.dofEnabled != 0u && cocTexture.get_width() > 0 && blurTexture.get_width() > 0) {
    //     const float2 coc = cocTexture.sample(linearSampler, uv).rg;
    //     const float cocAmount = clamp(max(coc.r, coc.g), 0.0, 1.0);
    //     const float3 blurColor = blurTexture.sample(linearSampler, uv).rgb;
    //     result = mix(result, blurColor, cocAmount * clamp(uniforms.dofIntensity, 0.0, 1.0));
    // }

    // 4. Fringing (chromatic aberration). BEFORE bloom, as upstream compose.js orders
    // it: applyFringing re-samples the scene texture for the red and blue channels, so
    // running it after bloom would throw away the bloom already added to those two
    // channels and leave it only in green — a green cast over everything that blooms.
    if (uniforms.fringingIntensity > 0.0) {
        result = applyFringing(result, uv, uniforms.fringingIntensity, sceneTexture, linearSampler);
    }

    // 4b. Bloom
    if (uniforms.bloomEnabled != 0u && bloomTexture.get_width() > 0) {
        const float3 bloomColor = bloomTexture.sample(linearSampler, uv).rgb;
        result += bloomColor * max(uniforms.bloomIntensity, 0.0);
    }

    // 4c. Color enhance (HDR)
    if (uniforms.colorEnhanceEnabled != 0u) {
        result = applyColorEnhance(result, uniforms.ceShadows, uniforms.ceHighlights,
            uniforms.ceVibrance, uniforms.ceDehaze, uniforms.ceMidtones);
    }

    // 4d. Color grading (HDR)
    if (uniforms.gradingEnabled != 0u) {
        result = applyGrading(result, uniforms.gradingBrightness, uniforms.gradingSaturation,
            uniforms.gradingContrast,
            float3(uniforms.gradingTintR, uniforms.gradingTintG, uniforms.gradingTintB));
    }

    // 5. Tonemapping (tonemapping dispatch)
    result = max(result, float3(0.0));
    if (uniforms.tonemapMode == 1u) {           // TONEMAP_FILMIC
        result = toneMapFilmic(result, uniforms.exposure);
    } else if (uniforms.tonemapMode == 2u) {    // TONEMAP_HEJL
        result = toneMapHejl(result, uniforms.exposure);
    } else if (uniforms.tonemapMode == 3u) {    // TONEMAP_ACES
        result = toneMapAces(result, uniforms.exposure);
    } else if (uniforms.tonemapMode == 4u) {    // TONEMAP_ACES2
        result = toneMapAces2(result, uniforms.exposure);
    } else if (uniforms.tonemapMode == 5u) {    // TONEMAP_NEUTRAL
        result = toneMapNeutral(result, uniforms.exposure);
    } else if (uniforms.tonemapMode == 6u) {    // TONEMAP_NONE
        // no-op
    } else {                                     // TONEMAP_LINEAR (default)
        result = toneMapLinear(result, uniforms.exposure);
    }

    // 5b. 3D color LUT (post-tonemap)
    if (uniforms.lutEnabled != 0u && colorLUT.get_width() > 0) {
        result = applyColorLUT(result, colorLUT, colorLUT2, linearSampler,
            uniforms.lut2Enabled, uniforms.lutIntensity1, uniforms.lutIntensity2,
            uniforms.lutBlend);
    }

    // 6. Vignette (applied in tonemapped linear space, before gamma)
    if (uniforms.vignetteEnabled != 0u) {
        float3 vigColor = float3(uniforms.vignetteColorR, uniforms.vignetteColorG, uniforms.vignetteColorB);
        result = applyVignette(result, uv, uniforms.vignetteInner, uniforms.vignetteOuter,
                               uniforms.vignetteCurvature, uniforms.vignetteIntensity,
                               vigColor);
    }

    // 7. Gamma correction (gammaCorrectOutput)
    // The back buffer is BGRA8Unorm (not sRGB), so we must apply gamma in the shader.
    result = pow(max(result, float3(0.0)) + 0.0000001, float3(1.0 / 2.2));

    return float4(result, 1.0);
}
)";

    constexpr const char* COMPOSE_GLSL = R"(#version 450

#ifdef VT_VERTEX_SHADER
layout(location = 0) in vec3 vertexPosition;
layout(location = 2) in vec2 vertexUv0;
layout(location = 0) out vec2 vUv;
void main() { vUv = vertexUv0; gl_Position = vec4(vertexPosition, 1.0); }
#endif

#ifdef VT_FRAGMENT_SHADER
layout(location = 0) in vec2 vUv;

// Compose pass — port of metalComposePass.cpp composeFragment.
// Order: CAS -> SSAO -> DOF (single-pass from depth) -> Bloom -> ToneMap ->
// Vignette -> display gamma. Runs as a fullscreen draw inside the compose
// render pass (usually targeting the swapchain).

layout(set = 1, binding = 0) uniform sampler2D sceneTex;
layout(set = 1, binding = 1) uniform sampler2D bloomTex;
layout(set = 1, binding = 2) uniform sampler2D ssaoTex;
layout(set = 1, binding = 3) uniform sampler2D depthTex;
layout(set = 1, binding = 4) uniform sampler2D colorLut1;
layout(set = 1, binding = 5) uniform sampler2D colorLut2;

layout(std140, set = 0, binding = 0) uniform ComposeParams {
    // Mirrors ComposeUniforms in composeShaders.h field for field. A block of
    // scalars (plus one vec2) packs identically under std140 and MSL, so both
    // backends read one layout.
    uint dofEnabled;
    uint taaEnabled;
    uint ssaoEnabled;
    uint bloomEnabled;
    uint blurTextureUpscale;
    float bloomIntensity;
    float dofIntensity;
    float sharpness;
    uint tonemapMode;
    float exposure;
    vec2 sceneTextureInvRes;
    float dofFocusDistance;
    float dofFocusRange;
    float dofBlurRadius;
    float dofCameraNear;
    float dofCameraFar;
    float _pad0;
    uint vignetteEnabled;
    float vignetteInner;
    float vignetteOuter;
    float vignetteCurvature;
    float vignetteIntensity;
    float vignetteColorR;
    float vignetteColorG;
    float vignetteColorB;
    float fringingIntensity;
    uint gradingEnabled;
    float gradingBrightness;
    float gradingContrast;
    float gradingSaturation;
    float gradingTintR;
    float gradingTintG;
    float gradingTintB;
    uint colorEnhanceEnabled;
    float ceShadows;
    float ceHighlights;
    float ceVibrance;
    float ceDehaze;
    float ceMidtones;
    uint lutEnabled;
    uint lut2Enabled;
    float lutIntensity1;
    float lutIntensity2;
    float lutBlend;
} pc;

layout(location = 0) out vec4 outColor;

// ── Tonemapping (same curves as forward.frag / common.metal) ──
vec3 toneMapAcesFit(vec3 x) {
    const float tA = 2.51, tB = 0.03, tC = 2.43, tD = 0.59, tE = 0.14;
    return (x * (tA * x + tB)) / (x * (tC * x + tD) + tE);
}
vec3 uncharted2Tonemap(vec3 x) {
    const float A = 0.15, B = 0.50, C = 0.10, D = 0.20, E = 0.02, F = 0.30;
    return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
}
vec3 toneMapFilmic(vec3 color) {
    const float W = 11.2;
    color = uncharted2Tonemap(color * 2.0);
    return color * (1.0 / uncharted2Tonemap(vec3(W)));
}
vec3 toneMapHejl(vec3 color) {
    const float A = 0.22, B = 0.3, C = 0.1, D = 0.2, E = 0.01, F = 0.3;
    const float scl = 1.25;
    vec3 h = max(vec3(0.0), color - 0.004);
    return (h * ((scl * A) * h + scl * (C * B)) + scl * (D * E))
         / (h * (A * h + B) + (D * F)) - scl * (E / F);
}
vec3 RRTAndODTFit(vec3 v) {
    vec3 a = v * (v + 0.0245786) - 0.000090537;
    vec3 b = v * (0.983729 * v + 0.4329510) + 0.238081;
    return a / b;
}
vec3 toneMapAces2(vec3 color) {
    const mat3 ACESInputMat = mat3(
        0.59719, 0.07600, 0.02840,
        0.35458, 0.90834, 0.13383,
        0.04823, 0.01566, 0.83777);
    const mat3 ACESOutputMat = mat3(
         1.60475, -0.10208, -0.00327,
        -0.53108,  1.10813, -0.07276,
        -0.07367, -0.00605,  1.07602);
    color /= 0.6;
    color = ACESInputMat * color;
    color = RRTAndODTFit(color);
    color = ACESOutputMat * color;
    return clamp(color, vec3(0.0), vec3(1.0));
}
vec3 toneMapNeutral(vec3 color) {
    const float startCompression = 0.8 - 0.04;
    const float desaturation = 0.15;
    float x = min(color.r, min(color.g, color.b));
    float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
    color -= offset;
    float peak = max(color.r, max(color.g, color.b));
    if (peak < startCompression) return color;
    float d = 1.0 - startCompression;
    float newPeak = 1.0 - d * d / (peak + d - startCompression);
    color *= newPeak / peak;
    float g = 1.0 - 1.0 / (desaturation * (peak - newPeak) + 1.0);
    return mix(color, vec3(newPeak), g);
}

float maxComp(vec3 c) { return max(c.r, max(c.g, c.b)); }
vec3 toSDR(vec3 c) { return c / (1.0 + maxComp(c)); }
vec3 toHDR(vec3 c) { return c / (1.0 - maxComp(c)); }

// Contrast Adaptive Sharpening.
vec3 applyCas(vec3 color, vec2 uv, float sharpness, vec2 invRes) {
    vec3 a = toSDR(texture(sceneTex, uv + vec2(0.0, -invRes.y)).rgb);
    vec3 b = toSDR(texture(sceneTex, uv + vec2(-invRes.x, 0.0)).rgb);
    vec3 c = toSDR(color);
    vec3 d = toSDR(texture(sceneTex, uv + vec2(invRes.x, 0.0)).rgb);
    vec3 e = toSDR(texture(sceneTex, uv + vec2(0.0, invRes.y)).rgb);

    float min_g = min(a.g, min(b.g, min(c.g, min(d.g, e.g))));
    float max_g = max(a.g, max(b.g, max(c.g, max(d.g, e.g))));
    float sharpening_amount = sqrt(min(1.0 - max_g, min_g) / max(max_g, 1e-6));
    float w = sharpening_amount * sharpness;
    vec3 res = (w * (a + b + d + e) + c) / (4.0 * w + 1.0);
    return toHDR(max(res, vec3(0.0)));
}

vec3 applyVignette(vec3 color, vec2 uv, float inner, float outer,
                   float curvature, float intensity, vec3 vigColor) {
    vec2 curve = pow(abs(uv * 2.0 - 1.0), vec2(1.0 / curvature));
    float edge = pow(length(curve), curvature);
    float vignette = 1.0 - intensity * smoothstep(inner, outer, edge);
    return mix(vigColor, color, vignette);
}

float linearizeSceneDepth(float rawDepth, float cameraNear, float cameraFar) {
    return (cameraNear * cameraFar) / (cameraFar - rawDepth * (cameraFar - cameraNear));
}

vec3 sampleStripLut(sampler2D lut, vec3 color) {
    color = clamp(color, 0.0, 1.0);
    float blue = color.b * 15.0;
    float slice0 = floor(blue);
    float slice1 = min(slice0 + 1.0, 15.0);
    vec2 uv0 = vec2((slice0 * 16.0 + color.r * 15.0 + 0.5) / 256.0,
                    (color.g * 15.0 + 0.5) / 16.0);
    vec2 uv1 = vec2((slice1 * 16.0 + color.r * 15.0 + 0.5) / 256.0,
                    uv0.y);
    return mix(texture(lut, uv0).rgb, texture(lut, uv1).rgb, fract(blue));
}

// Single-pass DOF from the depth buffer (far blur only, upstream-style CoC).
vec3 applyDofSinglePass(vec3 sharpColor, vec2 uv, vec2 invRes,
    float focusDistance, float focusRange, float blurRadius,
    float cameraNear, float cameraFar)
{
    float rawDepth = texture(depthTex, uv).r;
    float linearDepth = linearizeSceneDepth(rawDepth, cameraNear, cameraFar);

    float farRange = focusDistance + focusRange * 0.5;
    float invRange = 1.0 / max(focusRange, 0.001);
    float cocFar = clamp((linearDepth - farRange) * invRange, 0.0, 1.0);

    if (cocFar < 0.005) return sharpColor;

    const vec2 offsets[12] = vec2[](
        vec2(-0.326, -0.406), vec2(-0.840, -0.074), vec2(-0.696,  0.457),
        vec2(-0.203,  0.621), vec2( 0.962, -0.195), vec2( 0.473, -0.480),
        vec2( 0.519,  0.767), vec2( 0.185, -0.893), vec2( 0.507,  0.064),
        vec2(-0.321, -0.882), vec2(-0.860,  0.370), vec2( 0.871,  0.414));

    vec2 stepUv = cocFar * blurRadius * invRes;
    vec3 blurSum = vec3(0.0);
    float totalWeight = 0.0;

    for (int i = 0; i < 12; i++) {
        vec2 sampleUV = clamp(uv + offsets[i] * stepUv, vec2(0.0), vec2(1.0));
        float sampleRawDepth = texture(depthTex, sampleUV).r;
        float sampleLinearDepth = linearizeSceneDepth(sampleRawDepth, cameraNear, cameraFar);
        float sampleCoc = clamp((sampleLinearDepth - farRange) * invRange, 0.0, 1.0);
        blurSum += texture(sceneTex, sampleUV).rgb * sampleCoc;
        totalWeight += sampleCoc;
    }

    vec3 blurColor = (totalWeight > 0.0) ? blurSum / totalWeight : sharpColor;
    return mix(sharpColor, blurColor, cocFar);
}

void main() {
    vec2 invRes = pc.sceneTextureInvRes;
    vec2 uv = clamp(vUv, vec2(0.0), vec2(1.0));
    vec3 result = texture(sceneTex, uv).rgb;

    // 1. CAS
    if (pc.sharpness > 0.0) {
        result = applyCas(result, uv, pc.sharpness, invRes);
    }

    // 2. SSAO
    if (float(pc.ssaoEnabled) > 0.5) {
        float ssao = clamp(texture(ssaoTex, uv).r, 0.0, 1.0);
        result *= ssao;
    }

    // 3. DOF (single-pass from depth)
    if (float(pc.dofEnabled) > 0.5) {
        result = applyDofSinglePass(result, uv, invRes,
            pc.dofFocusDistance, pc.dofFocusRange, pc.dofBlurRadius, pc.dofCameraNear, pc.dofCameraFar);
    }

    // 4. Fringing (chromatic aberration). Sits between DOF and bloom, matching upstream
    // compose.js and metalComposePass: red and blue are RE-SAMPLED from the scene
    // texture, so running it after bloom would keep bloom in green only. The offset is
    // the SQUARED distance from centre, as upstream — a linear offset smears the whole
    // mid-field instead of just the corners.
    if (pc.fringingIntensity > 0.0) {
        vec2 centerDistance = uv - 0.5;
        vec2 offset = pc.fringingIntensity * centerDistance * centerDistance;
        result.r = texture(sceneTex, uv - offset).r;
        result.b = texture(sceneTex, uv + offset).b;
    }

    // 5. Bloom
    if (float(pc.bloomEnabled) > 0.5) {
        result += texture(bloomTex, uv).rgb * max(pc.bloomIntensity, 0.0);
    }

    if (float(pc.colorEnhanceEnabled) > 0.5) {
        float luma = dot(result, vec3(0.2126, 0.7152, 0.0722));
        float shadowMask = 1.0 - smoothstep(0.0, 0.5, luma);
        float highlightMask = smoothstep(0.5, 1.0, luma);
        float midMask = 1.0 - abs(luma * 2.0 - 1.0);
        result *= exp2(pc.ceShadows * shadowMask + pc.ceHighlights * highlightMask +
                       pc.ceMidtones * midMask);
        float saturation = maxComp(result) - min(result.r, min(result.g, result.b));
        result = mix(vec3(dot(result, vec3(0.2126, 0.7152, 0.0722))), result,
            1.0 + pc.ceVibrance * (1.0 - saturation));
        result = max(result - vec3(pc.ceDehaze * 0.02), 0.0) *
            (1.0 + pc.ceDehaze);
    }
    if (float(pc.gradingEnabled) > 0.5) {
        result *= pc.gradingBrightness;
        result = (result - 0.5) * pc.gradingContrast + 0.5;
        float luma = dot(result, vec3(0.2126, 0.7152, 0.0722));
        result = mix(vec3(luma), result, pc.gradingSaturation) * vec3(pc.gradingTintR, pc.gradingTintG, pc.gradingTintB);
    }

    // 6. Tonemapping. Every curve consumes exposure-scaled color (Metal
    // passes exposure into each curve); TONEMAP_NONE applies neither the
    // curve nor exposure — matching metalComposePass exactly.
    result = max(result, vec3(0.0));
    int mode = int(float(pc.tonemapMode) + 0.5);
    float exposure = pc.exposure;
    if      (mode == 1) result = toneMapFilmic(result * exposure);
    else if (mode == 2) result = toneMapHejl(result * exposure);
    else if (mode == 3) result = toneMapAcesFit(result * exposure);
    else if (mode == 4) result = toneMapAces2(result * exposure);
    else if (mode == 5) result = toneMapNeutral(result * exposure);
    else if (mode == 6) { /* TONEMAP_NONE: no curve, no exposure */ }
    else                result *= exposure;   // TONEMAP_LINEAR

    if (float(pc.lutEnabled) > 0.5) {
        result = mix(result, sampleStripLut(colorLut1, result),
            clamp(pc.lutIntensity1, 0.0, 1.0));
    }
    if (float(pc.lut2Enabled) > 0.5) {
        vec3 graded2 = mix(result, sampleStripLut(colorLut2, result),
            clamp(pc.lutIntensity2, 0.0, 1.0));
        result = mix(result, graded2, clamp(pc.lutBlend, 0.0, 1.0));
    }

    // 7. Vignette (tonemapped linear space, before gamma)
    if (float(pc.vignetteEnabled) > 0.5) {
        result = applyVignette(result, uv, pc.vignetteInner, pc.vignetteOuter, pc.vignetteCurvature, pc.vignetteIntensity, vec3(pc.vignetteColorR, pc.vignetteColorG, pc.vignetteColorB));
    }

    // 8. Display gamma (linear UNORM swapchain, matching Metal BGRA8Unorm)
    result = pow(max(result, vec3(0.0)) + 0.0000001, vec3(1.0 / 2.2));
    outColor = vec4(result, 1.0);
}
#endif
)";
}
