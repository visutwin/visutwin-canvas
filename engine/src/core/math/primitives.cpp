// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 25.07.2025.
//

#include "primitives.h"

namespace visutwin::canvas
{
    void Frustum::create(const Matrix4& viewProjection)
    {
        // We are interested in columns of the matrix, so transpose because we can access only rows:
        const Matrix4 mat = viewProjection.transpose();

        const auto c0 =  mat.getColumn(0);
        const auto c1 =  mat.getColumn(1);
        const auto c2 =  mat.getColumn(2);
        const auto c3 =  mat.getColumn(3);

        // Near plane (OpenGL-style clip space: z >= -w):
        planes[0] = (c3 + c2).planeNormalize();

        // Far plane:
        planes[1] = (c3 - c2).planeNormalize();

        // Left plane:
        planes[2] = (c3 + c0).planeNormalize();

        // Right plane:
        planes[3] = (c3 - c0).planeNormalize();

        // Top plane:
        planes[4] = (c3 - c1).planeNormalize();

        // Bottom plane:
        planes[5] = (c3 + c1).planeNormalize();
    }

    bool Frustum::checkPoint(const Vector3& point) const
    {
        const auto p = Vector4(point);
        for (const auto& plane : planes)
        {
            if (plane.planeDotCoord(p) < 0.0f)
            {
                return false;
            }
        }
        return true;
    }

    bool Frustum::checkSphere(const Vector3& center, const float radius) const
    {
        const auto c = Vector4(center);
        for (const auto& plane : planes)
        {
            if (plane.planeDotCoord(c) < -radius)
            {
                return false;
            }
        }
        return true;
    }
}
