// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Vulkan render target — stub.
//

#ifdef VISUTWIN_HAS_VULKAN

#include "vulkanRenderTarget.h"

#include "spdlog/spdlog.h"

namespace visutwin::canvas
{
    VulkanRenderTarget::VulkanRenderTarget(const RenderTargetOptions& options)
        : RenderTarget(options)
    {
        // TODO: create VkRenderPass + VkFramebuffer from options
    }

    VulkanRenderTarget::~VulkanRenderTarget()
    {
        destroyFrameBuffers();
    }

    void VulkanRenderTarget::destroyFrameBuffers()
    {
        if (_vkDevice != VK_NULL_HANDLE) {
            if (_framebuffer != VK_NULL_HANDLE) {
                vkDestroyFramebuffer(_vkDevice, _framebuffer, nullptr);
                _framebuffer = VK_NULL_HANDLE;
            }
            if (_renderPass != VK_NULL_HANDLE) {
                vkDestroyRenderPass(_vkDevice, _renderPass, nullptr);
                _renderPass = VK_NULL_HANDLE;
            }
        }
    }

    void VulkanRenderTarget::createFrameBuffers()
    {
        // TODO: create VkRenderPass + VkFramebuffer from color/depth attachments
    }
}

#endif // VISUTWIN_HAS_VULKAN
