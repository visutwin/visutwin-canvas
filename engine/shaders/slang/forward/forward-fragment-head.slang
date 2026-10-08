// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 07.10.2026
//
// The forward program's declarations: the per-draw material and per-pass lighting blocks,
// every texture and sampler the fragment stage reads, the cluster buffers, and the helpers
// the chunks share. Composed first (programs/forward.slang); every later chunk reads these
// globals by name.
//
// Bindings carry both backends' slots: register() is the Metal one, [[vk::binding]] the
// Vulkan one (see the binding table, platform/graphics/shaderBindings.h). Where the two
// backends read a map through different TYPES the declaration is per target
// (VT_TARGET_METAL): a shadow map is a depth texture read through the hardware comparison
// sampler on Metal, and a plain image whose comparison the shader does by hand on Vulkan,
// whose fragment stage has no comparison sampler to spare under MoltenVK's 16-sampler limit.
#include "bindings.slang"

VT_DECLARE_MATERIAL_DATA

// ---- Per-pass lighting block (LightingBlock in platform/graphics/lightingBlock.h, 2896
// bytes: Metal buffer 4, Vulkan set 2 binding 0). Field for field the C++ layout. ----
struct Light
{
    float4 positionRange;      // xyz position, w range
    float4 directionType;      // xyz direction, w type (0 dir, 1 point, 2 spot)
    float4 colorIntensity;     // rgb colour, w intensity
    float4 coneParams;         // innerCos, outerCos, falloffLinear, shadow slot (-1 none)
    float4 areaRightHalfWidth;
    float4 areaUpHalfHeight;
    float4 cookieFlags;        // hasCookie, cookie slot, CookieChannel, cookieFalloff
};

struct LightingData
{
    float4 ambient;
    float4 cameraPosExposure;          // xyz camera position, w exposure
    uint4 lightCount;                  // x active light count
    Light lights[8];
    float4 fogColorDensity;
    float4 fogStartEndType;            // start, end, type (0 off, 1 linear, 2 exp, 3 exp2)
    float4 envParams;                  // skyboxIntensity, hasEnvAtlas, encoding, skyboxMip
    float4x4 shadowMatrices[4];        // per cascade: world -> shadow-atlas UV + depth
    float4 shadowCascadeDistances;
    float4 shadowParams;               // enabled, numCascades, depthBias, strength
    float4 shadowParams2;              // normalBias, cascadeBlend, toneMapping, enableNormalMaps
    float4 pcssParams;                 // filterSamples, blockerSamples, penumbraSize, penumbraFalloff
    float4 pcssCascadeRadii;
    float4 pcssCascadeDepthRanges;
    float4x4 localShadowMatrix0;
    float4x4 localShadowMatrix1;
    float4 localShadowParams0;         // depthBias, normalBias, intensity, isOmni
    float4 localShadowParams1;
    float4 omniShadowParams0;          // near, far, depthBias, intensity
    float4 omniShadowParams1;
    float4 localShadowPcss0;           // searchArea UV (0 off), near, far, VSM slot
    float4 localShadowPcss1;
    float4x4 cookieMatrix2D0;
    float4x4 cookieMatrix2D1;
    float4x4 cookieMatrixCube0;
    float4x4 cookieMatrixCube1;
    float4 cookieParams2D0;            // intensity, cookieFalloff, CookieChannel
    float4 cookieParams2D1;
    float4 cookieParamsCube0;
    float4 cookieParamsCube1;
    float4 skyParams2;                 // xyz sky dome centre, w flags (bit 0 cubemap, bit 1 dome)
    float4 ambientSH[9];
    float4 clusterBoundsMin;
    float4 clusterBoundsRange;
    float4 clusterCellsCountByBoundsSize;
    uint4 clusterParams;
    uint4 clusterParams2;              // x clustered light count, y the light mask bit accepted
    float4 reflectionProbeBoxMin;
    float4 reflectionProbeBoxMax;
    float4 reflectionProbePosition;
    float4 reflectionProbeParams;
    float4x4 viewProjection;           // world -> clip, for the SSR march and refraction
    float4 cameraNearFar;              // near, far, colour grab bound, depth grab bound
    float4 atmoPlanetCenterAndRadius;
    float4 atmoRadiusAndSunIntensity;
    float4 atmoRayleighCoeffAndScale;
    float4 atmoMieCoeffAndScale;
    float4 atmoSunDirection;
    float4 atmoCameraAltitudeAndParams;
    float4 screenInvResolution;        // xy 1 / viewport, zw viewport
    float4 reflectionParams;           // intensity, blur, fadeStrength, angleFade
    float4 reflectionFadeColor;
    float4 reflectionDepthParams;      // planeDistance, heightRange, colour bound, depth bound
    uint4 flagsAndPad;                 // [0] frame flags (bit 5 linear HDR target), [1] debug pass
    float4x4 dirShadow1Matrices[4];
    float4 dirShadow1CascadeDistances;
    float4 dirShadow1Params;
    float4 dirShadow1Params2;
    float4 dirShadow1PcssParams;
    float4 dirShadow1PcssCascadeRadii;
    float4 dirShadow1PcssCascadeDepthRanges;
    float4 skyboxRotation[3];          // columns of Scene::skyboxRotation; [0].w flags one at all
    float4 cookieTransform2D[2];       // spot cookie 2x2 per slot, columns xy, zw
    float4 ditherJitter;
};

