//
// Created by Arnis Lektauers on 27.07.2025.
//
#pragma once

#include "vector3.h"

namespace visutwin::canvas
{
    //Vector4 Vector4::UNIT_X = Vector4(1, 0, 0, 0);
    //Vector4 Vector4::UNIT_Y = Vector4(0, 1, 0, 0);
   // Vector4 Vector4::UNIT_Z = Vector4(0, 0, 1, 0);

    inline Vector4::Vector4(const Vector3& vec3, float w)
    {
#if defined(USE_SIMD_SSE)
        m128 = _mm_insert_ps(vec3.m128, _mm_set_ss(w), 0x30);
#elif defined(USE_SIMD_APPLE)
        m128 = simd_make_float4(vec3.m128, w);
#elif defined(USE_SIMD_NEON)
        m128 = vsetq_lane_f32(w, vec3.m128, 3);
#else
        v[0] = vec3.x;
        v[1] = vec3.y;
        v[2] = vec3.z;
        v[3] = w;
#endif
    }

    inline Vector3 Vector4::perspectiveDivide() const
    {
#if defined(USE_SIMD_SSE)
        const __m128 w = _mm_shuffle_ps(m128, m128, _MM_SHUFFLE(3, 3, 3, 3));
        return Vector3(_mm_insert_ps(_mm_div_ps(m128, w), _mm_setzero_ps(), 0x30));
#elif defined(USE_SIMD_APPLE)
        return Vector3(simd_make_float3(m128) / m128.w);
#elif defined(USE_SIMD_NEON)
        return Vector3(vsetq_lane_f32(0.0f, vdivq_f32(m128, vdupq_laneq_f32(m128, 3)), 3));
#else
        return Vector3(x / w, y / w, z / w);
#endif
    }
}
