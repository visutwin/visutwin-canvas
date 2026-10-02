// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 19.02.2026
//
#pragma once

#include <Metal/Metal.hpp>
#include "metalFrameGate.h"
#include <spdlog/spdlog.h>
#include <algorithm>
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
     * - Triple buffering, paced by the device's MetalFrameGate
     * - 256-byte alignment for constant buffer offsets
     *
     * Usage:
     *   1. beginFrame()           -- after the device's frame gate: grow if the last
     *                                 frame overflowed, advance to the next region
     *   2. allocate(data, size)   -- write per-draw data, get buffer offset
     *   3. encoder->setVertexBufferOffset(offset, index)  -- cheap offset-only bind
     *   The gate's completion signal on the frame's last command buffer is what
     *   frees a region again (MetalFrameGate).
     */
    class MetalUniformRingBuffer
    {
    public:
        static constexpr int kMaxInflightFrames = MetalFrameGate::kMaxInflightFrames;
        static constexpr size_t kAlignment = 256; // Metal constant buffer offset alignment

        /**
         * @param device           Metal device for buffer allocation
         * @param maxDrawsPerFrame Starting capacity in draw calls per frame. Not a
         *                         limit: a frame that asks for more grows the ring
         *                         (see growIfNeeded), at the cost of one wrong frame.
         * @param uniformStructSize Size of the largest uniform struct this ring will hold
         * @param label            Debug label for Metal GPU capture
         */
        MetalUniformRingBuffer(MTL::Device* device, MetalFrameGate& gate, size_t maxDrawsPerFrame,
                               size_t uniformStructSize, const char* label = "UniformRing")
            : _device(device), _label(label), _gate(gate)
        {
            _alignedSlotSize = alignUp(uniformStructSize, kAlignment);
            allocateBuffer(maxDrawsPerFrame);
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
         * Call at frame start, after MetalFrameGate::waitForFrame() has made the region
         * free. Must be called before any allocate() calls for the new frame.
         */
        void beginFrame()
        {
            growIfNeeded();
            _frameIndex = (_frameIndex + 1) % kMaxInflightFrames;
            _drawCount = 0;
            _requestedDraws = 0;
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
            if (!_basePtr) {
                return 0;
            }
            // Overflowing the frame region would memcpy into the next in-flight
            // frame's data (or past the MTLBuffer). Reuse the last slot without
            // modifying it: overwriting it would also change the uniforms of the
            // already-encoded draw that owns that slot before the GPU reads it.
            // The excess draws are wrong for THIS frame and cannot be made right —
            // every offset already handed out is referenced by an encoded draw — so
            // the demand is counted instead and the ring grows at the next frame
            // boundary, where a full GPU drain makes reallocation safe.
            ++_requestedDraws;
            if (_drawCount >= _maxDrawsPerFrame) [[unlikely]] {
                const size_t lastSlot = _maxDrawsPerFrame - 1;
                return regionIndex() * _regionSize + lastSlot * _alignedSlotSize;
            }

            const size_t offset = regionIndex() * _regionSize + _drawCount * _alignedSlotSize;
            std::memcpy(_basePtr + offset, data, std::min(dataSize, _alignedSlotSize));
            ++_drawCount;
            return offset;
        }

        /**
         * Work encoded BEFORE the first frame (an environment bake at load time) writes
         * into region 0, which frame 0 reuses. The device calls this once that work has
         * COMPLETED on the GPU, so the next load-time bake, and frame 0, start the region
         * over rather than piling up in it. No-op once frames have begun.
         */
        void resetBeforeFirstFrame()
        {
            if (_frameIndex < 0) {
                _drawCount = 0;
            }
        }

        [[nodiscard]] MTL::Buffer* buffer() const { return _buffer; }
        [[nodiscard]] size_t alignedSlotSize() const { return _alignedSlotSize; }
        [[nodiscard]] size_t currentDrawCount() const { return _drawCount; }
        [[nodiscard]] size_t maxDrawsPerFrame() const { return _maxDrawsPerFrame; }
        [[nodiscard]] size_t totalSize() const { return _totalSize; }

    private:
        // Region 0 until the first beginFrame() advances the cursor onto it.
        [[nodiscard]] size_t regionIndex() const { return _frameIndex < 0 ? 0 : static_cast<size_t>(_frameIndex); }

        static size_t alignUp(size_t value, size_t alignment)
        {
            return (value + alignment - 1) & ~(alignment - 1);
        }

        void allocateBuffer(const size_t maxDrawsPerFrame)
        {
            _maxDrawsPerFrame = maxDrawsPerFrame;
            _regionSize = maxDrawsPerFrame * _alignedSlotSize;
            _totalSize = kMaxInflightFrames * _regionSize;

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
         * Reallocate to fit the demand the PREVIOUS frame actually had. Growing
         * by taking another buffer from a pool cannot be done mid-frame here, because an offset is only meaningful
         * against the buffer bound at the start of the render pass. So the growth
         * happens here, at a frame boundary, behind a full drain: the caller has
         * already waited for THIS region, and the other kMaxInflightFrames - 1
         * regions are waited for below, after which nothing references the old
         * buffer and it can be replaced. The drain is why this is worth avoiding
         * rather than relying on — but it happens once per size increase, not per
         * frame, and the alternative is a permanently wrong frame.
         */
        void growIfNeeded()
        {
            if (_requestedDraws <= _maxDrawsPerFrame) {
                return;
            }
            const size_t wanted = std::max(_requestedDraws, _maxDrawsPerFrame * 2);

            const size_t previous = _maxDrawsPerFrame;
            _gate.withOtherFramesDrained([&] { allocateBuffer(wanted); });

            spdlog::warn("{}: {} allocations requested but only {} fit; the excess shared one "
                         "uniform slot for that frame. Grown to {} ({} MB); the next frame is correct.",
                         _label, _requestedDraws, previous, _maxDrawsPerFrame,
                         _totalSize / (1024 * 1024));
            _frameIndex = -1;   // the region cursor restarts against the new buffer
        }

        MTL::Device* _device = nullptr;
        const char* _label = "UniformRing";
        MTL::Buffer* _buffer = nullptr;
        uint8_t* _basePtr = nullptr;
        MetalFrameGate& _gate;

        size_t _alignedSlotSize = 0;
        size_t _regionSize = 0;
        size_t _totalSize = 0;
        size_t _maxDrawsPerFrame = 0;

        int _frameIndex = -1; // Will become 0 on first beginFrame()
        size_t _drawCount = 0;
        // Allocations ASKED for this frame, including those that did not fit. The
        // difference from _drawCount is what the ring has to grow by.
        size_t _requestedDraws = 0;
    };
}
