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
        // Map a VkFormat to every image aspect it owns. Combined
        // depth-stencil resources must retain both aspects for attachment
        // transitions and stencil clear/store operations.
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

        if (_image != VK_NULL_HANDLE) {
            const VkImage oldImage = _image;
            const VmaAllocation oldAllocation = _allocation;
            const VkImageView oldView = _imageView;
            const VkSampler oldSampler = _sampler;
            vkDev->deferDestroy(
                [vkDevice = _vkDevice, allocator = _allocator,
                 oldImage, oldAllocation, oldView, oldSampler] {
                    if (oldSampler != VK_NULL_HANDLE)
                        vkDestroySampler(vkDevice, oldSampler, nullptr);
                    if (oldView != VK_NULL_HANDLE)
                        vkDestroyImageView(vkDevice, oldView, nullptr);
                    vmaDestroyImage(allocator, oldImage, oldAllocation);
                });
            _image = VK_NULL_HANDLE;
            _allocation = VK_NULL_HANDLE;
            _imageView = VK_NULL_HANDLE;
            _sampler = VK_NULL_HANDLE;
        }

        const uint32_t width = _owner->width();
        const uint32_t height = _owner->height();
        _format = vulkanMapPixelFormat(_owner->format());
        if (_format == VK_FORMAT_UNDEFINED) {
            spdlog::error("VulkanTexture: pixel format {} has no Vulkan mapping",
                static_cast<uint32_t>(_owner->format()));
            return;
        }
        if (_format == VK_FORMAT_D24_UNORM_S8_UINT) {
            // Not supported by MoltenVK on Apple GPUs — probe for a fallback.
            _format = vulkanSupportedDepthStencilFormat(vkDev->physicalDevice());
        } else if (_format == VK_FORMAT_D32_SFLOAT ||
                   _format == VK_FORMAT_D16_UNORM) {
            VkFormatProperties depthProperties{};
            vkGetPhysicalDeviceFormatProperties(
                vkDev->physicalDevice(), _format, &depthProperties);
            if (!(depthProperties.optimalTilingFeatures &
                  VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
                _format = vulkanSupportedDepthFormat(vkDev->physicalDevice());
            }
        }
        if (_format == VK_FORMAT_UNDEFINED) {
            spdlog::error(
                "VulkanTexture: no supported Vulkan format for pixel format {}",
                static_cast<uint32_t>(_owner->format()));
            return;
        }
        _aspect = aspectForFormat(_format);
        const bool isDepth = formatIsDepth(_format);
        const bool isCubemap = _owner->isCubemap();
        // Array textures (the clustered spot-shadow atlas is one) carry their slice
        // count in arrayLength. Ignoring it created a single-layer VkImage, and every
        // per-slice render target then tried to carve an attachment view at
        // baseArrayLayer >= 1 — invalid, and the slices had nowhere to render.
        _arrayLayers = isCubemap
            ? 6u
            : std::max(1u, _owner->getArrayLength());
        _mipLevels = std::max(1u, _owner->getNumLevels());

        VkFormatProperties formatProperties{};
        vkGetPhysicalDeviceFormatProperties(
            vkDev->physicalDevice(), _format, &formatProperties);
        const auto optimalFeatures = formatProperties.optimalTilingFeatures;
        _supportsLinearSampling =
            (optimalFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
        _supportsLinearBlit = !isDepth &&
            !isCompressedPixelFormat(_owner->format()) &&
            (optimalFeatures & (VK_FORMAT_FEATURE_BLIT_SRC_BIT |
                                VK_FORMAT_FEATURE_BLIT_DST_BIT |
                                VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)) ==
                (VK_FORMAT_FEATURE_BLIT_SRC_BIT |
                 VK_FORMAT_FEATURE_BLIT_DST_BIT |
                 VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
        bool ownerHasHigherMips = false;
        for (uint32_t layer = 0; layer < _arrayLayers && !ownerHasHigherMips; ++layer) {
            for (uint32_t mip = 1; mip < _mipLevels; ++mip) {
                ownerHasHigherMips =
                    _owner->getLevel(mip, layer) != nullptr &&
                    _owner->getLevelDataSize(mip, layer) != 0;
                if (ownerHasHigherMips) break;
            }
        }
        if (_mipLevels > 1 && !_supportsLinearBlit && !ownerHasHigherMips) {
            spdlog::warn(
                "VulkanTexture: format {} cannot generate mipmaps; using level 0 only",
                static_cast<int>(_format));
            _mipLevels = 1;
        }

        const VkImageCreateFlags imageFlags = isCubemap
            ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
        _supportsColorAttachment = !isDepth &&
            (optimalFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT);
        VkImageUsageFlags imageUsage = VK_IMAGE_USAGE_SAMPLED_BIT |
            VK_IMAGE_USAGE_TRANSFER_DST_BIT |
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        if (isDepth) {
            imageUsage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        } else if (_supportsColorAttachment) {
            imageUsage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        }
        if (_owner->storage()) {
            imageUsage |= VK_IMAGE_USAGE_STORAGE_BIT;
        }

        VkFormatFeatureFlags requiredFeatures = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        if (isDepth) {
            requiredFeatures |= VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
        }
        if (_owner->storage()) {
            requiredFeatures |= VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
        }
        if (!vulkanFormatSupportsImage(vkDev->physicalDevice(), _format,
                VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, imageUsage,
                imageFlags, requiredFeatures, {width, height, 1},
                _mipLevels, _arrayLayers, VK_SAMPLE_COUNT_1_BIT)) {
            spdlog::error(
                "VulkanTexture: format {} does not support requested usage {:#x}{}",
                static_cast<int>(_format), static_cast<uint32_t>(imageUsage),
                isCubemap ? " as a cubemap" : "");
            return;
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
        imageInfo.usage = imageUsage;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageInfo.flags = imageFlags;

        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;

        VkResult result = vmaCreateImage(_allocator, &imageInfo, &allocInfo,
            &_image, &_allocation, nullptr);
        if (result != VK_SUCCESS) {
            spdlog::error("VulkanTexture: failed to create VkImage ({}x{}, fmt={})",
                width, height, static_cast<int>(_format));
            return;
        }

        _subresourceLayouts.assign(
            static_cast<size_t>(_mipLevels) * _arrayLayers,
            VK_IMAGE_LAYOUT_UNDEFINED);

        // Image view (full-resource view used for sampling).  Render-target
        // attachments use their own per-face / per-mip views owned by
        // VulkanRenderTarget, so this sampling view is always the full image.
        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = _image;
        // A VIEW_TYPE_2D view may only cover one layer, so a multi-layer image
        // needs the array view type to match layerCount below.
        viewInfo.viewType = isCubemap
            ? VK_IMAGE_VIEW_TYPE_CUBE
            : (_arrayLayers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY
                                : VK_IMAGE_VIEW_TYPE_2D);
        viewInfo.format = _format;
        viewInfo.subresourceRange.aspectMask = _aspect;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = _mipLevels;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = _arrayLayers;
        if (vkCreateImageView(_vkDevice, &viewInfo, nullptr, &_imageView) !=
            VK_SUCCESS) {
            spdlog::error("VulkanTexture: failed to create image view");
            vmaDestroyImage(_allocator, _image, _allocation);
            _image = VK_NULL_HANDLE;
            _allocation = VK_NULL_HANDLE;
            return;
        }

        // Sampler — only meaningful for color textures, but harmless on depth.
        if (!createSampler(vkDev, _sampler)) {
            vkDestroyImageView(_vkDevice, _imageView, nullptr);
            vmaDestroyImage(_allocator, _image, _allocation);
            _imageView = VK_NULL_HANDLE;
            _image = VK_NULL_HANDLE;
            _allocation = VK_NULL_HANDLE;
            _subresourceLayouts.clear();
            return;
        }

        struct UploadData
        {
            const void* data = nullptr;
            size_t size = 0;
            uint32_t mipLevel = 0;
            uint32_t layer = 0;
            uint32_t width = 1;
            uint32_t height = 1;
        };
        std::vector<UploadData> uploads;
        size_t totalSize = 0;
        bool allBaseLayersPresent = !isDepth;
        bool hasExplicitHigherMips = false;

        if (!isDepth) {
            const PixelFormat pixelFormat = _owner->format();
            const bool compressed = isCompressedPixelFormat(pixelFormat);
            const uint32_t blockBytes = compressedPixelFormatBlockSize(pixelFormat);
            const uint32_t blockWidth = compressedPixelFormatBlockWidth(pixelFormat);
            const uint32_t blockHeight = compressedPixelFormatBlockHeight(pixelFormat);
            const uint32_t bytesPerPixel = pixelFormatBytesPerPixel(pixelFormat);

            for (uint32_t layer = 0; layer < _arrayLayers; ++layer) {
                bool basePresent = false;
                for (uint32_t mip = 0; mip < _mipLevels; ++mip) {
                    const void* source = _owner->getLevel(mip, layer);
                    const size_t sourceSize = _owner->getLevelDataSize(mip, layer);
                    if (!source || sourceSize == 0) {
                        continue;
                    }

                    const uint32_t mipWidth = std::max(width >> mip, 1u);
                    const uint32_t mipHeight = std::max(height >> mip, 1u);
                    const size_t expectedSize = compressed
                        ? static_cast<size_t>((mipWidth + blockWidth - 1) / blockWidth) *
                          ((mipHeight + blockHeight - 1) / blockHeight) * blockBytes
                        : static_cast<size_t>(mipWidth) * mipHeight * bytesPerPixel;
                    if (expectedSize == 0 || sourceSize < expectedSize) {
                        spdlog::error(
                            "VulkanTexture: subresource face={} mip={} has {} bytes, expected at least {}",
                            layer, mip, sourceSize, expectedSize);
                        continue;
                    }

                    totalSize = (totalSize + 3u) & ~size_t(3u);
                    uploads.push_back(
                        {source, expectedSize, mip, layer, mipWidth, mipHeight});
                    totalSize += expectedSize;
                    basePresent |= mip == 0;
                    hasExplicitHigherMips |= mip > 0;
                }
                allBaseLayersPresent &= basePresent;
            }
        }

        if (!uploads.empty()) {
            VkBuffer stagingBuffer = VK_NULL_HANDLE;
            VmaAllocation stagingAlloc = VK_NULL_HANDLE;

            VkBufferCreateInfo stagingInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            stagingInfo.size = totalSize;
            stagingInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

            VmaAllocationCreateInfo stagingAllocInfo{};
            stagingAllocInfo.usage = VMA_MEMORY_USAGE_CPU_ONLY;

            if (vmaCreateBuffer(_allocator, &stagingInfo, &stagingAllocInfo,
                    &stagingBuffer, &stagingAlloc, nullptr) != VK_SUCCESS) {
                spdlog::error(
                    "VulkanTexture: failed to allocate {}-byte staging buffer",
                    totalSize);
                destroySampler();
                vkDestroyImageView(_vkDevice, _imageView, nullptr);
                _imageView = VK_NULL_HANDLE;
                vmaDestroyImage(_allocator, _image, _allocation);
                _image = VK_NULL_HANDLE;
                _allocation = VK_NULL_HANDLE;
                return;
            }

            std::vector<VkBufferImageCopy> regions;
            regions.reserve(uploads.size());
            {
                void* mapped = nullptr;
                if (vmaMapMemory(_allocator, stagingAlloc, &mapped) != VK_SUCCESS) {
                    spdlog::error("VulkanTexture: failed to map staging buffer");
                    vmaDestroyBuffer(_allocator, stagingBuffer, stagingAlloc);
                    destroySampler();
                    vkDestroyImageView(_vkDevice, _imageView, nullptr);
                    _imageView = VK_NULL_HANDLE;
                    vmaDestroyImage(_allocator, _image, _allocation);
                    _image = VK_NULL_HANDLE;
                    _allocation = VK_NULL_HANDLE;
                    return;
                }
                size_t offset = 0;
                for (const auto& upload : uploads) {
                    offset = (offset + 3u) & ~size_t(3u);
                    memcpy(static_cast<uint8_t*>(mapped) + offset,
                        upload.data, upload.size);

                    VkBufferImageCopy region{};
                    region.bufferOffset = offset;
                    region.imageSubresource = {
                        _aspect, upload.mipLevel, upload.layer, 1};
                    region.imageExtent = {upload.width, upload.height, 1};
                    regions.push_back(region);

                    offset += upload.size;
                }
                vmaUnmapMemory(_allocator, stagingAlloc);
            }

            const VkImage image = _image;
            const VkImageAspectFlags aspect = _aspect;
            const uint32_t mipLevels = _mipLevels;
            const uint32_t arrayLayers = _arrayLayers;
            const bool generate = _supportsLinearBlit &&
                mipLevels > 1 && allBaseLayersPresent && !hasExplicitHigherMips;
            vkDev->enqueueUpload(
                [image, aspect, mipLevels, arrayLayers, width, height,
                 stagingBuffer, regions = std::move(regions), generate](VkCommandBuffer cmd) {
                vulkanTransitionImageLayout(cmd, image,
                    VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    aspect, 0, mipLevels, 0, arrayLayers);

                vkCmdCopyBufferToImage(cmd, stagingBuffer, image,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    static_cast<uint32_t>(regions.size()), regions.data());

                if (generate) {
                    int32_t mipW = static_cast<int32_t>(width);
                    int32_t mipH = static_cast<int32_t>(height);
                    for (uint32_t level = 1; level < mipLevels; ++level) {
                        vulkanTransitionImageLayout(cmd, image,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        aspect, level - 1, 1, 0, arrayLayers);

                        const int32_t nextW = std::max(mipW / 2, 1);
                        const int32_t nextH = std::max(mipH / 2, 1);

                        VkImageBlit blit{};
                        blit.srcSubresource = {aspect, level - 1, 0, arrayLayers};
                        blit.srcOffsets[1] = {mipW, mipH, 1};
                        blit.dstSubresource = {aspect, level, 0, arrayLayers};
                        blit.dstOffsets[1] = {nextW, nextH, 1};
                        vkCmdBlitImage(cmd,
                            image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                            image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            1, &blit, VK_FILTER_LINEAR);

                        mipW = nextW;
                        mipH = nextH;
                    }

                    vulkanTransitionImageLayout(cmd, image,
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        aspect, 0, mipLevels - 1, 0, arrayLayers);
                    vulkanTransitionImageLayout(cmd, image,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        aspect, mipLevels - 1, 1, 0, arrayLayers);
                } else {
                    vulkanTransitionImageLayout(cmd, image,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        aspect, 0, mipLevels, 0, arrayLayers);
                }
            }, [allocator = _allocator, stagingBuffer, stagingAlloc] {
                vmaDestroyBuffer(allocator, stagingBuffer, stagingAlloc);
            });
        } else {
            // No host data — but the image must still be in a defined layout
            // before *any* shader can sample it (e.g. as a default-bound slot)
            // and before any descriptor that references its view is allowed
            // to be in flight.  Transition to SHADER_READ_ONLY here; the
            // first render-target use will transition to the appropriate
            // attachment layout, which is fine because LOAD_OP_CLEAR /
            // LOAD_OP_DONT_CARE discard the contents anyway.
            const VkImage image = _image;
            const VkImageAspectFlags aspect = _aspect;
            const uint32_t mipLevels = _mipLevels;
            const uint32_t arrayLayers = _arrayLayers;
            vkDev->enqueueUpload([image, aspect, mipLevels, arrayLayers](VkCommandBuffer cmd) {
                vulkanTransitionImageLayout(cmd, image,
                    VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    aspect, 0, mipLevels, 0, arrayLayers);
            });
        }
        std::fill(_subresourceLayouts.begin(), _subresourceLayouts.end(),
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }

    VkImageLayout VulkanTexture::layout(
        const uint32_t mipLevel, const uint32_t arrayLayer) const
    {
        if (mipLevel >= _mipLevels || arrayLayer >= _arrayLayers ||
            _subresourceLayouts.empty()) {
            return VK_IMAGE_LAYOUT_UNDEFINED;
        }
        return _subresourceLayouts[
            static_cast<size_t>(arrayLayer) * _mipLevels + mipLevel];
    }

    void VulkanTexture::transitionLayout(VkCommandBuffer commandBuffer,
        const VkImageLayout newLayout, const uint32_t baseMipLevel,
        uint32_t levelCount, const uint32_t baseArrayLayer,
        uint32_t layerCount)
    {
        if (_image == VK_NULL_HANDLE || baseMipLevel >= _mipLevels ||
            baseArrayLayer >= _arrayLayers) {
            return;
        }
        if (levelCount == VK_REMAINING_MIP_LEVELS) {
            levelCount = _mipLevels - baseMipLevel;
        }
        if (layerCount == VK_REMAINING_ARRAY_LAYERS) {
            layerCount = _arrayLayers - baseArrayLayer;
        }
        levelCount = std::min(levelCount, _mipLevels - baseMipLevel);
        layerCount = std::min(layerCount, _arrayLayers - baseArrayLayer);

        // Layouts may differ between faces and mips, so barriers are emitted
        // only for the exact subresources that need changing.
        for (uint32_t layer = baseArrayLayer;
             layer < baseArrayLayer + layerCount; ++layer) {
            for (uint32_t mip = baseMipLevel;
                 mip < baseMipLevel + levelCount; ++mip) {
                auto& oldLayout = _subresourceLayouts[
                    static_cast<size_t>(layer) * _mipLevels + mip];
                if (oldLayout == newLayout) {
                    continue;
                }
                vulkanTransitionImageLayout(commandBuffer, _image,
                    oldLayout, newLayout, _aspect, mip, 1, layer, 1);
                oldLayout = newLayout;
            }
        }
    }

    bool VulkanTexture::generateMipmaps(VkCommandBuffer commandBuffer,
        const uint32_t baseArrayLayer, uint32_t layerCount)
    {
        if (!_supportsLinearBlit || _mipLevels <= 1 ||
            baseArrayLayer >= _arrayLayers) {
            return false;
        }
        if (layerCount == VK_REMAINING_ARRAY_LAYERS) {
            layerCount = _arrayLayers - baseArrayLayer;
        }
        layerCount = std::min(layerCount, _arrayLayers - baseArrayLayer);

        transitionLayout(commandBuffer, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1, _mipLevels - 1, baseArrayLayer, layerCount);

        int32_t mipWidth = static_cast<int32_t>(_owner->width());
        int32_t mipHeight = static_cast<int32_t>(_owner->height());
        for (uint32_t mip = 1; mip < _mipLevels; ++mip) {
            transitionLayout(commandBuffer, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                mip - 1, 1, baseArrayLayer, layerCount);

            const int32_t nextWidth = std::max(mipWidth / 2, 1);
            const int32_t nextHeight = std::max(mipHeight / 2, 1);
            VkImageBlit blit{};
            blit.srcSubresource = {
                _aspect, mip - 1, baseArrayLayer, layerCount};
            blit.srcOffsets[1] = {mipWidth, mipHeight, 1};
            blit.dstSubresource = {
                _aspect, mip, baseArrayLayer, layerCount};
            blit.dstOffsets[1] = {nextWidth, nextHeight, 1};
            vkCmdBlitImage(commandBuffer,
                _image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                _image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                1, &blit, VK_FILTER_LINEAR);
            mipWidth = nextWidth;
            mipHeight = nextHeight;
        }

        transitionLayout(commandBuffer, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            0, _mipLevels, baseArrayLayer, layerCount);
        return true;
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
        VkSampler replacement = VK_NULL_HANDLE;
        if (!createSampler(dev, replacement)) {
            return;
        }

        // Defer the old sampler's destruction — descriptors already written
        // this frame (or in-flight frames) may still reference it.
        const VkSampler oldSampler = _sampler;
        _sampler = replacement;
        if (oldSampler != VK_NULL_HANDLE) {
            dev->deferDestroy([vkDevice = _vkDevice, sampler = oldSampler] {
                vkDestroySampler(vkDevice, sampler, nullptr);
            });
        }
    }

    bool VulkanTexture::createSampler(
        VulkanGraphicsDevice* device, VkSampler& sampler) const
    {
        VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        const VkFilter requestedMag = vulkanMapFilterMode(_owner->magFilter());
        const VkFilter requestedMin = vulkanMapFilterMode(_owner->minFilter());
        samplerInfo.magFilter = _supportsLinearSampling
            ? requestedMag : VK_FILTER_NEAREST;
        samplerInfo.minFilter = _supportsLinearSampling
            ? requestedMin : VK_FILTER_NEAREST;
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

        const VkResult result =
            vkCreateSampler(device->device(), &samplerInfo, nullptr, &sampler);
        if (result != VK_SUCCESS) {
            sampler = VK_NULL_HANDLE;
            spdlog::error(
                "VulkanTexture: failed to create sampler ({})",
                static_cast<int>(result));
            return false;
        }
        return true;
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
