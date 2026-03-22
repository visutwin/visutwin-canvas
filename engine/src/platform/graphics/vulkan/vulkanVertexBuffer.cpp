// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//

#ifdef VISUTWIN_HAS_VULKAN

#include "vulkanVertexBuffer.h"
#include "vulkanGraphicsDevice.h"
#include "vulkanUtils.h"

#include <cstring>
#include "spdlog/spdlog.h"

namespace visutwin::canvas
{
    VulkanVertexBuffer::VulkanVertexBuffer(GraphicsDevice* device,
        const std::shared_ptr<VertexFormat>& format, int numVertices,
        const VertexBufferOptions& options)
        : VertexBuffer(device, format, numVertices, options)
    {
        auto* vkDev = static_cast<VulkanGraphicsDevice*>(device);
        _allocator = vkDev->vmaAllocator();

        size_t bufferSize = _storage.size();
        if (bufferSize == 0) return;

        VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufferInfo.size = bufferSize;
        bufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;

        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;

        vmaCreateBuffer(_allocator, &bufferInfo, &allocInfo, &_buffer, &_allocation, nullptr);

        if (!options.data.empty()) {
            unlock();
        }
    }

    VulkanVertexBuffer::~VulkanVertexBuffer()
    {
        if (_allocator != VK_NULL_HANDLE && _buffer != VK_NULL_HANDLE) {
            vmaDestroyBuffer(_allocator, _buffer, _allocation);
        }
    }

    void VulkanVertexBuffer::unlock()
    {
        if (_storage.empty() || !_allocator || !_buffer) return;

        auto* vkDev = static_cast<VulkanGraphicsDevice*>(_device);
        size_t dataSize = _storage.size();

        // Create staging buffer
        VkBuffer stagingBuffer;
        VmaAllocation stagingAlloc;

        VkBufferCreateInfo stagingInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        stagingInfo.size = dataSize;
        stagingInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

        VmaAllocationCreateInfo stagingAllocInfo{};
        stagingAllocInfo.usage = VMA_MEMORY_USAGE_CPU_ONLY;

        vmaCreateBuffer(_allocator, &stagingInfo, &stagingAllocInfo,
            &stagingBuffer, &stagingAlloc, nullptr);

        void* mapped;
        vmaMapMemory(_allocator, stagingAlloc, &mapped);
        memcpy(mapped, _storage.data(), dataSize);
        vmaUnmapMemory(_allocator, stagingAlloc);

        vulkanImmediateSubmit(vkDev, [&](VkCommandBuffer cmd) {
            VkBufferCopy copy{};
            copy.size = dataSize;
            vkCmdCopyBuffer(cmd, stagingBuffer, _buffer, 1, &copy);
        });

        vmaDestroyBuffer(_allocator, stagingBuffer, stagingAlloc);
    }
}

#endif // VISUTWIN_HAS_VULKAN
