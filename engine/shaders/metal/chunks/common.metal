// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
#include <metal_stdlib>
using namespace metal;

struct VertexData {
    float3 position [[attribute(0)]];
    float3 normal   [[attribute(1)]];
    float2 uv0      [[attribute(2)]];
    float4 tangent  [[attribute(3)]];
    float2 uv1      [[attribute(4)]];
#if VT_FEATURE_VERTEX_COLORS
    float4 color    [[attribute(5)]];
#endif
#if VT_FEATURE_DYNAMIC_BATCH
    // DEVIATION: dynamic batching uses per-vertex bone index (mesh instance index)
    // into a matrix palette buffer at slot 6. Upstream uses a bone texture instead.
    // Mutually exclusive with VT_FEATURE_VERTEX_COLORS (both use attribute 5).
    float  boneIndex [[attribute(5)]];
#endif
#if VT_FEATURE_INSTANCING
    // per-instance model matrix as 4 column vectors + diffuse color.
    // Per-instance model matrix as 4 column vectors + diffuse color.
    // bufferIndex=5, stepFunction=perInstance via vertex descriptor.
    float4 instance_line1 [[attribute(6)]];   // model matrix column 0
    float4 instance_line2 [[attribute(7)]];   // model matrix column 1
    float4 instance_line3 [[attribute(8)]];   // model matrix column 2
    float4 instance_line4 [[attribute(9)]];   // model matrix column 3
    float4 instanceColor  [[attribute(10)]];  // sRGB diffuse color
#endif
};

struct RasterizerData {
    float4 position [[position]];
    float3 worldPos;
    float3 worldNormal;
    float4 worldTangent;
    float2 uv0;
    float2 uv1;
#if VT_FEATURE_VERTEX_COLORS
    float4 vertexColor;
#endif
#if VT_FEATURE_INSTANCING
    float4 instanceColor;
#endif
#if VT_FEATURE_POINT_SIZE
    float pointSize [[point_size]];
#endif
};

struct ModelData {
    float4x4 modelMatrix;
    float4x4 normalMatrix;
    float normalSign;
    float3 _pad;
};

struct SceneData {
    float4x4 projViewMatrix;
};

struct MaterialData {
    float4 baseColor;
    float4 emissiveColor;
    uint flags;
    uint occludeSpecularMode;
    float alphaCutoff;
    float metallicFactor;
    float roughnessFactor;
    float normalScale;
    float occlusionStrength;
    float occludeSpecularIntensity;
    // per-texture UV transforms as pre-computed 3×2 affine matrices.
    // Each pair encodes one row: uv' = float2(dot(float3(uv,1), row0.xyz), dot(float3(uv,1), row1.xyz))
    float4 baseColorTransform0;     // {cos(θ)*sx, -sin(θ)*sy, ox, 0}
    float4 baseColorTransform1;     // {sin(θ)*sx,  cos(θ)*sy, 1-sy-oy, 0}
    float4 normalTransform0;
    float4 normalTransform1;
    float4 metalRoughTransform0;
    float4 metalRoughTransform1;
    float4 occlusionTransform0;
    float4 occlusionTransform1;
    float4 emissiveTransform0;
    float4 emissiveTransform1;
    // clearcoat dual-layer material properties.
    float clearCoatFactor;       // 0 = disabled, 1 = full clearcoat
    float clearCoatRoughness;    // 0 = mirror, 1 = rough
    float clearCoatBumpiness;    // clearcoat normal map intensity
    float heightMapFactor;       // parallax height scale (0 = no parallax)

    float anisotropy;            // anisotropic specular: -1..1 (0 = isotropic)
    float transmissionFactor;    // 0 = opaque, 1 = fully transmissive
    float refractionIndex;       // IOR (1.0 = air, 1.5 = glass, 1.33 = water)
    float thickness;             // volume thickness for absorption scaling

