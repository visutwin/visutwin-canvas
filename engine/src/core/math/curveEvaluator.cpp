// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
#include "curveEvaluator.h"

#include <cmath>

#include "curve.h"

namespace visutwin::canvas
{
    CurveEvaluator::CurveEvaluator(Curve* curve, const float time) : _curve(curve)
    {
        reset(time);
    }

    float CurveEvaluator::evaluate(const float time, const bool forceReset)
    {
        if (!_curve) {
            return 0.0f;
        }

        if (forceReset || time < _left || time >= _right) {
            reset(time);
        }

        if (_curve->type == CURVE_STEP) {
            return _p0;
        }

        const float t = (_recip == 0.0f) ? 0.0f : (time - _left) * _recip;
        if (_curve->type == CURVE_LINEAR) {
            return _p0 + (_p1 - _p0) * t;
        }

        if (_curve->type == CURVE_SMOOTHSTEP) {
            const float s = t * t * (3.0f - 2.0f * t);
            return _p0 + (_p1 - _p0) * s;
        }

        return evaluateHermite(_p0, _p1, _m0, _m1, t);
    }

    void CurveEvaluator::reset(const float time)
    {
        if (!_curve) {
            _left = -std::numeric_limits<float>::infinity();
            _right = std::numeric_limits<float>::infinity();
            _recip = 0.0f;
            _p0 = _p1 = _m0 = _m1 = 0.0f;
            return;
        }

        const auto& keys = _curve->keys;
        const size_t len = keys.size();

        if (len == 0) {
            _left = -std::numeric_limits<float>::infinity();
            _right = std::numeric_limits<float>::infinity();
            _recip = 0.0f;
            _p0 = _p1 = _m0 = _m1 = 0.0f;
            return;
        }

        if (time < keys[0].first) {
            _left = -std::numeric_limits<float>::infinity();
            _right = keys[0].first;
            _recip = 0.0f;
            _p0 = _p1 = keys[0].second;
            _m0 = _m1 = 0.0f;
            return;
        }

        if (time >= keys[len - 1].first) {
            _left = keys[len - 1].first;
            _right = std::numeric_limits<float>::infinity();
            _recip = 0.0f;
            _p0 = _p1 = keys[len - 1].second;
            _m0 = _m1 = 0.0f;
            return;
        }

        size_t index = 0;
        while (time >= keys[index + 1].first) {
            index++;
        }

        _left = keys[index].first;
        _right = keys[index + 1].first;
        const float diff = 1.0f / (_right - _left);
        _recip = std::isfinite(diff) ? diff : 0.0f;
        _p0 = keys[index].second;
        _p1 = keys[index + 1].second;

        if (_curve->type == CURVE_SPLINE) {
            calcTangents(keys, index);
        } else {
            _m0 = _m1 = 0.0f;
        }
    }

    void CurveEvaluator::calcTangents(const std::vector<std::pair<float, float>>& keys, const size_t index)
    {
        std::pair<float, float> a;
        const std::pair<float, float>& b = keys[index];
        const std::pair<float, float>& c = keys[index + 1];
        std::pair<float, float> d;

        if (index == 0) {
            a = {
                keys[0].first + (keys[0].first - keys[1].first),
                keys[0].second + (keys[0].second - keys[1].second)
            };
        } else {
            a = keys[index - 1];
        }

        if (index == keys.size() - 2) {
            d = {
                keys[index + 1].first + (keys[index + 1].first - keys[index].first),
                keys[index + 1].second + (keys[index + 1].second - keys[index].second)
            };
        } else {
            d = keys[index + 2];
        }

        const float s1_ = 2.0f * (c.first - b.first) / (c.first - a.first);
        const float s2_ = 2.0f * (c.first - b.first) / (d.first - b.first);

        _m0 = _curve->tension * (std::isfinite(s1_) ? s1_ : 0.0f) * (c.second - a.second);
        _m1 = _curve->tension * (std::isfinite(s2_) ? s2_ : 0.0f) * (d.second - b.second);
    }

    float CurveEvaluator::evaluateHermite(const float p0, const float p1, const float m0, const float m1, const float t)
    {
        const float t2 = t * t;
        const float twot = t + t;
        const float omt = 1.0f - t;
        const float omt2 = omt * omt;

        return p0 * ((1.0f + twot) * omt2) +
               m0 * (t * omt2) +
               p1 * (t2 * (3.0f - twot)) +
               m1 * (t2 * (t - 1.0f));
    }
}
