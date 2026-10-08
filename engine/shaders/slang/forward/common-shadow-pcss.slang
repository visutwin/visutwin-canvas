// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 07.10.2026
//
// ── Directional shadow slots ──
// Two directional lights can be shadowed (ShadowParams::kMaxDirectionalShadows);
// the light's coneParams.w picks the slot. These accessors return a slot's
// uniforms, and directionalShadowTexel reads its map. The maps are SEPARATE
// images, and a sampler built over one may only appear where it is used — never
// as a call argument — so the slot is chosen at each tap. Each map reads through
// the filter its own sampler has (ShadowMap::create): EVSM moments linear,
// depth nearest. The shadow type is scene-wide, so slot 0's mode decides.

float4x4 directionalShadowMatrix(int slot, int cascade) {
    return slot == 0 ? lighting.shadowMatrices[cascade] : lighting.dirShadow1Matrices[cascade];
}
float4 directionalShadowDistances(int slot) {
    return slot == 0 ? lighting.shadowCascadeDistances : lighting.dirShadow1CascadeDistances;
}
// enabled, numCascades, depthBias, strength
float4 directionalShadowParams(int slot) {
    return slot == 0 ? lighting.shadowParams : lighting.dirShadow1Params;
}
// normalBias, cascadeBlend (slot 0's zw carry unrelated values)
float2 directionalShadowParams2(int slot) {
    return slot == 0 ? lighting.shadowParams2.xy : lighting.dirShadow1Params2.xy;
}
float4 directionalShadowPcss(int slot) {
    return slot == 0 ? lighting.pcssParams : lighting.dirShadow1PcssParams;
}
float4 directionalShadowPcssRadii(int slot) {
    return slot == 0 ? lighting.pcssCascadeRadii : lighting.dirShadow1PcssCascadeRadii;
}
float4 directionalShadowPcssDepthRanges(int slot) {
    return slot == 0 ? lighting.pcssCascadeDepthRanges : lighting.dirShadow1PcssCascadeDepthRanges;
}

float4 directionalShadowTexel(int slot, float2 uv) {
#if defined(VT_TARGET_METAL)
    // On Metal a VSM slot's moments are a map of their own (39 / 40), the same texture bound
    // at both; the variant reads one of the pair, by the VSM FEATURE, so specialisation
    // leaves the other argument inactive (a colour map at a depth slot is a type mismatch).
    if (vtFeatureEnabled(VT_FEATURE_VSM_SHADOWS_BIT)) {
        return slot == 0 ? shadowMoments0.SampleLevel(linearClampSampler, uv, 0.0)
                         : shadowMoments1.SampleLevel(linearClampSampler, uv, 0.0);
    }
    return float4(directionalShadowDepth(slot, uv), 0.0, 0.0, 0.0);
#else
    if (lighting.shadowParams.x > 1.5) {
        return slot == 0 ? shadowMap0.SampleLevel(linearClampSampler, uv, 0.0)
                         : shadowMap1.SampleLevel(linearClampSampler, uv, 0.0);
    }
    return slot == 0 ? shadowMap0.SampleLevel(nearestClampSampler, uv, 0.0)
                     : shadowMap1.SampleLevel(nearestClampSampler, uv, 0.0);
#endif
}

// A depth map's texel (PCF, PCSS): always the nearest sampler.
float directionalShadowDepth(int slot, float2 uv) {
#if defined(VT_TARGET_METAL)
    if (vtFeatureEnabled(VT_FEATURE_VSM_SHADOWS_BIT)) {
        return 1.0;
    }
#endif
    return slot == 0 ? shadowDepthReadDirectional(shadowMap0, uv) : shadowDepthReadDirectional(shadowMap1, uv);
}

float2 directionalShadowSize(int slot) {
#if defined(VT_TARGET_METAL)
    if (vtFeatureEnabled(VT_FEATURE_VSM_SHADOWS_BIT)) {
        return slot == 0 ? float2(vtTextureSize(shadowMoments0, 0)) : float2(vtTextureSize(shadowMoments1, 0));
    }
#endif
    return slot == 0 ? float2(vtTextureSize(shadowMap0, 0))
                     : float2(vtTextureSize(shadowMap1, 0));
}

// One bilinear comparison on a directional slot's map (see shadowTap in
// common-parallax: the map is a separate image, so the slot is chosen at the gather).
float directionalShadowTap(int slot, float2 uv, float2 size, float receiver) {
    float2 f;
    float2 corner = shadowTapCorner(uv, size, f);
#if defined(VT_TARGET_METAL)
    if (vtFeatureEnabled(VT_FEATURE_VSM_SHADOWS_BIT)) {
        return 1.0;
    }
    return slot == 0 ? shadowCompare(shadowMap0, uv, receiver) : shadowCompare(shadowMap1, uv, receiver);
#else
    float4 depths = slot == 0 ? shadowMap0.GatherRed(nearestClampSampler, corner)
                            : shadowMap1.GatherRed(nearestClampSampler, corner);
    return shadowTapResult(depths, f, receiver);
#endif
}

