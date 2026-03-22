//
// Created by Arnis Lektauers on 25.07.2025.
//
#pragma once

#include "vector2.h"
#include "vector4.h"
#include "quaternion.h"

namespace visutwin::canvas
{
    inline Matrix4::Matrix4(const Vector4& col0, const Vector4& col1, const Vector4& col2, const Vector4& col3)
    {
#if defined(USE_SIMD_APPLE)
        cm = simd_matrix(col0.m128, col1.m128, col2.m128, col3.m128);
#elif defined(USE_SIMD_SSE) || defined(USE_SIMD_NEON)
        c[0] = col0.m128;
        c[1] = col1.m128;
        c[2] = col2.m128;
        c[3] = col3.m128;
#else
        m[0][0] = col0.getX(); m[0][1] = col0.getY(); m[0][2] = col0.getZ(); m[0][3] = col0.getW();
        m[1][0] = col1.getX(); m[1][1] = col1.getY(); m[1][2] = col1.getZ(); m[1][3] = col1.getW();
        m[2][0] = col2.getX(); m[2][1] = col2.getY(); m[2][2] = col2.getZ(); m[2][3] = col2.getW();
        m[3][0] = col3.getX(); m[3][1] = col3.getY(); m[3][2] = col3.getZ(); m[3][3] = col3.getW();
#endif
    }

    inline Vector4 Matrix4::getColumn(int col) const
    {
#if defined(USE_SIMD_APPLE)
        return Vector4(cm.columns[col]);
#elif defined(USE_SIMD_SSE) || defined(USE_SIMD_NEON)
        return Vector4(c[col]);
#else
        return Vector4(m[col][0], m[col][1], m[col][2], m[col][3]);
#endif
    }

    inline void Matrix4::setColumn(const int col, const Vector4& v)
    {
#if defined(USE_SIMD_APPLE)
        cm.columns[col] = v.m128;
#elif defined(USE_SIMD_SSE) || defined(USE_SIMD_NEON)
        c[col] = v.m128;
#else
        m[col][0] = v.x;
        m[col][1] = v.y;
        m[col][2] = v.z;
        m[col][3] = v.w;
#endif
    }

    inline Matrix4 Matrix4::operator*(const Matrix4& rhs) const
    {
#if defined(USE_SIMD_SSE)
        Matrix4 result;
        for (int i = 0; i < 4; ++i)
        {
            // Broadcast each component of rhs column i using shuffles (portable SSE)
            __m128 rhs0 = _mm_shuffle_ps(rhs.c[i], rhs.c[i], _MM_SHUFFLE(0, 0, 0, 0));
            __m128 rhs1 = _mm_shuffle_ps(rhs.c[i], rhs.c[i], _MM_SHUFFLE(1, 1, 1, 1));
            __m128 rhs2 = _mm_shuffle_ps(rhs.c[i], rhs.c[i], _MM_SHUFFLE(2, 2, 2, 2));
            __m128 rhs3 = _mm_shuffle_ps(rhs.c[i], rhs.c[i], _MM_SHUFFLE(3, 3, 3, 3));

            // Multiply-accumulate the column vector
            __m128 col = _mm_mul_ps(c[0], rhs0);
            col = _mm_add_ps(col, _mm_mul_ps(c[1], rhs1));
            col = _mm_add_ps(col, _mm_mul_ps(c[2], rhs2));
            col = _mm_add_ps(col, _mm_mul_ps(c[3], rhs3));

            result.c[i] = col;
        }
        return result;
#elif defined(USE_SIMD_APPLE)
        return Matrix4(simd_mul(cm, rhs.cm));
#elif defined(USE_SIMD_NEON)
        Matrix4 result;
        for (int i = 0; i < 4; ++i)
        {
            // Broadcast each component of rhs column i
            const float32x4_t col0 = vmulq_n_f32(c[0], vgetq_lane_f32(rhs.c[i], 0)); // m[0][0] * rhs[i][0]
            const float32x4_t col1 = vmulq_n_f32(c[1], vgetq_lane_f32(rhs.c[i], 1)); // m[1][0] * rhs[i][1]
            const float32x4_t col2 = vmulq_n_f32(c[2], vgetq_lane_f32(rhs.c[i], 2)); // m[2][0] * rhs[i][2]
            const float32x4_t col3 = vmulq_n_f32(c[3], vgetq_lane_f32(rhs.c[i], 3)); // m[3][0] * rhs[i][3]

            // Accumulate the results into result.r[i]
            const float32x4_t sum1 = vaddq_f32(col0, col1);
            const float32x4_t sum2 = vaddq_f32(col2, col3);
            result.c[i] = vaddq_f32(sum1, sum2);
        }
        return result;
#else
        Matrix4 result;
        for (int i = 0; i < 4; ++i)
        {
            for (int row = 0; row < 4; ++row)
            {
                result.m[i][row] =
                    m[0][row] * rhs.m[i][0] +
                    m[1][row] * rhs.m[i][1] +
                    m[2][row] * rhs.m[i][2] +
                    m[3][row] * rhs.m[i][3];
            }
        }
        return result;
#endif
    }

