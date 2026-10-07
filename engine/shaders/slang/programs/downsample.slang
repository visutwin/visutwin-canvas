// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 07.10.2026
//
// The downsamples of the bloom mip chain and the depth-of-field far pass, one source
// with four variants:
//   box               a single bilinear tap (the hardware averages 2x2), for the
//                     scene-half copy that must stay crisp;
//   box-premultiply-r the box tap multiplied by the r channel of a second texture at the
//                     same uv: the depth-of-field far pass weights the scene by the far
//                     circle of confusion before blurring, so in-focus pixels do not bleed;
//   karis             the 13-tap partial average of Call of Duty: Advanced Warfare's "Next
//                     Generation Post Processing", for the bloom chain: it damps fireflies
//                     at each mip so a single very bright pixel does not cascade unfiltered;
//   karis-prefilter   the same plus a soft-knee high pass on the FILTERED result (knee =
//                     half the threshold), compiled only into the first bloom downsample
//                     while the bloom threshold is above zero. Running after the filter
//                     means an isolated bright pixel the filter already averaged down needs
//                     a proportionally lower threshold.
// Every variant clamps negative or invalid values so bloom and depth of field cannot
// propagate NaN or Inf from the scene texture.
//
// @variant box: BOX=1
// @variant box-premultiply-r: BOX=1 PREMULTIPLY=1
// @variant karis: KARIS=1
// @variant karis-prefilter: KARIS=1 PREFILTER=1
import vtquad;
#include "bindings.slang"

VT_DECLARE_QUAD_INPUT0(sourceTexture)
#if defined(PREMULTIPLY)
VT_DECLARE_QUAD_INPUT1(premultiplyTexture)
#endif
#if defined(PREFILTER)
struct PrefilterUniforms
{
    float4 thresholdKnee;   // x: threshold, y: knee
};
VT_DECLARE_QUAD_UNIFORMS(PrefilterUniforms, u)
#endif

[shader("vertex")]
QuadVarying downsampleVertex(QuadVertexIn input)
{
    return quadPassThrough(input);
}

[shader("fragment")]
float4 downsampleFragment(QuadVarying input) : SV_Target0
{
#if defined(BOX)
    const float2 uv = clamp(input.uv, float2(0.0, 0.0), float2(1.0, 1.0));
    float3 value = max(sourceTexture.Sample(uv).rgb, float3(0.0, 0.0, 0.0));
#if defined(PREMULTIPLY)
    value *= premultiplyTexture.Sample(uv).r;
#endif
    return float4(value, 1.0);
#else
    uint width;
    uint height;
    sourceTexture.GetDimensions(width, height);
    const float2 texel = float2(1.0 / float(width), 1.0 / float(height));
    const float2 uv = clamp(input.uv, float2(0.0, 0.0), float2(1.0, 1.0));
    const float x = texel.x;
    const float y = texel.y;

    const float3 e = sourceTexture.Sample(uv).rgb;

    // outer ring (corners + mid-edges) at 2*texel offset
    const float3 a = sourceTexture.Sample(float2(uv.x - 2.0 * x, uv.y + 2.0 * y)).rgb;
    const float3 b = sourceTexture.Sample(float2(uv.x,           uv.y + 2.0 * y)).rgb;
    const float3 c = sourceTexture.Sample(float2(uv.x + 2.0 * x, uv.y + 2.0 * y)).rgb;
    const float3 d = sourceTexture.Sample(float2(uv.x - 2.0 * x, uv.y          )).rgb;
    const float3 f = sourceTexture.Sample(float2(uv.x + 2.0 * x, uv.y          )).rgb;
    const float3 g = sourceTexture.Sample(float2(uv.x - 2.0 * x, uv.y - 2.0 * y)).rgb;
    const float3 h = sourceTexture.Sample(float2(uv.x,           uv.y - 2.0 * y)).rgb;
    const float3 i = sourceTexture.Sample(float2(uv.x + 2.0 * x, uv.y - 2.0 * y)).rgb;

    // inner diamond at texel offset (contributes half the total weight)
    const float3 j = sourceTexture.Sample(float2(uv.x - x, uv.y + y)).rgb;
    const float3 k = sourceTexture.Sample(float2(uv.x + x, uv.y + y)).rgb;
    const float3 l = sourceTexture.Sample(float2(uv.x - x, uv.y - y)).rgb;
    const float3 m = sourceTexture.Sample(float2(uv.x + x, uv.y - y)).rgb;

    float3 value = e * 0.125;
    value += (a + c + g + i) * 0.03125;
    value += (b + d + f + h) * 0.0625;
    value += (j + k + l + m) * 0.125;

    value = max(value, float3(0.0, 0.0, 0.0));
#if defined(PREFILTER)
    {
        const float luminance = max(value.r, max(value.g, value.b));
        const float threshold = u.thresholdKnee.x;
        const float knee = u.thresholdKnee.y;
        float soft = clamp(luminance - threshold + knee, 0.0, 2.0 * knee);
        soft = soft * soft / (4.0 * knee + 1e-4);
        value *= clamp(max(soft, luminance - threshold) / max(luminance, 1e-4), 0.0, 1.0);
    }
#endif
    return float4(value, 1.0);
#endif
}
