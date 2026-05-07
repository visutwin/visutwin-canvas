// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//

#ifdef VISUTWIN_HAS_VULKAN

#include "vulkanRenderTarget.h"

#include "vulkanGraphicsDevice.h"
#include "vulkanTexture.h"
#include "vulkanUtils.h"

#include "platform/graphics/texture.h"
#include "spdlog/spdlog.h"

namespace visutwin::canvas
{
    namespace
    {
        // Build a VkImageView that targets a single (face, mipLevel) of the
        // texture's VkImage.  For the trivial case (face 0, mip 0, non-cubemap)
        // we return the texture's own primary view to avoid the extra alloc.
        VkImageView resolveAttachmentView(VkDevice device,
            gpu::VulkanTexture* tex, int face, int mipLevel,
            bool& outOwn)
        {
            outOwn = false;
            if (!tex || tex->image() == VK_NULL_HANDLE) {
                return VK_NULL_HANDLE;
            }

            const bool needsCarvedView = (face != 0) || (mipLevel != 0) ||
                                         (tex->arrayLayers() > 1);

            if (!needsCarvedView) {
                return tex->imageView();
            }

            VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            viewInfo.image = tex->image();
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = tex->format();
            viewInfo.subresourceRange.aspectMask = tex->aspect();
            viewInfo.subresourceRange.baseMipLevel = static_cast<uint32_t>(mipLevel);
            viewInfo.subresourceRange.levelCount = 1;
            viewInfo.subresourceRange.baseArrayLayer = static_cast<uint32_t>(face);
            viewInfo.subresourceRange.layerCount = 1;

            VkImageView view = VK_NULL_HANDLE;
            if (vkCreateImageView(device, &viewInfo, nullptr, &view) != VK_SUCCESS) {
                spdlog::error("VulkanRenderTarget: failed to carve attachment view "
                              "(face={}, mip={})", face, mipLevel);
                return VK_NULL_HANDLE;
            }
            outOwn = true;
            return view;
        }
    }

    VulkanRenderTarget::VulkanRenderTarget(const RenderTargetOptions& options)
        : RenderTarget(options)
    {
        // Match the Metal RT contract: attachments are resolved at construction
        // time so the first render pass can begin immediately.
        createFrameBuffers();
    }

    VulkanRenderTarget::~VulkanRenderTarget()
    {
        destroyFrameBuffers();
    }

    VulkanGraphicsDevice* VulkanRenderTarget::vulkanDevice() const
    {
        return dynamic_cast<VulkanGraphicsDevice*>(device());
    }

    void VulkanRenderTarget::destroyFrameBuffers()
    {
        auto* vkDev = vulkanDevice();
        VkDevice vk = vkDev ? vkDev->device() : VK_NULL_HANDLE;
        VmaAllocator allocator = vkDev ? vkDev->vmaAllocator() : VK_NULL_HANDLE;

        if (vk != VK_NULL_HANDLE) {
            for (auto& a : _colorAttachments) {
                if (a.ownView && a.view != VK_NULL_HANDLE) {
                    vkDestroyImageView(vk, a.view, nullptr);
                }
            }
        }
        _colorAttachments.clear();

        if (vk != VK_NULL_HANDLE) {
            if (_depthAttachment.ownView && _depthAttachment.view != VK_NULL_HANDLE) {
                vkDestroyImageView(vk, _depthAttachment.view, nullptr);
            }
            if (allocator != VK_NULL_HANDLE && _depthAttachment.internalImage != VK_NULL_HANDLE) {
                vmaDestroyImage(allocator, _depthAttachment.internalImage,
                                _depthAttachment.internalAllocation);
            }
        }
        _depthAttachment = VulkanDepthAttachment{};
    }

