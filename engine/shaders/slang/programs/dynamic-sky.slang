// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 17.10.2026
//
// The dynamic sky effect (DynamicSkyEffect), one program, a variant per pass (DS_PASS):
//   0 sky           the sky the effect draws for its camera, on its own sky mesh: the day
//                   atmosphere, the night sky, their lights and the clouds composited over them
//                   (none, flat, volumetric marched here, or volumetric read from the cloud
//                   buffer, its light range compressed when it is 8-bit);
//   1 cloud-buffer  the volumetric clouds into a texture of their own, at a reduced resolution;
//   2 bake          one face of the cube the scene's lighting is generated from: the sky
//                   without its lights (the sun disc, stars, moon), the clouds evaluated in place;
//   3 blend         the lighting blended from its previous update to its latest, RGBP decoded
//                   and encoded again (DS_COPY: a texel copy of an atlas).
//
// The day atmosphere is a port of the three.js `Sky` shader (MIT licensed), which implements the
// Preetham et al. analytic daylight model:
//   - Model:  A. J. Preetham, P. Shirley, B. Smits, "A Practical Analytic Model for Daylight" (1999)
//   - Shader: three.js `Sky` - Simon Wallner (original atmospheric scattering), Martin Upitis
//             (improved Rayleigh/Mie), Joshua Koo / zz85 (three.js port). https://threejs.org (MIT)
// The volumetric clouds are adapted from the sky shaders demo of the Godot Engine demo projects,
// https://github.com/godotengine/godot-demo-projects/tree/master/3d/sky_shaders - MIT License,
// Copyright (c) 2014-present Godot Engine contributors, Copyright (c) 2007-2014 Juan Linietsky,
// Ariel Manzur. Based on A. Schneider, "The Real-Time Volumetric Cloudscapes of Horizon: Zero
// Dawn", SIGGRAPH 2015.
//
// Directions are in sky space: the world turned by Scene::skyboxRotation, with X flipped, as the
// engine samples cubemaps and the environment atlas, so the visible sky and its baked lighting
// line up.
//
// @variant sky: DS_PASS=0
// @variant sky-flat: DS_PASS=0 DS_CLOUDS_FLAT=1
// @variant sky-volumetric: DS_PASS=0 DS_CLOUDS_VOLUMETRIC=1
// @variant sky-buffer: DS_PASS=0 DS_CLOUDS_VOLUMETRIC=1 DS_CLOUDS_BUFFER=1
// @variant sky-buffer-encoded: DS_PASS=0 DS_CLOUDS_VOLUMETRIC=1 DS_CLOUDS_BUFFER=1 DS_CLOUDS_ENCODED=1
// @variant cloud-buffer: DS_PASS=1 DS_CLOUDS_VOLUMETRIC=1
// @variant cloud-buffer-encoded: DS_PASS=1 DS_CLOUDS_VOLUMETRIC=1 DS_CLOUDS_ENCODED=1
// @variant bake: DS_PASS=2
// @variant bake-flat: DS_PASS=2 DS_CLOUDS_FLAT=1
// @variant bake-volumetric: DS_PASS=2 DS_CLOUDS_VOLUMETRIC=1
// @variant blend: DS_PASS=3
// @variant copy: DS_PASS=3 DS_COPY=1
import vtdraw;
import vtenv;
#include "bindings.slang"
#define VT_TONEMAP_OPERATORS_ONLY
#include "forward/common-tonemap.slang"

#if DS_PASS == 0 && (defined(DS_CLOUDS_FLAT) || defined(DS_CLOUDS_VOLUMETRIC))
#define DS_CLOUDS 1
#elif DS_PASS == 1
#define DS_CLOUDS 1
#elif DS_PASS == 2 && (defined(DS_CLOUDS_FLAT) || defined(DS_CLOUDS_VOLUMETRIC))
#define DS_CLOUDS 1
#endif

