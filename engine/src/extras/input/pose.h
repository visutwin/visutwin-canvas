// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers 02.01.2026
//
#pragma once

#include <limits>

#include "core/math/vector2.h"
#include "core/math/vector3.h"

namespace visutwin::canvas
{
    /**
     * What an InputController produces — a position, Euler angles in
     * degrees and, for controllers that orbit, the distance to the focus point. The
     * ranges clamp the results of rotate() and move().
     */
    class Pose
    {
    public:
        Pose() = default;
        Pose(const Vector3& position, const Vector3& angles, float distance)
        {
            set(position, angles, distance);
        }

        Vector3 position = Vector3(0.0f);
        Vector3 angles = Vector3(0.0f);
        float distance = 0.0f;

        Vector2 pitchRange = Vector2(-std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity());
        Vector2 yawRange = Vector2(-std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity());
        Vector2 xRange = Vector2(-std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity());
        Vector2 yRange = Vector2(-std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity());
        Vector2 zRange = Vector2(-std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity());

        /// Copies position, angles and distance (not the ranges).
        Pose& copy(const Pose& other) { return set(other.position, other.angles, other.distance); }

        [[nodiscard]] bool equalsApprox(const Pose& other, float epsilon = 1e-6f) const;

        /// Interpolates position by alpha1, angles (the short way round) by alpha2 and the
        /// distance by alpha3.
        Pose& lerp(const Pose& lhs, const Pose& rhs, float alpha1, float alpha2, float alpha3);
        Pose& lerp(const Pose& lhs, const Pose& rhs, const float alpha) { return lerp(lhs, rhs, alpha, alpha, alpha); }

        Pose& move(const Vector3& offset);
        Pose& rotate(const Vector3& euler);
        Pose& set(const Vector3& position, const Vector3& angles, float distance);

        /// Look from `from` at `to`: the position is `from`, the distance between them.
        Pose& look(const Vector3& from, const Vector3& to);

        /// The position plus the forward vector scaled by the distance.
        [[nodiscard]] Vector3 getFocus() const;
    };
}