    inline Vector4 Matrix4::operator*(const Vector4& v) const
    {
#if defined(USE_SIMD_SSE)
        Vector4 result;

        __m128 x = _mm_set1_ps(v.getX());
        __m128 y = _mm_set1_ps(v.getY());
        __m128 z = _mm_set1_ps(v.getZ());
        __m128 w = _mm_set1_ps(v.getW());

        __m128 res = _mm_add_ps(
            _mm_add_ps(_mm_mul_ps(c[0], x), _mm_mul_ps(c[1], y)),
            _mm_add_ps(_mm_mul_ps(c[2], z), _mm_mul_ps(c[3], w)));

        result.m128 = res;
        return result;
#elif defined(USE_SIMD_APPLE)
        return Vector4(simd_mul(cm, v.m128));
#elif defined(USE_SIMD_NEON)
        float32x4_t x = vdupq_n_f32(v.getX());
        float32x4_t y = vdupq_n_f32(v.getY());
        float32x4_t z = vdupq_n_f32(v.getZ());
        float32x4_t w = vdupq_n_f32(v.getW());

        float32x4_t res = vmulq_f32(c[0], x);
        res = vmlaq_f32(res, c[1], y);
        res = vmlaq_f32(res, c[2], z);
        res = vmlaq_f32(res, c[3], w);

        return Vector4(res);
#else
        Vector4 result;
        for (int row = 0; row < 4; ++row)
        {
            result.v[row] = m[0][row] * v.x +
                m[1][row] * v.y +
                m[2][row] * v.z +
                m[3][row] * v.w;
        }
        return result;
#endif
    }

    inline Vector3 Matrix4::operator*(const Vector3& v) const
    {
#if defined(USE_SIMD_SSE)
        const __m128 x = _mm_set1_ps(v.getX());
        const __m128 y = _mm_set1_ps(v.getY());
        const __m128 z = _mm_set1_ps(v.getZ());
        const __m128 res = _mm_add_ps(
            _mm_add_ps(_mm_mul_ps(c[0], x), _mm_mul_ps(c[1], y)),
            _mm_mul_ps(c[2], z)
        );

        alignas(16) float out[4];
        _mm_store_ps(out, res);
        return Vector3(out[0], out[1], out[2]);
#elif defined(USE_SIMD_APPLE)
        const simd_float4 vec = simd_make_float4(v.getX(), v.getY(), v.getZ(), 0.0f);
        const simd_float4 res = simd_mul(cm, vec);
        return Vector3(res.x, res.y, res.z);
#elif defined(USE_SIMD_NEON)
        const float32x4_t x = vdupq_n_f32(v.getX());
        const float32x4_t y = vdupq_n_f32(v.getY());
        const float32x4_t z = vdupq_n_f32(v.getZ());
        float32x4_t res = vmulq_f32(c[0], x);
        res = vmlaq_f32(res, c[1], y);
        res = vmlaq_f32(res, c[2], z);
        return Vector3(
            vgetq_lane_f32(res, 0),
            vgetq_lane_f32(res, 1),
            vgetq_lane_f32(res, 2)
        );
#else
        // Transform direction vector (w=0): apply only linear 3x3 part.
        return Vector3(
            m[0][0] * v.x + m[1][0] * v.y + m[2][0] * v.z,
            m[0][1] * v.x + m[1][1] * v.y + m[2][1] * v.z,
            m[0][2] * v.x + m[1][2] * v.y + m[2][2] * v.z
        );
#endif
    }

