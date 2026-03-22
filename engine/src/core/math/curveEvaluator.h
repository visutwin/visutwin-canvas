// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
#pragma once

#include <limits>
#include <vector>

namespace visutwin::canvas
{
    class Curve;

    class CurveEvaluator
    {
    public:
        explicit CurveEvaluator(Curve* curve, float time = 0.0f);

        float evaluate(float time, bool forceReset = false);

    private:
        void reset(float time);
        void calcTangents(const std::vector<std::pair<float, float>>& keys, size_t index);

        static float evaluateHermite(float p0, float p1, float m0, float m1, float t);

        Curve* _curve = nullptr;

        float _left = -std::numeric_limits<float>::infinity();
        float _right = std::numeric_limits<float>::infinity();

        float _recip = 0.0f;
        float _p0 = 0.0f;
        float _p1 = 0.0f;
        float _m0 = 0.0f;
        float _m1 = 0.0f;
    };
}
