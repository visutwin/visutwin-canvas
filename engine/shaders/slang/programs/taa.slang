// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 07.10.2026
//
// The TAA resolve: the pixel is reprojected into the previous frame through the scene
// depth (point sampled: an edge pixel reprojected with a depth halfway between the object
// and the background fetches history from neither and ghosts), the history is fetched with
// Catmull-Rom or bilinear, clamped to the 3x3 neighbourhood of the current frame and mixed
// 5% (100% when the reprojection leaves the frame), rising towards 50% with the distance the
// history was reprojected from. Inputs: 0 source, 1 history, 2 depth.
//
// The mix is taken under a reversible tone mapping of the EXPOSED colours. Mixed in linear
// HDR, the samples of a bright surface partially covering a pixel dominate its average, which
// the display tone mapping then shows nearly as bright as the surface: the edges of bright
// geometry stay thick and aliased.
//
// The clamp and the mix happen in PREMULTIPLIED space, the coverage-correct one (straight
// RGB interpolates colour and opacity independently at edges, which fringes and ghosts),
// and the result is un-premultiplied by the CURRENT alpha: alpha is not filtered over time.
// With alpha 1 that is the straight-RGB mix bit for bit.
//
// DEVIATION: upstream linearizes and delinearizes the depth around the reprojection; the
// round trip is an identity on the raw hardware depth, so the raw depth is used. The depth
// holds (ndcZ_gl + 1) / 2 and is mapped back to GL NDC Z; the uv has v = 0 at the top while
// the projection is GL's (+Y up), so ndcY = 1 - 2v.
import vtquad;
import vttonemapreversible;
#include "bindings.slang"

struct TaaUniforms
{
    float4x4 viewProjectionPrevious;
    float4x4 viewProjectionInverse;
    float4 jitters;        // xy = current-frame NDC jitter, zw = previous frame's
    float4 texSizeFlags;   // xy = texture size, z = highQuality, w = historyValid
    float4 cameraParams;
    float4 blendParams;    // x = the exposure the scene is displayed with
};

// The most of the current frame taken while the camera moves a pixel's surface across the
// screen, and the motion (in history pixels) at which all of it is taken. With the camera
// still the history accumulates at the base weight.
static const float kTaaMotionBlend = 0.5;
static const float kTaaMotionPixels = 32.0;
VT_DECLARE_QUAD_UNIFORMS(TaaUniforms, u)
VT_DECLARE_QUAD_INPUT0(sourceTexture)
VT_DECLARE_QUAD_INPUT1(historyTexture)
VT_DECLARE_QUAD_DEPTH_INPUT2(depthTexture)

float2 reproject(float2 uv, float depth)
{
    depth = depth * 2.0 - 1.0;
    float4 ndc = float4(uv.x * 2.0 - 1.0, 1.0 - 2.0 * uv.y, depth, 1.0);
    ndc.xy -= u.jitters.xy;

    float4 worldPosition = mul(ndc, u.viewProjectionInverse);
    worldPosition /= worldPosition.w;

    // The previous frame's jitter is removed too: the history holds the image without it.
    const float4 screenPrevious = mul(worldPosition, u.viewProjectionPrevious);
    const float2 prevNdc = screenPrevious.xy / screenPrevious.w + u.jitters.zw;
    return float2(prevNdc.x * 0.5 + 0.5, 0.5 - prevNdc.y * 0.5);
}