[[vk::binding(0, 2)]] ConstantBuffer<LightingData> lighting : register(b4);

// ---- Material maps (Vulkan set 1). The six combined ones carry their own sampler (Metal
// samplers 1-6); the rest are images read through the shared material sampler. ----
VT_DECLARE_baseColorMap
VT_DECLARE_normalMap
VT_DECLARE_metalRoughMap
VT_DECLARE_occlusionMap
VT_DECLARE_emissiveMap
VT_DECLARE_lightMap
VT_DECLARE_heightMap
VT_DECLARE_detailNormalMap
VT_DECLARE_materialSampler
VT_DECLARE_clearCoatMap
VT_DECLARE_clearCoatGlossMap
VT_DECLARE_clearCoatNormalMap
VT_DECLARE_glossMap
VT_DECLARE_thicknessMap
VT_DECLARE_refractionMap
VT_DECLARE_opacityMap

// ---- Scene samplers. Vulkan set 3 bindings 12 / 13; on Metal the forward draw binds the
// same states at fragment samplers 7 / 8 (MetalGraphicsDevice::bindDrawSampler), and the
// comparison sampler of the hardware shadow compare at 9. ----
[[vk::binding(12, 3)]] SamplerState linearClampSampler : register(s7);    // linear, linear mips, clamp
[[vk::binding(13, 3)]] SamplerState nearestClampSampler : register(s8);   // nearest, no mips, clamp
#if defined(VT_TARGET_METAL)
[[vk::binding(62, 3)]] SamplerComparisonState shadowCompareSampler : register(s9);   // linear, less-equal, clamp
#endif

// ---- Scene textures (Vulkan set 3). ----
// On Metal the env atlas has a sampler slot of its own (10), bound to the linear-clamp state.
[[vk::binding(0, 3)]] Sampler2D envAtlas : register(t2) : register(s10);
VT_DECLARE_skyboxCube
VT_DECLARE_reflectionProbeCube
VT_DECLARE_areaLightLut1
VT_DECLARE_areaLightLut2
VT_DECLARE_sceneColorGrab
VT_DECLARE_sceneDepthGrab
VT_DECLARE_planarReflection
VT_DECLARE_planarReflectionDepth
VT_DECLARE_cookie2D0
VT_DECLARE_cookie2D1
VT_DECLARE_cookieCube0
VT_DECLARE_cookieCube1
VT_DECLARE_ssao
VT_DECLARE_clusterCookieAtlas
#if defined(VT_TARGET_METAL)
// Depth maps for the hardware compare, and the EVSM moments of a VSM light in slots of their
// own (a specialised library cannot give one slot two types).
// The Vulkan bindings named here are those maps' bindings on Vulkan, or unused numbers for the
// Metal-only maps; nothing compiled for Vulkan declares these.
[[vk::binding(1, 3)]] DepthTexture2D shadowMap0 : register(t6);
[[vk::binding(22, 3)]] DepthTexture2D shadowMap1 : register(t35);
[[vk::binding(64, 3)]] Texture2D shadowMoments0 : register(t39);
[[vk::binding(65, 3)]] Texture2D shadowMoments1 : register(t40);
[[vk::binding(2, 3)]] DepthTexture2D localShadowMap0 : register(t11);
[[vk::binding(3, 3)]] DepthTexture2D localShadowMap1 : register(t12);
[[vk::binding(66, 3)]] Texture2D localVsmMap0 : register(t37);
[[vk::binding(67, 3)]] Texture2D localVsmMap1 : register(t38);
[[vk::binding(4, 3)]] DepthTextureCube omniShadowCube0 : register(t15);
[[vk::binding(5, 3)]] DepthTextureCube omniShadowCube1 : register(t16);
[[vk::binding(14, 3)]] DepthTexture2D clusterShadowAtlas : register(t26);
#else
VT_DECLARE_shadowMap0
VT_DECLARE_shadowMap1
[[vk::binding(2, 3)]] Sampler2D localShadowMap0;
[[vk::binding(3, 3)]] Sampler2D localShadowMap1;
[[vk::binding(4, 3)]] SamplerCube omniShadowCube0;
[[vk::binding(5, 3)]] SamplerCube omniShadowCube1;
VT_DECLARE_clusterShadowAtlas
#endif

