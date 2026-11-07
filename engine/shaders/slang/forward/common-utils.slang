// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 07.10.2026
//
// ── Environment atlas layout (matches the engine's bake) ──
static const float ATLAS_SIZE = 512.0;
static const float ATLAS_SEAM = 1.0 / 512.0;

// Equirectangular direction → atlas UV (atan2 azimuth, asin elevation).
float2 dirToEquirect(float3 dir) {
    // atan2(0, 0) is undefined, and a direction of exactly +/-Y hits it. Some
    // targets happen to return 0 and so read the right texel; the guard picks
    // azimuth 0 at the pole so no backend depends on that.
    float azimuth = (dir.x == 0.0 && dir.z == 0.0) ? 0.0 : atan2(dir.x, dir.z);
    float2 sph = float2(azimuth, asin(clamp(dir.y, -1.0, 1.0)));
    float2 uv = sph / float2(6.28318530718, 3.14159265359) + 0.5;
    return float2(uv.x, 1.0 - uv.y);
}

// Map a [0,1] uv into a packed sub-rect (x,y,w,h), insetting by the 1px seam.
float2 mapRect(float2 uv, float4 rect) {
    return float2(lerp(rect.x + ATLAS_SEAM, rect.x + rect.z - ATLAS_SEAM, uv.x),
                lerp(rect.y + ATLAS_SEAM, rect.y + rect.w - ATLAS_SEAM, uv.y));
}

// Lambert irradiance sub-rect: 64×32 region at (128,384).
float2 mapAmbientUv(float2 uv) {
    return mapRect(uv, float4(128.0 / ATLAS_SIZE, 384.0 / ATLAS_SIZE,
                            64.0 / ATLAS_SIZE, 32.0 / ATLAS_SIZE));
}

// Prefiltered roughness mip `level` down the left edge: rect (0, 1-t, t, t/2).
float2 mapRoughnessUv(float2 uv, float level) {
    float t = 1.0 / exp2(level);
    return mapRect(uv, float4(0.0, 1.0 - t, t, t * 0.5));
}

// Sharp ("shiny") mip chain down the diagonal: rect (1-t, 1-t, t, t/2). The
// prefiltered chain above is convolved for roughness; level 0 of THIS one is the
// unconvolved environment, which is what a mirror must sample. Without it every
// mirror would sample the roughness rect and come out blurred.
float2 mapShinyUv(float2 uv, float level) {
    float t = 1.0 / exp2(level);
    return mapRect(uv, float4(1.0 - t, 1.0 - t, t, t * 0.5));
}

float3 decodeRGBP(float4 raw) { float3 c = raw.rgb * (-raw.a * 7.0 + 8.0); return c * c; }
float3 decodeRGBM(float4 raw) { float3 c = (8.0 * raw.a) * raw.rgb; return c * c; }
// srgbToLinear / linearToSrgb come from the vtsrgb module, which the program imports.

// A texel of a material's planar reflection map, decoded: a float map holds linear colours.
float3 decodePlanarReflection(float4 texel)
{
    return material.planarReflectionParams.y > 0.5 ? texel.rgb : srgbToLinear(texel.rgb);
}

// The weights of the cubic B-spline of the four texels around a sample position.
float4 planarCubicWeights(float v)
{
    const float4 n = float4(1.0, 2.0, 3.0, 4.0) - v;
    const float4 s = n * n * n;
    const float x = s.x;
    const float y = s.y - 4.0 * s.x;
    const float z = s.z - 4.0 * s.y + 6.0 * s.x;
    return float4(x, y, z, 6.0 - x - y - z) * (1.0 / 6.0);
}

// A mip level of the planar reflection map, filtered by a bicubic B-spline from four bilinear
// taps. Where a small mip level covers many pixels this is smooth, while the bilinear filtering
// shows its texels.
float3 samplePlanarBicubic(float2 uv, float level)
{
    const int2 mapSize = sizePlanarReflectionMap(int(level));
    const float2 size = float2(max(mapSize.x, 1), max(mapSize.y, 1));
    float2 texel = uv * size - 0.5;
    const float2 f = frac(texel);
    texel -= f;

    const float4 wx = planarCubicWeights(f.x);
    const float4 wy = planarCubicWeights(f.y);
    const float4 c = texel.xxyy + float2(-0.5, 1.5).xyxy;
    const float4 s = float4(wx.xz + wx.yw, wy.xz + wy.yw);
    const float4 offset = (c + float4(wx.yw, wy.yw) / s) / size.xxyy;

    const float3 s0 = decodePlanarReflection(samplePlanarReflectionMapLevel(offset.xz, level));
    const float3 s1 = decodePlanarReflection(samplePlanarReflectionMapLevel(offset.yz, level));
    const float3 s2 = decodePlanarReflection(samplePlanarReflectionMapLevel(offset.xw, level));
    const float3 s3 = decodePlanarReflection(samplePlanarReflectionMapLevel(offset.yw, level));

    const float sx = s.x / (s.x + s.y);
    const float sy = s.z / (s.z + s.w);
    return lerp(lerp(s3, s2, sx), lerp(s1, s0, sx), sy);
}

