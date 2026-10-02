// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 07.03.2026
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
// Same triple-buffered pattern as MetalUniformRingBuffer, paced by the same MetalFrameGate:
//   1. beginFrame()           — after the gate: grow if the last frame overflowed,
//                               advance to the next region
//   2. allocate(data, size, version) — bump-allocate, memcpy data, return offset;
//                               a repeated version returns this frame's first copy
//   3. encoder->setVertexBufferOffset(offset, 6) — cheap offset-only bind
//
#pragma once

#include <Metal/Metal.hpp>
#include "metalFrameGate.h"
#include <cassert>
#include <cstring>

#include "platform/graphics/paletteFrameAllocator.h"
#include "spdlog/spdlog.h"

namespace visutwin::canvas
{
    class MetalPaletteRingBuffer
    {
    public:
        static constexpr int kMaxInflightFrames = MetalFrameGate::kMaxInflightFrames;
        static constexpr size_t kAlignment = 256;          // Metal constant buffer offset alignment
        // Starting size of a frame region, not a limit: a frame that asks for more
        // grows the ring (see growIfNeeded). 256KB = 4096 matrices, across every
        // dynamic batch and skin drawn in one frame.
        static constexpr size_t kInitialRegionSize = 256 * 1024;

        MetalPaletteRingBuffer(MTL::Device* device, MetalFrameGate& gate, const char* label = "PaletteRing")
            : _device(device), _label(label), _gate(gate), _frame(kInitialRegionSize, kAlignment)
        {
            allocateBuffer(kInitialRegionSize);
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
         * Call at frame start, after MetalFrameGate::waitForFrame() has made the region
         * free. Must be called before any allocate() calls for the new frame.
         */
        void beginFrame()
        {
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

            _gate.withOtherFramesDrained([&] {
                allocateBuffer(wanted);
                _frame.setRegionSize(wanted);
            });

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
        MetalFrameGate& _gate;

        size_t _totalSize = 0;
        int _frameIndex = -1;   // Will become 0 on first beginFrame()
        // Offsets, sharing and demand of the frame in progress.
        PaletteFrameAllocator _frame;
    };
}
