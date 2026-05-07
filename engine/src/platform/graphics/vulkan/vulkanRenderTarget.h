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

    protected:
        void destroyFrameBuffers() override;
        void createFrameBuffers() override;

    private:
        VulkanGraphicsDevice* vulkanDevice() const;

        std::vector<VulkanColorAttachment> _colorAttachments;
        VulkanDepthAttachment _depthAttachment{};
    };
}

#endif // VISUTWIN_HAS_VULKAN
