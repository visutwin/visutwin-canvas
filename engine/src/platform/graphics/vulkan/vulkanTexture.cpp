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
    namespace
    {
        // Map a VkFormat to its image aspect bits.  We intentionally treat
        // depth-stencil formats as depth-only here because every consumer in
        // the engine that writes to or reads from these textures is depth-only.
        // Stencil sampling/clearing would require a separate aspect mask.
        VkImageAspectFlags aspectForFormat(VkFormat fmt)
        {
            switch (fmt) {
            case VK_FORMAT_D16_UNORM:
            case VK_FORMAT_D32_SFLOAT:
            case VK_FORMAT_X8_D24_UNORM_PACK32:
                return VK_IMAGE_ASPECT_DEPTH_BIT;
            case VK_FORMAT_D24_UNORM_S8_UINT:
            case VK_FORMAT_D32_SFLOAT_S8_UINT:
            case VK_FORMAT_D16_UNORM_S8_UINT:
                return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
            default:
                return VK_IMAGE_ASPECT_COLOR_BIT;
            }
        }

        bool formatIsDepth(VkFormat fmt)
        {
            return (aspectForFormat(fmt) & VK_IMAGE_ASPECT_DEPTH_BIT) != 0;
        }

        // Usage flags every texture gets so any of them can be bound as a
        // render-target attachment without a separate "render target" hint
        // path through the cross-platform API.  On modern desktop drivers
        // this is essentially free; tile-based mobile drivers may pay a
        // small cost but we don't target those.
        VkImageUsageFlags defaultUsageForAspect(VkImageAspectFlags aspect)
        {
            VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT |
                                      VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            if (aspect & VK_IMAGE_ASPECT_DEPTH_BIT) {
                usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
            } else {
                usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
            }
            return usage;
        }
    }

    VulkanTexture::VulkanTexture(Texture* owner)
        : _owner(owner)
    {
    }

    VulkanTexture::~VulkanTexture()
    {
        if (_vkDevice != VK_NULL_HANDLE) {
            destroySampler();
            if (_imageView != VK_NULL_HANDLE) {
                vkDestroyImageView(_vkDevice, _imageView, nullptr);
                _imageView = VK_NULL_HANDLE;
            }
        }
        if (_allocator != VK_NULL_HANDLE && _image != VK_NULL_HANDLE) {
            vmaDestroyImage(_allocator, _image, _allocation);
            _image = VK_NULL_HANDLE;
            _allocation = VK_NULL_HANDLE;
        }
    }

    void VulkanTexture::uploadImmediate(GraphicsDevice* device)
    {
        auto* vkDev = static_cast<VulkanGraphicsDevice*>(device);
        _vkDevice = vkDev->device();
        _allocator = vkDev->vmaAllocator();

        const uint32_t width = _owner->width();
        const uint32_t height = _owner->height();
        _format = vulkanMapPixelFormat(_owner->format());
        _aspect = aspectForFormat(_format);
        const bool isDepth = formatIsDepth(_format);
        const bool isCubemap = _owner->isCubemap();
        _arrayLayers = isCubemap ? 6u : 1u;

        // Image creation
        VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = _format;
        imageInfo.extent = {width, height, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = _arrayLayers;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = defaultUsageForAspect(_aspect);
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (isCubemap) {
            imageInfo.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        }

        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;

        VkResult result = vmaCreateImage(_allocator, &imageInfo, &allocInfo,
            &_image, &_allocation, nullptr);
        if (result != VK_SUCCESS) {
            spdlog::error("VulkanTexture: failed to create VkImage ({}x{}, fmt={})",
                width, height, static_cast<int>(_format));
            return;
        }

        _currentLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        // Image view (full-resource view used for sampling).  Render-target
        // attachments use their own per-face / per-mip views owned by
        // VulkanRenderTarget, so this sampling view is always the full image.
        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = _image;
        viewInfo.viewType = isCubemap ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = _format;
        viewInfo.subresourceRange.aspectMask = _aspect;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = _arrayLayers;
        vkCreateImageView(_vkDevice, &viewInfo, nullptr, &_imageView);

        // Sampler — only meaningful for color textures, but harmless on depth.
        createSampler(vkDev);

        // Upload pixel data if available (color textures only — depth/stencil
        // formats are populated by render passes, never by host data).
        const bool haveLevelData = !isDepth && _owner->hasLevels() &&
                                   _owner->getLevel(0) != nullptr;

        if (haveLevelData) {
            const size_t dataSize = _owner->getLevelDataSize(0, 0);
            const void* data = _owner->getLevel(0);

            VkBuffer stagingBuffer = VK_NULL_HANDLE;
            VmaAllocation stagingAlloc = VK_NULL_HANDLE;

            VkBufferCreateInfo stagingInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            stagingInfo.size = dataSize;
            stagingInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

            VmaAllocationCreateInfo stagingAllocInfo{};
            stagingAllocInfo.usage = VMA_MEMORY_USAGE_CPU_ONLY;

            vmaCreateBuffer(_allocator, &stagingInfo, &stagingAllocInfo,
                &stagingBuffer, &stagingAlloc, nullptr);

            void* mapped = nullptr;
            vmaMapMemory(_allocator, stagingAlloc, &mapped);
            memcpy(mapped, data, dataSize);
            vmaUnmapMemory(_allocator, stagingAlloc);

            vulkanImmediateSubmit(vkDev, [&](VkCommandBuffer cmd) {
                vulkanTransitionImageLayout(cmd, _image,
                    VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    _aspect, 0, 1, 0, _arrayLayers);

                VkBufferImageCopy region{};
                region.imageSubresource = {_aspect, 0, 0, 1};
                region.imageExtent = {width, height, 1};
                vkCmdCopyBufferToImage(cmd, stagingBuffer, _image,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

                vulkanTransitionImageLayout(cmd, _image,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    _aspect, 0, 1, 0, _arrayLayers);
            });

            vmaDestroyBuffer(_allocator, stagingBuffer, stagingAlloc);
            _currentLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        } else {
            // No host data — leave the image in UNDEFINED so the first
            // render-target use can cleanly transition it to the appropriate
            // attachment layout (UNDEFINED→COLOR/DEPTH_ATTACHMENT discards
            // contents, which is what we want for a fresh RT).
            _currentLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        }
    }

    void VulkanTexture::propertyChanged(uint32_t flag)
    {
        (void)flag;
        // Filter/address mode changes are reflected in the sampler.  Recreate
        // it lazily — the texture object reference doesn't change, but the
        // VkSampler handle does, so callers that cache descriptor sets must
        // re-write them.  In our descriptor-pool-reset-per-frame model the
        // next frame's descriptors will pick up the new sampler.
        if (_vkDevice == VK_NULL_HANDLE || _owner == nullptr) {
            return;
        }
        // We need a VulkanGraphicsDevice* to recreate the sampler.  The owner
        // Texture exposes its GraphicsDevice — cast to the Vulkan flavour.
        auto* dev = dynamic_cast<VulkanGraphicsDevice*>(_owner->device());
        if (!dev) return;
        destroySampler();
        createSampler(dev);
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

    void VulkanTexture::destroySampler()
    {
        if (_sampler != VK_NULL_HANDLE && _vkDevice != VK_NULL_HANDLE) {
            vkDestroySampler(_vkDevice, _sampler, nullptr);
            _sampler = VK_NULL_HANDLE;
        }
    }
}

#endif // VISUTWIN_HAS_VULKAN
