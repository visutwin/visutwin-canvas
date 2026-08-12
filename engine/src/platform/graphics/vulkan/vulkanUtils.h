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

    // Descriptor type of scene-set (set 3) binding `binding`.
    //
    // The set is deliberately mixed: bindings 0-5 are combined image samplers,
    // 6-11 and 14 are separate sampled images, 12-13 are the two shared samplers
    // they read through. Combined bindings each cost one of the device's 16
    // per-stage sampler slots (a Metal limit MoltenVK inherits) and set 1 already
    // claims six, so anything added here must be a separate image.
    //
    // Both the layout creation and the descriptor writes have to agree with this
    // exactly — writing the wrong descriptor type is undefined behaviour — so it
    // lives here rather than being spelled out at each site.
    VkDescriptorType vulkanSceneDescriptorType(uint32_t binding);

    // Enum mapping functions.
    VkFormat vulkanMapPixelFormat(PixelFormat format);
    VkFilter vulkanMapFilterMode(FilterMode mode);
    VkSamplerAddressMode vulkanMapAddressMode(AddressMode mode);
    VkCullModeFlags vulkanMapCullMode(CullMode mode);
    VkPrimitiveTopology vulkanMapPrimitiveType(PrimitiveType type);
    VkBlendFactor vulkanMapBlendFactor(int factor);
    VkBlendOp vulkanMapBlendOp(int op);

    // Nearest single-source equivalent of a BLENDMODE_SRC1_* factor, used to degrade
    // a dual-source blend state on a device without the dualSrcBlend feature. Any
    // other factor passes through unchanged.
    int blendFactorWithoutSrc1(int factor);
    VkCompareOp vulkanMapStencilCompare(StencilCompareFunction function);
    VkStencilOp vulkanMapStencilOperation(StencilOperation operation);
    bool vulkanFormatHasStencil(VkFormat format);
}

#endif // VISUTWIN_HAS_VULKAN
