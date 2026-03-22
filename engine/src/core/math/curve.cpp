// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
#include "curve.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "curveEvaluator.h"

namespace visutwin::canvas
{
    Curve::Curve() : _eval(std::make_unique<CurveEvaluator>(this))
    {
    }

    Curve::Curve(const std::vector<float>& data) : _eval(std::make_unique<CurveEvaluator>(this))
    {
        for (size_t i = 0; i + 1 < data.size(); i += 2) {
            keys.emplace_back(data[i], data[i + 1]);
        }

        sort();
    }

    Curve::Curve(const Curve& other)
        : keys(other.keys), type(other.type), tension(other.tension), _eval(std::make_unique<CurveEvaluator>(this))
    {
    }

    Curve& Curve::operator=(const Curve& other)
    {
        if (this == &other) {
            return *this;
        }

        keys = other.keys;
        type = other.type;
        tension = other.tension;
        _eval = std::make_unique<CurveEvaluator>(this);
        return *this;
    }

    Curve::Curve(Curve&& other) noexcept
        : keys(std::move(other.keys)), type(other.type), tension(other.tension), _eval(std::make_unique<CurveEvaluator>(this))
    {
    }

    Curve& Curve::operator=(Curve&& other) noexcept
    {
        if (this == &other) {
            return *this;
        }

        keys = std::move(other.keys);
        type = other.type;
        tension = other.tension;
        _eval = std::make_unique<CurveEvaluator>(this);
        return *this;
    }

    Curve::~Curve() = default;

    std::pair<float, float> Curve::add(const float time, const float value)
    {
        const auto key = std::make_pair(time, value);
        const size_t len = keys.size();
        size_t i = 0;

        for (; i < len; i++) {
            if (keys[i].first > time) {
                break;
            }
        }

        keys.insert(keys.begin() + static_cast<std::ptrdiff_t>(i), key);
        return key;
    }

    std::pair<float, float> Curve::get(const size_t index) const
    {
        return keys[index];
    }

    void Curve::sort()
    {
        std::sort(keys.begin(), keys.end(), [](const auto& a, const auto& b) {
            return a.first < b.first;
        });
    }

    float Curve::value(const float time)
    {
        return _eval->evaluate(time, true);
    }

    std::pair<float, float> Curve::closest(const float time) const
    {
        const size_t len = keys.size();
        if (!len) {
            return {0.0f, 0.0f};
        }

        float min = 2.0f;
        std::pair<float, float> result = keys.front();

        for (size_t i = 0; i < len; i++) {
            const float diff = std::abs(time - keys[i].first);
            if (min >= diff) {
                min = diff;
                result = keys[i];
            } else {
                break;
            }
        }

        return result;
    }

    Curve Curve::clone() const
    {
        Curve result;
        result.keys = keys;
        result.type = type;
        result.tension = tension;
        return result;
    }

    std::vector<float> Curve::quantize(size_t precision)
    {
        precision = std::max<size_t>(precision, 2);

        std::vector<float> values(precision, 0.0f);
        const float step = 1.0f / static_cast<float>(precision - 1);

        values[0] = _eval->evaluate(0.0f, true);
        for (size_t i = 1; i < precision; ++i) {
            values[i] = _eval->evaluate(step * static_cast<float>(i));
        }

        return values;
    }

    std::vector<float> Curve::quantizeClamped(const size_t precision, const float min, const float max)
    {
        auto result = quantize(precision);
        for (auto& v : result) {
            v = std::min(max, std::max(min, v));
        }
        return result;
    }
}
