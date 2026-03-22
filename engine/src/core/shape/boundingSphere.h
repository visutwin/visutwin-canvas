// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
#pragma once

#include <core/math/vector3.h>
#include <core/shape/ray.h>

namespace visutwin::canvas
{
    class BoundingSphere
    {
    public:
        BoundingSphere();
        BoundingSphere(const Vector3& center, float radius);

        const Vector3& center() const { return _center; }
        void setCenter(const Vector3& center) { _center = center; }

        float radius() const { return _radius; }
        void setRadius(const float radius) { _radius = radius; }

        BoundingSphere& copy(const BoundingSphere& src);
        [[nodiscard]] BoundingSphere clone() const;
        [[nodiscard]] bool containsPoint(const Vector3& point) const;
        bool intersectsRay(const Ray& ray, Vector3* point = nullptr) const;
        [[nodiscard]] bool intersectsBoundingSphere(const BoundingSphere& sphere) const;

    private:
        Vector3 _center;
        float _radius;
    };
}
