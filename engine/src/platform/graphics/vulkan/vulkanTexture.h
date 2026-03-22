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

    private:
        void createSampler(VulkanGraphicsDevice* device);

        Texture* _owner = nullptr;
        VkDevice _vkDevice = VK_NULL_HANDLE;
        VmaAllocator _allocator = VK_NULL_HANDLE;
        VkImage _image = VK_NULL_HANDLE;
        VmaAllocation _allocation = VK_NULL_HANDLE;
        VkImageView _imageView = VK_NULL_HANDLE;
        VkSampler _sampler = VK_NULL_HANDLE;
    };
}

#endif // VISUTWIN_HAS_VULKAN
