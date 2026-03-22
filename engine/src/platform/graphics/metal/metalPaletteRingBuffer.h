// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 07.03.2026.
//
// Variable-size triple-buffered ring buffer for dynamic batch matrix palettes.
//
// Unlike MetalUniformRingBuffer (fixed-slot-size, 256B-aligned), this uses
// bump allocation to handle variable-size palette data efficiently:
//   - 10 instances → 640B + padding = 768B
//   - 33 instances → 2.1KB + padding = 2.25KB
//   - 330 instances → 21KB + padding = 21.25KB
//
// Same triple-buffered semaphore pattern as MetalUniformRingBuffer:
//   1. beginFrame()           — wait for GPU, advance to next 256KB region
//   2. allocate(data, size)   — bump-allocate, memcpy data, return offset
//   3. encoder->setVertexBufferOffset(offset, 6) — cheap offset-only bind
//   4. endFrame(commandBuffer) — register GPU completion signal
//
#pragma once

#include <Metal/Metal.hpp>
#include <dispatch/dispatch.h>
#include <cassert>
#include <cstring>

#include "spdlog/spdlog.h"

namespace visutwin::canvas
{
    class MetalPaletteRingBuffer
    {
    public:
        static constexpr int kMaxInflightFrames = 3;
        static constexpr size_t kAlignment = 256;          // Metal constant buffer offset alignment
        static constexpr size_t kRegionSize = 256 * 1024;  // 256KB per frame region
        // 256KB = 4096 instances × 64 bytes (float4x4).  This is the total budget
        // across ALL dynamic batches rendered in a single frame.

        MetalPaletteRingBuffer(MTL::Device* device, const char* label = "PaletteRing")
        {
            _totalSize = kMaxInflightFrames * kRegionSize;
            _buffer = device->newBuffer(_totalSize, MTL::ResourceStorageModeShared);
            if (_buffer) {
                _buffer->setLabel(NS::String::string(label, NS::UTF8StringEncoding));
                _basePtr = static_cast<uint8_t*>(_buffer->contents());
            }
            _frameSemaphore = dispatch_semaphore_create(kMaxInflightFrames);
        }

        ~MetalPaletteRingBuffer()
        {
            if (_buffer) {
                _buffer->release();
                _buffer = nullptr;
            }
        }

        // Non-copyable, non-movable
        MetalPaletteRingBuffer(const MetalPaletteRingBuffer&) = delete;
        MetalPaletteRingBuffer& operator=(const MetalPaletteRingBuffer&) = delete;
        MetalPaletteRingBuffer(MetalPaletteRingBuffer&&) = delete;
        MetalPaletteRingBuffer& operator=(MetalPaletteRingBuffer&&) = delete;

        /**
         * Call at frame start. Blocks if GPU hasn't finished with this region.
         * Must be called before any allocate() calls for the new frame.
         */
        void beginFrame()
        {
            dispatch_semaphore_wait(_frameSemaphore, DISPATCH_TIME_FOREVER);
            _frameIndex = (_frameIndex + 1) % kMaxInflightFrames;
            _writeOffset = 0;
        }

        /**
         * Bump-allocate palette data into the ring buffer.
         *
         * @param data Pointer to palette data (N × float4x4, column-major)
         * @param size Size in bytes of the palette data
         * @return Byte offset into the MTLBuffer — pass to setVertexBufferOffset()
         *
         * Returns SIZE_MAX if the allocation would exceed the frame region budget.
         */
        [[nodiscard]] size_t allocate(const void* data, size_t size)
        {
            assert(data != nullptr);
            assert(size > 0);

            const size_t alignedSize = alignUp(size, kAlignment);
            if (_writeOffset + alignedSize > kRegionSize) {
                spdlog::warn("PaletteRingBuffer: frame allocation exceeded {}KB budget "
                    "(requested {}B at offset {})", kRegionSize / 1024, size, _writeOffset);
                return SIZE_MAX;
            }

            const size_t absoluteOffset = static_cast<size_t>(_frameIndex) * kRegionSize + _writeOffset;
            std::memcpy(_basePtr + absoluteOffset, data, size);
            _writeOffset += alignedSize;
            return absoluteOffset;
        }

        /**
         * Register GPU completion signal on the frame's command buffer.
         * Must be called on the LAST command buffer committed per frame.
         */
        void endFrame(MTL::CommandBuffer* commandBuffer)
        {
            assert(commandBuffer != nullptr);
            dispatch_semaphore_t sem = _frameSemaphore;
            commandBuffer->addCompletedHandler(^(MTL::CommandBuffer*) {
                dispatch_semaphore_signal(sem);
            });
        }

        [[nodiscard]] MTL::Buffer* buffer() const { return _buffer; }
        [[nodiscard]] size_t writeOffset() const { return _writeOffset; }
        [[nodiscard]] size_t totalSize() const { return _totalSize; }

    private:
        static size_t alignUp(size_t value, size_t alignment)
        {
            return (value + alignment - 1) & ~(alignment - 1);
        }

        MTL::Buffer* _buffer = nullptr;
        uint8_t* _basePtr = nullptr;
        dispatch_semaphore_t _frameSemaphore = nullptr;

        size_t _totalSize = 0;
        int _frameIndex = -1;   // Will become 0 on first beginFrame()
        size_t _writeOffset = 0; // Bump pointer within current frame region
    };
}
