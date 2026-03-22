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
        __m128 xyz = _mm_set_ps(w, vec3.getZ(), vec3.getY(), vec3.getX());
        m128 = xyz;
#elif defined(USE_SIMD_APPLE)
        m128 = simd_make_float4(vec3.m128, w);
#elif defined(USE_SIMD_NEON)
        float temp[4] = {vec3.getX(), vec3.getY(), vec3.getZ(), w};
        m128 = vld1q_f32(temp);
#else
        v[0] = vec3.x;
        v[1] = vec3.y;
        v[2] = vec3.z;
        v[3] = w;
#endif
    }
}