// pcf3x3 over a directional slot's map.
float pcf3x3Directional(int slot, float2 uv, float receiver) {
    float2 size = directionalShadowSize(slot);
    float2 taps[4];
    float weights[4];
    pcf3x3Taps(uv, size, taps, weights);
    float sum = 0.0;
    for (int i = 0; i < 4; ++i) {
        sum += weights[i] * directionalShadowTap(slot, taps[i], size, receiver);
    }
    return sum;
}

// PCF1 over a directional slot's map: one bilinear comparison.
float pcf1Directional(int slot, float2 uv, float receiver) {
    return directionalShadowTap(slot, uv, directionalShadowSize(slot), receiver);
}

// PCF5 over a directional slot's map: nine bilinear comparisons.
float pcf5x5Directional(int slot, float2 uv, float receiver) {
    float2 size = directionalShadowSize(slot);
    float2 taps[9];
    float weights[9];
    pcf5x5Taps(uv, size, taps, weights);
    float sum = 0.0;
    for (int i = 0; i < 9; ++i) {
        sum += weights[i] * directionalShadowTap(slot, taps[i], size, receiver);
    }
    return clamp(sum, 0.0, 1.0);
}

// ── PCSS: contact-hardening soft shadows ──
// Vogel-disk blocker search sizes a per-fragment penumbra, then a second disk
// pass filters at that radius.  Every shadow map here is already bound through
// a NEAREST clamp-to-edge non-comparison sampler.  The directional path is
// selected by the VT_FEATURE_PCSS_SHADOWS specialization constant; the
// spot/omni paths branch at runtime on a non-zero search area.

// For the non-negative vtFragCoord.xy inputs fmod and the floor-based glslMod
// are identical.
float pcssFractSinRand(float2 uv) {
    const float a = 12.9898, b = 78.233, c = 43758.5453;
    float dt = dot(uv, float2(a, b));
    return frac(sin(glslMod(dt, PI)) * c);
}

// Vogel disk: point `id` of `invCount` = 1/count, rotated by `initialAngle`.
float2 pcssDiskSample(float id, float invCount, float initialAngle) {
    const float GOLDEN_ANGLE = 2.399963;
    float r = sqrt((id + 0.5) * invCount);
    float theta = id * GOLDEN_ANGLE + initialAngle;
    return float2(r * cos(theta), r * sin(theta));
}

// Directional PCSS.  `orthoRadius` / `depthRange` are the cascade's shadow-camera
// world half-extent and caster depth span; `receiverDepth` arrives biased.
float getShadowPCSSDirectional(int slot, float2 uv, float receiverDepth,
                               float orthoRadius, float depthRange) {
    // Clamp so cleared texels (depth 1) are not treated as blockers when the
    // receiver sits outside the tightened cascade depth range.
    float receiverDepthClamped = min(receiverDepth, 0.9999);
    float initialAngle = pcssFractSinRand(vtFragCoord.xy) * 2.0 * PI;

    // A zero filter count would divide the accumulated visibility by zero and
    // poison the frame with NaN; the renderer always sends 16.
    float4 pcssParams = directionalShadowPcss(slot);
    int shadowSamples = max(int(pcssParams.x), 1);
    int blockerSamples = int(pcssParams.y);
    float penumbraSize = pcssParams.z;
    float penumbraFalloff = pcssParams.w;

    float worldPerUv = 2.0 * orthoRadius;

    float filterRadius;
    if (blockerSamples > 0) {
        // The blocker search radius bounds the largest possible penumbra.
        float searchWidthUv = (penumbraSize * depthRange) / worldPerUv;
        float invBlockers = 1.0 / float(blockerSamples);
        float blockerSum = 0.0;
        int numBlockers = 0;
        for (int i = 0; i < blockerSamples; ++i) {
            float2 sampleUv = uv +
                pcssDiskSample(float(i), invBlockers, initialAngle) * searchWidthUv;
            float occluder = directionalShadowDepth(slot, sampleUv);
            if (occluder < receiverDepthClamped) {
                blockerSum += occluder;
                numBlockers++;
            }
        }
        if (numBlockers < 1) {
            return 1.0;
        }
        float avgBlockerDepth = blockerSum / float(numBlockers);

        // World-space penumbra with shape control: reaches penumbraSize *
        // depthRange when the blocker sits at the far end of the caster range.
        float worldDist = max((receiverDepth - avgBlockerDepth) * depthRange, 0.0);
        float t = clamp(worldDist / depthRange, 0.0, 1.0);
        float shape = 1.0 - pow(1.0 - t, penumbraFalloff);
        filterRadius = (shape * penumbraSize * depthRange) / worldPerUv;
    } else {
        // Constant filter size — no contact hardening.
        filterRadius = penumbraSize / worldPerUv;
    }

    float invSamples = 1.0 / float(shadowSamples);
    float sum = 0.0;
    for (int i = 0; i < shadowSamples; ++i) {
        float2 sampleUv = uv +
            pcssDiskSample(float(i), invSamples, initialAngle) * filterRadius;
        sum += step(receiverDepthClamped, directionalShadowDepth(slot, sampleUv));
    }
    return sum * invSamples;
}

