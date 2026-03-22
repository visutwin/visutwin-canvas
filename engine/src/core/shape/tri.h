// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
#pragma once

#include <string>

#include <core/math/vector3.h>
#include <core/shape/ray.h>

namespace visutwin::canvas
{
    class Tri
    {
    public:
        Tri();
        Tri(const Vector3& v0, const Vector3& v1, const Vector3& v2);

        const Vector3& v0() const { return _v0; }
        Vector3& v0() { return _v0; }
        const Vector3& v1() const { return _v1; }
        Vector3& v1() { return _v1; }
        const Vector3& v2() const { return _v2; }
        Vector3& v2() { return _v2; }

        Tri& set(const Vector3& v0, const Vector3& v1, const Vector3& v2);
        bool intersectsRay(const Ray& ray, Vector3* point = nullptr) const;
        [[nodiscard]] std::string toString() const;

    private:
        Vector3 _v0;
        Vector3 _v1;
        Vector3 _v2;
    };
}
