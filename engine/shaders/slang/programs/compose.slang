// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 07.10.2026
//
// The compose pass, the end of a camera frame's post chain:
//   CAS -> DOF -> SSAO -> Fringing -> Bloom -> ColorEnhance -> Grading -> ToneMap
//   -> ColorLUT -> Vignette -> display gamma
// Inputs: 0 scene, 1 bloom, 2 ssao, 3 depth, 4 colour LUT, 5 colour LUT 2, 6 CoC, 7 DOF
// blur (the last two only with the multi-pass depth of field; dofMultipass says which).
// Every optional input is gated on its flag: an unbound Metal texture reports a width but
// samples zero, and Vulkan binds a white fallback, so a texture query proves nothing.
//
// Order notes. DOF runs BEFORE SSAO, so occlusion multiplies the already-defocused colour
// and keeps full strength out of focus; where DOF is not blurring the order cannot matter.
// Fringing runs BEFORE bloom: it re-samples the scene for red and blue, and after bloom it
// would keep bloom in green only. The CAS weight is NEGATIVE (the CPU remaps the user
// sharpness to lerp(-0.125, -0.2, s)); a positive one turns the kernel into a blur.
// Tone mapping NONE applies neither the curve nor exposure. The 3D LUT is a 256x16 strip
// loaded non-sRGB, so its sample is decoded here (DEVIATION: upstream lets the sampler).
import vtquad;
#include "bindings.slang"

struct ComposeUniforms
{
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
    float sceneTextureInvResX;   // the C++ float[2], as two scalars so no vec2 packing rule applies
    float sceneTextureInvResY;
    float dofFocusDistance;
    float dofFocusRange;
    float dofBlurRadius;
    float dofCameraNear;
    float dofCameraFar;
    float sceneDownscale;
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
    uint dofMultipass;
    float dofBlurTexelX;
    float dofBlurTexelY;
};
VT_DECLARE_QUAD_UNIFORMS(ComposeUniforms, u)
VT_DECLARE_QUAD_INPUT0(sceneTexture)
VT_DECLARE_QUAD_INPUT1(bloomTexture)
VT_DECLARE_QUAD_INPUT2(ssaoTexture)
VT_DECLARE_QUAD_DEPTH_INPUT3(depthTexture)
VT_DECLARE_QUAD_INPUT4(colorLut1)
VT_DECLARE_QUAD_INPUT5(colorLut2)
VT_DECLARE_QUAD_SAMPLER
VT_DECLARE_QUAD_INPUT6(cocImage)
VT_DECLARE_QUAD_INPUT7(blurTexture)

float3 toneMapAces(float3 color, float exposure)
{
    const float tA = 2.51;
    const float tB = 0.03;
    const float tC = 2.43;
    const float tD = 0.59;
    const float tE = 0.14;
    const float3 x = color * exposure;
    return (x * (tA * x + tB)) / (x * (tC * x + tD) + tE);
}

// https://modelviewer.dev/examples/tone-mapping
float3 toneMapNeutral(float3 color, float exposure)
{
    color *= exposure;
    const float startCompression = 0.8 - 0.04;
    const float desaturation = 0.15;
    const float x = min(color.r, min(color.g, color.b));
    const float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
    color -= offset;
    const float peak = max(color.r, max(color.g, color.b));
    if (peak < startCompression) {
        return color;
    }
    const float d = 1.0 - startCompression;
    const float newPeak = 1.0 - d * d / (peak + d - startCompression);
    color *= newPeak / peak;
    const float g = 1.0 - 1.0 / (desaturation * (peak - newPeak) + 1.0);
    return lerp(color, float3(newPeak, newPeak, newPeak), g);
}

// Uncharted 2 filmic operator.
float3 uncharted2Tonemap(float3 x)
{
    const float A = 0.15, B = 0.50, C = 0.10, D = 0.20, E = 0.02, F = 0.30;
    return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
}

float3 toneMapFilmic(float3 color, float exposure)
{
    const float W = 11.2;
    color = uncharted2Tonemap(color * exposure);
    const float3 whiteScale = 1.0 / uncharted2Tonemap(float3(W, W, W));
    return color * whiteScale;
}

// Hejl / Burgess-Dawson operator.
float3 toneMapHejl(float3 color, float exposure)
{
    color *= exposure;
    const float A = 0.22, B = 0.3, C = 0.1, D = 0.2, E = 0.01, F = 0.3;
    const float scl = 1.25;
    const float3 h = max(float3(0.0, 0.0, 0.0), color - 0.004);
    return (h * ((scl * A) * h + scl * (C * B)) + scl * (D * E))
         / (h * (A * h + B) + (D * F))
         - scl * (E / F);
}