// ---- Shadow-map access that differs per target. Metal reads depth maps as depth textures:
// a compare tap is the hardware comparison (bilinear: four texels compared and blended), a
// raw read the point-sampled depth. Vulkan reads them as plain images and emulates the
// compare tap by gathering the four texels (shadowTap in common-parallax), with the same
// result to rounding. A local light's VSM moments are a separate map on Metal (37 / 38)
// and the same binding as its depth map on Vulkan. ----
#if defined(VT_TARGET_METAL)
typealias LocalShadowMap = DepthTexture2D;
typealias OmniShadowMap = DepthTextureCube;
float shadowDepthRead(DepthTexture2D map, float2 uv) { return map.SampleLevel(nearestClampSampler, uv, 0.0); }
float shadowDepthRead(DepthTextureCube map, float3 dir) { return map.SampleLevel(nearestClampSampler, dir, 0.0); }
float shadowCompare(DepthTexture2D map, float2 uv, float receiver) { return map.SampleCmpLevelZero(shadowCompareSampler, uv, receiver); }
float shadowCompare(DepthTextureCube map, float3 dir, float receiver) { return map.SampleCmpLevelZero(shadowCompareSampler, dir, receiver); }
int2 vtTextureSize(DepthTexture2D t, int lod) { uint w, h, levels; t.GetDimensions(uint(lod), w, h, levels); return int2(int(w), int(h)); }
float shadowDepthReadDirectional(DepthTexture2D map, float2 uv) { return shadowDepthRead(map, uv); }
float4 localShadowMoments(int slot, float2 uv)
{
    return slot == 0 ? localVsmMap0.SampleLevel(linearClampSampler, uv, 0.0)
                     : localVsmMap1.SampleLevel(linearClampSampler, uv, 0.0);
}
#else
typealias LocalShadowMap = Sampler2D;
typealias OmniShadowMap = SamplerCube;
float shadowDepthRead(Sampler2D map, float2 uv) { return map.SampleLevel(uv, 0.0).r; }
float shadowDepthRead(SamplerCube map, float3 dir) { return map.SampleLevel(dir, 0.0).r; }
float shadowDepthReadDirectional(Texture2D map, float2 uv) { return map.SampleLevel(nearestClampSampler, uv, 0.0).r; }
float4 localShadowMoments(int slot, float2 uv)
{
    return slot == 0 ? localShadowMap0.SampleLevel(uv, 0.0) : localShadowMap1.SampleLevel(uv, 0.0);
}
#endif

// ---- Clustered lights (Vulkan set 5; Metal fragment buffers 7 / 8). ----
struct ClusterLight
{
    float4 positionRange;
    float4 directionSpot;
    float4 colorIntensity;
    float4 params;
    float4x4 shadowMatrix;
    float4 shadowData;
    float4 areaHalfWidth;      // area light: xyz world half-width axis, w LightShape (0 punctual)
    float4 areaHalfHeight;     // xyz world half-height axis, w the light's mask bits
};
[[vk::binding(0, 5)]] StructuredBuffer<ClusterLight> clusterLights : register(t7, space1);
// A cell holds light indices + 1, one byte each: Metal uploads the bytes as they are, Vulkan
// widens them to 32 bits on the CPU (VulkanGraphicsDevice::setClusterBuffers).
#if defined(VT_TARGET_METAL)
[[vk::binding(1, 5)]] StructuredBuffer<uint8_t> clusterCells : register(t8, space1);
#else
[[vk::binding(1, 5)]] StructuredBuffer<uint> clusterCells : register(t8, space1);
#endif

