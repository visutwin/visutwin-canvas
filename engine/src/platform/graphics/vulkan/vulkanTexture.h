// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Vulkan texture implementation — VkImage + VkImageView + VkSampler.
//
#pragma once

#ifdef VISUTWIN_HAS_VULKAN

#include <memory>
#include <vector>
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
        bool read(GraphicsDevice* device, const TextureReadRegion& region,
            uint8_t* out, size_t outSize) override;

        [[nodiscard]] VkImage image() const { return _image; }
        [[nodiscard]] VkImageView imageView() const { return _imageView; }
        [[nodiscard]] VkSampler sampler() const { return _sampler; }
        [[nodiscard]] VkFormat format() const { return _format; }
        [[nodiscard]] VkImageAspectFlags aspect() const { return _aspect; }
        [[nodiscard]] bool isDepth() const { return (_aspect & VK_IMAGE_ASPECT_DEPTH_BIT) != 0; }
        [[nodiscard]] uint32_t arrayLayers() const { return _arrayLayers; }
        [[nodiscard]] uint32_t mipLevels() const { return _mipLevels; }
        [[nodiscard]] bool supportsColorAttachment() const {
            return _supportsColorAttachment;
        }

        [[nodiscard]] VkImageLayout layout(uint32_t mipLevel, uint32_t arrayLayer) const;
        void transitionLayout(VkCommandBuffer commandBuffer, VkImageLayout newLayout,
            uint32_t baseMipLevel = 0, uint32_t levelCount = VK_REMAINING_MIP_LEVELS,
            uint32_t baseArrayLayer = 0, uint32_t layerCount = VK_REMAINING_ARRAY_LAYERS);
        bool generateMipmaps(VkCommandBuffer commandBuffer,
            uint32_t baseArrayLayer = 0,
            uint32_t layerCount = VK_REMAINING_ARRAY_LAYERS);

    private:
        [[nodiscard]] bool createSampler(
            VulkanGraphicsDevice* device, VkSampler& sampler) const;
        void destroySampler();

        // uploadImmediate's stages, in the order it runs them. Each bool stage
        // returns false when the upload has to stop; it has logged why and left
        // nothing half-built behind.

        /// Hands the current image, view and sampler to the device to destroy once
        /// the frames in flight that may still use them have completed.
        void retireImage();
        /// The VkFormat and aspect for the owner's pixel format, with a depth
        /// fallback where the device lacks the exact format.
        [[nodiscard]] bool resolveFormat(VulkanGraphicsDevice* device);
        /// Layer and mip counts, and what the format supports (linear sampling,
        /// blit mip generation, colour attachment).
        void resolveLevelsAndCapabilities(VulkanGraphicsDevice* device);
        [[nodiscard]] bool ownerHasHigherMipLevels() const;
        [[nodiscard]] bool createImage(VulkanGraphicsDevice* device);
        [[nodiscard]] bool createImageView();
        /// Destroys the sampler, view and image at once, for an upload that failed
        /// before anything was recorded against them.
        void releaseImageResources();

        Texture* _owner = nullptr;
        // Set in uploadImmediate; used to defer GPU-resource destruction until
        // in-flight frames that may reference this texture have completed.
        VulkanGraphicsDevice* _deviceRef = nullptr;
        std::weak_ptr<bool> _deviceAlive;
        VkDevice _vkDevice = VK_NULL_HANDLE;
        VmaAllocator _allocator = VK_NULL_HANDLE;
        VkImage _image = VK_NULL_HANDLE;
        VmaAllocation _allocation = VK_NULL_HANDLE;
        VkImageView _imageView = VK_NULL_HANDLE;
        VkSampler _sampler = VK_NULL_HANDLE;
        VkFormat _format = VK_FORMAT_UNDEFINED;
        VkImageAspectFlags _aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        uint32_t _arrayLayers = 1;
        uint32_t _mipLevels = 1;
        bool _supportsLinearBlit = false;
        bool _supportsLinearSampling = false;
        bool _supportsColorAttachment = false;
        std::vector<VkImageLayout> _subresourceLayouts;
    };
}

#endif // VISUTWIN_HAS_VULKAN
