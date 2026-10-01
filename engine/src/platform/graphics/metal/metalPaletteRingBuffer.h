// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 07.03.2026.
//
// Variable-size triple-buffered ring buffer for matrix palettes (dynamic batches and
// GPU skinning).
//
// Unlike MetalUniformRingBuffer (fixed-slot-size, 256B-aligned), this uses
// bump allocation to handle variable-size palette data efficiently:
//   - 10 instances → 640B + padding = 768B
//   - 33 instances → 2.1KB + padding = 2.25KB
//   - 330 instances → 21KB + padding = 21.25KB
//
// Same triple-buffered semaphore pattern as MetalUniformRingBuffer:
//   1. beginFrame()           — wait for GPU, grow if last frame overflowed,
//                               advance to the next region
//   2. allocate(data, size, version) — bump-allocate, memcpy data, return offset;
//                               a repeated version returns this frame's first copy
//   3. encoder->setVertexBufferOffset(offset, 6) — cheap offset-only bind
//   4. endFrame(commandBuffer) — register GPU completion signal
//
#pragma once

#include <Metal/Metal.hpp>
#include <dispatch/dispatch.h>
#include <cassert>
#include <cstring>

#include "platform/graphics/paletteFrameAllocator.h"
#include "spdlog/spdlog.h"

namespace visutwin::canvas
{
    class MetalPaletteRingBuffer
    {
    public:
        static constexpr int kMaxInflightFrames = 3;
        static constexpr size_t kAlignment = 256;          // Metal constant buffer offset alignment
        // Starting size of a frame region, not a limit: a frame that asks for more
        // grows the ring (see growIfNeeded). 256KB = 4096 matrices, across every
        // dynamic batch and skin drawn in one frame.
        static constexpr size_t kInitialRegionSize = 256 * 1024;

        MetalPaletteRingBuffer(MTL::Device* device, const char* label = "PaletteRing")
            : _device(device), _label(label), _frame(kInitialRegionSize, kAlignment)
        {
            allocateBuffer(kInitialRegionSize);
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
            growIfNeeded();
            _frameIndex = (_frameIndex + 1) % kMaxInflightFrames;
            _frame.beginFrame();
        }

        /// As MetalUniformRingBuffer::resetBeforeFirstFrame: load-time work has completed,
        /// so region 0 starts over. No-op once frames have begun.
        void resetBeforeFirstFrame()
        {
            if (_frameIndex < 0) {
                _frame.beginFrame();
            }
        }

        /**
         * Place palette data in this frame's region.
         *
         * @param data Pointer to palette data (N × float4x4, column-major)
         * @param size Size in bytes of the palette data
         * @param contentVersion Nonzero names the contents (GraphicsDevice::
         *        nextPaletteVersion): every draw carrying the same version this frame
         *        gets the one copy. 0 is never shared.
         * @return Byte offset into the MTLBuffer — pass to setVertexBufferOffset()
         *
         * Returns SIZE_MAX if the palette does not fit the frame region. The frame is
         * then wrong for that draw and cannot be made right — the buffer is bound for
         * the whole pass — so the demand is counted and the ring grows at the next
         * frame boundary.
         */
        [[nodiscard]] size_t allocate(const void* data, const size_t size, const uint64_t contentVersion = 0)
        {
            assert(data != nullptr);
            assert(size > 0);

            if (!_basePtr || !data || size == 0) {
                return SIZE_MAX;
            }

            const auto allocation = _frame.allocate(size, contentVersion);
            if (!allocation.offset) {
                return SIZE_MAX;
            }

            // Region 0 before the first beginFrame(), as MetalUniformRingBuffer
            const size_t region = _frameIndex < 0 ? 0 : static_cast<size_t>(_frameIndex);
            const size_t absoluteOffset = region * _frame.regionSize() + *allocation.offset;
            if (allocation.isNew) {
                std::memcpy(_basePtr + absoluteOffset, data, size);
            }
            return absoluteOffset;
        }

        /**
         * Register GPU completion signal on the frame's command buffer.
         * Must be called on the LAST command buffer committed per frame.
         */
        void endFrame(MTL::CommandBuffer* commandBuffer)
        {
            dispatch_semaphore_t sem = _frameSemaphore;
            if (!commandBuffer) {
                dispatch_semaphore_signal(sem);
                return;
            }
            commandBuffer->addCompletedHandler(^(MTL::CommandBuffer*) {
                dispatch_semaphore_signal(sem);
            });
        }

        [[nodiscard]] MTL::Buffer* buffer() const { return _buffer; }
        [[nodiscard]] size_t writeOffset() const { return _frame.writeOffset(); }
        [[nodiscard]] size_t totalSize() const { return _totalSize; }

    private:
        void allocateBuffer(const size_t regionSize)
        {
            _totalSize = kMaxInflightFrames * regionSize;
            if (_buffer) {
                _buffer->release();
                _buffer = nullptr;
                _basePtr = nullptr;
            }
            _buffer = _device ? _device->newBuffer(_totalSize, MTL::ResourceStorageModeShared) : nullptr;
            if (_buffer) {
                _buffer->setLabel(NS::String::string(_label, NS::UTF8StringEncoding));
                _basePtr = static_cast<uint8_t*>(_buffer->contents());
            }
        }

        /**
         * Reallocate to fit what the PREVIOUS frame asked for, behind a full drain, as
         * MetalUniformRingBuffer::growIfNeeded does and for the same reason: an offset
         * means something only against the buffer bound when the pass began, so the
         * buffer can be replaced only where nothing in flight references it. The caller
         * has waited for this region; the other regions are waited for here.
         */
        void growIfNeeded()
        {
            if (!_frame.overflowed()) {
                return;
            }
            const size_t requested = _frame.requestedBytes();
            const size_t previous = _frame.regionSize();
            const size_t wanted = _frame.wantedRegionSize();

            for (int i = 1; i < kMaxInflightFrames; ++i) {
                dispatch_semaphore_wait(_frameSemaphore, DISPATCH_TIME_FOREVER);
            }
            allocateBuffer(wanted);
            _frame.setRegionSize(wanted);
            for (int i = 1; i < kMaxInflightFrames; ++i) {
                dispatch_semaphore_signal(_frameSemaphore);
            }

            spdlog::warn("{}: {} KB of matrix palettes requested in a frame but only {} KB fit; the "
                         "excess draws kept the palette bound before them for that frame. Grown to "
                         "{} KB a frame; the next frame is correct.",
                         _label, requested / 1024, previous / 1024, wanted / 1024);
            _frameIndex = -1;   // the region cursor restarts against the new buffer
        }

        MTL::Device* _device = nullptr;
        const char* _label = "PaletteRing";
        MTL::Buffer* _buffer = nullptr;
        uint8_t* _basePtr = nullptr;
        dispatch_semaphore_t _frameSemaphore = nullptr;

        size_t _totalSize = 0;
        int _frameIndex = -1;   // Will become 0 on first beginFrame()
        // Offsets, sharing and demand of the frame in progress.
        PaletteFrameAllocator _frame;
    };
}
