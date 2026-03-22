// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Vulkan vertex buffer — VMA-backed VkBuffer.
//
#pragma once

#ifdef VISUTWIN_HAS_VULKAN

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include "platform/graphics/vertexBuffer.h"

namespace visutwin::canvas
{
    class VulkanVertexBuffer : public VertexBuffer
    {
    public:
        VulkanVertexBuffer(GraphicsDevice* device, const std::shared_ptr<VertexFormat>& format,
            int numVertices, const VertexBufferOptions& options = VertexBufferOptions{});
        ~VulkanVertexBuffer() override;

        void unlock() override;
        void* nativeBuffer() const override { return reinterpret_cast<void*>(_buffer); }

        [[nodiscard]] VkBuffer buffer() const { return _buffer; }

    private:
        VkBuffer _buffer = VK_NULL_HANDLE;
        VmaAllocation _allocation = VK_NULL_HANDLE;
        VmaAllocator _allocator = VK_NULL_HANDLE;
    };
}

#endif // VISUTWIN_HAS_VULKAN