    // Sheen (KHR_materials_sheen): fabric/velvet sheen layer.
    float4 sheenColor;               // rgb=sheen color, w=sheen roughness
    // Iridescence (KHR_materials_iridescence): thin-film interference.
    float4 iridescenceParams;        // x=intensity, y=IOR, z=thicknessMin(nm), w=thicknessMax(nm)
    // Spec-Gloss (KHR_materials_pbrSpecularGlossiness): alternative PBR.
    float4 specGlossParams;          // rgb=specular color, w=glossiness
    // Detail normals + displacement.
    float4 detailDisplacementParams; // x=detailNormalScale, y=displacementScale, z=displacementBias, w=pad
    // Detail normal UV transform (3×2 affine matrix as two float4 rows).
    float4 detailNormalTransform0;
    float4 detailNormalTransform1;
};

/// CPU-side packing layout for hardware instancing (VT_FEATURE_INSTANCING).
/// Each instance carries its own model transform and base color.
/// Data is fed to the vertex shader via vertex descriptor layout(5) with perInstance step function.
/// The shader reads it through [[stage_in]] attributes (instance_line1..4 + instanceColor).
/// Mirrors hardware instancing layout + per-instance color.
struct InstanceData {
    float4x4 modelMatrix;    // 64 bytes — world transform for this instance
    float4   diffuseColor;   // 16 bytes — sRGB base color (transfer-function mapped)
};  // 80 bytes total, 16-byte aligned

// Dynamic batch palette: bound at buffer(6) as `constant float4x4 *palette`.
// Each entry is a float4x4 world transform for one mesh instance in the batch.
// Uses a ring-buffer allocation — no fixed size limit.
// DEVIATION: uses a Metal buffer instead of an RGBA32F bone texture.

struct GpuLight {
    float4 positionRange;
    float4 directionCone;
    float4 colorIntensity;
    float4 coneAngles;
    uint4 typeCastShadows;
};

// Clustered lighting: per-light data packed into a Metal buffer (slot 7).
// 64 bytes per light, 16-byte aligned. Maps 1:1 to CPU GpuClusteredLight.
struct ClusteredLight {
    float4 positionRange;     // xyz=position, w=range
    float4 directionSpot;     // xyz=direction, w=outerConeCos
    float4 colorIntensity;    // xyz=color (linear), w=intensity
    float4 params;            // x=innerConeCos, y=isSpot, z=falloffLinear, w=unused
};

struct LightingData {
    float4 ambientColor;
    uint4 lightCountAndFlags;
    uint4 flagsAndPad;
    float4 cameraPositionSkyboxIntensity;
    float4 skyboxMipAndPad;
    GpuLight lights[8];
    float4 fogColorDensity;
    float4 fogStartEndType;
    float4 shadowBiasNormalStrength;
    // CSM: 4 cascade VP matrices (viewport-scaled). Replaces single shadowViewProj.
    // Each matrix bakes in projection, view, NDC-to-atlas-UV, Metal Y-flip, and Z [0,1] mapping.
    //_shadowMatrixPalette.
    float4x4 shadowMatrixPalette[4];
    // CSM: per-cascade split distances (view-space far distance per cascade).
    //_shadowCascadeDistances.
    float4 shadowCascadeDistances;
    // CSM: [x]=cascadeCount, [y]=cascadeBlend, [z]=pad, [w]=pad
    float4 shadowCascadeParams;
    // world-space dome center (tripod).
    // xyz = center position, w = 1.0 for dome/box, 0.0 for infinite.
    float4 skyDomeCenter;
    // DEVIATION: screen inverse resolution for planar reflection screen-space UV.
    // xy = 1/width, 1/height; zw = width, height.
    float4 screenInvResolution;
    // DEVIATION: blurred planar reflection parameters.
    // x: intensity (0..1), y: blurAmount (0..2), z: fadeStrength (0..5), w: angleFade (0..1)
    float4 reflectionParams;
    // rgb: fade color (linear space), w: unused.
    float4 reflectionFadeColor;
    // DEVIATION: planar reflection depth pass parameters.
    // x: planeDistance (world-space Y offset), y: heightRange (normalization), z/w: unused.
    float4 reflectionDepthParams;

