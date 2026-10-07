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
    // Per-instance model matrix as 4 column vectors, optionally followed by a diffuse color.
    // bufferIndex=5, stepFunction=perInstance via vertex descriptor.
    float4 instance_line1 [[attribute(6)]];   // model matrix column 0
    float4 instance_line2 [[attribute(7)]];   // model matrix column 1
    float4 instance_line3 [[attribute(8)]];   // model matrix column 2
    float4 instance_line4 [[attribute(9)]];   // model matrix column 3
#if VT_FEATURE_INSTANCING_COLOR
    // Only present with the 80-byte instance stride. The matrix-only 64-byte stride
    // (the default instancing format) leaves base color to the material, and the
    // vertex descriptor declares no attribute(10) for it.
    float4 instanceColor  [[attribute(10)]];  // sRGB diffuse color
#endif
#endif
#if VT_FEATURE_SKINNING
    // GPU skinning: 4-bone weighted blend. Interleaved after uv1 in the 88-byte
    // skinned vertex layout (weights @56, indices @72). Indices are stored as
    // float4 (glTF joints are u8/u16 — float carries them exactly).
    // Mutually exclusive with VT_FEATURE_DYNAMIC_BATCH and VT_FEATURE_INSTANCING;
    // the matrix palette shares buffer slot 6 with dynamic batching.
    float4 blendWeights [[attribute(11)]];
    float4 blendIndices [[attribute(12)]];
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
#if VT_FEATURE_INSTANCING_COLOR
    float4 instanceColor;
#endif
#if VT_FEATURE_POINT_SIZE
    float pointSize [[point_size]];
#endif
#if VT_FEATURE_DYNAMIC_REFRACTION
    // The model matrix's per-axis scale: the
    // fragment stage has no model matrix of its own.
    float3 modelScale [[flat]];
#endif
};

struct ModelData {
    float4x4 modelMatrix;
    float4x4 normalMatrix;
};

struct SceneData {
    float4x4 projViewMatrix;
};

// Emitted by ProgramLibrary from scene/materials/materialUniformFields.h.
VT_MATERIAL_DATA_BLOCK

/// CPU-side packing layout for hardware instancing (VT_FEATURE_INSTANCING).
/// Data is fed to the vertex shader via vertex descriptor layout(5) with perInstance step function.
/// The shader reads it through [[stage_in]] attributes (instance_line1..4 + instanceColor).
///
/// Two strides are supported, chosen by the VertexFormat the app builds:
///   64 bytes — model matrix only (VertexFormat::defaultInstancingFormat, the default).
///              Base color comes from the material, as it does for non-instanced draws.
///   80 bytes — model matrix + per-instance sRGB base color that replaces the material's
///              (VertexFormat::colorInstancingFormat), gated by VT_FEATURE_INSTANCING_COLOR.
struct InstanceData {
    float4x4 modelMatrix;    // 64 bytes — world transform for this instance
    float4   diffuseColor;   // 16 bytes — sRGB base color (transfer-function mapped)
};  // 80 bytes total, 16-byte aligned

// Dynamic batch palette: bound at buffer(6) as `constant float4x4 *palette`.
// Each entry is a float4x4 world transform for one mesh instance in the batch.
// Uses a ring-buffer allocation — no fixed size limit.
// DEVIATION: uses a Metal buffer instead of an RGBA32F bone texture.
// GPU skinning (VT_FEATURE_SKINNING) shares the same slot-6 palette: one
// float4x4 per bone, relative to the mesh instance's node (see SkinInstance).

#if VT_FEATURE_MORPHS
// Morph targets: packed delta buffer at vertex buffer slot 9 (per target, per
// vertex: float4 positionDelta + float4 normalDelta) + MorphParams at slot 10.
// DEVIATION: upstream accumulates active targets into RGBA textures with a
// render pass each frame; this port sums the active targets directly in the
// vertex shader from a static buffer — no per-frame GPU pass.
struct MorphParams {
    uint activeCount;      // number of active targets (<= 8)
    uint vertexCount;      // vertices per target in the delta buffer
    uint2 _pad;
    uint indices[8];       // target indices into the delta buffer
    float weights[8];      // matching blend weights
};

static inline void applyMorph(thread float3 &position, thread float3 &normal,
                              const uint vid,
                              constant float4 *morphDeltas,
                              constant MorphParams &mp)
{
    for (uint k = 0; k < mp.activeCount; ++k) {
        const uint base = (mp.indices[k] * mp.vertexCount + vid) * 2;
        position += mp.weights[k] * morphDeltas[base].xyz;
        normal   += mp.weights[k] * morphDeltas[base + 1].xyz;
    }
}
#endif

