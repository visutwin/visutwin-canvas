// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 18.07.2025.
//

#pragma once

#include <cassert>
#include <iostream>
#include <iomanip>

#include "defines.h"
#include "util/simd.h"

namespace visutwin::canvas
{
    struct Vector3;
    struct Vector4;
    struct Quaternion;

    struct alignas(16) Matrix4
    {
        // Column-major layout: m[col][row]
        union
        {
#if defined(USE_SIMD_SSE)
            __m128 c[4];
#elif defined(USE_SIMD_APPLE)
            simd_float4x4 cm;
#elif defined(USE_SIMD_NEON)
            float32x4_t c[4];
#else
            float m[4][4];
#endif
        };

        // Default constructor creates identity matrix (matching upstream Mat4 behavior)
        Matrix4()
        {
#if defined(USE_SIMD_SSE)
            c[0] = VECTOR4_MASK_X; // [1, 0, 0, 0]
            c[1] = VECTOR4_MASK_Y; // [0, 1, 0, 0]
            c[2] = VECTOR4_MASK_Z; // [0, 0, 1, 0]
            c[3] = VECTOR4_MASK_W; // [0, 0, 0, 1]
#elif defined(USE_SIMD_APPLE)
            cm = matrix_identity_float4x4;
#elif defined(USE_SIMD_NEON)
            c[0] = vdupq_n_f32(0.0f);
            c[1] = vdupq_n_f32(0.0f);
            c[2] = vdupq_n_f32(0.0f);
            c[3] = vdupq_n_f32(0.0f);
            c[0] = vsetq_lane_f32(1.0f, c[0], 0);
            c[1] = vsetq_lane_f32(1.0f, c[1], 1);
            c[2] = vsetq_lane_f32(1.0f, c[2], 2);
            c[3] = vsetq_lane_f32(1.0f, c[3], 3);
#else
            m[0][0] = 1.0f; m[0][1] = 0.0f; m[0][2] = 0.0f; m[0][3] = 0.0f;
            m[1][0] = 0.0f; m[1][1] = 1.0f; m[1][2] = 0.0f; m[1][3] = 0.0f;
            m[2][0] = 0.0f; m[2][1] = 0.0f; m[2][2] = 1.0f; m[2][3] = 0.0f;
            m[3][0] = 0.0f; m[3][1] = 0.0f; m[3][2] = 0.0f; m[3][3] = 1.0f;
#endif
        }

        explicit Matrix4(const Vector4& col0, const Vector4& col1, const Vector4& col2, const Vector4& col3);

#if defined(USE_SIMD_APPLE)
        explicit Matrix4(const matrix_float4x4& m) : cm(m) {}

        explicit Matrix4(const simd_float4 col0, const simd_float4 col1, const simd_float4 col2, const simd_float4 col3)
            : cm(simd_matrix(col0, col1, col2, col3)) {}
#endif

        Matrix4& operator=(const Matrix4& other)
        {
            if (this == &other)
            {
                return *this; // self-assignment check
            }

#if defined(USE_SIMD_SSE) || defined(USE_SIMD_NEON)
            c[0] = other.c[0];
            c[1] = other.c[1];
            c[2] = other.c[2];
            c[3] = other.c[3];
#elif defined(USE_SIMD_APPLE)
            cm = other.cm;
#else
            for (int col = 0; col < 4; ++col)
            {
                for (int row = 0; row < 4; ++row)
                {
                    m[col][row] = other.m[col][row];
                }
            }
#endif
            return *this;
        }

        static Matrix4 identity()
        {
            return Matrix4();
        }

        Matrix4 operator*(const Matrix4& rhs) const;

        Vector4 operator*(const Vector4& v) const;

        Vector3 operator*(const Vector3& v) const;

