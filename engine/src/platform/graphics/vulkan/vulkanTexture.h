// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Vulkan texture implementation — VkImage + VkImageView + VkSampler.
//
#pragma once

#ifdef VISUTWIN_HAS_VULKAN

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include "platform/graphics/gpu.h"
#include "platform/graphics/constants.h"

namespace visutwin::canvas
{
    class GraphicsDevice;
    class Texture;
    class VulkanGraphicsDevice;
}

namespace visutwin::canvas::gpu
{
    class VulkanTexture : public HardwareTexture
    {
    public:
        explicit VulkanTexture(Texture* owner);
        ~VulkanTexture() override;

        void uploadImmediate(GraphicsDevice* device) override;
        void propertyChanged(uint32_t flag) override;

        [[nodiscard]] VkImage image() const { return _image; }
        [[nodiscard]] VkImageView imageView() const { return _imageView; }
        [[nodiscard]] VkSampler sampler() const { return _sampler; }
        [[nodiscard]] VkFormat format() const { return _format; }
        [[nodiscard]] VkImageAspectFlags aspect() const { return _aspect; }
        [[nodiscard]] bool isDepth() const { return (_aspect & VK_IMAGE_ASPECT_DEPTH_BIT) != 0; }
        [[nodiscard]] uint32_t arrayLayers() const { return _arrayLayers; }

        // Tracks the layout the image is currently in.  Render-target writes,
        // copies, and shader reads all need to know the source layout when
        // inserting their barriers; this is the single source of truth.
        [[nodiscard]] VkImageLayout currentLayout() const { return _currentLayout; }
        void setCurrentLayout(VkImageLayout layout) { _currentLayout = layout; }

    private:
        void createSampler(VulkanGraphicsDevice* device);
        void destroySampler();

        Texture* _owner = nullptr;
        VkDevice _vkDevice = VK_NULL_HANDLE;
        VmaAllocator _allocator = VK_NULL_HANDLE;
        VkImage _image = VK_NULL_HANDLE;
        VmaAllocation _allocation = VK_NULL_HANDLE;
        VkImageView _imageView = VK_NULL_HANDLE;
        VkSampler _sampler = VK_NULL_HANDLE;
        VkFormat _format = VK_FORMAT_UNDEFINED;
        VkImageAspectFlags _aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        uint32_t _arrayLayers = 1;
        VkImageLayout _currentLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    };
}

#endif // VISUTWIN_HAS_VULKAN