    // Local light shadows (spot/point): up to 2 VP matrices + per-light params.
    // Matches MetalUniformBinder::LightingUniforms layout.
    float4x4 localShadowMatrix0;    // VP matrix for local shadow light 0
    float4x4 localShadowMatrix1;    // VP matrix for local shadow light 1
    // [x]=bias, [y]=normalBias, [z]=intensity, [w]=reserved
    float4 localShadowParams0;
    float4 localShadowParams1;

    // Omni cubemap shadow params: [x]=near, [y]=far, [z]=bias, [w]=normalBias
    float4 omniShadowParams0;
    // [x]=intensity, [y-w]=reserved
    float4 omniShadowParams0Extra;
    float4 omniShadowParams1;
    float4 omniShadowParams1Extra;

    // Clustered lighting grid parameters.
    // WorldClusters uniforms.
    float4 clusterBoundsMin;                // xyz=grid min corner, w=unused
    float4 clusterBoundsRange;              // xyz=grid size (max-min), w=unused
    float4 clusterCellsCountByBoundsSize;   // xyz=cells/range (for world→cell conversion), w=unused
    uint4 clusterParams;                    // x=cellsX, y=cellsY, z=cellsZ, w=maxLightsPerCell
    uint4 clusterParams2;                   // x=numClusteredLights, y-w=unused
};

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
    const float2 sph = float2(atan2(dir.x, dir.z), asin(clamp(dir.y, -1.0, 1.0)));
    const float2 uv = sph / float2(PI * 2.0, PI) + 0.5;
    return float2(uv.x, 1.0 - uv.y);
}

// NOTE: An earlier implementation carried an `envSeamSampler` (bilinear,
// non-anisotropic) + an `isAtEnvSeam` runtime detector for handling the
// equirectangular atan2 wrap at ±180°. Both have been removed in favour
// of the upstream upstream approach: the env atlas is baked with a
// 1-pixel duplicated seam border at every rect edge (see envLighting.cpp
// `seamPixels = 1.0f`), which lets the default anisotropic sampler
// produce continuous values across the wrap without any runtime
// branching. This eliminates the filter-mode-discontinuity artifact
// that appeared as two dashed vertical lines on the skybox at |n.x|=W.

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

static inline float3 toneMapLinear(float3 color, float exposure)
{
    return color * exposure;
}

// https://modelviewer.dev/examples/tone-mapping
static inline float3 toneMapNeutral(float3 color, float exposure)
{
    color *= exposure;

    const float startCompression = 0.8 - 0.04;
    const float desaturation = 0.15;

    const float x = min(color.r, min(color.g, color.b));
    const float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
    color -= offset;

    const float peak = max(color.r, max(color.g, color.b));
    if (peak < startCompression) return color;

    const float d = 1.0 - startCompression;
    const float newPeak = 1.0 - d * d / (peak + d - startCompression);
    color *= newPeak / peak;

    const float g = 1.0 - 1.0 / (desaturation * (peak - newPeak) + 1.0);
    return mix(color, float3(newPeak), g);
}

static inline float3 toneMapAces(float3 color, float exposure)
{
    const float tA = 2.51;
    const float tB = 0.03;
    const float tC = 2.43;
    const float tD = 0.59;
    const float tE = 0.14;
    color *= exposure;
    return (color * (tA * color + tB)) / (color * (tC * color + tD) + tE);
}

// ACES approximation by Stephen Hill — TONEMAP_ACES2 (used by upstream
// camera.toneMapping = TONEMAP_ACES2). Two-matrix fit with RRT+ODT polynomial.
// Higher dynamic range than the simpler Narkowicz fit in toneMapAces — bright
// HDR values (e.g. specular highlights) roll off smoothly past 1.0 instead of
// clipping, which is essential for parity with upstream published demos.
static inline float3 RRTAndODTFit(float3 v)
{
    const float3 a = v * (v + 0.0245786) - 0.000090537;
    const float3 b = v * (0.983729 * v + 0.4329510) + 0.238081;
    return a / b;
}

