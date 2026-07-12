// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
static inline float3 getFresnel(float cosTheta, float gloss, float3 specularity)
{
    const float fresnel = pow(1.0 - saturate(cosTheta), 5.0);
    const float glossSq = gloss * gloss;
    const float specIntensity = max(specularity.r, max(specularity.g, specularity.b));
    return specularity + (max(float3(glossSq * specIntensity), specularity) - specularity) * fresnel;
}

// Fixed F0 = 0.04 (IOR ≈ 1.5, typical for clear coatings like polyurethane/lacquer).
static inline float getFresnelCC(float cosTheta)
{
    return 0.04 + 0.96 * pow(1.0 - saturate(cosTheta), 5.0);
}

// Kelemen visibility term for clearcoat — simpler than Smith-GGX since clearcoat
// is typically smooth. V = 0.25 / (LdotH^2). Used by Filament and upstream.
static inline float getVisibilityKelemen(float LdotH)
{
    return 0.25 / max(LdotH * LdotH, 1e-5);
}
