// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
#pragma once

#include <cmath>
#include <cstdint>
#include <numbers>
#include <random>

#include <core/math/vector2.h>
#include <core/math/vector3.h>

namespace visutwin::canvas
{
    class Random
    {
    public:
        static void circlePoint(Vector2& point)
        {
            static thread_local std::mt19937 generator(std::random_device{}());
            static thread_local std::uniform_real_distribution<float> unit(0.0f, 1.0f);

            const float r = std::sqrt(unit(generator));
            const float theta = unit(generator) * 2.0f * std::numbers::pi_v<float>;
            point = Vector2(r * std::cos(theta), r * std::sin(theta));
        }

        static void circlePointDeterministic(Vector2& point, const int index, const int numPoints)
        {
            constexpr float goldenAngle = 2.39996322972865332f;
            const float theta = static_cast<float>(index) * goldenAngle;
            const float r = std::sqrt(static_cast<float>(index) / static_cast<float>(numPoints));
            point = Vector2(r * std::cos(theta), r * std::sin(theta));
        }

        static void spherePointDeterministic(Vector3& point, const int index, const int numPoints,
                                             float start = 0.0f, float end = 1.0f)
        {
            constexpr float goldenAngle = 2.39996322972865332f;
            start = 1.0f - 2.0f * start;
            end = 1.0f - 2.0f * end;

            const float y = start + (end - start) * (static_cast<float>(index) / static_cast<float>(numPoints));
            const float radius = std::sqrt(std::max(0.0f, 1.0f - y * y));
            const float theta = goldenAngle * static_cast<float>(index);

            point = Vector3(std::cos(theta) * radius, y, std::sin(theta) * radius);
        }

        static float radicalInverse(uint32_t i)
        {
            uint32_t bits = (i << 16u) | (i >> 16u);
            bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
            bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
            bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
            bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
            return static_cast<float>(static_cast<double>(bits) * 2.3283064365386963e-10);
        }
    };
}
