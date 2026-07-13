// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
// ---------------------------------------------------------------------------
// PCSS directional shadows (upstream shadowSoft.js PCSSDirectional): Vogel-disk
// blocker search + filter with world-space contact-hardening penumbra.
// DEVIATION: samples the standard hardware depth map raw (non-comparison
// sampler) instead of upstream's dedicated R32F linear-depth map — the ortho
// directional shadow camera stores linear [0,1] depth either way.
// ---------------------------------------------------------------------------

// Raw (non-comparison) depth reads for the blocker search and filter taps.
constexpr sampler shadowRawSampler(coord::normalized, filter::nearest, address::clamp_to_edge);

static inline float pcssFractSinRand(float2 uv)
{
    const float a = 12.9898, b = 78.233, c = 43758.5453;
    const float dt = dot(uv, float2(a, b));
    const float sn = fmod(dt, PI);
    return fract(sin(sn) * c);
}

struct PcssDiskData {
    float invNumSamples;
    float initialAngle;
    float currentPointId;
};

static inline void pcssPrepareDisk(thread PcssDiskData &data, int sampleCount, float randomSeed)
{
    data.invNumSamples = 1.0 / float(sampleCount);
    data.initialAngle = randomSeed * 2.0 * PI;
    data.currentPointId = 0.0;
}

static inline float2 pcssDiskSample(thread PcssDiskData &data)
{
    const float GOLDEN_ANGLE = 2.399963;
    const float r = sqrt((data.currentPointId + 0.5) * data.invNumSamples);
    const float theta = data.currentPointId * GOLDEN_ANGLE + data.initialAngle;
    data.currentPointId += 1.0;
    return float2(r * cos(theta), r * sin(theta));
}

// shadowCoords: atlas UV + depth from the cascade's palette matrix.
// orthoRadius / depthRange: this cascade's shadow-camera world half-extent and
// caster depth span. pcssParams = {filterSamples, blockerSamples, penumbraSize,
// penumbraFalloff}. NOTE: large penumbras can sample across cascade tiles at
// atlas boundaries — cascade blending masks most of it.
static inline float getShadowPCSSDirectional(depth2d<float> shadowMap, float3 shadowCoords,
    float orthoRadius, float depthRange, float4 pcssParams, float2 fragCoord)
{
    const float receiverDepth = shadowCoords.z;
    // clamp so cleared texels (depth = 1) are not treated as blockers when the
    // receiver sits outside the tightened cascade depth range.
    const float receiverDepthClamped = min(receiverDepth, 0.9999);
    const float randomSeed = pcssFractSinRand(fragCoord);
    const int shadowSamples = int(pcssParams.x);
    const int blockerSamples = int(pcssParams.y);
    const float penumbraSize = pcssParams.z;    // world-space light area size
    const float penumbraFalloff = pcssParams.w; // curve shape (>= 1)

    const float worldPerUv = 2.0 * orthoRadius;

    float filterRadius;
    if (blockerSamples > 0) {
        // blocker search radius bounds the largest possible penumbra
        const float searchWidthUv = (penumbraSize * depthRange) / worldPerUv;

        PcssDiskData diskData;
        pcssPrepareDisk(diskData, blockerSamples, randomSeed);
        float blockerSum = 0.0;
        int numBlockers = 0;
        for (int i = 0; i < blockerSamples; ++i) {
            const float2 sampleUv = shadowCoords.xy + pcssDiskSample(diskData) * searchWidthUv;
            const float shadowMapDepth = shadowMap.sample(shadowRawSampler, sampleUv);
            if (shadowMapDepth < receiverDepthClamped) {
                blockerSum += shadowMapDepth;
                numBlockers++;
            }
        }
        if (numBlockers < 1) {
            return 1.0;
        }
        const float avgBlockerDepth = blockerSum / float(numBlockers);

        // world-space penumbra with shape control: reaches penumbraSize*depthRange
        // when the blocker sits at the far end of the caster depth range.
        const float worldDist = max((receiverDepth - avgBlockerDepth) * depthRange, 0.0);
        const float t = clamp(worldDist / depthRange, 0.0, 1.0);
        const float shape = 1.0 - pow(1.0 - t, penumbraFalloff);
        filterRadius = (shape * penumbraSize * depthRange) / worldPerUv;
    } else {
        // constant filter size, no contact hardening
        filterRadius = penumbraSize / worldPerUv;
    }

    PcssDiskData diskData;
    pcssPrepareDisk(diskData, shadowSamples, randomSeed);
    float sum = 0.0;
    for (int i = 0; i < shadowSamples; ++i) {
        const float2 sampleUv = shadowCoords.xy + pcssDiskSample(diskData) * filterRadius;
        const float depth = shadowMap.sample(shadowRawSampler, sampleUv);
        sum += step(receiverDepthClamped, depth);
    }
    return sum / float(shadowSamples);
}


// ---------------------------------------------------------------------------
// Local light PCSS (upstream shadowPCSS.js): contact-hardening shadows for
// spot (2D perspective map) and omni (cubemap) lights. DEVIATION: samples the
// standard hardware depth maps raw and linearizes per tap instead of
// upstream's dedicated R32F linear-depth targets.
// ---------------------------------------------------------------------------

#define PCSS_LOCAL_SAMPLE_COUNT 16

// Perspective [0,1] depth -> linear view distance.
static inline float pcssLinearizeDepth(float z, float nearClip, float farClip)
{
    return (nearClip * farClip) / max(farClip - z * (farClip - nearClip), 1e-6);
}