static inline float3 toneMapAces2(float3 color, float exposure)
{
    // sRGB → XYZ → D65_2_D60 → AP1 → RRT_SAT
    const float3x3 ACESInputMat = float3x3(
        float3(0.59719, 0.35458, 0.04823),
        float3(0.07600, 0.90834, 0.01566),
        float3(0.02840, 0.13383, 0.83777));
    // ODT_SAT → XYZ → D60_2_D65 → sRGB
    const float3x3 ACESOutputMat = float3x3(
        float3( 1.60475, -0.53108, -0.07367),
        float3(-0.10208,  1.10813, -0.00605),
        float3(-0.00327, -0.07276,  1.07602));

    color *= exposure / 0.6;
    color = color * ACESInputMat;
    color = RRTAndODTFit(color);
    color = color * ACESOutputMat;
    return clamp(color, float3(0.0), float3(1.0));
}

// dispatch tone mapping by mode.
// Mode is passed via skyboxMipAndPad.z and matches scene/constants.h:
//   0=linear, 1=filmic, 3=aces, 4=aces2, 5=neutral, 6=none.
static inline float3 toneMap(float3 color, float exposure, float mode)
{
    if (mode > 4.5 && mode < 5.5) {
        return toneMapNeutral(color, exposure);
    } else if (mode > 3.5 && mode < 4.5) {
        return toneMapAces2(color, exposure);
    } else if (mode > 2.5 && mode < 3.5) {
        return toneMapAces(color, exposure);
    } else if (mode > 5.5) {
        return color; // TONEMAP_NONE
    }
    return toneMapLinear(color, exposure);
}

// GLSL parity (falloffLinear / falloffInvSquared / spotPS).
static inline float getFalloffWindow(float lightRadius, float3 lightDir)
{
    const float sqrDist = dot(lightDir, lightDir);
    const float invRadius = 1.0 / max(lightRadius, 1e-4);
    return square(saturate(1.0 - square(sqrDist * square(invRadius))));
}

static inline float getFalloffLinear(float lightRadius, float3 lightDir)
{
    const float d = length(lightDir);
    return max((lightRadius - d) / max(lightRadius, 1e-4), 0.0);
}

static inline float getFalloffInvSquared(float lightRadius, float3 lightDir)
{
    const float sqrDist = dot(lightDir, lightDir);
    float falloff = 1.0 / (sqrDist + 1.0);
    const float invRadius = 1.0 / max(lightRadius, 1e-4);

    falloff *= 16.0;
    falloff *= square(saturate(1.0 - square(sqrDist * square(invRadius))));
    return falloff;
}

static inline float getSpotEffect(float3 lightSpotDir, float lightInnerConeAngle, float lightOuterConeAngle, float3 lightDirNorm)
{
    const float cosAngle = dot(lightDirNorm, lightSpotDir);
    return smoothstep(lightOuterConeAngle, lightInnerConeAngle, cosAngle);
}

// Cascade split distances mark the far boundary of each cascade slice.
// step(distances, depth) produces 1.0 for each cascade whose far boundary is <= depth,
// and dot(comparisons, vec4(1)) counts how many cascades the fragment is past.
static inline int getShadowCascadeIndex(float4 distances, int count, float depth) {
    const float4 comparisons = step(distances, float4(depth));
    return min(int(dot(comparisons, float4(1.0))), count - 1);
}

