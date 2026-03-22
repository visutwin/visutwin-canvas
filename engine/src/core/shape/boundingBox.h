// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers 20.12.2025.
//
#pragma once

#include <core/math/vector3.h>

namespace visutwin::canvas
{
    /**
     * Axis-Aligned Bounding Box.
     *
     * An AABB is defined by a center point and half-extents (half the distance
     * across the box in each axis). This representation enables efficient
     * intersection of testing and transformation operations.
     */
    class BoundingBox
    {
    public:
        BoundingBox(): _center(0, 0, 0), _halfExtents(0.5f, 0.5f, 0.5f) {}
        BoundingBox(const Vector3& center, const Vector3& halfExtents)
            : _center(center), _halfExtents(halfExtents) {}

        const Vector3& center() const { return _center; }
        void setCenter(const Vector3& center) { _center = center; }

        void setCenter(float x, float y, float z)
        {
            _center = {x, y, z};
        }

        const Vector3& halfExtents() const { return _halfExtents; }
        void setHalfExtents(const Vector3& halfExtents) { _halfExtents = halfExtents; }

        void setHalfExtents(float x, float y, float z)
        {
            _halfExtents = {x, y, z};
        }

        void setFromTransformedAabb(const BoundingBox& aabb, const Matrix4& m, bool ignoreScale = false);

        void add(const BoundingBox& other);

    private:
        Vector3 _center;

        // Half the distance across the box in each axis
        Vector3 _halfExtents;

        // Cached min/max vectors for getMin/getMax (avoid repeated allocations)
        mutable Vector3 _min;
        mutable Vector3 _max;
    };
}
