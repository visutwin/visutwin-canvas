// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 18.07.2025.
//

#pragma once

#include "defines.h"

namespace visutwin::canvas
{
    struct Vector3;

    /**
     * @brief 4D vector for homogeneous coordinates, color values, and SIMD operations.
     * @ingroup group_core_math
     */
    struct alignas(16) Vector4
    {
        union
        {
#if defined(USE_SIMD_SSE)
            __m128 m128;
#elif defined(USE_SIMD_APPLE)
            simd_float4 m128;
#elif defined(USE_SIMD_NEON)
            float32x4_t m128;
#else
            struct
            {
                float x, y, z, w;
            };

            float v[4];
#endif
        };

        //static Vector4 UNIT_X;
        //static Vector4 UNIT_Y;
        //static Vector4 UNIT_Z;

        Vector4()
        {
#if defined(USE_SIMD_SSE)
            m128 = _mm_setzero_ps();
#elif defined(USE_SIMD_APPLE)
            m128 = simd_make_float4(0.0f, 0.0f, 0.0f, 0.0f);
#elif defined(USE_SIMD_NEON)
            m128 = vdupq_n_f32(0.0f);
#else
            x = y = z = w = 0.0f;
#endif
        }

        Vector4(const float x, const float y, const float z, const float w)
        {
#if defined(USE_SIMD_SSE)
            m128 = _mm_set_ps(w, z, y, x); // Note: reversed order
#elif defined(USE_SIMD_APPLE)
            m128 = simd_make_float4(x, y, z, w);
#elif defined(USE_SIMD_NEON)
            float data[4] = {x, y, z, w};
            m128 = vld1q_f32(data);
#else
            this->x = x;
            this->y = y;
            this->z = z;
            this->w = w;
#endif
        }

#if defined(USE_SIMD_APPLE)
        explicit Vector4(const simd_float4& data) : m128(data)
        {
        }
#elif defined(USE_SIMD_SSE)
        explicit Vector4(const __m128& data) : m128(data) {}
#elif defined(USE_SIMD_NEON)
        explicit Vector4(const float32x4_t& data) : m128(data) {}
#endif

        explicit Vector4(const Vector3& vec3, float w = 0.0f);

        [[nodiscard]] float getX() const
        {
#if defined(USE_SIMD_SSE)
            return _mm_cvtss_f32(m128); // extracts the lowest float from __m128
#elif defined(USE_SIMD_APPLE)
            return m128.x;
#elif defined(USE_SIMD_NEON)
            return vgetq_lane_f32(m128, 0); // extract lane 0
#else
            return x; // fallback scalar path
#endif
        }

        [[nodiscard]] float getY() const
        {
#if defined(USE_SIMD_SSE)
            return _mm_cvtss_f32(_mm_shuffle_ps(m128, m128, _MM_SHUFFLE(1, 1, 1, 1)));
#elif defined(USE_SIMD_APPLE)
            return m128.y;
#elif defined(USE_SIMD_NEON)
            return vgetq_lane_f32(m128, 1);
#else
            return y;
#endif
        }

        [[nodiscard]] float getZ() const
        {
#if defined(USE_SIMD_SSE)
            return _mm_cvtss_f32(_mm_shuffle_ps(m128, m128, _MM_SHUFFLE(2, 2, 2, 2)));
#elif defined(USE_SIMD_APPLE)
            return m128.z;
#elif defined(USE_SIMD_NEON)
            return vgetq_lane_f32(m128, 2);
#else
            return z;
#endif
        }

        [[nodiscard]] float getW() const
        {
#if defined(USE_SIMD_SSE)
            return _mm_cvtss_f32(_mm_shuffle_ps(m128, m128, _MM_SHUFFLE(3, 3, 3, 3)));
#elif defined(USE_SIMD_APPLE)
            return m128.w;
#elif defined(USE_SIMD_NEON)
            return vgetq_lane_f32(m128, 3);
#else
            return w;
#endif
        }

        [[nodiscard]] float dot(const Vector4& other) const
        {
#if defined(USE_SIMD_SSE)
            // Multiply the vectors
            __m128 mul = _mm_mul_ps(m128, other.m128);
            // SSE2-compatible horizontal sum
            __m128 shuf = _mm_shuffle_ps(mul, mul, _MM_SHUFFLE(2, 3, 0, 1));
            __m128 sums = _mm_add_ps(mul, shuf);
            shuf = _mm_shuffle_ps(sums, sums, _MM_SHUFFLE(1, 0, 3, 2));
            sums = _mm_add_ps(sums, shuf);
            return _mm_cvtss_f32(sums); // extract the lowest float
#elif defined(USE_SIMD_APPLE)
            return simd_dot(m128, other.m128);
#elif defined(USE_SIMD_NEON)
            float32x4_t mul = vmulq_f32(m128, other.m128);
            const float32x2_t sum2 = vadd_f32(vget_low_f32(mul), vget_high_f32(mul));
            const float sum = vget_lane_f32(sum2, 0) + vget_lane_f32(sum2, 1);
            return sum;
#else
            return x * other.x + y * other.y + z * other.z + w * other.w;
#endif
        }

        [[nodiscard]] float length() const
        {
#if defined(USE_SIMD_APPLE)
            return simd_length(m128);
#else
            return std::sqrt(dot(*this));
#endif
        }

        Vector4 operator+(const Vector4& other) const
        {
#if defined(USE_SIMD_SSE)
            Vector4 result;
            result.m128 = _mm_add_ps(this->m128, other.m128);
            return result;
#elif defined(USE_SIMD_APPLE)
            return Vector4(this->m128 + other.m128); // simd_float4 supports operator+
#elif defined(USE_SIMD_NEON)
            return Vector4(vaddq_f32(this->m128, other.m128));
#else
            return Vector4(x + other.x, y + other.y, z + other.z, w + other.w);
#endif
        }

