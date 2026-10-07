// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 07.10.2026
//
// The GPU lightmapper's post-bake passes, one per variant:
//   dilate      an unbaked texel takes the first baked one of its eight neighbours, so
//               bilinear filtering across a chart edge does not pull in the black outside
//               the unwrap;
//   denoise     a 15 x 15 bilateral filter over the baked texels only, its spatial kernel
//               and range normaliser computed on the CPU (lightmapFilters.h);
//   ambient-ao  the accumulated ambient visibility (input 0) shaped by the occlusion
//               contrast and brightness, saturated, multiplying the ambient light baked for
//               the same texel (input 1);
//   copy        a plain copy, moving a result back into the lightmap's own target.
// A texel is BAKED when its alpha is above zero. DEVIATION: this port's lightmaps are
// RGBA16F, upstream's HDR mode, whose test reads the colour instead and so takes a baked
// texel that came out black (fully shadowed, no ambient) for an empty one. The bake clears
// its targets to alpha 0 and writes alpha 1, which makes alpha the coverage, as in
// upstream's default RGBM mode. Every tap lands on a texel centre, read nearest.
//
// @variant copy: LM_COPY=1
// @variant dilate: LM_DILATE=1
// @variant denoise: LM_DENOISE=1
// @variant ambient-ao: LM_AMBIENT_AO=1
import vtquad;
#include "bindings.slang"

struct LightmapFilterUniforms
{
    float4 offsetAndSigmas;    // xy = pixel offset, zw = sigmas
    float4 params;             // x = bZnorm, y = occlusion contrast, z = occlusion brightness
    float4 kernelWeights[4];   // the 15 spatial weights, 4 per vec4
};
VT_DECLARE_QUAD_UNIFORMS(LightmapFilterUniforms, u)
VT_DECLARE_QUAD_POINT_INPUT0(source)
#if defined(LM_AMBIENT_AO)
VT_DECLARE_QUAD_POINT_INPUT1(ambientSource)
#endif

bool isUsed(float4 pixel)
{
    return pixel.a > 0.0;
}

float4 tap(float2 uv)
{
    return source.SampleLevel(uv, 0.0);
}

#if defined(LM_DENOISE)
float normpdf3(float3 v, float sigma)
{
    return 0.39894 * exp(-0.5 * dot(v, v) / (sigma * sigma)) / sigma;
}

float kernelAt(int index)
{
    return u.kernelWeights[index >> 2][index & 3];
}
#endif

[shader("vertex")]
QuadVarying lightmapFilterVertex(QuadVertexIn input)
{
    return quadPassThrough(input);
}

[shader("fragment")]
float4 lightmapFilterFragment(QuadVarying input) : SV_Target0
{
    const float2 uv = input.uv;
#if defined(LM_DILATE)
    const float2 o = u.offsetAndSigmas.xy;
    float4 c = tap(uv);
    c = isUsed(c) ? c : tap(uv - o);
    c = isUsed(c) ? c : tap(uv + float2(0.0, -o.y));
    c = isUsed(c) ? c : tap(uv + float2(o.x, -o.y));
    c = isUsed(c) ? c : tap(uv + float2(-o.x, 0.0));
    c = isUsed(c) ? c : tap(uv + float2(o.x, 0.0));
    c = isUsed(c) ? c : tap(uv + float2(-o.x, o.y));
    c = isUsed(c) ? c : tap(uv + float2(0.0, o.y));
    c = isUsed(c) ? c : tap(uv + o);
    return c;
#elif defined(LM_DENOISE)
    const float4 pixel = tap(uv);
    // Unbaked texels pass through untouched, which keeps them unbaked for the dilate.
    if (!isUsed(pixel)) {
        return pixel;
    }
    const float bSigma = u.offsetAndSigmas.w;
    const float3 pixelHdr = pixel.rgb;
    float3 accumulatedHdr = float3(0.0, 0.0, 0.0);
    float accumulatedFactor = 0.000001;
    const int kSize = (15 - 1) / 2;
    for (int i = -kSize; i <= kSize; ++i) {
        for (int j = -kSize; j <= kSize; ++j) {
            const float2 coord = uv + float2(float(i), float(j)) * u.offsetAndSigmas.xy;
            const float4 pix = tap(coord);
            if (isUsed(pix)) {
                const float3 hdr = pix.rgb;
                float factor = kernelAt(kSize + j) * kernelAt(kSize + i);
                factor *= normpdf3(hdr - pixelHdr, bSigma) * u.params.x;
                accumulatedHdr += factor * hdr;
                accumulatedFactor += factor;
            }
        }
    }
    return float4(accumulatedHdr / accumulatedFactor, 1.0);
#elif defined(LM_AMBIENT_AO)
    const float4 occlusion = tap(uv);
    const float4 ambient = ambientSource.SampleLevel(uv, 0.0);
    float3 ao = ((occlusion.rgb - 0.5) * max(u.params.y + 1.0, 0.0)) + 0.5;
    ao += float3(u.params.z, u.params.z, u.params.z);
    ao = saturate(ao);
    return float4(ao * ambient.rgb, ambient.a);
#else
    return tap(uv);
#endif
}
