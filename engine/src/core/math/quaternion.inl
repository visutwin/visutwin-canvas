//
// Created by Arnis Lektauers on 18.08.2025.
//
#pragma once

#include "matrix4.h"

namespace visutwin::canvas
{
    inline Quaternion::Quaternion()
    {
#if defined(USE_SIMD_SSE)
        simd = _mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f); // w, z, y, x
#elif defined(USE_SIMD_APPLE)
        simd = simd_quaternion(0.0f, 0.0f, 0.0f, 1.0f); // x, y, z, w
#elif defined(USE_SIMD_NEON)
        simd = vdupq_n_f32(0.0f); // Set all lanes to 0.0
        simd = vsetq_lane_f32(1.0f, simd, 3); // Set w (lane 3) to 1.0f
#else
        x = 0.0f;
        y = 0.0f;
        z = 0.0f;
        w = 1.0f;
#endif
    }

    inline Quaternion::Quaternion(const float x, const float y, const float z, const float w)
    {
#if defined(USE_SIMD_SSE)
        simd = _mm_set_ps(w, z, y, x); // _mm_set_ps: high to low = w, z, y, x
#elif defined(USE_SIMD_APPLE)
        simd = simd_quaternion(x, y, z, w);
#elif defined(USE_SIMD_NEON)
        float temp[4] = {x, y, z, w};
        simd = vld1q_f32(temp);
#else
        this->x = x;
        this->y = y;
        this->z = z;
        this->w = w;
#endif
    }

    inline Quaternion Quaternion::fromAxisAngle(const Vector3& axis, const float angle)
    {
        // Build unit quaternion: q = [ axis * sin(θ/2), cos(θ/2) ]
        const float half = angle * 0.5f * DEG_TO_RAD;
        const float sa = std::sin(half);
        const float ca = std::cos(half);

#if defined(USE_SIMD_SSE)
        __m128 axis_vec = _mm_set_ps(0.0f, axis.getZ(), axis.getY(), axis.getX()); // w = 0
        __m128 scale = _mm_set1_ps(sa);
        __m128 imag = _mm_mul_ps(axis_vec, scale); // (x, y, z, 0)
        __m128 w_vec = _mm_set_ps(ca, 0.0f, 0.0f, 0.0f); // (0, 0, 0, w)
        __m128 q = _mm_add_ps(imag, w_vec); // (x, y, z, w)

        return Quaternion(q);

#elif defined(USE_SIMD_APPLE)
        const simd_float3 s = axis.m128 * sa;
        return Quaternion(simd_quaternion(s.x, s.y, s.z, ca));

#elif defined(USE_SIMD_NEON)
        float temp[4] = {axis.getX() * sa, axis.getY() * sa, axis.getZ() * sa, ca};
        return Quaternion(vld1q_f32(temp));

#else
        return Quaternion(axis.x * sa, axis.y * sa, axis.z * sa, ca);
#endif
    }

    inline Quaternion Quaternion::conjugate() const
    {
#if defined(USE_SIMD_SSE)
        static const __m128 mask = _mm_set_ps(1.0f, -1.0f, -1.0f, -1.0f); // Mask: (-1, -1, -1, 1)
        Quaternion result;
        result.simd = _mm_mul_ps(simd, mask);
        return result;
#elif defined(USE_SIMD_APPLE)
        return Quaternion(simd_conjugate(simd));
#elif defined(USE_SIMD_NEON)
        // Mask: (-1, -1, -1, 1)
        static const uint32_t mask_data[4] = {0x80000000, 0x80000000, 0x80000000, 0x00000000};
        const uint32x4_t mask = vld1q_u32(mask_data);
        Quaternion result;
        result.simd = vreinterpretq_f32_u32(veorq_u32(vreinterpretq_u32_f32(this->simd), mask));
        return result;
#else
        return Quaternion(-x, -y, -z, w);
#endif
    }

    inline Quaternion Quaternion::operator*(const Quaternion& rhs) const
    {
#if defined(USE_SIMD_SSE)
        // Hamilton product via broadcast-multiply-add with sign masks
        // q = [x, y, z, w], result:
        //   rx = w1*x2 + x1*w2 + y1*z2 - z1*y2
        //   ry = w1*y2 - x1*z2 + y1*w2 + z1*x2
        //   rz = w1*z2 + x1*y2 - y1*x2 + z1*w2
        //   rw = w1*w2 - x1*x2 - y1*y2 - z1*z2
        __m128 q1 = simd;
        __m128 q2 = rhs.simd;

        __m128 q1_wwww = _mm_shuffle_ps(q1, q1, _MM_SHUFFLE(3, 3, 3, 3));
        __m128 q1_xxxx = _mm_shuffle_ps(q1, q1, _MM_SHUFFLE(0, 0, 0, 0));
        __m128 q1_yyyy = _mm_shuffle_ps(q1, q1, _MM_SHUFFLE(1, 1, 1, 1));
        __m128 q1_zzzz = _mm_shuffle_ps(q1, q1, _MM_SHUFFLE(2, 2, 2, 2));

        // w1 * [x2, y2, z2, w2]
        __m128 term0 = _mm_mul_ps(q1_wwww, q2);

        // x1 * [w2, -z2, y2, -x2]
        static const __m128 sign_x = _mm_set_ps(-1.0f, 1.0f, -1.0f, 1.0f);
        __m128 q2_wzyx = _mm_shuffle_ps(q2, q2, _MM_SHUFFLE(0, 1, 2, 3));
        __m128 term1 = _mm_mul_ps(q1_xxxx, _mm_mul_ps(q2_wzyx, sign_x));

        // y1 * [z2, w2, -x2, -y2]
        static const __m128 sign_y = _mm_set_ps(-1.0f, -1.0f, 1.0f, 1.0f);
        __m128 q2_zwxy = _mm_shuffle_ps(q2, q2, _MM_SHUFFLE(1, 0, 3, 2));
        __m128 term2 = _mm_mul_ps(q1_yyyy, _mm_mul_ps(q2_zwxy, sign_y));

        // z1 * [-y2, x2, w2, -z2]
        static const __m128 sign_z = _mm_set_ps(-1.0f, 1.0f, 1.0f, -1.0f);
        __m128 q2_yxwz = _mm_shuffle_ps(q2, q2, _MM_SHUFFLE(2, 3, 0, 1));
        __m128 term3 = _mm_mul_ps(q1_zzzz, _mm_mul_ps(q2_yxwz, sign_z));

        return Quaternion(_mm_add_ps(_mm_add_ps(term0, term1), _mm_add_ps(term2, term3)));
#elif defined(USE_SIMD_APPLE)
        return Quaternion(simd_mul(simd, rhs.simd));
#elif defined(USE_SIMD_NEON)
        const float32x4_t q1 = this->simd;
        const float32x4_t q2 = rhs.simd;

        float x1 = vgetq_lane_f32(q1, 0);
        float y1 = vgetq_lane_f32(q1, 1);
        float z1 = vgetq_lane_f32(q1, 2);
        float w1 = vgetq_lane_f32(q1, 3);

        float x2 = vgetq_lane_f32(q2, 0);
        float y2 = vgetq_lane_f32(q2, 1);
        float z2 = vgetq_lane_f32(q2, 2);
        float w2 = vgetq_lane_f32(q2, 3);

        return Quaternion(
            w1 * x2 + x1 * w2 + y1 * z2 - z1 * y2,
            w1 * y2 - x1 * z2 + y1 * w2 + z1 * x2,
            w1 * z2 + x1 * y2 - y1 * x2 + z1 * w2,
            w1 * w2 - x1 * x2 - y1 * y2 - z1 * z2
        );
#else
        return Quaternion(
            w * rhs.x + x * rhs.w + y * rhs.z - z * rhs.y,
            w * rhs.y - x * rhs.z + y * rhs.w + z * rhs.x,
            w * rhs.z + x * rhs.y - y * rhs.x + z * rhs.w,
            w * rhs.w - x * rhs.x - y * rhs.y - z * rhs.z
        );
#endif
    }

    inline Vector3 Quaternion::operator*(const Vector3& v) const
    {
#if defined(USE_SIMD_APPLE)
        const simd_float3 qv = simd_make_float3(simd.vector); // (x,y,z)
        const float qw = simd.vector.w; // w
        const simd_float3 t = 2.0f * simd_cross(qv, v.m128);
        const simd_float3 vv = v.m128 + qw * t + simd_cross(qv, t);
        return Vector3(vv);
#else
        const Vector3 qv(getX(), getY(), getZ());
        const Vector3 t = qv.cross(v) * 2.0f;
        return v + t * getW() + qv.cross(t);
#endif
    }

    inline Quaternion Quaternion::operator*(const float scalar) const
    {
#if defined(USE_SIMD_SSE)
        return Quaternion(_mm_mul_ps(simd, _mm_set1_ps(scalar)));
#elif defined(USE_SIMD_APPLE)
        return Quaternion(simd_quaternion(simd.vector * scalar));
#elif defined(USE_SIMD_NEON)
        return Quaternion(vmulq_n_f32(simd, scalar));
#else
        return Quaternion(x * scalar, y * scalar, z * scalar, w * scalar);
#endif
    }

    inline Quaternion Quaternion::normalized() const
    {
#if defined(USE_SIMD_SSE)
        __m128 dot = _mm_dp_ps(simd, simd, 0xFF); // Dot product of all 4 parts
        __m128 invSqrt = _mm_rsqrt_ps(dot); // Approximate reciprocal sqrt
        return Quaternion(_mm_mul_ps(simd, invSqrt)); // Normalize
#elif defined(USE_SIMD_APPLE)
        return Quaternion(simd_normalize(simd));
#elif defined(USE_SIMD_NEON)
        float32x4_t mul = vmulq_f32(simd, simd); // Element-wise square
        float sum = vgetq_lane_f32(mul, 0) +
            vgetq_lane_f32(mul, 1) +
            vgetq_lane_f32(mul, 2) +
            vgetq_lane_f32(mul, 3);
        float invLen = 1.0f / std::sqrt(sum);
        float32x4_t norm = vmulq_n_f32(simd, invLen);
        Quaternion result;
        result.simd = norm;
        return result;
#else
        const float len = lengthSquared();
        if (len > 0.0f)
        {
            const float invLen = 1.0f / std::sqrt(len);
            return Quaternion(x * invLen, y * invLen, z * invLen, w * invLen);
        }
        return Quaternion(); // identity
#endif
    }

    inline float Quaternion::lengthSquared() const
    {
#if defined(USE_SIMD_SSE)
        __m128 dot = _mm_dp_ps(simd, simd, 0xFF);
        return _mm_cvtss_f32(dot);
#elif defined(USE_SIMD_APPLE)
        return simd.vector.x * simd.vector.x + simd.vector.y * simd.vector.y + simd.vector.z * simd.vector.z + simd.
            vector.w * simd.vector.w;
#elif defined(USE_SIMD_NEON)
        float32x4_t mul = vmulq_f32(simd, simd);
        return vgetq_lane_f32(mul, 0) + vgetq_lane_f32(mul, 1) + vgetq_lane_f32(mul, 2) + vgetq_lane_f32(mul, 3);
#else
        return x * x + y * y + z * z + w * w;
#endif
    }

    inline float Quaternion::length() const
    {
        return std::sqrt(lengthSquared());
    }

    inline Matrix4 Quaternion::toRotationMatrix() const
    {
#if defined(USE_SIMD_SSE)
        __m128 q = simd;

        // Ensure we're using a unit quaternion (cheap safeguard)
        const float lsq = lengthSquared();
        const float s = (lsq > 0.0f) ? (2.0f / lsq) : 0.0f;

        float qx = _mm_cvtss_f32(q);
        float qy = _mm_cvtss_f32(_mm_shuffle_ps(q, q, _MM_SHUFFLE(1, 1, 1, 1)));
        float qz = _mm_cvtss_f32(_mm_shuffle_ps(q, q, _MM_SHUFFLE(2, 2, 2, 2)));
        float qw = _mm_cvtss_f32(_mm_shuffle_ps(q, q, _MM_SHUFFLE(3, 3, 3, 3)));

        float xx = qx * qx * s, yy = qy * qy * s, zz = qz * qz * s;
        float xy = qx * qy * s, xz = qx * qz * s, yz = qy * qz * s;
        float wx = qw * qx * s, wy = qw * qy * s, wz = qw * qz * s;

        // Build columns as __m128 and assign directly to Matrix4::c[]
        Matrix4 M;
        M.c[0] = _mm_set_ps(0.0f, xz - wy, xy + wz, 1.0f - (yy + zz));
        M.c[1] = _mm_set_ps(0.0f, yz + wx, 1.0f - (xx + zz), xy - wz);
        M.c[2] = _mm_set_ps(0.0f, 1.0f - (xx + yy), yz - wx, xz + wy);
        M.c[3] = _mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f);
        return M;
