// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 01.10.2026.
//
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace visutwin::canvas
{
    /**
     * Hands out runs of consecutive slots from a fixed capacity and takes them back:
     * the bookkeeping of a buffer many small owners share. No memory of its own, so
     * the policy can be tested without the buffer.
     *
     * First fit over a list of free runs kept sorted by start; a returned run merges
     * with its neighbours, so a capacity that is entirely free is always one run and
     * can be handed out whole again.
     */
    class RangeAllocator
    {
    public:
        explicit RangeAllocator(const uint32_t capacity) : _capacity(capacity)
        {
            if (capacity > 0) {
                _free.push_back({0, capacity});
            }
        }

        /// The start of `count` consecutive free slots, or nothing when no run is long
        /// enough (or `count` is 0).
        std::optional<uint32_t> allocate(const uint32_t count)
        {
            if (count == 0) {
                return std::nullopt;
            }
            for (size_t i = 0; i < _free.size(); ++i) {
                Run& run = _free[i];
                if (run.count < count) {
                    continue;
                }
                const uint32_t start = run.start;
                run.start += count;
                run.count -= count;
                if (run.count == 0) {
                    _free.erase(_free.begin() + static_cast<std::ptrdiff_t>(i));
                }
                _freeTotal -= count;
                return start;
            }
            return std::nullopt;
        }

        /// Returns a run allocate() handed out. Returning one twice, or one that was
        /// never handed out, is the caller's bug; it is not detected.
        void release(const uint32_t start, const uint32_t count)
        {
            if (count == 0) {
                return;
            }
            const auto after = std::lower_bound(_free.begin(), _free.end(), start,
                [](const Run& run, const uint32_t value) { return run.start < value; });
            const size_t index = static_cast<size_t>(after - _free.begin());
            const bool joinsBefore = index > 0 && _free[index - 1].start + _free[index - 1].count == start;
            const bool joinsAfter = index < _free.size() && start + count == _free[index].start;
            if (joinsBefore && joinsAfter) {
                _free[index - 1].count += count + _free[index].count;
                _free.erase(_free.begin() + static_cast<std::ptrdiff_t>(index));
            } else if (joinsBefore) {
                _free[index - 1].count += count;
            } else if (joinsAfter) {
                _free[index].start = start;
                _free[index].count += count;
            } else {
                _free.insert(after, {start, count});
            }
            _freeTotal += count;
        }

        [[nodiscard]] uint32_t capacity() const { return _capacity; }
        [[nodiscard]] uint32_t freeTotal() const { return _freeTotal; }
        [[nodiscard]] bool allFree() const { return _freeTotal == _capacity; }
        /// How many separate free runs there are: 1 for an untouched or fully returned
        /// capacity, 0 for a full one.
        [[nodiscard]] size_t freeRunCount() const { return _free.size(); }

    private:
        struct Run
        {
            uint32_t start;
            uint32_t count;
        };

        uint32_t _capacity;
        uint32_t _freeTotal = _capacity;
        std::vector<Run> _free;
    };
}
