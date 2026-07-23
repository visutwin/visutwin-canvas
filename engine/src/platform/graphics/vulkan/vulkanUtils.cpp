// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//

#ifdef VISUTWIN_HAS_VULKAN

#include "vulkanUtils.h"
#include "vulkanGraphicsDevice.h"

#include "spdlog/spdlog.h"

namespace visutwin::canvas
{
    void vulkanTransitionImageLayout(VkCommandBuffer cmd, VkImage image,
        VkImageLayout oldLayout, VkImageLayout newLayout, VkImageAspectFlags aspect,
        uint32_t baseMipLevel, uint32_t levelCount,
        uint32_t baseArrayLayer, uint32_t layerCount)
    {
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.oldLayout = oldLayout;
        barrier.newLayout = newLayout;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange.aspectMask = aspect;
        barrier.subresourceRange.baseMipLevel = baseMipLevel;
        barrier.subresourceRange.levelCount = levelCount;
        barrier.subresourceRange.baseArrayLayer = baseArrayLayer;
        barrier.subresourceRange.layerCount = layerCount;

        // Sensible default — full-pipeline barrier. Specialized below.
        VkPipelineStageFlags srcStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkPipelineStageFlags dstStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = 0;

        // Source side: derive masks from oldLayout.
        switch (oldLayout) {
        case VK_IMAGE_LAYOUT_UNDEFINED:
        case VK_IMAGE_LAYOUT_PREINITIALIZED:
            barrier.srcAccessMask = 0;
            srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
            break;
        case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            break;
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            break;
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
            barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
            srcStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            break;
        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
            barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            srcStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            break;
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
        case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL:
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
        case VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL:
            barrier.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            srcStage = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
            break;
        case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
            barrier.srcAccessMask = 0;
            srcStage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
            break;
        default:
            break;
        }

        // Destination side: derive masks from newLayout.
        switch (newLayout) {
        case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            break;
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            break;
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            dstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            break;
        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
            barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                    VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
            dstStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            break;
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
        case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL:
            barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                                    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
            dstStage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
            break;
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
        case VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL:
            barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                    VK_ACCESS_SHADER_READ_BIT;
            dstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            break;
        case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
            barrier.dstAccessMask = 0;
            dstStage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
            break;
        default:
            break;
        }

        vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0,
            0, nullptr, 0, nullptr, 1, &barrier);
    }

    VkFormat vulkanMapPixelFormat(PixelFormat format)
    {
        switch (format) {
        // RGB8 is widened to RGBA8: the engine already treats RGB8 as 4
        // bytes/pixel (constants.cpp) and the Metal backend maps it the same
        // way.  A true 3-byte R8G8B8 image would misread that data and is
        // barely supported by desktop drivers anyway.
        case PixelFormat::PIXELFORMAT_RGB8:         return VK_FORMAT_R8G8B8A8_UNORM;
        case PixelFormat::PIXELFORMAT_RGBA8:        return VK_FORMAT_R8G8B8A8_UNORM;
        case PixelFormat::PIXELFORMAT_DXT1:         return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
        case PixelFormat::PIXELFORMAT_DXT3:         return VK_FORMAT_BC2_UNORM_BLOCK;
        case PixelFormat::PIXELFORMAT_DXT5:         return VK_FORMAT_BC3_UNORM_BLOCK;
        case PixelFormat::PIXELFORMAT_RGBA16F:      return VK_FORMAT_R16G16B16A16_SFLOAT;
        case PixelFormat::PIXELFORMAT_RGBA32F:      return VK_FORMAT_R32G32B32A32_SFLOAT;
        case PixelFormat::PIXELFORMAT_R32F:         return VK_FORMAT_R32_SFLOAT;
        case PixelFormat::PIXELFORMAT_DEPTH:        return VK_FORMAT_D32_SFLOAT;
        case PixelFormat::PIXELFORMAT_DEPTH16:      return VK_FORMAT_D16_UNORM;
        case PixelFormat::PIXELFORMAT_DEPTHSTENCIL: return VK_FORMAT_D24_UNORM_S8_UINT;
        case PixelFormat::PIXELFORMAT_R8:           return VK_FORMAT_R8_UNORM;
        case PixelFormat::PIXELFORMAT_RG8:          return VK_FORMAT_R8G8_UNORM;
        case PixelFormat::PIXELFORMAT_ASTC_4x4:     return VK_FORMAT_ASTC_4x4_UNORM_BLOCK;
        case PixelFormat::PIXELFORMAT_ASTC_5x5:     return VK_FORMAT_ASTC_5x5_UNORM_BLOCK;
        case PixelFormat::PIXELFORMAT_ASTC_6x6:     return VK_FORMAT_ASTC_6x6_UNORM_BLOCK;
        case PixelFormat::PIXELFORMAT_ASTC_8x8:     return VK_FORMAT_ASTC_8x8_UNORM_BLOCK;
        case PixelFormat::PIXELFORMAT_ASTC_10x10:   return VK_FORMAT_ASTC_10x10_UNORM_BLOCK;
        case PixelFormat::PIXELFORMAT_ASTC_12x12:   return VK_FORMAT_ASTC_12x12_UNORM_BLOCK;
        case PixelFormat::PIXELFORMAT_BC4:          return VK_FORMAT_BC4_UNORM_BLOCK;
        case PixelFormat::PIXELFORMAT_BC5:          return VK_FORMAT_BC5_UNORM_BLOCK;
        case PixelFormat::PIXELFORMAT_BC6H:         return VK_FORMAT_BC6H_UFLOAT_BLOCK;
        case PixelFormat::PIXELFORMAT_BC7:          return VK_FORMAT_BC7_UNORM_BLOCK;
        default:                                    return VK_FORMAT_UNDEFINED;
        }
    }

    bool vulkanFormatSupportsImage(const VkPhysicalDevice physicalDevice,
        const VkFormat format, const VkImageType imageType,
        const VkImageTiling tiling, const VkImageUsageFlags usage,
        const VkImageCreateFlags flags, const VkFormatFeatureFlags requiredFeatures,
        const VkExtent3D extent, const uint32_t mipLevels,
        const uint32_t arrayLayers, const VkSampleCountFlagBits samples)
    {
        if (format == VK_FORMAT_UNDEFINED) {
            return false;
        }

        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(physicalDevice, format, &properties);
        const VkFormatFeatureFlags available = tiling == VK_IMAGE_TILING_OPTIMAL
            ? properties.optimalTilingFeatures : properties.linearTilingFeatures;
        if ((available & requiredFeatures) != requiredFeatures) {
            return false;
        }

        VkImageFormatProperties imageProperties{};
        if (vkGetPhysicalDeviceImageFormatProperties(
            physicalDevice, format, imageType, tiling, usage, flags,
            &imageProperties) != VK_SUCCESS) {
            return false;
        }
        return extent.width <= imageProperties.maxExtent.width &&
               extent.height <= imageProperties.maxExtent.height &&
               extent.depth <= imageProperties.maxExtent.depth &&
               mipLevels <= imageProperties.maxMipLevels &&
               arrayLayers <= imageProperties.maxArrayLayers &&
               (imageProperties.sampleCounts & samples) != 0;
    }

    VkFormat vulkanSupportedDepthStencilFormat(VkPhysicalDevice physicalDevice)
    {
        for (const VkFormat format : {VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT}) {
            VkFormatProperties props{};
            vkGetPhysicalDeviceFormatProperties(physicalDevice, format, &props);
            if (props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
                return format;
            }
        }
        return VK_FORMAT_UNDEFINED;
    }

    VkFormat vulkanSupportedDepthFormat(const VkPhysicalDevice physicalDevice)
    {
        for (const VkFormat format : {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D16_UNORM}) {
            VkFormatProperties props{};
            vkGetPhysicalDeviceFormatProperties(physicalDevice, format, &props);
            if (props.optimalTilingFeatures &
                VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
                return format;
            }
        }
        return VK_FORMAT_UNDEFINED;
    }

    VkFilter vulkanMapFilterMode(FilterMode mode)
    {
        switch (mode) {
        case FilterMode::FILTER_NEAREST:
        case FilterMode::FILTER_NEAREST_MIPMAP_NEAREST:
        case FilterMode::FILTER_NEAREST_MIPMAP_LINEAR:
            return VK_FILTER_NEAREST;
        default:
            return VK_FILTER_LINEAR;
        }
    }

    VkSamplerAddressMode vulkanMapAddressMode(AddressMode mode)
    {
        switch (mode) {
        case ADDRESS_CLAMP_TO_EDGE:  return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        case ADDRESS_MIRRORED_REPEAT: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        default:                      return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        }
    }

    VkCullModeFlags vulkanMapCullMode(CullMode mode)
    {
        switch (mode) {
        case CullMode::CULLFACE_NONE:         return VK_CULL_MODE_NONE;
        case CullMode::CULLFACE_BACK:         return VK_CULL_MODE_BACK_BIT;
        case CullMode::CULLFACE_FRONT:        return VK_CULL_MODE_FRONT_BIT;
        case CullMode::CULLFACE_FRONTANDBACK: return VK_CULL_MODE_FRONT_AND_BACK;
        default:                              return VK_CULL_MODE_BACK_BIT;
        }
    }

    VkPrimitiveTopology vulkanMapPrimitiveType(PrimitiveType type)
    {
        switch (type) {
        case PRIMITIVE_POINTS:    return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
        case PRIMITIVE_LINES:     return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        case PRIMITIVE_LINESTRIP: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
        case PRIMITIVE_TRIANGLES: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        case PRIMITIVE_TRISTRIP:  return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        case PRIMITIVE_TRIFAN:    return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
        default:                  return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        }
    }

    VkBlendFactor vulkanMapBlendFactor(int factor)
    {
        switch (factor) {
        case BLENDMODE_ZERO:                  return VK_BLEND_FACTOR_ZERO;
        case BLENDMODE_ONE:                   return VK_BLEND_FACTOR_ONE;
        case BLENDMODE_SRC_COLOR:             return VK_BLEND_FACTOR_SRC_COLOR;
        case BLENDMODE_ONE_MINUS_SRC_COLOR:   return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case BLENDMODE_DST_COLOR:             return VK_BLEND_FACTOR_DST_COLOR;
        case BLENDMODE_ONE_MINUS_DST_COLOR:   return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
        case BLENDMODE_SRC_ALPHA:             return VK_BLEND_FACTOR_SRC_ALPHA;
        case BLENDMODE_SRC_ALPHA_SATURATE:    return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
        case BLENDMODE_ONE_MINUS_SRC_ALPHA:   return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case BLENDMODE_DST_ALPHA:             return VK_BLEND_FACTOR_DST_ALPHA;
        case BLENDMODE_ONE_MINUS_DST_ALPHA:   return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case BLENDMODE_CONSTANT:              return VK_BLEND_FACTOR_CONSTANT_COLOR;
        case BLENDMODE_ONE_MINUS_CONSTANT:    return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
        default:                              return VK_BLEND_FACTOR_ONE;
        }
    }

    VkBlendOp vulkanMapBlendOp(int op)
    {
        switch (op) {
        case BLENDEQUATION_ADD:              return VK_BLEND_OP_ADD;
        case BLENDEQUATION_SUBTRACT:         return VK_BLEND_OP_SUBTRACT;
        case BLENDEQUATION_REVERSE_SUBTRACT: return VK_BLEND_OP_REVERSE_SUBTRACT;
        case BLENDEQUATION_MIN:              return VK_BLEND_OP_MIN;
        case BLENDEQUATION_MAX:              return VK_BLEND_OP_MAX;
        default:                             return VK_BLEND_OP_ADD;
        }
    }

    VkCompareOp vulkanMapStencilCompare(const StencilCompareFunction function)
    {
        switch (function) {
        case StencilCompareFunction::Never:        return VK_COMPARE_OP_NEVER;
        case StencilCompareFunction::Less:         return VK_COMPARE_OP_LESS;
        case StencilCompareFunction::Equal:        return VK_COMPARE_OP_EQUAL;
        case StencilCompareFunction::LessEqual:    return VK_COMPARE_OP_LESS_OR_EQUAL;
        case StencilCompareFunction::Greater:      return VK_COMPARE_OP_GREATER;
        case StencilCompareFunction::NotEqual:     return VK_COMPARE_OP_NOT_EQUAL;
        case StencilCompareFunction::GreaterEqual: return VK_COMPARE_OP_GREATER_OR_EQUAL;
        case StencilCompareFunction::Always:       return VK_COMPARE_OP_ALWAYS;
        }
        return VK_COMPARE_OP_ALWAYS;
    }

    VkStencilOp vulkanMapStencilOperation(const StencilOperation operation)
    {
        switch (operation) {
        case StencilOperation::Keep:           return VK_STENCIL_OP_KEEP;
        case StencilOperation::Zero:           return VK_STENCIL_OP_ZERO;
        case StencilOperation::Replace:        return VK_STENCIL_OP_REPLACE;
        case StencilOperation::IncrementClamp: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
        case StencilOperation::DecrementClamp: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
        case StencilOperation::Invert:         return VK_STENCIL_OP_INVERT;
        case StencilOperation::IncrementWrap:  return VK_STENCIL_OP_INCREMENT_AND_WRAP;
        case StencilOperation::DecrementWrap:  return VK_STENCIL_OP_DECREMENT_AND_WRAP;
        }
        return VK_STENCIL_OP_KEEP;
    }

    bool vulkanFormatHasStencil(const VkFormat format)
    {
        return format == VK_FORMAT_D16_UNORM_S8_UINT ||
               format == VK_FORMAT_D24_UNORM_S8_UINT ||
               format == VK_FORMAT_D32_SFLOAT_S8_UINT;
    }
}

#endif // VISUTWIN_HAS_VULKAN
