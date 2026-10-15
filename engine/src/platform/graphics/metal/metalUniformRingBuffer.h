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
#include <vector>

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
     * The ring starts SMALL and grows inside the frame that needs more: an allocation
     * that does not fit replaces the buffer with one twice the size and continues there.
     * Every offset already handed out still refers to the OLD buffer, which the draws
     * encoded so far have bound, so the old buffer is kept until the frames that may read
     * it have completed (kMaxInflightFrames frame starts later). A caller therefore binds
     * an offset against buffer(), and must re-bind the BUFFER, not just the offset, when
     * buffer() differs from the one it bound (MetalUniformBinder does).
     *
     * Usage:
     *   1. beginFrame()           -- after the device's frame gate: advance to the next
     *                                 region, free buffers no frame in flight reads
     *   2. allocate(data, size)   -- write per-draw data, get buffer offset
     *   3. bind buffer() at that offset (offset only while buffer() is unchanged)
     */
    class MetalUniformRingBuffer
    {
    public:
        static constexpr int kMaxInflightFrames = MetalFrameGate::kMaxInflightFrames;
        static constexpr size_t kAlignment = 256; // Metal constant buffer offset alignment

        /**
         * @param device           Metal device for buffer allocation
         * @param maxDrawsPerFrame Starting capacity in allocations per frame. Not a limit:
         *                         a frame that asks for more grows the ring in place.
         * @param uniformStructSize Size of the largest uniform struct this ring will hold
         * @param label            Debug label for Metal GPU capture
         */
        MetalUniformRingBuffer(MTL::Device* device, size_t maxDrawsPerFrame,
                               size_t uniformStructSize, const char* label = "UniformRing")
            : _device(device), _label(label)
        {
            _alignedSlotSize = alignUp(uniformStructSize, kAlignment);
            allocateBuffer(std::max<size_t>(maxDrawsPerFrame, 1));
        }

        ~MetalUniformRingBuffer()
        {
            for (const auto& retired : _retired) {
                retired.buffer->release();
            }
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
            // The gate has just waited out the frame kMaxInflightFrames before this one,
            // so a buffer retired that many frame starts ago is read by nothing.
            std::erase_if(_retired, [](Retired& retired) {
                if (--retired.framesLeft > 0) {
                    return false;
                }
                retired.buffer->release();
                return true;
            });
            _frameIndex = (_frameIndex + 1) % kMaxInflightFrames;
            _drawCount = 0;
        }

        /**
         * Write uniform data for one draw call into the ring buffer.
         *
         * @param data     Pointer to uniform struct data
         * @param dataSize Size in bytes of the data to copy (must be <= alignedSlotSize)
         * @return Byte offset into buffer() as it is AFTER the call
         */
        [[nodiscard]] size_t allocate(const void* data, size_t dataSize)
        {
            assert(data != nullptr);
            assert(dataSize <= _alignedSlotSize && "Data exceeds aligned slot size");
            if (_drawCount >= _maxDrawsPerFrame) [[unlikely]] {
                grow();
            }
            if (!_basePtr) {
                return 0;
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
        /// The current buffer plus any retired one frames in flight may still read.
        [[nodiscard]] size_t totalSize() const
        {
            size_t total = _totalSize;
            for (const auto& retired : _retired) {
                total += retired.buffer->length();
            }
            return total;
        }

    private:
        struct Retired
        {
            MTL::Buffer* buffer;
            int framesLeft;
        };

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
            _buffer = _device ? _device->newBuffer(_totalSize, MTL::ResourceStorageModeShared) : nullptr;
            _basePtr = nullptr;
            if (_buffer) {
                _buffer->setLabel(NS::String::string(_label, NS::UTF8StringEncoding));
                _basePtr = static_cast<uint8_t*>(_buffer->contents());
            }
        }

        /**
         * Replace the buffer with one of twice the capacity, mid-frame. The draws encoded
         * so far keep the old buffer bound and read their offsets from it, so it is retired
         * rather than freed; this frame continues in its own region of the new buffer, whose
         * every region is unused by the GPU.
         */
        void grow()
        {
            const size_t previous = _maxDrawsPerFrame;
            if (_buffer) {
                _retired.push_back({_buffer, kMaxInflightFrames});
            }
            allocateBuffer(previous * 2);
            _drawCount = 0;
            spdlog::debug("{}: grown from {} to {} slots a frame ({} KB)", _label, previous,
                _maxDrawsPerFrame, _totalSize / 1024);
        }

        MTL::Device* _device = nullptr;
        const char* _label = "UniformRing";
        MTL::Buffer* _buffer = nullptr;
        uint8_t* _basePtr = nullptr;
        std::vector<Retired> _retired;

        size_t _alignedSlotSize = 0;
        size_t _regionSize = 0;
        size_t _totalSize = 0;
        size_t _maxDrawsPerFrame = 0;

        int _frameIndex = -1; // Will become 0 on first beginFrame()
        size_t _drawCount = 0;
    };
}