        void print() const
        {
            std::cout << std::fixed << std::setprecision(3);
#if defined(USE_SIMD_SSE)
            alignas(16) float temp[4];
            for (int row = 0; row < 4; ++row)
            {
                std::cout << "[ ";
                for (int col = 0; col < 4; ++col)
                {
                    _mm_store_ps(temp, c[col]);  // store c[col] into temp[]
                    std::cout << std::setw(8) << temp[row] << " ";
                }
                std::cout << "]\n";
            }
#elif defined(USE_SIMD_APPLE)
            for (int row = 0; row < 4; ++row)
            {
                std::cout << "[ ";
                for (const auto column : cm.columns)
                {
                    std::cout << std::setw(8) << column[row] << " ";
                }
                std::cout << "]\n";
            }
#elif defined(USE_SIMD_NEON)
            alignas(16) float temp[4];
            for (int row = 0; row < 4; ++row)
            {
                std::cout << "[ ";
                for (int col = 0; col < 4; ++col)
                {
                    vst1q_f32(temp, c[col]);  // store NEON vector to temp
                    std::cout << std::setw(8) << temp[row] << " ";
                }
                std::cout << "]\n";
            }
#else
            // Fallback to standard float array
            for (int row = 0; row < 4; ++row)
            {
                std::cout << "[ ";
                for (int col = 0; col < 4; ++col)
                {
                    std::cout << std::setw(8) << m[col][row] << " ";
                }
                std::cout << "]\n";
            }
#endif
        }

        [[nodiscard]] Matrix4 transpose() const
        {
            Matrix4 result;
#if defined(USE_SIMD_APPLE)
            const simd_float4x4 transposed = simd_transpose(this->cm);
            result.cm = transposed;
#elif defined(USE_SIMD_NEON)
            // Step 1: Pairwise interleave at 32-bit granularity
            const float32x4x2_t t01 = vtrnq_f32(this->c[0], this->c[1]);
            const float32x4x2_t t23 = vtrnq_f32(this->c[2], this->c[3]);

            // Step 2: Combine low/high halves to complete the transpose
            result.c[0] = vcombine_f32(vget_low_f32(t01.val[0]),  vget_low_f32(t23.val[0]));
            result.c[1] = vcombine_f32(vget_low_f32(t01.val[1]),  vget_low_f32(t23.val[1]));
            result.c[2] = vcombine_f32(vget_high_f32(t01.val[0]), vget_high_f32(t23.val[0]));
            result.c[3] = vcombine_f32(vget_high_f32(t01.val[1]), vget_high_f32(t23.val[1]));
#elif defined(USE_SIMD_SSE)
            __m128 col0 = this->c[0];
            __m128 col1 = this->c[1];
            __m128 col2 = this->c[2];
            __m128 col3 = this->c[3];

            // Perform in-place transpose of four __m128 registers
            _MM_TRANSPOSE4_PS(col0, col1, col2, col3);

            result.c[0] = col0;
            result.c[1] = col1;
            result.c[2] = col2;
            result.c[3] = col3;
#else
            // Scalar fallback: manually transpose
            for (int col = 0; col < 4; ++col)
            {
                for (int row = 0; row < 4; ++row)
                {
                    result.m[col][row] = this->m[row][col];
                }
            }
#endif
            return result;
        }

        [[nodiscard]] Vector4 getColumn(int col) const;

        void setColumn(int col, const Vector4& v);

