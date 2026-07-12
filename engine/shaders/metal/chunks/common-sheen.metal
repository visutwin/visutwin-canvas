// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
// ---------------------------------------------------------------------------
// Sheen BRDF (Charlie distribution + Ashikhmin visibility)
// Based on Estevez & Kulla 2017, "Production Friendly Microfacet Sheen BRDF".
// ---------------------------------------------------------------------------

// Charlie sheen distribution — inverted Gaussian model for fabric BRDF.
// Produces softer, wider lobes than GGX, suitable for velvet/fabric.
static inline float sheenDistribution(float NoH, float roughness)
{
    const float invR = 1.0 / max(roughness, 0.001);
    const float cos2h = NoH * NoH;
    const float sin2h = max(1.0 - cos2h, 0.0078125);  // prevent pow(0, x)
    return (2.0 + invR) * pow(sin2h, invR * 0.5) / (2.0 * PI);
}

// Ashikhmin visibility for sheen — energy-conserving geometric term for fabric.
static inline float sheenVisibility(float NoV, float NoL)
{
    return 1.0 / (4.0 * (NoL + NoV - NoL * NoV));
}

// Analytical sheen IBL approximation.
// Piecewise exp(a * NoV + b) fit that avoids a precomputed LUT texture.
// Returns the directional albedo E(NoV, roughness) for sheen.
static inline float sheenIBLApprox(float NoV, float roughness)
{
    const float r2 = roughness * roughness;
    float a, b;
    if (NoV < 0.5) {
        a = -0.0275 - 0.2159 * roughness + 2.3519 * r2 - 0.3414 * r2 * roughness;
        b =  0.1003 + 1.3324 * roughness - 1.4688 * r2 - 0.1819 * r2 * roughness;
    } else {
        a = -0.5765 - 0.1299 * roughness + 0.5271 * r2 + 0.7462 * r2 * roughness;
        b =  0.1835 - 0.2939 * roughness + 0.2079 * r2 - 0.3522 * r2 * roughness;
    }
    return saturate(exp(a * NoV + b));
}
