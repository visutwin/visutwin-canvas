// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//

#ifdef VISUTWIN_HAS_VULKAN

#include "vulkanIndexBuffer.h"
#include "vulkanGraphicsDevice.h"
#include "vulkanUtils.h"

#include <cstring>
#include <vector>
#include "spdlog/spdlog.h"

namespace visutwin::canvas
{
    VulkanIndexBuffer::VulkanIndexBuffer(GraphicsDevice* device, IndexFormat format, int numIndices)
        : IndexBuffer(device, format, numIndices)
    {
        auto* vkDev = static_cast<VulkanGraphicsDevice*>(device);
        _deviceRef = vkDev;
        _deviceAlive = vkDev->aliveToken();
        _allocator = vkDev->vmaAllocator();

        _indexType = format == INDEXFORMAT_UINT32
            ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
        const size_t deviceBytesPerIndex =
            _indexType == VK_INDEX_TYPE_UINT32 ? sizeof(uint32_t) : sizeof(uint16_t);
        const size_t bufferSize =
            static_cast<size_t>(numIndices) * deviceBytesPerIndex;
        if (bufferSize == 0) return;

        VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufferInfo.size = bufferSize;
        bufferInfo.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;

        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;

        if (vmaCreateBuffer(_allocator, &bufferInfo, &allocInfo,
                &_buffer, &_allocation, nullptr) != VK_SUCCESS) {
            spdlog::error("VulkanIndexBuffer: GPU allocation failed");
        }
    }

    VulkanIndexBuffer::~VulkanIndexBuffer()
    {
        if (_deviceRef && _deviceAlive.expired()) {
            return; // device gone — VMA allocator and buffers died with it
        }
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

        const size_t sourceBytesPerIndex = format() == INDEXFORMAT_UINT32
            ? sizeof(uint32_t)
            : format() == INDEXFORMAT_UINT16 ? sizeof(uint16_t) : sizeof(uint8_t);
        const size_t expectedSize =
            static_cast<size_t>(numIndices()) * sourceBytesPerIndex;
        if (data.size() != expectedSize) {
            spdlog::error(
                "VulkanIndexBuffer: received {} bytes for {} indices, expected {}",
                data.size(), numIndices(), expectedSize);
            return false;
        }

        bool uploaded = false;
        if (format() == INDEXFORMAT_UINT8) {
            std::vector<uint16_t> widened(static_cast<size_t>(numIndices()));
            for (size_t i = 0; i < widened.size(); ++i) {
                widened[i] = data[i];
            }
            uploaded = uploadStaging(
                widened.data(), widened.size() * sizeof(uint16_t));
        } else {
            uploaded = uploadStaging(data.data(), data.size());
        }
        if (!uploaded) {
            return false;
        }

        // Preserve the source representation for CPU-side mesh batching.
        _storage = data;
        return true;
    }

    bool VulkanIndexBuffer::uploadStaging(const void* data, size_t size)
    {
        auto* vkDev = static_cast<VulkanGraphicsDevice*>(_device);

        VkBuffer stagingBuffer;
        VmaAllocation stagingAlloc;

        VkBufferCreateInfo stagingInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        stagingInfo.size = size;
        stagingInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

        VmaAllocationCreateInfo stagingAllocInfo{};
        stagingAllocInfo.usage = VMA_MEMORY_USAGE_CPU_ONLY;

        if (vmaCreateBuffer(_allocator, &stagingInfo, &stagingAllocInfo,
                &stagingBuffer, &stagingAlloc, nullptr) != VK_SUCCESS) {
            spdlog::error("VulkanIndexBuffer: staging allocation failed");
            return false;
        }

        void* mapped;
        if (vmaMapMemory(_allocator, stagingAlloc, &mapped) != VK_SUCCESS) {
            spdlog::error("VulkanIndexBuffer: staging map failed");
            vmaDestroyBuffer(_allocator, stagingBuffer, stagingAlloc);
            return false;
        }
        memcpy(mapped, data, size);
        vmaUnmapMemory(_allocator, stagingAlloc);

        const VkBuffer destinationBuffer = _buffer;
        vkDev->enqueueUpload([destinationBuffer, stagingBuffer, size](VkCommandBuffer cmd) {
            // Queue-wide ordering: prior in-flight frames finish their index
            // reads before the copy; later reads see the copied data.
            VkBufferMemoryBarrier2 pre{
                VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
            pre.srcStageMask = VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT;
            pre.srcAccessMask = VK_ACCESS_2_INDEX_READ_BIT;
            pre.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            pre.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            pre.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            pre.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            pre.buffer = destinationBuffer;
            pre.size = VK_WHOLE_SIZE;
            VkDependencyInfo dependency{
                VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dependency.bufferMemoryBarrierCount = 1;
            dependency.pBufferMemoryBarriers = &pre;
            vkCmdPipelineBarrier2(cmd, &dependency);

            VkBufferCopy copy{};
            copy.size = size;
            vkCmdCopyBuffer(cmd, stagingBuffer, destinationBuffer, 1, &copy);

            VkBufferMemoryBarrier2 post = pre;
            post.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            post.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            post.dstStageMask = VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT;
            post.dstAccessMask = VK_ACCESS_2_INDEX_READ_BIT;
            dependency.pBufferMemoryBarriers = &post;
            vkCmdPipelineBarrier2(cmd, &dependency);
        }, [allocator = _allocator, stagingBuffer, stagingAlloc] {
            vmaDestroyBuffer(allocator, stagingBuffer, stagingAlloc);
        });
        return true;
    }
}

#endif // VISUTWIN_HAS_VULKAN
