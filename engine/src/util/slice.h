// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 23.07.2025.
//

#pragma once

#include <vector>

namespace vis
{
    enum class SliceDirection {
        Forward,
        Backward
    };

    template <typename T>
    class Slice {
    public:
        Slice(const std::vector<T>& vec, const SliceDirection dir)
            : data(vec.data()), length(vec.size()), direction(dir) {}

        Slice(const T* data, const std::size_t length, const SliceDirection dir)
            : data(data), length(length), direction(dir) {}

        [[nodiscard]] std::size_t size() const {
            return length;
        }

        const T& operator[](std::size_t idx) const {
            if (idx >= length)
            {
                throw std::out_of_range("Slice index out of bounds");
            }

            if (direction == SliceDirection::Forward) {
                return data[idx];
            }
            { // Direction::Backward
                return data[length - 1 - idx];
            }
        }

    private:
        const T* data;
        std::size_t length;
        SliceDirection direction;
    };
}