    void VulkanRenderTarget::createFrameBuffers()
    {
        auto* vkDev = vulkanDevice();
        if (!vkDev) {
            spdlog::error("VulkanRenderTarget requires VulkanGraphicsDevice");
            return;
        }

        // Re-entrant call (e.g. on resize) — start from clean state.
        destroyFrameBuffers();

        VkDevice vk = vkDev->device();
        VmaAllocator allocator = vkDev->vmaAllocator();

        // Color attachments
        const int colorCount = colorBufferCount();
        _colorAttachments.reserve(static_cast<size_t>(colorCount));

        for (int i = 0; i < colorCount; ++i) {
            Texture* colorBuffer = getColorBuffer(i);
            if (!colorBuffer) {
                continue;
            }
            colorBuffer->upload();

            auto* vkTex = dynamic_cast<gpu::VulkanTexture*>(colorBuffer->impl());
            if (!vkTex || vkTex->image() == VK_NULL_HANDLE) {
                spdlog::warn("VulkanRenderTarget: color buffer {} has no VkImage", i);
                continue;
            }

            VulkanColorAttachment attachment{};
            attachment.format = vkTex->format();
            attachment.texture = vkTex;
            attachment.view = resolveAttachmentView(vk, vkTex, face(), mipLevel(),
                                                    attachment.ownView);
            if (attachment.view == VK_NULL_HANDLE) {
                continue;
            }
            _colorAttachments.push_back(attachment);
        }

        // Depth attachment — three cases:
        //   1. options.depthBuffer set → use that texture's view.
        //   2. options.depth = true, no depthBuffer → allocate an internal depth image.
        //   3. neither → no depth attachment.
        if (Texture* depthTex = depthBuffer()) {
            depthTex->upload();
            auto* vkTex = dynamic_cast<gpu::VulkanTexture*>(depthTex->impl());
            if (vkTex && vkTex->image() != VK_NULL_HANDLE) {
                _depthAttachment.format = vkTex->format();
                _depthAttachment.texture = vkTex;
                _depthAttachment.view = resolveAttachmentView(vk, vkTex, face(), mipLevel(),
                                                              _depthAttachment.ownView);
                _depthAttachment.currentLayout = vkTex->currentLayout();
            } else {
                spdlog::warn("VulkanRenderTarget: depthBuffer has no VkImage");
            }
        } else if (hasDepth()) {
            const VkFormat depthFormat = hasStencil()
                ? VK_FORMAT_D24_UNORM_S8_UINT
                : VK_FORMAT_D32_SFLOAT;

            const uint32_t w = static_cast<uint32_t>(width());
            const uint32_t h = static_cast<uint32_t>(height());

            VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            imageInfo.imageType = VK_IMAGE_TYPE_2D;
            imageInfo.format = depthFormat;
            imageInfo.extent = {w, h, 1};
            imageInfo.mipLevels = 1;
            imageInfo.arrayLayers = 1;
            imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
            imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
            imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;

            VmaAllocationCreateInfo allocInfo{};
            allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;

            if (vmaCreateImage(allocator, &imageInfo, &allocInfo,
                               &_depthAttachment.internalImage,
                               &_depthAttachment.internalAllocation, nullptr) == VK_SUCCESS) {
                VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
                viewInfo.image = _depthAttachment.internalImage;
                viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
                viewInfo.format = depthFormat;
                viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT |
                    (hasStencil() ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
                viewInfo.subresourceRange.baseMipLevel = 0;
                viewInfo.subresourceRange.levelCount = 1;
                viewInfo.subresourceRange.baseArrayLayer = 0;
                viewInfo.subresourceRange.layerCount = 1;
                vkCreateImageView(vk, &viewInfo, nullptr, &_depthAttachment.view);

                _depthAttachment.format = depthFormat;
                _depthAttachment.ownView = true;
                _depthAttachment.currentLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            } else {
                spdlog::error("VulkanRenderTarget: failed to allocate internal depth image");
            }
        }
    }
}

#endif // VISUTWIN_HAS_VULKAN
