// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
#pragma once

#include <algorithm>
#include <functional>
#include <vector>

namespace visutwin::canvas
{
    template<typename T>
    struct SortedLoopArrayOptions
    {
        std::function<float(const T&)> keyExtractor;
    };

    template<typename T>
    class SortedLoopArray
    {
    public:
        explicit SortedLoopArray(const SortedLoopArrayOptions<T>& options)
            : _keyExtractor(options.keyExtractor) {}

        explicit SortedLoopArray(std::function<float(const T&)> keyExtractor)
            : _keyExtractor(std::move(keyExtractor))
        {
        }

        std::vector<T> items;
        size_t length = 0;
        int loopIndex = -1;

        void insert(const T& item)
        {
            const int index = binarySearch(item);
            items.insert(items.begin() + index, item);
            length = items.size();
            if (loopIndex >= index) {
                loopIndex++;
            }
        }

        void append(const T& item)
        {
            items.push_back(item);
            length = items.size();
        }

        void remove(const T& item)
        {
            const auto it = std::find(items.begin(), items.end(), item);
            if (it == items.end()) {
                return;
            }

            const int index = static_cast<int>(std::distance(items.begin(), it));
            items.erase(it);
            length = items.size();
            if (loopIndex >= index) {
                loopIndex--;
            }
        }

        void sort()
        {
            T current{};
            bool hasCurrent = loopIndex >= 0 && loopIndex < static_cast<int>(items.size());
            if (hasCurrent) {
                current = items[loopIndex];
            }

            std::sort(items.begin(), items.end(), [&](const T& a, const T& b) {
                return _keyExtractor(a) < _keyExtractor(b);
            });

            if (hasCurrent) {
                auto it = std::find(items.begin(), items.end(), current);
                loopIndex = (it == items.end()) ? -1 : static_cast<int>(std::distance(items.begin(), it));
            }

            length = items.size();
        }

    private:
        int binarySearch(const T& item) const
        {
            int left = 0;
            int right = static_cast<int>(items.size()) - 1;
            const float key = _keyExtractor(item);

            while (left <= right) {
                const int middle = (left + right) / 2;
                const float current = _keyExtractor(items[middle]);
                if (current <= key) {
                    left = middle + 1;
                } else {
                    right = middle - 1;
                }
            }

            return left;
        }

        std::function<float(const T&)> _keyExtractor;
    };
}