// DynamicSkyEffect's block (dynamicSkyEffect.cpp: DynamicSkyData), the same for every pass.
struct DynamicSkyData
{
    float4 sunDir;          // xyz toward the sun, w the sun's intensity E
    float4 betaR;           // xyz Rayleigh scattering, w Mie anisotropy
    float4 betaM;           // xyz Mie scattering, w the scale of the sky's radiance
    float4 night;           // xyz the night colour times its brightness, w the night blend
    float4 nightParams;     // star brightness, star density, star size, twilight glow
    float4 moonDir;         // xyz toward the moon
    float4 moonParams;      // xyz the moon disc's colour times its glow, w cos of its radius
    float4 cloudParams;     // coverage, softness, pattern repeats per km, height in km
    float4 cloudOffset;     // xy drift of the shapes, in their repeats; zw drift of the detail
    float4 cloudShading;    // x opacity, z fine step per coarse step, w offset of the jitter
    float4 cloudVolume;     // bottom and top of the layer in km, steps, steps toward the light
    float4 cloudLight;      // xyz the light above the clouds, w the scale of the 8-bit clouds
    float4 cloudLightDir;   // xyz toward that light, w the cube face (bake)
    float4 skyRotation[3];  // world to sky space (before the X flip), by columns
    float4 bufferRay[3];    // cloud buffer: view-space ray (ndc x projScale, -1) to sky space, by columns
    float4 bufferView[3];   // sky: sky space to the camera's view space, by columns
    float4 bufferProj;      // xy the projection's scale (p00, p11), zw 1 / the buffer's size
    float4 output;          // x exposure, y tone mapping, z 1 for linear HDR output, w the blend weight
};
VT_DECLARE_QUAD_UNIFORMS(DynamicSkyData, sky)

static const float kPi = 3.141592653589793;
static const float3 kUp = float3(0.0, 1.0, 0.0);

float3 columns3(float4 c[3], float3 v) { return c[0].xyz * v.x + c[1].xyz * v.y + c[2].xyz * v.z; }

// ---- the atmosphere and the night sky

float dsRayleighPhase(float cosT) { return (3.0 / (16.0 * kPi)) * (1.0 + cosT * cosT); }

float dsHgPhase(float cosT, float g)
{
    const float g2 = g * g;
    const float inv = 1.0 / pow(max(1.0 - 2.0 * g * cosT + g2, 0.0), 1.5);
    return (1.0 / (4.0 * kPi)) * ((1.0 - g2) * inv);
}

// The radiance of the last day-sky evaluation (lighting the clouds) and its sun disc, a light of
// the sky composited apart from it.
// Zero until the day sky runs: at night it does not, and the sun disc is read all the same.
static float3 dsScatter = float3(0.0, 0.0, 0.0);
static float3 dsSunDisc = float3(0.0, 0.0, 0.0);

// The day sky's radiance in a direction, without the sun disc (dsSunDisc).
float3 dynamicSkyDay(float3 dirIn)
{
    const float3 dir = normalize(dirIn);
    const float3 sunDir = normalize(sky.sunDir.xyz);
    const float3 betaR = sky.betaR.xyz;
    const float3 betaM = sky.betaM.xyz;
    const float sunE = sky.sunDir.w;

    // the optical length along the view direction
    const float zenithAngle = acos(max(0.0, dot(kUp, dir)));
    const float denom = cos(zenithAngle) + 0.15 * pow(max(93.885 - zenithAngle * (180.0 / kPi), 0.0), -1.253);
    const float sR = 8.4e3 / denom;
    const float sM = 1.25e3 / denom;
    const float3 fex = exp(-(betaR * sR + betaM * sM));

    // in-scattering
    const float cosTheta = dot(dir, sunDir);
    const float3 betaRTheta = betaR * dsRayleighPhase(cosTheta * 0.5 + 0.5);
    const float3 betaMTheta = betaM * dsHgPhase(cosTheta, sky.betaR.w);
    const float3 betaSum = betaR + betaM;
    float3 lin = pow(sunE * ((betaRTheta + betaMTheta) / betaSum) * (1.0 - fex), float3(1.5, 1.5, 1.5));
    lin *= lerp(float3(1.0, 1.0, 1.0), pow(sunE * ((betaRTheta + betaMTheta) / betaSum) * fex, float3(0.5, 0.5, 0.5)),
        clamp(pow(1.0 - dot(kUp, sunDir), 5.0), 0.0, 1.0));

    // the base of the sky and the sun disc, kept apart so the clouds can block it fully and the
    // lighting can leave it out
    const float3 l0 = float3(0.1, 0.1, 0.1) * fex;
    const float sunAngularCos = 0.999956676;
    const float sundisk = smoothstep(sunAngularCos, sunAngularCos + 0.00002, cosTheta);
    const float3 disc = sunE * 19000.0 * fex * sundisk;

    const float luminance = sky.betaM.w;
    dsScatter = ((lin + l0) * 0.04 + float3(0.0, 0.0003, 0.00075)) * luminance;
    dsSunDisc = disc * (0.04 * luminance);
    return dsScatter;
}

