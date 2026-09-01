
// ── Nishita single-scattering atmosphere (parity with common-atmosphere.metal) ──
// Ray-sphere intersection. Returns (tNear, tFar), or (-1, -1) when there is no hit.
vec2 raySphereIntersect(vec3 rayOrigin, vec3 rayDir, vec3 sphereCenter, float sphereRadius) {
    vec3 oc = rayOrigin - sphereCenter;
    float b = dot(oc, rayDir);
    float c = dot(oc, oc) - sphereRadius * sphereRadius;
    float discriminant = b * b - c;
    if (discriminant < 0.0) return vec2(-1.0);
    float sqrtD = sqrt(discriminant);
    return vec2(-b - sqrtD, -b + sqrtD);
}

// Sky colour for a view direction, ray-marched through the atmosphere. Works
// from ground level and from space.
//
// PRECISION: all geometry (ray origin, sphere radii) is normalized by the
// planet radius so the math stays near 1.0. Without this, camera distances of
// ~20M m against radii of ~6.4M m cause catastrophic cancellation in the
// ray-sphere intersection (b*b - c with both terms ~1e12, but float32 carries
// only ~7 digits). Density and optical depth stay in real metres.
vec3 nishitaScatter(vec3 viewDir) {
    vec3  planetCenter = lighting.atmoPlanetCenterAndRadius.xyz;
    float planetR      = lighting.atmoPlanetCenterAndRadius.w;
    float atmoR        = lighting.atmoRadiusAndSunIntensity.x;
    float sunIntensity = lighting.atmoRadiusAndSunIntensity.y;
    float sunDiskCos   = lighting.atmoRadiusAndSunIntensity.z;
    vec3  betaR        = lighting.atmoRayleighCoeffAndScale.xyz;
    float hR           = lighting.atmoRayleighCoeffAndScale.w;
    float betaM        = lighting.atmoMieCoeffAndScale.x;
    float hM           = lighting.atmoMieCoeffAndScale.y;
    float g            = lighting.atmoMieCoeffAndScale.z;
    vec3  sunDir       = normalize(lighting.atmoSunDirection.xyz);
    float atmoThickness = atmoR - planetR;

    float invR = 1.0 / planetR;
    vec3  rayOriginN = -planetCenter * invR;   // camera position in planet radii
    float atmoRN     = atmoR * invR;           // ~1.0157

    vec2 atmoHit = raySphereIntersect(rayOriginN, viewDir, vec3(0.0), atmoRN);
    if (atmoHit.y < 0.0) return vec3(0.0);

    vec2 planetHit = raySphereIntersect(rayOriginN, viewDir, vec3(0.0), 1.0);
    bool hitsGround = planetHit.x > 0.0;

    float tStartN = max(atmoHit.x, 0.0);
    float tEndN = hitsGround ? planetHit.x : atmoHit.y;
    if (tEndN <= tStartN) return vec3(0.0);

    // Step counts are compile-time constants here, exactly as in the Metal
    // chunk — cameraAltitudeAndParams.yz carry the same values but a dynamic
    // loop bound would cost more than it buys.
    const int kPrimarySteps = 32;
    const int kSecondarySteps = 8;
    float segLenN = (tEndN - tStartN) / float(kPrimarySteps);
    float segLenM = segLenN * planetR;

    vec3 totalR = vec3(0.0);
    vec3 totalM = vec3(0.0);
    float opticalDepthR = 0.0;
    float opticalDepthM = 0.0;

    for (int i = 0; i < kPrimarySteps; ++i) {
        vec3 sampleN = rayOriginN + viewDir * (tStartN + (float(i) + 0.5) * segLenN);
        float altitudeM = clamp((length(sampleN) - 1.0) * planetR, 0.0, atmoThickness);

        float densityR = exp(-altitudeM / hR) * segLenM;
        float densityM = exp(-altitudeM / hM) * segLenM;
        opticalDepthR += densityR;
        opticalDepthM += densityM;

        vec2 sunHit = raySphereIntersect(sampleN, sunDir, vec3(0.0), atmoRN);
        if (sunHit.y > 0.0) {
            vec2 sunPlanetHit = raySphereIntersect(sampleN, sunDir, vec3(0.0), 1.0);
            if (sunPlanetHit.x > 0.0) {
                continue;  // in planet shadow
            }

            float sunSegLenN = sunHit.y / float(kSecondarySteps);
            float sunSegLenM = sunSegLenN * planetR;
            float sunOptDepthR = 0.0;
            float sunOptDepthM = 0.0;
            for (int j = 0; j < kSecondarySteps; ++j) {
                vec3 sunSampleN = sampleN + sunDir * ((float(j) + 0.5) * sunSegLenN);
                float sunAltM = clamp((length(sunSampleN) - 1.0) * planetR, 0.0, atmoThickness);
                sunOptDepthR += exp(-sunAltM / hR) * sunSegLenM;
                sunOptDepthM += exp(-sunAltM / hM) * sunSegLenM;
            }

            vec3 tau = betaR * (opticalDepthR + sunOptDepthR) +
                       betaM * 1.1 * (opticalDepthM + sunOptDepthM);
            vec3 attenuation = exp(-tau);

            totalR += densityR * attenuation;
            totalM += densityM * attenuation;
        }
    }

    float cosTheta = dot(viewDir, sunDir);
    float cos2 = cosTheta * cosTheta;

    // Rayleigh phase: 3/(16pi) * (1 + cos^2 theta)
    float phaseR = 3.0 / (16.0 * PI) * (1.0 + cos2);

    // Mie phase: Henyey-Greenstein
    float g2 = g * g;
    float denom = pow(max(1.0 + g2 - 2.0 * g * cosTheta, 1e-6), 1.5);
    float phaseM = 3.0 / (8.0 * PI) * ((1.0 - g2) * (1.0 + cos2)) /
                   ((2.0 + g2) * denom);

    vec3 skyColor = sunIntensity * (phaseR * betaR * totalR + phaseM * betaM * totalM);

    if (cosTheta > sunDiskCos) {
        vec3 tauView = betaR * opticalDepthR + betaM * 1.1 * opticalDepthM;
        vec3 sunTransmittance = exp(-tauView);
        float sunEdge = smoothstep(sunDiskCos, sunDiskCos + 0.0002, cosTheta);
        skyColor += sunIntensity * sunTransmittance * sunEdge;
    }

    // NaN guard: clamp alone does not catch NaN (its comparisons are all false).
    if (any(isnan(skyColor)) || any(isinf(skyColor))) {
        return vec3(0.0);
    }
    return clamp(skyColor, vec3(0.0), vec3(100.0));
}

// GGX normal distribution.
float distributionGGX(float NdotH, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float d = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / max(PI * d * d, 1e-7);
}

// Smith geometry term (Schlick-GGX, direct lighting k).
float geometrySmith(float NdotV, float NdotL, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    float gv = NdotV / (NdotV * (1.0 - k) + k);
    float gl = NdotL / (NdotL * (1.0 - k) + k);
    return gv * gl;
}

vec3 fresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

