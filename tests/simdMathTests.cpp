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
#include <cstdlib>
#include <iostream>
#include <string>

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

namespace
{
    // The backend defines.h actually selected. A build that asked for SSE but
    // lacked -msse4.1, or asked for NEON without USE_SIMD_PREFER_NEON reaching
    // the compiler, falls through to another backend silently and still passes —
    // it just tests the wrong code. CI sets VISUTWIN_EXPECT_SIMD_BACKEND so that
    // fall-through fails instead of reading as coverage.
    const char* compiledBackend()
    {
#if defined(USE_SIMD_SSE)
        return "sse";
#elif defined(USE_SIMD_NEON)
        return "neon";
#elif defined(USE_SIMD_APPLE)
        return "apple";
#else
        return "scalar";
#endif
    }
}

int main()
{
    std::cout << "simd math contracts (backend: " << compiledBackend() << ")\n";
    if (const char* expected = std::getenv("VISUTWIN_EXPECT_SIMD_BACKEND");
        expected && *expected && std::string(expected) != compiledBackend()) {
        std::cout << "FAIL expected the " << expected << " backend but this build compiled "
                  << compiledBackend() << "\n";
        return 1;
    }

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

    // Quaternion::dot is the shortest-arc test of every rotation blend and the body of
    // lengthSquared; it must agree with the component sum on every backend.
    {
        const Quaternion p(1.0f, 2.0f, 3.0f, 4.0f);
        const Quaternion q(-2.0f, 0.5f, 1.0f, 2.0f);
        check(near(p.dot(q), -2.0f + 1.0f + 3.0f + 8.0f), "Quaternion dot matches the component sum");
        check(near(p.dot(p), p.lengthSquared()), "Quaternion dot with itself is lengthSquared");
        check(near(p.dot(p), 30.0f), "Quaternion lengthSquared of (1,2,3,4) is 30");
        check(p.dot(Quaternion(-1.0f, -2.0f, -3.0f, -4.0f)) < 0.0f, "the negated quaternion has a negative dot");
        check(near(Quaternion(1.0f, 0.0f, 0.0f, 0.0f).dot(Quaternion(0.0f, 0.0f, 0.0f, 1.0f)), 0.0f),
            "orthogonal quaternions dot to 0");
    }

    // Quaternion::slerp / nlerp: the one interpolation every animation blend uses.
    {
        const float h = std::sqrt(0.5f);
        const Quaternion id(0.0f, 0.0f, 0.0f, 1.0f);
        const Quaternion quarterY(0.0f, h, 0.0f, h);          // 90 degrees about Y
        const Quaternion eighthY(0.0f, std::sin(0.3926991f), 0.0f, std::cos(0.3926991f));   // 45 degrees
        const Quaternion s0 = Quaternion::slerp(id, quarterY, 0.0f);
        const Quaternion s1 = Quaternion::slerp(id, quarterY, 1.0f);
        const Quaternion sh = Quaternion::slerp(id, quarterY, 0.5f);
        const Quaternion nh = Quaternion::nlerp(id, quarterY, 0.5f);
        check(near(s0.dot(id), 1.0f), "slerp at t=0 is the start");
        check(near(s1.dot(quarterY), 1.0f), "slerp at t=1 is the end");
        check(near(sh.dot(eighthY), 1.0f), "slerp midpoint of identity and a quarter turn is the eighth turn");
        check(near(nh.dot(eighthY), 1.0f), "nlerp midpoint agrees with slerp for unit inputs");
        check(near(sh.lengthSquared(), 1.0f) && near(nh.lengthSquared(), 1.0f), "both blends return unit quaternions");
        // q and -q are one rotation: blending toward the negated spelling must not swing the long way.
        const Quaternion minusQuarter = quarterY * -1.0f;
        check(near(std::fabs(Quaternion::slerp(id, minusQuarter, 0.5f).dot(eighthY)), 1.0f),
            "slerp takes the shorter arc when the end is negated");
        check(near(std::fabs(Quaternion::nlerp(id, minusQuarter, 0.5f).dot(eighthY)), 1.0f),
            "nlerp takes the shorter arc when the end is negated");
        check(near(Quaternion::slerp(quarterY, quarterY, 0.3f).dot(quarterY), 1.0f),
            "slerp of a quaternion with itself is itself (parallel fallback)");
        const Quaternion sum = Quaternion(1.0f, 2.0f, 3.0f, 4.0f) + Quaternion(4.0f, 3.0f, 2.0f, 1.0f);
        check(near(sum.getX(), 5.0f) && near(sum.getY(), 5.0f) && near(sum.getZ(), 5.0f) && near(sum.getW(), 5.0f),
            "Quaternion operator+ is the component sum");
    }

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

    // ── Component-wise Vector3 operations ────────────────────────────────────
    // min/max must answer std::min / std::max on every backend, NaN included: the
    // native instructions disagree (NEON's vminq propagates NaN, Apple's simd_min
    // drops it), and an AABB accumulated over a NaN position must not depend on
    // which backend the build selected.
    {
        const Vector3 a(1.0f, -2.0f, 3.0f);
        const Vector3 b(0.5f, 4.0f, 3.0f);
        const Vector3 lo = Vector3::min(a, b);
        const Vector3 hi = Vector3::max(a, b);
        check(lo.getX() == 0.5f && lo.getY() == -2.0f && lo.getZ() == 3.0f, "Vector3::min is component-wise");
        check(hi.getX() == 1.0f && hi.getY() == 4.0f && hi.getZ() == 3.0f, "Vector3::max is component-wise");

        const float nan = std::nanf("");
        const Vector3 withNan(nan, 1.0f, nan);
        const Vector3 minKeepsA = Vector3::min(a, withNan);
        check(minKeepsA.getX() == 1.0f && minKeepsA.getY() == -2.0f && minKeepsA.getZ() == 3.0f,
            "Vector3::min(a, NaN) keeps a, as std::min does");
        const Vector3 maxKeepsA = Vector3::max(a, withNan);
        check(maxKeepsA.getX() == 1.0f && maxKeepsA.getY() == 1.0f && maxKeepsA.getZ() == 3.0f,
            "Vector3::max(a, NaN) keeps a, as std::max does");
        check(std::isnan(Vector3::min(withNan, a).getX()) && std::isnan(Vector3::max(withNan, a).getX()),
            "a NaN in the FIRST argument stays, as std::min / std::max leave it");

        const Vector3 c = Vector3::clamp(Vector3(-5.0f, 0.5f, 5.0f), Vector3(0.0f), Vector3(1.0f));
        check(c.getX() == 0.0f && c.getY() == 0.5f && c.getZ() == 1.0f, "Vector3::clamp");

        const Vector3 absd = Vector3(-1.5f, 2.0f, -0.0f).abs();
        check(absd.getX() == 1.5f && absd.getY() == 2.0f && absd.getZ() == 0.0f && !std::signbit(absd.getZ()),
            "Vector3::abs clears every sign bit, -0 included");

        const Vector3 fl = Vector3(1.5f, -1.5f, -2.0f).floor();
        check(fl.getX() == 1.0f && fl.getY() == -2.0f && fl.getZ() == -2.0f, "Vector3::floor rounds toward -inf");

        const Vector3 q = Vector3(1.0f, 6.0f, -9.0f) / Vector3(2.0f, 3.0f, 3.0f);
        check(q.getX() == 0.5f && q.getY() == 2.0f && q.getZ() == -3.0f, "Vector3 component-wise division");
        const Vector3 qs = Vector3(3.0f, 6.0f, 9.0f) / 3.0f;
        check(qs.getX() == 1.0f && qs.getY() == 2.0f && qs.getZ() == 3.0f, "Vector3 division by a scalar");
        check(std::isinf((Vector3(1.0f) / Vector3(0.0f, 1.0f, 1.0f)).getX()), "a zero divisor gives inf");
        // The unused lane of the divisor is zero on SSE and NEON; 0/0 there must not
        // leak into the 3-component length.
        check(near((Vector3(3.0f, 4.0f, 0.0f) / Vector3(1.0f)).length(), 5.0f),
            "division leaves the fourth lane clean for length()");

        check(a.minComponent() == -2.0f && a.maxComponent() == 3.0f, "Vector3 min/maxComponent");
        check(a[0] == 1.0f && a[1] == -2.0f && a[2] == 3.0f, "Vector3::operator[] reads x, y, z");

        float buffer[4] = {9.0f, 9.0f, 9.0f, 9.0f};
        a.store(buffer);
        check(buffer[0] == 1.0f && buffer[1] == -2.0f && buffer[2] == 3.0f && buffer[3] == 9.0f,
            "Vector3::store writes exactly three floats");
        const Vector3 loaded = Vector3::load(buffer);
        check(loaded.getX() == 1.0f && loaded.getY() == -2.0f && loaded.getZ() == 3.0f, "Vector3::load");
    }

    // ── Vector4 additions ────────────────────────────────────────────────────
    {
        const Vector4 a(1.0f, 2.0f, 3.0f, 4.0f);
        check(a == Vector4(1.0f, 2.0f, 3.0f, 4.0f), "Vector4 == on equal vectors");
        check(a != Vector4(1.0f, 2.0f, 3.0f, 4.5f), "Vector4 != when only w differs");
        check(Vector4(0.0f, 0.0f, 0.0f, 0.0f) == Vector4(-0.0f, 0.0f, 0.0f, 0.0f), "Vector4 == treats -0 as +0");
        const float nan = std::nanf("");
        check(!(Vector4(nan, 0.0f, 0.0f, 0.0f) == Vector4(nan, 0.0f, 0.0f, 0.0f)), "Vector4 NaN equals nothing");

        const Vector4 l = Vector4::lerp(a, Vector4(3.0f, 2.0f, 1.0f, 0.0f), 0.5f);
        check(l == Vector4(2.0f, 2.0f, 2.0f, 2.0f), "Vector4::lerp midpoint");
        check(a * Vector4(2.0f, 0.5f, -1.0f, 0.0f) == Vector4(2.0f, 1.0f, -3.0f, 0.0f), "Vector4 component product");
        check(Vector4::min(a, Vector4(0.0f, 5.0f, 3.0f, 9.0f)) == Vector4(0.0f, 2.0f, 3.0f, 4.0f), "Vector4::min");
        check(Vector4::max(a, Vector4(0.0f, 5.0f, 3.0f, 9.0f)) == Vector4(1.0f, 5.0f, 3.0f, 9.0f), "Vector4::max");
        check(Vector4::min(a, Vector4(nan, nan, nan, nan)) == a, "Vector4::min(a, NaN) keeps a");
        check(Vector4(-1.0f, 2.0f, -3.0f, -0.5f).abs() == Vector4(1.0f, 2.0f, 3.0f, 0.5f), "Vector4::abs");

        const Vector3 ndc = Vector4(2.0f, -4.0f, 1.0f, 2.0f).perspectiveDivide();
        check(ndc.getX() == 1.0f && ndc.getY() == -2.0f && ndc.getZ() == 0.5f, "Vector4::perspectiveDivide");

        float buffer[4];
        a.store(buffer);
        check(buffer[0] == 1.0f && buffer[1] == 2.0f && buffer[2] == 3.0f && buffer[3] == 4.0f, "Vector4::store");
        check(Vector4::load(buffer) == a, "Vector4::load round-trips store");

        // The conversions are lane inserts on SSE and NEON now; they must still drop
        // or set w and leave x, y, z alone.
        const Vector3 dropped(a);
        check(dropped.getX() == 1.0f && dropped.getY() == 2.0f && dropped.getZ() == 3.0f &&
            near(dropped.lengthSquared(), 14.0f), "Vector3(Vector4) drops w");
        check(Vector4(Vector3(1.0f, 2.0f, 3.0f), 7.0f) == Vector4(1.0f, 2.0f, 3.0f, 7.0f), "Vector4(Vector3, w) sets w");
    }

    // ── Vector2 additions ────────────────────────────────────────────────────
    {
        const Vector2 s = Vector2(1.0f, 2.0f) + Vector2(3.0f, -1.0f);
        check(s.x == 4.0f && s.y == 1.0f, "Vector2 operator+");
        const Vector2 p = Vector2(2.0f, 3.0f) * Vector2(0.5f, -1.0f);
        check(p.x == 1.0f && p.y == -3.0f, "Vector2 component product");
        check(Vector2(3.0f, 4.0f).lengthSquared() == 25.0f, "Vector2 lengthSquared");
        const Vector2 n = Vector2(3.0f, 4.0f).normalized();
        check(near(n.x, 0.6f) && near(n.y, 0.8f), "Vector2 normalized");
        const Vector2 z = Vector2(0.0f, 0.0f).normalized();
        check(z.x == 0.0f && z.y == 0.0f, "a zero Vector2 normalizes to zero");
    }

    {
        const float raw[4] = {0.1f, 0.2f, 0.3f, 0.9f};
        const Quaternion q = Quaternion::load(raw);
        check(q.getX() == 0.1f && q.getY() == 0.2f && q.getZ() == 0.3f && q.getW() == 0.9f,
            "Quaternion::load reads x, y, z, w");
    }

    // ── Matrix4 additions ────────────────────────────────────────────────────
    {
        const Matrix4 a = Matrix4::trs(Vector3(1.0f, -2.0f, 3.0f), Quaternion::fromEulerAngles(20.0f, 35.0f, -50.0f),
            Vector3(1.5f, 0.5f, 2.0f));
        const Matrix4 b = Matrix4::trs(Vector3(-4.0f, 0.5f, 2.0f), Quaternion::fromEulerAngles(-10.0f, 80.0f, 5.0f),
            Vector3(0.75f, 3.0f, 1.25f));
        const Matrix4 affine = a.mulAffine(b);
        const Matrix4 general = a * b;
        bool agrees = true;
        for (int col = 0; col < 4; ++col) {
            for (int row = 0; row < 4; ++row) {
                agrees = agrees && near(affine.getElement(col, row), general.getElement(col, row), 1e-5f);
            }
        }
        check(agrees, "mulAffine agrees with operator* for affine operands");

        // The bottom row is forced, not computed: a non-affine rhs still comes back affine.
        Matrix4 skewed = b;
        skewed.setElement(0, 3, 5.0f);
        skewed.setElement(3, 3, 7.0f);
        const Matrix4 forced = a.mulAffine(skewed);
        check(forced.getElement(0, 3) == 0.0f && forced.getElement(1, 3) == 0.0f &&
            forced.getElement(2, 3) == 0.0f && forced.getElement(3, 3) == 1.0f,
            "mulAffine forces the bottom row to (0, 0, 0, 1)");

        const Matrix4 t = Matrix4::translation(Vector3(1.0f, 2.0f, 3.0f));
        const Matrix4 tf = Matrix4::translation(1.0f, 2.0f, 3.0f);
        bool same = true;
        for (int col = 0; col < 4; ++col) {
            for (int row = 0; row < 4; ++row) {
                same = same && t.getElement(col, row) == tf.getElement(col, row);
            }
        }
        check(same, "translation(Vector3) is translation(x, y, z)");

        check(near(Matrix4::trs(Vector3(0.0f), Quaternion(), Vector3(2.0f, 3.0f, 4.0f)).determinant3x3(), 24.0f),
            "determinant3x3 of a scale is the product of its factors");
        check(Matrix4::trs(Vector3(0.0f), Quaternion::fromEulerAngles(0.0f, 30.0f, 0.0f),
            Vector3(-1.0f, 1.0f, 1.0f)).determinant3x3() < 0.0f, "a mirrored transform has a negative determinant");

        // normalMatrix is the inverse transpose of the upper 3x3, with the signed
        // determinant: compare against the general 4x4 path.
        const Matrix4 n = a.normalMatrix();
        const Matrix4 reference = a.inverse().transpose();
        bool normalAgrees = true;
        for (int col = 0; col < 3; ++col) {
            for (int row = 0; row < 3; ++row) {
                normalAgrees = normalAgrees && near(n.getElement(col, row), reference.getElement(col, row), 1e-5f);
            }
            normalAgrees = normalAgrees && n.getElement(col, 3) == 0.0f && n.getElement(3, col) == 0.0f;
        }
        check(normalAgrees && n.getElement(3, 3) == 1.0f, "normalMatrix is the upper-3x3 inverse transpose");

        const Matrix4 mirrored = Matrix4::trs(Vector3(0.0f), Quaternion(), Vector3(-2.0f, 1.0f, 1.0f));
        check(near(mirrored.normalMatrix().getElement(0, 0), -0.5f),
            "a mirrored normal matrix keeps the determinant's sign");
        const Matrix4 flat = Matrix4::trs(Vector3(0.0f), Quaternion(), Vector3(1.0f, 0.0f, 1.0f)).normalMatrix();
        bool zero3x3 = true;
        for (int col = 0; col < 3; ++col) {
            for (int row = 0; row < 3; ++row) {
                zero3x3 = zero3x3 && flat.getElement(col, row) == 0.0f;
            }
        }
        check(zero3x3, "a singular 3x3 gives a zero normal matrix, not infinities");

        float raw[16];
        a.store(raw);
        bool columnMajor = true;
        for (int col = 0; col < 4; ++col) {
            for (int row = 0; row < 4; ++row) {
                columnMajor = columnMajor && raw[col * 4 + row] == a.getElement(col, row);
            }
        }
        check(columnMajor, "Matrix4::store writes column-major");
        const Matrix4 back = Matrix4::load(raw);
        bool roundTrip = true;
        for (int col = 0; col < 4; ++col) {
            for (int row = 0; row < 4; ++row) {
                roundTrip = roundTrip && back.getElement(col, row) == a.getElement(col, row);
            }
        }
        check(roundTrip, "Matrix4::load round-trips store");
    }

    if (failures == 0) {
        std::cout << "simd math contracts: all checks passed\n";
        return 0;
    }
    std::cout << "simd math contracts: " << failures << " check(s) FAILED\n";
    return 1;
}