        Vector4 operator-(const Vector4& other) const
        {
#if defined(USE_SIMD_SSE)
            Vector4 result;
            result.m128 = _mm_sub_ps(this->m128, other.m128);
            return result;
#elif defined(USE_SIMD_APPLE)
            return Vector4(this->m128 - other.m128); // simd_float4 supports operator+
#elif defined(USE_SIMD_NEON)
            return Vector4(vsubq_f32(this->m128, other.m128));
#else
            return Vector4(x - other.x, y - other.y, z - other.z, w - other.w);
#endif
        }

        Vector4 operator*(float scalar) const
        {
#if defined(USE_SIMD_SSE)
            Vector4 result;
            __m128 scalarVec = _mm_set1_ps(scalar); // Broadcast scalar to all 4 lanes
            result.m128 = _mm_mul_ps(this->m128, scalarVec); // Multiply vectors
            return result;
#elif defined(USE_SIMD_APPLE)
            return Vector4(m128 * scalar); // simd_float4 supports scalar multiplication
#elif defined(USE_SIMD_NEON)
            Vector4 result;
            float32x4_t scalarVec = vdupq_n_f32(scalar); // Broadcast scalar to all 4 lanes
            result.m128 = vmulq_f32(this->m128, scalarVec); // Multiply vectors
            return result;
#else
            return Vector4(x * scalar, y * scalar, z * scalar, w * scalar);
#endif
        }

        // Normalize the plane (A, B, C, D) such that the normal vector (A, B, C) has unit length, and D is scaled accordingly
        [[nodiscard]] Vector4 planeNormalize() const
        {
#if defined(USE_SIMD_SSE)
            // Dot product of (x, y, z) only
            __m128 xyz = _mm_set_ps(0.0f, getZ(), getY(), getX()); // w = 0
            __m128 dot = _mm_mul_ps(xyz, xyz);

            // SSE2 reduction over xyz
            __m128 shuf = _mm_shuffle_ps(dot, dot, _MM_SHUFFLE(2, 3, 0, 1));
            __m128 sums = _mm_add_ps(dot, shuf);
            __m128 lengthSq = _mm_add_ss(sums, _mm_shuffle_ps(sums, sums, _MM_SHUFFLE(1, 1, 1, 1)));

            float len = _mm_cvtss_f32(lengthSq);
            if (len > 0)
            {
                float invLen = 1.0f / std::sqrt(len);
                __m128 scale = _mm_set1_ps(invLen);
                __m128 scaled = _mm_mul_ps(m128, scale);
                alignas(16) float out[4];
                _mm_store_ps(out, scaled);
                return {out[0], out[1], out[2], out[3]};
            }
            return {};
#elif defined(USE_SIMD_APPLE)
            simd_float4 xyz = m128;
            xyz.w = 0.0f;
            if (float len = simd_length(xyz); len > 0.0f)
            {
                float invLen = 1.0f / len;
                return Vector4(m128 * invLen);
            }
            return {};
#elif defined(USE_SIMD_NEON)
            float32x4_t xyz = m128;
            xyz = vsetq_lane_f32(0.0f, xyz, 3); // zero out w

            float32x4_t dot = vmulq_f32(xyz, xyz);
            const float32x2_t sum2 = vadd_f32(vget_low_f32(dot), vget_high_f32(dot));

            if (float len = vget_lane_f32(sum2, 0) + vget_lane_f32(sum2, 1); len > 0)
            {
                float invLen = 1.0f / std::sqrt(len);
                const float32x4_t result = vmulq_n_f32(m128, invLen);
                Vector4 r;
                r.m128 = result;
                return r;
            }
            return {};
#else
            // Plane normalization: normalize by the 3D normal length (x,y,z only), NOT the 4D vector length.
            // The w component (distance) is scaled by the same factor but must not contribute to the length.
            const float lenSq = x * x + y * y + z * z;
            if (lenSq > 0) {
                const float invLength = 1.0f / std::sqrt(lenSq);
                return {
                    x * invLength,
                    y * invLength,
                    z * invLength,
                    w * invLength
                };
            }
            // Return a zero plane if the normal vector is degenerate
            return {};
#endif
        }

        // result=x⋅A+y⋅B+z⋅C+1⋅D
        [[nodiscard]] float planeDotCoord(const Vector4& point) const
        {
#if defined(USE_SIMD_SSE)
            __m128 vec = _mm_insert_ps(point.m128, _mm_set_ss(1.0f), 0x30); // insert 1.0f into lane 3 (w) → [x, y, z, 1]
            __m128 result = _mm_dp_ps(this->m128, vec, 0xF1); // 0xF1: dot all 4 floats, store in lane 0
            return _mm_cvtss_f32(result);
#elif defined(USE_SIMD_APPLE)
            simd_float4 vec = point.m128;
            vec.w = 1.0f;
            return simd_dot(m128, vec);
#elif defined(USE_SIMD_NEON)
            float32x4_t vec = point.m128;
            vec = vsetq_lane_f32(1.0f, vec, 3); // set w = 1
            const float32x4_t mul = vmulq_f32(this->m128, vec);
            const float32x2_t sum1 = vadd_f32(vget_low_f32(mul), vget_high_f32(mul));
            const float32x2_t sum2 = vpadd_f32(sum1, sum1);
            return vget_lane_f32(sum2, 0);
#else
            return x * point.x + y * point.y + z * point.z + w; // use 1.0 for w
#endif
        }
    };
}

#include "vector4.inl"
