// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 02.10.2026
//
// The unit tests' one check, their tolerance comparisons and their exit code.
//
// check() counts every call and prints "  ok   <what>" or "  FAIL <what>"; a suite whose
// checks run in loops of thousands calls quietPasses() first, and then only failures
// print. finish() prints one summary line ("<suite>: <n> passed, <m> failed") and returns
// the process exit code, so every suite reports the same way.
//
// Each tolerance is passed EXPLICITLY, and the comparison is in the name: near() accepts
// a difference EQUAL to the tolerance (<=), nearStrict() does not (<). A suite keeps the
// epsilon and the comparison it was written with.

#pragma once

#include <cmath>
#include <iostream>
#include <string_view>

#include "core/math/vector3.h"

namespace visutwin::canvas::test
{
    namespace detail
    {
        inline int checks = 0;
        inline int failures = 0;
        inline bool printPasses = true;
    }

    /// Print failures only, from here on.
    inline void quietPasses() { detail::printPasses = false; }

    /// Counts the check; prints it (passes only unless quietPasses() was called).
    /// Returns `condition`, so a caller can stop when what follows depends on it.
    inline bool check(const bool condition, const std::string_view what)
    {
        ++detail::checks;
        if (!condition) {
            ++detail::failures;
        }
        if (!condition || detail::printPasses) {
            std::cout << (condition ? "  ok   " : "  FAIL ") << what << '\n';
        }
        return condition;
    }

    /// A check that failed: for a site that only ever reported failures (a loop that
    /// names the offending element), so the pass count stays what it was.
    inline void fail(const std::string_view what) { check(false, what); }

    /// Checks failed so far.
    [[nodiscard]] inline int failureCount() { return detail::failures; }

    /// Prints "<suite>: <n> passed, <m> failed" and returns the exit code (0 when nothing
    /// failed, 1 otherwise).
    inline int finish(const std::string_view suite)
    {
        std::cout << suite << ": " << (detail::checks - detail::failures) << " passed, " << detail::failures
                  << " failed\n";
        return detail::failures == 0 ? 0 : 1;
    }

    /// |a - b| <= eps.
    [[nodiscard]] inline bool near(const float a, const float b, const float eps)
    {
        return std::abs(a - b) <= eps;
    }

    /// |a - b| < eps.
    [[nodiscard]] inline bool nearStrict(const float a, const float b, const float eps)
    {
        return std::abs(a - b) < eps;
    }

    /// Every component within eps (<=).
    [[nodiscard]] inline bool near(const Vector3& a, const Vector3& b, const float eps)
    {
        return near(a.getX(), b.getX(), eps) && near(a.getY(), b.getY(), eps) && near(a.getZ(), b.getZ(), eps);
    }

    /// Every component within eps (<).
    [[nodiscard]] inline bool nearStrict(const Vector3& a, const float x, const float y, const float z,
        const float eps)
    {
        return nearStrict(a.getX(), x, eps) && nearStrict(a.getY(), y, eps) && nearStrict(a.getZ(), z, eps);
    }
}
