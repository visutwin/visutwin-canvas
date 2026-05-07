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
    //
    // Source/destination access masks and pipeline stages are derived from the
    // old/new layouts using a generic table.  This handles the common cases
    // (UNDEFINED→*, transfer/shader-read↔attachment, attachment↔shader-read,
    // *→present-src) without each call site having to spell out every
    // permutation.  For unusual layouts the helper falls back to ALL_COMMANDS
    // barriers, which is correct but conservative.
    void vulkanTransitionImageLayout(VkCommandBuffer cmd, VkImage image,
        VkImageLayout oldLayout, VkImageLayout newLayout,
        VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT,
        uint32_t baseMipLevel = 0, uint32_t levelCount = 1,
        uint32_t baseArrayLayer = 0, uint32_t layerCount = 1);

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
