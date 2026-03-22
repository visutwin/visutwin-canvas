// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
#pragma once

#include <core/math/vector3.h>

namespace visutwin::canvas
{
    class Ray
    {
    public:
        Ray();
        Ray(const Vector3& origin, const Vector3& direction);

        const Vector3& origin() const { return _origin; }
        Vector3& origin() { return _origin; }
        const Vector3& direction() const { return _direction; }
        Vector3& direction() { return _direction; }

        Ray& set(const Vector3& origin, const Vector3& direction);
        Ray& copy(const Ray& src);
        [[nodiscard]] Ray clone() const;

    private:
        Vector3 _origin;
        Vector3 _direction;
    };
}
