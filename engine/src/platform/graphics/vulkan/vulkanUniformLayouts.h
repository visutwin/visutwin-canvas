// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// GPU-side uniform block layouts for the Vulkan backend.
//
// These mirror the std140 layout declared in the embedded GLSL shaders
// (forward_basic.frag).  Every member is either a vec4 or a run of exactly
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
    };

    // Per-pass lighting block bound at set 2, binding 0.  1136 bytes.
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

        // xyz = sky dome center (world), w = flags: bit0 = has skybox cubemap,
        // bit1 = dome projection (view dir from dome center, not camera).
        float skyParams2[4] = {0.0f, 0.0f, 0.0f, 0.0f};
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
