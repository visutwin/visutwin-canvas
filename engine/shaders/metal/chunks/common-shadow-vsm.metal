// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
// ---------------------------------------------------------------------------
// EVSM (Exponential Variance Shadow Maps), 16-bit float storage.
// Match upstream SHADOW_VSM_16F (shadowEVSM.js + blurVSM.js).
//
// Storage convention (set by shadow-fragment.metal):
//   moments.x = exp(c · z),   moments.y = exp(c · z)²,   moments.z = 1.0 (rendered flag),
//   moments.w = 1.0 (unused)
// Cleared pixels are (0,0,0,0); the (1 - moments.z) fallback path in calculateEVSM
// then synthesizes "fully lit" for any sample that landed outside the rendered region.
//
// `c` is VSM_EXPONENT. The actual 16F storage constraint is on the SECOND
// moment (exp(c·z)²), not the first — so we need exp(2c) < 65504, giving
// c < ln(65504)/2 ≈ 5.54. Going higher (e.g. 8 or 11) overflows the second
// moment to fp16 ∞, which explodes the variance reconstruction and
// produces severe light bleeding (the opposite of the intended effect).
// 5.54 is also upstream default for SHADOW_VSM_16F.
// ---------------------------------------------------------------------------
constant float VSM_EXPONENT = 5.54;

static inline float linstepSat(float a, float b, float v)
{
    return saturate((v - a) / (b - a));
}

// Trim a [0, amount] tail off the upper-bound probability and rescale (amount, 1] → [0, 1].
// Combats VSM's "light bleeding" by clipping low-confidence shadow samples to 0.
static inline float reduceLightBleeding(float pMax, float amount)
{
    return linstepSat(amount, 1.0, pMax);
}

// One-tailed Chebyshev upper bound on P(receiver is lit) given filtered (M1, M2)
// moments and the receiver's depth. Returns 1 when receiver is in front of the
// occluder mean (i.e. unambiguously lit), else the variance-based upper bound.
static inline float chebyshevUpperBound(float2 moments, float mean, float minVariance,
                                        float lightBleedingReduction)
{
    float variance = moments.y - moments.x * moments.x;
    variance = max(variance, minVariance);

    const float d = mean - moments.x;
    float pMax = variance / (variance + d * d);

    pMax = reduceLightBleeding(pMax, lightBleedingReduction);

    return (mean <= moments.x) ? 1.0 : pMax;
}

// Apply EVSM warp to receiver depth, combine with stored moments (or fall back to
// "lit" for cleared pixels), then run Chebyshev. `Z` is the receiver's NDC depth ∈ [0,1].
static inline float calculateEVSM(float3 moments, float Z, float vsmBias, float exponent)
{
    Z = 2.0 * Z - 1.0;                          // [0,1] → [-1,1]
    const float warpedDepth = exp(exponent * Z);

    // Cleared (unrendered) pixels have moments.z == 0 → synthesize fully-lit moments
    // by injecting (warpedDepth, warpedDepth²); rendered pixels have moments.z == 1
    // → use stored moments unchanged.
    const float2 stored = moments.xy + float2(warpedDepth, warpedDepth * warpedDepth) * (1.0 - moments.z);

    const float depthScale = vsmBias * exponent * warpedDepth;
    const float minVariance = depthScale * depthScale;
    // 0.1 = upstream default light-bleeding reduction. With c at the proper
    // 16F-safe value, the Chebyshev probability is well-conditioned and 0.1
    // is enough to clip residual bleeding without darkening contact shadows.
    return chebyshevUpperBound(stored, warpedDepth, minVariance, 0.1);
}

// Public entry: sample the moments texture and return shadow visibility ∈ [0, 1].
static inline float getShadowVSM16(texture2d<float> momentsTex, float2 shadowUv, float receiverDepth, float vsmBias)
{
    constexpr sampler vsmSampler(coord::normalized, filter::linear, address::clamp_to_edge);
    const float3 moments = momentsTex.sample(vsmSampler, shadowUv).xyz;
    return calculateEVSM(moments, receiverDepth, vsmBias, VSM_EXPONENT);
}
