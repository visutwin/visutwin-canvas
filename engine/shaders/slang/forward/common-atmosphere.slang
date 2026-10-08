// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 07.10.2026
//

// ── Nishita single-scattering atmosphere ──
// Ray-sphere intersection. Returns (tNear, tFar), or (-1, -1) when there is no hit.
float2 raySphereIntersect(float3 rayOrigin, float3 rayDir, float3 sphereCenter, float sphereRadius) {
    float3 oc = rayOrigin - sphereCenter;
    float b = dot(oc, rayDir);
    float c = dot(oc, oc) - sphereRadius * sphereRadius;
    float discriminant = b * b - c;
    if (discriminant < 0.0) return float2(-1.0);
    float sqrtD = sqrt(discriminant);
    return float2(-b - sqrtD, -b + sqrtD);
}

// Sky colour for a view direction, ray-marched through the atmosphere. Works
// from ground level and from space.
//
// PRECISION: all geometry (ray origin, sphere radii) is normalized by the
// planet radius so the math stays near 1.0. Without this, camera distances of
// ~20M m against radii of ~6.4M m cause catastrophic cancellation in the
// ray-sphere intersection (b*b - c with both terms ~1e12, but float32 carries
// only ~7 digits). Density and optical depth stay in real metres.
float3 nishitaScatter(float3 viewDir) {
    float3  planetCenter = lighting.atmoPlanetCenterAndRadius.xyz;
    float planetR      = lighting.atmoPlanetCenterAndRadius.w;
    float atmoR        = lighting.atmoRadiusAndSunIntensity.x;
    float sunIntensity = lighting.atmoRadiusAndSunIntensity.y;
    float sunDiskCos   = lighting.atmoRadiusAndSunIntensity.z;
    float3  betaR        = lighting.atmoRayleighCoeffAndScale.xyz;
    float hR           = lighting.atmoRayleighCoeffAndScale.w;
    float betaM        = lighting.atmoMieCoeffAndScale.x;
    float hM           = lighting.atmoMieCoeffAndScale.y;
    float g            = lighting.atmoMieCoeffAndScale.z;
    float3  sunDir       = normalize(lighting.atmoSunDirection.xyz);
    float atmoThickness = atmoR - planetR;

    float invR = 1.0 / planetR;
    float3  rayOriginN = -planetCenter * invR;   // camera position in planet radii
    float atmoRN     = atmoR * invR;           // ~1.0157

    float2 atmoHit = raySphereIntersect(rayOriginN, viewDir, float3(0.0), atmoRN);
    if (atmoHit.y < 0.0) return float3(0.0);

    float2 planetHit = raySphereIntersect(rayOriginN, viewDir, float3(0.0), 1.0);
    bool hitsGround = planetHit.x > 0.0;

    float tStartN = max(atmoHit.x, 0.0);
    float tEndN = hitsGround ? planetHit.x : atmoHit.y;
    if (tEndN <= tStartN) return float3(0.0);

    // Step counts are compile-time constants here — cameraAltitudeAndParams.yz
    // carry the same values but a dynamic loop bound would cost more than it buys.
    const int kPrimarySteps = 32;
    const int kSecondarySteps = 8;
    float segLenN = (tEndN - tStartN) / float(kPrimarySteps);
    float segLenM = segLenN * planetR;

    float3 totalR = float3(0.0);
    float3 totalM = float3(0.0);
    float opticalDepthR = 0.0;
    float opticalDepthM = 0.0;

    for (int i = 0; i < kPrimarySteps; ++i) {
        float3 sampleN = rayOriginN + viewDir * (tStartN + (float(i) + 0.5) * segLenN);
        float altitudeM = clamp((length(sampleN) - 1.0) * planetR, 0.0, atmoThickness);

        float densityR = exp(-altitudeM / hR) * segLenM;
        float densityM = exp(-altitudeM / hM) * segLenM;
        opticalDepthR += densityR;
        opticalDepthM += densityM;

        float2 sunHit = raySphereIntersect(sampleN, sunDir, float3(0.0), atmoRN);
        if (sunHit.y > 0.0) {
            float2 sunPlanetHit = raySphereIntersect(sampleN, sunDir, float3(0.0), 1.0);
            if (sunPlanetHit.x > 0.0) {
                continue;  // in planet shadow
            }

            float sunSegLenN = sunHit.y / float(kSecondarySteps);
            float sunSegLenM = sunSegLenN * planetR;
            float sunOptDepthR = 0.0;
            float sunOptDepthM = 0.0;
            for (int j = 0; j < kSecondarySteps; ++j) {
                float3 sunSampleN = sampleN + sunDir * ((float(j) + 0.5) * sunSegLenN);
                float sunAltM = clamp((length(sunSampleN) - 1.0) * planetR, 0.0, atmoThickness);
                sunOptDepthR += exp(-sunAltM / hR) * sunSegLenM;
                sunOptDepthM += exp(-sunAltM / hM) * sunSegLenM;
            }

            float3 tau = betaR * (opticalDepthR + sunOptDepthR) +
                       betaM * 1.1 * (opticalDepthM + sunOptDepthM);
            float3 attenuation = exp(-tau);

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

    float3 skyColor = sunIntensity * (phaseR * betaR * totalR + phaseM * betaM * totalM);

    if (cosTheta > sunDiskCos) {
        float3 tauView = betaR * opticalDepthR + betaM * 1.1 * opticalDepthM;
        float3 sunTransmittance = exp(-tauView);
        float sunEdge = smoothstep(sunDiskCos, sunDiskCos + 0.0002, cosTheta);
        skyColor += sunIntensity * sunTransmittance * sunEdge;
    }

    // NaN guard: clamp alone does not catch NaN (its comparisons are all false).
    if (any(isnan(skyColor)) || any(isinf(skyColor))) {
        return float3(0.0);
    }
    return clamp(skyColor, float3(0.0), float3(100.0));
}

// The BRDF terms (distributionGGX, the Smith visibility, the Fresnel) live in
// common-brdf.slang. Do not define local variants here: a second spelling is a
// divergence by construction.

