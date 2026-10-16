// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 16.10.2026
//
// The auto exposure's luminance histogram, in three passes, one variant each:
//   meter  the scene (its half-resolution copy) averaged into a 128 x 64 grid, each texel the
//          average of the whole block of scene texels it covers (neighbouring blocks meet with
//          no gap or overlap, so a small light counts once wherever it is); it stores the
//          block's brightness as its position along the histogram's range, in 16 bits, the
//          high and the low byte in red and green of an RGBA8 target;
//   rows   each fragment (i, row) counts the meter texels of its row into bins 4i..4i+3,
//          GATHERING rather than scattering with blending, so the counts are exact; a texel
//          adds itself to its two nearest bins by its distance to them, so the histogram changes
//          gradually, weighted towards the centre of the screen by centerWeight;
//   sum    the rows summed into the histogram of the whole screen, 64 bins as 16 RGBA texels,
//          which is read back.
// Input 0 per variant: meter the scene, rows the meter, sum the rows.
//
// DEVIATION: the counts are floats in RGBA32F targets (exact: a bin of the whole screen
// counts at most 8192 weights), where upstream's are 16.16 fixed point in RGBA32U ones; the
// engine has no integer formats.
//
// @variant meter: AE_PASS=0
// @variant rows: AE_PASS=1
// @variant sum: AE_PASS=2
import vtquad;
#include "bindings.slang"

static const int kMeterWidth = 128;
static const int kMeterHeight = 64;
static const int kBins = 64;

struct AutoExposureUniforms
{
    float4 source;      // xy = the scene's size in texels, z = log2 luminance at the range's start,
                        // w = 1 / the range's size
    float4 params;      // x = centre weight 0-1
};
VT_DECLARE_QUAD_UNIFORMS(AutoExposureUniforms, u)

[shader("vertex")]
QuadVarying autoExposureVertex(QuadVertexIn input)
{
    return quadPassThrough(input);
}

#if AE_PASS == 0

VT_DECLARE_QUAD_INPUT0(sourceTexture)

float4 meter(int2 texel)
{
    // The block of source texels this meter texel covers.
    const int2 meterSize = int2(kMeterWidth, kMeterHeight);
    const int2 sourceSize = int2(u.source.xy);
    const int2 start = texel * sourceSize / meterSize;
    const int2 end = max((texel + 1) * sourceSize / meterSize, start + 1);

    // Sampled two by two texels, between the four, where the bilinear filter weights them
    // equally (or between two, or at one, at the edge of a block of an odd size).
    float3 sum = float3(0.0, 0.0, 0.0);
    float count = 0.0;
    for (int y = start.y; y < end.y; y += 2) {
        const int h = min(2, end.y - y);
        for (int x = start.x; x < end.x; x += 2) {
            const int w = min(2, end.x - x);
            const float2 uv = (float2(float(x), float(y)) + float2(float(w), float(h)) * 0.5) / u.source.xy;
            const float n = float(w * h);
            sum += sourceTexture.SampleLevel(uv, 0.0).rgb * n;
            count += n;
        }
    }

    // The position in 16 bits: the high and the low byte in red and green.
    const float luminance = dot(sum / count, float3(0.2126, 0.7152, 0.0722));
    const float position = clamp((log2(max(luminance, 1.0e-10)) - u.source.z) * u.source.w, 0.0, 1.0);
    const float value = floor(position * 65535.0 + 0.5);
    const float high = floor(value / 256.0);
    return float4(high, value - high * 256.0, 0.0, 255.0) / 255.0;
}

#elif AE_PASS == 1

VT_DECLARE_QUAD_TEXEL_INPUT0(meterTexture)

float4 rows(int2 fragment)
{
    const int row = fragment.y;
    const float4 bins = float4(float(fragment.x * 4), float(fragment.x * 4), float(fragment.x * 4),
        float(fragment.x * 4)) + float4(0.0, 1.0, 2.0, 3.0);

    // The metering weight of the row's texels, by their distance to the centre.
    const float v = (float(row) + 0.5) / float(kMeterHeight) - 0.5;

    float4 counts = float4(0.0, 0.0, 0.0, 0.0);
    for (int x = 0; x < kMeterWidth; ++x) {
        const float2 bytes = floor(meterTexture.Load(int3(x, row, 0)).rg * 255.0 + 0.5);
        const float position = (bytes.r * 256.0 + bytes.g) / 65535.0 * float(kBins - 1);
        const float uc = (float(x) + 0.5) / float(kMeterWidth) - 0.5;
        const float weight = lerp(1.0, exp(-8.0 * (uc * uc + v * v)), u.params.x);
        counts += max(float4(0.0, 0.0, 0.0, 0.0), 1.0 - abs(bins - position)) * weight;
    }
    return counts;
}

#else

VT_DECLARE_QUAD_TEXEL_INPUT0(rowsTexture)

float4 sum(int2 fragment)
{
    float4 counts = float4(0.0, 0.0, 0.0, 0.0);
    for (int row = 0; row < kMeterHeight; ++row) {
        counts += rowsTexture.Load(int3(fragment.x, row, 0));
    }
    return counts;
}

#endif

// One entry for the three variants (the bundle finds entries in the source text), running
// the pass its variant compiles at this fragment's texel.
[shader("fragment")]
float4 autoExposureFragment(QuadVarying input) : SV_Target0
{
    const int2 texel = int2(floor(input.position.xy));
#if AE_PASS == 0
    return meter(texel);
#elif AE_PASS == 1
    return rows(texel);
#else
    return sum(texel);
#endif
}
