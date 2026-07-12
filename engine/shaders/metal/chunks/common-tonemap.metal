// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
static inline float3 toneMapLinear(float3 color, float exposure)
{
    return color * exposure;
}

// https://modelviewer.dev/examples/tone-mapping
static inline float3 toneMapNeutral(float3 color, float exposure)
{
    color *= exposure;

    const float startCompression = 0.8 - 0.04;
    const float desaturation = 0.15;

    const float x = min(color.r, min(color.g, color.b));
    const float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
    color -= offset;

    const float peak = max(color.r, max(color.g, color.b));
    if (peak < startCompression) return color;

    const float d = 1.0 - startCompression;
    const float newPeak = 1.0 - d * d / (peak + d - startCompression);
    color *= newPeak / peak;

    const float g = 1.0 - 1.0 / (desaturation * (peak - newPeak) + 1.0);
    return mix(color, float3(newPeak), g);
}

static inline float3 toneMapAces(float3 color, float exposure)
{
    const float tA = 2.51;
    const float tB = 0.03;
    const float tC = 2.43;
    const float tD = 0.59;
    const float tE = 0.14;
    color *= exposure;
    return (color * (tA * color + tB)) / (color * (tC * color + tD) + tE);
}

// ACES approximation by Stephen Hill — TONEMAP_ACES2 (used by upstream
// camera.toneMapping = TONEMAP_ACES2). Two-matrix fit with RRT+ODT polynomial.
// Higher dynamic range than the simpler Narkowicz fit in toneMapAces — bright
// HDR values (e.g. specular highlights) roll off smoothly past 1.0 instead of
// clipping, which is essential for parity with upstream published demos.
static inline float3 RRTAndODTFit(float3 v)
{
    const float3 a = v * (v + 0.0245786) - 0.000090537;
    const float3 b = v * (0.983729 * v + 0.4329510) + 0.238081;
    return a / b;
}

static inline float3 toneMapAces2(float3 color, float exposure)
{
    // sRGB → XYZ → D65_2_D60 → AP1 → RRT_SAT
    const float3x3 ACESInputMat = float3x3(
        float3(0.59719, 0.35458, 0.04823),
        float3(0.07600, 0.90834, 0.01566),
        float3(0.02840, 0.13383, 0.83777));
    // ODT_SAT → XYZ → D60_2_D65 → sRGB
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

// Uncharted 2 filmic operator (upstream TONEMAP_FILMIC, tonemappingFilmicPS).
static inline float3 uncharted2Tonemap(float3 x)
{
    const float A = 0.15; // shoulder strength
    const float B = 0.50; // linear strength
    const float C = 0.10; // linear angle
    const float D = 0.20; // toe strength
    const float E = 0.02; // toe numerator
    const float F = 0.30; // toe denominator
    return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
}

static inline float3 toneMapFilmic(float3 color, float exposure)
{
    const float W = 11.2; // linear white point
    color = uncharted2Tonemap(color * exposure * 2.0);
    const float3 whiteScale = 1.0 / uncharted2Tonemap(float3(W));
    return color * whiteScale;
}

// Hejl/Burgess-Dawson operator (upstream TONEMAP_HEJL, tonemappingHejlPS).
// Output includes the sRGB curve baked in by construction.
static inline float3 toneMapHejl(float3 color, float exposure)
{
    color *= exposure;
    const float A = 0.22, B = 0.3, C = 0.1, D = 0.2, E = 0.01, F = 0.3;
    const float scl = 1.25;
    const float3 h = max(float3(0.0), color - 0.004);
    return (h * ((scl * A) * h + scl * (C * B)) + scl * (D * E))
         / (h * (A * h + B) + (D * F))
         - scl * (E / F);
}

// dispatch tone mapping by mode.
// Mode is passed via skyboxMipAndPad.z and matches scene/constants.h:
//   0=linear, 1=filmic, 2=hejl, 3=aces, 4=aces2, 5=neutral, 6=none.
static inline float3 toneMap(float3 color, float exposure, float mode)
{
    if (mode > 4.5 && mode < 5.5) {
        return toneMapNeutral(color, exposure);
    } else if (mode > 3.5 && mode < 4.5) {
        return toneMapAces2(color, exposure);
    } else if (mode > 2.5 && mode < 3.5) {
        return toneMapAces(color, exposure);
    } else if (mode > 1.5 && mode < 2.5) {
        return toneMapHejl(color, exposure);
    } else if (mode > 0.5 && mode < 1.5) {
        return toneMapFilmic(color, exposure);
    } else if (mode > 5.5) {
        return color; // TONEMAP_NONE
    }
    return toneMapLinear(color, exposure);
}

// GLSL parity (falloffLinear / falloffInvSquared / spotPS).
