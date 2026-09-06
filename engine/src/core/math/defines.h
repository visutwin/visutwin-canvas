// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers  on 31.08.2025
//
#pragma once

#include <ostream>
#include <numbers>

#if defined(__SSE__) || defined(__SSE2__)
    #include <immintrin.h>
#endif
#if defined(__ARM_NEON)
    #include <arm_neon.h>
#endif
#if defined(__APPLE__)
    #include <simd/simd.h>
#endif

#define USE_SIMD_MATH

// SIMD backend selection priority:
//   1. USE_SIMD_PREFER_NEON — force NEON on ARM (including Apple Silicon), useful for testing
//   2. SSE on x86
//   3. Apple SIMD on Apple platforms (preferred on Apple Silicon — uses Accelerate framework)
//   4. NEON on non-Apple ARM (e.g., Android, Linux ARM)
//
// The SSE path requires SSE4.1, not merely SSE: it uses _mm_dp_ps and _mm_insert_ps
// (vector3.inl, vector4.h, matrix4.inl, util/simd.h). Selecting it on __SSE__ alone —
// which every x86-64 target defines — meant it could not compile at all on a stock
// x86-64 baseline, where clang stops at SSE2 and no -msse4.1 is set anywhere in the
// build. Falling through leaves x86 on the scalar path rather than failing to build.
#if defined(USE_SIMD_MATH) && defined(USE_SIMD_PREFER_NEON) && defined(__ARM_NEON)
    #define USE_SIMD_NEON
#elif defined(USE_SIMD_MATH) && defined(__SSE4_1__)
    #define USE_SIMD_SSE
#elif defined(USE_SIMD_MATH) && defined(__APPLE__)
    #define USE_SIMD_APPLE
#elif defined(USE_SIMD_MATH) && defined(__ARM_NEON)
    #define USE_SIMD_NEON
#else
    #undef USE_SIMD_MATH
#endif

namespace visutwin::canvas
{
    // Archimedes' constant (π)
    constexpr float PI = std::numbers::pi;

    // The full circle constant (τ)
    constexpr float TAU = std::numbers::pi * 2;

    constexpr float RAD_TO_DEG = 180.0f / PI;
    constexpr float DEG_TO_RAD = PI / 180.0f;

    constexpr float degToRad(const float degrees) {
        return degrees * DEG_TO_RAD;
    }
}