// The night sky without its lights: a deep gradient, and a warm twilight glow centred on the
// horizon below the sun, fading as the sun sinks well below it.
float3 dynamicSkyNightBase(float3 dir)
{
    float3 col = sky.night.xyz * lerp(1.1, 0.4, max(dir.y, 0.0));
    const float3 sunH = normalize(float3(sky.sunDir.x, 0.0, sky.sunDir.z) + float3(1e-5, 0.0, 0.0));
    const float toSun = max(dot(dir, sunH), 0.0);
    const float belowFade = clamp(1.0 + sky.sunDir.y * 4.0, 0.0, 1.0);
    col += float3(1.0, 0.45, 0.15) * (pow(toSun, 5.0) * belowFade * sky.nightParams.w);
    return col;
}

float dsHash13(float3 p3)
{
    p3 = frac(p3 * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.x + p3.y) * p3.z);
}

#if DS_PASS == 0
// The lights of the night sky: the stars, one candidate per cell of a grid along the view
// direction, and the moon disc, their edges anti-aliased against the pixel footprint.
float3 dynamicSkyNightLights(float3 dir)
{
    const float3 p = dir * 70.0;
    const float3 cell = floor(p);
    const float present = step(1.0 - sky.nightParams.y, dsHash13(cell));
    const float3 sp = float3(0.3, 0.3, 0.3) + 0.4 * float3(dsHash13(cell + 11.0), dsHash13(cell + 23.0), dsHash13(cell + 37.0));
    const float sizeRnd = dsHash13(cell + 53.0);
    const float radius = lerp(0.05, 0.22, sizeRnd * sizeRnd) * sky.nightParams.z;
    const float d = length(frac(p) - sp);
    const float aa = length(fwidth(p)) + 1e-4;
    const float star = present * lerp(0.3, 1.0, dsHash13(cell + 71.0)) * (1.0 - smoothstep(radius - aa, radius + aa, d));
    float3 col = float3(star, star, star) * (sky.nightParams.x * smoothstep(0.0, 0.04, dir.y));

    const float c = dot(dir, normalize(sky.moonDir.xyz));
    const float aaMoon = max(fwidth(c), 1e-5);
    const float disk = smoothstep(sky.moonParams.w - aaMoon, sky.moonParams.w + aaMoon, c);
    col += sky.moonParams.xyz * disk;
    return col;
}
#endif

// The sky's radiance in a direction without its lights, the day and the night blended, and the
// ambient light around it. The branch is the same for every pixel.
float3 dynamicSkyBase(float3 dir, out float3 ambient)
{
    const float nb = sky.night.w;
    const float3 nightAmbient = sky.night.xyz * 0.6;
    float3 radiance;
    if (nb <= 0.0) {
        radiance = dynamicSkyDay(dir);
        ambient = dsScatter;
    } else if (nb >= 1.0) {
        radiance = dynamicSkyNightBase(dir);
        ambient = nightAmbient;
    } else {
        radiance = lerp(dynamicSkyDay(dir), dynamicSkyNightBase(dir), nb);
        ambient = lerp(dsScatter, nightAmbient, nb);
    }
    return radiance;
}

// The ambient half of dynamicSkyBase alone (the day's ambient is its radiance).
float3 dynamicSkyAmbient(float3 dir)
{
    const float nb = sky.night.w;
    const float3 nightAmbient = sky.night.xyz * 0.6;
    if (nb >= 1.0) {
        return nightAmbient;
    }
    const float3 day = dynamicSkyDay(dir);
    return nb <= 0.0 ? day : lerp(day, nightAmbient, nb);
}

// ---- the textures, per pass: a material draw reads them through the material rows, a quad
// pass through its inputs (with their own samplers, so the noise repeats on both backends).