// ACES fit by Stephen Hill: the RRT + ODT polynomial between two colour-space matrices.
float3 RRTAndODTFit(float3 v)
{
    const float3 a = v * (v + 0.0245786) - 0.000090537;
    const float3 b = v * (0.983729 * v + 0.4329510) + 0.238081;
    return a / b;
}

float3 toneMapAces2(float3 color, float exposure)
{
    color *= exposure / 0.6;
    // Each output channel is the dot of the colour with one row of the matrix.
    color = float3(dot(color, float3(0.59719, 0.35458, 0.04823)),
                   dot(color, float3(0.07600, 0.90834, 0.01566)),
                   dot(color, float3(0.02840, 0.13383, 0.83777)));
    color = RRTAndODTFit(color);
    color = float3(dot(color, float3( 1.60475, -0.53108, -0.07367)),
                   dot(color, float3(-0.10208,  1.10813, -0.00605)),
                   dot(color, float3(-0.00327, -0.07276,  1.07602)));
    return clamp(color, float3(0.0, 0.0, 0.0), float3(1.0, 1.0, 1.0));
}

float maxComp(float x, float y, float z) { return max(x, max(y, z)); }
float3 toSDR(float3 c) { return c / (1.0 + maxComp(c.r, c.g, c.b)); }
float3 toHDR(float3 c) { return c / max(1.0 - maxComp(c.r, c.g, c.b), 1e-4); }

// Contrast Adaptive Sharpening.
float3 applyCas(float3 color, float2 uv, float sharpness, float2 invRes)
{
    const float3 a = toSDR(sceneTexture.Sample(uv + float2(0.0, -invRes.y)).rgb);
    const float3 b = toSDR(sceneTexture.Sample(uv + float2(-invRes.x, 0.0)).rgb);
    const float3 c = toSDR(color);
    const float3 d = toSDR(sceneTexture.Sample(uv + float2(invRes.x, 0.0)).rgb);
    const float3 e = toSDR(sceneTexture.Sample(uv + float2(0.0, invRes.y)).rgb);

    const float min_g = min(a.g, min(b.g, min(c.g, min(d.g, e.g))));
    const float max_g = max(a.g, max(b.g, max(c.g, max(d.g, e.g))));
    const float sharpening_amount = sqrt(min(1.0 - max_g, min_g) / max(max_g, 1e-4));
    const float w = sharpening_amount * sharpness;
    const float3 res = (w * (a + b + d + e) + c) / (4.0 * w + 1.0);
    return toHDR(max(res, float3(0.0, 0.0, 0.0)));
}

// Fringing (chromatic aberration): red and blue shifted by the SQUARED distance from the
// centre (a linear offset smears the whole mid-field instead of just the corners).
float3 applyFringing(float3 color, float2 uv, float intensity)
{
    const float2 centerDistance = uv - 0.5;
    const float2 offset = intensity * centerDistance * centerDistance;
    color.r = sceneTexture.Sample(uv - offset).r;
    color.b = sceneTexture.Sample(uv + offset).b;
    return color;
}

// HDR colour grading; 1.0 = no change for every parameter.
float3 applyGrading(float3 color, float brt, float sat, float con, float3 tint)
{
    color *= tint;
    color = color * brt;
    float grey = dot(color, float3(0.3, 0.59, 0.11));
    grey = grey / max(1.0, max(color.r, max(color.g, color.b)));   // normalise luminance in HDR
    color = lerp(float3(grey, grey, grey), color, sat);
    return lerp(float3(0.5, 0.5, 0.5), color, con);
}

