// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// COMPILED WITH -ffp-contract=off (engine/CMakeLists.txt). Without it the
// compiler fuses the scalar dot product into FMA and the SIMD path no longer
// matches it — see gsplatSortKeys.h.
//
// Define VISUTWIN_KERNELS_FORCE_SCALAR to build the scalar path on a SIMD target,
// for measuring what the SIMD path is worth.
//
#include "gsplatSortKeys.h"

#include <algorithm>

#if !defined(VISUTWIN_KERNELS_FORCE_SCALAR) && defined(__SSE2__)
    #include <emmintrin.h>
    #include <xmmintrin.h>
#elif !defined(VISUTWIN_KERNELS_FORCE_SCALAR) && defined(__ARM_NEON) && defined(__aarch64__)
    #include <arm_neon.h>
#endif

namespace visutwin::canvas
{
    namespace
    {
        /// Depth in bin units to key: the scalar reference, and the tail of every
        /// SIMD block loop.
        ///
        /// The depth is clamped to the bin range BEFORE either integer conversion.
        /// Rounding can put a splat on the bound's nearest corner slightly below 0,
        /// and the unclamped conversion of that negative offset to uint32_t is
        /// undefined: x86 wrapped it to a huge value and the splat took the
        /// FARTHEST key, while ARM saturated it to 0. Clamped, it is the nearest,
        /// which is where it is.
        ///
        /// Every step here has an exact SIMD counterpart for the values the sorter
        /// produces — a depth in [0, GSPLAT_SORT_BINS], a divider below 2^24, a key
        /// below 2^31 — which is what lets the SIMD paths convert keys too rather
        /// than hand each lane back to this function.
        inline uint32_t keyFromBinDepth(float d, const GSplatSortKeyParams& p)
        {
            d = std::clamp(d, 0.0f, static_cast<float>(GSPLAT_SORT_BINS));
            const int bin = std::min(static_cast<int>(d), GSPLAT_SORT_BINS - 1);
            const auto offset = static_cast<uint32_t>(
                static_cast<float>(p.binDivider[bin]) * (d - static_cast<float>(bin)));
            return std::min(p.bucketCount - 1u, p.binBase[bin] + offset);
        }
    }

    void computeGSplatSortKeysScalar(const float* centers, const size_t count,
        const GSplatSortKeyParams& p, uint32_t* outKeys)
    {
        for (size_t i = 0; i < count; ++i) {
            const float x = centers[i * 3 + 0];
            const float y = centers[i * 3 + 1];
            const float z = centers[i * 3 + 2];
            // Evaluation order is part of the contract: ((x*dx + y*dy) + z*dz) - minDist,
            // then the multiply. The SIMD paths perform the same operations in the same order.
            const float d = (x * p.dx + y * p.dy + z * p.dz - p.minDist) * p.invBinRange;
            outKeys[i] = keyFromBinDepth(d, p);
        }
    }

#if !defined(VISUTWIN_KERNELS_FORCE_SCALAR) && defined(__SSE2__)
    const char* gsplatSortKeysBackend() { return "sse2"; }