#if DS_PASS == 0
#if defined(DS_CLOUDS_FLAT) || (defined(DS_CLOUDS_VOLUMETRIC) && !defined(DS_CLOUDS_BUFFER))
VT_DECLARE_baseColorMap_AS(Sampler2D, cloudMap)
#endif
#if defined(DS_CLOUDS_VOLUMETRIC) && !defined(DS_CLOUDS_BUFFER)
VT_DECLARE_normalMap_AS(Sampler2DArray, cloudShapeMap)
VT_DECLARE_metalRoughMap_AS(Sampler2DArray, cloudDetailMap)
VT_DECLARE_occlusionMap_AS(Sampler2D, blueNoise)
#endif
#if defined(DS_CLOUDS_BUFFER)
VT_DECLARE_emissiveMap_AS(Sampler2D, cloudBuffer)
#endif
#elif DS_PASS == 1 || DS_PASS == 2
#if defined(DS_CLOUDS)
VT_DECLARE_QUAD_INPUT0(cloudMap)
#endif
#if defined(DS_CLOUDS_VOLUMETRIC)
VT_DECLARE_QUAD_ARRAY_INPUT1(cloudShapeMap)
VT_DECLARE_QUAD_ARRAY_INPUT2(cloudDetailMap)
VT_DECLARE_QUAD_INPUT3(blueNoise)
#endif
#else
VT_DECLARE_QUAD_TEXEL_INPUT0(previousAtlas)
VT_DECLARE_QUAD_TEXEL_INPUT1(latestAtlas)
#endif

// ---- the clouds: a layer at an altitude above a spherical planet, shaped by tileable noise
// drifting with the wind. A technique returns the light the clouds scatter toward the viewer in
// rgb and the fraction of the light behind them they let through in a.

#if defined(DS_CLOUDS)

static const float kPlanetRadius = 6371.0;   // km

// the distance along a view ray of the elevation `y` to a layer at the height `h`, in km
float dsLayerDistance(float y, float h)
{
    const float b = kPlanetRadius * y;
    return -b + sqrt(b * b + 2.0 * kPlanetRadius * h + h * h);
}

// how visible the clouds in a direction are, 0-1: they fade into the haze with the distance,
// and out toward the horizon
float dynamicSkyCloudFade(float3 dir)
{
    const float t = dsLayerDistance(max(dir.y, 0.0), sky.cloudParams.w);
    return sky.cloudShading.x * smoothstep(0.0, 0.08, dir.y) * exp(-t / 120.0);
}

// the clouds over the sky behind them and its lights; the lights, far brighter than the clouds,
// pass by the transmittance alone, and the dense clouds the march stopped in block them fully
float3 dynamicSkyCompositeClouds(float3 skyColor, float3 lights, float4 clouds, float3 dir)
{
    const float lightsTransmit = lerp(1.0, clamp((clouds.a - 0.01) / 0.99, 0.0, 1.0), sky.cloudShading.x);
    return skyColor * lerp(1.0, clouds.a, dynamicSkyCloudFade(dir)) + lights * lightsTransmit + clouds.rgb;
}

#if defined(DS_CLOUDS_FLAT)

float dsFlatDensity(float2 uv)
{
    const float4 shape = cloudMap.Sample(uv + sky.cloudOffset.xy);
    const float n = shape.r * 0.62 + shape.g * 0.38;
    return clamp((n - (1.0 - sky.cloudParams.x)) / sky.cloudParams.y, 0.0, 1.0);
}

// the flat clouds in a direction, lit by the light above them and the sky around them
float4 dynamicSkyCloudsFlat(float3 dir, float3 ambient)
{
    const float y = max(dir.y, 0.0);
    const float3 l = normalize(sky.cloudLightDir.xyz);
    const float mu = dot(dir, l);
    const float t = dsLayerDistance(y, sky.cloudParams.w);
    const float density = dsFlatDensity(dir.xz * (t * sky.cloudParams.z));
    const float rim = pow(max(mu, 0.0), 8.0) * (1.0 - density) * 1.5;
    float3 color = ambient * lerp(1.0, 0.55, density) + sky.cloudLight.xyz * (exp(-density * 1.2) * 0.75 + rim);
    color = lerp(color, ambient, 1.0 - exp(-t / 45.0));
    const float cover = 1.0 - exp(-density * 5.0);
    const float transmit = (1.0 - cover) * (1.0 - density * density * density);
    return float4(color * (cover * dynamicSkyCloudFade(dir)), transmit);
}

