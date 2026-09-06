// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// The distance the forward pass sorts draws on. Upstream (layer.js,
// _calculateSortDistances) uses the SIGNED depth along the camera's forward
// vector, not the radial distance to the camera: two transparent surfaces at the
// same view depth must draw in a stable order whatever their screen position,
// and radial distance ranks an off-axis surface as farther than a centred one by
// up to 1 / cos(fov / 2). Radial distance also cannot tell "behind the camera"
// from "in front". Kept as free functions so a unit test can hold the contract
// without a renderer.
#pragma once

#include "core/math/matrix4.h"
#include "core/math/vector3.h"

namespace visutwin::canvas
{
    /** World-space forward vector of a camera node (it looks down its local -Z). */
    inline Vector3 cameraForwardOf(const Matrix4& cameraWorld)
    {
        const Vector3 forward = cameraWorld.transformPoint(Vector3(0.0f, 0.0f, -1.0f)) -
                                cameraWorld.transformPoint(Vector3(0.0f));
        return forward.lengthSquared() > 0.0f ? forward.normalized() : Vector3(0.0f, 0.0f, -1.0f);
    }

    /** Signed depth of `point` along `cameraForward`, measured from `cameraPosition`. */
    inline float forwardSortDistance(const Vector3& point, const Vector3& cameraPosition,
                                     const Vector3& cameraForward)
    {
        return (point - cameraPosition).dot(cameraForward);
    }
}
