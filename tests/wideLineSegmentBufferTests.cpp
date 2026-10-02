// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 15.09.2026
//
// The WideLineRenderer's segment buffer across growing, shrinking and empty sets.
//
// A renderer that uploads exactly the live records into a buffer sized for the
// largest set so far fails: VertexBuffer::setData refuses any payload that is not the
// buffer's full size, so the first frame with FEWER segments logs an error and
// every frame after it draws stale lines. Nothing visual shows this unless a line
// set shrinks while the example is being watched, which is why it is pinned here
// against a stub device instead.
//
// What is pinned:
//   - the control: setData really does refuse a short payload (the contract the
//     defect ran into; if it ever stops refusing, this test says so);
//   - grow, shrink, zero and grow-past-capacity all upload successfully, put the
//     live records at the front, and reallocate only when the buffer is too small;
//   - the capacity never shrinks and grows geometrically.

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <memory>
#include <vector>

#include "platform/graphics/vertexBuffer.h"
#include "scene/graphics/wideLineSegmentBuffer.h"
#include "support/check.h"
#include "support/stubDevice.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

namespace
{
    constexpr int kRecordSize = 8 * 4 * static_cast<int>(sizeof(float));

    /// A CPU-only vertex buffer that counts its uploads.
    class CountingVertexBuffer final : public CpuVertexBuffer
    {
    public:
        CountingVertexBuffer(GraphicsDevice* device, const std::shared_ptr<VertexFormat>& format,
            const int numVertices, const VertexBufferOptions& options)
            : CpuVertexBuffer(device, format, numVertices, options) {}

        void unlock() override { ++uploads; }

        int uploads = 0;
    };

    /// A device that creates CPU-only vertex buffers and nothing else.
    class StubDevice final : public StubGraphicsDevice
    {
    public:
        StubDevice() : StubGraphicsDevice(Options{.cpuBuffers = true, .nullIndexBuffers = true}) {}

        std::shared_ptr<VertexBuffer> createVertexBuffer(const std::shared_ptr<VertexFormat>& format,
            const int numVertices, const VertexBufferOptions& options) override
        {
            ++created;
            return std::make_shared<CountingVertexBuffer>(this, format, numVertices, options);
        }

        int created = 0;
    };

    /// `count` records whose every byte identifies the record and the upload.
    std::vector<uint8_t> makeRecords(const int count, const uint8_t tag)
    {
        std::vector<uint8_t> bytes(static_cast<size_t>(count) * kRecordSize);
        for (int i = 0; i < count; ++i) {
            std::fill_n(bytes.begin() + static_cast<long>(i) * kRecordSize, kRecordSize,
                static_cast<uint8_t>(tag + i));
        }
        return bytes;
    }

    /// The buffer holds exactly `capacity` records, the first `count` are `records`,
    /// and the rest are zero.
    bool holds(const VertexBuffer& buffer, const int capacity, const std::vector<uint8_t>& records,
        const int count)
    {
        const auto& storage = buffer.storage();
        if (buffer.numVertices() != capacity ||
            storage.size() != static_cast<size_t>(capacity) * kRecordSize) {
            return false;
        }
        const size_t live = static_cast<size_t>(count) * kRecordSize;
        if (!std::equal(records.begin(), records.begin() + static_cast<long>(live), storage.begin())) {
            return false;
        }
        return std::all_of(storage.begin() + static_cast<long>(live), storage.end(),
            [](const uint8_t b) { return b == 0; });
    }

    void checkShortPayloadIsRefused()
    {
        StubDevice device;
        auto format = std::make_shared<VertexFormat>(kRecordSize, true, false);
        CountingVertexBuffer buffer(&device, format, 4, VertexBufferOptions{});
        check(!buffer.setData(makeRecords(2, 1)),
            "control: setData refuses a payload smaller than the buffer");
        check(buffer.setData(makeRecords(4, 1)),
            "control: setData accepts a payload of the buffer's full size");
    }

