// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 07.10.2026
//
// ── BRDF terms ──
//
// Every path takes these terms from here, never from a local copy; a second
// spelling of a term is a divergence by construction.
//
// Convention: `roughness` is LINEAR roughness
// (1 - gloss). The distribution squares it twice (alpha = roughness^4) and the
// visibility squares that again (roughness^8) — a non-standard extra squaring
// reproduced from upstream, kept deliberately.

// GGX normal distribution.
float distributionGGX(float NdotH, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float d = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / max(PI * d * d, 1e-7);
}

// Height-correlated Smith visibility. This is a VISIBILITY term, not a separable
// geometry term: the 1/(4 NdotL NdotV) denominator is folded in, so the caller
// writes `D * Vis * F` with no further division. The separable Schlick-GGX form
// (k = (roughness + 1)^2 / 8, then an explicit /(4 NdotV NdotL)) is a different
// BRDF.
float getVisibilitySmithGGX(float NdotV, float NdotL, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;          // the distribution's alpha
    float a4 = a2 * a2;        // and the lambda term's, squared once more
    float lambdaV = NdotL * sqrt(NdotV * NdotV * (1.0 - a4) + a4);
    float lambdaL = NdotV * sqrt(NdotL * NdotL * (1.0 - a4) + a4);
    return 0.5 / max(lambdaV + lambdaL, 1e-5);
}

// A light's specular lobe, the D * Vis product the light loops multiply by the
// Fresnel, the radiance and NdotL. Normalised Blinn-Phong of the gloss by default
// (exponent 2^(11 gloss), scaled by (n + 2) / 8), which is upstream's default light
// specular; with `ggx` (VT_FEATURE_GGX_SPECULAR) the GGX distribution and Smith
// visibility above. The image-based terms are unaffected either way.
float directSpecularLobe(float NdotH, float NdotV, float NdotL, float gloss, bool ggx) {
    if (ggx) {
        const float roughness = 1.0 - gloss;
        return distributionGGX(NdotH, roughness) * getVisibilitySmithGGX(NdotV, NdotL, roughness);
    }
    const float specPow = max(exp2(gloss * 11.0), 0.0001);
    return pow(NdotH, specPow) * (specPow + 2.0) / 8.0;
}

// Gloss-aware Schlick Fresnel. F90 is scaled by gloss squared rather than being a
// flat 1, so a rough dielectric stops short of full white at grazing angles instead
// of rimming.
float3 getFresnel(float cosTheta, float gloss, float3 specularity) {
    float f = pow(1.0 - clamp(cosTheta, 0.0, 1.0), 5.0);
    float glossSq = gloss * gloss;
    float specIntensity = max(specularity.r, max(specularity.g, specularity.b));
    return specularity +
        (max(float3(glossSq * specIntensity), specularity) - specularity) * f;
}

// The name the IBL, probe and SSR call sites in forward-fragment-ambient use.
float3 ssrFresnel(float cosTheta, float gloss, float3 specularity) {
    return getFresnel(cosTheta, gloss, specularity);
}

// Fixed F0 = 0.04 (IOR ~ 1.5, typical for clear coatings like polyurethane/lacquer).
float getFresnelCC(float cosTheta) {
    return 0.04 + 0.96 * pow(1.0 - clamp(cosTheta, 0.0, 1.0), 5.0);
}


// ── Anisotropic GGX ──
//
// `anisotropy` is the INTENSITY in [0, 1]; the engine's signed value only picks the
// direction (tangent or bitangent), which the surface chunk resolves into T and B.
//
// (at, ab): alpha = ((1 - gloss)^2)^2, stretched toward 1 along
// the tangent by intensity SQUARED. Note that this alpha is one squaring beyond the
// isotropic distribution's (whose GGX alpha is (1 - gloss)^2), so the highlight
// narrows as soon as a material turns anisotropy on. That formulation is
// upstream's, kept deliberately; the lambda terms line up
// with the isotropic visibility's extra squaring.
float2 getAnisotropicAlpha(float gloss, float anisotropy) {
    float r = max((1.0 - gloss) * (1.0 - gloss), 0.001);
    float a = r * r;
    return float2(lerp(a, 1.0, anisotropy * anisotropy), clamp(a, 0.001, 1.0));
}

// D * Vis for one light (Burley 2012 anisotropic GGX + height-correlated Smith),
// with the 1/(4 NdotL NdotV) folded in like getVisibilitySmithGGX. The caller
// writes `DVis * F`. DEVIATION: NdotV and NdotL are clamped at zero as the
// isotropic path does; upstream leaves them signed and can go negative at a
// normal-mapped silhouette.
float getLightSpecularAnisoGGX(float3 N, float3 V, float3 H, float3 L, float3 T, float3 B,
                               float2 alpha) {
    float at = alpha.x;
    float ab = alpha.y;
    float a2 = at * ab;
    float3 v = float3(ab * dot(T, H), at * dot(B, H), a2 * dot(N, H));
    float w2 = a2 / dot(v, v);
    float D = a2 * w2 * w2 / PI;

    float NoV = max(dot(N, V), 0.0);
    float NoL = max(dot(N, L), 0.0);
    float lambdaV = NoL * length(float3(at * dot(T, V), ab * dot(B, V), NoV));
    float lambdaL = NoV * length(float3(at * dot(T, L), ab * dot(B, L), NoL));
    return D * 0.5 / max(lambdaV + lambdaL, 1e-5);
}

// Reflection direction for the image-based terms: the normal is bent toward the
// plane spanned by the anisotropy direction B and the view, by an amount that
// grows with intensity and roughness. DEVIATION: falls back to the unbent normal
// when B is parallel to V, where upstream normalizes a zero vector.
float3 getReflDirAniso(float3 N, float3 V, float3 B, float gloss, float anisotropy) {
    float roughness = sqrt(1.0 - min(gloss, 1.0));
    float3 anisoNormal = cross(cross(B, V), B);
    float bend = 1.0 - anisotropy * (1.0 - roughness);
    float bend4 = bend * bend * bend * bend;
    float3 bentNormal = dot(anisoNormal, anisoNormal) > 1e-12
        ? normalize(lerp(normalize(anisoNormal), normalize(N), bend4))
        : N;
    return reflect(-V, bentNormal);
}
