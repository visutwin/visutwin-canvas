// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Vulkan index buffer — VMA-backed VkBuffer.
//
#pragma once

#ifdef VISUTWIN_HAS_VULKAN

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include "platform/graphics/indexBuffer.h"

namespace visutwin::canvas
{
    class VulkanIndexBuffer : public IndexBuffer
    {
    public:
        VulkanIndexBuffer(GraphicsDevice* device, IndexFormat format, int numIndices);
        ~VulkanIndexBuffer() override;

        void* nativeBuffer() const override { return reinterpret_cast<void*>(_buffer); }
        bool setData(const std::vector<uint8_t>& data) override;

        [[nodiscard]] VkBuffer buffer() const { return _buffer; }

    private:
        void uploadStaging(const void* data, size_t size);

        VkBuffer _buffer = VK_NULL_HANDLE;
        VmaAllocation _allocation = VK_NULL_HANDLE;
        VmaAllocator _allocator = VK_NULL_HANDLE;
    };
}

#endif // VISUTWIN_HAS_VULKAN
