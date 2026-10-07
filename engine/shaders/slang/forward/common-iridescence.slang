// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 07.10.2026
//
// ── Thin-film iridescence, the GLSL twin of common-iridescence.metal ──
// KHR_materials_iridescence, after Belcour & Barla 2017. Wavelength-dependent
// Fresnel at two interfaces (air to film, film to substrate) with the optical
// path difference between them producing the hue shift with viewing angle.
//
// Do not approximate it as a fixed `0.5 + 0.5 * cos(phase + rgb offsets)` tint
// blended into F0: that is a rainbow that never moves with the view direction,
// which is the one thing iridescence is.

float iorToF0(float transmittedIor, float incidentIor) {
    float r = (transmittedIor - incidentIor) / (transmittedIor + incidentIor);
    return r * r;
}

float3 iorToF0Vec(float3 transmittedIor, float incidentIor) {
    float3 r = (transmittedIor - incidentIor) / (transmittedIor + incidentIor);
    return r * r;
}

float3 f0ToIor(float3 f0) {
    float3 s = sqrt(f0);
    return (1.0 + s) / (1.0 - s);
}

float iridescenceFresnel(float cosTheta, float f0) {
    float x = clamp(1.0 - cosTheta, 0.0, 1.0);
    float x2 = x * x;
    return f0 + (1.0 - f0) * (x2 * x2 * x);
}

float3 iridescenceFresnelVec(float cosTheta, float3 f0) {
    float x = clamp(1.0 - cosTheta, 0.0, 1.0);
    float x2 = x * x;
    return f0 + (1.0 - f0) * (x2 * x2 * x);
}

// Optical path difference to visible colour, via Gaussian basis functions in CIE
// XYZ and then a conversion to Rec.709.
float3 iridescenceSensitivity(float opd, float3 shift) {
    float phase = 2.0 * PI * opd * 1.0e-9;

    float3 val = float3(5.4856e-13, 4.4201e-13, 5.2481e-13);
    float3 pos = float3(1.6810e+06, 1.7953e+06, 2.2084e+06);
    float3 var = float3(4.3278e+09, 9.3046e+09, 6.6121e+09);

    float3 xyz = val * sqrt(2.0 * PI * var) * cos(pos * phase + shift)
             * exp(-(phase * phase) * var);

    // A fourth Gaussian for X: the red sensitivity curve is bimodal.
    xyz.x += 9.7470e-14 * sqrt(2.0 * PI * 4.5282e+09)
           * cos(2.2399e+06 * phase + shift.x)
           * exp(-4.5282e+09 * phase * phase);

    xyz /= float3(1.0685e-07);

    // CIE XYZ -> Rec.709. Written COLUMN-major, which is what both GLSL and MSL
    // constructors take, so the rows below read transposed on the page.
    const float3x3 XYZ_TO_REC709 = float3x3(
         3.2404542, -0.9692660,  0.0556434,
        -1.5371385,  1.8760108, -0.2040259,
        -0.4985314,  0.0415560,  1.0572252
    );

    return mul(xyz, XYZ_TO_REC709);
}

// Thin-film Fresnel replacing the substrate's own.
//   outsideIor: surrounding medium (1.0 for air)
//   cosTheta:   dot(V, N) at the surface
//   baseF0:     substrate specularity
//   thickness:  film thickness in nanometres
//   filmIor:    film index of refraction
float3 calcIridescence(float outsideIor, float cosTheta, float3 baseF0,
                     float thickness, float filmIor) {
    // Very thin films fade back to the surrounding medium rather than popping.
    float iridIor = lerp(outsideIor, filmIor, smoothstep(0.0, 0.03, thickness));

    // Snell: sin^2(t2) = (n1/n2)^2 * sin^2(t1)
    float sinTheta2Sq = (outsideIor / iridIor) * (outsideIor / iridIor)
                        * (1.0 - cosTheta * cosTheta);
    float cosTheta2Sq = 1.0 - sinTheta2Sq;
    if (cosTheta2Sq < 0.0) {
        return float3(1.0);   // total internal reflection
    }
    float cosTheta2 = sqrt(cosTheta2Sq);

    // Interface 1: outside -> film.
    float r0 = iorToF0(iridIor, outsideIor);
    float r12 = iridescenceFresnel(cosTheta, r0);
    float t121 = 1.0 - r12;
    float phi12 = (iridIor < outsideIor) ? PI : 0.0;
    float phi21 = PI - phi12;

    // Interface 2: film -> substrate, per channel.
    float3 baseIor = f0ToIor(baseF0 + float3(0.0001));
    float3 r1 = iorToF0Vec(baseIor, iridIor);
    float3 r23 = iridescenceFresnelVec(cosTheta2, r1);

    float3 phi23 = float3(0.0);
    if (baseIor.x < iridIor) phi23.x = PI;
    if (baseIor.y < iridIor) phi23.y = PI;
    if (baseIor.z < iridIor) phi23.z = PI;

    float opd = 2.0 * iridIor * thickness * cosTheta2;
    float3 phi = float3(phi21) + phi23;

    // Airy summation, two orders.
    float3 r123Sq = clamp(float3(r12) * r23, float3(1e-5), float3(0.9999));
    float3 r123 = sqrt(r123Sq);
    float3 rs = (t121 * t121) * r23 / (1.0 - r123Sq);

    float3 i = float3(r12) + rs;
    float3 cm = rs - t121;
    for (int m = 1; m <= 2; ++m) {
        cm *= r123;
        float3 sm = 2.0 * iridescenceSensitivity(float(m) * opd, float(m) * phi);
        i += cm * sm;
    }
    return max(i, float3(0.0));
}

// Air (IOR 1.0) -> film -> substrate.
float3 getIridescence(float cosTheta, float3 specularity, float thickness, float filmIor) {
    return calcIridescence(1.0, cosTheta, specularity, thickness, filmIor);
}