// Stored omni cube depth (far*(d-near)/((far-near)*d)) -> normalized distance d/far.
static inline float pcssCubeStoredToLinear(float stored, float nearClip, float farClip)
{
    const float d = (farClip * nearClip) / max(farClip - stored * (farClip - nearClip), 1e-6);
    return d / farClip;
}

// Vogel sphere sample (upstream vogelSphere + vogelSpherePrecalculationSamples:
// radius = weight = i/count).
static inline float3 pcssVogelSphere(int sampleIndex, int count, float phi)
{
    const float GOLDEN_ANGLE = 2.4;
    const float theta = float(sampleIndex) * GOLDEN_ANGLE + phi;
    const float weight = float(sampleIndex) / float(count);
    return float3(cos(theta) * weight, weight, sin(theta) * weight);
}

// Spot light PCSS: blocker search + filter in linear view depth. searchArea is
// the blocker-search radius in shadow-map UV (penumbraSize/resolution*fovRatio,
// packed CPU-side). shadowCoords.z arrives with the shader bias applied.
static inline float getShadowPCSSSpot(depth2d<float> shadowMap, float3 shadowCoords,
    float searchArea, float nearClip, float farClip, float2 fragCoord)
{
    const float receiverDepth = pcssLinearizeDepth(shadowCoords.z, nearClip, farClip);
    const float randomSeed = pcssFractSinRand(fragCoord);

    PcssDiskData diskData;
    pcssPrepareDisk(diskData, PCSS_LOCAL_SAMPLE_COUNT, randomSeed);
    float blockerSum = 0.0;
    int numBlockers = 0;
    for (int i = 0; i < PCSS_LOCAL_SAMPLE_COUNT; ++i) {
        const float2 sampleUv = shadowCoords.xy + pcssDiskSample(diskData) * searchArea;
        const float depthLin = pcssLinearizeDepth(
            shadowMap.sample(shadowRawSampler, sampleUv), nearClip, farClip);
        if (depthLin < receiverDepth) {
            blockerSum += depthLin;
            numBlockers++;
        }
    }
    if (numBlockers < 1) {
        return 1.0;
    }
    const float avgBlockerDepth = blockerSum / float(numBlockers);

    // upstream: filterRadius = (receiverDepth - avgBlocker) / 3.0 * searchArea
    const float filterRadius = ((receiverDepth - avgBlockerDepth) / 3.0) * searchArea;

    pcssPrepareDisk(diskData, PCSS_LOCAL_SAMPLE_COUNT, randomSeed);
    float sum = 0.0;
    for (int i = 0; i < PCSS_LOCAL_SAMPLE_COUNT; ++i) {
        const float2 sampleUv = shadowCoords.xy + pcssDiskSample(diskData) * filterRadius;
        const float depthLin = pcssLinearizeDepth(
            shadowMap.sample(shadowRawSampler, sampleUv), nearClip, farClip);
        sum += step(receiverDepth, depthLin);
    }
    return sum / float(PCSS_LOCAL_SAMPLE_COUNT);
}

// Omni light PCSS: Vogel-sphere direction perturbation on the cubemap, blocker
// search + filter in normalized linear distance (upstream PCSSCube).
// lightDir = fragment - light position (world, unnormalized).
static inline float getShadowPCSSOmni(depthcube<float> shadowMap, float3 lightDir,
    float searchArea, float nearClip, float farClip, float bias, float2 fragCoord)
{
    const float receiverDepth = length(lightDir) / farClip - bias;
    const float3 lightDirNorm = normalize(lightDir);
    const float phi = pcssFractSinRand(fragCoord) * 2.0 * PI;

    float blockerSum = 0.0;
    int numBlockers = 0;
    for (int i = 0; i < PCSS_LOCAL_SAMPLE_COUNT; ++i) {
        const float3 sampleDir = normalize(
            lightDirNorm + pcssVogelSphere(i, PCSS_LOCAL_SAMPLE_COUNT, phi) * searchArea);
        const float depthLin = pcssCubeStoredToLinear(
            shadowMap.sample(shadowRawSampler, sampleDir), nearClip, farClip);
        if (depthLin < receiverDepth) {
            blockerSum += depthLin;
            numBlockers++;
        }
    }
    if (numBlockers < 1) {
        return 1.0;
    }
    const float avgBlockerDepth = blockerSum / float(numBlockers);

    // upstream: filterRadius = (receiver - blocker) / blocker * searchArea
    const float filterRadius =
        ((receiverDepth - avgBlockerDepth) / max(avgBlockerDepth, 1e-4)) * searchArea;

    float sum = 0.0;
    for (int i = 0; i < PCSS_LOCAL_SAMPLE_COUNT; ++i) {
        const float3 sampleDir = normalize(
            lightDirNorm + pcssVogelSphere(i, PCSS_LOCAL_SAMPLE_COUNT, phi) * filterRadius);
        const float depthLin = pcssCubeStoredToLinear(
            shadowMap.sample(shadowRawSampler, sampleDir), nearClip, farClip);
        sum += step(receiverDepth, depthLin);
    }
    return sum / float(PCSS_LOCAL_SAMPLE_COUNT);
}

// The glossSq term scales F90 (grazing-angle reflectance) by roughness, preventing
// rough surfaces from showing excessive Fresnel at grazing angles.