// Colour enhance, in HDR before tone mapping: shadows / highlights, midtones, vibrance and
// dark-channel dehaze. Every saturation and luminance term is normalised against the
// largest channel, since the naive SDR forms desaturate bright pixels toward black.
float3 applyColorEnhance(float3 color, float shadows, float highlights, float vibrance, float dehaze,
    float midtones)
{
    float lum = dot(color, float3(0.2126, 0.7152, 0.0722));

    // Shadows / highlights: an exponential curve, pow(2, param) = +/-1 stop at +/-1.
    if (shadows != 0.0 || highlights != 0.0) {
        const float logLum = clamp(log2(max(lum, 0.001)) / 10.0 + 0.5, 0.0, 1.0);
        const float shadowWeight = pow(1.0 - logLum, 2.0);
        const float highlightWeight = pow(logLum, 2.0);
        color *= pow(2.0, shadows * shadowWeight);
        color *= pow(2.0, highlights * highlightWeight);
    }

    // Midtones: localised exposure in log-luminance space.
    if (midtones != 0.0) {
        const float pivot = 0.18;
        const float widthStops = 1.25;
        const float maxStops = 2.0;
        const float y = max(dot(color, float3(0.2126, 0.7152, 0.0722)), 1e-6);
        const float d = log2(y / pivot);
        const float w = exp(-(d * d) / (2.0 * widthStops * widthStops));
        color *= exp2(midtones * maxStops * w);
    }

    // Vibrance: boost the saturation of muted colours only.
    if (vibrance != 0.0) {
        const float minChannel = min(color.r, min(color.g, color.b));
        const float maxChannel = max(color.r, max(color.g, color.b));
        const float sat = (maxChannel - minChannel) / max(maxChannel, 0.001);
        lum = dot(color, float3(0.2126, 0.7152, 0.0722));
        const float normalizedLum = lum / max(1.0, maxChannel);
        const float3 grey = float3(normalizedLum, normalizedLum, normalizedLum) * maxChannel;
        const float satBoost = vibrance * (1.0 - sat);
        color = lerp(grey, color, 1.0 + satBoost);
    }

    // Dehaze: the dark channel prior — haze lifts the minimum RGB channel.
    if (dehaze != 0.0) {
        const float maxChannel = max(color.r, max(color.g, color.b));
        const float scale = max(1.0, maxChannel);
        const float3 normalized = color / scale;
        const float darkChannel = min(normalized.r, min(normalized.g, normalized.b));
        const float atmosphericLight = 0.95;
        const float t = max(1.0 - dehaze * darkChannel / atmosphericLight, 0.1);
        const float3 dehazed = (normalized - atmosphericLight) / t + atmosphericLight;
        color = dehazed * scale;
    }

    return max(float3(0.0, 0.0, 0.0), color);
}

float3 sampleColorLUT(Sampler2D lut, float2 uv_l, float2 uv_h, float t)
{
    const float3 color_l = lut.SampleLevel(uv_l, 0.0).rgb;
    const float3 color_h = lut.SampleLevel(uv_h, 0.0).rgb;
    const float3 srgb = lerp(color_l, color_h, t);
    return pow(max(srgb, float3(0.0, 0.0, 0.0)) + 0.0000001, float3(2.2, 2.2, 2.2));
}

// 3D colour LUT through a 256x16 strip (unwrapped 16^3), applied after tone mapping. The
// lookup coordinate is sRGB-encoded. The second LUT is a weighted blend against the SAME
// input, not a second pass chained onto the first result.
float3 applyColorLUT(float3 color, bool lut2Enabled, float intensity1, float intensity2, float blend)
{
    const float LUT_N = 16.0;
    const float LUT_MAX = LUT_N - 1.0;
    const float LUT_HALF_PX_X = 0.5 / 256.0;
    const float LUT_HALF_PX_Y = 0.5 / LUT_N;
    const float LUT_R_SCALE = LUT_MAX / 256.0;
    const float LUT_G_SCALE = LUT_MAX / LUT_N;
    const float LUT_SLICE = 1.0 / LUT_N;

    const float3 c = clamp(pow(max(color, float3(0.0, 0.0, 0.0)) + 0.0000001, float3(1.0 / 2.2, 1.0 / 2.2, 1.0 / 2.2)),
        0.0, 1.0);

    const float cell = c.b * LUT_MAX;
    const float cell_l = floor(cell);
    const float cell_h = ceil(cell);
    const float t = frac(cell);

    const float r_offset = LUT_HALF_PX_X + c.r * LUT_R_SCALE;
    const float g_offset = LUT_HALF_PX_Y + c.g * LUT_G_SCALE;
    const float2 uv_l = float2(cell_l * LUT_SLICE + r_offset, g_offset);
    const float2 uv_h = float2(cell_h * LUT_SLICE + r_offset, g_offset);

    const float3 lutColor1 = sampleColorLUT(colorLut1, uv_l, uv_h, t);
    if (lut2Enabled) {
        const float3 lutColor2 = sampleColorLUT(colorLut2, uv_l, uv_h, t);
        const float w1 = intensity1 * (1.0 - blend);
        const float w2 = intensity2 * blend;
        return color + (lutColor1 - color) * w1 + (lutColor2 - color) * w2;
    }
    return lerp(color, lutColor1, intensity1);
}

