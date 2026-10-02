// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 21.03.2026
//

#ifdef VISUTWIN_HAS_VULKAN

#include "vulkanTexture.h"
#include "vulkanGraphicsDevice.h"
#include "vulkanUtils.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>
#include <vector>
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

    namespace
    {
        /// One subresource of host data to copy into the image.
        struct SubresourceUpload
        {
            const void* data = nullptr;
            size_t size = 0;
            uint32_t mipLevel = 0;
            uint32_t layer = 0;
            uint32_t width = 1;
            uint32_t height = 1;
        };

        /// The host data a texture carries, laid out for one staging buffer.
        struct HostUploads
        {
            std::vector<SubresourceUpload> uploads;
            // Bytes the staging buffer needs, each upload 4-byte aligned.
            size_t totalSize = 0;
            bool allBaseLayersPresent = false;
            bool hasExplicitHigherMips = false;
        };

        size_t alignTo4(const size_t value)
        {
            return (value + 3u) & ~size_t(3u);
        }

        /// The size one mip level of the owner's format needs, block-rounded when the
        /// format is compressed.
        size_t expectedLevelSize(const PixelFormat pixelFormat, const uint32_t mipWidth, const uint32_t mipHeight)
        {
            if (isCompressedPixelFormat(pixelFormat)) {
                const uint32_t blockWidth = compressedPixelFormatBlockWidth(pixelFormat);
                const uint32_t blockHeight = compressedPixelFormatBlockHeight(pixelFormat);
                return static_cast<size_t>((mipWidth + blockWidth - 1) / blockWidth) *
                    ((mipHeight + blockHeight - 1) / blockHeight) * compressedPixelFormatBlockSize(pixelFormat);
            }
            return static_cast<size_t>(mipWidth) * mipHeight * pixelFormatBytesPerPixel(pixelFormat);
        }

        /// Every subresource the owner holds host data for. A depth texture never
        /// carries any. A level shorter than its format needs is reported and skipped.
        HostUploads collectHostUploads(const Texture& owner, const bool isDepth,
            const uint32_t arrayLayers, const uint32_t mipLevels)
        {
            HostUploads result;
            result.allBaseLayersPresent = !isDepth;
            if (isDepth) {
                return result;
            }

            const PixelFormat pixelFormat = owner.format();
            const uint32_t width = owner.width();
            const uint32_t height = owner.height();
            for (uint32_t layer = 0; layer < arrayLayers; ++layer) {
                bool basePresent = false;
                for (uint32_t mip = 0; mip < mipLevels; ++mip) {
                    const void* source = owner.getLevel(mip, layer);
                    const size_t sourceSize = owner.getLevelDataSize(mip, layer);
                    if (!source || sourceSize == 0) {
                        continue;
                    }

                    const uint32_t mipWidth = std::max(width >> mip, 1u);
                    const uint32_t mipHeight = std::max(height >> mip, 1u);
                    const size_t expectedSize = expectedLevelSize(pixelFormat, mipWidth, mipHeight);
                    if (expectedSize == 0 || sourceSize < expectedSize) {
                        spdlog::error(
                            "VulkanTexture: subresource face={} mip={} has {} bytes, expected at least {}",
                            layer, mip, sourceSize, expectedSize);
                        continue;
                    }

                    result.totalSize = alignTo4(result.totalSize);
                    result.uploads.push_back({source, expectedSize, mip, layer, mipWidth, mipHeight});
                    result.totalSize += expectedSize;
                    basePresent |= mip == 0;
                    result.hasExplicitHigherMips |= mip > 0;
                }
                result.allBaseLayersPresent &= basePresent;
            }
            return result;
        }

        /// A filled staging buffer and the copy regions that read it.
        struct StagedUploads
        {
            VkBuffer buffer = VK_NULL_HANDLE;
            VmaAllocation allocation = VK_NULL_HANDLE;
            std::vector<VkBufferImageCopy> regions;
        };

        /// Copies the host data into a new CPU-visible staging buffer. Nothing is left
        /// allocated when it fails.
        std::optional<StagedUploads> stageHostUploads(VmaAllocator allocator, const VkImageAspectFlags aspect,
            const HostUploads& host)
        {
            StagedUploads staged;
            VkBufferCreateInfo stagingInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            stagingInfo.size = host.totalSize;
            stagingInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
            VmaAllocationCreateInfo stagingAllocInfo{};
            stagingAllocInfo.usage = VMA_MEMORY_USAGE_CPU_ONLY;
            if (vmaCreateBuffer(allocator, &stagingInfo, &stagingAllocInfo,
                    &staged.buffer, &staged.allocation, nullptr) != VK_SUCCESS) {
                spdlog::error("VulkanTexture: failed to allocate {}-byte staging buffer", host.totalSize);
                return std::nullopt;
            }

            void* mapped = nullptr;
            if (vmaMapMemory(allocator, staged.allocation, &mapped) != VK_SUCCESS) {
                spdlog::error("VulkanTexture: failed to map staging buffer");
                vmaDestroyBuffer(allocator, staged.buffer, staged.allocation);
                return std::nullopt;
            }
            staged.regions.reserve(host.uploads.size());
            size_t offset = 0;
            for (const auto& upload : host.uploads) {
                offset = alignTo4(offset);
                memcpy(static_cast<uint8_t*>(mapped) + offset, upload.data, upload.size);

                VkBufferImageCopy region{};
                region.bufferOffset = offset;
                region.imageSubresource = {aspect, upload.mipLevel, upload.layer, 1};
                region.imageExtent = {upload.width, upload.height, 1};
                staged.regions.push_back(region);

                offset += upload.size;
            }
            vmaUnmapMemory(allocator, staged.allocation);
            return staged;
        }

        /// What the recorded upload works on, captured by value: the commands run
        /// later, when the device flushes its upload queue.
        struct UploadTarget
        {
            VkImage image = VK_NULL_HANDLE;
            VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
            uint32_t mipLevels = 1;
            uint32_t arrayLayers = 1;
            uint32_t width = 1;
            uint32_t height = 1;
        };

        /// Fills levels 1..n-1 from level 0 by successive linear blits, leaving the
        /// whole chain in SHADER_READ_ONLY. Level 0 enters in TRANSFER_DST.
        void recordMipChainBlits(VkCommandBuffer cmd, const UploadTarget& target)
        {
            int32_t mipW = static_cast<int32_t>(target.width);
            int32_t mipH = static_cast<int32_t>(target.height);
            for (uint32_t level = 1; level < target.mipLevels; ++level) {
                vulkanTransitionImageLayout(cmd, target.image,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    target.aspect, level - 1, 1, 0, target.arrayLayers);

                const int32_t nextW = std::max(mipW / 2, 1);
                const int32_t nextH = std::max(mipH / 2, 1);

                VkImageBlit blit{};
                blit.srcSubresource = {target.aspect, level - 1, 0, target.arrayLayers};
                blit.srcOffsets[1] = {mipW, mipH, 1};
                blit.dstSubresource = {target.aspect, level, 0, target.arrayLayers};
                blit.dstOffsets[1] = {nextW, nextH, 1};
                vkCmdBlitImage(cmd,
                    target.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    target.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    1, &blit, VK_FILTER_LINEAR);

                mipW = nextW;
                mipH = nextH;
            }

            vulkanTransitionImageLayout(cmd, target.image,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                target.aspect, 0, target.mipLevels - 1, 0, target.arrayLayers);
            vulkanTransitionImageLayout(cmd, target.image,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                target.aspect, target.mipLevels - 1, 1, 0, target.arrayLayers);
        }

        /// Copies the staged data into the image, generates the remaining mips when
        /// asked, and leaves every subresource in SHADER_READ_ONLY.
        void recordStagedUpload(VkCommandBuffer cmd, const UploadTarget& target, VkBuffer stagingBuffer,
            const std::vector<VkBufferImageCopy>& regions, const bool generateMips)
        {
            vulkanTransitionImageLayout(cmd, target.image,
                VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                target.aspect, 0, target.mipLevels, 0, target.arrayLayers);

            vkCmdCopyBufferToImage(cmd, stagingBuffer, target.image,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                static_cast<uint32_t>(regions.size()), regions.data());

            if (generateMips) {
                recordMipChainBlits(cmd, target);
            } else {
                vulkanTransitionImageLayout(cmd, target.image,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    target.aspect, 0, target.mipLevels, 0, target.arrayLayers);
            }
        }
    }

    void VulkanTexture::uploadImmediate(GraphicsDevice* device)
    {
        auto* vkDev = static_cast<VulkanGraphicsDevice*>(device);
        _deviceRef = vkDev;
        _deviceAlive = vkDev->aliveToken();
        _vkDevice = vkDev->device();
        _allocator = vkDev->vmaAllocator();

        retireImage();
        if (!resolveFormat(vkDev)) {
            return;
        }
        resolveLevelsAndCapabilities(vkDev);
        if (!createImage(vkDev) || !createImageView()) {
            return;
        }
        // Sampler — only meaningful for color textures, but harmless on depth.
        if (!createSampler(vkDev, _sampler)) {
            releaseImageResources();
            return;
        }

        const UploadTarget target{_image, _aspect, _mipLevels, _arrayLayers, _owner->width(), _owner->height()};
        const HostUploads host = collectHostUploads(*_owner, isDepth(), _arrayLayers, _mipLevels);
        if (!host.uploads.empty()) {
            auto staged = stageHostUploads(_allocator, _aspect, host);
            if (!staged) {
                releaseImageResources();
                return;
            }
            // Mips are generated only when level 0 of every layer arrived and no
            // higher level did: explicit higher levels are the owner's own chain.
            const bool generateMips = _supportsLinearBlit && _mipLevels > 1 &&
                host.allBaseLayersPresent && !host.hasExplicitHigherMips;
            vkDev->enqueueUpload(
                [target, stagingBuffer = staged->buffer, regions = std::move(staged->regions),
                 generateMips](VkCommandBuffer cmd) {
                    recordStagedUpload(cmd, target, stagingBuffer, regions, generateMips);
                },
                [allocator = _allocator, stagingBuffer = staged->buffer, stagingAlloc = staged->allocation] {
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
            vkDev->enqueueUpload([target](VkCommandBuffer cmd) {
                vulkanTransitionImageLayout(cmd, target.image,
                    VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    target.aspect, 0, target.mipLevels, 0, target.arrayLayers);
            });
        }
        std::fill(_subresourceLayouts.begin(), _subresourceLayouts.end(),
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }

    void VulkanTexture::retireImage()
    {
        // A re-upload replaces the image. The old one may still be referenced by a
        // frame in flight, so the device destroys it once that frame has completed.
        if (_image == VK_NULL_HANDLE) {
            return;
        }
        _deviceRef->deferDestroy(
            [vkDevice = _vkDevice, allocator = _allocator,
             oldImage = _image, oldAllocation = _allocation, oldView = _imageView, oldSampler = _sampler] {
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

    bool VulkanTexture::resolveFormat(VulkanGraphicsDevice* device)
    {
        _format = vulkanMapPixelFormat(_owner->format());
        if (_format == VK_FORMAT_UNDEFINED) {
            spdlog::error("VulkanTexture: pixel format {} has no Vulkan mapping",
                static_cast<uint32_t>(_owner->format()));
            return false;
        }
        if (_format == VK_FORMAT_D24_UNORM_S8_UINT) {
            // Not supported by MoltenVK on Apple GPUs — probe for a fallback.
            _format = vulkanSupportedDepthStencilFormat(device->physicalDevice());
        } else if (_format == VK_FORMAT_D32_SFLOAT ||
                   _format == VK_FORMAT_D16_UNORM) {
            VkFormatProperties depthProperties{};
            vkGetPhysicalDeviceFormatProperties(
                device->physicalDevice(), _format, &depthProperties);
            if (!(depthProperties.optimalTilingFeatures &
                  VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
                _format = vulkanSupportedDepthFormat(device->physicalDevice());
            }
        }
        if (_format == VK_FORMAT_UNDEFINED) {
            spdlog::error(
                "VulkanTexture: no supported Vulkan format for pixel format {}",
                static_cast<uint32_t>(_owner->format()));
            return false;
        }
        _aspect = aspectForFormat(_format);
        return true;
    }

    bool VulkanTexture::ownerHasHigherMipLevels() const
    {
        for (uint32_t layer = 0; layer < _arrayLayers; ++layer) {
            for (uint32_t mip = 1; mip < _mipLevels; ++mip) {
                if (_owner->getLevel(mip, layer) != nullptr && _owner->getLevelDataSize(mip, layer) != 0) {
                    return true;
                }
            }
        }
        return false;
    }

    void VulkanTexture::resolveLevelsAndCapabilities(VulkanGraphicsDevice* device)
    {
        // Array textures (the clustered spot-shadow atlas is one) carry their slice
        // count in arrayLength. Ignoring it created a single-layer VkImage, and every
        // per-slice render target then tried to carve an attachment view at
        // baseArrayLayer >= 1 — invalid, and the slices had nowhere to render.
        _arrayLayers = _owner->isCubemap()
            ? 6u
            : std::max(1u, _owner->getArrayLength());
        _mipLevels = std::max(1u, _owner->getNumLevels());

        VkFormatProperties formatProperties{};
        vkGetPhysicalDeviceFormatProperties(
            device->physicalDevice(), _format, &formatProperties);
        const auto optimalFeatures = formatProperties.optimalTilingFeatures;
        constexpr VkFormatFeatureFlags kLinearBlitFeatures = VK_FORMAT_FEATURE_BLIT_SRC_BIT |
            VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        _supportsLinearSampling =
            (optimalFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
        _supportsLinearBlit = !isDepth() &&
            !isCompressedPixelFormat(_owner->format()) &&
            (optimalFeatures & kLinearBlitFeatures) == kLinearBlitFeatures;
        _supportsColorAttachment = !isDepth() &&
            (optimalFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT);

        // A chain this format cannot blit is kept only when the owner supplies it.
        if (_mipLevels > 1 && !_supportsLinearBlit && !ownerHasHigherMipLevels()) {
            spdlog::warn(
                "VulkanTexture: format {} cannot generate mipmaps; using level 0 only",
                static_cast<int>(_format));
            _mipLevels = 1;
        }
    }

    bool VulkanTexture::createImage(VulkanGraphicsDevice* device)
    {
        const uint32_t width = _owner->width();
        const uint32_t height = _owner->height();
        const bool isCubemap = _owner->isCubemap();
        const VkImageCreateFlags imageFlags = isCubemap
            ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;

        VkImageUsageFlags imageUsage = VK_IMAGE_USAGE_SAMPLED_BIT |
            VK_IMAGE_USAGE_TRANSFER_DST_BIT |
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        VkFormatFeatureFlags requiredFeatures = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        if (isDepth()) {
            imageUsage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
            requiredFeatures |= VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
        } else if (_supportsColorAttachment) {
            imageUsage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        }
        if (_owner->storage()) {
            imageUsage |= VK_IMAGE_USAGE_STORAGE_BIT;
            requiredFeatures |= VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
        }
        if (!vulkanFormatSupportsImage(device->physicalDevice(), _format,
                VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, imageUsage,
                imageFlags, requiredFeatures, {width, height, 1},
                _mipLevels, _arrayLayers, VK_SAMPLE_COUNT_1_BIT)) {
            spdlog::error(
                "VulkanTexture: format {} does not support requested usage {:#x}{}",
                static_cast<int>(_format), static_cast<uint32_t>(imageUsage),
                isCubemap ? " as a cubemap" : "");
            return false;
        }

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

        if (vmaCreateImage(_allocator, &imageInfo, &allocInfo, &_image, &_allocation, nullptr) != VK_SUCCESS) {
            spdlog::error("VulkanTexture: failed to create VkImage ({}x{}, fmt={})",
                width, height, static_cast<int>(_format));
            return false;
        }

        _subresourceLayouts.assign(
            static_cast<size_t>(_mipLevels) * _arrayLayers,
            VK_IMAGE_LAYOUT_UNDEFINED);
        return true;
    }

    bool VulkanTexture::createImageView()
    {
        // Image view (full-resource view used for sampling).  Render-target
        // attachments use their own per-face / per-mip views owned by
        // VulkanRenderTarget, so this sampling view is always the full image.
        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = _image;
        // A VIEW_TYPE_2D view may only cover one layer, so a multi-layer image
        // needs the array view type to match layerCount below.
        viewInfo.viewType = _owner->isCubemap()
            ? VK_IMAGE_VIEW_TYPE_CUBE
            : (_arrayLayers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY
                                : VK_IMAGE_VIEW_TYPE_2D);
        viewInfo.format = _format;
        // A sampled view may name ONE aspect: a depth-stencil texture samples its depth.
        // Render targets carve their own both-aspect view to attach it.
        viewInfo.subresourceRange.aspectMask = (_aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ? VK_IMAGE_ASPECT_DEPTH_BIT : _aspect;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = _mipLevels;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = _arrayLayers;
        if (vkCreateImageView(_vkDevice, &viewInfo, nullptr, &_imageView) != VK_SUCCESS) {
            spdlog::error("VulkanTexture: failed to create image view");
            _imageView = VK_NULL_HANDLE;
            releaseImageResources();
            return false;
        }
        return true;
    }

    void VulkanTexture::releaseImageResources()
    {
        // For a failed upload: nothing has been recorded against these yet, so they
        // are destroyed at once rather than deferred.
        destroySampler();
        if (_imageView != VK_NULL_HANDLE) {
            vkDestroyImageView(_vkDevice, _imageView, nullptr);
            _imageView = VK_NULL_HANDLE;
        }
        if (_image != VK_NULL_HANDLE) {
            vmaDestroyImage(_allocator, _image, _allocation);
            _image = VK_NULL_HANDLE;
            _allocation = VK_NULL_HANDLE;
        }
        _subresourceLayouts.clear();
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
        // The device's anisotropy ratio, which is the same number the Metal
        // default sampler uses; without it oblique ground textures smear into
        // radial lines, and with a different number the two backends filter
        // differently on every oblique surface.
        const float anisotropy = device->maxAnisotropy();
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

    bool VulkanTexture::read(GraphicsDevice* device, const TextureReadRegion& region,
        uint8_t* out, const size_t outSize)
    {
        auto* vulkanDevice = dynamic_cast<VulkanGraphicsDevice*>(device);
        if (!vulkanDevice || _image == VK_NULL_HANDLE || !out) {
            return false;
        }
        if (vulkanDevice->recording()) {
            // The work that produced these pixels is in a command buffer that has
            // not been submitted, so a one-shot read would run AHEAD of it and
            // return the previous contents. Refuse rather than answer wrongly.
            spdlog::error("VulkanTexture::read: cannot read back while a frame or an "
                "offline scope is recording — close it first");
            return false;
        }
        const uint32_t bytesPerPixel = pixelFormatBytesPerPixel(_owner->format());
        const VkDeviceSize size =
            static_cast<VkDeviceSize>(region.width) * region.height * bytesPerPixel;
        if (bytesPerPixel == 0 || outSize < size) {
            return false;
        }

        VkBuffer staging = VK_NULL_HANDLE;
        VmaAllocation allocation = nullptr;
        VmaAllocationInfo mapped{};
        if (!vulkanCreateReadbackBuffer(vulkanDevice->vmaAllocator(), size, staging, allocation, &mapped)) {
            spdlog::error("VulkanTexture::read: failed to allocate a {}-byte staging buffer",
                static_cast<size_t>(size));
            return false;
        }

        // The subresource is left exactly as it was found. A texture read between
        // frames is still bound by descriptors written for it, and handing it back
        // in TRANSFER_SRC would make every later sample of it invalid.
        const VkImageLayout previous = layout(region.mipLevel, region.face);
        const bool submitted = vulkanDevice->runOneShotCommands(
            [&](VkCommandBuffer cmd) {
                transitionLayout(cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    region.mipLevel, 1, region.face, 1);

                VkBufferImageCopy copy{};
                // Zero means tightly packed to imageExtent, which is how `out` is
                // sized — Vulkan has no 256-byte row alignment to pad around.
                copy.bufferRowLength = 0;
                copy.bufferImageHeight = 0;
                copy.imageSubresource.aspectMask = _aspect;
                copy.imageSubresource.mipLevel = region.mipLevel;
                copy.imageSubresource.baseArrayLayer = region.face;
                copy.imageSubresource.layerCount = 1;
                copy.imageOffset = {static_cast<int32_t>(region.x),
                    static_cast<int32_t>(region.y), 0};
                copy.imageExtent = {region.width, region.height, 1};
                vkCmdCopyImageToBuffer(cmd, _image,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging, 1, &copy);

                vulkanRecordCopyToHostBarrier(cmd);

                if (previous != VK_IMAGE_LAYOUT_UNDEFINED &&
                    previous != VK_IMAGE_LAYOUT_PREINITIALIZED) {
                    transitionLayout(cmd, previous, region.mipLevel, 1, region.face, 1);
                }
            });

        bool ok = false;
        if (submitted && mapped.pMappedData) {
            vmaInvalidateAllocation(vulkanDevice->vmaAllocator(), allocation, 0,
                VK_WHOLE_SIZE);
            std::memcpy(out, mapped.pMappedData, static_cast<size_t>(size));
            ok = true;
        } else if (submitted) {
            spdlog::error("VulkanTexture::read: staging buffer is not mapped");
        }
        vmaDestroyBuffer(vulkanDevice->vmaAllocator(), staging, allocation);
        return ok;
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
