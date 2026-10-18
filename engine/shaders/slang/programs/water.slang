// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 17.10.2026
//
// The water surface of the Water script (extras/script/water.h): two scrolling layers of a
// ripple normal map over optional Gerstner waves, the planar reflection or the sky, the planar
// refraction absorbed with the water depth (the scene depth grab), a sun highlight widened by
// the waves filtered out in the distance, shoreline foam and a soft shoreline, and, seen from
// below, the Snell window over the underwater fog.
//
// Inputs: the reflection at the material's base colour map, the ripple normal map at its normal
// map, the refraction at its emissive map (both planar textures hold gamma-encoded colour and
// are decoded here), the scene's environment atlas and the depth grab. Which parts exist is
// feature word 0 (water.cpp: WaterSwitch), so one library serves every combination. The output
// is linear HDR on a camera frame's scene, gamma-encoded otherwise, and never tone mapped.
import vtdraw;
#include "bindings.slang"

struct WaterData
{
    float4 shallowColor;        // rgb linear, w: ripple tiling
    float4 deepColor;           // rgb linear, w: ripple speed
    float4 lightDir;            // xyz the direction the light travels, w: bumpiness
    float4 lightColor;          // rgb linear x intensity, w: distortion
    float4 cameraPosition;      // xyz, w: time
    float4 cameraForward;       // xyz, w: sky blur
    float4 params0;             // fresnel power, reflection strength, depth fade, shore softness
    float4 params1;             // opacity, specular power, specular intensity, diffuse intensity
    float4 params2;             // wave filtering, underwater fog density, camera below, Snell window
    float4 underwaterFogColor;  // rgb linear, w: unused
    float4 foam;                // rgb linear foam colour, w: foam depth
    float4 waveParams;          // amplitude, frequency, speed, steepness
    float4 swellParams;         // amplitude, frequency, speed, steepness
    float4 waveDirs;            // xy the wave direction, zw the swell's
    float4 output;              // near, far, 1 for linear output, the env atlas encoding
};
VT_DECLARE_MATERIAL_UNIFORMS(WaterData, water)

VT_DECLARE_baseColorMap
VT_DECLARE_normalMap
VT_DECLARE_emissiveMap
VT_DECLARE_envAtlas
VT_DECLARE_sceneDepthGrab
// The forward pass's clamping samplers, at the Metal slots it binds them to (the table's rows
// have none).
[[vk::binding(12, 3)]] SamplerState linearClampSampler : register(s7);
[[vk::binding(13, 3)]] SamplerState nearestClampSampler : register(s8);

static const uint kWaterReflectionPlanar = 1u << 0u;
static const uint kWaterReflectionSky = 1u << 1u;
static const uint kWaterRefraction = 1u << 2u;
static const uint kWaterDepthEffects = 1u << 3u;
static const uint kWaterFoam = 1u << 4u;
static const uint kWaterWaves = 1u << 5u;

bool waterSwitch(uint bit)
{
    return (vtFeatureMask0 & bit) != 0u;
}

// ---- Gerstner waves: three chop octaves and two swell octaves derived from a main direction.

// Per octave: x the direction's angle offset (radians), y the amplitude, z the frequency and w
// the speed multipliers. Irrational-ish frequency ratios and irregular directions and phases
// keep the interference from repeating visibly.
static const float4 kChopOctaves[3] = {
    float4(0.00, 1.00, 1.00, 1.00),
    float4(0.41, 0.55, 1.73, 0.87),
    float4(-0.87, 0.32, 2.61, 0.79)
};
static const float4 kSwellOctaves[2] = {
    float4(0.00, 1.00, 1.00, 1.00),
    float4(-0.51, 0.65, 0.63, 1.13)
};

float2 waveRotate(float2 v, float a)
{
    const float c = cos(a);
    const float s = sin(a);
    return float2(c * v.x - s * v.y, s * v.x + c * v.y);
}

// A slowly varying amplitude envelope, different per octave, so the field does not look the
// same everywhere over large distances.
float waveEnvelope(float2 p, float2 dir, float freq, float seed)
{
    const float2 envDir = waveRotate(dir, 1.07);
    return 0.55 + 0.45 * sin(freq * 0.17 * dot(envDir, p) + seed * 4.7);
}

float3 gerstnerDisplace(float2 p, float4 octave, float4 params, float2 mainDir, float seed, float time)
{
    const float2 dir = waveRotate(mainDir, octave.x);
    const float freq = params.y * octave.z;
    const float amp = params.x * octave.y * waveEnvelope(p, dir, freq, seed);
    const float th = freq * dot(dir, p) + params.z * octave.w * freq * time + seed * 2.3;
    return float3(dir.x * params.w * amp * cos(th), amp * sin(th), dir.y * params.w * amp * cos(th));
}