#endif

#if defined(DS_CLOUDS_VOLUMETRIC) && !defined(DS_CLOUDS_BUFFER)

static const float kShapeLayers = 64.0;
static const float kDetailLayers = 32.0;
static const float kDetailScale = 10.0;     // repeats of the detail per repeat of the shapes
static const float kWeatherScale = 0.25;    // repeats of the weather per repeat of the shapes
static const float kExtinction = 50.0;      // per km, at the full density
static const float kMaxPath = 20.0;         // km: the longest march through the layer
static const int kMaxSteps = 128;

// directions spreading the samples toward the light into a cone
static const float3 kCone[6] = {
    float3(0.38051305, 0.92453449, -0.02111345),
    float3(-0.50625799, -0.03590792, -0.86163418),
    float3(-0.32509218, -0.94557439, 0.01428793),
    float3(0.09026238, -0.27376545, 0.95755165),
    float3(0.28128598, 0.42443639, -0.86065785),
    float3(-0.16852403, 0.14748697, 0.97460106)
};

// Henyey-Greenstein, relative to the isotropic phase
float dsCloudHg(float mu, float g)
{
    const float g2 = g * g;
    return (1.0 - g2) / pow(max(1.0 + g2 - 2.0 * g * mu, 1e-4), 1.5);
}

// 3D noise in the layers of a texture array, interpolated between the layers
float dsShapeNoise(float3 p)
{
    const float z = p.y * kShapeLayers - 0.5;
    const float z0 = floor(z);
    const float l0 = z0 - kShapeLayers * floor(z0 / kShapeLayers);
    const float l1 = l0 + 1.0 < kShapeLayers ? l0 + 1.0 : 0.0;
    const float a = cloudShapeMap.SampleLevel(float3(p.xz, l0), 0.0).r;
    const float b = cloudShapeMap.SampleLevel(float3(p.xz, l1), 0.0).r;
    return lerp(a, b, z - z0);
}

float dsDetailNoise(float3 p)
{
    const float z = p.y * kDetailLayers - 0.5;
    const float z0 = floor(z);
    const float l0 = z0 - kDetailLayers * floor(z0 / kDetailLayers);
    const float l1 = l0 + 1.0 < kDetailLayers ? l0 + 1.0 : 0.0;
    const float a = cloudDetailMap.SampleLevel(float3(p.xz, l0), 0.0).r;
    const float b = cloudDetailMap.SampleLevel(float3(p.xz, l1), 0.0).r;
    return lerp(a, b, z - z0);
}

// the fraction of the height through the layer of a point relative to the viewer, in km
float dsHeightFraction(float3 pos)
{
    const float altitude = pos.y + dot(pos.xz, pos.xz) / (2.0 * kPlanetRadius);
    return clamp((altitude - sky.cloudVolume.x) / (sky.cloudVolume.y - sky.cloudVolume.x), 0.0, 1.0);
}

// the profile of the clouds over the layer by their kind: stratus (0), stratocumulus (0.5), cumulus (1)
float dsHeightGradient(float h, float kind)
{
    const float4 g = float4(0.02, 0.05, 0.09, 0.11) * clamp(1.0 - kind * 2.0, 0.0, 1.0) +
        float4(0.02, 0.2, 0.48, 0.625) * (1.0 - abs(kind - 0.5) * 2.0) +
        float4(0.01, 0.0625, 0.78, 1.0) * clamp(kind - 0.5, 0.0, 1.0) * 2.0;
    return smoothstep(g.x, g.y, h) - smoothstep(g.z, g.w, h);
}

// the coverage at a point of the weather pattern: the weather gathers the clouds into fields
float dsFieldCoverage(float weather)
{
    const float coverage = sky.cloudParams.x;
    const float spread = min(coverage * 2.5, 1.0);
    const float field = smoothstep(1.0 - spread, 1.0 - spread * 0.6, weather);
    return field * max(coverage, 0.3) * lerp(0.6, 1.0, weather);
}