        // Create an orthographic projection matrix using a left-handed coordinate system
        // Clip z in [0,1], reverse-Z (near -> 1, far -> 0)
        static Matrix4 orthographicLHReverseZ(const float viewWidth, const float viewHeight, const float nearZ,
                                              const float farZ)
        {
            assert(std::abs(viewWidth) > 0.00001f);
            assert(std::abs(viewHeight) > 0.00001f);
            assert(std::abs(farZ - nearZ) > 0.00001f);

            const float halfW = 2.0f / viewWidth;
            const float halfH = 2.0f / viewHeight;
            const float invDZ = 1.0f / (farZ - nearZ);

            Matrix4 result;
#if defined(USE_SIMD_SSE)
            result.c[0] = _mm_set_ps(0.0f, 0.0f, 0.0f, halfW);                         // [2/w, 0, 0, 0]
            result.c[1] = _mm_set_ps(0.0f, 0.0f, halfH, 0.0f);                         // [0, 2/h, 0, 0]
            result.c[2] = _mm_set_ps(0.0f, -invDZ, 0.0f, 0.0f);                        // [0, 0, -invDZ, 0]
            result.c[3] = _mm_set_ps(1.0f, farZ * invDZ, 0.0f, 0.0f);                  // [0, 0, farZ*invDZ, 1]
#elif defined(USE_SIMD_APPLE)
            result.cm.columns[0] = simd_make_float4(halfW, 0.0f, 0.0f, 0.0f);
            result.cm.columns[1] = simd_make_float4(0.0f, halfH, 0.0f, 0.0f);
            result.cm.columns[2] = simd_make_float4(0.0f, 0.0f, -invDZ, 0.0f);
            result.cm.columns[3] = simd_make_float4(0.0f, 0.0f, farZ * invDZ, 1.0f);
#elif defined(USE_SIMD_NEON)
            result.c[0] = vdupq_n_f32(0.0f);
            result.c[1] = vdupq_n_f32(0.0f);
            result.c[2] = vdupq_n_f32(0.0f);
            result.c[3] = vdupq_n_f32(0.0f);

            result.c[0] = vsetq_lane_f32(halfW, result.c[0], 0);
            result.c[1] = vsetq_lane_f32(halfH, result.c[1], 1);
            result.c[2] = vsetq_lane_f32(-invDZ, result.c[2], 2);
            result.c[3] = vsetq_lane_f32(farZ * invDZ, result.c[3], 2);
            result.c[3] = vsetq_lane_f32(1.0f, result.c[3], 3);
#else
            result.m[0][0] = halfW;  result.m[0][1] = 0.0f;           result.m[0][2] = 0.0f;  result.m[0][3] = 0.0f;
            result.m[1][0] = 0.0f;   result.m[1][1] = halfH;          result.m[1][2] = 0.0f;  result.m[1][3] = 0.0f;
            result.m[2][0] = 0.0f;   result.m[2][1] = 0.0f;           result.m[2][2] = -invDZ; result.m[2][3] = 0.0f;
            result.m[3][0] = 0.0f;   result.m[3][1] = 0.0f;           result.m[3][2] = farZ * invDZ; result.m[3][3] = 1.0f;
#endif
            return result;
        }

        // Clip z in [0,1], reverse-Z (near -> 1, far -> 0)
        static Matrix4 perspectiveFovLHReverseZ(const float fovY, const float aspect, const float zNear, const float zFar)
        {
            assert(zNear > 0.f && zFar > 0.f);
            assert(std::abs(fovY) > 0.00001f * 2.0f);
            assert(std::abs(aspect) > 0.00001f);
            assert(std::abs(zFar - zNear) > 0.00001f);

            const float yScale = 1.0f / std::tan(fovY * 0.5f); // same as cosFov / sinFov
            const float xScale = yScale / aspect;
            const float fRange = zNear / (zFar - zNear);
            const float fTranslate = zFar * fRange;

            Matrix4 result;
#if defined(USE_SIMD_SSE)
            result.c[0] = _mm_set_ps(0.0f, 0.0f, 0.0f, xScale);
            result.c[1] = _mm_set_ps(0.0f, 0.0f, yScale, 0.0f);
            result.c[2] = _mm_set_ps(1.0f, fRange, 0.0f, 0.0f);
            result.c[3] = _mm_set_ps(0.0f, fTranslate, 0.0f, 0.0f);
#elif defined(USE_SIMD_APPLE)
            result.cm.columns[0] = simd_make_float4(xScale, 0.0f,   0.0f,       0.0f);
            result.cm.columns[1] = simd_make_float4(0.0f,   yScale, 0.0f,       0.0f);
            result.cm.columns[2] = simd_make_float4(0.0f,   0.0f,   fRange,     1.0f);
            result.cm.columns[3] = simd_make_float4(0.0f,   0.0f,   fTranslate, 0.0f);
#elif defined(USE_SIMD_NEON)
            result.c[0] = vdupq_n_f32(0.0f);
            result.c[1] = vdupq_n_f32(0.0f);
            result.c[2] = vdupq_n_f32(0.0f);
            result.c[3] = vdupq_n_f32(0.0f);

            result.c[0] = vsetq_lane_f32(xScale, result.c[0], 0);
            result.c[1] = vsetq_lane_f32(yScale, result.c[1], 1);
            result.c[2] = vsetq_lane_f32(fRange, result.c[2], 2);
            result.c[2] = vsetq_lane_f32(1.0f, result.c[2], 3);
            result.c[3] = vsetq_lane_f32(fTranslate, result.c[3], 2);
#else
            result.m[0][0] = xScale; result.m[0][1] = 0.0f;   result.m[0][2] = 0.0f;       result.m[0][3] = 0.0f;
            result.m[1][0] = 0.0f;   result.m[1][1] = yScale; result.m[1][2] = 0.0f;       result.m[1][3] = 0.0f;
            result.m[2][0] = 0.0f;   result.m[2][1] = 0.0f;   result.m[2][2] = fRange;     result.m[2][3] = 1.0f;
            result.m[3][0] = 0.0f;   result.m[3][1] = 0.0f;   result.m[3][2] = fTranslate; result.m[3][3] = 0.0f;
#endif
            return result;
        }