// Uses 4 hardware comparison samples with bilinear interpolation to cover a 3×3 texel region.
// Each sample_compare with filter::linear performs a 2×2 PCF automatically; carefully chosen
// offsets and weights combine four such lookups into a full 3×3 kernel (4 taps, not 9).
//_getShadowPCF3x3().
static inline float getShadowPCF3x3(depth2d<float> shadowMap, float2 shadowUv, float depth, float resolution) {
    constexpr sampler shadowCompSampler(coord::normalized, filter::linear,
                                        compare_func::less_equal, address::clamp_to_edge);

    const float z = depth;
    const float2 uv = shadowUv * resolution;         // UV → texel space
    const float shadowMapSizeInv = 1.0 / resolution;
    const float2 base_uv_full = floor(uv + 0.5);
    const float s = (uv.x + 0.5 - base_uv_full.x);
    const float t = (uv.y + 0.5 - base_uv_full.y);
    const float2 base_uv = (base_uv_full - float2(0.5)) * shadowMapSizeInv;

    const float uw0 = (3.0 - 2.0 * s);
    const float uw1 = (1.0 + 2.0 * s);

    const float u0 = ((2.0 - s) / uw0 - 1.0) * shadowMapSizeInv + base_uv.x;
    const float u1 = (s / uw1 + 1.0) * shadowMapSizeInv + base_uv.x;

    const float vw0 = (3.0 - 2.0 * t);
    const float vw1 = (1.0 + 2.0 * t);

    const float v0 = ((2.0 - t) / vw0 - 1.0) * shadowMapSizeInv + base_uv.y;
    const float v1 = (t / vw1 + 1.0) * shadowMapSizeInv + base_uv.y;

    float sum = 0.0;
    sum += uw0 * vw0 * shadowMap.sample_compare(shadowCompSampler, float2(u0, v0), z);
    sum += uw1 * vw0 * shadowMap.sample_compare(shadowCompSampler, float2(u1, v0), z);
    sum += uw0 * vw1 * shadowMap.sample_compare(shadowCompSampler, float2(u0, v1), z);
    sum += uw1 * vw1 * shadowMap.sample_compare(shadowCompSampler, float2(u1, v1), z);

    return sum * (1.0 / 16.0);
}

// ---------------------------------------------------------------------------
// EVSM (Exponential Variance Shadow Maps), 16-bit float storage.
// Match upstream SHADOW_VSM_16F (shadowEVSM.js + blurVSM.js).
//
// Storage convention (set by shadow-fragment.metal):
//   moments.x = exp(c · z),   moments.y = exp(c · z)²,   moments.z = 1.0 (rendered flag),
//   moments.w = 1.0 (unused)
// Cleared pixels are (0,0,0,0); the (1 - moments.z) fallback path in calculateEVSM
// then synthesizes "fully lit" for any sample that landed outside the rendered region.
//
// `c` is VSM_EXPONENT. The actual 16F storage constraint is on the SECOND
// moment (exp(c·z)²), not the first — so we need exp(2c) < 65504, giving
// c < ln(65504)/2 ≈ 5.54. Going higher (e.g. 8 or 11) overflows the second
// moment to fp16 ∞, which explodes the variance reconstruction and
// produces severe light bleeding (the opposite of the intended effect).
// 5.54 is also upstream default for SHADOW_VSM_16F.
// ---------------------------------------------------------------------------
constant float VSM_EXPONENT = 5.54;

static inline float linstepSat(float a, float b, float v)
{
    return saturate((v - a) / (b - a));
}

// Trim a [0, amount] tail off the upper-bound probability and rescale (amount, 1] → [0, 1].
// Combats VSM's "light bleeding" by clipping low-confidence shadow samples to 0.
static inline float reduceLightBleeding(float pMax, float amount)
{
    return linstepSat(amount, 1.0, pMax);
}

// One-tailed Chebyshev upper bound on P(receiver is lit) given filtered (M1, M2)
// moments and the receiver's depth. Returns 1 when receiver is in front of the
// occluder mean (i.e. unambiguously lit), else the variance-based upper bound.
static inline float chebyshevUpperBound(float2 moments, float mean, float minVariance,
                                        float lightBleedingReduction)
{
    float variance = moments.y - moments.x * moments.x;
    variance = max(variance, minVariance);

    const float d = mean - moments.x;
    float pMax = variance / (variance + d * d);

    pMax = reduceLightBleeding(pMax, lightBleedingReduction);

    return (mean <= moments.x) ? 1.0 : pMax;
}

