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

// ── Anisotropic GGX (upstream lightSpecularAnisoGGX + reflDirAniso) ──
// The twin of the same block in common-brdf.glsl; change the two together.
//
// `anisotropy` is the INTENSITY in [0, 1]; the engine's signed value only picks the
// direction (tangent or bitangent), which the surface chunk resolves into T and B.
//
// (at, ab) exactly as upstream: alpha = ((1 - gloss)^2)^2, stretched toward 1 along
// the tangent by intensity SQUARED. This alpha is one squaring beyond the isotropic
// distribution's (whose GGX alpha is (1 - gloss)^2), so the highlight narrows as
// soon as a material turns anisotropy on — upstream's own formulation, kept so both
// backends and upstream agree. What stood here before scaled roughness^2 by
// (1 +/- anisotropy), which is not upstream's shape.
static inline float2 getAnisotropicAlpha(float gloss, float anisotropy)
{
    const float r = max((1.0 - gloss) * (1.0 - gloss), 0.001);
    const float a = r * r;
    return float2(mix(a, 1.0, anisotropy * anisotropy), clamp(a, 0.001, 1.0));
}

// D * Vis for one light (Burley 2012 anisotropic GGX + height-correlated Smith),
// with the 1/(4 NdotL NdotV) folded in. DEVIATION: NdotV and NdotL are clamped at
// zero as the isotropic path does; upstream leaves them signed.
static inline float getLightSpecularAnisoGGX(float3 N, float3 V, float3 H, float3 L,
                                             float3 T, float3 B, float2 alpha)
{
    const float at = alpha.x;
    const float ab = alpha.y;
    const float a2 = at * ab;
    const float3 v = float3(ab * dot(T, H), at * dot(B, H), a2 * dot(N, H));
    const float w2 = a2 / dot(v, v);
    const float D = a2 * w2 * w2 / PI;

    const float NoV = max(dot(N, V), 0.0);
    const float NoL = max(dot(N, L), 0.0);
    const float lambdaV = NoL * length(float3(at * dot(T, V), ab * dot(B, V), NoV));
    const float lambdaL = NoV * length(float3(at * dot(T, L), ab * dot(B, L), NoL));
    return D * 0.5 / max(lambdaV + lambdaL, 1e-5);
}

// Reflection direction for the image-based terms: the normal is bent toward the
// plane spanned by the anisotropy direction B and the view. DEVIATION: falls back
// to the unbent normal when B is parallel to V, where upstream normalizes zero.
static inline float3 getReflDirAniso(float3 N, float3 V, float3 B, float gloss, float anisotropy)
{
    const float roughness = sqrt(1.0 - min(gloss, 1.0));
    const float3 anisoNormal = cross(cross(B, V), B);
    const float bend = 1.0 - anisotropy * (1.0 - roughness);
    const float bend4 = bend * bend * bend * bend;
    const float3 bentNormal = dot(anisoNormal, anisoNormal) > 1e-12
        ? normalize(mix(normalize(anisoNormal), normalize(N), bend4))
        : N;
    return reflect(-V, bentNormal);
}
