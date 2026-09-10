// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Vulkan render target — dynamic-rendering attachment bundle.
//
// We use VK_KHR_dynamic_rendering (Vulkan 1.3 core), so this class doesn't
// own a VkRenderPass / VkFramebuffer.  It just resolves the per-face /
// per-mip image views needed to populate VkRenderingAttachmentInfo at
// vkCmdBeginRendering time, and tracks any internally-owned resources
// (e.g. an internal depth image when the caller asked for depth without
// supplying a depth texture).
//
#pragma once

#ifdef VISUTWIN_HAS_VULKAN

#include <vector>
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include "platform/graphics/renderTarget.h"

namespace visutwin::canvas
{
    class VulkanGraphicsDevice;
    namespace gpu { class VulkanTexture; }

    struct VulkanColorAttachment
    {
        // Texture-backed view (owned elsewhere — by gpu::VulkanTexture for the
        // full-resource case, or by this RT when it carved out a face/mip view).
        VkImageView view = VK_NULL_HANDLE;
        VkFormat    format = VK_FORMAT_UNDEFINED;
        gpu::VulkanTexture* texture = nullptr;  // for layout tracking; may be null
        bool ownView = false;                    // RT created `view` and must destroy it

        // Multisampled surface, owned here, present only when samples() > 1.
        // Rendering targets `msaaView` and `view` becomes the resolve
        // destination, which is what keeps every later pass sampling a plain
        // single-sample texture.
        VkImage msaaImage = VK_NULL_HANDLE;
        VmaAllocation msaaAllocation = VK_NULL_HANDLE;
        VkImageView msaaView = VK_NULL_HANDLE;
        mutable VkImageLayout msaaLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    };

    struct VulkanDepthAttachment
    {
        VkImageView view = VK_NULL_HANDLE;
        VkFormat    format = VK_FORMAT_UNDEFINED;
        gpu::VulkanTexture* texture = nullptr;   // null when internally owned

        // Internal depth resources — populated when `RenderTargetOptions::depth`
        // is set without a depthBuffer texture.  We allocate one VkImage that
        // is private to the RT and freed when the RT is destroyed.
        VkImage internalImage = VK_NULL_HANDLE;
        VmaAllocation internalAllocation = VK_NULL_HANDLE;
        VkImageLayout currentLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        bool ownView = false;

        // Multisampled depth, owned here. Only allocated over a TEXTURE-backed
        // depth buffer: an internally-owned depth image is nothing but the
        // pass's own scratch depth, so it is simply created multisampled and
        // never resolved.
        VkImage msaaImage = VK_NULL_HANDLE;
        VmaAllocation msaaAllocation = VK_NULL_HANDLE;
        VkImageView msaaView = VK_NULL_HANDLE;
        mutable VkImageLayout msaaLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    };

    class VulkanRenderTarget : public RenderTarget
    {
    public:
        explicit VulkanRenderTarget(const RenderTargetOptions& options);
        ~VulkanRenderTarget() override;

        const std::vector<VulkanColorAttachment>& colorAttachments() const { return _colorAttachments; }
        const VulkanDepthAttachment& depthAttachment() const { return _depthAttachment; }

        bool hasDepthAttachment() const { return _depthAttachment.view != VK_NULL_HANDLE; }

        VkExtent2D extent() const { return {static_cast<uint32_t>(width()),
                                            static_cast<uint32_t>(height())}; }

        // Sample count of the attachments as the enum bit a pipeline wants.
        // Always in step with samples(): a target whose multisampled surfaces
        // could not be created reports one sample from both.
        VkSampleCountFlagBits sampleCountFlag() const;

    protected:
        void destroyFrameBuffers() override;
        void createFrameBuffers() override;

    private:
        VulkanGraphicsDevice* vulkanDevice() const;

        // Allocate the multisampled color/depth surfaces for samples() > 1.
        // Returns false when the device cannot render this format at that
        // sample count, which drops the target back to single-sample.
        bool createMultisampledSurfaces(VkFormat internalDepthFormat);

        std::vector<VulkanColorAttachment> _colorAttachments;
        VulkanDepthAttachment _depthAttachment{};
    };
}

#endif // VISUTWIN_HAS_VULKAN
