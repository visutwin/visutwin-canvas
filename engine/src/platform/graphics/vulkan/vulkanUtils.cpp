// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//

#ifdef VISUTWIN_HAS_VULKAN

#include "vulkanUtils.h"
#include "vulkanGraphicsDevice.h"

#include "spdlog/spdlog.h"

namespace visutwin::canvas
{
    void vulkanImmediateSubmit(VulkanGraphicsDevice* device,
        const std::function<void(VkCommandBuffer)>& func)
    {
        VkDevice vk = device->device();
        VkCommandPool pool = device->uploadCommandPool();

        VkCommandBufferAllocateInfo allocInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocInfo.commandPool = pool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;

        VkCommandBuffer cmd;
        vkAllocateCommandBuffers(vk, &allocInfo, &cmd);

        VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &beginInfo);

        func(cmd);

        vkEndCommandBuffer(cmd);

        VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &cmd;

        VkFence fence = device->uploadFence();
        vkQueueSubmit(device->graphicsQueue(), 1, &submitInfo, fence);
        vkWaitForFences(vk, 1, &fence, VK_TRUE, UINT64_MAX);
        vkResetFences(vk, 1, &fence);

        vkFreeCommandBuffers(vk, pool, 1, &cmd);
    }

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
        case PixelFormat::PIXELFORMAT_RGB8:         return VK_FORMAT_R8G8B8_UNORM;
        case PixelFormat::PIXELFORMAT_RGBA8:        return VK_FORMAT_R8G8B8A8_UNORM;
        case PixelFormat::PIXELFORMAT_RGBA16F:      return VK_FORMAT_R16G16B16A16_SFLOAT;
        case PixelFormat::PIXELFORMAT_RGBA32F:      return VK_FORMAT_R32G32B32A32_SFLOAT;
        case PixelFormat::PIXELFORMAT_R32F:         return VK_FORMAT_R32_SFLOAT;
        case PixelFormat::PIXELFORMAT_DEPTH:        return VK_FORMAT_D32_SFLOAT;
        case PixelFormat::PIXELFORMAT_DEPTH16:      return VK_FORMAT_D16_UNORM;
        case PixelFormat::PIXELFORMAT_DEPTHSTENCIL: return VK_FORMAT_D24_UNORM_S8_UINT;
        case PixelFormat::PIXELFORMAT_R8:           return VK_FORMAT_R8_UNORM;
        case PixelFormat::PIXELFORMAT_RG8:          return VK_FORMAT_R8G8_UNORM;
        default:                                    return VK_FORMAT_R8G8B8A8_UNORM;
        }
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
}

#endif // VISUTWIN_HAS_VULKAN