static const float PI = 3.14159265359;

// GLSL's gl_FragCoord and gl_FrontFacing, which helper functions read: set by the fragment
// entry before anything else runs.
static float4 vtFragCoord;
static bool vtFrontFacing;

// GLSL's mod, which floors (Slang's fmod truncates toward zero).
float glslMod(float x, float y) { return x - y * floor(x / y); }
float2 glslMod(float2 x, float2 y) { return x - y * floor(x / y); }
float2 glslMod(float2 x, float y) { return x - y * floor(x / y); }
float3 glslMod(float3 x, float3 y) { return x - y * floor(x / y); }
float3 glslMod(float3 x, float y) { return x - y * floor(x / y); }
float4 glslMod(float4 x, float4 y) { return x - y * floor(x / y); }
float4 glslMod(float4 x, float y) { return x - y * floor(x / y); }

// textureSize / textureQueryLevels.
int2 vtTextureSize(Texture2D t, int lod) { uint w, h, levels; t.GetDimensions(uint(lod), w, h, levels); return int2(int(w), int(h)); }
int2 vtTextureSize(Sampler2D t, int lod) { uint w, h, levels; t.GetDimensions(uint(lod), w, h, levels); return int2(int(w), int(h)); }
int vtTextureLevels(Texture2D t) { uint w, h, levels; t.GetDimensions(0, w, h, levels); return int(levels); }
int vtTextureLevels(Sampler2D t) { uint w, h, levels; t.GetDimensions(0, w, h, levels); return int(levels); }
int vtTextureLevels(TextureCube t) { uint w, h, levels; t.GetDimensions(0, w, h, levels); return int(levels); }

// The direction an environment sample (sky, env atlas, SH) reads along under
// Scene::skyboxRotation. Column 0's w flags a rotation at all.
float3 cubeMapRotate(float3 dir)
{
    if (lighting.skyboxRotation[0].w < 0.5) {
        return dir;
    }
    return mul(dir, float3x3(lighting.skyboxRotation[0].xyz, lighting.skyboxRotation[1].xyz,
                             lighting.skyboxRotation[2].xyz));
}

// Debug shader passes (scene/constants.h DebugShaderPass), mode in flagsAndPad[1].
static const uint VT_DEBUGPASS_NONE        = 0u;
static const uint VT_DEBUGPASS_ALBEDO      = 1u;
static const uint VT_DEBUGPASS_WORLDNORMAL = 2u;
static const uint VT_DEBUGPASS_OPACITY     = 3u;
static const uint VT_DEBUGPASS_SPECULARITY = 4u;
static const uint VT_DEBUGPASS_GLOSS       = 5u;
static const uint VT_DEBUGPASS_METALNESS   = 6u;
static const uint VT_DEBUGPASS_AO          = 7u;
static const uint VT_DEBUGPASS_EMISSION    = 8u;
static const uint VT_DEBUGPASS_LIGHTING    = 9u;
static const uint VT_DEBUGPASS_UV0         = 10u;

// ---- The varyings every forward vertex entry writes (locations 0-6). modelScale is the
// model's per-axis world scale, which dynamic refraction scales its offset by: the vertex
// stage computes it once, flat. ----
struct ForwardVarying
{
    float4 position : SV_Position;
    [[vk::location(0)]] float3 worldPos : TEXCOORD0;
    [[vk::location(1)]] float3 worldNormal : TEXCOORD1;
    [[vk::location(2)]] float2 uv0 : TEXCOORD2;
    [[vk::location(3)]] float2 uv1 : TEXCOORD3;
    [[vk::location(4)]] float4 worldTangent : TEXCOORD4;
    [[vk::location(5)]] float viewDepth : TEXCOORD5;
    [[vk::location(6)]] float4 color : COLOR0;
    [[vk::location(7)]] nointerpolation float3 modelScale : TEXCOORD6;
    // An instanced draw's per-instance base colour (sRGB), which replaces the material's under
    // VT_FEATURE_INSTANCING_COLOR; the material's own base colour everywhere else.
    [[vk::location(8)]] nointerpolation float4 instanceColor : TEXCOORD7;
};