// The share of a wave of frequency `freq` kept at a pixel footprint (world units): a wave
// shorter than about two pixels aliases into flicker, so it fades out from eight pixels a
// wave length.
float waveKeep(float freq, float footprint)
{
    return 1.0 - smoothstep(0.125, 0.5, footprint * freq * 0.15915494);
}

// A wave's normal, faded out by its share kept at the footprint; the mean square of the slope
// faded out is added to `variance`.
float3 gerstnerNormal(float2 p, float4 octave, float4 params, float2 mainDir, float seed, float time,
    float footprint, inout float variance)
{
    const float2 dir = waveRotate(mainDir, octave.x);
    const float freq = params.y * octave.z;
    float amp = params.x * octave.y * waveEnvelope(p, dir, freq, seed);
    const float keep = waveKeep(freq, footprint);
    const float slope = freq * amp;
    variance += 0.5 * slope * slope * (1.0 - keep * keep);
    amp *= keep;
    const float th = freq * dot(dir, p) + params.z * octave.w * freq * time + seed * 2.3;
    return float3(-dir.x * freq * amp * cos(th), -params.w * freq * amp * sin(th), -dir.y * freq * amp * cos(th));
}

float3 waveDisplacement(float2 p)
{
    const float time = water.cameraPosition.w;
    float3 d = float3(0.0);
    for (int i = 0; i < 3; i++) {
        d += gerstnerDisplace(p, kChopOctaves[i], water.waveParams, water.waveDirs.xy, float(i), time);
    }
    for (int i = 0; i < 2; i++) {
        d += gerstnerDisplace(p, kSwellOctaves[i], water.swellParams, water.waveDirs.zw, float(i) + 5.0, time);
    }
    return d;
}

// The waves' normal with those shorter than the footprint faded out, and the variance of their
// slopes, which roughens the highlight instead.
float3 waveNormal(float2 p, float footprint, out float variance)
{
    const float time = water.cameraPosition.w;
    variance = 0.0;
    float3 n = float3(0.0, 1.0, 0.0);
    for (int i = 0; i < 3; i++) {
        n += gerstnerNormal(p, kChopOctaves[i], water.waveParams, water.waveDirs.xy, float(i), time, footprint, variance);
    }
    for (int i = 0; i < 2; i++) {
        n += gerstnerNormal(p, kSwellOctaves[i], water.swellParams, water.waveDirs.zw, float(i) + 5.0, time,
            footprint, variance);
    }
    return n;
}

// ---- The environment atlas, laid out as the engine bakes it.

float2 waterDirToEquirect(float3 dir)
{
    const float azimuth = (dir.x == 0.0 && dir.z == 0.0) ? 0.0 : atan2(dir.x, dir.z);
    const float2 sph = float2(azimuth, asin(clamp(dir.y, -1.0, 1.0)));
    const float2 uv = sph / float2(6.28318530718, 3.14159265359) + 0.5;
    return float2(uv.x, 1.0 - uv.y);
}

float2 waterRoughnessUv(float2 uv, float level)
{
    const float seam = 1.0 / 512.0;
    const float t = 1.0 / exp2(level);
    const float4 rect = float4(0.0, 1.0 - t, t, t * 0.5);
    return float2(lerp(rect.x + seam, rect.x + rect.z - seam, uv.x), lerp(rect.y + seam, rect.y + rect.w - seam, uv.y));
}

float3 waterDecodeEnv(float4 raw, uint encoding)
{
    if (encoding == 1u) {
        const float3 c = raw.rgb * (-raw.a * 7.0 + 8.0);
        return c * c;
    }
    if (encoding == 2u) {
        const float3 c = (8.0 * raw.a) * raw.rgb;
        return c * c;
    }
    return pow(max(raw.rgb, float3(0.0)), float3(2.2));
}

float3 decodeGamma(float3 c)
{
    return pow(max(c, float3(0.0)), float3(2.2));
}

// The linear view depth of a depth-grab texel.
float linearSceneDepth(float2 uv)
{
    const float depth = sceneDepthGrab.SampleLevel(nearestClampSampler, uv, 0.0).r;
    const float n = water.output.x;
    const float f = water.output.y;
    const float ndc = depth * 2.0 - 1.0;
    return 2.0 * n * f / (f + n - ndc * (f - n));
}

float3 waterOutput(float3 color)
{
    return water.output.z > 0.5 ? color : pow(max(color, float3(0.0)), float3(1.0 / 2.2));
}

struct VertexIn
{
    [[vk::location(0)]] float3 position : POSITION;
};

struct Varyings
{
    float4 position : SV_Position;
    float3 worldPos : TEXCOORD0;
    float4 screenPos : TEXCOORD1;
    float2 wavePos : TEXCOORD2;
    float viewDepth : TEXCOORD3;
};

