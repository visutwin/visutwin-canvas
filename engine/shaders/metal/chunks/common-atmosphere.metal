// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
// ---------------------------------------------------------------------------
// Atmosphere scattering data (Nishita single-scattering).
// Bound at fragment buffer slot 9 when VT_FEATURE_ATMOSPHERE is enabled.
// ---------------------------------------------------------------------------

struct AtmosphereData {
    float4 planetCenterAndRadius;               // xyz = planet center (camera-local), w = planet radius (m)
    float4 atmosphereRadiusAndSunIntensity;     // x = atmosphere outer radius (m), y = sun intensity, z = cos(sun disk half-angle), w = 0
    float4 rayleighCoeffAndScaleHeight;         // xyz = Rayleigh scattering coefficients (per m), w = Rayleigh scale height (m)
    float4 mieCoeffAndScaleHeight;              // x = Mie scattering coefficient, y = Mie scale height (m), z = Mie g (HG phase), w = 0
    float4 sunDirection;                        // xyz = normalized sun direction (camera-local), w = 0
    float4 cameraAltitudeAndParams;             // x = altitude above surface (m), y = primary ray steps, z = secondary ray steps, w = 0
};

// Ray-sphere intersection. Returns (tNear, tFar) or (-1, -1) if no hit.
static inline float2 raySphereIntersect(float3 rayOrigin, float3 rayDir, float3 sphereCenter, float sphereRadius)
{
    const float3 oc = rayOrigin - sphereCenter;
    const float b = dot(oc, rayDir);
    const float c = dot(oc, oc) - sphereRadius * sphereRadius;
    const float discriminant = b * b - c;
    if (discriminant < 0.0) return float2(-1.0);
    const float sqrtD = sqrt(discriminant);
    return float2(-b - sqrtD, -b + sqrtD);
}

