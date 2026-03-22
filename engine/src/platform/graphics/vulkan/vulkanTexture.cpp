// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//

#ifdef VISUTWIN_HAS_VULKAN

#include "vulkanTexture.h"
#include "vulkanGraphicsDevice.h"
#include "vulkanUtils.h"

#include <cstring>
#include "platform/graphics/texture.h"
#include "spdlog/spdlog.h"

namespace visutwin::canvas::gpu
{
    VulkanTexture::VulkanTexture(Texture* owner)
        : _owner(owner)
    {
    }

    VulkanTexture::~VulkanTexture()
    {
        if (_vkDevice != VK_NULL_HANDLE) {
            if (_sampler != VK_NULL_HANDLE)
                vkDestroySampler(_vkDevice, _sampler, nullptr);
            if (_imageView != VK_NULL_HANDLE)
                vkDestroyImageView(_vkDevice, _imageView, nullptr);
        }
        if (_allocator != VK_NULL_HANDLE && _image != VK_NULL_HANDLE) {
            vmaDestroyImage(_allocator, _image, _allocation);
        }
    }

    void VulkanTexture::uploadImmediate(GraphicsDevice* device)
    {
        auto* vkDev = static_cast<VulkanGraphicsDevice*>(device);
        _vkDevice = vkDev->device();
        _allocator = vkDev->vmaAllocator();

        uint32_t width = _owner->width();
        uint32_t height = _owner->height();
        VkFormat format = vulkanMapPixelFormat(_owner->pixelFormat());

        // Create VkImage
        VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = format;
        imageInfo.extent = {width, height, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;

        VkResult result = vmaCreateImage(_allocator, &imageInfo, &allocInfo,
            &_image, &_allocation, nullptr);
        if (result != VK_SUCCESS) {
            spdlog::error("VulkanTexture: failed to create VkImage ({}x{}, fmt={})",
                width, height, static_cast<int>(format));
            return;
        }

        // Image view
        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = _image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = format;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCreateImageView(_vkDevice, &viewInfo, nullptr, &_imageView);

        // Sampler
        createSampler(vkDev);

        // Upload pixel data if available
        if (_owner->hasLevels() && _owner->getLevel(0) != nullptr) {
            size_t dataSize = _owner->getLevelDataSize(0, 0);
            const void* data = _owner->getLevel(0);

            // Staging buffer
            VkBuffer stagingBuffer;
            VmaAllocation stagingAlloc;

            VkBufferCreateInfo stagingInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            stagingInfo.size = dataSize;
            stagingInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

            VmaAllocationCreateInfo stagingAllocInfo{};
            stagingAllocInfo.usage = VMA_MEMORY_USAGE_CPU_ONLY;

            vmaCreateBuffer(_allocator, &stagingInfo, &stagingAllocInfo,
                &stagingBuffer, &stagingAlloc, nullptr);

            void* mapped;
            vmaMapMemory(_allocator, stagingAlloc, &mapped);
            memcpy(mapped, data, dataSize);
            vmaUnmapMemory(_allocator, stagingAlloc);

            vulkanImmediateSubmit(vkDev, [&](VkCommandBuffer cmd) {
                vulkanTransitionImageLayout(cmd, _image,
                    VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

                VkBufferImageCopy region{};
                region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                region.imageExtent = {width, height, 1};
                vkCmdCopyBufferToImage(cmd, stagingBuffer, _image,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

                vulkanTransitionImageLayout(cmd, _image,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            });

            vmaDestroyBuffer(_allocator, stagingBuffer, stagingAlloc);
        } else {
            // No data — just transition to shader-readable layout
            vulkanImmediateSubmit(vkDev, [&](VkCommandBuffer cmd) {
                vulkanTransitionImageLayout(cmd, _image,
                    VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            });
        }
    }

    void VulkanTexture::propertyChanged(uint32_t flag)
    {
        (void)flag;
        // TODO: recreate sampler on filter/address changes
    }

    void VulkanTexture::createSampler(VulkanGraphicsDevice* device)
    {
        VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        samplerInfo.magFilter = vulkanMapFilterMode(_owner->magFilter());
        samplerInfo.minFilter = vulkanMapFilterMode(_owner->minFilter());
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        samplerInfo.addressModeU = vulkanMapAddressMode(_owner->addressU());
        samplerInfo.addressModeV = vulkanMapAddressMode(_owner->addressV());
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
        samplerInfo.maxAnisotropy = 1.0f;

        vkCreateSampler(device->device(), &samplerInfo, nullptr, &_sampler);
    }
}

#endif // VISUTWIN_HAS_VULKAN
