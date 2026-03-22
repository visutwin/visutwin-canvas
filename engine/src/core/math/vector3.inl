//
// Created by Arnis Lektauers on 27.07.2025.
//
#pragma once

#include "matrix4.h"
#include "vector4.h"

namespace visutwin::canvas
{
    inline Vector3 Vector3::UNIT_X = Vector3(1, 0, 0);
    inline Vector3 Vector3::UNIT_Y = Vector3(0, 1, 0);
    inline Vector3 Vector3::UNIT_Z = Vector3(0, 0, 1);

    inline Vector3::Vector3(const Vector4& other)
    {
#if defined(USE_SIMD_APPLE)
        m128 = simd_make_float3(other.m128);
#elif defined(USE_SIMD_SSE)
        m128 = _mm_set_ps(0.0f, other.getZ(), other.getY(), other.getX());
#elif defined(USE_SIMD_NEON)
        float tmp[4] = {other.getX(), other.getY(), other.getZ(), 0.0f};
        m128 = vld1q_f32(tmp);
#else
        x = other.getX();
        y = other.getY();
        z = other.getZ();
#endif
    }

    inline Vector3 Vector3::operator*(float scalar) const
    {
#if defined(USE_SIMD_SSE)
        __m128 scalarVec = _mm_set1_ps(scalar);            // Broadcast scalar to all 4 lanes
        return Vector3(_mm_mul_ps(this->m128, scalarVec));   // Multiply vectors
#elif defined(USE_SIMD_APPLE)
        return Vector3(m128 * scalar);
#elif defined(USE_SIMD_NEON)
        float32x4_t scalarVec = vdupq_n_f32(scalar); // Broadcast scalar to all 4 lanes
        return Vector3(vmulq_f32(this->m128, scalarVec)); // Multiply vectors
#else
        return Vector3(x * scalar, y * scalar, z * scalar);
#endif
    }

    inline Vector3 Vector3::operator*(const Vector3& other) const
    {
#if defined(USE_SIMD_SSE)
        return Vector3(_mm_mul_ps(this->m128, other.m128));   // Element-wise multiply
#elif defined(USE_SIMD_APPLE)
        return Vector3(m128 * other.m128);
#elif defined(USE_SIMD_NEON)
        return Vector3(vmulq_f32(this->m128, other.m128)); // Element-wise multiply
#else
        return Vector3(x * other.x, y * other.y, z * other.z);
#endif
    }

    inline float Vector3::dot(const Vector3& other) const
    {
#if defined(USE_SIMD_SSE)
        // 3-component dot product: mask 0x71 = multiply lanes 0,1,2 only, store in lane 0
        return _mm_cvtss_f32(_mm_dp_ps(this->m128, other.m128, 0x71));
#elif defined(USE_SIMD_APPLE)
        return simd_dot(this->m128, other.m128);
#elif defined(USE_SIMD_NEON)
        // 3-component dot: explicitly sum only lanes 0,1,2 (exclude w)
        const float32x4_t mul = vmulq_f32(this->m128, other.m128);
        return vgetq_lane_f32(mul, 0) + vgetq_lane_f32(mul, 1) + vgetq_lane_f32(mul, 2);
#else
        return x * other.x + y * other.y + z * other.z;
#endif
    }

    inline Vector3 Vector3::normalized() const
    {
        const float len = length();
        if (len == 0.0f) {
            return {};
        }
#if defined(USE_SIMD_SSE)
        __m128 invLen = _mm_set1_ps(1.0f / len);
        __m128 scaled = _mm_mul_ps(this->m128, invLen);
        // Zero out w lane (lane 3) to keep it clean
        scaled = _mm_insert_ps(scaled, _mm_setzero_ps(), 0x30);
        Vector3 result;
        result.m128 = scaled;
        return result;
#elif defined(USE_SIMD_APPLE)
        return Vector3(simd_normalize(this->m128));
#elif defined(USE_SIMD_NEON)
        float32x4_t invLen = vdupq_n_f32(1.0f / len);
        float32x4_t scaled = vmulq_f32(this->m128, invLen);
        // Zero out w lane (lane 3) to keep it clean
        scaled = vsetq_lane_f32(0.0f, scaled, 3);
        Vector3 result;
        result.m128 = scaled;
        return result;
#else
        return Vector3(x / len, y / len, z / len);
#endif
    }

