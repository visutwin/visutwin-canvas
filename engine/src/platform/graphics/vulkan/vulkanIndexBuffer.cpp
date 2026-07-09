// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//

#ifdef VISUTWIN_HAS_VULKAN

#include "vulkanIndexBuffer.h"
#include "vulkanGraphicsDevice.h"
#include "vulkanUtils.h"

#include <cstring>
#include "spdlog/spdlog.h"

namespace visutwin::canvas
{
    VulkanIndexBuffer::VulkanIndexBuffer(GraphicsDevice* device, IndexFormat format, int numIndices)
        : IndexBuffer(device, format, numIndices)
    {
        auto* vkDev = static_cast<VulkanGraphicsDevice*>(device);
        _deviceRef = vkDev;
        _allocator = vkDev->vmaAllocator();

        int bytesPerIndex = (format == INDEXFORMAT_UINT32) ? 4 : (format == INDEXFORMAT_UINT16) ? 2 : 1;
        size_t bufferSize = static_cast<size_t>(numIndices) * bytesPerIndex;
        if (bufferSize == 0) return;

        VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufferInfo.size = bufferSize;
        bufferInfo.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;

        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;

        vmaCreateBuffer(_allocator, &bufferInfo, &allocInfo, &_buffer, &_allocation, nullptr);
    }

    VulkanIndexBuffer::~VulkanIndexBuffer()
    {
        if (_allocator != VK_NULL_HANDLE && _buffer != VK_NULL_HANDLE) {
            // Defer: an in-flight frame's index binding may still read this.
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

    bool VulkanIndexBuffer::setData(const std::vector<uint8_t>& data)
    {
        if (data.empty() || !_allocator || !_buffer) return false;
        _storage = data;
        uploadStaging(data.data(), data.size());
        return true;
    }

    void VulkanIndexBuffer::uploadStaging(const void* data, size_t size)
    {
        auto* vkDev = static_cast<VulkanGraphicsDevice*>(_device);

        VkBuffer stagingBuffer;
        VmaAllocation stagingAlloc;

        VkBufferCreateInfo stagingInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        stagingInfo.size = size;
        stagingInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

        VmaAllocationCreateInfo stagingAllocInfo{};
        stagingAllocInfo.usage = VMA_MEMORY_USAGE_CPU_ONLY;

        vmaCreateBuffer(_allocator, &stagingInfo, &stagingAllocInfo,
            &stagingBuffer, &stagingAlloc, nullptr);

        void* mapped;
        vmaMapMemory(_allocator, stagingAlloc, &mapped);
        memcpy(mapped, data, size);
        vmaUnmapMemory(_allocator, stagingAlloc);

        vulkanImmediateSubmit(vkDev, [&](VkCommandBuffer cmd) {
            // Queue-wide ordering: prior in-flight frames finish their index
            // reads before the copy; later reads see the copied data.
            VkBufferMemoryBarrier pre{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
            pre.srcAccessMask = VK_ACCESS_INDEX_READ_BIT;
            pre.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            pre.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            pre.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            pre.buffer = _buffer;
            pre.size = VK_WHOLE_SIZE;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1, &pre, 0, nullptr);

            VkBufferCopy copy{};
            copy.size = size;
            vkCmdCopyBuffer(cmd, stagingBuffer, _buffer, 1, &copy);

            VkBufferMemoryBarrier post = pre;
            post.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            post.dstAccessMask = VK_ACCESS_INDEX_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 0, nullptr, 1, &post, 0, nullptr);
        });

        vmaDestroyBuffer(_allocator, stagingBuffer, stagingAlloc);
    }
}

#endif // VISUTWIN_HAS_VULKAN