        static Matrix4 translation(float x, float y, float z)
        {
            Matrix4 result; // Zero-initialized
#if defined(USE_SIMD_SSE)
            result.c[0] = _mm_set_ps(0.0f, 0.0f, 0.0f, 1.0f);   // [1, 0, 0, 0]
            result.c[1] = _mm_set_ps(0.0f, 0.0f, 1.0f, 0.0f);   // [0, 1, 0, 0]
            result.c[2] = _mm_set_ps(0.0f, 1.0f, 0.0f, 0.0f);   // [0, 0, 1, 0]
            result.c[3] = _mm_set_ps(1.0f, z,     y,     x);    // [x, y, z, 1]
#elif defined(USE_SIMD_APPLE)
            result.cm.columns[0] = simd_make_float4(1.0f, 0.0f, 0.0f, 0.0f); // X-axis basis
            result.cm.columns[1] = simd_make_float4(0.0f, 1.0f, 0.0f, 0.0f); // Y-axis basis
            result.cm.columns[2] = simd_make_float4(0.0f, 0.0f, 1.0f, 0.0f); // Z-axis basis
            result.cm.columns[3] = simd_make_float4(x,        y,      z,    1.0f);    // Translation column
#elif defined(USE_SIMD_NEON)
            result.c[0] = vdupq_n_f32(0.0f);
            result.c[1] = vdupq_n_f32(0.0f);
            result.c[2] = vdupq_n_f32(0.0f);
            result.c[3] = vdupq_n_f32(0.0f);

            result.c[0] = vsetq_lane_f32(1.0f, result.c[0], 0); // m[0][0] = 1
            result.c[1] = vsetq_lane_f32(1.0f, result.c[1], 1); // m[1][1] = 1
            result.c[2] = vsetq_lane_f32(1.0f, result.c[2], 2); // m[2][2] = 1

            result.c[3] = vsetq_lane_f32(x, result.c[3], 0); // m[3][0] = x
            result.c[3] = vsetq_lane_f32(y, result.c[3], 1); // m[3][1] = y
            result.c[3] = vsetq_lane_f32(z, result.c[3], 2); // m[3][2] = z
            result.c[3] = vsetq_lane_f32(1.0f, result.c[3], 3); // m[3][3] = 1
#else
            result.m[0][0] = 1.0f;
            result.m[1][1] = 1.0f;
            result.m[2][2] = 1.0f;
            result.m[3][3] = 1.0f;

            result.m[3][0] = x;
            result.m[3][1] = y;
            result.m[3][2] = z;
#endif
            return result;
        }

        // Returns left-handed view matrix, looking from the eye in dir direction with up
        static Matrix4 lookToLH(const Vector3& eye, const Vector3& dir, const Vector3& up);

        [[nodiscard]] Matrix4 inverse() const;

        [[nodiscard]] Vector3 getPosition() const;

        [[nodiscard]] Quaternion getRotation() const;

        [[nodiscard]] Vector3 getTranslation() const;