// A material's planar reflection map (VT_FEATURE_PLANAR_REFLECTION_MAP) along a reflected
// direction from `worldPos`: the point a short way along the ray is projected by the mirrored
// camera's view-projection into the map. For a surface lying on the plane this is the texel the
// surface itself projects to; a normal tilted by a normal map or a bevel moves it and distorts
// the reflection. A rougher surface reads a smaller mip, blurring it, up to the map's last level
// (planarReflectionParams.z, 0 without mipmaps). The top level is sampled bilinearly, which keeps
// a mirror sharp, the blurred levels by the bicubic B-spline, blending the two levels around the
// level of detail.
float3 samplePlanarReflectionMap(float3 worldPos, float3 reflDir, float gloss)
{
    const float4x4 m = float4x4(material.planarReflectionMatrix0, material.planarReflectionMatrix1,
                                material.planarReflectionMatrix2, material.planarReflectionMatrix3);
    const float4 clip = mul(float4(worldPos + reflDir * 0.2, 1.0), m);
    const float2 uv = saturate(clip.xy / max(clip.w, 1e-6) * float2(0.5, -0.5) + 0.5);
    const float lod = min((1.0 - gloss) * material.planarReflectionParams.x, material.planarReflectionParams.z);
    const float level = floor(lod);

    float3 reflection;
    if (level < 1.0) {
        reflection = decodePlanarReflection(samplePlanarReflectionMapLevel(uv, 0.0));
    } else {
        reflection = samplePlanarBicubic(uv, level);
    }
    if (lod > level) {
        reflection = lerp(reflection, samplePlanarBicubic(uv, level + 1.0), lod - level);
    }
    return reflection;
}

// An environment texel decoded by its encoding: 0 gamma, 1 RGBP, 2 RGBM, 3 linear.
float3 decodeEnvAs(float4 raw, uint enc) {
    if (enc == 1u) return decodeRGBP(raw);
    if (enc == 2u) return decodeRGBM(raw);
    if (enc == 3u) return raw.rgb;
    return srgbToLinear(raw.rgb);
}

// The scene atlas's (and the skybox's) decode, as a variant: the frame switches carry the
// encoding the binders write to envParams.z, so the shader holds one decode instead of every
// encoding at each of its four or five fetches (ambient-occlusion's forward pass: 0.065 ms).
float3 decodeEnv(float4 raw) {
    if (vtFeatureEnabled(VT_FEATURE_ENV_ATLAS_RGBP_BIT)) return decodeRGBP(raw);
    if (vtFeatureEnabled(VT_FEATURE_ENV_ATLAS_RGBM_BIT)) return decodeRGBM(raw);
    return srgbToLinear(raw.rgb);
}

// The environment a material reflects and takes its ambient from: its own atlas when it has
// one, the scene's otherwise. The scene's skybox intensity and rotation belong to the scene's
// atlas only, and a material that turns the scene environment off (flags bit 18, useSkybox
// off) still keeps an atlas of its own.
bool envFromMaterial() { return vtFeatureEnabled(VT_FEATURE_MATERIAL_ENV_ATLAS_BIT); }

bool envAvailable() {
    return envFromMaterial() || (vtFeatureEnabled(VT_FEATURE_ENV_ATLAS_BIT) &&
        lighting.envParams.y > 0.5 && (material.flags & (1u << 18)) == 0u);
}

float3 envFetch(float2 uv) {
    if (envFromMaterial()) {
        return decodeEnvAs(VT_BINDLESS_TEXTURE(VT_MATERIAL_LANE(VT_MAP_LANE_materialEnvAtlas)).SampleLevel(linearClampSampler, uv, 0.0),
            uint(material.envBoxMin.w + 0.5));
    }
    return decodeEnv(envAtlas.Sample(uv));
}

float3 envRotate(float3 dir) { return envFromMaterial() ? dir : cubeMapRotate(dir); }

