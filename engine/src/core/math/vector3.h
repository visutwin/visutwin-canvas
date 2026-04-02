// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 18.07.2025.
//

#pragma once

#include <iostream>

#include "defines.h"

namespace visutwin::canvas
{
    struct Matrix4;
    struct Vector4;

    typedef std::array<float, 3> PackedVector3f;

    /**
     * @brief 3D vector for positions, directions, and normals with multi-backend SIMD acceleration.
     * @ingroup group_core_math
     *
     * Vector3 uses a 16-byte aligned union supporting scalar, SSE, Apple SIMD, and NEON
     * backends. The active backend is controlled by USE_SIMD_MATH / USE_SIMD_PREFER_NEON
     * defines (currently scalar fallback is active).
     */
    struct alignas(16) Vector3
    {
        union
        {
#if defined(USE_SIMD_SSE)
            __m128 m128;
#elif defined(USE_SIMD_APPLE)
            simd_float3 m128;
#elif defined(USE_SIMD_NEON)
            float32x4_t m128;
#else
            struct
            {
                float x, y, z;
            };
            float v[3];
#endif
        };

        static Vector3 UNIT_X;
        static Vector3 UNIT_Y;
        static Vector3 UNIT_Z;

        Vector3()
        {
#if defined(USE_SIMD_SSE)
            m128 = _mm_setzero_ps();
#elif defined(USE_SIMD_APPLE)
            m128 = simd_make_float3(0.0f, 0.0f, 0.0f);
#elif defined(USE_SIMD_NEON)
            m128 = vdupq_n_f32(0.0f);
#else
            x = y = z = 0.0f;
#endif
        }

        explicit Vector3(const float s)
        {
#if defined(USE_SIMD_SSE)
            m128 = _mm_set_ps(0.0f, s, s, s); // w must be 0 for 3-component dot/length
#elif defined(USE_SIMD_APPLE)
            m128 = simd_make_float3(s, s, s);
#elif defined(USE_SIMD_NEON)
            m128 = vsetq_lane_f32(0.0f, vdupq_n_f32(s), 3); // w must be 0 for 3-component dot/length
#else
            x = y = z = s;
#endif
        }

        Vector3(const float x, const float y, const float z)
        {
#if defined(USE_SIMD_SSE)
            m128 = _mm_set_ps(0.0f, z, y, x); // last = x, then y, then z, then 0
#elif defined(USE_SIMD_APPLE)
            m128 = simd_make_float3(x, y, z);
#elif defined(USE_SIMD_NEON)
            float temp[4] = { x, y, z, 0.0f };
            m128 = vld1q_f32(temp);
#else
            v[0] = x;
            v[1] = y;
            v[2] = z;
#endif
        }

        Vector3(const Vector4& other);

#if defined(USE_SIMD_SSE)
        explicit Vector3(const __m128& data) : m128(data) {}
#elif defined(USE_SIMD_NEON)
        explicit Vector3(const float32x4_t& data) : m128(data) {}
#endif

#if defined(USE_SIMD_APPLE)
        explicit Vector3(const simd_float4& data) : m128(simd_make_float3(data)) {}

        explicit Vector3(const simd_float3& data) : m128(data) {}
#endif

        explicit Vector3(const PackedVector3f& packed) {
#if defined(USE_SIMD_SSE)
            m128 = _mm_set_ps(0.0f, packed[2], packed[1], packed[0]); // w, z, y, x (reverse order)
#elif defined(USE_SIMD_APPLE)
            m128 = simd_make_float3(packed[0], packed[1], packed[2]);
#elif defined(USE_SIMD_NEON)
            float temp[4] = { packed[0], packed[1], packed[2], 0.0f };
            m128 = vld1q_f32(temp);
#else
            x = packed[0];
            y = packed[1];
            z = packed[2];
#endif
        }