// Vignette: darken the edges with configurable curvature and inner / outer radii.
float3 applyVignette(float3 color, float2 uv, float inner, float outer, float curvature, float intensity,
    float3 vigColor)
{
    const float2 curve = pow(abs(uv * 2.0 - 1.0), float2(1.0 / curvature, 1.0 / curvature));
    const float edge = pow(length(curve), curvature);
    const float vignette = 1.0 - intensity * smoothstep(inner, outer, edge);
    return lerp(vigColor, color, vignette);
}

float4 sampleCoc(float2 uv)
{
    return cocImage.SampleLevel(quadSampler, uv, 0.0);
}

// Multi-pass DOF: the blur pass's result mixed in by the total circle of confusion. With
// a quarter-resolution blur (low quality) the blur is upsampled through a 3x3 CoC-weighted
// fetch so in-focus texels do not bleed.
float3 applyDof(float3 color, float2 uv, bool upscale, float2 blurTexel)
{
    const float2 coc = sampleCoc(uv).rg;
    float3 blur;
    if (upscale) {
        float3 sum = float3(0.0, 0.0, 0.0);
        float totalWeight = 0.0;
        for (int i = -1; i <= 1; ++i) {
            for (int j = -1; j <= 1; ++j) {
                const float2 offset = float2(float(i), float(j)) * blurTexel;
                const float2 cocSample = sampleCoc(uv + offset).rg;
                const float3 blurSample = blurTexture.SampleLevel(uv + offset, 0.0).rgb;
                const float w = saturate(cocSample.r + cocSample.g);
                sum += blurSample * w;
                totalWeight += w;
            }
        }
        blur = totalWeight > 0.0 ? sum / totalWeight : sum;
    } else {
        blur = blurTexture.SampleLevel(uv, 0.0).rgb;
    }
    return lerp(color, blur, saturate(coc.r + coc.g));
}

// Single-pass DOF from the depth buffer: the fallback when no CoC texture is bound. Twelve
// Poisson-like taps, each weighted by its own CoC so a sharp foreground does not leak.
float3 applyDofSinglePass(float3 sharpColor, float2 uv, float2 invRes, float focusDistance, float focusRange,
    float blurRadius, float cameraNear, float cameraFar)
{
    const float rawDepth = depthTexture.Sample(uv);
    const float linearDepth = (cameraNear * cameraFar) / (cameraFar - rawDepth * (cameraFar - cameraNear));

    const float farRange = focusDistance + focusRange * 0.5;
    const float invRange = 1.0 / max(focusRange, 0.001);
    const float cocFar = clamp((linearDepth - farRange) * invRange, 0.0, 1.0);
    if (cocFar < 0.005) {
        return sharpColor;
    }

    const float2 offsets[12] = {
        float2(-0.326, -0.406), float2(-0.840, -0.074), float2(-0.696,  0.457),
        float2(-0.203,  0.621), float2( 0.962, -0.195), float2( 0.473, -0.480),
        float2( 0.519,  0.767), float2( 0.185, -0.893), float2( 0.507,  0.064),
        float2(-0.321, -0.882), float2(-0.860,  0.370), float2( 0.871,  0.414)
    };

    const float2 stepUv = cocFar * blurRadius * invRes;
    float3 sum = float3(0.0, 0.0, 0.0);
    float totalWeight = 0.0;
    for (int i = 0; i < 12; i++) {
        const float2 sampleUV = clamp(uv + offsets[i] * stepUv, float2(0.0, 0.0), float2(1.0, 1.0));
        const float sampleRawDepth = depthTexture.Sample(sampleUV);
        const float sampleLinearDepth =
            (cameraNear * cameraFar) / (cameraFar - sampleRawDepth * (cameraFar - cameraNear));
        const float sampleCoc = clamp((sampleLinearDepth - farRange) * invRange, 0.0, 1.0);
        sum += sceneTexture.Sample(sampleUV).rgb * sampleCoc;
        totalWeight += sampleCoc;
    }

    const float3 blurColor = (totalWeight > 0.0) ? sum / totalWeight : sharpColor;
    return lerp(sharpColor, blurColor, cocFar);
}

// Reduce the supersampled scene to one output pixel. One output pixel covers scale x scale
// source texels; a single bilinear tap averages a 2x2 block, which is the footprint at a
// scale of two and only a corner of it above that, so a grid of bilinear taps each centred
// on its own 2x2 block covers the whole footprint. At scale one and two it collapses to one
// centred tap.
float3 sampleSceneReduced(float2 uv, float2 invRes, float scale)
{
    const float span = max(scale, 1.0);
    const int taps = int(clamp(floor(span * 0.5 + 0.5), 1.0, 4.0));
    if (taps <= 1) {
        return sceneTexture.Sample(uv).rgb;
    }

    const float stepSize = span / float(taps);
    const float start = -0.5 * (span - stepSize);

    float3 sum = float3(0.0, 0.0, 0.0);
    for (int y = 0; y < taps; ++y) {
        for (int x = 0; x < taps; ++x) {
            const float2 offset = float2(start + stepSize * float(x), start + stepSize * float(y));
            sum += sceneTexture.Sample(uv + offset * invRes).rgb;
        }
    }
    return sum / float(taps * taps);
}