// the density at a point in repeats of the shapes, at the height fraction h; the detail erodes
// the edges, and the softness thins or thickens the clouds
float dsVolumeDensity(float3 p, float h, float coverage, float kind, bool detail)
{
    const float shape = dsShapeNoise(p + float3(sky.cloudOffset.x, 0.0, sky.cloudOffset.y));
    float cloud = shape * dsHeightGradient(h, kind) + coverage - 1.0;
    if (detail && cloud > 0.0) {
        float hfbm = dsDetailNoise(p * kDetailScale + float3(sky.cloudOffset.z, 0.0, sky.cloudOffset.w));
        hfbm = lerp(hfbm, 1.0 - hfbm, clamp(h * 4.0, 0.0, 1.0));
        const float erosion = hfbm * lerp(0.1, 0.3, h);
        cloud = (cloud - erosion) / (1.0 - erosion);
    }
    return pow(clamp(cloud, 0.0, 1.0), (1.0 - h) * 0.8 + 0.5) * (0.3 / sky.cloudParams.y);
}

// blue noise tiled over the pixels, offset every frame when frames are accumulated
float dsJitter(float2 pixel)
{
    const int2 texel = int2(floor(pixel)) & 31;
    const float noise = blueNoise.SampleLevel((float2(texel) + 0.5) / 32.0, 0.0).r;
    return frac(noise + sky.cloudShading.w);
}

// the volumetric clouds in a direction: coarse steps of the cheap density without the detail
// across the empty sky, stepping back into fine steps on finding a cloud, until it is left
float4 dynamicSkyCloudsVolumetric(float3 dir, float3 ambient, float2 pixel)
{
    const float fadeIn = sky.cloudShading.x * smoothstep(0.0, 0.08, dir.y);
    if (fadeIn <= 0.0 || sky.cloudParams.x <= 0.0) {
        return float4(0.0, 0.0, 0.0, 1.0);
    }

    const float y = max(dir.y, 0.0);
    const float3 l = normalize(sky.cloudLightDir.xyz);
    const float mu = dot(dir, l);
    const float steps = sky.cloudVolume.z;
    const float lightSteps = sky.cloudVolume.w;
    const float scale = sky.cloudParams.z;

    const float tStart = dsLayerDistance(y, sky.cloudVolume.x);
    const float tEnd = min(dsLayerDistance(y, sky.cloudVolume.y), tStart + kMaxPath);
    const float coarse = (tEnd - tStart) / (steps * 0.5);
    const float ds = coarse * sky.cloudShading.z;

    // the samples toward the light reach about a sixth of the layer, whatever their count
    const float lightStep = (sky.cloudVolume.y - sky.cloudVolume.x) / (6.0 * lightSteps);
    const float3 cloudAmbient = lerp(ambient, float3(1.0, 1.0, 1.0) * dot(ambient, float3(0.2126, 0.7152, 0.0722)), 0.5);

    // stacked phase lobes: the forward lobe gives the silver lining, the others back scattering
    const float phase = max(max(dsCloudHg(mu, 0.6), dsCloudHg(mu, 0.4 - 1.4 * l.y)), dsCloudHg(mu, -0.2));

    float transmittance = 1.0;
    float3 scattered = float3(0.0, 0.0, 0.0);
    float t = tStart + coarse * dsJitter(pixel);
    bool inCloud = false;
    float empty = 0.0;
    float coverage = 0.0;
    float kind = 0.0;
    for (int i = 0; i < kMaxSteps; ++i) {
        if (float(i) >= steps || t > tEnd || transmittance < 0.01) {
            break;
        }
        const float3 pos = dir * t;
        const float h = dsHeightFraction(pos);

        if (!inCloud) {
            // the weather sets the fields and the kind; the fine steps through a cloud reuse it
            const float4 weather = cloudMap.SampleLevel((pos.xz * scale + sky.cloudOffset.xy) * kWeatherScale, 0.0);
            coverage = dsFieldCoverage(weather.a);
            kind = lerp(0.5, 1.0, weather.r);
            if (dsVolumeDensity(pos * scale, h, coverage, kind, false) > 0.0) {
                inCloud = true;
                empty = 0.0;
                t -= coarse - ds;
            } else {
                t += coarse;
            }
            continue;
        }
        t += ds;

        // the detail is lost in the distance
        const float density = dsVolumeDensity(pos * scale, h, coverage, kind, t < 25.0);
        if (density <= 0.0) {
            empty += 1.0;
            inCloud = empty < 6.0;
        } else {
            empty = 0.0;

            // the cloud toward the light: samples spreading in a cone, and a distant one
            float lightDensity = 0.0;
            float3 lp = pos;
            for (int j = 0; j < 6; ++j) {
                if (float(j) >= lightSteps) {
                    break;
                }
                lp += (l + kCone[j] * float(j)) * lightStep;
                lightDensity += dsVolumeDensity(lp * scale, dsHeightFraction(lp), coverage, kind, false);
            }
            const float3 fp = pos + l * (3.0 * lightSteps * lightStep);
            lightDensity += dsVolumeDensity(fp * scale, dsHeightFraction(fp), coverage, kind, false);

            // the weaker second absorption keeps the insides bright (multiple scattering)
            const float depth = kExtinction * lightDensity * lightStep;
            const float beers = max(exp(-depth), exp(-depth * 0.25) * 0.4);

            // the sky lights the tops of the clouds more than their bases
            const float3 inScatter = sky.cloudLight.xyz * (beers * phase) + cloudAmbient * lerp(0.3, 1.0, h);
            const float stepTransmittance = exp(-kExtinction * density * ds);
            scattered += inScatter * (transmittance * (1.0 - stepTransmittance));
            transmittance *= stepTransmittance;
        }
    }

    // the haze between the viewer and the distant clouds
    scattered = lerp(scattered, ambient * (1.0 - transmittance), 1.0 - exp(-tStart / 45.0));
    return float4(scattered * (fadeIn * exp(-tStart / 120.0)), transmittance);
}