        [[nodiscard]] float getElement(const int col, int row) const {
            assert(0 <= col && col < 4 && 0 <= row && row < 4);
#if defined(USE_SIMD_APPLE)
            return cm.columns[col][row];
#elif defined(USE_SIMD_SSE)
            alignas(16) float tmp[4];
            _mm_store_ps(tmp, c[col]);
            return tmp[row];
#elif defined(USE_SIMD_NEON)
            switch (row) {
                case 0: return vgetq_lane_f32(c[col], 0);
                case 1: return vgetq_lane_f32(c[col], 1);
                case 2: return vgetq_lane_f32(c[col], 2);
                case 3: return vgetq_lane_f32(c[col], 3);
                default: return 0.0f;
            }
#else
            return m[col][row];
#endif
        }

        void setElement(const int col, int row, const float value) {
            assert(0 <= col && col < 4 && 0 <= row && row < 4);
#if defined(USE_SIMD_APPLE)
            cm.columns[col][row] = value;
#elif defined(USE_SIMD_SSE)
            alignas(16) float tmp[4];
            _mm_store_ps(tmp, c[col]);
            tmp[row] = value;
            c[col] = _mm_load_ps(tmp);
#elif defined(USE_SIMD_NEON)
            switch (row) {
                case 0: c[col] = vsetq_lane_f32(value, c[col], 0); break;
                case 1: c[col] = vsetq_lane_f32(value, c[col], 1); break;
                case 2: c[col] = vsetq_lane_f32(value, c[col], 2); break;
                case 3: c[col] = vsetq_lane_f32(value, c[col], 3); break;
                default: break;
            }
#else
            m[col][row] = value;
#endif
        }

        /**
         * Extracts the scale component from the specified 4x4 matrix
         */
        Vector3 getScale() const;

        /**
         * Transforms a 3-dimensional point by a 4x4 matrix
         */
        Vector3 transformPoint(const Vector3& v) const;

        /**
         * Multiplies the specified 4x4 matrices together and stores the result in the current instance.
         * This function assumes the matrices are affine transformation matrices, where the
         * upper left 3x3 elements are a rotation matrix, and the bottom left 3 elements are translation.
         * The rightmost column is assumed to be [0, 0, 0, 1].
         * The parameters are not verified to be in the expected format.
         */
        Matrix4 mulAffine(const Matrix4& rhs) const;

        static Matrix4 perspective(float fov, float aspect, float zNear, float zFar, bool fovIsHorizontal = false);

        static Matrix4 frustum(float left, float right, float bottom, float top, float zNear, float zFar);

        static Matrix4 ortho(float left, float right, float bottom, float top, float near, float far);

        /**
         * Sets the specified matrix to the concatenation of a translation, a quaternion rotation and a scale
         * @param t
         * @param r
         * @param s
         */
        static Matrix4 trs(const Vector3& t, const Quaternion& r, const Vector3& s);

        /**
         * Creates a reflection matrix for mirroring across a plane defined by a normal and distance.
         * Mat4.setReflection(normal, distance).
         * Uses the Householder reflection formula: R = I - 2nn^T, T = -2d*n.
         * @param normal - The unit normal of the reflection plane.
         * @param distance - The signed distance from the origin to the plane (d = -dot(normal, pointOnPlane)).
         */
        static Matrix4 reflection(float nx, float ny, float nz, float distance)
        {
            const float a = nx, b = ny, c = nz;
            Matrix4 r;
            r.setElement(0, 0, 1.0f - 2.0f * a * a);
            r.setElement(0, 1, -2.0f * a * b);
            r.setElement(0, 2, -2.0f * a * c);
            r.setElement(0, 3, 0.0f);

            r.setElement(1, 0, -2.0f * a * b);
            r.setElement(1, 1, 1.0f - 2.0f * b * b);
            r.setElement(1, 2, -2.0f * b * c);
            r.setElement(1, 3, 0.0f);

            r.setElement(2, 0, -2.0f * a * c);
            r.setElement(2, 1, -2.0f * b * c);
            r.setElement(2, 2, 1.0f - 2.0f * c * c);
            r.setElement(2, 3, 0.0f);

            r.setElement(3, 0, -2.0f * a * distance);
            r.setElement(3, 1, -2.0f * b * distance);
            r.setElement(3, 2, -2.0f * c * distance);
            r.setElement(3, 3, 1.0f);
            return r;
        }
    };
}

#include "matrix4.inl"
#include "matrix4Inverse.inl"
