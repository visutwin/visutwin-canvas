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
        _deviceRef = vkDev;
        _deviceAlive = vkDev->aliveToken();
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
        if (_deviceRef && _deviceAlive.expired()) {
            return; // device gone — VMA allocator and buffers died with it
        }
        if (_allocator != VK_NULL_HANDLE && _buffer != VK_NULL_HANDLE) {
            // Defer: an in-flight frame's vertex bindings may still read this.
            if (_deviceRef) {
                _deviceRef->deferDestroy(
                    [allocator = _allocator, buffer = _buffer, allocation = _allocation] {
                        vmaDestroyBuffer(allocator, buffer, allocation);
                    });
            } else {
                vmaDestroyBuffer(_allocator, _buffer, _allocation);
            }
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
            // Pipeline barriers order queue-wide in submission order: the
            // pre-barrier makes already-submitted frames finish reading the
            // buffer before the copy overwrites it; the post-barrier orders
            // the copy against subsequent vertex reads.
            VkBufferMemoryBarrier pre{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
            pre.srcAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
            pre.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            pre.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            pre.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            pre.buffer = _buffer;
            pre.size = VK_WHOLE_SIZE;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1, &pre, 0, nullptr);

            VkBufferCopy copy{};
            copy.size = dataSize;
            vkCmdCopyBuffer(cmd, stagingBuffer, _buffer, 1, &copy);

            VkBufferMemoryBarrier post = pre;
            post.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            post.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 0, nullptr, 1, &post, 0, nullptr);
        });

        vmaDestroyBuffer(_allocator, stagingBuffer, stagingAlloc);
    }
}

#endif // VISUTWIN_HAS_VULKAN
