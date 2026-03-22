// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers 02.01.2026
//
#include "pose.h"

namespace visutwin::canvas
{
    Pose* Pose::look(const Vector3& from, const Vector3& to)
    {
        _position = from;
        _distance = from.distance(to);

        const Vector3 dir = (to - from).normalized();
        const float dx = dir.getX();
        const float dy = dir.getY();
        const float dz = dir.getZ();

        float elev = std::atan2(-dy, std::sqrt(dx * dx + dz * dz)) * RAD_TO_DEG;
        float azim = std::atan2(-dx, -dz) * RAD_TO_DEG;

        _angles = Vector3(-elev, azim, 0.0f);

        return this;
    }
}
