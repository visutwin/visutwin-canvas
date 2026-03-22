// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 19.02.2026.
//
#pragma once

#include <Metal/Metal.hpp>
#include <dispatch/dispatch.h>
#include <cassert>
#include <cstring>

namespace visutwin::canvas
{
    /**
     * Triple-buffered ring buffer for per-draw uniform data.
     *
     * Replaces setVertexBytes()/setFragmentBytes() with a pre-allocated
     * MTLBuffer where each draw call's uniforms are sub-allocated from a
     * linearly-advancing write cursor. Three frame regions prevent CPU/GPU
     * contention without explicit fencing per draw.
     *
     * Apple Metal Best Practices Guide recommends:
     * - setVertexBytes() for data < 4 KB (acceptable for small draw counts)
     * - Persistent MTLBuffer + setVertexBufferOffset() for high draw counts
     * - Triple buffering with dispatch_semaphore_t for CPU/GPU sync
     * - 256-byte alignment for constant buffer offsets
     *
     * Usage:
     *   1. beginFrame()           -- wait for GPU, advance to next region
     *   2. allocate(data, size)   -- write per-draw data, get buffer offset
     *   3. encoder->setVertexBufferOffset(offset, index)  -- cheap offset-only bind
     *   4. endFrame(commandBuffer) -- register GPU completion signal
     */
    class MetalUniformRingBuffer
    {
    public:
        static constexpr int kMaxInflightFrames = 3;
        static constexpr size_t kAlignment = 256; // Metal constant buffer offset alignment

        /**
         * @param device           Metal device for buffer allocation
         * @param maxDrawsPerFrame Maximum draw calls expected per frame (grows if exceeded)
         * @param uniformStructSize Size of the largest uniform struct this ring will hold
         * @param label            Debug label for Metal GPU capture
         */
        MetalUniformRingBuffer(MTL::Device* device, size_t maxDrawsPerFrame,
                               size_t uniformStructSize, const char* label = "UniformRing")
        {
            _alignedSlotSize = alignUp(uniformStructSize, kAlignment);
            _regionSize = maxDrawsPerFrame * _alignedSlotSize;
            _totalSize = kMaxInflightFrames * _regionSize;
            _maxDrawsPerFrame = maxDrawsPerFrame;

            _buffer = device->newBuffer(_totalSize, MTL::ResourceStorageModeShared);
            if (_buffer) {
                _buffer->setLabel(NS::String::string(label, NS::UTF8StringEncoding));
                _basePtr = static_cast<uint8_t*>(_buffer->contents());
            }

            _frameSemaphore = dispatch_semaphore_create(kMaxInflightFrames);
        }

        ~MetalUniformRingBuffer()
        {
            if (_buffer) {
                _buffer->release();
                _buffer = nullptr;
            }
        }

        // Non-copyable, non-movable
        MetalUniformRingBuffer(const MetalUniformRingBuffer&) = delete;
        MetalUniformRingBuffer& operator=(const MetalUniformRingBuffer&) = delete;
        MetalUniformRingBuffer(MetalUniformRingBuffer&&) = delete;
        MetalUniformRingBuffer& operator=(MetalUniformRingBuffer&&) = delete;

        /**
         * Call at frame start. Blocks if GPU hasn't finished with this region.
         * Must be called before any allocate() calls for the new frame.
         */
        void beginFrame()
        {
            dispatch_semaphore_wait(_frameSemaphore, DISPATCH_TIME_FOREVER);
            _frameIndex = (_frameIndex + 1) % kMaxInflightFrames;
            _drawCount = 0;
        }

        /**
         * Write uniform data for one draw call into the ring buffer.
         *
         * @param data     Pointer to uniform struct data
         * @param dataSize Size in bytes of the data to copy (must be <= alignedSlotSize)
         * @return Byte offset into the MTLBuffer — pass to setVertexBufferOffset/setFragmentBufferOffset
         */
        [[nodiscard]] size_t allocate(const void* data, size_t dataSize)
        {
            assert(data != nullptr);
            assert(dataSize <= _alignedSlotSize && "Data exceeds aligned slot size");
            assert(_drawCount < _maxDrawsPerFrame && "Ring buffer overflow: too many draws this frame");

            const size_t offset = _frameIndex * _regionSize + _drawCount * _alignedSlotSize;
            std::memcpy(_basePtr + offset, data, dataSize);
            ++_drawCount;
            return offset;
        }

        /**
         * Register GPU completion signal on the frame's command buffer.
         * Must be called on the LAST command buffer committed per frame
         * (typically the present buffer in onFrameEnd).
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
        [[nodiscard]] size_t alignedSlotSize() const { return _alignedSlotSize; }
        [[nodiscard]] size_t currentDrawCount() const { return _drawCount; }
        [[nodiscard]] size_t maxDrawsPerFrame() const { return _maxDrawsPerFrame; }
        [[nodiscard]] size_t totalSize() const { return _totalSize; }

    private:
        static size_t alignUp(size_t value, size_t alignment)
        {
            return (value + alignment - 1) & ~(alignment - 1);
        }

        MTL::Buffer* _buffer = nullptr;
        uint8_t* _basePtr = nullptr;
        dispatch_semaphore_t _frameSemaphore = nullptr;

        size_t _alignedSlotSize = 0;
        size_t _regionSize = 0;
        size_t _totalSize = 0;
        size_t _maxDrawsPerFrame = 0;

        int _frameIndex = -1; // Will become 0 on first beginFrame()
        size_t _drawCount = 0;
    };
}
