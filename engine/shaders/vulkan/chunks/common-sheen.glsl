// ── Sheen BRDF, the GLSL twin of common-sheen.metal ──
// Charlie distribution + Ashikhmin visibility, after Estevez & Kulla 2017,
// "Production Friendly Microfacet Sheen BRDF". Fabric and velvet: a much wider,
// softer lobe than GGX, brightest at grazing angles.
//
// This backend used to approximate the whole thing as
// `pow(1 - NdotH, mix(2, 8, sheenRoughness))`, a velvet term with no
// distribution, no visibility, no image-based lighting and no energy taken from
// the base layer — which is to say it was not sheen, it just looked soft.

// Charlie sheen distribution — an inverted Gaussian rather than GGX.
float sheenDistribution(float NoH, float roughness) {
    float invR = 1.0 / max(roughness, 0.001);
    float cos2h = NoH * NoH;
    float sin2h = max(1.0 - cos2h, 0.0078125);   // keeps pow(0, x) off the table
    return (2.0 + invR) * pow(sin2h, invR * 0.5) / (2.0 * PI);
}

// Ashikhmin visibility — the energy-conserving geometric term for fabric.
float sheenVisibility(float NoV, float NoL) {
    return 1.0 / (4.0 * (NoL + NoV - NoL * NoV));
}

// Analytical sheen directional albedo E(NoV, roughness): a piecewise exp fit that
// stands in for the precomputed DFG lookup upstream samples, so no LUT texture is
// needed. Same fit as the Metal chunk.
float sheenIBLApprox(float NoV, float roughness) {
    float r2 = roughness * roughness;
    float a, b;
    if (NoV < 0.5) {
        a = -0.0275 - 0.2159 * roughness + 2.3519 * r2 - 0.3414 * r2 * roughness;
        b =  0.1003 + 1.3324 * roughness - 1.4688 * r2 - 0.1819 * r2 * roughness;
    } else {
        a = -0.5765 - 0.1299 * roughness + 0.5271 * r2 + 0.7462 * r2 * roughness;
        b =  0.1835 - 0.2939 * roughness + 0.2079 * r2 - 0.3522 * r2 * roughness;
    }
    return clamp(exp(a * NoV + b), 0.0, 1.0);
}
