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

// ── Anisotropic GGX (upstream lightSpecularAnisoGGX + reflDirAniso) ──
//
// `anisotropy` is the INTENSITY in [0, 1]; the engine's signed value only picks the
// direction (tangent or bitangent), which the surface chunk resolves into T and B.
//
// (at, ab) exactly as upstream: alpha = ((1 - gloss)^2)^2, stretched toward 1 along
// the tangent by intensity SQUARED. Note that this alpha is one squaring beyond the
// isotropic distribution's (whose GGX alpha is (1 - gloss)^2), so the highlight
// narrows as soon as a material turns anisotropy on. That is upstream's own
// formulation, kept so both backends and upstream agree; the lambda terms line up
// with the isotropic visibility's extra squaring.
vec2 getAnisotropicAlpha(float gloss, float anisotropy) {
    float r = max((1.0 - gloss) * (1.0 - gloss), 0.001);
    float a = r * r;
    return vec2(mix(a, 1.0, anisotropy * anisotropy), clamp(a, 0.001, 1.0));
}

// D * Vis for one light (Burley 2012 anisotropic GGX + height-correlated Smith),
// with the 1/(4 NdotL NdotV) folded in like getVisibilitySmithGGX. The caller
// writes `DVis * F`. DEVIATION: NdotV and NdotL are clamped at zero as the
// isotropic path does; upstream leaves them signed and can go negative at a
// normal-mapped silhouette.
float getLightSpecularAnisoGGX(vec3 N, vec3 V, vec3 H, vec3 L, vec3 T, vec3 B,
                               vec2 alpha) {
    float at = alpha.x;
    float ab = alpha.y;
    float a2 = at * ab;
    vec3 v = vec3(ab * dot(T, H), at * dot(B, H), a2 * dot(N, H));
    float w2 = a2 / dot(v, v);
    float D = a2 * w2 * w2 / PI;

    float NoV = max(dot(N, V), 0.0);
    float NoL = max(dot(N, L), 0.0);
    float lambdaV = NoL * length(vec3(at * dot(T, V), ab * dot(B, V), NoV));
    float lambdaL = NoV * length(vec3(at * dot(T, L), ab * dot(B, L), NoL));
    return D * 0.5 / max(lambdaV + lambdaL, 1e-5);
}

// Reflection direction for the image-based terms: the normal is bent toward the
// plane spanned by the anisotropy direction B and the view, by an amount that
// grows with intensity and roughness. DEVIATION: falls back to the unbent normal
// when B is parallel to V, where upstream normalizes a zero vector.
vec3 getReflDirAniso(vec3 N, vec3 V, vec3 B, float gloss, float anisotropy) {
    float roughness = sqrt(1.0 - min(gloss, 1.0));
    vec3 anisoNormal = cross(cross(B, V), B);
    float bend = 1.0 - anisotropy * (1.0 - roughness);
    float bend4 = bend * bend * bend * bend;
    vec3 bentNormal = dot(anisoNormal, anisoNormal) > 1e-12
        ? normalize(mix(normalize(anisoNormal), normalize(N), bend4))
        : N;
    return reflect(-V, bentNormal);
}