// Apply EVSM warp to receiver depth, combine with stored moments (or fall back to
// "lit" for cleared pixels), then run Chebyshev. `Z` is the receiver's NDC depth ∈ [0,1].
static inline float calculateEVSM(float3 moments, float Z, float vsmBias, float exponent)
{
    Z = 2.0 * Z - 1.0;                          // [0,1] → [-1,1]
    const float warpedDepth = exp(exponent * Z);

    // Cleared (unrendered) pixels have moments.z == 0 → synthesize fully-lit moments
    // by injecting (warpedDepth, warpedDepth²); rendered pixels have moments.z == 1
    // → use stored moments unchanged.
    const float2 stored = moments.xy + float2(warpedDepth, warpedDepth * warpedDepth) * (1.0 - moments.z);

    const float depthScale = vsmBias * exponent * warpedDepth;
    const float minVariance = depthScale * depthScale;
    // 0.1 = upstream default light-bleeding reduction. With c at the proper
    // 16F-safe value, the Chebyshev probability is well-conditioned and 0.1
    // is enough to clip residual bleeding without darkening contact shadows.
    return chebyshevUpperBound(stored, warpedDepth, minVariance, 0.1);
}

// Public entry: sample the moments texture and return shadow visibility ∈ [0, 1].
static inline float getShadowVSM16(texture2d<float> momentsTex, float2 shadowUv, float receiverDepth, float vsmBias)
{
    constexpr sampler vsmSampler(coord::normalized, filter::linear, address::clamp_to_edge);
    const float3 moments = momentsTex.sample(vsmSampler, shadowUv).xyz;
    return calculateEVSM(moments, receiverDepth, vsmBias, VSM_EXPONENT);
}

// The glossSq term scales F90 (grazing-angle reflectance) by roughness, preventing
// rough surfaces from showing excessive Fresnel at grazing angles.
static inline float3 getFresnel(float cosTheta, float gloss, float3 specularity)
{
    const float fresnel = pow(1.0 - saturate(cosTheta), 5.0);
    const float glossSq = gloss * gloss;
    const float specIntensity = max(specularity.r, max(specularity.g, specularity.b));
    return specularity + (max(float3(glossSq * specIntensity), specularity) - specularity) * fresnel;
}

// Fixed F0 = 0.04 (IOR ≈ 1.5, typical for clear coatings like polyurethane/lacquer).
static inline float getFresnelCC(float cosTheta)
{
    return 0.04 + 0.96 * pow(1.0 - saturate(cosTheta), 5.0);
}

// Kelemen visibility term for clearcoat — simpler than Smith-GGX since clearcoat
// is typically smooth. V = 0.25 / (LdotH^2). Used by Filament and upstream.
static inline float getVisibilityKelemen(float LdotH)
{
    return 0.25 / max(LdotH * LdotH, 1e-5);
}

// ---------------------------------------------------------------------------
// Sheen BRDF (Charlie distribution + Ashikhmin visibility)
// Based on Estevez & Kulla 2017, "Production Friendly Microfacet Sheen BRDF".
// ---------------------------------------------------------------------------

// Charlie sheen distribution — inverted Gaussian model for fabric BRDF.
// Produces softer, wider lobes than GGX, suitable for velvet/fabric.
static inline float sheenDistribution(float NoH, float roughness)
{
    const float invR = 1.0 / max(roughness, 0.001);
    const float cos2h = NoH * NoH;
    const float sin2h = max(1.0 - cos2h, 0.0078125);  // prevent pow(0, x)
    return (2.0 + invR) * pow(sin2h, invR * 0.5) / (2.0 * PI);
}

// Ashikhmin visibility for sheen — energy-conserving geometric term for fabric.
static inline float sheenVisibility(float NoV, float NoL)
{
    return 1.0 / (4.0 * (NoL + NoV - NoL * NoV));
}

// Analytical sheen IBL approximation.
// Piecewise exp(a * NoV + b) fit that avoids a precomputed LUT texture.
// Returns the directional albedo E(NoV, roughness) for sheen.
static inline float sheenIBLApprox(float NoV, float roughness)
{
    const float r2 = roughness * roughness;
    float a, b;
    if (NoV < 0.5) {
        a = -0.0275 - 0.2159 * roughness + 2.3519 * r2 - 0.3414 * r2 * roughness;
        b =  0.1003 + 1.3324 * roughness - 1.4688 * r2 - 0.1819 * r2 * roughness;
    } else {
        a = -0.5765 - 0.1299 * roughness + 0.5271 * r2 + 0.7462 * r2 * roughness;
        b =  0.1835 - 0.2939 * roughness + 0.2079 * r2 - 0.3522 * r2 * roughness;
    }
    return saturate(exp(a * NoV + b));
}

