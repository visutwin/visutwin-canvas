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

#include <cassert>
#include <cstring>
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
            bufInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;

            VmaAllocationCreateInfo allocInfo{};
            allocInfo.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
            allocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;

            VmaAllocationInfo info{};
            vmaCreateBuffer(_allocator, &bufInfo, &allocInfo, &_buffer, &_allocation, &info);
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
        // Returns 0 and logs nothing on overflow — the slot is clamped to the
        // region start so a malformed frame degrades rather than corrupts.
        [[nodiscard]] uint32_t allocate(const void* data, VkDeviceSize size)
        {
            const VkDeviceSize aligned = alignUp(size, _alignment);
            if (_cursor + aligned > _regionSize) {
                assert(false && "VulkanUniformRingBuffer region overflow");
                return static_cast<uint32_t>(_frameIndex * _regionSize);
            }
            const VkDeviceSize offset = _frameIndex * _regionSize + _cursor;
            if (_mapped && data) {
                std::memcpy(_mapped + offset, data, size);
            }
            _cursor += aligned;
            return static_cast<uint32_t>(offset);
        }

        [[nodiscard]] VkBuffer buffer() const { return _buffer; }

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
