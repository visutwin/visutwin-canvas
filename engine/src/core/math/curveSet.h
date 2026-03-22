// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
#pragma once

#include <cstddef>
#include <vector>

#include "curve.h"

namespace visutwin::canvas
{
    class CurveSet
    {
    public:
        std::vector<Curve> curves;

        CurveSet();
        explicit CurveSet(size_t numCurves);
        explicit CurveSet(const std::vector<std::vector<float>>& keys);

        [[nodiscard]] size_t length() const { return curves.size(); }

        void setType(CurveType value);
        [[nodiscard]] CurveType type() const { return _type; }

        Curve& get(size_t index);
        const Curve& get(size_t index) const;

        std::vector<float> value(float time, std::vector<float> result = {});

        [[nodiscard]] CurveSet clone() const;

        std::vector<float> quantize(size_t precision);
        std::vector<float> quantizeClamped(size_t precision, float min, float max);

    private:
        CurveType _type = CURVE_SMOOTHSTEP;
    };
}
