// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Contracts the four SIMD math backends (scalar, SSE, Apple, NEON) must all satisfy.
// Each was chosen because one backend broke it while the others did not, which is
// exactly the kind of defect no single-architecture test run catches:
//
//  - the SSE horizontal sums in Vector2::dot and Vector4::planeNormalize folded in
//    the wrong lane, doubling the former and dropping z*z from the latter;
//  - Matrix4::inverse and Quaternion::normalized returned NaN for degenerate input on
//    the Apple backend, where every other backend returns identity.
//
// IMPORTANT: this exercises whichever backend the build selected (see
// core/math/defines.h), so on Apple silicon it does NOT cover the SSE arithmetic.
// The SSE fixes are only really guarded by running this on an x86-64 target with
// SSE4.1 — which is the argument for an x86 build in CI.

#include <cmath>
#include <iostream>

#include "core/math/matrix4.h"
#include "core/math/quaternion.h"
#include "core/math/vector2.h"
#include "core/math/vector3.h"
#include "core/math/vector4.h"

using namespace visutwin::canvas;

namespace
{
    int failures = 0;

    void check(const bool condition, const char* what)
    {
        std::cout << (condition ? "  ok   " : "  FAIL ") << what << '\n';
        if (!condition) {
            ++failures;
        }
    }

    bool near(const float a, const float b, const float eps = 1e-4f)
    {
        return std::fabs(a - b) <= eps;
    }

    bool isIdentity(const Matrix4& m)
    {
        for (int col = 0; col < 4; ++col) {
            for (int row = 0; row < 4; ++row) {
                if (!near(m.getElement(col, row), col == row ? 1.0f : 0.0f)) {
                    return false;
                }
            }
        }
        return true;
    }
}

int main()
{
    std::cout << "simd math contracts\n";

    // ── Horizontal sums ──────────────────────────────────────────────────────
    // 3-4-5 triangle: the classic case, and the one the doubled SSE reduction got
    // wrong by a factor of sqrt(2).
    check(near(Vector2(3.0f, 4.0f).dot(Vector2(3.0f, 4.0f)), 25.0f), "Vector2 dot with itself is 25");
    check(near(Vector2(3.0f, 4.0f).length(), 5.0f), "Vector2(3,4) length is 5");
    check(near(Vector2(1.0f, 0.0f).dot(Vector2(0.0f, 1.0f)), 0.0f), "Vector2 perpendicular dot is 0");

    check(near(Vector3(3.0f, 4.0f, 0.0f).length(), 5.0f), "Vector3(3,4,0) length is 5");
    check(near(Vector3(1.0f, 2.0f, 2.0f).length(), 3.0f), "Vector3(1,2,2) length is 3");
    check(near(Vector4(1.0f, 2.0f, 3.0f, 4.0f).dot(Vector4(1.0f, 1.0f, 1.0f, 1.0f)), 10.0f),
        "Vector4 dot sums all four lanes");

    // ── Plane normalisation ──────────────────────────────────────────────────
    // A plane whose normal lies along Z is the case the broken reduction destroyed:
    // it summed 2*(x*x + y*y) and dropped z*z, so this normal read as zero length and
    // the plane collapsed to (0,0,0,0) — a frustum plane that culls nothing. An
    // axis-aligned camera produces exactly this near plane.
    {
        const Vector4 p = Vector4(0.0f, 0.0f, 2.0f, -8.0f).planeNormalize();
        check(near(p.getX(), 0.0f) && near(p.getY(), 0.0f) && near(p.getZ(), 1.0f) && near(p.getW(), -4.0f),
            "plane (0,0,2,-8) normalises to (0,0,1,-4)");
    }
    {
        // A normal in the XY plane: the case the broken code got right by accident,
        // since there z*z was zero anyway. It must stay right.
        const Vector4 p = Vector4(3.0f, 4.0f, 0.0f, -10.0f).planeNormalize();
        check(near(p.getX(), 0.6f) && near(p.getY(), 0.8f) && near(p.getZ(), 0.0f) && near(p.getW(), -2.0f),
            "plane (3,4,0,-10) normalises to (0.6,0.8,0,-2)");
    }
    {
        const Vector4 p = Vector4(1.0f, 2.0f, 2.0f, -9.0f).planeNormalize();
        const float len = std::sqrt(p.getX() * p.getX() + p.getY() * p.getY() + p.getZ() * p.getZ());
        check(near(len, 1.0f), "a general plane normal comes out unit length");
        check(near(p.getW(), -3.0f), "and its distance is scaled by the same factor");
    }

    // ── Degenerate matrix inverse ────────────────────────────────────────────
    // A zero component in a node's local scale is the ordinary way to reach a
    // singular world transform. Every backend must answer identity, not NaN: the
    // value flows into the view matrix and from there into every frustum plane.
    {
        const Matrix4 flattened = Matrix4::trs(Vector3(1.0f, 2.0f, 3.0f), Quaternion(),
            Vector3(1.0f, 0.0f, 1.0f));
        const Matrix4 inv = flattened.inverse();
        check(isIdentity(inv), "inverse of a zero-scale (singular) matrix is identity, not NaN");
        check(std::isfinite(inv.getElement(0, 0)), "and carries no infinities");
    }
    {
        // The non-singular case still has to invert properly.
        const Matrix4 m = Matrix4::trs(Vector3(5.0f, -2.0f, 1.0f),
            Quaternion::fromEulerAngles(0.0f, 90.0f, 0.0f), Vector3(2.0f));
        check(isIdentity(m * m.inverse()), "a well-formed matrix times its inverse is identity");
    }

    // ── Degenerate quaternion ────────────────────────────────────────────────
    {
        const Quaternion zero(0.0f, 0.0f, 0.0f, 0.0f);
        const Quaternion n = zero.normalized();
        check(near(n.getX(), 0.0f) && near(n.getY(), 0.0f) && near(n.getZ(), 0.0f) && near(n.getW(), 1.0f),
            "a zero quaternion normalises to identity, not NaN");
        const Quaternion i = zero.invert();
        check(std::isfinite(i.getX()) && std::isfinite(i.getW()), "invert of a zero quaternion is finite");
    }
    {
        const Quaternion q(1.0f, 2.0f, 3.0f, 4.0f);
        const Quaternion n = q.normalized();
        const float len = std::sqrt(n.getX() * n.getX() + n.getY() * n.getY() +
                                    n.getZ() * n.getZ() + n.getW() * n.getW());
        check(near(len, 1.0f, 1e-5f), "a real quaternion normalises to unit length");
    }

    if (failures == 0) {
        std::cout << "simd math contracts: all checks passed\n";
        return 0;
    }
    std::cout << "simd math contracts: " << failures << " check(s) FAILED\n";
    return 1;
}