[shader("vertex")]
QuadVarying composeVertex(QuadVertexIn input)
{
    return quadPassThrough(input);
}

[shader("fragment")]
float4 composeFragment(QuadVarying input) : SV_Target0
{
    const float2 invRes = float2(u.sceneTextureInvResX, u.sceneTextureInvResY);
    const float2 uv = clamp(input.uv, float2(0.0, 0.0), float2(1.0, 1.0));
    float3 result = sampleSceneReduced(uv, invRes, u.sceneDownscale);

    // 1. CAS (negative = on).
    if (u.sharpness < 0.0) {
        result = applyCas(result, uv, u.sharpness, invRes);
    }

    // 2. DOF, before SSAO.
    if (u.dofEnabled != 0u) {
        if (u.dofMultipass != 0u) {
            result = applyDof(result, uv, u.blurTextureUpscale != 0u, float2(u.dofBlurTexelX, u.dofBlurTexelY));
        } else {
            result = applyDofSinglePass(result, uv, invRes, u.dofFocusDistance, u.dofFocusRange,
                u.dofBlurRadius, u.dofCameraNear, u.dofCameraFar);
        }
    }

    // 3. SSAO.
    if (u.ssaoEnabled != 0u) {
        const float ssao = clamp(ssaoTexture.Sample(uv).r, 0.0, 1.0);
        result *= ssao;
    }

    // 4. Fringing, before bloom.
    if (u.fringingIntensity > 0.0) {
        result = applyFringing(result, uv, u.fringingIntensity);
    }

    // 4b. Bloom.
    if (u.bloomEnabled != 0u) {
        const float3 bloomColor = bloomTexture.Sample(uv).rgb;
        result += bloomColor * max(u.bloomIntensity, 0.0);
    }

    // 4c. Colour enhance (HDR).
    if (u.colorEnhanceEnabled != 0u) {
        result = applyColorEnhance(result, u.ceShadows, u.ceHighlights, u.ceVibrance, u.ceDehaze, u.ceMidtones);
    }

    // 4d. Colour grading (HDR).
    if (u.gradingEnabled != 0u) {
        result = applyGrading(result, u.gradingBrightness, u.gradingSaturation, u.gradingContrast,
            float3(u.gradingTintR, u.gradingTintG, u.gradingTintB));
    }

    // 5. Tone mapping.
    result = max(result, float3(0.0, 0.0, 0.0));
    if (u.tonemapMode == 1u) {           // FILMIC
        result = toneMapFilmic(result, u.exposure);
    } else if (u.tonemapMode == 2u) {    // HEJL
        result = toneMapHejl(result, u.exposure);
    } else if (u.tonemapMode == 3u) {    // ACES
        result = toneMapAces(result, u.exposure);
    } else if (u.tonemapMode == 4u) {    // ACES2
        result = toneMapAces2(result, u.exposure);
    } else if (u.tonemapMode == 5u) {    // NEUTRAL
        result = toneMapNeutral(result, u.exposure);
    } else if (u.tonemapMode == 6u) {    // NONE: no curve, no exposure
    } else {                             // LINEAR (default)
        result = result * u.exposure;
    }

    // 5b. 3D colour LUT (after tone mapping).
    if (u.lutEnabled != 0u) {
        result = applyColorLUT(result, u.lut2Enabled != 0u, u.lutIntensity1, u.lutIntensity2, u.lutBlend);
    }

    // 6. Vignette (tone-mapped linear space, before gamma).
    if (u.vignetteEnabled != 0u) {
        const float3 vigColor = float3(u.vignetteColorR, u.vignetteColorG, u.vignetteColorB);
        result = applyVignette(result, uv, u.vignetteInner, u.vignetteOuter, u.vignetteCurvature,
            u.vignetteIntensity, vigColor);
    }

    // 7. Display gamma: the back buffer is a UNORM format, so gamma is applied here.
    result = pow(max(result, float3(0.0, 0.0, 0.0)) + 0.0000001, float3(1.0 / 2.2, 1.0 / 2.2, 1.0 / 2.2));
    return float4(result, 1.0);
}