    void checkGrowShrinkZero()
    {
        StubDevice device;
        std::shared_ptr<VertexBuffer> buffer;
        int capacity = 0;

        // First upload allocates.
        auto records = makeRecords(5, 10);
        check(wideline::uploadSegmentRecords(device, buffer, capacity, records.data(), 5, kRecordSize),
            "first upload succeeds");
        check(buffer != nullptr && device.created == 1 && capacity == 5, "first upload allocates 5");
        check(buffer && holds(*buffer, 5, records, 5), "first upload holds its records");
        const VertexBuffer* allocated = buffer.get();

        // SHRINK: the defect. Must upload in place, not be refused.
        records = makeRecords(2, 40);
        check(wideline::uploadSegmentRecords(device, buffer, capacity, records.data(), 2, kRecordSize),
            "shrinking upload succeeds");
        check(buffer.get() == allocated && device.created == 1 && capacity == 5,
            "shrinking keeps the buffer");
        check(holds(*buffer, 5, records, 2), "shrinking puts the live records at the front");

        // ZERO: nothing uploaded, buffer kept.
        const int uploadsBefore = static_cast<CountingVertexBuffer*>(buffer.get())->uploads;
        check(wideline::uploadSegmentRecords(device, buffer, capacity, nullptr, 0, kRecordSize),
            "empty upload succeeds");
        check(buffer.get() == allocated && capacity == 5 &&
            static_cast<CountingVertexBuffer*>(buffer.get())->uploads == uploadsBefore,
            "empty upload keeps the buffer and uploads nothing");

        // Back up to exactly the capacity: in place.
        records = makeRecords(5, 70);
        check(wideline::uploadSegmentRecords(device, buffer, capacity, records.data(), 5, kRecordSize),
            "upload at capacity succeeds");
        check(buffer.get() == allocated && device.created == 1, "upload at capacity keeps the buffer");
        check(holds(*buffer, 5, records, 5), "upload at capacity holds its records");

        // Past the capacity: reallocate, geometrically.
        records = makeRecords(6, 100);
        check(wideline::uploadSegmentRecords(device, buffer, capacity, records.data(), 6, kRecordSize),
            "growing upload succeeds");
        check(device.created == 2 && capacity == 10, "growing reallocates to double the capacity");
        check(buffer && holds(*buffer, 10, records, 6), "growing holds its records");

        // Well past double: exactly what is needed.
        records = makeRecords(37, 3);
        check(wideline::uploadSegmentRecords(device, buffer, capacity, records.data(), 37, kRecordSize),
            "large growing upload succeeds");
        check(device.created == 3 && capacity == 37, "a jump past double allocates what is needed");
        check(buffer && holds(*buffer, 37, records, 37), "large growing upload holds its records");
    }

    void checkCapacityRule()
    {
        check(wideline::segmentBufferCapacity(0, 0) == 0, "empty stays empty");
        check(wideline::segmentBufferCapacity(0, 3) == 3, "first allocation is what is needed");
        check(wideline::segmentBufferCapacity(8, 1) == 8, "never shrinks");
        check(wideline::segmentBufferCapacity(8, 8) == 8, "exact fit keeps the capacity");
        check(wideline::segmentBufferCapacity(8, 9) == 16, "grows by doubling");
        check(wideline::segmentBufferCapacity(8, 40) == 40, "a jump past double takes what is needed");

        // One more segment each frame reallocates O(log n) times, not n times.
        int capacity = 0;
        int reallocations = 0;
        for (int required = 1; required <= 1000; ++required) {
            const int next = wideline::segmentBufferCapacity(capacity, required);
            reallocations += next != capacity ? 1 : 0;
            check(next >= required && next >= capacity, "capacity covers the request and is monotone");
            capacity = next;
        }
        check(reallocations <= 11, "growing one segment at a time reallocates logarithmically");
    }
}

int main()
{
    quietPasses();
    checkShortPayloadIsRefused();
    checkGrowShrinkZero();
    checkCapacityRule();

    return finish("wide-line segment buffer");
}
