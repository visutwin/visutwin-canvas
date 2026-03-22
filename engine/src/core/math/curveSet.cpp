// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
#include "curveSet.h"

#include <algorithm>

namespace visutwin::canvas
{
    CurveSet::CurveSet()
    {
        curves.emplace_back();
    }

    CurveSet::CurveSet(const size_t numCurves)
    {
        curves.reserve(numCurves);
        for (size_t i = 0; i < numCurves; ++i) {
            curves.emplace_back();
        }
    }

    CurveSet::CurveSet(const std::vector<std::vector<float>>& keys)
    {
        if (keys.empty()) {
            curves.emplace_back();
            return;
        }

        curves.reserve(keys.size());
        for (const auto& keyData : keys) {
            curves.emplace_back(keyData);
        }
    }

    void CurveSet::setType(const CurveType value)
    {
        _type = value;
        for (auto& curve : curves) {
            curve.type = value;
        }
    }

    Curve& CurveSet::get(const size_t index)
    {
        return curves[index];
    }

    const Curve& CurveSet::get(const size_t index) const
    {
        return curves[index];
    }

    std::vector<float> CurveSet::value(const float time, std::vector<float> result)
    {
        const size_t len = curves.size();
        result.resize(len);

        for (size_t i = 0; i < len; i++) {
            result[i] = curves[i].value(time);
        }

        return result;
    }

    CurveSet CurveSet::clone() const
    {
        CurveSet result;
        result.curves.clear();

        for (const auto& curve : curves) {
            result.curves.push_back(curve.clone());
        }

        result._type = _type;
        return result;
    }

    std::vector<float> CurveSet::quantize(size_t precision)
    {
        precision = std::max<size_t>(precision, 2);

        const size_t numCurves = curves.size();
        std::vector<float> values(precision * numCurves, 0.0f);
        const float step = 1.0f / static_cast<float>(precision - 1);

        for (size_t c = 0; c < numCurves; c++) {
            for (size_t i = 0; i < precision; i++) {
                values[i * numCurves + c] = curves[c].value(step * static_cast<float>(i));
            }
        }

        return values;
    }

    std::vector<float> CurveSet::quantizeClamped(const size_t precision, const float min, const float max)
    {
        auto result = quantize(precision);
        for (auto& v : result) {
            v = std::min(max, std::max(min, v));
        }
        return result;
    }
}