    void computeGSplatSortKeys(const float* centers, const size_t count,
        const GSplatSortKeyParams& p, uint32_t* outKeys)
    {
        const __m128 vdx = _mm_set1_ps(p.dx);
        const __m128 vdy = _mm_set1_ps(p.dy);
        const __m128 vdz = _mm_set1_ps(p.dz);
        const __m128 vmin = _mm_set1_ps(p.minDist);
        const __m128 vinv = _mm_set1_ps(p.invBinRange);
        const __m128 zero = _mm_setzero_ps();
        const __m128 binsF = _mm_set1_ps(static_cast<float>(GSPLAT_SORT_BINS));
        const __m128i lastBin = _mm_set1_epi32(GSPLAT_SORT_BINS - 1);
        const __m128i lastKey = _mm_set1_epi32(static_cast<int>(p.bucketCount - 1u));

        // The divider as a float once per call, not once per splat; the conversion
        // is the same one keyFromBinDepth makes.
        float dividerF[GSPLAT_SORT_BINS];
        for (int b = 0; b < GSPLAT_SORT_BINS; ++b) dividerF[b] = static_cast<float>(p.binDivider[b]);

        alignas(16) int32_t bin[4];
        size_t i = 0;
        // A 4-float load at splat k reads x, y, z and the NEXT splat's x, so the
        // block's fourth load touches splat i + 4: require that it exists.
        for (; i + 4 < count; i += 4) {
            __m128 r0 = _mm_loadu_ps(centers + i * 3);       // x0 y0 z0 x1
            __m128 r1 = _mm_loadu_ps(centers + (i + 1) * 3); // x1 y1 z1 x2
            __m128 r2 = _mm_loadu_ps(centers + (i + 2) * 3); // x2 y2 z2 x3
            __m128 r3 = _mm_loadu_ps(centers + (i + 3) * 3); // x3 y3 z3 x4
            _MM_TRANSPOSE4_PS(r0, r1, r2, r3);               // r0 = all x, r1 = all y, r2 = all z

            __m128 d = _mm_add_ps(_mm_mul_ps(r0, vdx), _mm_mul_ps(r1, vdy));
            d = _mm_add_ps(d, _mm_mul_ps(r2, vdz));
            d = _mm_mul_ps(_mm_sub_ps(d, vmin), vinv);
            d = _mm_min_ps(_mm_max_ps(d, zero), binsF);

            // Truncation matches static_cast<int>; SSE2 has no signed 32-bit min, so
            // cap the bin with a compare and select.
            __m128i b = _mm_cvttps_epi32(d);
            const __m128i over = _mm_cmpgt_epi32(b, lastBin);
            b = _mm_or_si128(_mm_and_si128(over, lastBin), _mm_andnot_si128(over, b));
            const __m128 frac = _mm_sub_ps(d, _mm_cvtepi32_ps(b));

            _mm_store_si128(reinterpret_cast<__m128i*>(bin), b);
            const __m128 divider = _mm_set_ps(dividerF[bin[3]], dividerF[bin[2]],
                dividerF[bin[1]], dividerF[bin[0]]);
            const __m128i base = _mm_set_epi32(static_cast<int>(p.binBase[bin[3]]),
                static_cast<int>(p.binBase[bin[2]]), static_cast<int>(p.binBase[bin[1]]),
                static_cast<int>(p.binBase[bin[0]]));

            // Every value below 2^31, so the signed conversion and compare equal the
            // scalar path's unsigned ones.
            const __m128i sum = _mm_add_epi32(base, _mm_cvttps_epi32(_mm_mul_ps(divider, frac)));
            const __m128i capped = _mm_cmpgt_epi32(sum, lastKey);
            const __m128i key = _mm_or_si128(_mm_and_si128(capped, lastKey), _mm_andnot_si128(capped, sum));
            _mm_storeu_si128(reinterpret_cast<__m128i*>(outKeys + i), key);
        }
        computeGSplatSortKeysScalar(centers + i * 3, count - i, p, outKeys + i);
    }
#elif !defined(VISUTWIN_KERNELS_FORCE_SCALAR) && defined(__ARM_NEON) && defined(__aarch64__)
    const char* gsplatSortKeysBackend() { return "neon"; }

    void computeGSplatSortKeys(const float* centers, const size_t count,
        const GSplatSortKeyParams& p, uint32_t* outKeys)
    {
        const float32x4_t vdx = vdupq_n_f32(p.dx);
        const float32x4_t vdy = vdupq_n_f32(p.dy);
        const float32x4_t vdz = vdupq_n_f32(p.dz);
        const float32x4_t vmin = vdupq_n_f32(p.minDist);
        const float32x4_t vinv = vdupq_n_f32(p.invBinRange);
        const float32x4_t zero = vdupq_n_f32(0.0f);
        const float32x4_t binsF = vdupq_n_f32(static_cast<float>(GSPLAT_SORT_BINS));
        const int32x4_t lastBin = vdupq_n_s32(GSPLAT_SORT_BINS - 1);
        const uint32x4_t lastKey = vdupq_n_u32(p.bucketCount - 1u);

        float dividerF[GSPLAT_SORT_BINS];
        for (int b = 0; b < GSPLAT_SORT_BINS; ++b) dividerF[b] = static_cast<float>(p.binDivider[b]);

        int32_t bin[4];
        size_t i = 0;
        // vld3q_f32 deinterleaves exactly twelve floats, so a block never reads past
        // its own four splats.
        for (; i + 4 <= count; i += 4) {
            const float32x4x3_t xyz = vld3q_f32(centers + i * 3);
            float32x4_t d = vaddq_f32(vmulq_f32(xyz.val[0], vdx), vmulq_f32(xyz.val[1], vdy));
            d = vaddq_f32(d, vmulq_f32(xyz.val[2], vdz));
            d = vmulq_f32(vsubq_f32(d, vmin), vinv);
            d = vminq_f32(vmaxq_f32(d, zero), binsF);

            const int32x4_t b = vminq_s32(vcvtq_s32_f32(d), lastBin);  // truncates, like static_cast<int>
            const float32x4_t frac = vsubq_f32(d, vcvtq_f32_s32(b));

            vst1q_s32(bin, b);
            const float dividers[4] = {dividerF[bin[0]], dividerF[bin[1]], dividerF[bin[2]], dividerF[bin[3]]};
            const uint32_t bases[4] = {p.binBase[bin[0]], p.binBase[bin[1]], p.binBase[bin[2]], p.binBase[bin[3]]};

            const uint32x4_t offset = vcvtq_u32_f32(vmulq_f32(vld1q_f32(dividers), frac));
            const uint32x4_t key = vminq_u32(vaddq_u32(vld1q_u32(bases), offset), lastKey);
            vst1q_u32(outKeys + i, key);
        }
        computeGSplatSortKeysScalar(centers + i * 3, count - i, p, outKeys + i);
    }
#else
    const char* gsplatSortKeysBackend() { return "scalar"; }

    void computeGSplatSortKeys(const float* centers, const size_t count,
        const GSplatSortKeyParams& p, uint32_t* outKeys)
    {
        computeGSplatSortKeysScalar(centers, count, p, outKeys);
    }
#endif
}