float4 sampleCatmullRom(float2 uv, float2 texSize)
{
    const float2 samplePos = uv * texSize;
    const float2 texPos1 = floor(samplePos - 0.5) + 0.5;
    const float2 f = samplePos - texPos1;

    const float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    const float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    const float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    const float2 w3 = f * f * (-0.5 + 0.5 * f);

    const float2 w12 = w1 + w2;
    const float2 offset12 = w2 / (w1 + w2);

    const float2 texPos0 = (texPos1 - 1.0) / texSize;
    const float2 texPos3 = (texPos1 + 2.0) / texSize;
    const float2 texPos12 = (texPos1 + offset12) / texSize;

    float4 result = float4(0.0, 0.0, 0.0, 0.0);
    result += historyTexture.SampleLevel(float2(texPos0.x, texPos0.y), 0.0) * w0.x * w0.y;
    result += historyTexture.SampleLevel(float2(texPos12.x, texPos0.y), 0.0) * w12.x * w0.y;
    result += historyTexture.SampleLevel(float2(texPos3.x, texPos0.y), 0.0) * w3.x * w0.y;

    result += historyTexture.SampleLevel(float2(texPos0.x, texPos12.y), 0.0) * w0.x * w12.y;
    result += historyTexture.SampleLevel(float2(texPos12.x, texPos12.y), 0.0) * w12.x * w12.y;
    result += historyTexture.SampleLevel(float2(texPos3.x, texPos12.y), 0.0) * w3.x * w12.y;

    result += historyTexture.SampleLevel(float2(texPos0.x, texPos3.y), 0.0) * w0.x * w3.y;
    result += historyTexture.SampleLevel(float2(texPos12.x, texPos3.y), 0.0) * w12.x * w3.y;
    result += historyTexture.SampleLevel(float2(texPos3.x, texPos3.y), 0.0) * w3.x * w3.y;
    return result;
}

float3 colorClampPremul(float2 uv, float3 historyPremul, float2 texSize)
{
    float3 minPremul = float3(9999.0, 9999.0, 9999.0);
    float3 maxPremul = float3(-9999.0, -9999.0, -9999.0);
    for (float x = -1.0; x <= 1.0; x += 1.0) {
        for (float y = -1.0; y <= 1.0; y += 1.0) {
            const float4 s = sourceTexture.Sample(uv + float2(x, y) / texSize);
            const float3 premul = s.rgb * s.a;
            minPremul = min(minPremul, premul);
            maxPremul = max(maxPremul, premul);
        }
    }
    return clamp(historyPremul, minPremul, maxPremul);
}

[shader("vertex")]
QuadVarying taaVertex(QuadVertexIn input)
{
    return quadPassThrough(input);
}

[shader("fragment")]
float4 taaFragment(QuadVarying input) : SV_Target0
{
    const float2 texSize = u.texSizeFlags.xy;
    const float2 uv = clamp(input.uv, float2(0.0, 0.0), float2(1.0, 1.0));

    const float4 srcColor = sourceTexture.Sample(uv);
    if (u.texSizeFlags.w < 0.5) {   // no valid history yet
        return srcColor;
    }

    const float depth = depthTexture.Sample(uv);
    const float2 historyUv = reproject(uv, depth);

    const float4 historyColor = (u.texSizeFlags.z > 0.5)
        ? sampleCatmullRom(historyUv, texSize)
        : historyTexture.Sample(historyUv);

    const float3 historyPremul = historyColor.rgb * historyColor.a;
    const float3 srcPremul = srcColor.rgb * srcColor.a;
    const float3 historyPremulClamped = colorClampPremul(uv, historyPremul, texSize);

    // The further the history was reprojected from, the further it has been resampled (which
    // blurs it) and the likelier it does not hold this surface at all.
    const float baseBlend = 0.05;
    const float motion = length((uv - historyUv) * texSize);
    const float blend = lerp(baseBlend, max(baseBlend, kTaaMotionBlend),
                             clamp(motion / kTaaMotionPixels, 0.0, 1.0));
    const float mixFactor = (historyUv.x < 0.0 || historyUv.x > 1.0 ||
                             historyUv.y < 0.0 || historyUv.y > 1.0) ? 1.0 : blend;

    const float e = max(u.blendParams.x, 1e-6);
    const float3 mixedPremul = reversibleTonemapInverse(
        lerp(reversibleTonemap(historyPremulClamped * e), reversibleTonemap(srcPremul * e), mixFactor)) / e;
    const float a = srcColor.a;
    // Below one UNORM8 alpha step un-premultiplying is ill-defined; use the current RGB.
    const float3 rgbStraight = (a > 1.0 / 255.0) ? (mixedPremul / a) : srcColor.rgb;
    return float4(rgbStraight, a);
}
