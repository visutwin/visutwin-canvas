// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 25.07.2025.
//
#pragma once

#if defined(__SSE__) || defined(__SSE2__)
#include <immintrin.h>
#endif
#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif
#if defined(__APPLE__)
#include <simd/simd.h>
#endif
#include <cmath>

namespace visutwin::canvas
{
#if defined(__SSE__)
    const __m128 VECTOR4_MASK_X = _mm_set_ps(0.0f, 0.0f, 0.0f, 1.0f); // x = lowest index
    const __m128 VECTOR4_MASK_Y = _mm_set_ps(0.0f, 0.0f, 1.0f, 0.0f);
    const __m128 VECTOR4_MASK_Z = _mm_set_ps(0.0f, 1.0f, 0.0f, 0.0f);
    const __m128 VECTOR4_MASK_W = _mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f);

    inline __m128 _mm_cross3_ps(__m128 a, __m128 b) {
        __m128 a_yzx = _mm_shuffle_ps(a, a, _MM_SHUFFLE(3, 0, 2, 1));
        __m128 b_yzx = _mm_shuffle_ps(b, b, _MM_SHUFFLE(3, 0, 2, 1));
        __m128 c = _mm_sub_ps(
            _mm_mul_ps(a, b_yzx),
            _mm_mul_ps(a_yzx, b)
        );
        return _mm_shuffle_ps(c, c, _MM_SHUFFLE(3, 0, 2, 1));
    }

    inline __m128 _mm_normalize3_ps(__m128 v) {
        __m128 dp = _mm_dp_ps(v, v, 0x7F);  // dot3 in .x
        __m128 rsqrt = _mm_rsqrt_ps(dp);
        return _mm_mul_ps(v, rsqrt);
    }
#elif defined(__ARM_NEON)
    const float32x4_t VECTOR4_MASK_X = (float32x4_t){ 1.0f, 0.0f, 0.0f, 0.0f };
    const float32x4_t VECTOR4_MASK_Y = (float32x4_t){ 0.0f, 1.0f, 0.0f, 0.0f };
    const float32x4_t VECTOR4_MASK_Z = (float32x4_t){ 0.0f, 0.0f, 1.0f, 0.0f };

    inline float32x4_t vcrossq_f32(float32x4_t a, float32x4_t b)
    {
        // yzxw permutation via byte-level table lookup (AArch64)
        // Maps: lane 0←lane1, lane 1←lane2, lane 2←lane0, lane 3←lane3
        // vextq_f32(v,v,1) does circular rotation [1,2,3,0] which is WRONG for cross product
        static const uint8x16_t yzxw = {4,5,6,7, 8,9,10,11, 0,1,2,3, 12,13,14,15};

        const float32x4_t a_yzx = vreinterpretq_f32_u8(vqtbl1q_u8(vreinterpretq_u8_f32(a), yzxw));
        const float32x4_t b_yzx = vreinterpretq_f32_u8(vqtbl1q_u8(vreinterpretq_u8_f32(b), yzxw));

        // c = a * b_yzx - a_yzx * b  →  {a.x*b.y - a.y*b.x, a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, 0}
        const float32x4_t c = vsubq_f32(vmulq_f32(a, b_yzx), vmulq_f32(a_yzx, b));

        // Permute result back from (z', x', y', 0) to (x', y', z', 0)
        return vreinterpretq_f32_u8(vqtbl1q_u8(vreinterpretq_u8_f32(c), yzxw));
    }

    inline float32x4_t neon_normalize3(float32x4_t v)
    {
        float32x4_t dp = vmulq_f32(v, v);
        const float sum = vgetq_lane_f32(dp, 0) + vgetq_lane_f32(dp, 1) + vgetq_lane_f32(dp, 2);
        const float invLen = 1.0f / std::sqrt(sum);
        return vmulq_n_f32(v, invLen);
    }

    inline float vdotq3_f32(float32x4_t a, float32x4_t b)
    {
        const float32x4_t mul = vmulq_f32(a, b);
        return vgetq_lane_f32(mul, 0) + vgetq_lane_f32(mul, 1) + vgetq_lane_f32(mul, 2);
    }

    inline void neon_transpose4x4(
        const float32x4_t row0, const float32x4_t row1, const float32x4_t row2, const float32x4_t row3,
        float32x4_t& col0, float32x4_t& col1, float32x4_t& col2, float32x4_t& col3)
    {
        // Step 1: Pairwise interleave at 32-bit granularity
        // vtrnq_f32({a,b,c,d}, {e,f,g,h}) → val[0]={a,e,c,g}, val[1]={b,f,d,h}
        const float32x4x2_t t01 = vtrnq_f32(row0, row1);
        const float32x4x2_t t23 = vtrnq_f32(row2, row3);

        // Step 2: Combine low/high halves to complete the transpose
        // No second vtrn_f32 needed — just vcombine the correct halves
        col0 = vcombine_f32(vget_low_f32(t01.val[0]),  vget_low_f32(t23.val[0]));
        col1 = vcombine_f32(vget_low_f32(t01.val[1]),  vget_low_f32(t23.val[1]));
        col2 = vcombine_f32(vget_high_f32(t01.val[0]), vget_high_f32(t23.val[0]));
        col3 = vcombine_f32(vget_high_f32(t01.val[1]), vget_high_f32(t23.val[1]));
    }
#elif defined(__APPLE__)
#endif
}
