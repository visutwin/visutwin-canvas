// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 04.11.2026
//
// The atmosphere of the dynamic sky (DynamicSkyEffect) without its lights, shared by the sky's
// own program (programs/dynamic-sky.slang) and the forward tail's haze (VT_FEATURE_SKY_HAZE),
// which fades a lit surface into the sky behind it. Its CPU twin is DynamicSkyEffect::
// getSkyColor. Directions are in sky space: the world turned by the scene's sky rotation, with X
// flipped.
//
// The day atmosphere is a port of the three.js `Sky` shader (MIT licensed), which implements the
// Preetham et al. analytic daylight model:
//   - Model:  A. J. Preetham, P. Shirley, B. Smits, "A Practical Analytic Model for Daylight" (1999)
//   - Shader: three.js `Sky` - Simon Wallner (original atmospheric scattering), Martin Upitis
//             (improved Rayleigh/Mie), Joshua Koo / zz85 (three.js port). https://threejs.org (MIT)
module vtdynamicsky;

static const float kDynamicSkyPi = 3.141592653589793;

// The lanes of the effect's block the atmosphere reads.
public struct DynamicSkyAtmosphere
{
    public float4 sunDir;       // xyz toward the sun, w the sun's intensity E
    public float4 betaR;        // xyz Rayleigh scattering, w Mie anisotropy
    public float4 betaM;        // xyz Mie scattering, w the scale of the sky's radiance
    public float4 night;        // xyz the night colour times its brightness, w the night blend
    public float4 nightParams;  // w the twilight glow
};

float dynamicSkyRayleighPhase(float cosT) { return (3.0 / (16.0 * kDynamicSkyPi)) * (1.0 + cosT * cosT); }

float dynamicSkyHgPhase(float cosT, float g)
{
    const float g2 = g * g;
    const float inv = 1.0 / pow(max(1.0 - 2.0 * g * cosT + g2, 0.0), 1.5);
    return (1.0 / (4.0 * kDynamicSkyPi)) * ((1.0 - g2) * inv);
}

// The day sky's radiance in a direction, without the sun disc, which goes to `sunDisc`.
public float3 dynamicSkyDayRadiance(DynamicSkyAtmosphere atmo, float3 dirIn, out float3 sunDisc)
{
    const float3 up = float3(0.0, 1.0, 0.0);
    const float3 dir = normalize(dirIn);
    const float3 sunDir = normalize(atmo.sunDir.xyz);
    const float3 betaR = atmo.betaR.xyz;
    const float3 betaM = atmo.betaM.xyz;
    const float sunE = atmo.sunDir.w;

    // the optical length along the view direction
    const float zenithAngle = acos(max(0.0, dot(up, dir)));
    const float denom = cos(zenithAngle) + 0.15 * pow(max(93.885 - zenithAngle * (180.0 / kDynamicSkyPi), 0.0), -1.253);
    const float sR = 8.4e3 / denom;
    const float sM = 1.25e3 / denom;
    const float3 fex = exp(-(betaR * sR + betaM * sM));

    // in-scattering
    const float cosTheta = dot(dir, sunDir);
    const float3 betaRTheta = betaR * dynamicSkyRayleighPhase(cosTheta * 0.5 + 0.5);
    const float3 betaMTheta = betaM * dynamicSkyHgPhase(cosTheta, atmo.betaR.w);
    const float3 betaSum = betaR + betaM;
    float3 lin = pow(sunE * ((betaRTheta + betaMTheta) / betaSum) * (1.0 - fex), float3(1.5, 1.5, 1.5));
    lin *= lerp(float3(1.0, 1.0, 1.0), pow(sunE * ((betaRTheta + betaMTheta) / betaSum) * fex, float3(0.5, 0.5, 0.5)),
        clamp(pow(1.0 - dot(up, sunDir), 5.0), 0.0, 1.0));

    // the base of the sky and the sun disc, kept apart so the clouds can block it fully and the
    // lighting can leave it out
    const float3 l0 = float3(0.1, 0.1, 0.1) * fex;
    const float sunAngularCos = 0.999956676;
    const float sundisk = smoothstep(sunAngularCos, sunAngularCos + 0.00002, cosTheta);
    const float3 disc = sunE * 19000.0 * fex * sundisk;

    const float luminance = atmo.betaM.w;
    sunDisc = disc * (0.04 * luminance);
    return ((lin + l0) * 0.04 + float3(0.0, 0.0003, 0.00075)) * luminance;
}

// The night sky without its lights: a deep gradient, and a warm twilight glow centred on the
// horizon below the sun, fading as the sun sinks well below it.
public float3 dynamicSkyNightRadiance(DynamicSkyAtmosphere atmo, float3 dir)
{
    float3 col = atmo.night.xyz * lerp(1.1, 0.4, max(dir.y, 0.0));
    const float3 sunH = normalize(float3(atmo.sunDir.x, 0.0, atmo.sunDir.z) + float3(1e-5, 0.0, 0.0));
    const float toSun = max(dot(dir, sunH), 0.0);
    const float belowFade = clamp(1.0 + atmo.sunDir.y * 4.0, 0.0, 1.0);
    col += float3(1.0, 0.45, 0.15) * (pow(toSun, 5.0) * belowFade * atmo.nightParams.w);
    return col;
}

// The sky's radiance in a direction without its lights, the day and the night blended.
public float3 dynamicSkyRadiance(DynamicSkyAtmosphere atmo, float3 dir)
{
    const float nb = atmo.night.w;
    float3 disc;
    if (nb <= 0.0) {
        return dynamicSkyDayRadiance(atmo, dir, disc);
    }
    if (nb >= 1.0) {
        return dynamicSkyNightRadiance(atmo, dir);
    }
    return lerp(dynamicSkyDayRadiance(atmo, dir, disc), dynamicSkyNightRadiance(atmo, dir), nb);
}

// The colour of the haze in a world direction: the sky behind it, turned into sky space by the
// columns of `rotation` and X flipped. Below the horizon, where the daylight model has no answer,
// the haze takes the colour of the sky just above it.
public float3 dynamicSkyHazeColor(DynamicSkyAtmosphere atmo, float4 rotation[3], float3 worldDir)
{
    float3 dir = rotation[0].xyz * worldDir.x + rotation[1].xyz * worldDir.y + rotation[2].xyz * worldDir.z;
    dir = float3(-dir.x, dir.y, dir.z) / max(length(dir), 1e-6);
    dir.y = max(dir.y, 0.015);
    return dynamicSkyRadiance(atmo, normalize(dir));
}

// The fraction of a surface the haze hides, from its position and the camera's: the haze's
// density (haze.x at the base height haze.z) falls off exponentially with the height (haze.y is
// one over its scale height), integrated along the ray, with its limit along a level ray.
public float dynamicSkyHazeAmount(float4 haze, float3 cameraPos, float3 worldPos)
{
    const float b = haze.y;
    const float h0 = cameraPos.y - haze.z;
    const float h1 = worldPos.y - haze.z;
    const float x = b * (h1 - h0);
    const float e0 = exp(-b * h0);
    const float density = abs(x) < 1e-3 ? e0 * (1.0 - 0.5 * x) : (e0 - exp(-b * h1)) / x;
    return 1.0 - exp(-haze.x * density * distance(worldPos, cameraPos));
}
