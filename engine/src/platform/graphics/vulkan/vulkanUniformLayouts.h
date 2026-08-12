// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// GPU-side uniform block layouts for the Vulkan backend.
//
// These mirror the std140 layout declared in the embedded GLSL shaders
// (forward.frag). Every member is either a vec4 or a run of exactly
// four 4-byte scalars, so the natural C++ layout already satisfies std140 —
// no explicit padding is required.  Keep these structs and the GLSL blocks in
// lock-step; a mismatch shifts every field that follows it.
//
#pragma once

#ifdef VISUTWIN_HAS_VULKAN

#include <cstdint>

namespace visutwin::canvas
{
    // One light, matching the GLSL `Light` struct (set 2).  64 bytes.
    struct VulkanGpuLight
    {
        float positionRange[4]  = {0.0f, 0.0f, 0.0f, 0.0f};   // xyz position, w range
        float directionType[4]  = {0.0f, -1.0f, 0.0f, 0.0f};  // xyz direction, w type
        float colorIntensity[4] = {1.0f, 1.0f, 1.0f, 0.0f};   // rgb color, w intensity
        // innerCos, outerCos, falloffLinear, localShadowIndex (-1 = none, 0/1 = local slot)
        float coneParams[4]     = {1.0f, 1.0f, 1.0f, -1.0f};
        float areaRightHalfWidth[4] = {1.0f, 0.0f, 0.0f, 0.0f};
        float areaUpHalfHeight[4] = {0.0f, 0.0f, 1.0f, 0.0f};
    };

    // Per-pass lighting block bound at set 2, binding 0.
    struct VulkanLightingUBO
    {
        float ambient[4]            = {0.0f, 0.0f, 0.0f, 0.0f};  // rgb ambient
        float cameraPosExposure[4]  = {0.0f, 0.0f, 0.0f, 1.0f};  // xyz camera, w exposure
        uint32_t lightCount[4]      = {0u, 0u, 0u, 0u};          // x = active light count
        VulkanGpuLight lights[8];
        float fogColorDensity[4]    = {0.0f, 0.0f, 0.0f, 0.0f};  // rgb fog color, w density
        float fogStartEndType[4]    = {10.0f, 100.0f, 0.0f, 0.0f}; // start, end, type(0/1/2), pad
        // x=skyboxIntensity, y=hasEnvAtlas(0/1), z=encoding(0 srgb,1 rgbp,2 rgbm), w=skyboxMip
        float envParams[4]          = {1.0f, 0.0f, 0.0f, 0.0f};

        // Directional cascaded shadows.  shadowMatrices is 4 column-major mat4
        // (GLSL `mat4 shadowMatrices[4]`) — each maps world space to a cascade's
        // shadow-atlas UV + depth.  Matches ShadowParams::shadowMatrixPalette.
        float shadowMatrices[64]       = {};
        float shadowCascadeDistances[4]= {0.0f, 0.0f, 0.0f, 0.0f}; // per-cascade far split (view depth)
        // x=enabled(0/1), y=numCascades, z=depthBias, w=strength
        float shadowParams[4]          = {0.0f, 1.0f, 0.0001f, 1.0f};
        // x=normalBias, y=cascadeBlend, z/w pad
        // x=normalBias, y=cascadeBlend, z=toneMapping mode, w=enableNormalMaps
        float shadowParams2[4]         = {0.0f, 0.0f, 0.0f, 0.0f};

        // Directional PCSS (SHADOW_PCSS_32F), read only when the shader is
        // specialized with VT_FEATURE_PCSS_SHADOWS.  The world-space penumbra
        // math needs each cascade's ortho half-extent and caster depth span.
        // x=filterSamples, y=blockerSamples, z=penumbraSize, w=penumbraFalloff
        float pcssParams[4]            = {16.0f, 16.0f, 1.0f, 1.0f};
        float pcssCascadeRadii[4]       = {1.0f, 1.0f, 1.0f, 1.0f};
        float pcssCascadeDepthRanges[4] = {1.0f, 1.0f, 1.0f, 1.0f};