// ---------------------------------------------------------------------------
// Iridescence — thin-film interference (KHR_materials_iridescence)
// Based on Laurent Belcour & Pascal Barla 2017.
// Computes wavelength-dependent Fresnel at two interfaces (air→film, film→substrate)
// with optical path difference producing rainbow-shift colors.
// ---------------------------------------------------------------------------

// IOR to Fresnel F0 conversion (Schlick approximation inverse).
static inline float iorToF0(float transmittedIor, float incidentIor)
{
    const float r = (transmittedIor - incidentIor) / (transmittedIor + incidentIor);
    return r * r;
}

static inline float3 iorToF0Vec(float3 transmittedIor, float incidentIor)
{
    const float3 r = (transmittedIor - incidentIor) / (transmittedIor + incidentIor);
    return r * r;
}

// Fresnel F0 to IOR inversion.
static inline float3 f0ToIor(float3 f0)
{
    const float3 s = sqrt(f0);
    return (1.0 + s) / (1.0 - s);
}

// Schlick Fresnel for iridescence interfaces.
static inline float iridescenceFresnel(float cosTheta, float f0)
{
    const float x = saturate(1.0 - cosTheta);
    const float x2 = x * x;
    return f0 + (1.0 - f0) * (x2 * x2 * x);
}

static inline float3 iridescenceFresnelVec(float cosTheta, float3 f0)
{
    const float x = saturate(1.0 - cosTheta);
    const float x2 = x * x;
    return f0 + (1.0 - f0) * (x2 * x2 * x);
}

// Spectral sensitivity function: converts optical path difference (OPD) to visible color.
// Uses Gaussian basis functions in CIE XYZ color space, then converts to sRGB (Rec.709).
static inline float3 iridescenceSensitivity(float opd, float3 shift)
{
    const float phase = 2.0 * PI * opd * 1.0e-9;

    // CIE XYZ Gaussian basis amplitudes, centers, and variances.
    const float3 val = float3(5.4856e-13, 4.4201e-13, 5.2481e-13);
    const float3 pos = float3(1.6810e+06, 1.7953e+06, 2.2084e+06);
    const float3 var = float3(4.3278e+09, 9.3046e+09, 6.6121e+09);

    float3 xyz = val * sqrt(2.0 * PI * var) * cos(pos * phase + shift)
                 * exp(-(phase * phase) * var);

    // Fourth Gaussian for the X tristimulus (red sensitivity has a bimodal peak).
    xyz.x += 9.7470e-14 * sqrt(2.0 * PI * 4.5282e+09)
           * cos(2.2399e+06 * phase + shift.x)
           * exp(-4.5282e+09 * phase * phase);

    // Normalize to unit peak.
    xyz /= float3(1.0685e-07);

    // CIE XYZ → sRGB (Rec.709) matrix.
    const float3x3 XYZ_TO_REC709 = float3x3(
        float3( 3.2404542, -1.5371385, -0.4985314),
        float3(-0.9692660,  1.8760108,  0.0415560),
        float3( 0.0556434, -0.2040259,  1.0572252)
    );

    return XYZ_TO_REC709 * xyz;
}