    inline Matrix4 Matrix4::lookToLH(const Vector3& eye, const Vector3& dir, const Vector3& up)
    {
        Matrix4 result;
#if defined(USE_SIMD_SSE)
        __m128 zaxis = _mm_normalize3_ps(dir.m128); // forward
        __m128 xaxis = _mm_normalize3_ps(_mm_cross3_ps(up.m128, zaxis)); // right
        __m128 yaxis = _mm_cross3_ps(zaxis, xaxis); // up

        __m128 negEye = _mm_sub_ps(_mm_setzero_ps(), eye.m128); // -eye
        __m128 d0 = _mm_dp_ps(xaxis, negEye, 0x71);
        __m128 d1 = _mm_dp_ps(yaxis, negEye, 0x71);
        __m128 d2 = _mm_dp_ps(zaxis, negEye, 0x71);

        __m128 r0 = _mm_insert_ps(xaxis, d0, 0x30); // insert d0[0] into lane 3 (w)
        __m128 r1 = _mm_insert_ps(yaxis, d1, 0x30);
        __m128 r2 = _mm_insert_ps(zaxis, d2, 0x30);
        __m128 r3 = _mm_set_ps(0.0f, 0.0f, 0.0f, 1.0f);

        // Transpose to column-major
        _MM_TRANSPOSE4_PS(r0, r1, r2, r3);

        result.c[0] = r0;
        result.c[1] = r1;
        result.c[2] = r2;
        result.c[3] = r3;
#elif defined(USE_SIMD_APPLE)
        const simd_float3 forward = simd_normalize(dir.m128); // z axis (look direction)
        const simd_float3 right = simd_normalize(simd_cross(up.m128, forward)); // x axis
        const simd_float3 newUp = simd_cross(forward, right); // y axis

        float dx = -simd_dot(right, eye.m128);
        float dy = -simd_dot(newUp, eye.m128);
        float dz = -simd_dot(forward, eye.m128);

        // Column-major: each column holds one component of each axis, matching the scalar path
        // Column 0: [right.x, newUp.x, forward.x, 0]
        // Column 1: [right.y, newUp.y, forward.y, 0]
        // Column 2: [right.z, newUp.z, forward.z, 0]
        // Column 3: [dx, dy, dz, 1]
        result.cm.columns[0] = simd_make_float4(right[0], newUp[0], forward[0], 0.0f);
        result.cm.columns[1] = simd_make_float4(right[1], newUp[1], forward[1], 0.0f);
        result.cm.columns[2] = simd_make_float4(right[2], newUp[2], forward[2], 0.0f);
        result.cm.columns[3] = simd_make_float4(dx, dy, dz, 1.0f);
#elif defined(USE_SIMD_NEON)
        float32x4_t zaxis = neon_normalize3(dir.m128);
        float32x4_t xaxis = neon_normalize3(vcrossq_f32(up.m128, zaxis));
        float32x4_t yaxis = vcrossq_f32(zaxis, xaxis);

        float32x4_t negEye = vnegq_f32(eye.m128);

        float d0 = vdotq3_f32(xaxis, negEye);
        float d1 = vdotq3_f32(yaxis, negEye);
        float d2 = vdotq3_f32(zaxis, negEye);

        float32x4_t r0 = xaxis;
        float32x4_t r1 = yaxis;
        float32x4_t r2 = zaxis;
        float32x4_t r3 = {0.0f, 0.0f, 0.0f, 1.0f};

        r0 = vsetq_lane_f32(d0, r0, 3);
        r1 = vsetq_lane_f32(d1, r1, 3);
        r2 = vsetq_lane_f32(d2, r2, 3);

        // Transpose 4x4 matrix (row-major to column-major)
        neon_transpose4x4(r0, r1, r2, r3, result.c[0], result.c[1], result.c[2], result.c[3]);
#else
        const Vector3 forward = dir.normalized();
        const Vector3 right = up.cross(forward).normalized();
        const Vector3 newUp = forward.cross(right);

        const float dx = -right.dot(eye);
        const float dy = -newUp.dot(eye);
        const float dz = -forward.dot(eye);

        result.m[0][0] = right.x;   result.m[1][0] = right.y;   result.m[2][0] = right.z;   result.m[3][0] = dx;
        result.m[0][1] = newUp.x;   result.m[1][1] = newUp.y;   result.m[2][1] = newUp.z;   result.m[3][1] = dy;
        result.m[0][2] = forward.x; result.m[1][2] = forward.y; result.m[2][2] = forward.z; result.m[3][2] = dz;
        result.m[0][3] = 0.0f;      result.m[1][3] = 0.0f;      result.m[2][3] = 0.0f;      result.m[3][3] = 1.0f;
#endif
        return result;
    }


