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
    // (upstream's default instancing format) leaves base color to the material, and the
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
    // Volume attenuation (KHR_materials_volume): rgb=attenuationColor, w=attenuationDistance (0 = off).
    float4 attenuationParams;
    // Dispersion (KHR_materials_dispersion): x=strength, yzw=pad.
    float4 dispersionParams;
};

/// CPU-side packing layout for hardware instancing (VT_FEATURE_INSTANCING).
/// Data is fed to the vertex shader via vertex descriptor layout(5) with perInstance step function.
/// The shader reads it through [[stage_in]] attributes (instance_line1..4 + instanceColor).
///
/// Two strides are supported, chosen by the VertexFormat the app builds:
///   64 bytes — model matrix only (VertexFormat::defaultInstancingFormat, upstream's default).
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
    float4 positionRange;
    float4 directionCone;
    float4 colorIntensity;
    float4 coneAngles;
    uint4 typeCastShadows;
};

// Clustered lighting: per-light data packed into a Metal buffer (slot 7).
// 144 bytes per light, 16-byte aligned. Maps 1:1 to CPU GpuClusteredLight.
struct ClusteredLight {
    float4 positionRange;     // xyz=position, w=range
    float4 directionSpot;     // xyz=direction, w=outerConeCos
    float4 colorIntensity;    // xyz=color (linear), w=intensity
    float4 params;            // x=innerConeCos, y=isSpot, z=falloffLinear, w=unused
    float4x4 shadowMatrix;    // world→atlas-slice shadow VP (clustered spot shadows)
    float4 shadowData;        // x=castShadows, y=bias, z=intensity, w=atlasSlice
};

// Opacity dither matrices. Must match scene/constants.h :: DitherMode. The active mode arrives
// per material in MaterialData::flags bits 25-27.
constant uint VT_DITHER_NONE    = 0u;
constant uint VT_DITHER_BAYER2  = 1u;
constant uint VT_DITHER_BAYER4  = 2u;
constant uint VT_DITHER_BAYER8  = 3u;
constant uint VT_DITHER_BAYER16 = 4u;

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

    // Ambient SH light probes: premultiplied irradiance coefficients (upstream AMBIENTSH).
    float4 ambientSH[9];
    // Camera view-projection for fragment-stage screen projection
    // (VT_FEATURE_DYNAMIC_REFRACTION grab-pass UV).
    float4x4 viewProjection;
    // PCSS directional shadows: {filterSamples, blockerSamples, penumbraSize,
    // penumbraFalloff}; per-cascade shadow-cam ortho radii and depth ranges.
    float4 pcssParams;
    float4 pcssCascadeRadii;
    float4 pcssCascadeDepthRanges;
    // Local light PCSS (spot/omni contact-hardening shadows):
    // [x]=searchArea in shadow-map UV (0 = PCSS off, use PCF/hardware compare),
    // [y]=shadow camera near, [z]=shadow camera far, [w]=pad.
    float4 localShadowPcss0;
    float4 localShadowPcss1;
    // Reflection probe (box-projected cubemap): world-space box bounds + params.
    // params = {boxProjection flag, intensity, maxMipLod, pad}.
    float4 reflectionProbeBoxMin;
    float4 reflectionProbeBoxMax;
    float4 reflectionProbeParams;
    // Camera clip planes for SSR depth linearization: x=near, y=far, zw=pad.
    float4 cameraNearFar;
};
