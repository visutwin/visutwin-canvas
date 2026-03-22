// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
#pragma once

#include <core/math/matrix4.h>
#include <core/math/vector3.h>
#include <core/shape/boundingBox.h>
#include <core/shape/boundingSphere.h>
#include <core/shape/ray.h>

namespace visutwin::canvas
{
    class OrientedBox
    {
    public:
        OrientedBox();
        OrientedBox(const Matrix4& worldTransform, const Vector3& halfExtents = Vector3(0.5f));

        const Matrix4& worldTransform() const { return _worldTransform; }
        void setWorldTransform(const Matrix4& value);
        const Vector3& halfExtents() const { return _halfExtents; }
        void setHalfExtents(const Vector3& halfExtents);
        bool intersectsRay(const Ray& ray, Vector3* point = nullptr) const;
        bool containsPoint(const Vector3& point) const;
        bool intersectsBoundingSphere(const BoundingSphere& sphere) const;

    private:
        bool intersectsLocalAabbRay(const Ray& localRay, Vector3* localPoint) const;
        bool fastIntersectsLocalAabbRay(const Ray& localRay) const;
        Vector3 closestPointOnLocalAabb(const Vector3& localPoint) const;

        Vector3 _halfExtents;
        Matrix4 _modelTransform;
        Matrix4 _worldTransform;
        BoundingBox _aabb;
    };
}