    inline Vector3 Matrix4::getPosition() const
    {
#if defined(USE_SIMD_SSE)
        alignas(16) float tmp[4];
        _mm_store_ps(tmp, c[3]);
        return {tmp[0], tmp[1], tmp[2]};
#elif defined(USE_SIMD_APPLE)
        return Vector3(cm.columns[3]);
#elif defined(USE_SIMD_NEON)
        return {
            vgetq_lane_f32(c[3], 0),
            vgetq_lane_f32(c[3], 1),
            vgetq_lane_f32(c[3], 2)
        };
#else
        // Fallback: column-major, so translation is in 4th column (index 3)
        return {m[3][0], m[3][1], m[3][2]};
#endif
    }

    inline Quaternion Matrix4::getRotation() const
    {
        // Get the linear part columns (basis vectors carrying rotation+scale) and remove non-uniform scale
        const Vector3 x = Vector3(getColumn(0)).normalized();
        Vector3 y = Vector3(getColumn(1)).normalized();
        Vector3 z = Vector3(getColumn(2)).normalized();

        // Orthonormal tidy-up (optional but improves stability)
        // Gram-Schmidt lite: make Y orthogonal to X, then recompute Z as X×Y
        y = (y - x * y.dot(x)).normalized();
        z = x.cross(y).normalized();

        // Fix possible reflection (determinant < 0): flip one axis
        const auto det = x.dot(y.cross(z));
        if (det < 0.0f)
        {
            z = -z; // Flip the last axis to eliminate the reflection
        }

        // Convert to quaternion
        float r00 = x.getX(), r01 = y.getX(), r02 = z.getX();
        float r10 = x.getY(), r11 = y.getY(), r12 = z.getY();
        float r20 = x.getZ(), r21 = y.getZ(), r22 = z.getZ();

        float trace = r00 + r11 + r22;
        Quaternion q;

        if (trace > 0.0f)
        {
            const float s = 0.5f / std::sqrt(trace + 1.0f);
            q = Quaternion((r21 - r12) * s, (r02 - r20) * s, (r10 - r01) * s, 0.25f / s);
        }
        else if (r00 > r11 && r00 > r22)
        {
            const float s = 2.0f * std::sqrt(std::max(0.0f, 1.0f + r00 - r11 - r22));
            q = Quaternion(0.25f * s, (r01 + r10) / s, (r02 + r20) / s, (r21 - r12) / s);
        }
        else if (r11 > r22)
        {
            const float s = 2.0f * std::sqrt(std::max(0.0f, 1.0f + r11 - r00 - r22));
            q = Quaternion((r01 + r10) / s, 0.25f * s, (r12 + r21) / s, (r02 - r20) / s);
        }
        else
        {
            const float s = 2.0f * std::sqrt(std::max(0.0f, 1.0f + r22 - r00 - r11));
            q = Quaternion((r02 + r20) / s, (r12 + r21) / s, 0.25f * s, (r10 - r01) / s);
        }

        return q.normalized();
    }

