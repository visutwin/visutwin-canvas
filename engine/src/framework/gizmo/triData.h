// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 02.10.2026
//
// The triangles a gizmo shape is PICKED against.
//
// A shape is selected by intersecting the pointer ray with triangles, not by a screen
// distance: each shape keeps one or more TriData, a unit primitive (cone, cylinder,
// box, sphere, plane, torus) plus the transform that places it inside the shape's
// entity. Picking carries the ray into that space (the inverse of entity world matrix
// times this transform) and tests every triangle.
//
#pragma once

#include <vector>

#include "core/math/matrix4.h"
#include "core/math/quaternion.h"
#include "core/math/vector3.h"
#include "core/shape/ray.h"
#include "core/shape/tri.h"

namespace visutwin::canvas
{
    struct PrimitiveGeometry;

    class TriData
    {
    public:
        /// @param priority 0 = no priority; a higher value wins over a nearer hit
        /// (two prioritised hits order by priority, otherwise by distance).
        explicit TriData(const PrimitiveGeometry& geometry, int priority = 0);

        const Matrix4& transform() const { return _transform; }
        int priority() const { return _priority; }
        const std::vector<Tri>& tris() const { return _tris; }

        void setTransform(const Vector3& position = Vector3(0.0f),
                          const Quaternion& rotation = Quaternion(),
                          const Vector3& scale = Vector3(0.0f));

        void fromGeometry(const PrimitiveGeometry& geometry);

        /**
         * The nearest hit of a WORLD-space ray against these triangles placed by
         * `parentWorld * transform()`, as the distance from the ray origin to the
         * hit in world units.
         */
        bool intersect(const Matrix4& parentWorld, const Vector3& origin, const Vector3& direction,
                       float& outDistance) const;

    private:
        int _priority = 0;
        Matrix4 _transform = Matrix4::identity();
        std::vector<Tri> _tris;
    };
}
