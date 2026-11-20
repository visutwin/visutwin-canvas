// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 21.07.2025
//
#pragma once

#include <numbers>

#include "matrix4.h"
#include "vector3.h"

namespace visutwin::canvas
{
    /*
     * A sphere primitive, representing the set of all points some distance from the origin
     */
    struct Sphere
    {
        float radius;
    };

    struct Frustum
    {
        Vector4 planes[6];

        void create(const Matrix4& viewProjection);

        /// Grows the frustum to also contain the frustum of another view-projection: each
        /// plane keeps its orientation and is pushed out just far enough to hold the other
        /// frustum's eight corners. Conservative, and right for the asymmetric projections of
        /// a headset's eyes, whose matching planes have different normals.
        void add(const Matrix4& otherViewProjection);

        [[nodiscard]] bool checkPoint(const Vector3&) const;
        [[nodiscard]] bool checkSphere(const Vector3&, float) const;

        enum class Intersect
        {
            Outside,
            Intersects,
            Inside,
        };
        //Intersect checkBox(const AABB& box) const;
        //bool checkBoxFast(const AABB& box) const;

        [[nodiscard]] const Vector4& getNearPlane() const { return planes[0]; };
        [[nodiscard]] const Vector4& getFarPlane() const { return planes[1]; };
        [[nodiscard]] const Vector4& getLeftPlane() const { return planes[2]; };
        [[nodiscard]] const Vector4& getRightPlane() const { return planes[3]; };
        [[nodiscard]] const Vector4& getTopPlane() const { return planes[4]; };
        [[nodiscard]] const Vector4& getBottomPlane() const { return planes[5]; };
    };
}
