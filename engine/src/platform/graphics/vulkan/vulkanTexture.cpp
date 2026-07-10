// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//

#ifdef VISUTWIN_HAS_VULKAN

#include "vulkanTexture.h"
#include "vulkanGraphicsDevice.h"
#include "vulkanUtils.h"

#include <algorithm>
#include <cmath>
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
        // Device already destroyed (static-cache teardown at exit): its child
        // objects died with it — touching it would crash.
        if (_deviceRef && _deviceAlive.expired()) {
            return;
        }

        // Defer through the device: an in-flight frame's descriptors may still
        // reference this image/view/sampler.
        if (_deviceRef) {
            _deviceRef->deferDestroy(
                [vkDevice = _vkDevice, allocator = _allocator, image = _image,
                 allocation = _allocation, view = _imageView, sampler = _sampler] {
                    if (vkDevice != VK_NULL_HANDLE) {
                        if (sampler != VK_NULL_HANDLE) vkDestroySampler(vkDevice, sampler, nullptr);
                        if (view != VK_NULL_HANDLE) vkDestroyImageView(vkDevice, view, nullptr);
                    }
                    if (allocator != VK_NULL_HANDLE && image != VK_NULL_HANDLE) {
                        vmaDestroyImage(allocator, image, allocation);
                    }
                });
            return;
        }

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
        _deviceRef = vkDev;
        _deviceAlive = vkDev->aliveToken();
        _vkDevice = vkDev->device();
        _allocator = vkDev->vmaAllocator();

        const uint32_t width = _owner->width();
        const uint32_t height = _owner->height();
        _format = vulkanMapPixelFormat(_owner->format());
        if (_format == VK_FORMAT_D24_UNORM_S8_UINT) {
            // Not supported by MoltenVK on Apple GPUs — probe for a fallback.
            _format = vulkanSupportedDepthStencilFormat(vkDev->physicalDevice());
        }
        _aspect = aspectForFormat(_format);
        const bool isDepth = formatIsDepth(_format);
        const bool isCubemap = _owner->isCubemap();
        _arrayLayers = isCubemap ? 6u : 1u;

        // Full mip chain for textures that request mipmaps — Metal has had
        // this from the start; single-mip sampling caused heavy minification
        // aliasing on ground/terrain textures. Requires blit + linear-filter
        // support for the format (probe, fall back to 1 level).
        _mipLevels = 1;
        if (_owner->mipmaps() && !isDepth && width > 1 && height > 1) {
            VkFormatProperties formatProps{};
            vkGetPhysicalDeviceFormatProperties(vkDev->physicalDevice(), _format, &formatProps);
            const VkFormatFeatureFlags needed = VK_FORMAT_FEATURE_BLIT_SRC_BIT |
                VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
            if ((formatProps.optimalTilingFeatures & needed) == needed) {
                _mipLevels = 1 + static_cast<uint32_t>(
                    std::floor(std::log2(static_cast<float>(std::max(width, height)))));
            }
        }

        // Image creation
        VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = _format;
        imageInfo.extent = {width, height, 1};
        imageInfo.mipLevels = _mipLevels;
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
        viewInfo.subresourceRange.levelCount = _mipLevels;
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
            // Gather level-0 data for every available layer — cubemaps store
            // one buffer per face, and a single layer-0 copy would leave
            // faces 1-5 with undefined contents.
            struct LayerData { const void* data; size_t size; };
            std::vector<LayerData> layers;
            size_t totalSize = 0;
            for (uint32_t layer = 0; layer < _arrayLayers; ++layer) {
                const void* layerData = _owner->getLevel(0, layer);
                const size_t layerSize = _owner->getLevelDataSize(0, layer);
                if (!layerData || layerSize == 0) {
                    break; // upload the contiguous prefix of populated faces
                }
                layers.push_back({layerData, layerSize});
                totalSize += layerSize;
            }

            VkBuffer stagingBuffer = VK_NULL_HANDLE;
            VmaAllocation stagingAlloc = VK_NULL_HANDLE;

            VkBufferCreateInfo stagingInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            stagingInfo.size = totalSize;
            stagingInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

            VmaAllocationCreateInfo stagingAllocInfo{};
            stagingAllocInfo.usage = VMA_MEMORY_USAGE_CPU_ONLY;

            vmaCreateBuffer(_allocator, &stagingInfo, &stagingAllocInfo,
                &stagingBuffer, &stagingAlloc, nullptr);

            std::vector<VkBufferImageCopy> regions;
            regions.reserve(layers.size());
            {
                void* mapped = nullptr;
                vmaMapMemory(_allocator, stagingAlloc, &mapped);
                size_t offset = 0;
                for (uint32_t layer = 0; layer < layers.size(); ++layer) {
                    memcpy(static_cast<uint8_t*>(mapped) + offset, layers[layer].data, layers[layer].size);

                    VkBufferImageCopy region{};
                    region.bufferOffset = offset;
                    region.imageSubresource = {_aspect, 0, layer, 1};
                    region.imageExtent = {width, height, 1};
                    regions.push_back(region);

                    offset += layers[layer].size;
                }
                vmaUnmapMemory(_allocator, stagingAlloc);
            }

            vulkanImmediateSubmit(vkDev, [&](VkCommandBuffer cmd) {
                vulkanTransitionImageLayout(cmd, _image,
                    VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    _aspect, 0, _mipLevels, 0, _arrayLayers);

                vkCmdCopyBufferToImage(cmd, stagingBuffer, _image,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    static_cast<uint32_t>(regions.size()), regions.data());

                // Generate the mip chain: blit each level from the previous
                // one (all layers per blit), stepping the source level into
                // TRANSFER_SRC as we descend.
                int32_t mipW = static_cast<int32_t>(width);
                int32_t mipH = static_cast<int32_t>(height);
                for (uint32_t level = 1; level < _mipLevels; ++level) {
                    vulkanTransitionImageLayout(cmd, _image,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        _aspect, level - 1, 1, 0, _arrayLayers);

                    const int32_t nextW = std::max(mipW / 2, 1);
                    const int32_t nextH = std::max(mipH / 2, 1);

                    VkImageBlit blit{};
                    blit.srcSubresource = {_aspect, level - 1, 0, _arrayLayers};
                    blit.srcOffsets[1] = {mipW, mipH, 1};
                    blit.dstSubresource = {_aspect, level, 0, _arrayLayers};
                    blit.dstOffsets[1] = {nextW, nextH, 1};
                    vkCmdBlitImage(cmd,
                        _image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        _image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        1, &blit, VK_FILTER_LINEAR);

                    mipW = nextW;
                    mipH = nextH;
                }

                // Finalize: levels [0, N-1) are TRANSFER_SRC, the last level
                // is still TRANSFER_DST (or level 0 when no mips were made).
                if (_mipLevels > 1) {
                    vulkanTransitionImageLayout(cmd, _image,
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        _aspect, 0, _mipLevels - 1, 0, _arrayLayers);
                }
                vulkanTransitionImageLayout(cmd, _image,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    _aspect, _mipLevels - 1, 1, 0, _arrayLayers);
            });

            vmaDestroyBuffer(_allocator, stagingBuffer, stagingAlloc);
            _currentLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        } else {
            // No host data — but the image must still be in a defined layout
            // before *any* shader can sample it (e.g. as a default-bound slot)
            // and before any descriptor that references its view is allowed
            // to be in flight.  Transition to SHADER_READ_ONLY here; the
            // first render-target use will transition to the appropriate
            // attachment layout, which is fine because LOAD_OP_CLEAR /
            // LOAD_OP_DONT_CARE discard the contents anyway.
            vulkanImmediateSubmit(vkDev, [&](VkCommandBuffer cmd) {
                vulkanTransitionImageLayout(cmd, _image,
                    VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    _aspect, 0, _mipLevels, 0, _arrayLayers);
            });
            _currentLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
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
        // Defer the old sampler's destruction — descriptors already written
        // this frame (or in-flight frames) may still reference it.
        if (_sampler != VK_NULL_HANDLE) {
            dev->deferDestroy([vkDevice = _vkDevice, sampler = _sampler] {
                vkDestroySampler(vkDevice, sampler, nullptr);
            });
            _sampler = VK_NULL_HANDLE;
        }
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
        // 16x anisotropy (or the device max) — matches the Metal default
        // sampler; without it oblique ground textures smear into radial lines.
        const float anisotropy = device->maxSamplerAnisotropy();
        samplerInfo.anisotropyEnable = anisotropy > 1.0f ? VK_TRUE : VK_FALSE;
        samplerInfo.maxAnisotropy = std::max(anisotropy, 1.0f);

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