    inline Vector3 Vector3::cross(const Vector3& other) const
    {
#if defined(USE_SIMD_SSE)
        // Shuffle components: (y, z, x, w)
        __m128 a_yzx = _mm_shuffle_ps(this->m128, this->m128, _MM_SHUFFLE(3, 0, 2, 1));
        __m128 b_yzx = _mm_shuffle_ps(other.m128, other.m128, _MM_SHUFFLE(3, 0, 2, 1));

        // Cross product: a × b = (a.yzx * b.zxy - a.zxy * b.yzx)
        __m128 a_zxy = _mm_shuffle_ps(this->m128, this->m128, _MM_SHUFFLE(3, 1, 0, 2));
        __m128 b_zxy = _mm_shuffle_ps(other.m128, other.m128, _MM_SHUFFLE(3, 1, 0, 2));

        __m128 c1 = _mm_mul_ps(a_yzx, b_zxy);
        __m128 c2 = _mm_mul_ps(a_zxy, b_yzx);
        __m128 crossResult = _mm_sub_ps(c1, c2);
        // Zero out w lane (lane 3) to keep it clean
        crossResult = _mm_insert_ps(crossResult, _mm_setzero_ps(), 0x30);
        Vector3 result;
        result.m128 = crossResult;
        return result;
#elif defined(USE_SIMD_APPLE)
        return Vector3(simd_cross(this->m128, other.m128));
#elif defined(USE_SIMD_NEON)
        Vector3 result;
        result.m128 = vcrossq_f32(this->m128, other.m128);
        return result;
#else
        return Vector3(y * other.z - z * other.y, z * other.x - x * other.z, x * other.y - y * other.x);
#endif
    }