[shader("vertex")]
Varyings waterVertex(VertexIn input)
{
    Varyings output;
    float4 worldPos = mul(float4(input.position, 1.0), drawModel());
    output.wavePos = worldPos.xz;
    if (waterSwitch(kWaterWaves)) {
        worldPos.xyz += waveDisplacement(worldPos.xz);
    }
    output.worldPos = worldPos.xyz;

    float4 clip = mul(worldPos, drawViewProjection());
    output.screenPos = clip;
    output.viewDepth = dot(worldPos.xyz - water.cameraPosition.xyz, water.cameraForward.xyz);
    clip.z = 0.5 * (clip.z + clip.w);
    output.position = clip;
    return output;
}

[shader("fragment")]
float4 waterFragment(Varyings input) : SV_Target0
{
    const float time = water.cameraPosition.w;
    const float rippleTiling = water.shallowColor.w;
    const float rippleSpeed = water.deepColor.w;
    const float bumpiness = water.lightDir.w;
    const float distortion = water.lightColor.w;
    const float3 shallowColorIn = water.shallowColor.rgb;
    const float3 deepColorIn = water.deepColor.rgb;
    const float3 lightColor = water.lightColor.rgb;

    // Two layers of scrolling detail normals in the tangent space of a horizontal plane, the
    // second turned so the two tilings never line up into a grid.
    const float2 baseUv = input.worldPos.xz * rippleTiling;
    const float st = time * rippleSpeed;
    const float2 rotUv = float2(0.79 * baseUv.x - 0.61 * baseUv.y, 0.61 * baseUv.x + 0.79 * baseUv.y);
    const float3 nm1 = normalMap.Sample(baseUv + float2(st, st * 0.7)).xyz * 2.0 - 1.0;
    const float3 nm2 = normalMap.Sample(rotUv * 1.73 + float2(-st * 0.8, st * 1.1)).xyz * 2.0 - 1.0;
    const float2 detail = (nm1.xy + nm2.xy) * 0.5 * bumpiness;

    // With the waves, filtered by the pixel's footprint on the water along its longer axis,
    // which stretches far toward the horizon.
    float3 waveN = float3(0.0, 1.0, 0.0);
    float slopeVariance = 0.0;
    if (waterSwitch(kWaterWaves)) {
        const float footprint = max(length(ddx(input.wavePos)), length(ddy(input.wavePos))) * water.params2.x;
        waveN = waveNormal(input.wavePos, footprint, slopeVariance);
    }
    const float3 N = normalize(float3(waveN.x + detail.x, waveN.y, waveN.z + detail.y));

    const float3 V = normalize(water.cameraPosition.xyz - input.worldPos);
    const float NdotV = max(dot(N, V), 0.0);

    // The screen position: the refraction camera matches this one, the reflection camera is
    // mirrored by the plane, which turns its image upside down.
    const float2 ndc = input.screenPos.xy / input.screenPos.w;
    const float2 screenUV = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    const float2 refractionUV = screenUV + N.xz * distortion;
    const float2 reflectionUV = float2(screenUV.x, 1.0 - screenUV.y) + N.xz * distortion;

    // Every texture is sampled up front, before any divergent flow. The undistorted refraction
    // is the fallback where the distortion reaches past a silhouette into what is above water.
    float3 refrSample = shallowColorIn;
    float3 refrUndistorted = shallowColorIn;
    if (waterSwitch(kWaterRefraction)) {
        refrSample = decodeGamma(emissiveMap.Sample(refractionUV).rgb);
        refrUndistorted = decodeGamma(emissiveMap.Sample(screenUV).rgb);
    }
    float3 reflSample = deepColorIn;
    if (waterSwitch(kWaterReflectionPlanar)) {
        reflSample = decodeGamma(baseColorMap.Sample(reflectionUV).rgb);
    }
    if (waterSwitch(kWaterReflectionSky)) {
        float3 R = reflect(-V, N);
        R.y = abs(R.y); // nothing below the horizon: the sky mirrored instead
        const float2 skyUv = waterDirToEquirect(float3(-R.x, R.y, R.z));
        const float level = clamp(water.cameraForward.w, 0.0, 4.0);
        const float ilevel = floor(level);
        const uint encoding = uint(water.output.w + 0.5);
        const float3 skyA = waterDecodeEnv(envAtlas.SampleLevel(linearClampSampler, waterRoughnessUv(skyUv, ilevel), 0.0), encoding);
        const float3 skyB = waterDecodeEnv(envAtlas.SampleLevel(linearClampSampler, waterRoughnessUv(skyUv, ilevel + 1.0), 0.0), encoding);
        reflSample = lerp(skyA, skyB, level - ilevel);
    }

    // Seen from below: the world above in the Snell window overhead (the refraction), total
    // internal reflection of the underwater scene around it (the reflection), fading into the
    // underwater fog with distance. Chosen by the camera's side, not per pixel: at grazing
    // angles the perturbed normal can turn against the view and speckle.
    if (water.params2.z > 0.5) {
        const float NdotUp = max(dot(-N, V), 0.0);
        const float windowCenter = 1.0 - water.params2.w;
        const float window = smoothstep(windowCenter - 0.07, windowCenter + 0.07, NdotUp);
        float3 col = lerp(reflSample, refrSample, window);
        const float dist = length(water.cameraPosition.xyz - input.worldPos);
        col = lerp(water.underwaterFogColor.rgb, col, exp(-dist * water.params2.y));
        return float4(waterOutput(col), 1.0);
    }

    // The distance the view ray travels under water.
    float waterDepth = 0.0;
    float absorb = 0.8;
    if (waterSwitch(kWaterDepthEffects)) {
        float sceneDepth = linearSceneDepth(screenUV + N.xz * distortion);
        if (sceneDepth < input.viewDepth) {
            // The distorted tap lies above the water: the undistorted one, for the depth and the
            // refraction alike, so nothing above water leaks into the shoreline.
            sceneDepth = linearSceneDepth(screenUV);
            refrSample = refrUndistorted;
        }
        waterDepth = max(sceneDepth - input.viewDepth, 0.0);
        absorb = 1.0 - exp(-waterDepth / water.params0.z);
    }

    // The sun's diffuse light, tinting the water body.
    const float3 L = -water.lightDir.xyz;
    const float3 sunDiffuse = lerp(float3(1.0), lightColor * max(dot(N, L), 0.0), water.params1.w);
    const float3 shallowColor = shallowColorIn * sunDiffuse;
    const float3 deepColor = deepColorIn * sunDiffuse;

    // The water body: the refracted scene, absorbed with depth.
    const float3 base = waterSwitch(kWaterRefraction)
        ? lerp(refrSample * shallowColor, deepColor, absorb)
        : lerp(shallowColor, deepColor, absorb);

    // Fresnel between the body and the reflection.
    const bool hasReflection = waterSwitch(kWaterReflectionPlanar) || waterSwitch(kWaterReflectionSky);
    const float fresnel = lerp(0.02, 1.0, pow(1.0 - NdotV, water.params0.x));
    float3 color = lerp(base, hasReflection ? reflSample : float3(0.0),
        (hasReflection ? 1.0 : 0.0) * clamp(fresnel * water.params0.y, 0.0, 1.0));

    // The sun's highlight. The slopes of the waves filtered out in the distance roughen it:
    // added to its mean square slope (Beckmann m^2 = 2 / (power + 2)) they widen it into the
    // glitter of the sun on the far waves, its energy kept.
    const float specularPower = water.params1.y;
    const float3 H = normalize(L + V);
    const float specPower = max(2.0 / (2.0 / (specularPower + 2.0) + slopeVariance) - 2.0, 1.0);
    const float specScale = (specPower + 2.0) / (specularPower + 2.0);
    color += lightColor * pow(max(dot(N, H), 0.0), specPower) * water.params1.z * specScale;

    // Foam along the shoreline, lit by the light so it dims at night; the extra luminance
    // factor keeps white foam from glowing against a dark night scene, and its colour stays
    // its own (no orange foam at sunrise).
    if (waterSwitch(kWaterFoam) && waterSwitch(kWaterDepthEffects)) {
        const float foamNoise = clamp(0.5 + 0.5 * (nm1.x + nm2.y), 0.0, 1.0);
        float foamMask = 1.0 - smoothstep(0.0, water.foam.w, waterDepth);
        foamMask = smoothstep(0.3, 0.7, foamMask * (0.4 + 0.6 * foamNoise));
        const float lightLum = dot(lightColor, float3(0.299, 0.587, 0.114));
        const float foamAtten = clamp(lightLum, 0.0, 1.0);
        const float3 foamLit = water.foam.rgb * (max(dot(N, L), 0.0) * lightLum * foamAtten * foamAtten * foamAtten);
        color = lerp(color, foamLit, foamMask);
    }

    // Opaque where the refraction supplies the background, otherwise by the Fresnel.
    float alpha = waterSwitch(kWaterRefraction) ? 1.0 : lerp(water.params1.x, 1.0, clamp(fresnel, 0.0, 1.0));

    // The soft intersection with the geometry along the shoreline.
    if (waterSwitch(kWaterDepthEffects)) {
        alpha *= clamp(waterDepth / water.params0.w, 0.0, 1.0);
    }

    return float4(waterOutput(color), alpha);
}
