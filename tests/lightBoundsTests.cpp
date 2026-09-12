// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// The cluster grid is sized from the union of these bounds and the shader ignores any
// fragment outside it, so a bound that is too SMALL drops lighting on surfaces the
// light really reaches, and one that is too LARGE only coarsens every cell in the
// scene. Neither reads as an obvious defect in a render — the first looks like a
// falloff, the second like nothing at all — so the property is checked here.
//
// A spot used to be bounded by its whole range SPHERE. At a 20-degree cone that is
// about thirty times the volume the light can light, and the grid paid for all of it.
//
#include <cmath>
#include <cstdio>
#include <numbers>
#include <random>

#include "scene/lighting/lightBounds.h"

using namespace visutwin::canvas;

namespace
{
    int failures = 0;

    void check(const bool condition, const char* what)
    {
        std::printf("  %s %s\n", condition ? "ok  " : "FAIL", what);
        if (!condition) {
            ++failures;
        }
    }

    bool contains(const BoundingBox& box, const Vector3& point, const float slack = 1e-3f)
    {
        const Vector3 low = box.center() - box.halfExtents();
        const Vector3 high = box.center() + box.halfExtents();
        return point.getX() >= low.getX() - slack && point.getX() <= high.getX() + slack
            && point.getY() >= low.getY() - slack && point.getY() <= high.getY() + slack
            && point.getZ() >= low.getZ() - slack && point.getZ() <= high.getZ() + slack;
    }

    float volume(const BoundingBox& box)
    {
        const Vector3 h = box.halfExtents();
        return 8.0f * h.getX() * h.getY() * h.getZ();
    }
}

int main()
{
    constexpr float degToRad = std::numbers::pi_v<float> / 180.0f;

    // ── Containment: every point the spot lights is inside its bound ─────────
    // The property that matters. Sampled over the spherical sector rather than
    // asserted against a formula, so it holds whatever the bound is spelled as.
    {
        const Vector3 apex(2.0f, -1.0f, 3.0f);
        const Vector3 axis = Vector3(0.3f, -1.0f, 0.45f).normalized();
        const float range = 12.0f;

        for (const float outerDegrees : {5.0f, 20.0f, 45.0f, 70.0f, 100.0f}) {
            const BoundingBox box = spotConeAabb(apex, axis, range, outerDegrees);

            // Build any two directions perpendicular to the axis to sweep the cone.
            Vector3 side = std::fabs(axis.getY()) < 0.9f
                ? Vector3(0.0f, 1.0f, 0.0f) : Vector3(1.0f, 0.0f, 0.0f);
            side = axis.cross(side).normalized();
            const Vector3 other = axis.cross(side).normalized();

            bool allInside = true;
            std::mt19937 rng(7u);
            std::uniform_real_distribution<float> unit(0.0f, 1.0f);
            for (int i = 0; i < 4000; ++i) {
                // A point of the sector: any angle up to outer, any radius up to range.
                const float phi = outerDegrees * degToRad * unit(rng);
                const float theta = 2.0f * std::numbers::pi_v<float> * unit(rng);
                const float radius = range * unit(rng);
                const Vector3 direction = axis * std::cos(phi)
                    + (side * std::cos(theta) + other * std::sin(theta)) * std::sin(phi);
                if (!contains(box, apex + direction * radius)) {
                    allInside = false;
                    break;
                }
            }
            check(allInside, outerDegrees < 50.0f
                ? "a narrow spot's bound contains every point it lights"
                : "a wide spot's bound contains every point it lights");
        }
    }

    // ── Tightness, which is the reason for the change ────────────────────────
    {
        const Vector3 apex(0.0f, 0.0f, 0.0f);
        const Vector3 axis(0.0f, -1.0f, 0.0f);
        const float range = 10.0f;

        const BoundingBox narrow = spotConeAabb(apex, axis, range, 20.0f);
        const BoundingBox sphere = omniAabb(apex, range);
        check(volume(narrow) < volume(sphere) * 0.1f,
            "a 20-degree spot is bounded in under a tenth of its range sphere");

        // A spot that opens all the way round is the sphere, and must not be smaller.
        const BoundingBox full = spotConeAabb(apex, axis, range, 180.0f);
        check(volume(full) >= volume(sphere) * 0.99f,
            "a 180-degree spot is bounded by its whole range sphere");

        // Wider cones bound more. Monotonic, with no step where the formula switches
        // from the in-cone case to the rim case.
        float previous = 0.0f;
        bool monotonic = true;
        for (int degrees = 5; degrees <= 180; degrees += 5) {
            const float current = volume(spotConeAabb(apex, axis, range,
                static_cast<float>(degrees)));
            if (current < previous - 1e-3f) {
                monotonic = false;
                break;
            }
            previous = current;
        }
        check(monotonic, "the bound grows monotonically with the cone angle");
    }

    // ── The bound follows the light's direction ──────────────────────────────
    // A spot aimed down reaches below its apex and barely above it. Getting this
    // backwards would bound the wrong half of the scene, and the grid would simply
    // not cover what the light lights.
    {
        const BoundingBox down = spotConeAabb(Vector3(0.0f), Vector3(0.0f, -1.0f, 0.0f),
            10.0f, 15.0f);
        const Vector3 low = down.center() - down.halfExtents();
        const Vector3 high = down.center() + down.halfExtents();
        check(low.getY() <= -9.9f, "a downward spot reaches a full range below itself");
        check(high.getY() <= 0.001f, "and essentially nothing above itself");
    }

    // ── An omni is its range in every direction ──────────────────────────────
    {
        const BoundingBox omni = omniAabb(Vector3(1.0f, 2.0f, 3.0f), 4.0f);
        check(contains(omni, Vector3(1.0f, 2.0f, -1.0f)) &&
              contains(omni, Vector3(5.0f, 2.0f, 3.0f)),
            "an omni's bound reaches its range on every axis");
    }

    if (failures == 0) {
        std::printf("light bounds: all checks passed\n");
    }
    return failures == 0 ? 0 : 1;
}
