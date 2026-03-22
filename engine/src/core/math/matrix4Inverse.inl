//
// Created by Arnis Lektauers on 25.07.2025.
//
#pragma once

#include <cstring>

namespace visutwin::canvas
{
    inline Matrix4 Matrix4::inverse() const
    {
#if defined(USE_SIMD_APPLE)
        // Use the Accelerate framework for optimal general inverse
        return Matrix4(simd_inverse(cm));
#else
        // General 4x4 inverse using cofactors + determinant (GLM / MESA implementation).
        // Works correctly for all matrices (affine, projective, arbitrary).
        alignas(16) float a[16];
        alignas(16) float invOut[16];

        // Store column-major data to flat array
#if defined(USE_SIMD_SSE)
        _mm_store_ps(a,      c[0]);
        _mm_store_ps(a + 4,  c[1]);
        _mm_store_ps(a + 8,  c[2]);
        _mm_store_ps(a + 12, c[3]);
#elif defined(USE_SIMD_NEON)
        vst1q_f32(a,      c[0]);
        vst1q_f32(a + 4,  c[1]);
        vst1q_f32(a + 8,  c[2]);
        vst1q_f32(a + 12, c[3]);
#else
        std::memcpy(a, &m[0][0], sizeof(a));
#endif

        invOut[0] = a[5]  * a[10] * a[15] -
                    a[5]  * a[11] * a[14] -
                    a[9]  * a[6]  * a[15] +
                    a[9]  * a[7]  * a[14] +
                    a[13] * a[6]  * a[11] -
                    a[13] * a[7]  * a[10];

        invOut[4] = -a[4]  * a[10] * a[15] +
                     a[4]  * a[11] * a[14] +
                     a[8]  * a[6]  * a[15] -
                     a[8]  * a[7]  * a[14] -
                     a[12] * a[6]  * a[11] +
                     a[12] * a[7]  * a[10];

        invOut[8] = a[4]  * a[9] * a[15] -
                    a[4]  * a[11] * a[13] -
                    a[8]  * a[5] * a[15] +
                    a[8]  * a[7] * a[13] +
                    a[12] * a[5] * a[11] -
                    a[12] * a[7] * a[9];

        invOut[12] = -a[4]  * a[9] * a[14] +
                      a[4]  * a[10] * a[13] +
                      a[8]  * a[5] * a[14] -
                      a[8]  * a[6] * a[13] -
                      a[12] * a[5] * a[10] +
                      a[12] * a[6] * a[9];

        invOut[1] = -a[1]  * a[10] * a[15] +
                     a[1]  * a[11] * a[14] +
                     a[9]  * a[2] * a[15] -
                     a[9]  * a[3] * a[14] -
                     a[13] * a[2] * a[11] +
                     a[13] * a[3] * a[10];

        invOut[5] = a[0]  * a[10] * a[15] -
                    a[0]  * a[11] * a[14] -
                    a[8]  * a[2] * a[15] +
                    a[8]  * a[3] * a[14] +
                    a[12] * a[2] * a[11] -
                    a[12] * a[3] * a[10];

        invOut[9] = -a[0]  * a[9] * a[15] +
                     a[0]  * a[11] * a[13] +
                     a[8]  * a[1] * a[15] -
                     a[8]  * a[3] * a[13] -
                     a[12] * a[1] * a[11] +
                     a[12] * a[3] * a[9];

        invOut[13] = a[0]  * a[9] * a[14] -
                     a[0]  * a[10] * a[13] -
                     a[8]  * a[1] * a[14] +
                     a[8]  * a[2] * a[13] +
                     a[12] * a[1] * a[10] -
                     a[12] * a[2] * a[9];

        invOut[2] = a[1]  * a[6] * a[15] -
                    a[1]  * a[7] * a[14] -
                    a[5]  * a[2] * a[15] +
                    a[5]  * a[3] * a[14] +
                    a[13] * a[2] * a[7] -
                    a[13] * a[3] * a[6];

        invOut[6] = -a[0]  * a[6] * a[15] +
                     a[0]  * a[7] * a[14] +
                     a[4]  * a[2] * a[15] -
                     a[4]  * a[3] * a[14] -
                     a[12] * a[2] * a[7] +
                     a[12] * a[3] * a[6];

        invOut[10] = a[0]  * a[5] * a[15] -
                     a[0]  * a[7] * a[13] -
                     a[4]  * a[1] * a[15] +
                     a[4]  * a[3] * a[13] +
                     a[12] * a[1] * a[7] -
                     a[12] * a[3] * a[5];

        invOut[14] = -a[0]  * a[5] * a[14] +
                      a[0]  * a[6] * a[13] +
                      a[4]  * a[1] * a[14] -
                      a[4]  * a[2] * a[13] -
                      a[12] * a[1] * a[6] +
                      a[12] * a[2] * a[5];

        invOut[3] = -a[1] * a[6] * a[11] +
                     a[1] * a[7] * a[10] +
                     a[5] * a[2] * a[11] -
                     a[5] * a[3] * a[10] -
                     a[9] * a[2] * a[7] +
                     a[9] * a[3] * a[6];

        invOut[7] = a[0] * a[6] * a[11] -
                    a[0] * a[7] * a[10] -
                    a[4] * a[2] * a[11] +
                    a[4] * a[3] * a[10] +
                    a[8] * a[2] * a[7] -
                    a[8] * a[3] * a[6];

        invOut[11] = -a[0] * a[5] * a[11] +
                      a[0] * a[7] * a[9] +
                      a[4] * a[1] * a[11] -
                      a[4] * a[3] * a[9] -
                      a[8] * a[1] * a[7] +
                      a[8] * a[3] * a[5];

        invOut[15] = a[0] * a[5] * a[10] -
                     a[0] * a[6] * a[9] -
                     a[4] * a[1] * a[10] +
                     a[4] * a[2] * a[9] +
                     a[8] * a[1] * a[6] -
                     a[8] * a[2] * a[5];

        float det = a[0] * invOut[0] + a[1] * invOut[4] + a[2] * invOut[8] + a[3] * invOut[12];

        if (det == 0)
        {
            return Matrix4(); // fallback: identity
        }

        det = 1.0f / det;

        for (int i = 0; i < 16; ++i)
            invOut[i] *= det;

        // Load result back from flat array
        Matrix4 result;
#if defined(USE_SIMD_SSE)
        result.c[0] = _mm_load_ps(invOut);
        result.c[1] = _mm_load_ps(invOut + 4);
        result.c[2] = _mm_load_ps(invOut + 8);
        result.c[3] = _mm_load_ps(invOut + 12);
#elif defined(USE_SIMD_NEON)
        result.c[0] = vld1q_f32(invOut);
        result.c[1] = vld1q_f32(invOut + 4);
        result.c[2] = vld1q_f32(invOut + 8);
        result.c[3] = vld1q_f32(invOut + 12);
#else
        std::memcpy(&result.m[0][0], invOut, sizeof(invOut));
#endif
        return result;
#endif
    }
}
