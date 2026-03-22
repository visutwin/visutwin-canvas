// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Vulkan render target — VkFramebuffer + VkRenderPass (or dynamic rendering).
//
#pragma once

#ifdef VISUTWIN_HAS_VULKAN

#include <vulkan/vulkan.h>

#include "platform/graphics/renderTarget.h"

namespace visutwin::canvas
{
    class VulkanRenderTarget : public RenderTarget
    {
    public:
        explicit VulkanRenderTarget(const RenderTargetOptions& options);
        ~VulkanRenderTarget() override;

        [[nodiscard]] VkFramebuffer framebuffer() const { return _framebuffer; }
        [[nodiscard]] VkRenderPass renderPass() const { return _renderPass; }

    protected:
        void destroyFrameBuffers() override;
        void createFrameBuffers() override;

    private:
        VkDevice _vkDevice = VK_NULL_HANDLE;
        VkFramebuffer _framebuffer = VK_NULL_HANDLE;
        VkRenderPass _renderPass = VK_NULL_HANDLE;
    };
}

#endif // VISUTWIN_HAS_VULKAN