float envIntensity() { return envFromMaterial() ? 1.0 : max(lighting.envParams.x, 0.0); }

// Box projection of a reflection direction (CUBEPROJ_BOX): the ray from the fragment meets the
// material's box, and the lookup is re-aimed from the box CENTRE at that point, so a flat
// environment tracks a room's walls as the surface moves. Without it, the direction as given.
float3 envProject(float3 dir, float3 worldPos) {
    if (!vtFeatureEnabled(VT_FEATURE_BOX_PROJECTION_BIT)) {
        return dir;
    }
    const float3 boxMin = material.envBoxMin.xyz;
    const float3 boxMax = material.envBoxMax.xyz;
    const float3 invDir = 1.0 / dir;
    const float3 toMax = (boxMax - worldPos) * invDir;
    const float3 toMin = (boxMin - worldPos) * invDir;
    const float3 far = select(dir > float3(0.0), toMax, toMin);
    const float t = min(min(far.x, far.y), far.z);
    return normalize(worldPos + dir * t - (boxMin + boxMax) * 0.5);
}

// Environment atlas lookup along a world direction at a roughness: the unconvolved shiny rect at a screen-space
// mip for a mirror, the prefiltered chain otherwise, blended toward the next
// roughness level. Decoded, NOT scaled by the environment intensity (envIntensity()). The
// material's atlas when it has one (envFetch). Uses screen
// derivatives, so call it from uniform control flow only. The env-atlas
// refraction uses it; the specular block in forward-fragment-ambient spells the
// same lookup inline.
float3 sampleEnvAtlas(float3 dir, float roughness) {
    dir = envRotate(dir);   // Scene::skyboxRotation for the scene's atlas
    float2 envUv = dirToEquirect(float3(-dir.x, dir.y, dir.z));
    float level = clamp(roughness * 5.0, 0.0, 5.0);
    float l0 = floor(level);

    float2 uvA = envUv * ATLAS_SIZE;
    float2 uvB = float2(frac(envUv.x + 0.5), envUv.y) * ATLAS_SIZE;
    float maxd = min(max(dot(ddx(uvA), ddx(uvA)), dot(ddy(uvA), ddy(uvA))),
                     max(dot(ddx(uvB), ddx(uvB)), dot(ddy(uvB), ddy(uvB))));
    float shinyLevel = clamp(0.5 * log2(max(maxd, 1e-12)) - 1.0, 0.0, 5.0);
    float shinyL0 = floor(shinyLevel);

    float3 envA;
    if (l0 == 0.0) {
        envA = lerp(envFetch(mapShinyUv(envUv, shinyL0)),
                   envFetch(mapShinyUv(envUv, shinyL0 + 1.0)),
                   shinyLevel - shinyL0);
    } else {
        envA = envFetch(mapRoughnessUv(envUv, l0));
    }
    float3 envB = envFetch(mapRoughnessUv(envUv, l0 + 1.0));
    return lerp(envA, envB, level - l0);
}


// The ambient diffuse irradiance arriving along the normal n: the SH light probes, else the
// environment atlas' Lambert rect (the material's own or the scene's), else the flat ambient.
// The ambient chunk reads it along the shading normal, and diffuse transmission along the
// flipped one, for the light arriving at the back of the surface.
float3 ambientIrradianceToward(float3 n) {
    if (vtFeatureEnabled(VT_FEATURE_LIGHT_PROBES_BIT)) {
        // 9-coefficient irradiance in the normal direction (coefficients premultiplied).
        float3 shN = cubeMapRotate(n);
        return max(
            lighting.ambientSH[0].rgb +
            lighting.ambientSH[1].rgb * shN.x +
            lighting.ambientSH[2].rgb * shN.y +
            lighting.ambientSH[3].rgb * shN.z +
            lighting.ambientSH[4].rgb * (shN.x * shN.z) +
            lighting.ambientSH[5].rgb * (shN.z * shN.y) +
            lighting.ambientSH[6].rgb * (shN.y * shN.x) +
            lighting.ambientSH[7].rgb * (3.0 * shN.z * shN.z - 1.0) +
            lighting.ambientSH[8].rgb * (shN.x * shN.x - shN.y * shN.y),
            float3(0.0));
    }
    if (envAvailable()) {
        // The negate-X matches the engine's atlas lookup handedness.
        float3 envN = envRotate(n);
        float3 diffDir = float3(-envN.x, envN.y, envN.z);
        return envFetch(mapAmbientUv(dirToEquirect(diffDir))) * envIntensity();
    }
    return lighting.ambient.rgb;
}
