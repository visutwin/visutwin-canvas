// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 30.10.2026
//
// AgX tone mapping (TONEMAP_AGX, TONEMAP_AGX_PUNCHY), shared by the forward tail and the
// compose pass. Filament's implementation with Blender's AgX matrices
// (https://github.com/google/filament/blob/0880ea059b9f62f135bd5f5446c9b514326c435d/filament/src/ToneMapper.cpp#L691-L787);
// Filament's conversion from linear sRGB to Rec.2020 is folded into the inset matrix. The sigmoid
// is the 6th order polynomial fit from https://iolite-engine.com/blog_posts/minimal_agx_implementation,
// which reaches white at the top of the curve (Filament's 7th order fit stops at 0.98). The Punchy
// look is Filament's, with the saturation luminance taken after the power.
module vttonemapagx;

// log2 range of the curve: middle grey (0.18) from 10 stops below to 6.5 stops above
static const float kAgxMinEv = -12.47393;
static const float kAgxMaxEv = 4.026069;

float3 agxContrastApprox(float3 x)
{
    const float3 x2 = x * x;
    const float3 x4 = x2 * x2;
    return 15.5 * x4 * x2
         - 40.14 * x4 * x
         + 31.96 * x4
         - 6.868 * x2 * x
         + 0.4298 * x2
         + 0.1191 * x
         - 0.00232;
}

// Exposure-scaled linear sRGB in, display-referred linear sRGB in [0, 1] out.
public float3 toneMapAgx(float3 color, bool punchy)
{
    // Slang float3x3 constructors take ROWS and mul(v, M) multiplies a row vector, so each
    // line below is a COLUMN of the published matrix (M * v).
    // linear sRGB to Rec.2020, followed by the AgX inset matrix
    const float3x3 insetMatrix = float3x3(
        0.5449046634, 0.1404396285, 0.0888268956,
        0.3737799450, 0.7541105648, 0.1788773492,
        0.0813857278, 0.1054334821, 0.7322502399);
    const float3x3 outsetMatrix = float3x3(
         1.1271005818, -0.1413297635, -0.1413297635,
        -0.1106066431,  1.1578237022, -0.1106066431,
        -0.0164939387, -0.0164939387,  1.2519364066);
    const float3x3 rec2020ToSrgb = float3x3(
         1.6602133532, -0.1245520386, -0.0181550265,
        -0.5875643107,  1.1329461928, -0.1006047909,
        -0.0728275822, -0.0083483300,  1.1188316722);

    float3 c = mul(max(color, float3(0.0)), insetMatrix);

    // log2 encoding
    c = clamp((log2(max(c, float3(1e-10))) - kAgxMinEv) / (kAgxMaxEv - kAgxMinEv), float3(0.0), float3(1.0));
    c = agxContrastApprox(c);

    if (punchy) {
        // the Punchy look: an ASC CDL power, then saturation around the luminance
        c = pow(max(c, float3(0.0)), float3(1.35));
        const float luma = dot(c, float3(0.2126, 0.7152, 0.0722));
        c = luma + 1.4 * (c - luma);
    }

    c = mul(c, outsetMatrix);

    // linearise, then back to linear sRGB, clamping the colours outside its gamut
    c = pow(max(c, float3(0.0)), float3(2.2));
    return clamp(mul(c, rec2020ToSrgb), float3(0.0), float3(1.0));
}