// Nishita single-scattering atmospheric model.
// Computes sky color for a given view direction by ray marching through the atmosphere.
// Works from both ground level and space.
//
// PRECISION: All geometry (ray origin, sphere radii) is normalized by planet radius
// so the math stays near 1.0.  Without this, camera distances of ~20M meters and
// radii of ~6.4M meters cause catastrophic cancellation in ray-sphere intersection
// (b^2 - c where both terms are ~10^12, but float32 only has 7 digits).
// Density and optical depth are computed in real meters (altitude * planetR).
static inline float3 nishitaScatter(float3 viewDir, constant AtmosphereData& atmo)
{
    const float3 planetCenter = atmo.planetCenterAndRadius.xyz;
    const float planetR = atmo.planetCenterAndRadius.w;
    const float atmoR = atmo.atmosphereRadiusAndSunIntensity.x;
    const float sunIntensity = atmo.atmosphereRadiusAndSunIntensity.y;
    const float sunDiskCos = atmo.atmosphereRadiusAndSunIntensity.z;
    const float3 betaR = atmo.rayleighCoeffAndScaleHeight.xyz;
    const float hR = atmo.rayleighCoeffAndScaleHeight.w;
    const float betaM = atmo.mieCoeffAndScaleHeight.x;
    const float hM = atmo.mieCoeffAndScaleHeight.y;
    const float g = atmo.mieCoeffAndScaleHeight.z;
    const float3 sunDir = normalize(atmo.sunDirection.xyz);
    const float atmoThickness = atmoR - planetR;

    // ── Normalize by planet radius so all geometry is near 1.0 ──────────
    const float invR = 1.0 / planetR;
    const float3 rayOriginN = -planetCenter * invR;   // camera pos in planet-radii
    const float atmoRN = atmoR * invR;                // ~1.0157

    // Ray-sphere intersections in normalized space (planet = unit sphere).
    const float2 atmoHit = raySphereIntersect(rayOriginN, viewDir, float3(0.0), atmoRN);
    if (atmoHit.y < 0.0) return float3(0.0);

    const float2 planetHit = raySphereIntersect(rayOriginN, viewDir, float3(0.0), 1.0);
    const bool hitsGround = (planetHit.x > 0.0);

    // Ray segment through atmosphere (in normalized units).
    const float tStartN = max(atmoHit.x, 0.0);
    float tEndN = atmoHit.y;
    if (hitsGround) {
        tEndN = planetHit.x;
    }
    if (tEndN <= tStartN) return float3(0.0);

    // ── Ray march ──────────────────────────────────────────────────────
    constexpr int kPrimarySteps = 32;
    constexpr int kSecondarySteps = 8;
    const float segLenN = (tEndN - tStartN) / float(kPrimarySteps);
    // Segment length in real meters (for density integration).
    const float segLenM = segLenN * planetR;

    float3 totalR = float3(0.0);
    float3 totalM = float3(0.0);
    float opticalDepthR = 0.0;
    float opticalDepthM = 0.0;

    for (int i = 0; i < kPrimarySteps; i++) {
        const float3 sampleN = rayOriginN + viewDir * (tStartN + (float(i) + 0.5) * segLenN);
        // Altitude in meters: (|pos_normalized| - 1.0) * planetR
        const float altitudeM = clamp((length(sampleN) - 1.0) * planetR, 0.0, atmoThickness);

        const float densityR = exp(-altitudeM / hR) * segLenM;
        const float densityM = exp(-altitudeM / hM) * segLenM;
        opticalDepthR += densityR;
        opticalDepthM += densityM;

        // Secondary ray toward sun.
        const float2 sunHit = raySphereIntersect(sampleN, sunDir, float3(0.0), atmoRN);
        if (sunHit.y > 0.0) {
            // Planet shadow test.
            const float2 sunPlanetHit = raySphereIntersect(sampleN, sunDir, float3(0.0), 1.0);
            if (sunPlanetHit.x > 0.0) {
                continue;  // in planet shadow
            }

            const float sunSegLenN = sunHit.y / float(kSecondarySteps);
            const float sunSegLenM = sunSegLenN * planetR;
            float sunOptDepthR = 0.0;
            float sunOptDepthM = 0.0;
            for (int j = 0; j < kSecondarySteps; j++) {
                const float3 sunSampleN = sampleN + sunDir * ((float(j) + 0.5) * sunSegLenN);
                const float sunAltM = clamp((length(sunSampleN) - 1.0) * planetR, 0.0, atmoThickness);
                sunOptDepthR += exp(-sunAltM / hR) * sunSegLenM;
                sunOptDepthM += exp(-sunAltM / hM) * sunSegLenM;
            }

            const float3 tau = betaR * (opticalDepthR + sunOptDepthR) +
                               betaM * 1.1 * (opticalDepthM + sunOptDepthM);
            const float3 attenuation = exp(-tau);

            totalR += densityR * attenuation;
            totalM += densityM * attenuation;
        }
    }

    // ── Phase functions ────────────────────────────────────────────────
    const float cosTheta = dot(viewDir, sunDir);
    const float cos2 = cosTheta * cosTheta;

    // Rayleigh phase: 3/(16pi) * (1 + cos^2 theta)
    const float phaseR = 3.0 / (16.0 * PI) * (1.0 + cos2);

    // Mie phase: Henyey-Greenstein
    const float g2 = g * g;
    const float denom = pow(max(1.0 + g2 - 2.0 * g * cosTheta, 1e-6), 1.5);
    const float phaseM = 3.0 / (8.0 * PI) * ((1.0 - g2) * (1.0 + cos2)) /
                         ((2.0 + g2) * denom);

    float3 skyColor = sunIntensity * (phaseR * betaR * totalR + phaseM * betaM * totalM);

    // Sun disk.
    if (cosTheta > sunDiskCos) {
        const float3 tauView = betaR * opticalDepthR + betaM * 1.1 * opticalDepthM;
        const float3 sunTransmittance = exp(-tauView);
        const float sunEdge = smoothstep(sunDiskCos, sunDiskCos + 0.0002, cosTheta);
        skyColor += sunIntensity * sunTransmittance * sunEdge;
    }

    // NaN guard: any(isnan()) catches precision edge cases the clamp alone misses
    // (NaN comparisons always return false, so clamp(NaN, 0, 100) = NaN).
    if (any(isnan(skyColor)) || any(isinf(skyColor))) {
        return float3(0.0);
    }
    return clamp(skyColor, float3(0.0), float3(100.0));
}

// Parallax Occlusion Mapping — adaptive ray-march with linear interpolation.
// Unreal/Unity/Wicked) instead of upstream's single-sample offset.
// viewDirTS: view direction in tangent space (normalized).
// Returns the parallax-adjusted UV.
