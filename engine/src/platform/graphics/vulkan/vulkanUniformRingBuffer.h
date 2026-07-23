// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Triple-buffered host-visible ring buffer for per-draw / per-pass uniform data.
//
// Mirrors the Metal MetalUniformRingBuffer: one persistently-mapped VkBuffer is
// split into kFramesInFlight regions.  Each frame uses one region as a linear
// bump allocator; the offset returned by allocate() is bound through a
// VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC descriptor so a single descriptor
// set serves every draw (only the dynamic offset changes).
//
// Frame-to-frame reuse safety is provided by the device's per-frame
// inFlightFence: onFrameStart waits on it before calling beginFrame(), which
// guarantees the GPU has finished reading the region we are about to overwrite.
//
#pragma once

#ifdef VISUTWIN_HAS_VULKAN

#include <cstring>
#include <limits>
#include <optional>
#include <stdexcept>
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

namespace visutwin::canvas
{
    class VulkanUniformRingBuffer
    {
    public:
        VulkanUniformRingBuffer(VmaAllocator allocator, uint32_t framesInFlight,
            VkDeviceSize regionSize, VkDeviceSize offsetAlignment)
            : _allocator(allocator)
            , _framesInFlight(framesInFlight)
            , _alignment(offsetAlignment > 0 ? offsetAlignment : 1)
            , _regionSize(alignUp(regionSize, offsetAlignment))
        {
            _totalSize = _regionSize * _framesInFlight;

            VkBufferCreateInfo bufInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            bufInfo.size = _totalSize;
            // Geometry palettes share this fence-gated upload ring with
            // uniforms. Individual allocations are exposed through either
            // UBO or SSBO descriptors depending on the shader contract.
            bufInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;

            VmaAllocationCreateInfo allocInfo{};
            allocInfo.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
            allocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;

            VmaAllocationInfo info{};
            const VkResult result =
                vmaCreateBuffer(_allocator, &bufInfo, &allocInfo, &_buffer, &_allocation, &info);
            if (result != VK_SUCCESS || info.pMappedData == nullptr) {
                if (_buffer != VK_NULL_HANDLE) {
                    vmaDestroyBuffer(_allocator, _buffer, _allocation);
                    _buffer = VK_NULL_HANDLE;
                    _allocation = VK_NULL_HANDLE;
                }
                throw std::runtime_error(
                    "VulkanUniformRingBuffer: failed to create mapped uniform buffer");
            }
            _mapped = static_cast<uint8_t*>(info.pMappedData);
        }

        ~VulkanUniformRingBuffer()
        {
            if (_allocator != VK_NULL_HANDLE && _buffer != VK_NULL_HANDLE) {
                vmaDestroyBuffer(_allocator, _buffer, _allocation);
            }
        }

        VulkanUniformRingBuffer(const VulkanUniformRingBuffer&) = delete;
        VulkanUniformRingBuffer& operator=(const VulkanUniformRingBuffer&) = delete;

        // Begin a frame's region.  The caller must have already waited on the
        // fence that gates this frame index, so the region is free to reuse.
        void beginFrame(uint32_t frameIndex)
        {
            _frameIndex = frameIndex % _framesInFlight;
            _cursor = 0;
        }

        // Copy `size` bytes into the current region and return the absolute
        // byte offset into the buffer (suitable as a dynamic descriptor offset).
        // Failure is explicit: callers must skip the draw/pass rather than
        // accidentally aliasing offset zero or an earlier allocation.
        [[nodiscard]] std::optional<uint32_t> allocate(
            const void* data, VkDeviceSize size)
        {
            if (data == nullptr || size == 0 || size > _regionSize ||
                size > std::numeric_limits<VkDeviceSize>::max() - (_alignment - 1)) {
                return std::nullopt;
            }
            const VkDeviceSize aligned = alignUp(size, _alignment);
            if (_cursor > _regionSize - aligned) {
                return std::nullopt;
            }
            const VkDeviceSize offset = _frameIndex * _regionSize + _cursor;
            if (offset > std::numeric_limits<uint32_t>::max()) {
                return std::nullopt;
            }
            std::memcpy(_mapped + offset, data, size);
            _cursor += aligned;
            // VMA rounds this range to nonCoherentAtomSize as needed. On
            // coherent heaps this is a no-op; on discrete/non-coherent heaps
            // it is required before the GPU can observe the copied uniforms.
            if (vmaFlushAllocation(_allocator, _allocation, offset, size) !=
                VK_SUCCESS) {
                return std::nullopt;
            }
            return static_cast<uint32_t>(offset);
        }

        [[nodiscard]] VkBuffer buffer() const { return _buffer; }
        [[nodiscard]] VkDeviceSize usedBytes() const { return _cursor; }
        [[nodiscard]] VkDeviceSize capacityPerFrame() const { return _regionSize; }

    private:
        static VkDeviceSize alignUp(VkDeviceSize value, VkDeviceSize alignment)
        {
            if (alignment <= 1) return value;
            return (value + alignment - 1) & ~(alignment - 1);
        }

        VmaAllocator _allocator = VK_NULL_HANDLE;
        VkBuffer _buffer = VK_NULL_HANDLE;
        VmaAllocation _allocation = VK_NULL_HANDLE;
        uint8_t* _mapped = nullptr;

        uint32_t _framesInFlight = 1;
        VkDeviceSize _alignment = 1;
        VkDeviceSize _regionSize = 0;
        VkDeviceSize _totalSize = 0;

        uint32_t _frameIndex = 0;
        VkDeviceSize _cursor = 0;
    };
}

#endif // VISUTWIN_HAS_VULKAN