#endif
#endif

#if defined(DS_CLOUDS_BUFFER)

float4 dsCloudsTap(float2 uv)
{
    float4 clouds = cloudBuffer.SampleLevel(uv, 0.0);
#if defined(DS_CLOUDS_ENCODED)
    clouds.rgb = clouds.rgb / max(1.0 - clouds.rgb, float3(1e-4, 1e-4, 1e-4)) * sky.cloudLight.w;
#endif
    return clouds;
}

// the clouds along a sky direction (before the X flip), from the texture they were rendered to:
// the direction projected as the pass derives each texel's, blurred over 3x3 of its texels by 4
// bilinear taps 2/3 of a texel from the centre, which removes the grain of the jittered steps
float4 dynamicSkyCloudsBuffer(float3 rotatedDir)
{
    const float3 v = columns3(sky.bufferView, rotatedDir);
    const float2 ndc = (v.xy / max(-v.z, 1e-6)) * sky.bufferProj.xy;
    const float2 uv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    const float2 offset = (2.0 / 3.0) * sky.bufferProj.zw;
    return 0.25 * (dsCloudsTap(uv + float2(-offset.x, -offset.y)) + dsCloudsTap(uv + float2(offset.x, -offset.y)) +
        dsCloudsTap(uv + float2(-offset.x, offset.y)) + dsCloudsTap(uv + float2(offset.x, offset.y)));
}

#endif

// ---- the stages

struct DynamicSkyVertexIn
{
    [[vk::location(0)]] float3 position : POSITION;
#if DS_PASS != 0
    [[vk::location(2)]] float2 uv0 : TEXCOORD0;
#endif
};

struct DynamicSkyVarying
{
    float4 position : SV_Position;
    float3 viewDir : TEXCOORD0;   // the sky: the world direction; the quads: uv in xy
};

// One entry per stage for every variant (the bundle finds entries in the source text).
[shader("vertex")]
DynamicSkyVarying dynamicSkyVertex(DynamicSkyVertexIn input)
{
    DynamicSkyVarying output;
#if DS_PASS == 0
    // the sky mesh follows the camera: the pre-transform position is the view ray, drawn a hair
    // inside the far plane
    float4 clip = mul(float4(input.position, 1.0), mul(drawModel(), drawViewProjection()));
    clip.z = clip.w - 1.0e-7;
    output.position = clip;
    output.viewDir = input.position;
#else
    output.position = float4(input.position, 1.0);
    output.viewDir = float3(input.uv0, 0.0);
#endif
    return output;
}