        // Local light shadows (spot 2D + omni cubemap), up to 2 casters.
        // Spot slots use a per-light VP matrix (world → shadow UV + depth);
        // omni slots use a distance compare against the cubemap (no matrix).
        float localShadowMatrix0[16]   = {};   // spot slot 0 world → atlas UV+depth
        float localShadowMatrix1[16]   = {};   // spot slot 1
        // x=depthBias, y=normalBias, z=intensity, w=isOmni(0/1)
        float localShadowParams0[4]    = {0.0001f, 0.0f, 1.0f, 0.0f};
        float localShadowParams1[4]    = {0.0001f, 0.0f, 1.0f, 0.0f};
        // Omni cubemap params.  x=near, y=far, z=depthBias, w=intensity
        float omniShadowParams0[4]     = {0.01f, 100.0f, 0.0001f, 1.0f};
        float omniShadowParams1[4]     = {0.01f, 100.0f, 0.0001f, 1.0f};
        // Local-light PCSS, per slot.  Unlike the directional path this is a
        // runtime branch (no shader variant): x = blocker-search radius in
        // shadow-map UV, 0 = PCSS off for that slot.  y=near, z=far, w=pad.
        float localShadowPcss0[4]      = {0.0f, 0.01f, 100.0f, 0.0f};
        float localShadowPcss1[4]      = {0.0f, 0.01f, 100.0f, 0.0f};

        // xyz = sky dome center (world), w = flags: bit0 = has skybox cubemap,
        // bit1 = dome projection (view dir from dome center, not camera).
        float skyParams2[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        // Nine premultiplied irradiance SH coefficients.
        float ambientSH[9][4] = {};
        float clusterBoundsMin[4] = {};
        float clusterBoundsRange[4] = {};
        float clusterCellsCountByBoundsSize[4] = {};
        uint32_t clusterParams[4] = {};
        uint32_t clusterParams2[4] = {};
        float reflectionProbeBoxMin[4] = {};
        float reflectionProbeBoxMax[4] = {};
        float reflectionProbePosition[4] = {};
        float reflectionProbeParams[4] = {};

        // Screen-space reflections: the camera view-projection used to project a
        // marched world-space position to screen UV, and the clip planes needed
        // to linearize the sampled scene depth.  z/w flag whether the scene
        // colour and depth grabs are bound: refraction needs the colour grab,
        // the SSR march needs both.
        float viewProjection[16] = {};
        float cameraNearFar[4]   = {0.1f, 1000.0f, 0.0f, 0.0f};

        // Nishita atmosphere (96 bytes = 6 vec4), layout-identical to
        // UniformBinder::AtmosphereUniforms and Metal's AtmosphereData.
        //
        // DEVIATION: Metal binds this as its own fragment buffer at slot 9.
        // Here it rides the lighting UBO instead — the block is scene-global and
        // uploaded only when dirty (once per pass, not per draw), so the 96
        // bytes cost nothing measurable, and a separate binding would mean a new
        // descriptor set layout, pool sizing, and another entry in all three
        // layout validators. Defaults mirror AtmosphereUniforms so a device that
        // never receives setAtmosphereUniforms still holds sane values.
        float atmoPlanetCenterAndRadius[4]  = {0.0f, 0.0f, 0.0f, 6371000.0f};
        float atmoRadiusAndSunIntensity[4]  = {6471000.0f, 22.0f, 0.9998f, 0.0f};
        float atmoRayleighCoeffAndScale[4]  = {5.5e-6f, 13.0e-6f, 22.4e-6f, 8500.0f};
        float atmoMieCoeffAndScale[4]       = {21.0e-6f, 1200.0f, 0.758f, 0.0f};
        float atmoSunDirection[4]           = {0.0f, 1.0f, 0.0f, 0.0f};
        float atmoCameraAltitudeAndParams[4] = {0.0f, 32.0f, 8.0f, 0.0f};

        // Blurred planar reflection. Defaults match ReflectionBlurParams and
        // UniformBinder::LightingUniforms so an unset device behaves the same.
        // xy = 1/viewport, zw = viewport (screen-space UV for the reflection).
        float screenInvResolution[4]    = {0.0f, 0.0f, 0.0f, 0.0f};
        // x = intensity, y = blurAmount, z = fadeStrength, w = angleFade
        float reflectionParams[4]       = {1.0f, 0.0f, 1.0f, 0.5f};
        float reflectionFadeColor[4]    = {0.5f, 0.5f, 0.5f, 0.0f};
        // x = planeDistance, y = heightRange (depth-pass distance normalization)
        float reflectionDepthParams[4]  = {0.0f, 10.0f, 0.0f, 0.0f};
    };

    // Env-atlas encoding tag stored in VulkanLightingUBO::envParams[2].
    enum class VulkanEnvEncoding : uint32_t
    {
        Srgb = 0u,
        Rgbp = 1u,
        Rgbm = 2u,
    };

    // Light type encoding stored in VulkanGpuLight::directionType[3].
    enum class VulkanLightTypeTag : uint32_t
    {
        Directional = 0u,
        Point = 1u,
        Spot = 2u,
    };
}

#endif // VISUTWIN_HAS_VULKAN
