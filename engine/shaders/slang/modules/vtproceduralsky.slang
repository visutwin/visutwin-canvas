// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 12.10.2026
//
// The procedural sky: an analytic Preetham daylight model, the three.js `Sky` shader
// (MIT) — A. J. Preetham, P. Shirley, B. Smits, "A Practical Analytic Model for Daylight"
// (1999); shader by Simon Wallner, Martin Upitis and Joshua Koo — plus a night sky of a
// gradient, a twilight band, hashed stars and a moon disk. Every direction here is in SKY
// space, the world with X negated: the space the environment atlas is looked up in, so the
// visible sun and its reflections line up. The view-independent scattering terms come
// precomputed from the CPU (ProceduralSky::update).
module vtproceduralsky;

/// The one uniform block both sky programs read (ProceduralSkyBlock on the C++ side).
public struct ProceduralSkyData
{
    public float4 sunDir;       // xyz: towards the sun, sky space; w: sun intensity (sunE)
    public float4 betaR;        // xyz: Rayleigh coefficient; w: Mie anisotropy g
    public float4 betaM;        // xyz: Mie coefficient; w: luminance (output HDR scale)
    public float4 night;        // x: night blend (0 day, 1 night), y: night brightness,
                                // z: star brightness, w: star density
    public float4 nightColor;   // xyz: night base colour; w: star size
    public float4 moonDir;      // xyz: towards the moon, sky space; w: moon radius (radians)
    public float4 moonColor;    // xyz: moon colour; w: moon glow
    public float4 output;       // x: sky intensity, y: exposure, z: tone mapping, w: flags
    public float4 extra;        // x: twilight glow, y: cube face (bake), zw: unused
};

/// output.w: the target takes linear HDR (a camera frame), so no tone mapping or gamma.
public static const uint SKY_FLAG_LINEAR_OUTPUT = 1u;

static const float PI_SKY = 3.141592653589793;
static const float3 SKY_UP = float3(0.0, 1.0, 0.0);
static const float SKY_RAYLEIGH_ZENITH = 8.4e3;
static const float SKY_MIE_ZENITH = 1.25e3;
static const float SKY_SUN_ANGULAR_COS = 0.999956676;   // cos of the sun disk's half angle

float skyRayleighPhase(float cosT)
{
    return (3.0 / (16.0 * PI_SKY)) * (1.0 + cosT * cosT);
}

float skyHgPhase(float cosT, float g)
{
    const float g2 = g * g;
    const float inv = 1.0 / pow(max(1.0 - 2.0 * g * cosT + g2, 0.0), 1.5);
    return (1.0 / (4.0 * PI_SKY)) * ((1.0 - g2) * inv);
}

/// Linear HDR daylight radiance along `dirIn`.
public float3 proceduralDaySky(ProceduralSkyData sky, float3 dirIn)
{
    const float3 dir = normalize(dirIn);
    const float3 sunDir = normalize(sky.sunDir.xyz);
    const float3 betaR = sky.betaR.xyz;
    const float3 betaM = sky.betaM.xyz;
    const float sunE = sky.sunDir.w;

    // Optical length along the view direction.
    const float zenithAngle = acos(max(0.0, dot(SKY_UP, dir)));
    const float denom = cos(zenithAngle) + 0.15 * pow(max(93.885 - degrees(zenithAngle), 0.0), -1.253);
    const float sR = SKY_RAYLEIGH_ZENITH / denom;
    const float sM = SKY_MIE_ZENITH / denom;

    const float3 Fex = exp(-(betaR * sR + betaM * sM));

    // In-scattering.
    const float cosTheta = dot(dir, sunDir);
    const float3 betaRTheta = betaR * skyRayleighPhase(cosTheta * 0.5 + 0.5);
    const float3 betaMTheta = betaM * skyHgPhase(cosTheta, sky.betaR.w);
    const float3 betaSum = betaR + betaM;

    float3 Lin = pow(sunE * ((betaRTheta + betaMTheta) / betaSum) * (1.0 - Fex), float3(1.5));
    Lin *= lerp(float3(1.0),
        pow(sunE * ((betaRTheta + betaMTheta) / betaSum) * Fex, float3(0.5)),
        clamp(pow(1.0 - dot(SKY_UP, sunDir), 5.0), 0.0, 1.0));

    // The sky base and the sun disk.
    float3 L0 = float3(0.1) * Fex;
    const float sundisk = smoothstep(SKY_SUN_ANGULAR_COS, SKY_SUN_ANGULAR_COS + 0.00002, cosTheta);
    L0 += sunE * 19000.0 * Fex * sundisk;

    const float3 col = (Lin + L0) * 0.04 + float3(0.0, 0.0003, 0.00075);
    // The sun disk is capped at a value the bloom chain's half-float sums hold.
    return min(col * sky.betaM.w, float3(6000.0));
}

float skyHash13(float3 p3)
{
    p3 = frac(p3 * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.x + p3.y) * p3.z);
}

/// Linear night radiance along `dirIn`: the stars and the moon disk are anti-aliased
/// against the pixel footprint, so this is for a fragment stage only.
public float3 proceduralNightSky(ProceduralSkyData sky, float3 dirIn)
{
    const float3 dir = normalize(dirIn);
    const float3 sunDir = sky.sunDir.xyz;

    // A base gradient, darker overhead.
    float3 col = sky.nightColor.xyz * sky.night.y * lerp(1.1, 0.4, max(dir.y, 0.0));

    // Twilight: a warm afterglow on the horizon point below the sun, fading out as the sun
    // sinks well below the horizon.
    const float3 sunH = normalize(float3(sunDir.x, 0.0, sunDir.z) + float3(1e-5, 0.0, 0.0));
    const float toSun = max(dot(dir, sunH), 0.0);
    const float belowFade = clamp(1.0 + sunDir.y * 4.0, 0.0, 1.0);
    col += float3(1.0, 0.45, 0.15) * (pow(toSun, 5.0) * belowFade * sky.extra.x);

    // Stars: one candidate per grid cell along the view direction, each of a random size
    // and brightness, with its edge anti-aliased so it stays a stable dot as the view turns.
    const float3 p = dir * 70.0;
    const float3 cell = floor(p);
    const float present = step(1.0 - sky.night.w, skyHash13(cell));
    const float3 sp = float3(0.3) + 0.4 * float3(skyHash13(cell + 11.0), skyHash13(cell + 23.0),
        skyHash13(cell + 37.0));
    const float sizeRnd = skyHash13(cell + 53.0);
    const float radius = lerp(0.05, 0.22, sizeRnd * sizeRnd) * sky.nightColor.w;
    const float d = length(frac(p) - sp);
    const float aa = length(fwidth(p)) + 1e-4;
    const float star = present * lerp(0.3, 1.0, skyHash13(cell + 71.0)) *
        (1.0 - smoothstep(radius - aa, radius + aa, d));
    col += float3(star * sky.night.z * smoothstep(0.0, 0.04, dir.y));

    // The moon: a small, sharp disk.
    const float c = dot(dir, normalize(sky.moonDir.xyz));
    const float aaMoon = max(fwidth(c), 1e-5);
    const float disk = smoothstep(cos(sky.moonDir.w) - aaMoon, cos(sky.moonDir.w) + aaMoon, c);
    col += sky.moonColor.xyz * sky.moonColor.w * disk;

    return col;
}
