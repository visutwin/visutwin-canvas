// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Curve::closest and the CurveType numbers, as upstream's curve.test.mjs has them since
// 2.23.0-beta.20 (#9543, #9544).
//
// closest() started its search at a distance of 2 and fell back to the FIRST key, so a
// curve whose first key sat more than 2 from the time answered with that first key
// even when another key matched exactly; and without clamping, a time far beyond the
// curve rounds every key to one distance and the tie-break walks to the last key. The
// type numbers are upstream's (SPLINE 4, STEP 5) so curve data authored there reads
// the same here; they used to be 2 and 3.

#include <cmath>
#include <iostream>
#include <limits>
#include <string>
#include <utility>

#include "core/math/curve.h"

using namespace visutwin::canvas;

namespace
{
    int failures = 0;

    void check(const bool condition, const std::string& what)
    {
        std::cout << (condition ? "  ok   " : "  FAIL ") << what << '\n';
        if (!condition) {
            ++failures;
        }
    }

    bool is(const Curve& c, const float time, const float keyTime, const float keyValue)
    {
        const auto key = c.closest(time);
        return key && key->first == keyTime && key->second == keyValue;
    }
}

int main()
{
    std::cout << std::unitbuf;
    constexpr float inf = std::numeric_limits<float>::infinity();

    std::cout << "closest\n";
    {
        const Curve c({0.0f, 1.0f, 0.5f, 2.0f, 1.0f, 3.0f});
        check(is(c, 0.6f, 0.5f, 2.0f) && is(c, 0.2f, 0.0f, 1.0f) && is(c, 0.9f, 1.0f, 3.0f),
            "returns the key closest to the time");
    }
    {
        const Curve c({0.0f, 0.0f, 10.0f, 1.0f});
        check(is(c, 5.5f, 10.0f, 1.0f) && is(c, 9.0f, 10.0f, 1.0f) && is(c, -5.0f, 0.0f, 0.0f),
            "finds the nearest key outside the 0-to-1 range (#9543)");
    }
    {
        const Curve c({0.0f, 0.0f, 1e6f, 1.0f, 2e6f, 2.0f});
        check(is(c, 2e5f, 0.0f, 0.0f) && is(c, 1.2e6f, 1e6f, 1.0f) && is(c, 1.9e6f, 2e6f, 2.0f),
            "however far apart the keys are");
    }
    {
        const Curve c({0.0f, 0.0f, 1.0f, 1.0f, 2.0f, 2.0f});
        check(is(c, -1e6f, 0.0f, 0.0f) && is(c, 1e6f, 2.0f, 2.0f), "a time beyond the curve gets the key at that end");
        check(is(c, -inf, 0.0f, 0.0f) && is(c, inf, 2.0f, 2.0f), "at infinity too (#9544)");
        check(is(c, -1e17f, 0.0f, 0.0f), "and far enough out that every key rounds to one distance");
    }
    {
        check(is(Curve({0.0f, 0.0f, 1.0f, 1.0f}), 0.5f, 1.0f, 1.0f), "a tie goes to the later key");
        const Curve c({0.0f, 1.0f, 0.0f, 2.0f, 1.0f, 3.0f});
        check(is(c, 0.0f, 0.0f, 2.0f) && is(c, -1.0f, 0.0f, 2.0f) && is(c, -inf, 0.0f, 2.0f),
            "keys that share a time: the later one, from any time");
    }
    check(!Curve().closest(5.0f).has_value(), "an empty curve has no closest key");

    std::cout << "\ntype numbers (upstream's)\n";
    check(CURVE_LINEAR == 0 && CURVE_SMOOTHSTEP == 1 && CURVE_SPLINE == 4 && CURVE_STEP == 5,
        "LINEAR 0, SMOOTHSTEP 1, SPLINE 4, STEP 5");
    {
        Curve step({0.0f, 0.0f, 1.0f, 1.0f});
        step.type = CURVE_STEP;
        Curve linear({0.0f, 0.0f, 1.0f, 1.0f});
        linear.type = CURVE_LINEAR;
        check(step.value(0.5f) == 0.0f && std::abs(linear.value(0.5f) - 0.5f) < 1e-6f,
            "and the evaluator still dispatches on them by name");
    }

    std::cout << (failures == 0 ? "\nAll curve tests passed\n" : "\nCurve tests FAILED\n");
    return failures == 0 ? 0 : 1;
}