#elif defined(USE_SIMD_APPLE)
        const float lsq = lengthSquared();
        const float s = (lsq > 0.0f) ? (2.0f / lsq) : 0.0f;

        const float xx = simd.vector.x * simd.vector.x * s, yy = simd.vector.y * simd.vector.y * s, zz = simd.vector.z *
                        simd.vector.z * s;
        const float xy = simd.vector.x * simd.vector.y * s, xz = simd.vector.x * simd.vector.z * s, yz = simd.vector.y *
                        simd.vector.z * s;
        const float wx = simd.vector.w * simd.vector.x * s, wy = simd.vector.w * simd.vector.y * s, wz = simd.vector.w *
                        simd.vector.z * s;

        // columns (column-major: c[col] = {row0, row1, row2, row3})
        const simd_float4 c0 = {1.0f - (yy + zz), (xy + wz), (xz - wy), 0.0f};
        const simd_float4 c1 = {(xy - wz), 1.0f - (xx + zz), (yz + wx), 0.0f};
        const simd_float4 c2 = {(xz + wy), (yz - wx), 1.0f - (xx + yy), 0.0f};
        const simd_float4 c3 = {0.0f, 0.0f, 0.0f, 1.0f};

        return Matrix4(c0, c1, c2, c3);
#elif defined(USE_SIMD_NEON)
        // Ensure we're using a unit quaternion (cheap safeguard)
        const float lsq = lengthSquared();
        const float s = (lsq > 0.0f) ? (2.0f / lsq) : 0.0f;

        float qx = vgetq_lane_f32(simd, 0);
        float qy = vgetq_lane_f32(simd, 1);
        float qz = vgetq_lane_f32(simd, 2);
        float qw = vgetq_lane_f32(simd, 3);

        float xx = qx * qx * s, yy = qy * qy * s, zz = qz * qz * s;
        float xy = qx * qy * s, xz = qx * qz * s, yz = qy * qz * s;
        float wx = qw * qx * s, wy = qw * qy * s, wz = qw * qz * s;

        // Build columns as float arrays and load into NEON vectors
        Matrix4 M;
        float c0[4] = {1.0f - (yy + zz), xy + wz, xz - wy, 0.0f};
        float c1[4] = {xy - wz, 1.0f - (xx + zz), yz + wx, 0.0f};
        float c2[4] = {xz + wy, yz - wx, 1.0f - (xx + yy), 0.0f};
        float c3[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        M.c[0] = vld1q_f32(c0);
        M.c[1] = vld1q_f32(c1);
        M.c[2] = vld1q_f32(c2);
        M.c[3] = vld1q_f32(c3);
        return M;
#else
        // Ensure we're using a unit quaternion (cheap safeguard)
        const float lsq = lengthSquared();
        const float s = (lsq > 0.0f) ? (2.0f / lsq) : 0.0f;

        const float xx = x * x * s;
        const float yy = y * y * s;
        const float zz = z * z * s;
        const float xy = x * y * s;
        const float xz = x * z * s;
        const float yz = y * z * s;
        const float wx = w * x * s;
        const float wy = w * y * s;
        const float wz = w * z * s;

        Matrix4 result;
        result.m[0][0] = 1.0f - (yy + zz);
        result.m[0][1] = xy + wz;
        result.m[0][2] = xz - wy;
        result.m[0][3] = 0.0f;

        result.m[1][0] = xy - wz;
        result.m[1][1] = 1.0f - (xx + zz);
        result.m[1][2] = yz + wx;
        result.m[1][3] = 0.0f;

        result.m[2][0] = xz + wy;
        result.m[2][1] = yz - wx;
        result.m[2][2] = 1.0f - (xx + yy);
        result.m[2][3] = 0.0f;

        result.m[3][0] = 0.0f;
        result.m[3][1] = 0.0f;
        result.m[3][2] = 0.0f;
        result.m[3][3] = 1.0f;
        return result;
#endif
    }

    inline Quaternion Quaternion::fromEulerAngles(const float ax, const float ay, const float az)
    {
        constexpr float halfToRad = 0.5 * DEG_TO_RAD;
        const float ex = ax * halfToRad;
        const float ey = ay * halfToRad;
        const float ez = az * halfToRad;

        const float sx = std::sin(ex);
        const float cx = std::cos(ex);
        const float sy = std::sin(ey);
        const float cy = std::cos(ey);
        const float sz = std::sin(ez);
        const float cz = std::cos(ez);

        return Quaternion(
            sx * cy * cz - cx * sy * sz,
            cx * sy * cz + sx * cy * sz,
            cx * cy * sz - sx * sy * cz,
            cx * cy * cz + sx * sy * sz
        );
    }

    inline Quaternion Quaternion::fromMatrix4(const Matrix4& mat)
    {
        auto m00 = mat.getElement(0, 0);
        auto m01 = mat.getElement(0, 1);
        auto m02 = mat.getElement(0, 2);
        auto m10 = mat.getElement(1, 0);
        auto m11 = mat.getElement(1, 1);
        auto m12 = mat.getElement(1, 2);
        auto m20 = mat.getElement(2, 0);
        auto m21 = mat.getElement(2, 1);
        auto m22 = mat.getElement(2, 2);

        // if negative, the space is inverted so flip X axis to restore right-handedness
        const auto det = m00 * (m11 * m22 - m12 * m21) -
            m01 * (m10 * m22 - m12 * m20) + m02 * (m10 * m21 - m11 * m20);
        if (det < 0) {
            m00 = -m00;
            m01 = -m01;
            m02 = -m02;
        }

        // remove scaling from axis vectors
        auto l = m00 * m00 + m01 * m01 + m02 * m02;
        if (l == 0)
        {
            return Quaternion(0, 0, 0, 1);
        }
        l = 1 / std::sqrt(l);
        m00 *= l;
        m01 *= l;
        m02 *= l;

        l = m10 * m10 + m11 * m11 + m12 * m12;
        if (l == 0)
        {
            return Quaternion(0, 0, 0, 1);
        }
        l = 1 / std::sqrt(l);
        m10 *= l;
        m11 *= l;
        m12 *= l;

        l = m20 * m20 + m21 * m21 + m22 * m22;
        if (l == 0)
        {
            return Quaternion(0, 0, 0, 1);
        }
        l = 1 / std::sqrt(l);
        m20 *= l;
        m21 *= l;
        m22 *= l;

        // https://d3cw3dd2w32x2b.cloudfront.net/wp-content/uploads/2015/01/matrix-to-quat.pdf
        Quaternion q;
        if (m22 < 0) {
            if (m00 > m11) {
                q = Quaternion(1 + m00 - m11 - m22, m01 + m10, m20 + m02, m12 - m21);
            } else {
                q = Quaternion(m01 + m10, 1 - m00 + m11 - m22, m12 + m21, m20 - m02);
            }
        } else {
            if (m00 < -m11) {
                q = Quaternion(m20 + m02, m12 + m21, 1 - m00 - m11 + m22, m01 - m10);
            } else {
                q = Quaternion(m12 - m21, m20 - m02, m01 - m10, 1 + m00 + m11 + m22);
            }
        }

        // Instead of scaling by 0.5 / Math.sqrt(t) (to match the original implementation),
        // instead we normalize the result. It costs 3 more adds and muls, but we get
        // a stable result and in some cases normalization is required anyway, see
        // https://github.com/blender/blender/blob/v4.1.1/source/blender/blenlib/intern/math_rotation.c#L368
        return q * (1.0 / q.length());
    }

    inline Quaternion Quaternion::invert() const
    {
        return conjugate().normalized();
    }
}
