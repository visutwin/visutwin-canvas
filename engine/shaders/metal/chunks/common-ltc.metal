// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
// ---------------------------------------------------------------------------
// LTC area lights — Real-Time Polygonal-Light Shading with Linearly
// Transformed Cosines (Heitz, Dupuy, Hill, Neubelt).
// Port of upstream ltc.js, rect path only (disk/sphere shapes not ported).
// ---------------------------------------------------------------------------

constexpr sampler ltcLutSampler(filter::linear, mip_filter::none, address::clamp_to_edge);

static inline float2 ltcUv(float3 N, float3 V, float gloss)
{
    const float LUT_SIZE = 64.0;
    const float roughness = max((1.0 - gloss) * (1.0 - gloss), 0.001);
    const float dotNV = saturate(dot(N, V));
    const float2 uv = float2(roughness, sqrt(1.0 - dotNV));
    return uv * ((LUT_SIZE - 1.0) / LUT_SIZE) + (0.5 / LUT_SIZE);
}

// An approximation of the form factor of a horizon-clipped rectangle
// ("Real-Time Area Lighting: a Journey from Research to Production", p.102).
static inline float ltcClippedSphereFormFactor(float3 f)
{
    const float l = length(f);
    return max((l * l + f.z) / (l + 1.0), 0.0);
}

static inline float3 ltcEdgeVectorFormFactor(float3 v1, float3 v2)
{
    const float x = dot(v1, v2);
    const float y = abs(x);
    // rational polynomial approximation to theta / sin(theta) / 2PI
    const float a = 0.8543985 + (0.4965155 + 0.0145206 * y) * y;
    const float b = 3.4175940 + (4.1616724 + y) * y;
    const float v = a / b;
    const float thetaSintheta = (x > 0.0) ? v : 0.5 * rsqrt(max(1.0 - x * x, 1e-7)) - v;
    return cross(v1, v2) * thetaSintheta;
}

// Evaluates the LTC integral of a rect light (corners p0..p3, ccw) over the
// cosine distribution transformed by mInv, as seen from surface point P.
static inline float ltcEvaluateRect(float3 N, float3 V, float3 P, float3x3 mInv,
    float3 p0, float3 p1, float3 p2, float3 p3)
{
    const float3 v1 = p1 - p0;
    const float3 v2 = p3 - p0;
    const float3 lightNormal = cross(v1, v2);
    const float factor = sign(-dot(lightNormal, P - p0));

    // construct orthonormal basis around N
    const float3 T1 = normalize(V - N * dot(V, N));
    const float3 T2 = factor * cross(N, T1); // negated from paper due to handedness

    // rows of transpose(mat3(T1, T2, N)) then transformed by mInv
    const float3x3 mat = mInv * transpose(float3x3(T1, T2, N));

    // transform rect corners and project onto sphere
    const float3 c0 = normalize(mat * (p0 - P));
    const float3 c1 = normalize(mat * (p1 - P));
    const float3 c2 = normalize(mat * (p2 - P));
    const float3 c3 = normalize(mat * (p3 - P));

    float3 vectorFormFactor = float3(0.0);
    vectorFormFactor += ltcEdgeVectorFormFactor(c0, c1);
    vectorFormFactor += ltcEdgeVectorFormFactor(c1, c2);
    vectorFormFactor += ltcEdgeVectorFormFactor(c2, c3);
    vectorFormFactor += ltcEdgeVectorFormFactor(c3, c0);

    return ltcClippedSphereFormFactor(vectorFormFactor);
}
