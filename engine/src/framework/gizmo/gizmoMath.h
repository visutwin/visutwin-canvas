// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 24.10.2026
//
// Small vector helpers the overlay gizmos share.
//
#pragma once

#include <cmath>
#include <limits>

#include "core/math/quaternion.h"
#include "core/math/vector3.h"

namespace visutwin::canvas
{
    /// The shortest rotation turning unit direction `from` onto unit direction `to`; opposite
    /// directions turn half way round an axis perpendicular to `from`.
    inline Quaternion gizmoRotationBetween(const Vector3& from, const Vector3& to)
    {
        const float d = 1.0f + from.dot(to);
        if (d < std::numeric_limits<float>::epsilon()) {
            if (std::abs(from.getX()) > std::abs(from.getY())) {
                return Quaternion(-from.getZ(), 0.0f, from.getX(), 0.0f).normalized();
            }
            return Quaternion(0.0f, -from.getZ(), from.getY(), 0.0f).normalized();
        }
        const Vector3 c = from.cross(to);
        return Quaternion(c.getX(), c.getY(), c.getZ(), d).normalized();
    }

    /// The largest of three values.
    inline float gizmoMax3(const float a, const float b, const float c)
    {
        return std::fmax(a, std::fmax(b, c));
    }
}