// Compute thin-film iridescence Fresnel replacement.
// outsideIor: IOR of surrounding medium (1.0 for air).
// cosTheta:   dot(V, N) at the surface.
// baseF0:     base material F0 specularity (substrate).
// thickness:  thin-film thickness in nanometers.
// filmIor:    IOR of the thin film.
static inline float3 calcIridescence(float outsideIor, float cosTheta, float3 baseF0,
                                      float thickness, float filmIor)
{
    // Smooth transition: for very thin films, revert to base outside IOR.
    const float iridIor = mix(outsideIor, filmIor, smoothstep(0.0, 0.03, thickness));

    // Snell's law: sin²θ₂ = (n₁/n₂)² * sin²θ₁
    const float sinTheta2Sq = (outsideIor / iridIor) * (outsideIor / iridIor)
                              * (1.0 - cosTheta * cosTheta);
    const float cosTheta2Sq = 1.0 - sinTheta2Sq;

    // Total internal reflection — return full reflection.
    if (cosTheta2Sq < 0.0) {
        return float3(1.0);
    }

    const float cosTheta2 = sqrt(cosTheta2Sq);

    // Interface 1: outside → thin film.
    const float r0 = iorToF0(iridIor, outsideIor);
    const float r12 = iridescenceFresnel(cosTheta, r0);
    const float t121 = 1.0 - r12;  // transmission through interface 1 (both ways)

    // Phase shift at interface 1 (π if film IOR < outside IOR).
    const float phi12 = (iridIor < outsideIor) ? PI : 0.0;
    const float phi21 = PI - phi12;

    // Interface 2: thin film → substrate.
    // Convert base F0 to IOR per channel for the film→substrate interface.
    const float3 baseIor = f0ToIor(baseF0 + float3(0.0001));
    const float3 r1 = iorToF0Vec(baseIor, iridIor);
    const float3 r23 = iridescenceFresnelVec(cosTheta2, r1);

    // Phase shift at interface 2 (π per channel if substrate IOR < film IOR).
    float3 phi23 = float3(0.0);
    if (baseIor.x < iridIor) phi23.x = PI;
    if (baseIor.y < iridIor) phi23.y = PI;
    if (baseIor.z < iridIor) phi23.z = PI;

    // Optical path difference (in nanometers).
    const float opd = 2.0 * iridIor * thickness * cosTheta2;

    // Total phase shift.
    const float3 phi = float3(phi21) + phi23;

    // Multi-bounce Airy summation (2 orders).
    const float3 r123Sq = clamp(float3(r12) * r23, float3(1e-5), float3(0.9999));
    const float3 r123 = sqrt(r123Sq);
    const float3 rs = (t121 * t121) * r23 / (1.0 - r123Sq);

    // Order 0: direct reflection + transmitted-reflected.
    float3 i = float3(r12) + rs;

    // Orders 1-2: interference fringes via spectral sensitivity.
    float3 cm = rs - t121;
    for (int m = 1; m <= 2; m++) {
        cm *= r123;
        const float3 sm = 2.0 * iridescenceSensitivity(float(m) * opd, float(m) * phi);
        i += cm * sm;
    }

    return max(i, float3(0.0));
}

// Convenience wrapper: compute iridescence Fresnel for air (IOR=1.0) → thin film → substrate.
static inline float3 getIridescence(float cosTheta, float3 specularity,
                                     float thickness, float filmIor)
{
    return calcIridescence(1.0, cosTheta, specularity, thickness, filmIor);
}

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
static inline float2 parallaxOcclusionMap(float2 uv, float3 viewDirTS,
                                           texture2d<float> heightMap, sampler s,
                                           float heightScale) {
    // Adaptive step count: more steps at grazing angles where parallax is most visible.
    const int minSteps = 8;
    const int maxSteps = 32;
    const int numSteps = int(mix(float(maxSteps), float(minSteps), abs(viewDirTS.z)));
    const float layerDepth = 1.0 / float(numSteps);

    // UV step per layer along view direction projected onto surface.
    const float2 deltaUV = viewDirTS.xy * heightScale / (abs(viewDirTS.z) + 1e-5) / float(numSteps);

    float2 curUV = uv;
    float curLayerDepth = 0.0;
    float curHeight = 1.0 - heightMap.sample(s, curUV).r;

    // Step through layers until we go below the surface.
    for (int i = 0; i < maxSteps; ++i) {
        if (curLayerDepth >= curHeight) break;
        curUV -= deltaUV;
        curHeight = 1.0 - heightMap.sample(s, curUV).r;
        curLayerDepth += layerDepth;
    }

    // Linear interpolation between last two layers for smooth result.
    const float2 prevUV = curUV + deltaUV;
    const float afterDepth = curHeight - curLayerDepth;
    const float beforeDepth = (1.0 - heightMap.sample(s, prevUV).r) - (curLayerDepth - layerDepth);
    const float weight = afterDepth / (afterDepth - beforeDepth + 1e-6);
    return mix(curUV, prevUV, weight);
}
