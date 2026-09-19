// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 24.07.2025.
//
#pragma once

#include <cstdint>

#include "defines.h"

namespace visutwin::canvas
{
    struct  Vector3;
    struct  Matrix4;

    /**
     * @brief Unit quaternion for rotation representation with SIMD-accelerated slerp and multiply.
     * @ingroup group_core_math
     */
    struct alignas(16) Quaternion
    {
        union
        {
#ifdef USE_SIMD_SSE
            __m128 simd;
#elif defined(USE_SIMD_APPLE)
            simd_quatf simd;
#elif defined(USE_SIMD_NEON)
            float32x4_t simd;
#else
            struct
            {
                float x, y, z, w;
            };
            float q[4];
#endif
        };

        Quaternion();

        Quaternion(float x, float y, float z, float w);

#ifdef USE_SIMD_SSE
        explicit Quaternion(__m128 simd) : simd(simd) {}
#elif defined(USE_SIMD_APPLE)
        explicit Quaternion(const simd_quatf& simd) : simd(simd) {}
#elif defined(USE_SIMD_NEON)
        explicit Quaternion(float32x4_t simd) : simd(simd) {}
#endif

        [[nodiscard]] float getX() const
        {
#if defined(USE_SIMD_SSE)
            return _mm_cvtss_f32(simd);
#elif defined(USE_SIMD_APPLE)
            return simd.vector.x;
#elif defined(USE_SIMD_NEON)
            return vgetq_lane_f32(simd, 0);
#else
            return x;
#endif
        }

        [[nodiscard]] float getY() const
        {
#if defined(USE_SIMD_SSE)
            return _mm_cvtss_f32(_mm_shuffle_ps(simd, simd, _MM_SHUFFLE(1, 1, 1, 1)));
#elif defined(USE_SIMD_APPLE)
            return simd.vector.y;
#elif defined(USE_SIMD_NEON)
            return vgetq_lane_f32(simd, 1);
#else
            return y;
#endif
        }

        [[nodiscard]] float getZ() const
        {
#if defined(USE_SIMD_SSE)
            return _mm_cvtss_f32(_mm_shuffle_ps(simd, simd, _MM_SHUFFLE(2, 2, 2, 2)));
#elif defined(USE_SIMD_APPLE)
            return simd.vector.z;
#elif defined(USE_SIMD_NEON)
            return vgetq_lane_f32(simd, 2);
#else
            return z;
#endif
        }

        [[nodiscard]] float getW() const
        {
#if defined(USE_SIMD_SSE)
            return _mm_cvtss_f32(_mm_shuffle_ps(simd, simd, _MM_SHUFFLE(3, 3, 3, 3)));
#elif defined(USE_SIMD_APPLE)
            return simd.vector.w;
#elif defined(USE_SIMD_NEON)
            return vgetq_lane_f32(simd, 3);
#else
            return w;
#endif
        }

        [[nodiscard]] Quaternion conjugate() const;

        Quaternion operator*(const Quaternion& rhs) const;

        Vector3 operator*(const Vector3& v) const;

        Quaternion operator*(const float scalar) const;

        /** Component-wise sum, on the active SIMD backend (the blend helpers' weighted sums). */
        Quaternion operator+(const Quaternion& rhs) const;

        /**
         * Spherical linear interpolation from `a` to `b` by `t`, along the shorter arc
         * (`b` is negated when a.dot(b) < 0), falling back to a normalised lerp when the
         * two are within 1e-6 of parallel. The result is renormalised. One definition for
         * the whole engine: AnimEvaluator, AnimTrack and Skeleton each carried their own
         * scalar copy until 2026-09-19.
         */
        static Quaternion slerp(const Quaternion& a, const Quaternion& b, float t);

        /**
         * Normalised linear interpolation along the shorter arc — upstream's
         * AnimBlend.blendQuat, which its layer and clip blends use in place of slerp.
         * Cheaper than slerp, and exact at t = 0.5 for unit inputs.
         */
        static Quaternion nlerp(const Quaternion& a, const Quaternion& b, float t);

        [[nodiscard]] Quaternion normalized() const;

        [[nodiscard]] Matrix4 toRotationMatrix() const;

        /**
         * Returns the magnitude of the specified quaternion
         */
        float length() const;

        /*
         * Returns the magnitude squared of the specified quaternion
         */
        float lengthSquared() const;

        /**
         * Four-component dot product, on the active SIMD backend. Its sign is the
         * shortest-arc test every quaternion blend needs (negative means the two
         * represent the same rotation from opposite hemispheres, so one is negated
         * before interpolating); use this rather than spelling the sum out of getX()..getW(),
         * which leaves the backend and rounds differently from lengthSquared().
         */
        float dot(const Quaternion& other) const;

        /**
         * Generates the inverse of the specified quaternion
         */
        Quaternion invert() const;

        friend std::ostream& operator<<(std::ostream& os, const Quaternion& q)
        {
            return os << "Quaternion(" << q.getX() << ", " << q.getY() << ", " << q.getZ() << ", " << q.getW() << ")";
        }

        /*
         * Creates a quaternion from an angular rotation around an axis
         */
        static Quaternion fromAxisAngle(const Vector3& axis, float angle);

        /*
         * Creates a quaternion to represent a rotation specified by Euler angles in degrees
         */
        static Quaternion fromEulerAngles(float ax, float ay, float az);

        /**
         *  Converts the specified 4x4 matrix to a quaternion
         */
        static Quaternion fromMatrix4(const Matrix4& m);
    };
}

#include "quaternion.inl"
