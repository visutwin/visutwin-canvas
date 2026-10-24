// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 24.10.2026
//
// Records reused across frames, taken in order each frame and created as needed. Each record
// is held by pointer, so one taken stays where it is while the pool grows.
//
#pragma once

#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace visutwin::canvas
{
    template <typename T>
    class Pool
    {
    public:
        explicit Pool(std::function<std::unique_ptr<T>()> create) : _create(std::move(create)) {}

        /// The next record of this frame.
        T& next()
        {
            if (count == _items.size()) {
                _items.push_back(_create());
            }
            return *_items[count++];
        }

        /// The records created so far.
        const std::vector<std::unique_ptr<T>>& items() const { return _items; }
        std::vector<std::unique_ptr<T>>& items() { return _items; }

        /// The number of records taken this frame.
        size_t count = 0;

    private:
        std::function<std::unique_ptr<T>()> _create;
        std::vector<std::unique_ptr<T>> _items;
    };
}