        [[nodiscard]] float getX() const {
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

        [[nodiscard]] float getY() const {
#if defined(USE_SIMD_SSE)
            return _mm_cvtss_f32(_mm_shuffle_ps(m128, m128, _MM_SHUFFLE(1,1,1,1)));
#elif defined(USE_SIMD_APPLE)
            return m128.y;
#elif defined(USE_SIMD_NEON)
            return vgetq_lane_f32(m128, 1);
#else
            return y;
#endif
        }

        [[nodiscard]] float getZ() const {
#if defined(USE_SIMD_SSE)
            return _mm_cvtss_f32(_mm_shuffle_ps(m128, m128, _MM_SHUFFLE(2,2,2,2)));
#elif defined(USE_SIMD_APPLE)
            return m128.z;
#elif defined(USE_SIMD_NEON)
            return vgetq_lane_f32(m128, 2);
#else
            return z;
#endif
        }

        Vector3 operator+(const Vector3& other) const
        {
#if defined(USE_SIMD_SSE)
            return Vector3(_mm_add_ps(this->m128, other.m128));
#elif defined(USE_SIMD_APPLE)
            return Vector3(this->m128 + other.m128);
#elif defined(USE_SIMD_NEON)
            return Vector3(vaddq_f32(this->m128, other.m128));
#else
            return Vector3(this->x + other.x, this->y + other.y, this->z + other.z);
#endif
        }

        Vector3& operator+=(const Vector3& rhs)
        {
#if defined(USE_SIMD_SSE)
            m128 = _mm_add_ps(m128, rhs.m128);
#elif defined(USE_SIMD_APPLE)
            m128 = m128 + rhs.m128;
#elif defined(USE_SIMD_NEON)
            m128 = vaddq_f32(m128, rhs.m128);
#else
            x += rhs.x;
            y += rhs.y;
            z += rhs.z;
#endif
            return *this;
        }

        Vector3 operator-(const Vector3& other) const
        {
#if defined(USE_SIMD_SSE)
            return Vector3(_mm_sub_ps(this->m128, other.m128));
#elif defined(USE_SIMD_APPLE)
            return Vector3(this->m128 - other.m128);
#elif defined(USE_SIMD_NEON)
            return Vector3(vsubq_f32(this->m128, other.m128));
#else
            return Vector3(this->x - other.x, this->y - other.y, this->z - other.z);
#endif
        }

        Vector3& operator-=(const Vector3& rhs)
        {
#if defined(USE_SIMD_SSE)
            m128 = _mm_sub_ps(m128, rhs.m128);
#elif defined(USE_SIMD_APPLE)
            m128 = m128 - rhs.m128;
#elif defined(USE_SIMD_NEON)
            m128 = vsubq_f32(m128, rhs.m128);
#else
            x -= rhs.x;
            y -= rhs.y;
            z -= rhs.z;
#endif
            return *this;
        }

        Vector3 operator*(float scalar) const;

        Vector3 operator*(const Vector3& other) const;

        [[nodiscard]] float dot(const Vector3& other) const;

        [[nodiscard]] Vector3 normalized() const;

        [[nodiscard]] float length() const
        {
#if defined(USE_SIMD_APPLE)
            return simd_length(m128);
#else
            return std::sqrt(dot(*this));
#endif
        }

        [[nodiscard]] float lengthSquared() const
        {
#if defined(USE_SIMD_APPLE)
            return simd_length_squared(m128);
#else
            return dot(*this);
#endif
        }

        [[nodiscard]] float distance(const Vector3& other) const
        {
#if defined(USE_SIMD_APPLE)
            return simd_distance(m128, other.m128);
#else
            return (*this - other).length();
#endif
        }

        [[nodiscard]] Vector3 cross(const Vector3& other) const;

        void print() const
        {
#if defined(USE_SIMD_APPLE)
            std::cout << "Vector3(" << m128.x << ", " << m128.y << ", " << m128.z << ")\n";
#elif defined(USE_SIMD_SSE) || defined(USE_SIMD_NEON)
            std::cout << "Vector3(" << getX() << ", " << getY() << ", " << getZ() << ")\n";
#else
            std::cout << "Vector3(" << x << ", " << y << ", " << z << ")\n";
#endif
        }

        /*
        * Constructs a 4×4 scaling matrix from a 3D scaling vector
        */
        [[nodiscard]] Matrix4 toScalingMatrix() const;

        /*
         * Creates a 4×4 transformation matrix that represents a translation using the given 3D offset vector
         */
        [[nodiscard]] Matrix4 toTranslationMatrix() const;

        /*
         * Transforms the given 3D vector by a 4×4 transformation matrix. It applies translation, rotation, and scaling.
         */
        [[nodiscard]] Vector3 transform(const Matrix4& mat) const;

        /*
         * Transforms the given 3D direction vector by a 4×4 transformation matrix, ignoring translation.
         */
        [[nodiscard]] Vector3 transformNormal(const Matrix4& mat) const;

        Vector3 operator-() const;
    };

    template<typename T>
    struct Vector3T
    {
        T x, y, z;

        Vector3T() : x(0), y(0), z(0)
        {
        }

        explicit Vector3T(const T s) : x(s), y(s), z(s)
        {
        }

        Vector3T(const T x, const T y, const T z) : x(x), y(y), z(z)
        {
        }

        Vector3T<T> operator-(const Vector3T& other) const
        {
            return Vector3T<T>(x - other.x, y - other.y, z - other.z);
        }

        [[nodiscard]] float length() const
        {
            return std::sqrt(x * x + y * y + z * z);
        }
    };

    typedef Vector3T<int> Vector3i;
}

#include "vector3.inl"
