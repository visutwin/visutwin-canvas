// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Vulkan utility functions: immediate submit, layout transitions, enum mappings.
//
#pragma once

#ifdef VISUTWIN_HAS_VULKAN

#include <functional>
#include <vulkan/vulkan.h>

#include "platform/graphics/constants.h"
#include "scene/mesh.h"

namespace visutwin::canvas
{
    class VulkanGraphicsDevice;

    // Execute a one-shot command buffer and block until GPU completes.
    void vulkanImmediateSubmit(VulkanGraphicsDevice* device,
        const std::function<void(VkCommandBuffer)>& func);

    // Insert an image layout transition barrier.
    void vulkanTransitionImageLayout(VkCommandBuffer cmd, VkImage image,
        VkImageLayout oldLayout, VkImageLayout newLayout,
        VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT);

    // Enum mapping functions.
    VkFormat vulkanMapPixelFormat(PixelFormat format);
    VkFilter vulkanMapFilterMode(FilterMode mode);
    VkSamplerAddressMode vulkanMapAddressMode(AddressMode mode);
    VkCullModeFlags vulkanMapCullMode(CullMode mode);
    VkPrimitiveTopology vulkanMapPrimitiveType(PrimitiveType type);
    VkBlendFactor vulkanMapBlendFactor(int factor);
    VkBlendOp vulkanMapBlendOp(int op);
}

#endif // VISUTWIN_HAS_VULKAN
