// ── BRDF terms, the GLSL twin of common-brdf.metal ──
//
// These used to live in three places and disagree with Metal in two of them: the
// distribution here, a separable UE4 Smith term and a plain F90 = 1 Schlick in
// common-atmosphere.glsl, and a gloss-aware Fresnel used only by the IBL paths.
// A shading edit has to land in this file and its Metal twin together; anything
// that only touches one of the two is a backend divergence by construction.
//
// Convention, matching Metal and upstream: `roughness` is LINEAR roughness
// (1 - gloss). The distribution squares it twice (alpha = roughness^4) and the
// visibility squares that again (roughness^8) — upstream's own non-standard extra
// squaring, kept deliberately so the two backends agree.

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
// this replaces (k = (roughness + 1)^2 / 8, then an explicit /(4 NdotV NdotL))
// is a different BRDF, and it was the largest single reason this backend did not
// match Metal on direct specular.
float getVisibilitySmithGGX(float NdotV, float NdotL, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;          // the distribution's alpha
    float a4 = a2 * a2;        // and the lambda term's, squared once more
    float lambdaV = NdotL * sqrt(NdotV * NdotV * (1.0 - a4) + a4);
    float lambdaL = NdotV * sqrt(NdotL * NdotL * (1.0 - a4) + a4);
    return 0.5 / max(lambdaV + lambdaL, 1e-5);
}

// Gloss-aware Schlick Fresnel (twin of common-brdf.metal getFresnel). F90 is
// scaled by gloss squared rather than being a flat 1, so a rough dielectric stops
// short of full white at grazing angles instead of rimming.
vec3 getFresnel(float cosTheta, float gloss, vec3 specularity) {
    float f = pow(1.0 - clamp(cosTheta, 0.0, 1.0), 5.0);
    float glossSq = gloss * gloss;
    float specIntensity = max(specularity.r, max(specularity.g, specularity.b));
    return specularity +
        (max(vec3(glossSq * specIntensity), specularity) - specularity) * f;
}

// The name the IBL, probe and SSR call sites in forward-fragment-ambient use.
vec3 ssrFresnel(float cosTheta, float gloss, vec3 specularity) {
    return getFresnel(cosTheta, gloss, specularity);
}

// Fixed F0 = 0.04 (IOR ~ 1.5, typical for clear coatings like polyurethane/lacquer).
float getFresnelCC(float cosTheta) {
    return 0.04 + 0.96 * pow(1.0 - clamp(cosTheta, 0.0, 1.0), 5.0);
}

// Kelemen visibility for clearcoat — simpler than Smith-GGX because a coat is
// typically smooth. V = 0.25 / LdotH^2. Used by Filament and upstream.
float getVisibilityKelemen(float LdotH) {
    return 0.25 / max(LdotH * LdotH, 1e-5);
}