    inline Matrix4 Vector3::toScalingMatrix() const
    {
#if defined(USE_SIMD_SSE)
        Matrix4 result;
        float sx = getX(), sy = getY(), sz = getZ();
        result.c[0] = _mm_set_ps(0.0f, 0.0f, 0.0f, sx);   // [sx, 0, 0, 0]
        result.c[1] = _mm_set_ps(0.0f, 0.0f, sy,   0.0f);  // [0, sy, 0, 0]
        result.c[2] = _mm_set_ps(0.0f, sz,   0.0f, 0.0f);  // [0, 0, sz, 0]
        result.c[3] = _mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f);  // [0, 0, 0, 1]
        return result;
#elif defined(USE_SIMD_APPLE)
        const simd_float4 x = simd_make_float4(m128.x, 0.0f, 0.0f, 0.0f);
        const simd_float4 y = simd_make_float4(0.0f, m128.y, 0.0f, 0.0f);
        const simd_float4 z = simd_make_float4(0.0f, 0.0f, m128.z, 0.0f);
        const simd_float4 w = simd_make_float4(0.0f, 0.0f, 0.0f, 1.0f);
        return Matrix4(x, y, z, w);
#elif defined(USE_SIMD_NEON)
        float sx = getX(), sy = getY(), sz = getZ();
        Matrix4 result;
        result.c[0] = vdupq_n_f32(0.0f);
        result.c[1] = vdupq_n_f32(0.0f);
        result.c[2] = vdupq_n_f32(0.0f);
        result.c[3] = vdupq_n_f32(0.0f);
        result.c[0] = vsetq_lane_f32(sx, result.c[0], 0);
        result.c[1] = vsetq_lane_f32(sy, result.c[1], 1);
        result.c[2] = vsetq_lane_f32(sz, result.c[2], 2);
        result.c[3] = vsetq_lane_f32(1.0f, result.c[3], 3);
        return result;
#else
        Matrix4 m;
        m.m[0][0] = x;    m.m[0][1] = 0.0f; m.m[0][2] = 0.0f; m.m[0][3] = 0.0f;
        m.m[1][0] = 0.0f; m.m[1][1] = y;    m.m[1][2] = 0.0f; m.m[1][3] = 0.0f;
        m.m[2][0] = 0.0f; m.m[2][1] = 0.0f; m.m[2][2] = z;    m.m[2][3] = 0.0f;
        m.m[3][0] = 0.0f; m.m[3][1] = 0.0f; m.m[3][2] = 0.0f; m.m[3][3] = 1.0f;
        return m;
#endif
    }

    inline Matrix4 Vector3::toTranslationMatrix() const
    {
#if defined(USE_SIMD_SSE)
        Matrix4 result;
        result.c[0] = _mm_setr_ps(1.0f, 0.0f, 0.0f, 0.0f);
        result.c[1] = _mm_setr_ps(0.0f, 1.0f, 0.0f, 0.0f);
        result.c[2] = _mm_setr_ps(0.0f, 0.0f, 1.0f, 0.0f);
        // Build translation column: [x, y, z, 1]
        __m128 trans = this->m128;
        trans = _mm_insert_ps(trans, _mm_set_ss(1.0f), 0x30); // Set w (lane 3) = 1.0f
        result.c[3] = trans;
        return result;
#elif defined(USE_SIMD_APPLE)
        Matrix4 result;
        result.cm.columns[3] = simd_make_float4(this->m128, 1.0f);
        return result;
#elif defined(USE_SIMD_NEON)
        Matrix4 result;
        float tx = getX(), ty = getY(), tz = getZ();
        result.c[0] = vdupq_n_f32(0.0f);
        result.c[1] = vdupq_n_f32(0.0f);
        result.c[2] = vdupq_n_f32(0.0f);
        result.c[3] = vdupq_n_f32(0.0f);
        result.c[0] = vsetq_lane_f32(1.0f, result.c[0], 0);
        result.c[1] = vsetq_lane_f32(1.0f, result.c[1], 1);
        result.c[2] = vsetq_lane_f32(1.0f, result.c[2], 2);
        result.c[3] = vsetq_lane_f32(tx, result.c[3], 0);
        result.c[3] = vsetq_lane_f32(ty, result.c[3], 1);
        result.c[3] = vsetq_lane_f32(tz, result.c[3], 2);
        result.c[3] = vsetq_lane_f32(1.0f, result.c[3], 3);
        return result;
#else
        Matrix4 result;
        result.m[0][0] = 1.0f;
        result.m[1][1] = 1.0f;
        result.m[2][2] = 1.0f;
        result.m[3][0] = x;
        result.m[3][1] = y;
        result.m[3][2] = z;
        result.m[3][3] = 1.0f;
        return result;
#endif
    }

    inline Vector3 Vector3::transform(const Matrix4& mat) const
    {
#if defined(USE_SIMD_SSE)
        Vector3 result;
        __m128 x = _mm_shuffle_ps(this->m128, this->m128, _MM_SHUFFLE(0,0,0,0));
        __m128 y = _mm_shuffle_ps(this->m128, this->m128, _MM_SHUFFLE(1,1,1,1));
        __m128 z = _mm_shuffle_ps(this->m128, this->m128, _MM_SHUFFLE(2,2,2,2));

        __m128 res = _mm_add_ps(_mm_add_ps(
            _mm_mul_ps(x, mat.c[0]),
            _mm_mul_ps(y, mat.c[1])),
            _mm_add_ps(_mm_mul_ps(z, mat.c[2]), mat.c[3])
        );

        result.m128 = res;
        return result;
#elif defined(USE_SIMD_APPLE)
        const simd_float4 v4 = simd_make_float4(m128, 1.0f);
        const simd_float4 transformed = simd_mul(mat.cm, v4);
        return Vector3(transformed);
#elif defined(USE_SIMD_NEON)
        Vector3 result;
        float32x4_t x = vmulq_n_f32(mat.c[0], vgetq_lane_f32(this->m128, 0));
        float32x4_t y = vmulq_n_f32(mat.c[1], vgetq_lane_f32(this->m128, 1));
        float32x4_t z = vmulq_n_f32(mat.c[2], vgetq_lane_f32(this->m128, 2));

        float32x4_t res = vaddq_f32(vaddq_f32(x, y), vaddq_f32(z, mat.c[3]));
        result.m128 = res;
        return result;
#else
        Vector3 result;
        result.x = x * mat.m[0][0] + y * mat.m[1][0] + z * mat.m[2][0] + mat.m[3][0];
        result.y = x * mat.m[0][1] + y * mat.m[1][1] + z * mat.m[2][1] + mat.m[3][1];
        result.z = x * mat.m[0][2] + y * mat.m[1][2] + z * mat.m[2][2] + mat.m[3][2];
        return result;
#endif
    }

    inline Vector3 Vector3::transformNormal(const Matrix4& mat) const
    {
#if defined(USE_SIMD_SSE)
        Vector3 result;
        // Treat vec as a direction: w = 0, so no translation applied
        __m128 x = _mm_shuffle_ps(this->m128, this->m128, _MM_SHUFFLE(0, 0, 0, 0));
        __m128 y = _mm_shuffle_ps(this->m128, this->m128, _MM_SHUFFLE(1, 1, 1, 1));
        __m128 z = _mm_shuffle_ps(this->m128, this->m128, _MM_SHUFFLE(2, 2, 2, 2));

        __m128 res = _mm_add_ps(
            _mm_add_ps(
                _mm_mul_ps(x, mat.c[0]),
                _mm_mul_ps(y, mat.c[1])
            ),
            _mm_mul_ps(z, mat.c[2])
        );

        result.m128 = res;
        return result;
#elif defined(USE_SIMD_APPLE)
        // Treat vector as a direction: no translation (w = 0.0f)
        const simd_float4 dir = simd_make_float4(m128, 0.0f);
        const simd_float4 transformed = simd_mul(mat.cm, dir);
        return Vector3(transformed);
#elif defined(USE_SIMD_NEON)
        Vector3 result;
        float32x4_t x = vmulq_n_f32(mat.c[0], vgetq_lane_f32(this->m128, 0));
        float32x4_t y = vmulq_n_f32(mat.c[1], vgetq_lane_f32(this->m128, 1));
        float32x4_t z = vmulq_n_f32(mat.c[2], vgetq_lane_f32(this->m128, 2));

        float32x4_t res = vaddq_f32(vaddq_f32(x, y), z);
        result.m128 = res;
        return result;
#else
        Vector3 result;
        result.x = x * mat.m[0][0] + y * mat.m[1][0] + z * mat.m[2][0];
        result.y = x * mat.m[0][1] + y * mat.m[1][1] + z * mat.m[2][1];
        result.z = x * mat.m[0][2] + y * mat.m[1][2] + z * mat.m[2][2];
        return result;
#endif
    }

    inline Vector3 Vector3::operator-() const
    {
#if defined(USE_SIMD_APPLE)
        return Vector3(-m128);
#elif defined(USE_SIMD_SSE) || defined(USE_SIMD_NEON)
        return Vector3(-getX(), -getY(), -getZ());
#else
        return Vector3(-x, -y, -z);
#endif
    }
}
