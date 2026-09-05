// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
constant float PI = 3.14159265358979323846;
constant float ATLAS_SIZE = 512.0;
// Seam inset MUST match the atlas-bake `seamPixels` convention. Both
// upstream and visutwin-canvas use 1-pixel duplicated border at every
// rect edge — pre-baked .png atlases (e.g. helipad-env-atlas.png) also
// follow this convention, so keep this at 1 pixel regardless of any
// CPU-bake seamPixels experimentation.
constant float ATLAS_SEAM = 1.0 / ATLAS_SIZE;
constant uint SPECOCC_NONE = 0u;
constant uint SPECOCC_AO = 1u;
constant uint SPECOCC_GLOSSDEPENDENT = 2u;

// apply a pre-computed 3×2 affine UV transform.
// row0/row1 encode tiling, offset, and rotation as computed on the CPU.
static inline float2 applyUvTransform(float2 uv, float4 row0, float4 row1) {
    return float2(dot(float3(uv, 1.0), row0.xyz),
                  dot(float3(uv, 1.0), row1.xyz));
}

static inline float3 srgbToLinear(float3 c) { return pow(max(c, float3(0.0)), float3(2.2)); }
static inline float3 linearToSrgb(float3 c) { return pow(max(c, float3(0.0)) + 0.0000001, float3(1.0 / 2.2)); }
static inline float3 decodeRGBP(float4 raw) { const float3 color = raw.rgb * (-raw.a * 7.0 + 8.0); return color * color; }
static inline float3 decodeRGBM(float4 raw) { const float3 color = (8.0 * raw.a) * raw.rgb; return color * color; }
static inline float square(float x) { return x * x; }

static inline float3 decodeEnvironment(float4 raw, constant LightingData& lighting) {
#if VT_FEATURE_ENV_ATLAS
    const uint envFlags = lighting.flagsAndPad.x;
    const bool envIsRgbp = (envFlags & (1u << 3)) != 0u;
    const bool envIsRgbm = (envFlags & (1u << 4)) != 0u;
    if (envIsRgbp) {
        return decodeRGBP(raw);
    }
    if (envIsRgbm) {
        return decodeRGBM(raw);
    }
#endif
    return srgbToLinear(raw.rgb);
}

static inline float2 toSphericalUv(float3 dir)
{
    // atan2(0, 0) is undefined, and a direction of exactly +/-Y hits it — which is
    // every fragment of an unrotated ground plane. Metal returned an out-of-range
    // azimuth there, so the mapped UV left the intended atlas rect and the plane
    // read its irradiance out of the roughness column instead. The azimuth is
    // arbitrary at the pole; pick zero, as the Vulkan chunk's atan already did.
    const float azimuth = (dir.x == 0.0 && dir.z == 0.0) ? 0.0 : atan2(dir.x, dir.z);
    const float2 sph = float2(azimuth, asin(clamp(dir.y, -1.0, 1.0)));
    const float2 uv = sph / float2(PI * 2.0, PI) + 0.5;
    return float2(uv.x, 1.0 - uv.y);
}

// Non-anisotropic sampler for env-atlas reads (skybox + all IBL paths).
// The default sampler runs at 16× anisotropy, which reads dfdx(U) at the
// equirectangular atan2 wrap (n.z<0, n.x≈0) — where U jumps ~1.0 inside a
// single quad — as a giant footprint and filters across unrelated atlas
// regions, producing a vertical dashed line anchored to world −Z. The
// pre-baked 1-pixel seam border (envLighting.cpp `seamPixels = 1`) only
// covers bilinear; it cannot compensate for an anisotropic kernel that may
// be tens of pixels wide. Anisotropy gives nothing useful here anyway —
// each atlas pixel maps to a unique world direction, so there is no
// oblique-angle minification to recover. Use plain trilinear instead.
constexpr sampler envAtlasSampler(
    coord::normalized,
    filter::linear,
    mip_filter::linear,
    address::clamp_to_edge);

// Reflection-probe cubemap sampler (trilinear across roughness mips).
constexpr sampler reflectionProbeSampler(
    filter::linear,
    mip_filter::linear,
    address::clamp_to_edge);

static inline float2 mapUv(float2 uv, float4 rect)
{
    return float2(mix(rect.x + ATLAS_SEAM, rect.x + rect.z - ATLAS_SEAM, uv.x),
                  mix(rect.y + ATLAS_SEAM, rect.y + rect.w - ATLAS_SEAM, uv.y));
}

static inline float2 mapRoughnessUv(float2 uv, float level)
{
    const float t = 1.0 / exp2(level);
    return mapUv(uv, float4(0.0, 1.0 - t, t, t * 0.5));
}

static inline float2 mapAmbientUv(float2 uv)
{
    return mapUv(uv, float4(128.0 / ATLAS_SIZE, (256.0 + 128.0) / ATLAS_SIZE,
                            64.0 / ATLAS_SIZE, 32.0 / ATLAS_SIZE));
}

static inline float2 mapShinyUv(float2 uv, float level)
{
    const float t = 1.0 / exp2(level);
    return mapUv(uv, float4(1.0 - t, 1.0 - t, t, t * 0.5));
}

static inline float3 processEnvironment(float3 color, float skyboxIntensity)
{
    return color * skyboxIntensity;
}

constexpr sampler grabPassSampler(filter::linear, mip_filter::linear, address::clamp_to_edge);

// LUT parameterized by sqrt(GGX alpha) x sqrt(1 - cos(view angle)).
