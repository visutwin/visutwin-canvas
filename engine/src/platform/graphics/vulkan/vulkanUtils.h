// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Vulkan utility functions: immediate submit, layout transitions, enum mappings.
//
#pragma once

#ifdef VISUTWIN_HAS_VULKAN

#include <vulkan/vulkan.h>

#include "platform/graphics/constants.h"
#include "platform/graphics/stencilParameters.h"
#include "scene/mesh.h"

namespace visutwin::canvas
{
    class VulkanGraphicsDevice;

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

    // Picks a device-supported depth-stencil format: D24_UNORM_S8_UINT when
    // available, else D32_SFLOAT_S8_UINT (MoltenVK on Apple GPUs has no D24S8).
    VkFormat vulkanSupportedDepthStencilFormat(VkPhysicalDevice physicalDevice);
    VkFormat vulkanSupportedDepthFormat(VkPhysicalDevice physicalDevice);

    bool vulkanFormatSupportsImage(VkPhysicalDevice physicalDevice, VkFormat format,
        VkImageType imageType, VkImageTiling tiling, VkImageUsageFlags usage,
        VkImageCreateFlags flags, VkFormatFeatureFlags requiredFeatures,
        VkExtent3D extent = {1, 1, 1}, uint32_t mipLevels = 1,
        uint32_t arrayLayers = 1,
        VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT);

    // Enum mapping functions.
    VkFormat vulkanMapPixelFormat(PixelFormat format);
    VkFilter vulkanMapFilterMode(FilterMode mode);
    VkSamplerAddressMode vulkanMapAddressMode(AddressMode mode);
    VkCullModeFlags vulkanMapCullMode(CullMode mode);
    VkPrimitiveTopology vulkanMapPrimitiveType(PrimitiveType type);
    VkBlendFactor vulkanMapBlendFactor(int factor);
    VkBlendOp vulkanMapBlendOp(int op);
    VkCompareOp vulkanMapStencilCompare(StencilCompareFunction function);
    VkStencilOp vulkanMapStencilOperation(StencilOperation operation);
    bool vulkanFormatHasStencil(VkFormat format);
}

#endif // VISUTWIN_HAS_VULKAN
