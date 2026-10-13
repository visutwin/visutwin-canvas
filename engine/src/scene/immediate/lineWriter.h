// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2026
//
#pragma once

#include <cassert>
#include <cstddef>

#include "core/math/color.h"
#include "core/math/vector3.h"

namespace visutwin::canvas
{
    /**
     * A cursor over vertices allocated in an immediate line batch (Immediate::allocateLines).
     * Vertices are interleaved, position then colour, kFloatsPerVertex floats each; two make a
     * segment. A shape allocates exactly the vertices it writes and fills them before the next
     * allocation: the storage behind the cursor moves when a batch grows.
     */
    class LineWriter
    {
    public:
        static constexpr size_t kFloatsPerVertex = 7;

        void reset(float* vertices, const size_t first, const size_t count, const Color& color)
        {
            _vertices = vertices;
            _cursor = first;
            _end = first + count;
            setColor(color);
        }

        /// The colour `segment` writes.
        void setColor(const Color& color)
        {
            _r = color.r;
            _g = color.g;
            _b = color.b;
            _a = color.a;
        }

        [[nodiscard]] bool filled() const { return _cursor == _end; }
        [[nodiscard]] float* vertices() const { return _vertices; }
        [[nodiscard]] size_t cursor() const { return _cursor; }
        void setCursor(const size_t value) { _cursor = value; }
        [[nodiscard]] size_t end() const { return _end; }

        /// One segment in the writer's colour.
        void segment(const float x0, const float y0, const float z0, const float x1, const float y1, const float z1)
        {
            assert(_cursor + 2 <= _end && "LineWriter::segment wrote past its allocation");
            float* v = _vertices + _cursor * kFloatsPerVertex;
            v[0] = x0; v[1] = y0; v[2] = z0;
            v[3] = _r; v[4] = _g; v[5] = _b; v[6] = _a;
            v[7] = x1; v[8] = y1; v[9] = z1;
            v[10] = _r; v[11] = _g; v[12] = _b; v[13] = _a;
            _cursor += 2;
        }

        void segment(const Vector3& a, const Vector3& b)
        {
            segment(a.getX(), a.getY(), a.getZ(), b.getX(), b.getY(), b.getZ());
        }

        /// One vertex with a colour of its own.
        void vertex(const float x, const float y, const float z, const float r, const float g, const float b,
            const float a)
        {
            assert(_cursor < _end && "LineWriter::vertex wrote past its allocation");
            float* v = _vertices + _cursor * kFloatsPerVertex;
            v[0] = x; v[1] = y; v[2] = z;
            v[3] = r; v[4] = g; v[5] = b; v[6] = a;
            ++_cursor;
        }

    private:
        float* _vertices = nullptr;
        size_t _cursor = 0;
        size_t _end = 0;
        float _r = 1.0f;
        float _g = 1.0f;
        float _b = 1.0f;
        float _a = 1.0f;
    };
}
