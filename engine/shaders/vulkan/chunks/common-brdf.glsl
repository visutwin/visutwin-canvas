// Gloss-aware Schlick Fresnel (parity with common-brdf.metal getFresnel). The
// glossSq term scales F90 by roughness so rough surfaces don't show excessive
// grazing-angle reflectance.
vec3 ssrFresnel(float cosTheta, float gloss, vec3 specularity) {
    float f = pow(1.0 - clamp(cosTheta, 0.0, 1.0), 5.0);
    float glossSq = gloss * gloss;
    float specIntensity = max(specularity.r, max(specularity.g, specularity.b));
    return specularity +
        (max(vec3(glossSq * specIntensity), specularity) - specularity) * f;
}