float4 dynamicSkyShade(DynamicSkyVarying input)
{
#if DS_PASS == 0
    // to sky space: turned by the sky's rotation, X flipped
    const float3 rotated = columns3(sky.skyRotation, normalize(input.viewDir));
    float3 dir = normalize(rotated);
    dir.x = -dir.x;

    float3 ambient;
    float3 radiance = dynamicSkyBase(dir, ambient);

    // the lights: the sun disc, and at night the stars and the moon
    float3 lights = dsSunDisc * (1.0 - sky.night.w);
    if (sky.night.w > 0.0) {
        lights += dynamicSkyNightLights(dir) * sky.night.w;
    }

#if defined(DS_CLOUDS_BUFFER)
    radiance = dynamicSkyCompositeClouds(radiance, lights, dynamicSkyCloudsBuffer(rotated), dir);
#elif defined(DS_CLOUDS_FLAT)
    radiance = dynamicSkyCompositeClouds(radiance, lights, dynamicSkyCloudsFlat(dir, ambient), dir);
#elif defined(DS_CLOUDS_VOLUMETRIC)
    radiance = dynamicSkyCompositeClouds(radiance, lights, dynamicSkyCloudsVolumetric(dir, ambient, input.position.xy), dir);
#else
    radiance += lights;
#endif

    // the sun disc clamped to what the bloom's half-float sums can hold
    radiance = min(radiance, float3(6000.0, 6000.0, 6000.0));
    if (sky.output.z > 0.5) {
        return float4(max(radiance, float3(0.0, 0.0, 0.0)), 1.0);
    }
    const float3 mapped = toneMapExposed(radiance, sky.output.x, int(sky.output.y + 0.5));
    return float4(pow(max(mapped, float3(0.0, 0.0, 0.0)), float3(1.0 / 2.2, 1.0 / 2.2, 1.0 / 2.2)), 1.0);

#elif DS_PASS == 1
    // the sky-space direction of the texel (a perspective camera); the quad's v runs down
    const float2 uv = input.viewDir.xy;
    const float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float3 dir = normalize(columns3(sky.bufferRay, float3(ndc, -1.0)));
    dir.x = -dir.x;
    float4 clouds = dynamicSkyCloudsVolumetric(dir, dynamicSkyAmbient(dir), input.position.xy);
#if defined(DS_CLOUDS_ENCODED)
    const float3 light = clouds.rgb / sky.cloudLight.w;
    clouds.rgb = light / (1.0 + light);
#endif
    return clouds;

#elif DS_PASS == 2
    // the face's direction as the hardware's cube lookup derives it; the sky without its lights
    const float2 uv = clamp(input.viewDir.xy, float2(0.0, 0.0), float2(1.0, 1.0));
    const float3 dir = cubeFaceUvToDir(uint(sky.cloudLightDir.w + 0.5), uv);
    float3 ambient;
    float3 radiance = dynamicSkyBase(dir, ambient);
#if defined(DS_CLOUDS_FLAT)
    radiance = dynamicSkyCompositeClouds(radiance, float3(0.0, 0.0, 0.0), dynamicSkyCloudsFlat(dir, ambient), dir);
#elif defined(DS_CLOUDS_VOLUMETRIC)
    radiance = dynamicSkyCompositeClouds(radiance, float3(0.0, 0.0, 0.0),
        dynamicSkyCloudsVolumetric(dir, ambient, input.position.xy), dir);
#endif
    return float4(radiance, 1.0);

#else
    // each texel read where it is written: the atlases share their layout
    const int3 texel = int3(int2(floor(input.position.xy)), 0);
#if defined(DS_COPY)
    return latestAtlas.Load(texel);
#else
    const float4 p = previousAtlas.Load(texel);
    const float4 l = latestAtlas.Load(texel);
    const float3 previous = p.rgb * (-p.a * 7.0 + 8.0);
    const float3 latest = l.rgb * (-l.a * 7.0 + 8.0);
    return packRgbp(lerp(previous * previous, latest * latest, sky.output.w));
#endif
#endif
}

[shader("fragment")]
float4 dynamicSkyFragment(DynamicSkyVarying input) : SV_Target0
{
    return dynamicSkyShade(input);
}
