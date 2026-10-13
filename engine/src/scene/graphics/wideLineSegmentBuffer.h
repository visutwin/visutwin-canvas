// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 15.09.2026
//
// Sizing and upload of the WideLineRenderer's per-segment instance buffer.
//
// Kept apart from the renderer so it can be tested against a stub device: the
// renderer needs an Engine, this needs only something that creates vertex buffers.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/vertexBuffer.h"
#include "platform/graphics/vertexFormat.h"

namespace visutwin::canvas::wideline
{
    /// The capacity, in records, the segment buffer needs to hold `required`
    /// records. It never shrinks, and it grows geometrically so a line set that
    /// gains one segment a frame does not reallocate every frame.
    [[nodiscard]] inline int segmentBufferCapacity(const int currentCapacity, const int required)
    {
        if (required <= currentCapacity) {
            return currentCapacity;
        }
        return std::max(required, currentCapacity * 2);
    }

    /**
     * Uploads `count` records of `recordSize` bytes into the FRONT of `buffer`,
     * reallocating it only when it is too small, and updates `capacity` to match.
     *
     * The payload is always the buffer's full size. `VertexBuffer::setData` accepts
     * nothing else, so uploading only the live records into a buffer sized for an
     * earlier, larger set is refused outright and the lines stop updating. The
     * records past `count` are zeroed and never drawn: the draw's instance count is
     * given explicitly through `MeshInstance::setStorageDraw`, and both backends
     * bind the whole buffer regardless of it.
     *
     * A count of zero uploads nothing and keeps the buffer for reuse.
     * Returns false when the device refused the allocation or the upload.
     */
    inline bool uploadSegmentRecords(GraphicsDevice& device, std::shared_ptr<VertexBuffer>& buffer,
        int& capacity, const uint8_t* records, const int count, const int recordSize)
    {
        if (count <= 0) {
            return true;
        }

        const int currentCapacity = buffer != nullptr ? capacity : 0;
        const int newCapacity = segmentBufferCapacity(currentCapacity, count);

        std::vector<uint8_t> bytes(static_cast<size_t>(newCapacity) * static_cast<size_t>(recordSize), 0);
        std::memcpy(bytes.data(), records, static_cast<size_t>(count) * static_cast<size_t>(recordSize));

        if (buffer == nullptr || newCapacity != currentCapacity) {
            auto format = std::make_shared<VertexFormat>(recordSize, true, false);
            VertexBufferOptions options;
            options.usage = BUFFER_DYNAMIC;
            options.data = std::move(bytes);
            buffer = device.createVertexBuffer(format, newCapacity, options);
            capacity = buffer != nullptr ? newCapacity : 0;
            return buffer != nullptr;
        }

        return buffer->setData(bytes);
    }

    /**
     * The segment buffers of one renderer, one per frame in flight. On Metal an upload is a
     * copy into memory the GPU reads directly, and up to `maxFramesInFlight()` earlier frames
     * may still be drawing the lines they were given, so a renderer whose lines change every
     * frame must not rewrite the buffer a frame in flight draws from.
     *
     * `upload` writes into the slot of the current frame, named by `frameKey`
     * (`GraphicsDevice::renderVersion()`): a new key moves to the next slot, the same key
     * rewrites the slot already written for it, which no frame has drawn yet. The key moves at
     * every frame start, so writes on either side of a render go to different slots, and a slot
     * comes back round only after `maxFramesInFlight()` frames, by which time the frame gate has
     * retired the frame that drew it. Each slot grows on its own (uploadSegmentRecords).
     */
    struct SegmentBufferRing
    {
        struct Slot
        {
            std::shared_ptr<VertexBuffer> buffer;
            int capacity = 0;
        };

        std::vector<Slot> slots;
        size_t current = 0;
        bool written = false;
        int lastKey = 0;

        /// Uploads `count` records into the frame's slot; returns its buffer, or null when the
        /// device refused it. A count of zero uploads nothing and returns null.
        std::shared_ptr<VertexBuffer> upload(GraphicsDevice& device, const uint8_t* records, const int count,
            const int recordSize, const int frameKey)
        {
            if (count <= 0) {
                return nullptr;
            }
            const size_t size = static_cast<size_t>(std::max(device.maxFramesInFlight(), 1));
            if (slots.size() != size) {
                slots.resize(size);
                current = 0;
                written = false;
            }
            if (written && frameKey != lastKey) {
                current = (current + 1) % slots.size();
            }
            written = true;
            lastKey = frameKey;

            Slot& slot = slots[current];
            if (!uploadSegmentRecords(device, slot.buffer, slot.capacity, records, count, recordSize)) {
                return nullptr;
            }
            return slot.buffer;
        }
    };
}
