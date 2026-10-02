// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
// ---------------------------------------------------------------------------
// EVSM (Exponential Variance Shadow Maps), 16-bit float storage.
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
    // 0.1 = default light-bleeding reduction. With c at the proper
    // 16F-safe value, the Chebyshev probability is well-conditioned and 0.1
    // is enough to clip residual bleeding without darkening contact shadows.
    return chebyshevUpperBound(stored, warpedDepth, minVariance, 0.1);
}

// Public entry: sample the moments texture and return shadow visibility ∈ [0, 1].
static inline float getShadowVSM16(texture2d<float> momentsTex, float2 shadowUv, float receiverDepth, float vsmBias)
{
    constexpr sampler vsmSampler(coord::normalized, filter::linear, address::clamp_to_edge);
    const float3 moments = momentsTex.sample(vsmSampler, shadowUv, level(0)).xyz;
    return calculateEVSM(moments, receiverDepth, vsmBias, VSM_EXPONENT);
}

// A VSM spot light's shadow pass stores distance / range
// (min(distance(view_position, vPositionW) / light_radius, 0.99999)): perspective depth with a near plane of 0.01 is crushed
// against 1, where the exponential warp leaves no precision at all. The pass's own
// view-projection carries both inputs, so no light uniform is needed: the light is
// the projection centre, where clip x, y and w vanish (rows 0, 1 and 3), and the far
// plane follows from row 2 (clip z = A z_view + B, far = B / (A + 1) for the GL-style
// projection this engine builds). Returns false for an orthographic projection
// (a directional light), whose depth is stored as is.
static inline bool shadowDistanceRatio(float4x4 vp, float3 worldPos, thread float& ratio)
{
    const float4 r0 = float4(vp[0][0], vp[1][0], vp[2][0], vp[3][0]);
    const float4 r1 = float4(vp[0][1], vp[1][1], vp[2][1], vp[3][1]);
    const float4 r2 = float4(vp[0][2], vp[1][2], vp[2][2], vp[3][2]);
    const float4 r3 = float4(vp[0][3], vp[1][3], vp[2][3], vp[3][3]);
    if (dot(r3.xyz, r3.xyz) < 1e-12) {
        return false;
    }
    const float3 c01 = cross(r0.xyz, r1.xyz);
    const float3 c13 = cross(r1.xyz, r3.xyz);
    const float3 c30 = cross(r3.xyz, r0.xyz);
    const float det = dot(r0.xyz, c13);
    const float3 lightPos = -(r0.w * c13 + r1.w * c30 + r3.w * c01) / det;
    const float3 forward = normalize(r3.xyz);
    const float a = -dot(r2.xyz, forward);
    const float b = dot(r2.xyz, lightPos) + r2.w;
    const float farClip = b / (a + 1.0);
    ratio = min(distance(worldPos, lightPos) / farClip, 0.99999);
    return true;
}
