// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 16.06.2026
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

#include <array>
#include <cstdint>

#include "platform/graphics/lightingBlock.h"
#include "platform/graphics/shaderBindings.h"

namespace visutwin::canvas
{
    // The set-1 material bindings (kMaterialTextureBindings, in QUAD SLOT ORDER),
    // vulkanMaterialBindingIsSeparateImage, kMaterialExtraSamplerBinding and the set-3
    // binding count (kSceneTextureBindingCount) are derived from the ONE binding table in
    // platform/graphics/shaderBindings.h, which the layout, the binding loop, the
    // descriptor writes, the bundle validator and the Metal binder all read. A new slot
    // is one row there; the derived constants are pinned to their previous literals by
    // static_asserts beside them.

    // The per-pass lighting block bound at set 2, binding 0, and its per-light struct:
    // THE layout every backend shares (platform/graphics/lightingBlock.h), which the
    // GLSL `LightingData` / `Light` declare field for field.
    using VulkanGpuLight = GpuLightBlock;
    using VulkanLightingUBO = LightingBlock;
    using VulkanEnvEncoding = EnvAtlasEncoding;

    // Light type encoding stored in VulkanGpuLight::directionType[3].
    enum class VulkanLightTypeTag : uint32_t
    {
        Directional = 0u,
        Point = 1u,
        Spot = 2u,
    };
}

#endif // VISUTWIN_HAS_VULKAN
