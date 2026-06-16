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
        float coneParams[4]     = {1.0f, 1.0f, 1.0f, 0.0f};   // innerCos, outerCos, falloffLinear, pad
    };

    // Per-pass lighting block bound at set 2, binding 0.  592 bytes.
    struct VulkanLightingUBO
    {
        float ambient[4]            = {0.0f, 0.0f, 0.0f, 0.0f};  // rgb ambient
        float cameraPosExposure[4]  = {0.0f, 0.0f, 0.0f, 1.0f};  // xyz camera, w exposure
        uint32_t lightCount[4]      = {0u, 0u, 0u, 0u};          // x = active light count
        VulkanGpuLight lights[8];
        float fogColorDensity[4]    = {0.0f, 0.0f, 0.0f, 0.0f};  // rgb fog color, w density
        float fogStartEndType[4]    = {10.0f, 100.0f, 0.0f, 0.0f}; // start, end, type(0/1/2), pad
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
