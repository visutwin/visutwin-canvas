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
            const float shadowMapDepth = shadowMap.sample(shadowRawSampler, sampleUv, level(0));
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
        const float depth = shadowMap.sample(shadowRawSampler, sampleUv, level(0));
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
            shadowMap.sample(shadowRawSampler, sampleUv, level(0)), nearClip, farClip);
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
            shadowMap.sample(shadowRawSampler, sampleUv, level(0)), nearClip, farClip);
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
            shadowMap.sample(shadowRawSampler, sampleDir, level(0)), nearClip, farClip);
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
            shadowMap.sample(shadowRawSampler, sampleDir, level(0)), nearClip, farClip);
        sum += step(receiverDepth, depthLin);
    }
    return sum / float(PCSS_LOCAL_SAMPLE_COUNT);
}

// One directional shadow slot's visibility (1 = lit): the cascade pick and its
// dither, the normal offset, the projection, the filter and the fade at the
// shadow distance. Both slots call it with their own uniforms and map, so the
// second directional shadow cannot drift from the first. `biasNormalStrength.w`
// is the slot's enable flag, checked by the caller.
#if VT_FEATURE_VSM_SHADOWS
#define VT_DIRECTIONAL_SHADOW_MAP texture2d<float>
#else
#define VT_DIRECTIONAL_SHADOW_MAP depth2d<float>
#endif
static inline float evaluateDirectionalShadow(VT_DIRECTIONAL_SHADOW_MAP shadowMap,
    constant float4x4* palette, float4 cascadeDistances, float4 cascadeParams,
    float4 biasNormalStrength, float4 pcssParams, float4 pcssRadii, float4 pcssDepthRanges,
    float3 worldPos, float3 N, float3 L, float linearDepth, float2 fragCoord)
{
    float shadowFactor = 1.0;
    const int cascadeCount = max(int(cascadeParams.x), 1);
    // Beyond the shadow distance the fragment is lit, and nothing is
    // sampled (upstream a59f9ef29).
    const float shadowDistance = cascadeDistances[cascadeCount - 1];
    if (linearDepth > shadowDistance) {
        return shadowFactor;
    }
    // cascadeBlend is a FRACTION (upstream): it dithers the cascade pick
    // across the end of each cascade and fades the shadow out toward the
    // shadow distance, and 0 turns both off.
    const float cascadeBlend = cascadeParams.y;
    int cascadeIndex = getShadowCascadeIndex(cascadeDistances, cascadeCount, linearDepth);
    if (cascadeBlend > 0.0) {
        cascadeIndex = ditherShadowCascadeIndex(cascadeIndex, cascadeDistances, cascadeCount,
            cascadeBlend, linearDepth, fragCoord);
    }

    // Apply normal bias in world space, scaled by sin(angle) between
    // normal and light direction so grazing surfaces get more offset
    // while directly-lit faces get almost none — prevents light leaking
    // at triangle edges on curved geometry.
    const float csmNdotL = saturate(dot(N, L));
    const float csmSinAngle = sqrt(1.0 - csmNdotL * csmNdotL);
    const float3 worldPosBiased = worldPos + N * (biasNormalStrength.y * csmSinAngle);

    // Transform world position via the cascade's viewport-scaled shadow matrix.
    // The matrix already bakes in projection, view, NDC-to-atlas-UV, Metal Y-flip,
    // and Z [0,1] mapping — no manual coordinate conversion needed.
    const float4 shadowClip = palette[cascadeIndex] * float4(worldPosBiased, 1.0);
    const float shadowW = max(shadowClip.w, 1e-6);
    const float3 shadowCoord = shadowClip.xyz / shadowW;

    const float2 shadowUv = shadowCoord.xy;
    // The receiver's depth is SATURATED, not range-tested (upstream's
    // getShadowSampleCoord for an ortho light). The shadow camera's near
    // and far are fitted to the CASTERS each frame, so a receiver that is
    // not itself a caster — a ground plane, or any surface further along
    // the light than the last caster — projects to z > 1. Rejecting it
    // left every such receiver unshadowed, and where the fit ended INSIDE
    // a shadow the shadow was cut off along a straight line that moved
    // with the casters' bounds: a fly's body shadow ended in a hard edge
    // that flickered with every wing beat. Clamped to 1 it compares
    // against the cleared map (1.0) as lit and against any caster in
    // front as shadowed, which is the right answer for both. A receiver
    // in front of the near plane clamps to 0 and is lit, since nothing
    // casts from in front of the nearest caster. The UV test stays: a
    // point outside the cascade's footprint has no map to read.
    const float shadowDepth = saturate(shadowCoord.z);

    const float resolution = float(shadowMap.get_width());
    const bool insideShadow = shadowUv.x >= 0.0 && shadowUv.x <= 1.0 &&
        shadowUv.y >= 0.0 && shadowUv.y <= 1.0;
    if (insideShadow) {
#if VT_FEATURE_VSM_SHADOWS
        // EVSM_16F — sample exponentially-warped moments and reconstruct
        // visibility via Chebyshev's inequality. No depth-bias subtraction —
        // VSM uses a separate vsmBias inside calculateEVSM.
        const float vsmBias = max(biasNormalStrength.x, 1e-4);
        const float visible = getShadowVSM16(shadowMap, shadowUv, shadowDepth, vsmBias);
#elif VT_FEATURE_PCSS_SHADOWS
        // PCSS — contact-hardening soft shadows: Vogel-disk blocker search
        // sets a world-space penumbra per fragment.
        const float pcssDepth = shadowDepth - biasNormalStrength.x;
        const float visible = getShadowPCSSDirectional(shadowMap,
            float3(shadowUv, pcssDepth), pcssRadii[cascadeIndex], pcssDepthRanges[cascadeIndex],
            pcssParams, fragCoord);
#else
        // PCF3_32F — optimized bilinear 3×3 PCF.
        const float receiverDepth = shadowDepth - biasNormalStrength.x;
        const float visible = getShadowPCF3x3(shadowMap, shadowUv, receiverDepth, resolution);
#endif
        shadowFactor = mix(1.0 - clamp(biasNormalStrength.z, 0.0, 1.0), 1.0, visible);
    }

    // Fade to fully lit at the shadow distance without sampling another
    // cascade (upstream 0b30839ea). The shadow intensity is already folded
    // into shadowFactor, which commutes with this mix.
    // NOTE: this is upstream's CODE, which starts the fade at cascadeBlend x
    // the distance, so 0.1 fades over the last 90%; upstream's JSDoc says
    // "the last 10%". The shader is what upstream renders.
    if (cascadeBlend > 0.0) {
        shadowFactor = mix(shadowFactor, 1.0,
            smoothstep(cascadeBlend * shadowDistance, shadowDistance, linearDepth));
    }
    return shadowFactor;
}

// The glossSq term scales F90 (grazing-angle reflectance) by roughness, preventing
// rough surfaces from showing excessive Fresnel at grazing angles.
