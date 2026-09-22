// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
#include "animTrack.h"

#include <algorithm>
#include <cmath>

namespace visutwin::canvas
{
    namespace
    {
        // The four cubic Hermite basis weights at t, the same polynomials AnimTrack::hermite
        // evaluates per component, computed once so a whole Vector3 / Quaternion can be blended.
        struct HermiteBasis
        {
            float h00, h10, h01, h11;

            explicit HermiteBasis(const float t)
            {
                const float t2 = t * t;
                const float t3 = t2 * t;
                h00 = 2.0f * t3 - 3.0f * t2 + 1.0f;
                h10 = t3 - 2.0f * t2 + t;
                h01 = -2.0f * t3 + 3.0f * t2;
                h11 = t3 - t2;
            }

            template <typename T>
            [[nodiscard]] T blend(const T& p0, const T& m0, const T& p1, const T& m1) const
            {
                return p0 * h00 + m0 * h10 + p1 * h01 + m1 * h11;
            }
        };
    }

    AnimTrack::AnimTrack(std::string name, const float duration) : _name(std::move(name)), _duration(duration)
    {
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
                    transform.position = Vector3::load(v);
                    transform.hasPosition = true;
                } else if (curve.propertyPath == "localRotation") {
                    transform.rotation = Quaternion::load(v);
                    transform.hasRotation = true;
                } else if (curve.propertyPath == "localScale") {
                    transform.scale = Vector3::load(v);
                    transform.hasScale = true;
                } else if (curve.propertyPath == "weights") {
                    transform.weights.assign(v, v + comp);
                    transform.hasWeights = true;
                }

            } else if (curve.interpolation == AnimInterpolation::LINEAR) {
                const size_t i1 = std::min(i0 + 1, keyCount - 1);
                const float* v0 = &output.data[i0 * static_cast<size_t>(comp)];
                const float* v1 = &output.data[i1 * static_cast<size_t>(comp)];

                if (curve.propertyPath == "localPosition") {
                    transform.position = Vector3::lerp(Vector3::load(v0), Vector3::load(v1), alpha);
                    transform.hasPosition = true;
                } else if (curve.propertyPath == "localRotation") {
                    transform.rotation = Quaternion::slerp(Quaternion::load(v0), Quaternion::load(v1), alpha);
                    transform.hasRotation = true;
                } else if (curve.propertyPath == "localScale") {
                    transform.scale = Vector3::lerp(Vector3::load(v0), Vector3::load(v1), alpha);
                    transform.hasScale = true;
                } else if (curve.propertyPath == "weights") {
                    transform.weights.resize(static_cast<size_t>(comp));
                    for (int c = 0; c < comp; ++c) {
                        transform.weights[static_cast<size_t>(c)] = v0[c] + (v1[c] - v0[c]) * alpha;
                    }
                    transform.hasWeights = true;
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

                // Hermite spline. Weights channels can have arbitrarily many components (one per
                // morph target); transform channels use at most 4.
                if (curve.propertyPath == "weights") {
                    transform.weights.resize(static_cast<size_t>(comp));
                    for (int c = 0; c < comp; ++c) {
                        transform.weights[static_cast<size_t>(c)] =
                            hermite(alpha, val0[c], outTan0[c] * timeDelta, val1[c], inTan1[c] * timeDelta);
                    }
                    transform.hasWeights = true;
                    continue;
                }
                // Transform channels blend the whole vector with the basis weights.
                const HermiteBasis basis(alpha);
                const auto blendVector3 = [&] {
                    return basis.blend(Vector3::load(val0), Vector3::load(outTan0) * timeDelta,
                        Vector3::load(val1), Vector3::load(inTan1) * timeDelta);
                };

                if (curve.propertyPath == "localPosition") {
                    transform.position = blendVector3();
                    transform.hasPosition = true;
                } else if (curve.propertyPath == "localRotation") {
                    transform.rotation = basis.blend(Quaternion::load(val0), Quaternion::load(outTan0) * timeDelta,
                        Quaternion::load(val1), Quaternion::load(inTan1) * timeDelta).normalized();
                    transform.hasRotation = true;
                } else if (curve.propertyPath == "localScale") {
                    transform.scale = blendVector3();
                    transform.hasScale = true;
                }
            }
        }
    }
}
