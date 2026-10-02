// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 21.03.2026
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

    bool VulkanIndexBuffer::writeRange(const size_t offset, const void* data, const size_t size)
    {
        // A uint8 buffer is widened on upload, so its device bytes are not the caller's.
        if (format() == INDEXFORMAT_UINT8 || !data || size == 0 || !_allocator || !_buffer ||
            offset + size > _storage.size()) {
            return false;
        }
        if (!uploadStaging(data, size, offset)) {
            return false;
        }
        std::memcpy(_storage.data() + offset, data, size);
        return true;
    }

    bool VulkanIndexBuffer::uploadStaging(const void* data, size_t size, const size_t destinationOffset)
    {
        return vulkanEnqueueBufferUpload(*static_cast<VulkanGraphicsDevice*>(_device), _allocator, _buffer,
            destinationOffset, data, size, VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT, VK_ACCESS_2_INDEX_READ_BIT,
            "VulkanIndexBuffer");
    }
}

#endif // VISUTWIN_HAS_VULKAN
