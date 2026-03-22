// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 21.07.2025.
//

#pragma once

#include "defines.h"

namespace visutwin::canvas
{
    struct Vector2
    {
        float x, y;

        Vector2() : x(0), y(0)
        {
        }

        explicit Vector2(const float s) : x(s), y(s)
        {
        }

        Vector2(const float x, const float y) : x(x), y(y)
        {
        }

        [[nodiscard]] float dot(const Vector2& other) const
        {
#if defined(USE_SIMD_NEON)
            float32x4_t a = {x, y, 0.0f, 0.0f};
            float32x4_t b = {other.x, other.y, 0.0f, 0.0f};
            const float32x4_t m = vmulq_f32(a, b);
            float32x2_t sum = vadd_f32(vget_low_f32(m), vget_high_f32(m));
            return vget_lane_f32(sum, 0) + vget_lane_f32(sum, 1);
#elif defined(USE_SIMD_SSE)
            __m128 a = _mm_set_ps(0.0f, 0.0f, y, x);
            __m128 b = _mm_set_ps(0.0f, 0.0f, other.y, other.x);
            __m128 mul = _mm_mul_ps(a, b);
            // SSE2 reduction for x*x + y*y
            __m128 shuf = _mm_shuffle_ps(mul, mul, _MM_SHUFFLE(2, 3, 0, 1));
            __m128 sums = _mm_add_ps(mul, shuf);
            __m128 dot = _mm_add_ss(sums, _mm_shuffle_ps(sums, sums, _MM_SHUFFLE(1, 1, 1, 1)));
            return _mm_cvtss_f32(dot);
#else
            return x * other.x + y * other.y;
#endif
        }

        [[nodiscard]] float length() const
        {
            return std::sqrt(dot(*this));
        }

        Vector2 operator-(const Vector2& other) const
        {
#if defined(USE_SIMD_NEON)
            float32x4_t a = {x, y, 0.0f, 0.0f};
            float32x4_t b = {other.x, other.y, 0.0f, 0.0f};
            float32x4_t result = vsubq_f32(a, b);
            return {vgetq_lane_f32(result, 0), vgetq_lane_f32(result, 1)};
#elif defined(USE_SIMD_SSE)
            __m128 a = _mm_set_ps(0.0f, 0.0f, y, x);
            __m128 b = _mm_set_ps(0.0f, 0.0f, other.y, other.x);
            __m128 result = _mm_sub_ps(a, b);
            alignas(16) float res[4];
            _mm_store_ps(res, result);
            return { res[0], res[1] };
#else
            return { x - other.x, y - other.y };
#endif
        }

        Vector2 operator*(float scalar) const
        {
#if defined(USE_SIMD_NEON)
            float32x4_t a = {x, y, 0.0f, 0.0f};
            float32x4_t s = vdupq_n_f32(scalar);
            float32x4_t result = vmulq_f32(a, s);
            return {vgetq_lane_f32(result, 0), vgetq_lane_f32(result, 1)};
#elif defined(USE_SIMD_SSE)
            __m128 a = _mm_set_ps(0.0f, 0.0f, y, x);
            __m128 s = _mm_set1_ps(scalar);
            __m128 result = _mm_mul_ps(a, s);
            alignas(16) float res[4];
            _mm_store_ps(res, result);
            return { res[0], res[1] };
#else
            return { x * scalar, y * scalar };
#endif
        }
    };

    template<typename T>
    struct Vector2T
    {
        T x, y;

        Vector2T() : x(0), y(0)
        {
        }

        explicit Vector2T(const T s) : x(s), y(s)
        {
        }

        Vector2T(const T x, const T y) : x(x), y(y)
        {
        }

        Vector2T<T> operator-(const Vector2T& other) const
        {
            return Vector2T<T>(x - other.x, y - other.y);
        }

        [[nodiscard]] float length() const
        {
            return std::sqrt(x * x + y * y);
        }
    };

    typedef Vector2T<uint32_t> Vector2u;
    typedef Vector2T<int32_t> Vector2i;
}
