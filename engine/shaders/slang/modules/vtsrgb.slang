// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 04.11.2026
//
// The sRGB transfer functions (IEC 61966-2-1), the curves a GPU applies when it samples an sRGB
// texture or writes an sRGB render target, so a colour converted here matches the hardware's.
// Every gamma decode and encode in the engine's shaders goes through this module; the only
// 2.2 power curves left are the AgX tone mapping's and the opacity dither's, which are not
// colour-space conversions. The CPU twins are `gammaToLinear` / `linearToGamma` in
// core/math/color.h. Negative inputs are clamped to 0.
module vtsrgb;

// (c + 0.055) / 1.055 and c / 12.92 written as a multiply-add and a multiply.
public float srgbToLinear(float c)
{
    c = max(c, 0.0);
    return c <= 0.04045 ? c * 0.0773993808 : pow(c * 0.9478672986 + 0.0521327014, 2.4);
}

public float3 srgbToLinear(float3 c)
{
    c = max(c, float3(0.0));
    return select(c <= float3(0.04045), c * 0.0773993808,
                  pow(c * 0.9478672986 + float3(0.0521327014), float3(2.4)));
}

public float linearToSrgb(float c)
{
    c = max(c, 0.0);
    return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}

public float3 linearToSrgb(float3 c)
{
    c = max(c, float3(0.0));
    return select(c <= float3(0.0031308), c * 12.92,
                  1.055 * pow(c, float3(1.0 / 2.4)) - float3(0.055));
}