// Local-light PCSS works in linear view distance, so every tap is linearized.
static const int PCSS_LOCAL_SAMPLE_COUNT = 16;

float pcssLinearizeDepth(float z, float nearClip, float farClip) {
    return (nearClip * farClip) / max(farClip - z * (farClip - nearClip), 1e-6);
}

// Stored omni depth (far*(d-near)/((far-near)*d)) → normalized distance d/far.
float pcssCubeStoredToLinear(float stored, float nearClip, float farClip) {
    float d = (farClip * nearClip) / max(farClip - stored * (farClip - nearClip), 1e-6);
    return d / farClip;
}

// Vogel sphere (radius = weight = i/count).
float3 pcssVogelSphere(int sampleIndex, int count, float phi) {
    const float GOLDEN_ANGLE = 2.4;
    float theta = float(sampleIndex) * GOLDEN_ANGLE + phi;
    float weight = float(sampleIndex) / float(count);
    return float3(cos(theta) * weight, weight, sin(theta) * weight);
}

// Spot PCSS.  `searchArea` is the blocker-search radius in shadow-map UV
// (penumbraSize / resolution * fovRatio, packed CPU-side); `receiverZ` arrives
// with the depth bias already applied.
float getShadowPCSSSpot(LocalShadowMap tex, float2 uv, float receiverZ,
                        float searchArea, float nearClip, float farClip) {
    float receiverDepth = pcssLinearizeDepth(receiverZ, nearClip, farClip);
    float initialAngle = pcssFractSinRand(vtFragCoord.xy) * 2.0 * PI;
    const float invCount = 1.0 / float(PCSS_LOCAL_SAMPLE_COUNT);

    float blockerSum = 0.0;
    int numBlockers = 0;
    for (int i = 0; i < PCSS_LOCAL_SAMPLE_COUNT; ++i) {
        float2 sampleUv = uv +
            pcssDiskSample(float(i), invCount, initialAngle) * searchArea;
        float depthLin = pcssLinearizeDepth(shadowDepthRead(tex, sampleUv), nearClip, farClip);
        if (depthLin < receiverDepth) {
            blockerSum += depthLin;
            numBlockers++;
        }
    }
    if (numBlockers < 1) {
        return 1.0;
    }
    float avgBlockerDepth = blockerSum / float(numBlockers);

    // filterRadius = (receiver - avgBlocker) / 3 * searchArea
    float filterRadius = ((receiverDepth - avgBlockerDepth) / 3.0) * searchArea;

    float sum = 0.0;
    for (int i = 0; i < PCSS_LOCAL_SAMPLE_COUNT; ++i) {
        float2 sampleUv = uv +
            pcssDiskSample(float(i), invCount, initialAngle) * filterRadius;
        float depthLin = pcssLinearizeDepth(shadowDepthRead(tex, sampleUv), nearClip, farClip);
        sum += step(receiverDepth, depthLin);
    }
    return sum * invCount;
}

// Omni PCSS: Vogel-sphere direction perturbation on the depth cube, blocker
// search and filter in normalized linear distance.  `lightDir` is the
// unnormalized light → fragment vector.
float getShadowPCSSOmni(OmniShadowMap tex, float3 lightDir, float searchArea,
                        float nearClip, float farClip, float bias) {
    float receiverDepth = length(lightDir) / farClip - bias;
    float3 lightDirNorm = normalize(lightDir);
    float phi = pcssFractSinRand(vtFragCoord.xy) * 2.0 * PI;
    const float invCount = 1.0 / float(PCSS_LOCAL_SAMPLE_COUNT);

    float blockerSum = 0.0;
    int numBlockers = 0;
    for (int i = 0; i < PCSS_LOCAL_SAMPLE_COUNT; ++i) {
        float3 sampleDir = normalize(lightDirNorm +
            pcssVogelSphere(i, PCSS_LOCAL_SAMPLE_COUNT, phi) * searchArea);
        float depthLin = pcssCubeStoredToLinear(shadowDepthRead(tex, sampleDir), nearClip, farClip);
        if (depthLin < receiverDepth) {
            blockerSum += depthLin;
            numBlockers++;
        }
    }
    if (numBlockers < 1) {
        return 1.0;
    }
    float avgBlockerDepth = blockerSum / float(numBlockers);

    // filterRadius = (receiver - blocker) / blocker * searchArea
    float filterRadius =
        ((receiverDepth - avgBlockerDepth) / max(avgBlockerDepth, 1e-4)) * searchArea;

    float sum = 0.0;
    for (int i = 0; i < PCSS_LOCAL_SAMPLE_COUNT; ++i) {
        float3 sampleDir = normalize(lightDirNorm +
            pcssVogelSphere(i, PCSS_LOCAL_SAMPLE_COUNT, phi) * filterRadius);
        float depthLin = pcssCubeStoredToLinear(shadowDepthRead(tex, sampleDir), nearClip, farClip);
        sum += step(receiverDepth, depthLin);
    }
    return sum * invCount;
}

