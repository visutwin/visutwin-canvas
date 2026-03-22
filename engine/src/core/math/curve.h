// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace visutwin::canvas
{
    class CurveEvaluator;

    enum CurveType : uint8_t
    {
        CURVE_LINEAR = 0,
        CURVE_SMOOTHSTEP = 1,
        CURVE_SPLINE = 2,
        CURVE_STEP = 3
    };

    class Curve
    {
    public:
        std::vector<std::pair<float, float>> keys;
        CurveType type = CURVE_SMOOTHSTEP;
        float tension = 0.5f;

        Curve();
        explicit Curve(const std::vector<float>& data);
        Curve(const Curve& other);
        Curve& operator=(const Curve& other);
        Curve(Curve&& other) noexcept;
        Curve& operator=(Curve&& other) noexcept;
        ~Curve();

        [[nodiscard]] size_t length() const { return keys.size(); }

        std::pair<float, float> add(float time, float value);

        std::pair<float, float> get(size_t index) const;

        void sort();

        [[nodiscard]] float value(float time);

        [[nodiscard]] std::pair<float, float> closest(float time) const;

        [[nodiscard]] Curve clone() const;

        [[nodiscard]] std::vector<float> quantize(size_t precision);

        [[nodiscard]] std::vector<float> quantizeClamped(size_t precision, float min, float max);

    private:
        std::unique_ptr<CurveEvaluator> _eval;
    };
}
