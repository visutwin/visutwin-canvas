// ── Thin-film iridescence, the GLSL twin of common-iridescence.metal ──
// KHR_materials_iridescence, after Belcour & Barla 2017. Wavelength-dependent
// Fresnel at two interfaces (air to film, film to substrate) with the optical
// path difference between them producing the hue shift with viewing angle.
//
// This backend used to approximate it as a fixed `0.5 + 0.5 * cos(phase + rgb
// offsets)` tint blended into F0 — a rainbow that never moved with the view
// direction, which is the one thing iridescence is.

float iorToF0(float transmittedIor, float incidentIor) {
    float r = (transmittedIor - incidentIor) / (transmittedIor + incidentIor);
    return r * r;
}

vec3 iorToF0Vec(vec3 transmittedIor, float incidentIor) {
    vec3 r = (transmittedIor - incidentIor) / (transmittedIor + incidentIor);
    return r * r;
}

vec3 f0ToIor(vec3 f0) {
    vec3 s = sqrt(f0);
    return (1.0 + s) / (1.0 - s);
}

float iridescenceFresnel(float cosTheta, float f0) {
    float x = clamp(1.0 - cosTheta, 0.0, 1.0);
    float x2 = x * x;
    return f0 + (1.0 - f0) * (x2 * x2 * x);
}

vec3 iridescenceFresnelVec(float cosTheta, vec3 f0) {
    float x = clamp(1.0 - cosTheta, 0.0, 1.0);
    float x2 = x * x;
    return f0 + (1.0 - f0) * (x2 * x2 * x);
}

// Optical path difference to visible colour, via Gaussian basis functions in CIE
// XYZ and then a conversion to Rec.709.
vec3 iridescenceSensitivity(float opd, vec3 shift) {
    float phase = 2.0 * PI * opd * 1.0e-9;

    vec3 val = vec3(5.4856e-13, 4.4201e-13, 5.2481e-13);
    vec3 pos = vec3(1.6810e+06, 1.7953e+06, 2.2084e+06);
    vec3 var = vec3(4.3278e+09, 9.3046e+09, 6.6121e+09);

    vec3 xyz = val * sqrt(2.0 * PI * var) * cos(pos * phase + shift)
             * exp(-(phase * phase) * var);

    // A fourth Gaussian for X: the red sensitivity curve is bimodal.
    xyz.x += 9.7470e-14 * sqrt(2.0 * PI * 4.5282e+09)
           * cos(2.2399e+06 * phase + shift.x)
           * exp(-4.5282e+09 * phase * phase);

    xyz /= vec3(1.0685e-07);

    // CIE XYZ -> Rec.709. Written COLUMN-major, which is what both GLSL and MSL
    // constructors take, so the rows below read transposed on the page. Upstream
    // (iridescenceDiffraction.js) has the same layout.
    const mat3 XYZ_TO_REC709 = mat3(
         3.2404542, -0.9692660,  0.0556434,
        -1.5371385,  1.8760108, -0.2040259,
        -0.4985314,  0.0415560,  1.0572252
    );

    return XYZ_TO_REC709 * xyz;
}

// Thin-film Fresnel replacing the substrate's own.
//   outsideIor: surrounding medium (1.0 for air)
//   cosTheta:   dot(V, N) at the surface
//   baseF0:     substrate specularity
//   thickness:  film thickness in nanometres
//   filmIor:    film index of refraction
vec3 calcIridescence(float outsideIor, float cosTheta, vec3 baseF0,
                     float thickness, float filmIor) {
    // Very thin films fade back to the surrounding medium rather than popping.
    float iridIor = mix(outsideIor, filmIor, smoothstep(0.0, 0.03, thickness));

    // Snell: sin^2(t2) = (n1/n2)^2 * sin^2(t1)
    float sinTheta2Sq = (outsideIor / iridIor) * (outsideIor / iridIor)
                        * (1.0 - cosTheta * cosTheta);
    float cosTheta2Sq = 1.0 - sinTheta2Sq;
    if (cosTheta2Sq < 0.0) {
        return vec3(1.0);   // total internal reflection
    }
    float cosTheta2 = sqrt(cosTheta2Sq);

    // Interface 1: outside -> film.
    float r0 = iorToF0(iridIor, outsideIor);
    float r12 = iridescenceFresnel(cosTheta, r0);
    float t121 = 1.0 - r12;
    float phi12 = (iridIor < outsideIor) ? PI : 0.0;
    float phi21 = PI - phi12;

    // Interface 2: film -> substrate, per channel.
    vec3 baseIor = f0ToIor(baseF0 + vec3(0.0001));
    vec3 r1 = iorToF0Vec(baseIor, iridIor);
    vec3 r23 = iridescenceFresnelVec(cosTheta2, r1);

    vec3 phi23 = vec3(0.0);
    if (baseIor.x < iridIor) phi23.x = PI;
    if (baseIor.y < iridIor) phi23.y = PI;
    if (baseIor.z < iridIor) phi23.z = PI;

    float opd = 2.0 * iridIor * thickness * cosTheta2;
    vec3 phi = vec3(phi21) + phi23;

    // Airy summation, two orders.
    vec3 r123Sq = clamp(vec3(r12) * r23, vec3(1e-5), vec3(0.9999));
    vec3 r123 = sqrt(r123Sq);
    vec3 rs = (t121 * t121) * r23 / (1.0 - r123Sq);

    vec3 i = vec3(r12) + rs;
    vec3 cm = rs - t121;
    for (int m = 1; m <= 2; ++m) {
        cm *= r123;
        vec3 sm = 2.0 * iridescenceSensitivity(float(m) * opd, float(m) * phi);
        i += cm * sm;
    }
    return max(i, vec3(0.0));
}

// Air (IOR 1.0) -> film -> substrate.
vec3 getIridescence(float cosTheta, vec3 specularity, float thickness, float filmIor) {
    return calcIridescence(1.0, cosTheta, specularity, thickness, filmIor);
}
