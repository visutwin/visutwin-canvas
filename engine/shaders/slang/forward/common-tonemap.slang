// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 07.10.2026
//
// ── Tonemapping (parity with the Metal common.metal toneMap dispatch) ──
// Modes match scene/constants.h: 0=linear, 1=filmic, 2=hejl, 3=aces,
// 4=aces2, 5=neutral, 6=none. Applied before display-gamma encode.

float3 toneMapAcesFit(float3 x) {
    const float tA = 2.51, tB = 0.03, tC = 2.43, tD = 0.59, tE = 0.14;
    return (x * (tA * x + tB)) / (x * (tC * x + tD) + tE);
}

float3 uncharted2Tonemap(float3 x) {
    const float A = 0.15, B = 0.50, C = 0.10, D = 0.20, E = 0.02, F = 0.30;
    return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
}

float3 toneMapFilmic(float3 color) {
    const float W = 11.2;
    color = uncharted2Tonemap(color);
    return color * (1.0 / uncharted2Tonemap(float3(W)));
}

float3 toneMapHejl(float3 color) {
    const float A = 0.22, B = 0.3, C = 0.1, D = 0.2, E = 0.01, F = 0.3;
    const float scl = 1.25;
    float3 h = max(float3(0.0), color - 0.004);
    return (h * ((scl * A) * h + scl * (C * B)) + scl * (D * E))
         / (h * (A * h + B) + (D * F))
         - scl * (E / F);
}

float3 RRTAndODTFit(float3 v) {
    float3 a = v * (v + 0.0245786) - 0.000090537;
    float3 b = v * (0.983729 * v + 0.4329510) + 0.238081;
    return a / b;
}

float3 toneMapAces2(float3 color) {
    // GLSL float3x3 constructors take COLUMNS; these are the transposes of the
    // row-major matrices in common.metal (which multiplies vector * matrix).
    const float3x3 ACESInputMat = float3x3(
        0.59719, 0.07600, 0.02840,
        0.35458, 0.90834, 0.13383,
        0.04823, 0.01566, 0.83777);
    const float3x3 ACESOutputMat = float3x3(
         1.60475, -0.10208, -0.00327,
        -0.53108,  1.10813, -0.07276,
        -0.07367, -0.00605,  1.07602);
    color /= 0.6;
    color = mul(color, ACESInputMat);
    color = RRTAndODTFit(color);
    color = mul(color, ACESOutputMat);
    return clamp(color, float3(0.0), float3(1.0));
}

float3 toneMapNeutral(float3 color) {
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
    return lerp(color, float3(newPeak), g);
}

// Exposure-scaled color -> tonemapped color, by mode.
float3 toneMapByMode(float3 color, int mode) {
    if (mode == 1) return toneMapFilmic(color);
    if (mode == 2) return toneMapHejl(color);
    if (mode == 3) return toneMapAcesFit(color);
    if (mode == 4) return toneMapAces2(color);
    if (mode == 5) return toneMapNeutral(color);
    return color; // LINEAR (0) and NONE (6): exposure only
}

// Exposure then tonemap, by mode — the twin of common-tonemap.metal's toneMap(color,
// exposure, mode). NONE (6) returns the colour UNTOUCHED, exposure included.
// Callers pass exposure here rather than multiplying
// by it first, or NONE would apply exposure on Vulkan alone.
float3 toneMapExposed(float3 color, float exposure, int mode) {
    if (mode == 6) {
        return color;
    }
    return toneMapByMode(color * exposure, mode);
}

// The forward pass's dispatch, by the scene mode in the lighting block. A shader
// with no lighting block (gsplat.vert) defines VT_TONEMAP_OPERATORS_ONLY before
// including this file and calls toneMapByMode itself.
#ifndef VT_TONEMAP_OPERATORS_ONLY
// A material with useTonemap off (UI) compiles VT_FEATURE_NO_TONEMAP and takes
// TONEMAP_NONE here: neither the curve nor exposure. Every forward return path that
// tone maps comes through this function; the gamma encode after it still runs.
float3 applyToneMap(float3 color) {
    if (vtFeatureEnabled(VT_FEATURE_NO_TONEMAP_BIT)) {
        return color;
    }
    return toneMapExposed(color, lighting.cameraPosExposure.w, int(lighting.shadowParams2.z + 0.5));
}
#endif
