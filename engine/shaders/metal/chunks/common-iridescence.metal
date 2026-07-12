// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
// ---------------------------------------------------------------------------
// Iridescence — thin-film interference (KHR_materials_iridescence)
// Based on Laurent Belcour & Pascal Barla 2017.
// Computes wavelength-dependent Fresnel at two interfaces (air→film, film→substrate)
// with optical path difference producing rainbow-shift colors.
// ---------------------------------------------------------------------------

// IOR to Fresnel F0 conversion (Schlick approximation inverse).
static inline float iorToF0(float transmittedIor, float incidentIor)
{
    const float r = (transmittedIor - incidentIor) / (transmittedIor + incidentIor);
    return r * r;
}

static inline float3 iorToF0Vec(float3 transmittedIor, float incidentIor)
{
    const float3 r = (transmittedIor - incidentIor) / (transmittedIor + incidentIor);
    return r * r;
}

// Fresnel F0 to IOR inversion.
static inline float3 f0ToIor(float3 f0)
{
    const float3 s = sqrt(f0);
    return (1.0 + s) / (1.0 - s);
}

// Schlick Fresnel for iridescence interfaces.
static inline float iridescenceFresnel(float cosTheta, float f0)
{
    const float x = saturate(1.0 - cosTheta);
    const float x2 = x * x;
    return f0 + (1.0 - f0) * (x2 * x2 * x);
}

static inline float3 iridescenceFresnelVec(float cosTheta, float3 f0)
{
    const float x = saturate(1.0 - cosTheta);
    const float x2 = x * x;
    return f0 + (1.0 - f0) * (x2 * x2 * x);
}

// Spectral sensitivity function: converts optical path difference (OPD) to visible color.
// Uses Gaussian basis functions in CIE XYZ color space, then converts to sRGB (Rec.709).
static inline float3 iridescenceSensitivity(float opd, float3 shift)
{
    const float phase = 2.0 * PI * opd * 1.0e-9;

    // CIE XYZ Gaussian basis amplitudes, centers, and variances.
    const float3 val = float3(5.4856e-13, 4.4201e-13, 5.2481e-13);
    const float3 pos = float3(1.6810e+06, 1.7953e+06, 2.2084e+06);
    const float3 var = float3(4.3278e+09, 9.3046e+09, 6.6121e+09);

    float3 xyz = val * sqrt(2.0 * PI * var) * cos(pos * phase + shift)
                 * exp(-(phase * phase) * var);

    // Fourth Gaussian for the X tristimulus (red sensitivity has a bimodal peak).
    xyz.x += 9.7470e-14 * sqrt(2.0 * PI * 4.5282e+09)
           * cos(2.2399e+06 * phase + shift.x)
           * exp(-4.5282e+09 * phase * phase);

    // Normalize to unit peak.
    xyz /= float3(1.0685e-07);

    // CIE XYZ → sRGB (Rec.709) matrix.
    const float3x3 XYZ_TO_REC709 = float3x3(
        float3( 3.2404542, -1.5371385, -0.4985314),
        float3(-0.9692660,  1.8760108,  0.0415560),
        float3( 0.0556434, -0.2040259,  1.0572252)
    );

    return XYZ_TO_REC709 * xyz;
}

// Compute thin-film iridescence Fresnel replacement.
// outsideIor: IOR of surrounding medium (1.0 for air).
// cosTheta:   dot(V, N) at the surface.
// baseF0:     base material F0 specularity (substrate).
// thickness:  thin-film thickness in nanometers.
// filmIor:    IOR of the thin film.
static inline float3 calcIridescence(float outsideIor, float cosTheta, float3 baseF0,
                                      float thickness, float filmIor)
{
    // Smooth transition: for very thin films, revert to base outside IOR.
    const float iridIor = mix(outsideIor, filmIor, smoothstep(0.0, 0.03, thickness));

    // Snell's law: sin²θ₂ = (n₁/n₂)² * sin²θ₁
    const float sinTheta2Sq = (outsideIor / iridIor) * (outsideIor / iridIor)
                              * (1.0 - cosTheta * cosTheta);
    const float cosTheta2Sq = 1.0 - sinTheta2Sq;

    // Total internal reflection — return full reflection.
    if (cosTheta2Sq < 0.0) {
        return float3(1.0);
    }

    const float cosTheta2 = sqrt(cosTheta2Sq);

    // Interface 1: outside → thin film.
    const float r0 = iorToF0(iridIor, outsideIor);
    const float r12 = iridescenceFresnel(cosTheta, r0);
    const float t121 = 1.0 - r12;  // transmission through interface 1 (both ways)

    // Phase shift at interface 1 (π if film IOR < outside IOR).
    const float phi12 = (iridIor < outsideIor) ? PI : 0.0;
    const float phi21 = PI - phi12;

    // Interface 2: thin film → substrate.
    // Convert base F0 to IOR per channel for the film→substrate interface.
    const float3 baseIor = f0ToIor(baseF0 + float3(0.0001));
    const float3 r1 = iorToF0Vec(baseIor, iridIor);
    const float3 r23 = iridescenceFresnelVec(cosTheta2, r1);

    // Phase shift at interface 2 (π per channel if substrate IOR < film IOR).
    float3 phi23 = float3(0.0);
    if (baseIor.x < iridIor) phi23.x = PI;
    if (baseIor.y < iridIor) phi23.y = PI;
    if (baseIor.z < iridIor) phi23.z = PI;

    // Optical path difference (in nanometers).
    const float opd = 2.0 * iridIor * thickness * cosTheta2;

    // Total phase shift.
    const float3 phi = float3(phi21) + phi23;

    // Multi-bounce Airy summation (2 orders).
    const float3 r123Sq = clamp(float3(r12) * r23, float3(1e-5), float3(0.9999));
    const float3 r123 = sqrt(r123Sq);
    const float3 rs = (t121 * t121) * r23 / (1.0 - r123Sq);

    // Order 0: direct reflection + transmitted-reflected.
    float3 i = float3(r12) + rs;

    // Orders 1-2: interference fringes via spectral sensitivity.
    float3 cm = rs - t121;
    for (int m = 1; m <= 2; m++) {
        cm *= r123;
        const float3 sm = 2.0 * iridescenceSensitivity(float(m) * opd, float(m) * phi);
        i += cm * sm;
    }

    return max(i, float3(0.0));
}

// Convenience wrapper: compute iridescence Fresnel for air (IOR=1.0) → thin film → substrate.
static inline float3 getIridescence(float cosTheta, float3 specularity,
                                     float thickness, float filmIor)
{
    return calcIridescence(1.0, cosTheta, specularity, thickness, filmIor);
}
