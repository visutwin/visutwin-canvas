// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Vulkan index buffer — VMA-backed VkBuffer.
//
#pragma once

#ifdef VISUTWIN_HAS_VULKAN

#include <memory>
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include "platform/graphics/indexBuffer.h"

namespace visutwin::canvas
{
    class VulkanGraphicsDevice;

    class VulkanIndexBuffer : public IndexBuffer
    {
    public:
        VulkanIndexBuffer(GraphicsDevice* device, IndexFormat format, int numIndices);
        ~VulkanIndexBuffer() override;

        void* nativeBuffer() const override { return reinterpret_cast<void*>(_buffer); }
        bool setData(const std::vector<uint8_t>& data) override;

        [[nodiscard]] VkBuffer buffer() const { return _buffer; }
        [[nodiscard]] VkIndexType indexType() const { return _indexType; }

    private:
        bool uploadStaging(const void* data, size_t size);

        VkBuffer _buffer = VK_NULL_HANDLE;
        // Vulkan's core index types are uint16 and uint32. Public uint8 index
        // buffers are widened to uint16 during upload for portable support.
        VkIndexType _indexType = VK_INDEX_TYPE_UINT16;
        VmaAllocation _allocation = VK_NULL_HANDLE;
        VmaAllocator _allocator = VK_NULL_HANDLE;
        // Deferred-destroy routing (in-flight frames may reference the buffer).
        VulkanGraphicsDevice* _deviceRef = nullptr;
        std::weak_ptr<bool> _deviceAlive;
    };
}

#endif // VISUTWIN_HAS_VULKAN
