// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 21.03.2026
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
        // Also INDIRECT (a compute kernel writes draw arguments into one, as the GPU
        // instance culler does) and TRANSFER_SRC (read() copies out of it).
        bufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
            VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
            VK_BUFFER_USAGE_TRANSFER_DST_BIT;

        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;

        if (vmaCreateBuffer(_allocator, &bufferInfo, &allocInfo,
                &_buffer, &_allocation, nullptr) != VK_SUCCESS) {
            spdlog::error("VulkanVertexBuffer: GPU allocation failed");
            return;
        }

        if (!options.data.empty()) {
            unlock();
        }
    }

    VulkanVertexBuffer::VulkanVertexBuffer(GraphicsDevice* device,
        const std::shared_ptr<VertexFormat>& format, int numVertices,
        VkBuffer externalBuffer)
        : VertexBuffer(device, format, numVertices,
              numVertices * (format ? format->size() : 0)),
          _buffer(externalBuffer), _ownsBuffer(false)
    {
        auto* vkDev = static_cast<VulkanGraphicsDevice*>(device);
        _deviceRef = vkDev;
        _deviceAlive = vkDev->aliveToken();
        _allocator = vkDev->vmaAllocator();
    }

    VulkanVertexBuffer::~VulkanVertexBuffer()
    {
        if (_deviceRef && _deviceAlive.expired()) {
            return; // device gone — VMA allocator and buffers died with it
        }
        if (_ownsBuffer && _allocator != VK_NULL_HANDLE && _buffer != VK_NULL_HANDLE) {
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
        uploadRange(0, _storage.size());
    }

    void VulkanVertexBuffer::uploadRange(const size_t offset, const size_t size)
    {
        if (_storage.empty() || !_allocator || !_buffer || size == 0 || offset + size > _storage.size()) return;

        // Read as vertex attributes, and as storage by compute and storage draws.
        vulkanEnqueueBufferUpload(*static_cast<VulkanGraphicsDevice*>(_device), _allocator, _buffer, offset,
            _storage.data() + offset, size,
            VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
            "VulkanVertexBuffer");
    }

    bool VulkanVertexBuffer::read(const size_t offset, const size_t size, void* out)
    {
        auto* device = static_cast<VulkanGraphicsDevice*>(_device);
        if (_buffer == VK_NULL_HANDLE || !out || size == 0 || offset + size > static_cast<size_t>(numBytes())) {
            return false;
        }
        if (device->recording()) {
            // As VulkanTexture::read: the work that wrote the buffer may sit in a command
            // buffer not yet submitted, which a one-shot copy would run ahead of.
            spdlog::error("VulkanVertexBuffer::read: cannot read back while a frame or an "
                "offline scope is recording — close it first");
            return false;
        }
        // Queued uploads and compute dispatches go first, so the copy is ordered behind them.
        device->flushUploads();

        VkBuffer staging = VK_NULL_HANDLE;
        VmaAllocation allocation = nullptr;
        VmaAllocationInfo mapped{};
        if (!vulkanCreateReadbackBuffer(_allocator, size, staging, allocation, &mapped)) {
            spdlog::error("VulkanVertexBuffer::read: failed to allocate a {}-byte staging buffer", size);
            return false;
        }
        const VkBuffer source = _buffer;
        const bool submitted = device->runOneShotCommands([&](VkCommandBuffer cmd) {
            VkMemoryBarrier2 before{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
            before.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            before.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
            before.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
            before.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
            VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dependency.memoryBarrierCount = 1;
            dependency.pMemoryBarriers = &before;
            vkCmdPipelineBarrier2(cmd, &dependency);

            VkBufferCopy copy{};
            copy.srcOffset = offset;
            copy.size = size;
            vkCmdCopyBuffer(cmd, source, staging, 1, &copy);
            vulkanRecordCopyToHostBarrier(cmd);
        });
        if (submitted) {
            vmaInvalidateAllocation(_allocator, allocation, 0, size);
            std::memcpy(out, mapped.pMappedData, size);
        }
        vmaDestroyBuffer(_allocator, staging, allocation);
        return submitted;
    }
}

#endif // VISUTWIN_HAS_VULKAN