struct GpuLight {
    float4 positionRange;        // xyz position, w range
    float4 directionType;        // xyz direction, w type (0 dir, 1 point, 2 spot)
    float4 colorIntensity;       // rgb linear colour, w intensity
    float4 coneParams;           // innerCos, outerCos, falloffLinear, shadow slot (-1 none; local 0/1, or the directional slot)
    float4 areaRightHalfWidth;   // xyz world half-width axis, w LightShape (0 punctual, 1 rect, 2 disk, 3 sphere)
    float4 areaUpHalfHeight;     // xyz world half-height axis
    float4 cookieFlags;          // hasCookie, cookie slot (2D or cube pool by type), CookieChannel, cookieFalloff
};

// Clustered lighting: per-light data packed into a Metal buffer (slot 7).
// 176 bytes per light, 16-byte aligned. Maps 1:1 to CPU GpuClusteredLight.
struct ClusteredLight {
    float4 positionRange;     // xyz=position, w=range
    float4 directionSpot;     // xyz=direction, w=outerConeCos
    float4 colorIntensity;    // xyz=color (linear), w=intensity
    float4 params;            // x=innerConeCos, y=isSpot, z=falloffLinear, w=unused
    float4x4 shadowMatrix;    // spot: world→atlas-rect shadow VP; omni: [0]=rect(x,y,size,edge), [1]=(near,far,bias,-)
    float4 shadowData;        // x=castShadows, y=normal bias, z=intensity, w=1 spot / 2 omni
    float4 areaHalfWidth;     // an area light: xyz world half-width axis, w LightShape (0 punctual)
    float4 areaHalfHeight;    // xyz world half-height axis, w the light's mask bits (1 dynamic, 2 lightmapped)
};

// Opacity dither matrices. Must match scene/constants.h :: DitherMode. The active mode arrives
// per material in MaterialData::flags bits 25-27.
constant uint VT_DITHER_NONE    = 0u;
constant uint VT_DITHER_BAYER2  = 1u;
constant uint VT_DITHER_BAYER4  = 2u;
constant uint VT_DITHER_BAYER8  = 3u;
constant uint VT_DITHER_BAYER16 = 4u;
constant uint VT_DITHER_BLUENOISE = 5u;
constant uint VT_DITHER_IGNNOISE  = 6u;

// Debug shader passes. Must match scene/constants.h :: DebugShaderPass. The active mode arrives
// in LightingData::flagsAndPad.y, so all modes share one compiled variant (VT_FEATURE_DEBUG_PASS)
// and switching between them needs no recompile.
constant uint VT_DEBUGPASS_NONE        = 0u;
constant uint VT_DEBUGPASS_ALBEDO      = 1u;
constant uint VT_DEBUGPASS_WORLDNORMAL = 2u;
constant uint VT_DEBUGPASS_OPACITY     = 3u;
constant uint VT_DEBUGPASS_SPECULARITY = 4u;
constant uint VT_DEBUGPASS_GLOSS       = 5u;
constant uint VT_DEBUGPASS_METALNESS   = 6u;
constant uint VT_DEBUGPASS_AO          = 7u;
constant uint VT_DEBUGPASS_EMISSION    = 8u;
constant uint VT_DEBUGPASS_LIGHTING    = 9u;
constant uint VT_DEBUGPASS_UV0         = 10u;