    /*
     * Function which evaluates perspective projection matrix half-size on the near plane
     */
    inline Vector2 getPerspectiveHalfSize(const float fov, const float aspect, const float zNear, const bool fovIsHorizontal) {
        if (fovIsHorizontal) {
            const float x = zNear * std::tan(fov * PI / 360.0f);
            return Vector2(x, x / aspect);
        }

        const float y = zNear * std::tan(fov * PI / 360.0f);
        return Vector2(y * aspect, y);
    }

    inline Matrix4 Matrix4::frustum(const float left, const float right, const float bottom, const float top, const float zNear, const float zFar)
    {
        const float zNear2 = 2 * zNear;
        const float width = right - left;
        const float height = top - bottom;
        const float depth = zFar - zNear;

        Matrix4 r;
        r.setElement(0, 0, zNear2 / width);
        r.setElement(0, 1, 0.0f);
        r.setElement(0, 2, 0.0f);
        r.setElement(0, 3, 0.0f);

        r.setElement(1, 0, 0.0f);
        r.setElement(1, 1, zNear2 / height);
        r.setElement(1, 2, 0.0f);
        r.setElement(1, 3, 0.0f);

        r.setElement(2, 0, (right + left) / width);
        r.setElement(2, 1, (top + bottom) / height);
        r.setElement(2, 2, (-zFar - zNear) / depth);
        r.setElement(2, 3, -1);

        r.setElement(3, 0, 0.0f);
        r.setElement(3, 1, 0.0f);
        r.setElement(3, 2, (-zNear2 * zFar) / depth);
        r.setElement(3, 3, 0.0f);

        return r;
    }

    inline Vector3 Matrix4::getScale() const
    {
        Vector3 x = getColumn(0);
        Vector3 y = getColumn(1);
        Vector3 z = getColumn(2);
        return Vector3(x.length(), y.length(), z.length());
    }

    inline Matrix4 Matrix4::perspective(const float fov, const float aspect, const float zNear, const float zFar, const bool fovIsHorizontal)
    {
        const auto halfSize = getPerspectiveHalfSize(fov, aspect, zNear, fovIsHorizontal);
        return Matrix4::frustum(-halfSize.x, halfSize.x, -halfSize.y, halfSize.y, zNear, zFar);
    }

    inline Matrix4 Matrix4::ortho(const float left, const float right, const float bottom, const float top, const float near, const float far)
    {
        Matrix4 r;
        r.setElement(0, 0, 2 / (right - left));
        r.setElement(0, 1, 0.0f);
        r.setElement(0, 2, 0.0f);
        r.setElement(0, 3, 0.0f);

        r.setElement(1, 0, 0.0f);
        r.setElement(1, 1, 2 / (top - bottom));
        r.setElement(1, 2, 0.0f);
        r.setElement(1, 3, 0.0f);

        r.setElement(2, 0, 0.0f);
        r.setElement(2, 1, 0.0f);
        r.setElement(2, 2, -2 / (far - near));
        r.setElement(2, 3, 0.0f);

        r.setElement(3, 0, -(right + left) / (right - left));
        r.setElement(3, 1, -(top + bottom) / (top - bottom));
        r.setElement(3, 2, -(far + near) / (far - near));
        r.setElement(3, 3, 1);
        return r;
    }

