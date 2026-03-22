// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
#pragma once

#include <functional>
#include <memory>
#include <vector>

namespace visutwin::canvas
{
    template<typename T>
    class ObjectPool
    {
    public:
        using Constructor = std::function<std::unique_ptr<T>()>;

        explicit ObjectPool(const size_t size = 1)
            : ObjectPool([] { return std::make_unique<T>(); }, size)
        {
        }

        ObjectPool(Constructor constructorFunc, const size_t size = 1)
            : _constructor(std::move(constructorFunc))
        {
            resize(size);
        }

        T* allocate()
        {
            if (_count >= _pool.size()) {
                resize(_pool.empty() ? 1 : _pool.size() * 2);
            }
            return _pool[_count++].get();
        }

        void freeAll()
        {
            _count = 0;
        }

        [[nodiscard]] size_t count() const
        {
            return _count;
        }

        [[nodiscard]] size_t size() const
        {
            return _pool.size();
        }

    private:
        void resize(const size_t size)
        {
            if (size <= _pool.size()) {
                return;
            }

            for (size_t i = _pool.size(); i < size; ++i) {
                _pool.push_back(_constructor ? _constructor() : std::make_unique<T>());
            }
        }

        Constructor _constructor;
        std::vector<std::unique_ptr<T>> _pool;
        size_t _count = 0;
    };
}
