// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers 02.01.2026
//
#pragma once

#include "core/math/vector3.h"

namespace visutwin::canvas
{
    /**
     * Represents a pose in 3D space, including position and rotation
     */
    class Pose
    {
    public:
        // Sets the pose to look in the direction of the given vector
        Pose* look(const Vector3& from, const Vector3& to);

    private:
        Vector3 _position;

        // The angles of the pose in degrees calculated from the forward vector
        Vector3 _angles;

        // The focus distance from the position to the pose
        float _distance;
    };
}