    inline Matrix4 Matrix4::trs(const Vector3& t, const Quaternion& r, const Vector3& s)
    {
        auto qx = r.getX();
        auto qy = r.getY();
        auto qz = r.getZ();
        auto qw = r.getW();

        auto sx = s.getX();
        auto sy = s.getY();
        auto sz = s.getZ();

        auto x2 = qx + qx;
        auto y2 = qy + qy;
        auto z2 = qz + qz;
        auto xx = qx * x2;
        auto xy = qx * y2;
        auto xz = qx * z2;
        auto yy = qy * y2;
        auto yz = qy * z2;
        auto zz = qz * z2;
        auto wx = qw * x2;
        auto wy = qw * y2;
        auto wz = qw * z2;

        Vector4 col0((1 - (yy + zz)) * sx, (xy + wz) * sx, (xz - wy) * sx, 0);
        Vector4 col1((xy - wz) * sy, (1 - (xx + zz)) * sy, (yz + wx) * sy, 0);
        Vector4 col2((xz + wy) * sz, (yz - wx) * sz, (1 - (xx + yy)) * sz, 0);
        Vector4 col3(t.getX(), t.getY(), t.getZ(), 1);

        return Matrix4(col0, col1, col2, col3);
    }

    inline Vector3 Matrix4::getTranslation() const
    {
        return getPosition();
    }

    inline Vector3 Matrix4::transformPoint(const Vector3& v) const
    {
        // w=1 transform: apply full 3x3 rotation + translation
        return v.transform(*this);
    }

    inline Matrix4 Matrix4::mulAffine(const Matrix4& rhs) const
    {
        auto a00 = getElement(0, 0);
        auto a01 = getElement(0, 1);
        auto a02 = getElement(0, 2);
        auto a10 = getElement(1, 0);
        auto a11 = getElement(1, 1);
        auto a12 = getElement(1, 2);
        auto a20 = getElement(2, 0);
        auto a21 = getElement(2, 1);
        auto a22 = getElement(2, 2);
        auto a30 = getElement(3, 0);
        auto a31 = getElement(3, 1);
        auto a32 = getElement(3, 2);

        auto b0 = rhs.getElement(0, 0);
        auto b1 = rhs.getElement(0, 1);
        auto b2 = rhs.getElement(0, 2);
        Vector4 col0(a00 * b0 + a10 * b1 + a20 * b2,
            a01 * b0 + a11 * b1 + a21 * b2,
            a02 * b0 + a12 * b1 + a22 * b2,
            0);

        b0 = rhs.getElement(1, 0);
        b1 = rhs.getElement(1, 1);
        b2 = rhs.getElement(1, 2);
        Vector4 col1(a00 * b0 + a10 * b1 + a20 * b2,
            a01 * b0 + a11 * b1 + a21 * b2,
            a02 * b0 + a12 * b1 + a22 * b2,
            0);

        b0 = rhs.getElement(2, 0);
        b1 = rhs.getElement(2, 1);
        b2 = rhs.getElement(2, 2);
        Vector4 col2(a00 * b0 + a10 * b1 + a20 * b2,
            a01 * b0 + a11 * b1 + a21 * b2,
            a02 * b0 + a12 * b1 + a22 * b2,
            0);

        b0 = rhs.getElement(3, 0);
        b1 = rhs.getElement(3, 1);
        b2 = rhs.getElement(3, 2);
        Vector4 col3(a00 * b0 + a10 * b1 + a20 * b2 + a30,
            a01 * b0 + a11 * b1 + a21 * b2 + a31,
            a02 * b0 + a12 * b1 + a22 * b2 + a32,
            1);

        return Matrix4(col0, col1, col2, col3);
    }
}
