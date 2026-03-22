// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
#pragma once

#include <core/math/vector3.h>
#include <core/shape/ray.h>

namespace visutwin::canvas
{
    class Plane
    {
    public:
        Plane();
        Plane(const Vector3& normal, float distance);

        const Vector3& normal() const { return _normal; }
        void setNormal(const Vector3& normal) { _normal = normal; }
        float distance() const { return _distance; }
        void setDistance(float distance) { _distance = distance; }

        Plane& copy(const Plane& src);
        [[nodiscard]] Plane clone() const;
        bool intersectsLine(const Vector3& start, const Vector3& end, Vector3* point = nullptr) const;
        bool intersectsRay(const Ray& ray, Vector3* point = nullptr) const;
        Plane& normalize();
        Plane& set(float nx, float ny, float nz, float d);
        Plane& setFromPointNormal(const Vector3& point, const Vector3& normal);

    private:
        Vector3 _normal;
        float _distance;
    };
}