// Buffer 4: the per-pass lighting block, THE layout every backend shares
// (platform/graphics/lightingBlock.h; the GLSL LightingData declares the same).
struct LightingData {
    float4 ambient;               // rgb ambient (linear)
    float4 cameraPosExposure;     // xyz camera position, w exposure
    uint4 lightCount;             // x = active light count
    GpuLight lights[8];
    float4 fogColorDensity;       // rgb fog colour, w density
    float4 fogStartEndType;       // start, end, type (0 off, 1 linear, 2 exp, 3 exp2), pad
    float4 envParams;             // skyboxIntensity, hasEnvAtlas, encoding (0 srgb, 1 rgbp, 2 rgbm), skyboxMip
    // Directional shadows, slot 0: per-cascade world -> shadow-atlas UV + depth
    // (projection, view, NDC-to-UV, the Y flip and Z [0,1] baked in).
    float4x4 shadowMatrices[4];
    float4 shadowCascadeDistances; // per-cascade far split (view-space depth)
    float4 shadowParams;          // enabled (0 off, 1 PCF, 2 EVSM), numCascades, depthBias, strength
    float4 shadowParams2;         // normalBias, cascadeBlend, toneMapping mode, enableNormalMaps
    float4 pcssParams;            // filterSamples, blockerSamples, penumbraSize, penumbraFalloff
    float4 pcssCascadeRadii;      // per-cascade shadow-camera ortho half-extent
    float4 pcssCascadeDepthRanges; // per-cascade caster depth span (far - near)
    float4x4 localShadowMatrix0;  // spot slot 0: world -> shadow UV + depth
    float4x4 localShadowMatrix1;  // spot slot 1
    float4 localShadowParams0;    // depthBias, normalBias, intensity, isOmni
    float4 localShadowParams1;
    float4 omniShadowParams0;     // near, far, relative depthBias, intensity
    float4 omniShadowParams1;
    float4 localShadowPcss0;      // searchArea UV (0 = off), near, far, 1 for VSM
    float4 localShadowPcss1;
    // Light cookies: spot slots carry a world -> cookie-UV projection, omni slots the
    // light's world transform (its rotation maps light->fragment into cube space).
    float4x4 cookieMatrix2D0;
    float4x4 cookieMatrix2D1;
    float4x4 cookieMatrixCube0;
    float4x4 cookieMatrixCube1;
    float4 cookieParams2D0;       // intensity, cookieFalloff, CookieChannel, pad
    float4 cookieParams2D1;
    float4 cookieParamsCube0;
    float4 cookieParamsCube1;
    float4 skyParams2;            // xyz sky dome centre, w flags: bit0 cubemap bound, bit1 dome projection
    // Ambient SH light probes: premultiplied irradiance coefficients.
    float4 ambientSH[9];
    // Clustered lighting grid (WorldClusters).
    float4 clusterBoundsMin;                // xyz = grid min corner
    float4 clusterBoundsRange;              // xyz = grid size (max - min)
    float4 clusterCellsCountByBoundsSize;   // xyz = cells / range (world -> cell)
    uint4 clusterParams;                    // cellsX, cellsY, cellsZ, maxLightsPerCell
    uint4 clusterParams2;                   // x numClusteredLights, y the cluster-light mask bit this draw accepts
    // Reflection probe (box-projected cubemap): world-space box, centre, and
    // params = {boxProjection flag, intensity, maxMipLod, pad}.
    float4 reflectionProbeBoxMin;
    float4 reflectionProbeBoxMax;
    float4 reflectionProbePosition;
    float4 reflectionProbeParams;
    // Camera view-projection for fragment-stage screen projection (the dynamic
    // refraction grab UV and the SSR march), and the clip planes that linearize
    // the sampled scene depth: x near, y far, z colour grab bound, w depth grab bound.
    float4x4 viewProjection;
    float4 cameraNearFar;
    // Nishita atmosphere (common-atmosphere.metal), the Scene's block copied in.
    float4 atmoPlanetCenterAndRadius;   // xyz planet centre (camera-local), w radius (m)
    float4 atmoRadiusAndSunIntensity;   // x outer radius (m), y sun intensity, z cos(sun disk half-angle)
    float4 atmoRayleighCoeffAndScale;   // xyz Rayleigh coefficients (per m), w scale height (m)
    float4 atmoMieCoeffAndScale;        // x Mie coefficient, y scale height (m), z HG g
    float4 atmoSunDirection;            // xyz normalized sun direction (camera-local)
    float4 atmoCameraAltitudeAndParams; // x altitude (m), y primary steps, z secondary steps
    // DEVIATION: blurred planar reflection.
    float4 screenInvResolution;         // xy = 1/viewport, zw = viewport
    float4 reflectionParams;            // intensity (0..1), blurAmount (0..2), fadeStrength (0..5), angleFade (0..1)
    float4 reflectionFadeColor;         // rgb fade colour (linear)
    float4 reflectionDepthParams;       // planeDistance, heightRange, colour map bound, depth map bound
    // [0] bits: bit 5 = the forward pass writes linear HDR for a camera frame;
    // [1] = the DebugShaderPass mode.
    uint4 flagsAndPad;
    // Second directional shadow slot: the slot-0 fields for the light whose
    // coneParams.w is 1; its map is texture 35.
    float4x4 dirShadow1Matrices[4];
    float4 dirShadow1CascadeDistances;
    float4 dirShadow1Params;            // enabled, numCascades, depthBias, strength
    float4 dirShadow1Params2;           // normalBias, cascadeBlend
    float4 dirShadow1PcssParams;
    float4 dirShadow1PcssCascadeRadii;
    float4 dirShadow1PcssCascadeDepthRanges;
    // Scene::skyboxRotation, one column per vector (w of the first says "rotated"):
    // environment samples read along R * dir.
    float4 skyboxRotation[3];
    // Spot cookie 2x2 per 2D cookie slot, mat2 columns xy, zw.
    float4 cookieTransform2D[2];
    // Blue-noise jitter: xy offset the opacity dither per frame while the camera
    // jitters (TAA), zero otherwise.
    float4 ditherJitter;
};
