// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
#pragma once

#include <algorithm>
#include <array>
#include <cmath>

#include "core/math/vector2.h"

namespace visutwin::canvas
{
    /// A joystick drawn where a finger lands: value() is the finger's offset from that
    /// point over `range` points, each axis in [-1, 1]. down/move/up return the base
    /// and stick positions for a UI to draw ([-1, -1, -1, -1] when released).
    class VirtualJoystick
    {
    public:
        explicit VirtualJoystick(const float range = 70.0f) : _range(range) {}

        [[nodiscard]] const Vector2& value() const { return _value; }

        std::array<float, 4> down(const float x, const float y)
        {
            _position = Vector2(x, y);
            _value = Vector2(0.0f, 0.0f);
            return {x, y, x, y};
        }

        std::array<float, 4> move(const float x, const float y)
        {
            float vx = x - _position.x;
            float vy = y - _position.y;
            const float len = std::sqrt(vx * vx + vy * vy);
            if (len > _range) {
                vx *= _range / len;
                vy *= _range / len;
            }
            _value = Vector2(std::clamp(vx / _range, -1.0f, 1.0f), std::clamp(vy / _range, -1.0f, 1.0f));
            return {_position.x, _position.y, _position.x + vx, _position.y + vy};
        }

        std::array<float, 4> up()
        {
            _position = Vector2(0.0f, 0.0f);
            _value = Vector2(0.0f, 0.0f);
            return {-1.0f, -1.0f, -1.0f, -1.0f};
        }

    private:
        float _range = 70.0f;
        Vector2 _position = Vector2(0.0f, 0.0f);
        Vector2 _value = Vector2(0.0f, 0.0f);
    };
}
