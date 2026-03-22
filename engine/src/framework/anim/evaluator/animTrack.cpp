// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
#include "animTrack.h"

#include <algorithm>
#include <cmath>

namespace visutwin::canvas
{
    AnimTrack::AnimTrack(std::string name, const float duration) : _name(std::move(name)), _duration(duration)
    {
    }

    Vector3 AnimTrack::lerpVec3(const Vector3& a, const Vector3& b, const float alpha)
    {
        return a + (b - a) * alpha;
    }

    Quaternion AnimTrack::slerpQuat(const Quaternion& a, const Quaternion& b, const float alpha)
    {
        float ax = a.getX();
        float ay = a.getY();
        float az = a.getZ();
        float aw = a.getW();

        float bx = b.getX();
        float by = b.getY();
        float bz = b.getZ();
        float bw = b.getW();

        float dot = ax * bx + ay * by + az * bz + aw * bw;
        if (dot < 0.0f) {
            bx = -bx;
            by = -by;
            bz = -bz;
            bw = -bw;
            dot = -dot;
        }

        constexpr float epsilon = 1e-6f;
        float scale0 = 1.0f - alpha;
        float scale1 = alpha;

        if ((1.0f - dot) > epsilon) {
            const float theta = std::acos(std::clamp(dot, -1.0f, 1.0f));
            const float invSinTheta = 1.0f / std::sin(theta);
            scale0 = std::sin((1.0f - alpha) * theta) * invSinTheta;
            scale1 = std::sin(alpha * theta) * invSinTheta;
        }

        return Quaternion(
            scale0 * ax + scale1 * bx,
            scale0 * ay + scale1 * by,
            scale0 * az + scale1 * bz,
            scale0 * aw + scale1 * bw).normalized();
    }

    float AnimTrack::hermite(const float t, const float p0, const float m0, const float p1, const float m1)
    {
        const float t2 = t * t;
        const float t3 = t2 * t;
        return (2.0f * t3 - 3.0f * t2 + 1.0f) * p0 +
               (t3 - 2.0f * t2 + t) * m0 +
               (-2.0f * t3 + 3.0f * t2) * p1 +
               (t3 - t2) * m1;
    }

    void AnimTrack::eval(float time, std::unordered_map<std::string, AnimTransform>& transforms) const
    {
        if (_curves.empty()) {
            return;
        }

        const float t = std::clamp(time, 0.0f, _duration);

        for (const auto& curve : _curves) {
            if (curve.inputIndex >= _inputs.size() || curve.outputIndex >= _outputs.size()) {
                continue;
            }

            const auto& input = _inputs[curve.inputIndex];
            const auto& output = _outputs[curve.outputIndex];
            if (input.data.empty() || output.data.empty()) {
                continue;
            }

            const size_t keyCount = input.count();
            if (keyCount == 0) {
                continue;
            }

            // Binary search for bracketing keyframe pair.
            size_t i0 = 0;
            float alpha = 0.0f;

            if (t <= input.data[0]) {
                i0 = 0;
                alpha = 0.0f;
            } else if (t >= input.data[keyCount - 1]) {
                i0 = keyCount - 1;
                alpha = 0.0f;
            } else {
                size_t lo = 0, hi = keyCount - 1;
                while (lo + 1 < hi) {
                    const size_t mid = (lo + hi) / 2;
                    if (input.data[mid] <= t) {
                        lo = mid;
                    } else {
                        hi = mid;
                    }
                }
                i0 = lo;
                const float dt = input.data[i0 + 1] - input.data[i0];
                alpha = (dt > 0.0f) ? (t - input.data[i0]) / dt : 0.0f;
            }

            auto& transform = transforms[curve.nodeName];
            const int comp = output.components;

            if (curve.interpolation == AnimInterpolation::STEP) {
                // STEP: use value at i0 directly (no interpolation).
                const float* v = &output.data[i0 * static_cast<size_t>(comp)];

                if (curve.propertyPath == "localPosition") {
                    transform.position = Vector3(v[0], v[1], v[2]);
                    transform.hasPosition = true;
                } else if (curve.propertyPath == "localRotation") {
                    transform.rotation = Quaternion(v[0], v[1], v[2], v[3]);
                    transform.hasRotation = true;
                } else if (curve.propertyPath == "localScale") {
                    transform.scale = Vector3(v[0], v[1], v[2]);
                    transform.hasScale = true;
                }

            } else if (curve.interpolation == AnimInterpolation::LINEAR) {
                const size_t i1 = std::min(i0 + 1, keyCount - 1);
                const float* v0 = &output.data[i0 * static_cast<size_t>(comp)];
                const float* v1 = &output.data[i1 * static_cast<size_t>(comp)];

                if (curve.propertyPath == "localPosition") {
                    transform.position = lerpVec3(
                        Vector3(v0[0], v0[1], v0[2]),
                        Vector3(v1[0], v1[1], v1[2]), alpha);
                    transform.hasPosition = true;
                } else if (curve.propertyPath == "localRotation") {
                    transform.rotation = slerpQuat(
                        Quaternion(v0[0], v0[1], v0[2], v0[3]),
                        Quaternion(v1[0], v1[1], v1[2], v1[3]), alpha);
                    transform.hasRotation = true;
                } else if (curve.propertyPath == "localScale") {
                    transform.scale = lerpVec3(
                        Vector3(v0[0], v0[1], v0[2]),
                        Vector3(v1[0], v1[1], v1[2]), alpha);
                    transform.hasScale = true;
                }

            } else if (curve.interpolation == AnimInterpolation::CUBIC) {
                // glTF CUBICSPLINE: output has 3 values per keyframe (in-tangent, value, out-tangent).
                // Each group = comp * 3 floats.
                const size_t i1 = std::min(i0 + 1, keyCount - 1);
                const float timeDelta = input.data[std::min(i0 + 1, keyCount - 1)] - input.data[i0];
                const int stride3 = comp * 3;
                const float* g0 = &output.data[i0 * static_cast<size_t>(stride3)];
                const float* g1 = &output.data[i1 * static_cast<size_t>(stride3)];
                const float* val0 = g0 + comp;           // value at i0
                const float* outTan0 = g0 + comp * 2;    // out-tangent at i0
                const float* val1 = g1 + comp;           // value at i1
                const float* inTan1 = g1;                // in-tangent at i1

                // Hermite spline per component.
                float result[4];
                for (int c = 0; c < comp && c < 4; ++c) {
                    result[c] = hermite(alpha, val0[c], outTan0[c] * timeDelta, val1[c], inTan1[c] * timeDelta);
                }

                if (curve.propertyPath == "localPosition") {
                    transform.position = Vector3(result[0], result[1], result[2]);
                    transform.hasPosition = true;
                } else if (curve.propertyPath == "localRotation") {
                    transform.rotation = Quaternion(result[0], result[1], result[2], result[3]).normalized();
                    transform.hasRotation = true;
                } else if (curve.propertyPath == "localScale") {
                    transform.scale = Vector3(result[0], result[1], result[2]);
                    transform.hasScale = true;
                }
            }
        }
    }
}
