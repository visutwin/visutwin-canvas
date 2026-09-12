// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// World-space bounds of what a local light can reach, for the cluster grid.
//
// Free functions over plain values so a unit test can hold them without a Light, a
// device or a grid — a bound that is too small drops lighting, a bound that is too
// large only wastes cells, and neither shows up as an obvious defect in a render.
#pragma once

#include <algorithm>
#include <cmath>
#include <numbers>

#include "core/math/vector3.h"
#include "core/shape/boundingBox.h"

namespace visutwin::canvas
{
    /**
     * The exact axis-aligned bound of a spot light's lit volume: the spherical
     * SECTOR of radius `range` and half-angle `outerConeAngleDegrees` about
     * `direction`, with its apex at `position`.
     *
     * Derived from the sector's support function rather than from a box, so it is
     * tight in every orientation. Along a query axis at angle phi from the light's
     * own axis the sector reaches `range` when phi is inside the cone, and only
     * `range * cos(phi - outer)` — the nearest point on the cap's rim — when it is
     * outside; past a quarter turn from the rim it reaches nothing at all and the
     * apex is the extreme point, which is what the clamp at zero expresses.
     *
     * DEVIATION, in the tighter direction: upstream (light.js getBoundingBox) builds
     * a box of `sin(outer) * range` across by `range` deep and transforms it by the
     * node, which bounds the sector but is far looser for a narrow cone — and a
     * rotated box's own AABB is looser again. The grid is sized from the union of
     * these, so slack here coarsens every cell in the scene.
     *
     * A cone's bound is NOT the range sphere. That was the old approximation, and at
     * 20 degrees it is about thirty times the volume the light can actually light.
     */
    inline BoundingBox spotConeAabb(const Vector3& position, const Vector3& direction,
        const float range, const float outerConeAngleDegrees)
    {
        Vector3 axis = direction;
        if (axis.lengthSquared() < 1e-12f) {
            axis = Vector3(0.0f, -1.0f, 0.0f);
        } else {
            axis = axis.normalized();
        }

        const float outer = std::clamp(outerConeAngleDegrees, 0.0f, 180.0f)
            * (std::numbers::pi_v<float> / 180.0f);
        const float axisComponent[3] = {axis.getX(), axis.getY(), axis.getZ()};
        const float apex[3] = {position.getX(), position.getY(), position.getZ()};

        float boundsMin[3];
        float boundsMax[3];
        for (int i = 0; i < 3; ++i) {
            // How far the sector reaches along +e_i and along -e_i. Never less than
            // zero: the apex is always in the volume, so it is the fallback extreme.
            const auto reach = [&](const float cosPhi) {
                const float phi = std::acos(std::clamp(cosPhi, -1.0f, 1.0f));
                const float extent = (phi <= outer) ? range : range * std::cos(phi - outer);
                return std::max(extent, 0.0f);
            };
            boundsMax[i] = apex[i] + reach(axisComponent[i]);
            boundsMin[i] = apex[i] - reach(-axisComponent[i]);
        }

        const Vector3 low(boundsMin[0], boundsMin[1], boundsMin[2]);
        const Vector3 high(boundsMax[0], boundsMax[1], boundsMax[2]);
        return BoundingBox((low + high) * 0.5f, (high - low) * 0.5f);
    }

    /** An omni light reaches its range in every direction. */
    inline BoundingBox omniAabb(const Vector3& position, const float range)
    {
        return BoundingBox(position, Vector3(range, range, range));
    }
}
